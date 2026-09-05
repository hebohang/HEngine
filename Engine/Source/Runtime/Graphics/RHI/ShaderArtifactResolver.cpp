#include "ShaderArtifactResolver.h"

#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <fstream>
#include <stdexcept>
#include <string>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] bool IsFormatDirectory(std::string_view name) noexcept
        {
            return name == "DXIL" || name == "SPIRV";
        }

        [[nodiscard]] bool IsSafeRelativePath(const std::filesystem::path& path) noexcept
        {
            if (path.empty() || path.is_absolute() || path.has_root_name())
                return false;
            for (const std::filesystem::path& component : path)
            {
                if (component == "..")
                    return false;
            }
            return true;
        }

        [[nodiscard]] std::filesystem::path TryMakeRelative(
            const std::filesystem::path& path,
            const std::filesystem::path& root)
        {
            std::error_code error;
            std::filesystem::path relative = std::filesystem::relative(path, root, error);
            if (error || !IsSafeRelativePath(relative))
                return {};
            return relative.lexically_normal();
        }
    }

    ShaderArtifactResolver::ShaderArtifactResolver(
        std::filesystem::path cacheRoot,
        RenderBackend backend)
        : mBackend(backend)
        , mBinaryFormat(GetShaderBinaryFormat(backend))
    {
        if (cacheRoot.empty())
            throw std::invalid_argument("ShaderArtifactResolver requires a shader cache root");

        cacheRoot = cacheRoot.lexically_normal();
        const std::string leaf = cacheRoot.filename().generic_string();
        mCachesRoot = IsFormatDirectory(leaf) ? cacheRoot.parent_path() : cacheRoot;
        if (mCachesRoot.empty())
            throw std::invalid_argument("ShaderArtifactResolver cache root has no parent");
    }

    ShaderArtifactResolver::ShaderArtifactResolver(
        std::filesystem::path cacheRoot,
        nvrhi::IDevice* device)
        : ShaderArtifactResolver(
            std::move(cacheRoot),
            device && device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN
                ? RenderBackend::Vulkan
                : RenderBackend::DX12)
    {
        if (!device)
            throw std::invalid_argument("ShaderArtifactResolver requires a valid NVRHI device");
    }

    RenderBackend ShaderArtifactResolver::GetBackend() const noexcept
    {
        return mBackend;
    }

    ShaderBinaryFormat ShaderArtifactResolver::GetBinaryFormat() const noexcept
    {
        return mBinaryFormat;
    }

    const std::filesystem::path& ShaderArtifactResolver::GetCachesRoot() const noexcept
    {
        return mCachesRoot;
    }

    std::filesystem::path ShaderArtifactResolver::GetArtifactDirectory() const
    {
        return mCachesRoot / GetShaderArtifactDirectoryName(mBinaryFormat);
    }

    std::filesystem::path ShaderArtifactResolver::MakeRelativeArtifactPath(
        const std::filesystem::path& artifactPath) const
    {
        if (artifactPath.empty())
            throw std::invalid_argument("Shader artifact path must not be empty");

        if (!artifactPath.is_absolute() && !artifactPath.has_root_name())
        {
            std::filesystem::path normalized = artifactPath.lexically_normal();
            if (!IsSafeRelativePath(normalized))
                throw std::invalid_argument("Shader artifact path must stay inside its format directory");

            const std::string first = normalized.begin()->generic_string();
            if (IsFormatDirectory(first))
                normalized = normalized.lexically_relative(*normalized.begin());
            if (!IsSafeRelativePath(normalized))
                throw std::invalid_argument("Shader artifact relative path is invalid");
            return normalized;
        }

        for (const std::string_view formatDirectory : {
                std::string_view("DXIL"), std::string_view("SPIRV") })
        {
            const std::filesystem::path relative = TryMakeRelative(
                artifactPath.lexically_normal(),
                (mCachesRoot / formatDirectory).lexically_normal());
            if (!relative.empty())
                return relative;
        }
        throw std::invalid_argument(
            "Absolute shader artifact path is outside the configured CACHES root: " +
            Utils::PathToUtf8(artifactPath));
    }

    std::filesystem::path ShaderArtifactResolver::Resolve(
        const std::filesystem::path& artifactPath) const
    {
        return GetArtifactDirectory() / MakeRelativeArtifactPath(artifactPath);
    }

    std::string ShaderArtifactResolver::ReadContentHash(
        const std::filesystem::path& artifactPath) const
    {
        const std::filesystem::path sidecar = MakeShaderCacheKeyPath(artifactPath);
        std::ifstream stream(sidecar);
        if (!stream)
        {
            throw std::runtime_error(
                "Failed to open shader cache-key sidecar: " + Utils::PathToUtf8(sidecar));
        }

        std::string line;
        while (std::getline(stream, line))
        {
            constexpr std::string_view Prefix = "key=";
            if (line.starts_with(Prefix) && line.size() > Prefix.size())
                return line.substr(Prefix.size());
        }
        throw std::runtime_error(
            "Shader cache-key sidecar has no content hash: " + Utils::PathToUtf8(sidecar));
    }

    nvrhi::ShaderHandle ShaderArtifactResolver::LoadShaderFile(
        ShaderManager& shaderManager,
        ShaderCacheKey key,
        const std::filesystem::path& artifactPath,
        std::string_view debugName) const
    {
        const std::filesystem::path resolvedPath = Resolve(artifactPath);
        key.Backend = mBackend;
        key.BinaryFormat = mBinaryFormat;
        key.ContentHash = ReadContentHash(resolvedPath);
        return shaderManager.LoadShaderFile(key, resolvedPath, debugName);
    }
}
