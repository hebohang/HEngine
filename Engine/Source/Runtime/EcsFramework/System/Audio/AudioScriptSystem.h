#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <fmod.hpp>

namespace HEngine
{
	class AudioScriptSystem : public System
	{
	public:
		AudioScriptSystem(Level* level);
		virtual ~AudioScriptSystem() = default;
	public:
		void OnRuntiemStart() override;
		void OnUpdateRuntime(Timestep ts) override;
		void OnRuntimeStop() override;
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
	private:
		FMOD::System* mFmodSystem;
		FMOD::Sound* mSound1;
		FMOD::Channel* mChannel = 0;
	};
}