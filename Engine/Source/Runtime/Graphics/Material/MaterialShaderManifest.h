#pragma once

#include "Runtime/Graphics/RHI/ShaderArtifact.h"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    inline constexpr std::uint32_t MaterialShaderManifestFormatVersion = 3;
    inline constexpr std::uint32_t MaterialShaderAbiVersion = 1;
    inline constexpr std::uint32_t MaterialShaderParameterAlignment = 16;
    inline constexpr std::size_t MaterialParameterMaximumPropertyCount = 64;
    inline constexpr std::size_t MaterialParameterMaximumTextureCount = 16;
    inline constexpr std::size_t MaterialParameterMaximumByteSize = 1024;

    enum class MaterialShaderDomain { Surface };
    enum class MaterialShaderShadingModel { Lit, Unlit };
    enum class MaterialShaderBlendMode { Opaque, Mask, Transparent };
    enum class MaterialShaderCullMode { Back, None };
    enum class MaterialShaderDescriptorMode { Baseline, HeapDirect };
    enum class MaterialShaderVertexPath { Static, Skinned, GpuDriven };
    enum class MaterialShaderPass { Forward, Depth, Shadow, Outline };
    enum class MaterialShaderArtifactStage : std::uint8_t { Invalid, Vertex, Pixel };
    enum class MaterialShaderPropertyType { Float, Float2, Float3, Color, Bool, Int, Texture2D };
    enum class MaterialShaderTextureSemantic
    {
        None,
        BaseColor,
        Normal,
        MetallicRoughness,
        Occlusion,
        Emissive,
        Data
    };
    enum class MaterialShaderColorSpace { Linear, Srgb };
    enum class MaterialShaderDiagnosticSeverity { Info, Warning, Error };

    struct MaterialShaderDiagnostic
    {
        MaterialShaderDiagnosticSeverity Severity = MaterialShaderDiagnosticSeverity::Error;
        std::string Code;
        std::string Message;
        std::string Source;
        std::uint32_t Line = 0;
        std::uint32_t Column = 0;
    };

    struct MaterialShaderPropertyDefault
    {
        std::vector<double> Numeric;
        bool Boolean = false;
        std::int64_t Integer = 0;
        std::string Texture;
    };

    struct MaterialShaderPropertyRange
    {
        bool bEnabled = false;
        double Minimum = 0.0;
        double Maximum = 0.0;
    };

    struct MaterialShaderPropertyBinding
    {
        std::string RegisterType;
        std::int32_t RegisterIndex = -1;
        std::int32_t RegisterSpace = -1;
        std::int32_t DescriptorIndex = -1;
    };

    struct MaterialShaderProperty
    {
        std::string StableId;
        std::string Name;
        std::string DisplayName;
        std::string Group;
        std::string Source;
        std::uint32_t Line = 0;
        std::uint32_t Column = 0;
        MaterialShaderPropertyType Type = MaterialShaderPropertyType::Float;
        std::uint32_t ArrayCount = 1;
        std::uint32_t RowOffset = 0;
        std::uint32_t ByteOffset = 0;
        std::uint32_t ByteSize = 4;
        MaterialShaderPropertyDefault Default;
        MaterialShaderPropertyRange Range;
        MaterialShaderTextureSemantic TextureSemantic = MaterialShaderTextureSemantic::None;
        MaterialShaderColorSpace ColorSpace = MaterialShaderColorSpace::Linear;
        std::string Fallback;
        MaterialShaderPropertyBinding Binding;
    };

    struct MaterialShaderParameterBlock
    {
        std::uint32_t ByteSize = 0;
        std::uint32_t Alignment = MaterialShaderParameterAlignment;
    };

    struct MaterialShaderArtifact
    {
        MaterialShaderPass Pass = MaterialShaderPass::Forward;
        std::string Stage;
        std::string EntryPoint;
        std::string TargetProfile;
        std::string Permutation;
        MaterialShaderDescriptorMode DescriptorMode = MaterialShaderDescriptorMode::Baseline;
        MaterialShaderVertexPath VertexPath = MaterialShaderVertexPath::Static;
        std::string RelativePath;
        std::string ContentHash;
    };

    struct MaterialShaderArtifactSet
    {
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;
        std::vector<MaterialShaderArtifact> Artifacts;
    };

    // This is the exact lookup identity consumed by the runtime shader map.
    // Permutation names are diagnostics/debug metadata and are deliberately not
    // part of the ABI key.
    struct MaterialShaderArtifactKey
    {
        MaterialShaderPass Pass = MaterialShaderPass::Forward;
        MaterialShaderVertexPath VertexPath = MaterialShaderVertexPath::Static;
        MaterialShaderDescriptorMode DescriptorMode = MaterialShaderDescriptorMode::Baseline;
        MaterialShaderArtifactStage Stage = MaterialShaderArtifactStage::Invalid;

        [[nodiscard]] bool operator==(const MaterialShaderArtifactKey&) const = default;
    };

    struct MaterialShaderManifest
    {
        std::uint32_t FormatVersion = MaterialShaderManifestFormatVersion;
        std::uint32_t MaterialAbiVersion = MaterialShaderAbiVersion;
        std::string SourcePath;
        std::string RecursiveSourceHash;
        MaterialShaderDomain Domain = MaterialShaderDomain::Surface;
        MaterialShaderShadingModel ShadingModel = MaterialShaderShadingModel::Lit;
        MaterialShaderBlendMode BlendMode = MaterialShaderBlendMode::Opaque;
        MaterialShaderCullMode CullMode = MaterialShaderCullMode::Back;
        bool bDepthWrite = true;
        bool bCastsShadow = true;
        MaterialShaderDescriptorMode DescriptorMode = MaterialShaderDescriptorMode::Baseline;
        std::vector<MaterialShaderProperty> Properties;
        MaterialShaderParameterBlock ParameterBlock;
        std::string BindingSignatureHash;
        std::vector<MaterialShaderArtifactSet> ArtifactSets;
        // Compatibility view for older engine-side producers and tests. New
        // manifests serialize ArtifactSets; when this vector is used alone it
        // is interpreted as the DX12/DXIL set.
        std::vector<MaterialShaderArtifact> Artifacts;
        std::vector<MaterialShaderDiagnostic> Diagnostics;
    };

    struct MaterialShaderManifestLoadResult
    {
        MaterialShaderManifest Manifest;
        std::vector<MaterialShaderDiagnostic> Diagnostics;

        [[nodiscard]] bool Succeeded() const noexcept;
    };

    [[nodiscard]] const char* ToString(MaterialShaderDomain value) noexcept;
    [[nodiscard]] const char* ToString(MaterialShaderShadingModel value) noexcept;
    [[nodiscard]] const char* ToString(MaterialShaderBlendMode value) noexcept;
    [[nodiscard]] const char* ToString(MaterialShaderCullMode value) noexcept;
    [[nodiscard]] const char* ToString(MaterialShaderDescriptorMode value) noexcept;
    [[nodiscard]] const char* ToString(MaterialShaderVertexPath value) noexcept;
    [[nodiscard]] const char* ToString(MaterialShaderPass value) noexcept;
    [[nodiscard]] const char* ToString(MaterialShaderPropertyType value) noexcept;
    [[nodiscard]] const char* ToString(MaterialShaderTextureSemantic value) noexcept;
    [[nodiscard]] const char* ToString(MaterialShaderColorSpace value) noexcept;
    [[nodiscard]] const char* ToString(MaterialShaderDiagnosticSeverity value) noexcept;
    [[nodiscard]] const MaterialShaderDiagnostic* FindFirstMaterialShaderError(
        std::span<const MaterialShaderDiagnostic> diagnostics) noexcept;
    [[nodiscard]] std::string FormatMaterialShaderDiagnostics(
        std::span<const MaterialShaderDiagnostic> diagnostics);

    [[nodiscard]] std::uint32_t MaterialShaderPropertyByteSize(
        MaterialShaderPropertyType type) noexcept;
    [[nodiscard]] bool IsValidMaterialShaderTextureDefault(
        std::string_view value,
        std::string* error = nullptr);
    [[nodiscard]] MaterialShaderArtifactKey BuildMaterialShaderArtifactKey(
        const MaterialShaderArtifact& artifact);
    [[nodiscard]] const MaterialShaderArtifactSet* FindMaterialShaderArtifactSet(
        const MaterialShaderManifest& manifest,
        RenderBackend backend,
        ShaderBinaryFormat binaryFormat) noexcept;
    [[nodiscard]] std::span<const MaterialShaderArtifact> GetMaterialShaderArtifacts(
        const MaterialShaderManifest& manifest,
        RenderBackend backend,
        ShaderBinaryFormat binaryFormat) noexcept;
    [[nodiscard]] std::string ComputeMaterialShaderSha256(
        std::span<const std::byte> bytes);
    [[nodiscard]] bool ComputeMaterialShaderFileSha256(
        const std::filesystem::path& path,
        std::string& hash,
        std::string& error);
    [[nodiscard]] std::string BuildMaterialShaderBindingSignatureHash(
        const MaterialShaderManifest& manifest);
    [[nodiscard]] nlohmann::json MaterialShaderManifestToJson(
        const MaterialShaderManifest& manifest);
    [[nodiscard]] nlohmann::json MaterialShaderDiagnosticsToJson(
        const std::vector<MaterialShaderDiagnostic>& diagnostics);
    [[nodiscard]] MaterialShaderManifestLoadResult ParseMaterialShaderManifest(
        const nlohmann::json& json,
        std::string sourceName = {});
    [[nodiscard]] MaterialShaderManifestLoadResult LoadMaterialShaderManifest(
        const std::filesystem::path& path);
    [[nodiscard]] std::vector<MaterialShaderDiagnostic> ValidateMaterialShaderManifest(
        const MaterialShaderManifest& manifest,
        std::string sourceName = {});
}
