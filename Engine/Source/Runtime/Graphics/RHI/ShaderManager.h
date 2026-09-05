#pragma once

#include "Runtime/Graphics/RHI/ShaderArtifact.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    struct ShaderCacheKey
    {
        std::string Source;
        std::string Variant;
        nvrhi::ShaderType Stage = nvrhi::ShaderType::None;
        std::string EntryPoint;
        std::string TargetProfile;
        std::string ContentHash;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;

        [[nodiscard]] bool operator==(const ShaderCacheKey&) const = default;
    };

    struct ShaderCacheKeyHash
    {
        [[nodiscard]] size_t operator()(const ShaderCacheKey& key) const noexcept;
    };

    // Source paths are project identities and therefore compare
    // case-insensitively with slash normalization. Every other field remains a
    // typed part of the cache identity.
    [[nodiscard]] bool ShaderCacheKeysMatchCanonicalIdentity(
        const ShaderCacheKey& left,
        const ShaderCacheKey& right);

    struct ShaderLibraryCacheKey
    {
        std::string Source;
        std::string Variant;
        std::string TargetProfile;
        std::string ContentHash;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;

        [[nodiscard]] bool operator==(const ShaderLibraryCacheKey&) const = default;
    };

    struct ShaderLibraryCacheKeyHash
    {
        [[nodiscard]] size_t operator()(const ShaderLibraryCacheKey& key) const noexcept;
    };

    struct ShaderBinaryCreateRequest
    {
        ShaderCacheKey Key;
        std::span<const std::byte> Binary;
        std::string DebugName;
    };

    struct ShaderDxilCreateRequest
    {
        ShaderCacheKey Key;
        std::span<const std::byte> Dxil;
        std::string DebugName;
    };

    class ShaderManager final
    {
    public:
        explicit ShaderManager(nvrhi::IDevice* device);
        ~ShaderManager() = default;

        ShaderManager(const ShaderManager&) = delete;
        ShaderManager& operator=(const ShaderManager&) = delete;

        [[nodiscard]] nvrhi::ShaderHandle LoadShaderFile(
            const ShaderCacheKey& key,
            const std::filesystem::path& binaryPath,
            std::string_view debugName = {});
        [[nodiscard]] nvrhi::ShaderHandle CreateShader(
            const ShaderCacheKey& key,
            std::span<const std::byte> binary,
            std::string_view debugName = {});
        // Creates a complete set while holding the cache lock. If any device
        // creation fails, entries inserted by this call are removed before the
        // lock is released, so another consumer cannot observe a partial set.
        [[nodiscard]] std::vector<nvrhi::ShaderHandle> CreateShaderBatch(
            std::span<const ShaderBinaryCreateRequest> requests);

        [[nodiscard]] nvrhi::ShaderLibraryHandle LoadShaderLibraryFile(
            const ShaderLibraryCacheKey& key,
            const std::filesystem::path& binaryPath);
        [[nodiscard]] nvrhi::ShaderLibraryHandle CreateShaderLibrary(
            const ShaderLibraryCacheKey& key,
            std::span<const std::byte> binary);
        [[nodiscard]] nvrhi::ShaderHandle GetShaderFromLibrary(
            const ShaderLibraryCacheKey& key,
            std::string_view entryPoint,
            nvrhi::ShaderType stage) const;

        // Compatibility surface for the existing DX12 renderer. New code
        // should use the backend-neutral methods above.
        [[nodiscard]] nvrhi::ShaderHandle LoadDxilFile(
            const ShaderCacheKey& key,
            const std::filesystem::path& dxilPath,
            std::string_view debugName = {});
        [[nodiscard]] nvrhi::ShaderHandle CreateDxil(
            const ShaderCacheKey& key,
            std::span<const std::byte> dxil,
            std::string_view debugName = {});
        [[nodiscard]] std::vector<nvrhi::ShaderHandle> CreateDxilBatch(
            std::span<const ShaderDxilCreateRequest> requests);

        [[nodiscard]] bool Contains(const ShaderCacheKey& key) const;
        [[nodiscard]] size_t Size() const;
        [[nodiscard]] RenderBackend GetBackend() const noexcept;
        [[nodiscard]] ShaderBinaryFormat GetBinaryFormat() const noexcept;
        void RemoveSourceExcept(
            std::string_view source,
            std::span<const ShaderCacheKey> retainedKeys);
        void Clear();

    private:
        struct CacheEntry
        {
            nvrhi::ShaderHandle Shader;
            uint64_t BytecodeFingerprint = 0;
        };

        struct LibraryCacheEntry
        {
            nvrhi::ShaderLibraryHandle Library;
            uint64_t BytecodeFingerprint = 0;
        };

        [[nodiscard]] nvrhi::ShaderHandle Find(const ShaderCacheKey& key) const;
        [[nodiscard]] nvrhi::ShaderLibraryHandle FindLibrary(
            const ShaderLibraryCacheKey& key) const;

        nvrhi::DeviceHandle mDevice;
        mutable std::mutex mMutex;
        std::unordered_map<ShaderCacheKey, CacheEntry, ShaderCacheKeyHash> mShaders;
        std::unordered_map<
            ShaderLibraryCacheKey,
            LibraryCacheEntry,
            ShaderLibraryCacheKeyHash> mLibraries;
    };
}
