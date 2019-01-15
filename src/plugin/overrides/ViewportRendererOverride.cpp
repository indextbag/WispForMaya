#include "ViewportRendererOverride.hpp"
#include "QuadRendererOverride.hpp"
#include "UIOverride.hpp"
#include "miscellaneous/Settings.hpp"
#include "miscellaneous/Functions.hpp"

#include <memory>
#include <algorithm>
#include "wisp.hpp"
#include "renderer.hpp"
#include "render_tasks/d3d12_imgui_render_task.hpp"
#include "render_tasks/d3d12_deferred_main.hpp"
#include "render_tasks/d3d12_deferred_composition.hpp"
#include "render_tasks/d3d12_deferred_render_target_copy.hpp"
#include "scene_graph\camera_node.hpp"
#include "scene_graph\scene_graph.hpp"

#include "../demo/engine_interface.hpp"
#include "../demo/scene_viknell.hpp"
#include "../demo/resources.hpp"
#include "../demo/scene_cubes.hpp"

#include <maya/MString.h>
#include <maya/M3dView.h>
#include <maya/MMatrix.h>
#include <maya/MDagPath.h>
#include <maya/MFnCamera.h>
#include <maya/MImage.h>
#include <maya/M3dView.h>
#include <maya\MGlobal.h>
#include <maya\MQuaternion.h>
#include <maya\MEulerRotation.h>


#include <sstream>


auto window = std::make_unique<wr::Window>( GetModuleHandleA( nullptr ), "D3D12 Test App", 1280, 720 );
const bool load_images_from_disk = true;
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

	ViewportRenderer::ViewportRenderer(const MString& name)
		: MRenderOverride(name)
		, m_ui_name(wisp::settings::PRODUCT_NAME)
		, m_current_render_operation(-1)
	{
		ConfigureRenderOperations();
		SetDefaultColorTextureState();
	}

	ViewportRenderer::~ViewportRenderer()
	{
		ReleaseColorTextureResources();
	}

	void ViewportRenderer::Initialize()
	{
		CreateRenderOperations();
		InitializeWispRenderer();
	}

	void ViewportRenderer::Destroy()
	{
		m_render_system->WaitForAllPreviousWork();
		m_framegraph->Destroy();
		m_model_loader.reset();
		m_render_system.reset();
		m_framegraph.reset();

	}

	void ViewportRenderer::ConfigureRenderOperations()
	{
		m_render_operation_names[0] = "wisp_SceneBlit";
		m_render_operation_names[1] = "wisp_UIDraw";
		m_render_operation_names[2] = "wisp_Present";
	}

	void ViewportRenderer::SetDefaultColorTextureState()
	{
		m_color_texture.texture = nullptr;
		m_color_texture_desc.setToDefault2DTexture();
	}

	void ViewportRenderer::ReleaseColorTextureResources() const
	{
		const auto maya_renderer = MHWRender::MRenderer::theRenderer();

		if (!maya_renderer)
		{
			return;
		}

		const auto maya_texture_manager = maya_renderer->getTextureManager();

		if (!maya_texture_manager || !m_color_texture.texture)
		{
			return;
		}

		maya_texture_manager->releaseTexture(m_color_texture.texture);
	}

	void ViewportRenderer::CreateRenderOperations()
	{
		if (!m_render_operations[0])
		{
			m_render_operations[0] = std::make_unique<WispScreenBlitter>(m_render_operation_names[0]);
			m_render_operations[1] = std::make_unique<WispUIRenderer>(m_render_operation_names[1]);
			m_render_operations[2] = std::make_unique<MHWRender::MHUDRender>();
			m_render_operations[3] = std::make_unique<MHWRender::MPresentTarget>(m_render_operation_names[2]);
		}
	}

	void ViewportRenderer::InitializeWispRenderer()
	{
		util::log_callback::impl = [ & ]( std::string const & str )
		{
			engine::debug_console.AddLog( str.c_str() );
		};

		m_render_system = std::make_unique<wr::D3D12RenderSystem>();

		m_model_loader = std::make_unique<wr::AssimpModelLoader>();

		m_render_system->Init( window.get() );

		resources::CreateResources( m_render_system.get() );

		m_scenegraph = std::make_shared<wr::SceneGraph>( m_render_system.get() );

		m_viewport_camera = m_scenegraph->CreateChild<wr::CameraNode>( nullptr, 90.f, ( float )window->GetWidth() / ( float )window->GetHeight() );
		m_viewport_camera->SetPosition( { 0, 0, -1 } );

		SCENE::CreateScene( m_scenegraph.get(), window.get() );

		m_render_system->InitSceneGraph( *m_scenegraph.get() );

		m_framegraph = std::make_unique<wr::FrameGraph>( 4 );
		wr::AddDeferredMainTask( *m_framegraph );
		wr::AddDeferredCompositionTask( *m_framegraph );
		wr::AddRenderTargetCopyTask<wr::DeferredCompositionTaskData>( *m_framegraph );
		auto render_editor = [&]()
		{
			engine::RenderEngine( m_render_system.get(), m_scenegraph.get() );
		};

		auto imgui_task = wr::GetImGuiTask( render_editor );
		
		m_framegraph->AddTask<wr::ImGuiTaskData>( imgui_task );
		m_framegraph->Setup( *m_render_system );
	}

	MHWRender::DrawAPI ViewportRenderer::supportedDrawAPIs() const
	{
		return (MHWRender::kOpenGL | MHWRender::kOpenGLCoreProfile | MHWRender::kDirectX11);
	}

	MHWRender::MRenderOperation* ViewportRenderer::renderOperation()
	{
		if (m_current_render_operation >= 0 && m_current_render_operation < 4)
		{
			if (m_render_operations[m_current_render_operation])
			{
				return m_render_operations[m_current_render_operation].get();
			}
		}

		return nullptr;
	}

	void ViewportRenderer::SynchronizeWispWithMayaViewportCamera()
	{
		M3dView maya_view;
		MStatus status = M3dView::getM3dViewFromModelPanel( wisp::settings::VIEWPORT_PANEL_NAME, maya_view );

		if( status != MStatus::kSuccess )
		{
			// Failure means no camera data for this frame, early-out!
			return;
		}

		// Model view matrix
		MMatrix mv_matrix;
		maya_view.modelViewMatrix( mv_matrix );

		MDagPath camera_dag_path;
		maya_view.getCamera( camera_dag_path );

		// Additional functionality
		
		MEulerRotation view_rotation = MEulerRotation::decompose(mv_matrix.inverse(), MEulerRotation::RotationOrder::kXYZ);
		
		std::stringstream strs;
		strs << "X: " << view_rotation.x << " Y: " << view_rotation.y << " Z: " << view_rotation.z << std::endl;
		MGlobal::displayInfo(std::string(strs.str()).c_str());
		
		m_viewport_camera->SetRotation( {  ( float )view_rotation.x,( float )view_rotation.y, ( float )view_rotation.z } );

		
		MMatrix cameraPos = camera_dag_path.inclusiveMatrix();
		MVector eye = MVector( static_cast<float>( cameraPos(3,0)), static_cast< float >( cameraPos( 3, 1 ) ), static_cast< float >( cameraPos( 3, 2 ) ) );
		m_viewport_camera->SetPosition( { ( float )-eye.x, ( float )-eye.y, ( float )-eye.z } );

		
		MFnCamera camera_functions( camera_dag_path );
		m_viewport_camera->m_frustum_far = camera_functions.farClippingPlane();
		m_viewport_camera->m_frustum_near = camera_functions.nearClippingPlane();
		
		m_viewport_camera->SetFov( AI_RAD_TO_DEG( camera_functions.horizontalFieldOfView()) );


	}

	MStatus ViewportRenderer::setup(const MString& destination)
	{
		SynchronizeWispWithMayaViewportCamera();
		SCENE::UpdateScene();

		auto texture = m_render_system->Render( m_scenegraph, *m_framegraph );

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

		if ( AreAllRenderOperationsSetCorrectly() )
		{
			assert( false );
			return MStatus::kFailure;
		}

		// Update textures used for scene blit
		if (!UpdateTextures(maya_renderer, maya_texture_manager))
		{
			assert( false );
			return MStatus::kFailure;
		}

		// Force the panel display style to smooth shaded if it is not already
		// this ensures that viewport selection behavior works as if shaded
		EnsurePanelDisplayShading(destination);

		return MStatus::kSuccess;
	}

	bool ViewportRenderer::AreAllRenderOperationsSetCorrectly() const
	{
		return (!m_render_operations[0] ||
			!m_render_operations[1] ||
			!m_render_operations[2] ||
			!m_render_operations[3]);
	}

	MStatus ViewportRenderer::cleanup()
	{
		m_current_render_operation = -1;
		return MStatus::kSuccess;
	}

	MString ViewportRenderer::uiName() const
	{
		return m_ui_name;
	}

	bool ViewportRenderer::startOperationIterator()
	{
		m_current_render_operation = 0;
		return true;
	}

	bool ViewportRenderer::nextRenderOperation()
	{
		++m_current_render_operation;

		if (m_current_render_operation < 4)
		{
			return true;
		}

		return false;
	}

	static void loadImageFromDisk( MString& image_location, MHWRender::MTextureDescription& color_texture_desc, MHWRender::MTextureAssignment& color_texture, MHWRender::MTextureManager* texture_manager )
	{
		unsigned char* texture_data = nullptr;

		MImage image;

		unsigned int target_width, target_height;

		image.readFromFile( image_location );
		image.getSize( target_width, target_height );

		texture_data = image.pixels();

		color_texture_desc.fWidth = target_width;
		color_texture_desc.fHeight = target_height;
		color_texture_desc.fDepth = 1;
		color_texture_desc.fBytesPerRow = 4 * target_width;
		color_texture_desc.fBytesPerSlice = color_texture_desc.fBytesPerRow * target_height;

		// Acquire a new texture
		color_texture.texture = texture_manager->acquireTexture( "", color_texture_desc, texture_data );

		if( color_texture.texture )
		{
			color_texture.texture->textureDescription( color_texture_desc );
		}
	}

	bool ViewportRenderer::UpdateTextures(MHWRender::MRenderer* renderer, MHWRender::MTextureManager* texture_manager)
	{
		if (!renderer || !texture_manager)
		{
			return false;
		}

		unsigned int target_width = 0;
		unsigned int target_height = 0;

		renderer->outputTargetSize(target_width, target_height);

		bool aquire_new_texture = false;
		bool force_reload = false;

		bool texture_resized = (m_color_texture_desc.fWidth != target_width ||
			m_color_texture_desc.fHeight != target_height);

		MString image_location(MString(getenv("MAYA_2018_DIR")) + MString("\\devkit\\plug-ins\\viewImageBlitOverride\\"));
		image_location += MString("renderedImage.iff");

		// If a resize occurred, or a texture has not been allocated yet, create new textures that match the output size
		// Any existing textures will be released
		if ( force_reload ||
			 !m_color_texture.texture ||
			 texture_resized )
		{
			if (m_color_texture.texture)
			{
				texture_manager->releaseTexture(m_color_texture.texture);
				m_color_texture.texture = nullptr;
			}

			aquire_new_texture = true;
			force_reload = false;
		}

		if (!m_color_texture.texture)
		{
			loadImageFromDisk( image_location, m_color_texture_desc, m_color_texture, texture_manager );
		}
		else
		{
			// TODO: Update the texture data here!
		}

		// Update the textures for the blit operation
		if (aquire_new_texture)
		{
			auto blit = (WispScreenBlitter*)m_render_operations[0].get();
			blit->SetColorTexture(m_color_texture);
		}

		if (m_color_texture.texture)
		{
			return true;
		}
		else
		{
			return false;
		}
	}

	
}
