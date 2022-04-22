#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"

namespace HEngine
{
	class RenderSystem3D : public System
	{
	public:
		RenderSystem3D(Level* level) : System(level) {}
		virtual ~RenderSystem3D() = default;
	public:
		void OnUpdateRuntime(Timestep ts) override;
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
	};
}
