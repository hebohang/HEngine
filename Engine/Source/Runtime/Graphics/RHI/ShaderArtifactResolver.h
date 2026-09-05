#pragma once

#include "Runtime/Graphics/RHI/ShaderManager.h"

#include <filesystem>
#include <string_view>

namespace HEngine
{
    // Resolves one logical shader artifact against the active graphics backend.
    // The input root may be either Shaders/CACHES or its legacy DXIL/SPIRV
    // child, which keeps existing callers source-compatible while preventing
    // backend directory selection from leaking into render passes.
    class ShaderArtifactResolver final
    {
    public:
        ShaderArtifactResolver(
            std::filesystem::path cacheRoot,
            RenderBackend backend);
        ShaderArtifactResolver(
            std::filesystem::path cacheRoot,
            nvrhi::IDevice* device);

        [[nodiscard]] RenderBackend GetBackend() const noexcept;
        [[nodiscard]] ShaderBinaryFormat GetBinaryFormat() const noexcept;
        [[nodiscard]] const std::filesystem::path& GetCachesRoot() const noexcept;
        [[nodiscard]] std::filesystem::path GetArtifactDirectory() const;
        [[nodiscard]] std::filesystem::path Resolve(
            const std::filesystem::path& artifactPath) const;

        [[nodiscard]] nvrhi::ShaderHandle LoadShaderFile(
            ShaderManager& shaderManager,
            ShaderCacheKey key,
            const std::filesystem::path& artifactPath,
            std::string_view debugName = {}) const;

    private:
        [[nodiscard]] std::filesystem::path MakeRelativeArtifactPath(
            const std::filesystem::path& artifactPath) const;
        [[nodiscard]] std::string ReadContentHash(
            const std::filesystem::path& artifactPath) const;

        std::filesystem::path mCachesRoot;
        RenderBackend mBackend = RenderBackend::DX12;
        ShaderBinaryFormat mBinaryFormat = ShaderBinaryFormat::Dxil;
    };
}
