#include "Hepch.h"

#include "SceneLocalShadowPass.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr uint32_t c_InvalidView = ~0u;

        struct alignas(16) ShadowViewRecord
        {
            glm::mat4 ViewProjection{ 1.0f };
            glm::vec4 PositionRange{ 0.0f };
            glm::vec4 BiasSettings{ 0.0f };
        };
        static_assert(sizeof(ShadowViewRecord) == 96);

        struct Candidate
        {
            uint32_t SourceIndex = 0;
            double Importance = 0.0;
        };
        struct Caster
        {
            const RenderObject* Object = nullptr;
            uint64_t Signature = 0;
        };
        struct UploadPassData { BufferHandle Records; };
        struct ShadowPassData { TextureHandle Depth; };

        void HashBytes(uint64_t& hash, const void* data, size_t bytes) noexcept
        {
            const auto* source = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < bytes; ++i)
                hash = (hash ^ source[i]) * 1099511628211ull;
        }
        template <typename T> void HashValue(uint64_t& hash, const T& value) noexcept
        {
            HashBytes(hash, &value, sizeof(value));
        }

        glm::vec3 SafeDirection(glm::vec3 direction) noexcept
        {
            const float length = glm::length(direction);
            return std::isfinite(length) && length > 1e-6f
                ? direction / length : glm::vec3(0.0f, 0.0f, -1.0f);
        }

        std::array<glm::vec4, 6> ExtractFrustumPlanes(const glm::mat4& matrix)
        {
            const glm::mat4 rows = glm::transpose(matrix);
            std::array<glm::vec4, 6> result = {
                rows[3] + rows[0], rows[3] - rows[0],
                rows[3] + rows[1], rows[3] - rows[1], rows[2], rows[3] - rows[2] };
            for (glm::vec4& plane : result)
            {
                const float length = glm::length(glm::vec3(plane));
                plane = std::isfinite(length) && length > 1e-8f
                    ? plane / length : glm::vec4(0.0f);
            }
            return result;
        }

        bool SphereIntersects(const std::array<glm::vec4, 6>& planes,
            const glm::vec3& position, float radius) noexcept
        {
            for (const glm::vec4& plane : planes)
                if (glm::dot(glm::vec3(plane), position) + plane.w < -radius)
                    return false;
            return true;
        }

        bool CasterIntersects(const SceneLocalShadowView& view, const RenderObject& object) noexcept
        {
            // Extraction expands skinned bounds over the current bone palette.
            // Unknown or non-finite bounds remain visible in every light view.
            if (!object.Bounds.bValid)
                return true;
            for (const glm::vec4& plane : view.FrustumPlanes)
            {
                const float distance = glm::dot(glm::vec3(plane), object.Bounds.Center) + plane.w;
                const bool bHasAabb = glm::any(glm::greaterThan(object.Bounds.Extents, glm::vec3(0.0f)));
                const float extent = bHasAabb
                    ? glm::dot(glm::abs(glm::vec3(plane)), glm::max(object.Bounds.Extents, glm::vec3(0.0f)))
                    : object.Bounds.Radius;
                if (distance < -std::max(extent, 0.0f))
                    return false;
            }
            return true;
        }

        std::vector<Caster> BuildCasters(const RenderWorld& world)
        {
            std::vector<Caster> casters;
            casters.reserve(world.Objects.size());
            for (const RenderObject& object : world.Objects)
            {
                if (!object.bVisible || !object.bCastShadow || !object.MeshResource)
                    continue;
                uint64_t signature = 14695981039346656037ull;
                HashValue(signature, object.ObjectGpuIndex);
                HashValue(signature, object.WorldTransform);
                const auto* mesh = object.MeshResource.get();
                HashValue(signature, mesh);
                HashBytes(signature, object.BonePalette.data(), object.BonePalette.size() * sizeof(glm::mat4));
                // ShadowDepth uses static coverage parameters, not the custom
                // surface shader's frame time.
                for (const RenderMeshSection& section : object.Sections)
                {
                    const auto* subMesh = section.SubMeshResource.get();
                    HashValue(signature, subMesh);
                    HashValue(signature, section.AlphaMode);
                    HashValue(signature, section.AlphaCutoff);
                    HashValue(signature, section.bDoubleSided);
                    HashValue(signature, section.GetMaterialInstance().Revision);
                    const std::string& path = section.GetMaterialInstance().AssetPath;
                    HashBytes(signature, path.data(), path.size());
                }
                casters.push_back({ &object, signature });
            }
            return casters;
        }

        template <typename Light>
        std::vector<Candidate> RankLights(const std::vector<Light>& lights,
            const std::vector<uint32_t>& previousSlots, const RenderWorld& world,
            const std::array<glm::vec4, 6>& cameraPlanes)
        {
            std::vector<Candidate> candidates;
            for (uint32_t index = 0; index < lights.size(); ++index)
            {
                const Light& light = lights[index];
                if (!std::isfinite(light.Intensity) || light.Intensity <= 0.0f ||
                    !std::isfinite(light.Radius) || light.Radius <= 0.001f ||
                    !std::isfinite(light.Position.x) || !std::isfinite(light.Position.y) ||
                    !std::isfinite(light.Position.z) ||
                    !SphereIntersects(cameraPlanes, light.Position, light.Radius))
                    continue;
                const double luminance = glm::dot(light.Color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
                if (!std::isfinite(luminance) || luminance <= 0.0)
                    continue;
                const glm::vec3 delta = light.Position - world.Camera.Position;
                const double distanceSquared = glm::dot(delta, delta);
                const double radiusSquared = double(light.Radius) * light.Radius;
                double importance = double(light.Intensity) * luminance * radiusSquared /
                    std::max(distanceSquared + radiusSquared, 0.001);
                if (std::find(previousSlots.begin(), previousSlots.end(), index) != previousSlots.end())
                    importance *= 1.2; // Hysteresis keeps similar lights in stable atlas slots.
                candidates.push_back({ index, importance });
            }
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
            {
                return a.Importance != b.Importance ? a.Importance > b.Importance : a.SourceIndex < b.SourceIndex;
            });
            return candidates;
        }

        void AssignSlots(std::vector<uint32_t>& slots, const std::vector<Candidate>& ranked)
        {
            const size_t count = std::min(slots.size(), ranked.size());
            const auto selected = [&](uint32_t source)
            {
                return std::any_of(ranked.begin(), ranked.begin() + count,
                    [source](const Candidate& item) { return item.SourceIndex == source; });
            };
            for (uint32_t& source : slots)
                if (!selected(source)) source = c_InvalidView;
            for (size_t i = 0; i < count; ++i)
            {
                if (std::find(slots.begin(), slots.end(), ranked[i].SourceIndex) != slots.end())
                    continue;
                *std::find(slots.begin(), slots.end(), c_InvalidView) = ranked[i].SourceIndex;
            }
        }
    }

    struct SceneLocalShadowPass::Impl
    {
        explicit Impl(const SceneLocalShadowPassDesc& desc) : Desc(desc)
        {
            if (!desc.Device || desc.Resolution < 16 || desc.Resolution > 4096 ||
                desc.SpotViewBudget + desc.PointLightBudget * 6u == 0 ||
                desc.SpotViewBudget > 32 || desc.PointLightBudget > 8)
                throw std::invalid_argument("Local shadow budget or device is invalid");
            const uint32_t layers = desc.SpotViewBudget + desc.PointLightBudget * 6u;
            Depth = desc.Device->createTexture(nvrhi::TextureDesc()
                .setWidth(desc.Resolution).setHeight(desc.Resolution).setArraySize(layers)
                .setDimension(nvrhi::TextureDimension::Texture2DArray).setFormat(nvrhi::Format::D32)
                .setIsRenderTarget(true).setInitialState(nvrhi::ResourceStates::Common)
                .setKeepInitialState(false).setDebugName("Budgeted Local Shadow Atlas"));
            RecordsBuffer = desc.Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(uint64_t(layers) * sizeof(ShadowViewRecord)).setStructStride(sizeof(ShadowViewRecord))
                .setInitialState(nvrhi::ResourceStates::Common).setKeepInitialState(false)
                .setDebugName("Local Shadow View Records"));
            if (!Depth || !RecordsBuffer)
                throw std::runtime_error("Local shadow resource allocation failed");
            Framebuffers.resize(layers);
            for (uint32_t layer = 0; layer < layers; ++layer)
            {
                nvrhi::FramebufferDesc framebuffer;
                framebuffer.setDepthAttachment(nvrhi::FramebufferAttachment()
                    .setTexture(Depth).setSubresources(nvrhi::TextureSubresourceSet(0, 1, layer, 1)));
                Framebuffers[layer] = desc.Device->createFramebuffer(framebuffer);
                if (!Framebuffers[layer])
                    throw std::runtime_error("Local shadow framebuffer creation failed");
            }
            SpotSlots.assign(desc.SpotViewBudget, c_InvalidView);
            PointSlots.assign(desc.PointLightBudget, c_InvalidView);
            Records.resize(layers);
            ValidSignatures.assign(layers, 0);
            PendingSignatures.assign(layers, 0);
            RecordedLayers.assign(layers, false);
        }

        SceneLocalShadowPassDesc Desc;
        nvrhi::TextureHandle Depth;
        nvrhi::BufferHandle RecordsBuffer;
        std::vector<nvrhi::FramebufferHandle> Framebuffers;
        std::vector<uint32_t> SpotSlots;
        std::vector<uint32_t> PointSlots;
        std::vector<uint32_t> SpotViews;
        std::vector<uint32_t> PointViews;
        std::vector<SceneLocalShadowView> Views;
        std::vector<ShadowViewRecord> Records;
        std::vector<uint64_t> ValidSignatures;
        std::vector<uint64_t> PendingSignatures;
        std::vector<bool> RecordedLayers;
        std::vector<uint32_t> DirtyViewIndices;
        SceneLocalShadowStats Stats;
        uint64_t SceneGeneration = 0;
        bool bPrepared = false;
        bool bRecordsDirty = true;
        bool bRecordsUploaded = false;
        bool bFrameRecorded = false;
        bool bResourcesSubmitted = false;
        bool bImportStateChanged = false;
    };

    SceneLocalShadowPass::SceneLocalShadowPass(const SceneLocalShadowPassDesc& desc)
        : mImpl(CreateScope<Impl>(desc)) {}
    SceneLocalShadowPass::~SceneLocalShadowPass() = default;

    bool SceneLocalShadowView::IsObjectVisible(uint32_t objectGpuIndex) const noexcept
    {
        return std::binary_search(VisibleObjectGpuIndices.begin(), VisibleObjectGpuIndices.end(), objectGpuIndex);
    }

    bool SceneLocalShadowPass::PrepareFrame(const RenderWorld& world, uint64_t completedSubmission)
    {
        // This fixed atlas never reallocates. Queue ordering protects updates;
        // the renderer owns final resource destruction after GPU completion.
        (void)completedSubmission;
        Impl& impl = *mImpl;
        const bool bGraphStateChanged = !impl.bPrepared || impl.bImportStateChanged;
        impl.bImportStateChanged = false;
        if (impl.SceneGeneration != world.SceneGeneration)
        {
            std::fill(impl.SpotSlots.begin(), impl.SpotSlots.end(), c_InvalidView);
            std::fill(impl.PointSlots.begin(), impl.PointSlots.end(), c_InvalidView);
            std::fill(impl.ValidSignatures.begin(), impl.ValidSignatures.end(), 0);
            impl.SceneGeneration = world.SceneGeneration;
        }
        impl.Stats = {};
        impl.Stats.ResidentBytes = uint64_t(impl.Records.size()) *
            (uint64_t(impl.Desc.Resolution) * impl.Desc.Resolution * 4u + sizeof(ShadowViewRecord));
        const auto cameraPlanes = ExtractFrustumPlanes(world.Camera.ViewProjection);
        const auto rankedSpots = RankLights(world.SpotLights, impl.SpotSlots, world, cameraPlanes);
        const auto rankedPoints = RankLights(world.PointLights, impl.PointSlots, world, cameraPlanes);
        AssignSlots(impl.SpotSlots, rankedSpots);
        AssignSlots(impl.PointSlots, rankedPoints);
        impl.Stats.SelectedSpotLights = static_cast<uint32_t>(std::min(rankedSpots.size(), impl.SpotSlots.size()));
        impl.Stats.SelectedPointLights = static_cast<uint32_t>(std::min(rankedPoints.size(), impl.PointSlots.size()));
        impl.Stats.UnshadowedCandidateLights = static_cast<uint32_t>(rankedSpots.size() + rankedPoints.size()) -
            impl.Stats.SelectedSpotLights - impl.Stats.SelectedPointLights;
        impl.SpotViews.assign(world.SpotLights.size(), c_InvalidView);
        impl.PointViews.assign(world.PointLights.size(), c_InvalidView);
        impl.Views.clear();
        impl.DirtyViewIndices.clear();
        std::fill(impl.RecordedLayers.begin(), impl.RecordedLayers.end(), false);
        impl.bRecordsUploaded = false;
        impl.bFrameRecorded = false;
        const auto casters = BuildCasters(world);
        std::vector<ShadowViewRecord> records(impl.Records.size());
        const auto appendView = [&](uint32_t layer, uint32_t sourceIndex, bool bPoint,
            uint32_t face, glm::vec3 position, glm::vec3 direction, glm::vec3 up,
            float radius, float fieldOfView)
        {
            const float nearPlane = std::clamp(radius * 0.001f, 0.0001f, std::min(radius * 0.5f, 0.25f));
            SceneLocalShadowView view;
            view.ViewProjection = glm::perspectiveRH_ZO(fieldOfView, 1.0f, nearPlane, radius) *
                glm::lookAtRH(position, position + direction, up);
            view.Position = position;
            view.FrustumPlanes = ExtractFrustumPlanes(view.ViewProjection);
            view.Layer = layer;
            view.Resolution = impl.Desc.Resolution;
            view.SourceLightIndex = sourceIndex;
            view.PointFace = face;
            view.bPointLight = bPoint;
            uint64_t signature = 14695981039346656037ull;
            HashValue(signature, world.SceneGeneration);
            HashValue(signature, view.ViewProjection);
            HashValue(signature, sourceIndex);
            for (const Caster& caster : casters)
            {
                if (!CasterIntersects(view, *caster.Object)) continue;
                view.VisibleObjectGpuIndices.push_back(caster.Object->ObjectGpuIndex == InvalidGpuSceneIndex
                    ? 0u : caster.Object->ObjectGpuIndex);
                HashValue(signature, caster.Signature);
            }
            std::sort(view.VisibleObjectGpuIndices.begin(), view.VisibleObjectGpuIndices.end());
            impl.Stats.VisibleCasterObjects += static_cast<uint32_t>(view.VisibleObjectGpuIndices.size());
            impl.PendingSignatures[layer] = signature;
            if (impl.ValidSignatures[layer] != signature)
                impl.DirtyViewIndices.push_back(static_cast<uint32_t>(impl.Views.size()));
            records[layer] = { view.ViewProjection, glm::vec4(position, radius),
                glm::vec4(1.0f / impl.Desc.Resolution, impl.Desc.DepthBias, impl.Desc.NormalBias, 1.0f) };
            impl.Views.push_back(std::move(view));
        };
        for (uint32_t slot = 0; slot < impl.SpotSlots.size(); ++slot)
        {
            const uint32_t sourceIndex = impl.SpotSlots[slot];
            if (sourceIndex == c_InvalidView) continue;
            const RenderSpotLight& light = world.SpotLights[sourceIndex];
            const glm::vec3 direction = SafeDirection(light.Direction);
            const glm::vec3 up = std::abs(direction.y) > 0.99f
                ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const float outerCosine = std::isfinite(light.OuterConeCosine)
                ? std::clamp(light.OuterConeCosine, 0.01745f, 0.99999f) : 0.866f;
            appendView(slot, sourceIndex, false, 0, light.Position, direction, up,
                light.Radius, 2.0f * std::acos(outerCosine));
            impl.SpotViews[sourceIndex] = slot;
        }
        const std::array<glm::vec3, 6> directions = {
            glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
            glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1) };
        const std::array<glm::vec3, 6> upDirections = {
            glm::vec3(0, -1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1),
            glm::vec3(0, 0, -1), glm::vec3(0, -1, 0), glm::vec3(0, -1, 0) };
        for (uint32_t slot = 0; slot < impl.PointSlots.size(); ++slot)
        {
            const uint32_t sourceIndex = impl.PointSlots[slot];
            if (sourceIndex == c_InvalidView) continue;
            const RenderPointLight& light = world.PointLights[sourceIndex];
            const uint32_t firstLayer = impl.Desc.SpotViewBudget + slot * 6u;
            for (uint32_t face = 0; face < 6; ++face)
                appendView(firstLayer + face, sourceIndex, true, face, light.Position,
                    directions[face], upDirections[face], light.Radius, glm::radians(90.2f));
            impl.PointViews[sourceIndex] = firstLayer;
        }
        impl.bRecordsDirty |= std::memcmp(records.data(), impl.Records.data(), records.size() * sizeof(ShadowViewRecord)) != 0;
        impl.Records = std::move(records);
        impl.Stats.ActiveViews = static_cast<uint32_t>(impl.Views.size());
        impl.Stats.DirtyViews = static_cast<uint32_t>(impl.DirtyViewIndices.size());
        impl.bPrepared = true;
        return bGraphStateChanged;
    }

    SceneLocalShadowGraphResources SceneLocalShadowPass::AddPasses(RenderGraph& graph,
        SceneLocalShadowCallbacks callbacks)
    {
        Impl& impl = *mImpl;
        if (!impl.bPrepared || !callbacks.DrawView || !callbacks.DeclareCasterResources)
            throw std::logic_error("Local shadow graph requires a prepared frame and caster callbacks");
        SceneLocalShadowGraphResources result;
        const nvrhi::ResourceStates initialState = impl.bResourcesSubmitted
            ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common;
        result.DepthArray = graph.ImportTexture(RenderGraphTextureDesc{
            .DebugName = "Budgeted Local Shadow Atlas", .Width = impl.Desc.Resolution,
            .Height = impl.Desc.Resolution, .ArrayLayers = static_cast<uint32_t>(impl.Records.size()),
            .Format = nvrhi::Format::D32, .Dimension = nvrhi::TextureDimension::Texture2DArray,
        }, impl.Depth, initialState, nvrhi::ResourceStates::ShaderResource);
        result.ViewRecords = graph.ImportBuffer(RenderGraphBufferDesc{
            .DebugName = "Local Shadow View Records", .ByteSize = impl.Records.size() * sizeof(ShadowViewRecord),
            .StructStride = sizeof(ShadowViewRecord),
        }, impl.RecordsBuffer, initialState, nvrhi::ResourceStates::ShaderResource);
        graph.AddPass<UploadPassData>("LocalShadowViewUpload", RenderGraphQueue::Graphics,
            [result](RenderGraphBuilder& builder, UploadPassData& data)
            { data.Records = builder.CopyDest(result.ViewRecords); },
            [state = mImpl.get()](RenderGraphContext& context, const UploadPassData& data)
            {
                auto* records = context.GetBuffer(data.Records, RenderGraphAccess::Write);
                if (!state->bRecordsDirty) return;
                const size_t bytes = state->Records.size() * sizeof(ShadowViewRecord);
                context.GetCommandList()->writeBuffer(records, state->Records.data(), bytes);
                state->Stats.UploadedBytes += bytes;
                state->bRecordsUploaded = true;
            });
        graph.AddPass<ShadowPassData>("BudgetedLocalShadows", RenderGraphQueue::Graphics,
            [result, declare = std::move(callbacks.DeclareCasterResources)](
                RenderGraphBuilder& builder, ShadowPassData& data)
            {
                data.Depth = builder.DepthWrite(result.DepthArray);
                declare(builder);
            },
            [state = mImpl.get(), draw = std::move(callbacks.DrawView)](
                RenderGraphContext& context, const ShadowPassData& data)
            {
                auto* texture = context.GetTexture(data.Depth, RenderGraphAccess::Write);
                state->bFrameRecorded = true;
                for (uint32_t viewIndex : state->DirtyViewIndices)
                {
                    const SceneLocalShadowView& view = state->Views[viewIndex];
                    context.GetCommandList()->clearDepthStencilTexture(texture,
                        nvrhi::TextureSubresourceSet(0, 1, view.Layer, 1), true, 1.0f, false, 0);
                    draw(context, view, state->Framebuffers[view.Layer]);
                    state->RecordedLayers[view.Layer] = true;
                    ++state->Stats.RenderedViews;
                }
            });
        return result;
    }

    std::span<const uint32_t> SceneLocalShadowPass::GetPointShadowViews() const noexcept { return mImpl->PointViews; }
    std::span<const uint32_t> SceneLocalShadowPass::GetSpotShadowViews() const noexcept { return mImpl->SpotViews; }
    std::span<const SceneLocalShadowView> SceneLocalShadowPass::GetViews() const noexcept { return mImpl->Views; }
    std::span<const uint32_t> SceneLocalShadowPass::GetDirtyViewIndices() const noexcept { return mImpl->DirtyViewIndices; }

    void SceneLocalShadowPass::InvalidateGeometry()
    {
        auto& state = *mImpl;
        state.DirtyViewIndices.clear();
        for (uint32_t i = 0; i < state.Views.size(); ++i)
        {
            state.ValidSignatures[state.Views[i].Layer] = 0;
            state.DirtyViewIndices.push_back(i);
        }
        state.Stats.DirtyViews = static_cast<uint32_t>(state.DirtyViewIndices.size());
    }
    nvrhi::ITexture* SceneLocalShadowPass::GetDepthArray() const noexcept { return mImpl->Depth; }
    nvrhi::IBuffer* SceneLocalShadowPass::GetViewRecordBuffer() const noexcept { return mImpl->RecordsBuffer; }
    const SceneLocalShadowStats& SceneLocalShadowPass::GetStats() const noexcept { return mImpl->Stats; }

    void SceneLocalShadowPass::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        (void)submission;
        if (mImpl->bFrameRecorded && !mImpl->bResourcesSubmitted)
        {
            mImpl->bResourcesSubmitted = true;
            // A cached graph imported the first frame from Common. Rebuild
            // its import declarations once to match subsequent SRV state.
            mImpl->bImportStateChanged = true;
        }
        for (size_t layer = 0; layer < mImpl->RecordedLayers.size(); ++layer)
            if (mImpl->RecordedLayers[layer])
                mImpl->ValidSignatures[layer] = mImpl->PendingSignatures[layer];
        if (mImpl->bRecordsUploaded) mImpl->bRecordsDirty = false;
        mImpl->bRecordsUploaded = false;
        std::fill(mImpl->RecordedLayers.begin(), mImpl->RecordedLayers.end(), false);
        mImpl->bFrameRecorded = false;
    }

    void SceneLocalShadowPass::AbortFrame() noexcept
    {
        mImpl->bRecordsUploaded = false;
        mImpl->bFrameRecorded = false;
        std::fill(mImpl->RecordedLayers.begin(), mImpl->RecordedLayers.end(), false);
    }
}
