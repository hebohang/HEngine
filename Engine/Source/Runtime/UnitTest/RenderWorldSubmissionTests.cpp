#include "Hepch.h"

#include "Runtime/UnitTest/RenderWorldSubmissionTests.h"

#include "Runtime/EcsFramework/Component/Light/DirectionalLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/PostProcess/PostProcessingComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/System/Render/RenderWorldExtractor.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/Renderer/SceneRenderer.h"
#include "Runtime/Graphics/Scene/PostProcessResolver.h"
#include "Runtime/Graphics/Scene/RenderWorldSubmission.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace HEngine::Tests
{
    void RenderWorldExtractorMeshPathContract()
    {
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

        const RenderObject& object = world.Objects.front();
        if (object.Path != RenderPath::CpuSkinned ||
            object.Sections[0].Path != RenderPath::GpuDrivenOpaque ||
            object.Sections[1].Path != RenderPath::CpuSkinned ||
            object.Sections[2].Path != RenderPath::CpuOpaque ||
            !object.Sections[2].MaterialInstance.bCustomSurface ||
            object.Sections[2].MaterialInstance.ShaderPath != "Shaders/TestSurface.hlsl")
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

        Level settingsLevel;
        Entity environmentEntity = settingsLevel.CreateEntity("Environment");
        EnvironmentLightComponent& environment =
            environmentEntity.AddComponent<EnvironmentLightComponent>();
        environment.Exposure = 0.6f;
        Entity globalPostEntity = settingsLevel.CreateEntity("GlobalPost");
        PostProcessingComponent& globalPost =
            globalPostEntity.AddComponent<PostProcessingComponent>();
        globalPost.Exposure = 1.2f;
        Entity legacyInactiveEntity = settingsLevel.CreateEntity("LegacyInactivePost");
        PostProcessingComponent& legacyInactive =
            legacyInactiveEntity.AddComponent<PostProcessingComponent>();
        legacyInactive.bGlobal = false;
        legacyInactive.Exposure = 8.0f;
        Entity directionalEntity = settingsLevel.CreateEntity("Sun");
        DirectionalLightComponent& directional =
            directionalEntity.AddComponent<DirectionalLightComponent>();
        directional.bCastShadows = true;

        RenderViewDescription settingsView;
        settingsView.ProjectPostProcess.Exposure = 0.3f;
        settingsView.bShadowsEnabled = false;
        RenderWorldExtractor settingsExtractor;
        const RenderWorld settingsWorld = settingsExtractor.Extract(
            settingsLevel,
            settingsView);
        if (std::abs(settingsWorld.PostProcess.Exposure - 1.2f) > 1e-5f ||
            settingsWorld.DirectionalLights.size() != 1 ||
            settingsWorld.DirectionalLights.front().bCastShadows)
        {
            throw std::runtime_error(
                "RenderWorld extraction did not preserve legacy post-process behavior or mask project-disabled shadows");
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
        if (customSection.MaterialInstance.AlphaMode != RenderMaterialAlphaMode::Mask ||
            customSection.AlphaMode != customSection.MaterialInstance.AlphaMode ||
            customSection.MaterialInstance.AlphaCutoff != 0.37f ||
            customSection.AlphaCutoff != customSection.MaterialInstance.AlphaCutoff ||
            !customSection.MaterialInstance.bDoubleSided ||
            customSection.bDoubleSided != customSection.MaterialInstance.bDoubleSided ||
            !customSection.MaterialInstance.bDepthWrite)
        {
            throw std::runtime_error(
                "RenderWorld extraction did not preserve one immutable material render-state snapshot");
        }

        meshComponent.bRuntimeBlackSilhouette = true;
        const RenderWorld silhouetteWorld = extractor.Extract(level, {});
        const RenderMeshSection::MaterialInstanceSnapshot& silhouette =
            silhouetteWorld.Objects.front().Sections.front().MaterialInstance;
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
