#pragma once

#include "Runtime/Graphics/RHI/PipelineCache.h"

#include <cstdint>
#include <string>

namespace HEngine
{
    enum class MaterialPipelinePass : uint8_t
    {
        ForwardOpaque,
        ForwardTransparent,
        GBuffer,
        Depth,
        Shadow,
        Outline,
    };

    enum class MaterialPipelineVertexPath : uint8_t
    {
        CpuStatic,
        GpuDriven,
        Skinned,
    };

    enum class MaterialPipelineDescriptorTier : uint8_t
    {
        Baseline,
        HeapDirect,
    };

    enum class MaterialPipelineBlendMode : uint8_t
    {
        Opaque,
        AlphaBlend,
    };

    enum class MaterialPipelineDepthWrite : uint8_t
    {
        Disabled,
        Enabled,
    };

    enum class MaterialPipelineCullMode : uint8_t
    {
        None,
        Front,
        Back,
    };

    enum class MaterialPipelineSidedness : uint8_t
    {
        SingleSided,
        DoubleSided,
    };

    enum class MaterialPipelineAlphaMode : uint8_t
    {
        Opaque,
        Mask,
        Blend,
    };

    struct MaterialPipelineProgramIdentity
    {
        // Hash of the material source plus every recursively included source.
        std::string RecursiveContentHash;
        // Hash of the complete typed artifact set and its physical DXIL hashes.
        std::string ArtifactIdentityHash;
        uint64_t Generation = 0;
        uint32_t MaterialAbiVersion = 0;
        std::string BindingSignature;

        [[nodiscard]] bool operator==(const MaterialPipelineProgramIdentity&) const = default;
    };

    struct MaterialInputLayoutIdentity
    {
        uint64_t Value = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return Value != 0; }
        [[nodiscard]] bool operator==(const MaterialInputLayoutIdentity&) const = default;
    };

    struct MaterialPipelineFramebufferIdentity
    {
        nvrhi::Format ColorFormat = nvrhi::Format::UNKNOWN;
        nvrhi::Format DepthFormat = nvrhi::Format::UNKNOWN;
        uint32_t SampleCount = 1;
        uint32_t SampleQuality = 0;

        [[nodiscard]] bool operator==(const MaterialPipelineFramebufferIdentity&) const = default;
    };

    // This description contains immutable program, vertex, render-state and
    // framebuffer identities only. Per-instance parameter values and texture
    // asset paths deliberately have no entry point into a pipeline key.
    struct MaterialPipelineDesc
    {
        MaterialPipelineProgramIdentity Program;
        MaterialPipelinePass Pass = MaterialPipelinePass::ForwardOpaque;
        MaterialPipelineVertexPath VertexPath = MaterialPipelineVertexPath::CpuStatic;
        MaterialPipelineDescriptorTier DescriptorTier = MaterialPipelineDescriptorTier::Baseline;
        MaterialInputLayoutIdentity InputLayout;
        MaterialPipelineBlendMode BlendMode = MaterialPipelineBlendMode::Opaque;
        MaterialPipelineDepthWrite DepthWrite = MaterialPipelineDepthWrite::Enabled;
        MaterialPipelineCullMode CullMode = MaterialPipelineCullMode::Back;
        MaterialPipelineSidedness Sidedness = MaterialPipelineSidedness::SingleSided;
        MaterialPipelineAlphaMode AlphaMode = MaterialPipelineAlphaMode::Opaque;
        MaterialPipelineFramebufferIdentity Framebuffer;

        [[nodiscard]] bool operator==(const MaterialPipelineDesc&) const = default;
    };

    [[nodiscard]] GraphicsPipelineKey BuildMaterialPipelineKey(
        const MaterialPipelineDesc& desc) noexcept;
}
