#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"

namespace HEngine
{
	class NativeScriptSystem : public System
	{
	public:
		NativeScriptSystem(Level* level) : System(level) {}
		virtual ~NativeScriptSystem() = default;
	public:
		void OnUpdateRuntime(Timestep ts) override;
	};
}