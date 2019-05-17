#include "viewport_renderer_override.hpp"

// Wisp plug-in
#include "framegraph/frame_graph_manager.hpp"
#include "miscellaneous/functions.hpp"
#include "miscellaneous/settings.hpp"
#include "parsers/camera_parser.hpp"
#include "parsers/scene_graph_parser.hpp"
#include "render_operations/gizmo_render_operation.hpp"
#include "render_operations/renderer_copy_operation.hpp"
#include "render_operations/renderer_draw_operation.hpp"
#include "render_operations/renderer_update_operation.hpp"
#include "render_operations/screen_render_operation.hpp"
#include "renderer/renderer.hpp"

// Wisp rendering framework
#include "d3d12/d3d12_renderer.hpp"
#include "window.hpp"

// Maya API
#include <maya/M3dView.h>
#include <maya/MDagPath.h>
#include <maya/MEulerRotation.h>
#include <maya/MFnCamera.h>
#include <maya/MFnTransform.h>
#include <maya/MGlobal.h>
#include <maya/MImage.h>
#include <maya/MMatrix.h>
#include <maya/MQuaternion.h>
#include <maya/MString.h>

// C++ standard
#include <algorithm>
#include <memory>
#include <fstream>
#include <sstream>
#include <string>

static std::shared_ptr<wr::TexturePool> texture_pool;
static std::shared_ptr<wr::MaterialPool> material_pool;
static wr::TextureHandle loaded_skybox;
static wr::TextureHandle loaded_skybox2;

#define SCENE viknell_scene
namespace wmr
{
	static void EnsurePanelDisplayShading( const MString& destination )
	{
		M3dView view;

		if( destination.length() &&
			M3dView::getM3dViewFromModelPanel( destination, view ) == MStatus::kSuccess )
		{
			if( view.displayStyle() != M3dView::kGouraudShaded )
			{
				view.setDisplayStyle( M3dView::kGouraudShaded );
			}
		}
	}

	ViewportRendererOverride::ViewportRendererOverride(const MString& name)
		: MRenderOverride(name)
		, m_ui_name(wmr::settings::PRODUCT_NAME)
		, m_current_render_operation(-1)
		, m_viewport_width(1)
		, m_viewport_height(1)
		, m_is_initialized(false)
	{
		const auto maya_renderer = MHWRender::MRenderer::theRenderer();
		if( maya_renderer )
		{
			maya_renderer->registerOverride( this );
		}
		else
		{
			assert( false );
		}

		m_renderer = std::make_unique<Renderer>();
		m_renderer->Initialize();

		CreateRenderOperations();

		m_scenegraph_parser = std::make_unique<ScenegraphParser>();
		m_scenegraph_parser->Initialize();



		// Let the user know that the plugin is in development
		InitialNotifyUser();
	}

	ViewportRendererOverride::~ViewportRendererOverride()
	{
		// Not the Wisp renderer, but the internal Maya renderer
		const auto maya_renderer = MHWRender::MRenderer::theRenderer();

		if( maya_renderer )
		{
			// De-register the actual plug-in
			maya_renderer->deregisterOverride( this );
		}
	}

	void ViewportRendererOverride::CreateRenderOperations()
	{
		if (!m_render_operations[0])
		{
			// Needs to be created first because other operations depend on it.
			std::unique_ptr<ScreenRenderOperation> screen_render_operation = std::make_unique<ScreenRenderOperation>(settings::RENDER_OPERATION_NAMES[3]);

			m_render_operations[0] = std::make_unique<RendererUpdateOperation>	(settings::RENDER_OPERATION_NAMES[0]);
			m_render_operations[1] = std::make_unique<RendererDrawOperation>	(settings::RENDER_OPERATION_NAMES[1]);
			m_render_operations[2] = std::make_unique<RendererCopyOperation>	(settings::RENDER_OPERATION_NAMES[2], *screen_render_operation );
			m_render_operations[3] = std::move(screen_render_operation);
			m_render_operations[4] = std::make_unique<GizmoRenderOperation>		(settings::RENDER_OPERATION_NAMES[4]);
			m_render_operations[5] = std::make_unique<MHWRender::MHUDRender>	();
			m_render_operations[6] = std::make_unique<MHWRender::MPresentTarget>(settings::RENDER_OPERATION_NAMES[5]);
		}
	}

	MHWRender::DrawAPI ViewportRendererOverride::supportedDrawAPIs() const
	{
		return (MHWRender::kOpenGL | MHWRender::kOpenGLCoreProfile | MHWRender::kDirectX11);
	}

	MHWRender::MRenderOperation* ViewportRendererOverride::renderOperation()
	{
		if (m_current_render_operation >= 0 && m_current_render_operation < settings::RENDER_OPERATION_COUNT)
		{
			if (m_render_operations[m_current_render_operation])
			{
				return m_render_operations[m_current_render_operation].get();
			}
		}

		return nullptr;
	}

	Renderer& ViewportRendererOverride::GetRenderer() const
	{
		return *m_renderer;
	}

	wmr::ScenegraphParser & ViewportRendererOverride::GetSceneGraphParser() const
	{
		return *m_scenegraph_parser;
	}

	const std::pair<uint32_t, uint32_t> ViewportRendererOverride::GetViewportSize() const noexcept
	{
		return { m_viewport_width, m_viewport_height };
	}

	bool ViewportRendererOverride::IsInitialized() const noexcept
	{
		return m_is_initialized;
	}

	MStatus ViewportRendererOverride::setup(const MString& destination)
	{
		// Update the viewport camera(s)
		m_scenegraph_parser->Update();
		m_scenegraph_parser->GetCameraParser().UpdateViewportCamera(destination);

		// Check if the viewport has been resized
		HandleViewportResize(destination);

		auto* const maya_renderer = MHWRender::MRenderer::theRenderer();

		if (!maya_renderer)
		{
			assert( false ); 
			return MStatus::kFailure;
		}

		auto* const maya_texture_manager = maya_renderer->getTextureManager();

		if (!maya_texture_manager)
		{
			assert( false );
			return MStatus::kFailure;
		}

		if ( !AreAllRenderOperationsSetCorrectly() )
		{
			assert( false );
			return MStatus::kFailure;
		}

		// Force the panel display style to smooth shaded if it is not already
		// this ensures that viewport selection behavior works as if shaded
		EnsurePanelDisplayShading(destination);

		// Ran the setup loop at least once
		if (!m_is_initialized)
			m_is_initialized = true;

		return MStatus::kSuccess;
	}

	void ViewportRendererOverride::HandleViewportResize(const MString& panel_name) noexcept
	{
		M3dView viewport;

		// Try to retrieve the current active viewport panel
		auto status = M3dView::getM3dViewFromModelPanel(panel_name, viewport);

		// Could not retrieve the viewport panel
		if (status == MStatus::kFailure)
			return;

		// Position and dimensions of the current Maya viewport
		std::uint32_t x, y;
		status = viewport.viewport(x, y, m_viewport_width, m_viewport_height);

		// Could not retrieve the viewport information
		if (status == MStatus::kFailure)
			return;

		// Size of the current frame graph
		const auto current_frame_graph_size = m_renderer->GetFrameGraph().GetCurrentDimensions();

		// Wisp <==> Maya viewport resolutions do not match
		if ((current_frame_graph_size.first != m_viewport_width) ||
			(current_frame_graph_size.second != m_viewport_height))
		{
			// Resize the frame graph
			m_renderer->GetFrameGraph().Resize(m_viewport_width, m_viewport_height, m_renderer->GetD3D12Renderer());
		}
	}

	bool ViewportRendererOverride::AreAllRenderOperationsSetCorrectly() const
	{
		bool all_good = true;

		for (const auto& operation : m_render_operations)
		{
			if (!operation)
			{
				all_good = false;
			}
		}

		return all_good;
	}

	MStatus ViewportRendererOverride::cleanup()
	{
		m_current_render_operation = -1;
		return MStatus::kSuccess;
	}

	MString ViewportRendererOverride::uiName() const
	{
		return m_ui_name;
	}

	bool ViewportRendererOverride::startOperationIterator()
	{
		m_current_render_operation = 0;
		return true;
	}

	bool ViewportRendererOverride::nextRenderOperation()
	{
		++m_current_render_operation;

		if (m_current_render_operation < settings::RENDER_OPERATION_COUNT)
		{
			return true;
		}

		return false;
	}
	void ViewportRendererOverride::InitialNotifyUser()
	{
		const char const* text_prefix = "text - ww on - align \"left\" - rs on - w 400 \"";
		const char const* text_postfix = "\";";

		// Create window
		MString notify_command("window -title \"Wisp\" -sizeable off -maximizeButton off -minimizeButton off WispInfoWindow;\n");

		// Set layout
		notify_command += "rowColumnLayout -columnOffset 1 \"both\" 10 -rowOffset 1 \"both\" 15 -nc 1 -cal 1 \"left\";\n";

		// Get file
		std::ifstream infile("resources/notify.txt");
		// Show hardcoded popup if the contents couldn't be found
		if (!infile.is_open()) {
			LOGE("Couldn't find notify.txt! Notifying the user with a default message.");
			// Show old (may be outdated) popup. Also warn the user that the popup may be outdated.
			MGlobal::executeCommand(
				"window -title \"Wisp\" -sizeable off -maximizeButton off -minimizeButton off WispInfoWindow;\
				rowColumnLayout - columnOffset 1 \"both\" 10 - rowOffset 1 \"both\" 15 - nc 1 - cal 1 \"left\";\
				text - ww on - align \"left\" - rs on - w 400 \"Hey there!\";\
				text - ww on - align \"left\" - rs on - w 400 \"Something went wrong with loading the contents of this window. Please keep in mind that the following could be outdated.\";\
				text - ww on - align \"left\" - rs on - w 400 \"Wisp is heavily under development which means that you might encounter weird, annoying and sometimes work-losing bugs/crashes. Don't worry we are working on them!\";\
				text - ww on - align \"left\" - rs on - w 400 \" \";\
				text - ww on - align \"left\" - rs on - w 400 \"Bug and feature updates will be released frequenty. If you encounter bugs or want to provide us with feedback, contact us on discord:\";\
				text - ww on - align \"left\" - rs on - w 400 \- hl on - label \"https://discordapp.com/invite/KthSUvs\" \"https://discordapp.com/invite/KthSUvs\";\
				text - ww on - align \"left\" - rs on - w 400 \" \";\
				text - ww on - align \"left\" - rs on - w 400 \"Either way, enjoy Wisp!\";\
				text - ww on - align \"left\" - rs on - w 400 \" \";\
				text - ww on - align \"left\" - rs on - w 400 \"/ Team Wisp\";\
				text - ww on - align \"left\" - rs on - w 400 \" \";\
				button - enable on - command \"deleteUI WispInfoWindow\" \"Ok\";\
				text - ww on - align \"left\" - rs on - w 400 \" \";\
				showWindow WispInfoWindow;"
			);

			return;
		}
		else {
			// Print text
			std::string line;
			while (std::getline(infile, line)) {
				// Add a space if an empty line was found
				if (line.length() <= 0) {
					line += " ";
				}

				// Text settings
				notify_command += text_prefix;
				// Add text
				notify_command += line.c_str();
				// Add end quote
				notify_command += text_postfix;
			}
			infile.close();
		}

		// Add empty line for proper spacing
		notify_command += text_prefix;
		notify_command += " ";
		notify_command += text_postfix;

		// Add button to close
		notify_command += "button - enable on - command \"deleteUI WispInfoWindow\" \"Ok\";\n";
		
		// Add spacing below the button
		notify_command += text_prefix;
		notify_command += " ";
		notify_command += text_postfix;

		// Display window
		notify_command += "showWindow WispInfoWindow;";

		MGlobal::displayInfo(notify_command);

		// Execute display window command
		MGlobal::executeCommand(notify_command);
	}
}
