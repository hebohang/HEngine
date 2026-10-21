#include "Hepch.h"

#include "Runtime/UnitTest/RenderWorldSubmissionTests.h"
#include "Runtime/Camera/EditorCamera.h"
#include "Runtime/Camera/SceneCamera.h"
#include "Runtime/EcsFramework/Component/RenderSettingsComponent.h"
#include "Runtime/EcsFramework/Component/Basic/IDComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/Graphics/Settings/RenderSettings.h"
#include "_generated/serializer/RenderSettingsComponent.gen.h"
#include "_generated/serializer/CameraComponent.gen.h"

#include "Runtime/EcsFramework/Component/Light/DirectionalLightComponent.h"
#include "_generated/serializer/DirectionalLightComponent.gen.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "_generated/serializer/EnvironmentLightComponent.gen.h"
#include "Runtime/EcsFramework/Component/Light/PointLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/SpotLightComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Script/NativeScriptComponent.h"
#include "Runtime/EcsFramework/Component/PostProcess/PostProcessingComponent.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "_generated/serializer/PostProcessingComponent.gen.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Entity/ScriptableEntity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/System/Render/RenderWorldExtractor.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/Renderer/SceneRenderer.h"
#include "Runtime/Graphics/Scene/PostProcessResolver.h"
#include "Runtime/Graphics/Scene/BakedIrradiance.h"
#include "Runtime/EcsFramework/Component/Light/BakedIrradianceVolumeComponent.h"
#include "Runtime/EcsFramework/Component/Light/ReflectionProbeComponent.h"
#include "_generated/serializer/BakedIrradianceVolumeComponent.gen.h"
#include "_generated/serializer/MeshComponent.gen.h"
#include "_generated/serializer/ReflectionProbeComponent.gen.h"
#include "Runtime/Graphics/Scene/RenderWorldSubmission.h"
#include "Runtime/Graphics/Scene/CameraView.h"
#include "Runtime/Graphics/Scene/SubmittedMotionHistory.h"
#include "Runtime/Graphics/Scene/SubmittedDeformationHistory.h"
#include "Runtime/Graphics/Scene/SubmittedSkyMotionHistory.h"
#include "Runtime/Graphics/Renderer/SceneBonePalette.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/Utils/MathUtils/MathUtils.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace HEngine::Tests
{
    void ImportedQuarterTurnTransformContract()
    {
        for (const auto& [rotation, scale] : std::array{
                 std::pair{glm::quat(0.707106054f, 0.0f, -0.707107484f, 0.0f), glm::vec3(1.0f)},
                 std::pair{glm::quat(0.707108796f, 0.0f, -0.707104862f, 0.0f), glm::vec3(-1.0f, 1.0f, 1.0f)},
             })
        {
            const glm::mat4 source = glm::toMat4(glm::normalize(rotation)) * glm::scale(glm::mat4(1.0f), scale);
            TransformComponent transform;
            if (!Math::DecomposeTransform(source, transform.Translation, transform.Rotation, transform.Scale))
                throw std::runtime_error("Quarter-turn glTF transform failed to decompose");
            const glm::mat4 rebuilt = transform.GetTransform();
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    if (std::abs(source[column][row] - rebuilt[column][row]) > 1e-4f)
                        throw std::runtime_error("Quarter-turn glTF transform changed during decomposition");
        }
    }

    void SceneCameraOrthographicDepthContract()
    {
        SceneCamera camera;
        camera.SetViewportSize(1280, 720);
        camera.SetOrthographic(16.0f, 0.1f, 100.0f);
        const auto depth = [&camera](float distance)
        {
            const glm::vec4 clip = camera.GetProjection() * glm::vec4(0, 0, -distance, 1);
            return clip.z / clip.w;
        };
        if (std::abs(depth(0.1f)) > 1e-6f || std::abs(depth(100.0f) - 1.0f) > 1e-6f ||
            depth(10.0f) <= 0.0f || depth(10.0f) >= 1.0f)
            throw std::runtime_error("Orthographic sprites must stay in device depth [0,1]");
        camera.SetOrthographic(10.0f, -1.0f, 1.0f);
        if (std::abs(depth(0.0f) - 0.5f) > 1e-6f)
            throw std::runtime_error("Symmetric 2D camera depth lost its centre plane");
    }

    void EditorCameraImportedOrientationContract()
    {
        const glm::vec3 position(-8.80743f, 1.59221947f, -0.85825783f);
        const std::array<glm::vec3, 5> rotations = {{
            { -2.94603507f, -1.38419538f, -3.14159242f }, // Intel Sponza primary camera
            { 0.2f, 0.8f, 0.0f },
            { -0.4f, -2.5f, 0.7f },
            { 1.5707963f, 0.0f, 0.0f },
            { -1.5707963f, 0.0f, 0.0f },
        }};
        for (const glm::vec3& rotation : rotations)
        {
            const glm::quat orientation(rotation);
            const glm::vec3 forward = glm::normalize(orientation * glm::vec3(0, 0, -1));
            EditorCamera camera(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
            camera.SetCenter(position + forward * 10.0f);
            camera.SetDistance(10.0f);
            camera.SetOrientation(orientation);
            if (glm::length(camera.GetForwardDirection() - forward) > 0.0001f ||
                glm::length(camera.GetPosition() - position) > 0.001f)
                throw std::runtime_error("Imported editor camera changed its viewing direction or position");
            const glm::vec4 centre = camera.GetViewMatrix() * glm::vec4(position + forward * 5.0f, 1.0f);
            if (glm::length(glm::vec3(centre) - glm::vec3(0, 0, -5)) > 0.001f)
                throw std::runtime_error("Imported camera target is not in the centre of its view");
        }
    }

    void RenderWorldExtractorMeshPathContract()
    {
        const glm::mat4 cameraPose = glm::translate(glm::mat4(1.0f), glm::vec3(24.0f, 3.0f, -61.0f)) *
            glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 scaledCamera = cameraPose * glm::scale(glm::mat4(1.0f), glm::vec3(0.016f));
        const glm::mat4 cameraView = BuildRigidCameraView(scaledCamera);
        const glm::vec4 pointAtTenMetres = cameraPose * glm::vec4(0.0f, 0.0f, -10.0f, 1.0f);
        if (glm::length(cameraView * pointAtTenMetres - glm::vec4(0.0f, 0.0f, -10.0f, 1.0f)) > 0.0001f)
            throw std::runtime_error("Inherited camera scale changed clip distances from world metres");
        const glm::mat4 projection = glm::perspective(glm::radians(65.0f), 16.0f / 9.0f, 0.1f, 500.0f);
        const glm::mat4 inverseCurrent = glm::inverse(projection * cameraView);
        const glm::vec4 clip(0.25f, -0.35f, 1.0f, 1.0f);
        const glm::mat4 translatedPose = glm::translate(glm::mat4(1.0f), glm::vec3(12.0f, 8.0f, -3.0f)) * cameraPose;
        const auto reproject = [&](const glm::mat4& previousPose)
        {
            return BuildSkyClipReprojection(inverseCurrent,
                projection * BuildRigidCameraView(previousPose), glm::vec3(cameraPose[3])) * clip;
        };
        const glm::vec4 translatedClip = reproject(translatedPose);
        if (glm::length(glm::vec2(translatedClip) / translatedClip.w - glm::vec2(clip)) > 0.0001f)
            throw std::runtime_error("Sky motion incorrectly includes camera translation");
        const glm::mat4 rotatedPose = cameraPose * glm::rotate(glm::mat4(1.0f),
            0.2f, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec4 worldPoint = inverseCurrent * clip;
        const glm::vec4 worldDirection(glm::vec3(worldPoint) - glm::vec3(cameraPose[3]) * worldPoint.w, 0.0f);
        const glm::vec4 expectedSky = projection * BuildRigidCameraView(rotatedPose) * worldDirection;
        const glm::vec4 rotatedClip = reproject(rotatedPose);
        if (glm::length(glm::vec2(rotatedClip) / rotatedClip.w - glm::vec2(expectedSky) / expectedSky.w) > 0.0001f ||
            glm::length(glm::vec2(rotatedClip) / rotatedClip.w - glm::vec2(clip)) < 0.01f)
            throw std::runtime_error("Sky motion lost camera rotation or changed projected direction");
        {
            const std::array<float, 3> fovs{65.0f, 45.0f, 75.0f};
            for (size_t index = 1; index < fovs.size(); ++index)
            {
                const auto currentProjection = glm::perspective(glm::radians(fovs[index]),
                    16.0f / 9.0f, .1f, 500.0f);
                const auto previousProjection = glm::perspective(glm::radians(fovs[index - 1]),
                    16.0f / 9.0f, .1f, 500.0f);
                const auto currentInverse = glm::inverse(currentProjection * cameraView);
                const auto previousView = BuildRigidCameraView(translatedPose);
                const auto point = currentInverse * clip;
                const glm::vec4 direction(glm::vec3(point) - glm::vec3(cameraPose[3]) * point.w, 0);
                const auto expected = previousProjection * previousView * direction;
                const auto actual = BuildSkyClipReprojection(currentInverse,
                    previousProjection * previousView, glm::vec3(cameraPose[3])) * clip;
                if (glm::distance(glm::vec2(actual) / actual.w, glm::vec2(expected) / expected.w) > 1e-4f ||
                    glm::distance(glm::vec2(actual) / actual.w, glm::vec2(clip)) < .01f)
                    throw std::runtime_error("Sky reprojection lost off-centre motion during consecutive FOV changes");
            }
        }
        {
            Level lookupLevel;
            const UUID reusedId;
            Entity first = lookupLevel.CreateEntity(reusedId, "First lookup owner");
            if (lookupLevel.TryGetEntityByUUID(reusedId) != first ||
                lookupLevel.TryGetEntityByUUID(reusedId) != first)
                throw std::runtime_error("Level UUID cache lost an existing entity");
            lookupLevel.DestroyEntity(first);
            if (lookupLevel.TryGetEntityByUUID(reusedId))
                throw std::runtime_error("Level UUID cache returned a destroyed entity");
            Entity replacement = lookupLevel.CreateEntity(reusedId, "Replacement lookup owner");
            if (lookupLevel.TryGetEntityByUUID(reusedId) != replacement)
                throw std::runtime_error("Level UUID cache missed a recreated entity");
            const UUID changedId;
            replacement.GetComponent<IDComponent>().ID = changedId;
            if (lookupLevel.TryGetEntityByUUID(reusedId) ||
                lookupLevel.TryGetEntityByUUID(changedId) != replacement)
                throw std::runtime_error("Level UUID cache retained an edited ID");
        }
        Level level;
        Entity entity = level.CreateEntity("MixedMesh");
        MeshComponent& meshComponent = entity.AddComponent<MeshComponent>();
        meshComponent.mMesh = CreateRef<Mesh>();

        std::vector<StaticVertex> staticVertices(3);
        staticVertices[0].Pos = { 0.0f, 0.0f, 0.0f };
        staticVertices[1].Pos = { 1.0f, 0.0f, 0.0f };
        staticVertices[2].Pos = { 0.0f, 1.0f, 0.0f };
        std::vector<SkinnedVertex> skinnedVertices(3);
        skinnedVertices[0].Pos = { 0.0f, 0.0f, 1.0f };
        skinnedVertices[1].Pos = { 1.0f, 0.0f, 1.0f };
        skinnedVertices[2].Pos = { 0.0f, 1.0f, 1.0f };
        const std::vector<uint32_t> indices = { 0, 1, 2 };

        Ref<Material> defaultMaterial = CreateRef<Material>();
        defaultMaterial->baseColorFactor = { 0.8f, 0.6f, 0.4f, 1.0f };
        Ref<Material> customMaterial = CreateRef<Material>();
        customMaterial->mType = MaterialType::Custom;
        customMaterial->mPassName = "TestSurface";
        customMaterial->mShaderPath = "Shaders/TestSurface.hlsl";
        customMaterial->mMatResPath = "Assets/Material/TestSurface.hmat";
        customMaterial->mPbrMatRes.alphaMode = "MASK";
        customMaterial->mPbrMatRes.alphaCutoff = 0.37f;
        customMaterial->mPbrMatRes.doubleSided = true;
        customMaterial->mMatRes.DepthWrite = true;
        meshComponent.mMesh->mMaterial = { defaultMaterial, customMaterial };

        Ref<SubMesh> defaultStatic = CreateRef<SubMesh>(staticVertices, indices);
        defaultStatic->mMaterialIndex = 0;
        meshComponent.mMesh->mSubMeshes.push_back(defaultStatic);
        meshComponent.mMesh->mSubMeshes.push_back(
            CreateRef<SubMesh>(std::move(skinnedVertices), indices));
        Ref<SubMesh> customStatic = CreateRef<SubMesh>(std::move(staticVertices), indices);
        customStatic->mMaterialIndex = 1;
        meshComponent.mMesh->mSubMeshes.push_back(customStatic);
        meshComponent.mMesh->bAnimated = false;

        RenderWorldExtractor extractor;
        RenderViewDescription temporalView;
        temporalView.SceneGeneration = 17;
        temporalView.JitterSequenceGeneration = 23;
        temporalView.ProjectionGeneration = 29;
        temporalView.bCameraCut = true;
        const RenderWorld world = extractor.Extract(level, temporalView);
        if (world.Objects.size() != 1 || world.Objects.front().Sections.size() != 3)
            throw std::runtime_error("Mixed mesh extraction did not preserve its sections");

        {
            Level presentationLevel;
            auto character = presentationLevel.CreateEntity("InterpolatedCharacter");
            auto visual = presentationLevel.CreateEntity("AttachedVisual");
            auto presentationMesh = CreateRef<Mesh>();
            presentationMesh->mMaterial = { defaultMaterial };
            presentationMesh->mSubMeshes = { defaultStatic };
            character.AddComponent<MeshComponent>().mMesh = presentationMesh;
            visual.AddComponent<MeshComponent>().mMesh = presentationMesh;
            character.AddComponent<CharacterMovementComponent>();
            if (!presentationLevel.SetEntityParent(visual, character, false))
                throw std::runtime_error("Could not construct character presentation hierarchy");
            auto& rootTransform = character.GetComponent<TransformComponent>();
            auto& visualTransform = visual.GetComponent<TransformComponent>();
            visualTransform.Translation = { .5f, 1.0f, 0.0f };
            rootTransform.Translation = { 2.0f, 0.0f, 0.0f };
            auto& history = character.GetComponent<CharacterMovementComponent>().RuntimePresentation;
            history.Advance(glm::vec3(0), rootTransform.Translation);
            if (presentationLevel.GetFixedInterpolationAlpha() != 0.0)
                throw std::runtime_error("Fresh test level must start at fixed interpolation alpha zero");
            RenderViewDescription presentationView;
            RenderWorldExtractor presentationExtractor;
            const auto findPresented = [](const RenderWorld& frame, Entity target) -> const RenderObject&
            {
                const int id = static_cast<int>(static_cast<uint32_t>(target));
                const auto found = std::find_if(frame.Objects.begin(), frame.Objects.end(),
                    [id](const auto& item) { return item.EntityId == id; });
                if (found == frame.Objects.end()) throw std::runtime_error("Presentation test object was culled");
                return *found;
            };
            const auto expectPosition = [](const glm::mat4& matrix, glm::vec3 expected)
            {
                if (glm::distance(glm::vec3(matrix[3]), expected) > 1e-5f)
                    throw std::runtime_error("Character presentation position/history differs from fixed interpolation");
            };
            const auto first = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(first, character).WorldTransform, glm::vec3(0));
            expectPosition(findPresented(first, visual).WorldTransform, {.5f, 1, 0});
            expectPosition(findPresented(first, character).PreviousWorldTransform, glm::vec3(0));
            rootTransform.Translation = { 4, 0, 0 };
            history.Advance({2, 0, 0}, rootTransform.Translation);
            const auto second = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(second, character).WorldTransform, {2, 0, 0});
            expectPosition(findPresented(second, visual).WorldTransform, {2.5f, 1, 0});
            expectPosition(findPresented(second, character).PreviousWorldTransform, glm::vec3(0));
            expectPosition(findPresented(second, visual).PreviousWorldTransform, {.5f, 1, 0});
            const auto repeated = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(repeated, character).WorldTransform, {2, 0, 0});
            expectPosition(findPresented(repeated, visual).WorldTransform, {2.5f, 1, 0});
            expectPosition(findPresented(repeated, visual).PreviousWorldTransform, {2.5f, 1, 0});
            if (rootTransform.Translation != glm::vec3(4, 0, 0) ||
                visualTransform.Translation != glm::vec3(.5f, 1, 0))
                throw std::runtime_error("Render interpolation modified authoritative physics transforms");
            presentationView.bEditorView = true;
            RenderWorldExtractor editorExtractor;
            const auto editor = editorExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(editor, character).WorldTransform, {4, 0, 0});
            expectPosition(findPresented(editor, visual).WorldTransform, {4.5f, 1, 0});
            rootTransform.Translation = {8, 0, 0};
            history.Reset(rootTransform.Translation);
            presentationView.bEditorView = false;
            const auto teleported = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(teleported, character).WorldTransform, {8, 0, 0});
            expectPosition(findPresented(teleported, visual).WorldTransform, {8.5f, 1, 0});
            const auto settled = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(settled, character).PreviousWorldTransform, {8, 0, 0});
            expectPosition(findPresented(settled, visual).PreviousWorldTransform, {8.5f, 1, 0});

            auto sibling = presentationLevel.CreateEntity("SiblingVisual");
            sibling.AddComponent<MeshComponent>().mMesh = presentationMesh;
            sibling.GetComponent<TransformComponent>().Translation = {.75f, 1, 0};
            if (!presentationLevel.SetEntityParent(sibling, character, false))
                throw std::runtime_error("Could not attach sibling visual");
            rootTransform.Translation = {10, 0, 0};
            history.Advance({8, 0, 0}, rootTransform.Translation);
            const auto siblings = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(siblings, visual).WorldTransform, {8.5f, 1, 0});
            expectPosition(findPresented(siblings, sibling).WorldTransform, {8.75f, 1, 0});

            // A child with its own controller must override the shared parent's offset.
            auto& childHistory = visual.AddComponent<CharacterMovementComponent>().RuntimePresentation;
            childHistory.Advance({6, 1, 0}, {10.5f, 1, 0});
            const auto childController = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(childController, visual).WorldTransform, {6, 1, 0});
            expectPosition(findPresented(childController, sibling).WorldTransform, {8.75f, 1, 0});
            visual.RemoveComponent<CharacterMovementComponent>();
            const auto inheritedAgain = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(inheritedAgain, visual).WorldTransform, {8.5f, 1, 0});

            // Detach without changing local TRS: never reuse the old parented world matrix.
            // Use identity TRS to also exercise a previously uninitialized local cache.
            visual.GetComponent<TransformComponent>().Translation = glm::vec3(0);
            const auto identityLocal = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(identityLocal, visual).WorldTransform, {8, 0, 0});
            if (!presentationLevel.SetEntityParent(visual, {}, false))
                throw std::runtime_error("Could not detach cached visual");
            const auto detached = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(detached, visual).WorldTransform, {0, 0, 0});
            expectPosition(findPresented(detached, visual).PreviousWorldTransform, {8, 0, 0});
            RenderWorldExtractor uncachedExtractor;
            const auto uncached = uncachedExtractor.Extract(presentationLevel, presentationView);
            const auto& cachedBounds = findPresented(detached, visual).Bounds;
            const auto& uncachedBounds = findPresented(uncached, visual).Bounds;
            if (cachedBounds.bValid != uncachedBounds.bValid ||
                (cachedBounds.bValid && glm::distance(cachedBounds.Center, uncachedBounds.Center) > 1e-5f))
                throw std::runtime_error("Removing presentation offset retained stale cached bounds");
            if (!presentationLevel.SetEntityParent(visual, character, false))
                throw std::runtime_error("Could not reattach cached visual");
            rootTransform.Translation = {12, 0, 0};
            history.Reset(rootTransform.Translation);
            const auto movedParent = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(movedParent, visual).WorldTransform, {12, 0, 0});
            expectPosition(findPresented(movedParent, sibling).WorldTransform, {12.75f, 1, 0});
            history.Advance({10, 0, 0}, rootTransform.Translation);
            const auto interpolatedAgain = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(interpolatedAgain, visual).WorldTransform, {10, 0, 0});
            character.RemoveComponent<CharacterMovementComponent>();
            const auto controllerRemoved = presentationExtractor.Extract(presentationLevel, presentationView);
            expectPosition(findPresented(controllerRemoved, visual).WorldTransform, {12, 0, 0});
            const auto freshWithoutController = uncachedExtractor.Extract(presentationLevel, presentationView);
            const auto& removedBounds = findPresented(controllerRemoved, visual).Bounds;
            const auto& freshBounds = findPresented(freshWithoutController, visual).Bounds;
            if (!removedBounds.bValid || !freshBounds.bValid ||
                glm::distance(removedBounds.Center, freshBounds.Center) > 1e-5f)
                throw std::runtime_error("Removing a controller retained its presentation offset in cached bounds");
        }

        {
            Level sharedLevel;
            auto sharedMaterial = CreateRef<Material>();
            sharedMaterial->baseColorFactor = { .8f, .6f, .4f, 1.0f };
            auto sharedMesh = CreateRef<Mesh>();
            sharedMesh->mMaterial = { sharedMaterial };
            sharedMesh->mSubMeshes = { defaultStatic };
            auto first = sharedLevel.CreateEntity("FirstSharedMaterial");
            auto second = sharedLevel.CreateEntity("SecondSharedMaterial");
            first.AddComponent<MeshComponent>().mMesh = sharedMesh;
            second.AddComponent<MeshComponent>().mMesh = sharedMesh;
            RenderWorldExtractor sharedExtractor;
            const auto before = sharedExtractor.Extract(sharedLevel, temporalView);
            const auto payload = before.Objects.at(0).Sections.at(0).SharedMaterialInstance;
            if (!payload || payload != before.Objects.at(1).Sections.at(0).SharedMaterialInstance)
                throw std::runtime_error("Repeated material instances deep-copied immutable payloads");
            first.GetComponent<MeshComponent>().bRuntimeBlackSilhouette = true;
            const auto overrideFrame = sharedExtractor.Extract(sharedLevel, temporalView);
            size_t black = 0, ordinary = 0;
            for (const auto& item : overrideFrame.Objects)
            {
                const auto& snapshot = item.Sections.at(0).GetMaterialInstance();
                if (glm::vec3(snapshot.BaseColor) == glm::vec3(0)) ++black;
                else if (snapshot.BaseColor == payload->BaseColor) ++ordinary;
            }
            if (black != 1 || ordinary != 1)
                throw std::runtime_error("Shared material override contaminated another mesh instance");
            sharedMaterial->SetTexturePath(MaterialTextureSemantic::BaseColor, "", false);
            sharedMaterial->baseColorFactor = { .2f, .3f, .4f, 1.0f };
            first.GetComponent<MeshComponent>().bRuntimeBlackSilhouette = false;
            const auto after = sharedExtractor.Extract(sharedLevel, temporalView);
            const auto& current = after.Objects.at(0).Sections.at(0).GetMaterialInstance();
            if (current.Revision == payload->Revision || current.BaseColor == payload->BaseColor ||
                payload->BaseColor != glm::vec4(.8f, .6f, .4f, 1.0f))
                throw std::runtime_error("Material refresh changed a submitted snapshot or kept stale values");
        }

        {
            Level skinnedLevel;
            Entity skinnedEntity = skinnedLevel.CreateEntity("AnimatedBounds");
            skinnedEntity.GetComponent<TransformComponent>().Translation = { 100.0f, 0.0f, 0.0f };
            MeshComponent& skinnedComponent = skinnedEntity.AddComponent<MeshComponent>();
            skinnedComponent.mMesh = CreateRef<Mesh>();
            std::vector<SkinnedVertex> vertices(3);
            vertices[0].Pos = { 0.0f, 0.0f, 0.0f };
            vertices[1].Pos = { 1.0f, 0.0f, 0.0f };
            vertices[2].Pos = { 0.0f, 1.0f, 0.0f };
            skinnedComponent.mMesh->mSubMeshes.push_back(
                CreateRef<SubMesh>(std::move(vertices), indices));
            skinnedComponent.mMesh->mMaterial.push_back(CreateRef<Material>());
            skinnedComponent.mMesh->mBindBoneMatrices.push_back(glm::mat4(1.0f));
            skinnedComponent.mMesh->bUseExternalBoneMatrices = true;
            skinnedComponent.mMesh->mExternalBoneMatrices.push_back(
                glm::translate(glm::mat4(1.0f), glm::vec3(20.0f, 0.0f, 0.0f)));
            RenderWorldExtractor skinnedExtractor;
            const RenderWorld firstPose = skinnedExtractor.Extract(skinnedLevel, temporalView);
            if (firstPose.Objects.size() != 1 || !firstPose.Objects[0].Bounds.bValid ||
                firstPose.Objects[0].Bounds.Center.x - firstPose.Objects[0].Bounds.Extents.x > 100.0f ||
                firstPose.Objects[0].Bounds.Center.x + firstPose.Objects[0].Bounds.Extents.x < 121.0f)
                throw std::runtime_error("Skinned bounds did not contain bind and first animated pose");
            skinnedComponent.mMesh->mExternalBoneMatrices[0] =
                glm::translate(glm::mat4(1.0f), glm::vec3(40.0f, 0.0f, 0.0f));
            const RenderWorld secondPose = skinnedExtractor.Extract(skinnedLevel, temporalView);
            if (secondPose.Objects.size() != 1 || !secondPose.Objects[0].Bounds.bValid ||
                secondPose.Objects[0].Bounds.Center.x + secondPose.Objects[0].Bounds.Extents.x < 141.0f)
                throw std::runtime_error("Skinned bounds did not follow the updated bone palette");
        }

        {
            Level staticLevel;
            Entity staticEntity = staticLevel.CreateEntity("CachedStaticTransform");
            auto& staticMesh = staticEntity.AddComponent<MeshComponent>();
            staticMesh.mMesh = CreateRef<Mesh>();
            staticMesh.mMesh->mSubMeshes = { defaultStatic };
            staticMesh.mMesh->mMaterial = { defaultMaterial };
            staticMesh.StaticLightmapPath = "Assets/Lighting/TestAtlas.dds";
            staticMesh.StaticLightmapUvScale = { 0.25f, 0.5f };
            staticMesh.StaticLightmapUvBias = { 0.125f, 0.25f };
            staticMesh.StaticLightmapScale0 = { 1.0f, 2.0f, 3.0f, 4.0f };
            staticMesh.StaticLightmapAdd0 = { 0.1f, 0.2f, 0.3f, 0.4f };
            staticMesh.StaticLightmapScale1 = { 5.0f, 6.0f, 7.0f, 8.0f };
            staticMesh.StaticLightmapAdd1 = { 0.5f, 0.6f, 0.7f, 0.8f };
            staticMesh.StaticLightmapBlend = 0.25f;
            RenderWorldExtractor staticExtractor;
            RenderViewDescription staticView;
            staticView.SelectedEntity = static_cast<int>(static_cast<uint32_t>(staticEntity));
            const RenderWorld first = staticExtractor.Extract(staticLevel, staticView);
            const RenderWorld repeated = staticExtractor.Extract(staticLevel, staticView);
            if (first.Objects.size() != 1 ||
                first.Objects[0].StaticLightmapPath != staticMesh.StaticLightmapPath ||
                !glm::all(glm::equal(first.Objects[0].StaticLightmapUvScale, staticMesh.StaticLightmapUvScale)) ||
                !glm::all(glm::equal(first.Objects[0].StaticLightmapUvBias, staticMesh.StaticLightmapUvBias)) ||
                !glm::all(glm::equal(first.Objects[0].StaticLightmapScale0, staticMesh.StaticLightmapScale0)) ||
                !glm::all(glm::equal(first.Objects[0].StaticLightmapAdd0, staticMesh.StaticLightmapAdd0)) ||
                !glm::all(glm::equal(first.Objects[0].StaticLightmapScale1, staticMesh.StaticLightmapScale1)) ||
                !glm::all(glm::equal(first.Objects[0].StaticLightmapAdd1, staticMesh.StaticLightmapAdd1)))
                throw std::runtime_error("Static lightmap data was lost during render world extraction");
            if (first.StaticLightmapAssets != std::vector<std::string>{staticMesh.StaticLightmapPath})
                throw std::runtime_error("Culled static lightmap prewarm set lost a scene atlas");
            if (first.Objects[0].StaticLightmapBlend != staticMesh.StaticLightmapBlend)
                throw std::runtime_error("Static lightmap blend was lost during render world extraction");
            if (first.Objects.size() != 1 || repeated.Objects.size() != 1 ||
                repeated.Objects[0].WorldTransform != first.Objects[0].WorldTransform)
                throw std::runtime_error("Unchanged static transform cache changed the render world");
            const std::string originalLightmapPath = staticMesh.StaticLightmapPath;
            staticMesh.StaticLightmapPath = "Assets/Lighting/ReplacementAtlas.dds";
            const RenderWorld changedLightmap = staticExtractor.Extract(staticLevel, staticView);
            if (changedLightmap.StaticLightmapAssets != std::vector<std::string>{staticMesh.StaticLightmapPath})
                throw std::runtime_error("Static lightmap prewarm set ignored an ECS path edit");
            staticMesh.StaticLightmapBlend = 0.0f;
            const RenderWorld disabledLightmap = staticExtractor.Extract(staticLevel, staticView);
            if (!disabledLightmap.StaticLightmapAssets.empty())
                throw std::runtime_error("Disabled static lightmap remained in the prewarm set");
            staticMesh.StaticLightmapPath = originalLightmapPath;
            staticMesh.StaticLightmapBlend = 0.25f;
            const RenderWorld restoredLightmap = staticExtractor.Extract(staticLevel, staticView);
            if (restoredLightmap.StaticLightmapAssets != std::vector<std::string>{originalLightmapPath})
                throw std::runtime_error("Restored static lightmap was missing from the prewarm set");
            staticEntity.GetComponent<TransformComponent>().Translation.x = 0.25f;
            const RenderWorld moved = staticExtractor.Extract(staticLevel, staticView);
            if (moved.Objects.size() != 1 ||
                std::abs(moved.Objects[0].WorldTransform[3].x - 0.25f) > 1e-6f ||
                std::abs(moved.Objects[0].PreviousWorldTransform[3].x) > 1e-6f)
                throw std::runtime_error("Static transform cache ignored an ECS edit or lost motion history");
            staticView.SelectedEntity = -1;
            staticEntity.GetComponent<TransformComponent>().Translation.x = 100.0f;
            const RenderWorld hidden = staticExtractor.Extract(staticLevel, staticView);
            if (!hidden.Objects.empty())
                throw std::runtime_error("Offscreen static mesh was submitted despite frustum culling");
            staticEntity.GetComponent<TransformComponent>().Translation.x = 0.5f;
            const RenderWorld visibleAgain = staticExtractor.Extract(staticLevel, staticView);
            if (visibleAgain.Objects.size() != 1 ||
                std::abs(visibleAgain.Objects[0].PreviousWorldTransform[3].x - 0.5f) > 1e-6f)
                throw std::runtime_error("Re-entering static mesh retained stale offscreen motion history");
            const Ref<Mesh> originalMesh = staticMesh.mMesh;
            std::vector<StaticVertex> largerVertices(3);
            largerVertices[0].Pos = { 0.0f, 0.0f, 0.0f };
            largerVertices[1].Pos = { 5.0f, 0.0f, 0.0f };
            largerVertices[2].Pos = { 0.0f, 1.0f, 0.0f };
            staticMesh.mMesh = CreateRef<Mesh>();
            staticMesh.mMesh->mSubMeshes = { CreateRef<SubMesh>(std::move(largerVertices), indices) };
            staticMesh.mMesh->mMaterial = { defaultMaterial };
            const RenderWorld replacedMesh = staticExtractor.Extract(staticLevel, staticView);
            if (replacedMesh.Objects.size() != 1 ||
                replacedMesh.Objects[0].Bounds.Center.x + replacedMesh.Objects[0].Bounds.Extents.x < 5.5f)
                throw std::runtime_error("Static world bounds cache ignored a replaced mesh resource");
            staticMesh.mMesh->mSubMeshes[0]->BeginDeformation();
            staticMesh.mMesh->mSubMeshes[0]->mStaticVertices[1].Pos.x = 6.0f;
            staticMesh.mMesh->mSubMeshes[0]->EndDeformation();
            const RenderWorld deformedMesh = staticExtractor.Extract(staticLevel, staticView);
            if (deformedMesh.Objects.size() != 1 ||
                deformedMesh.Objects[0].Bounds.Center.x + deformedMesh.Objects[0].Bounds.Extents.x < 6.5f)
                throw std::runtime_error("Static world bounds cache ignored a deformation revision");
            staticMesh.mMesh = originalMesh;
            staticView.SelectedEntity = static_cast<int>(static_cast<uint32_t>(staticEntity));
            staticEntity.GetComponent<TransformComponent>().Translation.x = 0.25f;
            Entity parent = staticLevel.CreateEntity("MovingParent");
            parent.GetComponent<TransformComponent>().Translation.x = 0.4f;
            if (!staticLevel.SetEntityParent(staticEntity, parent, false))
                throw std::runtime_error("Static transform test could not attach the moving parent");
            const RenderWorld parented = staticExtractor.Extract(staticLevel, staticView);
            if (parented.Objects.size() != 1 ||
                std::abs(parented.Objects[0].WorldTransform[3].x - 0.65f) > 1e-6f)
                throw std::runtime_error("Cached local transform was reused for a parented entity");
            parent.GetComponent<TransformComponent>().Translation.x = 1.0f;
            const RenderWorld movedParent = staticExtractor.Extract(staticLevel, staticView);
            if (movedParent.Objects.size() != 1 ||
                std::abs(movedParent.Objects[0].WorldTransform[3].x - 1.25f) > 1e-6f ||
                std::abs(movedParent.Objects[0].PreviousWorldTransform[3].x - 0.65f) > 1e-6f ||
                std::abs(movedParent.Objects[0].Bounds.Center.x -
                    parented.Objects[0].Bounds.Center.x - 0.6f) > 1e-5f)
                throw std::runtime_error("Parent motion left cached transform or bounds stale");
        }

        const RenderObject& object = world.Objects.front();
        if (object.Path != RenderPath::CpuSkinned ||
            object.Sections[0].Path != RenderPath::GpuDrivenOpaque ||
            object.Sections[1].Path != RenderPath::CpuSkinned ||
            object.Sections[2].Path != RenderPath::CpuOpaque ||
            !object.Sections[2].GetMaterialInstance().bCustomSurface ||
            object.Sections[2].GetMaterialInstance().ShaderPath != "Shaders/TestSurface.hlsl")
        {
            throw std::runtime_error(
                "Render paths did not preserve custom Surface identity and explicit CPU fallback");
        }
        if (world.SceneGeneration != temporalView.SceneGeneration ||
            world.Camera.JitterSequenceGeneration !=
                temporalView.JitterSequenceGeneration ||
            world.Camera.ProjectionGeneration !=
                temporalView.ProjectionGeneration ||
            !world.Camera.bCameraCut)
        {
            throw std::runtime_error(
                "RenderWorld extraction dropped temporal history identities");
        }

        meshComponent.mMesh->mBindBoneMatrices = {
            glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)) };
        meshComponent.mMesh->bPlayAnim = true;
        const RenderWorld awaitingAnimation = extractor.Extract(level, temporalView);
        if (awaitingAnimation.Objects.size() != 1 ||
            awaitingAnimation.Objects.front().BonePalette != meshComponent.mMesh->mBindBoneMatrices ||
            awaitingAnimation.Objects.front().PreviousBonePalette != meshComponent.mMesh->mBindBoneMatrices)
            throw std::runtime_error("An uninitialized animation pose did not use the imported bind palette");
        meshComponent.mMesh->bUseExternalBoneMatrices = true;
        meshComponent.mMesh->mExternalBoneMatrices = { glm::mat4(1.0f) };
        bool rejectedTruncatedPalette = false;
        try { (void)extractor.Extract(level, temporalView); }
        catch (const std::invalid_argument&) { rejectedTruncatedPalette = true; }
        if (!rejectedTruncatedPalette)
            throw std::runtime_error("A nonempty truncated bone palette was silently accepted");
        meshComponent.mMesh->mExternalBoneMatrices.clear();
        bool rejectedEmptyExternal = false;
        try { (void)extractor.Extract(level, temporalView); }
        catch (const std::invalid_argument&) { rejectedEmptyExternal = true; }
        if (!rejectedEmptyExternal)
            throw std::runtime_error("An empty external bone palette was silently accepted");
        meshComponent.mMesh->bUseExternalBoneMatrices = false;
        meshComponent.mMesh->mBindBoneMatrices.clear();
        meshComponent.mMesh->bPlayAnim = false;

        // Structural ECS edits must not reassign an existing material's GPU
        // identity. Silhouette overrides have a separate stable identity and
        // cannot contaminate the shared immutable snapshot cache.
        const uint32_t originalMaterialIndex = object.Sections[0].MaterialGpuIndex;
        Entity insertedEntity = level.CreateEntity("InsertedBeforeExistingIteration");
        auto& insertedMesh = insertedEntity.AddComponent<MeshComponent>();
        insertedMesh.mMesh = CreateRef<Mesh>();
        insertedMesh.mMesh->mSubMeshes = { defaultStatic };
        insertedMesh.mMesh->mMaterial = { CreateRef<Material>() };
        const int originalEntityId = object.EntityId;
        const auto findOriginal = [originalEntityId](const RenderWorld& frame) -> const RenderObject&
        {
            const auto found = std::find_if(frame.Objects.begin(), frame.Objects.end(),
                [originalEntityId](const RenderObject& candidate) { return candidate.EntityId == originalEntityId; });
            if (found == frame.Objects.end()) throw std::runtime_error("Original test render object disappeared");
            return *found;
        };
        temporalView.bCameraCut = false;
        const RenderWorld reorderedWorld = extractor.Extract(level, temporalView);
        if (findOriginal(reorderedWorld).Sections[0].MaterialGpuIndex != originalMaterialIndex)
            throw std::runtime_error("Structural edits reassigned an existing material GPU identity");
        entity.GetComponent<MeshComponent>().bRuntimeBlackSilhouette = true;
        const RenderWorld silhouetteWorld = extractor.Extract(level, temporalView);
        const auto& silhouetteSection = findOriginal(silhouetteWorld).Sections[0];
        if (silhouetteSection.MaterialGpuIndex == originalMaterialIndex ||
            glm::length(glm::vec3(silhouetteSection.GetMaterialInstance().BaseColor)) != 0.0f)
            throw std::runtime_error("Silhouette did not use an independent material identity/snapshot");
        const RenderWorld repeatedSilhouette = extractor.Extract(level, temporalView);
        if (findOriginal(repeatedSilhouette).Sections[0].MaterialGpuIndex != silhouetteSection.MaterialGpuIndex)
            throw std::runtime_error("Silhouette material identity grew on every frame");
        entity.GetComponent<MeshComponent>().bRuntimeBlackSilhouette = false;
        const RenderWorld restoredWorld = extractor.Extract(level, temporalView);
        if (findOriginal(restoredWorld).Sections[0].MaterialGpuIndex != originalMaterialIndex ||
            findOriginal(restoredWorld).Sections[0].GetMaterialInstance().BaseColor != defaultMaterial->baseColorFactor)
            throw std::runtime_error("Silhouette override contaminated the ordinary shared material snapshot");
        entity.GetComponent<MeshComponent>().mMesh->mMaterial[0] = CreateRef<Material>();
        const RenderWorld replacedMaterialWorld = extractor.Extract(level, temporalView);
        if (findOriginal(replacedMaterialWorld).Sections[0].MaterialGpuIndex == originalMaterialIndex)
            throw std::runtime_error("Replacement material lifetime reused a live material identity");

        {
            Level overrideLevel;
            auto sceneEntity = overrideLevel.CreateEntity("Scene Rendering Settings");
            auto& sceneSettings = sceneEntity.AddComponent<RenderSettingsComponent>();
            sceneSettings.Overrides = { { RenderSettingIds::Path, "Forward" }, { RenderSettingIds::AoEnabled, false } };
            auto cameraEntity = overrideLevel.CreateEntity("Active Camera");
            auto& camera = cameraEntity.AddComponent<CameraComponent>();
            camera.RenderOverrides = { { RenderSettingIds::Path, "Deferred" } };
            Json encoded;
            Serializer::Serialize(encoded, sceneSettings);
            RenderSettingsComponent restoredScene;
            Serializer::Deserialize(encoded, restoredScene);
            if (restoredScene.Overrides != sceneSettings.Overrides) throw std::runtime_error("Scene render overrides did not serialize");
            Serializer::Serialize(encoded, camera);
            CameraComponent restoredCamera;
            Serializer::Deserialize(encoded, restoredCamera);
            if (restoredCamera.RenderOverrides != camera.RenderOverrides) throw std::runtime_error("Camera render overrides did not serialize");
            CameraComponent legacyCamera;
            RenderSettingsComponent legacyScene;
            Serializer::Deserialize(Json::object(), legacyCamera);
            Serializer::Deserialize(Json::object(), legacyScene);
            if (!legacyCamera.RenderOverrides.empty() || !legacyScene.Overrides.empty())
                throw std::runtime_error("Old scenes acquired unsolicited rendering overrides");
            RenderViewDescription overrideView;
            overrideView.CameraEntityId = static_cast<int>(static_cast<uint32_t>(cameraEntity));
            overrideView.WorldRenderOverrides = { { RenderSettingIds::AoEnabled, true } };
            overrideView.CameraRenderOverrides = { { RenderSettingIds::SsrEnabled, false } };
            RenderWorldExtractor overrideExtractor;
            const auto overriddenWorld = overrideExtractor.Extract(overrideLevel, overrideView);
            const auto base = ResolveRenderSettings(MakeEmergencyFallbackRenderSettings(), {}, {}, {});
            const auto result = ResolveRenderViewSettings(base, overriddenWorld.RenderOverrides, overriddenWorld.Camera.RenderOverrides);
            if (result.Requested.Path != RenderingPath::Deferred || !result.Requested.AmbientOcclusion.bEnabled ||
                result.Requested.Reflections.bScreenSpaceEnabled)
                throw std::runtime_error("Persisted World/Camera overrides or explicit view precedence were not extracted");
            camera.RenderOverrides = Json::array();
            bool rejected = false;
            try { (void)overrideExtractor.Extract(overrideLevel, overrideView); } catch (const std::invalid_argument&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Malformed camera overrides were silently ignored");
            camera.RenderOverrides = Json::object();
            auto duplicate = overrideLevel.CreateEntity("Duplicate Scene Settings");
            duplicate.AddComponent<RenderSettingsComponent>();
            rejected = false;
            try { (void)overrideExtractor.Extract(overrideLevel, overrideView); } catch (const std::invalid_argument&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Multiple scene render settings components were silently ordered");
        }

        {
            Level boundaryLevel;
            auto cameraEntity = boundaryLevel.CreateEntity("Boundary Camera");
            auto& camera = cameraEntity.AddComponent<CameraComponent>();
            camera.Primary = true;
            RenderSystem3D system(&boundaryLevel);
            RenderPrimitiveCollector collector;
            const auto update = [&](bool draw)
            {
                collector.BeginFrame();
                {
                    Renderer2D::CollectorScope scope(collector);
                    if (draw)
                    {
                        Renderer2D::BeginScene(camera.Camera, glm::mat4(1.0f));
                        Renderer2D::DrawQuad(glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
                        Renderer2D::EndScene();
                    }
                    system.OnUpdateRuntime(Timestep(0.0f));
                }
                collector.EndFrame();
            };
            auto warmMesh = CreateRef<Mesh>();
            warmMesh->mSubMeshes.push_back(CreateRef<SubMesh>(
                std::vector<StaticVertex>(3), std::vector<uint32_t>{ 0, 1, 2 }));
            warmMesh->mMaterial.push_back(CreateRef<Material>());
            auto warmup = system.PrepareMeshResources(warmMesh);
            update(false);
            const auto warmWorld = system.AcquireLatestRenderWorld();
            if (!warmWorld || !warmWorld->Objects.empty() || warmWorld->ResourceWarmupObjects.size() != 1)
                throw std::runtime_error("Resource prewarm added visible objects or lost its requested mesh.");
            const auto warmSnapshot = warmWorld->ResourceWarmupObjects.front();
            update(false);
            if (system.AcquireLatestRenderWorld()->ResourceWarmupObjects.front() != warmSnapshot)
                throw std::runtime_error("Unchanged prewarm material snapshots were rebuilt every frame.");
            warmMesh->mMaterial[0] = CreateRef<Material>();
            update(false);
            if (system.AcquireLatestRenderWorld()->ResourceWarmupObjects.front() == warmSnapshot ||
                warmSnapshot->Sections.front().MaterialResource == warmMesh->mMaterial[0])
                throw std::runtime_error("Material replacement failed to publish a new immutable prewarm snapshot.");
            warmup.reset();
            update(false);
            if (!system.AcquireLatestRenderWorld()->ResourceWarmupObjects.empty())
                throw std::runtime_error("Released prewarm lease retained scene residency requests.");
            auto collapsed = boundaryLevel.CreateEntity("Collapsed loading resource");
            collapsed.AddComponent<MeshComponent>().mMesh = warmMesh;
            collapsed.GetComponent<TransformComponent>().Scale = glm::vec3(0.0f);
            auto shared = boundaryLevel.CreateEntity("Shared collapsed loading resource");
            shared.AddComponent<MeshComponent>().mMesh = warmMesh;
            shared.GetComponent<TransformComponent>().Scale = glm::vec3(0.0f);
            auto outside = boundaryLevel.CreateEntity("Culled loading resource");
            auto outsideMesh = CreateRef<Mesh>();
            outsideMesh->mSubMeshes = { defaultStatic };
            outsideMesh->mMaterial = { defaultMaterial };
            outside.AddComponent<MeshComponent>().mMesh = outsideMesh;
            outside.GetComponent<TransformComponent>().Translation = glm::vec3(100000.0f, 0.0f, -3.0f);
            auto unloaded = boundaryLevel.CreateEntity("Unloaded loading resource");
            unloaded.AddComponent<MeshComponent>();
            unloaded.GetComponent<TransformComponent>().Scale = glm::vec3(0.0f);
            boundaryLevel.CreateEntity("Null loading resource").AddComponent<MeshComponent>().mMesh.reset();
            auto levelWarmups = system.PrepareLevelMeshResources();
            if (levelWarmups.size() != 2)
                throw std::runtime_error("Level resource warmup missed hidden meshes or duplicated shared/unloaded meshes.");
            update(false);
            const auto hiddenWarmWorld = system.AcquireLatestRenderWorld();
            if (!hiddenWarmWorld || !hiddenWarmWorld->Objects.empty() || hiddenWarmWorld->ResourceWarmupObjects.size() != 2)
                throw std::runtime_error("Level loading warmup changed hidden visibility or lost resource-only submissions.");
            levelWarmups.clear();
            update(false);
            if (!system.AcquireLatestRenderWorld()->ResourceWarmupObjects.empty())
                throw std::runtime_error("Completed level warmup kept resource-only requests alive.");
            levelWarmups = system.PrepareLevelMeshResources();
            system.OnRuntimeStop();
            system.OnRuntiemStart();
            update(false);
            if (!system.AcquireLatestRenderWorld()->ResourceWarmupObjects.empty())
                throw std::runtime_error("Stopped level warmup leaked requests across runtime restart.");
            levelWarmups.clear();
            const auto previous = system.AcquireLatestRenderWorld();
            if (!previous) throw std::runtime_error("Initial world was not submitted");
            system.RequestCameraCut();
            camera.RenderOverrides = Json::array();
            update(true);
            update(true);
            if (system.AcquireLatestRenderWorld() != previous)
                throw std::runtime_error("Rejected extraction replaced the submitted world");
            camera.RenderOverrides = Json::object();
            update(false);
            const auto recovered = system.AcquireLatestRenderWorld();
            if (!recovered || recovered == previous || !recovered->Camera.bCameraCut ||
                recovered->FrameNumber != previous->FrameNumber + 1 || recovered->Primitives2D.size() != 1)
                throw std::runtime_error("Recovery lost pending camera cut/primitive data or accumulated duplicate frames");
            update(false);
            if (system.AcquireLatestRenderWorld()->Camera.bCameraCut ||
                !system.AcquireLatestRenderWorld()->Primitives2D.empty())
                throw std::runtime_error("Recovered pending frame was replayed more than once");
        }

        {
            class MoveCameraScript final : public ScriptableEntity
            {
                void OnUpdate(Timestep) override
                {
                    GetComponent<TransformComponent>().Translation.x += 3.0f;
                }
            };
            Level runtimeLevel;
            auto cameraEntity = runtimeLevel.CreateEntity("Scripted Camera");
            cameraEntity.AddComponent<CameraComponent>().Primary = true;
            cameraEntity.AddComponent<NativeScriptComponent>().Bind<MoveCameraScript>();
            runtimeLevel.OnRuntimeStart();
            for (int frame = 1; frame <= 2; ++frame)
            {
                runtimeLevel.OnUpdateRuntime(Timestep(0.0f));
                const auto snapshot = runtimeLevel.TryGetSystem<RenderSystem3D>()->AcquireLatestRenderWorld();
                if (!snapshot || std::abs(snapshot->Camera.Position.x - 3.0f * frame) > 1e-4f)
                    throw std::runtime_error("RenderPrepare missed a native script camera write in the same frame");
            }
            runtimeLevel.OnRuntimeStop();
        }

        Level settingsLevel;
        Entity pointEntity = settingsLevel.CreateEntity("PhotometricPoint");
        auto& point = pointEntity.AddComponent<PointLightComponent>();
        point.Intensity = 1256.6370614f;
        point.IntensityUnits = 2;
        Entity spotEntity = settingsLevel.CreateEntity("PhotometricSpot");
        auto& spot = spotEntity.AddComponent<SpotLightComponent>();
        spot.Intensity = 314.1592654f;
        spot.IntensityUnits = 2;
        spot.OuterConeAngle = 60.0f;
        const auto photometricWorld = extractor.Extract(settingsLevel, RenderViewDescription{});
        if (photometricWorld.PointLights.size() != 1 || photometricWorld.SpotLights.size() != 1 ||
            std::abs(photometricWorld.PointLights[0].Intensity - 100.0f) > 0.001f ||
            std::abs(photometricWorld.SpotLights[0].Intensity - 100.0f) > 0.001f)
            throw std::runtime_error("Light extraction did not submit canonical candela for both light types");
        Entity environmentEntity = settingsLevel.CreateEntity("Environment");
        EnvironmentLightComponent& environment =
            environmentEntity.AddComponent<EnvironmentLightComponent>();
        environment.ExposureEV100 = 0.7369656f;
        environment.SkyExposureCompensation = 2.0f;
        environment.SkyColorTint = glm::vec3(1.14f, 1.02f, 0.99f);
        environment.bFogEnabled = true;
        environment.FogDirectionalColor = glm::vec3(0.25f, 0.25f, 0.125f);
        environment.FogDirectionalIntensity = 1.5f;
        environment.FogDirectionalExponent = 4.0f;
        environment.SecondaryFogColor = glm::vec3(0.06f, 0.13f, 0.34f);
        environment.SecondaryFogDensity = 0.008f;
        environment.SecondaryFogStartDistance = 0.0f;
        environment.SecondaryFogBaseHeight = -2.0f;
        environment.SecondaryFogHeightFalloff = 0.003f;
        Json encodedEnvironment;
        Serializer::Serialize(encodedEnvironment, environment);
        EnvironmentLightComponent restoredEnvironment;
        Serializer::Deserialize(encodedEnvironment, restoredEnvironment);
        if (std::abs(restoredEnvironment.SkyExposureCompensation - 2.0f) > 1e-5f ||
            glm::length(restoredEnvironment.SkyColorTint - environment.SkyColorTint) > 1e-5f ||
            glm::length(restoredEnvironment.FogDirectionalColor - environment.FogDirectionalColor) > 1e-5f ||
            std::abs(restoredEnvironment.FogDirectionalIntensity - 1.5f) > 1e-5f ||
            std::abs(restoredEnvironment.FogDirectionalExponent - 4.0f) > 1e-5f ||
            glm::length(restoredEnvironment.SecondaryFogColor - environment.SecondaryFogColor) > 1e-5f ||
            std::abs(restoredEnvironment.SecondaryFogDensity - 0.008f) > 1e-5f ||
            std::abs(restoredEnvironment.SecondaryFogBaseHeight + 2.0f) > 1e-5f ||
            std::abs(restoredEnvironment.SecondaryFogHeightFalloff - 0.003f) > 1e-5f)
            throw std::runtime_error("Reflected sky display exposure did not round trip");
        Entity globalPostEntity = settingsLevel.CreateEntity("GlobalPost");
        PostProcessingComponent& globalPost =
            globalPostEntity.AddComponent<PostProcessingComponent>();
        globalPost.ExposureEV100 = -0.2630344f;
        globalPost.bOverrideBloomIntensity = true;
        globalPost.BloomIntensity = 0.6f;
        globalPost.bOverrideContrast = true;
        globalPost.Contrast = 1.5f;
        globalPost.bOverrideSaturation = true;
        globalPost.Saturation = 0.7f;
        globalPost.bOverrideVignetteIntensity = true;
        globalPost.VignetteIntensity = 0.6f;
        globalPost.bOverrideColorGain = true;
        globalPost.ColorGain = { 1.05f, 1.0f, 0.95f };
        globalPost.bOverrideColorOffset = true;
        globalPost.ColorOffset = { 0.0f, 0.002f, 0.005f };
        Json encodedPost;
        Serializer::Serialize(encodedPost, globalPost);
        PostProcessingComponent restoredPost;
        Serializer::Deserialize(encodedPost, restoredPost);
        if (!restoredPost.bOverrideBloomIntensity || !restoredPost.bOverrideContrast || !restoredPost.bOverrideSaturation ||
            !restoredPost.bOverrideVignetteIntensity || !restoredPost.bOverrideColorGain ||
            !restoredPost.bOverrideColorOffset ||
            std::abs(restoredPost.BloomIntensity - 0.6f) > 1e-5f || std::abs(restoredPost.Contrast - 1.5f) > 1e-5f ||
            std::abs(restoredPost.Saturation - 0.7f) > 1e-5f ||
            std::abs(restoredPost.VignetteIntensity - 0.6f) > 1e-5f ||
            glm::length(restoredPost.ColorGain - globalPost.ColorGain) > 1e-5f ||
            glm::length(restoredPost.ColorOffset - globalPost.ColorOffset) > 1e-5f)
            throw std::runtime_error("Reflected continuous Volume settings did not round trip");
        PostProcessingComponent oldPost;
        Serializer::Deserialize(Json::object(), oldPost);
        if (oldPost.bOverrideBloomIntensity || oldPost.bOverrideContrast || oldPost.bOverrideSaturation ||
            oldPost.bOverrideVignetteIntensity || oldPost.bOverrideColorGain ||
            oldPost.bOverrideColorOffset || !oldPost.bOverrideExposure)
            throw std::runtime_error("Legacy Volume migration changed override defaults");
        Entity legacyInactiveEntity = settingsLevel.CreateEntity("LegacyInactivePost");
        PostProcessingComponent& legacyInactive =
            legacyInactiveEntity.AddComponent<PostProcessingComponent>();
        legacyInactive.bGlobal = false;
        legacyInactive.ExposureEV100 = -3.0f;
        Entity directionalEntity = settingsLevel.CreateEntity("Sun");
        DirectionalLightComponent& directional =
            directionalEntity.AddComponent<DirectionalLightComponent>();
        directional.bCastShadows = true;
        DirectionalLightComponent legacyDirectional;
        Serializer::Deserialize(Json::object(), legacyDirectional);
        if (glm::length(legacyDirectional.LocalDirection - glm::vec3(0.0f, 1.0f, 0.0f)) > 1e-5f)
            throw std::runtime_error("Old scenes must retain the native +Y directional light axis");
        directional.LocalDirection = { 0.0f, 0.0f, -1.0f };
        Json encodedDirectional;
        Serializer::Serialize(encodedDirectional, directional);
        DirectionalLightComponent restoredDirectional;
        Serializer::Deserialize(encodedDirectional, restoredDirectional);
        if (glm::length(restoredDirectional.LocalDirection - directional.LocalDirection) > 1e-5f)
            throw std::runtime_error("Directional light local axis did not survive reflection serialization");

        RenderViewDescription settingsView;
        settingsView.ProjectPostProcess.Exposure = 0.3f;
        settingsView.bShadowsEnabled = false;
        RenderWorldExtractor settingsExtractor;
        const RenderWorld settingsWorld = settingsExtractor.Extract(
            settingsLevel,
            settingsView);
        if (std::abs(settingsWorld.PostProcess.BloomIntensity - 0.6f) > 1e-5f ||
            std::abs(settingsWorld.PostProcess.Contrast - 1.5f) > 1e-5f ||
            std::abs(settingsWorld.PostProcess.Saturation - 0.7f) > 1e-5f ||
            std::abs(settingsWorld.PostProcess.VignetteIntensity - 0.6f) > 1e-5f ||
            glm::length(settingsWorld.PostProcess.ColorGain - globalPost.ColorGain) > 1e-5f ||
            glm::length(settingsWorld.PostProcess.ColorOffset - globalPost.ColorOffset) > 1e-5f ||
            settingsWorld.PostProcess.BloomBaseWeight != 0.0f ||
            std::abs(settingsWorld.PostProcess.Exposure - 1.2f) > 1e-5f ||
            std::abs(settingsWorld.Environment.SkyExposureCompensation - 2.0f) > 1e-5f ||
            glm::length(settingsWorld.Environment.SkyColorTint - environment.SkyColorTint) > 1e-5f ||
            glm::length(settingsWorld.Environment.FogDirectionalColor - environment.FogDirectionalColor) > 1e-5f ||
            std::abs(settingsWorld.Environment.FogDirectionalIntensity - 1.5f) > 1e-5f ||
            std::abs(settingsWorld.Environment.FogDirectionalExponent - 4.0f) > 1e-5f ||
            glm::length(settingsWorld.Environment.SecondaryFogColor - environment.SecondaryFogColor) > 1e-5f ||
            std::abs(settingsWorld.Environment.SecondaryFogDensity - 0.008f) > 1e-5f ||
            std::abs(settingsWorld.Environment.SecondaryFogBaseHeight + 2.0f) > 1e-5f ||
            std::abs(settingsWorld.Environment.SecondaryFogHeightFalloff - 0.003f) > 1e-5f ||
            std::abs(settingsWorld.Environment.IblIntensity - environment.IBLIntensity) > 1e-5f ||
            settingsWorld.DirectionalLights.size() != 1 ||
            glm::length(settingsWorld.DirectionalLights.front().Direction - glm::vec3(0.0f, 0.0f, -1.0f)) > 1e-5f ||
            !settingsWorld.DirectionalLights.front().bCastShadows)
        {
            throw std::runtime_error(
                "RenderWorld extraction did not preserve post-process behavior or original per-light shadow flags");
        }
        settingsView.CameraExposureOverride = 1.7f;
        const RenderWorld cameraOverrideWorld = settingsExtractor.Extract(
            settingsLevel,
            settingsView);
        if (std::abs(cameraOverrideWorld.PostProcess.Exposure - 1.7f) > 1e-5f)
        {
            throw std::runtime_error(
                "Camera exposure did not override project, world, and volume exposure");
        }

        Level secondLevel;
        RenderSystem3D firstRenderSystem(&level);
        RenderSystem3D secondRenderSystem(&secondLevel);
        if (firstRenderSystem.GetSceneGeneration() == 0 ||
            secondRenderSystem.GetSceneGeneration() == 0 ||
            firstRenderSystem.GetSceneGeneration() ==
                secondRenderSystem.GetSceneGeneration())
        {
            throw std::runtime_error(
                "Distinct Level render systems did not receive distinct scene generations");
        }
        const RenderMeshSection& customSection = object.Sections[2];
        if (customSection.GetMaterialInstance().AlphaMode != RenderMaterialAlphaMode::Mask ||
            customSection.AlphaMode != customSection.GetMaterialInstance().AlphaMode ||
            customSection.GetMaterialInstance().AlphaCutoff != 0.37f ||
            customSection.AlphaCutoff != customSection.GetMaterialInstance().AlphaCutoff ||
            !customSection.GetMaterialInstance().bDoubleSided ||
            customSection.bDoubleSided != customSection.GetMaterialInstance().bDoubleSided ||
            !customSection.GetMaterialInstance().bDepthWrite)
        {
            throw std::runtime_error(
                "RenderWorld extraction did not preserve one immutable material render-state snapshot");
        }

        entity.GetComponent<MeshComponent>().bRuntimeBlackSilhouette = true;
        const RenderWorld reloadedSilhouetteWorld = extractor.Extract(level, {});
        const RenderMeshSection::MaterialInstanceSnapshot& silhouette =
            findOriginal(reloadedSilhouetteWorld).Sections.front().GetMaterialInstance();
        if (silhouette.BaseColor != glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) ||
            silhouette.Emissive != glm::vec3(0.0f) || silhouette.Metallic != 1.0f ||
            silhouette.Roughness != 1.0f)
        {
            throw std::runtime_error(
                "Runtime black silhouette did not override the extracted material snapshot");
        }
    }

    void RenderWorldSubmissionContract()
    {
        // The reference is a fixed world-space direction at infinity projected
        // independently into each camera, including its raster jitter.
        const glm::mat4 skyProjection = glm::perspectiveRH_ZO(0.9f, 16.0f / 9.0f, 0.1f, 200.0f);
        const glm::vec2 skySize(853, 480), firstJitter(0.25f, -0.4f), nextJitter(-0.3f, 0.2f);
        const glm::mat4 firstView = glm::lookAtRH(glm::vec3(0, 1, 8), glm::vec3(0, 1, 0), glm::vec3(0, 1, 0));
        const glm::mat4 translatedView = glm::lookAtRH(glm::vec3(4, 2, 9), glm::vec3(4, 2, 1), glm::vec3(0, 1, 0));
        const glm::mat4 rotatedView = glm::rotate(glm::mat4(1), 0.2f, glm::vec3(0, 1, 0)) * translatedView;
        auto skyClip = [&](const glm::mat4& view, glm::vec2 jitter, glm::vec3 direction) {
            glm::vec4 clip = skyProjection * view * glm::vec4(direction, 0);
            clip.x += 2.0f * jitter.x / skySize.x * clip.w;
            clip.y -= 2.0f * jitter.y / skySize.y * clip.w;
            return clip / clip.w;
        };
        auto assertSkyProjection = [&](const glm::mat4& reprojection, const glm::mat4& currentView,
            glm::vec2 currentJitter, const glm::mat4& previousView, glm::vec2 previousJitter) {
            for (glm::vec3 ray : {glm::vec3(0, 0, -1), glm::vec3(0.4f, 0.2f, -1), glm::vec3(-0.3f, -0.1f, -1)})
            {
                const glm::vec4 current = skyClip(currentView, currentJitter, ray);
                const glm::vec4 expected = skyClip(previousView, previousJitter, ray);
                glm::vec4 actual = reprojection * glm::vec4(current.x, current.y, 1, 1);
                actual /= actual.w;
                if (glm::length(glm::vec2(actual - expected)) > 1e-5f)
                    throw std::runtime_error("Sky motion differs from projected world direction");
            }
        };
        SubmittedSkyMotionHistory skyHistory;
        const auto initialSky = skyHistory.Stage(firstView, skyProjection, firstJitter, skySize, false);
        assertSkyProjection(initialSky, firstView, firstJitter, firstView, firstJitter);
        skyHistory.Commit();
        assertSkyProjection(skyHistory.Stage(translatedView, skyProjection, nextJitter, skySize, false),
            translatedView, nextJitter, firstView, firstJitter);
        // The unsubmitted translated frame must not replace the previous camera.
        assertSkyProjection(skyHistory.Stage(rotatedView, skyProjection, nextJitter, skySize, false),
            rotatedView, nextJitter, firstView, firstJitter);
        skyHistory.Commit();
        assertSkyProjection(skyHistory.Stage(firstView, skyProjection, firstJitter, skySize, false),
            firstView, firstJitter, rotatedView, nextJitter);
        assertSkyProjection(skyHistory.Stage(firstView, skyProjection, firstJitter, skySize, true),
            firstView, firstJitter, firstView, firstJitter);
        skyHistory.Reset();
        assertSkyProjection(skyHistory.Stage(rotatedView, skyProjection, nextJitter, skySize, false),
            rotatedView, nextJitter, rotatedView, nextJitter);

        BakedIrradianceVolumeComponent bakedComponent;
        bakedComponent.IrradianceSH.assign(9, glm::vec3(0));
        bakedComponent.IrradianceSH[0] = glm::vec3(2.5f, 1.5f, 0.5f);
        BakedIrradianceVolumeComponent bakedRestored;
        Json bakedJson;
        Serializer::Serialize(bakedJson, bakedComponent);
        Serializer::Deserialize(bakedJson, bakedRestored);
        if (bakedRestored.IrradianceSH != bakedComponent.IrradianceSH)
            throw std::runtime_error("Baked irradiance SH did not survive reflected serialization");
        MeshComponent encodedMesh;
        encodedMesh.StaticLightmapEncoding = 1;
        Serializer::Serialize(bakedJson, encodedMesh);
        MeshComponent restoredMesh;
        Serializer::Deserialize(bakedJson, restoredMesh);
        if (restoredMesh.StaticLightmapEncoding != 1)
            throw std::runtime_error("Native lightmap encoding was lost in scene serialization");
        MeshComponent legacyMesh;
        Serializer::Deserialize(Json::object(), legacyMesh);
        ReflectionProbeComponent legacyProbe;
        Serializer::Deserialize(Json::object(), legacyProbe);
        if (legacyMesh.StaticLightmapEncoding != 0 || !legacyProbe.bAffectDiffuse)
            throw std::runtime_error("Legacy baked lighting defaults changed");
        RenderBakedIrradianceVolume goodVolume;
        goodVolume.EntityId = 7;
        goodVolume.IrradianceSH[0] = bakedComponent.IrradianceSH[0];
        auto badVolume = goodVolume;
        badVolume.EntityId = 2;
        badVolume.IrradianceSH[3].x = std::numeric_limits<float>::quiet_NaN();
        auto secondVolume = goodVolume;
        secondVolume.EntityId = 3;
        secondVolume.Intensity = 2;
        std::array volumes{goodVolume, badVolume, secondVolume};
        const auto bakedGpu = PrepareBakedIrradiance(volumes, glm::vec3(0));
        if (bakedGpu.Count.x != 2 || bakedGpu.Volumes[0].BoundsMaxIntensity.w != 2 ||
            glm::vec3(bakedGpu.Volumes[1].SH[0]) != goodVolume.IrradianceSH[0])
            throw std::runtime_error("Baked irradiance failed invalid-data rejection/stable selection");
        badVolume = goodVolume;
        badVolume.BoundsMax = badVolume.BoundsMin;
        if (IsValidBakedIrradiance(badVolume))
            throw std::runtime_error("Degenerate baked irradiance volume accepted");
        RenderWorld clearedWorld;
        clearedWorld.BakedIrradianceVolumes.push_back(goodVolume);
        clearedWorld.ClearFrameData();
        if (!clearedWorld.BakedIrradianceVolumes.empty())
            throw std::runtime_error("Baked lighting leaked between scenes");

        // A detailed character must retain bones above the former 100-row
        // boundary, without aliasing the next object's current or past pose.
        std::vector<glm::mat4> palette(139, glm::mat4(1.0f));
        std::vector<glm::mat4> past = palette, currentRows, previousRows;
        palette[138][3].x = 3.0f;
        past[138][3].x = 2.0f;
        if (AppendSceneBonePalette(palette, past, currentRows, previousRows, 146) != 0 ||
            currentRows.size() != 139 || currentRows[138][3].x != 3.0f ||
            previousRows[138][3].x != 2.0f)
            throw std::runtime_error("Large character palette/history was truncated");
        std::vector<glm::mat4> secondPalette(7, glm::mat4(1.0f));
        secondPalette[6][3].y = 4.0f;
        if (AppendSceneBonePalette(secondPalette, past, currentRows, previousRows, 146) != 139 ||
            currentRows.size() != 146 || currentRows[145][3].y != 4.0f ||
            previousRows[145] != currentRows[145] || previousRows[138][3].x != 2.0f)
            throw std::runtime_error("Palette offsets or incompatible-history reset failed");
        bool rejected = false;
        try { AppendSceneBonePalette(secondPalette, {}, currentRows, previousRows, 146); }
        catch (const std::length_error&) { rejected = true; }
        if (!rejected || currentRows.size() != 146 || previousRows.size() != 146)
            throw std::runtime_error("Palette overflow partially published data");
        rejected = false;
        try { AppendSceneBonePalette({}, {}, currentRows, previousRows); }
        catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected) throw std::runtime_error("Empty skin palette was silently accepted");
        currentRows.clear(); previousRows.clear();
        AppendSceneBonePalette(palette, {}, currentRows, previousRows);
        if (currentRows != previousRows)
            throw std::runtime_error("Cut/first-frame bone history was not reset");

        SubmittedMotionHistory motion;
        SubmittedMotionHistory secondView;
        auto firstMotion = CreateRef<RenderWorld>();
        firstMotion->SceneGeneration = 7;
        firstMotion->Objects.resize(1);
        auto& firstObject = firstMotion->Objects[0];
        firstObject.EntityId = 5;
        firstObject.ObjectGpuIndex = 3;
        firstObject.MeshAssetPath = "Assets/Test.mesh";
        firstObject.WorldTransform[3].x = 1.0f;
        firstObject.BonePalette = {glm::mat4(1.0f)};
        motion.Stage(firstMotion);
        if (motion.Previous(*firstMotion, firstObject, false))
            throw std::runtime_error("Unsubmitted motion became visible");
        motion.Commit();
        auto skipped = CreateRef<RenderWorld>(*firstMotion);
        skipped->Objects[0].WorldTransform[3].x = 2.0f;
        skipped->Objects[0].BonePalette[0][3].y = 2.0f;
        motion.Stage(skipped);
        motion.Abort();
        auto latest = CreateRef<RenderWorld>(*skipped);
        latest->Objects[0].WorldTransform[3].x = 3.0f;
        const auto* previous = motion.Previous(*latest, latest->Objects[0], false);
        if (!previous || previous->WorldTransform[3].x != 1.0f ||
            previous->BonePalette[0][3].y != 0.0f)
            throw std::runtime_error("Discarded snapshot advanced object/bone history");
        if (secondView.Previous(*latest, latest->Objects[0], false))
            throw std::runtime_error("Motion history leaked between views");
        motion.Stage(latest);
        motion.Commit();
        previous = motion.Previous(*latest, latest->Objects[0], false);
        if (!previous || previous->WorldTransform != latest->Objects[0].WorldTransform ||
            previous->BonePalette != latest->Objects[0].BonePalette)
            throw std::runtime_error("Repeated snapshot retained stale object/bone motion");
        auto replaced = CreateRef<RenderWorld>(*latest);
        replaced->Objects[0].MeshAssetPath = "Assets/Replacement.mesh";
        if (motion.Previous(*replaced, replaced->Objects[0], false) ||
            motion.Previous(*latest, latest->Objects[0], true))
            throw std::runtime_error("Replacement/cut reused incompatible motion");
        replaced->SceneGeneration = 8;
        if (motion.HasSubmittedScene(8) || motion.Previous(*replaced, replaced->Objects[0], false))
            throw std::runtime_error("Reload reused previous scene motion");
        motion.Reset();
        if (motion.HasSubmittedScene(7))
            throw std::runtime_error("Motion reset retained submitted scene");
        SubmittedDeformationHistory deformation, otherDeformationView;
        std::vector<SkinnedVertex> simulated(1), uploaded;
        simulated[0].Pos = glm::vec3(1.0f);
        simulated[0].PreviousPosition = glm::vec3(777.0f);
        const auto stageDeformation = [&](uint64_t revision, uint64_t generation, bool reset = false) {
            deformation.Stage(std::span<const SkinnedVertex>(simulated), uploaded, revision, generation, reset);
        };
        stageDeformation(1, 0);
        if (uploaded[0].PreviousPosition != simulated[0].Pos ||
            simulated[0].PreviousPosition != glm::vec3(777.0f))
            throw std::runtime_error("First deformation upload used simulation history or mutated the asset");
        deformation.Commit();
        simulated[0].Pos = glm::vec3(4.0f); // Three simulation updates before another render.
        stageDeformation(4, 0);
        if (uploaded[0].PreviousPosition != glm::vec3(1.0f))
            throw std::runtime_error("Skipped simulation steps replaced submitted deformation history");
        deformation.Abort();
        simulated[0].Pos = glm::vec3(8.0f);
        stageDeformation(8, 0);
        if (uploaded[0].PreviousPosition != glm::vec3(1.0f))
            throw std::runtime_error("Aborted deformation upload advanced history");
        deformation.Commit();
        if (!deformation.NeedsUpload(8, 0))
            throw std::runtime_error("Repeated rendering would retain old deformation velocity");
        stageDeformation(8, 0);
        if (uploaded[0].PreviousPosition != glm::vec3(8.0f))
            throw std::runtime_error("Repeated rendering did not settle deformation velocity");
        deformation.Commit();
        if (deformation.NeedsUpload(8, 0))
            throw std::runtime_error("Settled deformation requires redundant uploads");
        otherDeformationView.Stage(std::span<const SkinnedVertex>(simulated), uploaded, 8, 0, false);
        otherDeformationView.Commit();
        simulated[0].Pos = glm::vec3(9.0f);
        stageDeformation(9, 0); deformation.Commit();
        simulated[0].Pos = glm::vec3(12.0f);
        otherDeformationView.Stage(std::span<const SkinnedVertex>(simulated), uploaded, 12, 0, false);
        if (uploaded[0].PreviousPosition != glm::vec3(8.0f))
            throw std::runtime_error("Views shared deformation submission history");
        stageDeformation(12, 1);
        if (uploaded[0].PreviousPosition != simulated[0].Pos)
            throw std::runtime_error("Simulation reset reused deformation history");
        deformation.Commit();
        simulated[0].Pos = glm::vec3(20.0f);
        stageDeformation(20, 1, true);
        if (uploaded[0].PreviousPosition != simulated[0].Pos)
            throw std::runtime_error("Camera cut reused deformation history");
        deformation.Commit();
        simulated.resize(2);
        simulated[1].Pos = glm::vec3(30.0f);
        stageDeformation(21, 1);
        if (uploaded[1].PreviousPosition != simulated[1].Pos)
            throw std::runtime_error("Topology change reused incompatible deformation history");
        SubMesh resetMesh(std::vector<StaticVertex>(1), std::vector<uint32_t>{});
        resetMesh.BeginDeformation(); resetMesh.EndDeformation(true);
        resetMesh.UpdatePureMorphWeights({}, true);
        const auto resetGeneration = resetMesh.GetDeformationHistoryGeneration();
        resetMesh.UpdatePureMorphWeights({}, true);
        if (resetMesh.GetDeformationHistoryGeneration() != resetGeneration + 1)
            throw std::runtime_error("Settled morph reset was not published to lagging views");
		PostProcessResolveInput localInput;
		localInput.ProjectDefaults.Exposure = 1.0f;
		localInput.CameraPosition = { 1.5f, 0.0f, 0.0f };
		PostProcessVolumeSnapshot localVolume;
		localVolume.StableEntityId = "local";
		localVolume.bGlobal = false;
		localVolume.bLocalVolumeEnabled = true;
		localVolume.WorldTransform = glm::scale(
			glm::mat4(1.0f),
			glm::vec3(2.0f));
		localVolume.BlendRadius = 1.0f;
		localVolume.Settings.Exposure = 3.0f;
		localInput.Volumes.push_back(localVolume);
		const RenderPostProcessSettings localResult =
			ResolvePostProcessSettings(localInput);
		if (std::abs(
				ComputePostProcessVolumeInfluence(
					localVolume,
					localInput.CameraPosition) - 0.5f) > 1e-5f ||
			std::abs(localResult.Exposure - 2.0f) > 1e-5f)
		{
			throw std::runtime_error(
				"Local post-process box did not apply its transform and blend radius");
		}

        PostProcessResolveInput continuousInput = localInput;
        continuousInput.ProjectDefaults.BloomIntensity = 0.2f;
        continuousInput.ProjectDefaults.Contrast = 1.0f;
        continuousInput.ProjectDefaults.Saturation = 0.2f;
        continuousInput.ProjectDefaults.Temperature = 0.0f;
        continuousInput.ProjectDefaults.VignetteIntensity = 0.2f;
        continuousInput.ProjectDefaults.ColorGain = { 1.0f, 1.0f, 1.0f };
        continuousInput.ProjectDefaults.ColorOffset = { 0.0f, 0.0f, 0.0f };
        auto& continuousVolume = continuousInput.Volumes.front();
        continuousVolume.bOverrideExposure = false;
        continuousVolume.bOverrideBloomIntensity = true;
        continuousVolume.bOverrideContrast = true;
        continuousVolume.bOverrideSaturation = true;
        continuousVolume.bOverrideTemperature = true;
        continuousVolume.bOverrideVignetteIntensity = true;
        continuousVolume.bOverrideColorGain = true;
        continuousVolume.bOverrideColorOffset = true;
        continuousVolume.Settings.BloomIntensity = 1.0f;
        continuousVolume.Settings.Contrast = 2.0f;
        continuousVolume.Settings.Saturation = 1.0f;
        continuousVolume.Settings.Temperature = -0.4f;
        continuousVolume.Settings.VignetteIntensity = 0.8f;
        continuousVolume.Settings.ColorGain = { 1.2f, 0.8f, 1.0f };
        continuousVolume.Settings.ColorOffset = { 0.02f, 0.0f, -0.02f };
        const auto continuous = ResolvePostProcessSettings(continuousInput);
        if (std::abs(continuous.BloomIntensity - 0.6f) > 1e-5f || std::abs(continuous.Contrast - 1.5f) > 1e-5f ||
            std::abs(continuous.Saturation - 0.6f) > 1e-5f ||
            std::abs(continuous.Temperature + 0.2f) > 1e-5f ||
            std::abs(continuous.VignetteIntensity - 0.5f) > 1e-5f ||
            glm::length(continuous.ColorGain - glm::vec3(1.1f, 0.9f, 1.0f)) > 1e-5f ||
            glm::length(continuous.ColorOffset - glm::vec3(0.01f, 0.0f, -0.01f)) > 1e-5f ||
            continuous.Exposure != continuousInput.ProjectDefaults.Exposure)
            throw std::runtime_error("Continuous Volume parameters did not independently use spatial influence");
        auto secondBaseInput = continuousInput;
        secondBaseInput.ProjectDefaults.BloomIntensity = 0.8f;
        secondBaseInput.ProjectDefaults.Contrast = 0.4f;
        secondBaseInput.ProjectDefaults.Saturation = 1.4f;
        secondBaseInput.ProjectDefaults.Temperature = 0.6f;
        secondBaseInput.ProjectDefaults.VignetteIntensity = 0.4f;
        secondBaseInput.ProjectDefaults.ColorGain = { 0.7f, 1.1f, 1.3f };
        secondBaseInput.ProjectDefaults.ColorOffset = { -0.01f, 0.02f, 0.03f };
        const auto secondBase = ResolvePostProcessSettings(secondBaseInput);
        if (std::abs(secondBase.BloomIntensity - (0.8f * continuous.BloomBaseWeight + continuous.BloomContribution)) > 1e-5f ||
            std::abs(secondBase.Contrast - (0.4f * continuous.ContrastBaseWeight + continuous.ContrastContribution)) > 1e-5f ||
            std::abs(secondBase.Saturation - (1.4f * continuous.SaturationBaseWeight + continuous.SaturationContribution)) > 1e-5f ||
            std::abs(secondBase.Temperature - (0.6f * continuous.TemperatureBaseWeight + continuous.TemperatureContribution)) > 1e-5f ||
            std::abs(secondBase.VignetteIntensity - (0.4f * continuous.VignetteBaseWeight + continuous.VignetteContribution)) > 1e-5f ||
            glm::length(secondBase.ColorGain - (secondBaseInput.ProjectDefaults.ColorGain *
                continuous.ColorGainBaseWeight + continuous.ColorGainContribution)) > 1e-5f ||
            glm::length(secondBase.ColorOffset - (secondBaseInput.ProjectDefaults.ColorOffset *
                continuous.ColorOffsetBaseWeight + continuous.ColorOffsetContribution)) > 1e-5f)
            throw std::runtime_error("Volume blend could not be rebased for a different view Profile");
        continuousVolume.bOverrideContrast = false;
        continuousVolume.Settings.BloomIntensity = std::numeric_limits<float>::quiet_NaN();
        continuousVolume.Settings.Temperature = std::numeric_limits<float>::quiet_NaN();
        continuousVolume.Settings.ColorGain.x = std::numeric_limits<float>::quiet_NaN();
        const auto ignored = ResolvePostProcessSettings(continuousInput);
        if (ignored.BloomIntensity != continuousInput.ProjectDefaults.BloomIntensity || ignored.BloomBaseWeight != 1.0f ||
            ignored.BloomContribution != 0.0f || ignored.Contrast != continuousInput.ProjectDefaults.Contrast ||
            ignored.Temperature != continuousInput.ProjectDefaults.Temperature ||
            ignored.TemperatureContribution != 0.0f ||
            glm::length(ignored.ColorGain - continuousInput.ProjectDefaults.ColorGain) > 1e-5f ||
            ignored.ColorGainBaseWeight != 1.0f)
            throw std::runtime_error("Disabled or non-finite Volume parameter contaminated the view base");

		PostProcessResolveInput sortedInput;
		sortedInput.ProjectDefaults.Exposure = 0.5f;
		sortedInput.EnvironmentExposureOverride = 0.75f;
		PostProcessVolumeSnapshot stableA;
		stableA.StableEntityId = "a";
		stableA.Priority = 5.0f;
		stableA.BlendWeight = 0.5f;
		stableA.Settings.Exposure = 2.0f;
		PostProcessVolumeSnapshot stableB = stableA;
		stableB.StableEntityId = "b";
		stableB.Settings.Exposure = 4.0f;
		PostProcessVolumeSnapshot legacyInactive = stableA;
		legacyInactive.StableEntityId = "legacy-disabled-local";
		legacyInactive.Priority = 100.0f;
		legacyInactive.bGlobal = false;
		legacyInactive.bLocalVolumeEnabled = false;
		legacyInactive.BlendWeight = 1.0f;
		legacyInactive.Settings.Exposure = 9.0f;
		sortedInput.Volumes = { stableB, legacyInactive, stableA };
		const RenderPostProcessSettings sortedResult =
			ResolvePostProcessSettings(sortedInput);
		std::reverse(sortedInput.Volumes.begin(), sortedInput.Volumes.end());
		const RenderPostProcessSettings reversedResult =
			ResolvePostProcessSettings(sortedInput);
		if (std::abs(sortedResult.Exposure - 2.6875f) > 1e-5f ||
			std::abs(reversedResult.Exposure - sortedResult.Exposure) > 1e-5f)
		{
			throw std::runtime_error(
				"Post-process volumes did not use priority and stable entity ID ordering");
		}
		sortedInput.CameraExposureOverride = 1.25f;
		if (std::abs(
				ResolvePostProcessSettings(sortedInput).Exposure - 1.25f) > 1e-5f)
		{
			throw std::runtime_error(
				"Post-process resolver did not apply camera override last");
		}

		Level emptyLevel;
		RenderViewDescription fixedTimeView;
		fixedTimeView.MaterialTimeSeconds = 1.25f;
		RenderWorldExtractor fixedTimeExtractor;
		const RenderWorld fixedTimeWorld = fixedTimeExtractor.Extract(
			emptyLevel,
			fixedTimeView);
		if (fixedTimeWorld.TimeSeconds != *fixedTimeView.MaterialTimeSeconds)
		{
			throw std::runtime_error(
				"RenderWorld extraction ignored the explicit material-time override");
		}

        RenderWorldSubmission submission;
        if (submission.HasSubmission() || submission.AcquireLatest())
            throw std::runtime_error("An empty RenderWorld submission queue reported data");

        RenderWorld first;
        first.FrameNumber = 7;
        first.Camera.Position = { 1.0f, 2.0f, 3.0f };
        first.Objects.push_back(RenderObject{ .EntityId = 42 });
        submission.Submit(std::move(first));

        const Ref<const RenderWorld> snapshot = submission.AcquireLatest();
        if (!snapshot || snapshot->FrameNumber != 7 || snapshot->Objects.size() != 1 ||
            snapshot->Camera.Position != glm::vec3(1.0f, 2.0f, 3.0f))
        {
            throw std::runtime_error("RenderWorld submission did not preserve its owning snapshot");
        }

        RenderWorld second;
        second.FrameNumber = 8;
        second.Objects.push_back(RenderObject{ .EntityId = 99 });
        submission.Submit(std::move(second));
        if (snapshot->Objects.front().EntityId != 42 ||
            submission.AcquireLatest()->Objects.front().EntityId != 99)
        {
            throw std::runtime_error("A new RenderWorld submission mutated an acquired snapshot");
        }

        bool bRejectedBackwardFrame = false;
        try
        {
            RenderWorld stale;
            stale.FrameNumber = 6;
            submission.Submit(std::move(stale));
        }
        catch (const std::logic_error&)
        {
            bRejectedBackwardFrame = true;
        }
        if (!bRejectedBackwardFrame)
            throw std::runtime_error("RenderWorld submission accepted a backward frame number");

        RenderMeshSection::MaterialInstanceSnapshot materialSnapshot;
        materialSnapshot.ShaderPath = "Shaders/Materials/Test.hlsl";
        materialSnapshot.ExpectedRecursiveSourceHash = "recursive";
        materialSnapshot.ExpectedBindingSignature = "bindings";
        materialSnapshot.ExpectedMaterialAbiVersion = 1;
        materialSnapshot.ExpectedArtifactIdentityHash = "artifacts";
        const MaterialShaderProgramIdentity programIdentity{
            .SourcePath = "SHADERS\\MATERIALS\\TEST.HLSL",
            .RecursiveContentHash = "recursive",
            .MaterialAbiVersion = 1,
            .BindingSignatureHash = "bindings",
            .ArtifactIdentityHash = "artifacts",
        };
        if (!IsMaterialSnapshotProgramCompatible(materialSnapshot, programIdentity, 42))
        {
            throw std::runtime_error(
                "Cold material snapshot did not accept an otherwise exact program identity");
        }
        materialSnapshot.ExpectedProgramGeneration = 42;
        if (!IsMaterialSnapshotProgramCompatible(materialSnapshot, programIdentity, 42) ||
            !IsMaterialSnapshotProgramCompatible(materialSnapshot, programIdentity, 1))
        {
            throw std::runtime_error(
                "Manager-local generation incorrectly rejected an exact program identity after renderer rebuild");
        }
        const auto requireIdentityMismatch = [&](MaterialShaderProgramIdentity mismatched)
        {
            if (IsMaterialSnapshotProgramCompatible(materialSnapshot, mismatched, 42))
            {
                throw std::runtime_error(
                    "Material snapshot accepted an incomplete program identity comparison");
            }
        };
        auto mismatchedIdentity = programIdentity;
        mismatchedIdentity.SourcePath = "Shaders/Materials/Other.hlsl";
        requireIdentityMismatch(mismatchedIdentity);
        mismatchedIdentity = programIdentity;
        mismatchedIdentity.RecursiveContentHash = "changed";
        requireIdentityMismatch(mismatchedIdentity);
        mismatchedIdentity = programIdentity;
        mismatchedIdentity.MaterialAbiVersion = 2;
        requireIdentityMismatch(mismatchedIdentity);
        mismatchedIdentity = programIdentity;
        mismatchedIdentity.BindingSignatureHash = "changed";
        requireIdentityMismatch(mismatchedIdentity);
        mismatchedIdentity = programIdentity;
        mismatchedIdentity.ArtifactIdentityHash = "changed";
        requireIdentityMismatch(mismatchedIdentity);

        MaterialShaderManifest renderState;
        renderState.BlendMode = MaterialShaderBlendMode::Mask;
        renderState.CullMode = MaterialShaderCullMode::None;
        renderState.bDepthWrite = true;
        materialSnapshot.AlphaMode = RenderMaterialAlphaMode::Mask;
        materialSnapshot.bDoubleSided = true;
        materialSnapshot.bDepthWrite = true;
        if (!IsMaterialSnapshotRenderStateCompatible(materialSnapshot, renderState))
            throw std::runtime_error("Exact material snapshot render state was rejected");
        materialSnapshot.bDoubleSided = false;
        if (IsMaterialSnapshotRenderStateCompatible(materialSnapshot, renderState))
        {
            throw std::runtime_error(
                "Material snapshot accepted a CullMode/sidedness mismatch");
        }
        materialSnapshot.bDoubleSided = true;
        materialSnapshot.bDepthWrite = false;
        if (IsMaterialSnapshotRenderStateCompatible(materialSnapshot, renderState))
        {
            throw std::runtime_error(
                "Material snapshot accepted a depth-write mismatch");
        }

        submission.Reset();
        if (submission.HasSubmission() || submission.GetLatestFrameNumber() != 0)
            throw std::runtime_error("RenderWorld submission reset retained frame state");

        glm::mat4 transparentWorld(1.0f);
        transparentWorld[3] = glm::vec4(0.0f, 0.0f, -10.0f, 1.0f);
        const float nearSectionDepth = ComputeTransparentSubMeshSortDepth(
            glm::mat4(1.0f),
            transparentWorld,
            true,
            glm::vec3(-1.0f, -1.0f, 3.0f),
            glm::vec3(1.0f, 1.0f, 5.0f));
        const float farSectionDepth = ComputeTransparentSubMeshSortDepth(
            glm::mat4(1.0f),
            transparentWorld,
            true,
            glm::vec3(-1.0f, -1.0f, -5.0f),
            glm::vec3(1.0f, 1.0f, -3.0f));
        const float originFallbackDepth = ComputeTransparentSubMeshSortDepth(
            glm::mat4(1.0f),
            transparentWorld,
            false,
            glm::vec3(500.0f),
            glm::vec3(700.0f));
        if (std::abs(nearSectionDepth - 6.0f) > 1e-5f ||
            std::abs(farSectionDepth - 14.0f) > 1e-5f ||
            std::abs(originFallbackDepth - 10.0f) > 1e-5f)
        {
            throw std::runtime_error(
                "Transparent sort depth did not use each SubMesh local bounds center");
        }

        std::array<TransparentDrawSortKey, 6> transparentKeys{
            TransparentDrawSortKey{ .Depth = 10.0f, .EntityId = 2,
                .SectionIndex = 1, .SubmissionOrder = 5 },
            TransparentDrawSortKey{ .Depth = 10.0f, .EntityId = 1,
                .SectionIndex = 9, .SubmissionOrder = 4 },
            TransparentDrawSortKey{ .Depth = 12.0f, .EntityId = 8,
                .SectionIndex = 0, .SubmissionOrder = 3 },
            TransparentDrawSortKey{ .Depth = 10.0f, .EntityId = 2,
                .SectionIndex = 0, .SubmissionOrder = 2 },
            TransparentDrawSortKey{ .Depth = 10.0f, .EntityId = 2,
                .SectionIndex = 1, .SubmissionOrder = 7 },
            TransparentDrawSortKey{ .Depth = 10.0f, .EntityId = 2,
                .SectionIndex = 1, .SubmissionOrder = 1 },
        };
        std::sort(
            transparentKeys.begin(),
            transparentKeys.end(),
            IsTransparentDrawSortKeyBefore);
        if (transparentKeys[0].Depth != 12.0f ||
            transparentKeys[1].EntityId != 1 ||
            transparentKeys[2].SectionIndex != 0 ||
            transparentKeys[3].SubmissionOrder != 1 ||
            transparentKeys[4].SubmissionOrder != 5 ||
            transparentKeys[5].SubmissionOrder != 7)
        {
            throw std::runtime_error(
                "Equal-depth transparent draws did not receive a deterministic total order");
        }
    }
}
