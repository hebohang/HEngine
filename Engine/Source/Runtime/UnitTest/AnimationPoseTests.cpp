#include "Hepch.h"

#include "Runtime/UnitTest/AnimationPoseTests.h"
#include "Runtime/UnitTest/Fixtures/ClothSimulationReference.h"
#include "Runtime/Animation/Animator.h"
#include "Runtime/Animation/AnimationStateMachine.h"
#include "Runtime/Animation/CharacterDeformation.h"
#include "Runtime/Graphics/Mesh/MeshDeformationInstanceCache.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Animation/MotionBlend.h"
#include "Runtime/Animation/SkeletonCompatibility.h"
#include "Runtime/Camera/CameraSpring.h"
#include "Runtime/Camera/FixedPositionHistory.h"
#include "Runtime/Core/SimulationClock.h"
#include "Runtime/Utils/MathUtils/Rotation.h"
#include "Runtime/Utils/MathUtils/MathUtils.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/System/Animation/AnimationSystem.h"
#include "Runtime/Platform/Scripting/ScriptPlatformServices.h"
#include "Runtime/Scripting/ScriptWorld.h"
#include "_generated/serializer/Animation.gen.h"
#include "_generated/serializer/AnimController.gen.h"
#include "_generated/serializer/SecondaryMotion.gen.h"
#include "_generated/serializer/CharacterPose.gen.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, const char* message)
        {
            if (!condition) throw std::runtime_error(message);
        }

        void VerifyMeshInstanceReuse()
        {
            auto source = CreateRef<Mesh>();
            std::vector<StaticVertex> vertices(1);
            vertices[0].Pos = glm::vec3(0.0f);
            source->mSubMeshes.push_back(CreateRef<SubMesh>(std::move(vertices), std::vector<uint32_t>{}));
            MeshDeformationInstanceCache cache;
            const auto first = cache.Acquire(1, source);
            const auto second = cache.Acquire(2, source);
            Require(first != source && second != first &&
                    first->mSubMeshes[0] != source->mSubMeshes[0] &&
                    first->mSubMeshes[0] != second->mSubMeshes[0],
                "Appearance owners shared writable deformation geometry.");
            const auto geometry = first->mSubMeshes[0];
            Require(cache.Acquire(1, source) == first && cache.Acquire(1, source)->mSubMeshes[0] == geometry,
                "Repeated appearance selection allocated a new GPU geometry identity.");
            first->mSubMeshes[0]->mStaticVertices[0].Pos.x = 9.0f;
            Require(second->mSubMeshes[0]->mStaticVertices[0].Pos.x == 0.0f &&
                    source->mSubMeshes[0]->mStaticVertices[0].Pos.x == 0.0f,
                "Appearance deformation leaked into another owner or source.");
            cache.RetainLiveOwners([](uint64_t owner) { return owner == 2; });
            Require(cache.Acquire(2, source) == second && cache.Acquire(1, source) != first,
                "Appearance cache failed to retire a destroyed owner.");
            const auto replacement = CreateRef<Mesh>(*source);
            Require(cache.Acquire(2, replacement) != second,
                "A changed appearance source reused stale geometry.");
        }

        void VerifyMorphTasking()
        {
            for (const size_t vertexCount : { size_t(16384), size_t(65536) })
            {
                std::vector<StaticVertex> base(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i)
                {
                    base[i].Pos = glm::vec3(static_cast<float>(i % 32), 0, 0);
                    base[i].Normal = glm::vec3(0, 0, 1);
                    base[i].Tangent = glm::vec3(1, 0, 0);
                    base[i].Bitangent = glm::vec3(0, 1, 0);
                }
                SubMesh section(std::move(base), {});
                MorphTarget first;
                first.Name = "First";
                first.Positions.assign(vertexCount, glm::vec3(.1f, 0, 0));
                first.Normals.assign(vertexCount, glm::vec3(.02f, 0, 0));
                MorphTarget second;
                second.Name = "Second";
                second.Positions.assign(vertexCount, glm::vec3(.2f, 0, 0));
                second.Tangents.assign(vertexCount, glm::vec3(0, .03f, 0));
                section.SetMorphTargets({ std::move(first), std::move(second) });
                const std::map<std::string, float> weights{ {"First", .5f}, {"Second", .25f} };
                section.BeginDeformation();
                section.ApplyMorphWeights(weights);
                section.EndDeformation();
                const glm::vec3 expectedNormal = glm::normalize(
                    glm::vec3(0, 0, 1) + glm::vec3(.02f, 0, 0) * .5f);
                glm::vec3 expectedTangent = glm::vec3(1, 0, 0) +
                    glm::vec3(0, .03f, 0) * .25f;
                const float sign = glm::dot(glm::cross(expectedNormal, expectedTangent),
                    glm::vec3(0, 1, 0)) < 0.0f ? -1.0f : 1.0f;
                expectedTangent = glm::normalize(expectedTangent - expectedNormal *
                    glm::dot(expectedNormal, expectedTangent));
                const glm::vec3 expectedBitangent = glm::cross(expectedNormal,
                    expectedTangent) * sign;
                for (size_t i = 0; i < vertexCount; ++i)
                {
                    const auto& vertex = section.mStaticVertices[i];
                    Require(std::abs(vertex.Pos.x -
                        (static_cast<float>(i % 32) + .1f)) < 1e-5f,
                        "Morphed vertex differs across serial and partitioned sizes");
                    Require(glm::distance(vertex.Normal, expectedNormal) < 1e-5f &&
                        glm::distance(vertex.Tangent, expectedTangent) < 1e-5f &&
                        glm::distance(vertex.Bitangent, expectedBitangent) < 1e-5f,
                        "Morphed tangent frame differs across serial and partitioned sizes");
                }
                if (vertexCount == 65536 && std::getenv("HE_MORPH_BENCHMARK"))
                {
                    std::vector<double> milliseconds;
                    milliseconds.reserve(20);
                    for (int repeat = 0; repeat < 20; ++repeat)
                    {
                        const auto start = std::chrono::steady_clock::now();
                        section.BeginDeformation();
                        section.ApplyMorphWeights(weights);
                        section.EndDeformation();
                        milliseconds.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start).count());
                    }
                    std::sort(milliseconds.begin(), milliseconds.end());
                    HE_CORE_INFO("[MorphTaskingMicrobenchmark] vertices={} samples={} p50_ms={} p95_ms={}",
                        vertexCount, milliseconds.size(), milliseconds[9], milliseconds[18]);
                }
            }

            // Exercise the frame update entry point, including unique mesh
            // ownership and the pure-morph cache, with enough vertices to use
            // the worker path. Alternate weights so each tick publishes data.
            constexpr size_t vertexCount = 65536;
            std::vector<StaticVertex> vertices(vertexCount);
            for (size_t i = 0; i < vertexCount; ++i)
            {
                vertices[i].Pos = glm::vec3(static_cast<float>(i % 32), 0, 0);
                vertices[i].Normal = glm::vec3(0, 0, 1);
                vertices[i].Tangent = glm::vec3(1, 0, 0);
                vertices[i].Bitangent = glm::vec3(0, 1, 0);
            }
            Mesh mesh;
            mesh.mSubMeshes.push_back(CreateRef<SubMesh>(std::move(vertices), std::vector<uint32_t>{}));
            MorphTarget first;
            first.Name = "First";
            first.Positions.assign(vertexCount, glm::vec3(.1f, 0, 0));
            MorphTarget second;
            second.Name = "Second";
            second.Positions.assign(vertexCount, glm::vec3(.2f, 0, 0));
            mesh.mSubMeshes[0]->SetMorphTargets({ std::move(first), std::move(second) });
            AnimatorComponent animator;
            animator.FacialWeights["First"] = .5f;
            animator.FacialWeights["Second"] = .25f;
            const auto update = [&](int frame)
            {
                const bool alternate = (frame & 1) != 0;
                animator.FacialWeights["First"] = alternate ? .25f : .5f;
                animator.FacialWeights["Second"] = alternate ? .5f : .25f;
                Require(UpdateCharacterDeformation(animator, mesh, glm::mat4(1),
                    1.0f / 60.0f, true) == vertexCount,
                    "Frame morph path did not publish the expected vertex count");
                const auto& deformed = mesh.mSubMeshes[0]->mStaticVertices;
                const float offset = alternate ? .125f : .1f;
                for (size_t i : { size_t(0), vertexCount / 2, vertexCount - 1 })
                    Require(std::abs(deformed[i].Pos.x - (static_cast<float>(i % 32) + offset)) < 1e-5f,
                        "Frame morph path published an incorrect position");
            };
            update(0);
            if (std::getenv("HE_MORPH_BENCHMARK"))
            {
                std::vector<double> milliseconds;
                milliseconds.reserve(20);
                for (int frame = 1; frame <= 20; ++frame)
                {
                    const auto start = std::chrono::steady_clock::now();
                    update(frame);
                    milliseconds.push_back(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count());
                }
                std::sort(milliseconds.begin(), milliseconds.end());
                HE_CORE_INFO("[MorphFramePathBenchmark] vertices={} samples={} p50_ms={} p95_ms={}",
                    vertexCount, milliseconds.size(), milliseconds[9], milliseconds[18]);
            }
            else update(1);
        }

        void VerifyClothPreparedConstraints()
        {
            // Paired CPU-only microbenchmark against the frozen pre-hoisting
            // solver. Timings are diagnostic; only trajectory equivalence gates.
            std::vector<glm::vec3> rest;
            std::vector<float> radii;
            std::vector<uint32_t> triangles;
            constexpr uint32_t width = 12;
            for (uint32_t y = 0; y < width; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    rest.emplace_back((float(x) - 5.5f) * .035f, 1.4f - float(y) * .035f, 0);
                    radii.push_back(y == 0 ? 0.0f : .25f);
                }
            for (uint32_t y = 0; y + 1 < width; ++y)
                for (uint32_t x = 0; x + 1 < width; ++x)
                {
                    const uint32_t a = y * width + x;
                    triangles.insert(triangles.end(), {a, a + width, a + 1, a + 1, a + width, a + width + 1});
                }
            ClothSimulation optimized;
            ReferenceClothSimulation reference;
            ClothPanel settings;
            double referenceMilliseconds = 0, optimizedMilliseconds = 0;
            float maximumDifference = 0;
            const float intervals[] = {1.f/120, 1.f/60, 1.f/30, 1.f/15, .1f, 0};
            for (int frame = 0; frame < 180; ++frame)
            {
                const float offset = .03f * std::sin(float(frame) * .05f);
                std::vector<ClothWorldCapsule> capsules = {
                    {{-.1f + offset,1.03f,-.06f},{-.1f + offset,1.4f,-.06f},.07f},
                    {{.1f,1.08f,-.055f},{.1f,1.08f,-.055f},.065f},
                    {{0,1,0},{0,1,0},-1},
                    {{std::numeric_limits<float>::quiet_NaN(),0,0},{0,0,0},.2f}};
                settings.Iterations = frame % 3 == 0 ? 12 : 8;
                settings.Thickness = frame % 13 == 0 ? std::numeric_limits<float>::quiet_NaN() : .008f;
                settings.StretchCompliance = frame % 11 == 0 ? -.1f : .000001f;
                settings.BendCompliance = frame % 17 == 0 ? std::numeric_limits<float>::quiet_NaN() : .001f;
                settings.SelfCollision = frame >= 150 && frame < 165;
                const float dt = frame == 90 ? .5f : intervals[(frame / 15) % 6];
                const glm::vec3 wind(1, 0, 2 + offset);
                const auto runReference = [&] {
                    const auto start = std::chrono::steady_clock::now();
                    reference.Step(dt, rest, triangles, radii, capsules, wind, settings, glm::vec3(0));
                    referenceMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                };
                const auto runOptimized = [&] {
                    const auto start = std::chrono::steady_clock::now();
                    optimized.Step(dt, rest, triangles, radii, capsules, wind, settings, glm::vec3(0));
                    optimizedMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                };
                if (frame % 2) { runOptimized(); runReference(); }
                else { runReference(); runOptimized(); }
                Require(optimized.Positions().size() == reference.Positions().size() &&
                    optimized.WasReset() == reference.WasReset(), "Prepared cloth constraints changed reset semantics");
                for (size_t i = 0; i < rest.size(); ++i)
                {
                    const float difference = glm::distance(optimized.Positions()[i], reference.Positions()[i]);
                    Require(std::isfinite(difference) && difference <= .00001f,
                        "Prepared cloth constraints diverged from the original trajectory by over 10 micrometres");
                    maximumDifference = std::max(maximumDifference, difference);
                }
            }
            HE_CORE_INFO("[ClothMicrobenchmark] frames=180 particles=144 reference_ms={} prepared_ms={} max_position_delta={}",
                referenceMilliseconds, optimizedMilliseconds, maximumDifference);
        }

        AnimRes MakeRig(float degrees)
        {
            AnimRes resource;
            resource.rootNode.name = "root";
            resource.rootNode.localTransform = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            AnimNodeRes child;
            child.name = "child";
            child.localTransform = {1,0,0,0, 0,1,0,0, 0,0,1,0, 1,0,0,1};
            resource.rootNode.children.push_back(child);
            AnimBoneInfoRes rootBone;
            rootBone.name = "root";
            rootBone.id = 0;
            rootBone.offsetMatrix = resource.rootNode.localTransform;
            resource.boneInfos.push_back(rootBone);
            rootBone.name = "child";
            rootBone.id = 1;
            resource.boneInfos.push_back(rootBone);
            AnimClipRes clip;
            clip.duration = 1.0f;
            clip.ticksPerSecond = 1.0f;
            AnimBoneChannelRes channel;
            channel.name = "root";
            const glm::quat rotation = glm::angleAxis(glm::radians(degrees), glm::vec3(0,0,1));
            AnimKeyRotationRes key;
            key.qw = rotation.w; key.qx = rotation.x; key.qy = rotation.y; key.qz = rotation.z;
            channel.rotationKeys.push_back(key);
            clip.channels.push_back(channel);
            // An explicitly present but empty child channel must retain bind translation.
            channel = {};
            channel.name = "child";
            clip.channels.push_back(channel);
            resource.clips.push_back(clip);
            return resource;
        }
    }

    void AnimationPoseContract()
    {
        VerifyMeshInstanceReuse();
        VerifyMorphTasking();
        {
            const std::map<std::string, BoneInfo> reference{
                { "root", { 0, glm::mat4(1) } }, { "head", { 1, glm::mat4(1) } } };
            Require(GetSkinPaletteIncompatibility(reference, reference).empty(),
                "Identical wardrobe skin palettes were rejected.");
            auto changed = reference;
            changed.at("head").id = 0;
            Require(!GetSkinPaletteIncompatibility(reference, changed).empty() &&
                    !GetSkinPaletteIncompatibility(changed, changed).empty(),
                "Mismatched or duplicated wardrobe palette slots were accepted.");
            changed = reference;
            changed.at("head").offset[3][1] = 0.01f;
            Require(!GetSkinPaletteIncompatibility(reference, changed).empty(),
                "Same-name wardrobe skeleton with a different bind pose was accepted.");
            changed.at("head").offset[3][1] = std::numeric_limits<float>::quiet_NaN();
            Require(!GetSkinPaletteIncompatibility(reference, changed).empty(),
                "Non-finite wardrobe inverse bind pose was accepted.");
        }
        VerifyClothPreparedConstraints();
        auto rigA = MakeRig(170.0f), rigB = MakeRig(-170.0f);
        std::map<std::string, BoneInfo> boneMap;
        Animation clipA(rigA, 0, boneMap), clipB(rigB, 0, boneMap);
        Animator from(&clipA), to(&clipB);
        const auto palette = Animator::BlendFinalMatrices(from, to, 0.5f);
        const glm::vec3 child(palette.at(1)[3]);
        Require(glm::length(child - glm::vec3(-1,0,0)) < 1e-4f,
            "Local quaternion blend did not preserve the rotating child offset at 180 degrees.");
        Require(std::abs(glm::determinant(palette[0]) - 1.0f) < 1e-4f,
            "Quaternion blend shrank the rotation basis.");
        Require(glm::length(glm::vec3(from.GetFinalBoneMatrices()[1][3])) > .999f,
            "Missing channels lost the bind-pose child translation.");

        std::vector<KeyPosition> positions = {{{1,0,0}, 0.0f}, {{3,0,0}, 1.0f}};
        Bone sampled("sample", 0, positions, {}, {});
        LocalPose bind;
        bind.Scale = {2,3,4};
        Require(sampled.Sample(2.0f, bind).Translation.x == 3.0f,
            "Sampling beyond the last key must clamp rather than assert.");
        Require(sampled.Sample(-1.0f, bind).Translation.x == 1.0f,
            "Sampling before the first key must clamp.");
        Require(sampled.Sample(.5f, bind).Scale == bind.Scale,
            "Missing scale channel lost bind scale.");

        auto eventRig = MakeRig(0);
        AnimEventRes event;
        event.id = "start"; event.name = "vfx"; event.timeStamp = 0;
        eventRig.clips[0].events.push_back(event);
        event.id = "footstep"; event.name = "audio"; event.timeStamp = .25f;
        eventRig.clips[0].events.push_back(event);
        Animation eventClip(eventRig, 0, boneMap);
        {
            Mesh oldOutfit, newOutfit;
            oldOutfit.GetBoneInfoMap() = newOutfit.GetBoneInfoMap() = boneMap;
            oldOutfit.GetBoneCount() = static_cast<int>(boneMap.size());
            oldOutfit.mAnimation = eventClip;
            oldOutfit.mAnimator = Animator(&oldOutfit.mAnimation);
            oldOutfit.mAnimator.UpdateAnimation(.3f);
            oldOutfit.bAnimated = oldOutfit.bPlayAnim = oldOutfit.bUseExternalBoneMatrices = true;
            oldOutfit.mExternalBoneMatrices = oldOutfit.mAnimator.GetFinalBoneMatrices();
            AnimatorComponent playing;
            playing.bLoaded = true;
            playing.bPaused = true;
            playing.mCurrentNodeId = 12;
            playing.mTargetNodeId = 19;
            playing.mCurrentStateName = "Walk";
            playing.mParameters["speed"] = 1.25f;
            playing.mInTransition = true;
            playing.mTransitionTime = .2f;
            playing.mTransitionDuration = .8f;
            playing.mTransitionFromAnimator = oldOutfit.mAnimator;
            playing.mTransitionToAnimator = Animator(&eventClip);
            playing.mTransitionToAnimator.UpdateAnimationAbsolute(.6f);
            playing.mClothSimulation[0] = {};
            playing.mDeformationWasActive = true;
            playing.RebindCompatibleMesh(oldOutfit, newOutfit);
            Require(playing.bLoaded && playing.bPaused && playing.mCurrentNodeId == 12 &&
                    playing.mTargetNodeId == 19 && playing.mCurrentStateName == "Walk" &&
                    playing.mParameters.at("speed") == 1.25f && playing.mInTransition &&
                    playing.mTransitionTime == .2f && playing.mTransitionDuration == .8f,
                "Outfit rebinding reset controller, pause or transition state.");
            Require(newOutfit.mAnimator.GetCurrentTime() == .3f &&
                    newOutfit.mExternalBoneMatrices == oldOutfit.mExternalBoneMatrices &&
                    newOutfit.mAnimator.ConsumeEvents().size() == 2,
                "Outfit rebinding lost current pose, time or queued events.");
            Require(playing.mClothSimulation.empty() && !playing.mDeformationWasActive,
                "Outfit rebinding retained geometry-specific simulation state.");
            oldOutfit.mAnimation = Animation(); // Simulate retired clip storage.
            newOutfit.mAnimator.UpdateAnimation(.1f);
            playing.mTransitionFromAnimator.UpdateAnimation(.1f);
            playing.mTransitionToAnimator.UpdateAnimation(.1f);
            Require(std::abs(newOutfit.mAnimator.GetCurrentTime() - .4f) < 1e-6f &&
                    std::abs(playing.mTransitionFromAnimator.GetCurrentTime() - .4f) < 1e-6f &&
                    std::abs(playing.mTransitionToAnimator.GetCurrentTime() - .7f) < 1e-6f,
                "Outfit rebinding kept stale mesh clip pointers or changed component-owned clips.");
            Mesh incompatible;
            incompatible.GetBoneInfoMap() = boneMap;
            incompatible.GetBoneInfoMap().begin()->second.id = 123;
            bool rejected = false;
            try { playing.RebindCompatibleMesh(newOutfit, incompatible); }
            catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected && playing.mCurrentNodeId == 12 && playing.mTransitionTime == .2f &&
                    !incompatible.mAnimator.HasCurrentAnimation(),
                "Incompatible outfit modified playback before validation.");
        }
        Animator events(&eventClip);
        events.UpdateAnimation(0.0f);
        Require(events.ConsumeEvents().empty(), "Paused animation emitted an event.");
        events.UpdateAnimation(2.5f);
        const auto crossed = events.ConsumeEvents();
        Require(crossed.size() == 6 && crossed[4].Cycle == 2 && crossed[5].Event.id == "footstep",
            "Multi-loop stepping lost or duplicated event boundaries.");
        Require(events.ConsumeEvents().empty(), "Event drain replayed already consumed events.");
        events.UpdateAnimationAbsolute(.1f);
        Require(events.ConsumeEvents().empty(), "Pose-only seek emitted events.");
        events.UpdateAnimation(.2f);
        Require(events.ConsumeEvents().size() == 1, "Stepping after seek did not use its new interval.");
        events.Reset();
        events.SetLoop(false);
        events.UpdateAnimation(2.0f);
        Require(events.HasFinished() && events.GetProgress() == 1.0f && events.ConsumeEvents().size() == 2,
            "Non-loop playback wrapped or skipped its terminal pose/events.");
        events.UpdateAnimation(1.0f);
        Require(events.ConsumeEvents().empty(), "Completed clip emitted events a second time.");
        events.ClearAnimation();
        Require(events.ConsumeEvents().empty(), "Cancel retained pending events.");

        AnimControllerRes controller;
        AnimControllerNodeRes node;
        node.id = 1; node.blendParameter = "speed";
        AnimBlendSampleRes low, high;
        low.nodeId = 1; low.threshold = 0;
        high.nodeId = 2; high.threshold = 2;
        node.blendSamples = {low, high};
        controller.nodes.push_back(node);
        std::vector<Animation> clips{clipA, clipB};
        std::map<int,int> mapping{{1,0},{2,1}};
        std::map<std::string,Json> parameters{{"speed",1.0f}};
        {
            AnimControllerRes entryController;
            entryController.entryNodeId = 1;
            AnimControllerNodeRes alive, dead;
            alive.id = 1; alive.name = "Alive"; alive.type = "State";
            dead.id = 2; dead.name = "Dead"; dead.type = "State";
            entryController.nodes = {alive, dead};
            AnimControllerTransitionRes deathTransition;
            deathTransition.fromNodeId = 1; deathTransition.toNodeId = 2;
            deathTransition.duration = .1f;
            AnimControllerConditionRes deathCondition;
            deathCondition.param = "dead"; deathCondition.op = "=="; deathCondition.value = true;
            deathTransition.conditions = {deathCondition};
            entryController.transitions = {deathTransition};
            std::map<std::string, Json> entryParameters{{"dead", false}};
            AnimationStateMachine machine;
            machine.SetController(entryController);
            machine.SetNodeToClipIndex(mapping);
            machine.SetClips(&clips);
            machine.SetParameterStorage(&entryParameters);
            std::vector<glm::mat4> matrices;
            float progress = 0;
            machine.Update(.2f, 1, true, matrices, progress);
            Require(machine.GetCurrentNodeId() == 1 && machine.GetCurrentStateName() == "Alive",
                "A playable entry state bypassed its false death transition condition");
            entryParameters["dead"] = true;
            machine.Update(.2f, 1, true, matrices, progress);
            Require(machine.GetCurrentNodeId() == 2,
                "A playable entry state did not evaluate its true transition after initialization");

            machine.Reset();
            entryParameters["dead"] = false;
            entryController.entryNodeId = 0;
            AnimControllerTransitionRes entryTransition;
            entryTransition.fromNodeId = 0; entryTransition.toNodeId = 1;
            entryController.transitions.insert(entryController.transitions.begin(), entryTransition);
            machine.Update(.2f, 1, true, matrices, progress);
            Require(machine.GetCurrentNodeId() == 1,
                "A non-playable Entry node no longer redirected to its initial state");

            machine.Reset();
            entryController.entryNodeId = 999;
            machine.Update(.2f, 1, true, matrices, progress);
            Require(machine.GetCurrentNodeId() == 1,
                "An unavailable entry state lost the first-playable-state fallback");
        }
        Animator motion(&clips[0]);
        ApplyMotionBlend(motion, 1, controller, mapping, clips, parameters, true);
        Require(glm::length(glm::vec3(motion.GetFinalBoneMatrices()[1][3]) - child) < 1e-4f,
            "Continuous parameter blend did not interpolate its neighboring clips.");

        auto& samples = controller.nodes[0].blendSamples;
        samples[0].threshold = -std::numeric_limits<float>::max();
        samples[1].threshold = std::numeric_limits<float>::max();
        parameters["speed"] = 0.0f;
        ApplyMotionBlend(motion, 1, controller, mapping, clips, parameters, true);
        Require(glm::length(glm::vec3(motion.GetFinalBoneMatrices()[1][3]) - child) < 1e-4f,
            "Finite extreme thresholds overflowed the blend weight.");
        const auto rejectsBlend = [&]() {
            bool rejected = false;
            try { ApplyMotionBlend(motion, 1, controller, mapping, clips, parameters, true); }
            catch (const std::exception&) { rejected = true; }
            Require(rejected, "Invalid blend input was accepted.");
        };
        parameters["speed"] = std::numeric_limits<float>::quiet_NaN(); rejectsBlend();
        parameters["speed"] = std::numeric_limits<float>::infinity(); rejectsBlend();
        parameters["speed"] = 0.0f;
        samples[1].threshold = samples[0].threshold; rejectsBlend();
        samples[1].threshold = 1.0f;
        samples[1].nodeId = 99; rejectsBlend();
        samples[1].nodeId = 2;
        mapping[2] = 100; rejectsBlend(); mapping[2] = 1;

        controller.nodes[0].blendParameterY = "direction";
        samples[0].threshold = -1; samples[0].thresholdY = 0;
        samples[1].threshold = 1; samples[1].thresholdY = 0;
        parameters["speed"] = 0; parameters["direction"] = 0;
        ApplyMotionBlend(motion, 1, controller, mapping, clips, parameters, true);
        Require(glm::length(glm::vec3(motion.GetFinalBoneMatrices()[1][3]) - child) < 1e-4f,
            "2D weighted quaternion blend lost the shortest arc or hierarchy.");
        parameters["speed"] = -1;
        ApplyMotionBlend(motion, 1, controller, mapping, clips, parameters, true);
        Require(glm::length(glm::vec3(motion.GetFinalBoneMatrices()[1][3]) - glm::vec3(from.GetFinalBoneMatrices()[1][3])) < 1e-4f,
            "2D exact sample coordinate did not reproduce the sample pose.");
        samples[1].threshold = -1; rejectsBlend(); samples[1].threshold = 1;
        parameters["direction"] = std::numeric_limits<float>::quiet_NaN(); rejectsBlend();
        parameters["direction"] = 0;

        auto layerRig = MakeRig(90);
        layerRig.clips[0].channels[1].positionKeys.push_back({3, 0, 0, 0});
        Animation layerClip(layerRig, 0, boneMap);
        Animator masked(&clipA), layer(&layerClip);
        const auto beforeRoot = masked.GetFinalBoneMatrices()[0];
        masked.ApplyLayer(layer, 1, {"child"}, false);
        Require(glm::length(masked.GetFinalBoneMatrices()[0][0] - beforeRoot[0]) < 1e-4f,
            "Child mask changed an unmasked parent rotation.");
        Require(std::abs(glm::length(glm::vec3(masked.GetFinalBoneMatrices()[1][3])) - 3) < 1e-4f,
            "Masked layer did not apply the child's local translation.");
        Animator additive(&clipA);
        additive.ApplyLayer(layer, .5f, {"child"}, true);
        Require(std::abs(glm::length(glm::vec3(additive.GetFinalBoneMatrices()[1][3])) - 2) < 1e-4f,
            "Additive layer did not use the clip bind pose as its reference.");
        bool badMask = false;
        try { additive.ApplyLayer(layer, 1, {"missing-bone"}, false); } catch (const std::invalid_argument&) { badMask = true; }
        Require(badMask, "Unknown animation mask bone was ignored.");

        controller.nodes[0].blendParameter.clear(); controller.nodes[0].blendParameterY.clear();
        AnimLayerRes layerRes; layerRes.nodeId = 2; layerRes.maskRoots = {"child"}; layerRes.weight = .5f;
        layerRes.additive = true; controller.nodes[0].layers = {layerRes};
        clips[1] = layerClip;
        Animator controllerLayer(&clips[0]);
        ApplyMotionBlend(controllerLayer, 1, controller, mapping, clips, parameters, true);
        Require(std::abs(glm::length(glm::vec3(controllerLayer.GetFinalBoneMatrices()[1][3])) - 2) < 1e-4f,
            "Controller layer settings did not reach the sampled pose.");
        ApplyMotionBlend(controllerLayer, 1, controller, mapping, clips, parameters, true);
        Require(std::abs(glm::length(glm::vec3(controllerLayer.GetFinalBoneMatrices()[1][3])) - 2) < 1e-4f,
            "Paused evaluation accumulated the additive layer twice.");
        Animator layeredEvents(&eventClip);
        layeredEvents.UpdateAnimation(.3f);
        ApplyMotionBlend(layeredEvents, 1, controller, mapping, clips, parameters, true);
        Require(layeredEvents.ConsumeEvents().size() == 2,
            "Layer evaluation consumed, duplicated or discarded the base clip's event track.");
        Json layeredJson = controller;
        const auto loadedLayers = layeredJson.get<AnimControllerRes>();
        Require(loadedLayers.nodes[0].layers[0].maskRoots[0] == "child" && loadedLayers.nodes[0].layers[0].additive,
            "Reflection roundtrip lost layer masks or additive mode.");
        // An action clock can move independently while the unmasked base pose stays intact.
        controller.nodes[0].layers[0].progressParameter = "actionProgress";
        parameters["actionProgress"] = .75f;
        auto timedRig = MakeRig(0);
        timedRig.clips[0].channels[1].positionKeys = {{1, 0, 0, 0}, {5, 0, 0, timedRig.clips[0].duration}};
        clips[1] = Animation(timedRig, 0, boneMap);
        controller.nodes[0].layers[0].additive = false;
        controller.nodes[0].layers[0].weight = 1;
        Animator timedLayer(&clips[0]);
        ApplyMotionBlend(timedLayer, 1, controller, mapping, clips, parameters, true);
        Require(std::abs(glm::length(glm::vec3(timedLayer.GetFinalBoneMatrices()[1][3])) - 4) < 1e-4f,
            "Independent action progress incorrectly followed locomotion phase.");
        Require(glm::length(timedLayer.GetFinalBoneMatrices()[0][0] - Animator(&clips[0]).GetFinalBoneMatrices()[0][0]) < 1e-4f,
            "Independent upper layer changed its unmasked parent.");
        parameters["actionProgress"] = -1;
        bool badProgress = false;
        try { ApplyMotionBlend(timedLayer, 1, controller, mapping, clips, parameters, true); }
        catch (const std::invalid_argument&) { badProgress = true; }
        Require(badProgress, "Invalid independent action progress was accepted.");
        // The outgoing clip is resampled during a state-machine crossfade. Its
        // upper-body action must remain on the shared action clock, not vanish
        // until the destination takes over. Root rotations differ, so check
        // the masked child's local offset length independently of locomotion.
        {
            AnimControllerRes transitionController;
            AnimControllerNodeRes source, target, action;
            source.id = 1; target.id = 2; action.id = 3;
            source.name = "Source"; target.name = "Target"; action.name = "Action";
            auto sharedLayer = controller.nodes[0].layers[0];
            sharedLayer.nodeId = 3; sharedLayer.weightParameter = "actionWeight";
            source.layers = {sharedLayer}; target.layers = {sharedLayer};
            transitionController.nodes = {source, target, action};
            AnimControllerTransitionRes transition;
            transition.fromNodeId = 1; transition.toNodeId = 2; transition.duration = .2f;
            AnimControllerConditionRes condition;
            condition.param = "go"; condition.value = true;
            transition.conditions = {condition}; transitionController.transitions = {transition};
            std::vector<Animation> transitionClips{clipA, clipB, Animation(timedRig, 0, boneMap)};
            std::map<int, int> transitionMapping{{1, 0}, {2, 1}, {3, 2}};
            std::map<std::string, Json> transitionParameters{
                {"go", false}, {"actionProgress", .25f}, {"actionWeight", 1.0f}};
            AnimationStateMachine machine;
            machine.SetController(transitionController);
            machine.SetNodeToClipIndex(transitionMapping);
            machine.SetClips(&transitionClips);
            machine.SetParameterStorage(&transitionParameters);
            machine.SetCurrentNodeId(1);
            std::vector<glm::mat4> matrices;
            float progress = 0;
            machine.Update(0, 1, true, matrices, progress);
            Require(std::abs(glm::length(glm::vec3(matrices.at(1)[3])) - 2) < 1e-4f,
                "Action layer was not active before the state transition.");
            transitionParameters["go"] = true;
            for (int step = 1; step <= 12; ++step)
            {
                const float actionProgress = .25f + .04f * step;
                const float actionWeight = step < 6 ? 1.0f : .5f;
                transitionParameters["actionProgress"] = actionProgress;
                transitionParameters["actionWeight"] = actionWeight;
                machine.Update(.02f, 1, true, matrices, progress);
                const float expectedLength = 1 + 4 * actionProgress * actionWeight;
                Require(std::abs(glm::length(glm::vec3(matrices.at(1)[3])) - expectedLength) < 1e-4f,
                    "Outgoing action layer lost its weight or shared time during crossfade.");
                const auto beforePause = matrices;
                machine.Update(0, 1, true, matrices, progress);
                Require(matrices == beforePause, "Paused crossfade accumulated an action layer.");
            }
            Require(machine.GetCurrentNodeId() == 2, "Layered transition did not complete.");
        }
        glm::vec3 camera30(0), camera120(0), velocity30(0), velocity120(0);
        const glm::vec3 stairTarget(0, .3f, 0), cameraRate(14, 9, 14);
        for (int i = 0; i < 30; ++i) AdvanceCameraSpring(camera30, velocity30, stairTarget, cameraRate, 1.0f / 30);
        for (int i = 0; i < 120; ++i) AdvanceCameraSpring(camera120, velocity120, stairTarget, cameraRate, 1.0f / 120);
        Require(glm::distance(camera30, camera120) < 1e-5f && camera30.y <= .3f && camera30.y > .299f,
            "Camera stair spring overshot or changed response with frame rate.");
        const glm::vec3 pausedCamera = camera30;
        AdvanceCameraSpring(camera30, velocity30, glm::vec3(0, 1, 0), cameraRate, 0);
        Require(camera30 == pausedCamera, "Paused camera spring advanced on zero delta.");
        // Camera recoil at +/-90 degree heading used to produce real 3-degree
        // direction jumps after a float quaternion -> Euler -> quaternion roundtrip.
        for (const float heading : {-90.001f, -90.0f, -89.999f, 0.0f, 89.999f, 90.0f, 90.001f, 180.0f})
        for (const float roll : {-35.0f, 0.0f, 24.0f})
        for (int sample = 0; sample <= 160; ++sample)
        {
            const glm::quat source(glm::radians(glm::vec3(-80.0f + sample, heading, roll)));
            const glm::mat4 expected = glm::mat4_cast(source);
            const glm::mat4 actual = glm::mat4_cast(glm::quat(Math::StableEulerAngles(source)));
            glm::vec3 translation, rotation, scale;
            Require(Math::DecomposeTransform(expected, translation, rotation, scale),
                "Valid camera rotation failed transform decomposition.");
            const glm::mat4 decomposed = glm::mat4_cast(glm::quat(rotation));
            for (int column = 0; column < 3; ++column)
                Require(glm::length(expected[column] - actual[column]) < 2e-5f &&
                    glm::length(expected[column] - decomposed[column]) < 2e-5f,
                    "Quaternion Euler conversion changed the camera's actual direction near a yaw pole.");
            const auto reflected = glm::translate(glm::mat4(1), glm::vec3(2, 3, 4)) *
                expected * glm::scale(glm::mat4(1), glm::vec3(-2, 1.3f, .7f));
            Require(Math::DecomposeTransform(reflected, translation, rotation, scale),
                "Reflected scaled transform failed stable decomposition.");
            const auto reconstructed = glm::translate(glm::mat4(1), translation) *
                glm::mat4_cast(glm::quat(rotation)) * glm::scale(glm::mat4(1), scale);
            for (int column = 0; column < 4; ++column)
                Require(glm::length(reflected[column] - reconstructed[column]) < 3e-5f,
                    "Stable Euler extraction changed a reflected/scaled imported transform.");
        }
        // Render samples of a fixed-tick controller must remain equally spaced,
        // including rates that do not divide the simulation frequency.
        for (const double renderHz : {30.0, 60.0, 75.0, 120.0, 144.0})
        {
            FixedSimulationStepper stepper;
            FixedPositionHistory history;
            glm::vec3 authoritative(0), previousRendered(0);
            history.Reset(authoritative);
            for (int frame = 0; frame < 300; ++frame)
            {
                const auto ticks = stepper.Advance(1.0 / renderHz);
                for (uint32_t tick = 0; tick < ticks; ++tick)
                {
                    const auto previous = authoritative;
                    authoritative.x += 3.0f * stepper.GetTickSeconds();
                    history.Advance(previous, authoritative);
                }
                const auto rendered = authoritative + history.Offset(authoritative, stepper.GetInterpolationAlpha());
                if (frame > 4)
                    Require(std::abs(rendered.x - previousRendered.x - 3.0 / renderHz) < 2e-5,
                        "Fixed-tick character presentation stair-stepped between render frames.");
                previousRendered = rendered;
            }
            history.Reset(glm::vec3(50));
            Require(history.Offset(glm::vec3(50), .4) == glm::vec3(0),
                "Teleport interpolation swept through the old location.");
            history.Advance(glm::vec3(49), glm::vec3(50));
            Require(history.Offset(glm::vec3(80), .5) == glm::vec3(0) &&
                history.Offset(glm::vec3(50), std::numeric_limits<double>::quiet_NaN()) == glm::vec3(0),
                "Stale character history or invalid clock changed the rendered transform.");
            stepper.Reset();
            stepper.Advance(1.0 / 120.0);
            const auto pausedAlpha = stepper.GetInterpolationAlpha();
            Require(stepper.Advance(20.0, true) == 0 && stepper.GetInterpolationAlpha() == pausedAlpha,
                "Pause changed the character's fractional presentation phase.");
        }
        AnimatorComponent component;
        component.mParameterTypes["speed"] = AnimControllerParamType::Float;
        component.SetFloat("speed", 2.0f);
        for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            bool rejected = false;
            try { component.SetFloat("speed", invalid); } catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected && component.GetFloat("speed") == 2.0f, "Invalid float parameter mutated its previous value.");
        }

        Json encoded;
        Serializer::Serialize(encoded, eventRig.clips[0]);
        AnimClipRes decoded;
        Serializer::Deserialize(encoded, decoded);
        Require(decoded.events.size() == 2 && decoded.events[1].id == "footstep",
            "Reflection serialization lost the event track.");
        encoded.erase("events");
        decoded = {};
        Serializer::Deserialize(encoded, decoded);
        Require(decoded.events.empty(), "Legacy clip default events must be empty.");
        Serializer::Serialize(encoded, node);
        AnimControllerNodeRes nodeCopy;
        Serializer::Deserialize(encoded, nodeCopy);
        Require(nodeCopy.blendSamples.size() == 2 && nodeCopy.blendParameter == "speed",
            "Reflection serialization lost the blend sample contract.");

        // Secondary motion must remain opt-in, stable across frame rates, and
        // leave the parent animation and shared clips unchanged.
        auto springRig = MakeRig(0);
        std::map<std::string, BoneInfo> springMap;
        Animation springClip(springRig, 0, springMap);
        Animator springAnimator(&springClip);
        const auto restPalette = springAnimator.GetFinalBoneMatrices();
        SecondaryMotionBone spring;
        spring.Bone = "child";
        spring.Tail = glm::vec3(0, 0.4f, 0);
        spring.GravityScale = 0;
        spring.MaxAngleDegrees = 25;
        const auto simulate = [&](int fps)
        {
            SecondaryMotion solver;
            auto result = restPalette;
            for (int frame = 0; frame < 3 * fps; ++frame)
            {
                result = restPalette;
                solver.Apply(1.0f / fps, glm::mat4(1), springClip, {spring}, {}, glm::vec3(2, 0, 0), result);
            }
            Require(glm::distance(glm::vec3(result[0][0]), glm::vec3(restPalette[0][0])) < 1e-6f,
                "Secondary motion altered an ancestor outside the declared subtree.");
            const glm::vec3 tip = glm::vec3(result[1] * glm::vec4(spring.Tail, 1));
            Require(tip.x > 1.005f && std::abs(glm::distance(tip, glm::vec3(1,0,0)) - 0.4f) < 1e-5f,
                "Spring did not respond to wind or violated its length constraint.");
            Require(glm::dot(glm::normalize(tip - glm::vec3(1,0,0)), glm::vec3(0,1,0)) >=
                std::cos(glm::radians(25.01f)), "Spring exceeded its angular constraint.");
            return tip;
        };
        Require(glm::distance(simulate(30), simulate(120)) < 0.003f,
            "Secondary motion is unstable between 30 and 120 Hz.");
        const auto transientAtRate = [&](int fps)
        {
            SecondaryMotion transientSolver;
            SecondaryMotionBone transientSpring = spring;
            transientSpring.Stiffness = 40.0f;
            transientSpring.Damping = 5.0f;
            transientSpring.MaxAngleDegrees = 45.0f;
            auto result = restPalette;
            for (int frame = 0; frame < fps / 2; ++frame)
            {
                result = restPalette;
                transientSolver.Apply(1.0f / fps, glm::mat4(1), springClip,
                    {transientSpring}, {}, glm::vec3(6, 0, 0), result);
            }
            return glm::vec3(result[1] * glm::vec4(transientSpring.Tail, 1));
        };
        Require(glm::distance(transientAtRate(10), transientAtRate(120)) < 0.012f,
            "Low-frame-rate secondary motion skipped elapsed simulation time.");
        const auto movingPivotAtRate = [&](int fps)
        {
            SecondaryMotion movingSolver;
            SecondaryMotionBone movingSpring = spring;
            movingSpring.Stiffness = 45.0f;
            movingSpring.Damping = 6.0f;
            movingSpring.MaxAngleDegrees = 55.0f;
            auto result = restPalette;
            movingSolver.Apply(1.0f / fps, glm::mat4(1), springClip,
                {movingSpring}, {}, glm::vec3(0), result);
            for (int frame = 1; frame <= fps / 2; ++frame)
            {
                result = restPalette;
                const float offset = 0.4f * frame / (fps / 2);
                movingSolver.Apply(1.0f / fps,
                    glm::translate(glm::mat4(1), glm::vec3(offset, 0, 0)),
                    springClip, {movingSpring}, {}, glm::vec3(0), result);
            }
            return glm::vec3(result[1] * glm::vec4(movingSpring.Tail, 1));
        };
        Require(glm::distance(movingPivotAtRate(10), movingPivotAtRate(120)) < 0.025f,
            "Moving hair root changed its response across frame rates.");
        const auto movingAirflow = [&](float drag)
        {
            SecondaryMotion airSolver;
            SecondaryMotionBone airSpring = spring;
            airSpring.Stiffness = 45.0f;
            airSpring.Damping = 12.0f;
            airSpring.MaxAngleDegrees = 50.0f;
            airSpring.MotionDrag = drag;
            auto result = restPalette;
            airSolver.Apply(1.0f / 60.0f, glm::mat4(1), springClip,
                {airSpring}, {}, glm::vec3(0), result);
            for (int frame = 1; frame <= 120; ++frame)
            {
                result = restPalette;
                airSolver.Apply(1.0f / 60.0f,
                    glm::translate(glm::mat4(1), glm::vec3(frame / 60.0f, 0, 0)),
                    springClip, {airSpring}, {}, glm::vec3(0), result);
            }
            return glm::vec3(result[1] * glm::vec4(airSpring.Tail, 1));
        };
        Require(movingAirflow(2.0f).x < movingAirflow(0.0f).x - .005f,
            "Movement airflow did not bend hair against sustained root motion.");
        SecondaryMotion solver;
        auto untouched = restPalette;
        solver.Apply(1.0f/60, glm::mat4(1), springClip, {}, {}, glm::vec3(2), untouched);
        Require(untouched == restPalette, "Empty secondary configuration changed the base pose.");
        auto moving = restPalette;
        solver.Apply(1.0f/60, glm::mat4(1), springClip, {spring}, {}, glm::vec3(2,0,0), moving);
        moving = restPalette;
        solver.Apply(1.0f/60, glm::translate(glm::mat4(1), glm::vec3(100,0,0)),
            springClip, {spring}, {}, glm::vec3(0), moving);
        Require(glm::distance(glm::vec3(moving[1][1]), glm::vec3(restPalette[1][1])) < 1e-5f,
            "Teleport retained stale world-space spring velocity.");
        // A cutscene can rotate a character in place without moving its hair
        // pivot. The old world-space spring direction must not survive the cut.
        solver.Reset();
        moving = restPalette;
        for (int frame = 0; frame < 30; ++frame)
        {
            moving = restPalette;
            solver.Apply(1.0f/60, glm::mat4(1), springClip, {spring}, {}, glm::vec3(10,0,0), moving);
        }
        const glm::mat4 turnInPlace = glm::translate(glm::mat4(1), glm::vec3(1,0,0)) *
            glm::rotate(glm::mat4(1), glm::radians(180.0f), glm::vec3(0,0,1)) *
            glm::translate(glm::mat4(1), glm::vec3(-1,0,0));
        moving = restPalette;
        solver.Apply(1.0f/60, turnInPlace, springClip, {spring}, {}, glm::vec3(0), moving);
        Require(glm::distance(glm::vec3(moving[1][1]), glm::vec3(restPalette[1][1])) < 1e-5f,
            "An instantaneous turn retained the previous hair direction.");
        SecondaryMotionSphere sphere;
        sphere.Bone = "root";
        sphere.Offset = glm::vec3(1.05f, .4f, 0);
        sphere.Radius = .025f;
        moving = restPalette;
        solver.Reset();
        solver.Apply(1.0f/60, glm::mat4(1), springClip, {spring}, {sphere}, glm::vec3(20,0,0), moving);
        Require(glm::distance(glm::vec3(moving[1] * glm::vec4(spring.Tail,1)), sphere.Offset) >= .039f,
            "Feasible body sphere failed to repel the spring tip.");
        // Collision projection must not create a large free-flight velocity when
        // the collider disappears on the following frame.
        SecondaryMotion contactSolver;
        SecondaryMotionBone contactSpring = spring;
        contactSpring.Stiffness = 0;
        contactSpring.Damping = 0;
        contactSpring.GravityScale = 0;
        SecondaryMotionSphere contactSphere;
        contactSphere.Bone = "root";
        contactSphere.Offset = glm::vec3(1.03f, .4f, 0);
        contactSphere.Radius = .05f;
        moving = restPalette;
        contactSolver.Apply(1.0f / 60, glm::mat4(1), springClip, {contactSpring},
            {contactSphere}, glm::vec3(0), moving);
        const glm::vec3 constrainedTip = glm::vec3(moving[1] * glm::vec4(spring.Tail, 1));
        Require(glm::distance(constrainedTip, glm::vec3(1, .4f, 0)) > .01f,
            "Collision impulse test did not move the spring tip.");
        moving = restPalette;
        contactSolver.Apply(1.0f / 60, glm::mat4(1), springClip, {contactSpring},
            {}, glm::vec3(0), moving);
        const glm::vec3 releasedTip = glm::vec3(moving[1] * glm::vec4(spring.Tail, 1));
        Require(glm::distance(releasedTip, constrainedTip) < .002f,
            "Collider correction injected free-flight hair velocity.");
        // Skeletal meshes imported in centimetres are commonly placed at 0.01
        // scale. Collider and tip radii must scale together so that the same
        // authored rig produces the same local-space constrained pose.
        SecondaryMotionBone scaledSpring = contactSpring;
        scaledSpring.Radius = .02f;
        SecondaryMotion unscaledSolver, scaledSolver;
        auto unscaledPalette = restPalette;
        auto scaledPalette = restPalette;
        unscaledSolver.Apply(1.0f / 60, glm::mat4(1), springClip,
            {scaledSpring}, {contactSphere}, glm::vec3(0), unscaledPalette);
        scaledSolver.Apply(1.0f / 60, glm::scale(glm::mat4(1), glm::vec3(.01f)),
            springClip, {scaledSpring}, {contactSphere}, glm::vec3(0), scaledPalette);
        const glm::vec3 unscaledLocalTip = glm::vec3(unscaledPalette[1] * glm::vec4(spring.Tail, 1));
        const glm::vec3 scaledLocalTip = glm::vec3(scaledPalette[1] * glm::vec4(spring.Tail, 1));
        Require(glm::distance(unscaledLocalTip, scaledLocalTip) < 1e-4f,
            "Secondary-motion collision changed with uniform character scale.");
        Serializer::Serialize(encoded, spring);
        SecondaryMotionBone restoredSpring;
        Serializer::Deserialize(encoded, restoredSpring);
        Require(restoredSpring.Bone == spring.Bone && restoredSpring.Tail == spring.Tail,
            "Reflection roundtrip lost secondary bone settings.");
        spring.MotionDrag = .5f;
        Serializer::Serialize(encoded, spring);
        Serializer::Deserialize(encoded, restoredSpring);
        Require(std::abs(restoredSpring.MotionDrag - .5f) < 1e-6f,
            "Reflection roundtrip lost movement airflow settings.");

        // A suspended two-triangle cloth patch: exact pins, gravity/wind,
        // frequency convergence, collision, pause and teleport reset.
        const std::vector<glm::vec3> clothRest = {{-.2f, 1, 0}, {.2f, 1, 0}, {-.2f, .6f, 0}, {.2f, .6f, 0}};
        const std::vector<uint32_t> clothTriangles = {0, 2, 1, 1, 2, 3};
        const std::vector<float> clothRadii = {0, 0, .3f, .3f};
        ClothPanel clothSettings;
        const auto clothAtRate = [&](int rate)
        {
            ClothSimulation cloth;
            for (int i = 0; i <= rate * 3; ++i)
                cloth.Step(1.0f / rate, clothRest, clothTriangles, clothRadii, {}, glm::vec3(0, 0, 2), clothSettings, glm::vec3(0));
            Require(cloth.Positions()[0] == clothRest[0] && cloth.Positions()[1] == clothRest[1],
                "Cloth pins moved away from their animated skin positions.");
            Require(cloth.Positions()[3].z > .015f && glm::distance(cloth.Positions()[3], clothRest[3]) <= .301f,
                "Cloth failed wind response or motion-radius constraint.");
            const auto paused = cloth.Positions();
            cloth.Step(0, clothRest, clothTriangles, clothRadii, {}, glm::vec3(0), clothSettings, glm::vec3(0));
            Require(cloth.Positions() == paused, "Paused cloth continued simulation.");
            return cloth.Positions()[3];
        };
        Require(glm::distance(clothAtRate(30), clothAtRate(120)) < .025f,
            "Cloth XPBD diverges between 30 and 120 Hz.");
        ClothSimulation clothCollision;
        ClothWorldCapsule body;
        body.Start = body.End = clothRest[3] + glm::vec3(0, 0, -.025f);
        body.Radius = .055f;
        for (int i = 0; i < 120; ++i)
            clothCollision.Step(1.0f / 60, clothRest, clothTriangles, clothRadii, {body}, glm::vec3(0), clothSettings, glm::vec3(0));
        Require(glm::distance(clothCollision.Positions()[3], body.Start) >= body.Radius + clothSettings.Thickness - .0001f,
            "Cloth penetrated its body collider.");
        auto teleported = clothRest;
        for (auto& p : teleported) p.x += 50;
        clothCollision.Step(1.0f / 60, teleported, clothTriangles, clothRadii, {}, glm::vec3(0), clothSettings, glm::vec3(50, 0, 0));
        Require(clothCollision.WasReset() && clothCollision.Positions() == teleported,
            "Cloth retained old positions after a teleport.");
        clothCollision.Step(.5f, teleported, clothTriangles, clothRadii, {}, glm::vec3(0), clothSettings, glm::vec3(50, 0, 0));
        Require(clothCollision.WasReset(), "Long pause resume did not reset cloth.");

        // Subnormal delta must freeze before XPBD divides by h*h. Exercise
        // self-collision as well, so an intermediate NaN cannot reach its grid.
        ClothPanel selfSettings = clothSettings;
        selfSettings.SelfCollision = true;
        ClothSimulation tinyDeltaCloth;
        tinyDeltaCloth.Step(1.0f / 60, clothRest, clothTriangles, clothRadii,
            {}, glm::vec3(0), selfSettings, glm::vec3(0));
        const auto beforeTinyDelta = tinyDeltaCloth.Positions();
        tinyDeltaCloth.Step(std::numeric_limits<float>::denorm_min(), clothRest,
            clothTriangles, clothRadii, {}, glm::vec3(30), selfSettings, glm::vec3(0));
        Require(!tinyDeltaCloth.WasReset() && tinyDeltaCloth.Positions() == beforeTinyDelta,
            "Subnormal cloth timestep lost its finite paused state.");

        ClothSimulation distantCloth;
        auto distantRest = clothRest;
        const glm::vec3 distantOrigin(100000000.0f, 0, 0);
        for (auto& point : distantRest) point += distantOrigin;
        for (int i = 0; i < 3; ++i)
            distantCloth.Step(1.0f / 60, distantRest, clothTriangles, clothRadii,
                {}, glm::vec3(0), selfSettings, distantOrigin);
        Require(distantCloth.Positions().size() == distantRest.size(),
            "Actor-relative cloth spatial bins discarded distant finite input.");
        for (const auto& point : distantCloth.Positions())
            Require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z),
                "Distant cloth self collision generated a non-finite position.");

        // Oversized local coordinates must be rejected before conversion, even
        // when they are finite; pinned input makes the expected result exact.
        ClothSimulation oversizedCloth;
        const std::vector<glm::vec3> oversizedRest = {glm::vec3(1e20f), glm::vec3(-1e20f)};
        const std::vector<float> pinnedRadii(2, 0);
        for (int i = 0; i < 2; ++i)
            oversizedCloth.Step(1.0f / 60, oversizedRest, {}, pinnedRadii,
                {}, glm::vec3(0), selfSettings, glm::vec3(0));
        Require(oversizedCloth.Positions() == oversizedRest,
            "Unrepresentable hash cell corrupted pinned cloth positions.");

        // UV/normal seams duplicate render vertices but must not tear cloth.
        std::vector<SkinnedVertex> seamVertices;
        for (const uint32_t sourceIndex : {0u, 2u, 1u, 1u, 2u, 3u})
        {
            SkinnedVertex vertex{};
            vertex.Pos = clothRest[sourceIndex];
            vertex.Normal = glm::vec3(0, 0, 1);
            vertex.Tangent = glm::vec3(1, 0, 0);
            vertex.Bitangent = glm::vec3(0, 1, 0);
            vertex.TexCoord = glm::vec2(vertex.Pos);
            vertex.Color = glm::vec4(clothRadii[sourceIndex] > 0 ? 1.0f : 0.0f, 1, 1, 1);
            for (int influence = 0; influence < MAX_BONE_INFLUENCE; ++influence) vertex.mBoneIDs[influence] = -1;
            seamVertices.push_back(vertex);
        }
        // Deliberate UV split across the duplicated geometric seam.
        seamVertices[3].TexCoord.x += 1;
        seamVertices[4].TexCoord.x += 1;
        Mesh seamMesh;
        const auto sharedSection = CreateRef<SubMesh>(seamVertices, std::vector<uint32_t>{0, 1, 2, 3, 4, 5}, 0);
        seamMesh.mSubMeshes.push_back(sharedSection);
        AnimatorComponent seamAnimator;
        ClothPanel seamPanel;
        seamPanel.MaterialIndex = 0;
        seamPanel.MaxDistance = .3f;
        seamAnimator.ClothPanels.push_back(seamPanel);
        seamAnimator.SecondaryWind = glm::vec3(0, 0, 2);
        for (int frame = 0; frame < 90; ++frame)
            UpdateCharacterDeformation(seamAnimator, seamMesh, glm::mat4(1), 1.0f / 60, true);
        const auto& welded = seamMesh.mSubMeshes[0]->mSkinnedVertices;
        Require(seamAnimator.mClothSimulation.at(0).ParticleCount == 4 &&
            seamAnimator.mClothSimulation.at(0).Positions().size() == 4 &&
            glm::distance(welded[1].Pos, welded[4].Pos) < 1e-6f,
            "UV seam split the cloth simulation or opened a crack.");
        Require(welded[1].Pos.z > .01f && sharedSection->mSkinnedVertices[1].Pos == seamVertices[1].Pos,
            "Per-instance cloth failed to move or mutated its shared asset.");
        const auto previousDeformed = welded[1].Pos;
        UpdateCharacterDeformation(seamAnimator, seamMesh, glm::mat4(1), 1.0f / 60, true);
        Require(seamMesh.mSubMeshes[0]->mSkinnedVertices[1].PreviousPosition == previousDeformed,
            "Cloth publication lost the previous local position required by TAA.");

        // Animator/Sequence pause keeps a non-rest solver alive while the level
        // continues ticking. Compare the resumed trajectory to an uninterrupted
        // reference trajectory, so clearing velocities or restarting at rest is detected.
        seamAnimator.bLoaded = true;
        seamAnimator.bPlay = true;
        AnimatorComponent pauseReferenceAnimator;
        pauseReferenceAnimator.ClothPanels.push_back(seamPanel);
        pauseReferenceAnimator.SecondaryWind = seamAnimator.SecondaryWind;
        Mesh pauseReferenceMesh;
        pauseReferenceMesh.mSubMeshes.push_back(sharedSection);
        for (int frame = 0; frame < 91; ++frame)
            UpdateCharacterDeformation(pauseReferenceAnimator, pauseReferenceMesh, glm::mat4(1), 1.0f / 60, true);
        const auto pausePositions = seamAnimator.mClothSimulation.at(0).Positions();
        Require(glm::distance(pausePositions[3], clothRest[3]) > .01f,
            "Pause fixture must start with non-rest cloth");
        seamAnimator.bPaused = true;
        for (int frame = 0; frame < 20; ++frame)
        {
            const auto tick = ResolveCharacterDeformationTick(seamAnimator, 1.0f / 60, true);
            Require(tick.Active && tick.DeltaSeconds == 0,
                "Animator pause was mistaken for deformation opt-out");
            UpdateCharacterDeformation(seamAnimator, seamMesh, glm::mat4(1), tick.DeltaSeconds, tick.Active);
            Require(seamAnimator.mClothSimulation.at(0).Positions() == pausePositions,
                "Animator pause cleared or advanced non-rest cloth");
        }
        seamAnimator.bPaused = false;
        UpdateCharacterDeformation(seamAnimator, seamMesh, glm::mat4(1), 1.0f / 60, true);
        UpdateCharacterDeformation(pauseReferenceAnimator, pauseReferenceMesh, glm::mat4(1), 1.0f / 60, true);
        Require(seamAnimator.mClothSimulation.at(0).Positions() == pauseReferenceAnimator.mClothSimulation.at(0).Positions(),
            "Resume lost cloth velocities or snapped to rest");
        seamAnimator.bPaused = true;
        seamAnimator.mStepSeconds = 1.0f / 30;
        const auto singleStep = ResolveCharacterDeformationTick(seamAnimator, 1.0f / 60, true);
        Require(singleStep.Active && singleStep.DeltaSeconds == seamAnimator.mStepSeconds,
            "Paused explicit single-step was ignored");
        const auto optedOut = ResolveCharacterDeformationTick(seamAnimator, 1.0f / 60, false);
        UpdateCharacterDeformation(seamAnimator, seamMesh, glm::mat4(1), optedOut.DeltaSeconds, optedOut.Active);
        Require(!optedOut.Active && seamAnimator.mClothSimulation.empty(),
            "Distance opt-out no longer releases its solver");
        seamAnimator.bPlay = false;
        Require(!ResolveCharacterDeformationTick(seamAnimator, 1.0f / 60, true).Active,
            "Stop was mistaken for pause");

        // Exercise the real TickEntity early-return path through the system:
        // Stop->Step must consume the request without starting a clip/cloth.
        auto stoppedLevel = CreateRef<Level>();
        auto stoppedEntity = stoppedLevel->CreateEntity("Stopped animation step");
        auto& stoppedMesh = stoppedEntity.AddComponent<MeshComponent>();
        stoppedMesh.mMesh = CreateRef<Mesh>();
        stoppedMesh.mMesh->mSubMeshes.push_back(sharedSection);
        auto& stoppedAnimator = stoppedEntity.AddComponent<AnimatorComponent>();
        stoppedAnimator.bLoaded = true;
        stoppedAnimator.ClothPanels.push_back(seamPanel);
        stoppedAnimator.Stop();
        stoppedAnimator.Step(1.0f / 30);
        AnimationSystem stoppedSystem(stoppedLevel.get());
        stoppedSystem.OnUpdateRuntime(Timestep(1.0f / 60));
        Require(stoppedAnimator.mStepSeconds == 0 && !stoppedAnimator.bPlay &&
            stoppedAnimator.mClothSimulation.empty(),
            "Stopped single-step was retained or activated cloth");
        stoppedSystem.OnUpdateRuntime(Timestep(1.0f / 60));
        Require(stoppedAnimator.mStepSeconds == 0 && stoppedAnimator.mClothSimulation.empty(),
            "Stopped single-step repeated on a later tick");

        // World->local normal transport must remain correct with nonuniform
        // scale and shear, not just with identity/rigid skin transforms.
        Mesh scaledClothMesh;
        scaledClothMesh.mSubMeshes.push_back(sharedSection);
        AnimatorComponent scaledClothAnimator;
        scaledClothAnimator.ClothPanels.push_back(seamPanel);
        scaledClothAnimator.SecondaryWind = glm::vec3(0, 0, 2);
        glm::mat4 scaledWorld(1);
        scaledWorld[0][0] = 1.4f; scaledWorld[1][1] = .8f;
        scaledWorld[2][2] = 1.2f; scaledWorld[1][0] = .2f;
        for (int frame = 0; frame < 8; ++frame)
            Require(UpdateCharacterDeformation(scaledClothAnimator, scaledClothMesh, scaledWorld, 1.0f / 60, true) == seamVertices.size(),
                "Cloth update count did not describe the published vertices");
        const auto& scaledVertices = scaledClothMesh.mSubMeshes[0]->mSkinnedVertices;
        const auto worldPoint = [&](size_t index) { return glm::vec3(scaledWorld * glm::vec4(scaledVertices[index].Pos, 1)); };
        const auto faceNormal = glm::normalize(glm::cross(worldPoint(1) - worldPoint(0), worldPoint(2) - worldPoint(0)));
        const auto expectedLocalNormal = glm::normalize(glm::transpose(glm::mat3(scaledWorld)) * faceNormal);
        Require(glm::dot(expectedLocalNormal, scaledVertices[0].Normal) > .9999f,
            "Cloth normal pullback changed under nonuniform scale/shear");

        Mesh cachedMorphMesh;
        cachedMorphMesh.mSubMeshes.push_back(CreateRef<SubMesh>(seamVertices, std::vector<uint32_t>{0,1,2,3,4,5}));
        MorphTarget lift;
        lift.Name = "Lift";
        lift.Positions.assign(seamVertices.size(), glm::vec3(0, 0, .1f));
        cachedMorphMesh.mSubMeshes[0]->SetMorphTargets({lift});
        AnimatorComponent cachedMorphAnimator;
        cachedMorphAnimator.FacialWeights["Lift"] = .5f;
        Require(UpdateCharacterDeformation(cachedMorphAnimator, cachedMorphMesh, glm::mat4(1), 1.0f / 60, true) == seamVertices.size() &&
            UpdateCharacterDeformation(cachedMorphAnimator, cachedMorphMesh, glm::mat4(1), 1.0f / 60, true) == 0,
            "Stable reset morph was reported as a full vertex update");
        cachedMorphAnimator.FacialWeights["Lift"] = .75f;
        Require(UpdateCharacterDeformation(cachedMorphAnimator, cachedMorphMesh, glm::mat4(1), 1.0f / 60, true) == seamVertices.size() &&
            UpdateCharacterDeformation(cachedMorphAnimator, cachedMorphMesh, glm::mat4(1), 0, true) == seamVertices.size() &&
            UpdateCharacterDeformation(cachedMorphAnimator, cachedMorphMesh, glm::mat4(1), 0, true) == 0,
            "Changed/settling/cached morph update counters or paused history are incorrect");
        UpdateCharacterDeformation(cachedMorphAnimator, cachedMorphMesh, glm::mat4(1), 0, false);
        Require(UpdateCharacterDeformation(cachedMorphAnimator, cachedMorphMesh, glm::mat4(1), 1.0f / 60, true) == seamVertices.size(),
            "Distance opt-out/reactivation retained an invalid pure-morph cache");

        // Editor authoring must work without a clip, including native defaults.
        std::vector<StaticVertex> previewVertices(3);
        for (size_t i = 0; i < previewVertices.size(); ++i)
        {
            previewVertices[i].Pos = glm::vec3(static_cast<float>(i == 1), static_cast<float>(i == 2), 0);
            previewVertices[i].Normal = glm::vec3(0, 0, 1);
            previewVertices[i].Tangent = glm::vec3(1, 0, 0);
            previewVertices[i].Bitangent = glm::vec3(0, 1, 0);
        }
        Mesh previewMesh;
        previewMesh.mSubMeshes.push_back(CreateRef<SubMesh>(previewVertices, std::vector<uint32_t>{0, 1, 2}));
        MorphTarget previewLift = lift;
        previewLift.DefaultWeight = .25f;
        previewLift.Positions.assign(3, glm::vec3(0, 0, .1f));
        previewMesh.mSubMeshes[0]->SetMorphTargets({previewLift});
        AnimatorComponent previewAnimator;
        previewAnimator.bPlay = false;
        previewAnimator.bLoaded = false;
        previewAnimator.mFacialPlayer.Seek(.05f);
        previewAnimator.FacialBlinkLeft = "Lift";
        previewAnimator.FacialBlinkInterval = 1.0f;
        Require(PreviewCharacterDeformation(previewAnimator, previewMesh) == 3 &&
            std::abs(previewMesh.mSubMeshes[0]->mStaticVertices[0].Pos.z - .025f) < 1e-6f,
            "Unloaded static mesh preview lost its native default morph weight");
        Require(PreviewCharacterDeformation(previewAnimator, previewMesh) == 0,
            "Unchanged static preview uploaded its stream again");
        previewAnimator.FacialWeights["Lift"] = .75f;
        Require(PreviewCharacterDeformation(previewAnimator, previewMesh) == 3 &&
            std::abs(previewMesh.mSubMeshes[0]->mStaticVertices[0].Pos.z - .075f) < 1e-6f &&
            previewMesh.mSubMeshes[0]->mStaticVertices[0].PreviousPosition == previewMesh.mSubMeshes[0]->mStaticVertices[0].Pos &&
            PreviewCharacterDeformation(previewAnimator, previewMesh) == 0,
            "Stopped preview edit failed to publish settled geometry exactly once");
        previewAnimator.FacialWeights.clear();
        PreviewCharacterDeformation(previewAnimator, previewMesh);
        Require(std::abs(previewMesh.mSubMeshes[0]->mStaticVertices[0].Pos.z - .025f) < 1e-6f &&
            previewAnimator.mFacialPlayer.GetTime() == .05f && previewAnimator.mClothSimulation.empty() &&
            !previewAnimator.mDeformationWasActive,
            "Static preview advanced performance/blink/simulation or lost fallback defaults");

        // A genuine non-rest cloth stream (without morph targets) must restore
        // source normals/tangents as well as positions, then remain cached.
        Require(glm::distance(pauseReferenceMesh.mSubMeshes[0]->mSkinnedVertices[1].Pos, seamVertices[1].Pos) > .01f,
            "Preview transition fixture no longer contains non-rest cloth");
        const float previewFaceTime = pauseReferenceAnimator.mFacialPlayer.GetTime();
        Require(PreviewCharacterDeformation(pauseReferenceAnimator, pauseReferenceMesh) == seamVertices.size() &&
            pauseReferenceAnimator.mClothSimulation.empty(), "Static preview did not release active cloth");
        for (size_t i = 0; i < seamVertices.size(); ++i)
        {
            const auto& vertex = pauseReferenceMesh.mSubMeshes[0]->mSkinnedVertices[i];
            Require(vertex.Pos == seamVertices[i].Pos && vertex.PreviousPosition == vertex.Pos &&
                vertex.Normal == seamVertices[i].Normal && vertex.Tangent == seamVertices[i].Tangent &&
                vertex.Bitangent == seamVertices[i].Bitangent,
                "Cloth-to-preview transition left simulated geometry/frame/history behind");
        }
        Require(PreviewCharacterDeformation(pauseReferenceAnimator, pauseReferenceMesh) == 0 &&
            pauseReferenceAnimator.mFacialPlayer.GetTime() == previewFaceTime,
            "Stable cloth-to-preview transition repeated upload or advanced facial playback");

        // Empty-target cache above must invalidate when a new target is attached.
        MorphTarget skinnedPreviewLift = lift;
        skinnedPreviewLift.DefaultWeight = .5f;
        pauseReferenceMesh.mSubMeshes[0]->SetMorphTargets({skinnedPreviewLift});
        Require(PreviewCharacterDeformation(pauseReferenceAnimator, pauseReferenceMesh) == seamVertices.size() &&
            std::abs(pauseReferenceMesh.mSubMeshes[0]->mSkinnedVertices[1].Pos.z - seamVertices[1].Pos.z - .05f) < 1e-6f &&
            PreviewCharacterDeformation(pauseReferenceAnimator, pauseReferenceMesh) == 0,
            "Restored cloth stream retained a stale empty morph cache");

        Mesh morphClothPreview;
        morphClothPreview.mSubMeshes.push_back(CreateRef<SubMesh>(seamVertices, std::vector<uint32_t>{0,1,2,3,4,5}, 0));
        morphClothPreview.mSubMeshes[0]->SetMorphTargets({lift});
        AnimatorComponent morphClothAnimator;
        morphClothAnimator.ClothPanels.push_back(seamPanel);
        morphClothAnimator.SecondaryWind = glm::vec3(0, 0, 2);
        morphClothAnimator.FacialWeights["Lift"] = .3f;
        PreviewCharacterDeformation(morphClothAnimator, morphClothPreview);
        for (int frame = 0; frame < 30; ++frame)
            UpdateCharacterDeformation(morphClothAnimator, morphClothPreview, glm::mat4(1), 1.0f / 60, true);
        const glm::vec3 authoredMorphPosition = seamVertices[1].Pos + glm::vec3(0, 0, .03f);
        Require(glm::distance(morphClothPreview.mSubMeshes[0]->mSkinnedVertices[1].Pos, authoredMorphPosition) > .01f &&
            PreviewCharacterDeformation(morphClothAnimator, morphClothPreview) == seamVertices.size() &&
            glm::distance(morphClothPreview.mSubMeshes[0]->mSkinnedVertices[1].Pos, authoredMorphPosition) < 1e-6f &&
            PreviewCharacterDeformation(morphClothAnimator, morphClothPreview) == 0,
            "Cloth left an old pure-morph cache valid across the preview/play/preview transition");

        auto ikRig = MakeRig(0);
        AnimNodeRes endNode = ikRig.rootNode.children[0];
        endNode.name = "foot";
        ikRig.rootNode.children[0].children.push_back(endNode);
        auto endBone = ikRig.boneInfos.back();
        endBone.name = "foot"; endBone.id = 2;
        ikRig.boneInfos.push_back(endBone);
        std::map<std::string, BoneInfo> ikBones;
        Animation ikAnimation(ikRig, 0, ikBones);
        Animator ikAnimator(&ikAnimation);
        auto ikPalette = ikAnimator.GetFinalBoneMatrices();
        const glm::vec3 ikTarget(1, 1, 0);
        Require(CharacterPose::SolveTwoBone(ikAnimation, "root", "child", "foot", ikTarget, ikPalette),
            "Two-bone IK failed on a valid chain.");
        glm::mat4 actualFoot, actualKnee, actualRoot;
        CharacterPose::BoneMatrix(ikAnimation, ikPalette, "foot", actualFoot);
        CharacterPose::BoneMatrix(ikAnimation, ikPalette, "child", actualKnee);
        CharacterPose::BoneMatrix(ikAnimation, ikPalette, "root", actualRoot);
        Require(glm::distance(glm::vec3(actualFoot[3]), ikTarget) < .001f &&
            std::abs(glm::distance(glm::vec3(actualFoot[3]), glm::vec3(actualKnee[3])) - 1) < .001f &&
            std::abs(glm::distance(glm::vec3(actualKnee[3]), glm::vec3(actualRoot[3])) - 1) < .001f,
            "Two-bone IK missed its endpoint or stretched a segment.");
        // An off-axis contact point rotates with the end bone; adding a world
        // target offset or solving the bone origin is not equivalent.
        const glm::vec3 contactOffset(.15f, .1f, .03f);
        const glm::vec3 contactTarget(1, 1, .25f);
        auto contactPalette = ikAnimator.GetFinalBoneMatrices();
        Require(CharacterPose::SolveTwoBone(ikAnimation, "root", "child", "foot", contactTarget,
            contactPalette, contactOffset), "IK rejected a valid local contact point");
        CharacterPose::BoneMatrix(ikAnimation, contactPalette, "foot", actualFoot);
        CharacterPose::BoneMatrix(ikAnimation, contactPalette, "child", actualKnee);
        CharacterPose::BoneMatrix(ikAnimation, contactPalette, "root", actualRoot);
        Require(glm::distance(glm::vec3(actualFoot * glm::vec4(contactOffset, 1)), contactTarget) < .001f &&
            glm::distance(glm::vec3(actualFoot[3]), contactTarget) > .1f &&
            std::abs(glm::distance(glm::vec3(actualFoot[3]), glm::vec3(actualKnee[3])) - 1) < .001f &&
            std::abs(glm::distance(glm::vec3(actualKnee[3]), glm::vec3(actualRoot[3])) - 1) < .001f,
            "Local contact IK missed the contact point or stretched an anatomical segment");
        auto invalidContactPalette = contactPalette;
        Require(!CharacterPose::SolveTwoBone(ikAnimation, "root", "child", "foot", contactTarget,
            invalidContactPalette, glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0, 0)) &&
            invalidContactPalette == contactPalette, "Non-finite contact offset mutated the pose");
        LimbConstraint contactConstraint;
        contactConstraint.Upper = "root";
        contactConstraint.Lower = "child";
        contactConstraint.End = "foot";
        contactConstraint.EffectorLocalOffset = contactOffset;
        contactConstraint.Weight = 1;
        auto legacyPose = ikAnimator.GetFinalBoneMatrices();
        auto zeroRotationPose = legacyPose;
        CharacterPose::SolveTwoBone(ikAnimation, "root", "child", "foot", contactTarget, legacyPose, contactOffset);
        Require(CharacterPose::SolveLimbPose(ikAnimation, contactConstraint, glm::mat4(1), contactTarget, zeroRotationPose) &&
            legacyPose == zeroRotationPose, "Zero rotation weight changed legacy contact IK");
        contactConstraint.WorldRotationDegrees = glm::vec3(15, 25, -35);
        contactConstraint.RotationWeight = 1;
        const glm::mat3 desiredContactRotation = glm::mat3_cast(glm::quat(glm::radians(contactConstraint.WorldRotationDegrees)));
        for (const auto& actorWorld : {glm::mat4(1),
            glm::translate(glm::mat4(1), glm::vec3(3, .5f, -2)) *
            glm::toMat4(glm::quat(glm::vec3(0, .4f, 0))) * glm::scale(glm::mat4(1), glm::vec3(1.2f, .8f, 1.1f))})
        {
            auto orientedPose = ikAnimator.GetFinalBoneMatrices();
            glm::mat4 originalEnd;
            CharacterPose::BoneMatrix(ikAnimation, orientedPose, "foot", originalEnd);
            const glm::mat4 worldEnd = actorWorld * originalEnd;
            const glm::vec3 scale(glm::length(glm::vec3(worldEnd[0])), glm::length(glm::vec3(worldEnd[1])), glm::length(glm::vec3(worldEnd[2])));
            const glm::vec3 desiredOrigin = glm::vec3(actorWorld * glm::vec4(1, 1, .1f, 1));
            const glm::vec3 desiredContact = desiredOrigin + desiredContactRotation * (scale * contactOffset);
            Require(CharacterPose::SolveLimbPose(ikAnimation, contactConstraint, actorWorld, desiredContact, orientedPose),
                "Oriented contact rejected a scaled/rotated actor");
            CharacterPose::BoneMatrix(ikAnimation, orientedPose, "foot", actualFoot);
            const glm::mat4 resultWorld = actorWorld * actualFoot;
            Require(glm::distance(glm::vec3(resultWorld * glm::vec4(contactOffset, 1)), desiredContact) < .001f,
                "Oriented limb failed to place the real contact point");
            for (int axis = 0; axis < 3; ++axis)
                Require(glm::distance(glm::normalize(glm::vec3(resultWorld[axis])), desiredContactRotation[axis]) < .001f,
                    "Oriented limb missed the requested world orientation");
            const auto validPose = orientedPose;
            auto invalidConstraint = contactConstraint;
            invalidConstraint.WorldRotationDegrees.x = std::numeric_limits<float>::quiet_NaN();
            Require(!CharacterPose::SolveLimbPose(ikAnimation, invalidConstraint, actorWorld, desiredContact, orientedPose) &&
                orientedPose == validPose, "Invalid world orientation contaminated the palette");
        }
        Serializer::Serialize(encoded, contactConstraint);
        LimbConstraint restoredConstraint;
        Serializer::Deserialize(encoded, restoredConstraint);
        Require(restoredConstraint.EffectorLocalOffset == contactOffset &&
            restoredConstraint.WorldRotationDegrees == contactConstraint.WorldRotationDegrees && restoredConstraint.RotationWeight == 1,
            "Reflected limb contact offset did not round-trip");
        encoded.erase("EffectorLocalOffset");
        encoded.erase("WorldRotationDegrees");
        encoded.erase("RotationWeight");
        LimbConstraint legacyConstraint;
        Serializer::Deserialize(encoded, legacyConstraint);
        Require(legacyConstraint.EffectorLocalOffset == glm::vec3(0) &&
            legacyConstraint.WorldRotationDegrees == glm::vec3(0) && legacyConstraint.RotationWeight == 0,
            "Legacy limb data no longer defaults to bone-origin contact without rotation");
        // Disabled and malformed pose constraints must not modify their input.
        CharacterPose boundaryPose;
        FootContact disabledFoot;
        disabledFoot.Upper = "root";
        disabledFoot.Lower = "child";
        disabledFoot.Foot = "foot";
        disabledFoot.Weight = 0;
        int floorQueries = 0;
        const auto floorBelow = [&](const glm::vec3&, float& height)
        { ++floorQueries; height = -.2f; return true; };
        const auto baseFootPalette = ikAnimator.GetFinalBoneMatrices();
        auto zeroWeightPalette = baseFootPalette;
        boundaryPose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
            {disabledFoot}, "root", floorBelow, true, zeroWeightPalette);
        Require(zeroWeightPalette == baseFootPalette && floorQueries == 0,
            "Zero-weight foot IK changed the pelvis/pose or queried physics.");

        disabledFoot.Weight = 1;
        disabledFoot.MaxCorrection = -.1f;
        auto badFootPalette = baseFootPalette;
        boundaryPose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
            {disabledFoot}, "root", floorBelow, true, badFootPalette);
        Require(badFootPalette == baseFootPalette && floorQueries == 0,
            "Negative correction envelope was not rejected before std::clamp.");

        // Invalid bone configuration must release an existing world-space foot
        // lock, even if the recovered foot is within the normal lock radius.
        CharacterPose recoveringFootPose;
        FootContact recoveringFoot;
        recoveringFoot.Upper = "root";
        recoveringFoot.Lower = "child";
        recoveringFoot.Foot = "foot";
        recoveringFoot.Weight = 1;
        recoveringFoot.LockFoot = true;
        recoveringFoot.SoleHeight = 0;
        recoveringFoot.PlantHeight = .1f;
        const auto floorAtFoot = [](const glm::vec3& point, float& height)
        { height = point.y; return true; };
        auto plantedPalette = baseFootPalette;
        recoveringFootPose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
            {recoveringFoot}, "", floorAtFoot, true, plantedPalette);
        recoveringFoot.Foot = "missing-foot";
        auto invalidFootPalette = baseFootPalette;
        recoveringFootPose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
            {recoveringFoot}, "", floorAtFoot, true, invalidFootPalette);
        recoveringFoot.Foot = "foot";
        const glm::mat4 shiftedWorld = glm::translate(glm::mat4(1), glm::vec3(.1f, 0, 0));
        auto recoveredPalette = baseFootPalette;
        recoveringFootPose.ApplyFeet(1.0f / 60, shiftedWorld, ikAnimation,
            {recoveringFoot}, "", floorAtFoot, true, recoveredPalette);
        glm::mat4 recoveredFoot, originalFoot;
        CharacterPose::BoneMatrix(ikAnimation, recoveredPalette, "foot", recoveredFoot);
        CharacterPose::BoneMatrix(ikAnimation, baseFootPalette, "foot", originalFoot);
        Require(std::abs(recoveredFoot[3].x - originalFoot[3].x) < .001f,
            "Recovered foot reused a stale world-space lock after bone failure");

        // A grounded actor can still have a swinging foot. A floor query must
        // not drag that foot down or use it to lower the pelvis.
        CharacterPose swingPose;
        const auto flatFloor = [](const glm::vec3&, float& height) { height = 0; return true; };
        auto initialContact = baseFootPalette;
        swingPose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
            {recoveringFoot}, "root", flatFloor, true, initialContact);
        auto swingingPalette = baseFootPalette;
        CharacterPose::TransformSubtree(ikAnimation, "root",
            glm::translate(glm::mat4(1), glm::vec3(0, .2f, 0)), swingingPalette);
        const auto animatedSwing = swingingPalette;
        swingPose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
            {recoveringFoot}, "root", flatFloor, true, swingingPalette);
        Require(swingingPalette == animatedSwing, "Swing foot was ground-snapped or lowered the pelvis");
        auto landedPalette = baseFootPalette;
        swingPose.ApplyFeet(1.0f / 60, shiftedWorld, ikAnimation,
            {recoveringFoot}, "", flatFloor, true, landedPalette);
        CharacterPose::BoneMatrix(ikAnimation, landedPalette, "foot", actualFoot);
        Require(std::abs(actualFoot[3].x-originalFoot[3].x) < .001f,
            "Landing reused the world-space lock from before the swing");
        auto penetratingPalette = baseFootPalette;
        CharacterPose::TransformSubtree(ikAnimation, "root",
            glm::translate(glm::mat4(1), glm::vec3(0, -.05f, 0)), penetratingPalette);
        CharacterPose penetratingPose;
        penetratingPose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
            {recoveringFoot}, "", flatFloor, true, penetratingPalette);
        CharacterPose::BoneMatrix(ikAnimation, penetratingPalette, "foot", actualFoot);
        Require(std::abs(actualFoot[3].y) < .002f,
            "Swing rejection also disabled below-floor contact correction");
        for (int axis = 0; axis < 3; ++axis)
            Require(glm::distance(glm::vec3(actualFoot[axis]), glm::vec3(originalFoot[axis])) < 1e-5f,
                "Flat-floor ankle correction changed the authored sole orientation");

        CharacterPose weightedFootPose;
        FootContact halfWeightFoot = recoveringFoot;
        halfWeightFoot.Weight = .5f;
        auto halfWeightPalette = baseFootPalette;
        weightedFootPose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
            {halfWeightFoot}, "", flatFloor, true, halfWeightPalette);
        glm::vec3 firstWeightedPosition(0);
        for (int frame = 0; frame < 8; ++frame)
        {
            halfWeightPalette = baseFootPalette;
            weightedFootPose.ApplyFeet(1.0f / 60, shiftedWorld, ikAnimation,
                {halfWeightFoot}, "", flatFloor, true, halfWeightPalette);
            CharacterPose::BoneMatrix(ikAnimation, halfWeightPalette, "foot", actualFoot);
            if (frame == 0) firstWeightedPosition = glm::vec3(actualFoot[3]);
            Require(glm::distance(glm::vec3(actualFoot[3]), firstWeightedPosition) < 1e-5f,
                "Partially weighted contact accumulated drift in its locked anchor");
        }
        Require(std::abs(firstWeightedPosition.x - 1.95f) < .001f,
            "Partially weighted foot did not blend toward its fixed world anchor");
        glm::vec3 edgePositions[2];
        for (int side = 0; side < 2; ++side)
        {
            CharacterPose edgePose;
            auto edgePalette = baseFootPalette;
            CharacterPose::TransformSubtree(ikAnimation, "root",
                glm::translate(glm::mat4(1), glm::vec3(0, .1f+(side == 0 ? -.0001f : .0001f), 0)), edgePalette);
            edgePose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
                {recoveringFoot}, "", flatFloor, true, edgePalette);
            CharacterPose::BoneMatrix(ikAnimation, edgePalette, "foot", actualFoot);
            edgePositions[side] = glm::vec3(actualFoot[3]);
        }
        Require(glm::distance(edgePositions[0], edgePositions[1]) < .001f,
            "Foot contact has a discontinuity at the authored swing threshold");

        CharacterPose boundedContactPose;
        FootContact boundedFoot = recoveringFoot;
        boundedFoot.MaxCorrection = .01f;
        for (int frame = 0; frame < 3; ++frame)
        {
            auto boundedPalette = baseFootPalette;
            CharacterPose::TransformSubtree(ikAnimation, "root",
                glm::translate(glm::mat4(1), glm::vec3(0, -.1f, 0)), boundedPalette);
            boundedContactPose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
                {boundedFoot}, "", flatFloor, true, boundedPalette);
            CharacterPose::BoneMatrix(ikAnimation, boundedPalette, "foot", actualFoot);
            Require(std::abs(actualFoot[3].y + .09f) < .001f,
                "Locked anchor bypassed MaxCorrection on a subsequent frame");
        }

        CharacterPose pelvisReleasePose;
        float previousPelvis = 0;
        for (int frame = 0; frame < 31; ++frame)
        {
            const float animatedHeight = frame < 30 ? .05f : .1001f;
            auto releasePalette = baseFootPalette;
            CharacterPose::TransformSubtree(ikAnimation, "root",
                glm::translate(glm::mat4(1), glm::vec3(0, animatedHeight, 0)), releasePalette);
            pelvisReleasePose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
                {recoveringFoot}, "root", flatFloor, true, releasePalette);
            glm::mat4 releasedRoot;
            CharacterPose::BoneMatrix(ikAnimation, releasePalette, "root", releasedRoot);
            const float pelvisOffset = releasedRoot[3].y - animatedHeight;
            if (frame == 29) previousPelvis = pelvisOffset;
            if (frame == 30)
                Require(previousPelvis < -.02f &&
                    std::abs(pelvisOffset - previousPelvis * std::exp(-12.0f / 60)) < 1e-5f,
                    "Last foot lift-off snapped historical pelvis displacement to zero");
        }

        CharacterPose reachLockPose;
        for (int frame = 0; frame <= 20; ++frame)
        {
            const float x = frame * .03f;
            auto reachPalette = baseFootPalette;
            const auto actorWorld = glm::translate(glm::mat4(1), glm::vec3(x, 0, 0));
            reachLockPose.ApplyFeet(1.0f / 60, actorWorld, ikAnimation,
                {recoveringFoot}, "", flatFloor, true, reachPalette);
            CharacterPose::BoneMatrix(ikAnimation, reachPalette, "foot", actualFoot);
            Require(std::abs(actualFoot[3].x + x - 2) < .001f,
                "Reachable stance anchor was reset because the animated foot moved away");
        }
        float previousWorldFoot = 2;
        for (int frame = 0; frame <= 120; ++frame)
        {
            const float x = -.001f * frame;
            auto reachPalette = baseFootPalette;
            const auto actorWorld = glm::translate(glm::mat4(1), glm::vec3(x, 0, 0));
            reachLockPose.ApplyFeet(1.0f / 60, actorWorld, ikAnimation,
                {recoveringFoot}, "", flatFloor, true, reachPalette);
            CharacterPose::BoneMatrix(ikAnimation, reachPalette, "foot", actualFoot);
            const float worldFoot = actualFoot[3].x + x;
            Require(std::abs(worldFoot - previousWorldFoot) < .01f,
                "Actual limb overreach release introduced a discontinuous foot target");
            previousWorldFoot = worldFoot;
        }
        for (float x : {.4f, .5f})
        {
            auto suppressedPalette = baseFootPalette;
            reachLockPose.ApplyFeet(1.0f / 60, glm::translate(glm::mat4(1), glm::vec3(x, 0, 0)), ikAnimation,
                {recoveringFoot}, "", flatFloor, true, suppressedPalette);
            CharacterPose::BoneMatrix(ikAnimation, suppressedPalette, "foot", actualFoot);
            Require(std::abs(actualFoot[3].x + x - 2) < .001f,
                "Foot lost its original stance anchor after returning within reach");
        }

        auto rearmedPalette = baseFootPalette;
        reachLockPose.ApplyFeet(1.0f / 60, glm::translate(glm::mat4(1), glm::vec3(.5f, .2f, 0)), ikAnimation,
            {recoveringFoot}, "", flatFloor, true, rearmedPalette);
        rearmedPalette = baseFootPalette;
        reachLockPose.ApplyFeet(1.0f / 60, glm::translate(glm::mat4(1), glm::vec3(.5f, 0, 0)), ikAnimation,
            {recoveringFoot}, "", flatFloor, true, rearmedPalette);
        rearmedPalette = baseFootPalette;
        reachLockPose.ApplyFeet(1.0f / 60, glm::translate(glm::mat4(1), glm::vec3(.55f, 0, 0)), ikAnimation,
            {recoveringFoot}, "", flatFloor, true, rearmedPalette);
        CharacterPose::BoneMatrix(ikAnimation, rearmedPalette, "foot", actualFoot);
        Require(std::abs(actualFoot[3].x - 1.95f) < .001f,
            "A real swing did not rearm the released foot lock");

        // A locomotion clip can blend into an idle pose without a swing frame.
        // Preserve stance during movement, then clear its residual world offset.
        CharacterPose settlingPose;
        auto settlingPalette = baseFootPalette;
        settlingPose.ApplyFeet(1.0f / 60, glm::mat4(1), ikAnimation,
            {recoveringFoot}, "", flatFloor, true, settlingPalette);
        for (int frame = 0; frame < 12; ++frame)
        {
            settlingPalette = baseFootPalette;
            settlingPose.ApplyFeet(1.0f / 60, shiftedWorld, ikAnimation,
                {recoveringFoot}, "", flatFloor, true, settlingPalette);
        }
        const auto beforeSettle = settlingPose.GetFootContactDiagnostics();
        Require(beforeSettle[0].Locked &&
            glm::distance(beforeSettle[0].Anchor, beforeSettle[0].Animated) > .09f,
            "Moving stance lock released before its settle delay");
        for (int frame = 0; frame < 60; ++frame)
        {
            settlingPalette = baseFootPalette;
            settlingPose.ApplyFeet(1.0f / 60, shiftedWorld, ikAnimation,
                {recoveringFoot}, "", flatFloor, true, settlingPalette);
        }
        const auto afterSettle = settlingPose.GetFootContactDiagnostics();
        Require(afterSettle[0].Locked &&
            glm::distance(afterSettle[0].Anchor, afterSettle[0].Animated) < .002f,
            "Idle foot retained a stale world-space stance lock");

        // A death/full-body action can release authored ground constraints
        // through the same scene-checked service used by managed gameplay.
        {
            Level level;
            ScriptWorld world(level);
            auto actor = level.CreateEntity("Foot contact action owner");
            auto& component = actor.AddComponent<AnimatorComponent>();
            component.Feet = {recoveringFoot, recoveringFoot};
            component.mCharacterPose = settlingPose;
            const auto owner = world.Bind(actor);
            const auto setWeight = [&](float weight) {
                return Json::parse(ScriptPlatformServices::Execute(owner, "animation.footContactWeight",
                    Json{{"entity", owner}, {"weight", weight}}.dump())).get<bool>();
            };
            Require(!component.mCharacterPose.GetFootContactDiagnostics().empty(),
                "Foot contact action fixture did not retain a planted contact");
            Require(setWeight(0) && component.Feet[0].Weight == 0 && component.Feet[1].Weight == 0 &&
                component.mCharacterPose.GetFootContactDiagnostics().empty(),
                "Releasing foot contacts retained a standing stance anchor");
            Require(setWeight(.5f) && component.Feet[0].Weight == .5f && component.Feet[1].Weight == .5f,
                "Foot contact action did not apply its absolute weight to each authored foot");
            for (const float invalid : {-1.0f, 1.1f})
            {
                bool rejected = false;
                try { (void)setWeight(invalid); }
                catch (const std::invalid_argument&) { rejected = true; }
                Require(rejected && component.Feet[0].Weight == .5f,
                    "Invalid foot weight changed an authored contact");
            }
            auto plain = level.CreateEntity("No animator");
            const auto plainHandle = world.Bind(plain);
            Require(!Json::parse(ScriptPlatformServices::Execute(owner, "animation.footContactWeight",
                Json{{"entity", plainHandle}, {"weight", 0}}.dump())).get<bool>(),
                "Foot contact service accepted an entity without an animator");
        }

        BoneLookAt badLook;
        badLook.Bone = "root";
        badLook.Weight = std::numeric_limits<float>::quiet_NaN();
        auto badLookPalette = baseFootPalette;
        CharacterPose::ApplyLook(glm::mat4(1), ikAnimation, {badLook}, badLookPalette);
        Require(badLookPalette == baseFootPalette,
            "Non-finite look weight contaminated the shared bone palette.");
        badLook.Weight = 1;
        badLook.LocalForward.x = std::numeric_limits<float>::quiet_NaN();
        CharacterPose::ApplyLook(glm::mat4(1), ikAnimation, {badLook}, badLookPalette);
        Require(badLookPalette == baseFootPalette,
            "Non-finite authored look axis contaminated the bone palette.");

        auto corruptPalette = baseFootPalette;
        corruptPalette[0][0][0] = std::numeric_limits<float>::quiet_NaN();
        glm::mat4 rejectedBone(1);
        Require(!CharacterPose::BoneMatrix(ikAnimation, corruptPalette, "root", rejectedBone),
            "Invalid source palette was accepted by constraint bone extraction.");

    }
}
