#pragma once

#include "BindlessDescriptorManager.h"
#include "EnvironmentSamplingProposal.h"
#include "TextureUploadState.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    struct TexturePreparationJob;
    class TexturePreparationQueue;
    enum class TextureColorSpace : uint8_t
    {
        Linear,
        SRgb,
    };

    [[nodiscard]] bool TryParseDefaultBindlessTexture(
        std::string_view token,
        DefaultBindlessTexture& textureKind) noexcept;

    struct ManagedTextureHandle
    {
        uint32_t Index = InvalidDescriptorIndex;
        uint32_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Index != InvalidDescriptorIndex && Generation != 0;
        }

        [[nodiscard]] bool operator==(const ManagedTextureHandle&) const = default;
    };

    // Device-independent reference/generation contract used by TextureManager
    // and by headless lifecycle tests.
    class ManagedTextureReferenceState final
    {
    public:
        [[nodiscard]] ManagedTextureHandle Acquire(uint32_t index);
        // Returns true only when the final reference was released.
        [[nodiscard]] bool Release(ManagedTextureHandle handle);
        [[nodiscard]] bool IsValid(ManagedTextureHandle handle) const noexcept;
        [[nodiscard]] uint32_t GetReferenceCount() const noexcept { return mReferenceCount; }

    private:
        uint32_t mGeneration = 1;
        uint32_t mReferenceCount = 0;
    };

    struct TextureLoadDesc
    {
        std::string AssetPath;
        TextureColorSpace ColorSpace = TextureColorSpace::Linear;
        DefaultBindlessTexture PendingFallback = DefaultBindlessTexture::White;
        bool bFlipVertically = false;
        bool bCreateBindlessDescriptor = true;
		// Trusted engine/editor callers may opt into an absolute source path.
		// Serialized material references must leave this disabled.
		bool bAllowAbsolutePath = false;
		// Sampled 2D textures use a complete CPU-generated mip chain by default,
		// including textures assigned to bounded descriptor pages. DDS assets
        // supply their complete mip chain offline; no decompression occurs.
        // Owned inputs
		// such as ImGui images or HDR precompute sources opt out explicitly.
        bool bGenerateMipmaps = true;
        // Producer load only; share the existing HDR decode, never decode per frame.
        bool bBuildEnvironmentProposal = false;
        // Alpha-tested base-color textures can retain their mip-0 texel
        // coverage across the generated chain. Other texture uses keep normal
        // straight-alpha area filtering.
        bool bPreserveAlphaCoverage = false;
        float AlphaCoverageCutoff = 0.5f;
		// Optional upper bound for either source dimension. This is intended for
		// UI previews that never need the full-resolution asset resident on the GPU.
        uint32_t MaximumDimension = 0;
        // Material-only opt-in. UI, lightmaps and precompute sources remain pinned.
        bool bStreamable = false;
        uint32_t FirstResidentMip = 0;
        // Consumers that cache a native texture handle immediately (environment
        // precompute, ray tracing) wait for this upload's query, never device idle.
        bool bRequireReady = false;
        // Validation/editor readback textures need tracked transitions instead
        // of the immutable ShaderResource permanent state.
        bool bAllowReadback = false;
        std::string DebugName;
    };

    // Typed immutable GPU texture metadata. NVRHI retains ownership and no
    // native resource/view escapes this resource layer.
    struct ManagedTexture
    {
        nvrhi::TextureHandle Texture;
        nvrhi::TextureHandle EnvironmentCdf;
        std::shared_ptr<const EnvironmentSamplingProposal> EnvironmentProposal;
        BindlessTextureHandle BindlessHandle;
        uint32_t ShaderDescriptorIndex = InvalidDescriptorIndex;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t MipLevels = 1;
        uint32_t SourceWidth = 0, SourceHeight = 0, FirstResidentMip = 0;
        // Texel/block payload bytes, excluding driver allocation alignment.
        uint64_t PayloadBytes = 0;
        nvrhi::Format Format = nvrhi::Format::UNKNOWN;
        std::string AssetPath;
        bool bDefault = false;
    };

    struct TextureManagerDesc
    {
        nvrhi::IDevice* Device = nullptr;
        std::filesystem::path AssetRootDirectory;
        std::filesystem::path EngineAssetRootDirectory;
        BindlessDescriptorManagerDesc Bindless;
    };

#if defined(HE_DEV_AGENT_RUNTIME)
    enum class TextureUploadFailure : uint8_t
    {
        None, TextureAllocation, CommandListCreation, CompletionQueryCreation, BeforeSubmission
    };

    struct TextureMipValidationData
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        std::vector<std::byte> Pixels;
    };

    // Uses the production CPU mip generator without creating an RHI device.
    [[nodiscard]] std::vector<TextureMipValidationData> GenerateRgba8MipsForValidation(
        std::span<const std::byte> pixels, uint32_t width, uint32_t height,
        TextureColorSpace colorSpace, bool bPreserveAlphaCoverage,
        float alphaCoverageCutoff);
#endif

    struct TextureUploadStatistics
    {
        uint64_t Requests = 0;
        uint64_t Completed = 0;
        uint64_t SubmittedBytes = 0;
        uint64_t GlobalWaits = 0;
        double CompletionLatencyMilliseconds = 0.0;
        TextureLatencyPercentiles RecentCompletionLatency;
        size_t Pending = 0;
        uint64_t PendingBytes = 0;
        size_t Preparing = 0;
        size_t Deferred = 0;
        uint64_t PreparedBytes = 0;
        uint64_t Failed = 0;
        double CpuPreparationMilliseconds = 0.0;
        size_t PeakPending = 0;
        uint64_t PeakPreparedBytes = 0;
        uint64_t PreparedByteBudget = 0;
        uint64_t DecodeBudgetStalls = 0;
        uint64_t OwnerClaims = 0;
        size_t LastCollectUploads = 0;
        uint64_t LastCollectBytes = 0;
        uint64_t StreamingBudgetBytes = 0, StreamingPlannedBytes = 0;
        uint64_t StreamingAccountedBytes = 0;
        uint64_t StreamingTransitions = 0, StreamingBudgetStalls = 0;
        uint64_t DescriptorReplacements = 0, DescriptorPublicationStalls = 0;
        size_t StreamableTextures = 0;
        bool bStreamingMinimumOverrun = false;
    };

    // Shared owner for decoded 2D assets and the global bindless texture and
    // sampler tables. GPU uploads publish after completion; callers never create
    // descriptors or binding sets per draw.
    class TextureManager final
    {
    public:
        explicit TextureManager(const TextureManagerDesc& desc);
        ~TextureManager();

        TextureManager(const TextureManager&) = delete;
        TextureManager& operator=(const TextureManager&) = delete;

        [[nodiscard]] ManagedTextureHandle LoadTexture(const TextureLoadDesc& desc);
        // Reprepare live matching assets without changing their handles.
        // An empty path requests all live assets.
        // Existing GPU texture stays visible until the replacement is ready.
        [[nodiscard]] size_t RequestReload(std::string_view assetPath);
        // Call once around a renderer's demand collection on its RHI owner thread.
        // Pixel demand uses output resolution (including temporal upscaling).
        void BeginStreamingFrame();
        void RequestTextureDetail(ManagedTextureHandle handle, float pixels);
        void EndStreamingFrame(uint64_t budgetBytes = 512ull * 1024 * 1024);
        void ReleaseTexture(ManagedTextureHandle handle, uint64_t retireAfterSubmission);
        void Collect(uint64_t completedSubmission, uint64_t lastSubmittedSubmission);

        [[nodiscard]] bool IsValid(ManagedTextureHandle handle) const noexcept;
        [[nodiscard]] TextureUploadStage GetUploadStage(ManagedTextureHandle handle) const;
        [[nodiscard]] const ManagedTexture& GetTexture(ManagedTextureHandle handle) const;
        [[nodiscard]] const ManagedTexture& GetDefaultTexture(
            DefaultBindlessTexture textureKind) const;
        [[nodiscard]] const ManagedTexture* FindDefaultTexture(
            std::string_view token) const noexcept;
        // Clears cached decode failures so hot-added or reimported assets may
        // be retried. An empty path clears all failures.
        size_t InvalidateFailedLoads(std::string_view assetPath = {});

        [[nodiscard]] const nvrhi::BindingLayoutHandle& GetTextureBindingLayout() const noexcept;
        [[nodiscard]] const nvrhi::DescriptorTableHandle& GetTextureDescriptorTable() const noexcept;
        [[nodiscard]] nvrhi::IBindingSet* GetTextureBindingSet() const noexcept;
        [[nodiscard]] const nvrhi::BindingLayoutHandle& GetSamplerBindingLayout() const noexcept;
        [[nodiscard]] const nvrhi::DescriptorTableHandle& GetSamplerDescriptorTable() const noexcept;
        [[nodiscard]] nvrhi::IBindingSet* GetSamplerBindingSet() const noexcept;
        [[nodiscard]] uint32_t GetMaterialSamplerIndex() const;

        // Returns every texture reachable through ResourceDescriptorHeap. The
        // renderer declares these as ShaderResource in its graph before use.
        [[nodiscard]] std::vector<nvrhi::TextureHandle> GetBindlessTextures() const;
        [[nodiscard]] uint64_t GetRevision() const noexcept { return mRevision; }
        [[nodiscard]] size_t GetResidentTextureCount() const noexcept;
        // Includes defaults and resources awaiting GPU retirement. This is a
        // payload accounting value, not a replacement for measured VRAM use.
        [[nodiscard]] uint64_t GetResidentTexturePayloadBytes() const noexcept;
        [[nodiscard]] TextureUploadStatistics GetUploadStatistics() const noexcept;
#if defined(HE_DEV_AGENT_RUNTIME)
        // One-shot, handle-specific fault at a real asynchronous upload boundary.
        // BeforeSubmission rejects locally; it does not simulate device loss.
        bool InjectUploadFailureForValidation(ManagedTextureHandle handle, TextureUploadFailure failure);
#endif

    private:
        struct Slot
        {
            ManagedTextureReferenceState References;
            std::string CacheKey;
            TextureLoadDesc SourceDesc;
            ManagedTexture Resource;
            ManagedTexture PendingResource;
            nvrhi::EventQueryHandle UploadCompletion;
            std::shared_ptr<TexturePreparationJob> Preparation;
            TextureUploadState UploadState;
            bool bReloading = false;
            float RequestedPixels = 0;
            uint64_t LastDemandFrame = 0, LastStreamingChange = 0;
            std::chrono::steady_clock::time_point RequestedAt{};
#if defined(HE_DEV_AGENT_RUNTIME)
            TextureUploadFailure ValidationFailure = TextureUploadFailure::None;
#endif
        };

        struct RetiredSlot
        {
            uint32_t Index = InvalidDescriptorIndex;
            uint64_t Submission = 0;
            nvrhi::TextureHandle Texture;
            nvrhi::TextureHandle EnvironmentCdf;
            nvrhi::EventQueryHandle UploadCompletion;
            uint64_t PayloadBytes = 0;
        };

		[[nodiscard]] std::filesystem::path ResolveAssetPath(
			std::string_view path,
			bool bAllowAbsolutePath = false) const;
        [[nodiscard]] std::string MakeCacheKey(
            const TextureLoadDesc& desc,
            const std::filesystem::path& resolvedPath) const;
        [[nodiscard]] uint32_t AllocateSlot();
        [[nodiscard]] ManagedTexture LoadDdsTexture(
            const TextureLoadDesc& desc, const std::filesystem::path& path,
            nvrhi::EventQueryHandle& completion);
        [[nodiscard]] ManagedTextureHandle RegisterTexture(
            const TextureLoadDesc& desc, const std::string& cacheKey,
            ManagedTexture resource, nvrhi::EventQueryHandle completion,
            std::chrono::steady_clock::time_point requestedAt);
        void PublishCompletedSlot(Slot& slot);
        void WaitForUpload(Slot& slot);
        void CompletePreparation(Slot& slot);
        void QueueDeferredPreparations();
        void QueueStreamingMip(Slot& slot, uint32_t mip);
        void CreateDefaults();

        nvrhi::DeviceHandle mDevice;
        // Reopening a submitted NVRHI list reuses only fence-completed native
        // allocators/upload chunks; pending copies retain their own storage.
        nvrhi::CommandListHandle mUploadCommands;
        std::filesystem::path mAssetRootDirectory;
        std::filesystem::path mEngineAssetRootDirectory;
        BindlessDescriptorManager mBindless;
        nvrhi::SamplerHandle mMaterialSampler;
        BindlessSamplerHandle mMaterialSamplerHandle;
        ManagedTexture mDefaultTextures[3];
        std::vector<Slot> mSlots;
        std::vector<uint32_t> mFreeSlots;
        std::vector<RetiredSlot> mRetiredSlots;
        std::unordered_map<std::string, ManagedTextureHandle> mCache;
        std::unordered_map<std::string, std::string> mFailedLoads;
        uint64_t mRevision = 1;
        uint64_t mLastSubmittedSubmission = 0;
        uint64_t mCompletedSubmission = 0;
        std::thread::id mOwnerThread = std::this_thread::get_id();
        TextureUploadStatistics mUploadStatistics;
        TextureCompletionLatencyWindow mCompletionLatencyWindow;
        uint64_t mCollectCount = 0;
        uint64_t mStreamingFrame = 0;
        std::unique_ptr<TexturePreparationQueue> mPreparationQueue;
    };
}
