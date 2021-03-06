// Copyright 2019 Breda University of Applied Sciences and Team Wisp (Viktor Zoutman, Emilio Laiso, Jens Hagen, Meine Zeinstra, Tahar Meijs, Koen Buitenhuis, Niels Brunekreef, Darius Bouma, Florian Schut)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "material_manager.hpp"

#include "plugin/parsers/model_parser.hpp"
#include "plugin/parsers/scene_graph_parser.hpp"
#include "plugin/renderer/renderer.hpp"
#include "plugin/viewport_renderer_override.hpp"
#include "plugin/renderer/texture_manager.hpp"
#include "miscellaneous/settings.hpp"

#include "d3d12/d3d12_renderer.hpp"
#include "scene_graph/mesh_node.hpp"
#include "util/log.hpp"
#include "texture_manager.hpp"

#include <maya/MFnMesh.h>
#include <maya/MFnTransform.h>
#include <maya/MGlobal.h>
#include <maya/MViewport2Renderer.h>


wmr::MaterialManager::MaterialManager()
	: m_scenegraph_parser(nullptr)
	, m_default_material_handle({ nullptr, 0 })
{
}

void wmr::MaterialManager::Initialize()
{
	const auto& renderer = dynamic_cast<const ViewportRendererOverride*>(MHWRender::MRenderer::theRenderer()->findRenderOverride(settings::VIEWPORT_OVERRIDE_NAME))->GetRenderer();
	m_texture_manager = &renderer.GetTextureManager();
	m_material_pool = renderer.GetD3D12Renderer().CreateMaterialPool(0);
	m_default_material_handle = m_material_pool->Create(m_texture_manager->GetTexturePool().get());

	wr::Material* internal_material = m_material_pool->GetMaterial(m_default_material_handle);

	internal_material->SetConstant<wr::MaterialConstant::COLOR>({ 1.0f, 1.0f, 1.0f } );
	internal_material->SetConstant<wr::MaterialConstant::METALLIC>(1.0f);
	internal_material->SetConstant<wr::MaterialConstant::ROUGHNESS>(1.0f);
}

void wmr::MaterialManager::Destroy() noexcept
{
	m_material_pool.reset();
	m_mesh_shading_relations.clear();
	m_surface_shader_shading_relations.clear();
}

wr::MaterialHandle wmr::MaterialManager::GetDefaultMaterial() noexcept
{
	return m_default_material_handle;
}

wr::MaterialHandle wmr::MaterialManager::CreateMaterial(MObject& mesh, MObject &shading_engine, MPlug &surface_shader)
{
	wr::MaterialHandle material_handle = ConnectShaderToShadingEngine(surface_shader, shading_engine);

	ConnectMeshToShadingEngine(mesh, shading_engine, &material_handle);

	return material_handle;
}

wmr::SurfaceShaderShadingEngineRelation * wmr::MaterialManager::OnCreateSurfaceShader(MPlug & surface_shader)
{
	MObject surface_shader_obj = surface_shader.node();
	auto relation = DoesSurfaceShaderExist(surface_shader_obj);
	if (relation != nullptr)
	{
		LOGE("Surface shader shading engine relation was not nullptr in plug \"{}\".", surface_shader.name().asChar());
		return nullptr;
	}
	// Surface shader doesn't have a material assigned to it yet
	// Create Wisp Material handle
	wr::MaterialHandle material_handle = m_material_pool->Create(&*m_texture_manager->GetTexturePool());
	// Create relationship between surface shader and shading engine
	m_surface_shader_shading_relations.push_back({
		material_handle,		// Wisp Material handle
		surface_shader,			// Maya surface shader plug
		std::vector<MObject>()	// Vector of shading engines
	});

	return &m_surface_shader_shading_relations[m_surface_shader_shading_relations.size() - 1];
}

void wmr::MaterialManager::OnRemoveSurfaceShader(MPlug & surface_shader)
{
	LOG("Starting surface shader removal of \"{}\".", surface_shader.name().asChar());

	// Find surface shader
	auto it = std::find_if(m_surface_shader_shading_relations.begin(), m_surface_shader_shading_relations.end(), [&surface_shader] (const std::vector<SurfaceShaderShadingEngineRelation>::value_type& vt)
	{
		return (vt.surface_shader == surface_shader);
	});
	// Set default material to all meshes that use this surface shader if it is found
	if (it != m_surface_shader_shading_relations.end())
	{
		MObject shading_engine;
		// Search for all shading engines until they are all removed from the meshes
		while (it->shading_engines.size() > 0)
		{
			shading_engine = *--it->shading_engines.end();
			// Find the mesh that the shading engine is attached to
			for (int i = m_mesh_shading_relations.size() - 1; i >= 0; --i)
			{
				// Bind default material to mesh when the shading engine has been found
				if (m_mesh_shading_relations[i].shading_engine == shading_engine)
				{
					auto mesh_shading_it = (m_mesh_shading_relations.begin() + i);
					ApplyMaterialToModel(m_default_material_handle, mesh_shading_it->shading_engine);
					m_mesh_shading_relations.erase(mesh_shading_it);
				}
			}
			it->shading_engines.pop_back();
		}

		// Clear all textures
		wr::Material* m = m_material_pool->GetMaterial(it->material_handle);
		{
			// Clear albedo texture
			if (m->HasTexture(wr::TextureType::ALBEDO)) {
				m_texture_manager->MarkTextureUnused(m->GetTexture(wr::TextureType::ALBEDO));
				m->ClearTexture(wr::TextureType::ALBEDO);
			}
			// Clear AO texture
			if (m->HasTexture(wr::TextureType::AO)) {
				m_texture_manager->MarkTextureUnused(m->GetTexture(wr::TextureType::AO));
				m->ClearTexture(wr::TextureType::AO);
			}
			// Clear emissive texture
			if (m->HasTexture(wr::TextureType::EMISSIVE)) {
				m_texture_manager->MarkTextureUnused(m->GetTexture(wr::TextureType::EMISSIVE));
				m->ClearTexture(wr::TextureType::EMISSIVE);
			}
			// Clear metallic texture
			if (m->HasTexture(wr::TextureType::METALLIC)) {
				m_texture_manager->MarkTextureUnused(m->GetTexture(wr::TextureType::METALLIC));
				m->ClearTexture(wr::TextureType::METALLIC);
			}
			// Clear normal texture
			if (m->HasTexture(wr::TextureType::NORMAL)) {
				m_texture_manager->MarkTextureUnused(m->GetTexture(wr::TextureType::NORMAL));
				m->ClearTexture(wr::TextureType::NORMAL);
			}
			// Clear roughness texture
			if (m->HasTexture(wr::TextureType::ROUGHNESS)) {
				m_texture_manager->MarkTextureUnused(m->GetTexture(wr::TextureType::ROUGHNESS));
				m->ClearTexture(wr::TextureType::ROUGHNESS);
			}
		}

		// Remove surface shader from vector
		m_surface_shader_shading_relations.erase(it);
	}

	LOG("Finished surface shader removal.");
}

wr::MaterialHandle wmr::MaterialManager::ConnectShaderToShadingEngine(MPlug & surface_shader, MObject & shading_engine, bool apply_material)
{
	LOG("Starting shader \"{}\" connection to shading engine of type \"{}\".", surface_shader.name().asChar(), shading_engine.apiTypeStr());

	// Find surface shader relationships
	MObject surface_shader_obj = surface_shader.node();
	auto relation = DoesSurfaceShaderExist(surface_shader_obj);
	if (relation != nullptr)
	{
		// Add shading engine if surface shader doesn't have a relation with the shading engine
		auto shading_engine_it = relation->FindShadingEngine(shading_engine);
		if (shading_engine_it == relation->shading_engines.end())
		{
			relation->shading_engines.push_back(shading_engine);
		}

		// Get mesh and bind surface shader
		if (apply_material)
		{
			auto it = std::find_if(m_mesh_shading_relations.begin(), m_mesh_shading_relations.end(), [this, &relation, &shading_engine] (const std::vector<MeshShadingEngineRelation>::value_type& vt)
			{
				if (vt.shading_engine == shading_engine)
				{
					MObject obj = vt.mesh;
					this->ApplyMaterialToModel(relation->material_handle, obj);
				}
				return false;
			});
		}

		return relation->material_handle;
	}
	// Surface shader doesn't have a material assigned to it yet
	// Create Wisp Material handle
	wr::MaterialHandle material_handle = m_material_pool->Create(m_texture_manager->GetTexturePool().get());
	// Create a vector for the shading engines
	std::vector<MObject> shading_engines;
	shading_engines.push_back(shading_engine);
	// Create relationship between surface shader and shading engine
	m_surface_shader_shading_relations.push_back({
		material_handle,		// Wisp Material handle
		surface_shader,			// Maya surface shader plug
		shading_engines			// Vector of shading engines
	});

	// Get mesh and bind surface shader
	if (apply_material)
	{
		auto it = std::find_if(m_mesh_shading_relations.begin(), m_mesh_shading_relations.end(), [this, &material_handle, &shading_engine] (const std::vector<MeshShadingEngineRelation>::value_type& vt)
		{
			if (vt.shading_engine == shading_engine)
			{
				MObject obj = vt.mesh;
				this->ApplyMaterialToModel(material_handle, obj);
			}
			return false;
		});
	}

	LOG("Finished connecting shader connection to shading engine.");

	return material_handle;
}

void wmr::MaterialManager::DisconnectShaderFromShadingEngine(MPlug & surface_shader, MObject & shading_engine)
{
	MObject surface_shader_obj = surface_shader.node();
	auto relation = DoesSurfaceShaderExist(surface_shader_obj);
	if (relation != nullptr)
	{
		auto shading_engine_it = relation->FindShadingEngine(shading_engine);
		if (shading_engine_it != relation->shading_engines.end())
		{
			relation->shading_engines.erase(shading_engine_it);
		}
	}
}

void wmr::MaterialManager::ConnectMeshToShadingEngine(MObject & mesh, MObject & shading_engine, wr::MaterialHandle * material_handle)
{
	auto it = std::find_if(m_mesh_shading_relations.begin(), m_mesh_shading_relations.end(), [&mesh] (const std::vector<MeshShadingEngineRelation>::value_type& vt)
	{
		return (vt.mesh == mesh);
	});
	// If the mesh already has a shading engine
	if (it != m_mesh_shading_relations.end())
	{
		it->shading_engine = shading_engine;
	}
	// If the mesh was not found
	else
	{
		MeshShadingEngineRelation newRelation;
		newRelation.mesh = mesh;
		newRelation.shading_engine = shading_engine;
		m_mesh_shading_relations.push_back(newRelation);
	}

	if (material_handle == nullptr)
	{
		wr::MaterialHandle handle = FindWispMaterialByShadingEngine(shading_engine);
		ApplyMaterialToModel(handle, mesh);
		return;
	}
	ApplyMaterialToModel(*material_handle, mesh);
}

void wmr::MaterialManager::DisconnectMeshFromShadingEngine(MObject & mesh, MObject & shading_engine, bool reset_material)
{
	auto it = std::find_if(m_mesh_shading_relations.begin(), m_mesh_shading_relations.end(), [&mesh, &shading_engine] (const std::vector<MeshShadingEngineRelation>::value_type& vt)
	{
		return (vt.mesh == mesh && vt.shading_engine == shading_engine);
	});
	// Found the relation between the two given parameters (mesh and shading engine)
	if (it != m_mesh_shading_relations.end())
	{
		m_mesh_shading_relations.erase(it);

		if (reset_material)
		{
			ApplyMaterialToModel(m_default_material_handle, mesh);
		}
	}
}

wr::Material * wmr::MaterialManager::GetWispMaterial(wr::MaterialHandle & material_handle)
{
	return m_material_pool->GetMaterial(material_handle);
}

wmr::SurfaceShaderShadingEngineRelation * wmr::MaterialManager::DoesMaterialHandleExist(wr::MaterialHandle & material_handle)
{
	// Search relationships for material handle
	for (auto& relation : m_surface_shader_shading_relations)
	{
		if (relation.material_handle == material_handle)
		{
			return &relation;
		}
	}
	return nullptr;
}

wmr::SurfaceShaderShadingEngineRelation * wmr::MaterialManager::DoesShaderEngineExist(MObject & shading_engine)
{
	auto it = std::find_if(m_surface_shader_shading_relations.begin(), m_surface_shader_shading_relations.end(), [&shading_engine] (const std::vector<SurfaceShaderShadingEngineRelation>::value_type& vt)
	{
		auto end_it = vt.shading_engines.end();
		auto shading_engines_it = std::find_if(vt.shading_engines.begin(), end_it, [&shading_engine] (const std::vector<MObject>::value_type& vt)
		{
			return (vt == shading_engine);
		});

		return (shading_engines_it != end_it);
	});

	if (it != m_surface_shader_shading_relations.end())
	{
		return &*it;
	}

	return nullptr;
}

wmr::SurfaceShaderShadingEngineRelation * wmr::MaterialManager::DoesSurfaceShaderExist(MObject & surface_shader)
{
	// Search relationships for shading engines
	for (auto& relation : m_surface_shader_shading_relations)
	{
		if (relation.surface_shader.node() == surface_shader)
		{
			return &relation;
		}
	}
	return nullptr;
}

wmr::ScenegraphParser * wmr::MaterialManager::GetSceneParser()
{
	if (m_scenegraph_parser == nullptr)
	{
		m_scenegraph_parser = &dynamic_cast<const ViewportRendererOverride*>(
			MHWRender::MRenderer::theRenderer()->findRenderOverride(settings::VIEWPORT_OVERRIDE_NAME)
			)->GetSceneGraphParser();

		LOG("Attempting to get a reference to the scenegraph parser via the renderer.");
	}
	return m_scenegraph_parser;
}

wr::MaterialHandle wmr::MaterialManager::FindWispMaterialByShadingEngine(MObject & shading_engine)
{
	wmr::SurfaceShaderShadingEngineRelation * relation = DoesShaderEngineExist(shading_engine);

	if (relation != nullptr)
	{
		return relation->material_handle;
	}
	return m_default_material_handle;
}

wr::MaterialHandle wmr::MaterialManager::FindWispMaterialBySurfaceShader(MObject & surface_shader)
{
	wmr::SurfaceShaderShadingEngineRelation * relation = DoesSurfaceShaderExist(surface_shader);

	if (relation != nullptr)
	{
		return relation->material_handle;
	}
	return m_default_material_handle;
}

void wmr::MaterialManager::ApplyMaterialToModel(wr::MaterialHandle & material_handle, MObject & fnmesh)
{
	std::shared_ptr<wr::MeshNode> wr_mesh_node = GetSceneParser()->GetModelParser().GetWRModel(fnmesh);
	if (wr_mesh_node != nullptr)
	{
		wr::Model* wr_model = wr_mesh_node->m_model;
		for (auto& mesh : wr_model->m_meshes)
		{
			mesh.second = material_handle;
		}
	}
}

std::vector<MObject>::iterator wmr::SurfaceShaderShadingEngineRelation::FindShadingEngine(MObject & shading_engine)
{
	auto it = std::find_if(shading_engines.begin(), shading_engines.end(), [&shading_engine] (const std::vector<MObject>::value_type& vt)
	{
		return (vt == shading_engine);
	});
	return it;
}
