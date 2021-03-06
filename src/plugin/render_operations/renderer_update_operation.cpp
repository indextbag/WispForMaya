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

#include "renderer_update_operation.hpp"

// Wisp plug-in
#include "miscellaneous/settings.hpp"
#include "plugin/renderer/renderer.hpp"
#include "plugin/viewport_renderer_override.hpp"

// Maya API
#include <maya/MViewport2Renderer.h>

namespace wmr
{
	RendererUpdateOperation::RendererUpdateOperation(const MString & name)
		: MHWRender::MUserRenderOperation(name)
		, m_renderer(dynamic_cast<const ViewportRendererOverride*>(MHWRender::MRenderer::theRenderer()->findRenderOverride(settings::VIEWPORT_OVERRIDE_NAME))->GetRenderer())
	{
		LOG("Attempting to get a reference to the renderer.");
	}

	RendererUpdateOperation::~RendererUpdateOperation()
	{
	}

	const MCameraOverride* RendererUpdateOperation::cameraOverride()
	{
		// Not using a camera override
		return nullptr;
	}

	MStatus RendererUpdateOperation::execute(const MDrawContext& draw_context)
	{
		// Update the Wisp rendering framework to prepare it for rendering
		m_renderer.Update();

		return MStatus::kSuccess;
	}

	bool RendererUpdateOperation::hasUIDrawables() const
	{
		// Not using any UI drawable
		return false;
	}

	bool RendererUpdateOperation::requiresLightData() const
	{
		// This operation does not require any light data
		return false;
	}
}
