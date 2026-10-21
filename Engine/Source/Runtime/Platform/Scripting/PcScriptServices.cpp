#include "ScriptPlatformServices.h"
#include "Runtime/Scripting/ScriptInputContext.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Events/MouseEvent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/FollowCameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/SpringArmComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/FileUtils/AtomicFile.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Scripting/ScriptWorld.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/EcsFramework/Component/Basic/TagComponent.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Animation/BoneAttachmentComponent.h"
#include "Runtime/EcsFramework/Component/Animation/SequenceComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/UI/Rml/RmlUiManager.h"
#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Audio/ScriptAudio.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Graphics/Renderer/RenderPipeline.h"
#include "Runtime/Animation/SkeletonCompatibility.h"
#include "Runtime/Graphics/Mesh/MeshRuntimeResource.h"
#include "Runtime/Graphics/Mesh/MeshDeformationInstanceCache.h"
#include "Runtime/Audio/ProjectAudioPath.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/TwoD/Rigidbody2DComponent.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#if HE_ENABLE_PHYSICS_2D
#include "Runtime/EcsFramework/System/Physics/PhysicSystem2D.h"
#endif
#if HE_ENABLE_NAVIGATION
#include "Runtime/EcsFramework/System/Navigation/NavPathDebugSystem.h"
#endif

#if HE_ENABLE_PHYSICS_3D
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace HEngine
{
    namespace
    {
#if HE_ENABLE_PHYSICS_3D
        struct ScriptRayBodyFilter final : JPH::BodyFilter
        {
            std::array<JPH::BodyID, 32> IgnoredBodies;
            size_t IgnoredCount = 0;
            const JPH::BodyInterface* Interface = nullptr;
            bool bStaticFloorOnly = false;

            bool ShouldCollide(const JPH::BodyID& id) const override
            {
                for (size_t index = 0; index < IgnoredCount; ++index)
                    if (id == IgnoredBodies[index]) return false;
                return !bStaticFloorOnly || (Interface &&
                    Interface->GetObjectLayer(id) == PhysicSystem3D::ObjectLayerStatic &&
                    !Interface->IsSensor(id));
            }
        };
#endif

        struct PreparedAppearance
        {
            MeshComponent Prototype;
            MeshDeformationInstanceCache Instances;
            std::unordered_map<uint64_t, Ref<RenderResourceWarmupRequest>> Warmups;
        };

        Ref<Mesh> PrepareAppearanceInstance(PreparedAppearance& prepared, uint64_t handle, Entity target)
        {
            if (!target || !target.HasComponent<MeshComponent>() || !target.GetComponent<MeshComponent>().mMesh)
                throw std::invalid_argument("Appearance target has no mesh.");
            auto& destination = target.GetComponent<MeshComponent>();
            if (target.HasComponent<AnimatorComponent>())
            {
                const auto incompatibility = GetSkinPaletteIncompatibility(
                    prepared.Prototype.mMesh->GetBoneInfoMap(), destination.mMesh->GetBoneInfoMap());
                if (!incompatibility.empty()) throw std::invalid_argument(incompatibility);
            }
            prepared.Instances.RetainLiveOwners([](uint64_t owner) { return bool(ScriptWorld::Resolve(owner)); });
            std::erase_if(prepared.Warmups, [](const auto& item) { return !ScriptWorld::Resolve(item.first); });
            auto mesh = prepared.Instances.Acquire(handle, prepared.Prototype.mMesh);
            mesh->mSkeletonPath = destination.mMesh->mSkeletonPath;
            if (!prepared.Warmups.contains(handle))
            {
                auto* renderer = target.GetLevel()->TryGetSystem<RenderSystem3D>();
                if (!renderer) throw std::runtime_error("Appearance GPU preparation requires the 3D render system.");
                prepared.Warmups.emplace(handle, renderer->PrepareMeshResources(mesh));
            }
            return mesh;
        }
    }

    std::string ScriptPlatformServices::Execute(std::uint64_t context, const std::string& name, const std::string& request)
    {
        auto owner = ScriptWorld::Resolve(context);
        if (!owner) throw std::invalid_argument("Platform services require a live scene context.");
        const auto args = Json::parse(request);
        if (!args.is_object()) throw std::invalid_argument("Platform service request must be an object.");
        if (name == "service.release")
            return Json(ScriptWorld::ReleaseService(context, args.at("kind").get<std::string>(), args.at("lease").get<std::uint64_t>())).dump();
        if (name == "audio.play" || name == "audio.ownedPlay")
        {
            ProjectAudioPath path;
            const auto validation = TryResolveProjectAudioPath(ConfigManager::GetInstance().GetRootFolder(), args.at("path").get<std::string>(), path);
            if (!validation) throw std::invalid_argument(validation.Message);
            const auto params = ReadScriptAudioParams(args);
            const auto handle = AudioEngine::GetInstance().Play(path.FullPath, params);
            if (name == "audio.play") return Json(handle.Value).dump();
            if (!handle.Value) throw std::runtime_error("Audio playback could not start: " + path.FullPath.generic_string());
            return Json(ScriptWorld::OwnService(context, "audio", handle.Value,
                [handle] { AudioEngine::GetInstance().Stop(handle); })).dump();
        }
        if (name.starts_with("audio."))
        {
            const auto handle = args.contains("lease") ? ScriptWorld::ReadService(context, "audio", args.at("lease").get<std::uint64_t>()) : args.value("handle", std::uint64_t{0});
            return ExecuteScriptAudioControl(AudioEngine::GetInstance(), name.substr(6), args, {handle}).dump();
        }
        if (name == "vfx.stop") { EffekseerRuntime::StopEffect(args.at("handle").get<int32_t>()); return "true"; }
        if (name == "vfx.play" || name == "vfx.update" || name == "vfx.ownedPlay" || name == "vfx.ownedUpdate")
        {
            const auto vector = [&](const char* key) {
                const auto& values = args.at(key);
                if (!values.is_array() || values.size() != 3) throw std::invalid_argument("VFX vectors require three numbers.");
                glm::vec3 result(values[0].get<float>(), values[1].get<float>(), values[2].get<float>());
                if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) throw std::invalid_argument("VFX vectors must be finite.");
                return result;
            };
            const auto position = vector("position"), forward = vector("forward");
            glm::vec4 color(1.0f);
            const bool bHasColor = args.contains("color") && !args.at("color").is_null();
            if (bHasColor)
            {
                const auto& values = args.at("color");
                if (!values.is_array() || values.size() != 4)
                    throw std::invalid_argument("VFX color requires four RGBA values.");
                for (int channel = 0; channel < 4; ++channel)
                {
                    color[channel] = values[channel].get<float>();
                    if (!std::isfinite(color[channel]) || color[channel] < 0.0f || color[channel] > 1.0f)
                        throw std::invalid_argument("VFX color components must be finite and in [0, 1].");
                }
            }
            const bool bHasOpacity = args.contains("opacity");
            if (bHasOpacity)
            {
                const float opacity = args.at("opacity").get<float>();
                if (!std::isfinite(opacity))
                    throw std::invalid_argument("VFX opacity must be finite.");
                color.a *= std::clamp(opacity, 0.0f, 1.0f);
            }
            int32_t handle;
            const bool playing = name == "vfx.play" || name == "vfx.ownedPlay";
            if (playing)
            {
                std::string path;
                const auto validation = Utils::TryNormalizeProjectRelativePath(args.at("path").get<std::string>(), path);
                if (!validation) throw std::invalid_argument(validation.Message);
                const float scale = args.value("scale", 1.0f), speed = args.value("speed", 1.0f);
                if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(speed) || speed < 0) throw std::invalid_argument("Invalid VFX scale or speed.");
                handle = EffekseerRuntime::PlayEffectAdvanced(path, position, forward, scale, speed);
            }
            else handle = name == "vfx.ownedUpdate" ? static_cast<int32_t>(ScriptWorld::ReadService(context, "vfx", args.at("lease").get<std::uint64_t>())) : args.at("handle").get<int32_t>();
            if (handle >= 0)
            {
                EffekseerRuntime::SetEffectTransform(handle, position, forward);
                EffekseerRuntime::SetEffectPaused(handle, args.value("paused", false));
                if ((bHasColor || bHasOpacity) && !EffekseerRuntime::SetEffectColor(handle, color))
                {
                    if (bHasColor)
                    {
                        if (playing) EffekseerRuntime::StopEffect(handle);
                        throw std::runtime_error("VFX effect does not support global color.");
                    }
                }
            }
            if (name == "vfx.ownedPlay")
            {
                if (handle < 0) throw std::runtime_error("VFX playback could not start.");
                return Json(ScriptWorld::OwnService(context, "vfx", static_cast<std::uint64_t>(handle),
                    [handle] { EffekseerRuntime::StopEffect(handle); })).dump();
            }
            return playing ? Json(handle).dump() : Json(EffekseerRuntime::IsEffectPlaying(handle)).dump();
        }
        if (name == "input.touchControls")
            return Json(Input::SetTouchControlsEnabled(args.at("enabled").get<bool>())).dump();
        if (name == "input.buttons")
        {
            const bool enabled = ScriptInputContext::Region.Enabled && ScriptInputContext::Region.KeyboardEnabled;
            const auto pointer = Input::GetMousePosition();
            return Json{{"jump", enabled && Input::IsKeyPressed(KeyCode::Space)},
                {"captured", enabled && Application::GetWindow().IsMouseCursorCaptured() && !Input::IsKeyPressed(KeyCode::LeftAlt) && !Input::IsKeyPressed(KeyCode::RightAlt)},
                {"pause", enabled && Input::IsKeyPressed(KeyCode::Escape)},
                {"confirm", enabled && Input::IsKeyPressed(KeyCode::Enter)},
                {"up", enabled && Input::IsKeyPressed(KeyCode::Up)},
                {"down", enabled && Input::IsKeyPressed(KeyCode::Down)},
                {"left", enabled && Input::IsKeyPressed(KeyCode::Left)},
                {"rightKey", enabled && Input::IsKeyPressed(KeyCode::Right)},
                {"right", ScriptInputContext::Region.Enabled && ScriptInputContext::Region.Contains(pointer.x, pointer.y) && Input::IsMouseButtonPressed(MouseCode::ButtonRight)},
                {"pointerX", pointer.x}, {"pointerY", pointer.y}}.dump();
        }
        const auto entityArg = [&]() {
            auto entity = ScriptWorld::Resolve(args.at("entity").get<std::uint64_t>());
            if (!entity || entity.GetLevel() != owner.GetLevel()) throw std::invalid_argument("Invalid entity in this scene.");
            return entity;
        };
        const auto vectorArg = [&](const char* key) {
            const auto& value = args.at(key);
            if (!value.is_array() || value.size() != 3) throw std::invalid_argument("Expected a three-component vector.");
            glm::vec3 result(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
            for (int i = 0; i < 3; ++i) if (!std::isfinite(result[i])) throw std::invalid_argument("Vector must be finite.");
            return result;
        };
#if HE_ENABLE_PHYSICS_3D
        const auto configureRayBodyFilter = [&](ScriptRayBodyFilter& filter) {
            const auto append = [&](std::uint64_t handle) {
                auto ignored = ScriptWorld::Resolve(handle);
                if (!ignored || ignored.GetLevel() != owner.GetLevel())
                    throw std::invalid_argument("Ignored ray entity is not live in this scene.");
                if (ignored.HasComponent<Rigidbody3DComponent>())
                {
                    const auto* id = PhysicSystem3D::TryGetBodyID(
                        ignored.GetComponent<Rigidbody3DComponent>().RuntimeBody);
                    if (id && std::find(filter.IgnoredBodies.begin(),
                        filter.IgnoredBodies.begin() + filter.IgnoredCount, *id) ==
                        filter.IgnoredBodies.begin() + filter.IgnoredCount)
                        filter.IgnoredBodies[filter.IgnoredCount++] = *id;
                }
            };
            const auto single = args.value("entity", std::uint64_t(0));
            const auto multiple = args.find("entities");
            if (multiple != args.end() && (!multiple->is_array() ||
                multiple->size() + (single ? 1u : 0u) > filter.IgnoredBodies.size()))
                throw std::invalid_argument("Ray queries can exclude at most 32 entities.");
            if (single) append(single);
            if (multiple != args.end())
                for (const auto& value : *multiple)
                {
                    if (!value.is_number_unsigned() && !value.is_number_integer())
                        throw std::invalid_argument("Ignored ray entities must be entity handles.");
                    append(value.get<std::uint64_t>());
                }
        };
#endif
        // Scene-owned CPU preparation retains private deformable instances per
        // target. GPU resources enter the normal renderer upload lifecycle.
        if (name == "appearance.prepare")
        {
            std::string path;
            if (!Utils::TryNormalizeProjectRelativePath(args.at("path").get<std::string>(), path) || !path.ends_with(".hmesh"))
                throw std::invalid_argument("Appearance requires a project-relative .hmesh.");
            auto* group = owner.GetLevel()->GetOrCreateRuntimeResourceGroup();
            if (!group) throw std::runtime_error("Appearance preparation requires scene runtime resources.");
            const auto prototype = MeshRuntimeResource::AcquirePrototype(path, *group);
            auto lease = CreateRef<PreparedAppearance>();
            auto* prepared = &lease->Prototype;
            prepared->Path = path;
            prepared->mMesh = CreateRef<Mesh>(*prototype);
            if (!prepared->mMesh || prepared->mMesh->mSubMeshes.empty())
                throw std::invalid_argument("Appearance mesh contains no geometry.");
            prepared->MaterialOverrides = args.value("materials", std::vector<std::string>{});
            for (auto& material : prepared->MaterialOverrides)
            {
                if (material.empty()) continue;
                std::string normalized;
                if (!Utils::TryNormalizeProjectRelativePath(material, normalized) || !normalized.ends_with(".hmat"))
                    throw std::invalid_argument("Appearance material requires a project-relative .hmat.");
                material = normalized;
            }
            prepared->ApplyMaterialOverrides();
            if (args.value("entity", uint64_t{0}) != 0)
            {
                auto target = entityArg();
                (void)PrepareAppearanceInstance(*lease, args.at("entity").get<uint64_t>(), target);
            }
            return Json(ScriptWorld::OwnService(context, "appearance",
                reinterpret_cast<std::uint64_t>(lease.get()), [lease] {})).dump();
        }
        if (name == "appearance.apply")
        {
            auto entity = entityArg();
            if (!entity.HasComponent<MeshComponent>()) throw std::invalid_argument("Appearance target has no mesh.");
            auto& destination = entity.GetComponent<MeshComponent>();
            auto* lease = reinterpret_cast<PreparedAppearance*>(ScriptWorld::ReadService(
                context, "appearance", args.at("lease").get<std::uint64_t>()));
            if (!lease || !destination.mMesh) throw std::invalid_argument("Appearance lease or target is unavailable.");
            const auto* prepared = &lease->Prototype;
            auto mesh = PrepareAppearanceInstance(*lease, args.at("entity").get<uint64_t>(), entity);
            if (mesh == destination.mMesh) return "true";
            if (entity.HasComponent<AnimatorComponent>())
                entity.GetComponent<AnimatorComponent>().RebindCompatibleMesh(*destination.mMesh, *mesh);
            destination.Path = prepared->Path;
            destination.meshResPath = prepared->Path;
            destination.MaterialOverrides = prepared->MaterialOverrides;
            destination.mAuthoredMaterials = prepared->mAuthoredMaterials;
            destination.mMesh = std::move(mesh);
            return "true";
        }
        if (name == "scene.worldPosition")
        {
            const glm::vec3 position = owner.GetLevel()->GetWorldTranslation(entityArg());
            return Json::array({position.x, position.y, position.z}).dump();
        }
        if (name == "scene.load")
        {
            std::string path;
            const auto validation = Utils::TryNormalizeProjectRelativePath(args.at("path").get<std::string>(), path);
            if (!validation || !path.ends_with(".he")) throw std::invalid_argument("Scene loading requires a project-relative .he resource.");
            return Json(Application::GetInstance().RequestRuntimeScene(path, args.value("force", false))).dump();
        }
        if (name == "physics.characterState")
        {
#if HE_ENABLE_PHYSICS_3D
            auto entity = entityArg();
            if (!entity.HasComponent<CharacterMovementComponent>())
                throw std::invalid_argument("Character state requires CharacterMovementComponent.");
            const auto& movement = entity.GetComponent<CharacterMovementComponent>();
            return Json{{"grounded", movement.RuntimeIsGrounded},
                {"crouched", movement.RuntimeIsCrouched},
                {"velocity", {movement.RuntimeResolvedVelocity.x, movement.RuntimeResolvedVelocity.y,
                    movement.RuntimeResolvedVelocity.z}}}.dump();
#else
            throw std::invalid_argument("Character state requires Physics3D.");
#endif
        }
        if (name == "physics.characterMove")
        {
#if HE_ENABLE_PHYSICS_3D
            auto entity = entityArg(); const auto velocity = vectorArg("value");
            if (!entity.HasComponent<CharacterMovementComponent>()) return "false";
            auto& movement = entity.GetComponent<CharacterMovementComponent>();
            movement.RuntimeDesiredVelocity = velocity; movement.RuntimeHasExternalVelocity = true;
            return "true";
#else
            throw std::invalid_argument("Character movement requires Physics3D.");
#endif
        }
        if (name == "physics.readVelocity" || name == "physics.force" || name == "physics.impulse")
        {
            auto entity = entityArg();
            const bool reading = name == "physics.readVelocity";
            glm::vec3 value = reading ? glm::vec3(0) : vectorArg("value");
            bool success = false;
            if (args.value("twoD", false))
            {
#if HE_ENABLE_PHYSICS_2D
                auto* system = owner.GetLevel()->TryGetSystem<PhysicSystem2D>();
                if (system && entity.HasComponent<Rigidbody2DComponent>())
                {
                    auto* body = entity.GetComponent<Rigidbody2DComponent>().RuntimeBody;
                    glm::vec2 velocity{};
                    success = reading ? system->GetBodyLinearVelocity(body, velocity) : system->ApplyBodyForce(body, {value.x, value.y}, name == "physics.impulse");
                    if (reading) value = {velocity.x, velocity.y, 0};
                }
#else
                throw std::invalid_argument("Physics2D is disabled in this build.");
#endif
            }
            else
            {
                auto* system = owner.GetLevel()->TryGetSystem<PhysicSystem3D>();
                if (system && entity.HasComponent<Rigidbody3DComponent>())
                {
                    auto* body = entity.GetComponent<Rigidbody3DComponent>().RuntimeBody;
                    success = reading ? system->GetBodyLinearVelocity(body, value) : system->ApplyBodyForce(body, value, name == "physics.impulse");
                }
            }
            return reading ? Json{{"found", success}, {"value", {value.x, value.y, value.z}}}.dump() : Json(success).dump();
        }
        if (name == "physics.velocity")
        {
            auto entity = entityArg(); const auto velocity = vectorArg("value");
            if (args.value("twoD", false))
            {
#if HE_ENABLE_PHYSICS_2D
                auto* system = owner.GetLevel()->TryGetSystem<PhysicSystem2D>();
                if (!system || !entity.HasComponent<Rigidbody2DComponent>()) return "false";
                return Json(system->SetBodyLinearVelocity(entity.GetComponent<Rigidbody2DComponent>().RuntimeBody, {velocity.x, velocity.y})).dump();
#else
                throw std::invalid_argument("Physics2D is disabled in this build.");
#endif
            }
            auto* system = owner.GetLevel()->TryGetSystem<PhysicSystem3D>();
            if (!system || !entity.HasComponent<Rigidbody3DComponent>()) return "false";
            return Json(system->SetBodyLinearVelocity(entity.GetComponent<Rigidbody3DComponent>().RuntimeBody, velocity)).dump();
        }
        if (name == "animation.lookTarget")
        {
            auto entity = entityArg();
            if (!entity.HasComponent<AnimatorComponent>()) return "false";
            auto& looks = entity.GetComponent<AnimatorComponent>().LookAt;
            const auto index = args.at("index").get<size_t>();
            const auto target = vectorArg("target");
            const float weight = args.at("weight").get<float>();
            if (index >= looks.size() || !std::isfinite(weight) || weight < 0 || weight > 1)
                throw std::invalid_argument("Invalid authored look target index or weight.");
            looks[index].WorldTarget = target; looks[index].Weight = weight;
            return "true";
        }
        if (name == "animation.footContactWeight")
        {
            auto entity = entityArg();
            const float weight = args.at("weight").get<float>();
            if (!std::isfinite(weight) || weight < 0 || weight > 1)
                throw std::invalid_argument("Foot contact weight must be in [0, 1].");
            if (!entity.HasComponent<AnimatorComponent>()) return "false";
            auto& animator = entity.GetComponent<AnimatorComponent>();
            for (auto& foot : animator.Feet) foot.Weight = weight;
            if (weight == 0) animator.mCharacterPose.Reset();
            return "true";
        }
        if (name == "animation.attachment")
        {
            auto entity = entityArg();
            if (!entity.HasComponent<BoneAttachmentComponent>()) return "false";
            entity.GetComponent<BoneAttachmentComponent>().bEnabled = args.at("enabled").get<bool>();
            if (!args.at("enabled").get<bool>())
                entity.GetComponent<TransformComponent>().Scale = glm::vec3(0);
            return "true";
        }
        if (name == "physics.characterFacing")
        {
            auto entity = entityArg();
            if (!entity.HasComponent<CharacterMovementComponent>()) return "false";
            const float yaw = args.at("yaw").get<float>();
            if (!std::isfinite(yaw)) throw std::invalid_argument("Facing yaw must be finite.");
            entity.GetComponent<CharacterMovementComponent>().bOrientRotationToMovement = !args.at("enabled").get<bool>();
            if (args.at("enabled").get<bool>())
            {
                auto rotation = owner.GetLevel()->GetWorldRotation(entity);
                rotation.y = yaw;
                owner.GetLevel()->SetWorldRotation(entity, rotation);
            }
            return "true";
        }
        if (name == "camera.preview")
        {
            auto camera = owner.GetLevel()->GetPrimaryCameraEntity();
            if (!camera || !camera.HasComponent<FollowCameraComponent>() || !camera.HasComponent<SpringArmComponent>())
                throw std::invalid_argument("Camera preview requires an existing follow rig.");
            const auto follow = camera.GetComponent<FollowCameraComponent>();
            const auto arm = camera.GetComponent<SpringArmComponent>();
            const auto transform = camera.GetComponent<TransformComponent>();
            const auto cameraState = camera.GetComponent<CameraComponent>();
            return Json(ScriptWorld::OwnService(context, "camera-preview", 0,
                [camera, follow, arm, transform, cameraState]() mutable
                {
                    if (!camera || !camera.HasComponent<FollowCameraComponent>() || !camera.HasComponent<SpringArmComponent>()) return;
                    camera.GetComponent<FollowCameraComponent>() = follow;
                    camera.GetComponent<FollowCameraComponent>().RuntimeLastMousePos = Input::GetMousePosition();
                    camera.GetComponent<SpringArmComponent>() = arm;
                    camera.GetComponent<TransformComponent>() = transform;
                    camera.GetComponent<CameraComponent>() = cameraState;
                })).dump();
        }
        if (name == "camera.springArm")
        {
            const float distance = args.at("distance").get<float>();
            const float sensitivity = args.at("sensitivity").get<float>();
            const float pitchDelta = args.value("pitchDelta", 0.0f);
            const auto socket = vectorArg("socketOffset"), lookAt = vectorArg("lookAtOffset");
            if (!std::isfinite(distance) || distance <= 0 || !std::isfinite(sensitivity) || sensitivity <= 0 ||
                !std::isfinite(pitchDelta))
                throw std::invalid_argument("Live camera distance/sensitivity must be positive and recoil finite.");
            auto camera = owner.GetLevel()->GetPrimaryCameraEntity();
            if (!camera || !camera.HasComponent<SpringArmComponent>() || !camera.HasComponent<FollowCameraComponent>())
                return "false";
            auto& arm = camera.GetComponent<SpringArmComponent>();
            auto& follow = camera.GetComponent<FollowCameraComponent>();
            if (distance < arm.MinArmLength)
                throw std::invalid_argument("Live camera distance is below its collision minimum.");
            arm.ArmLength = distance;
            arm.SocketOffset = socket;
            arm.LookAtOffset = lookAt;
            follow.MouseSensitivity = sensitivity;
            follow.RuntimePitchRadians = glm::clamp(follow.RuntimePitchRadians + pitchDelta,
                glm::radians(follow.MinPitchDegrees), glm::radians(follow.MaxPitchDegrees));
            return "true";
        }
        if (name == "camera.fov")
        {
            auto camera = owner.GetLevel()->GetPrimaryCameraEntity();
            if (!camera || !camera.HasComponent<CameraComponent>()) return "false";
            const float fov = args.at("radians").get<float>();
            if (!std::isfinite(fov) || fov < 0.1f || fov > 2.8f)
                throw std::invalid_argument("Camera field of view must be in [0.1,2.8] radians.");
            camera.GetComponent<CameraComponent>().Camera.SetPerspectiveVerticalFOV(fov);
            return "true";
        }
        if (name == "camera.ray")
        {
            const auto camera = owner.GetLevel()->GetPrimaryCameraEntity();
            if (!camera) throw std::invalid_argument("Scene has no primary camera.");
            const auto world = owner.GetLevel()->GetWorldTransform(camera);
            const glm::vec3 origin(world[3]), forward(world[2]);
            if (!std::isfinite(glm::length(forward)) || glm::length(forward) < 1e-6f)
                throw std::invalid_argument("Primary camera has a degenerate world transform.");
            const glm::vec3 direction = -glm::normalize(forward);
            return Json{{"origin", {origin.x, origin.y, origin.z}},
                {"direction", {direction.x, direction.y, direction.z}}}.dump();
        }
        if (name == "physics.bodyEnabled")
        {
            auto entity = entityArg();
            const bool enabled = args.at("enabled").get<bool>();
#if HE_ENABLE_PHYSICS_3D
            auto* physics = owner.GetLevel()->TryGetSystem<PhysicSystem3D>();
            if (!physics || !physics->GetPhysicsSystem() || !entity.HasComponent<Rigidbody3DComponent>())
                return "false";
            const auto& component = entity.GetComponent<Rigidbody3DComponent>();
            const auto* runtimeBody = PhysicSystem3D::TryGetBodyID(component.RuntimeBody);
            if (!runtimeBody) return "false";
            const JPH::BodyID id = *runtimeBody;
            auto* system = physics->GetPhysicsSystem();
            {
                // Decode alone does not prove that this sequence-numbered
                // body still exists. Never AddBody with a retired BodyID.
                const JPH::BodyLockRead body(system->GetBodyLockInterface(), id);
                if (!body.Succeeded()) return "false";
            }
            auto& bodies = system->GetBodyInterface();
            if (bodies.IsAdded(id) == enabled) return "true";
            if (enabled)
            {
                const auto position = owner.GetLevel()->GetWorldTranslation(entity);
                const auto rotation = owner.GetLevel()->GetWorldRotation(entity);
                bodies.SetPositionAndRotation(id, Utils::GlmToJoltRVec3(position),
                    JPH::Quat::sEulerAngles(Utils::GlmToJoltVec3(rotation)), JPH::EActivation::DontActivate);
                if (component.Type != Rigidbody3DComponent::Body3DType::Static)
                    bodies.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                bodies.AddBody(id, component.Type == Rigidbody3DComponent::Body3DType::Static
                    ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            }
            else bodies.RemoveBody(id);
            return "true";
#else
            return "false";
#endif
        }
        if (name == "physics.raycastDistance")
        {
#if HE_ENABLE_PHYSICS_3D
            const auto origin = vectorArg("origin"), direction = vectorArg("direction");
            const float distance = args.at("distance").get<float>();
            if (!std::isfinite(distance) || distance <= 0 || distance > 100000 || glm::length(direction) < 1e-6f)
                throw std::invalid_argument("Ray distance/direction is invalid.");
            auto* physics = owner.GetLevel()->TryGetSystem<PhysicSystem3D>();
            if (!physics || !physics->GetPhysicsSystem()) throw std::invalid_argument("Scene has no physics world.");
            ScriptRayBodyFilter filter;
            configureRayBodyFilter(filter);
            auto* system = physics->GetPhysicsSystem();
            JPH::RRayCast ray(Utils::GlmToJoltRVec3(origin), Utils::GlmToJoltVec3(glm::normalize(direction) * distance));
            JPH::RayCastResult result;
            const bool hit = system->GetNarrowPhaseQuery().CastRay(ray, result,
                system->GetDefaultBroadPhaseLayerFilter(PhysicSystem3D::ObjectLayerMoving),
                system->GetDefaultLayerFilter(PhysicSystem3D::ObjectLayerMoving), filter);
            return Json(hit ? distance * result.mFraction : distance).dump();
#else
            throw std::invalid_argument("Ray queries require Physics3D.");
#endif
        }
        if (name == "physics.floor")
        {
            const auto position = vectorArg("position");
            const float above = args.value("above", 1.0f), below = args.value("below", 100.0f);
            if (!std::isfinite(above) || !std::isfinite(below) || above < 0 || below <= 0 || above + below > 100000)
                throw std::invalid_argument("Invalid floor ray distances.");
            float height = 0;
            bool found = false;
#if HE_ENABLE_PHYSICS_3D
            ScriptRayBodyFilter filter;
            configureRayBodyFilter(filter);
            auto* physics = owner.GetLevel()->TryGetSystem<PhysicSystem3D>();
            if (physics && physics->GetPhysicsSystem())
            {
                auto* system = physics->GetPhysicsSystem();
                filter.bStaticFloorOnly = true;
                filter.Interface = &system->GetBodyInterface();
                const auto origin = position + glm::vec3(0, above, 0);
                const JPH::RRayCast ray(Utils::GlmToJoltRVec3(origin),
                    JPH::Vec3(0, -(above + below), 0));
                JPH::RayCastResult result;
                found = system->GetNarrowPhaseQuery().CastRay(ray, result,
                    system->GetDefaultBroadPhaseLayerFilter(PhysicSystem3D::ObjectLayerMoving),
                    system->GetDefaultLayerFilter(PhysicSystem3D::ObjectLayerMoving), filter);
                if (found) height = static_cast<float>(ray.GetPointOnRay(result.mFraction).GetY());
            }
#endif
            return Json{{"found", found}, {"height", height}}.dump();
        }
        if (name == "navigation.build" || name == "navigation.path")
        {
#if HE_ENABLE_NAVIGATION
            auto* system = owner.GetLevel()->TryGetSystem<NavPathDebugSystem>();
            if (!system) throw std::invalid_argument("Scene has no navigation service.");
            auto& navigation = system->GetNavigation();
            if (name == "navigation.build")
            {
                NavigationBounds bounds{};
                const bool bounded = args.contains("minimum") || args.contains("maximum");
                if (bounded) bounds = {vectorArg("minimum"), vectorArg("maximum")};
                const bool built = system->Rebuild(NavigationService::LoadProjectSettings(), bounded ? &bounds : nullptr);
                return Json{{"success", built}, {"diagnostic", navigation.GetLastError()}}.dump();
            }
            std::vector<glm::vec3> path;
            const bool found = navigation.FindPath(vectorArg("start"), vectorArg("end"), path);
            Json points = Json::array();
            for (auto point : path) points.push_back({point.x, point.y, point.z});
            return Json{{"success", found}, {"diagnostic", navigation.GetLastError()}, {"points", points}}.dump();
#else
            throw std::invalid_argument("Navigation is disabled in this build.");
#endif
        }
        if (name == "scene.findTag")
        {
            const auto tag = args.at("tag").get<std::string>();
            for (const auto raw : owner.GetLevel()->GetAllEntitiesWith<TagComponent>())
            {
                Entity entity{raw, owner.GetLevel()};
                if (entity.GetComponent<TagComponent>().Tag == tag)
                    return Json(ScriptWorld::BindInContext(context, entity)).dump();
            }
            return "0";
        }
        if (name == "scene.instantiate")
        {
            std::string path;
            const auto validation = Utils::TryNormalizeProjectRelativePath(args.at("path").get<std::string>(), path);
            if (!validation) throw std::invalid_argument(validation.Message);
            const auto entity = owner.GetLevel()->InstantiatePrefab(path);
            if (!entity) throw std::runtime_error("Prefab could not be instantiated: " + path);
            return Json(ScriptWorld::BindInContext(context, entity)).dump();
        }
        if (name == "scene.camera")
            return Json(ScriptWorld::BindInContext(context, owner.GetLevel()->GetPrimaryCameraEntity())).dump();
        if (name == "camera.follow")
        {
            const bool enabled = args.at("enabled").get<bool>();
            if (enabled)
            {
                (void)entityArg();
                for (const char* key : {"yaw", "pitch", "minPitch", "maxPitch", "distance", "targetHeight", "socketHeight", "lagSpeed", "minDistance"})
                    if (!std::isfinite(args.at(key).get<float>())) throw std::invalid_argument("Camera settings must be finite.");
                if (args.at("distance").get<float>() <= 0 || args.at("minDistance").get<float>() < 0 ||
                    args.at("minDistance").get<float>() > args.at("distance").get<float>() || args.at("lagSpeed").get<float>() < 0 ||
                    args.at("minPitch").get<float>() > args.at("maxPitch").get<float>())
                    throw std::invalid_argument("Invalid camera follow range.");
            }
            auto camera = owner.GetLevel()->GetPrimaryCameraEntity();
            if (!camera) return "false";
            if (!camera.HasComponent<FollowCameraComponent>()) camera.AddComponent<FollowCameraComponent>();
            if (!camera.HasComponent<SpringArmComponent>()) camera.AddComponent<SpringArmComponent>();
            auto& follow = camera.GetComponent<FollowCameraComponent>();
            auto& arm = camera.GetComponent<SpringArmComponent>();
            follow.bHasTarget = args.at("enabled").get<bool>();
            if (!follow.bHasTarget) return "true";
            auto target = entityArg();
            follow.TargetUUID = target.UUID();
            follow.bRuntimeInitialized = true;
            follow.bAllowYawInput = follow.bAllowPitchInput = false;
            follow.RuntimeYawRadians = args.at("yaw").get<float>();
            follow.RuntimePitchRadians = args.at("pitch").get<float>();
            follow.MinPitchDegrees = args.at("minPitch").get<float>();
            follow.MaxPitchDegrees = args.at("maxPitch").get<float>();
            arm.ArmLength = args.at("distance").get<float>();
            arm.TargetOffset = glm::vec3(0, args.at("targetHeight").get<float>(), 0);
            arm.SocketOffset = glm::vec3(0, args.at("socketHeight").get<float>(), 0);
            arm.LagSpeed = args.at("lagSpeed").get<float>();
            arm.MinArmLength = args.at("minDistance").get<float>();
            if (args.value("reset", false))
            {
                arm.RuntimeCurrentDistance = arm.ArmLength;
                // Initialize at the target instead of interpolating from a menu preview.
                const float horizontal = arm.ArmLength * std::cos(follow.RuntimePitchRadians);
                camera.GetComponent<TransformComponent>().Translation = target.GetComponent<TransformComponent>().Translation +
                    arm.TargetOffset + arm.SocketOffset - glm::vec3(std::sin(follow.RuntimeYawRadians) * horizontal,
                        std::sin(follow.RuntimePitchRadians) * arm.ArmLength, std::cos(follow.RuntimeYawRadians) * horizontal);
            }
            return "true";
        }
        if (name == "scene.visual")
        {
            auto entity = entityArg();
            std::string path;
            const auto validation = Utils::TryNormalizeProjectRelativePath(args.at("mesh").get<std::string>(), path);
            if (!validation) throw std::invalid_argument(validation.Message);
            if (!entity.HasComponent<MeshComponent>()) throw std::invalid_argument("Entity has no mesh.");
            auto& mesh = entity.GetComponent<MeshComponent>();
            if (mesh.Path != path) { mesh.Path = path; mesh.meshResPath.clear(); mesh.mMesh = CreateRef<Mesh>(path); }
            return "true";
        }
        if (name == "animation.control" || name == "animation.state" || name == "animation.parameter" || name == "animation.hasParameter")
        {
            auto entity = entityArg();
            if (!entity.HasComponent<AnimatorComponent>())
            {
                if (name == "animation.control" || name == "animation.hasParameter") return "false";
                throw std::invalid_argument("Entity has no AnimatorComponent.");
            }
            auto& animator = entity.GetComponent<AnimatorComponent>();
            if (name == "animation.state") return Json{{"playing", animator.bPlay}, {"paused", animator.bPaused},
                {"speed", animator.playSpeed}, {"progress", animator.GetProgress()}, {"state", animator.GetCurrentState()}}.dump();
            if (name == "animation.hasParameter") return Json(animator.HasParameter(args.at("parameter").get<std::string>())).dump();
            if (name == "animation.parameter")
            {
                const auto parameter = args.at("parameter").get<std::string>();
                const auto kind = args.at("kind").get<std::string>();
                if (kind == "bool" && animator.IsParameterType(parameter, AnimControllerParamType::Bool)) return Json(animator.GetBool(parameter)).dump();
                if (kind == "int" && animator.IsParameterType(parameter, AnimControllerParamType::Int)) return Json(animator.GetInt(parameter)).dump();
                if (kind == "float" && animator.IsParameterType(parameter, AnimControllerParamType::Float)) return Json(animator.GetFloat(parameter)).dump();
                if (kind == "trigger" && animator.IsParameterType(parameter, AnimControllerParamType::Trigger)) return Json(animator.GetTrigger(parameter)).dump();
                throw std::invalid_argument("Animation parameter is missing or has a different type.");
            }
            const auto action = args.at("action").get<std::string>();
            if (action == "pause") animator.Pause();
            else if (action == "resume") animator.Resume();
            else if (action == "stop") animator.Stop();
            else if (action == "play") animator.Play();
            else if (action == "speed") animator.SetSpeed(args.at("value").get<float>());
            else throw std::invalid_argument("Unknown animation control action.");
            return "true";
        }
        if (name == "animation.set")
        {
            auto entity = entityArg();
            if (!entity.HasComponent<AnimatorComponent>()) return "false";
            auto& animator = entity.GetComponent<AnimatorComponent>();
            for (const auto& [key, type] : {std::pair{"bool", AnimControllerParamType::Bool},
                std::pair{"int", AnimControllerParamType::Int}, std::pair{"float", AnimControllerParamType::Float},
                std::pair{"trigger", AnimControllerParamType::Trigger}, std::pair{"resetTrigger", AnimControllerParamType::Trigger}})
            {
                if (!args.contains(key)) continue;
                const auto parameter = (std::string_view(key) == "trigger" || std::string_view(key) == "resetTrigger") ?
                    args.at(key).get<std::string>() : args.at("parameter").get<std::string>();
                if (!animator.IsParameterType(parameter, type)) return "false";
            }
            if (args.contains("controller"))
            {
                std::string path;
                const auto validation = Utils::TryNormalizeProjectRelativePath(args.at("controller").get<std::string>(), path);
                if (!validation) throw std::invalid_argument(validation.Message);
                animator.SetControllerPath(path);
            }
            if (args.contains("parameter") && !animator.HasParameter(args.at("parameter").get<std::string>())) return "false";
            if (args.contains("bool")) animator.SetBool(args.at("parameter").get<std::string>(), args.at("bool").get<bool>());
            if (args.contains("int")) animator.SetInt(args.at("parameter").get<std::string>(), args.at("int").get<int>());
            if (args.contains("float"))
            {
                const float value = args.at("float").get<float>();
                if (!std::isfinite(value)) throw std::invalid_argument("Animation value must be finite.");
                animator.SetFloat(args.at("parameter").get<std::string>(), value);
            }
            if (args.contains("trigger")) animator.SetTrigger(args.at("trigger").get<std::string>());
            if (args.contains("resetTrigger")) animator.ResetTrigger(args.at("resetTrigger").get<std::string>());
            if (args.contains("clip"))
            {
                std::string path;
                const auto validation = Utils::TryNormalizeProjectRelativePath(args.at("clip").get<std::string>(), path);
                if (!validation) throw std::invalid_argument(validation.Message);
                animator.SetControllerPath("None");
                animator.animResPath = path;
                animator.bLoop = args.value("loop", true);
                animator.Play();
            }
            return "true";
        }
        if (name == "animation.events")
        {
            auto entity = entityArg();
            if (!entity.HasComponent<AnimatorComponent>()) throw std::invalid_argument("Entity has no AnimatorComponent.");
            return entity.GetComponent<AnimatorComponent>().ConsumeEvents().dump();
        }
        if (name == "sequence.control")
        {
            auto entity = entityArg();
            if (!entity.HasComponent<SequenceComponent>()) return "false";
            auto& sequence = entity.GetComponent<SequenceComponent>();
            if (args.contains("pause"))
            {
                if (!sequence.Playing) return "false";
                sequence.Paused = args.at("pause").get<bool>();
                return "true";
            }
            if (args.contains("seek"))
            {
                const auto seconds = args.at("seek").get<float>();
                if (!std::isfinite(seconds) || seconds < 0 || seconds > sequence.Duration)
                    throw std::invalid_argument("Sequence seek must be within its duration.");
                if (!sequence.Playing) return "false";
                sequence.SeekTime = seconds;
                return "true";
            }
            if (args.at("play").get<bool>())
            {
                if (sequence.Playing) return "false";
                for (auto handle : owner.GetLevel()->GetAllEntitiesWith<SequenceComponent>())
                {
                    Entity other(handle, owner.GetLevel());
                    if (other != entity && other.GetComponent<SequenceComponent>().Playing) return "false";
                }
                sequence.Time = 0.0f;
                sequence.SeekTime = -1.0f;
                sequence.Error.clear();
                sequence.Paused = false;
                sequence.Playing = true;
            }
            else sequence.Playing = false;
            return "true";
        }
        if (name == "sequence.state")
        {
            auto entity = entityArg();
            if (!entity.HasComponent<SequenceComponent>()) throw std::invalid_argument("Entity has no SequenceComponent.");
            const auto& sequence = entity.GetComponent<SequenceComponent>();
            return Json{{"playing", sequence.Playing}, {"paused", sequence.Paused}, {"time", sequence.Time},
                {"duration", sequence.Duration}, {"error", sequence.Error}}.dump();
        }
        auto& ui = RmlUiManager::GetInstance();
        if (name == "settings.renderRead")
            return SerializeResolvedRenderSettings(RenderSettingsService::GetInstance().GetSnapshot()).dump();
        if (name == "settings.renderApply")
        {
            auto& settings = RenderSettingsService::GetInstance();
            auto draft = settings.CreateDraft(SettingsLayer::User);
            draft.Patch = args.at("patch");
            if (!draft.Patch.is_object()) throw std::invalid_argument("Render settings patch must be an object.");
            return SerializeRenderSettingsApplyResult(settings.ApplyDraft(draft, true)).dump();
        }
        if (name == "settings.display")
        {
            auto& window = Application::GetWindow();
            if (args.contains("MasterVolume")) AudioEngine::GetInstance().SetMasterVolume(std::clamp(args.at("MasterVolume").get<float>(), 0.0f, 1.0f));
            if (args.contains("VSync")) window.SetVSync(args.at("VSync").get<bool>());
            if (args.contains("Fullscreen")) window.SetFullscreen(args.at("Fullscreen").get<bool>());
            if (args.contains("UIScale")) ui.SetUserScale(std::clamp(args.at("UIScale").get<float>(), 0.75f, 1.5f));
            const auto& sceneStats = Application::GetInstance().GetMainSceneStats();
            return Json{{"MasterVolume", AudioEngine::GetInstance().GetMasterVolume()}, {"VSync", window.IsVSync()},
                {"Fullscreen", window.IsFullscreen()}, {"UIScale", ui.GetUserScale()},
                {"WindowWidth", window.GetWidth()}, {"WindowHeight", window.GetHeight()},
                {"OutputWidth", sceneStats.OutputWidth}, {"OutputHeight", sceneStats.OutputHeight},
                {"InternalWidth", sceneStats.InternalWidth}, {"InternalHeight", sceneStats.InternalHeight},
                {"UpscalingActive", sceneStats.bUpscalingActive}, {"UpscalingFailed", sceneStats.bUpscalingFailed},
                {"UpscalingStatus", ToString(sceneStats.UpscalingState)},
                {"UpscalingFailureReason", sceneStats.UpscalingFailureReason}}.dump();
        }
        if (name == "settings.read" || name == "settings.write")
        {
            std::string path;
            const auto validation = Utils::TryNormalizeProjectRelativePath(args.at("path").get<std::string>(), path);
            if (!validation || !path.starts_with("Config/") || !path.ends_with(".json"))
                throw std::invalid_argument("Settings require a Config-relative JSON path.");
            const auto saved = ConfigManager::GetSavedPath(path);
            if (name == "settings.write")
            {
                if (!args.at("value").is_object()) throw std::invalid_argument("Settings value must be an object.");
                const auto value = args.at("value").dump(2);
                if (value.size() > 65536) throw std::invalid_argument("Settings exceed 64 KiB.");
                std::filesystem::create_directories(saved.parent_path());
                Utils::WriteTextToFileAtomically(saved, value);
                return "true";
            }
            Json result = Json::object();
            for (const auto& source : {ConfigManager::GetInstance().GetRootFolder() / path, saved})
            {
                if (!std::filesystem::exists(source) || std::filesystem::file_size(source) > 65536) continue;
                std::ifstream stream(source);
                const auto value = Json::parse(stream, nullptr, false);
                if (value.is_object()) result.update(value);
            }
            return result.dump();
        }
        if (name == "ui.load" || name == "ui.ownedLoad")
        {
            std::string document;
            if (!ui.LoadDocument(args.at("path").get<std::string>(), owner.UUID(), &document))
                throw std::runtime_error("UI document could not be loaded.");
            ui.SetDocumentEventHandler(document, [context](const RmlUiNativeEvent& event) {
                if (event.Type == "click" && ScriptWorld::Resolve(context)) ScriptWorld::EmitEvent(context, "ui.event",
                    Json{{"type", event.Type}, {"source", event.SourceElementId}, {"target", event.TargetElementId}}.dump());
            });
            ui.ShowDocument(document);
            ui.SetActiveDocument(document);
            if (name == "ui.ownedLoad")
            {
                const auto lease = ScriptWorld::OwnService(context, "ui", 0, [document] { RmlUiManager::GetInstance().UnloadDocument(document); });
                return Json{{"lease", lease}, {"document", document}}.dump();
            }
            return Json(document).dump();
        }
        if (name == "ui.clear") { ui.ClearOwnerDocuments(owner.UUID()); return "true"; }
        const auto document = args.value("document", std::string{});
        if (name == "ui.cursor")
        {
            if (args.contains("path")) ui.SetCustomMouseCursor(document, args.at("path").get<std::string>(), 3, 2);
            return Json(ui.SetMouseCursorVisible(document, args.at("visible").get<bool>())).dump();
        }
        if (name == "ui.visible")
        {
            const bool bVisible = args.at("visible").get<bool>();
            if (!bVisible) return Json(ui.HideDocument(document)).dump();
            if (!ui.ShowDocument(document)) return "false";
            return Json(ui.SetActiveDocument(document)).dump();
        }
        const auto element = args.value("element", std::string{});
        if (name == "ui.text") return Json(ui.SetElementText(document, element, args.at("value").get<std::string>())).dump();
        if (name == "ui.scrollIntoView") return Json(ui.ScrollElementIntoView(document, element)).dump();
        if (name == "ui.markup") return Json(ui.SetElementMarkup(document, element, args.at("value").get<std::string>())).dump();
        if (name == "ui.class") return Json(ui.SetElementClass(document, element, args.at("name").get<std::string>(), args.at("enabled").get<bool>())).dump();
        if (name == "ui.style") return Json(ui.SetElementStyle(document, element, args.at("name").get<std::string>(), args.at("value").get<std::string>())).dump();
        if (name == "ui.attribute") return Json(ui.SetElementAttribute(document, element, args.at("name").get<std::string>(), args.at("value").get<std::string>())).dump();
        throw std::invalid_argument("Unknown platform service: " + name);
    }

    namespace
    {
        ScriptPointerState sPointer;
        glm::vec2 PointerPosition()
        {
#if defined(HE_DEV_AGENT_RUNTIME)
            glm::vec2 syntheticPosition;
            if (Input::TryGetSyntheticPointerPosition(syntheticPosition)) return syntheticPosition;
#endif
            const auto& region = ScriptInputContext::Region;
            // GLFW's captured coordinates are unbounded relative-motion values.
            // A captured pointer aims through the viewport center, not that value.
            if (Application::GetWindow().IsMouseCursorCaptured())
                return region.Restricted ? glm::vec2(region.X + region.Width * 0.5f, region.Y + region.Height * 0.5f)
                    : glm::vec2(ConfigManager::GetViewportWidth(), ConfigManager::GetViewportHeight()) * 0.5f;
            return Input::GetMousePosition();
        }
        std::filesystem::path SavePath(const std::string& key)
        {
            if (key.empty() || key.size() > 128 || Utils::IsWindowsReservedPathComponent(key) ||
                !std::all_of(key.begin(), key.end(), [](unsigned char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
                })) throw std::invalid_argument("Script save key must contain 1-128 ASCII letters, digits, '-' or '_'.");
            return ConfigManager::GetSavedPath("Scripting/" + key + ".txt");
        }
    }

    ScriptInputSnapshot ScriptPlatformServices::ReadInput(Entity context)
    {
        ScriptInputSnapshot input;
        const auto& region = ScriptInputContext::Region;
        const bool keyboardEnabled = region.Enabled && region.KeyboardEnabled;
        const auto position = PointerPosition();
        const bool pointerAccepted = region.Contains(position.x, position.y);
        sPointer.Update(Input::IsMouseButtonPressed(MouseCode::ButtonLeft), pointerAccepted);
        input.Pressed = sPointer.IsPressed(pointerAccepted);
        input.PressSequence = sPointer.Sequence();
        if (keyboardEnabled)
        {
            input.MoveX = float(Input::IsKeyPressed(KeyCode::D) || Input::IsKeyPressed(KeyCode::Right)) - float(Input::IsKeyPressed(KeyCode::A) || Input::IsKeyPressed(KeyCode::Left));
            input.MoveY = float(Input::IsKeyPressed(KeyCode::W) || Input::IsKeyPressed(KeyCode::Up)) - float(Input::IsKeyPressed(KeyCode::S) || Input::IsKeyPressed(KeyCode::Down));
            const auto gamepad = Input::GetGamepadState();
            if (gamepad.bConnected && input.MoveX == 0 && input.MoveY == 0)
            {
                input.MoveX = gamepad.GetAxis(GamepadAxis::LeftX);
                input.MoveY = -gamepad.GetAxis(GamepadAxis::LeftY);
            }
        }
        const float width = std::max(region.Restricted ? region.Width : ConfigManager::GetViewportWidth(), 1.0f);
        const float height = std::max(region.Restricted ? region.Height : ConfigManager::GetViewportHeight(), 1.0f);
        const float pointerX = position.x - (region.Restricted ? region.X : 0.0f);
        const float pointerY = position.y - (region.Restricted ? region.Y : 0.0f);
        float size = 10.0f;
        glm::vec3 origin{};
        if (context)
        {
            auto camera = context.GetLevel()->GetPrimaryCameraEntity();
            if (camera && camera.HasComponent<CameraComponent>())
            {
                size = camera.GetComponent<CameraComponent>().Camera.GetOrthographicSize();
                if (camera.HasComponent<TransformComponent>()) origin = camera.GetComponent<TransformComponent>().Translation;
            }
        }
        input.PointerX = (pointerX / width - 0.5f) * size * width / height + origin.x;
        input.PointerY = (0.5f - pointerY / height) * size + origin.y;
        return input;
    }

    void ScriptPlatformServices::ObserveEvent(Event& event)
    {
        if (event.GetEventType() == EventType::MouseButtonPressed &&
            static_cast<MouseButtonPressedEvent&>(event).GetMouseButton() == MouseCode::ButtonLeft)
        {
            const auto position = PointerPosition();
            sPointer.Update(true, !event.Handled && ScriptInputContext::Region.Contains(position.x, position.y));
        }
        else if (event.GetEventType() == EventType::MouseButtonReleased &&
            static_cast<MouseButtonReleasedEvent&>(event).GetMouseButton() == MouseCode::ButtonLeft)
            sPointer.Update(false, false);
    }

    std::string ScriptPlatformServices::ReadSave(const std::string& key)
    {
        const auto path = SavePath(key);
        if (!std::filesystem::exists(path)) return {};
        const auto size = std::filesystem::file_size(path);
        if (size > 1024 * 1024) throw std::runtime_error("Script save exceeds 1 MiB.");
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot open script save.");
        std::string value(static_cast<std::size_t>(size), '\0');
        stream.read(value.data(), static_cast<std::streamsize>(size));
        if (!stream) throw std::runtime_error("Cannot read complete script save.");
        return value;
    }

    void ScriptPlatformServices::WriteSave(const std::string& key, const std::string& value)
    {
        if (value.size() > 1024 * 1024) throw std::invalid_argument("Script save exceeds 1 MiB.");
        const auto path = SavePath(key);
        std::filesystem::create_directories(path.parent_path());
        Utils::WriteTextToFileAtomically(path, value);
    }
}
