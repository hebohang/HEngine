#include "CSharpRuntimeTests.h"
#include "Plugins/CSharpScript/CSharpBackend.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/EcsFramework/Component/Script/ScriptComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/System/Script/ScriptSystem.h"
#include "Runtime/Events/MouseEvent.h"
#include "Runtime/Platform/Scripting/ScriptPlatformServices.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Scripting/ScriptBackendRegistry.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"

#include <HEngine/ProjectBuildConfig.h>

#include <stdexcept>

namespace HEngine::Tests
{
    void CSharpRuntimeContract()
    {
        const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
        const auto saved = std::filesystem::temp_directory_path() / ("HEngine-CSharp-" + UUID().str());
        std::filesystem::create_directories(saved);
        struct Cleanup
        {
            std::filesystem::path Path;
            ~Cleanup()
            {
                Input::EndSyntheticKeyboardSession();
                ConfigManager::GetInstance().Clear();
                std::error_code error; std::filesystem::remove_all(Path, error);
            }
        } cleanup{saved};
        ConfigManager::GetInstance().Init(HE_DEFAULT_PROJECT_DIR, HE_ENGINE_CONTENT_DIR, saved, {}, "Config/BaseEngine.json");
        ConfigManager::mViewportSize = {1280, 720};
        Input::BeginSyntheticKeyboardSession(true);
        Input::SetSyntheticPointerPosition({640, 360});
        auto backend = std::dynamic_pointer_cast<CSharpBackend>(ScriptBackendRegistry::Find("he.script.csharp"));
        require(bool(backend), "C# plugin is not enabled for this test host.");
        require(backend->Descriptor().RuntimeLoading == ScriptRuntimeLoading::BackendHosted,
            "PC C# did not declare backend-hosted runtime loading.");
        Level level;
        auto entity = level.CreateEntity("Real C# script");
        entity.AddComponent<SpriteRendererComponent>();
        auto& component = entity.AddComponent<ScriptComponent>();
        component.Type = "sample.touch";
        component.Fields = EncodeScriptFields({{"score", std::int64_t(10)}, {"speed", 3.0}});
#if HE_WITH_PYTHON_TESTS
        auto observer = level.CreateEntity("Python observer of actual C# events");
        observer.AddComponent<SpriteRendererComponent>();
        auto& observerComponent = observer.AddComponent<ScriptComponent>();
        observerComponent.Backend = "he.script.python";
        observerComponent.Type = "sample.python-observer";
#endif
        ScriptSystem system(&level);
        system.OnRuntiemStart();
        require(backend->LiveInstanceCount() == 1, "C# instance was not created by the ECS system.");
        const KeyCode right[] = {KeyCode::D};
        require(Input::SetSyntheticKeys(right), "Synthetic input fixture did not activate.");
        system.OnUpdateRuntime(Timestep(0.016f));
        require(entity.GetComponent<TransformComponent>().Translation.x > 0, "Real C# did not move its ECS Transform.");
        MouseButtonPressedEvent press(MouseCode::ButtonLeft);
        MouseButtonReleasedEvent release(MouseCode::ButtonLeft);
        ScriptPlatformServices::ObserveEvent(press);
        ScriptPlatformServices::ObserveEvent(release);
        system.OnUpdateRuntime(Timestep(0.016f));
        auto fields = DecodeScriptFields(component.Fields);
        require(std::get<std::int64_t>(fields.at("score")) == 13, "C# did not observe a complete click between frames.");
        require(ScriptPlatformServices::ReadSave("script-touch") == "13", "C# save did not reach the platform save implementation.");
        require(entity.GetComponent<SpriteRendererComponent>().Color.r == 1.0f, "C# click did not change the ECS Sprite color.");
#if HE_WITH_PYTHON_TESTS
        system.OnUpdateRuntime(Timestep(0.016f));
        require(std::get<std::int64_t>(DecodeScriptFields(observerComponent.Fields).at("observedScore")) == 13,
            "Python did not observe the actual C# event through the common engine ABI.");
        require(observer.GetComponent<TransformComponent>().Translation.y > 1.0f,
            "The real Python observer did not write its ECS Transform.");
        observerComponent.Enabled = false;
        system.OnUpdateRuntime(Timestep(0.016f));
        observerComponent.Enabled = true;
        system.OnUpdateRuntime(Timestep(0.016f));
        require(std::get<std::int64_t>(DecodeScriptFields(observerComponent.Fields).at("enableCount")) == 2,
            "Python Enable was not called after re-enabling its common script component.");
#endif
        for (int i = 0; i < 20; ++i)
        {
            require(backend->Collect() > 0, "CoreCLR did not report its managed heap after GC.");
            system.OnUpdateRuntime(Timestep(0.016f));
        }
        require(backend->LiveInstanceCount() == 1, "GC invalidated the C# root.");
        fields = DecodeScriptFields(component.Fields);
        fields["throwOnUpdate"] = true;
        component.Fields = EncodeScriptFields(fields);
        const auto position = entity.GetComponent<TransformComponent>().Translation;
        system.OnUpdateRuntime(Timestep(0.016f));
        system.OnUpdateRuntime(Timestep(0.016f));
        require(entity.GetComponent<TransformComponent>().Translation == position, "A faulted C# instance continued executing.");
        system.OnRuntimeStop();
        {
            auto diagnosticEntity = level.CreateEntity("C# structured exception");
            diagnosticEntity.AddComponent<SpriteRendererComponent>();
            ScriptWorld world(level);
            std::vector<ScriptError> errors;
            ScriptCore core(ScriptPlatform::Windows, [](auto id) { return bool(ScriptWorld::Resolve(id)); },
                [&](auto, const ScriptError& error) { errors.push_back(error); });
            core.Register(backend);
            core.Add("he.script.csharp", "sample.touch", world.Bind(diagnosticEntity), {{"throwOnUpdate", true}});
            core.Update(0.016); core.Update(0.016);
            require(errors.size() == 1 && errors[0].Message.find("InvalidOperationException") != std::string::npos &&
                errors[0].Stack.find("TouchSample.Update") != std::string::npos,
                "Real CoreCLR exception lost its structured stack or repeated its log.");
            core.Stop();
        }
        require(backend->LiveInstanceCount() == 0, "Scene stop retained C# roots.");
        fields["throwOnUpdate"] = false;
        fields["score"] = std::int64_t(0);
        component.Fields = EncodeScriptFields(fields);
        system.OnRuntiemStart();
        system.OnUpdateRuntime(Timestep(0.016f));
        require(std::get<std::int64_t>(DecodeScriptFields(component.Fields).at("score")) == 13, "Scene re-entry did not reload the C# save.");
        level.DestroyEntity(entity);
        require(backend->LiveInstanceCount() == 0, "Entity destruction retained its C# instance.");
        system.OnRuntimeStop();
    }
}
