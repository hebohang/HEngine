#include "PythonRuntimeTests.h"
#include "Plugins/PythonScript/PythonBackend.h"
#include "Runtime/EcsFramework/Component/Script/ScriptComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/AI/BehaviorTreeComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/FollowCameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/SpringArmComponent.h"
#include "Runtime/EcsFramework/System/Script/ScriptSystem.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Platform/Scripting/ScriptPlatformServices.h"
#include "Runtime/Scripting/ScriptEngine.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"

#include <HEngine/ProjectBuildConfig.h>

#include <stdexcept>
#include <vector>

namespace HEngine::Tests
{
    void PythonRuntimeContract()
    {
        const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
        const auto saved = std::filesystem::temp_directory_path() / ("HEngine-Python-" + UUID().str());
        std::filesystem::create_directories(saved);
        struct Cleanup
        {
            std::filesystem::path Path;
            ~Cleanup() { ConfigManager::GetInstance().Clear(); std::error_code error; std::filesystem::remove_all(Path, error); }
        } cleanup{saved};
        // The contract exercises the rendering sample's real Python catalog;
        // the selected/default game need not contain sample scripts.
        const auto fixtureRoot = std::filesystem::path(HE_ENGINE_CONTENT_DIR).parent_path().parent_path() /
            "Examples/RenderingTests";
        require(std::filesystem::is_regular_file(fixtureRoot / "Scripts/PythonScripts.json") &&
            std::filesystem::is_regular_file(fixtureRoot / "Scripts/ScriptSampleObserver.py"),
            "Python runtime sample fixture is missing its catalog or source.");
        ConfigManager::GetInstance().Init(fixtureRoot, HE_ENGINE_CONTENT_DIR, saved, {}, "Config/BaseEngine.json");
        Level level;
        auto entity = level.CreateEntity("Real Python common lifecycle");
        entity.AddComponent<SpriteRendererComponent>();
        auto& component = entity.AddComponent<ScriptComponent>();
        component.Backend = "he.script.python"; component.Type = "sample.python-observer";
        component.Fields = EncodeScriptFields({{"observedScore", std::int64_t(5)}});
        ScriptSystem system(&level);
        system.OnRuntiemStart();
        system.OnUpdateRuntime(Timestep(0.016f));
        require(PythonBackend::HasLiveInstances(), "Common Python backend created no real instance.");
        auto fields = DecodeScriptFields(component.Fields);
        require(std::get<std::int64_t>(fields.at("observedScore")) == 5, "Python fields were not restored.");
        require(std::get<std::int64_t>(fields.at("enableCount")) == 1, "Python Enable was not called.");
        ScriptEngine::Execute(R"PY(
from Scripts.ScriptSampleObserver import ScriptSampleObserver
physics_events = []
def receive_contact(self, contact):
    assert contact.Other.IsValid
    assert list(contact.Point) == [1.0, 2.0, 3.0]
    physics_events.append((contact.Phase, contact.IsTrigger))
ScriptSampleObserver.Collision = receive_contact
ScriptSampleObserver.Trigger = receive_contact
)PY");
        auto other = level.CreateEntity("Python contact counterpart");
        system.QueueContact(entity, other, 0, false, {1, 2, 3}, {0, 1, 0});
        system.QueueContact(entity, other, 1, true, {1, 2, 3}, {0, 1, 0});
        system.OnUpdateRuntime(Timestep(0.016f));
        ScriptEngine::Execute(R"PY(
assert physics_events == [(0, False), (1, True)]
del ScriptSampleObserver.Collision
del ScriptSampleObserver.Trigger
)PY");
        bool shutdownRejected = false;
        try { ScriptEngine::GetInstance().Shutdown(); } catch (const std::logic_error&) { shutdownRejected = true; }
        require(shutdownRejected && ScriptEngine::IsInitialized(), "Interpreter shutdown ignored common Python roots.");
        fields["throwOnUpdate"] = true;
        component.Fields = EncodeScriptFields(fields);
        system.OnUpdateRuntime(Timestep(0.016f));
        system.OnUpdateRuntime(Timestep(0.016f));
        require(PythonBackend::HasLiveInstances(), "Fault isolation lost the Python root before Destroy.");
        system.OnRuntimeStop();
        // Exercise actual embedded imports/class construction through the
        // legacy registration entry, without publishing files into Project.
        ScriptEngine::Execute(R"PY(
import sys, types, Scripts
path_fixture_constructions = 0
class HEModulePathFixture:
    def __init__(self):
        global path_fixture_constructions
        path_fixture_constructions += 1
path_fixture_module = types.ModuleType('Scripts.HEModulePathFixture')
path_fixture_module.HEModulePathFixture = HEModulePathFixture
sys.modules['Scripts.HEModulePathFixture'] = path_fixture_module
path_fixture_package = types.ModuleType('Scripts.HEPathPackage')
path_fixture_package.__path__ = []
sys.modules['Scripts.HEPathPackage'] = path_fixture_package
sys.modules['Scripts.HEPathPackage.HEModulePathFixture'] = path_fixture_module
)PY");
        {
            struct PathFixtureCleanup
            {
                std::vector<UUID> Ids;
                ~PathFixtureCleanup() { for (const auto& id : Ids) ScriptEngine::UnregisterModule(id); }
            } pathCleanup;
            for (const std::string path : {
                "Scripts.HEModulePathFixture.py", "Scripts.HEModulePathFixture",
                "Scripts/HEModulePathFixture.py", "Scripts\\HEModulePathFixture.py",
                "Scripts/HEPathPackage/HEModulePathFixture.py", "Scripts.HEPathPackage.HEModulePathFixture.py" })
            {
                const UUID id; pathCleanup.Ids.push_back(id);
                require(ScriptEngine::RegisterModule(id, path), "Valid Python module/resource path failed actual registration.");
                require(ScriptEngine::IsModuleRegistered(id) && ScriptEngine::IsClassRegistered(id) &&
                    ScriptEngine::GetPyClassName(id) == "HEModulePathFixture", "Module path did not resolve the actual class name.");
            }
            ScriptEngine::Execute("assert path_fixture_constructions == 6");
            for (const std::string path : {
                "", "../HEModulePathFixture.py", "/Scripts/HEModulePathFixture.py",
                "C:/Scripts/HEModulePathFixture.py", "\\\\server\\Scripts\\HEModulePathFixture.py",
                "Scripts/../HEModulePathFixture.py", "Scripts/./HEModulePathFixture.py",
                "Scripts//HEModulePathFixture.py", "Scripts/HEModulePathFixture.pyc",
                "Scripts/HEModulePathFixture", "Other/HEModulePathFixture.py",
                "Scripts/HEPathPackage.HEModulePathFixture.py", "Scripts..HEModulePathFixture.py",
                "Scripts/bad-name.py", "Scripts/123name.py" })
            {
                const UUID id;
                require(!ScriptEngine::RegisterModule(id, path) && !ScriptEngine::IsModuleRegistered(id) &&
                    !ScriptEngine::IsClassRegistered(id) &&
                    ScriptEngine::GetLastRegistrationError(id).find("Invalid Python script path:") == 0,
                    "Unsafe/malformed Python script path was not rejected before import.");
                ScriptEngine::UnregisterModule(id);
            }
            ScriptEngine::Execute("assert path_fixture_constructions == 6");
        }
        ScriptEngine::Execute(R"PY(
for key in ('Scripts.HEModulePathFixture', 'Scripts.HEPathPackage.HEModulePathFixture', 'Scripts.HEPathPackage'):
    sys.modules.pop(key, None)
)PY");
        {
            auto backend = std::make_shared<PythonBackend>();
            require(backend->Descriptor().RuntimeLoading == ScriptRuntimeLoading::LinkedInterpreter,
                "Python did not declare linked interpreter loading.");
            ScriptWorld world(level);
            std::vector<ScriptError> errors;
            ScriptCore core(ScriptPlatform::Windows, [](auto id) { return bool(ScriptWorld::Resolve(id)); },
                [&](auto, const ScriptError& error) { errors.push_back(error); });
            core.Register(backend);
            core.Add("he.script.python", "sample.python-observer", world.Bind(entity), {{"throwOnUpdate", true}});
            core.Update(0.016); core.Update(0.016);
            require(errors.size() == 1 && errors[0].Message.find("RuntimeError") != std::string::npos &&
                errors[0].Stack.find("ScriptSampleObserver.py(") != std::string::npos &&
                errors[0].Stack.find("): Update") != std::string::npos,
                "Real CPython exception lost its structured traceback or repeated its log.");
            core.Stop();
        }
        // Existing Python syntax must retain nested live views without keeping
        // either a component address or a destroyed Level alive.
        ScriptEngine::Execute(R"PY(
import HEngine, sys, types
import faulthandler
faulthandler.enable()
class HELegacyReferenceFixture:
    def FixedUpdate(self, ts):
        fixed_steps.append(ts.GetSeconds())

    def Start(self, entity):
        global legacy_entity, legacy_transform, legacy_position, legacy_blackboard, legacy_get_float
        legacy_entity = entity
        global legacy_follow
        if entity.HasComponent('FollowCamera'):
            assert entity.HasComponent('FollowCameraComponent')
            assert entity.HasComponent('Camera') and entity.HasComponent('CameraComponent')
            assert entity.HasComponent('SpringArm') and entity.HasComponent('SpringArmComponent')
            legacy_follow = entity.GetComponent('FollowCamera')
            legacy_follow.bHasTarget = False
            assert not entity.GetComponent('FollowCameraComponent').bHasTarget
            entity.GetComponent('Camera').Primary = False
            assert not entity.GetComponent('CameraComponent').Primary
            entity.GetComponent('SpringArm').ArmLength = 4.5
            assert entity.GetComponent('SpringArmComponent').ArmLength == 4.5
        print('[PythonReferenceTest] get component', flush=True)
        legacy_transform = entity.GetComponent('Transform')
        print('[PythonReferenceTest] get nested field', flush=True)
        legacy_position = legacy_transform.Translation
        print('[PythonReferenceTest] get blackboard', flush=True)
        legacy_blackboard = entity.GetComponent('BehaviorTree').get_blackboard()
        legacy_transform.Translation = HEngine.vec3(1, 2, 3)
        legacy_position.x = 4
        copied = HEngine.vec3(legacy_position)
        assert copied.x == 4 and copied.y == 2
        entity.Move(legacy_position)
        legacy_blackboard.set_float('value', 7)
        legacy_get_float = legacy_blackboard.get_float
        import gc
        for iteration in range(100):
            if iteration % 10 == 0:
                gc.collect()
            assert legacy_get_float('value') == 7
        print('[PythonReferenceTest] callback complete', flush=True)
def legacy_expect_dead(callback):
    try:
        callback()
    except (ReferenceError, ValueError):
        return
    raise AssertionError('Stale legacy Python reference remained usable')
legacy_module = types.ModuleType('HELegacyReferenceFixture')
fixed_steps = []
legacy_module.HELegacyReferenceFixture = HELegacyReferenceFixture
sys.modules['HELegacyReferenceFixture'] = legacy_module
)PY");
        const UUID fixtureId;
        require(ScriptEngine::RegisterModule(fixtureId, "HELegacyReferenceFixture.py"), "Legacy reference fixture registration failed.");
        struct FixtureCleanup
        {
            UUID Id;
            ~FixtureCleanup() { ScriptEngine::UnregisterModule(Id); }
        } fixtureCleanup{fixtureId};
        Entity retainedEntity;
        {
            Level temporaryLevel;
            auto earlier = temporaryLevel.CreateEntity("First storage slot");
            auto retained = temporaryLevel.CreateEntity("Legacy Python retained view");
            retained.AddComponent<BehaviorTreeComponent>();
            retained.AddComponent<CameraComponent>();
            retained.AddComponent<FollowCameraComponent>();
            retained.AddComponent<SpringArmComponent>();
            ScriptEngine::Invoke(fixtureId, LegacyScriptCallback::Start, &retained);
            ScriptEngine::Invoke(fixtureId, LegacyScriptCallback::FixedUpdate,
                nullptr, 1.0f / 60.0f);
            ScriptEngine::Invoke(fixtureId, LegacyScriptCallback::FixedUpdate,
                nullptr, 1.0f / 60.0f);
            ScriptEngine::Execute("assert len(fixed_steps) == 2 and all(abs(step - 1.0 / 60.0) < 1e-6 for step in fixed_steps)");
            require(retained.GetComponent<TransformComponent>().Translation.x == 4, "Legacy nested write did not update the actual component.");
            temporaryLevel.DestroyEntity(earlier);
            ScriptEngine::Execute("assert legacy_position.x == 4\nlegacy_position.x = 5");
            require(retained.GetComponent<TransformComponent>().Translation.x == 5, "Component storage movement invalidated a live legacy view.");
            retained.RemoveComponent<BehaviorTreeComponent>();
            retained.AddComponent<BehaviorTreeComponent>();
            ScriptEngine::Execute("legacy_expect_dead(lambda: legacy_blackboard.get_float('value'))");
            ScriptEngine::Execute("legacy_expect_dead(lambda: legacy_get_float('value'))");
            retained.AddOrReplaceComponent<TransformComponent>();
            ScriptEngine::Execute("legacy_expect_dead(lambda: legacy_position.x)");
            ScriptEngine::Invoke(fixtureId, LegacyScriptCallback::Start, &retained);
            temporaryLevel.DestroyEntity(retained);
            ScriptEngine::Execute("legacy_expect_dead(lambda: legacy_follow.bHasTarget)");
            auto reused = temporaryLevel.CreateEntity("Reused entity slot");
            ScriptEngine::Execute("legacy_expect_dead(lambda: legacy_transform.Translation)\nlegacy_expect_dead(lambda: legacy_entity.GetUUID())");
            reused.AddComponent<BehaviorTreeComponent>();
            ScriptEngine::Invoke(fixtureId, LegacyScriptCallback::Start, &reused);
            retainedEntity = reused;
        }
        require(!retainedEntity && retainedEntity.GetLevel() == nullptr, "An Entity retained access to a destroyed Level.");
        ScriptEngine::Execute("legacy_expect_dead(lambda: legacy_position.x)\nlegacy_expect_dead(lambda: legacy_blackboard.get_float('value'))\nlegacy_expect_dead(lambda: legacy_entity.GetUUID())");
        require(!PythonBackend::HasLiveInstances(), "Scene stop retained common Python roots.");
        fields["throwOnUpdate"] = false;
        fields["destroyOnUpdate"] = true;
        component.Fields = EncodeScriptFields(fields);
        system.OnRuntiemStart();
        system.OnUpdateRuntime(Timestep(0.016f));
        require(!entity, "Python self-destruction did not reach the ECS barrier.");
        require(ScriptPlatformServices::ReadSave("python-self-delete") == "stale-rejected",
            "Actual Python stale-handle checks failed or were skipped during self-destruction.");
        require(!PythonBackend::HasLiveInstances(), "Python self-destruction retained a runtime root.");
        system.OnRuntimeStop();
    }
}
