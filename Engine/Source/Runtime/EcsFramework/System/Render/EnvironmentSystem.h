#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/Renderer/UniformBuffer.h"

namespace HEngine
{
	class EnvironmentSystem : public System
	{
	public:
		EnvironmentSystem(Level* level) : System(level) {}
		virtual ~EnvironmentSystem() = default;
	public:
		void OnUpdateRuntime(Timestep ts) override;
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
	private:
		void DrawSkyBox(const glm::mat4& ViewMatrix, const glm::mat4& ProjectMatrix);
		void DrawEnvironmentHdr(const glm::mat4& ViewMatrix, const glm::mat4& ProjectMatrix);
	};
}
