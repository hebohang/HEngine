#include "Hepch.h"

#include "Runtime/EcsFramework/System/Audio/AudioScriptSystem.h"

#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Audio/ProjectAudioPath.h"
#include "Runtime/EcsFramework/Component/Audio/AudioListenerComponent.h"
#include "Runtime/EcsFramework/Component/Audio/SoundComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace HEngine
{
	namespace
	{
		constexpr float kMinimumMotionDeltaSeconds = 0.0001f;
		constexpr float kEmitterTeleportDistance = 25.0f;
		constexpr float kEmitterTeleportDistanceSquared =
			kEmitterTeleportDistance * kEmitterTeleportDistance;

		AudioBus ResolveAudioBus(AudioBus bus)
		{
			switch (bus)
			{
			case AudioBus::Music:
			case AudioBus::Gameplay:
			case AudioBus::UI:
				return bus;
			default:
				return AudioBus::Gameplay;
			}
		}

		AudioAttenuationModel ResolveAttenuation(AudioAttenuationModel attenuation)
		{
			switch (attenuation)
			{
			case AudioAttenuationModel::None:
			case AudioAttenuationModel::Inverse:
			case AudioAttenuationModel::Linear:
			case AudioAttenuationModel::Exponential:
				return attenuation;
			default:
				return AudioAttenuationModel::Inverse;
			}
		}

		AudioPlayParams BuildPlayParams(const SoundComponent& component, const glm::vec3& position)
		{
			AudioPlayParams params;
			params.Bus = ResolveAudioBus(component.Bus);
			params.Position = position;
			params.Volume = std::clamp(component.Volume, 0.0f, 1.0f);
			params.Pitch = std::max(component.Pitch, 0.01f);
			params.Attenuation = ResolveAttenuation(component.Attenuation);
			params.MinDistance = std::max(component.MinDistance, 0.01f);
			params.MaxDistance = std::max(component.MaxDistance, params.MinDistance);
			params.Rolloff = std::max(component.Rolloff, 0.0f);
			params.DopplerFactor = std::max(component.DopplerFactor, 0.0f);
			params.bLooping = component.bLooping;
			params.bSpatialized = component.bSpatialized;
			params.bStream = component.bStream;
			return params;
		}

		void SetListenerOrientation(AudioListener& listener, const glm::vec3& rotation)
		{
			const glm::quat orientation(rotation);
			listener.Forward = glm::normalize(orientation * glm::vec3(0.0f, 0.0f, -1.0f));
			listener.Up = glm::normalize(orientation * glm::vec3(0.0f, 1.0f, 0.0f));
		}
	}

	AudioScriptSystem::AudioScriptSystem(Level* level)
		: System(level)
	{
	}

	AudioScriptSystem::~AudioScriptSystem()
	{
		OnRuntimeStop();
	}

	void AudioScriptSystem::OnRuntiemStart()
	{
		OnRuntimeStop();
		bHasLastListenerPosition = false;
		bWarnedMultipleListeners = false;
		UpdateRuntimeListener(Timestep{});
		UpdateSoundComponents(Timestep{});
	}

	void AudioScriptSystem::OnUpdateRuntime(Timestep ts)
	{
		UpdateRuntimeListener(ts);
		UpdateSoundComponents(ts);
	}

	void AudioScriptSystem::OnRuntimeStop()
	{
		auto& audio = AudioEngine::GetInstance();
		for (const auto& [entityHandle, state] : mEntitySounds)
			audio.Stop(state.Handle);
		mEntitySounds.clear();
		mRejectedEntityPaths.clear();

		mLastListenerEntity = entt::null;
		bHasLastListenerPosition = false;
		bWarnedMultipleListeners = false;
	}

	void AudioScriptSystem::UpdateSoundComponents(Timestep ts)
	{
		auto& audio = AudioEngine::GetInstance();
		std::unordered_set<EntityHandle> activeEntities;
		auto view = mLevel->mRegistry.view<TransformComponent, SoundComponent>();
		activeEntities.reserve(view.size_hint());

		for (auto entityHandle : view)
		{
			activeEntities.insert(entityHandle);
			Entity entity{ entityHandle, mLevel };
			const auto& component = view.get<SoundComponent>(entityHandle);
			auto stateIt = mEntitySounds.find(entityHandle);

			if (!component.Play || component.Path.empty() || component.Path == "None")
			{
				mRejectedEntityPaths.erase(entityHandle);
				if (stateIt != mEntitySounds.end())
				{
					audio.Stop(stateIt->second.Handle);
					mEntitySounds.erase(stateIt);
				}
				continue;
			}

			ProjectAudioPath audioPath;
			const Utils::PathValidationResult pathResult = TryResolveProjectAudioPath(
				ConfigManager::GetInstance().GetRootFolder(),
				component.Path,
				audioPath);
			if (!pathResult)
			{
				auto rejectedPath = mRejectedEntityPaths.find(entityHandle);
				if (rejectedPath == mRejectedEntityPaths.end() || rejectedPath->second != component.Path)
				{
					HE_CORE_WARN("Rejected SoundComponent path '{0}': {1}",
						component.Path, pathResult.Message);
					mRejectedEntityPaths[entityHandle] = component.Path;
				}
				if (stateIt != mEntitySounds.end())
				{
					audio.Stop(stateIt->second.Handle);
					mEntitySounds.erase(stateIt);
				}
				continue;
			}
			mRejectedEntityPaths.erase(entityHandle);

			const glm::vec3 worldPosition = mLevel->GetWorldTranslation(entity);
			const AudioPlayParams params = BuildPlayParams(component, worldPosition);
			const bool bNeedsRestart = stateIt != mEntitySounds.end() &&
				(stateIt->second.Path != audioPath.NormalizedPath ||
				 stateIt->second.Bus != params.Bus ||
				 stateIt->second.Attenuation != params.Attenuation ||
				 stateIt->second.MinDistance != params.MinDistance ||
				 stateIt->second.MaxDistance != params.MaxDistance ||
				 stateIt->second.Rolloff != params.Rolloff ||
				 stateIt->second.DopplerFactor != params.DopplerFactor ||
				 stateIt->second.bLooping != params.bLooping ||
				 stateIt->second.bSpatialized != params.bSpatialized ||
				 stateIt->second.bStream != params.bStream);
			if (bNeedsRestart)
			{
				audio.Stop(stateIt->second.Handle);
				stateIt = mEntitySounds.erase(stateIt);
			}

			if (stateIt == mEntitySounds.end())
			{
				ComponentPlaybackState state;
				state.Handle = audio.Play(audioPath.FullPath, params);
				if (!state.Handle)
					continue;
				state.Path = audioPath.NormalizedPath;
				state.Bus = params.Bus;
				state.Attenuation = params.Attenuation;
				state.MinDistance = params.MinDistance;
				state.MaxDistance = params.MaxDistance;
				state.Rolloff = params.Rolloff;
				state.DopplerFactor = params.DopplerFactor;
				state.bLooping = params.bLooping;
				state.bSpatialized = params.bSpatialized;
				state.bStream = params.bStream;
				state.LastPosition = worldPosition;
				state.bHasLastPosition = true;
				stateIt = mEntitySounds.emplace(entityHandle, std::move(state)).first;
			}

			if (stateIt->second.Handle)
			{
				audio.SetVolume(stateIt->second.Handle, params.Volume);
				audio.SetPitch(stateIt->second.Handle, params.Pitch);
				if (params.bSpatialized)
				{
					glm::vec3 velocity{ 0.0f };
					const float deltaSeconds = ts.GetSeconds();
					if (stateIt->second.bHasLastPosition && deltaSeconds > kMinimumMotionDeltaSeconds)
					{
						const glm::vec3 displacement = worldPosition - stateIt->second.LastPosition;
						if (glm::dot(displacement, displacement) <= kEmitterTeleportDistanceSquared)
							velocity = displacement / deltaSeconds;
					}
					audio.SetPosition(stateIt->second.Handle, worldPosition);
					audio.SetVelocity(stateIt->second.Handle, velocity);
				}
				stateIt->second.LastPosition = worldPosition;
				stateIt->second.bHasLastPosition = true;
			}
		}

		for (auto stateIt = mEntitySounds.begin(); stateIt != mEntitySounds.end();)
		{
			if (!activeEntities.contains(stateIt->first))
			{
				audio.Stop(stateIt->second.Handle);
				stateIt = mEntitySounds.erase(stateIt);
			}
			else
			{
				++stateIt;
			}
		}

		for (auto rejectedPath = mRejectedEntityPaths.begin(); rejectedPath != mRejectedEntityPaths.end();)
		{
			if (!activeEntities.contains(rejectedPath->first))
				rejectedPath = mRejectedEntityPaths.erase(rejectedPath);
			else
				++rejectedPath;
		}
	}

	void AudioScriptSystem::UpdateRuntimeListener(Timestep ts)
	{
		AudioListener listener;
		bool bFoundListener = false;
		bool bUsePrimaryCameraOrientation = false;
		EntityHandle listenerEntity = entt::null;

		auto listenerView = mLevel->mRegistry.view<TransformComponent, AudioListenerComponent>();
		EntityHandle selectedListener = entt::null;
		size_t activeListenerCount = 0;
		for (auto entityHandle : listenerView)
		{
			const auto& component = listenerView.get<AudioListenerComponent>(entityHandle);
			if (!component.bActive)
				continue;

			++activeListenerCount;
			if (selectedListener == entt::null ||
				static_cast<uint32_t>(entityHandle) < static_cast<uint32_t>(selectedListener))
			{
				selectedListener = entityHandle;
			}
		}

		if (activeListenerCount > 1 && !bWarnedMultipleListeners)
		{
			HE_CORE_WARN("Multiple active AudioListenerComponents found ({0}); using entity {1}",
				activeListenerCount, static_cast<uint32_t>(selectedListener));
		}
		bWarnedMultipleListeners = activeListenerCount > 1;

		if (selectedListener != entt::null)
		{
			const auto& component = listenerView.get<AudioListenerComponent>(selectedListener);
			Entity entity{ selectedListener, mLevel };
			listener.Position = mLevel->GetWorldTranslation(entity);
			SetListenerOrientation(listener, mLevel->GetWorldRotation(entity));
			bUsePrimaryCameraOrientation = component.bUsePrimaryCameraOrientation;
			listenerEntity = selectedListener;
			bFoundListener = true;
		}

		Entity primaryCamera = mLevel->GetPrimaryCameraEntity();
		if (primaryCamera && (!bFoundListener || bUsePrimaryCameraOrientation))
		{
			if (!bFoundListener)
			{
				listener.Position = mLevel->GetWorldTranslation(primaryCamera);
				listenerEntity = primaryCamera;
			}
			SetListenerOrientation(listener, mLevel->GetWorldRotation(primaryCamera));
			bFoundListener = true;
		}

		listener.bEnabled = bFoundListener;
		const float deltaSeconds = ts.GetSeconds();
		if (bFoundListener && bHasLastListenerPosition && listenerEntity == mLastListenerEntity && deltaSeconds > 0.0001f)
			listener.Velocity = (listener.Position - mLastListenerPosition) / deltaSeconds;
		if (bFoundListener)
		{
			mLastListenerPosition = listener.Position;
			mLastListenerEntity = listenerEntity;
			bHasLastListenerPosition = true;
		}
		else
		{
			mLastListenerEntity = entt::null;
			bHasLastListenerPosition = false;
		}

		AudioEngine::GetInstance().SetListener(0, listener);
	}

#if WITH_EDITOR
	void AudioScriptSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		AudioListener listener;
		listener.Position = camera.GetPosition();
		listener.Forward = camera.GetForwardDirection();
		listener.Up = camera.GetUpDirection();
		AudioEngine::GetInstance().SetListener(0, listener);
	}
#endif
}
