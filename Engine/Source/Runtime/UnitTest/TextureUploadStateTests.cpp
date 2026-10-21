#include "Hepch.h"
#include "Runtime/UnitTest/TextureUploadStateTests.h"
#include "Runtime/Graphics/RHI/TextureManager.h"
#include "Runtime/Graphics/RHI/TexturePreparationQueueState.h"

#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace HEngine::Tests
{
    void TextureUploadStateContract()
    {
        TexturePreparationQueueState<int> waiting;
        std::vector<std::shared_ptr<int>> jobs;
        for (int index = 0; index < 32; ++index)
        {
            jobs.push_back(std::make_shared<int>(index));
            if (!waiting.TryEnqueue(jobs.back(), 32))
                throw std::runtime_error("Texture preparation queue rejected available capacity");
        }
        const auto queuedReplacement = std::make_shared<int>(99);
        if (waiting.TryEnqueue(queuedReplacement, 32))
            throw std::runtime_error("Texture preparation queue exceeded its bound");
        const auto inFlight = waiting.TakeNext();
        if (waiting.CancelWaiting(inFlight) || waiting.Active() != 32)
            throw std::runtime_error("Active texture decode was removed from the waiting queue");
        if (!waiting.CancelWaiting(jobs[1]) || waiting.CancelWaiting(jobs[1]) ||
            waiting.Active() != 31 || !waiting.TryEnqueue(queuedReplacement, 32))
            throw std::runtime_error("Cancelling a waiting texture did not restore queue capacity");
        while (!waiting.Empty())
        {
            (void)waiting.TakeNext();
            waiting.CompleteActive();
        }
        waiting.CompleteActive();
        if (waiting.Active() != 0 || waiting.Waiting() != 0)
            throw std::runtime_error("Texture preparation queue retained completed jobs");

        TexturePreparationQueueState<int> budget(1024 * 1024);
        const auto firstWaiting = std::make_shared<int>(1);
        const auto ownerWaiting = std::make_shared<int>(2);
        const auto resumedWaiting = std::make_shared<int>(3);
        if (!budget.TryEnqueue(firstWaiting, 3) ||
            !budget.TryEnqueue(ownerWaiting, 3) || !budget.CanStart(1))
            throw std::runtime_error("Texture decode did not start below its byte budget");
        budget.AddPreparedBytes(5 * 1024 * 1024); // One in-flight decode may overshoot.
        if (budget.CanStart(1) || budget.TryEnqueue(resumedWaiting, 3))
            throw std::runtime_error("Texture decode ignored its retained-byte gate");
        budget.RecordBudgetStall();
        if (!budget.TakeWaitingForOwner(ownerWaiting) || budget.Active() != 1 ||
            budget.Waiting() != 1 || budget.TakeWaitingForOwner(ownerWaiting))
            throw std::runtime_error("Blocking owner could not claim exactly one queued job");
        budget.ReleasePreparedBytes(5 * 1024 * 1024);
        if (!budget.CanStart(1) || !budget.TryEnqueue(resumedWaiting, 3) ||
            budget.PreparedBytes() != 0 || budget.DecodeBudgetStalls() != 1 ||
            budget.PreparedByteBudget() != 1024 * 1024)
            throw std::runtime_error("Texture decoder did not resume after releasing bytes");
        (void)budget.TakeNext();
        (void)budget.TakeNext();
        budget.CompleteActive();
        budget.CompleteActive();

        TextureCompletionLatencyWindow latency;
        latency.Add(-1.0);
        latency.Add(std::numeric_limits<double>::infinity());
        latency.Add(std::numeric_limits<double>::quiet_NaN());
        if (latency.Get().Samples != 0)
            throw std::runtime_error("Invalid texture latency entered the rolling sample");
        for (int value = 1; value <= 300; ++value)
            latency.Add(static_cast<double>(value));
        const auto percentiles = latency.Get();
        if (percentiles.Samples != TextureCompletionLatencyWindow::Capacity ||
            percentiles.P50Milliseconds != 172.0 ||
            percentiles.P95Milliseconds != 288.0 ||
            percentiles.P99Milliseconds != 298.0 ||
            percentiles.MaxMilliseconds != 300.0)
            throw std::runtime_error("Texture latency window did not retain the latest 256 samples");
        TextureCompletionLatencyWindow sparseLatency;
        for (int value = 1; value <= 9; ++value)
            sparseLatency.Add(static_cast<double>(value));
        if (sparseLatency.Get().P95Milliseconds != 9.0 ||
            sparseLatency.Get().P99Milliseconds != 9.0)
            throw std::runtime_error("Small-sample texture tail latency missed its maximum");

        TextureUploadState state;
        if (state.Get() != TextureUploadStage::Empty)
            throw std::runtime_error("Texture slot did not start empty");
        state.BeginPreparation();
        state.Submit();
        state.Complete();
        if (state.Get() != TextureUploadStage::Ready)
            throw std::runtime_error("Completed texture was not published ready");
        state.BeginReload();
        state.Submit();
        state.Complete();
        state.BeginReload();
        state.Fail();
        state.Retry();
        state.Submit();
        state.Complete();
        bool rejected = false;
        try { state.Complete(); }
        catch (const std::logic_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("Double texture publication was accepted");
        state.Cancel();
        state.Reset();

        state.BeginPreparation();
        state.Fail();
        if (state.Get() != TextureUploadStage::Failed)
            throw std::runtime_error("Failed preparation did not retain its state");
        state.Retry();
        state.Submit();
        state.Complete();
        state.Cancel();
        state.Reset();

        state.Submit(); // Startup/explicit-ready upload skips background preparation.
        state.Cancel(); // Unload during a submitted GPU upload.
        rejected = false;
        try { state.Complete(); }
        catch (const std::logic_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("Cancelled upload was published");
        state.Reset();

        ManagedTextureReferenceState references;
        const ManagedTextureHandle oldHandle = references.Acquire(7);
        if (!references.Release(oldHandle))
            throw std::runtime_error("Last texture reference did not release");
        const ManagedTextureHandle replacement = references.Acquire(7);
        if (references.IsValid(oldHandle) || replacement.Generation == oldHandle.Generation)
            throw std::runtime_error("Reused texture slot accepted a stale handle");
        (void)references.Release(replacement);

#if defined(HE_DEV_AGENT_RUNTIME)
        constexpr uint32_t sourceWidth = 64;
        constexpr uint32_t sourceHeight = 64;
        std::vector<std::byte> rgba(
            static_cast<size_t>(sourceWidth) * sourceHeight * 4u);
        uint32_t randomState = 0x9e3779b9u;
        for (size_t pixel = 0; pixel < rgba.size() / 4u; ++pixel)
        {
            randomState = randomState * 1664525u + 1013904223u;
            rgba[pixel * 4u + 0u] = std::byte{ 96 };
            rgba[pixel * 4u + 1u] = std::byte{ 144 };
            rgba[pixel * 4u + 2u] = std::byte{ 208 };
            rgba[pixel * 4u + 3u] = std::byte{ static_cast<uint8_t>(randomState >> 24u) };
        }
        const auto coverage = [](std::span<const std::byte> pixels)
        {
            size_t covered = 0;
            for (size_t index = 3; index < pixels.size(); index += 4u)
                covered += std::to_integer<uint8_t>(pixels[index]) >= 128u;
            return static_cast<double>(covered) / (pixels.size() / 4u);
        };
        const double sourceCoverage = coverage(rgba);
        const auto straight = GenerateRgba8MipsForValidation(
            rgba, sourceWidth, sourceHeight, TextureColorSpace::SRgb, false, 0.5f);
        const auto preserved = GenerateRgba8MipsForValidation(
            rgba, sourceWidth, sourceHeight, TextureColorSpace::SRgb, true, 0.5f);
        if (straight.size() != 6 || preserved.size() != straight.size())
            throw std::runtime_error("Generated RGBA8 mip chain has an incorrect length");
        bool bCoverageImproved = false;
        uint32_t expectedWidth = sourceWidth;
        uint32_t expectedHeight = sourceHeight;
        for (size_t index = 0; index < straight.size(); ++index)
        {
            expectedWidth = std::max(expectedWidth / 2u, 1u);
            expectedHeight = std::max(expectedHeight / 2u, 1u);
            const auto& ordinaryMip = straight[index];
            const auto& preservedMip = preserved[index];
            const size_t expectedBytes =
                static_cast<size_t>(expectedWidth) * expectedHeight * 4u;
            if (ordinaryMip.Width != expectedWidth || ordinaryMip.Height != expectedHeight ||
                preservedMip.Width != expectedWidth || preservedMip.Height != expectedHeight ||
                ordinaryMip.Pixels.size() != expectedBytes ||
                preservedMip.Pixels.size() != expectedBytes)
                throw std::runtime_error("Generated RGBA8 mip dimensions or byte count changed");
            const double ordinaryError =
                std::abs(coverage(ordinaryMip.Pixels) - sourceCoverage);
            const double preservedError =
                std::abs(coverage(preservedMip.Pixels) - sourceCoverage);
            if (preservedError > ordinaryError + 1e-12)
                throw std::runtime_error("Alpha coverage preservation worsened a mip");
            bCoverageImproved |= preservedError + 1e-12 < ordinaryError;
        }
        if (!bCoverageImproved)
            throw std::runtime_error("Alpha coverage test fixture did not exercise preservation");

        bool bRejectedOverflow = false;
        try
        {
            (void)GenerateRgba8MipsForValidation({},
                std::numeric_limits<uint32_t>::max(),
                std::numeric_limits<uint32_t>::max(),
                TextureColorSpace::Linear, false, 0.5f);
        }
        catch (const std::runtime_error&) { bRejectedOverflow = true; }
        if (!bRejectedOverflow)
            throw std::runtime_error("Overflowing RGBA8 texture dimensions were accepted");
#endif
    }
}
