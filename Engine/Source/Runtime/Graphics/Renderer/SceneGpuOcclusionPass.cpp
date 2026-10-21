#include "Hepch.h"

#include "Runtime/Graphics/Renderer/SceneGpuOcclusionPass.h"

#include "Runtime/Graphics/GlobalIllumination/DepthPyramidPass.h"
#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    namespace
    {
        struct alignas(16) OcclusionConstants
        {
            glm::mat4 ViewProjection{ 1.0f };
            glm::uvec4 Counts{ 0u }; // draw count, batch count, pyramid base width/height
            glm::uvec4 Dimensions{ 0u }; // native width/height, mip count, operation
            glm::vec4 Parameters{ 0.0001f, 4.0f, 0.0f, 0.0f };
        };
        static_assert(sizeof(OcclusionConstants) == 112);
        struct PassData
        {
            TextureHandle Pyramid;
            BufferHandle Bounds, Draws, Visible, Arguments;
            glm::uvec4 Dimensions{ 0u };
            glm::uvec2 PyramidDimensions{ 0u };
            uint32_t Operation = 0;
        };
        struct BindingKey
        {
            nvrhi::ITexture* Pyramid = nullptr;
            std::array<nvrhi::IBuffer*, 4> Buffers{};
            bool operator==(const BindingKey&) const = default;
        };
        struct BindingHash
        {
            size_t operator()(const BindingKey& key) const noexcept
            {
                size_t hash = reinterpret_cast<size_t>(key.Pyramid);
                for (const auto* buffer : key.Buffers)
                    hash ^= reinterpret_cast<size_t>(buffer) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };
    }

    struct SceneGpuOcclusionPass::Impl
    {
        using BindingMap = std::unordered_map<BindingKey, nvrhi::BindingSetHandle, BindingHash>;
        struct RetiredState
        {
            uint64_t Submission = 0;
            BindingMap Bindings;
            std::function<void()> PyramidBindings;
        };
        explicit Impl(const SceneGpuOcclusionPassDesc& desc) : Device(desc.Device)
        {
            if (!Device || desc.ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("GPU occlusion requires a device and shader artifact root");
            if (!desc.SharedShaderManager) OwnedShaderManager = CreateScope<ShaderManager>(Device);
            auto* shaders = desc.SharedShaderManager ? desc.SharedShaderManager : OwnedShaderManager.get();
            Pyramid = CreateScope<DepthPyramidPass>(DepthPyramidPassDesc{ .Device = Device,
                .SharedShaderManager = shaders, .ShaderArtifactCacheRoot = desc.ShaderArtifactCacheRoot,
                .Backend = desc.Backend, .bMaximumReduction = true, .BaseDownsample = 4 });
            ShaderArtifactResolver resolver(desc.ShaderArtifactCacheRoot, desc.Backend);
            Shader = resolver.LoadShaderFile(*shaders, ShaderCacheKey{
                .Source = "Modern/SceneGpuOcclusion.hlsl", .Variant = "BASE",
                .Stage = nvrhi::ShaderType::Compute, .EntryPoint = "CS", .TargetProfile = "cs_6_0",
            }, "SceneGpuOcclusion.cs", "Same-frame GPU occlusion");
            nvrhi::BindingLayoutDesc layout;
            layout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(OcclusionConstants)))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
            Layout = Device->createBindingLayout(layout);
            Pipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
                .setComputeShader(Shader).addBindingLayout(Layout));
            if (!Shader || !Layout || !Pipeline)
                throw std::runtime_error("NVRHI failed to create GPU occlusion state");
        }
        nvrhi::IBindingSet* GetBindings(const BindingKey& key)
        {
            const auto found = Bindings.find(key);
            if (found != Bindings.end()) return found->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(OcclusionConstants)))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, key.Buffers[0]))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, key.Buffers[1]))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(2, key.Pyramid))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, key.Buffers[2]))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, key.Buffers[3]));
            auto binding = Device->createBindingSet(desc, Layout);
            if (!binding) throw std::runtime_error("NVRHI failed to bind GPU occlusion resources");
            return Bindings.emplace(key, std::move(binding)).first->second;
        }
        void Retire()
        {
            Retired.push_back({ LastSubmission, std::move(Bindings), Pyramid->CaptureGraphState() });
            Bindings.clear();
            Pyramid->ClearCaches();
        }
        void AddStage(RenderGraph& graph, const char* name, uint32_t operation, TextureHandle pyramid,
            BufferHandle bounds, BufferHandle draws, BufferHandle visible, BufferHandle arguments)
        {
            const glm::uvec4 graphDimensions = FrameConstants.Dimensions;
            const glm::uvec2 pyramidDimensions = { FrameConstants.Counts.z, FrameConstants.Counts.w };
            graph.AddPass<PassData>(name, RenderGraphQueue::Compute,
                [=](RenderGraphBuilder& builder, PassData& data)
                {
                    data.Pyramid = builder.ShaderResource(pyramid);
                    data.Bounds = builder.ShaderResource(bounds);
                    data.Draws = builder.ShaderResource(draws);
                    data.Visible = builder.Write(visible);
                    data.Arguments = builder.ReadWrite(arguments);
                    data.Operation = operation;
                    data.Dimensions = graphDimensions;
                    data.PyramidDimensions = pyramidDimensions;
                },
                [this](RenderGraphContext& context, const PassData& data)
                {
                    OcclusionConstants constants = FrameConstants;
                    constants.Dimensions = data.Dimensions;
                    constants.Counts.z = data.PyramidDimensions.x;
                    constants.Counts.w = data.PyramidDimensions.y;
                    constants.Dimensions.w = data.Operation;
                    BindingKey key{ .Pyramid = context.GetTexture(data.Pyramid, RenderGraphAccess::Read),
                        .Buffers = { context.GetBuffer(data.Bounds, RenderGraphAccess::Read),
                            context.GetBuffer(data.Draws, RenderGraphAccess::Read),
                            context.GetBuffer(data.Visible, RenderGraphAccess::Write),
                            context.GetBuffer(data.Arguments, RenderGraphAccess::ReadWrite) } };
                    auto* commands = context.GetCommandList();
                    nvrhi::ComputeState state;
                    state.setPipeline(Pipeline).addBindingSet(GetBindings(key));
                    commands->setComputeState(state);
                    commands->setPushConstants(&constants, sizeof(constants));
                    const uint32_t count = data.Operation == 0u ? constants.Counts.y : constants.Counts.x;
                    if (count > 0u) commands->dispatch((count + 63u) / 64u, 1u, 1u);
                    bFrameRecorded = true;
                });
        }
        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        Scope<DepthPyramidPass> Pyramid;
        nvrhi::ShaderHandle Shader;
        nvrhi::BindingLayoutHandle Layout;
        nvrhi::ComputePipelineHandle Pipeline;
        BindingMap Bindings;
        std::vector<RetiredState> Retired;
        OcclusionConstants FrameConstants;
        uint64_t LastSubmission = 0;
        bool bFrameRecorded = false;
    };

    SceneGpuOcclusionPass::SceneGpuOcclusionPass(const SceneGpuOcclusionPassDesc& desc) : mImpl(CreateScope<Impl>(desc)) {}
    SceneGpuOcclusionPass::~SceneGpuOcclusionPass() = default;
    void SceneGpuOcclusionPass::PrepareFrame(const RenderCamera& rasterCamera, uint32_t drawCount,
        uint32_t batchCount, uint64_t completedSubmission)
    {
        Collect(completedSubmission);
        mImpl->FrameConstants.ViewProjection = rasterCamera.ViewProjection;
        mImpl->FrameConstants.Counts.x = drawCount;
        mImpl->FrameConstants.Counts.y = batchCount;
    }
    void SceneGpuOcclusionPass::AddPasses(RenderGraph& graph, TextureHandle depth, uint32_t width, uint32_t height,
        BufferHandle bounds, BufferHandle drawData, BufferHandle visibleIndices, BufferHandle indirectArguments)
    {
        if (!depth || !bounds || !drawData || !visibleIndices || !indirectArguments)
            throw std::invalid_argument("GPU occlusion requires the current depth and authoritative scene buffers");
        mImpl->Retire();
        const DepthPyramidOutputs pyramid = mImpl->Pyramid->AddPasses(graph, depth, width, height);
        mImpl->FrameConstants.Counts.z = pyramid.Width;
        mImpl->FrameConstants.Counts.w = pyramid.Height;
        mImpl->FrameConstants.Dimensions = { width, height, pyramid.MipCount, 0u };
        mImpl->AddStage(graph, "GpuOcclusionResetArguments", 0u, pyramid.Depth,
            bounds, drawData, visibleIndices, indirectArguments);
        mImpl->AddStage(graph, "GpuSameFrameOcclusionCull", 1u, pyramid.Depth,
            bounds, drawData, visibleIndices, indirectArguments);
    }
    void SceneGpuOcclusionPass::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        if (mImpl->bFrameRecorded) mImpl->LastSubmission = submission;
        mImpl->bFrameRecorded = false;
    }
    void SceneGpuOcclusionPass::AbortFrame() noexcept { mImpl->bFrameRecorded = false; }
    void SceneGpuOcclusionPass::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mImpl->Retired, [completedSubmission](const Impl::RetiredState& state)
            { return state.Submission <= completedSubmission; });
    }
    void SceneGpuOcclusionPass::Clear(uint64_t completedSubmission)
    { mImpl->Retire(); Collect(completedSubmission); }
    std::function<void()> SceneGpuOcclusionPass::CaptureGraphState()
    {
        return [impl = mImpl.get(), bindings = mImpl->Bindings, retired = mImpl->Retired,
            pyramid = mImpl->Pyramid->CaptureGraphState()]() mutable noexcept
        { impl->Bindings.swap(bindings); impl->Retired.swap(retired); pyramid(); };
    }
    void SceneGpuOcclusionPass::ClearGraphResourceCaches() noexcept
    {
        mImpl->Bindings.clear();
        mImpl->Pyramid->ClearCaches();
        mImpl->Retired.clear();
    }
}
