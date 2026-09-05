#pragma once

#include "Runtime/Audio/AudioTypes.h"
#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <string>
#include <unordered_map>

namespace HEngine
{
	class AudioScriptSystem : public System
	{
	public:
		AudioScriptSystem(Level* level);
		~AudioScriptSystem() override;
	public:
		void OnRuntiemStart() override;
		void OnUpdateRuntime(Timestep ts) override;
		void OnRuntimeStop() override;
		[[nodiscard]] bool ShouldUpdateWhenRuntimePaused() const override { return true; }

#if WITH_EDITOR
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
#endif
	private:
		struct ComponentPlaybackState
		{
			AudioHandle Handle{};
			std::string Path;
			AudioBus Bus = AudioBus::Gameplay;
			AudioAttenuationModel Attenuation = AudioAttenuationModel::Inverse;
			float MinDistance = 1.0f;
			float MaxDistance = 50.0f;
			float Rolloff = 1.0f;
			float DopplerFactor = 1.0f;
			bool bLooping = false;
			bool bSpatialized = false;
			bool bStream = false;
			glm::vec3 LastPosition{ 0.0f };
			bool bHasLastPosition = false;
		};

		void UpdateSoundComponents(Timestep ts);
		void UpdateRuntimeListener(Timestep ts);

		std::unordered_map<EntityHandle, ComponentPlaybackState> mEntitySounds;
		std::unordered_map<EntityHandle, std::string> mRejectedEntityPaths;
		glm::vec3 mLastListenerPosition{ 0.0f };
		EntityHandle mLastListenerEntity = entt::null;
		bool bHasLastListenerPosition = false;
		bool bWarnedMultipleListeners = false;
	};
}
