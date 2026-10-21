#include "Hepch.h"

#include "SceneIndirectCompactionPass.h"

#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    namespace
    {
        constexpr uint32_t c_ArgumentStride = sizeof(nvrhi::DrawIndexedIndirectArguments);
        static_assert(c_ArgumentStride == 20 && sizeof(SceneIndirectGroupMapping) == 8);
        struct Constants { uint32_t Candidates, Groups, CountByteOffset, Operation; };
        struct PassData { BufferHandle Input, Mapping, Output; uint32_t CountByteOffset = 0, Operation = 0; };
        struct BindingKey
        {
            std::array<nvrhi::IBuffer*, 3> Buffers{};
            bool operator==(const BindingKey&) const = default;
        };
        struct BindingHash
        {
            size_t operator()(const BindingKey& key) const noexcept
            {
                size_t hash = 0;
                for (const auto* buffer : key.Buffers)
                    hash ^= reinterpret_cast<size_t>(buffer) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };
        uint32_t Capacity(size_t count)
        {
            if (count > (1u << 20u)) throw std::length_error("Indirect compaction exceeds 1048576 candidates/groups");
            return std::bit_ceil(std::max(static_cast<uint32_t>(count), 64u));
        }
    }

    struct SceneIndirectCompactionPass::Impl
    {
        using BindingMap = std::unordered_map<BindingKey, nvrhi::BindingSetHandle, BindingHash>;
        struct RetiredState
        {
            uint64_t Submission = 0, Bytes = 0;
            nvrhi::BufferHandle Mapping, Output;
            BindingMap Bindings;
        };
        explicit Impl(const SceneIndirectCompactionPassDesc& desc) : Device(desc.Device), DebugName(desc.DebugName)
        {
            if (!Device || desc.ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("Indirect compaction requires a device and shader root");
            if (!desc.SharedShaderManager) OwnedShaderManager = CreateScope<ShaderManager>(Device);
            auto* shaders = desc.SharedShaderManager ? desc.SharedShaderManager : OwnedShaderManager.get();
            ShaderArtifactResolver resolver(desc.ShaderArtifactCacheRoot, desc.Backend);
            Shader = resolver.LoadShaderFile(*shaders, ShaderCacheKey{
                .Source = "Modern/SceneIndirectCompaction.hlsl", .Variant = "BASE",
                .Stage = nvrhi::ShaderType::Compute, .EntryPoint = "CS", .TargetProfile = "cs_6_0",
            }, "SceneIndirectCompaction.cs", "Visible command group compaction");
            nvrhi::BindingLayoutDesc layout;
            layout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants)))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::RawBuffer_UAV(0));
            Layout = Device->createBindingLayout(layout);
            Pipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc().setComputeShader(Shader).addBindingLayout(Layout));
            if (!Shader || !Layout || !Pipeline) throw std::runtime_error("Indirect compaction state creation failed");
        }
        nvrhi::IBindingSet* GetBinding(const BindingKey& key)
        {
            const auto found = Bindings.find(key);
            if (found != Bindings.end()) return found->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Constants)))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, key.Buffers[0]))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, key.Buffers[1]))
                .addItem(nvrhi::BindingSetItem::RawBuffer_UAV(0, key.Buffers[2]));
            auto binding = Device->createBindingSet(desc, Layout);
            if (!binding) throw std::runtime_error("Indirect compaction binding creation failed");
            return Bindings.emplace(key, std::move(binding)).first->second;
        }
        nvrhi::DeviceHandle Device;
        std::string DebugName;
        Scope<ShaderManager> OwnedShaderManager;
        nvrhi::ShaderHandle Shader;
        nvrhi::BindingLayoutHandle Layout;
        nvrhi::ComputePipelineHandle Pipeline;
        nvrhi::BufferHandle MappingBuffer, OutputBuffer;
        uint32_t CandidateCapacity = 0, GroupCapacity = 0, GroupCount = 0;
        std::vector<SceneIndirectGroupMapping> Mapping, SubmittedMapping;
        BindingMap Bindings;
        std::vector<RetiredState> Retired;
        SceneIndirectCompactionStats Stats;
        uint64_t LastSubmission = 0;
        bool bSubmitted = false, bImportStateChanged = false, bFrameRecorded = false, bMappingRecorded = false;
    };

    SceneIndirectCompactionPass::SceneIndirectCompactionPass(const SceneIndirectCompactionPassDesc& desc) : mImpl(CreateScope<Impl>(desc)) {}
    SceneIndirectCompactionPass::~SceneIndirectCompactionPass() = default;
    bool SceneIndirectCompactionPass::PrepareFrame(std::span<const SceneIndirectGroupMapping> mapping,
        uint32_t groupCount, uint64_t completedSubmission)
    {
        auto& impl = *mImpl;
        Collect(completedSubmission);
        const uint32_t candidates = Capacity(mapping.size());
        const uint32_t groups = Capacity(groupCount);
        const bool bChanged = impl.GroupCount != groupCount || impl.Mapping.size() != mapping.size() ||
            !std::equal(mapping.begin(), mapping.end(), impl.Mapping.begin(), impl.Mapping.end());
        if (bChanged)
        {
            std::vector<uint32_t> counts(groupCount, 0u), bases(groupCount, ~0u);
            for (const auto& entry : mapping)
            {
                if (entry.GroupIndex >= groupCount) throw std::invalid_argument("Indirect command group index is out of range");
                if (bases[entry.GroupIndex] != ~0u && bases[entry.GroupIndex] != entry.GroupBase)
                    throw std::invalid_argument("Indirect command group bases disagree");
                bases[entry.GroupIndex] = entry.GroupBase;
                ++counts[entry.GroupIndex];
            }
            std::vector<std::pair<uint32_t, uint32_t>> ranges;
            for (uint32_t group = 0; group < groupCount; ++group)
            {
                if (counts[group] == 0) continue;
                const uint64_t end = uint64_t(bases[group]) + counts[group];
                if (end > mapping.size()) throw std::invalid_argument("Indirect group range exceeds its compact argument capacity");
                ranges.emplace_back(bases[group], static_cast<uint32_t>(end));
            }
            std::sort(ranges.begin(), ranges.end());
            for (size_t range = 1; range < ranges.size(); ++range)
                if (ranges[range].first < ranges[range - 1].second)
                    throw std::invalid_argument("Indirect command group ranges overlap");
        }
        bool bRebuild = impl.bImportStateChanged;
        impl.bImportStateChanged = false;
        if (!impl.OutputBuffer || candidates > impl.CandidateCapacity || groups > impl.GroupCapacity)
        {
            const uint32_t candidateCapacity = std::max(candidates, impl.CandidateCapacity);
            const uint32_t groupCapacity = std::max(groups, impl.GroupCapacity);
            auto mappingBuffer = impl.Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(uint64_t(candidateCapacity) * sizeof(SceneIndirectGroupMapping))
                .setStructStride(sizeof(SceneIndirectGroupMapping)).setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setDebugName("Resident indirect command group mapping"));
            auto output = impl.Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(uint64_t(candidateCapacity) * c_ArgumentStride + uint64_t(groupCapacity) * sizeof(uint32_t))
                .setCanHaveRawViews(true).setCanHaveUAVs(true).setIsDrawIndirectArgs(true)
                .setInitialState(nvrhi::ResourceStates::IndirectArgument).setDebugName("Compacted indexed commands and counts"));
            if (!mappingBuffer || !output) throw std::runtime_error("Indirect compaction buffer allocation failed");
            if (impl.OutputBuffer)
                impl.Retired.push_back({ impl.LastSubmission, impl.Stats.ResidentBytes,
                    impl.MappingBuffer, impl.OutputBuffer, std::move(impl.Bindings) });
            impl.MappingBuffer = std::move(mappingBuffer);
            impl.OutputBuffer = std::move(output);
            impl.CandidateCapacity = candidateCapacity;
            impl.GroupCapacity = groupCapacity;
            impl.SubmittedMapping.clear();
            impl.bSubmitted = false;
            bRebuild = true;
        }
        if (bChanged) impl.Mapping.assign(mapping.begin(), mapping.end());
        impl.SubmittedMapping.reserve(mapping.size());
        impl.GroupCount = groupCount;
        impl.Stats.ResidentBytes = uint64_t(impl.CandidateCapacity) * (c_ArgumentStride + sizeof(SceneIndirectGroupMapping)) +
            uint64_t(impl.GroupCapacity) * sizeof(uint32_t);
        impl.Stats.UploadedBytes = 0;
        impl.Stats.CandidateCount = static_cast<uint32_t>(mapping.size());
        impl.Stats.GroupCount = groupCount;
        return bRebuild;
    }

    BufferHandle SceneIndirectCompactionPass::AddPasses(RenderGraph& graph, BufferHandle inputArguments)
    {
        auto& impl = *mImpl;
        if (!impl.OutputBuffer || !inputArguments || graph.GetBufferDesc(inputArguments).StructStride != c_ArgumentStride ||
            graph.GetBufferDesc(inputArguments).ByteSize < impl.Mapping.size() * c_ArgumentStride)
            throw std::invalid_argument("Indirect compaction graph requires prepared buffers and 20-byte input arguments");
        const auto mapping = graph.ImportBuffer(RenderGraphBufferDesc{ .DebugName = "Indirect group mapping",
            .ByteSize = uint64_t(impl.CandidateCapacity) * sizeof(SceneIndirectGroupMapping), .StructStride = sizeof(SceneIndirectGroupMapping) },
            impl.MappingBuffer, nvrhi::ResourceStates::ShaderResource,
            nvrhi::ResourceStates::ShaderResource);
        const auto output = graph.ImportBuffer(RenderGraphBufferDesc{ .DebugName = "Compacted arguments and counts",
            .ByteSize = uint64_t(impl.CandidateCapacity) * c_ArgumentStride + uint64_t(impl.GroupCapacity) * sizeof(uint32_t) },
            impl.OutputBuffer, nvrhi::ResourceStates::IndirectArgument,
            nvrhi::ResourceStates::IndirectArgument);
        graph.AddPass<PassData>(impl.DebugName + "GroupMappingUpload", RenderGraphQueue::Graphics,
            [mapping](RenderGraphBuilder& builder, PassData& data) { data.Mapping = builder.CopyDest(mapping); },
            [state = mImpl.get()](RenderGraphContext& context, const PassData& data)
            {
                auto* destination = context.GetBuffer(data.Mapping, RenderGraphAccess::Write);
                const auto& current = state->Mapping;
                const auto& submitted = state->SubmittedMapping;
                size_t begin = 0;
                while (begin < std::min(current.size(), submitted.size()) && current[begin] == submitted[begin]) ++begin;
                size_t end = current.size();
                if (current.size() == submitted.size())
                    while (end > begin && current[end - 1] == submitted[end - 1]) --end;
                if (end > begin)
                {
                    const size_t bytes = (end - begin) * sizeof(SceneIndirectGroupMapping);
                    context.GetCommandList()->writeBuffer(destination, current.data() + begin, bytes,
                        begin * sizeof(SceneIndirectGroupMapping));
                    state->Stats.UploadedBytes += bytes;
                }
                state->bMappingRecorded = true;
            });
        const uint32_t countByteOffset = impl.CandidateCapacity * c_ArgumentStride;
        for (uint32_t operation = 0; operation < 2; ++operation)
        {
            graph.AddPass<PassData>(impl.DebugName + (operation == 0 ? "GroupCountReset" : "VisibleCommandCompaction"),
                RenderGraphQueue::Compute,
                [=](RenderGraphBuilder& builder, PassData& data)
                {
                    data.Input = builder.ShaderResource(inputArguments);
                    data.Mapping = builder.ShaderResource(mapping);
                    data.Output = operation == 0 ? builder.Write(output) : builder.ReadWrite(output);
                    data.CountByteOffset = countByteOffset;
                    data.Operation = operation;
                },
                [state = mImpl.get()](RenderGraphContext& context, const PassData& data)
                {
                    BindingKey key{ .Buffers = { context.GetBuffer(data.Input, RenderGraphAccess::Read),
                        context.GetBuffer(data.Mapping, RenderGraphAccess::Read),
                        context.GetBuffer(data.Output, data.Operation == 0 ? RenderGraphAccess::Write : RenderGraphAccess::ReadWrite) } };
                    const Constants constants{ static_cast<uint32_t>(state->Mapping.size()), state->GroupCount,
                        data.CountByteOffset, data.Operation };
                    const uint32_t count = data.Operation == 0 ? constants.Groups : constants.Candidates;
                    if (count > 0)
                    {
                        auto* commands = context.GetCommandList();
                        commands->setComputeState(nvrhi::ComputeState().setPipeline(state->Pipeline).addBindingSet(state->GetBinding(key)));
                        commands->setPushConstants(&constants, sizeof(constants));
                        commands->dispatch((count + 63u) / 64u, 1u, 1u);
                    }
                    state->bFrameRecorded = true;
                });
        }
        return output;
    }

    nvrhi::IBuffer* SceneIndirectCompactionPass::GetBuffer() const noexcept { return mImpl->OutputBuffer; }
    uint32_t SceneIndirectCompactionPass::GetArgumentOffset(uint32_t groupBase) const
    {
        if (groupBase >= mImpl->CandidateCapacity) throw std::out_of_range("Indirect group base is outside capacity");
        return groupBase * c_ArgumentStride;
    }
    uint32_t SceneIndirectCompactionPass::GetCountOffset(uint32_t groupIndex) const
    {
        if (groupIndex >= mImpl->GroupCount) throw std::out_of_range("Indirect count group is outside capacity");
        return mImpl->CandidateCapacity * c_ArgumentStride + groupIndex * sizeof(uint32_t);
    }
    const SceneIndirectCompactionStats& SceneIndirectCompactionPass::GetStats() const noexcept { return mImpl->Stats; }
    void SceneIndirectCompactionPass::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        auto& impl = *mImpl;
        if (impl.bFrameRecorded)
        {
            impl.LastSubmission = submission;
            impl.bSubmitted = true;
        }
        if (impl.bMappingRecorded) impl.SubmittedMapping = impl.Mapping;
        impl.bMappingRecorded = impl.bFrameRecorded = false;
    }
    void SceneIndirectCompactionPass::AbortFrame() noexcept { mImpl->bMappingRecorded = mImpl->bFrameRecorded = false; }
    void SceneIndirectCompactionPass::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mImpl->Retired, [completedSubmission](const Impl::RetiredState& state)
            { return state.Submission <= completedSubmission; });
        mImpl->Stats.RetiredBytes = 0;
        for (const auto& state : mImpl->Retired) mImpl->Stats.RetiredBytes += state.Bytes;
    }
    void SceneIndirectCompactionPass::Clear(uint64_t completedSubmission)
    {
        auto& impl = *mImpl;
        if (impl.OutputBuffer)
            impl.Retired.push_back({ impl.LastSubmission, impl.Stats.ResidentBytes,
                std::move(impl.MappingBuffer), std::move(impl.OutputBuffer), std::move(impl.Bindings) });
        impl.Mapping.clear(); impl.SubmittedMapping.clear();
        impl.CandidateCapacity = impl.GroupCapacity = impl.GroupCount = 0;
        impl.bSubmitted = impl.bFrameRecorded = impl.bMappingRecorded = false;
        impl.bImportStateChanged = true;
        impl.Stats = {};
        Collect(completedSubmission);
    }
    std::function<void()> SceneIndirectCompactionPass::CaptureGraphState()
    {
        return [impl = mImpl.get(), bindings = mImpl->Bindings, retired = mImpl->Retired]() mutable noexcept
        { impl->Bindings.swap(bindings); impl->Retired.swap(retired); };
    }
    void SceneIndirectCompactionPass::ClearGraphResourceCaches() noexcept
    {
        mImpl->Bindings.clear();
        for (auto& retired : mImpl->Retired) retired.Bindings.clear();
    }
}
