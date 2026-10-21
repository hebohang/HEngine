#include "Hepch.h"

#include "Runtime/UnitTest/MaterialHotReloadTests.h"

#include "Runtime/Graphics/Material/MaterialShaderManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        constexpr std::string_view c_SourcePath = "Shaders/Materials/Test.hlsl";
        constexpr std::string_view c_ForwardV1Content = "forward-v1";
        constexpr std::string_view c_ForwardV2Content = "forward-v2";
        constexpr std::string_view c_ForwardV1Hash =
            "c0d51ec72dacf917b1a2cf943411b8e51117c85186636220f7008bff42ebfd04";
        constexpr std::string_view c_ForwardV2Hash =
            "dec04b9b62971f371654bfbf8cef7ac50cff2e3a1c79031ecaa0b4dbe0b745ab";

        class ScopedMaterialCaches final
        {
        public:
            ScopedMaterialCaches()
            {
                static std::atomic<std::uint64_t> sequence{ 0 };
                const std::uint64_t suffix =
                    static_cast<std::uint64_t>(
                        std::chrono::steady_clock::now().time_since_epoch().count()) ^
                    sequence.fetch_add(1, std::memory_order_relaxed);
                mRoot = std::filesystem::temp_directory_path() /
                    ("HEngineMaterialHotReload-" + std::to_string(suffix));
                mCachesRoot = mRoot / "CACHES";
                std::filesystem::create_directories(mCachesRoot);
            }

            ~ScopedMaterialCaches()
            {
                std::error_code error;
                std::filesystem::remove_all(mRoot, error);
            }

            [[nodiscard]] const std::filesystem::path& GetCachesRoot() const noexcept
            {
                return mCachesRoot;
            }

            void WriteArtifact(std::string_view relativePath, std::string_view content) const
            {
                WriteFile(mCachesRoot / std::filesystem::path(relativePath), content);
            }

            void WriteManifest(const MaterialShaderManifest& manifest) const
            {
                const std::string text = MaterialShaderManifestToJson(manifest).dump(2);
                WriteFile(GetManifestPath(), text);
            }

            void WriteRawManifest(std::string_view text) const
            {
                WriteFile(GetManifestPath(), text);
            }

        private:
            [[nodiscard]] std::filesystem::path GetManifestPath() const
            {
                return mCachesRoot / "Materials" / "Materials" /
                    "Test.hlsl.material.json";
            }

            static void WriteFile(
                const std::filesystem::path& path,
                std::string_view content)
            {
                std::filesystem::create_directories(path.parent_path());
                std::ofstream stream(path, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!stream)
                    throw std::runtime_error("Failed to write material hot-reload fixture.");
                stream.write(content.data(), static_cast<std::streamsize>(content.size()));
                if (!stream)
                    throw std::runtime_error("Failed to finish material hot-reload fixture.");
            }

            std::filesystem::path mRoot;
            std::filesystem::path mCachesRoot;
        };

        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        [[nodiscard]] bool HasDiagnostic(
            const MaterialShaderRefreshResult& result,
            std::string_view code)
        {
            return std::ranges::any_of(result.Diagnostics, [code](const MaterialShaderDiagnostic& diagnostic)
            {
                return diagnostic.Code == code;
            });
        }

        [[nodiscard]] MaterialShaderArtifact MakeArtifact(
            MaterialShaderPass pass,
            MaterialShaderVertexPath vertexPath,
            MaterialShaderDescriptorMode descriptorMode,
            std::string stage,
            std::string relativePath,
            std::string contentHash)
        {
            const std::string passToken = pass == MaterialShaderPass::Forward
                ? "FORWARD"
                : pass == MaterialShaderPass::Depth ? "DEPTH" : "SHADOW";
            const std::string vertexToken = vertexPath == MaterialShaderVertexPath::Static
                ? "CPU_STATIC"
                : "SKINNED";
            const std::string descriptorToken =
                descriptorMode == MaterialShaderDescriptorMode::Baseline
                ? "BASELINE"
                : "HEAP_DIRECT";
            return {
                .Pass = pass,
                .Stage = stage,
                .EntryPoint = stage == "vs" ? "HEMaterialVS" : "HEMaterialPS",
                .TargetProfile = stage == "vs"
                    ? "vs_6_0"
                    : descriptorMode == MaterialShaderDescriptorMode::HeapDirect
                        ? "ps_6_6"
                        : "ps_6_0",
                .Permutation = passToken + "_" + vertexToken + "_" + descriptorToken,
                .DescriptorMode = descriptorMode,
                .VertexPath = vertexPath,
                .RelativePath = std::move(relativePath),
                .ContentHash = std::move(contentHash),
            };
        }

        [[nodiscard]] MaterialShaderManifest MakeManifest(bool bSecondVersion)
        {
            MaterialShaderManifest manifest;
            manifest.SourcePath = "Materials/Test.hlsl";
            // Artifact identity changes independently below; keeping these two
            // hashes stable proves that the artifact set also owns generation.
            manifest.RecursiveSourceHash = std::string(64, 'a');
            manifest.BindingSignatureHash = BuildMaterialShaderBindingSignatureHash(manifest);
            constexpr MaterialShaderPass pass = MaterialShaderPass::Forward;
            const std::string relativePath = bSecondVersion
                ? "DXIL/Materials/Test/forward-v2.ps"
                : "DXIL/Materials/Test/forward-v1.ps";
            const std::string contentHash =
                std::string(bSecondVersion ? c_ForwardV2Hash : c_ForwardV1Hash);
            for (const MaterialShaderVertexPath vertexPath : {
                    MaterialShaderVertexPath::Static,
                    MaterialShaderVertexPath::Skinned })
            {
                for (const MaterialShaderDescriptorMode descriptorMode : {
                        MaterialShaderDescriptorMode::Baseline,
                        MaterialShaderDescriptorMode::HeapDirect })
                {
                    for (const std::string_view stage : {
                            std::string_view("vs"), std::string_view("ps") })
                    {
                        manifest.Artifacts.push_back(MakeArtifact(
                            pass,
                            vertexPath,
                            descriptorMode,
                            std::string(stage),
                            relativePath,
                            contentHash));
                    }
                }
            }
            return manifest;
        }

        void WriteArtifacts(const ScopedMaterialCaches& fixture, bool bSecondVersion)
        {
            fixture.WriteArtifact(
                bSecondVersion
                    ? "DXIL/Materials/Test/forward-v2.ps"
                    : "DXIL/Materials/Test/forward-v1.ps",
                bSecondVersion ? c_ForwardV2Content : c_ForwardV1Content);
        }

        void RequireFailureKeepsLastGood(
            const MaterialShaderRefreshResult& result,
            const std::shared_ptr<const CompiledMaterialShader>& lastGood,
            std::string_view message)
        {
            Require(!result.Succeeded() && !result.bPublished && result.Program == lastGood,
                message);
        }
    }

    void MaterialHotReloadContract()
    {
        ScopedMaterialCaches fixture;
        WriteArtifacts(fixture, false);
        fixture.WriteManifest(MakeManifest(false));

        MaterialShaderManager manager(nullptr, fixture.GetCachesRoot());
        Require(manager.GetPublicationRevision() == 0,
            "A new material shader manager reported a publication revision.");
        const MaterialShaderRefreshResult first = manager.Refresh(c_SourcePath);
        Require(first.Succeeded() && first.bPublished && first.Program &&
                first.Program->GetGeneration() == 1 &&
                first.Program->GetIdentity().SourcePath == c_SourcePath &&
                !first.Program->GetIdentity().ArtifactIdentityHash.empty(),
            "Initial material program did not publish generation 1.");
        Require(manager.GetPublicationRevision() == 1,
            "Initial material program publication did not advance the shared revision.");
        const MaterialShaderHandleKey forwardKey{
            .Pass = MaterialShaderPass::Forward,
            .VertexPath = MaterialShaderVertexPath::Static,
            .DescriptorMode = MaterialShaderDescriptorMode::Baseline,
            .Stage = MaterialProgramShaderStage::Pixel,
        };
        Require(first.Program->HasShader(forwardKey) &&
                !first.Program->FindShader(forwardKey),
            "Metadata-only program did not retain typed artifact lookups.");
        const auto legacyGeometry = ResolveMaterialGeometryCapabilities(
            *first.Program, MaterialShaderDescriptorMode::Baseline, false);
        Require(legacyGeometry.bAuthoredLod && !legacyGeometry.bGpuInstances &&
                std::string_view(legacyGeometry.FallbackReason).find("missing") != std::string_view::npos,
            "Legacy opaque surface must keep LOD and explicitly fall back when GPU artifacts are absent.");
        {
            ScopedMaterialCaches gpuFixture;
            WriteArtifacts(gpuFixture, false);
            auto gpuManifest = MakeManifest(false);
            const auto cpuArtifacts = gpuManifest.Artifacts;
            for (auto artifact : cpuArtifacts)
            {
                artifact.Pass = MaterialShaderPass::GBuffer;
                artifact.Permutation = "GBUFFER_" + artifact.Permutation;
                gpuManifest.Artifacts.push_back(std::move(artifact));
            }
            for (const auto pass : { MaterialShaderPass::Forward, MaterialShaderPass::GBuffer })
                for (const auto stage : { "vs", "ps" })
                    gpuManifest.Artifacts.push_back(MakeArtifact(pass,
                        MaterialShaderVertexPath::GpuDriven, MaterialShaderDescriptorMode::Baseline,
                        stage, "DXIL/Materials/Test/forward-v1.ps", std::string(c_ForwardV1Hash)));
            gpuFixture.WriteManifest(gpuManifest);
            MaterialShaderManager gpuManager(nullptr, gpuFixture.GetCachesRoot());
            const auto gpu = gpuManager.Refresh(c_SourcePath);
            Require(gpu.Succeeded() && gpu.Program, "GPU material fixture publication failed.");
            const auto capable = ResolveMaterialGeometryCapabilities(
                *gpu.Program, MaterialShaderDescriptorMode::Baseline, false);
            Require(capable.bAuthoredLod && capable.bGpuInstances,
                "Complete opaque GPU shader contract was not admitted.");
            Require(!ResolveMaterialGeometryCapabilities(*gpu.Program,
                    MaterialShaderDescriptorMode::HeapDirect, false).bGpuInstances,
                "GPU capability must check the active descriptor tier.");
            const auto skinned = ResolveMaterialGeometryCapabilities(
                *gpu.Program, MaterialShaderDescriptorMode::Baseline, true);
            Require(!skinned.bAuthoredLod && !skinned.bGpuInstances,
                "Skinned materials must retain conservative geometry and CPU submission.");
        }
        const std::span<const ShaderCacheKey> firstCacheKeys =
            first.Program->GetShaderCacheKeys();
        std::unordered_set<ShaderCacheKey, ShaderCacheKeyHash> uniqueCacheKeys(
            firstCacheKeys.begin(), firstCacheKeys.end());
        Require(firstCacheKeys.size() == first.Program->GetManifest().Artifacts.size() &&
                uniqueCacheKeys.size() == firstCacheKeys.size() &&
                std::ranges::all_of(
                    firstCacheKeys,
                    [](const ShaderCacheKey& key)
                    {
                        return key.ContentHash == c_ForwardV1Hash;
                    }),
            "Material program did not retain distinct typed shader keys for byte-identical variants.");

        const auto firstInstanceProgram = manager.GetCurrent(c_SourcePath);
        const auto secondInstanceProgram = manager.GetCurrent(c_SourcePath);
        Require(firstInstanceProgram == first.Program &&
                secondInstanceProgram == firstInstanceProgram,
            "Two material instances did not share the current compiled program.");
        Require(manager.GetCurrent("SHADERS\\MATERIALS\\TEST.HLSL") == first.Program,
            "Program lookup did not use canonical case-insensitive source identity.");

        const MaterialShaderRefreshResult unchanged = manager.Refresh(c_SourcePath);
        Require(unchanged.Succeeded() && !unchanged.bPublished &&
                unchanged.Program == first.Program &&
                unchanged.Program->GetGeneration() == 1 &&
                manager.GetPublicationRevision() == 1,
            "Unchanged material identity advanced generation or replaced the shared program.");

        const std::shared_ptr<const CompiledMaterialShader> oldProgram = first.Program;
        WriteArtifacts(fixture, true);
        fixture.WriteManifest(MakeManifest(true));
        Require(manager.Invalidate(c_SourcePath),
            "Valid material shader source could not be invalidated.");
        std::size_t publishBarrierCalls = 0;
        const MaterialShaderRefreshResult blockedPublication =
            manager.RefreshIfChangedAtFrameBoundary(
                c_SourcePath,
                [&publishBarrierCalls]()
                {
                    ++publishBarrierCalls;
                    return false;
                });
        RequireFailureKeepsLastGood(
            blockedPublication,
            oldProgram,
            "A failed publication barrier replaced the last-known-good program.");
        Require(publishBarrierCalls == 1 &&
                HasDiagnostic(blockedPublication, "MaterialProgram.PublishBarrier") &&
                manager.GetCurrent(c_SourcePath) == oldProgram &&
                manager.GetPublicationRevision() == 1,
            "A failed publication barrier did not remain retryable and diagnostic.");

        const MaterialShaderRefreshResult second =
            manager.RefreshIfChangedAtFrameBoundary(
                c_SourcePath,
                [&publishBarrierCalls]()
                {
                    ++publishBarrierCalls;
                    return true;
                });
        Require(second.Succeeded() && second.bPublished && second.Program &&
                second.Program != oldProgram && second.Program->GetGeneration() == 2 &&
                publishBarrierCalls == 2 &&
                second.Program->GetIdentity().RecursiveContentHash ==
                    oldProgram->GetIdentity().RecursiveContentHash &&
                second.Program->GetIdentity().BindingSignatureHash ==
                    oldProgram->GetIdentity().BindingSignatureHash &&
                second.Program->GetIdentity().ArtifactIdentityHash !=
                    oldProgram->GetIdentity().ArtifactIdentityHash &&
                manager.GetPublicationRevision() == 2,
            "Changed artifact identity did not atomically publish generation 2.");
        Require(oldProgram->GetGeneration() == 1 &&
                oldProgram->GetManifest().Artifacts.front().ContentHash == c_ForwardV1Hash,
            "Publishing a new program mutated an in-flight old shared snapshot.");
        Require(manager.FindCompatible(oldProgram->GetIdentity()) == oldProgram &&
                manager.FindCompatible(second.Program->GetIdentity()) == second.Program,
            "Current/previous exact program identities were not both resolvable after publication.");
        MaterialShaderProgramIdentity unknownIdentity = oldProgram->GetIdentity();
        unknownIdentity.ArtifactIdentityHash = std::string(64, 'f');
        Require(!manager.FindCompatible(unknownIdentity),
            "Compatible program lookup accepted a mismatched artifact identity.");

        const MaterialShaderRefreshResult unchangedBoundary =
            manager.RefreshIfChangedAtFrameBoundary(c_SourcePath);
        Require(unchangedBoundary.Succeeded() && !unchangedBoundary.bPublished &&
                unchangedBoundary.Program == second.Program,
            "Unchanged frame-boundary refresh replaced the current program.");

        fixture.WriteRawManifest("{ invalid json");
        const MaterialShaderRefreshResult badManifest = manager.Refresh(c_SourcePath);
        RequireFailureKeepsLastGood(
            badManifest, second.Program,
            "Invalid manifest did not preserve last-known-good material program.");
        Require(HasDiagnostic(badManifest, "Manifest.Json"),
            "Invalid manifest did not produce structured JSON diagnostics.");

        MaterialShaderManifest badAbi = MakeManifest(true);
        badAbi.MaterialAbiVersion = MaterialShaderAbiVersion + 1;
        fixture.WriteManifest(badAbi);
        const MaterialShaderRefreshResult abiFailure = manager.Refresh(c_SourcePath);
        RequireFailureKeepsLastGood(
            abiFailure, second.Program,
            "ABI mismatch did not preserve last-known-good material program.");
        Require(HasDiagnostic(abiFailure, "Manifest.AbiVersion"),
            "ABI mismatch did not produce structured diagnostics.");

        MaterialShaderManifest missingArtifact = MakeManifest(true);
        missingArtifact.RecursiveSourceHash = std::string(64, 'c');
        missingArtifact.Artifacts.front().RelativePath =
            "DXIL/Materials/Test/missing.ps";
        fixture.WriteManifest(missingArtifact);
        const MaterialShaderRefreshResult artifactFailure = manager.Refresh(c_SourcePath);
        RequireFailureKeepsLastGood(
            artifactFailure, second.Program,
            "Missing artifact did not preserve last-known-good material program.");
        Require(HasDiagnostic(artifactFailure, "MaterialProgram.ArtifactMissing"),
            "Missing artifact did not produce structured diagnostics.");
        Require(HasDiagnostic(
                MaterialShaderRefreshResult{
                    .Program = manager.GetCurrent(c_SourcePath),
                    .Diagnostics = manager.GetLastDiagnostics(c_SourcePath),
                },
                "MaterialProgram.ArtifactMissing"),
            "Manager did not retain the last failed refresh diagnostics.");

        fixture.WriteArtifact(
            "DXIL/Materials/Test/forward-v2.ps", "tampered-artifact");
        fixture.WriteManifest(MakeManifest(true));
        const MaterialShaderRefreshResult hashFailure = manager.Refresh(c_SourcePath);
        RequireFailureKeepsLastGood(
            hashFailure, second.Program,
            "Artifact hash mismatch did not preserve last-known-good material program.");
        Require(HasDiagnostic(hashFailure, "MaterialProgram.ArtifactHash"),
            "Artifact hash mismatch did not produce structured diagnostics.");
        fixture.WriteArtifact(
            "DXIL/Materials/Test/forward-v2.ps", c_ForwardV2Content);

        MaterialShaderManifest unsafeArtifact = MakeManifest(true);
        unsafeArtifact.Artifacts.front().RelativePath = "../outside.ps";
        fixture.WriteManifest(unsafeArtifact);
        const MaterialShaderRefreshResult unsafeArtifactFailure =
            manager.Refresh(c_SourcePath);
        RequireFailureKeepsLastGood(
            unsafeArtifactFailure, second.Program,
            "Unsafe artifact path did not preserve last-known-good material program.");

        MaterialShaderManifest sourceMismatch = MakeManifest(true);
        sourceMismatch.SourcePath = "Materials/Other.hlsl";
        fixture.WriteManifest(sourceMismatch);
        const MaterialShaderRefreshResult sourceMismatchFailure =
            manager.Refresh(c_SourcePath);
        RequireFailureKeepsLastGood(
            sourceMismatchFailure, second.Program,
            "Manifest source mismatch did not preserve last-known-good material program.");
        Require(HasDiagnostic(sourceMismatchFailure, "MaterialProgram.SourceMismatch"),
            "Manifest source mismatch did not produce structured diagnostics.");

        fixture.WriteManifest(MakeManifest(true));
        const MaterialShaderRefreshResult restored = manager.Refresh(c_SourcePath);
        Require(restored.Succeeded() && !restored.bPublished &&
                restored.Program == second.Program &&
                restored.Program->GetGeneration() == 2,
            "Restoring the existing last-good identity incorrectly advanced generation.");

        const MaterialShaderRefreshResult unsafeSource =
            manager.Refresh("Shaders/../Outside.hlsl");
        Require(!unsafeSource.Succeeded() && !unsafeSource.Program &&
                HasDiagnostic(unsafeSource, "MaterialProgram.SourcePath") &&
                !manager.Invalidate("C:/Outside.hlsl"),
            "Unsafe material shader source path was accepted.");
        Require(manager.GetCurrent(c_SourcePath) == second.Program &&
                oldProgram->GetGeneration() == 1,
            "Failed refreshes disturbed current or in-flight material programs.");

        std::vector<std::shared_ptr<const CompiledMaterialShader>> generations;
        generations.push_back(second.Program);
        constexpr std::string_view generationHashDigits = "0123456789abcdef";
        for (std::size_t generationIndex = 0;
            generationIndex < MaterialShaderGenerationHistoryCapacity + 2;
            ++generationIndex)
        {
            MaterialShaderManifest generationManifest = MakeManifest(true);
            generationManifest.RecursiveSourceHash = std::string(
                64,
                generationHashDigits[generationIndex % generationHashDigits.size()]);
            fixture.WriteManifest(generationManifest);
            const MaterialShaderRefreshResult generation = manager.Refresh(c_SourcePath);
            Require(generation.Succeeded() && generation.bPublished && generation.Program,
                "A synthetic material generation did not publish.");
            generations.push_back(generation.Program);
        }

        Require(generations.back()->GetGeneration() == generations.size() + 1,
            "Material program generations did not remain monotonic.");
        for (std::size_t retainedIndex = generations.size() -
                (MaterialShaderGenerationHistoryCapacity + 1);
            retainedIndex < generations.size();
            ++retainedIndex)
        {
            Require(manager.FindCompatible(generations[retainedIndex]->GetIdentity()) ==
                    generations[retainedIndex],
                "A generation inside the bounded compatibility history was not resolvable.");
        }
        Require(!manager.FindCompatible(generations[0]->GetIdentity()) &&
                !manager.FindCompatible(generations[1]->GetIdentity()),
            "Material program compatibility history exceeded its declared bound.");
        Require(generations[0]->GetGeneration() == 2 &&
                generations[0]->GetManifest().Artifacts.front().ContentHash == c_ForwardV2Hash,
            "Evicting a compatibility-history entry invalidated an externally retained shared program.");

        ScopedMaterialCaches retryFixture;
        WriteArtifacts(retryFixture, false);
        retryFixture.WriteManifest(MakeManifest(false));
        MaterialShaderManager retryManager(nullptr, retryFixture.GetCachesRoot());
        const MaterialShaderRefreshResult retryFirst = retryManager.Refresh(c_SourcePath);
        Require(retryFirst.Succeeded() && retryFirst.Program,
            "Frame-boundary retry fixture did not publish its initial program.");
        WriteArtifacts(retryFixture, true);
        const MaterialShaderManifest retryManifest = MakeManifest(true);
        retryFixture.WriteManifest(retryManifest);
        const MaterialShaderProgramIdentity retryExpectedIdentity{
            .SourcePath = std::string(c_SourcePath),
            .RecursiveContentHash = retryManifest.RecursiveSourceHash,
            .MaterialAbiVersion = retryManifest.MaterialAbiVersion,
            .BindingSignatureHash = retryManifest.BindingSignatureHash,
            .ArtifactIdentityHash =
                BuildMaterialShaderArtifactIdentityHash(retryManifest),
        };
        const std::shared_ptr<const CompiledMaterialShader> missingCompatible =
            retryManager.FindCompatible(retryExpectedIdentity);
        Require(retryManager.GetCurrent(c_SourcePath) == retryFirst.Program &&
                RequiresMaterialShaderFrameBoundaryRefresh(missingCompatible),
            "An incompatible Current incorrectly suppressed the frame-boundary refresh decision.");
        const MaterialShaderRefreshResult retried =
            retryManager.RefreshIfChangedAtFrameBoundary(c_SourcePath);
        Require(retried.Succeeded() && retried.bPublished &&
                retryManager.FindCompatible(retryExpectedIdentity) == retried.Program &&
                !RequiresMaterialShaderFrameBoundaryRefresh(retried.Program),
            "Frame-boundary refresh did not resolve the newly compatible material program.");
    }
}
