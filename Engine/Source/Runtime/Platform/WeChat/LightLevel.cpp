// Selected only by the constrained WeChat target / its native contract host.
// Uses the existing Level/Entity, components and NativeScriptSystem. Export
// rejects all components outside this deliberately small system set.
#include "Hepch.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/EcsFramework/Component/Script/NativeScriptComponent.h"
#include "Runtime/EcsFramework/System/Script/NativeScriptSystem.h"
#include "Runtime/Graphics/Renderer2D.h"
#if defined(HE_SCRIPT_CSHARP_WASM)
#include "Runtime/EcsFramework/System/Script/ScriptSystem.h"
#include "Runtime/EcsFramework/Component/Script/ScriptComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#endif

namespace HEngine
{
    Level::Level()
    {
        mSystems.emplace_back(CreateScope<NativeScriptSystem>(this));
#if defined(HE_SCRIPT_CSHARP_WASM)
        mSystems.emplace_back(CreateScope<ScriptSystem>(this));
#endif
    }
    Level::~Level() { OnRuntimeStop(); mRegistry.clear(); }
    Entity Level::CreateEntity(UUID uuid, const std::string& name)
    {
        const auto handle = mRegistry.create();
        mRegistry.emplace<IDComponent>(handle, uuid);
        mRegistry.emplace<TagComponent>(handle, name);
        mRegistry.emplace<TransformComponent>(handle);
        return Entity(handle, this);
    }
    void Level::DestroyEntity(Entity entity)
    {
        if (!entity) return;
        if (auto* script = mRegistry.try_get<NativeScriptComponent>(entity)) script->Stop();
        if (entity) mRegistry.destroy(entity);
    }
    void Level::OnRuntimeStart()
    {
        if (mRuntimeSystemCount) throw std::logic_error("Level already running");
        mRuntimeSystemCount = mSystems.size();
        mRuntimePaused = false;
        for (auto& system : mSystems) system->OnRuntiemStart();
    }
    void Level::OnRuntimeStop()
    {
        for (size_t i = mRuntimeSystemCount; i > 0; --i) mSystems[i - 1]->OnRuntimeStop();
        mRuntimeSystemCount = 0;
    }
    void Level::OnUpdateRuntime(Timestep ts)
    {
        if (!mRuntimePaused)
            for (size_t i = 0; i < mRuntimeSystemCount; ++i) mSystems[i]->OnUpdateRuntime(ts);
    }
    void Level::SetRuntimePaused(bool bPaused) { mRuntimePaused = bPaused; }
    template<> void Level::OnComponentAdded<NativeScriptComponent>(Entity, NativeScriptComponent&) {}
    template<> void Level::OnComponentAdded<SpriteRendererComponent>(Entity, SpriteRendererComponent&) {}
#if defined(HE_SCRIPT_CSHARP_WASM)
    Entity Level::TryGetEntityByUUID(const UUID& id)
    {
        auto view = mRegistry.view<IDComponent>();
        for (const auto raw : view) if (view.get<IDComponent>(raw).ID == id) return Entity{raw, this};
        return {};
    }
    template<> void Level::OnComponentAdded<ScriptComponent>(Entity, ScriptComponent&) {}
    template<> void Level::OnComponentAdded<CameraComponent>(Entity, CameraComponent& camera) { camera.Camera.SetAspectRatio(camera.Camera.mAspectRatio); }
#endif
}
