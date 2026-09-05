#include "Hepch.h"

#include "Runtime/UnitTest/MaterialResourceCacheTests.h"

#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialResourceCache.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/Material/MaterialShaderManifest.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        [[nodiscard]] std::string MakeMaterialDocument(std::string_view owner)
        {
            return std::string(R"json({
  "SchemaVersion": 2,
  "Type": "Default",
  "PassName": "BasePBR",
  "ShaderPath": "Shaders/BasePBR.hlsl",
  "VariantKeywords": [],
  "Domain": "Surface",
  "ShadingModel": "Lit",
  "BlendMode": "Opaque",
  "CullMode": "Back",
  "DepthWrite": true,
  "AlphaCutoff": 0.5,
  "Parameters": {},
  "Textures": {},
  "OrphanedParameters": {},
  "Properties": {},
  "Owner": ")json") + std::string(owner) + "\"\n}\n";
        }

        [[nodiscard]] MaterialShaderManifest MakeConcurrentManifest()
        {
            MaterialShaderManifest manifest;
            manifest.SourcePath = "Concurrent.hlsl";
            manifest.RecursiveSourceHash = std::string(64, 'a');
            for (uint32_t index = 0; index < 8; ++index)
            {
                MaterialShaderProperty property;
                property.Name = "value" + std::to_string(index);
                property.StableId = "concurrent.value." + std::to_string(index);
                property.DisplayName = property.Name;
                property.Group = "Concurrent";
                property.Source = manifest.SourcePath;
                property.Line = index + 1;
                property.Column = 1;
                property.Type = MaterialShaderPropertyType::Float;
                property.RowOffset = index;
                property.ByteOffset = index * MaterialShaderParameterAlignment;
                property.ByteSize = MaterialShaderPropertyByteSize(property.Type);
                property.Default.Numeric = { static_cast<double>(index) };
                manifest.Properties.push_back(std::move(property));
            }
            manifest.ParameterBlock.ByteSize = static_cast<uint32_t>(
                manifest.Properties.size() * MaterialShaderParameterAlignment);
            manifest.BindingSignatureHash =
                BuildMaterialShaderBindingSignatureHash(manifest);

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
                        const bool bPixel = stage == "ps";
                        const std::string permutation = std::string("FORWARD_") +
                            (vertexPath == MaterialShaderVertexPath::Static
                                ? "CPU_STATIC_"
                                : "SKINNED_") +
                            (descriptorMode == MaterialShaderDescriptorMode::Baseline
                                ? "BASELINE"
                                : "HEAP_DIRECT");
                        manifest.Artifacts.push_back({
                            .Pass = MaterialShaderPass::Forward,
                            .Stage = std::string(stage),
                            .EntryPoint = bPixel ? "HEMaterialPS" : "HEMaterialVS",
                            .TargetProfile = bPixel
                                ? (descriptorMode == MaterialShaderDescriptorMode::HeapDirect
                                    ? "ps_6_6"
                                    : "ps_6_0")
                                : "vs_6_0",
                            .Permutation = permutation,
                            .DescriptorMode = descriptorMode,
                            .VertexPath = vertexPath,
                            .RelativePath = "DXIL/Materials/Concurrent/" +
                                permutation + "." + std::string(stage),
                            .ContentHash = std::string(64, 'b'),
                        });
                    }
                }
            }
            return manifest;
        }

        [[nodiscard]] std::string MakeConcurrentMaterialDocument()
        {
            return R"json({
  "SchemaVersion": 2,
  "Type": "Custom",
  "PassName": "Concurrent",
  "ShaderPath": "Shaders/Concurrent.hlsl",
  "VariantKeywords": [],
  "Domain": "Surface",
  "ShadingModel": "Lit",
  "BlendMode": "Opaque",
  "CullMode": "Back",
  "DepthWrite": true,
  "AlphaCutoff": 0.5,
  "Parameters": {},
  "Textures": {},
  "OrphanedParameters": {},
  "Properties": {}
})json";
        }

        class ScopedMaterialCacheFixture final
        {
        public:
            ScopedMaterialCacheFixture()
            {
                static std::atomic_uint64_t sSuffix = 0;
                const auto timestamp =
                    std::chrono::steady_clock::now().time_since_epoch().count();
                mRoot = std::filesystem::temp_directory_path() /
                    ("HEngine_MaterialResourceCache_" + std::to_string(timestamp) + "_" +
                        std::to_string(sSuffix.fetch_add(1, std::memory_order_relaxed)));
                mProjectRoot = mRoot / "Project";
                mEngineContentRoot = mRoot / "EngineContent";
                mGeneratedContentRoot = mRoot / "GeneratedContent";
                mSavedRoot = mRoot / "Saved";

                Write(mProjectRoot / "Project.heproject", "{}\n");
                Write(
                    mProjectRoot / "Config/BaseEngine.json",
                    "{\"Renderer\":{\"API\":\"DX12\"}}\n");
                Write(mProjectRoot / "Config/Game.json", "{\"Game\":{}}\n");
                Write(mProjectRoot / "Config/BaseEditor.json", "{}\n");
                Write(mEngineContentRoot / "Config/BaseEditor.json", "{}\n");
                Write(mEngineContentRoot / "Shaders/BasePBR.hlsl", "// cache fixture\n");
                Write(
                    mEngineContentRoot / "Assets/Materials/Shared.hmat",
                    MakeMaterialDocument("engine"));
                Write(
                    mEngineContentRoot / "Assets/Materials/Override.hmat",
                    MakeMaterialDocument("engine"));
                Write(
                    mProjectRoot / "Assets/Materials/Override.hmat",
                    MakeMaterialDocument("project"));
                std::filesystem::create_directories(
                    mGeneratedContentRoot / "Shaders/CACHES");

                MaterialResourceCache::GetInstance().Clear();
                ConfigManager::GetInstance().Clear();
                ConfigManager::GetInstance().Init(
                    mProjectRoot / "Project.heproject",
                    mEngineContentRoot,
                    mSavedRoot,
                    mGeneratedContentRoot);
            }

            ~ScopedMaterialCacheFixture()
            {
                MaterialResourceCache::GetInstance().Clear();
                ConfigManager::GetInstance().Clear();
                std::error_code error;
                std::filesystem::remove_all(mRoot, error);
            }

            void RewriteShared(std::string_view owner) const
            {
                Write(
                    mEngineContentRoot / "Assets/Materials/Shared.hmat",
                    MakeMaterialDocument(owner));
            }

            MaterialShaderManifest PrepareConcurrentMaterial() const
            {
                MaterialShaderManifest manifest = MakeConcurrentManifest();
                Write(
                    mProjectRoot / "Shaders/Concurrent.hlsl",
                    "// concurrent material cache fixture\n");
                Write(
                    mProjectRoot / "Assets/Materials/Concurrent.hmat",
                    MakeConcurrentMaterialDocument());
                Write(
                    mGeneratedContentRoot /
                        "Shaders/CACHES/Materials/Concurrent.hlsl.material.json",
                    MaterialShaderManifestToJson(manifest).dump(2));
                return manifest;
            }

            [[nodiscard]] bool TryCreateUnsafeMaterialSymlink() const
            {
                const std::filesystem::path outsideMaterial =
                    mRoot / "Outside/Unsafe.hmat";
                Write(outsideMaterial, MakeMaterialDocument("outside"));
                const std::filesystem::path link =
                    mProjectRoot / "Assets/Materials/Unsafe.hmat";
                std::error_code error;
                std::filesystem::create_symlink(outsideMaterial, link, error);
                return !error;
            }

        private:
            static void Write(
                const std::filesystem::path& path,
                std::string_view contents)
            {
                std::filesystem::create_directories(path.parent_path());
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                Require(stream.is_open(), "Could not create material-cache test fixture.");
                stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
                stream.close();
                Require(!stream.fail(), "Could not finish material-cache test fixture.");
            }

            std::filesystem::path mRoot;
            std::filesystem::path mProjectRoot;
            std::filesystem::path mEngineContentRoot;
            std::filesystem::path mGeneratedContentRoot;
            std::filesystem::path mSavedRoot;
        };
    }

    void MaterialResourceCacheContract()
    {
        ScopedMaterialCacheFixture fixture;
        MaterialResourceCache& cache = MaterialResourceCache::GetInstance();

        Ref<Material> first = cache.Get("Assets/Materials/Shared.hmat");
        Ref<Material> equivalent = cache.Get("./Assets\\Materials\\Shared.hmat");
        Require(first == equivalent,
            "Equivalent content-relative material paths did not share one instance.");
        Require(first->mSourceDocument.value("Owner", "") == "engine",
            "An engine-only material did not resolve through the engine content fallback.");

        Ref<Material> projectOverride = cache.Get("Assets/Materials/Override.hmat");
        Require(projectOverride->mSourceDocument.value("Owner", "") == "project",
            "The project material did not override the same engine content-relative path.");

        constexpr std::size_t workerCount = 8;
        std::array<std::future<Ref<Material>>, workerCount> requests;
        for (std::future<Ref<Material>>& request : requests)
        {
            request = std::async(std::launch::async, [&cache]
            {
                return cache.Get("assets/materials/shared.hmat");
            });
        }
        for (std::future<Ref<Material>>& request : requests)
        {
            Require(request.get() == first,
                "Concurrent material requests did not converge on one shared instance.");
        }

        const std::vector<Ref<Material>> baseMaterials =
            cache.GetLiveMaterialsForShader("Assets/Shaders/BasePBR.hlsl");
        Require(std::ranges::find(baseMaterials, first) != baseMaterials.end() &&
                std::ranges::find(baseMaterials, projectOverride) != baseMaterials.end(),
            "Shader-indexed live material lookup omitted cached materials.");

        Require(cache.Invalidate("Assets\\Materials\\Shared.hmat"),
            "A canonical cached material could not be invalidated.");
        Ref<Material> afterInvalidate = cache.Get("Assets/Materials/Shared.hmat");
        Require(afterInvalidate != first,
            "Invalidating a material did not make the next request create a new instance.");

        fixture.RewriteShared("reloaded");
        const uint64_t beforeReloadRevision = afterInvalidate->GetRuntimeRevision();
        Ref<Material> reloaded = cache.Reload("Assets/Materials/Shared.hmat");
        Require(reloaded == afterInvalidate &&
                cache.Get("Assets/Materials/Shared.hmat") == reloaded &&
                reloaded->mSourceDocument.value("Owner", "") == "reloaded" &&
                reloaded->GetRuntimeRevision() != beforeReloadRevision,
            "Reload did not update the existing live material identity in place.");

        fixture.RewriteShared("published");
        Ref<Material> draft =
            CreateRef<Material>("Assets/Materials/Shared.hmat");
        const uint64_t beforePublishRevision = reloaded->GetRuntimeRevision();
        Ref<Material> published =
            cache.Publish("Assets/Materials/Shared.hmat", draft);
        Require(published == reloaded &&
                reloaded->mSourceDocument.value("Owner", "") == "published" &&
                reloaded->GetRuntimeRevision() != beforePublishRevision &&
                cache.Get("Assets/Materials/Shared.hmat") == reloaded,
            "Publishing a draft did not update the existing live cache object.");
        draft->mSourceDocument["Owner"] = "unsaved-after-publish";
        Require(reloaded->mSourceDocument.value("Owner", "") == "published" &&
                cache.Get("Assets/Materials/Shared.hmat") == reloaded,
            "Unsaved edits after publication leaked into an existing cached instance.");

        cache.Clear();
        fixture.RewriteShared("saved-private-snapshot");
        Ref<Material> privateDraft =
            CreateRef<Material>("Assets/Materials/Shared.hmat");
        Require(cache.Publish("Assets/Materials/Shared.hmat", privateDraft) == privateDraft,
            "Publishing without a live cache entry did not preserve the private draft.");
        privateDraft->mSourceDocument["Owner"] = "unsaved-private-edit";
        Ref<Material> afterClear = cache.Get("Assets/Materials/Shared.hmat");
        Require(afterClear != reloaded &&
                afterClear != privateDraft &&
                afterClear->mSourceDocument.value("Owner", "") ==
                    "saved-private-snapshot",
            "A cache-missing publish exposed later unsaved draft edits through Get.");

        cache.Clear();
        std::weak_ptr<Material> weakMaterial;
        {
            Ref<Material> transient = cache.Get("Assets/Materials/Shared.hmat");
            weakMaterial = transient;
        }
        Require(weakMaterial.expired(),
            "The material cache retained a permanent strong resource reference.");

        const MaterialShaderManifest concurrentManifest =
            fixture.PrepareConcurrentMaterial();
        Ref<Material> concurrentMaterial =
            cache.Get("Assets/Materials/Concurrent.hmat");
        const std::string expectedArtifactIdentity =
            BuildMaterialShaderArtifactIdentityHash(concurrentManifest);
        std::atomic_bool bStartConcurrency = false;
        std::future<void> refresher = std::async(
            std::launch::async,
            [&cache, &bStartConcurrency]
            {
                while (!bStartConcurrency.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (uint32_t iteration = 0; iteration < 256; ++iteration)
                {
                    std::vector<std::string> diagnostics;
                    Require(
                        cache.RefreshShaderMaterials(
                            "Shaders/Concurrent.hlsl",
                            &diagnostics) == 1 && diagnostics.empty(),
                        "Concurrent shader-targeted material refresh failed.");
                }
            });
        std::future<void> snapshotter = std::async(
            std::launch::async,
            [&concurrentMaterial, &expectedArtifactIdentity, &bStartConcurrency]
            {
                while (!bStartConcurrency.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (uint32_t iteration = 0; iteration < 2048; ++iteration)
                {
                    const RenderMeshSection::MaterialInstanceSnapshot snapshot =
                        concurrentMaterial->BuildRenderSnapshot();
                    Require(snapshot.bValid && snapshot.bCustomSurface &&
                            snapshot.ShaderPath == "Shaders/Concurrent.hlsl" &&
                            snapshot.ParameterValues.size() == 8 &&
                            snapshot.ExpectedArtifactIdentityHash ==
                                expectedArtifactIdentity &&
                            (snapshot.ExpectedProgramGeneration == 0 ||
                                snapshot.ExpectedProgramGeneration == 1),
                        "Refresh-vs-snapshot concurrency exposed torn material identity or layout.");
                    for (uint32_t index = 0; index < 8; ++index)
                    {
                        const MaterialParameterValue& value =
                            snapshot.ParameterValues[index];
                        Require(value.StableId ==
                                "concurrent.value." + std::to_string(index) &&
                                value.Numeric[0] == static_cast<float>(index),
                            "Refresh-vs-snapshot concurrency exposed torn parameter values.");
                    }
                }
            });
        bStartConcurrency.store(true, std::memory_order_release);
        refresher.get();
        snapshotter.get();
        Require(concurrentMaterial->GetExpectedProgramGeneration() == 1,
            "Concurrent shader refresh did not publish its program generation.");

        if (fixture.TryCreateUnsafeMaterialSymlink())
        {
            bool bRejectedPhysicalEscape = false;
            try
            {
                Material unsafePhysicalPath("Assets/Materials/Unsafe.hmat");
            }
            catch (const std::invalid_argument&)
            {
                bRejectedPhysicalEscape = true;
            }
            Require(bRejectedPhysicalEscape,
                "Direct material loading followed a physical symlink outside content roots.");
        }
        else
        {
            HE_CORE_INFO(
                "[Test] SKIP MaterialResourceCache physical symlink: platform permission unavailable.");
        }

        bool bRejectedTraversal = false;
        try
        {
            (void)cache.Get("../Outside.hmat");
        }
        catch (const std::invalid_argument&)
        {
            bRejectedTraversal = true;
        }
        Require(bRejectedTraversal &&
                !cache.Invalidate("C:/Outside.hmat"),
            "The material cache accepted an unsafe rooted or parent-traversal path.");

        bool bRejectedWrongExtension = false;
        try
        {
            (void)cache.Get("Assets/Materials/Shared.json");
        }
        catch (const std::invalid_argument&)
        {
            bRejectedWrongExtension = true;
        }
        Require(bRejectedWrongExtension,
            "The material cache accepted a non-.hmat resource path.");

        std::vector<std::string> diagnostics;
        Require(cache.RefreshShaderMaterials("../Outside.hlsl", &diagnostics) == 0 &&
                !diagnostics.empty(),
            "Unsafe shader-targeted material refresh did not return a diagnostic.");
    }
}
