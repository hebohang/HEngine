#include "Hepch.h"

#include "RenderDevice.h"

#include <stdexcept>
#include <chrono>
#include <cmath>

namespace HEngine
{
    RenderDevice::~RenderDevice()
    {
        Shutdown();
    }

    void RenderDevice::Initialize(
        RenderBackend backend,
        RenderSurface& surface,
        bool bEnableValidation,
        const RenderAdapterSelectionOptions& selection,
        uint32_t framesInFlight)
    {
        if (mBackend)
            throw std::logic_error("RenderDevice is already initialized.");
        if (framesInFlight < 2 || framesInFlight > 3)
            throw std::invalid_argument("Frames in flight must be 2 or 3.");
        Scope<IRenderBackend> platformBackend = CreatePlatformRenderBackend(backend);
        platformBackend->Initialize(surface, bEnableValidation, selection, framesInFlight);
        mBackend = std::move(platformBackend);
    }

    void RenderDevice::Shutdown()
    {
        if (!mBackend)
            return;

        CancelGpuTimingFrame();
        // Shutdown is outside measured frames. Keep query/marker/event resources
        // alive until GPU retirement, then release them while allocators live.
        // No waits are introduced in Begin/Submit/Collect or settings toggles.
        if (!mTimingSlots.empty())
        {
            mBackend->WaitForIdle();
            CollectSubmissionTimings(true);
        }
        mTimingSlots.clear();
        mCompletedTimings.clear();
        mTimingSampleId = 0;
        mTimingEnabled = false;
        mBackend->Shutdown();
        mBackend.reset();
    }

    FrameContext& RenderDevice::BeginFrame()
    {
        FrameContext& frame = RequireBackend().BeginFrame();
        frame.TimingSampleId = 0;
        frame.FullFrameTimer = nullptr;
        frame.bFullFrameTimerEnded = false;
        if (mActiveTimingSlot != UINT32_MAX)
        {
            const auto& slot = mTimingSlots[mActiveTimingSlot];
            frame.TimingSampleId = slot.Sample.SampleId;
            frame.FullFrameTimer = slot.Query;
        }
        return frame;
    }

    void RenderDevice::AbortFrame(FrameContext& frame) noexcept
    {
        if (mBackend)
        {
            mBackend->AbortFrame(frame);
            FinishSubmissionTiming(frame);
        }
    }

    void RenderDevice::SubmitAndPresent(FrameContext& frame, bool bVSync)
    {
        try
        {
            RequireBackend().SubmitAndPresent(frame, bVSync);
        }
        catch (...)
        {
            // Present may fail after the graphics submission commit point.
            FinishSubmissionTiming(frame);
            throw;
        }
        FinishSubmissionTiming(frame);
    }

    void RenderDevice::Resize(uint32_t width, uint32_t height)
    {
        RequireBackend().Resize(width, height);
        CollectSubmissionTimings();
    }

    void RenderDevice::WaitForIdle()
    {
        RequireBackend().WaitForIdle();
        CollectSubmissionTimings(true);
    }

    void RenderDevice::SetSubmissionTimingEnabled(bool enabled)
    {
        if (enabled && mTimingSlots.empty()) mTimingSlots.resize(16);
        if (!enabled) CancelGpuTimingFrame();
        mTimingEnabled = enabled;
    }

    void RenderDevice::BeginGpuTimingFrame(uint64_t cpuFrameId)
    {
        if (!mTimingEnabled) return;
        CancelGpuTimingFrame();
        CollectSubmissionTimings();
        const auto start = std::chrono::steady_clock::now();
        const uint64_t sampleId = ++mTimingSampleId;
        for (uint32_t index = 0; index < mTimingSlots.size(); ++index)
        {
            auto& slot = mTimingSlots[index];
            if (slot.State != TimingSlotState::Free) continue;
            auto* device = RequireBackend().GetDevice();
            if (!slot.Query) slot.Query = device->createTimerQuery();
            if (!slot.Completion) slot.Completion = device->createEventQuery();
            if (!slot.Marker) slot.Marker = device->createCommandList();
            if (!slot.Query || !slot.Completion || !slot.Marker) break;
            slot.Sample = {};
            slot.Sample.SampleId = sampleId;
            slot.Sample.CpuFrameId = cpuFrameId;
            device->resetTimerQuery(slot.Query);
            device->resetEventQuery(slot.Completion);
            // Quarantine before recording. An exception from native submission
            // has no reliable return ID; do not reuse or infer an unissued list.
            slot.State = TimingSlotState::Untracked;
            slot.Sample.Status = RenderSubmissionTimingStatus::CompletionUnavailable;
            slot.Marker->open();
            slot.Marker->beginTimerQuery(slot.Query);
            slot.Marker->close();
            // One profiling-only graphics submission. No CPU/GPU wait. Do not
            // poll this query until the end marker has actually been submitted.
            slot.Sample.BeginSubmissionId = device->executeCommandList(
                slot.Marker, nvrhi::CommandQueue::Graphics);
            slot.Sample.MarkerSubmissionCount = 1;
            slot.Sample.MarkerCpuMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            slot.State = TimingSlotState::Active;
            mActiveTimingSlot = index;
            return;
        }
        // The CPU-frame/sample mapping persists even when the bounded ring fills.
        RenderSubmissionGpuTiming missing;
        missing.SampleId = sampleId;
        missing.CpuFrameId = cpuFrameId;
        mCompletedTimings.push_back(missing);
    }

    void RenderDevice::CancelGpuTimingFrame() noexcept
    {
        if (mActiveTimingSlot == UINT32_MAX || !mBackend) return;
        auto& slot = mTimingSlots[mActiveTimingSlot];
        mActiveTimingSlot = UINT32_MAX;
        slot.Sample.Status = RenderSubmissionTimingStatus::Aborted;
        try
        {
            // The start marker was submitted even if rendering was skipped or
            // recording failed. Retire it before query/command-list reuse. No
            // end marker is needed for an invalid sample and no new work is sent.
            mBackend->GetDevice()->setEventQuery(slot.Completion, nvrhi::CommandQueue::Graphics);
            slot.State = TimingSlotState::Pending;
        }
        catch (...)
        {
            slot.Sample.Status = RenderSubmissionTimingStatus::CompletionUnavailable;
            slot.State = TimingSlotState::Untracked;
        }
    }

    void RenderDevice::FinishSubmissionTiming(const FrameContext& frame) noexcept
    {
        if (mActiveTimingSlot == UINT32_MAX) return;
        auto& slot = mTimingSlots[mActiveTimingSlot];
        if (frame.TimingSampleId != slot.Sample.SampleId) return;
        if (!frame.bSubmissionIssued)
        {
            CancelGpuTimingFrame();
            return;
        }
        mActiveTimingSlot = UINT32_MAX;
        slot.Sample.EndSubmissionId = frame.SubmissionInstance;
        slot.Sample.Status = frame.bFullFrameTimerEnded
            ? RenderSubmissionTimingStatus::Complete : RenderSubmissionTimingStatus::Aborted;
        try
        {
            // setEventQuery captures the queue's existing last-submitted ID;
            // it neither submits commands nor waits on DX12/Vulkan.
            mBackend->GetDevice()->setEventQuery(slot.Completion, nvrhi::CommandQueue::Graphics);
            slot.State = TimingSlotState::Pending;
        }
        catch (...)
        {
            slot.Sample.Status = RenderSubmissionTimingStatus::CompletionUnavailable;
            slot.State = TimingSlotState::Untracked;
        }
    }

    void RenderDevice::CollectSubmissionTimings(bool bKnownIdle)
    {
        if (!mBackend) return;
        auto* device = mBackend->GetDevice();
        for (auto& slot : mTimingSlots)
        {
            if (slot.State == TimingSlotState::Untracked)
            {
                if (!bKnownIdle) continue;
            }
            else if (slot.State != TimingSlotState::Pending ||
                (!bKnownIdle && !device->pollEventQuery(slot.Completion))) continue;
            if (slot.Sample.Status == RenderSubmissionTimingStatus::Complete)
            {
                if (!device->pollTimerQuery(slot.Query))
                {
                    if (!bKnownIdle) continue;
                    slot.Sample.Status = RenderSubmissionTimingStatus::CompletionUnavailable;
                }
                else
                {
                    slot.Sample.Milliseconds = double(device->getTimerQueryTime(slot.Query)) * 1000.0;
                    if (!std::isfinite(slot.Sample.Milliseconds) || slot.Sample.Milliseconds < 0.0)
                        slot.Sample.Status = RenderSubmissionTimingStatus::CompletionUnavailable;
                }
            }
            mCompletedTimings.push_back(slot.Sample);
            device->resetTimerQuery(slot.Query);
            device->resetEventQuery(slot.Completion);
            slot.State = TimingSlotState::Free;
        }
    }

    std::vector<RenderSubmissionGpuTiming> RenderDevice::TakeSubmissionTimings()
    {
        CollectSubmissionTimings();
        std::vector<RenderSubmissionGpuTiming> result;
        result.swap(mCompletedTimings);
        return result;
    }

    RenderBackend RenderDevice::GetBackend() const
    {
        return RequireBackend().GetBackend();
    }

    const RenderCapabilities& RenderDevice::GetCapabilities() const
    {
        return RequireBackend().GetCapabilities();
    }

    nvrhi::IDevice* RenderDevice::GetDevice() const
    {
        return RequireBackend().GetDevice();
    }

    SwapChain& RenderDevice::GetSwapChain()
    {
        return RequireBackend().GetSwapChain();
    }

    RenderMemoryBudget RenderDevice::QueryMemoryBudget() const
    {
        return RequireBackend().QueryMemoryBudget();
    }

    IRenderBackend& RenderDevice::RequireBackend() const
    {
        if (!mBackend)
            throw std::logic_error("RenderDevice is not initialized.");
        return *mBackend;
    }
}
