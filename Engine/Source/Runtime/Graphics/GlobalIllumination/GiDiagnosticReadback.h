#pragma once

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace HEngine
{
    // Diagnostic only. Each instance owns counters and a bounded readback ring.
    // Rows are emitted only after completedSubmission covers the actual copy.
    class GiDiagnosticReadback final
    {
    public:
        static constexpr uint32_t CounterCount = 8;
        GiDiagnosticReadback(nvrhi::IDevice* device, const char* name) : mDevice(device)
        {
            if (!device) throw std::invalid_argument("GI diagnostics require a device");
            const char* directory = std::getenv("HE_GI_COUNTERS_DIR");
            bEnabled = directory && *directory;
            mGpu = device->createBuffer(nvrhi::BufferDesc().setByteSize(CounterCount * 4)
                .setStructStride(4).setCanHaveUAVs(true).setInitialState(nvrhi::ResourceStates::Common)
                .setDebugName(std::string(name) + " diagnostic counters"));
            if (!mGpu) throw std::runtime_error("GI counter allocation failed");
            if (!bEnabled) return;
            static std::atomic<uint64_t> instance{0};
            const auto path = std::filesystem::path(directory) /
                (std::string(name) + "-" + std::to_string(++instance) + ".csv");
            std::filesystem::create_directories(path.parent_path());
            if (std::filesystem::exists(path)) throw std::runtime_error("Refusing to overwrite GI diagnostic CSV");
            mOutput.open(path);
            if (!mOutput) throw std::runtime_error("Cannot open GI diagnostic CSV");
            mOutput << "submission,prepareSequence,status,c0,c1,c2,c3,c4,c5,c6,c7\n";
            for (auto& slot : mSlots)
            {
                slot.Buffer = device->createBuffer(nvrhi::BufferDesc().setByteSize(CounterCount * 4)
                    .setCpuAccess(nvrhi::CpuAccessMode::Read).setInitialState(nvrhi::ResourceStates::CopyDest)
                    .setDebugName(std::string(name) + " diagnostic readback"));
                if (!slot.Buffer) throw std::runtime_error("GI diagnostic readback allocation failed");
            }
        }
        nvrhi::IBuffer* GetBuffer() const noexcept { return mGpu; }
        bool IsRecording() const noexcept { return bEnabled && mCurrent >= 0; }
        void Prepare(uint64_t completedSubmission)
        {
            Collect(completedSubmission);
            Abort();
            ++mSequence;
            if (!bEnabled) return;
            for (size_t i = 0; i < mSlots.size(); ++i)
                if (mSlots[i].Submission == 0) { mCurrent = static_cast<int>(i); break; }
            if (mCurrent < 0) mOutput << "0," << mSequence << ",ring_full,-1,-1,-1,-1,-1,-1,-1,-1\n";
        }
        void NotifySubmitted(uint64_t submission) noexcept
        {
            if (mCurrent >= 0 && bCopied)
            {
                mSlots[mCurrent].Submission = submission;
                mSlots[mCurrent].Sequence = mSequence;
            }
            mCurrent = -1; bCopied = false;
        }
        void Abort() noexcept { mCurrent = -1; bCopied = false; }
        void Collect(uint64_t completedSubmission)
        {
            if (!bEnabled) return;
            // Slots may recycle in a different order; submission IDs are authoritative.
            for (auto& slot : mSlots)
            {
                if (!slot.Submission || slot.Submission > completedSubmission) continue;
                const void* data = mDevice->mapBuffer(slot.Buffer, nvrhi::CpuAccessMode::Read);
                if (!data) throw std::runtime_error("Completed GI diagnostic map failed");
                std::array<uint32_t, CounterCount> values{};
                std::memcpy(values.data(), data, sizeof(values));
                mDevice->unmapBuffer(slot.Buffer);
                mOutput << slot.Submission << ',' << slot.Sequence << ",complete";
                for (auto value : values) mOutput << ',' << value;
                mOutput << '\n';
                slot.Submission = 0;
            }
            mOutput.flush();
            if (!mOutput) throw std::runtime_error("GI diagnostic CSV write failed");
        }
        BufferHandle AddReset(RenderGraph& graph, const char* name)
        {
            const auto handle = graph.ImportBuffer(RenderGraphBufferDesc{
                .DebugName = std::string(name) + " counters", .ByteSize = CounterCount * 4, .StructStride = 4 },
                mGpu, nvrhi::ResourceStates::Common, nvrhi::ResourceStates::Common);
            if (!bEnabled) return handle;
            graph.AddPass<BufferHandle>(std::string(name) + "CounterReset", RenderGraphQueue::Copy,
                [handle](RenderGraphBuilder& builder, BufferHandle& data) { data = builder.CopyDest(handle); },
                [this](RenderGraphContext& context, const BufferHandle& data)
                {
                    if (!IsRecording()) return;
                    constexpr std::array<uint32_t, CounterCount> zeros{};
                    context.GetCommandList()->writeBuffer(context.GetBuffer(data, RenderGraphAccess::Write),
                        zeros.data(), sizeof(zeros));
                });
            return handle;
        }
        void AddReadback(RenderGraph& graph, BufferHandle source, const char* name)
        {
            if (!bEnabled) return;
            struct Data { BufferHandle Source; std::array<BufferHandle, 8> Destinations; };
            Data imports; imports.Source = source;
            for (size_t i = 0; i < mSlots.size(); ++i)
                imports.Destinations[i] = graph.ImportBuffer(RenderGraphBufferDesc{
                    .DebugName = std::string(name) + " readback " + std::to_string(i), .ByteSize = CounterCount * 4 },
                    mSlots[i].Buffer, nvrhi::ResourceStates::CopyDest, nvrhi::ResourceStates::CopyDest);
            graph.AddPass<Data>(std::string(name) + "CounterReadback", RenderGraphQueue::Copy,
                [imports](RenderGraphBuilder& builder, Data& data)
                {
                    data.Source = builder.CopySource(imports.Source);
                    for (size_t i = 0; i < data.Destinations.size(); ++i)
                        data.Destinations[i] = builder.CopyDest(imports.Destinations[i]);
                    builder.SetSideEffect();
                },
                [this](RenderGraphContext& context, const Data& data)
                {
                    if (!IsRecording()) return;
                    context.GetCommandList()->copyBuffer(
                        context.GetBuffer(data.Destinations[mCurrent], RenderGraphAccess::Write), 0,
                        context.GetBuffer(data.Source, RenderGraphAccess::Read), 0, CounterCount * 4);
                    bCopied = true;
                });
        }
    private:
        struct Slot { nvrhi::BufferHandle Buffer; uint64_t Submission = 0; uint64_t Sequence = 0; };
        nvrhi::IDevice* mDevice;
        nvrhi::BufferHandle mGpu;
        std::array<Slot, 8> mSlots;
        std::ofstream mOutput;
        uint64_t mSequence = 0;
        int mCurrent = -1;
        bool bEnabled = false;
        bool bCopied = false;
    };
}
