#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Core/Meta/Reflection/Reflection.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/EcsFramework/Entity/ScriptableEntity.h"
#include "Runtime/EcsFramework/Component/Script/NativeScriptComponent.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

extern "C" {
    int he_init(const char*, const char*, const char*, const char*);
    int he_shutdown();
    int he_frame(float);
    void he_touch(float, float, int);
    int he_visible(int);
    const char* he_save();
    const char* he_last_error();
    float he_last_delta();
}
using namespace HEngine;
namespace
{
    std::string sLifecycle;
    Level* sOwnerLevel = nullptr;
    Entity sOwner;
    struct Lifecycle : ScriptableEntity
    {
        void OnCreate() override { sLifecycle += 'C'; }
        void OnUpdate(Timestep) override { sLifecycle += 'U'; }
        void OnDestroy() override { sLifecycle += 'D'; }
    };
    struct SelfDelete : Lifecycle
    {
        void OnUpdate(Timestep) override { sLifecycle += 'U'; sOwnerLevel->DestroyEntity(sOwner); }
    };
    struct FailStart : Lifecycle
    {
        void OnCreate() override { sLifecycle += 'C'; throw std::runtime_error("expected native start failure"); }
    };
    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(std::string(message) + ": " + he_last_error());
    }
    void RegistryContracts()
    {
        RuntimeResourceRegistry registry;
        registry.Init(0);
        const auto key = RuntimeResourceKey::FromProjectPath(RuntimeResourceKind::Texture, "Assets/check.png", "wechat-webgl2-v1");
        const auto desktop = RuntimeResourceKey::FromProjectPath(RuntimeResourceKind::Texture, "Assets/check.png", "desktop-v1");
        Require(!(key == desktop), "backend variant isolation");
        auto group = registry.CreateGroup();
        bool rejected = false;
        try { (void)registry.QueueAsync<int, int>(key, [] { return 1; }, [](int&& value) { return std::make_shared<int>(value); }); }
        catch (const std::logic_error&) { rejected = true; }
        Require(rejected && registry.GetStats().PhysicalLoads == 0, "sync-only queue must fail before entry mutation");
        try { (void)registry.AcquireBlocking<int>(key, []() -> std::shared_ptr<int> { throw std::runtime_error("missing fixture"); }); }
        catch (const std::runtime_error&) {}
        Require(registry.GetState(key) == RuntimeResourceState::Failed, "failed loader is cached");
        Require(registry.Retry(key), "failed resource retry");
        auto first = registry.AcquireBlocking<int>(key, [] { return std::make_shared<int>(7); });
        auto second = registry.AcquireBlocking<int>(key, [] { return std::make_shared<int>(9); });
        Require(first.Get() == second.Get() && *second == 7, "same generation lease/cache");
        Require(registry.Invalidate(key), "version invalidation");
        auto next = registry.AcquireBlocking<int>(key, [] { return std::make_shared<int>(11); });
        Require(*first == 7 && *next == 11, "old lease survives generation change");
        first.Reset(); second.Reset(); next.Reset(); group = {};
        (void)registry.CollectUnused(); registry.Shutdown();
    }
    void NativeLifecycleContracts()
    {
        Level level;
        auto owner = level.CreateEntity(UUID("d3b2eacd-9495-4d54-87a4-ed7c06b0c61d"), "Lifecycle");
        auto& script = owner.AddComponent<NativeScriptComponent>();
        script.Bind<Lifecycle>();
        sLifecycle.clear(); level.OnRuntimeStart(); level.OnUpdateRuntime(Timestep(0));
        NativeScriptComponent copy(script);
        Require(!copy.Instance && script.Instance, "native copy must not share runtime ownership");
        level.OnRuntimeStop(); level.OnRuntimeStart(); level.OnUpdateRuntime(Timestep(0)); level.OnRuntimeStop();
        Require(sLifecycle == "CUDCUD", "native stop/restart lifecycle order");
        script.Bind<FailStart>(); sLifecycle.clear(); level.OnRuntimeStart();
        bool failed = false;
        try { level.OnUpdateRuntime(Timestep(0)); } catch (const std::runtime_error&) { failed = true; }
        level.OnRuntimeStop();
        Require(failed && sLifecycle == "CD" && !script.Instance, "failed native startup cleanup");
        script.Bind<SelfDelete>(); sOwner = owner; sOwnerLevel = &level; sLifecycle.clear();
        level.OnRuntimeStart(); level.OnUpdateRuntime(Timestep(0)); level.OnRuntimeStop();
        Require(!owner && sLifecycle == "CUD", "self-removal during update");
        sOwner = {}; sOwnerLevel = nullptr;
        auto first = level.CreateEntity(UUID("21140fdf-082c-44ae-a04d-493b9d8bda74"), "First");
        auto survivor = level.CreateEntity(UUID("31d5dc0d-8935-4cce-8f1a-c6270440c307"), "Survivor");
        first.AddComponent<NativeScriptComponent>().Bind<Lifecycle>();
        survivor.AddComponent<NativeScriptComponent>().Bind<Lifecycle>();
        level.OnRuntimeStart(); level.OnUpdateRuntime(Timestep(0));
        auto* survivingInstance = survivor.GetComponent<NativeScriptComponent>().Instance;
        level.DestroyEntity(first);
        Require(survivor.GetComponent<NativeScriptComponent>().Instance == survivingInstance, "ECS compaction must move runtime script ownership");
        level.OnRuntimeStop();
    }
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 5 && std::string(argv[1]) == "--validate-scene")
        {
            Require(he_init(argv[2], argv[3], argv[4], ""), "native export scene validation");
            Require(he_shutdown(), "native export cleanup");
            std::cout << "PASS native scene decode, reflected components and C++ startup\n";
            return 0;
        }
        if (argc != 4) throw std::runtime_error("Expected project-root scene-path texture-path");
        RegistryContracts();
        NativeLifecycleContracts();
        Require(he_init(argv[1], argv[2], argv[3], ""), "scene load/native registration");
        Require(RuntimeResourceCatalog::GetInstance().GetSnapshot()->Expand(argv[2], RuntimeCatalogExpand::Runtime).size() == 2, "HE catalog scene dependency");
        Require(Reflection::TypeMeta::newMetaFromName("TouchSampleState").isValid(), "generated project reflection");
        const Json initial = Json::parse(he_save());
        he_touch(0.7f, 0.3f, 1); Require(he_frame(0.016f), "touch frame");
        he_touch(0, 0, 0); Require(he_frame(0), "release frame");
        const std::string save = he_save();
        const Json changed = Json::parse(save);
        Require(changed["script"]["Properties"]["Score"] == 1, "native interaction score");
        Require(changed["transform"]["Translation"] != initial["transform"]["Translation"], "native movement");
        he_visible(0); Require(he_frame(10000), "background frame");
        Require(he_last_delta() == 0, "background simulation pause");
        he_visible(1); Require(he_frame(10000), "resume frame");
        Require(he_last_delta() <= 0.05f, "resume delta cap");
        Require(Json::parse(he_save())["transform"] == changed["transform"], "no stuck input after resume");
        Require(he_shutdown(), "shutdown");
        Require(he_init(argv[1], argv[2], argv[3], save.c_str()), "restart with saved state");
        Require(Json::parse(he_save()) == changed, "generated state round trip");
        he_touch(0, 0, 1); he_touch(0, 0, 0);
        Require(Json::parse(he_save())["script"]["Properties"]["Score"] == 2, "tap buffered between frames and flushed to save");
        Require(he_shutdown(), "second shutdown");
        Require(!he_init(argv[1], "Assets/does-not-exist.he", argv[3], ""), "missing scene failure");
        Require(!he_init(argv[1], argv[2], "Assets/does-not-exist.png", ""), "missing texture failure");
        Require(!he_init(argv[1], argv[2], argv[3], "{\"schema\":99}"), "corrupt save failure");
        Require(he_shutdown(), "failed startup cleanup");
        std::cout << "PASS native optimized HE scene/resource/script/save/lifecycle contracts\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        (void)he_shutdown();
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
