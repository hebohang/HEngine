#pragma once

#include "RenderBackend.h"
#include "RenderSurface.h"
#include "Runtime/Core/Base/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

namespace HEngine
{
    struct RenderMemoryBudget
    {
        // Unavailable is distinct from zero usage. DX12 reports OS budgeting
        // for this process, not device-wide physical occupancy or pool bytes.
        bool bAvailable = false;
        bool bPerProcess = false;
        uint64_t LocalBudgetBytes = 0;
        uint64_t LocalUsageBytes = 0;
        uint64_t LocalReservationBytes = 0;
        bool bNonLocalAvailable = false;
        uint64_t NonLocalBudgetBytes = 0;
        uint64_t NonLocalUsageBytes = 0;
    };

    enum class RenderSubmissionTimingStatus : uint8_t
    {
        Complete, Aborted, QueryUnavailable, CompletionUnavailable,
    };

    // Graphics-queue interval from an Application CPU-frame begin marker through
    // the final backbuffer transition. Includes intervening standalone graphics
    // submissions and queue idle gaps; not a sum of busy pass durations. Other
    // queues and scanout/Present latency are outside this timestamp domain.
    struct RenderSubmissionGpuTiming
    {
        uint64_t SampleId = 0;
        uint64_t CpuFrameId = 0;
        uint64_t BeginSubmissionId = 0;
        uint64_t EndSubmissionId = 0;
        double Milliseconds = 0.0;
        double MarkerCpuMilliseconds = 0.0;
        uint32_t MarkerSubmissionCount = 0;
        RenderSubmissionTimingStatus Status = RenderSubmissionTimingStatus::QueryUnavailable;
    };

    struct FrameContext
    {
        // Stable resource-lifetime slot. Vulkan intentionally keeps this
        // independent from the acquired swap-chain image index.
        uint32_t FrameIndex = 0;
        uint32_t SwapChainImageIndex = 0;
        uint64_t SubmissionInstance = 0;
        uint64_t CompletedSubmissionInstance = 0;
        nvrhi::CommandListHandle GraphicsCommandList;
        nvrhi::EventQueryHandle CompletionQuery;
        bool bInFlight = false;
        // Reset by BeginFrame and set immediately after executeCommandList
        // succeeds. Callers use this to distinguish a discardable recording
        // failure from a submitted frame whose resource lifetime must advance.
        bool bSubmissionIssued = false;
        uint64_t TimingSampleId = 0;
        nvrhi::TimerQueryHandle FullFrameTimer;
        bool bFullFrameTimerEnded = false;
    };

    enum class PresentMode : uint8_t
    {
        VSync,
        Mailbox,
        Immediate,
    };

    struct SwapChainInfo
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t ImageCount = 0;
        nvrhi::Format Format = nvrhi::Format::UNKNOWN;
        PresentMode EffectivePresentMode = PresentMode::VSync;
    };

    class SwapChain
    {
    public:
        virtual ~SwapChain() = default;

        virtual void Resize(uint32_t width, uint32_t height) = 0;
        [[nodiscard]] virtual uint32_t AcquireNextImage() = 0;
        virtual void Present(bool bVSync) = 0;

        [[nodiscard]] virtual uint32_t GetWidth() const = 0;
        [[nodiscard]] virtual uint32_t GetHeight() const = 0;
        [[nodiscard]] virtual uint32_t GetBufferCount() const = 0;
        [[nodiscard]] virtual nvrhi::Format GetFormat() const = 0;
        [[nodiscard]] virtual PresentMode GetPresentMode() const = 0;
        [[nodiscard]] virtual uint32_t GetCurrentImageIndex() const = 0;
        // Newly created Vulkan swap-chain images begin in Common/undefined;
        // after their first successful Present they begin subsequent frames in
        // Present. DX12 images always report Present.
        [[nodiscard]] virtual nvrhi::ResourceStates
            GetCurrentBackBufferInitialState() const = 0;
        [[nodiscard]] SwapChainInfo GetInfo() const
        {
            return {
                GetWidth(),
                GetHeight(),
                GetBufferCount(),
                GetFormat(),
                GetPresentMode(),
            };
        }
        [[nodiscard]] virtual nvrhi::ITexture* GetCurrentBackBuffer() const = 0;
        [[nodiscard]] virtual nvrhi::IFramebuffer* GetCurrentFramebuffer() const = 0;
    };

    class IRenderBackend
    {
    public:
        virtual ~IRenderBackend() = default;

        virtual void Initialize(
            RenderSurface& surface,
            bool bEnableValidation,
            const RenderAdapterSelectionOptions& selection,
            uint32_t framesInFlight) = 0;
        virtual void Shutdown() = 0;
        [[nodiscard]] virtual FrameContext& BeginFrame() = 0;
        virtual void AbortFrame(FrameContext& frame) noexcept = 0;
        virtual void SubmitAndPresent(FrameContext& frame, bool bVSync) = 0;
        virtual void Resize(uint32_t width, uint32_t height) = 0;
        virtual void WaitForIdle() = 0;

        [[nodiscard]] virtual RenderBackend GetBackend() const = 0;
        [[nodiscard]] virtual const RenderCapabilities& GetCapabilities() const = 0;
        [[nodiscard]] virtual nvrhi::IDevice* GetDevice() const = 0;
        [[nodiscard]] virtual SwapChain& GetSwapChain() = 0;
        [[nodiscard]] virtual RenderMemoryBudget QueryMemoryBudget() const { return {}; }
    };

    [[nodiscard]] Scope<IRenderBackend> CreatePlatformRenderBackend(RenderBackend backend);

    class RenderDevice final
    {
    public:
        RenderDevice() = default;
        ~RenderDevice();

        RenderDevice(const RenderDevice&) = delete;
        RenderDevice& operator=(const RenderDevice&) = delete;

        void Initialize(
            RenderBackend backend,
            RenderSurface& surface,
            bool bEnableValidation,
            const RenderAdapterSelectionOptions& selection = {},
            uint32_t framesInFlight = 3);
        void Shutdown();
        [[nodiscard]] FrameContext& BeginFrame();
        void AbortFrame(FrameContext& frame) noexcept;
        void SubmitAndPresent(FrameContext& frame, bool bVSync);
        void Resize(uint32_t width, uint32_t height);
        void WaitForIdle();

        [[nodiscard]] bool IsInitialized() const { return mBackend != nullptr; }
        [[nodiscard]] RenderBackend GetBackend() const;
        [[nodiscard]] const RenderCapabilities& GetCapabilities() const;
        [[nodiscard]] nvrhi::IDevice* GetDevice() const;
        [[nodiscard]] SwapChain& GetSwapChain();
        [[nodiscard]] RenderMemoryBudget QueryMemoryBudget() const;
        void SetSubmissionTimingEnabled(bool enabled);
        void BeginGpuTimingFrame(uint64_t cpuFrameId);
        void CancelGpuTimingFrame() noexcept;
        [[nodiscard]] std::vector<RenderSubmissionGpuTiming> TakeSubmissionTimings();
        [[nodiscard]] uint64_t GetLastTimingSampleId() const { return mTimingSampleId; }


    private:
        [[nodiscard]] IRenderBackend& RequireBackend() const;

        void CollectSubmissionTimings(bool bKnownIdle = false);
        void FinishSubmissionTiming(const FrameContext& frame) noexcept;
        enum class TimingSlotState : uint8_t { Free, Active, Pending, Untracked };
        struct TimingSlot
        {
            nvrhi::TimerQueryHandle Query;
            nvrhi::EventQueryHandle Completion;
            nvrhi::CommandListHandle Marker;
            RenderSubmissionGpuTiming Sample;
            TimingSlotState State = TimingSlotState::Free;
        };
        Scope<IRenderBackend> mBackend;
        std::vector<TimingSlot> mTimingSlots;
        std::vector<RenderSubmissionGpuTiming> mCompletedTimings;
        uint64_t mTimingSampleId = 0;
        uint32_t mActiveTimingSlot = UINT32_MAX;
        bool mTimingEnabled = false;
    };
}
