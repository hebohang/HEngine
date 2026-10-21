#include "Hepch.h"

#include "SceneShadowGpuSubmission.h"

#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        struct alignas(16) ShadowCullConstants
        {
            std::array<glm::vec4, 6> Planes{};
            uint32_t DrawCount = 0;
            uint32_t Padding[3]{};
        };
        static_assert(sizeof(ShadowCullConstants) == 112);
        static_assert(sizeof(SceneShadowGpuDraw) == 16);
        static_assert(sizeof(nvrhi::DrawIndexedIndirectArguments) == 20);

        uint32_t GrowCount(size_t count)
        {
            if (count > 1048576) throw std::length_error("GPU shadow candidate budget exceeded");
            uint32_t capacity = 64;
            while (capacity < count) capacity *= 2;
            return capacity;
        }

        template<typename T>
        uint64_t UploadChanged(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer,
            const std::vector<T>& current, const std::vector<T>& submitted)
        {
            uint64_t bytes = 0;
            size_t index = 0;
            const auto changed = [&](size_t row)
            { return row >= submitted.size() || std::memcmp(&current[row], &submitted[row], sizeof(T)) != 0; };
            while (index < current.size())
            {
                if (!changed(index)) { ++index; continue; }
                const size_t begin = index++;
                while (index < current.size() && changed(index)) ++index;
                const size_t length = (index - begin) * sizeof(T);
                commandList->writeBuffer(buffer, current.data() + begin, length, begin * sizeof(T));
                bytes += length;
            }
            return bytes;
        }

        struct UploadData { BufferHandle Draws; BufferHandle Templates; BufferHandle Groups; };
        struct CompactionConstants { uint32_t Candidates, Groups, CountByteOffset, Operation; };
        struct CompactionData
        {
            BufferHandle Groups;
            std::vector<BufferHandle> Arguments, Outputs;
            uint32_t CountByteOffset = 0, Operation = 0;
        };
        struct ResetData { BufferHandle Templates; BufferHandle Arguments; };
        struct CullData
        {
            BufferHandle Bounds;
            BufferHandle Draws;
            BufferHandle Visible;
            BufferHandle Arguments;
        };
    }

    struct SceneShadowGpuSubmission::Impl
    {
        struct View
        {
            nvrhi::BufferHandle Visible;
            nvrhi::BufferHandle Arguments;
            nvrhi::BufferHandle Compacted;
            nvrhi::BindingSetHandle Binding;
            nvrhi::BindingSetHandle CompactionBinding;
            nvrhi::IBuffer* BoundBounds = nullptr;
            ShadowCullConstants Constants;
            bool bEnabled = false;
        };
        struct Retired
        {
            uint64_t Submission = 0;
            uint64_t Bytes = 0;
            nvrhi::BufferHandle Draws;
            nvrhi::BufferHandle Templates;
            nvrhi::BufferHandle Groups;
            std::vector<View> Views;
        };

        explicit Impl(const SceneShadowGpuSubmissionDesc& desc)
            : Device(desc.Device)
            , OwnedShaders(desc.SharedShaderManager ? nullptr : CreateScope<ShaderManager>(desc.Device))
        {
            if (!Device || desc.ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("GPU shadow submission requires device and shader root");
            ShaderManager& shaders = desc.SharedShaderManager ? *desc.SharedShaderManager : *OwnedShaders;
            ShaderArtifactResolver resolver(desc.ShaderArtifactCacheRoot, desc.Backend);
            Shader = resolver.LoadShaderFile(shaders, ShaderCacheKey{
                .Source = "Modern/SceneShadowGpuCull.hlsl", .Variant = "BASE",
                .Stage = nvrhi::ShaderType::Compute, .EntryPoint = "CS", .TargetProfile = "cs_6_0",
            }, "SceneShadowGpuCull.cs", "Shadow view GPU frustum culling");
            nvrhi::BindingLayoutDesc layout;
            layout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ShadowCullConstants)))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
            Layout = Device->createBindingLayout(layout);
            Pipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
                .setComputeShader(Shader).addBindingLayout(Layout));
            if (!Layout || !Pipeline) throw std::runtime_error("GPU shadow cull pipeline creation failed");
            CompactionShader = resolver.LoadShaderFile(shaders, ShaderCacheKey{
                .Source = "Modern/SceneIndirectCompaction.hlsl", .Variant = "BASE",
                .Stage = nvrhi::ShaderType::Compute, .EntryPoint = "CS", .TargetProfile = "cs_6_0",
            }, "SceneIndirectCompaction.cs", "Shadow compatible command compaction");
            nvrhi::BindingLayoutDesc compactLayout;
            compactLayout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(CompactionConstants)))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::RawBuffer_UAV(0));
            CompactionLayout = Device->createBindingLayout(compactLayout);
            CompactionPipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
                .setComputeShader(CompactionShader).addBindingLayout(CompactionLayout));
            if (!CompactionShader || !CompactionLayout || !CompactionPipeline)
                throw std::runtime_error("GPU shadow compaction state creation failed");
        }

        nvrhi::BufferHandle CreateBuffer(uint64_t bytes, uint32_t stride, bool bUav,
            bool bIndirect, const char* name, bool bVertex = false)
        {
            auto buffer = Device->createBuffer(nvrhi::BufferDesc().setByteSize(bytes)
                .setStructStride(stride).setCanHaveUAVs(bUav).setIsDrawIndirectArgs(bIndirect)
                .setIsVertexBuffer(bVertex)
                .setInitialState(nvrhi::ResourceStates::Common).setDebugName(name));
            if (!buffer) throw std::runtime_error("GPU shadow buffer allocation failed");
            return buffer;
        }

        nvrhi::IDevice* Device;
        Scope<ShaderManager> OwnedShaders;
        nvrhi::ShaderHandle Shader;
        nvrhi::BindingLayoutHandle Layout;
        nvrhi::ComputePipelineHandle Pipeline;
        nvrhi::ShaderHandle CompactionShader;
        nvrhi::BindingLayoutHandle CompactionLayout;
        nvrhi::ComputePipelineHandle CompactionPipeline;
        nvrhi::BufferHandle DrawBuffer;
        nvrhi::BufferHandle TemplateBuffer;
        nvrhi::BufferHandle GroupBuffer;
        std::vector<View> Views;
        std::vector<Retired> RetiredResources;
        std::vector<SceneShadowGpuDraw> Draws;
        std::vector<SceneShadowGpuDraw> SubmittedDraws;
        std::vector<nvrhi::DrawIndexedIndirectArguments> Templates;
        std::vector<nvrhi::DrawIndexedIndirectArguments> SubmittedTemplates;
        std::vector<SceneIndirectGroupMapping> Groups, SubmittedGroups;
        SceneShadowGpuSubmissionStats Stats;
        uint32_t DrawCapacity = 0;
        uint32_t BatchCapacity = 0;
        uint32_t GroupCount = 0;
        uint64_t LastSubmission = 0;
        bool bUploadRecorded = false;
        bool bInputsDirty = true;
        bool bResourcesSubmitted = false;
        bool bImportStateChanged = false;
    };

    SceneShadowGpuSubmission::SceneShadowGpuSubmission(const SceneShadowGpuSubmissionDesc& desc)
        : mImpl(CreateScope<Impl>(desc)) {}
    SceneShadowGpuSubmission::~SceneShadowGpuSubmission() = default;

    bool SceneShadowGpuSubmission::PrepareFrame(std::span<const SceneShadowGpuDraw> draws,
        std::span<const nvrhi::DrawIndexedIndirectArguments> arguments,
        uint32_t viewCount, uint64_t completedSubmission,
        std::span<const SceneIndirectGroupMapping> groups, uint32_t groupCount)
    {
        Impl& impl = *mImpl;
        Collect(completedSubmission);
        if (viewCount == 0 || viewCount > 84)
            throw std::invalid_argument("GPU shadow view count is outside 1..84");
        if (groups.size() != arguments.size() || groupCount > arguments.size())
            throw std::invalid_argument("GPU shadow compaction groups must map every argument");
        const bool bGroupsChanged = impl.GroupCount != groupCount || impl.Groups.size() != groups.size() ||
            !std::equal(groups.begin(), groups.end(), impl.Groups.begin(), impl.Groups.end());
        if (bGroupsChanged)
        {
            std::vector<uint32_t> counts(groupCount, 0u), bases(groupCount, ~0u);
            for (const auto& group : groups)
            {
                if (group.GroupIndex >= groupCount) throw std::invalid_argument("Shadow group index exceeds its budget");
                auto& base = bases[group.GroupIndex];
                if (base != ~0u && base != group.GroupBase) throw std::invalid_argument("Shadow group bases disagree");
                base = group.GroupBase;
                ++counts[group.GroupIndex];
            }
            std::vector<std::pair<uint32_t, uint32_t>> ranges;
            for (uint32_t group = 0; group < groupCount; ++group)
            {
                if (!counts[group]) continue;
                const uint64_t end = uint64_t(bases[group]) + counts[group];
                if (end > arguments.size()) throw std::invalid_argument("Shadow compact range exceeds its argument budget");
                ranges.emplace_back(bases[group], static_cast<uint32_t>(end));
            }
            std::sort(ranges.begin(), ranges.end());
            for (size_t i = 1; i < ranges.size(); ++i)
                if (ranges[i].first < ranges[i - 1].second) throw std::invalid_argument("Shadow compact group ranges overlap");
        }
        const bool bInputChanged = draws.size() != impl.Draws.size() || arguments.size() != impl.Templates.size() ||
            (!draws.empty() && std::memcmp(draws.data(), impl.Draws.data(), draws.size_bytes()) != 0) ||
            (!arguments.empty() && std::memcmp(arguments.data(), impl.Templates.data(), arguments.size_bytes()) != 0);
        // Validate the atomic append ranges once on the CPU. Overlapping or
        // undersized ranges would otherwise corrupt a neighboring batch.
        if (bInputChanged)
        {
        std::vector<uint32_t> counts(arguments.size(), 0);
        std::vector<uint32_t> bases(arguments.size(), ~0u);
        for (const auto& draw : draws)
        {
            if (draw.BatchIndex >= arguments.size()) throw std::invalid_argument("GPU shadow batch index is invalid");
            uint32_t& base = bases[draw.BatchIndex];
            if (base != ~0u && base != draw.VisibleBase)
                throw std::invalid_argument("GPU shadow batch visible bases disagree");
            base = draw.VisibleBase;
            ++counts[draw.BatchIndex];
        }
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        for (size_t batch = 0; batch < arguments.size(); ++batch)
        {
            if (arguments[batch].instanceCount != 0 ||
                (counts[batch] != 0 && arguments[batch].startInstanceLocation != bases[batch]))
                throw std::invalid_argument("GPU shadow template instance count/base disagrees with its visible range");
            if (counts[batch] == 0) continue;
            const uint64_t end = uint64_t(bases[batch]) + counts[batch];
            if (end > draws.size()) throw std::invalid_argument("GPU shadow visible range exceeds candidates");
            ranges.emplace_back(bases[batch], static_cast<uint32_t>(end));
        }
        std::sort(ranges.begin(), ranges.end());
        for (size_t range = 1; range < ranges.size(); ++range)
            if (ranges[range].first < ranges[range - 1].second)
                throw std::invalid_argument("GPU shadow visible batch ranges overlap");
        }
        const uint32_t drawCapacity = std::max(impl.DrawCapacity, GrowCount(draws.size()));
        const uint32_t batchCapacity = std::max(impl.BatchCapacity, GrowCount(arguments.size()));
        const bool bAllocate = !impl.DrawBuffer || drawCapacity != impl.DrawCapacity ||
            batchCapacity != impl.BatchCapacity || viewCount != impl.Views.size();
        bool bChanged = bAllocate || impl.bImportStateChanged;
        impl.bImportStateChanged = false;
        if (bAllocate)
        {
            auto newDraws = impl.CreateBuffer(uint64_t(drawCapacity) * sizeof(SceneShadowGpuDraw),
                sizeof(SceneShadowGpuDraw), false, false, "Shadow candidate object rows");
            auto newTemplates = impl.CreateBuffer(uint64_t(batchCapacity) * sizeof(nvrhi::DrawIndexedIndirectArguments),
                sizeof(nvrhi::DrawIndexedIndirectArguments), false, false, "Shadow argument templates");
            auto newGroups = impl.CreateBuffer(uint64_t(batchCapacity) * sizeof(SceneIndirectGroupMapping),
                sizeof(SceneIndirectGroupMapping), false, false, "Shadow shared compatible group mapping");
            std::vector<Impl::View> views(viewCount);
            for (auto& view : views)
            {
                view.Visible = impl.CreateBuffer(uint64_t(drawCapacity) * sizeof(uint32_t),
                    sizeof(uint32_t), true, false, "Shadow view visible objects", true);
                view.Arguments = impl.CreateBuffer(uint64_t(batchCapacity) * sizeof(nvrhi::DrawIndexedIndirectArguments),
                    sizeof(nvrhi::DrawIndexedIndirectArguments), true, true, "Shadow view indirect arguments");
                view.Compacted = impl.Device->createBuffer(nvrhi::BufferDesc()
                    .setByteSize(uint64_t(batchCapacity) * 24u).setCanHaveRawViews(true).setCanHaveUAVs(true)
                    .setIsDrawIndirectArgs(true).setInitialState(nvrhi::ResourceStates::IndirectArgument)
                    .setDebugName("Shadow compact indexed arguments and group counts"));
                if (!view.Compacted) throw std::runtime_error("GPU shadow compact buffer allocation failed");
            }
            if (impl.DrawBuffer)
                impl.RetiredResources.push_back({ impl.LastSubmission, impl.Stats.ResidentBytes,
                    impl.DrawBuffer, impl.TemplateBuffer, impl.GroupBuffer, std::move(impl.Views) });
            impl.DrawBuffer = std::move(newDraws);
            impl.TemplateBuffer = std::move(newTemplates);
            impl.GroupBuffer = std::move(newGroups);
            impl.Views = std::move(views);
            impl.DrawCapacity = drawCapacity;
            impl.BatchCapacity = batchCapacity;
            impl.SubmittedDraws.clear();
            impl.SubmittedTemplates.clear();
            impl.SubmittedGroups.clear();
            impl.bResourcesSubmitted = false;
        }
        if (bInputChanged)
        {
            impl.Draws.assign(draws.begin(), draws.end());
            impl.Templates.assign(arguments.begin(), arguments.end());
        }
        // Preallocate commit storage outside the noexcept submission callback.
        impl.SubmittedDraws.reserve(draws.size());
        impl.SubmittedTemplates.reserve(arguments.size());
        if (bGroupsChanged) impl.Groups.assign(groups.begin(), groups.end());
        impl.SubmittedGroups.reserve(groups.size());
        impl.GroupCount = groupCount;
        impl.bInputsDirty |= bInputChanged || bAllocate;
        impl.Stats = {};
        impl.Stats.CandidateDraws = static_cast<uint32_t>(draws.size());
        impl.Stats.BatchCount = static_cast<uint32_t>(arguments.size());
        impl.Stats.GroupCount = groupCount;
        impl.Stats.ResidentBytes = uint64_t(drawCapacity) * (sizeof(SceneShadowGpuDraw) + viewCount * sizeof(uint32_t)) +
            uint64_t(batchCapacity) * (sizeof(nvrhi::DrawIndexedIndirectArguments) * (viewCount + 1u) +
                sizeof(SceneIndirectGroupMapping) + viewCount * 24u);
        for (const auto& retired : impl.RetiredResources) impl.Stats.RetiredBytes += retired.Bytes;
        for (auto& view : impl.Views) view.bEnabled = false;
        impl.bUploadRecorded = false;
        return bChanged;
    }

    void SceneShadowGpuSubmission::SetView(uint32_t viewIndex, const glm::mat4& viewProjection, bool bEnabled)
    {
        auto& view = mImpl->Views.at(viewIndex);
        const glm::mat4 rows = glm::transpose(viewProjection);
        view.Constants.Planes = { rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1],
            rows[3] - rows[1], rows[2], rows[3] - rows[2] };
        for (auto& plane : view.Constants.Planes)
        {
            const float length = glm::length(glm::vec3(plane));
            plane = std::isfinite(length) && length > 1e-8f ? plane / length : glm::vec4(0.0f);
        }
        view.Constants.DrawCount = static_cast<uint32_t>(mImpl->Draws.size());
        view.bEnabled = bEnabled;
    }

    std::vector<SceneShadowGpuViewResources> SceneShadowGpuSubmission::AddPasses(
        RenderGraph& graph, BufferHandle authoritativeBounds, bool bCompact)
    {
        Impl& impl = *mImpl;
        if (!impl.DrawBuffer || !authoritativeBounds)
            throw std::logic_error("GPU shadow graph requires prepared input buffers");
        const auto initial = impl.bResourcesSubmitted ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common;
        const auto draws = graph.ImportBuffer(RenderGraphBufferDesc{
            .DebugName = "Shadow candidate rows", .ByteSize = uint64_t(impl.DrawCapacity) * sizeof(SceneShadowGpuDraw),
            .StructStride = sizeof(SceneShadowGpuDraw),
        }, impl.DrawBuffer, initial, nvrhi::ResourceStates::ShaderResource);
        const auto templates = graph.ImportBuffer(RenderGraphBufferDesc{
            .DebugName = "Shadow argument templates", .ByteSize = uint64_t(impl.BatchCapacity) * sizeof(nvrhi::DrawIndexedIndirectArguments),
            .StructStride = sizeof(nvrhi::DrawIndexedIndirectArguments),
        }, impl.TemplateBuffer, initial, nvrhi::ResourceStates::ShaderResource);
        const auto groups = graph.ImportBuffer(RenderGraphBufferDesc{
            .DebugName = "Shadow shared compatible groups", .ByteSize = uint64_t(impl.BatchCapacity) * sizeof(SceneIndirectGroupMapping),
            .StructStride = sizeof(SceneIndirectGroupMapping),
        }, impl.GroupBuffer, initial, nvrhi::ResourceStates::ShaderResource);
        graph.AddPass<UploadData>("ShadowGpuInputUpload", RenderGraphQueue::Graphics,
            [draws, templates, groups](RenderGraphBuilder& builder, UploadData& data)
            { data.Draws = builder.CopyDest(draws); data.Templates = builder.CopyDest(templates); data.Groups = builder.CopyDest(groups); },
            [state = mImpl.get()](RenderGraphContext& context, const UploadData& data)
            {
                auto* commandList = context.GetCommandList();
                auto* draws = context.GetBuffer(data.Draws, RenderGraphAccess::Write);
                auto* templates = context.GetBuffer(data.Templates, RenderGraphAccess::Write);
                auto* groups = context.GetBuffer(data.Groups, RenderGraphAccess::Write);
                if (state->bInputsDirty)
                {
                    state->Stats.UploadedBytes += UploadChanged(commandList, draws, state->Draws, state->SubmittedDraws);
                    state->Stats.UploadedBytes += UploadChanged(commandList, templates, state->Templates, state->SubmittedTemplates);
                }
                state->Stats.UploadedBytes += UploadChanged(commandList, groups, state->Groups, state->SubmittedGroups);
                state->bUploadRecorded = true;
            });
        std::vector<SceneShadowGpuViewResources> result;
        result.reserve(impl.Views.size());
        for (uint32_t index = 0; index < impl.Views.size(); ++index)
        {
            auto& view = impl.Views[index];
            SceneShadowGpuViewResources resources;
            resources.VisibleObjects = graph.ImportBuffer(RenderGraphBufferDesc{
                .DebugName = "Shadow view visible list", .ByteSize = uint64_t(impl.DrawCapacity) * sizeof(uint32_t),
                .StructStride = sizeof(uint32_t),
            }, view.Visible, impl.bResourcesSubmitted ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::VertexBuffer);
            resources.IndirectArguments = graph.ImportBuffer(RenderGraphBufferDesc{
                .DebugName = "Shadow view indirect arguments", .ByteSize = uint64_t(impl.BatchCapacity) * sizeof(nvrhi::DrawIndexedIndirectArguments),
                .StructStride = sizeof(nvrhi::DrawIndexedIndirectArguments),
            }, view.Arguments, impl.bResourcesSubmitted ? nvrhi::ResourceStates::IndirectArgument : nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::IndirectArgument);
            if (bCompact)
                resources.CompactedArguments = graph.ImportBuffer(RenderGraphBufferDesc{
                    .DebugName = "Shadow view compacted arguments and counts", .ByteSize = uint64_t(impl.BatchCapacity) * 24u,
                }, view.Compacted, nvrhi::ResourceStates::IndirectArgument, nvrhi::ResourceStates::IndirectArgument);
            graph.AddPass<ResetData>("ShadowGpuArgumentResetView" + std::to_string(index), RenderGraphQueue::Graphics,
                [templates, resources](RenderGraphBuilder& builder, ResetData& data)
                { data.Templates = builder.CopySource(templates); data.Arguments = builder.CopyDest(resources.IndirectArguments); },
                [state = mImpl.get(), index](RenderGraphContext& context, const ResetData& data)
                {
                    auto* source = context.GetBuffer(data.Templates, RenderGraphAccess::Read);
                    auto* destination = context.GetBuffer(data.Arguments, RenderGraphAccess::Write);
                    if (state->Views[index].bEnabled && !state->Templates.empty())
                        context.GetCommandList()->copyBuffer(destination, 0, source, 0,
                            state->Templates.size() * sizeof(nvrhi::DrawIndexedIndirectArguments));
                });
            graph.AddPass<CullData>("ShadowGpuFrustumCullView" + std::to_string(index), RenderGraphQueue::Compute,
                [authoritativeBounds, draws, resources](RenderGraphBuilder& builder, CullData& data)
                {
                    data.Bounds = builder.ShaderResource(authoritativeBounds);
                    data.Draws = builder.ShaderResource(draws);
                    data.Visible = builder.UnorderedAccess(resources.VisibleObjects);
                    data.Arguments = builder.UnorderedAccess(resources.IndirectArguments);
                },
                [state = mImpl.get(), index](RenderGraphContext& context, const CullData& data)
                {
                    auto* bounds = context.GetBuffer(data.Bounds, RenderGraphAccess::Read);
                    auto* candidates = context.GetBuffer(data.Draws, RenderGraphAccess::Read);
                    auto* visible = context.GetBuffer(data.Visible, RenderGraphAccess::Write);
                    auto* arguments = context.GetBuffer(data.Arguments, RenderGraphAccess::ReadWrite);
                    auto& view = state->Views[index];
                    if (!view.bEnabled || state->Draws.empty()) return;
                    if (!view.Binding || view.BoundBounds != bounds)
                    {
                        nvrhi::BindingSetDesc desc;
                        desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ShadowCullConstants)))
                            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, bounds))
                            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, candidates))
                            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, visible))
                            .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, arguments));
                        view.Binding = state->Device->createBindingSet(desc, state->Layout);
                        if (!view.Binding) throw std::runtime_error("GPU shadow compute binding failed");
                        view.BoundBounds = bounds;
                    }
                    auto* commandList = context.GetCommandList();
                    commandList->setComputeState(nvrhi::ComputeState().setPipeline(state->Pipeline).addBindingSet(view.Binding));
                    commandList->setPushConstants(&view.Constants, sizeof(view.Constants));
                    commandList->dispatch((view.Constants.DrawCount + 63u) / 64u, 1, 1);
                    ++state->Stats.CulledViews;
                });
            result.push_back(resources);
        }
        if (bCompact)
        {
            const uint32_t countByteOffset = impl.BatchCapacity * sizeof(nvrhi::DrawIndexedIndirectArguments);
            for (uint32_t operation = 0; operation < 2; ++operation)
                graph.AddPass<CompactionData>(operation == 0 ? "ShadowGpuAllViewGroupCountReset" : "ShadowGpuAllViewCommandCompaction",
                    RenderGraphQueue::Compute,
                    [groups, result, countByteOffset, operation](RenderGraphBuilder& builder, CompactionData& data)
                    {
                        data.Groups = builder.ShaderResource(groups);
                        data.CountByteOffset = countByteOffset;
                        data.Operation = operation;
                        for (const auto& view : result)
                        {
                            data.Arguments.push_back(builder.ShaderResource(view.IndirectArguments));
                            data.Outputs.push_back(operation == 0 ? builder.Write(view.CompactedArguments) : builder.ReadWrite(view.CompactedArguments));
                        }
                    },
                    [state = mImpl.get()](RenderGraphContext& context, const CompactionData& data)
                    {
                        auto* groups = context.GetBuffer(data.Groups, RenderGraphAccess::Read);
                        const CompactionConstants constants{ static_cast<uint32_t>(state->Templates.size()),
                            state->GroupCount, data.CountByteOffset, data.Operation };
                        const uint32_t count = data.Operation == 0 ? constants.Groups : constants.Candidates;
                        if (!count) return;
                        auto* commands = context.GetCommandList();
                        for (uint32_t i = 0; i < state->Views.size(); ++i)
                        {
                            auto& view = state->Views[i];
                            auto* arguments = context.GetBuffer(data.Arguments[i], RenderGraphAccess::Read);
                            auto* output = context.GetBuffer(data.Outputs[i], data.Operation == 0 ? RenderGraphAccess::Write : RenderGraphAccess::ReadWrite);
                            if (!view.bEnabled) continue;
                            if (!view.CompactionBinding)
                            {
                                nvrhi::BindingSetDesc binding;
                                binding.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(CompactionConstants)))
                                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, arguments))
                                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, groups))
                                    .addItem(nvrhi::BindingSetItem::RawBuffer_UAV(0, output));
                                view.CompactionBinding = state->Device->createBindingSet(binding, state->CompactionLayout);
                                if (!view.CompactionBinding) throw std::runtime_error("Shadow compact binding creation failed");
                            }
                            commands->setComputeState(nvrhi::ComputeState().setPipeline(state->CompactionPipeline)
                                .addBindingSet(view.CompactionBinding));
                            commands->setPushConstants(&constants, sizeof(constants));
                            commands->dispatch((count + 63u) / 64u, 1u, 1u);
                            if (data.Operation == 1) ++state->Stats.CompactedViews;
                        }
                    });
        }
        return result;
    }

    nvrhi::IBuffer* SceneShadowGpuSubmission::GetVisibleBuffer(uint32_t viewIndex) const { return mImpl->Views.at(viewIndex).Visible; }
    nvrhi::IBuffer* SceneShadowGpuSubmission::GetIndirectBuffer(uint32_t viewIndex) const { return mImpl->Views.at(viewIndex).Arguments; }
    nvrhi::IBuffer* SceneShadowGpuSubmission::GetCompactedBuffer(uint32_t viewIndex) const { return mImpl->Views.at(viewIndex).Compacted; }
    uint32_t SceneShadowGpuSubmission::GetCountOffset(uint32_t groupIndex) const
    {
        if (groupIndex >= mImpl->GroupCount) throw std::out_of_range("Shadow compact count group is out of range");
        return mImpl->BatchCapacity * sizeof(nvrhi::DrawIndexedIndirectArguments) + groupIndex * sizeof(uint32_t);
    }
    const SceneShadowGpuSubmissionStats& SceneShadowGpuSubmission::GetStats() const noexcept { return mImpl->Stats; }

    void SceneShadowGpuSubmission::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        auto& impl = *mImpl;
        impl.LastSubmission = submission;
        if (impl.bUploadRecorded)
        {
            impl.SubmittedGroups = impl.Groups;
            if (impl.bInputsDirty)
            {
                impl.SubmittedDraws = impl.Draws;
                impl.SubmittedTemplates = impl.Templates;
                impl.bInputsDirty = false;
            }
            if (!impl.bResourcesSubmitted)
            { impl.bResourcesSubmitted = true; impl.bImportStateChanged = true; }
        }
        impl.bUploadRecorded = false;
    }

    void SceneShadowGpuSubmission::AbortFrame() noexcept { mImpl->bUploadRecorded = false; }

    void SceneShadowGpuSubmission::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mImpl->RetiredResources, [completedSubmission](const Impl::Retired& entry)
        { return entry.Submission <= completedSubmission; });
    }

    std::function<void()> SceneShadowGpuSubmission::CaptureGraphState()
    {
        struct BindingState { nvrhi::BindingSetHandle Cull, Compact; nvrhi::IBuffer* Bounds; };
        std::vector<BindingState> bindings;
        bindings.reserve(mImpl->Views.size());
        for (const auto& view : mImpl->Views) bindings.push_back({ view.Binding, view.CompactionBinding, view.BoundBounds });
        return [impl = mImpl.get(), bindings = std::move(bindings), retired = mImpl->RetiredResources]() mutable noexcept
        {
            for (size_t i = 0; i < std::min(bindings.size(), impl->Views.size()); ++i)
            {
                std::swap(bindings[i].Cull, impl->Views[i].Binding);
                std::swap(bindings[i].Compact, impl->Views[i].CompactionBinding);
                std::swap(bindings[i].Bounds, impl->Views[i].BoundBounds);
            }
            impl->RetiredResources.swap(retired);
        };
    }
    void SceneShadowGpuSubmission::ClearGraphResourceCaches() noexcept
    {
        for (auto& view : mImpl->Views)
        { view.Binding = nullptr; view.CompactionBinding = nullptr; view.BoundBounds = nullptr; }
        for (auto& retired : mImpl->RetiredResources)
            for (auto& view : retired.Views)
            { view.Binding = nullptr; view.CompactionBinding = nullptr; view.BoundBounds = nullptr; }
    }
}
