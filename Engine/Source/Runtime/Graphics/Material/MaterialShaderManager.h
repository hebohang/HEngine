#pragma once

#include "MaterialShaderManifest.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    class MaterialShaderManager;

    // Current plus this many immutable prior generations remain addressable by
    // exact identity. Frame submissions retain their own shared_ptr, so
    // evicting an entry from this lookup window never invalidates in-flight use.
    inline constexpr std::size_t MaterialShaderGenerationHistoryCapacity = 8;

    enum class MaterialProgramShaderStage : std::uint8_t
    {
        Vertex,
        Pixel,
        Compute,
    };

    struct MaterialShaderHandleKey
    {
        MaterialShaderPass Pass = MaterialShaderPass::Forward;
        MaterialShaderVertexPath VertexPath = MaterialShaderVertexPath::Static;
        MaterialShaderDescriptorMode DescriptorMode = MaterialShaderDescriptorMode::Baseline;
        MaterialProgramShaderStage Stage = MaterialProgramShaderStage::Pixel;

        [[nodiscard]] bool operator==(const MaterialShaderHandleKey&) const = default;
    };

    struct MaterialShaderHandleKeyHash
    {
        [[nodiscard]] std::size_t operator()(const MaterialShaderHandleKey& key) const noexcept;
    };

    struct MaterialShaderProgramIdentity
    {
        std::string SourcePath;
        std::string RecursiveContentHash;
        std::uint32_t MaterialAbiVersion = 0;
        std::string BindingSignatureHash;
        std::string ArtifactIdentityHash;

        [[nodiscard]] bool operator==(const MaterialShaderProgramIdentity&) const = default;
    };

    // Stable identity for the complete artifact set. This is shared by the
    // renderer and Material snapshots so a hot-reload boundary can verify the
    // exact active-backend artifact set that was published, not only the
    // source/layout hashes.
    [[nodiscard]] std::string BuildMaterialShaderArtifactIdentityHash(
        const MaterialShaderManifest& manifest);

    class CompiledMaterialShader final
    {
    public:
        [[nodiscard]] const MaterialShaderManifest& GetManifest() const noexcept;
        [[nodiscard]] const MaterialShaderProgramIdentity& GetIdentity() const noexcept;
        [[nodiscard]] std::uint64_t GetGeneration() const noexcept;
        [[nodiscard]] bool HasShader(const MaterialShaderHandleKey& key) const noexcept;
        [[nodiscard]] nvrhi::ShaderHandle FindShader(const MaterialShaderHandleKey& key) const;
        [[nodiscard]] std::span<const ShaderCacheKey> GetShaderCacheKeys() const noexcept;

    private:
        friend class MaterialShaderManager;

        using ShaderMap = std::unordered_map<
            MaterialShaderHandleKey,
            nvrhi::ShaderHandle,
            MaterialShaderHandleKeyHash>;

        CompiledMaterialShader(
            MaterialShaderManifest manifest,
            MaterialShaderProgramIdentity identity,
            std::uint64_t generation,
            ShaderMap shaders,
            std::vector<ShaderCacheKey> shaderCacheKeys);

        const MaterialShaderManifest mManifest;
        const MaterialShaderProgramIdentity mIdentity;
        const std::uint64_t mGeneration = 0;
        const ShaderMap mShaders;
        const std::vector<ShaderCacheKey> mShaderCacheKeys;
    };

    struct MaterialShaderRefreshResult
    {
        std::shared_ptr<const CompiledMaterialShader> Program;
        std::vector<MaterialShaderDiagnostic> Diagnostics;
        bool bPublished = false;

        [[nodiscard]] bool Succeeded() const noexcept;
    };

    // The engine-owned surface ABI has no user vertex deformation hook.
    // Coverage-changing and skinned materials keep conservative geometry.
    struct MaterialGeometryCapabilities
    {
        bool bAuthoredLod = false;
        bool bGpuInstances = false;
        const char* FallbackReason = "Unsupported material ABI";
    };

    [[nodiscard]] inline MaterialGeometryCapabilities ResolveMaterialGeometryCapabilities(
        const CompiledMaterialShader& program, MaterialShaderDescriptorMode descriptorMode, bool bSkinned)
    {
        if (program.GetManifest().MaterialAbiVersion != MaterialShaderAbiVersion)
            return {};
        if (bSkinned)
            return { false, false, "Skinning requires CPU submission and conservative full geometry" };
        if (program.GetManifest().BlendMode != MaterialShaderBlendMode::Opaque)
            return { false, false, "Alpha coverage and transparent ordering require CPU submission" };
        for (const auto pass : { MaterialShaderPass::Forward, MaterialShaderPass::GBuffer })
            for (const auto stage : { MaterialProgramShaderStage::Vertex, MaterialProgramShaderStage::Pixel })
                if (!program.HasShader({ pass, MaterialShaderVertexPath::GpuDriven, descriptorMode, stage }))
                    return { true, false, "GPU instance shader artifacts are missing; recompile the material" };
        return { true, true, "" };
    }

    class MaterialShaderManager final
    {
    public:
        using PublishBarrier = std::function<bool()>;

        // A null ShaderManager enables metadata-only validation and state-machine
        // tests. Artifact files and hashes are still fully validated in this mode.
        MaterialShaderManager(
            ShaderManager* shaderManager,
            std::filesystem::path cachesRoot);
        ~MaterialShaderManager();

        MaterialShaderManager(const MaterialShaderManager&) = delete;
        MaterialShaderManager& operator=(const MaterialShaderManager&) = delete;

        [[nodiscard]] MaterialShaderRefreshResult Refresh(std::string_view sourcePath);
        [[nodiscard]] bool Invalidate(std::string_view sourcePath);
        [[nodiscard]] MaterialShaderRefreshResult RefreshIfChangedAtFrameBoundary(
            std::string_view sourcePath,
            PublishBarrier beforePublish = {});

        [[nodiscard]] std::shared_ptr<const CompiledMaterialShader> GetCurrent(
            std::string_view sourcePath) const;
        // Keeps a bounded set of prior generations so immutable snapshots that
        // could not all migrate in one editor pass can still resolve their
        // exact last-known-good program.
        [[nodiscard]] std::shared_ptr<const CompiledMaterialShader> FindCompatible(
            const MaterialShaderProgramIdentity& identity) const;
        [[nodiscard]] std::vector<MaterialShaderDiagnostic> GetLastDiagnostics(
            std::string_view sourcePath) const;
        // Monotonic process-local publication revision shared by every
        // SceneRenderer using this manager. Renderers use it to retire their
        // own PSO caches after another offscreen/main renderer publishes.
        [[nodiscard]] std::uint64_t GetPublicationRevision() const;

    private:
        [[nodiscard]] MaterialShaderRefreshResult RefreshInternal(
            std::string_view sourcePath,
            bool bForce,
            const PublishBarrier& beforePublish);

        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };

    // A missing exact generation must probe publication state at the next
    // frame boundary even when the manager already has a different Current.
    // Kept as a pure predicate so the renderer's retry contract is testable
    // without constructing a graphics device.
    [[nodiscard]] bool RequiresMaterialShaderFrameBoundaryRefresh(
        const std::shared_ptr<const CompiledMaterialShader>& compatibleProgram) noexcept;
}
