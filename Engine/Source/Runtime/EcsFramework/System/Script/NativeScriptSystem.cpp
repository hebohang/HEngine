#include "Hepch.h"

#include "Runtime/EcsFramework/System/Script/NativeScriptSystem.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/Script/NativeScriptComponent.h"
#include "Runtime/EcsFramework/Entity/ScriptableEntity.h"

namespace HEngine
{
	void NativeScriptSystem::OnUpdateRuntime(Timestep ts)
	{
		HE_PROFILE;

		// Snapshot handles: callbacks may create/remove entities and invalidate a view.
		std::vector<EntityHandle> owners;
		for (auto owner : mLevel->mRegistry.view<NativeScriptComponent>()) owners.push_back(owner);
		for (auto owner : owners)
		{
			auto current = [&]() -> NativeScriptComponent*
			{
				return mLevel->mRegistry.valid(owner) ? mLevel->mRegistry.try_get<NativeScriptComponent>(owner) : nullptr;
			};
			auto* script = current();
			if (!script) continue;
			if (!script->Instance)
			{
				script->ResolveType();
				script->Instance = script->InstantiateScript();
				if (!script->Instance) throw std::runtime_error("Native script factory returned null: " + script->Type);
				auto* instance = script->Instance;
				instance->mEntity = Entity{ owner, mLevel };
				try
				{
                    instance->LoadProperties(script->Properties);
					if (auto* remaining = current(); remaining && remaining->Instance == instance) instance->OnCreate();
				}
				catch (...) { if (auto* remaining = current(); remaining && remaining->Instance == instance) remaining->Stop(); throw; }
			}
			script = current();
			if (!script || !script->Instance) continue;
			auto* instance = script->Instance;
			instance->OnUpdate(ts);
			script = current();
			if (script && script->Instance == instance && !script->Type.empty())
			{
				Json properties = instance->SaveProperties();
				script = current();
				if (script && script->Instance == instance) script->Properties = std::move(properties);
			}
		}
	}

	void NativeScriptSystem::OnRuntimeStop()
	{
		std::vector<EntityHandle> owners;
		for (auto owner : mLevel->mRegistry.view<NativeScriptComponent>()) owners.push_back(owner);
		for (auto owner : owners)
			if (mLevel->mRegistry.valid(owner))
				if (auto* script = mLevel->mRegistry.try_get<NativeScriptComponent>(owner)) script->Stop();
	}
}
