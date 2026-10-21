#include "Hepch.h"

#include "RenderWorldExtractor.h"

#include "Runtime/Core/PresentationClock.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/RenderSettingsComponent.h"
#include "Runtime/EcsFramework/Component/Light/DirectionalLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/ReflectionProbeComponent.h"
#include "Runtime/EcsFramework/Component/Light/BakedIrradianceVolumeComponent.h"
#include "Runtime/Graphics/Scene/BakedIrradiance.h"
#include "Runtime/EcsFramework/Component/Light/PointLightComponent.h"
#include "Runtime/EcsFramework/Component/Light/SpotLightComponent.h"
#include "Runtime/Graphics/Lighting/PhotometricUnits.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/PostProcess/PostProcessingComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/System/Movement/CharacterPresentation.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/Scene/PostProcessResolver.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unordered_map>
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

        static RenderBounds ComputeWorldBounds(const Mesh& mesh, const glm::mat4& transform,
            std::unordered_map<const Mesh*, RenderBounds>& localBounds)
        {
            auto [iterator, inserted] = localBounds.try_emplace(&mesh);
            if (inserted)
            {
                glm::vec3 localMinimum(std::numeric_limits<float>::max());
                glm::vec3 localMaximum(std::numeric_limits<float>::lowest());
                for (const Ref<SubMesh>& subMesh : mesh.mSubMeshes)
                {
                    if (!subMesh || !subMesh->HasBounds())
                        continue;
                    localMinimum = glm::min(localMinimum, subMesh->GetBoundsMin());
                    localMaximum = glm::max(localMaximum, subMesh->GetBoundsMax());
                    iterator->second.bValid = true;
                }
                if (iterator->second.bValid)
                {
                    iterator->second.Center = (localMinimum + localMaximum) * 0.5f;
                    iterator->second.Extents = (localMaximum - localMinimum) * 0.5f;
                }
            }
            const RenderBounds& local = iterator->second;
            if (!local.bValid)
                return {};
            RenderBounds bounds;
            bounds.Center = glm::vec3(transform * glm::vec4(local.Center, 1.0f));
            const glm::mat3 linear(transform);
            bounds.Extents = glm::abs(linear[0]) * local.Extents.x +
                glm::abs(linear[1]) * local.Extents.y +
                glm::abs(linear[2]) * local.Extents.z;
            bounds.Radius = glm::length(bounds.Extents);
            bounds.bValid = true;
            return bounds;
        }

        static RenderBounds ExpandSkinnedWorldBounds(const RenderBounds& base,
            const RenderBounds& local, const glm::mat4& worldTransform,
            const std::vector<glm::mat4>& bonePalette)
        {
            if (!base.bValid || !local.bValid) return {};
            glm::vec3 minimum = base.Center - base.Extents;
            glm::vec3 maximum = base.Center + base.Extents;
            // A skinned vertex is a convex blend of bone-transformed local
            // positions. The union of each bone's transformed local AABB also
            // contains morph/cloth vertices reflected in the current bounds.
            for (const glm::mat4& bone : bonePalette)
            {
                const glm::mat4 transform = worldTransform * bone;
                const glm::vec3 center(transform * glm::vec4(local.Center, 1.0f));
                const glm::mat3 linear(transform);
                const glm::vec3 extents = glm::abs(linear[0]) * local.Extents.x +
                    glm::abs(linear[1]) * local.Extents.y +
                    glm::abs(linear[2]) * local.Extents.z;
                const auto finite = [](const glm::vec3& value)
                {
                    return std::isfinite(value.x) && std::isfinite(value.y) &&
                        std::isfinite(value.z);
                };
                if (!finite(center) || !finite(extents)) return {};
                minimum = glm::min(minimum, center - extents);
                maximum = glm::max(maximum, center + extents);
            }
            RenderBounds result;
            result.Center = (minimum + maximum) * 0.5f;
            result.Extents = (maximum - minimum) * 0.5f;
            result.Radius = glm::length(result.Extents);
            result.bValid = true;
            return result;
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

        static std::array<glm::vec4, 4> SideFrustumPlanes(const glm::mat4& viewProjection)
        {
            const glm::vec4 row0(viewProjection[0][0], viewProjection[1][0],
                viewProjection[2][0], viewProjection[3][0]);
            const glm::vec4 row1(viewProjection[0][1], viewProjection[1][1],
                viewProjection[2][1], viewProjection[3][1]);
            const glm::vec4 row3(viewProjection[0][3], viewProjection[1][3],
                viewProjection[2][3], viewProjection[3][3]);
            std::array<glm::vec4, 4> planes{ row3 + row0, row3 - row0,
                row3 + row1, row3 - row1 };
            for (glm::vec4& plane : planes)
            {
                const float length = glm::length(glm::vec3(plane));
                if (std::isfinite(length) && length > 1e-6f)
                    plane /= length;
                else
                    plane = glm::vec4(0.0f);
            }
            return planes;
        }

        static bool IntersectsPaddedView(const RenderBounds& bounds,
            const std::array<glm::vec4, 4>& planes)
        {
            if (!bounds.bValid)
                return true;
            // Keep nearby offscreen casters for directional and local shadows.
            // Far-plane rejection remains on the GPU so large outdoor views do
            // not lose distant silhouettes merely because the camera turns.
            constexpr float shadowCasterPadding = 12.0f;
            for (const glm::vec4& plane : planes)
                if (glm::dot(glm::vec3(plane), bounds.Center) + plane.w <
                    -bounds.Radius - shadowCasterPadding)
                    return false;
            return true;
        }
    }

    namespace
    {
        void ExtractRenderOverrides(Level& level, const RenderViewDescription& view, RenderWorld& world)
        {
            if (!view.WorldRenderOverrides.is_object() || !view.CameraRenderOverrides.is_object())
                throw std::invalid_argument("Explicit World/Camera render overrides must be JSON objects.");
            Json sceneOverrides = Json::object();
            size_t sceneSettingsCount = 0;
            for (const auto entity : level.mRegistry.view<RenderSettingsComponent>())
            {
                if (++sceneSettingsCount > 1)
                    throw std::invalid_argument("A scene may contain only one RenderSettingsComponent.");
                sceneOverrides = level.mRegistry.get<RenderSettingsComponent>(entity).Overrides;
                if (!sceneOverrides.is_object())
                    throw std::invalid_argument("RenderSettingsComponent.Overrides must be a JSON object.");
            }
            Json cameraOverrides = Json::object();
            if (view.CameraEntityId != -1)
            {
                const auto entity = static_cast<entt::entity>(view.CameraEntityId);
                if (!level.mRegistry.valid(entity) || !level.mRegistry.all_of<CameraComponent>(entity))
                    throw std::invalid_argument("Render view references a missing CameraComponent.");
                cameraOverrides = level.mRegistry.get<CameraComponent>(entity).RenderOverrides;
                if (!cameraOverrides.is_object())
                    throw std::invalid_argument("CameraComponent.RenderOverrides must be a JSON object.");
            }
            sceneOverrides.update(view.WorldRenderOverrides);
            cameraOverrides.update(view.CameraRenderOverrides);
            world.RenderOverrides = std::move(sceneOverrides);
            world.Camera.RenderOverrides = std::move(cameraOverrides);
        }
    }

    RenderWorld RenderWorldExtractor::Extract(Level& level, const RenderViewDescription& view)
    {
        static const bool bTimeExtract = [] {
            const char* setting = std::getenv("HE_RUNTIME_SYSTEM_TIMINGS");
            return setting && (setting[0] == '1' || setting[0] == '2') &&
                setting[1] == '\0';
        }();
        static const bool bSampleExtract = [] {
            const char* setting = std::getenv("HE_RUNTIME_SYSTEM_TIMINGS");
            return setting && setting[0] == '2' && setting[1] == '\0';
        }();
        const auto extractStart = bTimeExtract ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        // Reject configuration before changing temporal/material extraction history.
        RenderWorld world;
        ExtractRenderOverrides(level, view, world);
        const uint64_t sceneGeneration = std::max<uint64_t>(view.SceneGeneration, 1);
        const bool bNewSceneGeneration = mPreviousSceneGeneration != sceneGeneration;
        if (bNewSceneGeneration)
        {
            mMaterialIndices.clear();
            mNextMaterialIndex = 0;
            mCachedLocalTransforms.clear();
            mCachedStaticLightmapAssets.clear();
            bStaticLightmapAssetsDirty = true;
        }
        if (view.bCameraCut || bNewSceneGeneration)
        {
            mPreviousTransforms.clear();
            mPreviousBonePalettes.clear();
            mPreviousBoneMeshes.clear();
            bHasPreviousView = false;
        }
        mPreviousSceneGeneration = sceneGeneration;
        // Expired ownership keys can be removed without reusing their IDs. A
        // stable GPU Scene may still retire an older snapshot asynchronously.
        std::erase_if(mMaterialIndices, [](const auto& entry) { return entry.first.expired(); });
        std::unordered_map<const Material*, std::array<Ref<const RenderMeshSection::MaterialInstanceSnapshot>, 2>> materialSnapshots;
        world.FrameNumber = view.FrameNumber;
        world.SceneGeneration = std::max<uint64_t>(view.SceneGeneration, 1);
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
            std::max<uint64_t>(view.JitterSequenceGeneration, 1);
        world.Camera.ProjectionGeneration =
            std::max<uint64_t>(view.ProjectionGeneration, 1);
        world.Camera.bCameraCut = view.bCameraCut;

        PostProcessResolveInput postProcessInput;
        postProcessInput.ProjectDefaults = view.ProjectPostProcess;
        postProcessInput.CameraPosition = view.CameraPosition;
        postProcessInput.CameraExposureOverride =
            view.CameraExposureOverride;

        std::unordered_set<int> submittedEntities;
        std::unordered_map<const Mesh*, RenderBounds> localBounds;
        localBounds.reserve(128);
        const auto sideFrustumPlanes = SideFrustumPlanes(world.Camera.ViewProjection);
        auto meshView = level.mRegistry.view<TransformComponent, MeshComponent>();
        // Siblings share the same parent transform throughout this extraction.
        // Keep the cache local to the frame so animated parents remain current.
        std::unordered_map<std::string, glm::mat4> parentWorldTransforms;
        parentWorldTransforms.reserve(meshView.size_hint() / 4);
        // Siblings inherit the same nearest character's interpolation offset.
        // Cache only within this extraction: reparenting and movement history
        // must be observed on the next frame, including a previously absent offset.
        std::unordered_map<std::string, std::optional<glm::vec3>> parentPresentationOffsets;
        parentPresentationOffsets.reserve(meshView.size_hint() / 4);
        submittedEntities.reserve(meshView.size_hint() / 4 + 64);
        world.Objects.reserve(meshView.size_hint());
        mCachedLocalTransforms.reserve(meshView.size_hint());
        size_t cacheIndex = 0;
        double candidateMilliseconds = 0.0;
        double submissionMilliseconds = 0.0;
        static const std::string emptyLightmapPath;
        const auto meshStart = bTimeExtract ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        for (auto entityHandle : meshView)
        {
            const auto candidateStart = bTimeExtract ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            Entity entity(entityHandle, &level);
            const int entityId = static_cast<int>(static_cast<uint32_t>(entity));
            if (cacheIndex == mCachedLocalTransforms.size())
                mCachedLocalTransforms.emplace_back();
            CachedLocalTransform& cached = mCachedLocalTransforms[cacheIndex++];
            const auto updateLightmapPath = [&](const std::string& path)
            {
                if (cached.StaticLightmapPath != path)
                {
                    cached.StaticLightmapPath = path;
                    bStaticLightmapAssetsDirty = true;
                }
            };

#if WITH_EDITOR
            if (view.bEditorView && !level.IsEntityVisibleInEditor(entity))
            {
                updateLightmapPath(emptyLightmapPath);
                if (bTimeExtract)
                    candidateMilliseconds += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - candidateStart).count();
                continue;
            }
#endif

            const TransformComponent& transform = meshView.get<TransformComponent>(entityHandle);
            const MeshComponent& meshComponent = meshView.get<MeshComponent>(entityHandle);
            if (!meshComponent.mMesh || IsCollapsed(transform))
            {
                updateLightmapPath(emptyLightmapPath);
                if (bTimeExtract)
                    candidateMilliseconds += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - candidateStart).count();
                continue;
            }
            updateLightmapPath(meshComponent.StaticLightmapBlend > 0.0f
                ? meshComponent.StaticLightmapPath : emptyLightmapPath);

            const bool bEntityChanged = cached.EntityId != entityId;
            const bool bLocalChanged = bEntityChanged ||
                !glm::all(glm::equal(cached.Translation, transform.Translation)) ||
                !glm::all(glm::equal(cached.Rotation, transform.Rotation)) ||
                !glm::all(glm::equal(cached.Scale, transform.Scale));
            if (bLocalChanged)
            {
                cached.Translation = transform.Translation;
                cached.Rotation = transform.Rotation;
                cached.Scale = transform.Scale;
                cached.Local = transform.GetTransform();
            }
            glm::mat4 worldTransform = cached.Local;
            if (!transform.ParentUuid.empty())
            {
                const auto parent = parentWorldTransforms.find(transform.ParentUuid);
                if (parent != parentWorldTransforms.end())
                    worldTransform = parent->second * cached.Local;
                else
                {
                    const Entity parentEntity = level.GetParent(entity);
                    if (parentEntity)
                    {
                        const glm::mat4 parentWorld = level.GetWorldTransform(parentEntity);
                        parentWorldTransforms.emplace(transform.ParentUuid, parentWorld);
                        worldTransform = parentWorld * cached.Local;
                    }
                }
            }
            bool bTransformChanged = bEntityChanged;
            for (int column = 0; column < 4 && !bTransformChanged; ++column)
                bTransformChanged = !glm::all(glm::equal(
                    cached.World[column], worldTransform[column]));
            cached.EntityId = entityId;
            cached.World = worldTransform;
            std::optional<glm::vec3> presentationOffset;
            if (!view.bEditorView)
            {
                if (entity.HasComponent<CharacterMovementComponent>())
                    presentationOffset = GetCharacterPresentationOffset(level, entity);
                else if (!transform.ParentUuid.empty())
                {
                    auto [offset, bNew] = parentPresentationOffsets.try_emplace(transform.ParentUuid);
                    if (bNew)
                        offset->second = GetCharacterPresentationOffset(level, level.GetParent(entity));
                    presentationOffset = offset->second;
                }
                if (presentationOffset)
                {
                    worldTransform[3] += glm::vec4(*presentationOffset, 0.0f);
                    bTransformChanged = true;
                }
            }
            bTransformChanged |= cached.bHadPresentationOffset != presentationOffset.has_value();
            cached.bHadPresentationOffset = presentationOffset.has_value();
            const bool bHasSkinnedSections = HasSkinnedSections(*meshComponent.mMesh);
            const bool bCacheableBounds = !view.bEditorView && !bHasSkinnedSections;
            uint64_t boundsRevision = 14695981039346656037ull;
            if (bCacheableBounds)
            {
                for (const Ref<SubMesh>& section : meshComponent.mMesh->mSubMeshes)
                {
                    boundsRevision = (boundsRevision ^ static_cast<uint64_t>(
                        reinterpret_cast<std::uintptr_t>(section.get()))) * 1099511628211ull;
                    boundsRevision = (boundsRevision ^ (section ? section->GetDeformationRevision() : 0ull))
                        * 1099511628211ull;
                }
            }
            const RenderBounds bounds = bCacheableBounds && !bTransformChanged &&
                cached.BoundsMesh == meshComponent.mMesh.get() &&
                cached.BoundsRevision == boundsRevision
                ? cached.WorldBounds
                : ComputeWorldBounds(*meshComponent.mMesh, worldTransform, localBounds);
            cached.BoundsMesh = bCacheableBounds ? meshComponent.mMesh.get() : nullptr;
            if (bCacheableBounds)
            {
                cached.BoundsRevision = boundsRevision;
                cached.WorldBounds = bounds;
            }
            const bool bCulled = !bHasSkinnedSections && entityId != view.SelectedEntity &&
                !IntersectsPaddedView(bounds, sideFrustumPlanes);
            const auto submissionStart = bTimeExtract ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            if (bTimeExtract)
                candidateMilliseconds += std::chrono::duration<double, std::milli>(
                    submissionStart - candidateStart).count();
            if (bCulled)
                continue;
            submittedEntities.insert(entityId);
            const auto previousTransform = mPreviousTransforms.find(entityId);
            RenderObject object;
            object.EntityId = entityId;
            object.MeshResource = meshComponent.mMesh;
            object.MeshAssetPath = meshComponent.Path;
            object.StaticLightmapPath = meshComponent.StaticLightmapPath;
            object.StaticLightmapUvScale = meshComponent.StaticLightmapUvScale;
            object.StaticLightmapUvBias = meshComponent.StaticLightmapUvBias;
            object.StaticLightmapScale0 = meshComponent.StaticLightmapScale0;
            object.StaticLightmapAdd0 = meshComponent.StaticLightmapAdd0;
            object.StaticLightmapScale1 = meshComponent.StaticLightmapScale1;
            object.StaticLightmapAdd1 = meshComponent.StaticLightmapAdd1;
            object.StaticLightmapBlend = meshComponent.StaticLightmapBlend;
            object.StaticLightmapEncoding = meshComponent.StaticLightmapEncoding;
            object.WorldTransform = worldTransform;
            object.PreviousWorldTransform = previousTransform != mPreviousTransforms.end()
                ? previousTransform->second : worldTransform;
            object.Bounds = bounds;
            object.ObjectGpuIndex = GetOrCreateObjectIndex(entityId);
            object.MeshGpuIndex = GetOrCreateMeshIndex(object.MeshResource.get());
            object.bSelected = view.bEditorView && entityId == view.SelectedEntity;
            // Static opaque meshes use the validated compute-cull + indexed
            // indirect path. Unsupported categories keep an explicit CPU path.
            object.Path = bHasSkinnedSections
                ? RenderPath::CpuSkinned
                : RenderPath::GpuDrivenOpaque;
            if (bHasSkinnedSections)
            {
                const std::vector<glm::mat4>& palette =
                    object.MeshResource->bUseExternalBoneMatrices
                    ? object.MeshResource->mExternalBoneMatrices
                    : (!object.MeshResource->bPlayAnim && !object.MeshResource->mBindBoneMatrices.empty() && object.MeshResource->mAnimation.GetBoneIDMap().empty()
                        ? object.MeshResource->mBindBoneMatrices
                        : object.MeshResource->mAnimator.GetFinalBoneMatrices());
                // An animation may not have produced its first pose yet when a
                // newly loaded mesh enters the scene. Keep the character
                // visible in bind pose until that first complete palette is
                // available; a nonempty truncated palette is still invalid.
                const std::vector<glm::mat4>& resolvedPalette = palette.empty() &&
                    !object.MeshResource->bUseExternalBoneMatrices &&
                    !object.MeshResource->mAnimator.HasCurrentAnimation() &&
                    !object.MeshResource->mBindBoneMatrices.empty()
                    ? object.MeshResource->mBindBoneMatrices : palette;
                if (resolvedPalette.size() < object.MeshResource->mBindBoneMatrices.size())
                    throw std::invalid_argument("Skinned mesh pose is shorter than its imported bind palette");
                object.BonePalette = resolvedPalette;
                if (!object.BonePalette.empty())
                    object.Bounds = ExpandSkinnedWorldBounds(object.Bounds,
                        localBounds.at(object.MeshResource.get()), object.WorldTransform,
                        object.BonePalette);
                const auto previousPalette = mPreviousBonePalettes.find(entityId);
                const auto previousMesh = mPreviousBoneMeshes.find(entityId);
                object.PreviousBonePalette =
                    previousPalette != mPreviousBonePalettes.end() &&
                    previousMesh != mPreviousBoneMeshes.end() &&
                    previousMesh->second == object.MeshResource.get() &&
                    previousPalette->second.size() == resolvedPalette.size()
                    ? previousPalette->second
                    : resolvedPalette;
                mPreviousBonePalettes[entityId] = resolvedPalette;
                mPreviousBoneMeshes[entityId] = object.MeshResource.get();
            }
            else
            {
                mPreviousBonePalettes.erase(entityId);
                mPreviousBoneMeshes.erase(entityId);
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
                    section.MaterialGpuIndex = GetOrCreateMaterialIndex(
                        section.MaterialResource, meshComponent.bRuntimeBlackSilhouette);
                    const auto [snapshot, bNewSnapshot] = materialSnapshots.try_emplace(section.MaterialResource.get());
                    if (bNewSnapshot)
                        snapshot->second[0] = CreateRef<RenderMeshSection::MaterialInstanceSnapshot>(
                            section.MaterialResource->BuildRenderSnapshot());
                    if (meshComponent.bRuntimeBlackSilhouette && !snapshot->second[1])
                    {
                        auto silhouette = CreateRef<RenderMeshSection::MaterialInstanceSnapshot>(*snapshot->second[0]);
                        silhouette->BaseColor = glm::vec4(
                            0.0f,
                            0.0f,
                            0.0f,
                            silhouette->BaseColor.a);
                        silhouette->Emissive = glm::vec3(0.0f);
                        silhouette->Metallic = 1.0f;
                        silhouette->Roughness = 1.0f;
                        snapshot->second[1] = std::move(silhouette);
                    }
                    section.SharedMaterialInstance = snapshot->second[meshComponent.bRuntimeBlackSilhouette ? 1 : 0];
                    section.AlphaCutoff = section.GetMaterialInstance().AlphaCutoff;
                    section.bDoubleSided = section.GetMaterialInstance().bDoubleSided;
                    section.AlphaMode = section.GetMaterialInstance().AlphaMode;
                }
                const bool bSkinnedSection = subMesh && subMesh->IsSkinned();
                section.Path = bSkinnedSection
                    ? section.AlphaMode == RenderMaterialAlphaMode::Blend
                        ? RenderPath::CpuSkinnedTransparent
                        : RenderPath::CpuSkinned
                    : section.AlphaMode == RenderMaterialAlphaMode::Blend
                        ? RenderPath::Transparent
                        : section.GetMaterialInstance().bCustomSurface
                            ? RenderPath::CpuOpaque
                            : RenderPath::GpuDrivenOpaque;
                object.Sections.push_back(std::move(section));
            }

            mPreviousTransforms[entityId] = object.WorldTransform;
            world.Objects.push_back(std::move(object));
            if (bTimeExtract)
                submissionMilliseconds += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - submissionStart).count();
        }
        bStaticLightmapAssetsDirty |= cacheIndex != mCachedLocalTransforms.size();
        mCachedLocalTransforms.resize(cacheIndex);
        if (bStaticLightmapAssetsDirty)
        {
            mCachedStaticLightmapAssets.clear();
            std::unordered_set<std::string> uniquePaths;
            for (const CachedLocalTransform& cached : mCachedLocalTransforms)
                if (!cached.StaticLightmapPath.empty() &&
                    uniquePaths.insert(cached.StaticLightmapPath).second)
                    mCachedStaticLightmapAssets.push_back(cached.StaticLightmapPath);
            bStaticLightmapAssetsDirty = false;
        }
        world.StaticLightmapAssets = mCachedStaticLightmapAssets;
        const auto meshEnd = bTimeExtract ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};

        for (auto iterator = mPreviousTransforms.begin(); iterator != mPreviousTransforms.end();)
        {
            if (!submittedEntities.contains(iterator->first))
                iterator = mPreviousTransforms.erase(iterator);
            else
                ++iterator;
        }
        for (auto iterator = mPreviousBonePalettes.begin(); iterator != mPreviousBonePalettes.end();)
        {
            if (!submittedEntities.contains(iterator->first))
            {
                mPreviousBoneMeshes.erase(iterator->first);
                iterator = mPreviousBonePalettes.erase(iterator);
            }
            else
                ++iterator;
        }

        auto spotLightView = level.mRegistry.view<TransformComponent, SpotLightComponent>();
        world.SpotLights.reserve(spotLightView.size_hint());
        for (auto entityHandle : spotLightView)
        {
            Entity entity(entityHandle, &level);
#if WITH_EDITOR
            if (view.bEditorView && !level.IsEntityVisibleInEditor(entity))
                continue;
#endif
            const SpotLightComponent& source = spotLightView.get<SpotLightComponent>(entityHandle);
            const glm::mat4 transform = level.GetWorldTransform(entity);
            const glm::vec3 direction = glm::vec3(transform * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
            const float directionLength = glm::length(direction);
            const float outerAngle = std::isfinite(source.OuterConeAngle)
                ? std::clamp(source.OuterConeAngle, 0.1f, 89.0f) : 30.0f;
            const float innerAngle = std::isfinite(source.InnerConeAngle)
                ? std::clamp(source.InnerConeAngle, 0.0f, outerAngle) : outerAngle;
            world.SpotLights.push_back({
                .Position = glm::vec3(transform[3]),
                .Intensity = Photometry::ToCandela(source.Intensity, source.IntensityUnits, outerAngle),
                .Color = source.LightColor,
                .Radius = source.Radius,
                .Direction = std::isfinite(directionLength) && directionLength > 1e-6f
                    ? direction / directionLength : glm::vec3(0.0f, 0.0f, -1.0f),
                .InnerConeCosine = std::cos(glm::radians(innerAngle)),
                .OuterConeCosine = std::cos(glm::radians(outerAngle)),
            });
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
                .Intensity = Photometry::ToCandela(source.Intensity, source.IntensityUnits),
                .Color = source.LightColor,
                .Radius = source.Radius,
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
            glm::vec3 localDirection = source.LocalDirection;
            if (!std::isfinite(localDirection.x) || !std::isfinite(localDirection.y) ||
                !std::isfinite(localDirection.z) ||
                glm::dot(localDirection, localDirection) <= 1e-6f)
                localDirection = { 0.0f, 1.0f, 0.0f };
            glm::vec3 direction = glm::vec3(
                level.GetWorldTransform(entity) * glm::vec4(localDirection, 0.0f));
            if (glm::dot(direction, direction) <= 1e-6f)
                direction = { 0.0f, -1.0f, 0.0f };
            else
                direction = glm::normalize(direction);

            world.DirectionalLights.push_back({
                .Direction = direction,
                .Intensity = Photometry::NonNegative(source.Intensity),
                .Color = source.Color,
                .bCastShadows = source.bCastShadows,
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

        auto probeView = level.mRegistry.view<TransformComponent, ReflectionProbeComponent>();
        for (auto entityHandle : probeView)
        {
            Entity entity(entityHandle, &level);
#if WITH_EDITOR
            if (view.bEditorView && !level.IsEntityVisibleInEditor(entity))
                continue;
#endif
            const auto& source = probeView.get<ReflectionProbeComponent>(entityHandle);
            if (!source.bEnabled || source.HdrTexturePath.empty() || source.Intensity <= 0.0f)
                continue;
            const glm::mat4 transform = level.GetWorldTransform(entity);
            const glm::vec3 extents = glm::max(glm::abs(source.BoxHalfExtents), glm::vec3(0.001f));
            glm::vec3 minimum(std::numeric_limits<float>::max());
            glm::vec3 maximum(-std::numeric_limits<float>::max());
            for (uint32_t corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 signs((corner & 1u) ? 1.0f : -1.0f,
                    (corner & 2u) ? 1.0f : -1.0f, (corner & 4u) ? 1.0f : -1.0f);
                const glm::vec3 worldCorner(transform * glm::vec4(extents * signs, 1.0f));
                minimum = glm::min(minimum, worldCorner);
                maximum = glm::max(maximum, worldCorner);
            }
            world.ReflectionProbes.push_back(RenderReflectionProbe{
                .EntityId = static_cast<int>(entityHandle),
                .HdrTexturePath = source.HdrTexturePath,
                .BoundsMin = minimum, .BoundsMax = maximum,
                .CapturePosition = glm::vec3(transform * glm::vec4(source.CaptureOffset, 1.0f)),
                .BlendDistance = std::max(source.BlendDistance, 0.001f),
                .Intensity = std::max(source.Intensity, 0.0f),
                .bAffectDiffuse = source.bAffectDiffuse,
            });
        }
        // ECS iteration order may change after structural edits. Stable ordering
        // keeps a view's limited probe selection deterministic.
        std::sort(world.ReflectionProbes.begin(), world.ReflectionProbes.end(),
            [](const RenderReflectionProbe& a, const RenderReflectionProbe& b)
            { return a.EntityId < b.EntityId; });

        auto bakedView = level.mRegistry.view<BakedIrradianceVolumeComponent>();
        for (auto entityHandle : bakedView)
        {
            Entity entity(entityHandle, &level);
#if WITH_EDITOR
            if (view.bEditorView && !level.IsEntityVisibleInEditor(entity)) continue;
#endif
            const auto& source = bakedView.get<BakedIrradianceVolumeComponent>(entityHandle);
            if (!source.bEnabled || source.IrradianceSH.size() != 9) continue;
            RenderBakedIrradianceVolume volume;
            volume.EntityId = static_cast<int>(entityHandle);
            volume.BoundsMin = source.BoundsMin;
            volume.BoundsMax = source.BoundsMax;
            volume.BlendDistance = source.BlendDistance;
            volume.Intensity = source.Intensity;
            std::copy(source.IrradianceSH.begin(), source.IrradianceSH.end(), volume.IrradianceSH.begin());
            if (IsValidBakedIrradiance(volume)) world.BakedIrradianceVolumes.push_back(volume);
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
            const float exposure = Photometry::ExposureMultiplier(
                source.ExposureEV100, source.ExposureCompensation);
            world.Environment.Exposure = exposure;
            world.Environment.IblIntensity = source.IBLIntensity;
            world.Environment.SkyExposureCompensation = std::isfinite(source.SkyExposureCompensation)
                ? std::clamp(source.SkyExposureCompensation, -8.0f, 8.0f) : 0.0f;
            world.Environment.SkyColorTint = glm::vec3(
                std::isfinite(source.SkyColorTint.x) ? std::clamp(source.SkyColorTint.x, 0.0f, 4.0f) : 1.0f,
                std::isfinite(source.SkyColorTint.y) ? std::clamp(source.SkyColorTint.y, 0.0f, 4.0f) : 1.0f,
                std::isfinite(source.SkyColorTint.z) ? std::clamp(source.SkyColorTint.z, 0.0f, 4.0f) : 1.0f);
            world.Environment.bFogEnabled = source.bFogEnabled;
            world.Environment.FogColor = glm::max(source.FogColor, glm::vec3(0.0f));
            world.Environment.FogDensity = std::max(source.FogDensity, 0.0f);
            world.Environment.FogStartDistance = std::max(source.FogStartDistance, 0.0f);
            world.Environment.FogBaseHeight = source.FogBaseHeight;
            world.Environment.FogHeightFalloff = std::max(source.FogHeightFalloff, 0.0f);
            world.Environment.SecondaryFogColor = glm::vec3(
                std::isfinite(source.SecondaryFogColor.x) ? std::max(source.SecondaryFogColor.x, 0.0f) : 0.0f,
                std::isfinite(source.SecondaryFogColor.y) ? std::max(source.SecondaryFogColor.y, 0.0f) : 0.0f,
                std::isfinite(source.SecondaryFogColor.z) ? std::max(source.SecondaryFogColor.z, 0.0f) : 0.0f);
            world.Environment.SecondaryFogDensity = std::isfinite(source.SecondaryFogDensity)
                ? std::clamp(source.SecondaryFogDensity, 0.0f, 2.0f) : 0.0f;
            world.Environment.SecondaryFogStartDistance = std::isfinite(source.SecondaryFogStartDistance)
                ? std::clamp(source.SecondaryFogStartDistance, 0.0f, 10000.0f) : 0.0f;
            world.Environment.SecondaryFogBaseHeight = std::isfinite(source.SecondaryFogBaseHeight)
                ? std::clamp(source.SecondaryFogBaseHeight, -10000.0f, 10000.0f) : 0.0f;
            world.Environment.SecondaryFogHeightFalloff = std::isfinite(source.SecondaryFogHeightFalloff)
                ? std::clamp(source.SecondaryFogHeightFalloff, 0.0f, 10.0f) : 0.0f;
            world.Environment.FogDirectionalColor = glm::max(source.FogDirectionalColor, glm::vec3(0.0f));
            world.Environment.FogDirectionalIntensity = std::isfinite(source.FogDirectionalIntensity)
                ? std::clamp(source.FogDirectionalIntensity, 0.0f, 16.0f) : 0.0f;
            world.Environment.FogDirectionalExponent = std::isfinite(source.FogDirectionalExponent)
                ? std::clamp(source.FogDirectionalExponent, 1.0f, 64.0f) : 4.0f;
            world.Clouds.bEnabled = source.bCloudsEnabled;
            world.Clouds.Bottom = source.CloudBottom;
            world.Clouds.Thickness = source.CloudThickness;
            world.Clouds.MaxDistance = source.CloudMaxDistance;
            world.Clouds.Coverage = source.CloudCoverage;
            world.Clouds.Density = source.CloudDensity;
            world.Clouds.Scale = source.CloudScale;
            world.Clouds.Roundness = source.CloudRoundness;
            world.Clouds.Erosion = source.CloudErosion;
            world.Clouds.Seed = source.CloudSeed;
            world.Clouds.WindAngle = source.CloudWindAngle;
            world.Clouds.WindSpeed = source.CloudWindSpeed;
            world.Clouds.EvolutionSpeed = source.CloudEvolutionSpeed;
            world.Clouds.LitColor = source.CloudLitColor;
            world.Clouds.ShadowColor = source.CloudShadowColor;
            world.Clouds.Ambient = source.CloudAmbient;
            world.Clouds.SilverLining = source.CloudSilverLining;
            NormalizeCloudLayer(world.Clouds);
            postProcessInput.EnvironmentExposureOverride = exposure;
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
                .Settings = {
                    .Exposure = Photometry::ExposureMultiplier(source.ExposureEV100, source.ExposureCompensation),
                    .BloomIntensity = source.BloomIntensity,
                    .Contrast = source.Contrast,
                    .Saturation = source.Saturation,
                    .Temperature = source.Temperature,
                    .VignetteIntensity = source.VignetteIntensity,
                    .ColorGain = source.ColorGain,
                    .ColorOffset = source.ColorOffset,
                },
                .Priority = source.Priority,
                .BlendRadius = source.BlendRadius,
                .BlendWeight = source.BlendWeight,
                .bEnabled = source.bEnabled,
                .bGlobal = source.bGlobal,
                .bLocalVolumeEnabled =
                    source.bLocalVolumeEnabled && bHasTransform,
                .bOverrideExposure = source.bOverrideExposure,
                .bOverrideBloomIntensity = source.bOverrideBloomIntensity,
                .bOverrideContrast = source.bOverrideContrast,
                .bOverrideSaturation = source.bOverrideSaturation,
                .bOverrideTemperature = source.bOverrideTemperature,
                .bOverrideVignetteIntensity = source.bOverrideVignetteIntensity,
                .bOverrideColorGain = source.bOverrideColorGain,
                .bOverrideColorOffset = source.bOverrideColorOffset,
            });
        }
        world.PostProcess = ResolvePostProcessSettings(postProcessInput);

        if (bTimeExtract)
        {
            const auto extractEnd = std::chrono::steady_clock::now();
            const double totalMilliseconds = std::chrono::duration<double, std::milli>(
                extractEnd - extractStart).count();
            if (totalMilliseconds > 11.0 || (bSampleExtract && view.FrameNumber % 60 == 0))
                HE_CORE_WARN("[RenderWorldExtractTiming] frame={} totalMs={} setupMs={} meshMs={} restMs={} candidateMs={} submissionMs={} meshEntities={} visibleObjects={} materialSnapshots={}",
                    view.FrameNumber, totalMilliseconds,
                    std::chrono::duration<double, std::milli>(meshStart - extractStart).count(),
                    std::chrono::duration<double, std::milli>(meshEnd - meshStart).count(),
                    std::chrono::duration<double, std::milli>(extractEnd - meshEnd).count(),
                    candidateMilliseconds, submissionMilliseconds,
                    meshView.size_hint(), world.Objects.size(), materialSnapshots.size());
        }

        mPreviousViewProjection = world.Camera.ViewProjection;
        bHasPreviousView = true;
        return world;
    }

    Ref<const RenderObject> RenderWorldExtractor::BuildResourceWarmupObject(const Ref<Mesh>& mesh)
    {
        if (!mesh) throw std::invalid_argument("Render resource warmup requires a mesh.");
        auto object = CreateRef<RenderObject>();
        object->MeshResource = mesh;
        object->bCastShadow = false;
        object->Sections.reserve(mesh->mSubMeshes.size());
        for (const auto& subMesh : mesh->mSubMeshes)
        {
            RenderMeshSection section;
            section.SubMeshResource = subMesh;
            if (subMesh && subMesh->mMaterialIndex < mesh->mMaterial.size())
                section.MaterialResource = mesh->mMaterial[subMesh->mMaterialIndex];
            if (section.MaterialResource)
            {
                section.MaterialGpuIndex = GetOrCreateMaterialIndex(section.MaterialResource, false);
                section.MaterialInstance = section.MaterialResource->BuildRenderSnapshot();
                section.AlphaCutoff = section.MaterialInstance.AlphaCutoff;
                section.bDoubleSided = section.MaterialInstance.bDoubleSided;
                section.AlphaMode = section.MaterialInstance.AlphaMode;
            }
            object->Sections.push_back(std::move(section));
        }
        return object;
    }

    void RenderWorldExtractor::Reset()
    {
        mPreviousTransforms.clear();
        mCachedLocalTransforms.clear();
        mCachedStaticLightmapAssets.clear();
        bStaticLightmapAssetsDirty = true;
        mPreviousBonePalettes.clear();
        mPreviousBoneMeshes.clear();
        mPreviousSceneGeneration = 0;
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

    uint32_t RenderWorldExtractor::GetOrCreateMaterialIndex(const Ref<Material>& material, bool bBlackSilhouette)
    {
        const auto iterator = mMaterialIndices.try_emplace(
            std::weak_ptr<Material>(material), std::array<uint32_t, 2>{ InvalidGpuSceneIndex, InvalidGpuSceneIndex }).first;
        uint32_t& index = iterator->second[bBlackSilhouette ? 1u : 0u];
        if (index == InvalidGpuSceneIndex)
        {
            if (mNextMaterialIndex == InvalidGpuSceneIndex)
                throw std::overflow_error("RenderWorld material identity capacity exhausted within one scene generation");
            index = mNextMaterialIndex++;
        }
        return index;
    }
}
