#include "Hepch.h"
#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/EcsFramework/Component/Script/NativeScriptComponent.h"
#include "Runtime/EcsFramework/System/Script/NativeScriptSystem.h"
#include "Runtime/Graphics/Renderer/LightRenderer.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Game/GameLayer/TouchSample.h"
#include "_generated/serializer/TransformComponent.gen.h"
#include "_generated/serializer/SpriteRendererComponent.gen.h"
#include "_generated/serializer/NativeScriptComponent.gen.h"
#if defined(HE_SCRIPT_CSHARP_WASM)
#include "Runtime/Core/Plugins/PluginManager.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Script/ScriptComponent.h"
#include "Runtime/Platform/Scripting/WasmScriptServices.h"
#include "Runtime/Scripting/ScriptFieldsJson.h"
#include "_generated/serializer/ScriptComponent.gen.h"
#include "_generated/serializer/CameraComponent.gen.h"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <cmath>
#include <stdexcept>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/heap.h>
#define HE_WASM_EXPORT EMSCRIPTEN_KEEPALIVE
// Only the platform implementation creates UUIDs; scene UUID parsing remains crossguid.
namespace xg
{
    Guid newGuid()
    {
        std::array<unsigned char, 16> bytes{};
        for (auto& byte : bytes) byte = static_cast<unsigned char>(emscripten_random() * 256.0);
        bytes[6] = (bytes[6] & 15) | 64;
        bytes[8] = (bytes[8] & 63) | 128;
        return Guid(bytes);
    }
}
#else
#define HE_WASM_EXPORT
#endif

namespace HEngine
{
    void RegisterWeChatReflection();
#if defined(HE_SCRIPT_CSHARP_WASM)
    void RegisterStaticPluginsForGame(PluginManager&);
#endif
    namespace
    {
        std::string sError, sSave;
        RuntimeResourceRegistry sResources;
        RuntimeResourceHandle<SceneDocument> sDocument;
        RuntimeResourceHandle<LightTexture> sTexture;
        RuntimeResourceGroup sGroup;
        Scope<Level> sLevel;
#if defined(HE_SCRIPT_CSHARP_WASM)
        Scope<PluginManager> sPlugins;
#endif
        Scope<LightRenderer> sRenderer;
        Entity sPlayer;
        bool sVisible = true;
        int sWidth = 1, sHeight = 1;
        float sLastDelta = 0;

        template<typename Callback> int Guard(Callback callback)
        {
            try { callback(); sError.clear(); return 1; }
            catch (const std::exception& error) { sError = error.what(); }
            catch (...) { sError = "Unknown C++ failure"; }
            Log::WriteFallbackError("WeChat runtime", sError);
            return 0;
        }
        void Shutdown()
        {
            if (sRenderer) sRenderer->Shutdown();
            sRenderer.reset();
            sPlayer = {};
            sLevel.reset();
#if defined(HE_SCRIPT_CSHARP_WASM)
            if (sPlugins) sPlugins->StopAll();
            sPlugins.reset();
            SetWasmScriptInput(0, 0, 0, 0, false);
#endif
            sTexture.Reset();
            sDocument.Reset();
            sGroup = {};
            if (sResources.IsInitialized()) sResources.Shutdown();
            RuntimeResourceCatalog::GetInstance().Clear();
            TouchSample::SetTouch(0, 0, false);
        }
        void ValidateTransform(const TransformComponent& transform)
        {
            for (int i = 0; i < 3; ++i)
                if (!std::isfinite(transform.Translation[i]) || !std::isfinite(transform.Rotation[i]) ||
                    !std::isfinite(transform.Scale[i]) || std::abs(transform.Translation[i]) > 100 || std::abs(transform.Scale[i]) > 100)
                    throw std::runtime_error("Invalid sprite transform");
            if (!transform.ParentUuid.empty()) throw std::runtime_error("WeChat sprite profile does not support transform parenting yet");
        }
        void Init(const char* projectRoot, const char* scenePath, const char* texturePath, const char* saved)
        {
            Shutdown();
#if defined(HE_SCRIPT_CSHARP_WASM)
            PluginHostEnvironment environment;
            environment.Host = PluginHost::Game;
            environment.BuildDistribution = PluginBuildDistribution::Shipping;
            environment.Configuration = PluginConfiguration::Release;
#if defined(HE_WEB)
            environment.Platform = PluginPlatform::Web;
#else
            environment.Platform = PluginPlatform::WeChat;
#endif
            sPlugins = CreateScope<PluginManager>(environment);
            RegisterStaticPluginsForGame(*sPlugins);
            sPlugins->StartThroughPhase(PluginLoadPhase::PreDevice);
#endif
            Log::Init();
            RegisterWeChatReflection();
            TouchSample::Register();
            sResources.Init(0);
            sGroup = sResources.CreateGroup();
            const auto root = Utils::Utf8ToPath(projectRoot);
            sDocument = AcquireSceneDocument(sResources, ResolveSceneDocumentPath(root, scenePath));
            (void)sGroup.Pin(sDocument);
            sLevel = CreateScope<Level>();
            for (const auto& source : sDocument->GetEntities())
            {
                for (const auto& [name, value] : source.Value.items())
                {
                    bool portableComponent = false;
#if defined(HE_SCRIPT_CSHARP_WASM)
                    portableComponent = name == "ScriptComponent" || name == "CameraComponent";
#endif
                    if (name == "PythonScriptComponent") throw std::runtime_error("PythonScriptComponent is unsupported on WeChat: " + source.Uuid);
                    if (!portableComponent && name != "TagComponent" && name != "TransformComponent" && name != "SpriteRendererComponent" && name != "NativeScriptComponent")
                        throw std::runtime_error("Unsupported WeChat component " + name + " on " + source.Uuid);
                }
                auto entity = sLevel->CreateEntity(UUID(source.Uuid), source.Tag);
                if (source.Value.contains("TransformComponent"))
                    Serializer::Deserialize(source.Value.at("TransformComponent"), entity.GetComponent<TransformComponent>());
                ValidateTransform(entity.GetComponent<TransformComponent>());
                if (source.Value.contains("SpriteRendererComponent"))
                {
                    auto& sprite = entity.AddComponent<SpriteRendererComponent>();
                    Serializer::Deserialize(source.Value.at("SpriteRendererComponent"), sprite);
                    if (sprite.Texture != texturePath) throw std::runtime_error("Sprite texture does not match the exported profile: " + sprite.Texture);
                    if (sprite.TilingFactor != 1) throw std::runtime_error("WeChat sprite profile requires TilingFactor=1");
                }
                if (source.Value.contains("NativeScriptComponent"))
                {
                    if (sPlayer) throw std::runtime_error("WeChat sample profile supports one TouchSample controller");
                    auto& script = entity.AddComponent<NativeScriptComponent>();
                    Serializer::Deserialize(source.Value.at("NativeScriptComponent"), script);
                    if (script.Type != "TouchSample" || !entity.HasComponent<SpriteRendererComponent>())
                        throw std::runtime_error("Unsupported native type or missing sprite: " + script.Type);
                    sPlayer = entity;
                }
#if defined(HE_SCRIPT_CSHARP_WASM)
                if (source.Value.contains("CameraComponent"))
                {
                    auto& camera = entity.AddComponent<CameraComponent>();
                    Serializer::Deserialize(source.Value.at("CameraComponent"), camera);
                    if (camera.Camera.GetProjectionType() != SceneCamera::ProjectionType::Orthographic ||
                        !std::isfinite(camera.Camera.GetOrthographicSize()) || camera.Camera.GetOrthographicSize() <= 0 ||
                        camera.Camera.GetOrthographicNearClip() >= camera.Camera.GetOrthographicFarClip())
                        throw std::runtime_error("Portable sprite camera requires a finite orthographic projection: " + source.Uuid);
                    camera.Camera.SetViewportSize(sWidth, sHeight);
                }
                if (source.Value.contains("ScriptComponent"))
                {
                    auto& script = entity.AddComponent<ScriptComponent>();
                    Serializer::Deserialize(source.Value.at("ScriptComponent"), script);
                    if (script.Backend != "he.script.csharp") throw std::runtime_error("Portable script backend is unsupported on entity " + source.Uuid + ": " + script.Backend);
                    (void)DecodeScriptFields(script.Fields);
                    if (!sPlayer) sPlayer = entity;
                }
#endif
            }
            if (!sPlayer) throw std::runtime_error("Scene must contain one TouchSample sprite");
            if (saved && *saved)
            {
                const Json state = Json::parse(saved);
                if (state.at("schema") != 1 || state.at("entity") != sPlayer.UUID().str()) throw std::runtime_error("Save schema/entity mismatch");
                auto transform = sPlayer.GetComponent<TransformComponent>();
                Serializer::Deserialize(state.at("transform"), transform);
                ValidateTransform(transform);
                sPlayer.GetComponent<TransformComponent>() = transform;
                // Older schema-1 saves contain only transform/script. Keep the
                // source scene's sprite for those saves; new saves restore it.
                if (state.contains("sprite"))
                {
                    if (!sPlayer.HasComponent<SpriteRendererComponent>()) throw std::runtime_error("Saved sprite has no scene component");
                    auto sprite = sPlayer.GetComponent<SpriteRendererComponent>();
                    Serializer::Deserialize(state.at("sprite"), sprite);
                    if (sprite.Texture != texturePath || sprite.TilingFactor != 1)
                        throw std::runtime_error("Saved sprite differs from the exported texture profile");
                    for (int index = 0; index < 4; ++index)
                        if (!std::isfinite(sprite.Color[index])) throw std::runtime_error("Saved sprite color is not finite");
                    sPlayer.GetComponent<SpriteRendererComponent>() = sprite;
                }
#if defined(HE_SCRIPT_CSHARP_WASM)
                if (sPlayer.HasComponent<ScriptComponent>())
                {
                    ScriptComponent script;
                    Serializer::Deserialize(state.at("script"), script);
                    const auto& original = sPlayer.GetComponent<ScriptComponent>();
                    if (script.Backend != original.Backend || script.Type != original.Type) throw std::runtime_error("Saved script binding differs from the scene.");
                    (void)DecodeScriptFields(script.Fields);
                    sPlayer.GetComponent<ScriptComponent>() = script;
                }
                else
#endif
                {
                    NativeScriptComponent script;
                    Serializer::Deserialize(state.at("script"), script);
                    if (script.Type != "TouchSample") throw std::runtime_error("Invalid saved native type");
                    sPlayer.GetComponent<NativeScriptComponent>() = script;
                }
            }
            std::filesystem::path textureFullPath;
            std::string normalizedTexture;
            const auto pathResult = Utils::TryResolveProjectRelativePath(root, texturePath, textureFullPath, normalizedTexture);
            if (!pathResult) throw std::runtime_error(pathResult.Message);
            const auto key = RuntimeResourceKey::FromProjectPath(RuntimeResourceKind::Texture, normalizedTexture, "wechat-webgl2-rgba8-v1");
            sTexture = sResources.AcquireBlocking<LightTexture>(key, [&]
            {
                int width, height, channels;
                unsigned char* pixels = stbi_load(Utils::PathToUtf8(textureFullPath).c_str(), &width, &height, &channels, 4);
                if (!pixels) throw std::runtime_error("PNG decode failed: " + normalizedTexture + ": " + stbi_failure_reason());
                std::unique_ptr<unsigned char, decltype(&stbi_image_free)> ownership(pixels, stbi_image_free);
                if (width < 1 || height < 1 || width > 1024 || height > 1024) throw std::runtime_error("Texture exceeds WeChat sample profile");
                auto texture = CreateRef<LightTexture>();
                texture->AssetPath = normalizedTexture;
                texture->Width = width; texture->Height = height;
                texture->Pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
                return texture;
            });
            (void)sGroup.Pin(sTexture);
            // Publish through HE's catalog, with canonical resource identity and
            // an explicit hard scene dependency. GPU ownership stays in the renderer.
            std::vector<RuntimeResourceCatalogEntry> entries(2);
            entries[0].Path = scenePath;
            entries[0].CacheKey = Utils::MakeProjectPathCacheKey(scenePath);
            entries[0].Type = RuntimeCatalogResourceType::Scene;
            entries[0].CookFormatVersion = 1;
            entries[0].CpuBytes = sDocument->GetSourceBytes();
            entries[0].Dependencies.push_back({ normalizedTexture, key.CachePath,
                RuntimeCatalogDependencyKind::HardRuntime, RuntimeCatalogDependencySource::Structured });
            entries[1].Path = normalizedTexture;
            entries[1].CacheKey = key.CachePath;
            entries[1].Type = RuntimeCatalogResourceType::Texture;
            entries[1].CookFormatVersion = 1;
            entries[1].CpuBytes = sTexture->Pixels.size();
            entries[1].GpuBytes = sTexture->Pixels.size();
            entries[1].Tags = { "wechat-webgl2-sprite-v1" };
            const auto catalogHash = RuntimeResourceCatalogSnapshot::ComputeRegistryHash(entries);
            RuntimeResourceCatalog::GetInstance().Install(RuntimeResourceCatalogSnapshot::Create(std::move(entries), catalogHash));
            sLevel->OnRuntimeStart();
            sLevel->OnUpdateRuntime(Timestep(0));
#if defined(__EMSCRIPTEN__)
            sRenderer = CreateLightRenderer();
            sRenderer->Init(*sTexture);
            sRenderer->Resize(sWidth, sHeight);
#endif
            sVisible = true;
        }
    }
}

using namespace HEngine;
extern "C"
{
    HE_WASM_EXPORT int he_probe() { return 1; }
    HE_WASM_EXPORT unsigned int he_heap_bytes()
    {
#if defined(__EMSCRIPTEN__)
        return static_cast<unsigned int>(emscripten_get_heap_size());
#else
        return 0;
#endif
    }
    HE_WASM_EXPORT const char* he_last_error() { return sError.c_str(); }
    HE_WASM_EXPORT int he_init(const char* root, const char* scene, const char* texture, const char* saved)
    {
        return Guard([&] { try { Init(root, scene, texture, saved); } catch (...) { Shutdown(); throw; } });
    }
    HE_WASM_EXPORT int he_shutdown() { return Guard([] { Shutdown(); }); }
    HE_WASM_EXPORT int he_resize(int width, int height)
    {
        return Guard([&] { if (width < 1 || height < 1 || width > 8192 || height > 8192) throw std::runtime_error("Invalid canvas dimensions");
            sWidth = width; sHeight = height; if (sRenderer) sRenderer->Resize(width, height);
#if defined(HE_SCRIPT_CSHARP_WASM)
            SetWasmScriptViewport(width, height);
            if (sLevel) sLevel->mRegistry.view<CameraComponent>().each([&](auto& camera) {
                if (!camera.FixedAspectRatio) camera.Camera.SetViewportSize(width, height);
            });
#endif
        });
    }
    HE_WASM_EXPORT void he_touch(float x, float y, int down)
    {
        TouchSample::SetTouch(std::clamp(x, -10.0f, 10.0f), std::clamp(y, -10.0f, 10.0f), down != 0 && sVisible);
    }
    HE_WASM_EXPORT int he_visible(int visible)
    {
        return Guard([&] {
            sVisible = visible != 0;
            TouchSample::SetTouch(0, 0, false);
#if defined(HE_SCRIPT_CSHARP_WASM)
            SetWasmScriptInput(0, 0, 0, 0, false);
#endif
            if (sLevel) { sLevel->SetRuntimePaused(false); sLevel->OnUpdateRuntime(Timestep(0)); sLevel->SetRuntimePaused(!sVisible); }
        });
    }
    HE_WASM_EXPORT int he_frame(float delta)
    {
        return Guard([&]
        {
            if (!sLevel) throw std::runtime_error("Runtime is not initialized");
            sLastDelta = sVisible && std::isfinite(delta) ? std::clamp(delta, 0.0f, 0.05f) : 0;
            sLevel->OnUpdateRuntime(Timestep(sLastDelta));
            if (!sRenderer || !sVisible) return;
            RenderCamera camera;
            const float aspect = static_cast<float>(sWidth) / sHeight;
            camera.ViewProjection = glm::orthoRH_NO(-aspect, aspect, -1.0f, 1.0f, -1.0f, 1.0f);
#if defined(HE_SCRIPT_CSHARP_WASM)
            auto cameras = sLevel->mRegistry.view<CameraComponent, TransformComponent>();
            for (const auto raw : cameras)
            {
                const auto& component = cameras.get<CameraComponent>(raw);
                if (!component.Primary) continue;
                camera.ViewProjection = component.Camera.GetProjection() * glm::inverse(cameras.get<TransformComponent>(raw).GetTransform());
                break;
            }
#endif
            std::vector<RenderPrimitive2D> primitives;
            sLevel->mRegistry.view<TransformComponent, SpriteRendererComponent>().each([&](auto& transform, auto& sprite)
            {
                RenderPrimitive2D primitive;
                primitive.Transform = transform.GetTransform();
                primitive.Color = sprite.Color;
                primitive.TextureAssetPath = sprite.Texture;
                primitives.push_back(std::move(primitive));
            });
            sRenderer->Submit(camera, primitives);
        });
    }
    HE_WASM_EXPORT const char* he_save()
    {
        const auto success = Guard([]
        {
            if (!sPlayer) throw std::runtime_error("No active scene to save");
            // Drain the sample's buffered input edges before asynchronous host persistence.
            sLevel->OnUpdateRuntime(Timestep(0));
            Json transform, script;
            Serializer::Serialize(transform, sPlayer.GetComponent<TransformComponent>());
#if defined(HE_SCRIPT_CSHARP_WASM)
            if (sPlayer.HasComponent<ScriptComponent>()) Serializer::Serialize(script, sPlayer.GetComponent<ScriptComponent>());
            else
#endif
                Serializer::Serialize(script, sPlayer.GetComponent<NativeScriptComponent>());
            Json state{{"schema", 1}, {"entity", sPlayer.UUID().str()}, {"transform", transform}, {"script", script}};
            if (sPlayer.HasComponent<SpriteRendererComponent>())
                Serializer::Serialize(state["sprite"], sPlayer.GetComponent<SpriteRendererComponent>());
            sSave = state.dump();
        });
        return success ? sSave.c_str() : nullptr;
    }
    HE_WASM_EXPORT float he_last_delta() { return sLastDelta; }
#if defined(HE_SCRIPT_CSHARP_WASM)
    HE_WASM_EXPORT const char* he_script_fields()
    {
        if (!sPlayer || !sPlayer.HasComponent<ScriptComponent>()) return nullptr;
        return sPlayer.GetComponent<ScriptComponent>().Fields.c_str();
    }
    HE_WASM_EXPORT int he_script_enabled(int enabled)
    {
        return Guard([&] {
            if (!sPlayer || !sPlayer.HasComponent<ScriptComponent>()) throw std::runtime_error("No active script controller.");
            sPlayer.GetComponent<ScriptComponent>().Enabled = enabled != 0;
        });
    }
    HE_WASM_EXPORT int he_script_is_enabled()
    {
        return sPlayer && sPlayer.HasComponent<ScriptComponent>() && sPlayer.GetComponent<ScriptComponent>().Enabled;
    }
    HE_WASM_EXPORT int he_script_destroy()
    {
        return Guard([] {
            if (!sPlayer) throw std::runtime_error("Script controller was already destroyed.");
            sLevel->DestroyEntity(sPlayer); sPlayer = {};
        });
    }
    HE_WASM_EXPORT int he_script_set_field(const char* id, const char* encodedValue)
    {
        return Guard([&] {
            if (!sPlayer || !sPlayer.HasComponent<ScriptComponent>() || !id || !encodedValue) throw std::runtime_error("No active script field target.");
            auto& component = sPlayer.GetComponent<ScriptComponent>();
            auto fields = DecodeScriptFields(component.Fields);
            const Json patch{{"schema", 1}, {"fields", {{id, Json::parse(encodedValue)}}}};
            const auto value = DecodeScriptFields(patch.dump());
            if (value.size() != 1) throw std::runtime_error("Expected one typed script field.");
            fields[id] = value.at(id); component.Fields = EncodeScriptFields(fields);
        });
    }
#endif
}
