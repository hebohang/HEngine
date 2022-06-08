#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"

namespace HEngine
{
	class PythonScriptSystem : public System
	{
	public:
		PythonScriptSystem(Level* level);
		virtual ~PythonScriptSystem() = default;
	public:
		void OnUpdateRuntime(Timestep ts) override;
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
	};
}