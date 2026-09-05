#pragma once

#include "BindlessDescriptorManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HEngine
{
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
        bool bFlipVertically = false;
        bool bCreateBindlessDescriptor = true;
		// Trusted engine/editor callers may opt into an absolute source path.
		// Serialized material references must leave this disabled.
		bool bAllowAbsolutePath = false;
		// Sampled 2D textures use a complete CPU-generated mip chain by default,
		// including textures assigned to bounded descriptor pages. Owned inputs
		// such as ImGui images or HDR precompute sources opt out explicitly.
        bool bGenerateMipmaps = true;
        // Alpha-tested base-color textures can retain their mip-0 texel
        // coverage across the generated chain. Other texture uses keep normal
        // straight-alpha area filtering.
        bool bPreserveAlphaCoverage = false;
        float AlphaCoverageCutoff = 0.5f;
		// Optional upper bound for either source dimension. This is intended for
		// UI previews that never need the full-resolution asset resident on the GPU.
		uint32_t MaximumDimension = 0;
        std::string DebugName;
    };

    // Typed immutable GPU texture metadata. NVRHI retains ownership and no
    // native resource/view escapes this resource layer.
    struct ManagedTexture
    {
        nvrhi::TextureHandle Texture;
        BindlessTextureHandle BindlessHandle;
        uint32_t ShaderDescriptorIndex = InvalidDescriptorIndex;
        uint32_t Width = 0;
        uint32_t Height = 0;
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

    // Shared owner for decoded 2D assets and the global bindless texture and
    // sampler tables. Uploads are synchronous and cached; callers never create
    // descriptors or binding sets per draw.
    class TextureManager final
    {
    public:
        explicit TextureManager(const TextureManagerDesc& desc);
        ~TextureManager() = default;

        TextureManager(const TextureManager&) = delete;
        TextureManager& operator=(const TextureManager&) = delete;

        [[nodiscard]] ManagedTextureHandle LoadTexture(const TextureLoadDesc& desc);
        void ReleaseTexture(ManagedTextureHandle handle, uint64_t retireAfterSubmission);
        void Collect(uint64_t completedSubmission);

        [[nodiscard]] bool IsValid(ManagedTextureHandle handle) const noexcept;
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

    private:
        struct Slot
        {
            ManagedTextureReferenceState References;
            std::string CacheKey;
            ManagedTexture Resource;
        };

        struct RetiredSlot
        {
            uint32_t Index = InvalidDescriptorIndex;
            uint64_t Submission = 0;
            nvrhi::TextureHandle Texture;
        };

		[[nodiscard]] std::filesystem::path ResolveAssetPath(
			std::string_view path,
			bool bAllowAbsolutePath = false) const;
        [[nodiscard]] std::string MakeCacheKey(
            const TextureLoadDesc& desc,
            const std::filesystem::path& resolvedPath) const;
        [[nodiscard]] uint32_t AllocateSlot();
        void CreateDefaults();

        nvrhi::DeviceHandle mDevice;
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
    };
}
