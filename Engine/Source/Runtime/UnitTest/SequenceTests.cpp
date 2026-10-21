#include "Hepch.h"
#include "Runtime/UnitTest/SequenceTests.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Animation/SequenceComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/FollowCameraComponent.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/System/Animation/SequenceSystem.h"
#include "_generated/serializer/engine/Runtime/EcsFramework/Component/Animation/SequenceComponent.gen.h"
#include <cmath>
#include <stdexcept>

namespace HEngine::Tests
{
    void SequencePlaybackContract()
    {
        const auto require = [](bool condition, const char* message)
        { if (!condition) throw std::runtime_error(message); };
        Level level;
        auto camera = level.CreateEntity("Cinematic camera");
        camera.AddComponent<CameraComponent>().Primary = false;
        camera.AddComponent<FollowCameraComponent>().bHasTarget = true;
        auto gameplay = level.CreateEntity("Gameplay camera");
        gameplay.AddComponent<CameraComponent>().Primary = true;
        auto actor = level.CreateEntity("Actor");
        auto& animator = actor.AddComponent<AnimatorComponent>();
        animator.mParameterTypes["Speed"] = AnimControllerParamType::Float;
        animator.mParameterTypes["Action"] = AnimControllerParamType::Trigger;
        animator.mParameters["Speed"] = 0.2f;
        animator.mParameters["Action"] = false;
        animator.mParameterTypes["Gesture"] = AnimControllerParamType::Int;
        animator.mParameters["Gesture"] = 0;
        animator.FacialWeights["Smile"] = 0.1f;
#if HE_ENABLE_PHYSICS_3D
        actor.AddComponent<CharacterMovementComponent>();
#endif
        auto owner = level.CreateEntity("Sequence");
        auto& sequence = owner.AddComponent<SequenceComponent>();
        sequence.Duration = 1.0f;
        sequence.BlendIn = sequence.BlendOut = 0.0f;
        sequence.Bindings = {{"Camera", camera.UUID().str()}, {"Actor", actor.UUID().str()}};
        sequence.PrefetchTextures = {"Assets/Textures/SequenceShot.dds"};
        SequenceKey a, b;
        a.Value = 40.0f;
        b.Time = 1.0f; b.Value = 60.0f; b.Position = glm::vec3(2, 0, 0);
        SequenceTrack cameraTrack;
        cameraTrack.Keys = {a, b};
        sequence.Tracks.push_back(cameraTrack);
        SequenceTrack expression;
        expression.Kind = "expression"; expression.Target = "Actor"; expression.Name = "Smile";
        a.Value = 0.0f; b.Value = 1.0f;
        expression.Keys = {a, b};
        sequence.Tracks.push_back(expression);
        SequenceTrack speed = expression;
        speed.Kind = "float"; speed.Name = "Speed";
        sequence.Tracks.push_back(speed);
        SequenceTrack event;
        event.Kind = "event"; event.Name = "Cue";
        a.Time = 0.25f; a.Value = 7.0f;
        event.Keys = {a};
        sequence.Tracks.push_back(event);
        SequenceTrack trigger = event;
        trigger.Kind = "trigger"; trigger.Target = "Actor"; trigger.Name = "Action";
        sequence.Tracks.push_back(trigger);
        SequenceTrack integer = event;
        integer.Kind = "int"; integer.Target = "Actor"; integer.Name = "Gesture";
        integer.Keys[0].Value = 2.0f;
        sequence.Tracks.push_back(integer);

        Json json;
        Serializer::Serialize(json, sequence);
        SequenceComponent restored;
        Serializer::Deserialize(json, restored);
        require(restored.Tracks.size() == 6 && restored.Bindings == sequence.Bindings &&
            restored.PrefetchTextures == sequence.PrefetchTextures,
            "Sequence reflection lost tracks, bindings or texture read-ahead assets.");
        require(!restored.Playing && restored.Events.empty(), "Transient playback state was serialized.");
        sequence.PrefetchTextures.clear();

        SequenceSystem system(&level);
        system.OnRuntiemStart();
        sequence.Playing = true;
        system.OnUpdateRuntime(Timestep(0.0f));
        require(camera.GetComponent<CameraComponent>().Primary && !gameplay.GetComponent<CameraComponent>().Primary,
            "Sequence did not acquire the active camera.");
        require(!camera.GetComponent<FollowCameraComponent>().bHasTarget, "Camera follow still overrides sequence keys.");
#if HE_ENABLE_PHYSICS_3D
        require(!actor.GetComponent<CharacterMovementComponent>().bPlayerInputEnabled, "Sequence did not acquire actor input.");
#endif
        system.OnUpdateRuntime(Timestep(0.5f));
        require(std::abs(camera.GetComponent<TransformComponent>().Translation.x - 1.0f) < 1e-5f, "Camera interpolation is incorrect.");
        require(std::abs(animator.FacialWeights["Smile"] - 0.5f) < 1e-5f && std::abs(animator.GetFloat("Speed") - 0.5f) < 1e-5f,
            "Expression and animation curves are not synchronized.");
        require(sequence.Events.size() == 1 && animator.GetTrigger("Action"), "Crossed timeline events were lost.");
        require(animator.GetInt("Gesture") == 2, "Integer timeline track did not drive the controller.");
        sequence.Paused = true;
        system.OnUpdateRuntime(Timestep(0.2f));
        require(sequence.Events.size() == 1 && sequence.Time == 0.5f, "Pause advanced the clock or duplicated a cue.");
        sequence.SeekTime = 0.1f;
        system.OnUpdateRuntime(Timestep(0.0f));
        require(sequence.Events.size() == 1 && sequence.Time == 0.1f, "Silent seek emitted events.");
        sequence.Playing = false;
        system.OnUpdateRuntime(Timestep(0.0f));
        require(gameplay.GetComponent<CameraComponent>().Primary && !camera.GetComponent<CameraComponent>().Primary &&
            camera.GetComponent<FollowCameraComponent>().bHasTarget, "Stop did not restore camera ownership.");
        require(std::abs(animator.FacialWeights["Smile"] - 0.1f) < 1e-5f && std::abs(animator.GetFloat("Speed") - 0.2f) < 1e-5f,
            "Stop did not restore actor parameters.");
#if HE_ENABLE_PHYSICS_3D
        require(actor.GetComponent<CharacterMovementComponent>().bPlayerInputEnabled, "Stop did not restore input.");
#endif
        sequence.ReferenceBinding = "Actor";
        actor.GetComponent<TransformComponent>().Translation = glm::vec3(10, 0, 0);
        sequence.Paused = false;
        sequence.Playing = true;
        system.OnUpdateRuntime(Timestep(0.0f));
        system.OnUpdateRuntime(Timestep(0.5f));
        require(std::abs(camera.GetComponent<TransformComponent>().Translation.x - 11.0f) < 1e-5f,
            "Relocated actor reference did not move the cinematic camera.");
        require(actor.GetComponent<TransformComponent>().Translation.x == 10.0f,
            "Sequence reference alignment teleported the actor.");
        system.OnUpdateRuntime(Timestep(0.5f));
        system.OnUpdateRuntime(Timestep(0.0f));
        require(!sequence.Playing && gameplay.GetComponent<CameraComponent>().Primary,
            "Natural sequence completion did not restore gameplay camera.");
        LimbConstraint stagedLimb;
        stagedLimb.End = "hand";
        stagedLimb.WorldTarget = glm::vec3(1, 2, 3);
        stagedLimb.WorldRotationDegrees = glm::vec3(15, 25, -35);
        stagedLimb.RotationWeight = 1;
        stagedLimb.Weight = .2f;
        auto entityLimb = stagedLimb;
        entityLimb.End = "otherHand";
        entityLimb.TargetUUID = camera.UUID().str();
        animator.LimbTargets = {stagedLimb, entityLimb};
        SequenceTrack limbTrack;
        limbTrack.Kind = "limb"; limbTrack.Target = "Actor"; limbTrack.Name = "hand";
        SequenceKey limbKey;
        limbKey.Value = 1;
        limbTrack.Keys = {limbKey};
        sequence.Tracks.push_back(limbTrack);
        limbTrack.Name = "otherHand";
        sequence.Tracks.push_back(limbTrack);
        actor.GetComponent<TransformComponent>().Rotation = glm::vec3(0, .7f, 0);
        const glm::mat4 stage = level.GetWorldTransform(actor);
        const glm::quat expectedRotation = glm::quat(glm::vec3(0, .7f, 0)) *
            glm::quat(glm::radians(stagedLimb.WorldRotationDegrees));
        sequence.Playing = true;
        sequence.Paused = true;
        system.OnUpdateRuntime(Timestep(0));
        for (int repeat = 0; repeat < 3; ++repeat)
        {
            sequence.SeekTime = .2f;
            system.OnUpdateRuntime(Timestep(0));
            require(glm::distance(animator.LimbTargets[0].WorldTarget,
                glm::vec3(stage * glm::vec4(stagedLimb.WorldTarget, 1))) < 1e-5f,
                "World limb target was not staged or accumulated repeated seek transforms");
            for (const auto& limb : animator.LimbTargets)
                require(std::abs(glm::dot(glm::quat(glm::radians(limb.WorldRotationDegrees)), expectedRotation)) > .99999f,
                    "Limb world orientation failed rotated stage alignment or accumulated on seek");
            require(animator.LimbTargets[1].WorldTarget == entityLimb.WorldTarget,
                "Entity target fallback was transformed twice by limb staging");
        }
        sequence.Playing = false;
        system.OnUpdateRuntime(Timestep(0));
        require(animator.LimbTargets[0].WorldRotationDegrees == stagedLimb.WorldRotationDegrees &&
            animator.LimbTargets[0].WorldTarget == stagedLimb.WorldTarget &&
            animator.LimbTargets[0].Weight == stagedLimb.Weight,
            "Stop did not restore authored limb pose constraints");
        sequence.Tracks[0].Keys[1].Time = 0.0f;
        sequence.Playing = true;
        system.OnUpdateRuntime(Timestep(0.0f));
        require(!sequence.Playing && !sequence.Error.empty(), "Duplicate key times were silently accepted.");
        system.OnRuntimeStop();
    }
}
