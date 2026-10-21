#include "Hepch.h"

#include "SceneLightGrid.h"

#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    namespace
    {
        constexpr uint32_t c_TileSize = 32;
        constexpr uint32_t c_DepthSlices = 16;
        constexpr uint32_t c_MaxLights = 4096;

        struct alignas(16) LightRecord
        {
            glm::vec4 PositionRadius{ 0.0f };
            glm::vec4 ColorIntensity{ 0.0f };
            glm::vec4 DirectionOuterCosine{ 0.0f, 0.0f, -1.0f, 0.0f };
            glm::vec4 ConeReservedTypeShadow{ 0.0f };
        };
        static_assert(sizeof(LightRecord) == 64);

        struct alignas(16) LightGridConstants
        {
            glm::mat4 View{ 1.0f };
            glm::mat4 InverseProjection{ 1.0f };
            glm::mat4 ViewProjection{ 1.0f };
            glm::uvec4 GridDimensions{ 0u };
            glm::uvec4 FrameDimensions{ 0u };
            glm::vec4 DepthParameters{ 0.0f };
        };
        static_assert(sizeof(LightGridConstants) == 240);

        struct UploadPassData
        {
            BufferHandle Constants;
            BufferHandle Lights;
        };
        struct GridPassData
        {
            BufferHandle Constants;
            BufferHandle Lights;
            BufferHandle Masks;
        };

        float FiniteNonnegative(float value) noexcept
        {
            return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
        }

        glm::vec3 SafeColor(const glm::vec3& value) noexcept
        {
            return { FiniteNonnegative(value.x), FiniteNonnegative(value.y),
                FiniteNonnegative(value.z) };
        }

        glm::vec3 SafeDirection(const glm::vec3& value) noexcept
        {
            const float lengthSquared = glm::dot(value, value);
            return std::isfinite(lengthSquared) && lengthSquared > 1e-8f
                ? value / std::sqrt(lengthSquared) : glm::vec3(0.0f, 0.0f, -1.0f);
        }

        bool FinitePosition(const glm::vec3& value) noexcept
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        uint64_t GrowCapacity(uint64_t required)
        {
            uint64_t capacity = 256;
            while (capacity < required)
                capacity *= 2;
            return capacity;
        }
    }

    struct SceneLightGrid::Impl
    {
        struct RetiredResources
        {
            uint64_t Submission = 0;
            uint64_t Bytes = 0;
            nvrhi::BufferHandle Lights;
            nvrhi::BufferHandle Masks;
            nvrhi::BindingSetHandle BindingSet;
        };

        explicit Impl(const SceneLightGridDesc& desc)
            : Device(desc.Device)
            , OwnedShaderManager(desc.SharedShaderManager ? nullptr : CreateScope<ShaderManager>(desc.Device))
            , Shaders(desc.SharedShaderManager ? *desc.SharedShaderManager : *OwnedShaderManager)
        {
            if (!Device || desc.ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("SceneLightGrid requires a device and shader artifact root");
            ShaderArtifactResolver resolver(desc.ShaderArtifactCacheRoot, desc.Backend);
            Shader = resolver.LoadShaderFile(Shaders, ShaderCacheKey{
                .Source = "Modern/SceneLightGrid.hlsl", .Variant = "BASE",
                .Stage = nvrhi::ShaderType::Compute, .EntryPoint = "CS", .TargetProfile = "cs_6_0",
            }, "SceneLightGrid.cs", "Scene 3D light grid");
            ConstantsBuffer = Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(sizeof(LightGridConstants)).setIsConstantBuffer(true)
                .setIsVolatile(true).setMaxVersions(64)
                .setDebugName("Scene Light Grid Constants"));
            nvrhi::BindingLayoutDesc layout;
            layout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(0)
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
            ComputeLayout = Device->createBindingLayout(layout);
            Pipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
                .setComputeShader(Shader).addBindingLayout(ComputeLayout));
            if (!ConstantsBuffer || !ComputeLayout || !Pipeline)
                throw std::runtime_error("Scene light grid immutable state creation failed");
        }

        void UpdateBindingSet()
        {
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, ConstantsBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, LightBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, MaskBuffer));
            ComputeBindingSet = Device->createBindingSet(desc, ComputeLayout);
            if (!ComputeBindingSet)
                throw std::runtime_error("Scene light grid resource binding failed");
        }

        nvrhi::IDevice* Device;
        Scope<ShaderManager> OwnedShaderManager;
        ShaderManager& Shaders;
        nvrhi::ShaderHandle Shader;
        nvrhi::BindingLayoutHandle ComputeLayout;
        nvrhi::ComputePipelineHandle Pipeline;
        nvrhi::BindingSetHandle ComputeBindingSet;
        nvrhi::BufferHandle ConstantsBuffer;
        nvrhi::BufferHandle LightBuffer;
        nvrhi::BufferHandle MaskBuffer;
        LightGridConstants Constants;
        std::vector<LightRecord> Lights;
        std::vector<uint32_t> PointShadowViews;
        std::vector<uint32_t> SpotShadowViews;
        std::vector<RetiredResources> Retired;
        SceneLightGridStats Stats;
        uint64_t LightCapacity = 0;
        uint64_t MaskCapacity = 0;
        uint64_t LastSubmission = 0;
        bool bPrepared = false;
        bool bLightsDirty = true;
        bool bGridDirty = true;
        bool bLightUploadRecorded = false;
        bool bGridRecorded = false;
        size_t DirtyLightBegin = 0;
        size_t DirtyLightEnd = 0;
    };

    SceneLightGrid::SceneLightGrid(const SceneLightGridDesc& desc)
        : mImpl(CreateScope<Impl>(desc)) {}
    SceneLightGrid::~SceneLightGrid() = default;

    bool SceneLightGrid::PrepareFrame(const RenderWorld& world,
        uint32_t width, uint32_t height, uint64_t completedSubmission)
    {
        if (width == 0 || height == 0)
            throw std::invalid_argument("Scene light grid dimensions must be nonzero");
        Collect(completedSubmission);
        Impl& impl = *mImpl;
        impl.Stats.UploadedBytes = 0;
        impl.Stats.bGridDispatched = false;
        impl.bLightUploadRecorded = false;
        impl.bGridRecorded = false;
        std::vector<LightRecord> lights;
        lights.reserve(world.PointLights.size() + world.SpotLights.size() + world.DirectionalLights.size());
        for (size_t sourceIndex = 0; sourceIndex < world.PointLights.size(); ++sourceIndex)
        {
            const RenderPointLight& source = world.PointLights[sourceIndex];
            const float intensity = FiniteNonnegative(source.Intensity);
            const float radius = FiniteNonnegative(source.Radius);
            if (intensity == 0.0f || radius == 0.0f || !FinitePosition(source.Position))
                continue;
            lights.push_back({ glm::vec4(source.Position, radius),
                glm::vec4(SafeColor(source.Color), intensity), glm::vec4(0.0f),
                glm::vec4(0.0f, 0.0f, 0.0f,
                    sourceIndex < impl.PointShadowViews.size() && impl.PointShadowViews[sourceIndex] != ~0u
                        ? float(impl.PointShadowViews[sourceIndex] + 1u) : 0.0f) });
        }
        for (size_t sourceIndex = 0; sourceIndex < world.SpotLights.size(); ++sourceIndex)
        {
            const RenderSpotLight& source = world.SpotLights[sourceIndex];
            const float intensity = FiniteNonnegative(source.Intensity);
            const float radius = FiniteNonnegative(source.Radius);
            if (intensity == 0.0f || radius == 0.0f || !FinitePosition(source.Position))
                continue;
            const float outerCosine = std::isfinite(source.OuterConeCosine)
                ? std::clamp(source.OuterConeCosine, -1.0f, 1.0f) : 0.866f;
            const float innerCosine = std::isfinite(source.InnerConeCosine)
                ? std::clamp(source.InnerConeCosine, outerCosine, 1.0f) : outerCosine;
            lights.push_back({ glm::vec4(source.Position, radius),
                glm::vec4(SafeColor(source.Color), intensity),
                glm::vec4(SafeDirection(source.Direction), outerCosine),
                glm::vec4(innerCosine, 0.0f, 1.0f,
                    sourceIndex < impl.SpotShadowViews.size() && impl.SpotShadowViews[sourceIndex] != ~0u
                        ? float(impl.SpotShadowViews[sourceIndex] + 1u) : 0.0f) });
        }
        for (size_t index = 0; index < world.DirectionalLights.size(); ++index)
        {
            const RenderDirectionalLight& source = world.DirectionalLights[index];
            const float intensity = FiniteNonnegative(source.Intensity);
            if (intensity == 0.0f)
                continue;
            lights.push_back({ glm::vec4(0.0f), glm::vec4(SafeColor(source.Color), intensity),
                glm::vec4(SafeDirection(source.Direction), 0.0f),
                glm::vec4(0.0f, 0.0f, 2.0f, index == 0 && source.bCastShadows ? 1.0f : 0.0f) });
        }
        if (lights.size() > c_MaxLights)
            throw std::length_error("Scene light grid exceeds its explicit 4096-light allocation budget");
        const bool bLightsChanged = lights.size() != impl.Lights.size() ||
            (!lights.empty() && std::memcmp(lights.data(), impl.Lights.data(), lights.size() * sizeof(LightRecord)) != 0);
        if (bLightsChanged)
        {
            size_t first = 0;
            while (first < lights.size() && first < impl.Lights.size() &&
                std::memcmp(&lights[first], &impl.Lights[first], sizeof(LightRecord)) == 0)
                ++first;
            size_t end = std::max(lights.size(), size_t(1));
            if (lights.size() == impl.Lights.size())
                while (end > first &&
                    std::memcmp(&lights[end - 1], &impl.Lights[end - 1], sizeof(LightRecord)) == 0)
                    --end;
            first = std::min(first, end - 1);
            impl.DirtyLightBegin = impl.bLightsDirty ? std::min(impl.DirtyLightBegin, first) : first;
            impl.DirtyLightEnd = impl.bLightsDirty ? std::max(impl.DirtyLightEnd, end) : end;
            impl.Lights = std::move(lights);
        }
        const uint32_t wordCount = std::max((static_cast<uint32_t>(impl.Lights.size()) + 31u) / 32u, 1u);
        const bool bOrthographic = std::abs(world.Camera.Projection[3][3]) > 0.5f;
        const float nearPlane = std::isfinite(world.Camera.NearPlane)
            ? (bOrthographic ? world.Camera.NearPlane : std::max(world.Camera.NearPlane, 0.001f)) : 0.01f;
        const float farPlane = std::isfinite(world.Camera.FarPlane)
            ? std::max(world.Camera.FarPlane, nearPlane + 0.01f) : 1000.0f;
        LightGridConstants constants;
        constants.View = world.Camera.View;
        constants.InverseProjection = glm::inverse(world.Camera.Projection);
        constants.ViewProjection = world.Camera.ViewProjection;
        constants.GridDimensions = { (width + c_TileSize - 1) / c_TileSize,
            (height + c_TileSize - 1) / c_TileSize, c_DepthSlices, wordCount };
        constants.FrameDimensions = { width, height, static_cast<uint32_t>(impl.Lights.size()), c_TileSize };
        constants.DepthParameters = { nearPlane, farPlane,
            bOrthographic ? 1.0f / (farPlane - nearPlane) : 1.0f / std::log2(farPlane / nearPlane),
            bOrthographic ? -float(c_DepthSlices) : float(c_DepthSlices) };
        const bool bCameraChanged = !impl.bPrepared ||
            std::memcmp(&constants, &impl.Constants, sizeof(constants)) != 0;
        const uint64_t clusterCount = uint64_t(constants.GridDimensions.x) * constants.GridDimensions.y * c_DepthSlices;
        if (clusterCount > std::numeric_limits<uint32_t>::max())
            throw std::length_error("Scene light grid dimensions exceed the dispatch budget");
        const uint64_t lightBytes = std::max(uint64_t(impl.Lights.size()) * sizeof(LightRecord), uint64_t(sizeof(LightRecord)));
        const uint64_t maskBytes = clusterCount * wordCount * sizeof(uint32_t);
        const bool bReallocate = lightBytes > impl.LightCapacity || maskBytes > impl.MaskCapacity;
        if (bReallocate)
        {
            const uint64_t lightCapacity = std::max(impl.LightCapacity, GrowCapacity(lightBytes));
            const uint64_t maskCapacity = std::max(impl.MaskCapacity, GrowCapacity(maskBytes));
            auto newLights = impl.Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(lightCapacity).setStructStride(sizeof(LightRecord))
                .setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true)
                .setDebugName("Scene Authoritative Light Records"));
            auto newMasks = impl.Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(maskCapacity).setStructStride(sizeof(uint32_t)).setCanHaveUAVs(true)
                .setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true)
                .setDebugName("Scene Conservative Cluster Light Masks"));
            if (!newLights || !newMasks)
                throw std::runtime_error("Scene light grid buffer allocation failed");
            if (impl.LightBuffer || impl.MaskBuffer)
                impl.Retired.push_back({ impl.LastSubmission, impl.LightCapacity + impl.MaskCapacity,
                    impl.LightBuffer, impl.MaskBuffer, impl.ComputeBindingSet });
            impl.LightBuffer = std::move(newLights);
            impl.MaskBuffer = std::move(newMasks);
            impl.LightCapacity = lightCapacity;
            impl.MaskCapacity = maskCapacity;
            impl.UpdateBindingSet();
            impl.DirtyLightBegin = 0;
            impl.DirtyLightEnd = std::max(impl.Lights.size(), size_t(1));
        }
        impl.Constants = constants;
        impl.bLightsDirty |= bLightsChanged || bReallocate;
        impl.bGridDirty |= bLightsChanged || bReallocate || bCameraChanged;
        impl.bPrepared = true;
        impl.Stats.LightCount = static_cast<uint32_t>(impl.Lights.size());
        impl.Stats.ClusterCount = static_cast<uint32_t>(clusterCount);
        impl.Stats.WordsPerCluster = wordCount;
        impl.Stats.ResidentBytes = impl.LightCapacity + impl.MaskCapacity + sizeof(LightGridConstants);
        impl.Stats.RetiredBytes = 0;
        for (const auto& retired : impl.Retired) impl.Stats.RetiredBytes += retired.Bytes;
        return bReallocate;
    }

    SceneLightGridGraphResources SceneLightGrid::AddPasses(RenderGraph& graph)
    {
        Impl& impl = *mImpl;
        if (!impl.bPrepared)
            throw std::logic_error("SceneLightGrid::AddPasses requires PrepareFrame");
        SceneLightGridGraphResources resources;
        resources.Constants = graph.ImportBuffer(RenderGraphBufferDesc{
            .DebugName = "Scene Light Grid Constants", .ByteSize = sizeof(LightGridConstants),
        }, impl.ConstantsBuffer, nvrhi::ResourceStates::ConstantBuffer, nvrhi::ResourceStates::ConstantBuffer);
        resources.Lights = graph.ImportBuffer(RenderGraphBufferDesc{
            .DebugName = "Scene Light Records", .ByteSize = impl.LightCapacity, .StructStride = sizeof(LightRecord),
        }, impl.LightBuffer, nvrhi::ResourceStates::ShaderResource, nvrhi::ResourceStates::ShaderResource);
        resources.ClusterMasks = graph.ImportBuffer(RenderGraphBufferDesc{
            .DebugName = "Scene Cluster Masks", .ByteSize = impl.MaskCapacity, .StructStride = sizeof(uint32_t),
        }, impl.MaskBuffer, nvrhi::ResourceStates::ShaderResource, nvrhi::ResourceStates::ShaderResource);
        graph.AddPass<UploadPassData>("SceneLightUpload", RenderGraphQueue::Graphics,
            [resources](RenderGraphBuilder& builder, UploadPassData& data)
            {
                data.Constants = builder.CopyDest(resources.Constants);
                data.Lights = builder.CopyDest(resources.Lights);
            },
            [state = mImpl.get()](RenderGraphContext& context, const UploadPassData& data)
            {
                auto* commandList = context.GetCommandList();
                auto* constants = context.GetBuffer(data.Constants, RenderGraphAccess::Write);
                auto* lights = context.GetBuffer(data.Lights, RenderGraphAccess::Write);
                commandList->writeBuffer(constants, &state->Constants, sizeof(state->Constants));
                state->Stats.UploadedBytes += sizeof(state->Constants);
                if (state->bLightsDirty)
                {
                    const LightRecord empty;
                    const size_t begin = std::min(state->DirtyLightBegin,
                        std::max(state->Lights.size(), size_t(1)) - 1);
                    const size_t end = std::min(state->DirtyLightEnd,
                        std::max(state->Lights.size(), size_t(1)));
                    const void* lightData = state->Lights.empty()
                        ? static_cast<const void*>(&empty) : static_cast<const void*>(state->Lights.data() + begin);
                    const size_t bytes = (end - begin) * sizeof(LightRecord);
                    commandList->writeBuffer(lights, lightData, bytes, begin * sizeof(LightRecord));
                    state->Stats.UploadedBytes += bytes;
                    state->bLightUploadRecorded = true;
                }
            });
        graph.AddPass<GridPassData>("SceneLightClustering", RenderGraphQueue::Compute,
            [resources](RenderGraphBuilder& builder, GridPassData& data)
            {
                data.Constants = builder.ConstantBuffer(resources.Constants);
                data.Lights = builder.ShaderResource(resources.Lights);
                data.Masks = builder.UnorderedAccess(resources.ClusterMasks);
            },
            [state = mImpl.get()](RenderGraphContext& context, const GridPassData& data)
            {
                (void)context.GetBuffer(data.Constants, RenderGraphAccess::Read);
                (void)context.GetBuffer(data.Lights, RenderGraphAccess::Read);
                (void)context.GetBuffer(data.Masks, RenderGraphAccess::Write);
                if (!state->bGridDirty)
                    return;
                auto* commandList = context.GetCommandList();
                commandList->setComputeState(nvrhi::ComputeState()
                    .setPipeline(state->Pipeline).addBindingSet(state->ComputeBindingSet));
                commandList->dispatch((state->Stats.ClusterCount + 63u) / 64u, 1, 1);
                state->bGridRecorded = true;
                state->Stats.bGridDispatched = true;
            });
        return resources;
    }

    void SceneLightGrid::SetLocalShadowIndices(std::span<const uint32_t> pointViews,
        std::span<const uint32_t> spotViews)
    {
        mImpl->PointShadowViews.assign(pointViews.begin(), pointViews.end());
        mImpl->SpotShadowViews.assign(spotViews.begin(), spotViews.end());
    }

    nvrhi::IBuffer* SceneLightGrid::GetConstantsBuffer() const noexcept { return mImpl->ConstantsBuffer; }
    nvrhi::IBuffer* SceneLightGrid::GetLightBuffer() const noexcept { return mImpl->LightBuffer; }
    nvrhi::IBuffer* SceneLightGrid::GetClusterMaskBuffer() const noexcept { return mImpl->MaskBuffer; }
    const SceneLightGridStats& SceneLightGrid::GetStats() const noexcept { return mImpl->Stats; }

    void SceneLightGrid::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        mImpl->LastSubmission = std::max(mImpl->LastSubmission, submission);
        if (mImpl->bLightUploadRecorded) mImpl->bLightsDirty = false;
        if (mImpl->bGridRecorded) mImpl->bGridDirty = false;
        mImpl->bLightUploadRecorded = false;
        mImpl->bGridRecorded = false;
    }

    void SceneLightGrid::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mImpl->Retired, [completedSubmission](const Impl::RetiredResources& resources)
        { return resources.Submission <= completedSubmission; });
        mImpl->Stats.RetiredBytes = 0;
        for (const auto& retired : mImpl->Retired) mImpl->Stats.RetiredBytes += retired.Bytes;
    }
}
