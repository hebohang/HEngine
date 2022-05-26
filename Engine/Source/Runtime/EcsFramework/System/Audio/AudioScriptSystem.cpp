#include "hepch.h"

#include "Runtime/EcsFramework/System/Audio/AudioScriptSystem.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"

namespace HEngine
{
	AudioScriptSystem::AudioScriptSystem(Level* level)
		: System(level)
	{
	}

	void AudioScriptSystem::OnRuntiemStart()
	{
		FMOD::System_Create(&mFmodSystem);
		mFmodSystem->init(32, FMOD_INIT_NORMAL, 0);



		auto view = mLevel->mRegistry.view<TransformComponent, SoundComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& sc = entity.GetComponent<SoundComponent>();

			mFmodSystem->createSound(AssetManager::GetFullPath(sc.Path).string().c_str(), FMOD_DEFAULT, 0, &sc.Sound);

			mFmodSystem->playSound(sc.Sound, nullptr, false, &sc.Channel);
		}
	}

	void AudioScriptSystem::OnUpdateRuntime(Timestep ts)
	{
		mFmodSystem->update();
	}

	void AudioScriptSystem::OnRuntimeStop()
	{
		mFmodSystem->close();
		mFmodSystem->release();
	}

	void AudioScriptSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{

	}
}