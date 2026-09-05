#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <vector>

namespace HEngine::Tests
{
    struct TextureReadbackQueueTestAccess;
}

namespace HEngine
{
    inline constexpr uint32_t InvalidTextureReadbackIndex = ~0u;

    struct TextureReadbackHandle
    {
        uint32_t Index = InvalidTextureReadbackIndex;
        uint32_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Index != InvalidTextureReadbackIndex && Generation != 0;
        }

        [[nodiscard]] bool operator==(const TextureReadbackHandle&) const = default;
    };

    enum class TextureReadbackStatus : uint8_t
    {
        PendingSubmission,
        InFlight,
        Complete,
        Failed,
    };

    enum class TextureReadbackOutputFormat : uint8_t
    {
        Auto,
        Png,
        Pfm,
        Raw,
    };

    enum class TextureReadbackAlphaMode : uint8_t
    {
        Preserve,
        Discard,
    };

    enum class TextureReadbackNonFinitePolicy : uint8_t
    {
        Reject,
        Preserve,
    };

    struct TextureReadbackDesc
    {
        std::filesystem::path OutputPath;
        nvrhi::TextureSlice SourceSlice;
        TextureReadbackOutputFormat OutputFormat = TextureReadbackOutputFormat::Auto;
        TextureReadbackAlphaMode AlphaMode = TextureReadbackAlphaMode::Preserve;
        TextureReadbackNonFinitePolicy NonFinitePolicy =
            TextureReadbackNonFinitePolicy::Reject;
        bool bReplaceExisting = true;
    };

    struct TextureReadbackResult
    {
        TextureReadbackStatus Status = TextureReadbackStatus::PendingSubmission;
        std::filesystem::path OutputPath;
        std::filesystem::path MetadataPath;
        std::string Error;
    };

    struct TextureReadbackRawEncoding
    {
        std::vector<uint8_t> Pixels;
        std::string MetadataJson;
    };

    // Records texture-to-staging copies on a RenderGraph-declared CopySource
    // pass. Mapping and file encoding happen only after the owning submission
    // is complete. No native graphics API object crosses this boundary.
    class TextureReadbackQueue final
    {
    public:
        explicit TextureReadbackQueue(nvrhi::IDevice* device);
        ~TextureReadbackQueue();

        TextureReadbackQueue(const TextureReadbackQueue&) = delete;
        TextureReadbackQueue& operator=(const TextureReadbackQueue&) = delete;

        [[nodiscard]] TextureReadbackHandle Record(
            nvrhi::ICommandList* commandList,
            nvrhi::ITexture* source,
            const TextureReadbackDesc& desc);
		[[nodiscard]] TextureReadbackHandle Reserve(const TextureReadbackDesc& desc);
		void RecordReserved(
			TextureReadbackHandle handle,
			nvrhi::ICommandList* commandList,
			nvrhi::ITexture* source);

        void NotifySubmitted(TextureReadbackHandle handle, uint64_t submission);
        void CancelPending(TextureReadbackHandle handle);
        // Polls completed GPU submissions without waiting for CPU encoding work.
        void Collect(uint64_t completedSubmission);
        // Blocks until CPU encoding work for GPU submissions up to this value is complete.
        void Drain(uint64_t completedSubmission);
        void Release(TextureReadbackHandle handle);
        void Clear();

        [[nodiscard]] bool IsValid(TextureReadbackHandle handle) const noexcept;
        [[nodiscard]] TextureReadbackResult GetResult(TextureReadbackHandle handle) const;
        [[nodiscard]] size_t GetPendingCount() const noexcept;

        // Exposed for the headless contract test and reusable non-GPU captures.
        [[nodiscard]] static std::vector<uint8_t> EncodeRgba8Png(
            uint32_t width,
            uint32_t height,
            const uint8_t* pixels,
            size_t rowPitch,
            bool bSourceBgra);
        [[nodiscard]] static std::vector<uint8_t> EncodePfm(
            uint32_t width,
            uint32_t height,
            nvrhi::Format sourceFormat,
            const uint8_t* pixels,
            size_t rowPitch,
            TextureReadbackAlphaMode alphaMode = TextureReadbackAlphaMode::Preserve,
            TextureReadbackNonFinitePolicy nonFinitePolicy =
                TextureReadbackNonFinitePolicy::Reject);
        [[nodiscard]] static TextureReadbackRawEncoding EncodeRaw(
            uint32_t width,
            uint32_t height,
            nvrhi::Format sourceFormat,
            const uint8_t* pixels,
            size_t rowPitch,
            TextureReadbackNonFinitePolicy nonFinitePolicy =
                TextureReadbackNonFinitePolicy::Reject);
        [[nodiscard]] static TextureReadbackOutputFormat ResolveOutputFormat(
            const std::filesystem::path& outputPath,
            TextureReadbackOutputFormat requestedFormat =
                TextureReadbackOutputFormat::Auto);

    private:
        friend struct Tests::TextureReadbackQueueTestAccess;

        struct AsyncCompletion
        {
            bool bSucceeded = false;
            std::string Error;
        };

        struct Slot
        {
            uint32_t Generation = 1;
            bool bOccupied = false;
            uint64_t Submission = 0;
            uint32_t Width = 0;
            uint32_t Height = 0;
            nvrhi::Format Format = nvrhi::Format::UNKNOWN;
			TextureReadbackOutputFormat OutputFormat = TextureReadbackOutputFormat::Auto;
			TextureReadbackAlphaMode AlphaMode = TextureReadbackAlphaMode::Preserve;
			TextureReadbackNonFinitePolicy NonFinitePolicy =
				TextureReadbackNonFinitePolicy::Reject;
			nvrhi::TextureSlice RequestedSourceSlice;
            nvrhi::TextureSlice StagingSlice;
            nvrhi::StagingTextureHandle Staging;
            TextureReadbackResult Result;
			bool bReplaceExisting = true;
			bool bRecorded = false;
			bool bReleaseWhenComplete = false;
            bool bPendingCounted = false;
            std::future<AsyncCompletion> Completion;
        };

        [[nodiscard]] Slot& Require(TextureReadbackHandle handle);
        [[nodiscard]] const Slot& Require(TextureReadbackHandle handle) const;
        [[nodiscard]] TextureReadbackHandle Allocate();
        void ProcessCompletions(uint64_t completedSubmission, bool bWaitForCpuCompletion);
        void BeginComplete(Slot& slot);
        [[nodiscard]] bool FinishComplete(Slot& slot);

        nvrhi::IDevice* mDevice = nullptr;
        std::vector<Slot> mSlots;
        std::vector<uint32_t> mFreeSlots;
        size_t mPendingCount = 0;
    };
}
