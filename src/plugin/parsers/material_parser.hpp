#pragma once

#include <vector>

// Maya API
#include <maya/MApiNamespace.h>

// C++ standard
#include <optional>

namespace wr
{
	class MaterialHandle;
	class Material;
}

namespace wmr
{
	class Renderer;

	namespace detail
	{
		enum class SurfaceShaderType
		{
			UNSUPPORTED = -1,

			LAMBERT,
			PHONG,
		};
	}

	struct MaterialData
	{
	};

	class MaterialParser
	{
	public:
		MaterialParser();
		~MaterialParser() = default;

		void Parse(const MFnMesh& mesh);
		const std::optional<MObject> GetMeshObjectFromMaterial(MObject & object);
		void HandleMaterialChange(MFnDependencyNode &fn, MPlug & plug, MString & plug_name, wr::Material & material);
		const Renderer & GetRenderer();

	private:
		const detail::SurfaceShaderType GetShaderType(const MObject& node);
		const MString GetPlugTexture(MPlug& plug);
		const MPlug GetPlugByName(const MObject& node, MString name);
		const std::optional<MPlug> GetSurfaceShader(const MObject& node);

		// Material parsing
		MColor GetColor(MFnDependencyNode & fn);

		// std::pair
		//    first: MObject, connected lambert plug
		//    second: MObject, MFnMesh.object()
		std::vector<std::pair<MObject, MObject>> mesh_material_relations;

		Renderer& m_renderer;
	};
}