#include "hepch.h"

#include "Runtime/EcsFramework/System/Script/NativeScriptSystem.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/Script/NativeScriptComponent.h"
#include "Runtime/EcsFramework/Entity/ScriptableEntity.h"

namespace HEngine
{
	void NativeScriptSystem::OnUpdateRuntime(Timestep ts)
	{
		mLevel->mRegistry.view<NativeScriptComponent>().each([=](auto entity, auto& nsc)  // nsc: native script component
			{
				// TODO: Move to Level::OnScenePlay
				if (!nsc.Instance)
				{
					nsc.Instance = nsc.InstantiateScript();
					nsc.Instance->mEntity = Entity{ entity, mLevel };
					nsc.Instance->OnCreate();
				}

				nsc.Instance->OnUpdate(ts);
			});
	}
}