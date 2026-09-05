#pragma once

#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace HEngine
{
    enum class ShaderBinaryFormat : uint8_t
    {
        Dxil,
        SpirV,
    };

    struct ShaderArtifactTarget
    {
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;

        [[nodiscard]] constexpr bool operator==(const ShaderArtifactTarget&) const = default;
    };

    [[nodiscard]] constexpr std::string_view ToString(ShaderBinaryFormat format) noexcept
    {
        switch (format)
        {
            case ShaderBinaryFormat::Dxil: return "DXIL";
            case ShaderBinaryFormat::SpirV: return "SPIR-V";
            default: return "Unknown";
        }
    }

    [[nodiscard]] constexpr ShaderBinaryFormat GetShaderBinaryFormat(
        RenderBackend backend) noexcept
    {
        return backend == RenderBackend::Vulkan
            ? ShaderBinaryFormat::SpirV
            : ShaderBinaryFormat::Dxil;
    }

    [[nodiscard]] constexpr bool IsShaderArtifactTargetCompatible(
        RenderBackend backend,
        ShaderBinaryFormat format) noexcept
    {
        return GetShaderBinaryFormat(backend) == format;
    }

    [[nodiscard]] constexpr std::string_view GetShaderArtifactDirectoryName(
        ShaderBinaryFormat format) noexcept
    {
        return format == ShaderBinaryFormat::SpirV ? "SPIRV" : "DXIL";
    }

    [[nodiscard]] inline std::filesystem::path MakeShaderCacheKeyPath(
        std::filesystem::path artifactPath)
    {
        artifactPath += ".cachekey";
        return artifactPath;
    }
}
