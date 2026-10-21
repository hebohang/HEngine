#include "Hepch.h"

#include "Runtime/EcsFramework/System/Animation/SequenceSystem.h"
#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Audio/ProjectAudioPath.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Animation/SequenceComponent.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/FollowCameraComponent.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/AssetReadAhead.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace HEngine
{
    namespace
    {
        Entity Resolve(Level* level, const std::string& id)
        {
            try { return level->TryGetEntityByUUID(UUID(id)); }
            catch (...) { return {}; }
        }

        std::string TargetId(const SequenceComponent& sequence, const SequenceTrack& track)
        {
            auto found = sequence.Bindings.find(track.Target);
            return found != sequence.Bindings.end() ? found->second : track.Target;
        }

        void CameraCut(Level* level)
        {
            if (auto* renderer = level->TryGetSystem<RenderSystem3D>()) renderer->RequestCameraCut();
        }

        SequenceKey Sample(const SequenceTrack& track, float time)
        {
            if (time <= track.Keys.front().Time) return track.Keys.front();
            if (time >= track.Keys.back().Time) return track.Keys.back();
            auto right = std::upper_bound(track.Keys.begin(), track.Keys.end(), time,
                [](float value, const SequenceKey& key) { return value < key.Time; });
            const auto& left = *(right - 1);
            float alpha = (time - left.Time) / (right->Time - left.Time);
            if (track.Smooth) alpha = alpha * alpha * (3.0f - 2.0f * alpha);
            SequenceKey result;
            result.Value = glm::mix(left.Value, right->Value, alpha);
            result.Position = glm::mix(left.Position, right->Position, alpha);
            result.Rotation = glm::eulerAngles(glm::slerp(glm::quat(left.Rotation), glm::quat(right->Rotation), alpha));
            return result;
        }
    }

    struct SequenceSystem::State
    {
        struct Snapshot
        {
            std::optional<TransformComponent> Transform;
            std::optional<CameraComponent> Camera;
            std::optional<bool> Follow;
            std::optional<bool> Input;
            std::optional<std::map<std::string, Json>> Parameters;
            std::optional<std::map<std::string, float>> Expressions;
            std::optional<bool> AnimationPaused;
            std::optional<std::vector<BoneLookAt>> LookAt;
            std::optional<std::vector<LimbConstraint>> Limbs;
        };
        std::string Owner;
        std::string ActiveCamera;
        std::string GameplayCamera;
        glm::mat4 EntryCameraWorld{1.0f};
        glm::mat4 StageDelta{1.0f};
        glm::quat StageRotation{1, 0, 0, 0};
        std::map<std::string, Snapshot> Saved;
        struct AudioCue { AudioHandle Handle; float StartTime = 0.0f; };
        std::vector<AudioCue> Audio;
        float PreviousTime = -1.0f;

        void StopAudio()
        {
            for (auto cue : Audio) AudioEngine::GetInstance().Stop(cue.Handle);
            Audio.clear();
        }

        void Restore(Level* level)
        {
            StopAudio();
            for (auto& [id, saved] : Saved)
            {
                auto entity = Resolve(level, id);
                if (!entity) continue;
                if (saved.Transform && entity.HasComponent<TransformComponent>())
                {
                    auto& transform = entity.GetComponent<TransformComponent>();
                    transform.Translation = saved.Transform->Translation;
                    transform.Rotation = saved.Transform->Rotation;
                    transform.Scale = saved.Transform->Scale;
                }
                if (saved.Camera && entity.HasComponent<CameraComponent>())
                {
                    auto& camera = entity.GetComponent<CameraComponent>();
                    camera.Primary = saved.Camera->Primary;
                    camera.Camera.SetPerspectiveVerticalFOV(saved.Camera->Camera.GetPerspectiveVerticalFOV());
                }
                if (saved.Follow && entity.HasComponent<FollowCameraComponent>())
                {
                    auto& follow = entity.GetComponent<FollowCameraComponent>();
                    follow.bHasTarget = *saved.Follow;
                }
                if (saved.Input && entity.HasComponent<CharacterMovementComponent>())
                    entity.GetComponent<CharacterMovementComponent>().bPlayerInputEnabled = *saved.Input;
                if (entity.HasComponent<AnimatorComponent>())
                {
                    auto& animator = entity.GetComponent<AnimatorComponent>();
                    if (saved.Parameters) animator.mParameters = *saved.Parameters;
                    if (saved.Expressions) animator.FacialWeights = *saved.Expressions;
                    if (saved.AnimationPaused) animator.bPaused = *saved.AnimationPaused;
                    if (saved.LookAt) animator.LookAt = *saved.LookAt;
                    if (saved.Limbs) animator.LimbTargets = *saved.Limbs;
                }
            }
            if (!Owner.empty()) CameraCut(level);
            Owner.clear();
            ActiveCamera.clear();
            GameplayCamera.clear();
            Saved.clear();
            PreviousTime = -1.0f;
            StageDelta = glm::mat4(1.0f);
        }

        bool Begin(Level* level, const std::string& owner, SequenceComponent& sequence)
        {
            sequence.Error.clear();
            StageDelta = glm::mat4(1.0f);
            if (!std::isfinite(sequence.Duration) || sequence.Duration <= 0.0f)
                sequence.Error = "Duration must be finite and positive.";
            if (!sequence.ReferenceBinding.empty())
            {
                const auto binding = sequence.Bindings.find(sequence.ReferenceBinding);
                auto reference = Resolve(level, binding == sequence.Bindings.end() ? sequence.ReferenceBinding : binding->second);
                if (!reference || !reference.HasComponent<TransformComponent>())
                    sequence.Error = "Reference binding must resolve to a transform.";
                else
                    StageDelta = level->GetWorldTransform(reference) * glm::inverse(
                        glm::translate(glm::mat4(1), sequence.ReferencePosition) * glm::toMat4(glm::quat(sequence.ReferenceRotation)) *
                        glm::scale(glm::mat4(1), sequence.ReferenceScale));
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row)
                        if (!std::isfinite(StageDelta[column][row])) sequence.Error = "Reference transform must be finite and invertible.";
            }
            glm::vec3 stageScale, stageTranslation, stageSkew;
            glm::vec4 stagePerspective;
            StageRotation = glm::quat(1, 0, 0, 0);
            if (!glm::decompose(StageDelta, stageScale, StageRotation, stageTranslation, stageSkew, stagePerspective))
                sequence.Error = "Reference stage rotation must be decomposable.";
            else StageRotation = glm::normalize(StageRotation);
            for (const auto& track : sequence.Tracks)
            {
                if (!sequence.Error.empty()) break;
                if (track.Keys.empty()) { sequence.Error = "A track has no keys."; break; }
                float previous = -1.0f;
                for (const auto& key : track.Keys)
                {
                    if (!std::isfinite(key.Time) || key.Time < 0.0f || key.Time <= previous ||
                        key.Time > sequence.Duration || !std::isfinite(key.Value) ||
                        !std::isfinite(key.Position.x) || !std::isfinite(key.Position.y) || !std::isfinite(key.Position.z) ||
                        !std::isfinite(key.Rotation.x) || !std::isfinite(key.Rotation.y) || !std::isfinite(key.Rotation.z))
                        sequence.Error = "Keys must be finite, ordered, unique, and inside the duration.";
                    previous = key.Time;
                }
                auto target = Resolve(level, TargetId(sequence, track));
                if (track.Kind == "camera")
                {
                    if (!target || !target.HasComponent<CameraComponent>() || !target.HasComponent<TransformComponent>())
                        sequence.Error = "Camera binding is missing a camera or transform: " + track.Target;
                }
                else if (track.Kind == "transform")
                {
                    if (!target || !target.HasComponent<TransformComponent>())
                        sequence.Error = "Transform binding is missing: " + track.Target;
                }
                else if (track.Kind == "float" || track.Kind == "int" || track.Kind == "bool" || track.Kind == "expression" || track.Kind == "lookat" || track.Kind == "limb" || track.Kind == "trigger")
                {
                    if (!target || !target.HasComponent<AnimatorComponent>())
                        sequence.Error = "Actor binding is missing an animator: " + track.Target;
                    else if (target.HasComponent<MeshComponent>() && !target.GetComponent<AnimatorComponent>().bLoaded)
                        return false; // AnimationSystem initializes imported clips on its first tick.
                    else if (track.Kind == "lookat")
                    {
                        const auto& lookAt = target.GetComponent<AnimatorComponent>().LookAt;
                        if (std::none_of(lookAt.begin(), lookAt.end(), [&](const auto& look) { return look.Bone == track.Name; }))
                            sequence.Error = "Look-at bone has no authored configuration: " + track.Name;
                    }
                    else if (track.Kind == "limb")
                    {
                        const auto& limbs = target.GetComponent<AnimatorComponent>().LimbTargets;
                        if (std::none_of(limbs.begin(), limbs.end(), [&](const auto& limb) { return limb.End == track.Name; }))
                            sequence.Error = "Limb end bone has no authored configuration: " + track.Name;
                    }
                    else if (track.Kind != "expression")
                    {
                        const auto& types = target.GetComponent<AnimatorComponent>().mParameterTypes;
                        const auto found = types.find(track.Name);
                        const auto expected = track.Kind == "float" ? AnimControllerParamType::Float :
                            track.Kind == "int" ? AnimControllerParamType::Int :
                            track.Kind == "bool" ? AnimControllerParamType::Bool : AnimControllerParamType::Trigger;
                        if (found == types.end() || found->second != expected)
                            sequence.Error = "Animation parameter is absent or has the wrong type: " + track.Name;
                    }
                }
                else if (track.Kind == "audio")
                {
                    ProjectAudioPath path;
                    if (!TryResolveProjectAudioPath(ConfigManager::GetInstance().GetRootFolder(), track.Name, path) ||
                        !std::filesystem::is_regular_file(path.FullPath))
                        sequence.Error = "Audio asset is missing or outside the project: " + track.Name;
                }
                else if (track.Kind != "event") sequence.Error = "Unknown track kind: " + track.Kind;
            }
            if (!sequence.Error.empty()) { sequence.Playing = false; HE_CORE_ERROR("Sequence: {0}", sequence.Error); return false; }
            Owner = owner;
            sequence.Time = 0.0f;
            sequence.Events = Json::array();
            auto cameras = level->GetAllEntitiesWith<IDComponent, CameraComponent>();
            for (auto handle : cameras)
            {
                Entity entity(handle, level);
                const auto id = entity.UUID().str();
                Saved[id].Camera = entity.GetComponent<CameraComponent>();
                if (Saved[id].Camera->Primary && GameplayCamera.empty())
                {
                    GameplayCamera = id;
                    EntryCameraWorld = level->GetWorldTransform(entity);
                }
            }
            for (const auto& track : sequence.Tracks)
            {
                auto id = TargetId(sequence, track);
                auto entity = Resolve(level, id);
                if (!entity) continue;
                auto& saved = Saved[id];
                if (track.Kind == "camera" || track.Kind == "transform")
                {
                    if (!saved.Transform) saved.Transform = entity.GetComponent<TransformComponent>();
                    if (track.Kind == "camera" && entity.HasComponent<FollowCameraComponent>() && !saved.Follow)
                    {
                        auto& follow = entity.GetComponent<FollowCameraComponent>();
                        saved.Follow = follow.bHasTarget;
                        follow.bHasTarget = false;
                    }
                }
                if (entity.HasComponent<CharacterMovementComponent>() && !saved.Input)
                {
                    auto& movement = entity.GetComponent<CharacterMovementComponent>();
                    saved.Input = movement.bPlayerInputEnabled;
                    movement.bPlayerInputEnabled = false;
                    movement.RuntimeDesiredVelocity = glm::vec3(0.0f);
                    movement.RuntimeHasExternalVelocity = false;
                }
                if (entity.HasComponent<AnimatorComponent>() && !saved.Parameters)
                {
                    auto& animator = entity.GetComponent<AnimatorComponent>();
                    saved.Parameters = animator.mParameters;
                    saved.Expressions = animator.FacialWeights;
                    saved.AnimationPaused = animator.bPaused;
                    saved.LookAt = animator.LookAt;
                    saved.Limbs = animator.LimbTargets;
                }
            }
            CameraCut(level);
            return true;
        }

        void Evaluate(Level* level, SequenceComponent& sequence, bool fireEvents)
        {
            for (const auto& [id, saved] : Saved)
            {
                auto actor = Resolve(level, id);
                if (saved.AnimationPaused && actor && actor.HasComponent<AnimatorComponent>())
                    actor.GetComponent<AnimatorComponent>().bPaused = sequence.Paused || *saved.AnimationPaused;
            }
            const SequenceTrack* camera = nullptr;
            for (const auto& track : sequence.Tracks)
            {
                if (track.Keys.empty()) continue;
                if (track.Kind == "camera" && sequence.Time >= track.Keys.front().Time &&
                    (!camera || track.Keys.front().Time >= camera->Keys.front().Time)) camera = &track;
                auto entity = Resolve(level, TargetId(sequence, track));
                if (track.Kind == "transform" && entity && entity.HasComponent<TransformComponent>())
                {
                    const auto sample = Sample(track, sequence.Time);
                    const auto saved = Saved.find(TargetId(sequence, track));
                    const auto scale = saved != Saved.end() && saved->second.Transform
                        ? saved->second.Transform->Scale : glm::vec3(1.0f);
                    level->SetWorldTransform(entity, StageDelta * glm::translate(glm::mat4(1), sample.Position) *
                        glm::toMat4(glm::quat(sample.Rotation)) * glm::scale(glm::mat4(1), scale));
                }
                if (track.Kind == "lookat" && entity && entity.HasComponent<AnimatorComponent>())
                {
                    const auto sample = Sample(track, sequence.Time);
                    for (auto& look : entity.GetComponent<AnimatorComponent>().LookAt)
                        if (look.Bone == track.Name) { look.WorldTarget = glm::vec3(StageDelta * glm::vec4(sample.Position, 1)); look.Weight = std::clamp(sample.Value, 0.0f, 1.0f); }
                }
                if (track.Kind == "limb" && entity && entity.HasComponent<AnimatorComponent>())
                {
                    const auto sample = Sample(track, sequence.Time);
                    const auto saved = Saved.find(TargetId(sequence, track));
                    auto& limbs = entity.GetComponent<AnimatorComponent>().LimbTargets;
                    if (saved != Saved.end() && saved->second.Limbs)
                        for (size_t index = 0; index < limbs.size() && index < saved->second.Limbs->size(); ++index)
                        {
                            auto& limb = limbs[index];
                            const auto& original = (*saved->second.Limbs)[index];
                            if (limb.End != track.Name) continue;
                            limb.Weight = std::clamp(sample.Value, 0.0f, 1.0f);
                            limb.WorldRotationDegrees = glm::degrees(glm::eulerAngles(StageRotation *
                                glm::quat(glm::radians(original.WorldRotationDegrees))));
                            if (original.TargetUUID.empty())
                                limb.WorldTarget = glm::vec3(StageDelta * glm::vec4(original.WorldTarget, 1));
                        }
                }
                if ((track.Kind == "float" || track.Kind == "int" || track.Kind == "bool" || track.Kind == "expression") && entity && entity.HasComponent<AnimatorComponent>())
                {
                    auto& animator = entity.GetComponent<AnimatorComponent>();
                    const float value = Sample(track, sequence.Time).Value;
                    if (track.Kind == "float") animator.SetFloat(track.Name, value);
                    else if (track.Kind == "expression") animator.FacialWeights[track.Name] = std::clamp(value, 0.0f, 1.0f);
                    else
                    {
                        auto key = std::upper_bound(track.Keys.begin(), track.Keys.end(), sequence.Time,
                            [](float time, const SequenceKey& item) { return time < item.Time; });
                        const float stepValue = (key == track.Keys.begin() ? *key : *(key - 1)).Value;
                        if (track.Kind == "int") animator.SetInt(track.Name, static_cast<int>(std::clamp(stepValue, -1000000.0f, 1000000.0f)));
                        else animator.SetBool(track.Name, stepValue >= 0.5f);
                    }
                }
                if (!fireEvents) continue;
                for (const auto& key : track.Keys)
                {
                    if (key.Time <= PreviousTime || key.Time > sequence.Time) continue;
                    if (track.Kind == "trigger" && entity && entity.HasComponent<AnimatorComponent>())
                        entity.GetComponent<AnimatorComponent>().SetTrigger(track.Name);
                    else if (track.Kind == "audio")
                    {
                        ProjectAudioPath path;
                        if (TryResolveProjectAudioPath(ConfigManager::GetInstance().GetRootFolder(), track.Name, path))
                        {
                            AudioPlayParams params;
                            params.Volume = std::clamp(key.Value, 0.0f, 1.0f);
                            auto handle = AudioEngine::GetInstance().Play(path.FullPath, params);
                            if (handle)
                            {
                                Audio.push_back({handle, key.Time});
                                if (sequence.Time - key.Time > 0.005f) AudioEngine::GetInstance().Seek(handle, sequence.Time - key.Time);
                            }
                            else sequence.Error = "Audio playback failed: " + track.Name;
                        }
                    }
                    else if (track.Kind == "event")
                    {
                        if (sequence.Events.size() >= 256) sequence.Events.erase(sequence.Events.begin());
                        sequence.Events.push_back({{"name", track.Name}, {"time", key.Time}, {"value", key.Value}});
                    }
                }
            }
            if (camera)
            {
                auto id = TargetId(sequence, *camera);
                auto entity = Resolve(level, id);
                if (entity && entity.HasComponent<TransformComponent>() && entity.HasComponent<CameraComponent>())
                {
                    auto sample = Sample(*camera, sequence.Time);
                    auto& transform = entity.GetComponent<TransformComponent>();
                    transform.Translation = sample.Position;
                    transform.Rotation = sample.Rotation;
                    if (!sequence.ReferenceBinding.empty())
                        level->SetWorldTransform(entity, StageDelta * glm::translate(glm::mat4(1), sample.Position) * glm::toMat4(glm::quat(sample.Rotation)));
                    glm::mat4 blendTarget = EntryCameraWorld;
                    float weight = sequence.BlendIn > 0.0f ? std::clamp(sequence.Time / sequence.BlendIn, 0.0f, 1.0f) : 1.0f;
                    if (!sequence.Loop && sequence.BlendOut > 0.0f && sequence.Time > sequence.Duration - sequence.BlendOut)
                    {
                        weight = std::clamp((sequence.Duration - sequence.Time) / sequence.BlendOut, 0.0f, 1.0f);
                        auto gameplay = Resolve(level, GameplayCamera);
                        if (gameplay && GameplayCamera != id) blendTarget = level->GetWorldTransform(gameplay);
                    }
                    if (weight < 1.0f && !GameplayCamera.empty())
                    {
                        const auto world = level->GetWorldTransform(entity);
                        glm::vec3 scaleA, scaleB, positionA, positionB, skew;
                        glm::quat rotationA, rotationB;
                        glm::vec4 perspective;
                        if (glm::decompose(blendTarget, scaleA, rotationA, positionA, skew, perspective) &&
                            glm::decompose(world, scaleB, rotationB, positionB, skew, perspective))
                        {
                            weight = weight * weight * (3.0f - 2.0f * weight);
                            level->SetWorldTransform(entity, glm::translate(glm::mat4(1), glm::mix(positionA, positionB, weight)) *
                                glm::toMat4(glm::slerp(rotationA, rotationB, weight)) * glm::scale(glm::mat4(1), scaleB));
                        }
                    }
                    float fov = glm::radians(std::clamp(sample.Value, 5.0f, 150.0f));
                    const auto original = Saved.find(GameplayCamera);
                    if (weight < 1.0f && original != Saved.end() && original->second.Camera)
                        fov = glm::mix(original->second.Camera->Camera.GetPerspectiveVerticalFOV(), fov, weight);
                    entity.GetComponent<CameraComponent>().Camera.SetPerspectiveVerticalFOV(fov);
                    for (auto handle : level->GetAllEntitiesWith<CameraComponent>())
                        Entity(handle, level).GetComponent<CameraComponent>().Primary = (Entity(handle, level).UUID().str() == id);
                    if (ActiveCamera != id) { ActiveCamera = id; CameraCut(level); }
                }
            }
            PreviousTime = sequence.Time;
        }
    };

    SequenceSystem::SequenceSystem(Level* level) : System(level), mState(std::make_unique<State>()) {}
    SequenceSystem::~SequenceSystem() = default;

    void SequenceSystem::OnRuntiemStart()
    {
        mState->Restore(mLevel);
        AssetReadAhead& readAhead = AssetReadAhead::GetInstance();
        readAhead.BeginScene();
        for (auto handle : mLevel->GetAllEntitiesWith<SequenceComponent>())
        {
            auto& sequence = Entity(handle, mLevel).GetComponent<SequenceComponent>();
            sequence.Playing = sequence.AutoPlay;
            sequence.Paused = false;
            sequence.Time = 0.0f;
            sequence.SeekTime = -1.0f;
            for (const std::string& assetPath : sequence.PrefetchTextures)
            {
                std::filesystem::path resolved;
                std::string normalized;
                const auto validation = Utils::TryResolveProjectRelativePath(
                    ConfigManager::GetInstance().GetRootFolder(), assetPath,
                    resolved, normalized);
                if (!validation || resolved.extension() != ".dds" ||
                    !readAhead.Queue(resolved))
                    HE_CORE_WARN("Sequence DDS read-ahead rejected asset '{}'", assetPath);
            }
        }
    }

    void SequenceSystem::OnRuntimeStop()
    {
        mState->Restore(mLevel);
        AssetReadAhead::GetInstance().EndScene();
    }

    void SequenceSystem::OnUpdateRuntime(Timestep ts)
    {
        HE_PROFILE;
        if (!mState->Owner.empty())
        {
            auto owner = Resolve(mLevel, mState->Owner);
            if (!owner || !owner.HasComponent<SequenceComponent>() || !owner.GetComponent<SequenceComponent>().Playing)
                mState->Restore(mLevel);
        }
        for (auto handle : mLevel->GetAllEntitiesWith<SequenceComponent>())
        {
            Entity entity(handle, mLevel);
            auto& sequence = entity.GetComponent<SequenceComponent>();
            if (!sequence.Playing) continue;
            const auto id = entity.UUID().str();
            if (mState->Owner.empty() && mLevel->IsRuntimePaused() && sequence.SeekTime < 0.0f) continue;
            if (mState->Owner.empty()) { if (!mState->Begin(mLevel, id, sequence)) continue; }
            if (mState->Owner != id)
            {
                sequence.Error = "Another sequence owns the level. Stop it before starting this sequence.";
                sequence.Playing = false;
                continue;
            }
            bool fireEvents = true;
            if (sequence.SeekTime >= 0.0f && std::isfinite(sequence.SeekTime))
            {
                sequence.Time = std::clamp(sequence.SeekTime, 0.0f, sequence.Duration);
                sequence.SeekTime = -1.0f;
                mState->StopAudio();
                CameraCut(mLevel);
                fireEvents = false;
            }
            else if (mState->PreviousTime >= 0.0f && !sequence.Paused && !mLevel->IsRuntimePaused())
                sequence.Time = std::min(sequence.Time + std::max(0.0f, static_cast<float>(ts)), sequence.Duration);
            for (auto cue : mState->Audio)
            {
                auto& audio = AudioEngine::GetInstance();
                audio.Pause(cue.Handle, sequence.Paused || mLevel->IsRuntimePaused());
                const double desired = std::max(0.0f, sequence.Time - cue.StartTime);
                if (audio.IsPlaying(cue.Handle) && std::abs(audio.GetPlaybackPosition(cue.Handle) - desired) > 0.08)
                    audio.Seek(cue.Handle, desired);
            }
            mState->Evaluate(mLevel, sequence, fireEvents);
            if (sequence.Time >= sequence.Duration && !sequence.Paused)
            {
                if (sequence.Loop) { sequence.Time = 0.0f; mState->PreviousTime = -1.0f; mState->StopAudio(); CameraCut(mLevel); }
                else sequence.Playing = false;
            }
        }
    }
}
