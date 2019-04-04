#include "light_parser.hpp"

#include "plugin/callback_manager.hpp"
#include "plugin/viewport_renderer_override.hpp"
#include "plugin/renderer/renderer.hpp"
#include "plugin/renderer/model_manager.hpp"
#include "miscellaneous/settings.hpp"
#include "plugin/renderer/texture_manager.hpp"
#include "plugin/renderer/material_manager.hpp"

#include "vertex.hpp"
#include "renderer.hpp"
#include "d3d12/d3d12_renderer.hpp"
#include "d3d12/d3d12_model_pool.hpp" 
#include "scene_graph/node.hpp"
#include "scene_graph/light_node.hpp"
#include "scene_graph/scene_graph.hpp"
#include "wisp.hpp"

#include <maya/MEulerRotation.h>
#include <maya/MFnDirectionalLight.h>
#include <maya/MFnLight.h>
#include <maya/MFnPointLight.h>
#include <maya/MFnSpotLight.h>
#include <maya/MFnTransform.h>
#include <maya/MGlobal.h>
#include <maya/MQuaternion.h>

#include <DirectXMath.h>

using namespace DirectX;

// region for internally used functions, these functions cannot be use outside this cpp file
#pragma region INTERNAL_FUNCTIONS

static void updateTransform( MFnTransform& transform, std::shared_ptr<wr::LightNode> mesh_node )
{
	MStatus status = MS::kSuccess;

	MVector pos = transform.getTranslation( MSpace::kTransform, &status );

	MQuaternion qrot;
	status = transform.getRotation( qrot, MSpace::kTransform );
	qrot.normalizeIt();
	MEulerRotation rot = qrot.asEulerRotation();
	rot.reorderIt( MEulerRotation::kZXY );

	double3 scale;
	status = transform.getScale( scale );

	assert( status == MS::kSuccess );

	mesh_node->SetPosition( { static_cast< float >( pos.x ), static_cast< float >( pos.y ), static_cast< float >( pos.z ) } );
	mesh_node->SetRotation( { static_cast< float >( rot.x ), static_cast< float >( rot.y ), static_cast< float >( rot.z ) } );
	mesh_node->SetScale( { static_cast< float >( scale[0] ), static_cast< float >( scale[1] ),static_cast< float >( scale[2] ) } );
}

auto getTransformFindAlgorithm( MFnTransform& transform)
{
	return [ &transform ]( std::pair<MObject, std::shared_ptr<wr::LightNode>> pair ) -> bool
	{
		MStatus status;
		MFnLight fn_light( pair.first );
		MFnDagNode dagnode = fn_light.parent( 0, &status );
		MObject object = dagnode.object();
		MFnTransform transform_rhs( dagnode.object(), &status );
		
		assert( status == MS::kSuccess );

		if( transform.object() == transform_rhs.object() )
		{
			return true;
		}
		return false;
	};
}

#pragma endregion

#pragma region callbacks
namespace wmr
{
	void AttributeLightTransformCallback( MNodeMessage::AttributeMessage msg, MPlug &plug, MPlug &other_plug, void *client_data )
	{
		// Check if attribute was set
		if( !( msg & MNodeMessage::kAttributeSet ) )
		{
			return;
		}

		MStatus status = MS::kSuccess;
		MFnTransform transform( plug.node(), &status );
		if( status != MS::kSuccess )
		{
			MGlobal::displayInfo( status.errorString() );
			return;
		}
		wmr::LightParser* light_parser = reinterpret_cast< wmr::LightParser* >( client_data );

		// specialized find_if algorithm
		auto it = std::find_if( light_parser->m_object_transform_vector.begin(), light_parser->m_object_transform_vector.end(), getTransformFindAlgorithm(transform) );
		
		MFnLight fn_light( it->first );
		MFnDagNode dagnode = fn_light.parent( 0, &status );
		MObject object = dagnode.object();
		MFnTransform transform_rhs( dagnode.object(), &status );
		if( transform_rhs.object() != transform.object() )
		{
			return; // find_if returns last element even if it is not a positive result
		}
		updateTransform( transform, it->second );
	}
}
#pragma endregion

wmr::LightParser::LightParser() :
	m_renderer( dynamic_cast< const ViewportRendererOverride* >(
		MHWRender::MRenderer::theRenderer()->findRenderOverride( settings::VIEWPORT_OVERRIDE_NAME )
		)->GetRenderer() ),
	m_mesh_added_callback_vector(),
	m_object_transform_vector()
{
}

wmr::LightParser::~LightParser()
{
}

void wmr::LightParser::SubscribeObject( MObject & maya_object )
{
	MStatus status = MS::kSuccess;

	MFnLight fn_light( maya_object, &status );
	if( status != MS::kSuccess )
	{
		return;
	}
	LightAdded( fn_light );

}


void wmr::LightParser::UnSubscribeObject( MObject & maya_object )
{
	MStatus status = MS::kSuccess;

	MFnLight fn_light( maya_object );

	auto findCallback = [ &maya_object ]( std::pair<MObject, std::shared_ptr<wr::LightNode>> pair ) -> bool
	{
		if( maya_object == pair.first )
		{
			return true;
		}
		return false;
	};
	auto it = std::find_if( m_object_transform_vector.begin(), m_object_transform_vector.end(), findCallback );
	if( it->first != maya_object )
	{
		assert( false );
		return; // find_if returns last element even if it is not a positive result
	}
	m_renderer.GetScenegraph().DestroyNode( it->second );
	
	auto it_end = --m_object_transform_vector.end();

	if( it == it_end )
	{
		m_object_transform_vector.pop_back();
	}
	else
	{
		std::iter_swap( it, it_end );
		m_object_transform_vector.pop_back();
	}
}

void wmr::LightParser::LightAdded( MFnLight & fn_light )
{

	auto api_type = fn_light.object().apiType();

	m_renderer.GetD3D12Renderer().WaitForAllPreviousWork();
	std::shared_ptr<wr::LightNode> light_node;
	switch( api_type )
	{
	case MFn::Type::kAmbientLight:
		//cry, crash, burn!!
		break;
	case MFn::Type::kPointLight:
	{
		MFnPointLight fn_point_light( fn_light.object() );
		auto light_color = fn_point_light.color();
		DirectX::XMVECTOR wisp_color{ light_color.r ,light_color.g ,light_color.b };
		wisp_color *= fn_point_light.intensity();
		light_node = m_renderer.GetScenegraph().CreateChild<wr::LightNode>(nullptr, wr::LightType::POINT, wisp_color );
		light_node->SetRadius( 20.0f );
	}
	break;
	case MFn::Type::kSpotLight:
	{
		MFnSpotLight fn_spot_light( fn_light.object() );
		auto light_color = fn_spot_light.color();
		DirectX::XMVECTOR wisp_color{ light_color.r ,light_color.g ,light_color.b };
		wisp_color *= fn_spot_light.intensity();
		light_node = m_renderer.GetScenegraph().CreateChild<wr::LightNode>( nullptr, wr::LightType::SPOT, wisp_color );
		light_node->SetAngle(fn_spot_light.coneAngle());
	}
		break;
	case MFn::Type::kDirectionalLight:
	{
		MFnDirectionalLight fn_dir_light( fn_light.object() );
		auto light_color = fn_dir_light.color();
		DirectX::XMVECTOR wisp_color{ light_color.r ,light_color.g ,light_color.b };
		wisp_color *= fn_dir_light.intensity();
		light_node = m_renderer.GetScenegraph().CreateChild<wr::LightNode>( nullptr, wr::LightType::DIRECTIONAL, wisp_color );
	}
		break;
	default:
		break;
	}

	MStatus status;

	MFnDagNode dagnode = fn_light.parent( 0, &status );
	if( status != MS::kSuccess )
	{
		MGlobal::displayError( "Error: " + status.errorString() );
	}

	MObject object = dagnode.object();


	MFnTransform transform( dagnode.object(), &status );
	if( status != MS::kSuccess )
	{
		MGlobal::displayError( "Error: " + status.errorString() );
	}

	updateTransform( transform, light_node );

	m_object_transform_vector.push_back( std::make_pair( fn_light.object(), light_node ) );

	MCallbackId attributeId = MNodeMessage::addAttributeChangedCallback(
		object,
		AttributeLightTransformCallback,
		this,
		&status
	);
	CallbackManager::GetInstance().RegisterCallback( attributeId );

}