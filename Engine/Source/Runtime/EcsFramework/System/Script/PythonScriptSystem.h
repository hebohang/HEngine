#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"

namespace HEngine
{
	class PythonScriptSystem : public System
	{
	public:
		PythonScriptSystem(Level* level) : System(level) {}
		virtual ~PythonScriptSystem() = default;
	public:
		void OnUpdateRuntime(Timestep ts) override;
	};
}