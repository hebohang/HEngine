#include "Hepch.h"

#include "RenderWorldExtractor.h"

#include "Runtime/Core/PresentationClock.h"
#include "Runtime/EcsFramework/Component/Light/DirectionalLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/PointLightComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/PostProcess/PostProcessingComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/Scene/PostProcessResolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace HEngine
{
    namespace
    {
        static bool IsCollapsed(const TransformComponent& transform)
        {
            constexpr float CollapsedScaleEpsilonSquared = 1e-12f;
            return glm::dot(transform.Scale, transform.Scale) <= CollapsedScaleEpsilonSquared;
        }

        static RenderBounds ComputeWorldBounds(const Mesh& mesh, const glm::mat4& transform)
        {
            glm::vec3 localMinimum(std::numeric_limits<float>::max());
            glm::vec3 localMaximum(std::numeric_limits<float>::lowest());
            bool bHasBounds = false;

            for (const Ref<SubMesh>& subMesh : mesh.mSubMeshes)
            {
                if (!subMesh || !subMesh->HasBounds())
                    continue;
                localMinimum = glm::min(localMinimum, subMesh->GetBoundsMin());
                localMaximum = glm::max(localMaximum, subMesh->GetBoundsMax());
                bHasBounds = true;
            }

            if (!bHasBounds)
                return {};

            const std::array<glm::vec3, 8> corners = {{
                { localMinimum.x, localMinimum.y, localMinimum.z },
                { localMaximum.x, localMinimum.y, localMinimum.z },
                { localMinimum.x, localMaximum.y, localMinimum.z },
                { localMaximum.x, localMaximum.y, localMinimum.z },
                { localMinimum.x, localMinimum.y, localMaximum.z },
                { localMaximum.x, localMinimum.y, localMaximum.z },
                { localMinimum.x, localMaximum.y, localMaximum.z },
                { localMaximum.x, localMaximum.y, localMaximum.z },
            }};

            glm::vec3 worldMinimum(std::numeric_limits<float>::max());
            glm::vec3 worldMaximum(std::numeric_limits<float>::lowest());
            for (const glm::vec3& corner : corners)
            {
                const glm::vec3 worldCorner = glm::vec3(transform * glm::vec4(corner, 1.0f));
                worldMinimum = glm::min(worldMinimum, worldCorner);
                worldMaximum = glm::max(worldMaximum, worldCorner);
            }

            RenderBounds bounds;
            bounds.Center = (worldMinimum + worldMaximum) * 0.5f;
            bounds.Extents = (worldMaximum - worldMinimum) * 0.5f;
            bounds.Radius = glm::length(bounds.Extents);
            bounds.bValid = true;
            return bounds;
        }

        static bool HasSkinnedSections(const Mesh& mesh)
        {
            return std::any_of(
                mesh.mSubMeshes.begin(),
                mesh.mSubMeshes.end(),
                [](const Ref<SubMesh>& subMesh)
                {
                    return subMesh && subMesh->IsSkinned();
                });
        }
    }

    RenderWorld RenderWorldExtractor::Extract(Level& level, const RenderViewDescription& view)
    {
        // Material GPU indices are frame-local snapshot identities. Recycling
        // them prevents long-running Editor asset reload churn from exhausting
        // the fixed GPU material table.
        mMaterialIndices.clear();
        mNextMaterialIndex = 0;
        RenderWorld world;
        world.FrameNumber = view.FrameNumber;
        world.SceneGeneration = std::max(view.SceneGeneration, 1ull);
        world.TimeSeconds = view.MaterialTimeSeconds.value_or(
			PresentationClock::GetInstance().GetElapsedSeconds());
        world.bEditorView = view.bEditorView;
        world.Camera.View = view.View;
        world.Camera.Projection = view.Projection;
        world.Camera.ViewProjection = view.Projection * view.View;
        world.Camera.PreviousViewProjection = bHasPreviousView
            ? mPreviousViewProjection
            : world.Camera.ViewProjection;
        world.Camera.NonJitteredViewProjection = world.Camera.ViewProjection;
        world.Camera.PreviousNonJitteredViewProjection =
            world.Camera.PreviousViewProjection;
        world.Camera.Position = view.CameraPosition;
        world.Camera.NearPlane = view.NearPlane;
        world.Camera.FarPlane = view.FarPlane;
        world.Camera.JitterSequenceGeneration =
            std::max(view.JitterSequenceGeneration, 1ull);
        world.Camera.ProjectionGeneration =
            std::max(view.ProjectionGeneration, 1ull);
        world.Camera.bCameraCut = view.bCameraCut;

        PostProcessResolveInput postProcessInput;
        postProcessInput.ProjectDefaults = view.ProjectPostProcess;
        postProcessInput.CameraPosition = view.CameraPosition;
        postProcessInput.CameraExposureOverride =
            view.CameraExposureOverride;

        std::unordered_set<int> liveEntities;
        auto meshView = level.mRegistry.view<TransformComponent, MeshComponent>();
        world.Objects.reserve(meshView.size_hint());
        for (auto entityHandle : meshView)
        {
            Entity entity(entityHandle, &level);
            const int entityId = static_cast<int>(static_cast<uint32_t>(entity));
            liveEntities.insert(entityId);

#if WITH_EDITOR
            if (view.bEditorView && !level.IsEntityVisibleInEditor(entity))
                continue;
#endif

            const TransformComponent& transform = meshView.get<TransformComponent>(entityHandle);
            const MeshComponent& meshComponent = meshView.get<MeshComponent>(entityHandle);
            if (!meshComponent.mMesh || IsCollapsed(transform))
                continue;

            RenderObject object;
            object.EntityId = entityId;
            object.MeshResource = meshComponent.mMesh;
            object.MeshAssetPath = meshComponent.Path;
            object.WorldTransform = transform.ParentUuid.empty()
                ? transform.GetTransform()
                : level.GetWorldTransform(entity);
            const auto previousTransform = mPreviousTransforms.find(entityId);
            object.PreviousWorldTransform = previousTransform != mPreviousTransforms.end()
                ? previousTransform->second
                : object.WorldTransform;
            object.Bounds = ComputeWorldBounds(*object.MeshResource, object.WorldTransform);
            object.ObjectGpuIndex = GetOrCreateObjectIndex(entityId);
            object.MeshGpuIndex = GetOrCreateMeshIndex(object.MeshResource.get());
            object.bSelected = view.bEditorView && entityId == view.SelectedEntity;
            // Static opaque meshes use the validated compute-cull + indexed
            // indirect path. Unsupported categories keep an explicit CPU path.
            const bool bHasSkinnedSections = HasSkinnedSections(*object.MeshResource);
            object.Path = bHasSkinnedSections
                ? RenderPath::CpuSkinned
                : RenderPath::GpuDrivenOpaque;
            if (bHasSkinnedSections)
            {
                const std::vector<glm::mat4>& palette =
                    object.MeshResource->bUseExternalBoneMatrices
                    ? object.MeshResource->mExternalBoneMatrices
                    : object.MeshResource->mAnimator.GetFinalBoneMatrices();
                object.BonePalette = palette;
            }

            object.Sections.reserve(object.MeshResource->mSubMeshes.size());
            for (const Ref<SubMesh>& subMesh : object.MeshResource->mSubMeshes)
            {
                RenderMeshSection section;
                section.SubMeshResource = subMesh;
                if (subMesh && subMesh->mMaterialIndex < object.MeshResource->mMaterial.size())
                    section.MaterialResource = object.MeshResource->mMaterial[subMesh->mMaterialIndex];
                if (section.MaterialResource)
                {
                    section.MaterialGpuIndex = meshComponent.bRuntimeBlackSilhouette
                        ? mNextMaterialIndex++
                        : GetOrCreateMaterialIndex(section.MaterialResource.get());
                    section.MaterialInstance =
                        section.MaterialResource->BuildRenderSnapshot();
                    if (meshComponent.bRuntimeBlackSilhouette)
                    {
                        section.MaterialInstance.BaseColor = glm::vec4(
                            0.0f,
                            0.0f,
                            0.0f,
                            section.MaterialInstance.BaseColor.a);
                        section.MaterialInstance.Emissive = glm::vec3(0.0f);
                        section.MaterialInstance.Metallic = 1.0f;
                        section.MaterialInstance.Roughness = 1.0f;
                    }
                    section.AlphaCutoff = section.MaterialInstance.AlphaCutoff;
                    section.bDoubleSided = section.MaterialInstance.bDoubleSided;
                    section.AlphaMode = section.MaterialInstance.AlphaMode;
                }
                const bool bSkinnedSection = subMesh && subMesh->IsSkinned();
                section.Path = bSkinnedSection
                    ? section.AlphaMode == RenderMaterialAlphaMode::Blend
                        ? RenderPath::CpuSkinnedTransparent
                        : RenderPath::CpuSkinned
                    : section.AlphaMode == RenderMaterialAlphaMode::Blend
                        ? RenderPath::Transparent
                        : section.MaterialInstance.bCustomSurface
                            ? RenderPath::CpuOpaque
                            : RenderPath::GpuDrivenOpaque;
                object.Sections.push_back(std::move(section));
            }

            mPreviousTransforms[entityId] = object.WorldTransform;
            world.Objects.push_back(std::move(object));
        }

        for (auto iterator = mPreviousTransforms.begin(); iterator != mPreviousTransforms.end();)
        {
            if (!liveEntities.contains(iterator->first))
                iterator = mPreviousTransforms.erase(iterator);
            else
                ++iterator;
        }

        auto pointLightView = level.mRegistry.view<TransformComponent, PointLightComponent>();
        world.PointLights.reserve(pointLightView.size_hint());
        for (auto entityHandle : pointLightView)
        {
            Entity entity(entityHandle, &level);
#if WITH_EDITOR
            if (view.bEditorView && !level.IsEntityVisibleInEditor(entity))
                continue;
#endif
            const PointLightComponent& source = pointLightView.get<PointLightComponent>(entityHandle);
            world.PointLights.push_back({
                .Position = level.GetWorldTranslation(entity),
                .Intensity = source.Intensity,
                .Color = source.LightColor,
                .Radius = source.Radius,
                .FalloffExponent = std::isfinite(source.FalloffExponent)
                    ? std::clamp(source.FalloffExponent, 0.0f, 8.0f)
                    : 2.0f,
            });
        }

        auto directionalLightView = level.mRegistry.view<TransformComponent, DirectionalLightComponent>();
        world.DirectionalLights.reserve(directionalLightView.size_hint());
        for (auto entityHandle : directionalLightView)
        {
            Entity entity(entityHandle, &level);
#if WITH_EDITOR
            if (view.bEditorView && !level.IsEntityVisibleInEditor(entity))
                continue;
#endif
            const DirectionalLightComponent& source = directionalLightView.get<DirectionalLightComponent>(entityHandle);
            glm::vec3 direction = glm::vec3(
                level.GetWorldTransform(entity) * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
            if (glm::dot(direction, direction) <= 1e-6f)
                direction = { 0.0f, -1.0f, 0.0f };
            else
                direction = glm::normalize(direction);

            world.DirectionalLights.push_back({
                .Direction = direction,
                .Intensity = source.Intensity,
                .Color = source.Color,
                .bCastShadows = source.bCastShadows && view.bShadowsEnabled,
                .CascadeCount = source.CascadeCount,
                .ShadowMapSize = source.ShadowMapSize,
                .ShadowDistance = source.ShadowDistance,
                .CascadeLambda = source.CascadeLambda,
                .DepthBias = source.DepthBias,
                .NormalBias = source.NormalBias,
                .PcfRadius = source.PCFRadius,
                .ShadowStrength = source.ShadowStrength,
                .bUsePcf = source.bUsePCF,
            });
        }

        auto environmentView = level.mRegistry.view<EnvironmentLightComponent>();
        for (auto entityHandle : environmentView)
        {
            Entity entity(entityHandle, &level);
#if WITH_EDITOR
            if (view.bEditorView && !level.IsEntityVisibleInEditor(entity))
                continue;
#endif
            const EnvironmentLightComponent& source = environmentView.get<EnvironmentLightComponent>(entityHandle);
            world.Environment.HdrTexturePath = source.HdrTexturePath;
            world.Environment.Exposure = source.Exposure;
            world.Environment.IblIntensity = source.IBLIntensity;
            postProcessInput.EnvironmentExposureOverride = source.Exposure;
            break;
        }

        auto postProcessView = level.mRegistry.view<PostProcessingComponent>();
        for (auto entityHandle : postProcessView)
        {
            Entity entity(entityHandle, &level);
#if WITH_EDITOR
            if (view.bEditorView && !level.IsEntityVisibleInEditor(entity))
                continue;
#endif
            const PostProcessingComponent& source = postProcessView.get<PostProcessingComponent>(entityHandle);
            const bool bHasTransform = entity.HasComponent<TransformComponent>();
            postProcessInput.Volumes.push_back({
                .StableEntityId = entity.UUID().str(),
                .WorldTransform = bHasTransform
                    ? level.GetWorldTransform(entity)
                    : glm::mat4(1.0f),
                .Settings = { .Exposure = source.Exposure },
                .Priority = source.Priority,
                .BlendRadius = source.BlendRadius,
                .BlendWeight = source.BlendWeight,
                .bEnabled = source.bEnabled,
                .bGlobal = source.bGlobal,
                .bLocalVolumeEnabled =
                    source.bLocalVolumeEnabled && bHasTransform,
                .bOverrideExposure = source.bOverrideExposure,
            });
        }
        world.PostProcess = ResolvePostProcessSettings(postProcessInput);

        mPreviousViewProjection = world.Camera.ViewProjection;
        bHasPreviousView = true;
        return world;
    }

    void RenderWorldExtractor::Reset()
    {
        mPreviousTransforms.clear();
        mObjectIndices.clear();
        mMeshIndices.clear();
        mMaterialIndices.clear();
        mPreviousViewProjection = glm::mat4(1.0f);
        mNextObjectIndex = 0;
        mNextMeshIndex = 0;
        mNextMaterialIndex = 0;
        bHasPreviousView = false;
    }

    uint32_t RenderWorldExtractor::GetOrCreateObjectIndex(int entityId)
    {
        const auto [iterator, bInserted] = mObjectIndices.try_emplace(entityId, mNextObjectIndex);
        if (bInserted)
            ++mNextObjectIndex;
        return iterator->second;
    }

    uint32_t RenderWorldExtractor::GetOrCreateMeshIndex(const Mesh* mesh)
    {
        const auto [iterator, bInserted] = mMeshIndices.try_emplace(mesh, mNextMeshIndex);
        if (bInserted)
            ++mNextMeshIndex;
        return iterator->second;
    }

    uint32_t RenderWorldExtractor::GetOrCreateMaterialIndex(const Material* material)
    {
        const auto [iterator, bInserted] = mMaterialIndices.try_emplace(material, mNextMaterialIndex);
        if (bInserted)
            ++mNextMaterialIndex;
        return iterator->second;
    }
}
