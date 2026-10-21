#include "Hepch.h"
#include "Runtime/Graphics/Renderer/RenderResolution.h"

#include "SceneRenderer.h"
#include "DirectionalShadowMath.h"
#include "MainViewAabb.h"
#include "RenderFeature.h"
#include "SceneLightGrid.h"
#include "SceneLocalShadowPass.h"
#include "SceneShadowGpuSubmission.h"
#include "SceneGpuOcclusionPass.h"
#include "SceneGeometryArena.h"
#include "SceneIndirectCompactionPass.h"
#include "SceneBonePalette.h"

#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialParameterPackingCache.h"
#include "Runtime/Graphics/Scene/SubmittedMotionHistory.h"
#include "Runtime/Graphics/Scene/SubmittedDeformationHistory.h"
#include "Runtime/Graphics/Scene/CameraView.h"
#include "Runtime/Graphics/Scene/BakedIrradiance.h"
#include "Runtime/Graphics/Material/MaterialPipelineKey.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationCompositePass.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationTechniqueRegistry.h"
#include "Runtime/Graphics/GlobalIllumination/DepthPyramidPass.h"
#include "Runtime/Graphics/GlobalIllumination/RealtimeRayTracedGlobalIllumination.h"
#include "Runtime/Graphics/GlobalIllumination/SceneSurfaceDataPass.h"
#include "Runtime/Graphics/GlobalIllumination/ScreenSpaceGlobalIllumination.h"
#include "Runtime/Graphics/GlobalIllumination/SvgfDenoiser.h"
#include "Runtime/Graphics/GlobalIllumination/SsgiReactiveShadowScope.h"
#include "Runtime/Graphics/Mesh/Mesh.h"
#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/RayTracing/RayTracingScene.h"
#include "Runtime/Graphics/Renderer/EnvironmentMapSizes.h"
#include "Runtime/Graphics/Renderer/SceneAntiAliasingPass.h"
#include "Runtime/Graphics/Renderer/SceneUpscalingPass.h"
#include "Runtime/Graphics/RHI/PipelineCache.h"
#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/RHI/MaterialDescriptorPaging.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/RHI/TextureManager.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RenderGraph/RenderGraphResourcePool.h"
#include "Runtime/Graphics/RenderGraph/TextureResolvePass.h"
#include "Runtime/Graphics/VFX/EffekseerRenderGraphPass.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace HEngine
{
    bool IsMaterialSnapshotProgramCompatible(
        const RenderMeshSection::MaterialInstanceSnapshot& snapshot,
        const MaterialShaderProgramIdentity& programIdentity,
        uint64_t programGeneration) noexcept
    {
        // Generation is local to one MaterialShaderManager lifetime. A renderer
        // restart legitimately recreates the exact same content/artifact
        // identity at generation 1, so it cannot be part of cross-lifetime
        // compatibility. It remains in the immutable snapshot for diagnostics
        // and in pipeline keys; the hashes below are the authoritative identity.
        (void)programGeneration;
        return !snapshot.ExpectedRecursiveSourceHash.empty() &&
            !snapshot.ExpectedBindingSignature.empty() &&
            !snapshot.ExpectedArtifactIdentityHash.empty() &&
            snapshot.ExpectedMaterialAbiVersion != 0 &&
            Utils::MakeProjectPathCacheKey(snapshot.ShaderPath) ==
                Utils::MakeProjectPathCacheKey(programIdentity.SourcePath) &&
            snapshot.ExpectedRecursiveSourceHash ==
                programIdentity.RecursiveContentHash &&
            snapshot.ExpectedBindingSignature ==
                programIdentity.BindingSignatureHash &&
            snapshot.ExpectedMaterialAbiVersion ==
                programIdentity.MaterialAbiVersion &&
            snapshot.ExpectedArtifactIdentityHash ==
                programIdentity.ArtifactIdentityHash;
    }

    bool IsMaterialSnapshotRenderStateCompatible(
        const RenderMeshSection::MaterialInstanceSnapshot& snapshot,
        const MaterialShaderManifest& manifest) noexcept
    {
        const bool bBlendMatches =
            (manifest.BlendMode == MaterialShaderBlendMode::Opaque &&
                snapshot.AlphaMode == RenderMaterialAlphaMode::Opaque) ||
            (manifest.BlendMode == MaterialShaderBlendMode::Mask &&
                snapshot.AlphaMode == RenderMaterialAlphaMode::Mask) ||
            (manifest.BlendMode == MaterialShaderBlendMode::Transparent &&
                snapshot.AlphaMode == RenderMaterialAlphaMode::Blend);
        const bool bCullMatches =
            (manifest.CullMode == MaterialShaderCullMode::None) ==
                snapshot.bDoubleSided;
        return bBlendMatches && bCullMatches &&
            manifest.bDepthWrite == snapshot.bDepthWrite;
    }

    float ComputeTransparentSubMeshSortDepth(
        const glm::mat4& view,
        const glm::mat4& world,
        bool bHasLocalBounds,
        const glm::vec3& localBoundsMinimum,
        const glm::vec3& localBoundsMaximum) noexcept
    {
        const glm::vec3 localCenter = bHasLocalBounds
            ? (localBoundsMinimum + localBoundsMaximum) * 0.5f
            : glm::vec3(0.0f);
        const glm::vec4 viewPosition = view * world * glm::vec4(localCenter, 1.0f);
        const float depth = -viewPosition.z;
        return std::isfinite(depth)
            ? depth
            : -std::numeric_limits<float>::infinity();
    }

    bool IsTransparentDrawSortKeyBefore(
        const TransparentDrawSortKey& left,
        const TransparentDrawSortKey& right) noexcept
    {
        if (left.Depth != right.Depth)
            return left.Depth > right.Depth;
        if (left.EntityId != right.EntityId)
            return left.EntityId < right.EntityId;
        if (left.SectionIndex != right.SectionIndex)
            return left.SectionIndex < right.SectionIndex;
        return left.SubmissionOrder < right.SubmissionOrder;
    }

    namespace
    {
        uint64_t EnvironmentQueryHoldFrames(const std::string& key) noexcept
        {
            const char* path = std::getenv("HE_ENVIRONMENT_IBL_QUERY_HOLD_PATH");
            if (path && *path && key != path) return 0;
            static const uint64_t holdFrames = []
            {
                const char* value = std::getenv("HE_ENVIRONMENT_IBL_QUERY_HOLD_FRAMES");
                if (!value || !*value) return uint64_t{ 0 };
                char* end = nullptr;
                const unsigned long long parsed = std::strtoull(value, &end, 10);
                return end && *end == '\0' && parsed <= 1000000000
                    ? static_cast<uint64_t>(parsed)
                    : uint64_t{ 0 };
            }();
            static uint64_t matchingSubmissions = 0;
            const char* afterFirst = std::getenv("HE_ENVIRONMENT_IBL_QUERY_HOLD_AFTER_FIRST");
            if (holdFrames != 0 && afterFirst && *afterFirst == '1' &&
                matchingSubmissions++ == 0)
                return 0;
            return holdFrames;
        }

        class OffGlobalIlluminationTechnique final
            : public IGlobalIlluminationTechnique
        {
        public:
            [[nodiscard]] bool IsSupported(
                const RenderCapabilities&) const override
            {
                return true;
            }

            [[nodiscard]] GlobalIlluminationPrepareResult PrepareFrame(
                const GlobalIlluminationFrameContext&,
                const GlobalIlluminationSettings&) override
            {
                return {};
            }

            [[nodiscard]] GlobalIlluminationOutputs AddPasses(
                RenderGraph&,
                const GlobalIlluminationInputs&,
                const GlobalIlluminationSettings&) override
            {
                return {};
            }

            void ResetHistory(GlobalIlluminationHistoryResetReason) override {}
            void UpdateImportedResources(RenderGraph&) override {}
            void NotifyFrameSubmitted(uint64_t) noexcept override {}
            void AbortFrame() noexcept override {}
            void Collect(uint64_t) override {}
            void Clear(uint64_t) override {}

            [[nodiscard]] bool RequiresContinuousUpdate() const noexcept override
            {
                return false;
            }

            [[nodiscard]] GlobalIlluminationStats GetStats() const override
            {
                return {
                    .Technique = GlobalIlluminationTechniqueKind::Off,
                    .Status = "Global illumination is disabled.",
                };
            }
        };

        constexpr nvrhi::Format c_SceneColorFormat = nvrhi::Format::RGBA16_FLOAT;
        constexpr nvrhi::Format c_SceneDepthFormat = nvrhi::Format::D32;
        constexpr uint32_t c_MaxDirectionalShadowCascades = 4;
        constexpr uint32_t c_DefaultDirectionalShadowMapSize = 1024;
        constexpr float c_DirectionalShadowCascadeBlendFraction = DirectionalShadowMath::CascadeBlendFraction;
        constexpr float c_DirectionalShadowFarFadeFraction = DirectionalShadowMath::FarFadeFraction;
        constexpr uint32_t c_MaxGpuDrivenObjects = 16384;
        constexpr uint32_t c_MaxGpuDrivenBatches = 4096;
        constexpr uint32_t c_MaxGpuMaterials = 4096;
		// Vulkan implements volatile constant buffers as an aligned version ring.
		// CPU fallback objects can be written by depth, shadow cascades, forward,
		// transparent and outline passes in the same command list.
		// Reserve the additional unculled post-TAA depth replay as well.
        constexpr uint32_t c_MaxObjectConstantWritesPerFrame = 65536;
        constexpr uint32_t c_MaxMaterialParameterRows =
            c_MaxGpuMaterials * static_cast<uint32_t>(MaterialParameterMaximumPropertyCount);
        constexpr uint32_t c_MaxPrimitive2DVertices = 65536;
        constexpr uint32_t c_EnvironmentSampleCount = 256;
        constexpr uint32_t c_MaterialFlagAlphaMask = 1u << 0u;
        constexpr uint32_t c_MaterialFlagAlphaBlend = 1u << 1u;
        constexpr uint32_t c_MaterialFlagDoubleSided = 1u << 2u;
        constexpr uint32_t c_MaterialNormalEncodingShift = 3u;

        [[nodiscard]] constexpr uint32_t GetNormalMapEncoding(nvrhi::Format format) noexcept
        {
            switch (format)
            {
            case nvrhi::Format::BC5_UNORM: return 1u;
            case nvrhi::Format::BC5_SNORM: return 3u;
            default: return 0u;
            }
        }

        [[nodiscard]] bool IsIdentityMatrix(const glm::mat4& matrix) noexcept
        {
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                {
                    const float expected = column == row ? 1.0f : 0.0f;
                    if (std::abs(matrix[column][row] - expected) > 1e-6f)
                        return false;
                }
            }
            return true;
        }

        struct alignas(16) FrameConstants
        {
            glm::mat4 ViewProjection{ 1.0f };
            glm::mat4 LightViewProjection{ 1.0f };
            glm::vec4 CameraPositionAndExposure{ 0.0f, 0.0f, 0.0f, 1.0f };
            glm::vec4 DirectionalDirectionAndIntensity{ 0.0f, -1.0f, 0.0f, 0.0f };
            glm::vec4 DirectionalColor{ 1.0f };
            glm::vec4 PointPositionAndIntensity{ 0.0f };
            glm::vec4 PointColorAndRadius{ 0.0f };
            glm::vec4 ShadowSettings{ 0.0f };
            // x: IBL intensity, y: prefilter max mip, z: resources ready.
            glm::vec4 IblSettings{ 0.0f };
            glm::mat4 InverseViewProjection{ 1.0f };
            // CSM data is appended to preserve the prefix consumed by the
            // depth, sky, outline and Primitive2D shaders.
            glm::mat4 ShadowCameraView{ 1.0f };
            std::array<glm::mat4, c_MaxDirectionalShadowCascades - 1>
                AdditionalLightViewProjections{
                    glm::mat4(1.0f),
                    glm::mat4(1.0f),
                    glm::mat4(1.0f),
                };
            glm::vec4 DirectionalShadowCascadeSplits{ 0.0f };
            // x: cascade count, y: map size, z: blend fraction, w: far fade fraction.
            glm::vec4 DirectionalShadowCascadeSettings{ 1.0f, 1.0f, 0.0f, 0.0f };
            // x: cascade debug, y: PBR debug mode, z: reserved,
            // w: sanitized camera near plane used by the first cascade blend span.
            glm::vec4 DirectionalShadowDebugSettings{ 0.0f };
            // x: monotonic presentation time in seconds, yzw reserved.
            glm::vec4 MaterialTimeAndPadding{ 0.0f };
            glm::mat4 PreviousViewProjection{ 1.0f };
            glm::mat4 PreviousView{ 1.0f };
            // RGB is linear HDR fog radiance; A is extinction per world unit.
            glm::vec4 FogColorDensity{ 0.0f };
            // x: start distance, y: base height, z: height falloff.
            glm::vec4 FogDistanceHeight{ 0.0f };
            // RGB: sunward sky-haze radiance, A: intensity; x: angular exponent.
            glm::vec4 FogDirectionalColorIntensity{ 0.0f };
            // x: sunward haze exponent; yzw: presentation-only sky color tint.
            glm::vec4 FogDirectionalParams{ 4.0f, 1.0f, 1.0f, 1.0f };
            // Independent secondary distance/height scattering layer.
            glm::vec4 SecondaryFogColorDensity{ 0.0f };
            glm::vec4 SecondaryFogDistanceHeight{ 0.0f };
            BakedIrradianceGpuData BakedIrradiance;
        };

        struct alignas(16) ObjectConstants
        {
            glm::mat4 World{ 1.0f };
            glm::mat4 WorldInverseTranspose{ 1.0f };
            // x: stable GPU material, y: stable object, z: section primitive prefix, w: bone palette offset.
            glm::uvec4 MaterialObjectMeshIndices{ 0u };
            glm::mat4 PreviousWorld{ 1.0f };
            glm::vec4 StaticLightmapUvScaleBias{ 1.0f, 1.0f, 0.0f, 0.0f };
            glm::vec4 StaticLightmapScale0{ 1.0f };
            glm::vec4 StaticLightmapAdd0{ 0.0f };
            glm::vec4 StaticLightmapScale1{ 1.0f };
            glm::vec4 StaticLightmapAdd1{ 0.0f };
            glm::uvec4 StaticLightmapDescriptor{ InvalidDescriptorIndex, 0u, 0u, 0u };
        };

        struct alignas(16) GpuMaterialData
        {
            glm::vec4 BaseColor{ 1.0f };
            glm::vec4 EmissiveAndMetallic{ 0.0f };
            glm::vec4 RoughnessAoNormalAndPadding{ 1.0f, 1.0f, 1.0f, 0.0f };
            glm::uvec4 MaterialTextureIndices{ 0u };
            glm::uvec4 EmissiveSamplerFlags{ 0u };
            // x: program id, y: parameter float4-row offset,
            // z: parameter row count, w: material ABI flags.
            glm::uvec4 ProgramParameterData{ 0u };
            TextureSamplingGpu Sampling[5];
        };

        struct alignas(16) ToneMappingConstants
        {
            float Exposure = 1.0f;
            uint32_t PbrDebugMode = 0;
            uint32_t GiDebugView = 0;
            float Upscale = 0;
        };

        struct alignas(16) GpuBoundsData
        {
            glm::vec4 CenterRadius{ 0.0f };
        };

        struct alignas(16) GpuDrawData
        {
            uint32_t ObjectIndex = 0;
            uint32_t BatchIndex = 0;
            uint32_t VisibleBase = 0;
            uint32_t Padding = 0;
        };

        struct alignas(16) CullConstants
        {
            glm::mat4 ViewProjection{ 1.0f };
            uint32_t ObjectCount = 0;
            uint32_t Padding[3]{};
        };

        struct alignas(16) ShadowPassConstants
        {
            uint32_t CascadeIndex = 0;
            uint32_t VisibleBase = 0;
            uint32_t Padding[2]{};
        };

        struct alignas(16) EnvironmentPrecomputeConstants
        {
            uint32_t Mode = 0;
            uint32_t Width = 0;
            uint32_t Height = 0;
            uint32_t SampleCount = c_EnvironmentSampleCount;
            float Roughness = 0.0f;
            float Padding[3]{};
        };

        struct Primitive2DVertex
        {
            glm::vec3 Position{ 0.0f };
            glm::vec3 LineEnd{ 0.0f };
            glm::vec4 Color{ 1.0f };
            glm::vec2 TexCoord{ 0.0f };
            glm::vec2 LocalCoord{ 0.0f };
            uint32_t TextureIndex = 0;
            uint32_t Kind = 0;
            float Tiling = 1.0f;
            float Thickness = 1.0f;
            float Fade = 0.005f;
            float LineWidth = 2.0f;
        };

        struct alignas(16) Primitive2DConstants
        {
            uint32_t SamplerIndex = 0;
            uint32_t Padding[3]{};
        };

        static_assert(sizeof(FrameConstants) == 848 + sizeof(BakedIrradianceGpuData));
        static_assert(offsetof(FrameConstants, ShadowCameraView) == 304);
        static_assert(offsetof(FrameConstants, AdditionalLightViewProjections) == 368);
        static_assert(offsetof(FrameConstants, DirectionalShadowCascadeSplits) == 560);
        static_assert(offsetof(FrameConstants, DirectionalShadowCascadeSettings) == 576);
        static_assert(offsetof(FrameConstants, DirectionalShadowDebugSettings) == 592);
        static_assert(sizeof(ObjectConstants) == 304);
        static_assert(sizeof(GpuMaterialData) == 336);
        static_assert(sizeof(ToneMappingConstants) == 16);
        static_assert(sizeof(GpuBoundsData) == 16);
        static_assert(sizeof(GpuDrawData) == 16);
        static_assert(sizeof(CullConstants) == 80);
        static_assert(sizeof(ShadowPassConstants) == 16);
        static_assert(sizeof(EnvironmentPrecomputeConstants) == 32);
        static_assert(sizeof(Primitive2DVertex) == 80);
        static_assert(sizeof(Primitive2DConstants) == 16);
        static_assert(sizeof(nvrhi::DrawIndexedIndirectArguments) == 20);

        [[nodiscard]] std::string ReadShaderContentHash(const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
                throw std::runtime_error(
                    "Failed to open shader cache-key sidecar: " + Utils::PathToUtf8(path));

            std::string line;
            while (std::getline(stream, line))
            {
                constexpr std::string_view c_KeyPrefix = "key=";
                if (line.starts_with(c_KeyPrefix) && line.size() > c_KeyPrefix.size())
                    return line.substr(c_KeyPrefix.size());
            }
            throw std::runtime_error(
                "Shader cache-key sidecar has no content hash: " + Utils::PathToUtf8(path));
        }

        [[nodiscard]] uint64_t MixKey(uint64_t seed, uint64_t value) noexcept
        {
            seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
            return seed == 0 ? 1 : seed;
        }

        [[nodiscard]] uint64_t MixFramebufferSamples(
            uint64_t seed,
            const nvrhi::FramebufferInfo& framebufferInfo) noexcept
        {
            seed = MixKey(seed, framebufferInfo.sampleCount);
            return MixKey(seed, framebufferInfo.sampleQuality);
        }

        void ConfigureFramebufferMultisampling(
            nvrhi::GraphicsPipelineDesc& desc,
            const nvrhi::FramebufferInfo& framebufferInfo)
        {
            if (framebufferInfo.sampleCount > 1)
                desc.renderState.rasterState.enableMultisample();
        }

        [[nodiscard]] float Halton(uint32_t index, uint32_t base) noexcept
        {
            float value = 0.0f;
            float fraction = 1.0f;
            while (index > 0u)
            {
                fraction /= static_cast<float>(base);
                value += fraction * static_cast<float>(index % base);
                index /= base;
            }
            return value;
        }

        [[nodiscard]] glm::vec2 GetTaaJitterPixels(
            uint64_t frameNumber,
            uint64_t sequenceGeneration) noexcept
        {
            static const std::array<glm::vec2, 8> sequence = []
            {
                std::array<glm::vec2, 8> result{};
                glm::vec2 mean(0.0f);
                for (uint32_t index = 0; index < result.size(); ++index)
                {
                    result[index] = {
                        Halton(index + 1u, 2u) - 0.5f,
                        Halton(index + 1u, 3u) - 0.5f,
                    };
                    mean += result[index];
                }
                mean /= static_cast<float>(result.size());
                for (glm::vec2& jitter : result)
                    jitter -= mean;
                return result;
            }();
            const uint64_t phase = frameNumber + sequenceGeneration - 1u;
            return sequence[phase % sequence.size()];
        }

        [[nodiscard]] glm::mat4 ApplyProjectionJitter(
            const glm::mat4& projection,
            const glm::vec2& jitterPixels,
            uint32_t width,
            uint32_t height)
        {
            if (width == 0u || height == 0u)
                return projection;
            glm::mat4 clipTranslation(1.0f);
            clipTranslation[3][0] = 2.0f * jitterPixels.x /
                static_cast<float>(width);
            clipTranslation[3][1] = -2.0f * jitterPixels.y /
                static_cast<float>(height);
            return clipTranslation * projection;
        }

        void HashBytes(uint64_t& hash, const void* data, size_t size) noexcept
        {
            constexpr uint64_t FnvPrime = 1099511628211ull;
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index)
            {
                hash ^= bytes[index];
                hash *= FnvPrime;
            }
        }

        template<typename Value>
        void HashValue(uint64_t& hash, const Value& value) noexcept
        {
            HashBytes(hash, &value, sizeof(value));
        }

        void HashString(uint64_t& hash, std::string_view value) noexcept
        {
            HashBytes(hash, value.data(), value.size());
            constexpr uint8_t Terminator = 0xff;
            HashBytes(hash, &Terminator, sizeof(Terminator));
        }

        [[nodiscard]] uint64_t FingerprintProjection(
            const RenderCamera& camera) noexcept
        {
            uint64_t hash = 14695981039346656037ull;
            HashValue(hash, camera.ProjectionGeneration);
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                    HashValue(hash, camera.Projection[column][row]);
            }
            return hash == 0 ? 1 : hash;
        }

        [[nodiscard]] uint64_t FingerprintOfflineViewportSceneState(
            const RenderWorld& world) noexcept
        {
            uint64_t hash = 14695981039346656037ull;
            HashValue(hash, world.SceneGeneration);
            HashValue(hash, world.Objects.size());
            for (const RenderObject& object : world.Objects)
            {
                HashValue(hash, object.EntityId);
                HashValue(hash, object.bVisible);
                HashString(hash, object.MeshAssetPath);
                for (glm::length_t column = 0; column < 4; ++column)
                {
                    for (glm::length_t row = 0; row < 4; ++row)
                        HashValue(hash, object.WorldTransform[column][row]);
                }
                HashValue(hash, object.Sections.size());
                for (const RenderMeshSection& section : object.Sections)
                {
                    HashValue(hash, section.GetMaterialInstance().Revision);
                    HashValue(hash, section.GetMaterialInstance().BaseColor);
                    HashValue(hash, section.GetMaterialInstance().Emissive);
                    HashValue(hash, section.GetMaterialInstance().Metallic);
                    HashValue(hash, section.GetMaterialInstance().Roughness);
                }
            }
            return hash == 0 ? 1 : hash;
        }

        [[nodiscard]] uint64_t FingerprintScreenSpaceTransportGeometry(
            const RenderWorld& world) noexcept
        {
            uint64_t hash = 14695981039346656037ull;
            for (const RenderObject& object : world.Objects)
            {
                if (!object.bVisible || !std::any_of(object.Sections.begin(), object.Sections.end(),
                    [](const RenderMeshSection& section) {
                        return section.GetMaterialInstance().bDepthWrite &&
                            section.AlphaMode != RenderMaterialAlphaMode::Blend;
                    }))
                    continue;
                // Raster SSGI also observes textured, smooth and skinned geometry
                // excluded by the hardware-RT scene's transform revision.
                HashValue(hash, object.EntityId);
                HashValue(hash, object.WorldTransform);
                for (const glm::mat4& bone : object.BonePalette)
                    HashValue(hash, bone);
            }
            return hash == 0 ? 1 : hash;
        }

        [[nodiscard]] RayTracingSceneRevisions BuildRayTracingSceneRevisions(
            const RenderWorld& world, bool bBasePbrMaterials)
        {
            RayTracingSceneRevisions revisions;
            revisions.GeometryContentRevisions.reserve(world.Objects.size());
            uint64_t topology = 14695981039346656037ull;
            uint64_t transforms = 14695981039346656037ull;
            uint64_t materials = 14695981039346656037ull;
            uint64_t lights = 14695981039346656037ull;
            HashValue(topology, world.Objects.size());
            for (const RenderObject& object : world.Objects)
            {
                HashValue(topology, object.EntityId);
                HashString(topology, object.MeshAssetPath);
                HashValue(topology, object.bVisible);
                HashValue(topology, object.Sections.size());
                bool bHasSupportedSection = false;
                for (uint32_t sectionIndex = 0;
                    sectionIndex < static_cast<uint32_t>(object.Sections.size());
                    ++sectionIndex)
                {
                    const RenderMeshSection& section = object.Sections[sectionIndex];
                    HashValue(topology, sectionIndex);
                    HashValue(topology, section.AlphaMode);
                    HashValue(topology, section.bDoubleSided);
                    const RayTracingGeometrySupport support = object.bVisible
                        ? ClassifyRayTracingGeometry(object, section, bBasePbrMaterials)
                        : RayTracingGeometrySupport::MissingGeometry;
                    HashValue(topology, support);
                    if (support ==
                        RayTracingGeometrySupport::SupportedStaticOpaque)
                    {
                        bHasSupportedSection = true;
                        HashValue(
                            topology,
                            MemoizeRayTracingGeometryContentRevision(
                                revisions.GeometryContentRevisions,
                                *section.SubMeshResource));
                    }
                    const auto& material = section.GetMaterialInstance();
                    HashString(materials, material.AssetPath);
                    HashString(materials, material.ShaderPath);
                    HashValue(materials, material.Revision);
                    HashValue(materials, material.BaseColor);
                    HashValue(materials, material.Emissive);
                    HashValue(materials, material.Metallic);
                    HashValue(materials, material.Roughness);
                    if (bBasePbrMaterials)
                    {
                        HashValue(materials, material.NormalScale);
                        HashValue(materials, material.AlphaCutoff);
                        for (const auto& texture : material.LegacyTextures)
                        {
                            HashString(materials, texture.AssetPath);
                            HashValue(materials, texture.bEnabled);
                        }
                        for (const auto& parameter : material.ParameterValues)
                        {
                            HashString(materials, parameter.Name);
                            HashString(materials, parameter.TexturePath);
                            HashValue(materials, parameter.Numeric);
                        }
                    }
                }
                if (bHasSupportedSection)
                {
                    HashValue(transforms, object.EntityId);
                    HashString(transforms, object.MeshAssetPath);
                    HashValue(transforms, object.WorldTransform);
                    HashValue(transforms, object.PreviousWorldTransform);
                }
            }
            for (const RenderPointLight& light : world.PointLights)
            {
                HashValue(lights, light.Position);
                HashValue(lights, light.Intensity);
                HashValue(lights, light.Color);
                HashValue(lights, light.Radius);
            }
            for (const RenderDirectionalLight& light : world.DirectionalLights)
            {
                HashValue(lights, light.Direction);
                HashValue(lights, light.Intensity);
                HashValue(lights, light.Color);
            }
            for (const auto& light : world.SpotLights)
            {
                HashValue(lights, light.Position);
                HashValue(lights, light.Direction);
                HashValue(lights, light.Intensity);
                HashValue(lights, light.Color);
                HashValue(lights, light.Radius);
                HashValue(lights, light.InnerConeCosine);
                HashValue(lights, light.OuterConeCosine);
            }
            HashString(lights, world.Environment.HdrTexturePath);
            HashValue(lights, world.Environment.IblIntensity);
            revisions.Scene = std::max<uint64_t>(world.SceneGeneration, 1);
            revisions.GeometryTopology = topology == 0 ? 1 : topology;
            revisions.Transform = transforms == 0 ? 1 : transforms;
            revisions.Material = materials == 0 ? 1 : materials;
            revisions.Light = lights == 0 ? 1 : lights;
            return revisions;
        }

        struct DirectionalShadowData
        {
            std::array<glm::mat4, c_MaxDirectionalShadowCascades> LightViewProjections{
                glm::mat4(1.0f),
                glm::mat4(1.0f),
                glm::mat4(1.0f),
                glm::mat4(1.0f),
            };
            glm::vec4 CascadeSplits{ 0.0f };
        };

        [[nodiscard]] DirectionalShadowData BuildDirectionalShadowData(
            const RenderWorld& world,
            const RenderDirectionalLight& light,
            uint32_t cascadeCount,
            uint32_t shadowMapSize)
        {
            DirectionalShadowData data;
            glm::vec3 lightDirection = light.Direction;
            if (glm::dot(lightDirection, lightDirection) < 1e-8f)
                lightDirection = glm::vec3(0.0f, -1.0f, 0.0f);
            lightDirection = glm::normalize(lightDirection);

            const float nearPlane = DirectionalShadowMath::SanitizeNearPlane(world.Camera.NearPlane);
            const float requestedFar = std::max(light.ShadowDistance, nearPlane + 1.0f);
            const float cameraFar =
                std::isfinite(world.Camera.FarPlane) && world.Camera.FarPlane > nearPlane
                ? world.Camera.FarPlane
                : requestedFar;
            const float farPlane =
                std::max(std::min(cameraFar, requestedFar), nearPlane + 0.001f);
            const float lambda = std::clamp(light.CascadeLambda, 0.0f, 1.0f);

            std::array<float, c_MaxDirectionalShadowCascades> splits{};
            for (uint32_t cascade = 0; cascade < cascadeCount; ++cascade)
            {
                const float ratio =
                    static_cast<float>(cascade + 1u) / static_cast<float>(cascadeCount);
                splits[cascade] = DirectionalShadowMath::ComputeSplit(
                    nearPlane,
                    farPlane,
                    ratio,
                    lambda);
            }
            for (uint32_t cascade = cascadeCount;
                cascade < c_MaxDirectionalShadowCascades;
                ++cascade)
            {
                splits[cascade] = farPlane;
            }

            for (uint32_t cascade = 0; cascade < cascadeCount; ++cascade)
            {
                const float cascadeNear = DirectionalShadowMath::ComputeSliceNear(
                    cascade, nearPlane, splits);
                data.LightViewProjections[cascade] = DirectionalShadowMath::ComputeCascadeMatrix(
                    world.Camera.View,
                    world.Camera.Projection,
                    cascadeNear,
                    splits[cascade],
                    lightDirection,
                    shadowMapSize);
                data.CascadeSplits[cascade] = splits[cascade];
            }
            return data;
        }

        struct OpaqueFramebufferKey
        {
            nvrhi::ITexture* Color = nullptr;
            nvrhi::ITexture* Depth = nullptr;

            [[nodiscard]] bool operator==(const OpaqueFramebufferKey&) const = default;
        };

        struct OpaqueFramebufferKeyHash
        {
            [[nodiscard]] size_t operator()(const OpaqueFramebufferKey& key) const noexcept
            {
                uint64_t value = reinterpret_cast<uintptr_t>(key.Color);
                value = MixKey(value, reinterpret_cast<uintptr_t>(key.Depth));
                return static_cast<size_t>(value);
            }
        };

        struct ShadowFramebufferKey
        {
            nvrhi::ITexture* Depth = nullptr;
            uint32_t ArraySlice = 0;

            [[nodiscard]] bool operator==(const ShadowFramebufferKey&) const = default;
        };

        struct ShadowFramebufferKeyHash
        {
            [[nodiscard]] size_t operator()(const ShadowFramebufferKey& key) const noexcept
            {
                uint64_t value = reinterpret_cast<uintptr_t>(key.Depth);
                value = MixKey(value, key.ArraySlice);
                return static_cast<size_t>(value);
            }
        };

        struct SceneUploadPassData
        {
            BufferHandle FrameConstantsBuffer;
            std::vector<BufferHandle> VertexBuffers;
            std::vector<BufferHandle> IndexBuffers;
            TextureHandle DefaultEnvironment;
            TextureHandle DefaultBrdfLut;
            BufferHandle GpuObjects;
            BufferHandle GpuMaterials;
            BufferHandle MaterialParameters;
            BufferHandle GpuBounds;
            BufferHandle GpuDrawData;
            BufferHandle VisibleObjectIndices;
            BufferHandle IndirectArguments;
            BufferHandle Primitive2DVertices;
            BufferHandle BoneMatrices;
            BufferHandle PreviousBoneMatrices;
        };

        struct GpuCullingPassData
        {
            BufferHandle GpuObjects;
            BufferHandle GpuBounds;
            BufferHandle GpuDrawData;
            BufferHandle VisibleObjectIndices;
            BufferHandle IndirectArguments;
        };

        struct DepthPrepassPassData
        {
            TextureHandle SceneDepth;
            bool bPostTaa = false;
            BufferHandle FrameConstantsBuffer;
            BufferHandle ObjectConstantsBuffer;
            BufferHandle GpuObjects;
            BufferHandle GpuMaterials;
            BufferHandle MaterialParameters;
            BufferHandle VisibleObjectIndices;
            BufferHandle IndirectArguments;
            BufferHandle BoneMatrices;
            std::vector<BufferHandle> VertexBuffers;
            std::vector<BufferHandle> IndexBuffers;
            std::vector<TextureHandle> MaterialTextures;
        };

        struct OpaquePassData
        {
            TextureHandle SceneColor;
            TextureHandle SceneDepth;
            BufferHandle FrameConstantsBuffer;
            BufferHandle ObjectConstantsBuffer;
            std::vector<BufferHandle> VertexBuffers;
            std::vector<BufferHandle> IndexBuffers;
            std::vector<TextureHandle> MaterialTextures;
            BufferHandle GpuObjects;
            BufferHandle GpuMaterials;
            BufferHandle MaterialParameters;
            BufferHandle VisibleObjectIndices;
            BufferHandle IndirectArguments;
            TextureHandle DirectionalShadow;
            TextureHandle Irradiance;
            TextureHandle Prefilter;
            TextureHandle BrdfLut;
        };

        struct SkyPassData
        {
            TextureHandle SceneColor;
            TextureHandle SceneDepth;
            TextureHandle Environment;
            BufferHandle FrameConstantsBuffer;
        };

        struct DirectionalShadowPassData
        {
            TextureHandle ShadowDepth;
            BufferHandle FrameConstantsBuffer;
            BufferHandle LightConstantsBuffer;
            BufferHandle LightRecords;
            BufferHandle ClusterMasks;
            BufferHandle ObjectConstantsBuffer;
            BufferHandle BoneMatrices;
            BufferHandle PreviousBoneMatrices;
            BufferHandle GpuMaterials;
            BufferHandle MaterialParameters;
            std::vector<BufferHandle> VertexBuffers;
            std::vector<BufferHandle> IndexBuffers;
            std::vector<TextureHandle> MaterialTextures;
        };

        struct GpuCullingReadbackPassData
        {
            BufferHandle IndirectArguments;
            BufferHandle Readback;
        };

        struct ToneMappingPassData
        {
            TextureHandle SceneColor;
            TextureHandle BackBuffer;
        };

        struct Primitive2DPassData
        {
            TextureHandle SceneColor;
            TextureHandle SceneDepth;
            BufferHandle FrameConstantsBuffer;
            BufferHandle Vertices;
            std::vector<TextureHandle> Textures;
        };

        struct TransparentPassData
        {
            TextureHandle SceneColor;
            TextureHandle SceneDepth;
            BufferHandle FrameConstantsBuffer;
            BufferHandle ObjectConstantsBuffer;
            BufferHandle GpuMaterials;
            BufferHandle MaterialParameters;
            BufferHandle BoneMatrices;
            std::vector<BufferHandle> VertexBuffers;
            std::vector<BufferHandle> IndexBuffers;
            std::vector<TextureHandle> MaterialTextures;
            TextureHandle DirectionalShadow;
            TextureHandle Irradiance;
            TextureHandle Prefilter;
            TextureHandle BrdfLut;
        };

        using SkinnedPassData = TransparentPassData;

        struct OutlinePassData
        {
            TextureHandle SceneColor;
            TextureHandle SceneDepth;
            BufferHandle FrameConstantsBuffer;
            BufferHandle ObjectConstantsBuffer;
            BufferHandle BoneMatrices;
            BufferHandle GpuMaterials;
            BufferHandle MaterialParameters;
            std::vector<BufferHandle> VertexBuffers;
            std::vector<BufferHandle> IndexBuffers;
            std::vector<TextureHandle> MaterialTextures;
        };

        struct PresentPassData
        {
            TextureHandle BackBuffer;
        };

        struct OverlayPassData
        {
            TextureHandle BackBuffer;
            TextureHandle OffscreenColor;
        };

        struct RuntimeUiBackdropCopyPassData
        {
            TextureHandle Source;
            TextureHandle Destination;
        };

        struct CapturePassData
        {
            TextureHandle BackBuffer;
        };

        struct LinearHdrCapturePassData
        {
            TextureHandle SceneColor;
            TextureHandle DiagnosticInput;
            TextureHandle SelectedNativeInput;
        };

        struct OffscreenCopyPassData
        {
            TextureHandle ColorSource;
            TextureHandle DepthSource;
            TextureHandle ColorDestination;
            TextureHandle DepthDestination;
        };

		struct ClearOutputPassData
		{
			TextureHandle BackBuffer;
		};

        struct OffscreenExportPassData
        {
            TextureHandle Color;
            TextureHandle Depth;
        };
    }

    struct SceneRenderer::Impl
    {
        struct EnvironmentEntry
        {
            SceneEnvironmentState State = SceneEnvironmentState::Unrequested;
            std::string Path;
            std::string Error;
            ManagedTextureHandle SourceAsset;
            nvrhi::TextureHandle SourceEquirectangular;
            nvrhi::TextureHandle EnvironmentCdf;
            std::shared_ptr<const EnvironmentSamplingProposal> EnvironmentProposal;
            nvrhi::TextureHandle Environment;
            nvrhi::TextureHandle Irradiance;
            nvrhi::TextureHandle Prefilter;
            nvrhi::EventQueryHandle PreparationComplete;
            nvrhi::CommandListHandle PreparationCommandList;
            std::vector<nvrhi::BindingSetHandle> PreparationBindings;
            uint64_t PreparationSubmission = 0;
            uint64_t NotBeforePumpFrame = 0;
            bool bCancelled = false;
            bool bReloadRequested = false;
        };

        struct RetiredEnvironmentEntry
        {
            EnvironmentEntry Resource;
            uint64_t RetireAfterSubmission = 0;
        };

        ~Impl() noexcept
        {
            for (auto& [path, entry] : EnvironmentCache)
            {
                (void)path;
                try
                {
                    if (entry.PreparationComplete)
                    {
                        Device->waitEventQuery(entry.PreparationComplete);
                        Device->resetEventQuery(entry.PreparationComplete);
                    }
                    if (TextureManagerInstance.IsValid(entry.SourceAsset))
                        TextureManagerInstance.ReleaseTexture(
                            entry.SourceAsset, LastSubmittedSubmission);
                }
                catch (const std::exception& exception)
                {
                    Log::WriteFallbackError(
                        "SceneRenderer environment cleanup failed", exception.what());
                }
                catch (...)
                {
                    Log::WriteFallbackError(
                        "SceneRenderer environment cleanup failed", "unknown exception");
                }
            }
        }

        struct EnvironmentBindingKey
        {
            nvrhi::ITexture* Shadow = nullptr;
            nvrhi::ITexture* Irradiance = nullptr;
            nvrhi::ITexture* Prefilter = nullptr;
            nvrhi::ITexture* BrdfLut = nullptr;

            [[nodiscard]] bool operator==(const EnvironmentBindingKey&) const = default;
        };

        struct EnvironmentBindingKeyHash
        {
            [[nodiscard]] size_t operator()(const EnvironmentBindingKey& key) const noexcept
            {
                uint64_t value = reinterpret_cast<uintptr_t>(key.Shadow);
                value = MixKey(value, reinterpret_cast<uintptr_t>(key.Irradiance));
                value = MixKey(value, reinterpret_cast<uintptr_t>(key.Prefilter));
                value = MixKey(value, reinterpret_cast<uintptr_t>(key.BrdfLut));
                return static_cast<size_t>(value);
            }
        };

        struct SkyBindingKey
        {
            nvrhi::ITexture* Environment = nullptr;

            [[nodiscard]] bool operator==(const SkyBindingKey&) const = default;
        };

        struct SkyBindingKeyHash
        {
            [[nodiscard]] size_t operator()(const SkyBindingKey& key) const noexcept
            {
                return static_cast<size_t>(reinterpret_cast<uintptr_t>(key.Environment));
            }
        };

        struct MeshEntry
        {
            struct LodRange
            {
                uint32_t FirstIndex = 0;
                uint32_t IndexCount = 0;
                uint32_t PrimitiveOffset = 0;
                float ObjectSpaceError = 0.0f;
            };
            Ref<SubMesh> Resource;
            SceneGeometryArenaAllocation ArenaAllocation;
            nvrhi::BufferHandle VertexBuffer;
            nvrhi::BufferHandle IndexBuffer;
            uint32_t IndexCount = 0;
            std::vector<uint32_t> ResidentIndices;
            std::vector<LodRange> Lods;
            uint32_t GraphIndex = InvalidRenderGraphIndex;
            bool bUploaded = false;
            uint64_t DeformationRevision = 0;
            SubmittedDeformationHistory DeformationHistory;
            bool bDynamicVertices = false;
            bool bSkinned = false;
        };

        struct GraphGeometry
        {
            const SubMesh* Key = nullptr;
            BufferHandle VertexBuffer;
            BufferHandle IndexBuffer;
        };

        struct GraphMaterialTexture
        {
            TextureHandle Handle;
            nvrhi::TextureHandle Resource;
        };

        struct DrawItem
        {
            const SubMesh* Key = nullptr;
            uint32_t LodIndex = 0;
            ObjectConstants Constants;
            std::shared_ptr<const CompiledMaterialShader> MaterialProgram;
            TransparentDrawSortKey SortKey;
            bool bDoubleSided = false;
            bool bSkinned = false;
            bool bGpuShadow = false;
        };

        struct GpuBatchKey
        {
            const SubMesh* Mesh = nullptr;
            const Material* MaterialResource = nullptr;
            uint32_t LodIndex = 0;
            bool bDoubleSided = false;

            [[nodiscard]] bool operator==(const GpuBatchKey&) const = default;
        };

        struct GpuBatchKeyHash
        {
            [[nodiscard]] size_t operator()(const GpuBatchKey& key) const noexcept
            {
                uint64_t value = reinterpret_cast<uintptr_t>(key.Mesh);
                value = MixKey(value, reinterpret_cast<uintptr_t>(key.MaterialResource));
                value = MixKey(value, key.LodIndex);
                value = MixKey(value, key.bDoubleSided ? 1u : 0u);
                return static_cast<size_t>(value);
            }
        };

        struct GpuBatch
        {
            GpuBatchKey Key;
            std::shared_ptr<const CompiledMaterialShader> MaterialProgram;
            uint32_t VisibleBase = 0;
            uint32_t ObjectCount = 0;
			uint32_t MaterialIndex = 0;
        };

        struct MaterialTextureSet
        {
            std::string Signature;
            std::array<ManagedTextureHandle, 5> Handles{};
            std::array<uint32_t, 5> ShaderIndices{};
            uint32_t NormalEncoding = 0;
            uint64_t LastUsedFrame = 0;
            uint64_t TextureRevision = 0;
        };

        struct MaterialParameterTextureSet
        {
            std::string Signature;
            std::vector<ManagedTextureHandle> Handles;
            std::vector<uint32_t> ShaderIndices;
			std::vector<uint32_t> NormalEncodings;
			std::vector<DefaultBindlessTexture> Fallbacks;
			std::vector<uint32_t> RowOffsets;
            uint64_t LastUsedFrame = 0;
            uint64_t TextureRevision = 0;
        };

		struct FrameMaterialDescriptorInput
		{
			uint32_t MaterialIndex = 0;
			std::vector<nvrhi::TextureHandle> Textures;
			std::vector<uint32_t> ParameterRowOffsets;
			bool bCustomParameters = false;
		};

		struct MaterialDescriptorPageState
		{
			std::vector<MaterialDescriptorTextureKey> TextureKeys;
			std::vector<nvrhi::TextureHandle> Textures;
			nvrhi::BindingSetHandle BindingSet;
		};

		struct RetiredMaterialDescriptorPages
		{
			uint64_t Submission = 0;
			std::vector<MaterialDescriptorPageState> Pages;
		};

        struct MaterialRegistryKey
        {
            uint32_t SnapshotIndex = InvalidGpuSceneIndex;
            const Material* FallbackResource = nullptr;

            [[nodiscard]] bool operator==(const MaterialRegistryKey&) const = default;
        };

        struct MaterialRegistryKeyHash
        {
            [[nodiscard]] size_t operator()(const MaterialRegistryKey& key) const noexcept
            {
                uint64_t value = key.SnapshotIndex;
                value = MixKey(value, reinterpret_cast<uintptr_t>(key.FallbackResource));
                return static_cast<size_t>(value);
            }
        };

        struct Primitive2DBatch
        {
            RenderPrimitiveDepthMode DepthMode = RenderPrimitiveDepthMode::DepthTest;
            uint32_t FirstVertex = 0;
            uint32_t VertexCount = 0;
        };

        struct Primitive2DBufferSlot
        {
            nvrhi::BufferHandle Vertices;
        };

        struct GpuReadbackSlot
        {
            nvrhi::BufferHandle Buffer;
            uint64_t Submission = 0;
            uint32_t BatchCount = 0;
            uint32_t ObjectCount = 0;
            bool bPending = false;
        };

        class CpuRenderGraphObserver final : public IRenderGraphCpuObserver
        {
        public:
            void Begin(SceneRendererStats& stats)
            {
                mStats = &stats;
                mUsed = 0;
                stats.bCpuGraphTimingValid = true;
            }
            void End()
            {
                mStats->CpuGraphTimings.resize(mUsed);
                mStats->CpuGraphTimingCount = static_cast<uint32_t>(mUsed);
                mStats = nullptr;
            }
            void OnCpuPhase(RenderGraphCpuPhase phase, uint32_t executionIndex,
                std::string_view name, double milliseconds, bool completed) noexcept override
            {
                if (!mStats) return;
                static constexpr const char* labels[] = {
                    "Compile", "Realize", "Validate", "Setup", "Barriers", "Record", "Trailing"};
                try
                {
                    auto& timings = mStats->CpuGraphTimings;
                    if (mUsed == timings.size()) timings.emplace_back();
                    auto& timing = timings[mUsed++];
                    timing.Phase = labels[static_cast<size_t>(phase)];
                    timing.PhaseIndex = static_cast<uint8_t>(phase);
                    timing.ExecutionIndex = executionIndex;
                    if (timing.Name != name) timing.Name.assign(name);
                    timing.Milliseconds = milliseconds;
                    timing.bCompleted = completed;
                    mStats->bCpuGraphTimingValid &= completed;
                }
                catch (...)
                {
                    // Diagnostics must never throw while unwinding a failed pass.
                    mStats->bCpuGraphTimingValid = false;
                }
            }
        private:
            SceneRendererStats* mStats = nullptr;
            size_t mUsed = 0;
        };

        class GpuRenderGraphInstrumentation final : public IRenderGraphInstrumentation
        {
        public:
            struct PassTiming
            {
                std::string Name;
                double TotalMilliseconds = 0.0;
                double LastMilliseconds = 0.0;
                double PeakMilliseconds = 0.0;
                uint64_t SampleCount = 0;
                std::vector<double> RecentSamples;
            };

            GpuRenderGraphInstrumentation(
                nvrhi::IDevice* device,
                uint32_t framesInFlight,
                bool enableGpuTiming)
                : mDevice(device)
                , mSlots(framesInFlight)
                , bEnabled(enableGpuTiming)
            {
            }

            void BeginFrame(uint32_t frameSlot, uint64_t completedSubmission,
                nvrhi::ICommandList* commandList)
            {
                if (!bEnabled)
                    return;

                CollectSlot(frameSlot, completedSubmission, false);
                Slot& slot = mSlots.at(frameSlot);
                if (slot.bPending)
                {
                    throw std::logic_error(
                        "SceneRenderer GPU timing slot was reused before its submission completed");
                }
                if (slot.bRecording || slot.bRecorded)
                    throw std::logic_error("SceneRenderer GPU timing slot is already active");

                for (uint32_t index = 0; index < slot.UsedQueryCount; ++index)
                {
                    if (slot.Queries[index].Handle)
                        mDevice->resetTimerQuery(slot.Queries[index].Handle);
                }
                slot.UsedQueryCount = 0;
                slot.bAllQueriesAvailable = true;
                slot.bRecording = true;
                mCurrentFrameSlot = frameSlot;
                if (!slot.FrameQuery) slot.FrameQuery = mDevice->createTimerQuery();
                if (slot.FrameQuery)
                {
                    mDevice->resetTimerQuery(slot.FrameQuery);
                    commandList->beginTimerQuery(slot.FrameQuery);
                }
                else slot.bAllQueriesAvailable = false;
            }

            void EndFrame(uint32_t frameSlot, nvrhi::ICommandList* commandList)
            {
                if (!bEnabled)
                    return;
                Slot& slot = mSlots.at(frameSlot);
                if (!slot.bRecording || mCurrentFrameSlot != frameSlot)
                    throw std::logic_error("SceneRenderer GPU timing frame is not recording");
                if (slot.FrameQuery) commandList->endTimerQuery(slot.FrameQuery);
                slot.bRecording = false;
                slot.bRecorded = true;
                mCurrentFrameSlot = InvalidRenderGraphIndex;
            }

            void NotifySubmitted(uint32_t frameSlot, uint64_t submission)
            {
                if (!bEnabled)
                    return;
                Slot& slot = mSlots.at(frameSlot);
                if (!slot.bRecorded)
                    return;
                slot.Submission = submission;
                slot.bRecorded = false;
                slot.bPending = true;
            }

            void AbortFrame(uint32_t frameSlot) noexcept
            {
                if (!bEnabled || frameSlot >= mSlots.size())
                    return;
                Slot& slot = mSlots[frameSlot];
                // A submitted slot is owned by the GPU and must be collected,
                // never reset through the CPU-recording abort path.
                if (slot.bPending)
                    return;
                for (uint32_t index = 0; index < slot.UsedQueryCount; ++index)
                {
                    if (slot.Queries[index].Handle)
                        mDevice->resetTimerQuery(slot.Queries[index].Handle);
                }
                if (slot.FrameQuery) mDevice->resetTimerQuery(slot.FrameQuery);
                slot.Submission = 0;
                slot.UsedQueryCount = 0;
                slot.bAllQueriesAvailable = true;
                slot.bRecording = false;
                slot.bRecorded = false;
                slot.bPending = false;
                if (mCurrentFrameSlot == frameSlot)
                    mCurrentFrameSlot = InvalidRenderGraphIndex;
            }

            void CollectAll()
            {
                if (!bEnabled)
                    return;
                for (uint32_t frameSlot = 0; frameSlot < mSlots.size(); ++frameSlot)
                {
                    CollectSlot(
                        frameSlot,
                        std::numeric_limits<uint64_t>::max(),
                        true);
                }
            }

            [[nodiscard]] nvrhi::ITimerQuery* GetTimerQuery(
                uint32_t executionIndex,
                std::string_view passName) override
            {
                if (!bEnabled || mCurrentFrameSlot == InvalidRenderGraphIndex)
                    return nullptr;

                Slot& slot = mSlots.at(mCurrentFrameSlot);
                if (slot.Queries.size() <= executionIndex)
                    slot.Queries.resize(static_cast<size_t>(executionIndex) + 1u);
                TimerRecord& query = slot.Queries[executionIndex];
                if (!query.Handle)
                    query.Handle = mDevice->createTimerQuery();
                query.PassName.assign(passName);
                slot.UsedQueryCount = std::max(
                    slot.UsedQueryCount,
                    executionIndex + 1u);
                if (!query.Handle)
                {
                    slot.bAllQueriesAvailable = false;
                    return nullptr;
                }
                return query.Handle;
            }

            [[nodiscard]] bool IsEnabled() const noexcept
            {
                return bEnabled;
            }
            void Enable() noexcept { bEnabled = true; }

            [[nodiscard]] double GetAverageMilliseconds() const noexcept
            {
                return mFrameSampleCount > 0
                    ? mTotalFrameMilliseconds / static_cast<double>(mFrameSampleCount)
                    : 0.0;
            }

            [[nodiscard]] double GetLastMilliseconds() const noexcept
            {
                return mLastFrameMilliseconds;
            }

            [[nodiscard]] uint64_t GetFrameSampleCount() const noexcept
            {
                return mFrameSampleCount;
            }

            [[nodiscard]] uint64_t GetLastSubmissionId() const noexcept
            {
                return mLastFrameSubmissionId;
            }

            [[nodiscard]] const std::vector<SceneGpuPassTiming>& GetLastFramePasses() const noexcept
            {
                return mLastFramePasses;
            }

            [[nodiscard]] double GetLastPassMilliseconds(
                std::string_view passName) const noexcept
            {
                const auto found = mPassTimingIndices.find(std::string(passName));
                return found == mPassTimingIndices.end()
                    ? 0.0
                    : mPassTimings[found->second].LastMilliseconds;
            }

            [[nodiscard]] double SumLastPassMillisecondsWithPrefix(
                std::string_view prefix) const noexcept
            {
                double total = 0.0;
                for (const PassTiming& pass : mPassTimings)
                {
                    if (pass.Name.starts_with(prefix))
                        total += pass.LastMilliseconds;
                }
                return total;
            }

            [[nodiscard]] double SumPeakPassMillisecondsWithPrefix(
                std::string_view prefix) const noexcept
            {
                double total = 0.0;
                for (const PassTiming& pass : mPassTimings)
                {
                    if (pass.Name.starts_with(prefix) &&
                        pass.SampleCount > 0u)
                    {
                        total += pass.PeakMilliseconds;
                    }
                }
                return total;
            }

            [[nodiscard]] std::string BuildDebugDump() const
            {
                if (!bEnabled)
                    return "GPU RenderGraph pass timing: disabled";

                const auto percentile = [](const std::vector<double>& samples, double value)
                {
                    if (samples.empty())
                        return 0.0;
                    std::vector<double> sorted = samples;
                    std::sort(sorted.begin(), sorted.end());
                    const double position = value *
                        static_cast<double>(sorted.size() - 1u);
                    const size_t lower = static_cast<size_t>(std::floor(position));
                    const size_t upper = static_cast<size_t>(std::ceil(position));
                    const double fraction = position - static_cast<double>(lower);
                    return glm::mix(sorted[lower], sorted[upper], fraction);
                };
                std::string dump = fmt::format(
                    "GPU RenderGraph pass timing (summed): samples={} averageMs={:.3f} "
                    "lastMs={:.3f} medianMs={:.3f} p95Ms={:.3f} p99Ms={:.3f} dropped={}\n",
                    mFrameSampleCount,
                    GetAverageMilliseconds(),
                    mLastFrameMilliseconds,
                    percentile(mRecentFrameSamples, 0.50),
                    percentile(mRecentFrameSamples, 0.95),
                    percentile(mRecentFrameSamples, 0.99),
                    mDroppedFrameCount);
                for (const PassTiming& pass : mPassTimings)
                {
                    const double averageMilliseconds = pass.SampleCount > 0
                        ? pass.TotalMilliseconds / static_cast<double>(pass.SampleCount)
                        : 0.0;
                    dump += fmt::format(
                        "  {}: samples={} averageMs={:.3f} lastMs={:.3f} "
                        "medianMs={:.3f} p95Ms={:.3f} p99Ms={:.3f}\n",
                        pass.Name,
                        pass.SampleCount,
                        averageMilliseconds,
                        pass.LastMilliseconds,
                        percentile(pass.RecentSamples, 0.50),
                        percentile(pass.RecentSamples, 0.95),
                        percentile(pass.RecentSamples, 0.99));
                }
                return dump;
            }

        private:
            struct TimerRecord
            {
                nvrhi::TimerQueryHandle Handle;
                std::string PassName;
            };

            struct Slot
            {
                std::vector<TimerRecord> Queries;
                nvrhi::TimerQueryHandle FrameQuery;
                uint64_t Submission = 0;
                uint32_t UsedQueryCount = 0;
                bool bAllQueriesAvailable = true;
                bool bRecording = false;
                bool bRecorded = false;
                bool bPending = false;
            };

            void CollectSlot(
                uint32_t frameSlot,
                uint64_t completedSubmission,
                bool bForce)
            {
                Slot& slot = mSlots.at(frameSlot);
                if (!slot.bPending || (!bForce && completedSubmission < slot.Submission))
                    return;

                double frameMilliseconds = 0.0;
                const bool bCompleteSample = slot.bAllQueriesAvailable;
                if (bCompleteSample) mLastFramePasses.clear();
                if (slot.FrameQuery)
                {
                    if (bCompleteSample)
                        RecordPassTiming("FrameGpuSpan", static_cast<double>(
                            mDevice->getTimerQueryTime(slot.FrameQuery)) * 1000.0);
                    mDevice->resetTimerQuery(slot.FrameQuery);
                }
                for (uint32_t index = 0; index < slot.UsedQueryCount; ++index)
                {
                    TimerRecord& query = slot.Queries[index];
                    if (!query.Handle)
                        continue;
                    const double milliseconds = static_cast<double>(
                        mDevice->getTimerQueryTime(query.Handle)) * 1000.0;
                    frameMilliseconds += milliseconds;
                    if (bCompleteSample)
                    {
                        RecordPassTiming(query.PassName, milliseconds);
                        mLastFramePasses.push_back({ query.PassName, milliseconds });
                    }
                    mDevice->resetTimerQuery(query.Handle);
                }

                if (bCompleteSample)
                {
                    mLastFrameMilliseconds = frameMilliseconds;
                    mTotalFrameMilliseconds += frameMilliseconds;
                    // Publish identity with this complete sample before releasing the slot.
                    mLastFrameSubmissionId = slot.Submission;
                    ++mFrameSampleCount;
                    mRecentFrameSamples.push_back(frameMilliseconds);
                    if (mRecentFrameSamples.size() > 512u)
                        mRecentFrameSamples.erase(mRecentFrameSamples.begin());
                }
                else
                {
                    ++mDroppedFrameCount;
                }
                slot.Submission = 0;
                slot.UsedQueryCount = 0;
                slot.bAllQueriesAvailable = true;
                slot.bPending = false;
            }

            void RecordPassTiming(std::string_view passName, double milliseconds)
            {
                const std::string key(passName);
                const auto existing = mPassTimingIndices.find(key);
                size_t index = 0;
                if (existing == mPassTimingIndices.end())
                {
                    index = mPassTimings.size();
                    mPassTimings.push_back(PassTiming{ .Name = key });
                    mPassTimingIndices.emplace(key, index);
                }
                else
                {
                    index = existing->second;
                }
                PassTiming& timing = mPassTimings[index];
                timing.LastMilliseconds = milliseconds;
                timing.PeakMilliseconds = std::max(
                    timing.PeakMilliseconds,
                    milliseconds);
                timing.TotalMilliseconds += milliseconds;
                ++timing.SampleCount;
                timing.RecentSamples.push_back(milliseconds);
                if (timing.RecentSamples.size() > 512u)
                    timing.RecentSamples.erase(timing.RecentSamples.begin());
            }

            nvrhi::IDevice* mDevice = nullptr;
            std::vector<Slot> mSlots;
            std::vector<PassTiming> mPassTimings;
            std::vector<SceneGpuPassTiming> mLastFramePasses;
            std::unordered_map<std::string, size_t> mPassTimingIndices;
            uint32_t mCurrentFrameSlot = InvalidRenderGraphIndex;
            double mTotalFrameMilliseconds = 0.0;
            double mLastFrameMilliseconds = 0.0;
            uint64_t mFrameSampleCount = 0;
            uint64_t mLastFrameSubmissionId = 0;
            uint64_t mDroppedFrameCount = 0;
            std::vector<double> mRecentFrameSamples;
            bool bEnabled = false;
        };

        void UpdateImportedResources(const SceneRendererFrameDesc& frame)
        {
            Graph.SetTextureResource(
                BackBufferHandle,
                frame.OutputMode == SceneRendererOutputMode::OffscreenOnly
                    ? OffscreenOutput.Color
                    : frame.BackBuffer);
            if (OffscreenColorHandle &&
                frame.OutputMode == SceneRendererOutputMode::SwapChainAndOffscreen)
            {
                Graph.SetTextureResource(OffscreenColorHandle, OffscreenOutput.Color);
                Graph.SetTextureResource(OffscreenDepthHandle, OffscreenOutput.Depth);
            }
            else if (OffscreenDepthHandle &&
                frame.OutputMode == SceneRendererOutputMode::OffscreenOnly)
            {
                Graph.SetTextureResource(OffscreenDepthHandle, OffscreenOutput.Depth);
            }
			if (frame.bReuseOffscreenScene)
				return;
            Graph.SetBufferResource(FrameBufferHandle, FrameConstantBuffer);
            Graph.SetBufferResource(ObjectBufferHandle, ObjectConstantBuffer);
            for (const GraphMaterialTexture& texture : GraphMaterialTextures)
                Graph.SetTextureResource(texture.Handle, texture.Resource);
            Graph.SetTextureResource(IrradianceHandle, ActiveIrradiance);
            Graph.SetTextureResource(PrefilterHandle, ActivePrefilter);
            Graph.SetTextureResource(BrdfLutHandle, ActiveBrdfLut);
            Graph.SetBufferResource(GpuObjectsHandle, GpuObjectBuffer);
            Graph.SetBufferResource(GpuMaterialsHandle, GpuMaterialBuffer);
            Graph.SetBufferResource(MaterialParametersHandle, MaterialParameterBuffer);
            Graph.SetBufferResource(GpuBoundsHandle, GpuBoundsBuffer);
            Graph.SetBufferResource(GpuDrawDataHandle, GpuDrawDataBuffer);
            Graph.SetBufferResource(VisibleObjectIndicesHandle, VisibleObjectIndicesBuffer);
            Graph.SetBufferResource(IndirectArgumentsHandle, IndirectArgumentsBuffer);
            Graph.SetBufferResource(GpuReadbackHandle, GpuReadbackSlots.at(frame.FrameSlot).Buffer);
            Graph.SetBufferResource(
                Primitive2DVerticesHandle,
                Primitive2DBufferSlots.at(frame.FrameSlot).Vertices);
            Graph.SetBufferResource(BoneMatricesHandle, BoneMatrixBuffer);
            Graph.SetBufferResource(PreviousBoneMatricesHandle, PreviousBoneMatrixBuffer);
            for (const GraphGeometry& geometry : GraphGeometryResources)
            {
                const MeshEntry& mesh = MeshCache.at(geometry.Key);
                Graph.SetBufferResource(geometry.VertexBuffer, mesh.VertexBuffer);
                Graph.SetBufferResource(geometry.IndexBuffer, mesh.IndexBuffer);
            }
            if (HardwareRayTracingScene &&
                CurrentRtGraphResources.Vertices)
            {
                HardwareRayTracingScene->UpdateImportedResources(
                    Graph,
                    CurrentRtGraphResources);
            }
            if (IsGlobalIlluminationActive())
            {
                GetActiveIlluminationTechnique()->UpdateImportedResources(Graph);
            }
            SceneAntiAliasing.UpdateImportedResources(Graph);
            Features->UpdateImportedResources(Graph);
        }

        explicit Impl(const SceneRendererDesc& desc)
            : Device(desc.Device)
            , ShaderManagerInstance(desc.Device)
            , ShaderArtifacts(desc.ShaderCacheDirectory, desc.Backend)
            , OwnedMaterialShaderManager(
                desc.SharedMaterialShaderManager
                    ? nullptr
                    : std::make_unique<MaterialShaderManager>(
                        &ShaderManagerInstance,
                        desc.ShaderCacheDirectory.parent_path()))
            , MaterialShaderManagerInstance(
                desc.SharedMaterialShaderManager
                    ? *desc.SharedMaterialShaderManager
                    : *OwnedMaterialShaderManager)
            , MaterialShaderHandleManagerInstance(
                desc.SharedMaterialShaderHandleManager
                    ? *desc.SharedMaterialShaderHandleManager
                    : ShaderManagerInstance)
            , PipelineCacheInstance(desc.Device)
            , ResourcePool(desc.Device, desc.FramesInFlight, desc.bTransientTextureReuse)
            , TextureManagerInstance({
                .Device = desc.Device,
                .AssetRootDirectory = desc.AssetRootDirectory,
                .EngineAssetRootDirectory = desc.EngineAssetRootDirectory,
                .Bindless = {
                    .Textures = {
                        .Capacity = desc.Backend == RenderBackend::DX12 && desc.bHeapDirectlyIndexed
                            ? 4096u
                            : BaselineMaterialTextureDescriptorCount,
                        .Visibility = nvrhi::ShaderType::Pixel,
                        .Access = desc.Backend == RenderBackend::DX12 && desc.bHeapDirectlyIndexed
                            ? ShaderDescriptorAccess::HeapDirectlyIndexed
                            : ShaderDescriptorAccess::BoundedTable,
                    },
                    .Samplers = {
                        .Capacity = desc.Backend == RenderBackend::DX12 && desc.bHeapDirectlyIndexed
                            ? 256u
                            : BaselineMaterialSamplerDescriptorCount,
                        .Visibility = nvrhi::ShaderType::Pixel,
                        .Access = desc.Backend == RenderBackend::DX12 && desc.bHeapDirectlyIndexed
                            ? ShaderDescriptorAccess::HeapDirectlyIndexed
                            : ShaderDescriptorAccess::BoundedTable,
                    },
                },
            })
            , VfxRenderGraphPass({
                .Device = desc.Device,
                .ShaderCacheDirectory = desc.ShaderCacheDirectory,
            })
            , RuntimeUiRenderPass({
                .Device = desc.Device,
                .ShaderCacheDirectory = desc.ShaderCacheDirectory,
            })
            , SceneAntiAliasing({
                .Device = desc.Device,
                .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                .ShaderArtifactCacheRoot = desc.ShaderCacheDirectory.parent_path(),
                .Backend = desc.Backend,
            })
            , GpuTiming(desc.Device, desc.FramesInFlight, desc.bEnableGpuTiming)
            , FramesInFlight(desc.FramesInFlight)
            , ShaderCacheDirectory(desc.ShaderCacheDirectory)
            , AssetRootDirectory(desc.AssetRootDirectory)
            , EngineAssetRootDirectory(desc.EngineAssetRootDirectory)
            , Backend(desc.Backend)
            , Path(desc.Path)
            , Capabilities(desc.Capabilities)
            , AaCapabilities(BuildAntiAliasingCapabilities(
                desc.Backend,
                desc.Capabilities))
            , RequestedAaSettings(desc.AntiAliasing)
            , RequestedUpscaling(desc.Upscaling)
            , AaSelection(ResolveAntiAliasingSelection(
                desc.AntiAliasing,
                AaCapabilities))
            , bHeapDirectlyIndexed(
                desc.Backend == RenderBackend::DX12 && desc.bHeapDirectlyIndexed)
            , bRunEmptyEffekseerPassForValidation(
                desc.bRunEmptyEffekseerPassForValidation)
        {
            if (static_cast<bool>(desc.SharedMaterialShaderManager) !=
                static_cast<bool>(desc.SharedMaterialShaderHandleManager))
            {
                throw std::invalid_argument(
                    "SceneRenderer shared material shader managers must be supplied together");
            }
            if (!Device)
                throw std::invalid_argument("SceneRenderer requires a valid NVRHI device");
            if (FramesInFlight == 0)
                throw std::invalid_argument("SceneRenderer requires at least one frame in flight");
            if (ShaderCacheDirectory.empty())
                throw std::invalid_argument("SceneRenderer requires the shader cache directory");
			ObservedMaterialShaderPublicationRevision =
				MaterialShaderManagerInstance.GetPublicationRevision();

            HE_CORE_INFO(
                "SceneRenderer descriptor/shader path: {0}",
                bHeapDirectlyIndexed
                    ? "heap-direct tables with explicit SM6.6 pixel variants"
                    : "bounded descriptor tables with baseline SM6.0 shaders (120 material SRV, 15 material samplers)");

            RecordedGpuObjectCounts.resize(FramesInFlight);
            RecordedGpuBatchCounts.resize(FramesInFlight);
			RecordedGpuReadbacks.resize(FramesInFlight);

            CreateShaders();
            LightGrid = CreateScope<SceneLightGrid>(SceneLightGridDesc{
                .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
            (void)LightGrid->PrepareFrame(RenderWorld{}, 1, 1, 0);
            LocalShadowPass = CreateScope<SceneLocalShadowPass>(SceneLocalShadowPassDesc{ .Device = Device });
            CreateImmutableState();
            CreateDefaultResources();
            ShadowGpuSubmission = CreateScope<SceneShadowGpuSubmission>(SceneShadowGpuSubmissionDesc{
                .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
            ShadowGpuVertexShader = LoadRasterShader("Modern/SceneShadow.hlsl", "SceneShadow_GPU_DRIVEN.vs",
                nvrhi::ShaderType::Vertex, "VS", "vs_6_0");
            nvrhi::BindingLayoutDesc gpuShadowLayout;
            gpuShadowLayout.setVisibility(nvrhi::ShaderType::Vertex).setRegisterSpaceAndDescriptorSet(2)
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
            ShadowGpuObjectLayout = Device->createBindingLayout(gpuShadowLayout);
            if (!ShadowGpuObjectLayout) throw std::runtime_error("GPU shadow layout creation failed");
            ActiveIrradiance = DefaultEnvironment;
            ActivePrefilter = DefaultEnvironment;
            ActiveEnvironment = DefaultEnvironment;
            ActiveBrdfLut = DefaultBrdfLut;
            Stats.SceneColorFormat = c_SceneColorFormat;
            Stats.SceneDepthFormat = c_SceneDepthFormat;
            Stats.bGpuTimingEnabled = desc.bEnableGpuTiming;
            bCpuGraphTimingEnabled = desc.bEnableCpuGraphTiming;
            RegisterGlobalIlluminationTechniques();
            UpscalingPass = CreateScope<SceneUpscalingPass>(SceneAntiAliasingPassDesc{
                .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
            Features = CreateScope<RenderFeaturePlan>(desc.PipelineFeatures, RenderFeatureCreateContext{
                .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(),
                .AssetRootDirectory = AssetRootDirectory, .EngineAssetRootDirectory = EngineAssetRootDirectory,
                .Backend = Backend, .FramesInFlight = FramesInFlight });
            ResourcePool.SetTransientCacheBudget(desc.TransientCacheBudgetBytes);
            TextureStreamingBudgetBytes = desc.TextureStreamingBudgetBytes;
            ConfigureGlobalIllumination(desc.GlobalIllumination);
            ReflectionSettings = desc.Reflections;
            bGpuOcclusionEnabled = desc.bGpuOcclusionEnabled;
            bGpuBatchingEnabled = desc.bGpuBatchingEnabled && SupportsCountedIndirectDraws(Capabilities);
            bGpuTightFrustumEnabled = desc.bGpuTightFrustumEnabled;
            bGpuShadowBatchingEnabled = desc.bGpuShadowBatchingEnabled && SupportsCountedIndirectDraws(Capabilities);
            GeometryArena = CreateScope<SceneGeometryArena>(SceneGeometryArenaDesc{ .Device = Device });
            SceneIndirectCompactionPassDesc compactionDesc{ .Device = Device,
                .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend,
                .DebugName = "DepthIndirect" };
            DepthIndirectCompaction = CreateScope<SceneIndirectCompactionPass>(compactionDesc);
            compactionDesc.DebugName = "MainIndirect";
            MainIndirectCompaction = CreateScope<SceneIndirectCompactionPass>(compactionDesc);
            bLodEnabled = desc.bLodEnabled;
            bShadowsEnabled = desc.bShadowsEnabled;
            bContinuousPcfEnabled = desc.bContinuousPcfEnabled;
            LodMaxPixelError = desc.LodMaxPixelError;
            GpuOcclusionPass = CreateScope<SceneGpuOcclusionPass>(SceneGpuOcclusionPassDesc{
                .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
            ReflectionPass = CreateScope<SceneReflectionPass>(SceneReflectionPassDesc{
                .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
            CloudQualitySettings = desc.Clouds;
            AmbientOcclusionSettings = desc.AmbientOcclusion;
            AmbientOcclusionPass = CreateScope<SceneAmbientOcclusionPass>(SceneAmbientOcclusionPassDesc{
                .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
            PostProcessSettings = desc.PostProcess;
            PostProcessPass = CreateScope<ScenePostProcessPass>(ScenePostProcessPassDesc{
                .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
            RefreshAntiAliasingSelection();
        }

        void RegisterGlobalIlluminationTechniques()
        {
            const auto commonOutputs =
                GlobalIlluminationTechniqueOutput::DiffuseIndirectRadiance |
                GlobalIlluminationTechniqueOutput::HitDistance |
                GlobalIlluminationTechniqueOutput::Confidence |
                GlobalIlluminationTechniqueOutput::CoverageMask |
                GlobalIlluminationTechniqueOutput::HistoryLength |
                GlobalIlluminationTechniqueOutput::Variance;
            GiTechniqueRegistry.Register(GlobalIlluminationTechniqueDescriptor{
                .Id = GlobalIlluminationTechniqueKind::Off,
                .DisplayName = "Off",
                .Factory = []() -> Scope<IGlobalIlluminationTechnique>
                {
                    return CreateScope<OffGlobalIlluminationTechnique>();
                },
                .OutputSemantics = {},
                .SupportedDebugViews = {
                    GlobalIlluminationDebugView::Composite,
                },
            });
            GiTechniqueRegistry.Register(GlobalIlluminationTechniqueDescriptor{
                .Id = GlobalIlluminationTechniqueKind::ScreenSpace,
                .DisplayName = "Screen Space GI",
                .Factory = [this]() -> Scope<IGlobalIlluminationTechnique>
                {
                    return CreateScope<ScreenSpaceGlobalIllumination>(
                        ScreenSpaceGlobalIlluminationDesc{
                            .Device = Device,
                            .SharedShaderManager =
                                &MaterialShaderHandleManagerInstance,
                            .Denoiser = SvgfDenoiserInstance.get(),
                            .ShaderArtifactCacheRoot =
                                ShaderCacheDirectory.parent_path(),
                            .Backend = Backend,
                            .FramesInFlight = FramesInFlight,
                        });
                },
                .CapabilityRequirements =
                    GlobalIlluminationTechniqueCapabilityRequirements::ComputeQueue,
                .RequiredInputs =
                    GlobalIlluminationTechniqueRequiredInput::SceneDepth |
                    GlobalIlluminationTechniqueRequiredInput::NormalRoughness |
                    GlobalIlluminationTechniqueRequiredInput::BaseColorMetalness |
                    GlobalIlluminationTechniqueRequiredInput::Emissive |
                    GlobalIlluminationTechniqueRequiredInput::MotionVectors |
                    GlobalIlluminationTechniqueRequiredInput::MaterialId |
                    GlobalIlluminationTechniqueRequiredInput::PreviousDepth |
                    GlobalIlluminationTechniqueRequiredInput::OpaqueDirectHdr |
                    GlobalIlluminationTechniqueRequiredInput::DepthPyramid |
                    GlobalIlluminationTechniqueRequiredInput::FrameBuffer,
                .OutputSemantics = { .Outputs = commonOutputs |
                    GlobalIlluminationTechniqueOutput::DirectDiffuseRadiance },
                .SupportedDebugViews = {
                    GlobalIlluminationDebugView::Composite,
                    GlobalIlluminationDebugView::RawDiffuse,
                    GlobalIlluminationDebugView::RawDirectDiffuse,
                    GlobalIlluminationDebugView::RawIndirectDiffuse,
                    GlobalIlluminationDebugView::Albedo,
                    GlobalIlluminationDebugView::DemodulatedDiffuseIllumination,
                    GlobalIlluminationDebugView::FilteredDiffuseIllumination,
                    GlobalIlluminationDebugView::RemodulatedDiffuse,
                    GlobalIlluminationDebugView::Normal,
                    GlobalIlluminationDebugView::HitDistance,
                    GlobalIlluminationDebugView::HistoryLength,
                    GlobalIlluminationDebugView::Confidence,
                    GlobalIlluminationDebugView::Coverage,
                    GlobalIlluminationDebugView::Variance,
                    GlobalIlluminationDebugView::SourceLighting,
                    GlobalIlluminationDebugView::TemporalDiffuseIllumination,
                    GlobalIlluminationDebugView::ViewDepth,
                    GlobalIlluminationDebugView::ScreenHitCoverage,
                    GlobalIlluminationDebugView::TraceTermination,
                    GlobalIlluminationDebugView::TraceHitLocation,
                },
            });
            GiTechniqueRegistry.Register(GlobalIlluminationTechniqueDescriptor{
                .Id = GlobalIlluminationTechniqueKind::HardwareRayTracing,
                .DisplayName = "Hardware RTGI",
                .Factory = [this]() -> Scope<IGlobalIlluminationTechnique>
                {
                    return CreateScope<RealtimeRayTracedGlobalIllumination>(
                        RealtimeRtgiDesc{
                            .Device = Device,
                            .SharedShaderManager =
                                &MaterialShaderHandleManagerInstance,
                            .Scene = HardwareRayTracingScene.get(),
                            .Denoiser = SvgfDenoiserInstance.get(),
                            .ShaderArtifactCacheRoot =
                                ShaderCacheDirectory.parent_path(),
                            .Backend = Backend,
                        });
                },
                .CapabilityRequirements =
                    GlobalIlluminationTechniqueCapabilityRequirements::GraphicsQueue |
                    GlobalIlluminationTechniqueCapabilityRequirements::
                        RayTracingAccelerationStructure |
                    GlobalIlluminationTechniqueCapabilityRequirements::
                        RayTracingPipeline,
                .RequiredInputs =
                    GlobalIlluminationTechniqueRequiredInput::SceneDepth |
                    GlobalIlluminationTechniqueRequiredInput::NormalRoughness |
                    GlobalIlluminationTechniqueRequiredInput::BaseColorMetalness |
                    GlobalIlluminationTechniqueRequiredInput::Emissive |
                    GlobalIlluminationTechniqueRequiredInput::MotionVectors |
                    GlobalIlluminationTechniqueRequiredInput::MaterialId |
                    GlobalIlluminationTechniqueRequiredInput::PreviousDepth |
                    GlobalIlluminationTechniqueRequiredInput::SceneTlas |
                    GlobalIlluminationTechniqueRequiredInput::RayTracingVertexBuffer |
                    GlobalIlluminationTechniqueRequiredInput::RayTracingIndexBuffer |
                    GlobalIlluminationTechniqueRequiredInput::
                        RayTracingInstanceBuffer |
                    GlobalIlluminationTechniqueRequiredInput::MaterialBuffer |
                    GlobalIlluminationTechniqueRequiredInput::GeometryBuffer |
                    GlobalIlluminationTechniqueRequiredInput::LightBuffer,
                .OutputSemantics = { .Outputs = commonOutputs |
                    GlobalIlluminationTechniqueOutput::DirectDiffuseRadiance },
                .SupportedDebugViews = {
                    GlobalIlluminationDebugView::Composite,
                    GlobalIlluminationDebugView::RawDiffuse,
                    GlobalIlluminationDebugView::RawDirectDiffuse,
                    GlobalIlluminationDebugView::RawIndirectDiffuse,
                    GlobalIlluminationDebugView::Albedo,
                    GlobalIlluminationDebugView::DemodulatedDiffuseIllumination,
                    GlobalIlluminationDebugView::FilteredDiffuseIllumination,
                    GlobalIlluminationDebugView::RemodulatedDiffuse,
                    GlobalIlluminationDebugView::Normal,
                    GlobalIlluminationDebugView::HitDistance,
                    GlobalIlluminationDebugView::HistoryLength,
                    GlobalIlluminationDebugView::Confidence,
                    GlobalIlluminationDebugView::Coverage,
                    GlobalIlluminationDebugView::Variance,
                    GlobalIlluminationDebugView::SourceLighting,
                    GlobalIlluminationDebugView::TemporalDiffuseIllumination,
                    GlobalIlluminationDebugView::ViewDepth,
                    GlobalIlluminationDebugView::ScreenHitCoverage,
                },
            });
        }

        [[nodiscard]] bool IsGlobalIlluminationActive() const noexcept
        {
            return static_cast<bool>(ActiveOfflineViewportRenderer) ||
                EffectiveGiSettings.Technique !=
                    GlobalIlluminationTechniqueKind::Off;
        }

        [[nodiscard]] bool IsOfflineViewportRendererActive() const noexcept
        {
            return static_cast<bool>(ActiveOfflineViewportRenderer);
        }

        [[nodiscard]] IGlobalIlluminationTechnique*
            GetActiveIlluminationTechnique() const noexcept
        {
            return ActiveOfflineViewportRenderer
                ? ActiveOfflineViewportRenderer.Get()
                : ActiveGiTechnique.Get();
        }

        [[nodiscard]] bool IsScreenSpaceGlobalIlluminationActive() const noexcept
        {
            return EffectiveGiSettings.Technique ==
                GlobalIlluminationTechniqueKind::ScreenSpace;
        }

        [[nodiscard]] bool UsesHardwareRayTracing() const noexcept
        {
            return HasGlobalIlluminationTechniqueCapabilityRequirement(
                    ActiveGiCapabilityRequirements,
                    GlobalIlluminationTechniqueCapabilityRequirements::
                        RayTracingAccelerationStructure) ||
                HasGlobalIlluminationTechniqueRequiredInput(
                    ActiveGiRequiredInputs,
                    GlobalIlluminationTechniqueRequiredInput::SceneTlas);
        }

        [[nodiscard]] bool RequiresGiInput(
            GlobalIlluminationTechniqueRequiredInput input) const noexcept
        {
            return HasGlobalIlluminationTechniqueRequiredInput(
                ActiveGiRequiredInputs,
                input);
        }

        [[nodiscard]] ScenePostProcessSettings ResolveViewPostProcess(const RenderWorld& world) const
        {
            auto settings = PostProcessSettings;
            settings.BloomIntensity = settings.BloomIntensity * world.PostProcess.BloomBaseWeight + world.PostProcess.BloomContribution;
            settings.Contrast = settings.Contrast * world.PostProcess.ContrastBaseWeight + world.PostProcess.ContrastContribution;
            settings.Saturation = settings.Saturation * world.PostProcess.SaturationBaseWeight + world.PostProcess.SaturationContribution;
            settings.Temperature = settings.Temperature * world.PostProcess.TemperatureBaseWeight + world.PostProcess.TemperatureContribution;
            settings.VignetteIntensity = settings.VignetteIntensity * world.PostProcess.VignetteBaseWeight + world.PostProcess.VignetteContribution;
            settings.ColorGain = settings.ColorGain * world.PostProcess.ColorGainBaseWeight + world.PostProcess.ColorGainContribution;
            settings.ColorOffset = settings.ColorOffset * world.PostProcess.ColorOffsetBaseWeight + world.PostProcess.ColorOffsetContribution;
            return settings;
        }

        [[nodiscard]] bool RequiresSceneSurfaceData() const noexcept
        {
            return (Features && (Features->RequiredInputs() &
                (uint32_t(RenderFeatureInput::Normal) | uint32_t(RenderFeatureInput::MotionVector)))) ||
                IsTemporalRasterActive() || RequiresGiInput(
                    GlobalIlluminationTechniqueRequiredInput::NormalRoughness) ||
                RequiresGiInput(
                    GlobalIlluminationTechniqueRequiredInput::BaseColorMetalness) ||
                RequiresGiInput(
                    GlobalIlluminationTechniqueRequiredInput::Emissive) ||
                RequiresGiInput(
                    GlobalIlluminationTechniqueRequiredInput::MotionVectors) ||
                RequiresGiInput(
                    GlobalIlluminationTechniqueRequiredInput::MaterialId) ||
                RequiresGiInput(
                    GlobalIlluminationTechniqueRequiredInput::PreviousDepth);
        }

        [[nodiscard]] bool RequiresSceneTableProvider() const noexcept
        {
            return RequiresGiInput(GlobalIlluminationTechniqueRequiredInput::SceneTlas) ||
                RequiresGiInput(GlobalIlluminationTechniqueRequiredInput::MaterialBuffer) ||
                RequiresGiInput(GlobalIlluminationTechniqueRequiredInput::GeometryBuffer) ||
                RequiresGiInput(GlobalIlluminationTechniqueRequiredInput::LightBuffer) ||
                RequiresGiInput(
                    GlobalIlluminationTechniqueRequiredInput::RayTracingVertexBuffer) ||
                RequiresGiInput(
                    GlobalIlluminationTechniqueRequiredInput::RayTracingIndexBuffer) ||
                RequiresGiInput(
                    GlobalIlluminationTechniqueRequiredInput::RayTracingInstanceBuffer);
        }

        [[nodiscard]] bool IsOfflineViewportRenderingActive() const noexcept
        {
            return IsOfflineViewportRendererActive();
        }

        void RefreshAntiAliasingSelection()
        {
            const AntiAliasingMode previousMode = AaSelection.Effective.Mode;
            const uint32_t previousSampleCount = GetAntiAliasingSampleCount(
                AaSelection.Effective);
            AaSelection = ResolveAntiAliasingSelection(
                RequestedAaSettings,
                AaCapabilities,
                IsOfflineViewportRenderingActive()
                    ? AntiAliasingRenderPath::OfflinePathTracing
                    : AntiAliasingRenderPath::RealtimeRaster);
            if ((Path == RenderingPath::Deferred || bGpuOcclusionEnabled || ReflectionSettings.bScreenSpaceEnabled ||
                    ReflectionSettings.bLocalProbesEnabled) &&
                AaSelection.Effective.Mode == AntiAliasingMode::Msaa)
            {
                AaSelection.Effective.Mode = AntiAliasingMode::Taa;
                AaSelection.bFallback = true;
                AaSelection.bRequestedModeAvailable = false;
                AaSelection.Reason = "Deferred, reflection composition and same-frame GPU occlusion use native-resolution TAA; Forward MSAA requires reflection passes and GPU occlusion disabled.";
            }
            if (!AaSelection.bBypassed &&
                EffectiveGiSettings.DebugView !=
                    GlobalIlluminationDebugView::Composite)
            {
                AaSelection.Effective.Mode = AntiAliasingMode::None;
                AaSelection.bFallback = true;
                AaSelection.Reason =
                    "Anti-aliasing is disabled for GI diagnostic views to preserve data and sample positions.";
            }
            if (IsFsrActive())
            {
                AaSelection.Effective.Mode = AntiAliasingMode::None;
                AaSelection.Reason = "FSR 3.1 provides temporal reconstruction and anti-aliasing.";
            }
            Stats.RequestedAntiAliasing = RequestedAaSettings.Mode;
            Stats.EffectiveAntiAliasing = AaSelection.Effective.Mode;
            Stats.RasterSampleCount = GetAntiAliasingSampleCount(
                AaSelection.Effective);
            Stats.bAntiAliasingFallback = AaSelection.bFallback ||
                AaSelection.bBypassed;
            if (previousMode != AaSelection.Effective.Mode ||
                previousSampleCount != Stats.RasterSampleCount)
            {
                SceneAntiAliasing.ResetHistory();
                bHasPreviousRasterCamera = false;
                bGraphDirty = true;
            }
        }

        [[nodiscard]] uint32_t GetRasterSampleCount() const
        {
            return GetAntiAliasingSampleCount(AaSelection.Effective);
        }

        [[nodiscard]] bool IsTaaActive() const noexcept
        {
            return AaSelection.Effective.Mode == AntiAliasingMode::Taa;
        }

        [[nodiscard]] bool IsFsrActive() const noexcept { return UpscalingPass && UpscalingPass->IsActive(); }
        [[nodiscard]] bool IsTemporalRasterActive() const noexcept { return IsTaaActive() || IsFsrActive(); }

        [[nodiscard]] bool IsFxaaActive() const noexcept
        {
            return AaSelection.Effective.Mode == AntiAliasingMode::Fxaa;
        }

        [[nodiscard]] bool IsSmaaActive() const noexcept
        {
            return AaSelection.Effective.Mode == AntiAliasingMode::Smaa;
        }

        [[nodiscard]] bool IsSpatialPostAntiAliasingActive() const noexcept
        {
            return IsFxaaActive() || IsSmaaActive();
        }

        [[nodiscard]] bool IsMsaaActive() const
        {
            return GetRasterSampleCount() > 1;
        }

        void PrepareAntiAliasingFrame(
            const RenderWorld& world,
            const SceneRendererFrameDesc& frame)
        {
            RefreshAntiAliasingSelection();
            CurrentRasterCamera = world.Camera;
            const bool bSequenceReset =
                PreviousRasterJitterGeneration !=
                    world.Camera.JitterSequenceGeneration;
            const bool bViewChanged = bHasPreviousRasterCamera &&
                std::memcmp(
                    &world.Camera.NonJitteredViewProjection,
                    &PreviousTaaNonJitteredViewProjection,
                    sizeof(glm::mat4)) != 0;
            const bool bReset = world.Camera.bCameraCut || bSequenceReset ||
                !bHasPreviousRasterCamera || !MotionHistory.HasSubmittedScene(world.SceneGeneration);
            bRasterCameraHistoryReset = bReset;
            if (bReset)
            {
                // Reset before choosing this frame's jitter, not afterwards in
                // PrepareFrame, so cuts/reloads start at a consistent sample.
                SceneAntiAliasing.ResetHistory();
                GlobalIlluminationRenderSequence = 0;
            }
            PendingRasterView = world.Camera.View;
            PendingRasterJitterPixels = glm::vec2(0.0f);
            PendingRasterWorldFrame = world.FrameNumber;
            if (bViewChanged && !bReset)
                SceneAntiAliasing.RestartConvergence();
            if (IsTemporalRasterActive())
            {
                const glm::vec2 jitterPixels = IsFsrActive()
                    ? UpscalingPass->PrepareFrame(world.Camera, world.TimeSeconds, bReset)
                    : GetTaaJitterPixels(
                    SceneAntiAliasing.GetTemporalFrameIndex(),
                    world.Camera.JitterSequenceGeneration);
                PendingRasterJitterPixels = jitterPixels;
                CurrentRasterCamera.Projection = ApplyProjectionJitter(
                    world.Camera.Projection,
                    jitterPixels,
                    frame.Width,
                    frame.Height);
                CurrentRasterCamera.ViewProjection =
                    CurrentRasterCamera.Projection * world.Camera.View;
                CurrentRasterCamera.PreviousViewProjection = bReset
                    ? CurrentRasterCamera.ViewProjection
                    : PreviousRasterViewProjection;
                PendingRasterViewProjection =
                    CurrentRasterCamera.ViewProjection;
                PendingRasterJitterGeneration =
                    world.Camera.JitterSequenceGeneration;
            }
            else
            {
                PendingRasterViewProjection = world.Camera.ViewProjection;
                CurrentRasterCamera.PreviousViewProjection = bReset
                    ? CurrentRasterCamera.ViewProjection
                    : PreviousRasterViewProjection;
                PendingRasterJitterGeneration =
                    world.Camera.JitterSequenceGeneration;
            }
            // Motion guides are also consumed by GI without TAA. Advance this
            // camera state only when its raster work is successfully submitted;
            // AA selection controls projection jitter, not camera ownership.
            PendingTaaNonJitteredViewProjection =
                world.Camera.NonJitteredViewProjection;
            bRasterCameraFrameRecorded = true;
            SceneAntiAliasing.PrepareFrame(
                AaSelection.Effective,
                IsSpatialPostAntiAliasingActive() ? LastOutputWidth : frame.Width,
                IsSpatialPostAntiAliasingActive() ? LastOutputHeight : frame.Height,
                frame.CompletedSubmission,
                bReset,
                PendingRasterJitterPixels);
            if (SceneAntiAliasing.ConsumeGraphRebuildRequest())
                bGraphDirty = true;
        }

        void RetireIllumination()
        {
            if (!ActiveOfflineViewportRenderer && !ActiveGiTechnique && !HardwareRayTracingScene)
                return;
            if (ActiveOfflineViewportRenderer) ActiveOfflineViewportRenderer->Cancel();
            RetiredIlluminationSets.push_back({LastSubmittedSubmission,
                std::move(HardwareRayTracingScene), std::move(SurfaceDataPassInstance),
                std::move(SvgfDenoiserInstance), std::move(DepthPyramidPassInstance),
                std::move(ActiveGiTechnique), std::move(ActiveOfflineViewportRenderer)});
        }

        void CollectIllumination(uint64_t completed)
        {
            std::erase_if(RetiredIlluminationSets, [completed](RetiredIllumination& retired)
            {
                if (retired.Offline) retired.Offline->Collect(completed);
                if (retired.Submission > completed || (retired.Offline && retired.Offline->HasPendingWork()))
                    return false;
                if (retired.Offline) retired.Offline->Clear(completed);
                if (retired.Realtime) retired.Realtime->Clear(completed);
                return true;
            });
        }

        void ConfigureOfflineViewportRenderer(std::string providerId)
        {
            if (providerId.empty())
                throw std::invalid_argument(
                    "Offline viewport renderer provider ID is empty");
            if (ActiveOfflineViewportRendererId == providerId &&
                ActiveOfflineViewportRenderer)
            {
                return;
            }

            OfflineViewportRendererProviderRegistry& registry =
                OfflineViewportRendererProviderRegistry::Get();
            const std::vector<OfflineViewportRendererProviderDescriptor>
                descriptors = registry.Enumerate();
            const auto descriptor = std::ranges::find_if(
                descriptors,
                [&providerId](
                    const OfflineViewportRendererProviderDescriptor& candidate)
                {
                    return candidate.Id == providerId;
                });
            if (descriptor == descriptors.end())
                throw std::out_of_range(
                    "Offline viewport renderer provider is not registered: " +
                    providerId);
            const OfflineViewportRendererSupport support =
                registry.QuerySupport(providerId, Capabilities);
            if (!support.bSupported)
                throw std::runtime_error(support.Reason);

            // Progressive providers need delayed whole-frame feedback even
            // when the optional CSV profiler was not enabled at startup.
            GpuTiming.Enable();
            Stats.bGpuTimingEnabled = true;

            RetireIllumination();
            try
            {
                ActiveGiRequiredInputs = descriptor->RequiredInputs;
                ActiveGiCapabilityRequirements =
                    descriptor->CapabilityRequirements;
                if (RequiresSceneTableProvider())
                {
                    HardwareRayTracingScene = CreateScope<RayTracingScene>(
                        RayTracingSceneDesc{
                            .Device = Device,
                            .FramesInFlight = FramesInFlight,
                            .bCreateAccelerationStructures =
                                UsesHardwareRayTracing(),
                            .bBasePbrMaterials = descriptor->bBasePbrMaterials,
                            .Textures = &TextureManagerInstance,
                        });
                }
                if (RequiresSceneSurfaceData())
                {
                    SurfaceDataPassInstance = CreateScope<SceneSurfaceDataPass>(
                        SceneSurfaceDataPassDesc{
                            .Device = Device,
                            .SharedShaderManager =
                                &MaterialShaderHandleManagerInstance,
                            .Scene = HardwareRayTracingScene.get(),
                            .ShaderArtifactCacheRoot =
                                ShaderCacheDirectory.parent_path(),
                            .Backend = Backend,
                        });
                }
                if (RequiresGiInput(
                        GlobalIlluminationTechniqueRequiredInput::DepthPyramid))
                {
                    DepthPyramidPassInstance = CreateScope<DepthPyramidPass>(
                        DepthPyramidPassDesc{
                            .Device = Device,
                            .SharedShaderManager =
                                &MaterialShaderHandleManagerInstance,
                            .ShaderArtifactCacheRoot =
                                ShaderCacheDirectory.parent_path(),
                            .Backend = Backend,
                        });
                }
                if (!CompositePassInstance)
                {
                    CompositePassInstance =
                        CreateScope<GlobalIlluminationCompositePass>(
                            GlobalIlluminationCompositePassDesc{
                                .Device = Device,
                                .SharedShaderManager =
                                    &MaterialShaderHandleManagerInstance,
                                .ShaderArtifactCacheRoot =
                                    ShaderCacheDirectory.parent_path(),
                                .Backend = Backend,
                            });
                }

                ActiveOfflineViewportRenderer = registry.Create(
                    providerId,
                    OfflineViewportRendererCreateContext{
                        .Device = Device,
                        .SharedShaderManager =
                            &MaterialShaderHandleManagerInstance,
                        .Scene = HardwareRayTracingScene.get(),
                        .ShaderArtifactCacheRoot =
                            descriptor->ShaderArtifactCacheRoot,
                        .Backend = Backend,
                        .Capabilities = Capabilities,
                        .FramesInFlight = FramesInFlight,
                    });
                if (!ActiveOfflineViewportRenderer->IsSupported(Capabilities))
                {
                    throw std::runtime_error(
                        "Offline viewport renderer session rejected active capabilities");
                }
                ActiveOfflineViewportRendererDescriptor = *descriptor;
                ActiveOfflineViewportRendererId = std::move(providerId);
                GlobalIlluminationRenderSequence = 0;
                bGlobalIlluminationFrameRecorded = false;
                Stats.GiStats = ActiveOfflineViewportRenderer->GetStats();
                RefreshAntiAliasingSelection();
                bGraphDirty = true;
            }
            catch (...)
            {
                ActiveOfflineViewportRenderer.Release();
                ActiveOfflineViewportRendererDescriptor = {};
                ActiveOfflineViewportRendererId.clear();
                HardwareRayTracingScene.reset();
                SurfaceDataPassInstance.reset();
                DepthPyramidPassInstance.reset();
                ConfigureGlobalIllumination(RequestedGiSettings);
                throw;
            }
        }

        void ClearOfflineViewportRenderer()
        {
            if (!ActiveOfflineViewportRenderer)
                return;
            RetireIllumination();
            ActiveOfflineViewportRendererDescriptor = {};
            ActiveOfflineViewportRendererId.clear();
            ConfigureGlobalIllumination(RequestedGiSettings);
        }

        void ConfigureGlobalIllumination(
            const GlobalIlluminationSettings& requestedSettings)
        {
            GlobalIlluminationRenderSequence = 0;
            bGlobalIlluminationFrameRecorded = false;
            GlobalIlluminationSettings normalized = requestedSettings;
            if (normalized.Technique ==
                    GlobalIlluminationTechniqueKind::ScreenSpace ||
                normalized.Technique ==
                    GlobalIlluminationTechniqueKind::HardwareRayTracing)
            {
                normalized.SamplesPerPixel = 1;
            }
            if (normalized.Denoiser == GlobalIlluminationDenoiserKind::Nrd)
            {
                throw std::invalid_argument(
                    "NRD is not available; no approved dependency/adapter is present");
            }

            // Preserve the exact request. Technique-specific normalization and
            // capability fallback belong exclusively to EffectiveGiSettings.
            RequestedGiSettings = requestedSettings;
            GiSelection = ResolveGlobalIlluminationSelection(
                normalized,
                Capabilities,
                true);
            EffectiveGiSettings = normalized;
            EffectiveGiSettings.Technique = GiSelection.Selected;
            if (EffectiveGiSettings.Denoiser != GlobalIlluminationDenoiserKind::Svgf &&
                EffectiveGiSettings.DebugView == GlobalIlluminationDebugView::TemporalDiffuseIllumination)
                EffectiveGiSettings.DebugView = GlobalIlluminationDebugView::RawIndirectDiffuse;
            const std::vector<GlobalIlluminationTechniqueDescriptor>
                registeredTechniques = GiTechniqueRegistry.Enumerate();
            const auto activeDescriptor = std::ranges::find_if(
                registeredTechniques,
                [this](const GlobalIlluminationTechniqueDescriptor& descriptor)
                {
                    return descriptor.Id == EffectiveGiSettings.Technique;
                });
            if (activeDescriptor == registeredTechniques.end())
                throw std::logic_error("Selected GI technique is not registered");
            ActiveGiRequiredInputs = activeDescriptor->RequiredInputs;
            ActiveGiCapabilityRequirements =
                activeDescriptor->CapabilityRequirements;
            if (normalized.Technique != GlobalIlluminationTechniqueKind::Off)
            {
                if (GiSelection.Selected == GlobalIlluminationTechniqueKind::Off)
                {
                    HE_CORE_WARN(
                        "Global illumination request fell back to Off: {}",
                        GiSelection.Reason);
                }
                else
                {
                    HE_CORE_INFO(
                        "Global illumination selected technique={} hardwareRT={}: {}",
                        static_cast<uint32_t>(GiSelection.Selected),
                        GiSelection.bUsesHardwareRayTracing,
                        GiSelection.Reason);
                }
            }
            RetireIllumination();
            RefreshAntiAliasingSelection();

            if (RequiresSceneTableProvider())
            {
                HardwareRayTracingScene = CreateScope<RayTracingScene>(
                    RayTracingSceneDesc{
                        .Device = Device,
                        .FramesInFlight = FramesInFlight,
                        .bCreateAccelerationStructures = UsesHardwareRayTracing(),
                    });
            }
            else
            {
                HardwareRayTracingScene.reset();
            }

            if (!IsGlobalIlluminationActive())
            {
                CompositePassInstance.reset();
                Stats.GiStats = {
                    .Technique = EffectiveGiSettings.Technique,
                    .Status = GiSelection.Reason,
                };
                ResetHardwareRayTracingStats();
                bGraphDirty = true;
                return;
            }

            if (!CompositePassInstance)
            {
                CompositePassInstance = CreateScope<GlobalIlluminationCompositePass>(
                    GlobalIlluminationCompositePassDesc{
                        .Device = Device,
                        .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                        .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(),
                        .Backend = Backend,
                    });
            }

            if (RequiresSceneSurfaceData())
            {
                if (EffectiveGiSettings.Denoiser ==
                    GlobalIlluminationDenoiserKind::Svgf)
                {
                    SvgfDenoiserInstance = CreateScope<SvgfDenoiser>(
                        SvgfDenoiserDesc{
                            .Device = Device,
                            .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                            .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(),
                            .Backend = Backend,
                        });
                }
                else
                {
                    SvgfDenoiserInstance.reset();
                }
                if (RequiresGiInput(
                        GlobalIlluminationTechniqueRequiredInput::DepthPyramid))
                {
                    DepthPyramidPassInstance = CreateScope<DepthPyramidPass>(
                        DepthPyramidPassDesc{
                            .Device = Device,
                            .SharedShaderManager =
                                &MaterialShaderHandleManagerInstance,
                            .ShaderArtifactCacheRoot =
                                ShaderCacheDirectory.parent_path(),
                            .Backend = Backend,
                    });
                }
            }
            else
            {
                // Techniques without raster surface guides own no shared SVGF.
            }
            ActiveGiTechnique = GiTechniqueRegistry.Create(
                EffectiveGiSettings.Technique);
            if (!ActiveGiTechnique->IsSupported(Capabilities))
            {
                throw std::logic_error(
                    "Selected GI registry technique rejected active capabilities");
            }
            Stats.GiStats = {
                .Technique = EffectiveGiSettings.Technique,
                .Status = GiSelection.Reason,
            };
            RefreshAntiAliasingSelection();
            bGraphDirty = true;
        }

        void PrepareSceneSurfaceFrame(
            const RenderWorld& world,
            const SceneRendererFrameDesc& frame)
        {
            if (!HardwareRayTracingScene && !IsGlobalIlluminationActive())
            {
                bHasCachedRtWorld = false;
                return;
            }
            // RenderWorldSubmission publishes immutable snapshots. Continuing
            // accumulation on that same snapshot must not rehash mesh bytes.
            const bool bBasePbrMaterials = HardwareRayTracingScene && HardwareRayTracingScene->UsesBasePbrMaterials();
            if (!bHasCachedRtWorld || CachedRtWorldFrame != world.FrameNumber || CachedRtWorldScene != world.SceneGeneration ||
                bCachedBasePbrMaterials != bBasePbrMaterials)
            {
                static const bool bTimeRevisions = []
                {
                    const char* setting = std::getenv("HE_RT_REVISION_TIMINGS");
                    return setting && setting[0] == '1' && setting[1] == '\0';
                }();
                const auto revisionStart = bTimeRevisions ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                CurrentRtRevisions = BuildRayTracingSceneRevisions(world, bBasePbrMaterials);
                if (bTimeRevisions && world.FrameNumber % 60 == 0)
                    HE_CORE_INFO("[RtRevisionTiming] frame={} objects={} uniqueMeshes={} totalMs={} topology={} transform={} material={}",
                        world.FrameNumber, world.Objects.size(),
                        CurrentRtRevisions.GeometryContentRevisions.size(),
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - revisionStart).count(),
                        CurrentRtRevisions.GeometryTopology,
                        CurrentRtRevisions.Transform,
                        CurrentRtRevisions.Material);
                bCachedBasePbrMaterials = bBasePbrMaterials;
                if (IsOfflineViewportRenderingActive())
                    CachedOfflineSceneFingerprint = FingerprintOfflineViewportSceneState(world);
                CachedRtWorldFrame = world.FrameNumber;
                CachedRtWorldScene = world.SceneGeneration;
                bHasCachedRtWorld = true;
            }
            if (!HardwareRayTracingScene)
                return;
            HardwareRayTracingScene->Prepare(
                world,
                CurrentRtRevisions,
                frame.CompletedSubmission);
            if (HardwareRayTracingScene->ConsumeGraphRebuildRequest())
                bGraphDirty = true;
            if (SurfaceDataPassInstance)
            {
                SurfaceDataPassInstance->PrepareFrame(
                    CurrentRasterCamera,
                    frame.Width,
                    frame.Height);
            }
        }

        void PrepareGlobalIlluminationFrame(
            const RenderWorld& world,
            const SceneRendererFrameDesc& frame)
        {
            if (!IsGlobalIlluminationActive())
                return;
            if (!IsOfflineViewportRenderingActive())
                FrameData.IblSettings.w = 1.0f;
            const RenderCamera& techniqueCamera =
                IsOfflineViewportRenderingActive()
                ? world.Camera
                : CurrentRasterCamera;

            GlobalIlluminationSettings techniqueSettings =
                EffectiveGiSettings;
            float techniqueMaximumRayDistance = world.Camera.FarPlane;
            glm::vec3 techniqueEnvironmentRadiance(0.0f);
            if (ActiveOfflineViewportRenderer)
            {
                const OfflineViewportRendererSettings& offlineSettings =
                    ActiveOfflineViewportRenderer->GetOfflineSettings();
                techniqueSettings.Technique =
                    GlobalIlluminationTechniqueKind::ExternalViewportRenderer;
                techniqueSettings.Denoiser =
                    GlobalIlluminationDenoiserKind::None;
                techniqueSettings.RenderScale = offlineSettings.RenderScale;
                techniqueSettings.SamplesPerPixel = 1;
                techniqueSettings.MaxBounces = offlineSettings.MaxBounces;
                techniqueSettings.bDirectOnly = offlineSettings.bDirectOnly;
                techniqueMaximumRayDistance = std::min(
                    world.Camera.FarPlane,
                    offlineSettings.MaximumRayDistance);
                techniqueEnvironmentRadiance =
                    offlineSettings.bUseSceneEnvironment
                    ? glm::vec3(FrameData.IblSettings.x)
                    : offlineSettings.EnvironmentRadiance;
            }
            CurrentIlluminationTechniqueSettings = techniqueSettings;

            const GlobalIlluminationHistorySignature historySignature{
                .Width = frame.Width,
                .Height = frame.Height,
                .RenderScale = techniqueSettings.RenderScale,
                .ProjectionFingerprint = FingerprintProjection(world.Camera),
                .JitterSequenceGeneration =
                    world.Camera.JitterSequenceGeneration,
                .SceneGeneration = CurrentRtRevisions.Scene,
                .GeometryTopologyGeneration =
                    CurrentRtRevisions.GeometryTopology,
                .GeometryTransformGeneration = IsScreenSpaceGlobalIlluminationActive()
                    ? FingerprintScreenSpaceTransportGeometry(world) : 0,
                .MaterialGeneration = CurrentRtRevisions.Material,
                .LightGeneration = CurrentRtRevisions.Light,
                .Technique = techniqueSettings.Technique,
                .Denoiser = techniqueSettings.Denoiser,
                .SamplesPerPixel =
                    IsScreenSpaceGlobalIlluminationActive() &&
                        !ActiveOfflineViewportRenderer
                    ? techniqueSettings.ScreenSpaceRayCount
                    : techniqueSettings.SamplesPerPixel,
                .MaxBounces = IsScreenSpaceGlobalIlluminationActive() &&
                        !ActiveOfflineViewportRenderer
                    ? 1u
                    : techniqueSettings.MaxBounces,
                .bHalfResolution = techniqueSettings.RenderScale < 0.999f,
                .bCameraCut = bRasterCameraHistoryReset,
            };
            SsgiReactiveRegion reactiveRegion;
            if (techniqueSettings.bSsgiLocalReactiveBudget && IsScreenSpaceGlobalIlluminationActive())
            {
                const bool bPotentialShadowTransport = SsgiHasPotentialShadowTransport(
                    world, bShadowsEnabled, !ActiveOfflineViewportRenderer && !UsesHardwareRayTracing());
                std::vector<SsgiReactiveObject> objects;
                objects.reserve(std::min(world.Objects.size(), SsgiReactiveBudget::MaxObjects + 1));
                for (const RenderObject& object : world.Objects)
                {
                    if (!object.bVisible || !std::any_of(object.Sections.begin(), object.Sections.end(),
                        [](const RenderMeshSection& section) { return section.GetMaterialInstance().bDepthWrite &&
                            section.AlphaMode != RenderMaterialAlphaMode::Blend; })) continue;
                    uint64_t revision = 14695981039346656037ull;
                    HashValue(revision, object.WorldTransform);
                    for (const auto& bone : object.BonePalette) HashValue(revision, bone);
                    bool trusted = object.Bounds.bValid && object.BonePalette.empty() &&
                        (object.Path == RenderPath::CpuOpaque || object.Path == RenderPath::GpuDrivenOpaque) &&
                        object.MeshResource && !object.MeshResource->mSubMeshes.empty();
                    if (object.MeshResource)
                        for (const auto& subMesh : object.MeshResource->mSubMeshes)
                            trusted = trusted && subMesh && subMesh->HasBounds();
                    bool continuousDeformation = !object.BonePalette.empty() ||
                        object.Path == RenderPath::CpuSkinned || object.Path == RenderPath::CpuSkinnedTransparent;
                    bool globalOnChange = object.bCastShadow && bPotentialShadowTransport;
                    for (const auto& section : object.Sections)
                    {
                        globalOnChange = globalOnChange || glm::any(glm::greaterThan(
                            glm::abs(section.GetMaterialInstance().Emissive), glm::vec3(0.f)));
                        HashValue(revision, section.GetMaterialInstance().Revision);
                        HashValue(revision, section.GetMaterialInstance().ExpectedProgramGeneration);
                        HashValue(revision, section.AlphaCutoff);
                        HashString(revision, section.GetMaterialInstance().ShaderPath);
                        // Reuse the same immutable classification that chooses the
                        // actual raster pipeline. BasePBR paths need not be empty.
                        trusted = trusted && !section.GetMaterialInstance().bCustomSurface;
                        continuousDeformation = continuousDeformation || section.GetMaterialInstance().bCustomSurface ||
                            section.Path == RenderPath::CpuSkinned || section.Path == RenderPath::CpuSkinnedTransparent;
                        HashValue(revision, section.GetMaterialInstance().bCustomSurface);
                    }
                    objects.push_back({ object.EntityId, revision,
                        object.Bounds.Center - object.Bounds.Extents,
                        object.Bounds.Center + object.Bounds.Extents, trusted, globalOnChange, continuousDeformation });
                    if (objects.size() > SsgiReactiveBudget::MaxObjects) break;
                }
                reactiveRegion = ReactiveBudget.Prepare(objects, world.SceneGeneration,
                    historySignature.GeometryTransformGeneration, techniqueCamera.ViewProjection,
                    std::max(1u, uint32_t(std::ceil(frame.Width * techniqueSettings.RenderScale))),
                    std::max(1u, uint32_t(std::ceil(frame.Height * techniqueSettings.RenderScale))),
                    techniqueSettings.ScreenSpaceMaximumRayDistance, techniqueSettings.MaxBounces,
                    techniqueSettings.ScreenSpaceThickness, historySignature.bCameraCut);
            }
            else ReactiveBudget = {};
            GetActiveIlluminationTechnique()->Collect(frame.CompletedSubmission);
            const GlobalIlluminationPrepareResult prepareResult =
                GetActiveIlluminationTechnique()->PrepareFrame(
                    GlobalIlluminationFrameContext{
                        .Camera = techniqueCamera,
                        .HistorySignature = historySignature,
                        .ReactiveRegion = reactiveRegion,
                        .Width = frame.Width,
                        .Height = frame.Height,
                        .FrameSequenceIndex = static_cast<uint32_t>(
                            GlobalIlluminationRenderSequence),
                        .DepthMipCount = CalculateMipCount(
                            frame.Width,
                            frame.Height),
                        .EmissiveTriangleCount =
                            HardwareRayTracingScene
                                ? HardwareRayTracingScene->GetStats()
                                    .EmissiveTriangleCount
                                : 0u,
                        .MaximumRayDistance = techniqueMaximumRayDistance,
                        .EnvironmentRadiance = techniqueEnvironmentRadiance,
                        .SceneStateFingerprint = IsOfflineViewportRenderingActive()
                            ? CachedOfflineSceneFingerprint
                            : 0,
                        .CompletedSubmission = frame.CompletedSubmission,
                    },
                    techniqueSettings);
            bGraphDirty = bGraphDirty ||
                prepareResult.bGraphRebuildRequested;
            bGlobalIlluminationFrameRecorded = true;
            RefreshGlobalIlluminationStats();
        }

        [[nodiscard]] GlobalIlluminationInputs BuildGlobalIlluminationInputs() const
        {
            return {
                .Surface = CurrentSurfaceData,
                .SceneTlas = CurrentRtGraphResources.SceneTlas,
                .FrameBuffer = FrameBufferHandle,
                .MaterialBuffer = CurrentRtGraphResources.Materials,
                .GeometryBuffer = CurrentRtGraphResources.Geometries,
                .LightBuffer = CurrentRtGraphResources.EmissiveTriangles,
                .RayTracingVertexBuffer = CurrentRtGraphResources.Vertices,
                .RayTracingIndexBuffer = CurrentRtGraphResources.Indices,
                .RayTracingInstanceBuffer = CurrentRtGraphResources.Instances,
                .AnalyticLightBuffer = CurrentRtGraphResources.AnalyticLights,
                .Environment = EnvironmentHandle,
                .EnvironmentCdf = EnvironmentCdfHandle,
                .EnvironmentProposal = ActiveEnvironmentProposal,
                .PackedNormalRoughnessAo = RasterSurfaceTargets[1],
                .AmbientOcclusion = CurrentAmbientOcclusion,
                .OpaqueDirectHdr = OpaqueDirectHdrHandle,
                .DepthPyramid = CurrentDepthPyramid.Depth,
                .Width = LastWidth,
                .Height = LastHeight,
            };
        }

        void AddGlobalIlluminationPasses()
        {
            NativeGiDiagnosticInput = {};
            if (!IsGlobalIlluminationActive())
                return;
            const GlobalIlluminationInputs inputs =
                BuildGlobalIlluminationInputs();
            TextureHandle compositeInput;
            GlobalIlluminationCompositeMode compositeMode =
                GlobalIlluminationCompositeMode::Additive;
            uint32_t compositeInputChannel = 0;
            CurrentGiOutputs = GetActiveIlluminationTechnique()->AddPasses(
                Graph,
                inputs,
                CurrentIlluminationTechniqueSettings);
            if (IsOfflineViewportRendererActive())
            {
                compositeInput = CurrentGiOutputs.CompositeRadiance;
                compositeMode = GlobalIlluminationCompositeMode::Replace;
            }
            else
            {
                compositeInput = CurrentGiOutputs.CompositeRadiance;
                if (EffectiveGiSettings.DebugView !=
                    GlobalIlluminationDebugView::Composite)
                {
                    compositeMode = GlobalIlluminationCompositeMode::Replace;
                    const GlobalIlluminationOutputs& raw = CurrentGiOutputs;
                    switch (EffectiveGiSettings.DebugView)
                    {
                    case GlobalIlluminationDebugView::RawDiffuse:
                        compositeInput = raw.RawDiffuse;
                        break;
                    case GlobalIlluminationDebugView::RawDirectDiffuse:
                        compositeInput = raw.RawDirectDiffuse;
                        break;
                    case GlobalIlluminationDebugView::RawIndirectDiffuse:
                        compositeInput = raw.RawIndirectDiffuse;
                        break;
                    case GlobalIlluminationDebugView::Albedo:
                        compositeInput = inputs.Surface.BaseColorMetalness;
                        break;
                    case GlobalIlluminationDebugView::DemodulatedDiffuseIllumination:
                        compositeInput =
                            CurrentGiOutputs.DemodulatedDiffuseIllumination;
                        break;
                    case GlobalIlluminationDebugView::FilteredDiffuseIllumination:
                        compositeInput =
                            CurrentGiOutputs.FilteredDiffuseIllumination;
                        break;
                    case GlobalIlluminationDebugView::RemodulatedDiffuse:
                    case GlobalIlluminationDebugView::DenoisedDiffuse:
                        compositeInput = CurrentGiOutputs.CompositeRadiance;
                        break;
                    case GlobalIlluminationDebugView::Normal:
                        compositeInput = inputs.Surface.NormalRoughness;
                        break;
                    case GlobalIlluminationDebugView::HitDistance:
                        compositeInput = CurrentGiOutputs.HitDistance;
                        compositeInputChannel = 1;
                        break;
                    case GlobalIlluminationDebugView::HistoryLength:
                        compositeInput = CurrentGiOutputs.HistoryLength;
                        compositeInputChannel = 1;
                        break;
                    case GlobalIlluminationDebugView::Confidence:
                        compositeInput = CurrentGiOutputs.Confidence;
                        compositeInputChannel = 1;
                        break;
                    case GlobalIlluminationDebugView::Coverage:
                        compositeInput = CurrentGiOutputs.CoverageMask;
                        compositeInputChannel = 1;
                        break;
                    case GlobalIlluminationDebugView::Variance:
                        compositeInput = CurrentGiOutputs.Variance;
                        compositeInputChannel = 1;
                        break;
                    case GlobalIlluminationDebugView::SourceLighting:
                        SceneColorHandle = OpaqueDirectHdrHandle;
                        NativeGiDiagnosticInput = OpaqueDirectHdrHandle;
                        return;
                    case GlobalIlluminationDebugView::TemporalDiffuseIllumination:
                        compositeInput = CurrentGiOutputs.TemporalDiffuseIllumination;
                        break;
                    case GlobalIlluminationDebugView::ViewDepth:
                        compositeInput = inputs.Surface.ViewDepth;
                        compositeInputChannel = 1;
                        break;
                    case GlobalIlluminationDebugView::ScreenHitCoverage:
                        compositeInput = raw.RawIndirectDiffuse;
                        compositeInputChannel = 2;
                        break;
                    case GlobalIlluminationDebugView::TraceTermination:
                    case GlobalIlluminationDebugView::TraceHitLocation:
                        compositeInput = raw.RawDirectDiffuse;
                        break;
                    default:
                        compositeInput = CurrentGiOutputs.CompositeRadiance;
                        break;
                    }
                }
            }
            if (!compositeInput)
                throw std::logic_error("Global illumination produced no composite input");
            if (EffectiveGiSettings.DebugView != GlobalIlluminationDebugView::Composite)
                NativeGiDiagnosticInput = compositeInput;
            if (IsMsaaActive())
            {
                if (compositeMode != GlobalIlluminationCompositeMode::Additive)
                {
                    throw std::logic_error(
                        "MSAA GI composition requires the additive composite view");
                }
                SceneColorHandle = CompositePassInstance->AddMultisamplePass(
                    Graph,
                    SceneColorHandle,
                    RasterSceneDepthHandle,
                    SceneDepthHandle,
                    compositeInput,
                    LastWidth,
                    LastHeight,
                    GetRasterSampleCount(),
                    compositeInputChannel);
            }
            else
            {
                SceneColorHandle = CompositePassInstance->AddPass(
                    Graph,
                    SceneColorHandle,
                    compositeInput,
                    compositeMode,
                    LastWidth,
                    LastHeight,
                    compositeInputChannel);
            }
        }

        void RefreshGlobalIlluminationStats()
        {
            if (!IsGlobalIlluminationActive())
                return;
            Stats.GiStats = GetActiveIlluminationTechnique()->GetStats();
            if (!HardwareRayTracingScene)
            {
                Stats.RayTracingBlasBuildCount = 0;
                Stats.RayTracingBlasReuseCount = 0;
                Stats.RayTracingTlasBuildCount = 0;
                Stats.RayTracingTlasUpdateCount = 0;
                Stats.RayTracingInstanceCount = 0;
                Stats.RayTracingExcludedInstanceCount = 0;
                Stats.RayTracingEmissiveTriangleCount = 0;
                Stats.RayTracingGeometryBytes = 0;
                Stats.RayTracingSceneTableBytes = 0;
                Stats.RayTracingActiveBufferAllocationRequirementBytes = 0;
                Stats.RayTracingActiveAsAllocationRequirementBytes = 0;
                return;
            }
            const RayTracingSceneStats& rtStats =
                HardwareRayTracingScene->GetStats();
            Stats.RayTracingBlasBuildCount = rtStats.BlasBuildCount;
            Stats.RayTracingBlasReuseCount = rtStats.BlasReuseCount;
            Stats.RayTracingTlasBuildCount = rtStats.TlasBuildCount;
            Stats.RayTracingTlasUpdateCount = rtStats.TlasUpdateCount;
            Stats.RayTracingInstanceCount = rtStats.TlasInstanceCount;
            Stats.RayTracingExcludedInstanceCount = rtStats.ExcludedInstanceCount;
            Stats.RayTracingEmissiveTriangleCount = rtStats.EmissiveTriangleCount;
            Stats.RayTracingGeometryBytes = rtStats.GeometryContentBytes;
            Stats.RayTracingSceneTableBytes = rtStats.SceneTableBytes;
            Stats.RayTracingActiveBufferAllocationRequirementBytes =
                rtStats.ActiveBufferAllocationRequirementBytes;
            Stats.RayTracingActiveAsAllocationRequirementBytes =
                rtStats.ActiveAccelerationStructureAllocationRequirementBytes;
        }

        void ResetHardwareRayTracingStats() noexcept
        {
            Stats.RayTracingBlasBuildCount = 0;
            Stats.RayTracingBlasReuseCount = 0;
            Stats.RayTracingTlasBuildCount = 0;
            Stats.RayTracingTlasUpdateCount = 0;
            Stats.RayTracingInstanceCount = 0;
            Stats.RayTracingExcludedInstanceCount = 0;
            Stats.RayTracingEmissiveTriangleCount = 0;
            Stats.RayTracingGeometryBytes = 0;
            Stats.RayTracingSceneTableBytes = 0;
            Stats.RayTracingActiveBufferAllocationRequirementBytes = 0;
            Stats.RayTracingActiveAsAllocationRequirementBytes = 0;
            Stats.GpuRayTracingBlasBuildEventMilliseconds = 0.0;
            Stats.GpuRayTracingTlasBuildEventMilliseconds = 0.0;
            Stats.GpuRayTracingTlasUpdateMilliseconds = 0.0;
            Stats.GpuGiTraceMilliseconds = 0.0;
            Stats.GpuSvgfMilliseconds = 0.0;
            Stats.GpuGiCompositeMilliseconds = 0.0;
            Stats.bGpuGiTimingValid = false;
        }

        void RefreshRenderGraphMemoryStats()
        {
            const RenderGraphResourcePoolStats poolStats = ResourcePool.GetStats();
            Stats.RenderGraphPersistentBytes =
                poolStats.PersistentTextureBytes + poolStats.PersistentBufferBytes;
            Stats.RenderGraphTransientReservedBytes =
                poolStats.TransientTextureBytes + poolStats.TransientBufferBytes;
            Stats.RenderGraphCachedTransientBytes = poolStats.CachedTransientBytes;
            Stats.RenderGraphRetiredResourceBytes = poolStats.RetiredResourceBytes;
            Stats.RenderGraphTrimmedTransientBytes = poolStats.TrimmedTransientBytes;
            Stats.RenderGraphPeakFrameTransientBytes =
                poolStats.PeakFrameTransientBytes;
        }

        void CreateShaders()
        {
            const char* materialPixelProfile = bHeapDirectlyIndexed ? "ps_6_6" : "ps_6_0";
            const std::string opaquePixelSource = bHeapDirectlyIndexed
                ? "Modern/SceneOpaqueHeapDirect.hlsl"
                : "Modern/SceneOpaque.hlsl";
            const std::string depthPixelSource = bHeapDirectlyIndexed
                ? "Modern/SceneDepthPrepassHeapDirect.hlsl"
                : "Modern/SceneDepthPrepass.hlsl";
            const std::string shadowPixelSource = bHeapDirectlyIndexed
                ? "Modern/SceneShadowHeapDirect.hlsl"
                : "Modern/SceneShadow.hlsl";
            const std::string primitivePixelSource = bHeapDirectlyIndexed
                ? "Modern/ScenePrimitive2DHeapDirect.hlsl"
                : "Modern/ScenePrimitive2D.hlsl";
            const std::filesystem::path opaqueVertexPath = ShaderCacheDirectory / "SceneOpaque.vs";
            const std::filesystem::path opaquePixelPath = ShaderCacheDirectory /
                (bHeapDirectlyIndexed
                    ? "SceneOpaqueHeapDirect__ps_6_6.ps"
                    : "SceneOpaque.ps");
            const std::filesystem::path gpuOpaqueVertexPath =
                ShaderCacheDirectory / "SceneOpaque_GPU_DRIVEN.vs";
            const std::filesystem::path gpuOpaquePixelPath = ShaderCacheDirectory /
                (bHeapDirectlyIndexed
                    ? "SceneOpaqueHeapDirect_GPU_DRIVEN__ps_6_6.ps"
                    : "SceneOpaque_GPU_DRIVEN.ps");
            const std::filesystem::path skinnedVertexPath =
                ShaderCacheDirectory / "SceneOpaque_SKINNED.vs";
            const std::filesystem::path skinnedPixelPath = ShaderCacheDirectory /
                (bHeapDirectlyIndexed
                    ? "SceneOpaqueHeapDirect_SKINNED__ps_6_6.ps"
                    : "SceneOpaque_SKINNED.ps");
            const std::filesystem::path depthVertexPath =
                ShaderCacheDirectory / "SceneDepthPrepass.vs";
            const std::filesystem::path depthPixelPath = ShaderCacheDirectory /
                (bHeapDirectlyIndexed
                    ? "SceneDepthPrepassHeapDirect__ps_6_6.ps"
                    : "SceneDepthPrepass.ps");
            const std::filesystem::path gpuDepthVertexPath =
                ShaderCacheDirectory / "SceneDepthPrepass_GPU_DRIVEN.vs";
            const std::filesystem::path gpuDepthPixelPath = ShaderCacheDirectory /
                (bHeapDirectlyIndexed
                    ? "SceneDepthPrepassHeapDirect_GPU_DRIVEN__ps_6_6.ps"
                    : "SceneDepthPrepass_GPU_DRIVEN.ps");
            const std::filesystem::path skinnedDepthVertexPath =
                ShaderCacheDirectory / "SceneDepthPrepass_SKINNED.vs";
            const std::filesystem::path skinnedDepthPixelPath = ShaderCacheDirectory /
                (bHeapDirectlyIndexed
                    ? "SceneDepthPrepassHeapDirect_SKINNED__ps_6_6.ps"
                    : "SceneDepthPrepass_SKINNED.ps");
            const std::filesystem::path outlineVertexPath =
                ShaderCacheDirectory / "SceneOutline.vs";
            const std::filesystem::path outlinePixelPath =
                ShaderCacheDirectory / "SceneOutline.ps";
            const std::filesystem::path skinnedOutlineVertexPath =
                ShaderCacheDirectory / "SceneOutline_SKINNED.vs";
            const std::filesystem::path skinnedOutlinePixelPath =
                ShaderCacheDirectory / "SceneOutline_SKINNED.ps";
            const std::filesystem::path gpuCullPath = ShaderCacheDirectory / "SceneGpuCull.cs";
            const std::filesystem::path environmentPrecomputePath =
                ShaderCacheDirectory / "SceneEnvironmentPrecompute.cs";
            const std::filesystem::path shadowVertexPath = ShaderCacheDirectory / "SceneShadow.vs";
            const std::filesystem::path shadowPixelPath = ShaderCacheDirectory /
                (bHeapDirectlyIndexed
                    ? "SceneShadowHeapDirect__ps_6_6.ps"
                    : "SceneShadow.ps");
            const std::filesystem::path skinnedShadowVertexPath =
                ShaderCacheDirectory / "SceneShadow_SKINNED.vs";
            const std::filesystem::path skinnedShadowPixelPath = ShaderCacheDirectory /
                (bHeapDirectlyIndexed
                    ? "SceneShadowHeapDirect_SKINNED__ps_6_6.ps"
                    : "SceneShadow_SKINNED.ps");
            const std::filesystem::path primitiveVertexPath =
                ShaderCacheDirectory / "ScenePrimitive2D.vs";
            const std::filesystem::path primitivePixelPath = ShaderCacheDirectory /
                (bHeapDirectlyIndexed
                    ? "ScenePrimitive2DHeapDirect__ps_6_6.ps"
                    : "ScenePrimitive2D.ps");
            const std::filesystem::path toneVertexPath = ShaderCacheDirectory / "SceneToneMap.vs";
            const std::filesystem::path tonePixelPath = ShaderCacheDirectory / "SceneToneMap.ps";
            const std::filesystem::path skyVertexPath = ShaderCacheDirectory / "SceneSky.vs";
            const std::filesystem::path skyPixelPath = ShaderCacheDirectory / "SceneSky.ps";

            OpaqueVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneOpaque.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(opaqueVertexPath, ".cachekey")),
                },
                opaqueVertexPath,
                "SceneRenderer Opaque VS");
            OpaquePixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = opaquePixelSource,
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = materialPixelProfile,
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(opaquePixelPath, ".cachekey")),
                },
                opaquePixelPath,
                "SceneRenderer Opaque PS");
            GpuOpaqueVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneOpaque.hlsl",
                    .Variant = "GPU_DRIVEN",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(gpuOpaqueVertexPath, ".cachekey")),
                },
                gpuOpaqueVertexPath,
                "SceneRenderer GPU-driven Opaque VS");
            GpuOpaquePixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = opaquePixelSource,
                    .Variant = "GPU_DRIVEN",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = materialPixelProfile,
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(gpuOpaquePixelPath, ".cachekey")),
                },
                gpuOpaquePixelPath,
                "SceneRenderer GPU-driven Opaque PS");
            SkinnedVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneOpaque.hlsl",
                    .Variant = "SKINNED",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(skinnedVertexPath, ".cachekey")),
                },
                skinnedVertexPath,
                "SceneRenderer Skinned VS");
            SkinnedPixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = opaquePixelSource,
                    .Variant = "SKINNED",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = materialPixelProfile,
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(skinnedPixelPath, ".cachekey")),
                },
                skinnedPixelPath,
                "SceneRenderer Skinned PS");
            DepthVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneDepthPrepass.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(depthVertexPath, ".cachekey")),
                },
                depthVertexPath,
                "SceneRenderer Depth Prepass VS");
            DepthPixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = depthPixelSource,
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = materialPixelProfile,
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(depthPixelPath, ".cachekey")),
                },
                depthPixelPath,
                "SceneRenderer Depth Prepass PS");
            GpuDepthVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneDepthPrepass.hlsl",
                    .Variant = "GPU_DRIVEN",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(gpuDepthVertexPath, ".cachekey")),
                },
                gpuDepthVertexPath,
                "SceneRenderer GPU Depth Prepass VS");
            GpuDepthPixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = depthPixelSource,
                    .Variant = "GPU_DRIVEN",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = materialPixelProfile,
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(gpuDepthPixelPath, ".cachekey")),
                },
                gpuDepthPixelPath,
                "SceneRenderer GPU Depth Prepass PS");
            SkinnedDepthVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneDepthPrepass.hlsl",
                    .Variant = "SKINNED",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(skinnedDepthVertexPath, ".cachekey")),
                },
                skinnedDepthVertexPath,
                "SceneRenderer Skinned Depth Prepass VS");
            SkinnedDepthPixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = depthPixelSource,
                    .Variant = "SKINNED",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = materialPixelProfile,
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(skinnedDepthPixelPath, ".cachekey")),
                },
                skinnedDepthPixelPath,
                "SceneRenderer Skinned Depth Prepass PS");
            OutlineVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneOutline.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(outlineVertexPath, ".cachekey")),
                },
                outlineVertexPath,
                "SceneRenderer Editor Outline VS");
            OutlinePixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneOutline.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(outlinePixelPath, ".cachekey")),
                },
                outlinePixelPath,
                "SceneRenderer Editor Outline PS");
            SkinnedOutlineVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneOutline.hlsl",
                    .Variant = "SKINNED",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(skinnedOutlineVertexPath, ".cachekey")),
                },
                skinnedOutlineVertexPath,
                "SceneRenderer Skinned Editor Outline VS");
            SkinnedOutlinePixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneOutline.hlsl",
                    .Variant = "SKINNED",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(skinnedOutlinePixelPath, ".cachekey")),
                },
                skinnedOutlinePixelPath,
                "SceneRenderer Skinned Editor Outline PS");
            GpuCullShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneGpuCull.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Compute,
                    .EntryPoint = "CS",
                    .TargetProfile = "cs_6_0",
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(gpuCullPath, ".cachekey")),
                },
                gpuCullPath,
                "SceneRenderer GPU frustum culling CS");
            EnvironmentPrecomputeShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneEnvironmentPrecompute.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Compute,
                    .EntryPoint = "CS",
                    .TargetProfile = "cs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(environmentPrecomputePath, ".cachekey")),
                },
                environmentPrecomputePath,
                "SceneRenderer environment IBL precompute CS");
            ShadowVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneShadow.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(shadowVertexPath, ".cachekey")),
                },
                shadowVertexPath,
                "SceneRenderer Directional Shadow VS");
            ShadowPixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = shadowPixelSource,
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = materialPixelProfile,
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(shadowPixelPath, ".cachekey")),
                },
                shadowPixelPath,
                "SceneRenderer Directional Shadow PS");
            SkinnedShadowVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneShadow.hlsl",
                    .Variant = "SKINNED",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(skinnedShadowVertexPath, ".cachekey")),
                },
                skinnedShadowVertexPath,
                "SceneRenderer Skinned Directional Shadow VS");
            SkinnedShadowPixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = shadowPixelSource,
                    .Variant = "SKINNED",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = materialPixelProfile,
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(skinnedShadowPixelPath, ".cachekey")),
                },
                skinnedShadowPixelPath,
                "SceneRenderer Skinned Directional Shadow PS");
            Primitive2DVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/ScenePrimitive2D.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(primitiveVertexPath, ".cachekey")),
                },
                primitiveVertexPath,
                "SceneRenderer Primitive2D VS");
            Primitive2DPixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = primitivePixelSource,
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = materialPixelProfile,
                    .ContentHash = ReadShaderContentHash(
                        Utils::PathWithSuffix(primitivePixelPath, ".cachekey")),
                },
                primitivePixelPath,
                "SceneRenderer Primitive2D PS");
            ToneVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneToneMap.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(toneVertexPath, ".cachekey")),
                },
                toneVertexPath,
                "SceneRenderer ToneMap VS");
            TonePixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneToneMap.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(tonePixelPath, ".cachekey")),
                },
                tonePixelPath,
                "SceneRenderer ToneMap PS");
            SkyVertexShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneSky.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(skyVertexPath, ".cachekey")),
                },
                skyVertexPath,
                "SceneRenderer Sky VS");
            SkyPixelShader = ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Modern/SceneSky.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                    .ContentHash = ReadShaderContentHash(Utils::PathWithSuffix(skyPixelPath, ".cachekey")),
                },
                skyPixelPath,
                "SceneRenderer Sky PS");
        }

        void CreateImmutableState()
        {
            nvrhi::BindingLayoutDesc frameLayoutDesc;
            frameLayoutDesc
                .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(0)
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(1))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2));
            FrameBindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x53434e4652414d45ull },
                frameLayoutDesc,
                "SceneRenderer space0 frame layout");

            nvrhi::BindingLayoutDesc objectLayoutDesc;
            objectLayoutDesc
                .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(2)
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
            ObjectBindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x53434e4f424a4c5aull },
                objectLayoutDesc,
                "SceneRenderer space2 object/material layout");

            nvrhi::BindingLayoutDesc shadowPassLayoutDesc;
            shadowPassLayoutDesc
                .setVisibility(nvrhi::ShaderType::Vertex)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0, sizeof(ShadowPassConstants)))
                // Vulkan requires descriptor pools to contain at least one
                // descriptor type even when a set otherwise carries only push
                // constants. This harmless unused sampler keeps the shared
                // NVRHI layout valid on both backends.
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            ShadowPassBindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x53434e5348504153ull },
                shadowPassLayoutDesc,
                "SceneRenderer space3 directional shadow pass layout");

            nvrhi::BindingLayoutDesc gpuObjectLayoutDesc;
            gpuObjectLayoutDesc
                .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(2)
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
            GpuObjectBindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x53434e4750554f42ull },
                gpuObjectLayoutDesc,
                "SceneRenderer space2 GPU object/visible layout");

            nvrhi::BindingLayoutDesc cullingLayoutDesc;
            cullingLayoutDesc
                .setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(CullConstants)))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
            GpuCullingBindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x53434e4750554355ull },
                cullingLayoutDesc,
                "SceneRenderer space3 GPU culling layout");

            nvrhi::BindingLayoutDesc environmentPrecomputeLayoutDesc;
            environmentPrecomputeLayoutDesc
                .setVisibility(nvrhi::ShaderType::Compute)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0, sizeof(EnvironmentPrecomputeConstants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(1))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            EnvironmentPrecomputeBindingLayout =
                PipelineCacheInstance.GetOrCreateBindingLayout(
                    BindingLayoutKey{ 0x53434e454e565043ull },
                    environmentPrecomputeLayoutDesc,
                    "SceneRenderer space3 environment precompute layout");

            nvrhi::BindingLayoutDesc shadowTextureLayoutDesc;
            shadowTextureLayoutDesc
                .setVisibility(nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(3))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(10))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(11))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(1));
            ShadowTextureBindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x53434e5348445445ull },
                shadowTextureLayoutDesc,
                "SceneRenderer space3 directional shadow texture layout");

            nvrhi::BindingLayoutDesc skyLayoutDesc;
            skyLayoutDesc
                .setVisibility(nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            SkyBindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x53434e534b594c59ull },
                skyLayoutDesc,
                "SceneRenderer space3 sky texture layout");

            nvrhi::BindingLayoutDesc toneLayoutDesc;
            toneLayoutDesc
                .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ToneMappingConstants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            ToneBindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x53434e544f4e454cull },
                toneLayoutDesc,
                "SceneRenderer space3 tone-map layout");

            nvrhi::BindingLayoutDesc primitiveLayoutDesc;
            primitiveLayoutDesc
                .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0, sizeof(Primitive2DConstants)))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            Primitive2DBindingLayout = PipelineCacheInstance.GetOrCreateBindingLayout(
                BindingLayoutKey{ 0x53434e5052494d32ull },
                primitiveLayoutDesc,
                "SceneRenderer space3 Primitive2D layout");

            nvrhi::SamplerDesc samplerDesc;
            samplerDesc.setAllFilters(true)
                .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
            ToneSampler = Device->createSampler(samplerDesc);
            ShadowSampler = Device->createSampler(samplerDesc);
            nvrhi::SamplerDesc comparisonDesc = samplerDesc;
            comparisonDesc.setReductionType(nvrhi::SamplerReductionType::Comparison);
            ShadowComparisonSampler = Device->createSampler(comparisonDesc);
            EnvironmentSampler = Device->createSampler(samplerDesc);
            samplerDesc.setAddressU(nvrhi::SamplerAddressMode::Wrap)
                .setAddressV(nvrhi::SamplerAddressMode::Clamp)
                .setAddressW(nvrhi::SamplerAddressMode::Clamp);
            EquirectangularSampler = Device->createSampler(samplerDesc);

            const std::array<nvrhi::VertexAttributeDesc, 8> attributes = {
                nvrhi::VertexAttributeDesc()
                    .setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, Pos)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc().setName("PREVIOUS_POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, PreviousPosition)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("NORMAL")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, Normal)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("TEXCOORD")
                    .setFormat(nvrhi::Format::RG32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, TexCoord)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("TANGENT")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, Tangent)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("BINORMAL")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, Bitangent)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORDONE")
                    .setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, TexCoord1)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc().setName("COLOR")
                    .setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, Color)))
                    .setElementStride(sizeof(StaticVertex)),
            };
            InputLayout = Device->createInputLayout(
                attributes.data(),
                static_cast<uint32_t>(attributes.size()),
                OpaqueVertexShader);
            const std::array<nvrhi::VertexAttributeDesc, 10> skinnedAttributes = {
                nvrhi::VertexAttributeDesc().setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Pos)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("PREVIOUS_POSITION").setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, PreviousPosition)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("NORMAL")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Normal)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORD")
                    .setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, TexCoord)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("TANGENT")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Tangent)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BINORMAL")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Bitangent)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BONEIDS").setArraySize(MAX_BONE_INFLUENCE / 4)
                    .setFormat(nvrhi::Format::RGBA32_SINT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mBoneIDs)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BONEWEIGHTS").setArraySize(MAX_BONE_INFLUENCE / 4)
                    .setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mWeights)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORDONE")
                    .setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, TexCoord1)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("COLOR")
                    .setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Color)))
                    .setElementStride(sizeof(SkinnedVertex)),
            };
            SkinnedInputLayout = Device->createInputLayout(
                skinnedAttributes.data(),
                static_cast<uint32_t>(skinnedAttributes.size()),
                SkinnedVertexShader);
            std::array<nvrhi::VertexAttributeDesc, 9> gpuAttributes;
            std::copy(attributes.begin(), attributes.end(), gpuAttributes.begin());
            gpuAttributes.back() = nvrhi::VertexAttributeDesc().setName("INPUT_OBJECT_ID")
                .setFormat(nvrhi::Format::R32_UINT).setBufferIndex(1).setOffset(0)
                .setElementStride(sizeof(uint32_t)).setIsInstanced(true);
            GpuInputLayout = Device->createInputLayout(
                gpuAttributes.data(), static_cast<uint32_t>(gpuAttributes.size()),
                GpuOpaqueVertexShader);
            const std::array<nvrhi::VertexAttributeDesc, 4> shadowAttributes = {
                nvrhi::VertexAttributeDesc()
                    .setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, Pos)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("TEXCOORD")
                    .setFormat(nvrhi::Format::RG32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, TexCoord)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORDONE")
                    .setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, TexCoord1)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc().setName("COLOR")
                    .setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, Color)))
                    .setElementStride(sizeof(StaticVertex)),
            };
            ShadowInputLayout = Device->createInputLayout(
                shadowAttributes.data(),
                static_cast<uint32_t>(shadowAttributes.size()),
                ShadowVertexShader);
            std::array<nvrhi::VertexAttributeDesc, 5> gpuDepthAttributes;
            std::copy(shadowAttributes.begin(), shadowAttributes.end(), gpuDepthAttributes.begin());
            gpuDepthAttributes.back() = nvrhi::VertexAttributeDesc().setName("INPUT_OBJECT_ID")
                .setFormat(nvrhi::Format::R32_UINT).setBufferIndex(1).setOffset(0)
                .setElementStride(sizeof(uint32_t)).setIsInstanced(true);
            GpuDepthInputLayout = Device->createInputLayout(gpuDepthAttributes.data(),
                static_cast<uint32_t>(gpuDepthAttributes.size()), GpuDepthVertexShader);
            gpuDepthAttributes.back().setOffset(static_cast<uint32_t>(offsetof(GpuDrawData, ObjectIndex)))
                .setElementStride(sizeof(GpuDrawData));
            GpuUnculledDepthInputLayout = Device->createInputLayout(gpuDepthAttributes.data(),
                static_cast<uint32_t>(gpuDepthAttributes.size()), GpuDepthVertexShader);
            const std::array<nvrhi::VertexAttributeDesc, 6> skinnedShadowAttributes = {
                nvrhi::VertexAttributeDesc().setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Pos)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORD")
                    .setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, TexCoord)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BONEIDS").setArraySize(MAX_BONE_INFLUENCE / 4)
                    .setFormat(nvrhi::Format::RGBA32_SINT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mBoneIDs)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BONEWEIGHTS").setArraySize(MAX_BONE_INFLUENCE / 4)
                    .setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mWeights)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORDONE")
                    .setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, TexCoord1)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("COLOR")
                    .setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Color)))
                    .setElementStride(sizeof(SkinnedVertex)),
            };
            SkinnedShadowInputLayout = Device->createInputLayout(
                skinnedShadowAttributes.data(),
                static_cast<uint32_t>(skinnedShadowAttributes.size()),
                SkinnedShadowVertexShader);

            const std::array<nvrhi::VertexAttributeDesc, 2> outlineAttributes = {
                nvrhi::VertexAttributeDesc().setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, Pos)))
                    .setElementStride(sizeof(StaticVertex)),
                nvrhi::VertexAttributeDesc().setName("NORMAL")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, Normal)))
                    .setElementStride(sizeof(StaticVertex)),
            };
            OutlineInputLayout = Device->createInputLayout(
                outlineAttributes.data(),
                static_cast<uint32_t>(outlineAttributes.size()),
                OutlineVertexShader);
            const std::array<nvrhi::VertexAttributeDesc, 4> skinnedOutlineAttributes = {
                nvrhi::VertexAttributeDesc().setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Pos)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("NORMAL")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Normal)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BONEIDS").setArraySize(MAX_BONE_INFLUENCE / 4)
                    .setFormat(nvrhi::Format::RGBA32_SINT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mBoneIDs)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BONEWEIGHTS").setArraySize(MAX_BONE_INFLUENCE / 4)
                    .setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mWeights)))
                    .setElementStride(sizeof(SkinnedVertex)),
            };
            SkinnedOutlineInputLayout = Device->createInputLayout(
                skinnedOutlineAttributes.data(),
                static_cast<uint32_t>(skinnedOutlineAttributes.size()),
                SkinnedOutlineVertexShader);

            const std::array<nvrhi::VertexAttributeDesc, 11> primitiveAttributes = {
                nvrhi::VertexAttributeDesc().setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, Position)))
                    .setElementStride(sizeof(Primitive2DVertex)),
                nvrhi::VertexAttributeDesc().setName("LINEEND")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, LineEnd)))
                    .setElementStride(sizeof(Primitive2DVertex)),
                nvrhi::VertexAttributeDesc().setName("COLOR")
                    .setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, Color)))
                    .setElementStride(sizeof(Primitive2DVertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORD")
                    .setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, TexCoord)))
                    .setElementStride(sizeof(Primitive2DVertex)),
                nvrhi::VertexAttributeDesc().setName("LOCALCOORD")
                    .setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, LocalCoord)))
                    .setElementStride(sizeof(Primitive2DVertex)),
                nvrhi::VertexAttributeDesc().setName("TEXINDEX")
                    .setFormat(nvrhi::Format::R32_UINT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, TextureIndex)))
                    .setElementStride(sizeof(Primitive2DVertex)),
                nvrhi::VertexAttributeDesc().setName("KIND")
                    .setFormat(nvrhi::Format::R32_UINT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, Kind)))
                    .setElementStride(sizeof(Primitive2DVertex)),
                nvrhi::VertexAttributeDesc().setName("TILING")
                    .setFormat(nvrhi::Format::R32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, Tiling)))
                    .setElementStride(sizeof(Primitive2DVertex)),
                nvrhi::VertexAttributeDesc().setName("THICKNESS")
                    .setFormat(nvrhi::Format::R32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, Thickness)))
                    .setElementStride(sizeof(Primitive2DVertex)),
                nvrhi::VertexAttributeDesc().setName("FADE")
                    .setFormat(nvrhi::Format::R32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, Fade)))
                    .setElementStride(sizeof(Primitive2DVertex)),
                nvrhi::VertexAttributeDesc().setName("LINEWIDTH")
                    .setFormat(nvrhi::Format::R32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(Primitive2DVertex, LineWidth)))
                    .setElementStride(sizeof(Primitive2DVertex)),
            };
            Primitive2DInputLayout = Device->createInputLayout(
                primitiveAttributes.data(),
                static_cast<uint32_t>(primitiveAttributes.size()),
                Primitive2DVertexShader);

            if (!FrameBindingLayout || !ObjectBindingLayout || !ShadowPassBindingLayout ||
                !GpuObjectBindingLayout || !GpuCullingBindingLayout ||
                !EnvironmentPrecomputeBindingLayout ||
                !ShadowTextureBindingLayout || !SkyBindingLayout || !ToneBindingLayout ||
                !Primitive2DBindingLayout ||
                !ToneSampler || !ShadowSampler || !ShadowComparisonSampler || !EnvironmentSampler ||
                !EquirectangularSampler ||
                !InputLayout || !GpuInputLayout || !GpuDepthInputLayout || !GpuUnculledDepthInputLayout || !ShadowInputLayout ||
                !SkinnedShadowInputLayout ||
                !OutlineInputLayout || !SkinnedOutlineInputLayout ||
                !Primitive2DInputLayout || !SkinnedInputLayout)
            {
                throw std::runtime_error("NVRHI failed to create SceneRenderer immutable state");
            }

            nvrhi::ComputePipelineDesc environmentPipelineDesc;
            environmentPipelineDesc
                .setComputeShader(EnvironmentPrecomputeShader)
                .addBindingLayout(EnvironmentPrecomputeBindingLayout);
            EnvironmentPrecomputePipeline = PipelineCacheInstance.GetOrCreateComputePipeline(
                ComputePipelineKey{ 0x53434e454e565050ull },
                environmentPipelineDesc,
                "SceneRenderer environment IBL precompute pipeline");
            if (!EnvironmentPrecomputePipeline)
                throw std::runtime_error("NVRHI failed to create environment precompute pipeline");
        }

        void CreateDefaultResources()
        {
            nvrhi::BufferDesc frameBufferDesc;
            frameBufferDesc.setByteSize(sizeof(FrameConstants))
                .setIsConstantBuffer(true)
                .setIsVolatile(true)
                .setMaxVersions(2u * std::max(FramesInFlight, 2u))
                .setInitialState(nvrhi::ResourceStates::Common)
                .setKeepInitialState(false)
                .setDebugName("SceneRenderer Volatile Frame Constants");
            FrameConstantBuffer = Device->createBuffer(frameBufferDesc);
            frameBufferDesc.setMaxVersions(20u * std::max(FramesInFlight, 2u))
                .setDebugName("SceneRenderer Local Shadow View Constants");
            LocalShadowFrameConstantBuffer = Device->createBuffer(frameBufferDesc);
            frameBufferDesc.setByteSize(sizeof(ObjectConstants))
				.setMaxVersions(c_MaxObjectConstantWritesPerFrame *
					std::max(FramesInFlight, 2u))
                .setDebugName("SceneRenderer Volatile Object/Material Constants");
            ObjectConstantBuffer = Device->createBuffer(frameBufferDesc);

            nvrhi::TextureDesc textureDesc;
            textureDesc.width = 1;
            textureDesc.height = 1;
            textureDesc.initialState = nvrhi::ResourceStates::Common;
            textureDesc.keepInitialState = false;
            textureDesc.arraySize = 6;
            textureDesc.mipLevels = 1;
            textureDesc.format = nvrhi::Format::RGBA32_FLOAT;
            textureDesc.dimension = nvrhi::TextureDimension::TextureCube;
            textureDesc.isShaderResource = true;
            textureDesc.isUAV = false;
            textureDesc.debugName = "SceneRenderer Empty Environment Cubemap";
            DefaultEnvironment = Device->createTexture(textureDesc);

            textureDesc.arraySize = 1;
            textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
            textureDesc.format = nvrhi::Format::RG32_FLOAT;
            textureDesc.debugName = "SceneRenderer Empty Environment BRDF LUT";
            DefaultBrdfLut = Device->createTexture(textureDesc);

            textureDesc.arraySize = 6;
            textureDesc.dimension = nvrhi::TextureDimension::TextureCube;
            textureDesc.format = nvrhi::Format::RGBA16_FLOAT;
            // NVRHI maps a false SRV flag to DENY_SHADER_RESOURCE; DX12 does
            // not permit that flag on a UAV-only color resource.
            textureDesc.isShaderResource = true;
            textureDesc.isUAV = true;
            textureDesc.debugName = "SceneRenderer Environment Dummy Cube UAV";
            EnvironmentDummyOutputCube = Device->createTexture(textureDesc);

            textureDesc.arraySize = 1;
            textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
            textureDesc.format = nvrhi::Format::RG16_FLOAT;
            textureDesc.debugName = "SceneRenderer Environment Dummy BRDF UAV";
            EnvironmentDummyOutputBrdf = Device->createTexture(textureDesc);

            nvrhi::BufferDesc gpuBufferDesc;
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuDrivenObjects) *
                    sizeof(ObjectConstants))
                .setStructStride(sizeof(ObjectConstants))
                .setInitialState(nvrhi::ResourceStates::Common)
                .setKeepInitialState(false)
                .setDebugName("SceneRenderer GPU Scene Objects");
            GpuObjectBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuMaterials) * sizeof(GpuMaterialData))
                .setStructStride(sizeof(GpuMaterialData))
                .setDebugName("SceneRenderer GPU Scene Materials");
            GpuMaterialBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxMaterialParameterRows) * sizeof(glm::vec4))
                .setStructStride(sizeof(glm::vec4))
                .setDebugName("SceneRenderer Material Parameter Rows");
            MaterialParameterBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(SceneBoneMatrixCapacity) * sizeof(glm::mat4))
                .setStructStride(sizeof(glm::mat4))
                .setCanHaveUAVs(false)
                .setIsDrawIndirectArgs(false)
                .setDebugName("SceneRenderer Skinned Bone Matrices");
            BoneMatrixBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc.setDebugName("SceneRenderer Previous Bone Matrices");
            PreviousBoneMatrixBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuDrivenObjects) * sizeof(GpuBoundsData))
                .setStructStride(sizeof(GpuBoundsData))
                .setDebugName("SceneRenderer GPU Scene Bounds");
            GpuBoundsBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuDrivenObjects) * sizeof(GpuDrawData))
                .setStructStride(sizeof(GpuDrawData))
                .setIsVertexBuffer(true)
                .setDebugName("SceneRenderer GPU Scene Draw Data");
            GpuDrawDataBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuDrivenObjects) * sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setIsVertexBuffer(true)
                .setDebugName("SceneRenderer GPU Visible Object Indices");
            VisibleObjectIndicesBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuDrivenBatches) *
                    sizeof(nvrhi::DrawIndexedIndirectArguments))
                .setStructStride(sizeof(nvrhi::DrawIndexedIndirectArguments))
                .setIsVertexBuffer(false)
                .setIsDrawIndirectArgs(true)
                .setDebugName("SceneRenderer GPU Indexed Indirect Arguments");
            IndirectArgumentsBuffer = Device->createBuffer(gpuBufferDesc);

            Primitive2DBufferSlots.resize(FramesInFlight);
            nvrhi::BufferDesc primitiveBufferDesc;
            primitiveBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxPrimitive2DVertices) *
                    sizeof(Primitive2DVertex))
                .setIsVertexBuffer(true)
                .setInitialState(nvrhi::ResourceStates::Common)
                .setKeepInitialState(false);
            for (uint32_t frameSlot = 0; frameSlot < FramesInFlight; ++frameSlot)
            {
                primitiveBufferDesc.setDebugName(
                    "SceneRenderer Primitive2D Vertices " + std::to_string(frameSlot));
                Primitive2DBufferSlots[frameSlot].Vertices =
                    Device->createBuffer(primitiveBufferDesc);
            }

            GpuReadbackSlots.resize(FramesInFlight);
            nvrhi::BufferDesc readbackDesc;
            readbackDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuDrivenBatches) *
                    sizeof(nvrhi::DrawIndexedIndirectArguments))
                .setCpuAccess(nvrhi::CpuAccessMode::Read)
                .setInitialState(nvrhi::ResourceStates::CopyDest)
                .setKeepInitialState(true);
            for (uint32_t frameSlot = 0; frameSlot < FramesInFlight; ++frameSlot)
            {
                readbackDesc.setDebugName(
                    "SceneRenderer GPU Culling Readback " + std::to_string(frameSlot));
                GpuReadbackSlots[frameSlot].Buffer = Device->createBuffer(readbackDesc);
            }

            if (!FrameConstantBuffer || !ObjectConstantBuffer ||
                !DefaultEnvironment || !DefaultBrdfLut ||
                !EnvironmentDummyOutputCube || !EnvironmentDummyOutputBrdf ||
                !GpuObjectBuffer || !GpuMaterialBuffer || !MaterialParameterBuffer ||
                !BoneMatrixBuffer || !PreviousBoneMatrixBuffer ||
                !GpuBoundsBuffer || !GpuDrawDataBuffer ||
                !VisibleObjectIndicesBuffer || !IndirectArgumentsBuffer ||
                std::ranges::any_of(
                    Primitive2DBufferSlots,
                    [](const Primitive2DBufferSlot& slot) { return !slot.Vertices; }) ||
                std::ranges::any_of(GpuReadbackSlots, [](const GpuReadbackSlot& slot)
                {
                    return !slot.Buffer;
                }))
                throw std::runtime_error("NVRHI failed to create SceneRenderer default resources");

            nvrhi::BindingSetDesc frameBindingDesc;
            frameBindingDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, FrameConstantBuffer))
                .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, LightGrid->GetConstantsBuffer()))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, LightGrid->GetLightBuffer()))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, LightGrid->GetClusterMaskBuffer()))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(2,
                    TextureManagerInstance.GetDefaultTexture(DefaultBindlessTexture::White).Texture));
            FrameBindingSet = PipelineCacheInstance.GetOrCreateStaticBindingSet(
                StaticBindingSetKey{ 0x53434e4652414d53ull },
                frameBindingDesc,
                FrameBindingLayout,
                "SceneRenderer frame binding set");

            frameBindingDesc.bindings[0] = nvrhi::BindingSetItem::ConstantBuffer(0, LocalShadowFrameConstantBuffer);
            LocalShadowFrameBindingSet = Device->createBindingSet(frameBindingDesc, FrameBindingLayout);
            if (!LocalShadowFrameConstantBuffer || !LocalShadowFrameBindingSet)
                throw std::runtime_error("Local shadow frame resource creation failed");

            nvrhi::BindingSetDesc objectBindingDesc;
            objectBindingDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                    0, ObjectConstantBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, GpuMaterialBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, BoneMatrixBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, MaterialParameterBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, PreviousBoneMatrixBuffer));
            ObjectBindingSet = PipelineCacheInstance.GetOrCreateStaticBindingSet(
                StaticBindingSetKey{ 0x53434e4f424a5346ull },
                objectBindingDesc,
                ObjectBindingLayout,
                "SceneRenderer object push-constant binding set");

            nvrhi::BindingSetDesc shadowPassBindingDesc;
            shadowPassBindingDesc.addItem(nvrhi::BindingSetItem::PushConstants(
                    0, sizeof(ShadowPassConstants)))
                .addItem(nvrhi::BindingSetItem::Sampler(0, ShadowSampler));
            ShadowPassBindingSet = Device->createBindingSet(
                shadowPassBindingDesc,
                ShadowPassBindingLayout);

            nvrhi::BindingSetDesc gpuObjectBindingDesc;
            gpuObjectBindingDesc
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, GpuObjectBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, GpuMaterialBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, MaterialParameterBuffer));
            GpuObjectBindingSet = Device->createBindingSet(
                gpuObjectBindingDesc,
                GpuObjectBindingLayout);

            nvrhi::BindingSetDesc cullingBindingDesc;
            cullingBindingDesc
                .addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(CullConstants)))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, GpuObjectBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, GpuBoundsBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, GpuDrawDataBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, VisibleObjectIndicesBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, IndirectArgumentsBuffer));
            GpuCullingBindingSet = Device->createBindingSet(
                cullingBindingDesc,
                GpuCullingBindingLayout);

            nvrhi::BindingSetDesc primitiveBindingDesc;
            primitiveBindingDesc.addItem(nvrhi::BindingSetItem::PushConstants(
                    0, sizeof(Primitive2DConstants)))
                .addItem(nvrhi::BindingSetItem::Sampler(0, ToneSampler));
            Primitive2DBindingSet = Device->createBindingSet(
                primitiveBindingDesc,
                Primitive2DBindingLayout);

            nvrhi::ComputePipelineDesc cullingPipelineDesc;
            cullingPipelineDesc
                .setComputeShader(GpuCullShader)
                .addBindingLayout(GpuCullingBindingLayout);
            GpuCullingPipeline = PipelineCacheInstance.GetOrCreateComputePipeline(
                ComputePipelineKey{ 0x53434e4750554350ull },
                cullingPipelineDesc,
                "SceneRenderer GPU frustum culling pipeline");

            if (!FrameBindingSet || !ObjectBindingSet || !ShadowPassBindingSet ||
                !GpuObjectBindingSet || !GpuCullingBindingSet || !GpuCullingPipeline)
                throw std::runtime_error("NVRHI failed to create SceneRenderer static binding sets");
            if (!Primitive2DBindingSet)
                throw std::runtime_error("NVRHI failed to create Primitive2D binding set");
        }

        #include "SceneRendererLod.inl"
        #include "SceneRendererGpuBatch.inl"

        MeshEntry& GetOrCreateMesh(const Ref<SubMesh>& subMesh)
        {
            const SubMesh* key = subMesh.get();
            const auto existing = MeshCache.find(key);
            if (existing != MeshCache.end())
            {
                if (!existing->second.bDynamicVertices && subMesh->GetDeformationRevision() != 0)
                {
                    existing->second.bDynamicVertices = true;
                    bGraphDirty = true;
                }
                return existing->second;
            }

            if (!subMesh ||
                (subMesh->mStaticVertices.empty() && subMesh->mSkinnedVertices.empty()) ||
                subMesh->GetIndices().empty())
            {
                throw std::invalid_argument("SceneRenderer mesh requires vertices and indices");
            }
            const bool bSkinned = !subMesh->mSkinnedVertices.empty();
            const uint64_t vertexBytes = bSkinned
                ? static_cast<uint64_t>(subMesh->mSkinnedVertices.size()) *
                    sizeof(SkinnedVertex)
                : static_cast<uint64_t>(subMesh->mStaticVertices.size()) *
                    sizeof(StaticVertex);

            nvrhi::BufferDesc vertexDesc;
            vertexDesc.setByteSize(vertexBytes)
                .setIsVertexBuffer(true)
                .setCanHaveRawViews(true)
                .setIsAccelStructBuildInput(true)
                .setInitialState(nvrhi::ResourceStates::Common)
                .setKeepInitialState(false)
                .setDebugName(bSkinned
                    ? "SceneRenderer Skinned Mesh Vertices"
                    : "SceneRenderer Static Mesh Vertices");
            MeshEntry entry;
            entry.ResidentIndices = subMesh->GetIndices();
            entry.Lods.push_back({ 0, static_cast<uint32_t>(entry.ResidentIndices.size()), 0, 0.0f });
            for (const SubMeshLod& lod : subMesh->GetLods())
            {
                const size_t firstIndex = entry.ResidentIndices.size();
                if (firstIndex + lod.Indices.size() > std::numeric_limits<uint32_t>::max())
                    throw std::length_error("Mesh resident LOD index capacity exceeded");
                entry.Lods.push_back({ static_cast<uint32_t>(firstIndex),
                    static_cast<uint32_t>(lod.Indices.size()), static_cast<uint32_t>(firstIndex / 3u),
                    lod.ObjectSpaceError });
                entry.ResidentIndices.insert(entry.ResidentIndices.end(), lod.Indices.begin(), lod.Indices.end());
            }
            nvrhi::BufferDesc indexDesc;
            indexDesc.setByteSize(
                    static_cast<uint64_t>(entry.ResidentIndices.size()) * sizeof(uint32_t))
                .setIsIndexBuffer(true)
                .setCanHaveRawViews(true)
                .setIsAccelStructBuildInput(true)
                .setInitialState(nvrhi::ResourceStates::Common)
                .setKeepInitialState(false)
                .setDebugName("SceneRenderer Static Mesh Indices");

            entry.Resource = subMesh;
            entry.DeformationRevision = subMesh->GetDeformationRevision();
            entry.bDynamicVertices = entry.DeformationRevision != 0;
            if (bSkinned)
            {
                entry.VertexBuffer = Device->createBuffer(vertexDesc);
                entry.IndexBuffer = Device->createBuffer(indexDesc);
            }
            else
            {
                entry.ArenaAllocation = GeometryArena->Allocate(static_cast<uint32_t>(subMesh->mStaticVertices.size()),
                    static_cast<uint32_t>(entry.ResidentIndices.size()), sizeof(StaticVertex));
                const auto& page = GeometryArena->GetPages()[entry.ArenaAllocation.PageIndex];
                entry.VertexBuffer = page.VertexBuffer;
                entry.IndexBuffer = page.IndexBuffer;
            }
            entry.IndexCount = static_cast<uint32_t>(subMesh->GetIndices().size());
            entry.bSkinned = bSkinned;
            if (!entry.VertexBuffer || !entry.IndexBuffer)
                throw std::runtime_error("NVRHI failed to create SceneRenderer mesh buffers");

            MeshOrder.push_back(key);
            const auto [iterator, bInserted] = MeshCache.emplace(key, std::move(entry));
            (void)bInserted;
            bGraphDirty = true;
            Stats.GraphBuildReasonMask |= SceneGraphRebuildReason::MeshAdded;
            return iterator->second;
        }

        [[nodiscard]] std::string NormalizeEnvironmentKey(std::string_view path) const
        {
            if (path.empty())
                return {};

            std::filesystem::path logicalPath = Utils::Utf8ToPath(path);
            if (logicalPath.is_absolute() && !AssetRootDirectory.empty())
            {
                const std::filesystem::path relative =
                    logicalPath.lexically_normal().lexically_relative(
                        AssetRootDirectory.lexically_normal());
                if (!relative.empty() && *relative.begin() != "..")
                    logicalPath = relative;
            }
            return Utils::PathToUtf8(logicalPath.lexically_normal());
        }

        [[nodiscard]] std::filesystem::path ResolveEnvironmentPath(
            const std::string& key) const
        {
            const std::filesystem::path path = Utils::Utf8ToPath(key);
            if (path.is_absolute())
            {
#if defined(HE_PACKAGED_GAME)
                throw std::invalid_argument(
                    "Packaged environment resources must remain inside the content roots");
#else
                return path;
#endif
            }

            const std::filesystem::path projectPath = AssetRootDirectory / path;
            std::error_code error;
            if (std::filesystem::exists(projectPath, error) && !error)
                return projectPath;
            return EngineAssetRootDirectory.empty()
                ? projectPath
                : EngineAssetRootDirectory / path;
        }

        [[nodiscard]] SceneEnvironmentPreparation GetEnvironmentPreparation(
            std::string_view path) const
        {
            const std::string key = NormalizeEnvironmentKey(path);
            if (key.empty())
            {
                return {
                    .State = SceneEnvironmentState::Ready,
                    .Path = {},
                };
            }

            const auto existing = EnvironmentCache.find(key);
            if (existing == EnvironmentCache.end())
            {
                return {
                    .State = SceneEnvironmentState::Unrequested,
                    .Path = key,
                };
            }
            return {
                .State = existing->second.State,
                .Path = existing->second.Path,
                .Error = existing->second.Error,
            };
        }

        [[nodiscard]] SceneEnvironmentPreparation PrepareEnvironment(std::string_view path)
        {
            const std::string key = NormalizeEnvironmentKey(path);
            if (key.empty())
                return GetEnvironmentPreparation({});

            auto iterator = EnvironmentCache.try_emplace(key).first;
            EnvironmentEntry& entry = iterator->second;
            if (entry.State == SceneEnvironmentState::Unrequested)
            {
                entry = {};
                entry.State = SceneEnvironmentState::Preparing;
                entry.Path = key;
                try
                {
                    (void)ResolveEnvironmentPath(key);
                    entry.SourceAsset = TextureManagerInstance.LoadTexture(TextureLoadDesc{
                        .AssetPath = key,
                        .ColorSpace = TextureColorSpace::Linear,
                        .bCreateBindlessDescriptor = false,
                        .bAllowAbsolutePath = Utils::Utf8ToPath(key).is_absolute(),
                        .bGenerateMipmaps = false,
                        .bBuildEnvironmentProposal = true,
                        .bRequireReady = false,
                        .DebugName = "SceneRenderer HDR Equirectangular: " + key,
                    });
                }
                catch (const std::exception& exception)
                {
                    entry.Error = exception.what();
                    entry.State = SceneEnvironmentState::Failed;
                }
                return GetEnvironmentPreparation(key);
            }
            if (entry.State != SceneEnvironmentState::Preparing)
                return GetEnvironmentPreparation(key);

            bool bCreateBrdfLut = false;
            uint64_t preparationSubmission = 0;
            const auto releaseSource = [&](uint64_t retireAfterSubmission)
            {
                const ManagedTextureHandle source = std::exchange(entry.SourceAsset, {});
                entry.SourceEquirectangular = nullptr;
                if (TextureManagerInstance.IsValid(source))
                    TextureManagerInstance.ReleaseTexture(source, retireAfterSubmission);
            };
            if (entry.PreparationComplete)
            {
                if (!entry.bCancelled &&
                    EnvironmentPumpFrame < entry.NotBeforePumpFrame)
                    return GetEnvironmentPreparation(key);
                if (!Device->pollEventQuery(entry.PreparationComplete))
                    return GetEnvironmentPreparation(key);
                Device->resetEventQuery(entry.PreparationComplete);
                entry.PreparationComplete = nullptr;
                entry.PreparationBindings.clear();
                entry.PreparationCommandList = nullptr;
                releaseSource(entry.PreparationSubmission);
                entry.PreparationSubmission = 0;
                if (entry.NotBeforePumpFrame != 0)
                    HE_CORE_INFO("Environment IBL GPU query complete: {} pump={} releaseAt={}",
                        key, EnvironmentPumpFrame, entry.NotBeforePumpFrame);
                if (entry.bCancelled)
                {
                    if (entry.NotBeforePumpFrame != 0)
                        HE_CORE_INFO("Environment IBL discarded after completed GPU query: {}", key);
                    entry.EnvironmentCdf = nullptr;
                    entry.EnvironmentProposal.reset();
                    entry.Environment = nullptr;
                    entry.Irradiance = nullptr;
                    entry.Prefilter = nullptr;
                    entry.State = SceneEnvironmentState::Failed;
                    return GetEnvironmentPreparation(key);
                }
                entry.State = SceneEnvironmentState::Ready;
                bGraphDirty = true;
                return GetEnvironmentPreparation(key);
            }
            if (!TextureManagerInstance.IsValid(entry.SourceAsset))
            {
                entry.Error = "Environment HDR source was released before preparation";
                entry.State = SceneEnvironmentState::Failed;
                return GetEnvironmentPreparation(key);
            }
            const TextureUploadStage sourceStage =
                TextureManagerInstance.GetUploadStage(entry.SourceAsset);
            if (sourceStage == TextureUploadStage::Failed ||
                sourceStage == TextureUploadStage::Cancelled)
            {
                entry.Error = "Environment HDR source upload failed: " + key;
                releaseSource(0);
                entry.State = SceneEnvironmentState::Failed;
                return GetEnvironmentPreparation(key);
            }
            if (sourceStage != TextureUploadStage::Ready)
                return GetEnvironmentPreparation(key);
            try
            {
                entry.SourceEquirectangular =
                    TextureManagerInstance.GetTexture(entry.SourceAsset).Texture;
                entry.EnvironmentCdf = TextureManagerInstance.GetTexture(entry.SourceAsset).EnvironmentCdf;
                entry.EnvironmentProposal = TextureManagerInstance.GetTexture(entry.SourceAsset).EnvironmentProposal;

                nvrhi::TextureDesc cubeDesc;
                cubeDesc.width = EnvironmentMapSizes::Capture;
                cubeDesc.height = EnvironmentMapSizes::Capture;
                cubeDesc.arraySize = 6;
                cubeDesc.mipLevels = std::bit_width(EnvironmentMapSizes::Capture);
                cubeDesc.format = nvrhi::Format::RGBA16_FLOAT;
                cubeDesc.dimension = nvrhi::TextureDimension::TextureCube;
                cubeDesc.isShaderResource = true;
                cubeDesc.isUAV = true;
                cubeDesc.initialState = nvrhi::ResourceStates::Common;
                cubeDesc.keepInitialState = false;
                cubeDesc.debugName = "SceneRenderer Environment Cubemap: " + key;
                entry.Environment = Device->createTexture(cubeDesc);

                cubeDesc.width = EnvironmentMapSizes::Irradiance;
                cubeDesc.height = EnvironmentMapSizes::Irradiance;
                cubeDesc.mipLevels = 1;
                cubeDesc.debugName = "SceneRenderer Irradiance Cubemap: " + key;
                entry.Irradiance = Device->createTexture(cubeDesc);

                cubeDesc.width = EnvironmentMapSizes::Prefilter;
                cubeDesc.height = EnvironmentMapSizes::Prefilter;
                cubeDesc.mipLevels = std::bit_width(EnvironmentMapSizes::Prefilter);
                cubeDesc.debugName = "SceneRenderer Prefilter Cubemap: " + key;
                entry.Prefilter = Device->createTexture(cubeDesc);

                bCreateBrdfLut = !BrdfLut;
                if (bCreateBrdfLut)
                {
                    nvrhi::TextureDesc brdfDesc;
                    brdfDesc.width = EnvironmentMapSizes::BrdfLut;
                    brdfDesc.height = EnvironmentMapSizes::BrdfLut;
                    brdfDesc.format = nvrhi::Format::RG16_FLOAT;
                    brdfDesc.dimension = nvrhi::TextureDimension::Texture2D;
                    brdfDesc.isShaderResource = true;
                    brdfDesc.isUAV = true;
                    brdfDesc.initialState = nvrhi::ResourceStates::Common;
                    brdfDesc.keepInitialState = false;
                    brdfDesc.debugName = "SceneRenderer Shared BRDF Integration LUT";
                    BrdfLut = Device->createTexture(brdfDesc);
                }

                if (!entry.SourceEquirectangular || !entry.Environment ||
                    !entry.Irradiance || !entry.Prefilter || !BrdfLut)
                {
                    throw std::runtime_error("NVRHI failed to allocate environment IBL resources");
                }

                nvrhi::CommandListHandle commandList = Device->createCommandList();
                if (!commandList)
                    throw std::runtime_error("NVRHI failed to create environment upload command list");
                commandList->open();
                const auto beginTextureTracking = [&commandList](
                    nvrhi::ITexture* texture,
                    nvrhi::ResourceStates state)
                {
                    commandList->beginTrackingTextureState(
                        texture,
                        nvrhi::AllSubresources,
                        state);
                };
                beginTextureTracking(
                    entry.SourceEquirectangular,
                    nvrhi::ResourceStates::ShaderResource);
                beginTextureTracking(entry.Environment, nvrhi::ResourceStates::Common);
                beginTextureTracking(entry.Irradiance, nvrhi::ResourceStates::Common);
                beginTextureTracking(entry.Prefilter, nvrhi::ResourceStates::Common);
                beginTextureTracking(
                    BrdfLut,
                    bCreateBrdfLut
                        ? nvrhi::ResourceStates::Common
                        : nvrhi::ResourceStates::ShaderResource);
                beginTextureTracking(
                    TextureManagerInstance.GetDefaultTexture(
                        DefaultBindlessTexture::White).Texture,
                    nvrhi::ResourceStates::ShaderResource);
                beginTextureTracking(
                    DefaultEnvironment,
                    bDefaultEnvironmentUploaded
                        ? nvrhi::ResourceStates::ShaderResource
                        : nvrhi::ResourceStates::Common);
                beginTextureTracking(
                    EnvironmentDummyOutputCube,
                    nvrhi::ResourceStates::Common);
                beginTextureTracking(
                    EnvironmentDummyOutputBrdf,
                    nvrhi::ResourceStates::Common);
                std::vector<nvrhi::BindingSetHandle> preparationBindings;
                auto dispatch = [&](uint32_t mode,
                                    uint32_t width,
                                    uint32_t height,
                                    float roughness,
                                    nvrhi::ITexture* source2d,
                                    nvrhi::ITexture* sourceCube,
                                    nvrhi::ITexture* outputCube,
                                    uint32_t outputMip,
                                    nvrhi::ITexture* outputBrdf)
                {
                    const nvrhi::TextureSubresourceSet sourceCubeSubresources =
                        mode == 4u
                        ? nvrhi::TextureSubresourceSet(outputMip - 1u, 1, 0, 6)
                        : nvrhi::AllSubresources;
                    nvrhi::BindingSetDesc bindingDesc;
                    bindingDesc
                        .addItem(nvrhi::BindingSetItem::PushConstants(
                            0, sizeof(EnvironmentPrecomputeConstants)))
                        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source2d))
                        .addItem(nvrhi::BindingSetItem::Texture_SRV(
                            1,
                            sourceCube,
                            nvrhi::Format::UNKNOWN,
                            sourceCubeSubresources,
                            nvrhi::TextureDimension::TextureCube))
                        .addItem(nvrhi::BindingSetItem::Texture_UAV(
                            0,
                            outputCube,
                            nvrhi::Format::UNKNOWN,
                            nvrhi::TextureSubresourceSet(outputMip, 1, 0, 6),
                            nvrhi::TextureDimension::Texture2DArray))
                        .addItem(nvrhi::BindingSetItem::Texture_UAV(
                            1,
                            outputBrdf,
                            nvrhi::Format::UNKNOWN,
                            nvrhi::TextureSubresourceSet(0, 1, 0, 1),
                            nvrhi::TextureDimension::Texture2D))
                        .addItem(nvrhi::BindingSetItem::Sampler(
                            0,
                            mode == 0u ? EquirectangularSampler : EnvironmentSampler));
                    nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(
                        bindingDesc,
                        EnvironmentPrecomputeBindingLayout);
                    if (!bindingSet)
                        throw std::runtime_error("NVRHI failed to create environment precompute binding set");
                    preparationBindings.push_back(bindingSet);

                    nvrhi::ComputeState state;
                    state.setPipeline(EnvironmentPrecomputePipeline).addBindingSet(bindingSet);
                    commandList->setComputeState(state);
                    const EnvironmentPrecomputeConstants constants{
                        .Mode = mode,
                        .Width = width,
                        .Height = height,
                        .SampleCount = c_EnvironmentSampleCount,
                        .Roughness = roughness,
                    };
                    commandList->setPushConstants(&constants, sizeof(constants));
                    commandList->dispatch(
                        (width + 7u) / 8u,
                        (height + 7u) / 8u,
                        mode == 3u ? 1u : 6u);
                };

                dispatch(
                    0,
                    EnvironmentMapSizes::Capture,
                    EnvironmentMapSizes::Capture,
                    0.0f,
                    entry.SourceEquirectangular,
                    DefaultEnvironment,
                    entry.Environment,
                    0,
                    EnvironmentDummyOutputBrdf);
                const uint32_t environmentMipCount = entry.Environment->getDesc().mipLevels;
                for (uint32_t mip = 1; mip < environmentMipCount; ++mip)
                {
                    const uint32_t mipWidth =
                        std::max(EnvironmentMapSizes::Capture >> mip, 1u);
                    dispatch(
                        4,
                        mipWidth,
                        mipWidth,
                        0.0f,
                        TextureManagerInstance.GetDefaultTexture(
                            DefaultBindlessTexture::White).Texture,
                        entry.Environment,
                        entry.Environment,
                        mip,
                        EnvironmentDummyOutputBrdf);
                }
                dispatch(
                    1,
                    EnvironmentMapSizes::Irradiance,
                    EnvironmentMapSizes::Irradiance,
                    0.0f,
                    TextureManagerInstance.GetDefaultTexture(
                        DefaultBindlessTexture::White).Texture,
                    entry.Environment,
                    entry.Irradiance,
                    0,
                    EnvironmentDummyOutputBrdf);

                const uint32_t prefilterMipCount = entry.Prefilter->getDesc().mipLevels;
                for (uint32_t mip = 0; mip < prefilterMipCount; ++mip)
                {
                    const uint32_t mipWidth = std::max(EnvironmentMapSizes::Prefilter >> mip, 1u);
                    const float roughness = prefilterMipCount > 1
                        ? static_cast<float>(mip) / static_cast<float>(prefilterMipCount - 1)
                        : 0.0f;
                    dispatch(
                        2,
                        mipWidth,
                        mipWidth,
                        roughness,
                        TextureManagerInstance.GetDefaultTexture(
                            DefaultBindlessTexture::White).Texture,
                        entry.Environment,
                        entry.Prefilter,
                        mip,
                        EnvironmentDummyOutputBrdf);
                }
                if (bCreateBrdfLut)
                {
                    dispatch(
                        3,
                        EnvironmentMapSizes::BrdfLut,
                        EnvironmentMapSizes::BrdfLut,
                        0.0f,
                        TextureManagerInstance.GetDefaultTexture(
                            DefaultBindlessTexture::White).Texture,
                        DefaultEnvironment,
                        EnvironmentDummyOutputCube,
                        0,
                        BrdfLut);
                }

                commandList->setTextureState(
                    entry.SourceEquirectangular,
                    nvrhi::AllSubresources,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setTextureState(
                    entry.Environment,
                    nvrhi::AllSubresources,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setTextureState(
                    entry.Irradiance,
                    nvrhi::AllSubresources,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setTextureState(
                    entry.Prefilter,
                    nvrhi::AllSubresources,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setTextureState(
                    BrdfLut,
                    nvrhi::AllSubresources,
                    nvrhi::ResourceStates::ShaderResource);
                // Restore helper resources to the state expected by the first
                // RenderGraph import (or by a later environment preparation).
                commandList->setTextureState(
                    TextureManagerInstance.GetDefaultTexture(
                        DefaultBindlessTexture::White).Texture,
                    nvrhi::AllSubresources,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setTextureState(
                    DefaultEnvironment,
                    nvrhi::AllSubresources,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setTextureState(
                    EnvironmentDummyOutputCube,
                    nvrhi::AllSubresources,
                    nvrhi::ResourceStates::UnorderedAccess);
                commandList->setTextureState(
                    EnvironmentDummyOutputBrdf,
                    nvrhi::AllSubresources,
                    nvrhi::ResourceStates::UnorderedAccess);
                commandList->commitBarriers();
                commandList->close();
                nvrhi::EventQueryHandle preparationComplete = Device->createEventQuery();
                if (!preparationComplete)
                    throw std::runtime_error("Failed to create environment IBL completion query");
                Device->resetEventQuery(preparationComplete);
                preparationSubmission = Device->executeCommandList(commandList, nvrhi::CommandQueue::Graphics);
                Device->setEventQuery(preparationComplete, nvrhi::CommandQueue::Graphics);
                entry.PreparationComplete = std::move(preparationComplete);
                entry.PreparationCommandList = std::move(commandList);
                entry.PreparationBindings = std::move(preparationBindings);
                entry.PreparationSubmission = preparationSubmission;
                const uint64_t holdFrames = EnvironmentQueryHoldFrames(key);
                entry.NotBeforePumpFrame = holdFrames == 0 ? 0 :
                    EnvironmentPumpFrame > std::numeric_limits<uint64_t>::max() - holdFrames
                    ? std::numeric_limits<uint64_t>::max()
                    : EnvironmentPumpFrame + holdFrames;
                if (holdFrames != 0)
                    HE_CORE_INFO("Environment IBL submitted: {} (hold {} preparation frames, pump={}, releaseAt={})",
                        key, holdFrames, EnvironmentPumpFrame, entry.NotBeforePumpFrame);
            }
            catch (const std::exception& exception)
            {
                if (bCreateBrdfLut) BrdfLut = nullptr;
                releaseSource(preparationSubmission);
                entry.EnvironmentCdf = nullptr;
                entry.EnvironmentProposal.reset();
                entry.Environment = nullptr;
                entry.Irradiance = nullptr;
                entry.Prefilter = nullptr;
                entry.Error = exception.what();
                entry.State = SceneEnvironmentState::Failed;
            }
            catch (...)
            {
                if (bCreateBrdfLut) BrdfLut = nullptr;
                releaseSource(preparationSubmission);
                entry.EnvironmentCdf = nullptr;
                entry.EnvironmentProposal.reset();
                entry.Environment = nullptr;
                entry.Irradiance = nullptr;
                entry.Prefilter = nullptr;
                entry.Error = "Unknown environment IBL preparation failure";
                entry.State = SceneEnvironmentState::Failed;
            }
            return GetEnvironmentPreparation(key);
        }

        void RetireObsoleteEnvironmentEntries(const RenderWorld& world)
        {
            if (EnvironmentSceneGeneration == world.SceneGeneration) return;
            EnvironmentSceneGeneration = world.SceneGeneration;
            std::unordered_set<std::string> used;
            const std::string active = NormalizeEnvironmentKey(world.Environment.HdrTexturePath);
            if (!active.empty()) used.insert(active);
            for (const RenderReflectionProbe& probe : world.ReflectionProbes)
            {
                const std::string key = NormalizeEnvironmentKey(probe.HdrTexturePath);
                if (!key.empty()) used.insert(key);
            }
            RetiredEnvironmentEntries.reserve(
                RetiredEnvironmentEntries.size() + EnvironmentCache.size());
            for (auto iterator = EnvironmentCache.begin(); iterator != EnvironmentCache.end();)
            {
                EnvironmentEntry& entry = iterator->second;
                if (entry.NotBeforePumpFrame != 0)
                    HE_CORE_INFO("Environment IBL generation audit: {} used={} state={} query={} pump={} releaseAt={}",
                        iterator->first, used.contains(iterator->first),
                        static_cast<int>(entry.State), static_cast<bool>(entry.PreparationComplete),
                        EnvironmentPumpFrame, entry.NotBeforePumpFrame);
                if (used.contains(iterator->first))
                {
                    if (!entry.bReloadRequested)
                        entry.bCancelled = false;
                    ++iterator;
                    continue;
                }
                if (entry.State == SceneEnvironmentState::Preparing &&
                    entry.PreparationComplete)
                {
                    entry.bCancelled = true;
                    if (entry.NotBeforePumpFrame != 0)
                        HE_CORE_INFO("Environment IBL cancelled before GPU query publication: {}",
                            iterator->first);
                    ++iterator;
                    continue;
                }
                if (TextureManagerInstance.IsValid(entry.SourceAsset))
                    TextureManagerInstance.ReleaseTexture(
                        entry.SourceAsset, LastSubmittedSubmission);
                entry.SourceAsset = {};
                if (entry.State == SceneEnvironmentState::Ready)
                    RetiredEnvironmentEntries.push_back({
                        std::move(entry), LastSubmittedSubmission });
                iterator = EnvironmentCache.erase(iterator);
                bGraphDirty = true;
            }
        }

        size_t InvalidateEnvironmentEntries(std::string_view path)
        {
            const std::string requested = NormalizeEnvironmentKey(path);
            const auto matches = [&requested](std::string_view key)
            {
                return requested.empty() ||
                    (key.size() == requested.size() &&
                        std::equal(key.begin(), key.end(), requested.begin(),
                            [](unsigned char left, unsigned char right)
                            { return std::tolower(left) == std::tolower(right); }));
            };
            size_t invalidated = 0;
            RetiredEnvironmentEntries.reserve(
                RetiredEnvironmentEntries.size() + EnvironmentCache.size());
            for (auto iterator = EnvironmentCache.begin(); iterator != EnvironmentCache.end();)
            {
                if (!matches(iterator->first))
                {
                    ++iterator;
                    continue;
                }
                EnvironmentEntry& entry = iterator->second;
                ++invalidated;
                if (entry.PreparationComplete)
                {
                    // Its command list still owns the HDR and derived textures.
                    // The frame pump discards them after the query completes.
                    entry.bCancelled = true;
                    entry.bReloadRequested = true;
                    ++iterator;
                    continue;
                }
                if (TextureManagerInstance.IsValid(entry.SourceAsset))
                    TextureManagerInstance.ReleaseTexture(
                        entry.SourceAsset, LastSubmittedSubmission);
                entry.SourceAsset = {};
                if (entry.State == SceneEnvironmentState::Ready)
                    RetiredEnvironmentEntries.push_back({
                        std::move(entry), LastSubmittedSubmission });
                iterator = EnvironmentCache.erase(iterator);
                bGraphDirty = true;
            }
            return invalidated;
        }

        void SelectEnvironment(const RenderEnvironment& environment)
        {
            const std::string key = NormalizeEnvironmentKey(environment.HdrTexturePath);
            nvrhi::TextureHandle irradiance = DefaultEnvironment;
            nvrhi::TextureHandle prefilter = DefaultEnvironment;
            nvrhi::TextureHandle environmentMap = DefaultEnvironment;
            nvrhi::TextureHandle environmentCdf;
            std::shared_ptr<const EnvironmentSamplingProposal> environmentProposal;
            nvrhi::TextureHandle brdf = DefaultBrdfLut;
            bool bReady = key.empty();
            if (!key.empty())
            {
                const auto existing = EnvironmentCache.find(key);
                if (existing != EnvironmentCache.end() &&
                    existing->second.State == SceneEnvironmentState::Ready)
                {
                    irradiance = existing->second.Irradiance;
                    prefilter = existing->second.Prefilter;
                    environmentMap = existing->second.Environment;
                    environmentCdf = existing->second.EnvironmentCdf;
                    environmentProposal = existing->second.EnvironmentProposal;
                    brdf = BrdfLut;
                    bReady = true;
                }
            }

            if (ActiveIrradiance != irradiance || ActivePrefilter != prefilter ||
                ActiveEnvironment != environmentMap || ActiveEnvironmentCdf != environmentCdf ||
                ActiveBrdfLut != brdf)
            {
                ActiveIrradiance = std::move(irradiance);
                ActivePrefilter = std::move(prefilter);
                ActiveEnvironment = std::move(environmentMap);
                ActiveEnvironmentCdf = std::move(environmentCdf);
                ActiveEnvironmentProposal = std::move(environmentProposal);
                ActiveBrdfLut = std::move(brdf);
                EnvironmentBindingSets.clear();
                SkyBindingSets.clear();
                bGraphDirty = true;
            }
            ActiveEnvironmentKey = key;
            Stats.bEnvironmentReady = bReady;
            Stats.EnvironmentPath = key;
            FrameData.IblSettings = glm::vec4(
                bReady ? std::max(environment.IblIntensity, 0.0f) : 0.0f,
                ActivePrefilter ? static_cast<float>(ActivePrefilter->getDesc().mipLevels - 1u) : 0.0f,
                bReady ? 1.0f : 0.0f,
                0.0f);
            const float fogDensity = environment.bFogEnabled
                ? std::max(environment.FogDensity, 0.0f) : 0.0f;
            FrameData.FogColorDensity = glm::vec4(
                glm::max(environment.FogColor, glm::vec3(0.0f)), fogDensity);
            FrameData.FogDistanceHeight = glm::vec4(
                std::max(environment.FogStartDistance, 0.0f),
                environment.FogBaseHeight,
                std::max(environment.FogHeightFalloff, 0.0f),
                environment.SkyExposureCompensation);
            FrameData.FogDirectionalColorIntensity = glm::vec4(
                glm::max(environment.FogDirectionalColor, glm::vec3(0.0f)),
                fogDensity > 0.0f ? std::max(environment.FogDirectionalIntensity, 0.0f) : 0.0f);
            FrameData.FogDirectionalParams = glm::vec4(
                std::max(environment.FogDirectionalExponent, 1.0f),
                glm::clamp(environment.SkyColorTint, glm::vec3(0.0f), glm::vec3(4.0f)));
            FrameData.SecondaryFogColorDensity = glm::vec4(
                glm::max(environment.SecondaryFogColor, glm::vec3(0.0f)),
                std::max(environment.SecondaryFogDensity, 0.0f));
            FrameData.SecondaryFogDistanceHeight = glm::vec4(
                std::max(environment.SecondaryFogStartDistance, 0.0f),
                environment.SecondaryFogBaseHeight,
                std::max(environment.SecondaryFogHeightFalloff, 0.0f), 0.0f);
        }

        [[nodiscard]] static std::string BuildMaterialTextureSignature(
            const RenderMeshSection::MaterialInstanceSnapshot& material,
            bool bPreserveAlphaCoverage,
            float alphaCoverageCutoff)
        {
            std::string signature;
            for (const auto& reference : material.LegacyTextures)
            {
                signature += reference.bEnabled ? '1' : '0';
                signature += std::to_string(reference.AssetPath.size());
                signature += ':';
                signature += reference.AssetPath;
                signature += ';';
            }
            if (bPreserveAlphaCoverage)
            {
                signature += "mask:";
                signature += std::to_string(
                    std::bit_cast<uint32_t>(alphaCoverageCutoff));
            }
            else
            {
                signature += "straight-alpha";
            }
            return signature;
        }

        void ReleaseMaterialTextureSet(
            MaterialTextureSet& textureSet,
            uint64_t retireAfterSubmission)
        {
            for (ManagedTextureHandle& handle : textureSet.Handles)
            {
                if (handle)
                    TextureManagerInstance.ReleaseTexture(handle, retireAfterSubmission);
                handle = {};
            }
        }

        [[nodiscard]] uint32_t GetOrCreateStaticLightmapDescriptor(
            const std::string& assetPath)
        {
            if (!bHeapDirectlyIndexed)
            {
                if (!bLoggedUnsupportedStaticLightmaps)
                {
                    HE_CORE_WARN("Static lightmaps require the DX12 heap-direct raster path; using dynamic lighting on this device");
                    bLoggedUnsupportedStaticLightmaps = true;
                }
                return InvalidDescriptorIndex;
            }
            auto found = StaticLightmapTextureCache.find(assetPath);
            if (found == StaticLightmapTextureCache.end())
            {
                const ManagedTextureHandle handle = TextureManagerInstance.LoadTexture(
                    TextureLoadDesc{
                        .AssetPath = assetPath,
                        .ColorSpace = TextureColorSpace::Linear,
                        .bCreateBindlessDescriptor = true,
                        .DebugName = "Static lightmap: " + assetPath,
                    });
                found = StaticLightmapTextureCache.emplace(assetPath, handle).first;
            }
            const uint32_t index = TextureManagerInstance.GetTexture(
                found->second).ShaderDescriptorIndex;
            if (index == InvalidDescriptorIndex)
                throw std::logic_error("Static lightmap has no heap-direct descriptor: " + assetPath);
            return index;
        }

        [[nodiscard]] MaterialTextureSet BuildMaterialTextureSet(
            const RenderMeshSection::MaterialInstanceSnapshot& material,
            std::string signature,
            bool bPreserveAlphaCoverage,
            float alphaCoverageCutoff)
        {
            constexpr std::array<DefaultBindlessTexture, 5> fallbacks = {
                DefaultBindlessTexture::White,
                DefaultBindlessTexture::Normal,
                DefaultBindlessTexture::White,
                DefaultBindlessTexture::White,
                DefaultBindlessTexture::White,
            };
            constexpr std::array<TextureColorSpace, 5> colorSpaces = {
                TextureColorSpace::SRgb,
                TextureColorSpace::Linear,
                TextureColorSpace::Linear,
                TextureColorSpace::Linear,
                TextureColorSpace::SRgb,
            };

            MaterialTextureSet textureSet;
            textureSet.Signature = std::move(signature);
            for (size_t index = 0; index < material.LegacyTextures.size(); ++index)
            {
                textureSet.ShaderIndices[index] = TextureManagerInstance.GetDefaultTexture(
                    fallbacks[index]).ShaderDescriptorIndex;
                const auto& reference = material.LegacyTextures[index];
                if (!reference.bEnabled || reference.AssetPath.empty())
                    continue;
                if (const ManagedTexture* defaultTexture =
                    TextureManagerInstance.FindDefaultTexture(reference.AssetPath))
                {
                    textureSet.ShaderIndices[index] = defaultTexture->ShaderDescriptorIndex;
                    continue;
                }
                try
                {
                    textureSet.Handles[index] = TextureManagerInstance.LoadTexture(
                        TextureLoadDesc{
                            .AssetPath = reference.AssetPath,
                            .ColorSpace = colorSpaces[index],
                            .PendingFallback = fallbacks[index],
							.bCreateBindlessDescriptor = bHeapDirectlyIndexed,
                            .bPreserveAlphaCoverage =
                                index == 0 && bPreserveAlphaCoverage,
                            .AlphaCoverageCutoff = alphaCoverageCutoff,
                            .bStreamable = true,
                            .FirstResidentMip = 2,
                            .DebugName = "Material texture: " + reference.AssetPath,
                        });
					if (bHeapDirectlyIndexed)
					{
						textureSet.ShaderIndices[index] = TextureManagerInstance.GetTexture(
							textureSet.Handles[index]).ShaderDescriptorIndex;
						if (textureSet.ShaderIndices[index] == InvalidDescriptorIndex)
							throw std::logic_error("Material texture has no heap-direct descriptor");
					}
                    if (index == 1)
                        textureSet.NormalEncoding = GetNormalMapEncoding(
                            TextureManagerInstance.GetTexture(textureSet.Handles[index]).Format);
                }
                catch (const std::exception& exception)
                {
                    if (textureSet.Handles[index])
                    {
                        TextureManagerInstance.ReleaseTexture(
                            textureSet.Handles[index],
                            0);
                    }
                    textureSet.Handles[index] = {};
                    const std::string failureKey =
                        std::to_string(index) + ':' + reference.AssetPath;
                    if (FailedMaterialTextures.insert(failureKey).second)
                    {
                        HE_CORE_WARN(
                            "Material texture '{}' is unavailable; using '{}' fallback: {}",
                            reference.AssetPath,
                            reference.Fallback,
                            exception.what());
                    }
                }
            }
            textureSet.TextureRevision = TextureManagerInstance.GetRevision();
            return textureSet;
        }

        [[nodiscard]] const MaterialTextureSet& GetOrCreateMaterialTextureSet(
            const Material* materialIdentity,
            const RenderMeshSection::MaterialInstanceSnapshot& material,
            bool bPreserveAlphaCoverage,
            float alphaCoverageCutoff)
        {
            std::string signature = BuildMaterialTextureSignature(
                material,
                bPreserveAlphaCoverage,
                alphaCoverageCutoff);
            if (!materialIdentity)
                throw std::invalid_argument("Material texture cache requires a stable identity");
            const auto existing = MaterialTextureCache.find(materialIdentity);
            if (existing != MaterialTextureCache.end() &&
                existing->second.Signature == signature)
            {
                existing->second.LastUsedFrame = CurrentMaterialFrame;
                if (existing->second.TextureRevision != TextureManagerInstance.GetRevision())
                {
                    if (bHeapDirectlyIndexed)
                        for (size_t index = 0; index < existing->second.Handles.size(); ++index)
                        {
                            const auto handle = existing->second.Handles[index];
                            if (handle && TextureManagerInstance.IsValid(handle))
                                existing->second.ShaderIndices[index] =
                                    TextureManagerInstance.GetTexture(handle).ShaderDescriptorIndex;
                        }
                    const ManagedTextureHandle normal = existing->second.Handles[1];
                    existing->second.NormalEncoding = normal && TextureManagerInstance.IsValid(normal)
                        ? GetNormalMapEncoding(TextureManagerInstance.GetTexture(normal).Format) : 0u;
                    existing->second.TextureRevision = TextureManagerInstance.GetRevision();
                }
                return existing->second;
            }

            MaterialTextureSet replacement = BuildMaterialTextureSet(
                material,
                std::move(signature),
                bPreserveAlphaCoverage,
                alphaCoverageCutoff);
            replacement.LastUsedFrame = CurrentMaterialFrame;
            if (existing != MaterialTextureCache.end())
            {
                ReleaseMaterialTextureSet(existing->second, LastSubmittedSubmission);
                existing->second = std::move(replacement);
                return existing->second;
            }
            return MaterialTextureCache.emplace(
                materialIdentity,
                std::move(replacement)).first->second;
        }

        void ReleaseMaterialParameterTextureSet(
            MaterialParameterTextureSet& textureSet,
            uint64_t retireAfterSubmission)
        {
            for (ManagedTextureHandle& handle : textureSet.Handles)
            {
                if (handle)
                    TextureManagerInstance.ReleaseTexture(handle, retireAfterSubmission);
                handle = {};
            }
        }

        [[nodiscard]] MaterialParameterTextureSet BuildMaterialParameterTextureSet(
            const MaterialParameterPackingResult& packing,
            RenderMaterialAlphaMode alphaMode,
            float alphaCutoff)
        {
            MaterialParameterTextureSet textureSet;
            textureSet.Signature = packing.PackedDataHash + ':' +
                std::to_string(static_cast<uint32_t>(alphaMode)) + ':' +
                std::to_string(std::bit_cast<uint32_t>(alphaCutoff));
            textureSet.Handles.resize(packing.TextureFixups.size());
            textureSet.ShaderIndices.resize(packing.TextureFixups.size());
            textureSet.NormalEncodings.resize(packing.TextureFixups.size(), 0u);
			textureSet.Fallbacks.resize(packing.TextureFixups.size());
			textureSet.RowOffsets.resize(packing.TextureFixups.size());
            for (size_t index = 0; index < packing.TextureFixups.size(); ++index)
            {
                const MaterialTextureParameterFixup& fixup = packing.TextureFixups[index];
                DefaultBindlessTexture defaultFallback = DefaultBindlessTexture::White;
                if (fixup.Semantic == MaterialShaderTextureSemantic::Normal)
                    defaultFallback = DefaultBindlessTexture::Normal;
                else if (fixup.Semantic == MaterialShaderTextureSemantic::Emissive)
                    defaultFallback = DefaultBindlessTexture::Black;
				DefaultBindlessTexture namedFallbackKind = defaultFallback;
				if (!fixup.Fallback.empty())
					(void)TryParseDefaultBindlessTexture(
						fixup.Fallback,
						namedFallbackKind);
				textureSet.Fallbacks[index] = namedFallbackKind;
				textureSet.RowOffsets[index] = fixup.RowOffset;

                const ManagedTexture* namedFallback = fixup.Fallback.empty()
                    ? nullptr
                    : TextureManagerInstance.FindDefaultTexture(fixup.Fallback);
                textureSet.ShaderIndices[index] = namedFallback
                    ? namedFallback->ShaderDescriptorIndex
                    : TextureManagerInstance.GetDefaultTexture(defaultFallback).ShaderDescriptorIndex;
                if (fixup.AssetPath.empty())
                    continue;
                if (const ManagedTexture* builtin =
                    TextureManagerInstance.FindDefaultTexture(fixup.AssetPath))
                {
                    textureSet.ShaderIndices[index] = builtin->ShaderDescriptorIndex;
                    continue;
                }

                try
                {
                    textureSet.Handles[index] = TextureManagerInstance.LoadTexture(
                        TextureLoadDesc{
                            .AssetPath = fixup.AssetPath,
                            .ColorSpace = fixup.ColorSpace == MaterialShaderColorSpace::Srgb
                                ? TextureColorSpace::SRgb
                                : TextureColorSpace::Linear,
                            .PendingFallback = fixup.Semantic == MaterialShaderTextureSemantic::Normal
                                ? DefaultBindlessTexture::Normal : DefaultBindlessTexture::White,
							.bCreateBindlessDescriptor = bHeapDirectlyIndexed,
                            .bPreserveAlphaCoverage =
                                alphaMode == RenderMaterialAlphaMode::Mask &&
                                fixup.Semantic == MaterialShaderTextureSemantic::BaseColor,
                            .AlphaCoverageCutoff = std::clamp(alphaCutoff, 0.0f, 1.0f),
                            .bStreamable = true,
                            .FirstResidentMip = 2,
                            .DebugName = "Material parameter texture: " + fixup.AssetPath,
                        });
					if (bHeapDirectlyIndexed)
					{
						textureSet.ShaderIndices[index] = TextureManagerInstance.GetTexture(
							textureSet.Handles[index]).ShaderDescriptorIndex;
						if (textureSet.ShaderIndices[index] == InvalidDescriptorIndex)
							throw std::logic_error(
								"Material parameter texture has no heap-direct descriptor");
					}
                    textureSet.NormalEncodings[index] = GetNormalMapEncoding(
                        TextureManagerInstance.GetTexture(textureSet.Handles[index]).Format);
                }
                catch (const std::exception& exception)
                {
                    if (textureSet.Handles[index])
                        TextureManagerInstance.ReleaseTexture(textureSet.Handles[index], 0);
                    textureSet.Handles[index] = {};
                    const std::string failureKey = "parameter:" + fixup.Name + ':' + fixup.AssetPath;
                    if (FailedMaterialTextures.insert(failureKey).second)
                    {
                        HE_CORE_WARN(
                            "Material parameter texture '{}' is unavailable; using '{}' fallback: {}",
                            fixup.AssetPath,
                            fixup.Fallback,
                            exception.what());
                    }
                }
            }
            textureSet.TextureRevision = TextureManagerInstance.GetRevision();
            return textureSet;
        }

        [[nodiscard]] const MaterialParameterTextureSet& GetOrCreateMaterialParameterTextureSet(
            const Material& material,
            const MaterialParameterPackingResult& packing,
            RenderMaterialAlphaMode alphaMode,
            float alphaCutoff)
        {
            const std::string signature = packing.PackedDataHash + ':' +
                std::to_string(static_cast<uint32_t>(alphaMode)) + ':' +
                std::to_string(std::bit_cast<uint32_t>(alphaCutoff));
            const auto existing = MaterialParameterTextureCache.find(&material);
            if (existing != MaterialParameterTextureCache.end() &&
                existing->second.Signature == signature)
            {
                existing->second.LastUsedFrame = CurrentMaterialFrame;
                if (existing->second.TextureRevision != TextureManagerInstance.GetRevision())
                {
                    for (size_t index = 0; index < existing->second.Handles.size(); ++index)
                    {
                        const ManagedTextureHandle handle = existing->second.Handles[index];
                        if (bHeapDirectlyIndexed && handle && TextureManagerInstance.IsValid(handle))
                            existing->second.ShaderIndices[index] =
                                TextureManagerInstance.GetTexture(handle).ShaderDescriptorIndex;
                        existing->second.NormalEncodings[index] =
                            handle && TextureManagerInstance.IsValid(handle)
                                ? GetNormalMapEncoding(TextureManagerInstance.GetTexture(handle).Format)
                                : 0u;
                    }
                    existing->second.TextureRevision = TextureManagerInstance.GetRevision();
                }
                return existing->second;
            }

            MaterialParameterTextureSet replacement = BuildMaterialParameterTextureSet(
                packing,
                alphaMode,
                alphaCutoff);
            replacement.LastUsedFrame = CurrentMaterialFrame;
            if (existing != MaterialParameterTextureCache.end())
            {
                ReleaseMaterialParameterTextureSet(
                    existing->second,
                    LastSubmittedSubmission);
                existing->second = std::move(replacement);
                return existing->second;
            }
            return MaterialParameterTextureCache.emplace(
                &material,
                std::move(replacement)).first->second;
        }

        void PopulateMaterialConstants(
            GpuMaterialData& constants,
            const RenderMeshSection::MaterialInstanceSnapshot* material,
            const Material* materialIdentity = nullptr,
            bool bLoadLegacyTextures = true,
            RenderMaterialAlphaMode alphaMode = RenderMaterialAlphaMode::Opaque,
            float alphaCutoff = 0.5f,
            bool bDoubleSided = false)
        {
            const float safeAlphaCutoff = std::isfinite(alphaCutoff)
                ? std::clamp(alphaCutoff, 0.0f, 1.0f)
                : 0.5f;
            const float baseColorAlpha = material &&
                std::isfinite(material->BaseColor.a)
                ? std::max(material->BaseColor.a, 0.0f)
                : 1.0f;
            float alphaCoverageCutoff = std::clamp(
                safeAlphaCutoff / std::max(baseColorAlpha, 1e-6f),
                0.0f,
                1.0f);
            if (alphaCoverageCutoff == 0.0f)
                alphaCoverageCutoff = 0.0f;
            const bool bPreserveAlphaCoverage =
                alphaMode == RenderMaterialAlphaMode::Mask;
            const MaterialTextureSet* textureSet = material && materialIdentity &&
                bLoadLegacyTextures
                ? &GetOrCreateMaterialTextureSet(
                    materialIdentity,
                    *material,
                    bPreserveAlphaCoverage,
                    alphaCoverageCutoff)
                : nullptr;
            const auto getIndex = [this, textureSet](
                size_t index,
                DefaultBindlessTexture fallback)
            {
                return textureSet
                    ? textureSet->ShaderIndices[index]
                    : TextureManagerInstance.GetDefaultTexture(fallback).ShaderDescriptorIndex;
            };
            constants.MaterialTextureIndices = glm::uvec4(
                getIndex(0, DefaultBindlessTexture::White),
                getIndex(1, DefaultBindlessTexture::Normal),
                getIndex(2, DefaultBindlessTexture::White),
                getIndex(3, DefaultBindlessTexture::White));
            uint32_t flags = bDoubleSided ? c_MaterialFlagDoubleSided : 0u;
            if (textureSet)
                flags |= textureSet->NormalEncoding << c_MaterialNormalEncodingShift;
            if (alphaMode == RenderMaterialAlphaMode::Mask)
                flags |= c_MaterialFlagAlphaMask;
            else if (alphaMode == RenderMaterialAlphaMode::Blend)
                flags |= c_MaterialFlagAlphaBlend;
            constants.EmissiveSamplerFlags = glm::uvec4(
                getIndex(4, DefaultBindlessTexture::White),
                TextureManagerInstance.GetMaterialSamplerIndex(),
                flags,
                std::bit_cast<uint32_t>(safeAlphaCutoff));
            if (material)
            {
                for (size_t slot = 0; slot < 5; ++slot)
                    constants.Sampling[slot] = PackTextureSampling(material->LegacyTextures[slot].Sampling);
                constants.BaseColor = material->BaseColor;
                constants.EmissiveAndMetallic = glm::vec4(
                    material->Emissive,
                    std::clamp(material->Metallic, 0.0f, 1.0f));
                constants.RoughnessAoNormalAndPadding.x =
                    std::clamp(material->Roughness, 0.04f, 1.0f);
                constants.RoughnessAoNormalAndPadding.y =
                    std::clamp(material->AoStrength, 0.0f, 1.0f);
                constants.RoughnessAoNormalAndPadding.z =
                    std::isfinite(material->NormalScale)
                        ? material->NormalScale
                        : 1.0f;
            }
            else
            {
                constants.EmissiveAndMetallic.w = 0.0f;
                constants.RoughnessAoNormalAndPadding.x = 0.7f;
            }
        }

        [[nodiscard]] uint32_t GetOrCreateGpuMaterialIndex(
            const RenderMeshSection& section)
        {
            if (!section.MaterialResource)
                return 0;
            const MaterialRegistryKey key = section.MaterialGpuIndex != InvalidGpuSceneIndex
                ? MaterialRegistryKey{
                    .SnapshotIndex = section.MaterialGpuIndex,
                    .FallbackResource = nullptr,
                }
                : MaterialRegistryKey{
                    .SnapshotIndex = InvalidGpuSceneIndex,
                    .FallbackResource = section.MaterialResource.get(),
                };
            const auto [iterator, bInserted] = GpuMaterialRegistry.try_emplace(
                key,
                NextGpuMaterialIndex);
            if (bInserted)
            {
                if (NextGpuMaterialIndex >= c_MaxGpuMaterials)
                {
                    GpuMaterialRegistry.erase(iterator);
                    throw std::length_error("SceneRenderer GPU material capacity exceeded");
                }
                ++NextGpuMaterialIndex;
            }
            return iterator->second;
        }

		[[nodiscard]] nvrhi::TextureHandle ResolveMaterialTexture(
			ManagedTextureHandle handle,
			DefaultBindlessTexture fallback) const
		{
			return handle
				? TextureManagerInstance.GetTexture(handle).Texture
				: TextureManagerInstance.GetDefaultTexture(fallback).Texture;
		}

		void CollectRetiredMaterialDescriptorPages()
		{
			std::erase_if(RetiredMaterialPages,
				[this](const RetiredMaterialDescriptorPages& retired)
				{
					return retired.Submission <= CurrentCompletedSubmission;
				});
		}

		void BuildBaselineMaterialDescriptorPages(
			std::span<const FrameMaterialDescriptorInput> frameInputs)
		{
			if (bHeapDirectlyIndexed)
			{
				MaterialPageByMaterialIndex.clear();
				Stats.MaterialDescriptorPageCount = 0;
				Stats.MaterialDescriptorPeakPageTextures = 0;
				Stats.MaterialBindingSetCreationCount = 0;
				return;
			}

			CollectRetiredMaterialDescriptorPages();
			const std::array<nvrhi::TextureHandle, 3> reservedTextures = {
				TextureManagerInstance.GetDefaultTexture(
					DefaultBindlessTexture::White).Texture,
				TextureManagerInstance.GetDefaultTexture(
					DefaultBindlessTexture::Black).Texture,
				TextureManagerInstance.GetDefaultTexture(
					DefaultBindlessTexture::Normal).Texture,
			};
			std::array<MaterialDescriptorTextureKey, 3> reservedKeys{};
			std::unordered_map<MaterialDescriptorTextureKey, nvrhi::TextureHandle>
				texturesByKey;
			const auto registerTexture = [&texturesByKey](
				const nvrhi::TextureHandle& texture)
			{
				if (!texture)
					throw std::invalid_argument(
						"Material descriptor paging received a null texture.");
				const MaterialDescriptorTextureKey key =
					static_cast<MaterialDescriptorTextureKey>(
						reinterpret_cast<std::uintptr_t>(texture.Get()));
				texturesByKey.try_emplace(key, texture);
				return key;
			};
			for (size_t index = 0; index < reservedTextures.size(); ++index)
				reservedKeys[index] = registerTexture(reservedTextures[index]);

			std::vector<MaterialDescriptorPageInput> pagingInputs;
			pagingInputs.reserve(frameInputs.size());
			for (const FrameMaterialDescriptorInput& input : frameInputs)
			{
				MaterialDescriptorPageInput pagingInput{
					.MaterialIndex = input.MaterialIndex,
				};
				pagingInput.TextureKeys.reserve(input.Textures.size());
				for (const nvrhi::TextureHandle& texture : input.Textures)
					pagingInput.TextureKeys.push_back(registerTexture(texture));
				pagingInputs.push_back(std::move(pagingInput));
			}

			const MaterialDescriptorPagingResult paging =
				BuildMaterialDescriptorPages(
					pagingInputs,
					reservedKeys,
					BaselineMaterialTextureDescriptorCount);
			MaterialPageByMaterialIndex.assign(
				GpuMaterialDataCpu.size(),
				0u);
			for (size_t assignmentIndex = 0;
				assignmentIndex < paging.Assignments.size();
				++assignmentIndex)
			{
				const MaterialDescriptorPageAssignment& assignment =
					paging.Assignments[assignmentIndex];
				if (assignment.MaterialIndex >= GpuMaterialDataCpu.size() ||
					assignment.MaterialIndex >= MaterialPageByMaterialIndex.size())
				{
					throw std::logic_error(
						"Material descriptor paging produced an invalid material index.");
				}
				MaterialPageByMaterialIndex[assignment.MaterialIndex] =
					assignment.PageIndex;
				const FrameMaterialDescriptorInput& input =
					frameInputs[assignmentIndex];
				if (input.bCustomParameters)
				{
					if (input.ParameterRowOffsets.size() !=
						assignment.LocalTextureIndices.size())
					{
						throw std::logic_error(
							"Custom material descriptor rows do not match their textures.");
					}
					for (size_t textureIndex = 0;
						textureIndex < input.ParameterRowOffsets.size();
						++textureIndex)
					{
						const uint32_t rowOffset =
							input.ParameterRowOffsets[textureIndex];
						if (rowOffset >= MaterialParameterRowsCpu.size())
							throw std::logic_error(
								"Custom material descriptor row escaped the parameter buffer.");
						MaterialParameterRowsCpu[rowOffset][0] =
							std::bit_cast<float>(
								assignment.LocalTextureIndices[textureIndex]);
					}
				}
				else
				{
					if (assignment.LocalTextureIndices.size() != 5)
						throw std::logic_error(
							"Legacy material descriptor input must contain five textures.");
					GpuMaterialData& material =
						GpuMaterialDataCpu[assignment.MaterialIndex];
					material.MaterialTextureIndices = glm::uvec4(
						assignment.LocalTextureIndices[0],
						assignment.LocalTextureIndices[1],
						assignment.LocalTextureIndices[2],
						assignment.LocalTextureIndices[3]);
					material.EmissiveSamplerFlags.x =
						assignment.LocalTextureIndices[4];
				}
			}

			bool bPagesChanged =
				MaterialPages.size() != paging.Pages.size();
			if (!bPagesChanged)
			{
				for (size_t pageIndex = 0;
					pageIndex < paging.Pages.size();
					++pageIndex)
				{
					if (MaterialPages[pageIndex].TextureKeys !=
						paging.Pages[pageIndex])
					{
						bPagesChanged = true;
						break;
					}
				}
			}

			Stats.MaterialBindingSetCreationCount = 0;
			if (bPagesChanged)
			{
				if (!MaterialPages.empty())
				{
					RetiredMaterialPages.push_back({
						.Submission = LastSubmittedSubmission,
						.Pages = std::move(MaterialPages),
					});
				}
				MaterialPages.clear();
				MaterialPages.reserve(paging.Pages.size());
				for (const std::vector<MaterialDescriptorTextureKey>& pageKeys :
					paging.Pages)
				{
					MaterialDescriptorPageState page;
					page.TextureKeys = pageKeys;
					page.Textures.reserve(pageKeys.size());
					nvrhi::BindingSetDesc bindingSetDesc;
					bindingSetDesc.bindings.reserve(
						BaselineMaterialTextureDescriptorCount);
					for (uint32_t localIndex = 0;
						localIndex < BaselineMaterialTextureDescriptorCount;
						++localIndex)
					{
						const nvrhi::TextureHandle texture =
							localIndex < pageKeys.size()
							? texturesByKey.at(pageKeys[localIndex])
							: reservedTextures[0];
						if (localIndex < pageKeys.size())
							page.Textures.push_back(texture);
						bindingSetDesc.addItem(
							nvrhi::BindingSetItem::Texture_SRV(
								0,
								texture).setArrayElement(localIndex));
					}
					page.BindingSet = Device->createBindingSet(
						bindingSetDesc,
						TextureManagerInstance.GetTextureBindingLayout());
					if (!page.BindingSet)
						throw std::runtime_error(
							"NVRHI failed to create a baseline material descriptor page.");
					MaterialPages.push_back(std::move(page));
				}
				Stats.MaterialBindingSetCreationCount =
					static_cast<uint32_t>(MaterialPages.size());
				bGraphDirty = true;
			}
			Stats.MaterialDescriptorPageCount =
				static_cast<uint32_t>(MaterialPages.size());
			Stats.MaterialDescriptorPeakPageTextures =
				paging.PeakPageTextureCount;
		}

		[[nodiscard]] nvrhi::IBindingSet* GetMaterialTextureBindingSet(
			uint32_t materialIndex) const
		{
			if (bHeapDirectlyIndexed)
				return TextureManagerInstance.GetTextureBindingSet();
			if (materialIndex >= MaterialPageByMaterialIndex.size())
				throw std::out_of_range(
					"Material index has no baseline descriptor-page assignment.");
			const uint32_t pageIndex =
				MaterialPageByMaterialIndex[materialIndex];
			if (pageIndex >= MaterialPages.size() ||
				!MaterialPages[pageIndex].BindingSet)
			{
				throw std::out_of_range(
					"Material descriptor-page assignment is invalid.");
			}
			return MaterialPages[pageIndex].BindingSet;
		}

        [[nodiscard]] uint32_t ResolvePrimitiveTexture(
            const RenderPrimitive2D& primitive,
            std::unordered_set<std::string>& usedTextures)
        {
            DefaultBindlessTexture fallback = DefaultBindlessTexture::White;
            switch (primitive.TextureFallback)
            {
                case RenderPrimitiveTextureFallback::White:
                    fallback = DefaultBindlessTexture::White;
                    break;
                case RenderPrimitiveTextureFallback::Black:
                    fallback = DefaultBindlessTexture::Black;
                    break;
                case RenderPrimitiveTextureFallback::FlatNormal:
                    fallback = DefaultBindlessTexture::Normal;
                    break;
            }
            if (primitive.TextureAssetPath.empty())
            {
                return TextureManagerInstance.GetDefaultTexture(
                    fallback).ShaderDescriptorIndex;
            }
            if (const ManagedTexture* defaultTexture =
                TextureManagerInstance.FindDefaultTexture(primitive.TextureAssetPath))
            {
                return defaultTexture->ShaderDescriptorIndex;
            }

            usedTextures.insert(primitive.TextureAssetPath);
            try
            {
                auto existing = PrimitiveTextureCache.find(primitive.TextureAssetPath);
                if (existing == PrimitiveTextureCache.end())
                {
                    const ManagedTextureHandle handle = TextureManagerInstance.LoadTexture(
                        TextureLoadDesc{
                            .AssetPath = primitive.TextureAssetPath,
                            .ColorSpace = TextureColorSpace::SRgb,
                            .bCreateBindlessDescriptor = true,
                            .DebugName = "Primitive2D texture: " + primitive.TextureAssetPath,
                        });
                    existing = PrimitiveTextureCache.emplace(
                        primitive.TextureAssetPath,
                        handle).first;
                }
                return TextureManagerInstance.GetTexture(
                    existing->second).ShaderDescriptorIndex;
            }
            catch (const std::exception& exception)
            {
                const std::string failureKey = "primitive:" + primitive.TextureAssetPath;
                if (FailedMaterialTextures.insert(failureKey).second)
                {
                    HE_CORE_WARN(
                        "Primitive2D texture '{}' is unavailable; using default: {}",
                        primitive.TextureAssetPath,
                        exception.what());
                }
                return TextureManagerInstance.GetDefaultTexture(
                    fallback).ShaderDescriptorIndex;
            }
        }

        void PreparePrimitive2DData(const RenderWorld& world)
        {
            Primitive2DVertices.clear();
            Primitive2DBatches.clear();
            std::vector<const RenderPrimitive2D*> sorted;
            sorted.reserve(world.Primitives2D.size());
            for (const RenderPrimitive2D& primitive : world.Primitives2D)
                sorted.push_back(&primitive);
            std::stable_sort(sorted.begin(), sorted.end(), [&world](const auto* left, const auto* right)
            {
                const auto leftKey = std::tie(left->Category, left->DepthMode, left->Layer);
                const auto rightKey = std::tie(right->Category, right->DepthMode, right->Layer);
                if (leftKey != rightKey) return leftKey < rightKey;
                // Scene sprites blend without depth writes. Submit far-to-near
                // within a layer so overlap does not depend on ECS storage order.
                if (left->Category == RenderPrimitive2DCategory::Scene &&
                    left->DepthMode == RenderPrimitiveDepthMode::DepthTest)
                {
                    const auto depth = [&world](const RenderPrimitive2D* item) {
                        const glm::vec4 center = item->Kind == RenderPrimitive2DKind::Line
                            ? glm::vec4((item->LineStart + item->LineEnd) * 0.5f, 1.0f)
                            : item->Transform[3];
                        const float value = (world.Camera.View * center).z;
                        return std::isfinite(value) ? value : 0.0f;
                    };
                    return depth(left) < depth(right);
                }
                return false;
            });

            std::unordered_set<std::string> usedTextures;
            constexpr std::array<glm::vec2, 6> corners = {{
                { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f },
                { -0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f },
            }};
            constexpr std::array<glm::vec2, 6> uvs = {{
                { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f },
                { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f },
            }};
            constexpr std::array<glm::vec2, 6> lineCorners = {{
                { 0.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f },
                { 0.0f, -1.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f },
            }};

            for (const RenderPrimitive2D* primitive : sorted)
            {
                if (Primitive2DVertices.size() + 6 > c_MaxPrimitive2DVertices)
                    throw std::length_error("SceneRenderer Primitive2D vertex capacity exceeded");
                if (Primitive2DBatches.empty() ||
                    Primitive2DBatches.back().DepthMode != primitive->DepthMode)
                {
                    Primitive2DBatches.push_back(Primitive2DBatch{
                        .DepthMode = primitive->DepthMode,
                        .FirstVertex = static_cast<uint32_t>(Primitive2DVertices.size()),
                    });
                }
                const uint32_t textureIndex = ResolvePrimitiveTexture(
                    *primitive,
                    usedTextures);
                for (size_t vertexIndex = 0; vertexIndex < corners.size(); ++vertexIndex)
                {
                    Primitive2DVertex vertex;
                    vertex.Color = primitive->Color;
                    vertex.TextureIndex = textureIndex;
                    vertex.Kind = static_cast<uint32_t>(primitive->Kind);
                    vertex.Tiling = std::max(primitive->TilingFactor, 0.0f);
                    vertex.Thickness = std::clamp(primitive->Thickness, 0.0f, 1.0f);
                    vertex.Fade = std::max(primitive->Fade, 0.0f);
                    vertex.LineWidth = std::max(primitive->LineWidth, 1.0f);
                    if (primitive->Kind == RenderPrimitive2DKind::Line)
                    {
                        vertex.Position = primitive->LineStart;
                        vertex.LineEnd = primitive->LineEnd;
                        vertex.LocalCoord = lineCorners[vertexIndex];
                        vertex.TexCoord = uvs[vertexIndex];
                    }
                    else
                    {
                        const glm::vec2 corner = corners[vertexIndex];
                        vertex.Position = glm::vec3(
                            primitive->Transform * glm::vec4(corner, 0.0f, 1.0f));
                        vertex.TexCoord = glm::mix(
                            primitive->UvMin,
                            primitive->UvMax,
                            uvs[vertexIndex]);
                        vertex.LocalCoord = corner * 2.0f;
                    }
                    Primitive2DVertices.push_back(vertex);
                    ++Primitive2DBatches.back().VertexCount;
                }
            }

            for (auto iterator = PrimitiveTextureCache.begin();
                iterator != PrimitiveTextureCache.end();)
            {
                if (usedTextures.contains(iterator->first))
                {
                    ++iterator;
                    continue;
                }
                TextureManagerInstance.ReleaseTexture(
                    iterator->second,
                    LastSubmittedSubmission);
                iterator = PrimitiveTextureCache.erase(iterator);
            }
            Stats.Primitive2DCount = static_cast<uint32_t>(sorted.size());
            Stats.Primitive2DBatchCount = static_cast<uint32_t>(Primitive2DBatches.size());
            Stats.Primitive2DVertexCount = static_cast<uint32_t>(Primitive2DVertices.size());
        }

        MaterialShaderRefreshResult RefreshMaterialShaderAtFrameBoundary(
            std::string_view sourcePath)
        {
            MaterialShaderRefreshResult refresh =
                MaterialShaderManagerInstance.RefreshIfChangedAtFrameBoundary(
                    sourcePath,
                    [this]() { return Device->waitForIdle(); });
            if (!refresh.bPublished || !refresh.Program ||
                refresh.Program->GetGeneration() <= 1)
            {
                return refresh;
            }

            // The manager invoked the idle barrier before switching Current, so
            // failure cannot expose a new program or reach either retirement.
            PipelineCacheInstance.Clear();
            ObservedMaterialShaderPublicationRevision =
                MaterialShaderManagerInstance.GetPublicationRevision();
            MaterialShaderHandleManagerInstance.RemoveSourceExcept(
                refresh.Program->GetIdentity().SourcePath,
                refresh.Program->GetShaderCacheKeys());
            return refresh;
        }

        void PrepareMaterialPublicationHistory(const SceneRendererFrameDesc& frame)
        {
            // A refresh can publish while PrepareFrameData resolves a material.
            // Observe the final revision after that work, independently per view.
            PendingMaterialShaderPublicationRevision =
                MaterialShaderManagerInstance.GetPublicationRevision();
            if (LastSubmittedSubmission == 0 ||
                PendingMaterialShaderPublicationRevision ==
                    SubmittedMaterialShaderPublicationRevision)
                return;

            SceneAntiAliasing.ResetHistory();
            UpscalingPass->ResetHistory();
            // Consume the reset for this frame, whose jittered camera and motion
            // constants have already been prepared. No geometry motion is lost.
            SceneAntiAliasing.PrepareFrame(AaSelection.Effective,
                IsSpatialPostAntiAliasingActive() ? LastOutputWidth : frame.Width,
                IsSpatialPostAntiAliasingActive() ? LastOutputHeight : frame.Height, frame.CompletedSubmission, false);
            Features->ResetHistory();
            if (auto* technique = GetActiveIlluminationTechnique())
                technique->ResetHistory(GlobalIlluminationHistoryResetReason::Material);
        }

        void PrepareFrameData(const RenderWorld& world)
        {
            HE_PROFILE;
            struct PreparationTimer
            {
                double& Elapsed;
                std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
                ~PreparationTimer()
                { Elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count(); }
            } preparationTimer{ Stats.CpuFramePreparationMilliseconds };
            static const bool bTracePreparation = [] {
                const char* setting = std::getenv("HE_FRAME_PREPARATION_TIMINGS");
                return setting && setting[0] == '1' && setting[1] == '\0';
            }();
            auto stageStart = preparationTimer.Start;
            const auto checkpoint = [&]() {
                if (!bTracePreparation) return 0.0;
                const auto now = std::chrono::steady_clock::now();
                const double elapsed = std::chrono::duration<double, std::milli>(now - stageStart).count();
                stageStart = now;
                return elapsed;
            };
            if (EnvironmentPumpFrame != std::numeric_limits<uint64_t>::max())
                ++EnvironmentPumpFrame;
            CurrentMaterialFrame = world.FrameNumber;
            TextureManagerInstance.BeginStreamingFrame();
            Stats.bMaterialPackingCacheEnabled = bMaterialPackingCacheEnabled;
			const std::uint64_t publicationRevision =
				MaterialShaderManagerInstance.GetPublicationRevision();
			if (publicationRevision != ObservedMaterialShaderPublicationRevision)
			{
				PipelineCacheInstance.Clear();
				ObservedMaterialShaderPublicationRevision = publicationRevision;
			}
            TextureManagerInstance.Collect(
                CurrentCompletedSubmission, LastSubmittedSubmission);
            const double textureCollectMs = checkpoint();
            RetireObsoleteEnvironmentEntries(world);
            std::erase_if(RetiredEnvironmentEntries,
                [this](const RetiredEnvironmentEntry& entry)
                { return entry.RetireAfterSubmission <= CurrentCompletedSubmission; });
            for (auto iterator = EnvironmentCache.begin(); iterator != EnvironmentCache.end();)
            {
                if (iterator->second.State == SceneEnvironmentState::Preparing)
                    (void)PrepareEnvironment(iterator->first);
                if (iterator->second.bCancelled &&
                    iterator->second.State != SceneEnvironmentState::Preparing)
                    iterator = EnvironmentCache.erase(iterator);
                else
                    ++iterator;
            }
            const double environmentMs = checkpoint();
            CpuDrawItems.clear();
            TransparentDrawItems.clear();
            SkinnedDrawItems.clear();
            BoneMatricesCpu.clear();
            PreviousBoneMatricesCpu.clear();
            Stats.SkinnedNonIdentityBoneMatrixCount = 0;
            ShadowDrawItems.clear();
            OutlineDrawItems.clear();
            GpuObjectData.clear();
            GpuBoundsDataCpu.clear();
            GpuDrawDataCpu.clear();
            GpuIndirectArgumentsCpu.clear();
            GpuBatches.clear();
            GpuBatchObjectIndices.clear();
			CollectRetiredMaterialDescriptorPages();
            Primitive2DVertices.clear();
            Primitive2DBatches.clear();
            GpuMaterialDataCpu.clear();
            MaterialParameterRowsCpu.clear();
            if (GpuSceneGeneration != world.SceneGeneration)
            {
                for (auto& [material, set] : MaterialTextureCache)
                    ReleaseMaterialTextureSet(set, LastSubmittedSubmission);
                MaterialTextureCache.clear();
                for (auto& [material, set] : MaterialParameterTextureCache)
                    ReleaseMaterialParameterTextureSet(set, LastSubmittedSubmission);
                MaterialParameterTextureCache.clear();
                for (auto& [path, handle] : StaticLightmapTextureCache)
                {
                    (void)path;
                    TextureManagerInstance.ReleaseTexture(handle, LastSubmittedSubmission);
                }
                StaticLightmapTextureCache.clear();
                GpuMaterialRegistry.clear();
                MaterialPackingCache.Clear();
                NextGpuMaterialIndex = 1;
                GpuSceneGeneration = world.SceneGeneration;
                MeshLodHistoryBySection.clear();
                UploadedSceneTables.clear();
                if (bHeapDirectlyIndexed)
                {
                    constexpr size_t MaxPreloadedStaticLightmapAtlases = 256;
                    if (world.StaticLightmapAssets.size() <= MaxPreloadedStaticLightmapAtlases)
                    {
                        for (const std::string& assetPath : world.StaticLightmapAssets)
                            (void)GetOrCreateStaticLightmapDescriptor(assetPath);
                    }
                    else
                    {
                        HE_CORE_WARN("Scene has {} static lightmap atlases; prewarm limit is {}, loading visible atlases on demand",
                            world.StaticLightmapAssets.size(), MaxPreloadedStaticLightmapAtlases);
                    }
                }
            }
            if (FailedMaterialPrograms.size() > 256)
                FailedMaterialPrograms.clear();
            GpuMaterialDataCpu.resize(1);
            PopulateMaterialConstants(GpuMaterialDataCpu.front(), nullptr);
            std::unordered_map<GpuBatchKey, uint32_t, GpuBatchKeyHash> batchLookup;
            std::vector<uint8_t> mainAabbRejectFlags;
            Stats.MainAabbTestedCandidates = 0;
            Stats.MainAabbRejectedCandidates = 0;
            Stats.MainAabbRejectedCandidateTriangles = 0;

            std::unordered_set<const Material*> frameMaterials;
            std::unordered_set<const Material*> frameLegacyTextureMaterials;
            std::unordered_set<const Material*> frameCustomParameterMaterials;
            std::unordered_set<uint32_t> frameGpuMaterials;
            std::unordered_set<uint32_t> framePreparedMaterials;
            std::unordered_map<uint32_t, std::shared_ptr<const CompiledMaterialShader>>
                frameMaterialPrograms;
            std::unordered_map<uint32_t, glm::uvec4> frameMaterialProgramHeaders;
			std::vector<FrameMaterialDescriptorInput>
				frameMaterialDescriptorInputs(1);
			frameMaterialDescriptorInputs[0] = {
				.MaterialIndex = 0,
				.Textures = {
					TextureManagerInstance.GetDefaultTexture(
						DefaultBindlessTexture::White).Texture,
					TextureManagerInstance.GetDefaultTexture(
						DefaultBindlessTexture::Normal).Texture,
					TextureManagerInstance.GetDefaultTexture(
						DefaultBindlessTexture::White).Texture,
					TextureManagerInstance.GetDefaultTexture(
						DefaultBindlessTexture::White).Texture,
					TextureManagerInstance.GetDefaultTexture(
						DefaultBindlessTexture::White).Texture,
				},
			};
            FrameData = {};
            FrameData.BakedIrradiance = PrepareBakedIrradiance(world.BakedIrradianceVolumes, world.Camera.Position);
            FrameData.MaterialTimeAndPadding.x = std::isfinite(world.TimeSeconds)
                ? static_cast<float>(std::clamp(
                    world.TimeSeconds,
                    0.0,
                    static_cast<double>(std::numeric_limits<float>::max())))
                : 0.0f;
            FrameData.MaterialTimeAndPadding.y =
                !world.Camera.bCameraCut && MaterialTimeSeconds >= 0.0 &&
                MaterialTimeSeconds <= world.TimeSeconds
                    ? static_cast<float>(std::clamp(
                        MaterialTimeSeconds,
                        0.0,
                        static_cast<double>(std::numeric_limits<float>::max())))
                    : FrameData.MaterialTimeAndPadding.x;
            MaterialTimeSeconds = world.TimeSeconds;
            FrameData.ViewProjection = CurrentRasterCamera.ViewProjection;
            FrameData.PreviousViewProjection = CurrentRasterCamera.PreviousViewProjection;
            FrameData.PreviousView = bRasterCameraHistoryReset ? world.Camera.View : PreviousRasterView;
            FrameData.InverseViewProjection = glm::inverse(
                CurrentRasterCamera.ViewProjection);
            FrameData.ShadowCameraView = world.Camera.View;
            SelectEnvironment(world.Environment);
            Exposure = std::max(world.PostProcess.Exposure, 0.0f);
            FrameData.CameraPositionAndExposure = glm::vec4(world.Camera.Position, Exposure);
            FrameData.DirectionalShadowCascadeSettings = glm::vec4(
                static_cast<float>(DirectionalShadowCascadeCount),
                static_cast<float>(DirectionalShadowMapSize),
                c_DirectionalShadowCascadeBlendFraction,
                c_DirectionalShadowFarFadeFraction);
            FrameData.DirectionalShadowDebugSettings = glm::vec4(
                bDirectionalShadowCascadeDebug ? 1.0f : 0.0f,
                static_cast<float>(CurrentPbrDebugMode),
                0.0f,
                DirectionalShadowMath::SanitizeNearPlane(world.Camera.NearPlane));
            bDirectionalShadowEnabled = false;

            if (!world.DirectionalLights.empty())
            {
                const RenderDirectionalLight& light = world.DirectionalLights.front();
                const uint32_t cascadeCount = static_cast<uint32_t>(
                    std::clamp(light.CascadeCount, 1,
                        static_cast<int>(c_MaxDirectionalShadowCascades)));
                const uint32_t shadowMapSize = static_cast<uint32_t>(
                    std::clamp(light.ShadowMapSize, 256, 8192));
                if (cascadeCount != DirectionalShadowCascadeCount ||
                    shadowMapSize != DirectionalShadowMapSize)
                {
                    ClearDirectionalShadowResourceCaches();
                    DirectionalShadowCascadeCount = cascadeCount;
                    DirectionalShadowMapSize = shadowMapSize;
                    bGraphDirty = true;
                }
                FrameData.DirectionalShadowCascadeSettings = glm::vec4(
                    static_cast<float>(DirectionalShadowCascadeCount),
                    static_cast<float>(DirectionalShadowMapSize),
                    c_DirectionalShadowCascadeBlendFraction,
                    c_DirectionalShadowFarFadeFraction);
                FrameData.DirectionalDirectionAndIntensity =
                    glm::vec4(light.Direction, std::max(light.Intensity, 0.0f));
                FrameData.DirectionalColor = glm::vec4(light.Color, 1.0f);
                const float shadowStrength = std::clamp(light.ShadowStrength, 0.0f, 1.0f);
                if (bShadowsEnabled && light.bCastShadows && light.Intensity > 0.0f && shadowStrength > 0.0f)
                {
                    bDirectionalShadowEnabled = true;
                    const DirectionalShadowData shadowData = BuildDirectionalShadowData(
                        world,
                        light,
                        DirectionalShadowCascadeCount,
                        DirectionalShadowMapSize);
                    FrameData.LightViewProjection = shadowData.LightViewProjections[0];
                    for (uint32_t cascade = 1;
                        cascade < c_MaxDirectionalShadowCascades;
                        ++cascade)
                    {
                        FrameData.AdditionalLightViewProjections[cascade - 1u] =
                            shadowData.LightViewProjections[cascade];
                    }
                    FrameData.DirectionalShadowCascadeSplits = shadowData.CascadeSplits;
                    FrameData.ShadowSettings = glm::vec4(
                        shadowStrength,
                        std::max(light.DepthBias, 0.0f),
                        std::max(light.NormalBias, 0.0f),
                        light.bUsePcf ? std::clamp(light.PcfRadius, 0.0f, 4.0f) : 0.0f);
                }
            }
            // Negative w encodes continuous comparison PCF: radius = -w - 1.
            // Also publish the mode when only local shadow lights are active.
            if (bContinuousPcfEnabled)
                FrameData.ShadowSettings.w = -(
                    (bDirectionalShadowEnabled ? FrameData.ShadowSettings.w : 0.0f) + 1.0f);
            if (!world.PointLights.empty())
            {
                const RenderPointLight& light = world.PointLights.front();
                FrameData.PointPositionAndIntensity =
                    glm::vec4(light.Position, std::max(light.Intensity, 0.0f));
                FrameData.PointColorAndRadius =
                    glm::vec4(light.Color, std::max(light.Radius, 0.0f));
                FrameData.DirectionalShadowDebugSettings.z = 0.0f;
            }

            Stats.bCpuCounterFrameValid = true;
            Stats.CpuCounterWorldFrame = world.FrameNumber;
            Stats.InputRenderObjects = world.Objects.size();
            Stats.PreparedRasterSections = 0;
            Stats.SectionsWithAuthoredLods = 0;
            Stats.SelectedLodSections.fill(0);
            Stats.BaseRasterTriangleCount = 0;
            Stats.SelectedRasterTriangleCount = 0;
            Stats.ReducedLodSectionCount = 0;
            const double frameSetupMs = checkpoint();
            for (size_t objectIndex = 0;
                objectIndex < world.Objects.size() + world.ResourceWarmupObjects.size(); ++objectIndex)
            {
                const bool bResourceWarmup = objectIndex >= world.Objects.size();
                const RenderObject& object = bResourceWarmup
                    ? *world.ResourceWarmupObjects[objectIndex - world.Objects.size()] : world.Objects[objectIndex];
                if (!object.bVisible)
                    continue;

                uint32_t bonePaletteOffset = 0;
                const RenderObject* previousObject = bResourceWarmup ? nullptr : MotionHistory.Previous(
                    world, object, bRasterCameraHistoryReset);
                const bool bObjectSkinned = object.Path == RenderPath::CpuSkinned ||
                    object.Path == RenderPath::CpuSkinnedTransparent;
                if (bObjectSkinned)
                {
                    bonePaletteOffset = AppendSceneBonePalette(object.BonePalette,
                        previousObject ? std::span<const glm::mat4>(previousObject->BonePalette)
                                       : std::span<const glm::mat4>{},
                        BoneMatricesCpu, PreviousBoneMatricesCpu);
                    for (const auto& boneMatrix : object.BonePalette)
                    {
                        if (!IsIdentityMatrix(boneMatrix))
                            ++Stats.SkinnedNonIdentityBoneMatrixCount;
                    }
                }

                glm::mat4 inverseTranspose(1.0f);
                if (std::abs(glm::determinant(object.WorldTransform)) > 1e-8f)
                    inverseTranspose = glm::transpose(glm::inverse(object.WorldTransform));

                uint32_t surfacePrimitiveOffset = 0;
                for (std::size_t renderSectionIndex = 0;
                    renderSectionIndex < object.Sections.size();
                    ++renderSectionIndex)
                {
                    const RenderMeshSection& section = object.Sections[renderSectionIndex];
                    const uint32_t sectionPrimitiveOffset = surfacePrimitiveOffset;
                    size_t sectionPrimitiveCount = section.SubMeshResource
                        ? section.SubMeshResource->GetIndices().size() / 3u : 0u;
                    if (section.SubMeshResource)
                        for (const SubMeshLod& lod : section.SubMeshResource->GetLods())
                            sectionPrimitiveCount += lod.Indices.size() / 3u;
                    if (sectionPrimitiveCount > std::numeric_limits<uint32_t>::max() - surfacePrimitiveOffset)
                        throw std::length_error("Surface primitive identity capacity exceeded");
                    surfacePrimitiveOffset += static_cast<uint32_t>(sectionPrimitiveCount);
                    RenderPath sectionPath = section.Path;
                    // Synthetic/legacy snapshots created before section-level
                    // classification inherit the explicit object path.
                    if (sectionPath == RenderPath::CpuOpaque &&
                        !section.GetMaterialInstance().bCustomSurface &&
                        object.Path != RenderPath::CpuOpaque)
                    {
                        sectionPath = object.Path;
                    }
                    if (sectionPath != RenderPath::CpuOpaque &&
                        sectionPath != RenderPath::GpuDrivenOpaque &&
                        sectionPath != RenderPath::Transparent &&
                        sectionPath != RenderPath::CpuSkinned &&
                        sectionPath != RenderPath::CpuSkinnedTransparent)
                    {
                        continue;
                    }
                    if (!section.SubMeshResource ||
                        (section.SubMeshResource->mStaticVertices.empty() &&
                            section.SubMeshResource->mSkinnedVertices.empty()) ||
                        section.SubMeshResource->GetIndices().empty())
                    {
                        continue;
                    }
                    if (section.MaterialResource && !section.GetMaterialInstance().bValid)
                    {
                        throw std::invalid_argument(
                            "RenderMeshSection with a MaterialResource requires an immutable material snapshot");
                    }
                    if (section.GetMaterialInstance().bCustomSurface)
                    {
                        const bool bSectionSkinned = section.SubMeshResource->IsSkinned();
                        sectionPath = bSectionSkinned
                            ? section.AlphaMode == RenderMaterialAlphaMode::Blend
                                ? RenderPath::CpuSkinnedTransparent
                                : RenderPath::CpuSkinned
                            : section.AlphaMode == RenderMaterialAlphaMode::Blend
                                ? RenderPath::Transparent
                                : RenderPath::CpuOpaque;
                    }

                    MeshEntry& mesh = GetOrCreateMesh(section.SubMeshResource);
                    const uint32_t lodIndex = bResourceWarmup ? 0u : SelectMeshLod(mesh, object, section,
                        world.Camera, static_cast<uint32_t>(renderSectionIndex));
                    if (!bResourceWarmup)
                    {
                    ++Stats.PreparedRasterSections;
                    Stats.SectionsWithAuthoredLods += mesh.Lods.size() > 1 ? 1u : 0u;
                    ++Stats.SelectedLodSections.at(lodIndex);
                    Stats.BaseRasterTriangleCount += mesh.Lods[0].IndexCount / 3u;
                    Stats.SelectedRasterTriangleCount += mesh.Lods[lodIndex].IndexCount / 3u;
                    Stats.ReducedLodSectionCount += lodIndex > 0 ? 1u : 0u;
                    }
                    ObjectConstants constants;
                    constants.World = object.WorldTransform;
                    constants.PreviousWorld = previousObject
                        ? previousObject->WorldTransform : object.WorldTransform;
                    constants.WorldInverseTranspose = inverseTranspose;
                    const uint32_t gpuMaterialIndex = GetOrCreateGpuMaterialIndex(section);
                    constants.MaterialObjectMeshIndices = glm::uvec4(
                        gpuMaterialIndex,
                        object.ObjectGpuIndex == InvalidGpuSceneIndex
                            ? 0u
                            : object.ObjectGpuIndex,
                        sectionPrimitiveOffset + mesh.Lods[lodIndex].PrimitiveOffset,
                        bonePaletteOffset);
                    if (!object.StaticLightmapPath.empty() &&
                        object.StaticLightmapBlend > 0.0f &&
                        object.BonePalette.empty() && lodIndex == 0u)
                    {
                        constants.StaticLightmapDescriptor.x =
                            GetOrCreateStaticLightmapDescriptor(object.StaticLightmapPath);
                        constants.StaticLightmapUvScaleBias = glm::vec4(
                            object.StaticLightmapUvScale, object.StaticLightmapUvBias);
                        constants.StaticLightmapScale0 = object.StaticLightmapScale0;
                        constants.StaticLightmapAdd0 = object.StaticLightmapAdd0;
                        constants.StaticLightmapScale1 = object.StaticLightmapScale1;
                        constants.StaticLightmapAdd1 = object.StaticLightmapAdd1;
                        constants.StaticLightmapDescriptor.z = object.StaticLightmapEncoding == 1 ? 1u : 0u;
                        constants.StaticLightmapDescriptor.y = static_cast<uint32_t>(
                            glm::round(glm::clamp(object.StaticLightmapBlend, 0.0f, 1.0f)
                                * 65535.0f));
                    }

                    if (GpuMaterialDataCpu.size() <= gpuMaterialIndex)
                        GpuMaterialDataCpu.resize(gpuMaterialIndex + 1u);
					if (frameMaterialDescriptorInputs.size() <= gpuMaterialIndex)
						frameMaterialDescriptorInputs.resize(gpuMaterialIndex + 1u);
                    std::shared_ptr<const CompiledMaterialShader> materialProgram;
                    if (!section.MaterialResource || !framePreparedMaterials.contains(gpuMaterialIndex))
                    {
                    GpuMaterialData materialData;
                    PopulateMaterialConstants(
                        materialData,
                        &section.GetMaterialInstance(),
                        section.MaterialResource.get(),
                        !section.GetMaterialInstance().bCustomSurface,
                        section.AlphaMode,
                        section.AlphaCutoff,
                        section.bDoubleSided);
                    if (section.MaterialResource &&
                        !section.GetMaterialInstance().bCustomSurface)
                    {
                        frameLegacyTextureMaterials.insert(section.MaterialResource.get());
						constexpr std::array<DefaultBindlessTexture, 5> fallbacks = {
							DefaultBindlessTexture::White,
							DefaultBindlessTexture::Normal,
							DefaultBindlessTexture::White,
							DefaultBindlessTexture::White,
							DefaultBindlessTexture::White,
						};
						const MaterialTextureSet& textures =
							MaterialTextureCache.at(section.MaterialResource.get());
						FrameMaterialDescriptorInput& descriptorInput =
							frameMaterialDescriptorInputs[gpuMaterialIndex];
						descriptorInput = {
							.MaterialIndex = gpuMaterialIndex,
						};
						descriptorInput.Textures.reserve(textures.Handles.size());
						for (size_t textureIndex = 0;
							textureIndex < textures.Handles.size();
							++textureIndex)
						{
							descriptorInput.Textures.push_back(
								ResolveMaterialTexture(
									textures.Handles[textureIndex],
									fallbacks[textureIndex]));
						}
                    }

                    if (section.GetMaterialInstance().bCustomSurface)
                    {
                        const auto existingProgram = frameMaterialPrograms.find(gpuMaterialIndex);
                        if (existingProgram != frameMaterialPrograms.end())
                        {
                            materialProgram = existingProgram->second;
                            materialData.ProgramParameterData =
                                frameMaterialProgramHeaders.at(gpuMaterialIndex);
                        }
                        else
                        {
                            const MaterialShaderProgramIdentity expectedIdentity{
                                .SourcePath = section.GetMaterialInstance().ShaderPath,
                                .RecursiveContentHash =
                                    section.GetMaterialInstance().ExpectedRecursiveSourceHash,
                                .MaterialAbiVersion =
                                    section.GetMaterialInstance().ExpectedMaterialAbiVersion,
                                .BindingSignatureHash =
                                    section.GetMaterialInstance().ExpectedBindingSignature,
                                .ArtifactIdentityHash =
                                    section.GetMaterialInstance().ExpectedArtifactIdentityHash,
                            };
                            materialProgram = MaterialShaderManagerInstance.FindCompatible(
                                expectedIdentity);

                            MaterialShaderRefreshResult refresh;
                            if (RequiresMaterialShaderFrameBoundaryRefresh(materialProgram))
                            {
                                refresh = RefreshMaterialShaderAtFrameBoundary(
                                    section.GetMaterialInstance().ShaderPath);
                                materialProgram = MaterialShaderManagerInstance.FindCompatible(
                                    expectedIdentity);
                            }
                            for (const MaterialShaderDiagnostic& diagnostic : refresh.Diagnostics)
                            {
                                if (diagnostic.Severity != MaterialShaderDiagnosticSeverity::Error)
                                    continue;
                                const std::string diagnosticKey =
                                    section.GetMaterialInstance().ShaderPath + ':' +
                                    diagnostic.Code + ':' + diagnostic.Message;
                                if (FailedMaterialPrograms.insert(diagnosticKey).second)
                                {
                                    HE_CORE_WARN(
                                        "Material shader '{}' retained last-known-good or skipped: [{}] {}",
                                        section.GetMaterialInstance().ShaderPath,
                                        diagnostic.Code,
                                        diagnostic.Message);
                                }
                            }
                            if (!materialProgram || !section.MaterialResource)
                                continue;

                            const MaterialShaderProgramIdentity& programIdentity =
                                materialProgram->GetIdentity();
                            const bool bSnapshotProgramCompatible =
                                IsMaterialSnapshotProgramCompatible(
                                    section.GetMaterialInstance(),
                                    programIdentity,
                                    materialProgram->GetGeneration());
                            if (!bSnapshotProgramCompatible)
                            {
                                const std::string diagnosticKey =
                                    section.GetMaterialInstance().AssetPath + ":snapshot-program-mismatch";
                                if (FailedMaterialPrograms.insert(diagnosticKey).second)
                                {
                                    HE_CORE_WARN(
                                        "Material '{}' snapshot does not match shader program '{}' generation {} (expected {}); draw deferred until an atomic layout migration",
                                        section.GetMaterialInstance().AssetPath,
                                        section.GetMaterialInstance().ShaderPath,
                                        materialProgram->GetGeneration(),
                                        section.GetMaterialInstance().ExpectedProgramGeneration);
                                }
                                continue;
                            }

                            const MaterialShaderManifest& programManifest =
                                materialProgram->GetManifest();
                            const MaterialShaderBlendMode programBlend = programManifest.BlendMode;
                            const bool bSnapshotStateMatchesSection =
                                section.GetMaterialInstance().AlphaMode == section.AlphaMode &&
                                section.GetMaterialInstance().bDoubleSided == section.bDoubleSided;
                            if (!IsMaterialSnapshotRenderStateCompatible(
                                    section.GetMaterialInstance(),
                                    programManifest) ||
                                !bSnapshotStateMatchesSection)
                            {
                                const std::string diagnosticKey =
                                    section.GetMaterialInstance().ShaderPath + ":render-state-mismatch";
                                if (FailedMaterialPrograms.insert(diagnosticKey).second)
                                {
                                    HE_CORE_WARN(
                                        "Material '{}' snapshot render state does not match shader manifest (blend='{}', cull='{}', depthWrite={}); draw skipped",
                                        section.GetMaterialInstance().AssetPath,
                                        ToString(programBlend),
                                        ToString(programManifest.CullMode),
                                        programManifest.bDepthWrite ? 1 : 0);
                                }
                                continue;
                            }

                            MaterialParameterPackingResult packing;
                            const auto* cachedPacking = bMaterialPackingCacheEnabled
                                ? MaterialPackingCache.Find(gpuMaterialIndex, section.MaterialResource.get(),
                                    section.GetMaterialInstance().Revision, materialProgram->GetIdentity(),
                                    materialProgram->GetGeneration()) : nullptr;
                            if (cachedPacking)
                            {
                                ++Stats.MaterialPackingCacheHitCount;
                                // Copy before per-frame texture/descriptor fixups.
                                packing = *cachedPacking;
                            }
                            else
                            {
                                const auto packingStart = std::chrono::steady_clock::now();
                                ++Stats.MaterialPackingCallCount;
                                packing = PackMaterialParameters(materialProgram->GetManifest(),
                                    section.GetMaterialInstance().ParameterValues);
                                Stats.CpuMaterialPackingMilliseconds += std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - packingStart).count();
                                if (bMaterialPackingCacheEnabled)
                                    MaterialPackingCache.Store(gpuMaterialIndex, section.MaterialResource,
                                        section.GetMaterialInstance().Revision, materialProgram->GetIdentity(),
                                        materialProgram->GetGeneration(), packing);
                            }
                            if (!packing.Succeeded())
                            {
                                for (const MaterialShaderDiagnostic& diagnostic : packing.Diagnostics)
                                {
                                    const std::string diagnosticKey =
                                        section.GetMaterialInstance().ShaderPath + ':' +
                                        diagnostic.Code + ':' + diagnostic.Message;
                                    if (FailedMaterialPrograms.insert(diagnosticKey).second)
                                    {
                                        HE_CORE_WARN(
                                            "Material parameters for '{}' are invalid: [{}] {}",
                                            section.GetMaterialInstance().ShaderPath,
                                            diagnostic.Code,
                                            diagnostic.Message);
                                    }
                                }
                                continue;
                            }
                            if (MaterialParameterRowsCpu.size() +
                                packing.PackedFloat4Rows.size() > c_MaxMaterialParameterRows)
                            {
                                throw std::length_error(
                                    "SceneRenderer material parameter row capacity exceeded");
                            }

                            const MaterialParameterTextureSet& parameterTextures =
                                GetOrCreateMaterialParameterTextureSet(
                                    *section.MaterialResource,
                                    packing,
                                    section.AlphaMode,
                                    section.AlphaCutoff);
                            frameCustomParameterMaterials.insert(
                                section.MaterialResource.get());
                            for (size_t textureIndex = 0;
                                textureIndex < packing.TextureFixups.size();
                                ++textureIndex)
                            {
                                const uint32_t rowOffset =
                                    packing.TextureFixups[textureIndex].RowOffset;
                                if (rowOffset >= packing.PackedFloat4Rows.size() ||
                                    textureIndex >= parameterTextures.ShaderIndices.size() ||
                                    textureIndex >= parameterTextures.NormalEncodings.size())
                                {
                                    throw std::logic_error(
                                        "Material texture fixup escaped its packed parameter block");
                                }
                                packing.PackedFloat4Rows[rowOffset][0] = std::bit_cast<float>(
                                    parameterTextures.ShaderIndices[textureIndex]);
                                // Texture properties own a float4 row. The descriptor
                                // uses x; y carries actual-format decoding metadata and
                                // remains intact when descriptor pages remap x.
                                packing.PackedFloat4Rows[rowOffset][1] = std::bit_cast<float>(
                                    parameterTextures.NormalEncodings[textureIndex]);
                            }

                            const uint32_t parameterRowOffset =
                                static_cast<uint32_t>(MaterialParameterRowsCpu.size());
                            for (const MaterialParameterFloat4Row& row : packing.PackedFloat4Rows)
                            {
                                MaterialParameterRowsCpu.emplace_back(
                                    row[0], row[1], row[2], row[3]);
                            }
                            materialData.ProgramParameterData = glm::uvec4(
                                static_cast<uint32_t>(materialProgram->GetGeneration()),
                                parameterRowOffset,
                                static_cast<uint32_t>(packing.PackedFloat4Rows.size()),
                                materialProgram->GetManifest().MaterialAbiVersion);
							FrameMaterialDescriptorInput& descriptorInput =
								frameMaterialDescriptorInputs[gpuMaterialIndex];
							descriptorInput = {
								.MaterialIndex = gpuMaterialIndex,
								.bCustomParameters = true,
							};
							descriptorInput.Textures.reserve(
								parameterTextures.Handles.size());
							descriptorInput.ParameterRowOffsets.reserve(
								parameterTextures.RowOffsets.size());
							for (size_t textureIndex = 0;
								textureIndex < parameterTextures.Handles.size();
								++textureIndex)
							{
								descriptorInput.Textures.push_back(
									ResolveMaterialTexture(
										parameterTextures.Handles[textureIndex],
										parameterTextures.Fallbacks[textureIndex]));
								descriptorInput.ParameterRowOffsets.push_back(
									parameterRowOffset +
										parameterTextures.RowOffsets[textureIndex]);
							}
                            frameMaterialPrograms.emplace(gpuMaterialIndex, materialProgram);
                            frameMaterialProgramHeaders.emplace(
                                gpuMaterialIndex,
                                materialData.ProgramParameterData);
                        }
                    }

                    if (section.MaterialResource)
                    {
                        const Material& material = *section.MaterialResource;
                        frameMaterials.insert(&material);
                        frameGpuMaterials.insert(gpuMaterialIndex);
                    }
                    GpuMaterialDataCpu[gpuMaterialIndex] = materialData;
                    if (section.MaterialResource) framePreparedMaterials.insert(gpuMaterialIndex);
                    }
                    else if (section.GetMaterialInstance().bCustomSurface)
                        materialProgram = frameMaterialPrograms.at(gpuMaterialIndex);
                    // Resource-only requests use the same upload/material lifecycle,
                    // but never contribute draws, shadows, visibility or bone history.
                    if (bResourceWarmup) continue;

                    if (materialProgram && sectionPath == RenderPath::CpuOpaque)
                    {
                        const auto geometryCapabilities = ResolveMaterialGeometryCapabilities(
                            *materialProgram, bHeapDirectlyIndexed
                                ? MaterialShaderDescriptorMode::HeapDirect
                                : MaterialShaderDescriptorMode::Baseline, mesh.bSkinned);
                        if (geometryCapabilities.bGpuInstances)
                            sectionPath = RenderPath::GpuDrivenOpaque;
                        else if (FailedMaterialPrograms.insert(section.GetMaterialInstance().ShaderPath +
                            ":gpu-fallback:" + geometryCapabilities.FallbackReason).second)
                            HE_CORE_INFO("Material '{}' uses CPU submission: {}",
                                section.GetMaterialInstance().ShaderPath, geometryCapabilities.FallbackReason);
                    }

                    if (object.bSelected)
                    {
                        OutlineDrawItems.push_back(DrawItem{
                            .Key = mesh.Resource.get(),
                            .LodIndex = lodIndex,
                            .Constants = constants,
                            .MaterialProgram = materialProgram,
                            .bDoubleSided = section.bDoubleSided,
                            .bSkinned = mesh.bSkinned,
                        });
                    }

                    if (sectionPath == RenderPath::CpuOpaque)
                    {
                        CpuDrawItems.push_back(DrawItem{
                            .Key = mesh.Resource.get(),
                            .LodIndex = lodIndex,
                            .Constants = constants,
                            .MaterialProgram = materialProgram,
                            .bDoubleSided = section.bDoubleSided,
                        });
                    }
                    else if (sectionPath == RenderPath::Transparent)
                    {
                        TransparentDrawItems.push_back(DrawItem{
                            .Key = mesh.Resource.get(),
                            .LodIndex = lodIndex,
                            .Constants = constants,
                            .MaterialProgram = materialProgram,
                            .SortKey = {
                                .Depth = ComputeTransparentSubMeshSortDepth(
                                    world.Camera.View,
                                    object.WorldTransform,
                                    mesh.Resource->HasBounds(),
                                    mesh.Resource->GetBoundsMin(),
                                    mesh.Resource->GetBoundsMax()),
                                .EntityId = object.EntityId,
                                .SectionIndex = static_cast<uint32_t>(renderSectionIndex),
                                .SubmissionOrder =
                                    static_cast<uint64_t>(TransparentDrawItems.size()),
                            },
                            .bDoubleSided = section.bDoubleSided,
                        });
                    }
                    else if (sectionPath == RenderPath::CpuSkinned)
                    {
                        SkinnedDrawItems.push_back(DrawItem{
                            .Key = mesh.Resource.get(),
                            .LodIndex = lodIndex,
                            .Constants = constants,
                            .MaterialProgram = materialProgram,
                            .bDoubleSided = section.bDoubleSided,
                            .bSkinned = true,
                        });
                    }
                    else if (sectionPath == RenderPath::CpuSkinnedTransparent)
                    {
                        TransparentDrawItems.push_back(DrawItem{
                            .Key = mesh.Resource.get(),
                            .LodIndex = lodIndex,
                            .Constants = constants,
                            .MaterialProgram = materialProgram,
                            .SortKey = {
                                .Depth = ComputeTransparentSubMeshSortDepth(
                                    world.Camera.View,
                                    object.WorldTransform,
                                    mesh.Resource->HasBounds(),
                                    mesh.Resource->GetBoundsMin(),
                                    mesh.Resource->GetBoundsMax()),
                                .EntityId = object.EntityId,
                                .SectionIndex = static_cast<uint32_t>(renderSectionIndex),
                                .SubmissionOrder =
                                    static_cast<uint64_t>(TransparentDrawItems.size()),
                            },
                            .bDoubleSided = section.bDoubleSided,
                            .bSkinned = true,
                        });
                    }

                    if ((sectionPath == RenderPath::CpuOpaque ||
                            sectionPath == RenderPath::GpuDrivenOpaque ||
                            sectionPath == RenderPath::CpuSkinned) &&
                        (bDirectionalShadowEnabled || LocalShadowPass->GetStats().ActiveViews > 0) && object.bCastShadow &&
                        (!materialProgram || materialProgram->GetManifest().bCastsShadow))
                    {
                        ShadowDrawItems.push_back(DrawItem{
                            .Key = mesh.Resource.get(),
                            .LodIndex = lodIndex,
                            .Constants = constants,
                            .MaterialProgram = materialProgram,
                            .bDoubleSided = section.bDoubleSided,
                            .bSkinned = sectionPath == RenderPath::CpuSkinned,
                        });
                    }

                    if (sectionPath != RenderPath::GpuDrivenOpaque)
                        continue;

                    if (GpuObjectData.size() >= c_MaxGpuDrivenObjects)
                    {
                        throw std::length_error(
                            "SceneRenderer GPU-driven object capacity exceeded");
                    }

                    RenderBounds bounds = object.Bounds;
                    glm::vec3 mainAabbExtents(0.f);
                    bool bMainAabbTrusted = false;
                    if (section.SubMeshResource->HasBounds())
                    {
                        const glm::vec3 localMinimum = section.SubMeshResource->GetBoundsMin();
                        const glm::vec3 localMaximum = section.SubMeshResource->GetBoundsMax();
                        const glm::vec3 localCenter = (localMinimum + localMaximum) * 0.5f;
                        const glm::vec3 localExtents = (localMaximum - localMinimum) * 0.5f;
                        bounds.Center = glm::vec3(object.WorldTransform * glm::vec4(localCenter, 1.0f));
                        // Per-section bounds avoid reusing a whole imported
                        // multi-section mesh sphere. Transform the full AABB;
                        // absolute columns remain conservative under shear.
                        const glm::vec3 worldExtents = glm::abs(glm::vec3(object.WorldTransform[0])) * localExtents.x +
                            glm::abs(glm::vec3(object.WorldTransform[1])) * localExtents.y +
                            glm::abs(glm::vec3(object.WorldTransform[2])) * localExtents.z;
                        mainAabbExtents = worldExtents;
                        bMainAabbTrusted = !section.GetMaterialInstance().bCustomSurface && !mesh.bSkinned &&
                            object.BonePalette.empty() && object.WorldTransform[0][3] == 0.f &&
                            object.WorldTransform[1][3] == 0.f && object.WorldTransform[2][3] == 0.f &&
                            object.WorldTransform[3][3] == 1.f;
                        bounds.Radius = glm::length(worldExtents);
                        bounds.bValid = true;
                    }

                    if (bGpuTightFrustumEnabled)
                    {
                        const bool bReject = bMainAabbTrusted && !IsMainViewAabbVisible(
                            CurrentRasterCamera.ViewProjection, bounds.Center, mainAabbExtents, LastWidth, LastHeight);
                        mainAabbRejectFlags.push_back(bReject ? 1u : 0u);
                        Stats.MainAabbTestedCandidates += bMainAabbTrusted ? 1u : 0u;
                        Stats.MainAabbRejectedCandidates += bReject ? 1u : 0u;
                        if (bReject) Stats.MainAabbRejectedCandidateTriangles += mesh.Lods[lodIndex].IndexCount / 3u;
                    }
                    const uint32_t objectIndex = static_cast<uint32_t>(GpuObjectData.size());
                    GpuObjectData.push_back(constants);
                    GpuBoundsDataCpu.push_back(GpuBoundsData{
                        .CenterRadius = bounds.bValid
                            ? glm::vec4(bounds.Center, std::max(bounds.Radius, 0.0f))
                            : glm::vec4(glm::vec3(object.WorldTransform[3]), 0.0f),
                    });

                    const GpuBatchKey batchKey{
                        .Mesh = mesh.Resource.get(),
                        .MaterialResource = section.MaterialResource.get(),
                        .LodIndex = lodIndex,
                        .bDoubleSided = section.bDoubleSided,
                    };
                    auto batch = batchLookup.find(batchKey);
                    if (batch == batchLookup.end())
                    {
                        if (GpuBatches.size() >= c_MaxGpuDrivenBatches)
                        {
                            throw std::length_error(
                                "SceneRenderer GPU-driven batch capacity exceeded");
                        }
                        const uint32_t batchIndex = static_cast<uint32_t>(GpuBatches.size());
						GpuBatches.push_back(GpuBatch{
							.Key = batchKey,
                            .MaterialProgram = materialProgram,
							.MaterialIndex = gpuMaterialIndex,
						});
                        GpuBatchObjectIndices.emplace_back();
                        batch = batchLookup.emplace(batchKey, batchIndex).first;
                    }
                    GpuBatchObjectIndices[batch->second].push_back(objectIndex);
                }
            }

            {
                for (const auto& object : world.Objects)
                {
                    float pixels = 16384.0f; // Unknown/deforming bounds must not lose detail.
                    if (object.Bounds.bValid && object.BonePalette.empty())
                    {
                        const auto& bounds = object.Bounds;
                        if (!IsMainViewAabbVisible(world.Camera.NonJitteredViewProjection,
                            bounds.Center, glm::vec3(bounds.Radius), LastOutputWidth, LastOutputHeight)) continue;
                        const auto center = world.Camera.View * glm::vec4(bounds.Center, 1);
                        const float depth = -center.z - bounds.Radius;
                        const bool perspective = std::abs(world.Camera.Projection[2][3]) > .5f;
                        if (!perspective || depth > world.Camera.NearPlane)
                            pixels = bounds.Radius * std::max(
                                LastOutputWidth * std::abs(world.Camera.Projection[0][0]),
                                LastOutputHeight * std::abs(world.Camera.Projection[1][1])) /
                                (perspective ? depth : 1.0f);
                    }
                    for (const auto& section : object.Sections)
                    {
                        if (const auto found = MaterialTextureCache.find(section.MaterialResource.get());
                            found != MaterialTextureCache.end())
                            for (const auto handle : found->second.Handles)
                                TextureManagerInstance.RequestTextureDetail(handle, pixels);
                        if (const auto found = MaterialParameterTextureCache.find(section.MaterialResource.get());
                            found != MaterialParameterTextureCache.end())
                            for (const auto handle : found->second.Handles)
                                TextureManagerInstance.RequestTextureDetail(handle, pixels);
                    }
                }
                TextureManagerInstance.EndStreamingFrame(TextureStreamingBudgetBytes);
            }
            const double objectsMs = checkpoint();
            std::sort(
                TransparentDrawItems.begin(),
                TransparentDrawItems.end(),
                [](const DrawItem& left, const DrawItem& right)
                {
                    return IsTransparentDrawSortKeyBefore(left.SortKey, right.SortKey);
                });

            uint32_t visibleBase = 0;
            for (uint32_t batchIndex = 0;
                batchIndex < static_cast<uint32_t>(GpuBatches.size());
                ++batchIndex)
            {
                GpuBatch& batch = GpuBatches[batchIndex];
                batch.VisibleBase = visibleBase;
                batch.ObjectCount = static_cast<uint32_t>(GpuBatchObjectIndices[batchIndex].size());
                const MeshEntry& mesh = MeshCache.at(batch.Key.Mesh);
                GpuIndirectArgumentsCpu.push_back(nvrhi::DrawIndexedIndirectArguments()
                    .setIndexCount(mesh.Lods[batch.Key.LodIndex].IndexCount)
                    .setInstanceCount(0)
                    .setStartIndexLocation(mesh.ArenaAllocation.FirstIndex + mesh.Lods[batch.Key.LodIndex].FirstIndex)
                    .setBaseVertexLocation(static_cast<int32_t>(mesh.ArenaAllocation.FirstVertex))
                    .setStartInstanceLocation(visibleBase));
                for (const uint32_t objectIndex : GpuBatchObjectIndices[batchIndex])
                {
                    GpuDrawDataCpu.push_back(GpuDrawData{
                        .ObjectIndex = objectIndex,
                        .BatchIndex = batchIndex,
                        .VisibleBase = visibleBase,
                        .Padding = bGpuTightFrustumEnabled ? mainAabbRejectFlags.at(objectIndex) : 0u,
                    });
                }
                visibleBase += batch.ObjectCount;
            }

            const double batchesMs = checkpoint();
            // Brief camera cuts must not synchronously reload the same material
            // textures when their owners leave and re-enter the view. Retain only
            // a bounded inactive working set; scene changes still release it.
            constexpr uint64_t MaxInactiveFrames = 300;
            constexpr uint64_t MaxInactiveBytes = 256ull * 1024 * 1024;
            struct InactiveMaterial
            {
                const Material* Identity;
                uint64_t LastUsedFrame;
                uint64_t Bytes;
                bool bParameters;
            };
            std::vector<InactiveMaterial> inactiveMaterials;
            uint64_t inactiveBytes = 0;
            const auto payloadBytes = [&](const auto& handles)
            {
                uint64_t bytes = 0;
                for (const auto handle : handles)
                    if (handle && TextureManagerInstance.IsValid(handle))
                        bytes += TextureManagerInstance.GetTexture(handle).PayloadBytes;
                return bytes;
            };
            for (auto iterator = MaterialTextureCache.begin(); iterator != MaterialTextureCache.end();)
            {
                if (frameLegacyTextureMaterials.contains(iterator->first)) { ++iterator; continue; }
                const auto& set = iterator->second;
                if (CurrentMaterialFrame - set.LastUsedFrame > MaxInactiveFrames)
                {
                    ReleaseMaterialTextureSet(iterator->second, LastSubmittedSubmission);
                    iterator = MaterialTextureCache.erase(iterator);
                    continue;
                }
                const uint64_t bytes = payloadBytes(set.Handles);
                inactiveMaterials.push_back({iterator->first, set.LastUsedFrame, bytes, false});
                inactiveBytes += bytes;
                ++iterator;
            }
            for (auto iterator = MaterialParameterTextureCache.begin(); iterator != MaterialParameterTextureCache.end();)
            {
                if (frameCustomParameterMaterials.contains(iterator->first)) { ++iterator; continue; }
                const auto& set = iterator->second;
                if (CurrentMaterialFrame - set.LastUsedFrame > MaxInactiveFrames)
                {
                    ReleaseMaterialParameterTextureSet(iterator->second, LastSubmittedSubmission);
                    iterator = MaterialParameterTextureCache.erase(iterator);
                    continue;
                }
                const uint64_t bytes = payloadBytes(set.Handles);
                inactiveMaterials.push_back({iterator->first, set.LastUsedFrame, bytes, true});
                inactiveBytes += bytes;
                ++iterator;
            }
            if (inactiveBytes > MaxInactiveBytes)
            {
                std::sort(inactiveMaterials.begin(), inactiveMaterials.end(),
                    [](const auto& left, const auto& right) { return left.LastUsedFrame < right.LastUsedFrame; });
                for (const auto& candidate : inactiveMaterials)
                {
                    if (inactiveBytes <= MaxInactiveBytes) break;
                    if (candidate.bParameters)
                    {
                        const auto found = MaterialParameterTextureCache.find(candidate.Identity);
                        if (found == MaterialParameterTextureCache.end()) continue;
                        ReleaseMaterialParameterTextureSet(found->second, LastSubmittedSubmission);
                        MaterialParameterTextureCache.erase(found);
                    }
                    else
                    {
                        const auto found = MaterialTextureCache.find(candidate.Identity);
                        if (found == MaterialTextureCache.end()) continue;
                        ReleaseMaterialTextureSet(found->second, LastSubmittedSubmission);
                        MaterialTextureCache.erase(found);
                    }
                    inactiveBytes -= candidate.Bytes;
                }
            }

			std::erase_if(frameMaterialDescriptorInputs,
				[](const FrameMaterialDescriptorInput& input)
				{
					return input.Textures.empty();
				});
			BuildBaselineMaterialDescriptorPages(
				frameMaterialDescriptorInputs);
            MaterialPackingCache.Retain(frameGpuMaterials);
            PreparePrimitive2DData(world);

            const double materialCachesMs = checkpoint();
            Stats.CpuFallbackDrawCount = static_cast<uint32_t>(CpuDrawItems.size());
            Stats.CustomMaterialCpuFallbackDrawCount = static_cast<uint32_t>(
                std::ranges::count_if(CpuDrawItems, [](const DrawItem& item)
                {
                    return static_cast<bool>(item.MaterialProgram);
                }) +
                std::ranges::count_if(SkinnedDrawItems, [](const DrawItem& item)
                {
                    return static_cast<bool>(item.MaterialProgram);
                }) +
                std::ranges::count_if(TransparentDrawItems, [](const DrawItem& item)
                {
                    return static_cast<bool>(item.MaterialProgram);
                }));
            std::unordered_set<const CompiledMaterialShader*> uniqueMaterialPrograms;
            for (const auto& [materialIndex, program] : frameMaterialPrograms)
            {
                (void)materialIndex;
                if (program)
                    uniqueMaterialPrograms.insert(program.get());
            }
            Stats.MaterialProgramCount = static_cast<uint32_t>(uniqueMaterialPrograms.size());
            Stats.MaterialParameterRowCount = static_cast<uint32_t>(MaterialParameterRowsCpu.size());
            Stats.MaterialProgramGenerationMax = 0;
            for (const auto& [materialIndex, program] : frameMaterialPrograms)
            {
                (void)materialIndex;
                Stats.MaterialProgramGenerationMax = std::max(
                    Stats.MaterialProgramGenerationMax,
                    program ? program->GetGeneration() : uint64_t{0});
            }
            Stats.CpuTransparentDrawCount =
                static_cast<uint32_t>(TransparentDrawItems.size());
            Stats.SkinnedDrawCount = static_cast<uint32_t>(SkinnedDrawItems.size() +
                std::ranges::count_if(TransparentDrawItems, [](const DrawItem& item)
                {
                    return item.bSkinned;
                }));
            Stats.SkinnedBoneMatrixCount = static_cast<uint32_t>(BoneMatricesCpu.size());
            Stats.EditorOutlineDrawCount = static_cast<uint32_t>(OutlineDrawItems.size());
            Stats.AlphaMaskDrawCount = 0;
            for (const GpuMaterialData& material : GpuMaterialDataCpu)
            {
                if ((material.EmissiveSamplerFlags.z & c_MaterialFlagAlphaMask) != 0)
                    ++Stats.AlphaMaskDrawCount;
            }
            const auto isAlphaMaskMaterial = [this](uint32_t materialIndex)
            {
                return materialIndex < GpuMaterialDataCpu.size() &&
                    (GpuMaterialDataCpu[materialIndex].EmissiveSamplerFlags.z &
                        c_MaterialFlagAlphaMask) != 0;
            };
            Stats.DepthPrepassDrawCount = static_cast<uint32_t>(
                CpuDrawItems.size() + SkinnedDrawItems.size() + GpuBatches.size());
            Stats.DepthPrepassIndirectDrawCount = static_cast<uint32_t>(GpuBatches.size());
            Stats.DepthPrepassSkinnedDrawCount = static_cast<uint32_t>(SkinnedDrawItems.size());
            Stats.DepthPrepassAlphaMaskDrawCount = 0;
            for (const DrawItem& drawItem : CpuDrawItems)
            {
                if (isAlphaMaskMaterial(drawItem.Constants.MaterialObjectMeshIndices.x))
                    ++Stats.DepthPrepassAlphaMaskDrawCount;
            }
            for (const DrawItem& drawItem : SkinnedDrawItems)
            {
                if (isAlphaMaskMaterial(drawItem.Constants.MaterialObjectMeshIndices.x))
                    ++Stats.DepthPrepassAlphaMaskDrawCount;
            }
            for (const std::vector<uint32_t>& objectIndices : GpuBatchObjectIndices)
            {
                if (!objectIndices.empty() &&
                    isAlphaMaskMaterial(GpuObjectData[objectIndices.front()]
                        .MaterialObjectMeshIndices.x))
                {
                    ++Stats.DepthPrepassAlphaMaskDrawCount;
                }
            }
            Stats.DirectionalShadowDrawCount = bDirectionalShadowEnabled
                ? static_cast<uint32_t>(ShadowDrawItems.size()) *
                    DirectionalShadowCascadeCount
                : 0u;
            Stats.DirectionalShadowCascadeCount = bDirectionalShadowEnabled
                ? DirectionalShadowCascadeCount
                : 0u;
            Stats.GpuDrivenObjectCount = static_cast<uint32_t>(GpuObjectData.size());
            Stats.GpuDrivenBatchCount = static_cast<uint32_t>(GpuBatches.size());
            Stats.GpuIndirectDrawCount = static_cast<uint32_t>(GpuBatches.size());
            Stats.GpuComputeDispatchCount = GpuObjectData.empty() ? 0u : 1u;
            Stats.StaticOpaqueDrawCount = Stats.CpuFallbackDrawCount + Stats.GpuIndirectDrawCount;
            Stats.CachedStaticMeshCount = static_cast<uint32_t>(MeshCache.size());
            Stats.BindlessMaterialCount = static_cast<uint32_t>(frameMaterials.size());
            Stats.GpuMaterialCount = static_cast<uint32_t>(frameGpuMaterials.size());
            Stats.PendingTextureUploads = static_cast<uint32_t>(
                TextureManagerInstance.GetUploadStatistics().Pending);
            Stats.BindlessTextureCount = static_cast<uint32_t>(
                TextureManagerInstance.GetBindlessTextures().size());
            if (LastTextureManagerRevision != TextureManagerInstance.GetRevision())
            {
                LastTextureManagerRevision = TextureManagerInstance.GetRevision();
                // Heap-direct textures are retained by TextureManager and remain
                // in permanent ShaderResource state after upload. Its descriptor
                // table can change without changing the render graph topology.
                if (!bHeapDirectlyIndexed)
                {
                    bGraphDirty = true;
                    Stats.GraphBuildReasonMask |= SceneGraphRebuildReason::TextureTable;
                }
            }
            if (bTracePreparation)
            {
                const double statsMs = checkpoint();
                const double totalMs = textureCollectMs + environmentMs + frameSetupMs +
                    objectsMs + batchesMs + materialCachesMs + statsMs;
                if (totalMs > 6.0 || world.FrameNumber % 60 == 0)
                    HE_CORE_INFO("[FramePreparation] frame={} totalMs={:.3f} textureCollectMs={:.3f} environmentMs={:.3f} setupMs={:.3f} objectsMs={:.3f} batchesMs={:.3f} materialCachesMs={:.3f} statsMs={:.3f}",
                        world.FrameNumber, totalMs, textureCollectMs, environmentMs, frameSetupMs,
                        objectsMs, batchesMs, materialCachesMs, statsMs);
            }
        }

        void UpdatePostTaaView()
        {
            const bool bNeedsPostTaaDepth = IsTaaActive() && !IsOfflineViewportRenderingActive() &&
                (!TransparentDrawItems.empty() || !OutlineDrawItems.empty() || !Primitive2DBatches.empty() ||
                    CurrentVfxRequirements.bHasActiveEffects || bRunEmptyEffekseerPassForValidation);
            if (bNeedsPostTaaDepth != bPostTaaDepthRequired)
            {
                bPostTaaDepthRequired = bNeedsPostTaaDepth;
                bGraphDirty = true;
                Stats.GraphBuildReasonMask |= SceneGraphRebuildReason::PostTaaDepth;
            }
            CurrentVfxView.Projection = bPostTaaDepthRequired
                ? CurrentRasterCamera.NonJitteredViewProjection * glm::inverse(CurrentRasterCamera.View)
                : CurrentRasterCamera.Projection;
            VfxRenderGraphPass.UpdateView(CurrentVfxView);
        }

        void BuildGraph(const SceneRendererFrameDesc& frame)
        {
            HE_PROFILE;
            UpdatePostTaaView();
            Graph.Clear();
            Graph.Resize(frame.Width, frame.Height);
            GraphGeometryResources.clear();
            GraphMaterialTextures.clear();

            OffscreenColorHandle = {};
            OffscreenDepthHandle = {};
            RuntimeUiBackdropHandle = {};
            const bool bOffscreenOnly =
                frame.OutputMode == SceneRendererOutputMode::OffscreenOnly;
            const bool bCopyToOffscreen =
                frame.OutputMode == SceneRendererOutputMode::SwapChainAndOffscreen;
			// Newly created Vulkan images are VK_IMAGE_LAYOUT_UNDEFINED. Track the
			// first offscreen write from Common, then use the exported shader-read
			// state once a frame has initialized the contents.
			const nvrhi::ResourceStates offscreenInitialState =
				bOffscreenOutputInitialized
				? nvrhi::ResourceStates::ShaderResource
				: nvrhi::ResourceStates::Common;
            nvrhi::TextureHandle outputTarget = bOffscreenOnly
                ? OffscreenOutput.Color
                : frame.BackBuffer;
            const nvrhi::Format outputFormat = bOffscreenOnly
                ? OffscreenOutput.ColorFormat
                : frame.BackBufferFormat;

            BackBufferHandle = Graph.ImportTexture(
                RenderGraphTextureDesc{
                    .DebugName = bOffscreenOnly
                        ? OffscreenDebugName + " Color"
                        : "SceneRenderer Imported Backbuffer",
                    .Width = LastOutputWidth,
                    .Height = LastOutputHeight,
                    .Format = outputFormat,
                },
                outputTarget,
                bOffscreenOnly
                    ? offscreenInitialState
                    : frame.BackBufferInitialState,
                bOffscreenOnly
                    ? nvrhi::ResourceStates::ShaderResource
                    : nvrhi::ResourceStates::Present);
            if (bOffscreenOnly)
            {
                OffscreenColorHandle = BackBufferHandle;
                OffscreenDepthHandle = Graph.ImportTexture(
                    RenderGraphTextureDesc{
                        .DebugName = OffscreenDebugName + " Depth",
                        .Width = OffscreenOutput.Width,
                        .Height = OffscreenOutput.Height,
                        .Format = OffscreenOutput.DepthFormat,
                    },
                    OffscreenOutput.Depth,
                    offscreenInitialState,
                    nvrhi::ResourceStates::ShaderResource);
            }
            else if (bCopyToOffscreen)
            {
                OffscreenColorHandle = Graph.ImportTexture(
                    RenderGraphTextureDesc{
                        .DebugName = OffscreenDebugName + " Color",
                        .Width = OffscreenOutput.Width,
                        .Height = OffscreenOutput.Height,
                        .Format = OffscreenOutput.ColorFormat,
                    },
                    OffscreenOutput.Color,
                    offscreenInitialState,
                    nvrhi::ResourceStates::ShaderResource);
                OffscreenDepthHandle = Graph.ImportTexture(
                    RenderGraphTextureDesc{
                        .DebugName = OffscreenDebugName + " Depth",
                        .Width = OffscreenOutput.Width,
                        .Height = OffscreenOutput.Height,
                        .Format = OffscreenOutput.DepthFormat,
                    },
                    OffscreenOutput.Depth,
                    offscreenInitialState,
                    nvrhi::ResourceStates::ShaderResource);
            }
			if (frame.bReuseOffscreenScene)
			{
				AddClearOutputPass();
				AddWindowOverlayPass();
				AddCapturePass();
				AddPresentPass();
				Graph.CompileOrThrow();
				LastBackBufferFormat = outputFormat;
				LastBackBufferInitialState = frame.BackBufferInitialState;
				LastOutputMode = frame.OutputMode;
				LastSceneOverlayMode = frame.SceneOverlayMode;
				bLastReuseOffscreenScene = true;
				bGraphDirty = false;
				Stats.CompiledPasses = Graph.GetCompiledPassNames();
				return;
			}

            FrameBufferHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer Frame Constants",
                    .ByteSize = sizeof(FrameConstants),
                },
                FrameConstantBuffer,
                bFrameConstantsInitialized
                    ? nvrhi::ResourceStates::ConstantBuffer
                    : nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::ConstantBuffer);
            ObjectBufferHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer Object/Material Constants",
                    .ByteSize = sizeof(ObjectConstants),
                },
                ObjectConstantBuffer,
                bObjectConstantsInitialized
                    ? nvrhi::ResourceStates::ConstantBuffer
                    : nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::ConstantBuffer);

            GpuObjectsHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer GPU Scene Objects",
                    .ByteSize = static_cast<uint64_t>(c_MaxGpuDrivenObjects) *
                        sizeof(ObjectConstants),
                    .StructStride = sizeof(ObjectConstants),
                },
                GpuObjectBuffer,
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Common);
            GpuMaterialsHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer GPU Scene Materials",
                    .ByteSize = static_cast<uint64_t>(c_MaxGpuMaterials) * sizeof(GpuMaterialData),
                    .StructStride = sizeof(GpuMaterialData),
                },
                GpuMaterialBuffer,
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Common);
            MaterialParametersHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer Material Parameter Rows",
                    .ByteSize = static_cast<uint64_t>(c_MaxMaterialParameterRows) * sizeof(glm::vec4),
                    .StructStride = sizeof(glm::vec4),
                },
                MaterialParameterBuffer,
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Common);
            GpuBoundsHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer GPU Scene Bounds",
                    .ByteSize = static_cast<uint64_t>(c_MaxGpuDrivenObjects) * sizeof(GpuBoundsData),
                    .StructStride = sizeof(GpuBoundsData),
                },
                GpuBoundsBuffer,
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Common);
            GpuDrawDataHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer GPU Scene Draw Data",
                    .ByteSize = static_cast<uint64_t>(c_MaxGpuDrivenObjects) * sizeof(GpuDrawData),
                    .StructStride = sizeof(GpuDrawData),
                },
                GpuDrawDataBuffer,
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Common);
            VisibleObjectIndicesHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer GPU Visible Object Indices",
                    .ByteSize = static_cast<uint64_t>(c_MaxGpuDrivenObjects) * sizeof(uint32_t),
                    .StructStride = sizeof(uint32_t),
                },
                VisibleObjectIndicesBuffer,
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Common);
            IndirectArgumentsHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer GPU Indexed Indirect Arguments",
                    .ByteSize = static_cast<uint64_t>(c_MaxGpuDrivenBatches) *
                        sizeof(nvrhi::DrawIndexedIndirectArguments),
                    .StructStride = sizeof(nvrhi::DrawIndexedIndirectArguments),
                },
                IndirectArgumentsBuffer,
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Common);
            GpuReadbackHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer GPU Culling Readback",
                    .ByteSize = static_cast<uint64_t>(c_MaxGpuDrivenBatches) *
                        sizeof(nvrhi::DrawIndexedIndirectArguments),
                    .StructStride = sizeof(nvrhi::DrawIndexedIndirectArguments),
                },
                GpuReadbackSlots.at(frame.FrameSlot).Buffer,
                nvrhi::ResourceStates::CopyDest,
                nvrhi::ResourceStates::CopyDest);
            Primitive2DVerticesHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer Primitive2D Vertices",
                    .ByteSize = static_cast<uint64_t>(c_MaxPrimitive2DVertices) *
                        sizeof(Primitive2DVertex),
                    .StructStride = sizeof(Primitive2DVertex),
                },
                Primitive2DBufferSlots.at(frame.FrameSlot).Vertices,
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Common);
            BoneMatricesHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer Skinned Bone Matrices",
                    .ByteSize = static_cast<uint64_t>(SceneBoneMatrixCapacity) * sizeof(glm::mat4),
                    .StructStride = sizeof(glm::mat4),
                },
                BoneMatrixBuffer,
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Common);
            PreviousBoneMatricesHandle = Graph.ImportBuffer(
                RenderGraphBufferDesc{
                    .DebugName = "SceneRenderer Previous Bone Matrices",
                    .ByteSize = static_cast<uint64_t>(SceneBoneMatrixCapacity) * sizeof(glm::mat4),
                    .StructStride = sizeof(glm::mat4),
                }, PreviousBoneMatrixBuffer,
                nvrhi::ResourceStates::Common, nvrhi::ResourceStates::Common);

			std::vector<nvrhi::TextureHandle> frameMaterialTextures;
			if (!bHeapDirectlyIndexed)
			{
				frameMaterialTextures = TextureManagerInstance.GetBindlessTextures();
				for (const MaterialDescriptorPageState& page : MaterialPages)
				{
					frameMaterialTextures.insert(
						frameMaterialTextures.end(),
						page.Textures.begin(),
						page.Textures.end());
				}
			}
			std::unordered_set<nvrhi::ITexture*> importedMaterialTextures;
			for (nvrhi::TextureHandle texture : frameMaterialTextures)
            {
				if (!texture ||
					!importedMaterialTextures.insert(texture.Get()).second)
				{
					continue;
				}
                const nvrhi::TextureDesc& desc = texture->getDesc();
                GraphMaterialTextures.push_back(GraphMaterialTexture{
                    .Handle = Graph.ImportTexture(
                        RenderGraphTextureDesc{
                            .DebugName = desc.debugName,
                            .Width = desc.width,
                            .Height = desc.height,
                            .MipLevels = desc.mipLevels,
                            .ArrayLayers = desc.arraySize,
                            .Format = desc.format,
                            .Dimension = desc.dimension,
                        },
                        texture,
                        nvrhi::ResourceStates::ShaderResource,
                        nvrhi::ResourceStates::ShaderResource),
                    .Resource = std::move(texture),
                });
            }

            const bool bUsingDefaultEnvironment =
                ActiveIrradiance == DefaultEnvironment &&
                ActivePrefilter == DefaultEnvironment;
            const nvrhi::ResourceStates environmentInitialState =
                bUsingDefaultEnvironment && !bDefaultEnvironmentUploaded
                    ? nvrhi::ResourceStates::Common
                    : nvrhi::ResourceStates::ShaderResource;
            IrradianceHandle = Graph.ImportTexture(
                RenderGraphTextureDesc{
                    .DebugName = "SceneRenderer Active Irradiance Cubemap",
                    .Width = ActiveIrradiance->getDesc().width,
                    .Height = ActiveIrradiance->getDesc().height,
                    .MipLevels = ActiveIrradiance->getDesc().mipLevels,
                    .ArrayLayers = ActiveIrradiance->getDesc().arraySize,
                    .Format = ActiveIrradiance->getDesc().format,
                    .Dimension = nvrhi::TextureDimension::TextureCube,
                },
                ActiveIrradiance,
                environmentInitialState,
                nvrhi::ResourceStates::ShaderResource);
            EnvironmentCdfHandle = {};
            if (ActiveEnvironmentCdf && ActiveEnvironmentProposal)
                EnvironmentCdfHandle = Graph.ImportTexture(RenderGraphTextureDesc{
                    .DebugName = "Environment proposal CDF",
                    .Width = EnvironmentSamplingProposal::Cells + 1, .Height = 1,
                    .Format = nvrhi::Format::R32_FLOAT }, ActiveEnvironmentCdf,
                    nvrhi::ResourceStates::ShaderResource, nvrhi::ResourceStates::ShaderResource);
            if (ActiveEnvironment == ActiveIrradiance)
            {
                EnvironmentHandle = IrradianceHandle;
            }
            else
            {
                EnvironmentHandle = Graph.ImportTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "SceneRenderer Active Environment Cubemap",
                        .Width = ActiveEnvironment->getDesc().width,
                        .Height = ActiveEnvironment->getDesc().height,
                        .MipLevels = ActiveEnvironment->getDesc().mipLevels,
                        .ArrayLayers = ActiveEnvironment->getDesc().arraySize,
                        .Format = ActiveEnvironment->getDesc().format,
                        .Dimension = nvrhi::TextureDimension::TextureCube,
                    },
                    ActiveEnvironment,
                    nvrhi::ResourceStates::ShaderResource,
                    nvrhi::ResourceStates::ShaderResource);
            }
            if (ActivePrefilter == ActiveIrradiance)
            {
                PrefilterHandle = IrradianceHandle;
            }
            else
            {
                PrefilterHandle = Graph.ImportTexture(
                    RenderGraphTextureDesc{
                        .DebugName = "SceneRenderer Active Prefilter Cubemap",
                        .Width = ActivePrefilter->getDesc().width,
                        .Height = ActivePrefilter->getDesc().height,
                        .MipLevels = ActivePrefilter->getDesc().mipLevels,
                        .ArrayLayers = ActivePrefilter->getDesc().arraySize,
                        .Format = ActivePrefilter->getDesc().format,
                        .Dimension = nvrhi::TextureDimension::TextureCube,
                    },
                    ActivePrefilter,
                    nvrhi::ResourceStates::ShaderResource,
                    nvrhi::ResourceStates::ShaderResource);
            }
            BrdfLutHandle = Graph.ImportTexture(
                RenderGraphTextureDesc{
                    .DebugName = "SceneRenderer Active BRDF Integration LUT",
                    .Width = ActiveBrdfLut->getDesc().width,
                    .Height = ActiveBrdfLut->getDesc().height,
                    .MipLevels = ActiveBrdfLut->getDesc().mipLevels,
                    .ArrayLayers = ActiveBrdfLut->getDesc().arraySize,
                    .Format = ActiveBrdfLut->getDesc().format,
                    .Dimension = nvrhi::TextureDimension::Texture2D,
                },
                ActiveBrdfLut,
                ActiveBrdfLut == DefaultBrdfLut && !bDefaultEnvironmentUploaded
                    ? nvrhi::ResourceStates::Common
                    : nvrhi::ResourceStates::ShaderResource,
                nvrhi::ResourceStates::ShaderResource);

            ImportGeometryArenaPages();
            for (const SubMesh* key : MeshOrder)
            {
                MeshEntry& mesh = MeshCache.at(key);
                mesh.GraphIndex = static_cast<uint32_t>(GraphGeometryResources.size());
                if (mesh.ArenaAllocation)
                {
                    const auto& page = GraphArenaPages.at(mesh.ArenaAllocation.PageIndex);
                    GraphGeometryResources.push_back({ .Key = key, .VertexBuffer = page.Vertices, .IndexBuffer = page.Indices });
                    continue;
                }
                const nvrhi::ResourceStates initialVertexState = mesh.bUploaded
                    ? nvrhi::ResourceStates::VertexBuffer
                    : nvrhi::ResourceStates::Common;
                const nvrhi::ResourceStates initialIndexState = mesh.bUploaded
                    ? nvrhi::ResourceStates::IndexBuffer
                    : nvrhi::ResourceStates::Common;
                GraphGeometryResources.push_back(GraphGeometry{
                    .Key = key,
                    .VertexBuffer = Graph.ImportBuffer(
                        RenderGraphBufferDesc{
                            .DebugName = mesh.bSkinned
                                ? "SceneRenderer Skinned Mesh Vertices"
                                : "SceneRenderer Static Mesh Vertices",
                            .ByteSize = mesh.bSkinned
                                ? static_cast<uint64_t>(mesh.Resource->mSkinnedVertices.size()) *
                                    sizeof(SkinnedVertex)
                                : static_cast<uint64_t>(mesh.Resource->mStaticVertices.size()) *
                                    sizeof(StaticVertex),
                            .StructStride = static_cast<uint32_t>(mesh.bSkinned
                                ? sizeof(SkinnedVertex)
                                : sizeof(StaticVertex)),
                        },
                        mesh.VertexBuffer,
                        initialVertexState,
                        nvrhi::ResourceStates::VertexBuffer),
                    .IndexBuffer = Graph.ImportBuffer(
                        RenderGraphBufferDesc{
                            .DebugName = "SceneRenderer Static Mesh Indices",
                            .ByteSize = static_cast<uint64_t>(mesh.ResidentIndices.size()) *
                                sizeof(uint32_t),
                            .StructStride = sizeof(uint32_t),
                        },
                        mesh.IndexBuffer,
                        initialIndexState,
                        nvrhi::ResourceStates::IndexBuffer),
                });
            }

            LightGraphResources = LightGrid->AddPasses(Graph);
            AddSceneUploadPass();
            if (!IsOfflineViewportRenderingActive())
            {
                ShadowGpuGraphViews = ShadowGpuSubmission->AddPasses(Graph, GpuBoundsHandle, bGpuShadowBatchingEnabled);
                AddLocalShadowPasses();
            }
            CurrentRtGraphResources = {};
            CurrentSurfaceData = {};
            RasterSurfaceTargets = {};
            CurrentAmbientOcclusion = {};
            CurrentDepthPyramid = {};
            CurrentGiOutputs = {};
            OpaqueDirectHdrHandle = {};
            ToneMappedSceneHandle = {};
            if (HardwareRayTracingScene && HardwareRayTracingScene->IsReady())
            {
                CurrentRtGraphResources =
                    HardwareRayTracingScene->AddBuildPasses(Graph);
            }
            if (!IsOfflineViewportRenderingActive()) AddDirectionalShadowPass();
            AddGpuCullingPass();
            DepthCompactedArgumentsHandle = bGpuBatchingEnabled
                ? DepthIndirectCompaction->AddPasses(Graph, IndirectArgumentsHandle) : BufferHandle{};
            AddDepthPrepass();
            if (IsMsaaActive())
                AddDepthPrepass(true);
            if (bGpuOcclusionEnabled)
                GpuOcclusionPass->AddPasses(Graph, SceneDepthHandle, frame.Width, frame.Height,
                    GpuBoundsHandle, GpuDrawDataHandle, VisibleObjectIndicesHandle, IndirectArgumentsHandle);
            MainCompactedArgumentsHandle = bGpuBatchingEnabled
                ? MainIndirectCompaction->AddPasses(Graph, IndirectArgumentsHandle) : BufferHandle{};
            // Read the final arguments after optional occlusion. The copy stays
            // asynchronous and is mapped only after its submission completes.
            AddGpuCullingReadbackPass();
            if (SurfaceDataPassInstance)
            {
                CurrentSurfaceData = SurfaceDataPassInstance->AddPasses(
                    Graph,
                    SceneDepthHandle,
                    CurrentRtGraphResources);
            }
            if (!IsOfflineViewportRenderingActive())
            {
            if (Path == RenderingPath::Deferred || RequiresSceneSurfaceData() || AmbientOcclusionSettings.bEnabled ||
                ReflectionSettings.bScreenSpaceEnabled || ReflectionSettings.bLocalProbesEnabled)
            {
                AddRasterSurfacePass();
                if (RequiresSceneSurfaceData())
                    AddRasterSurfaceUnpackPass();
            }
            CurrentAmbientOcclusion = AmbientOcclusionPass->AddPasses(
                Graph, SceneDepthHandle, RasterSurfaceTargets[1]);
            if (Path == RenderingPath::Deferred)
                AddDeferredLightingPass();
            else
                AddOpaquePass();
            RasterLitHdrHandle = SceneColorHandle;
            AddSkyPass();
            if (Path == RenderingPath::Forward)
                AddSkinnedPass();
            }
            if (DepthPyramidPassInstance)
            {
                CurrentDepthPyramid = DepthPyramidPassInstance->AddPasses(
                    Graph,
                    SceneDepthHandle,
                    frame.Width,
                    frame.Height);
            }
            OpaqueDirectHdrHandle = IsMsaaActive() && RequiresGiInput(
                    GlobalIlluminationTechniqueRequiredInput::OpaqueDirectHdr)
                ? AddTextureResolvePass(
                    Graph,
                    SceneColorHandle,
                    "SSGI Opaque Direct HDR")
                : SceneColorHandle;
            if (IsOfflineViewportRenderingActive())
                SceneColorHandle = Graph.CreateTexture(RenderGraphTextureDesc{
                    .DebugName = "Offline viewport HDR composition", .Width = LastWidth, .Height = LastHeight,
                    .Format = c_SceneColorFormat, .bRelativeToGraph = true});
            if (IsGlobalIlluminationActive())
                AddGlobalIlluminationPasses();
            RenderFeatureBuildContext featureContext{ Graph, SceneColorHandle, SceneDepthHandle,
                frame.Width, frame.Height, IsOfflineViewportRenderingActive(), IsMsaaActive(),
                CurrentPbrDebugMode != ScenePbrDebugMode::Disabled ||
                    EffectiveGiSettings.DebugView != GlobalIlluminationDebugView::Composite,
                [this]() { AddEditorOutlinePass(); }, PostProcessPass.get() };
            featureContext.Normal = CurrentSurfaceData.NormalRoughness;
            featureContext.MotionVector = CurrentSurfaceData.MotionVectors;
            featureContext.bDepthJittered = IsTemporalRasterActive();
            featureContext.bSurfaceInputsJittered = IsTemporalRasterActive();
            featureContext.bJittered = featureContext.bDepthJittered;
            if (IsOfflineViewportRenderingActive())
                Features->Build(RenderFeatureStage::SceneOverlay, featureContext);
            if (!IsOfflineViewportRenderingActive() &&
                CurrentPbrDebugMode == ScenePbrDebugMode::Disabled &&
                EffectiveGiSettings.DebugView == GlobalIlluminationDebugView::Composite)
            {
                auto ao = CurrentAmbientOcclusion;
                if (!ao)
                    ao = Graph.ImportTexture(RenderGraphTextureDesc{
                        .DebugName = "Neutral reflection AO", .Width = 1, .Height = 1,
                        .Format = TextureManagerInstance.GetDefaultTexture(DefaultBindlessTexture::White).Texture->getDesc().format,
                    }, TextureManagerInstance.GetDefaultTexture(DefaultBindlessTexture::White).Texture,
                        nvrhi::ResourceStates::ShaderResource, nvrhi::ResourceStates::ShaderResource);
                SceneColorHandle = ReflectionPass->AddPasses(Graph, SceneColorHandle, SceneDepthHandle,
                    RasterSurfaceTargets[0], RasterSurfaceTargets[1], RasterSurfaceTargets[2],
                    ao, PrefilterHandle, BrdfLutHandle, IrradianceHandle);
            }
            if (IsFsrActive() || (Features->RequiredInputs() & uint32_t(RenderFeatureInput::OpaqueColor)) != 0)
            {
                const auto source = IsMsaaActive()
                    ? AddTextureResolvePass(Graph, SceneColorHandle, "Feature Opaque Color Resolve") : SceneColorHandle;
                featureContext.OpaqueColor = Graph.CreateTexture(RenderGraphTextureDesc{
                    .DebugName = "Feature Opaque Color Snapshot", .Width = frame.Width,
                    .Height = frame.Height, .Format = c_SceneColorFormat });
                Graph.AddPass<RuntimeUiBackdropCopyPassData>("FeatureOpaqueColorSnapshot", RenderGraphQueue::Copy,
                    [&](RenderGraphBuilder& builder, RuntimeUiBackdropCopyPassData& data) {
                        data.Source = builder.CopySource(source);
                        data.Destination = builder.CopyDest(featureContext.OpaqueColor);
                    },
                    [](RenderGraphContext& context, const RuntimeUiBackdropCopyPassData& data) {
                        context.GetCommandList()->copyTexture(context.GetTexture(data.Destination, RenderGraphAccess::Write),
                            nvrhi::TextureSlice(), context.GetTexture(data.Source, RenderGraphAccess::Read), nvrhi::TextureSlice());
                    });
            }
            if (IsTaaActive())
            {
                SceneColorHandle = SceneAntiAliasing.AddTaaPasses(
                    Graph,
                    SceneTaaPassInputs{
                        .CurrentHdrColor = SceneColorHandle,
                        .MotionVectors = CurrentSurfaceData.MotionVectors,
                        .CurrentDepth = SceneDepthHandle,
                        .ViewDepth = CurrentSurfaceData.ViewDepth,
                        .NormalRoughness = CurrentSurfaceData.NormalRoughness,
                        .SurfaceId = CurrentSurfaceData.SurfaceId,
                        .TemporalGiConfidence =
                            EffectiveGiSettings.Denoiser ==
                                    GlobalIlluminationDenoiserKind::Svgf
                                ? CurrentGiOutputs.Confidence
                                : TextureHandle{},
                    });
            }
            if (!IsOfflineViewportRenderingActive())
            {
                featureContext.RasterDepth = RasterSceneDepthHandle;
                Features->Build(RenderFeatureStage::TemporalComposite, featureContext);
                if (bPostTaaDepthRequired)
                {
                    Graph.AddPass<BufferHandle>("PostTaaUnjitteredFrame", RenderGraphQueue::Graphics,
                        [this](RenderGraphBuilder& builder, BufferHandle& data)
                        {
                            // This read orders the version publication after
                            // temporal/color composition; the frame write also
                            // depends on every prior raster reader.
                            (void)builder.ShaderResource(SceneColorHandle);
                            data = builder.CopyDest(FrameBufferHandle);
                        },
                        [this](RenderGraphContext& context, const BufferHandle& data)
                        {
                            (void)context.GetBuffer(data, RenderGraphAccess::Write);
                            auto overlayFrame = FrameData;
                            overlayFrame.ViewProjection = CurrentRasterCamera.NonJitteredViewProjection;
                            overlayFrame.InverseViewProjection = glm::inverse(overlayFrame.ViewProjection);
                            overlayFrame.PreviousViewProjection = PreviousTaaNonJitteredViewProjection;
                            context.GetCommandList()->writeBuffer(FrameConstantBuffer, &overlayFrame, sizeof(overlayFrame));
                        });
                    AddDepthPrepass(false, true);
                }
                if (IsFsrActive()) UpscalingPass->BeginTransparency(Graph, SceneColorHandle);
                featureContext.bTransparencyCoverageInAlpha = IsFsrActive();
                AddPrimitive2DPass();
                AddTransparentPass();
                featureContext.Depth = RasterSceneDepthHandle;
                featureContext.bJittered = IsTemporalRasterActive() && !bPostTaaDepthRequired;
                featureContext.bDepthJittered = featureContext.bJittered;
                Features->Build(RenderFeatureStage::SceneOverlay, featureContext);
                if (IsMsaaActive())
                {
                    SceneColorHandle = AddTextureResolvePass(
                        Graph,
                        SceneColorHandle,
                        "SceneRenderer Resolved HDR Scene Color");
                }
                const EffekseerRenderGraphPassResult vfxResult =
                    VfxRenderGraphPass.AddToGraph(
                        Graph,
                        EffekseerRenderGraphInputs{
                            .SceneColor = SceneColorHandle,
                            .SceneDepth = bPostTaaDepthRequired ? RasterSceneDepthHandle : SceneDepthHandle,
                            .Width = frame.Width,
                            .Height = frame.Height,
                            .View = CurrentVfxView.View,
                            .Projection = CurrentVfxView.Projection,
                            .CameraPosition = CurrentVfxView.CameraPosition,
                            .ZNear = CurrentVfxView.ZNear,
                            .ZFar = CurrentVfxView.ZFar,
                            .LightDirection = CurrentVfxView.LightDirection,
                            .LightColor = CurrentVfxView.LightColor,
                            .LightAmbientColor = CurrentVfxView.LightAmbientColor,
                        },
                        CurrentVfxRequirements,
                        bRunEmptyEffekseerPassForValidation);
                Stats.bVfxPassAdded = vfxResult.bAdded;
                Stats.bVfxDepthSnapshotAdded = vfxResult.bDepthSnapshotAdded;
                Stats.bVfxBackgroundSnapshotAdded = vfxResult.bBackgroundCopyAdded;
            }
            else
            {
                Stats.bVfxPassAdded = false;
                Stats.bVfxDepthSnapshotAdded = false;
                Stats.bVfxBackgroundSnapshotAdded = false;
            }
            if (IsFsrActive())
                SceneColorHandle = UpscalingPass->AddPasses(Graph, SceneColorHandle, featureContext.OpaqueColor,
                    SceneDepthHandle, CurrentSurfaceData.MotionVectors);
            if (bLinearHdrCaptureEnabled) AddLinearHdrCapturePass();
            featureContext.bTransparencyCoverageInAlpha = false;
            if (IsFsrActive()) { featureContext.Width = LastOutputWidth; featureContext.Height = LastOutputHeight; }
            featureContext.bMsaa = false; // HDR resolve above precedes full-screen features.
            featureContext.Depth = bPostTaaDepthRequired ? RasterSceneDepthHandle : SceneDepthHandle;
            featureContext.bDepthJittered = IsTemporalRasterActive() && !bPostTaaDepthRequired;
            featureContext.bJittered = !IsFsrActive() && featureContext.bDepthJittered;
            Features->Build(RenderFeatureStage::HdrPostProcess, featureContext);
            AddToneMappingPass();
            AddSpatialAntiAliasingPass();
            featureContext.Width = LastOutputWidth;
            featureContext.Height = LastOutputHeight;
            featureContext.DisplayColor = BackBufferHandle;
            Features->Build(RenderFeatureStage::DisplayPostProcess, featureContext);
			if (!bOffscreenOnly)
            {
                if (CurrentSceneOverlayMode == RuntimeUiRenderMode::Offscreen)
                    AddRuntimeUiOffscreenPasses(outputFormat);
                else
				    AddSceneOverlayPass();
            }
            if (bCopyToOffscreen)
                AddOffscreenCopyPass();
            if (!bOffscreenOnly)
            {
				AddWindowOverlayPass();
            }
			AddCapturePass();
			if (!bOffscreenOnly)
				AddPresentPass();
            Graph.CompileOrThrow();

            LastBackBufferFormat = outputFormat;
            LastBackBufferInitialState = frame.BackBufferInitialState;
            LastOutputMode = frame.OutputMode;
			LastSceneOverlayMode = frame.SceneOverlayMode;
			bLastReuseOffscreenScene = false;
            bGraphDirty = false;
            Stats.CompiledPasses = Graph.GetCompiledPassNames();
        }

        void AddSceneUploadPass()
        {
            Graph.AddPass<SceneUploadPassData>(
                "SceneUpload",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, SceneUploadPassData& data)
                {
                    // The shared depth/outline frame binding includes b1 even
                    // when lighting is not sampled. Keep its upload alive;
                    // unused clustering still gets culled by the graph.
                    (void)builder.ConstantBuffer(LightGraphResources.Constants);
                    data.FrameConstantsBuffer = builder.CopyDest(FrameBufferHandle);
                    for (const GraphGeometry& geometry : GraphGeometryResources)
                    {
                        const MeshEntry& mesh = MeshCache.at(geometry.Key);
                        if (!mesh.bUploaded || mesh.bDynamicVertices)
                        {
                            data.VertexBuffers.push_back(builder.CopyDest(geometry.VertexBuffer));
                            data.IndexBuffers.push_back(builder.CopyDest(geometry.IndexBuffer));
                        }
                    }
                    if (!bDefaultEnvironmentUploaded &&
                        ActiveIrradiance == DefaultEnvironment)
                    {
                        data.DefaultEnvironment = builder.CopyDest(IrradianceHandle);
                        data.DefaultBrdfLut = builder.CopyDest(BrdfLutHandle);
                    }
                    data.GpuObjects = builder.CopyDest(GpuObjectsHandle);
                    data.GpuMaterials = builder.CopyDest(GpuMaterialsHandle);
                    data.MaterialParameters = builder.CopyDest(MaterialParametersHandle);
                    data.GpuBounds = builder.CopyDest(GpuBoundsHandle);
                    data.GpuDrawData = builder.CopyDest(GpuDrawDataHandle);
                    data.IndirectArguments = builder.CopyDest(IndirectArgumentsHandle);
                    data.Primitive2DVertices = builder.CopyDest(Primitive2DVerticesHandle);
                    data.BoneMatrices = builder.CopyDest(BoneMatricesHandle);
                    data.PreviousBoneMatrices = builder.CopyDest(PreviousBoneMatricesHandle);
                },
                [this](RenderGraphContext& context, const SceneUploadPassData& data)
                {
                    nvrhi::ICommandList* commandList = context.GetCommandList();
                    (void)context.GetBuffer(data.FrameConstantsBuffer, RenderGraphAccess::Write);
                    commandList->writeBuffer(FrameConstantBuffer, &FrameData, sizeof(FrameData));
                    bFrameConstantsInitialized = true;

                    size_t uploadIndex = 0;
                    for (const GraphGeometry& geometry : GraphGeometryResources)
                    {
                        MeshEntry& mesh = MeshCache.at(geometry.Key);
                        if (mesh.bUploaded && !mesh.bDynamicVertices)
                            continue;
                        const size_t resourceIndex = uploadIndex++;
                        if (mesh.bUploaded && !mesh.DeformationHistory.NeedsUpload(
                            mesh.Resource->GetDeformationRevision(), mesh.Resource->GetDeformationHistoryGeneration()))
                            continue;
                        const bool firstUpload = !mesh.bUploaded;
                        PendingMeshUploads.push_back(geometry.Key);
                        (void)context.GetBuffer(data.VertexBuffers.at(resourceIndex), RenderGraphAccess::Write);
                        (void)context.GetBuffer(data.IndexBuffers.at(resourceIndex), RenderGraphAccess::Write);
                        const auto* staticVertices = &mesh.Resource->mStaticVertices;
                        const auto* skinnedVertices = &mesh.Resource->mSkinnedVertices;
                        if (mesh.bDynamicVertices)
                        {
                            if (mesh.bSkinned)
                            {
                                mesh.DeformationHistory.Stage(std::span<const SkinnedVertex>(*skinnedVertices),
                                    DeformedSkinnedUpload, mesh.Resource->GetDeformationRevision(),
                                    mesh.Resource->GetDeformationHistoryGeneration(), bRasterCameraHistoryReset);
                                skinnedVertices = &DeformedSkinnedUpload;
                            }
                            else
                            {
                                mesh.DeformationHistory.Stage(std::span<const StaticVertex>(*staticVertices),
                                    DeformedStaticUpload, mesh.Resource->GetDeformationRevision(),
                                    mesh.Resource->GetDeformationHistoryGeneration(), bRasterCameraHistoryReset);
                                staticVertices = &DeformedStaticUpload;
                            }
                        }
                        if (mesh.ArenaAllocation)
                        {
                            GeometryArena->WriteAllocation(commandList, mesh.ArenaAllocation,
                                std::as_bytes(std::span(*staticVertices)), mesh.ResidentIndices);
                        }
                        else
                        {
                        commandList->writeBuffer(
                            mesh.VertexBuffer,
                            mesh.bSkinned
                                ? static_cast<const void*>(skinnedVertices->data())
                                : static_cast<const void*>(staticVertices->data()),
                            mesh.bSkinned
                                ? mesh.Resource->mSkinnedVertices.size() * sizeof(SkinnedVertex)
                                : mesh.Resource->mStaticVertices.size() * sizeof(StaticVertex));
                        if (firstUpload) commandList->writeBuffer(
                            mesh.IndexBuffer,
                            mesh.ResidentIndices.data(),
                            mesh.ResidentIndices.size() * sizeof(uint32_t));
                        }
                        mesh.bUploaded = true;
                        mesh.DeformationRevision = mesh.Resource->GetDeformationRevision();
                        ++Stats.UploadedMeshCount;
                        if (firstUpload)
                        {
                            bGraphDirty = true;
                            PendingGraphBuildReasonMask |= SceneGraphRebuildReason::MeshUploaded;
                        }
                    }

                    if (!bDefaultEnvironmentUploaded &&
                        ActiveIrradiance == DefaultEnvironment)
                    {
                        (void)context.GetTexture(
                            data.DefaultEnvironment,
                            RenderGraphAccess::Write);
                        (void)context.GetTexture(data.DefaultBrdfLut, RenderGraphAccess::Write);
                        constexpr std::array<float, 4> c_EmptyEnvironment = {
                            0.0f, 0.0f, 0.0f, 1.0f
                        };
                        constexpr std::array<float, 2> c_EmptyBrdf = { 1.0f, 0.0f };
                        for (uint32_t face = 0; face < 6; ++face)
                        {
                            commandList->writeTexture(
                                DefaultEnvironment,
                                face,
                                0,
                                c_EmptyEnvironment.data(),
                                sizeof(c_EmptyEnvironment),
                                sizeof(c_EmptyEnvironment));
                        }
                        commandList->writeTexture(
                            DefaultBrdfLut,
                            0,
                            0,
                            c_EmptyBrdf.data(),
                            sizeof(c_EmptyBrdf),
                            sizeof(c_EmptyBrdf));
                        bDefaultEnvironmentUploaded = true;
                        bGraphDirty = true;
                    }

                    (void)context.GetBuffer(data.GpuObjects, RenderGraphAccess::Write);
                    (void)context.GetBuffer(data.GpuMaterials, RenderGraphAccess::Write);
                    (void)context.GetBuffer(data.MaterialParameters, RenderGraphAccess::Write);
                    (void)context.GetBuffer(data.GpuBounds, RenderGraphAccess::Write);
                    (void)context.GetBuffer(data.GpuDrawData, RenderGraphAccess::Write);
                    (void)context.GetBuffer(data.IndirectArguments, RenderGraphAccess::Write);
                    (void)context.GetBuffer(data.Primitive2DVertices, RenderGraphAccess::Write);
                    (void)context.GetBuffer(data.BoneMatrices, RenderGraphAccess::Write);
                    (void)context.GetBuffer(data.PreviousBoneMatrices, RenderGraphAccess::Write);
                    if (!GpuObjectData.empty())
                    {
                        UploadSceneRows(commandList, GpuObjectBuffer, GpuObjectData);
                        UploadSceneRows(commandList, GpuBoundsBuffer, GpuBoundsDataCpu);
                        UploadSceneRows(commandList, GpuDrawDataBuffer, GpuDrawDataCpu);
                        commandList->writeBuffer(
                            IndirectArgumentsBuffer,
                            GpuIndirectArgumentsCpu.data(),
                            GpuIndirectArgumentsCpu.size() *
                                sizeof(nvrhi::DrawIndexedIndirectArguments));
                        Stats.SceneTableUploadedBytes += GpuIndirectArgumentsCpu.size() *
                            sizeof(nvrhi::DrawIndexedIndirectArguments);
                    }
                    if (!GpuMaterialDataCpu.empty())
                    {
                        UploadSceneRows(commandList, GpuMaterialBuffer, GpuMaterialDataCpu);
                    }
                    if (!MaterialParameterRowsCpu.empty())
                    {
                        UploadSceneRows(commandList, MaterialParameterBuffer, MaterialParameterRowsCpu);
                    }
                    if (!Primitive2DVertices.empty())
                    {
                        commandList->writeBuffer(
                            Primitive2DBufferSlots.at(CurrentFrameSlot).Vertices,
                            Primitive2DVertices.data(),
                            Primitive2DVertices.size() * sizeof(Primitive2DVertex));
                    }
                    if (!BoneMatricesCpu.empty())
                    {
                        UploadSceneRows(commandList, BoneMatrixBuffer, BoneMatricesCpu);
                        UploadSceneRows(commandList, PreviousBoneMatrixBuffer, PreviousBoneMatricesCpu);
                    }
                });
        }

        void AddGpuCullingPass()
        {
            Graph.AddPass<GpuCullingPassData>(
                "GpuFrustumCulling",
                RenderGraphQueue::Compute,
                [this](RenderGraphBuilder& builder, GpuCullingPassData& data)
                {
                    data.GpuObjects = builder.ShaderResource(GpuObjectsHandle);
                    data.GpuBounds = builder.ShaderResource(GpuBoundsHandle);
                    data.GpuDrawData = builder.ShaderResource(GpuDrawDataHandle);
                    data.VisibleObjectIndices = builder.UnorderedAccess(VisibleObjectIndicesHandle);
                    data.IndirectArguments = builder.UnorderedAccess(IndirectArgumentsHandle);
                },
                [this](RenderGraphContext& context, const GpuCullingPassData& data)
                {
                    (void)context.GetBuffer(data.GpuObjects, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.GpuBounds, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.GpuDrawData, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.VisibleObjectIndices, RenderGraphAccess::ReadWrite);
                    (void)context.GetBuffer(data.IndirectArguments, RenderGraphAccess::ReadWrite);
                    if (GpuObjectData.empty())
                        return;

                    nvrhi::ICommandList* commandList = context.GetCommandList();
                    nvrhi::ComputeState state;
                    state.setPipeline(GpuCullingPipeline)
                        .addBindingSet(GpuCullingBindingSet);
                    commandList->setComputeState(state);
                    const CullConstants constants{
                        .ViewProjection = FrameData.ViewProjection,
                        .ObjectCount = static_cast<uint32_t>(GpuObjectData.size()),
                    };
                    commandList->setPushConstants(&constants, sizeof(constants));
                    commandList->dispatch(
                        (static_cast<uint32_t>(GpuObjectData.size()) + 63u) / 64u);
                });
        }

        void AddDirectionalShadowPass()
        {
            Graph.AddPass<DirectionalShadowPassData>(
                "DirectionalShadow",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, DirectionalShadowPassData& data)
                {
                    data.ShadowDepth = builder.CreateTexture(RenderGraphTextureDesc{
                        .DebugName = "SceneRenderer Directional Shadow Cascades",
                        .Width = DirectionalShadowMapSize,
                        .Height = DirectionalShadowMapSize,
                        .ArrayLayers = DirectionalShadowCascadeCount,
                        .Format = c_SceneDepthFormat,
                        .Dimension = nvrhi::TextureDimension::Texture2DArray,
                        .ClearValue = nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f),
                        .bUseClearValue = true,
                    });
                    for (uint32_t cascade = 0;
                        cascade < DirectionalShadowCascadeCount;
                        ++cascade)
                    {
                        data.ShadowDepth = builder.DepthWrite(
                            data.ShadowDepth,
                            RenderGraphSubresourceRange{
                                .BaseMipLevel = 0,
                                .MipLevelCount = 1,
                                .BaseArraySlice = cascade,
                                .ArraySliceCount = 1,
                            });
                    }
                    DeclareGpuShadowResources(builder, 0, 4);
                    // The shared frame/object binding sets include these even
                    // when the shadow shader does not read every binding.
                    data.FrameConstantsBuffer = builder.ConstantBuffer(FrameBufferHandle);
                    data.LightConstantsBuffer = builder.ConstantBuffer(LightGraphResources.Constants);
                    data.LightRecords = builder.ShaderResource(LightGraphResources.Lights);
                    data.ClusterMasks = builder.ShaderResource(LightGraphResources.ClusterMasks);
                    data.ObjectConstantsBuffer = builder.ConstantBuffer(ObjectBufferHandle);
                    data.BoneMatrices = builder.ShaderResource(BoneMatricesHandle);
                    data.PreviousBoneMatrices = builder.ShaderResource(PreviousBoneMatricesHandle);
                    data.GpuMaterials = builder.ShaderResource(GpuMaterialsHandle);
                    data.MaterialParameters = builder.ShaderResource(MaterialParametersHandle);
                    for (const GraphMaterialTexture& texture : GraphMaterialTextures)
                        data.MaterialTextures.push_back(builder.ShaderResource(texture.Handle));
                    for (const GraphGeometry& geometry : GraphGeometryResources)
                    {
                        data.VertexBuffers.push_back(builder.VertexBuffer(geometry.VertexBuffer));
                        data.IndexBuffers.push_back(builder.IndexBuffer(geometry.IndexBuffer));
                    }
                    DirectionalShadowHandle = data.ShadowDepth;
                },
                [this](RenderGraphContext& context, const DirectionalShadowPassData& data)
                {
                    RecordDirectionalShadowPass(context, data);
                });
        }

        void AddDepthPrepass(bool bMultisample = false, bool bPostTaa = false)
        {
            Graph.AddPass<DepthPrepassPassData>(
                bPostTaa ? "PostTaaUnjitteredDepth" : bMultisample ? "SceneDepthPrepassMSAA" : "SceneDepthPrepass",
                RenderGraphQueue::Graphics,
                [this, bMultisample, bPostTaa](
                    RenderGraphBuilder& builder,
                    DepthPrepassPassData& data)
                {
                    data.bPostTaa = bPostTaa;
                    data.SceneDepth = !bPostTaa && !bMultisample &&
                            CurrentOutputMode == SceneRendererOutputMode::OffscreenOnly
                        ? OffscreenDepthHandle
                        : builder.CreateTexture(RenderGraphTextureDesc{
                            .DebugName = bPostTaa ? "Post TAA unjittered occlusion depth" : bMultisample
                                ? "SceneRenderer MSAA Scene Depth"
                                : "SceneRenderer Scene Depth",
                            .Width = LastWidth,
                            .Height = LastHeight,
                            .SampleCount = bMultisample
                                ? GetRasterSampleCount()
                                : 1u,
                            .Format = c_SceneDepthFormat,
                            .ClearValue = nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f),
                            .bUseClearValue = true,
                            .bRelativeToGraph = true,
                        });
                    data.SceneDepth = builder.DepthWrite(data.SceneDepth);
                    data.FrameConstantsBuffer = builder.ConstantBuffer(FrameBufferHandle);
                    data.ObjectConstantsBuffer = builder.ConstantBuffer(ObjectBufferHandle);
                    data.GpuObjects = builder.ShaderResource(GpuObjectsHandle);
                    data.GpuMaterials = builder.ShaderResource(GpuMaterialsHandle);
                    data.MaterialParameters = builder.ShaderResource(MaterialParametersHandle);
                    data.VisibleObjectIndices = builder.VertexBuffer(bPostTaa
                        ? GpuDrawDataHandle : VisibleObjectIndicesHandle);
                    data.IndirectArguments = builder.IndirectArgument(bGpuBatchingEnabled
                        ? DepthCompactedArgumentsHandle : IndirectArgumentsHandle);
                    data.BoneMatrices = builder.ShaderResource(BoneMatricesHandle);
                    for (const GraphMaterialTexture& texture : GraphMaterialTextures)
                        data.MaterialTextures.push_back(builder.ShaderResource(texture.Handle));
                    for (const GraphGeometry& geometry : GraphGeometryResources)
                    {
                        data.VertexBuffers.push_back(builder.VertexBuffer(geometry.VertexBuffer));
                        data.IndexBuffers.push_back(builder.IndexBuffer(geometry.IndexBuffer));
                    }
                    if (bPostTaa || bMultisample)
                        RasterSceneDepthHandle = data.SceneDepth;
                    else
                    {
                        SceneDepthHandle = data.SceneDepth;
                        RasterSceneDepthHandle = data.SceneDepth;
                    }
                    if (!bPostTaa && !bMultisample &&
                        CurrentOutputMode == SceneRendererOutputMode::OffscreenOnly)
                    {
                        builder.Export(data.SceneDepth);
                    }
                },
                [this](RenderGraphContext& context, const DepthPrepassPassData& data)
                {
                    RecordDepthPrepass(context, data);
                });
        }

        #include "SceneRendererRaster.inl"
        #include "SceneRendererShadow.inl"

        void AddOpaquePass()
        {
            Graph.AddPass<OpaquePassData>(
                "OpaqueForwardHDR",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, OpaquePassData& data)
                {
                    AddLightDependencies(builder);
                    data.SceneColor = builder.CreateTexture(RenderGraphTextureDesc{
                        .DebugName = "SceneRenderer HDR Scene Color",
                            .Width = LastWidth,
                            .Height = LastHeight,
                            .SampleCount = GetRasterSampleCount(),
                            .Format = c_SceneColorFormat,
                        .ClearValue = nvrhi::Color(0.004f, 0.008f, 0.016f, 1.0f),
                        .bUseClearValue = true,
                        .bRelativeToGraph = true,
                    });
                    data.SceneColor = builder.RenderTarget(data.SceneColor);
                    data.SceneDepth = builder.DepthRead(RasterSceneDepthHandle);
                    data.FrameConstantsBuffer = builder.ConstantBuffer(FrameBufferHandle);
                    data.ObjectConstantsBuffer = builder.ConstantBuffer(ObjectBufferHandle);
                    data.GpuMaterials = builder.ShaderResource(GpuMaterialsHandle);
                    data.MaterialParameters = builder.ShaderResource(MaterialParametersHandle);
                    for (const GraphMaterialTexture& texture : GraphMaterialTextures)
                        data.MaterialTextures.push_back(builder.ShaderResource(texture.Handle));
                    data.GpuObjects = builder.ShaderResource(GpuObjectsHandle);
                    data.VisibleObjectIndices = builder.VertexBuffer(VisibleObjectIndicesHandle);
                    data.IndirectArguments = builder.IndirectArgument(bGpuBatchingEnabled
                        ? MainCompactedArgumentsHandle : IndirectArgumentsHandle);
                    data.DirectionalShadow = builder.ShaderResource(
                        DirectionalShadowHandle,
                        RenderGraphSubresourceRange{
                            .BaseMipLevel = 0,
                            .MipLevelCount = 1,
                            .BaseArraySlice = 0,
                            .ArraySliceCount = DirectionalShadowCascadeCount,
                        });
                    data.Irradiance = builder.ShaderResource(IrradianceHandle);
                    data.Prefilter = builder.ShaderResource(PrefilterHandle);
                    data.BrdfLut = builder.ShaderResource(BrdfLutHandle);
                    for (const GraphGeometry& geometry : GraphGeometryResources)
                    {
                        data.VertexBuffers.push_back(builder.VertexBuffer(geometry.VertexBuffer));
                        data.IndexBuffers.push_back(builder.IndexBuffer(geometry.IndexBuffer));
                    }
                    SceneColorHandle = data.SceneColor;
                },
                [this](RenderGraphContext& context, const OpaquePassData& data)
                {
                    RecordOpaquePass(context, data);
                });
        }

        void AddSkyPass()
        {
            Graph.AddPass<SkyPassData>(
                "SceneSky",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, SkyPassData& data)
                {
                    data.SceneColor = builder.RenderTarget(SceneColorHandle);
                    data.SceneDepth = builder.DepthRead(RasterSceneDepthHandle);
                    data.Environment = builder.ShaderResource(EnvironmentHandle);
                    data.FrameConstantsBuffer = builder.ConstantBuffer(FrameBufferHandle);
                    SceneColorHandle = data.SceneColor;
                },
                [this](RenderGraphContext& context, const SkyPassData& data)
                {
                    nvrhi::ITexture* sceneColor = context.GetTexture(
                        data.SceneColor,
                        RenderGraphAccess::Write);
                    nvrhi::ITexture* sceneDepth = context.GetTexture(
                        data.SceneDepth,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* environment = context.GetTexture(
                        data.Environment,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(
                        data.FrameConstantsBuffer,
                        RenderGraphAccess::Read);

                    nvrhi::IFramebuffer* framebuffer =
                        GetOrCreateOpaqueFramebuffer(sceneColor, sceneDepth);
                    nvrhi::GraphicsState state;
                    state.setPipeline(GetOrCreateSkyPipeline(framebuffer))
                        .setFramebuffer(framebuffer)
                        .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                            nvrhi::Viewport(
                                static_cast<float>(LastWidth),
                                static_cast<float>(LastHeight))))
                        .addBindingSet(FrameBindingSet)
                        .addBindingSet(GetOrCreateSkyBindingSet(environment));
                    nvrhi::ICommandList* commandList = context.GetCommandList();
                    commandList->setGraphicsState(state);
                    commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
                });
        }

        void AddSkinnedPass()
        {
            Graph.AddPass<SkinnedPassData>(
                "SkinnedForwardHDR",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, SkinnedPassData& data)
                {
                    AddLightDependencies(builder);
                    data.SceneColor = builder.ReadWrite(
                        SceneColorHandle,
                        RenderGraphUsage::RenderTarget);
                    data.SceneDepth = builder.DepthRead(RasterSceneDepthHandle);
                    data.FrameConstantsBuffer = builder.ConstantBuffer(FrameBufferHandle);
                    data.ObjectConstantsBuffer = builder.ConstantBuffer(ObjectBufferHandle);
                    data.GpuMaterials = builder.ShaderResource(GpuMaterialsHandle);
                    data.MaterialParameters = builder.ShaderResource(MaterialParametersHandle);
                    data.BoneMatrices = builder.ShaderResource(BoneMatricesHandle);
                    for (const GraphMaterialTexture& texture : GraphMaterialTextures)
                        data.MaterialTextures.push_back(builder.ShaderResource(texture.Handle));
                    data.DirectionalShadow = builder.ShaderResource(
                        DirectionalShadowHandle,
                        RenderGraphSubresourceRange{
                            .BaseMipLevel = 0,
                            .MipLevelCount = 1,
                            .BaseArraySlice = 0,
                            .ArraySliceCount = DirectionalShadowCascadeCount,
                        });
                    data.Irradiance = builder.ShaderResource(IrradianceHandle);
                    data.Prefilter = builder.ShaderResource(PrefilterHandle);
                    data.BrdfLut = builder.ShaderResource(BrdfLutHandle);
                    for (const GraphGeometry& geometry : GraphGeometryResources)
                    {
                        data.VertexBuffers.push_back(builder.VertexBuffer(geometry.VertexBuffer));
                        data.IndexBuffers.push_back(builder.IndexBuffer(geometry.IndexBuffer));
                    }
                },
                [this](RenderGraphContext& context, const SkinnedPassData& data)
                {
                    nvrhi::ITexture* sceneColor = context.GetTexture(
                        data.SceneColor,
                        RenderGraphAccess::ReadWrite);
                    nvrhi::ITexture* sceneDepth = context.GetTexture(
                        data.SceneDepth,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.FrameConstantsBuffer, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.ObjectConstantsBuffer, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.GpuMaterials, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.MaterialParameters, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.BoneMatrices, RenderGraphAccess::Read);
                    for (TextureHandle texture : data.MaterialTextures)
                        (void)context.GetTexture(texture, RenderGraphAccess::Read);
                    nvrhi::ITexture* shadow = context.GetTexture(
                        data.DirectionalShadow,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* irradiance = context.GetTexture(
                        data.Irradiance,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* prefilter = context.GetTexture(
                        data.Prefilter,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* brdfLut = context.GetTexture(
                        data.BrdfLut,
                        RenderGraphAccess::Read);
                    if (SkinnedDrawItems.empty())
                        return;

                    nvrhi::IFramebuffer* framebuffer = GetOrCreateOpaqueFramebuffer(
                        sceneColor,
                        sceneDepth);
                    nvrhi::IBindingSet* environmentBindingSet =
                        GetOrCreateEnvironmentBindingSet(
                            shadow,
                            irradiance,
                            prefilter,
                            brdfLut);
                    for (const DrawItem& drawItem : SkinnedDrawItems)
                    {
                        const MeshEntry& mesh = MeshCache.at(drawItem.Key);
                        (void)context.GetBuffer(
                            data.VertexBuffers.at(mesh.GraphIndex),
                            RenderGraphAccess::Read);
                        (void)context.GetBuffer(
                            data.IndexBuffers.at(mesh.GraphIndex),
                            RenderGraphAccess::Read);
                        context.GetCommandList()->writeBuffer(
                            ObjectConstantBuffer,
                            &drawItem.Constants,
                            sizeof(drawItem.Constants));
                        nvrhi::GraphicsState state;
                        state.setPipeline(drawItem.MaterialProgram
                                ? GetOrCreateMaterialPipeline(
                                    framebuffer,
                                    *drawItem.MaterialProgram,
                                    MaterialShaderPass::Forward,
                                    true,
                                    drawItem.bDoubleSided,
                                    false)
                                : GetOrCreateSkinnedPipeline(
                                    framebuffer,
                                    false,
                                    drawItem.bDoubleSided))
                            .setFramebuffer(framebuffer)
                            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                                nvrhi::Viewport(
                                    static_cast<float>(LastWidth),
                                    static_cast<float>(LastHeight))))
                            .addBindingSet(GetLightingFrameBindingSet(context))
                            .addBindingSet(ObjectBindingSet)
                            .addBindingSet(environmentBindingSet)
							.addBindingSet(GetMaterialTextureBindingSet(
								drawItem.Constants.MaterialObjectMeshIndices.x))
                            .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                            .addVertexBuffer(nvrhi::VertexBufferBinding()
                                .setBuffer(mesh.VertexBuffer).setSlot(0).setOffset(MeshVertexByteOffset(mesh)))
                            .setIndexBuffer(nvrhi::IndexBufferBinding()
                                .setBuffer(mesh.IndexBuffer)
                                .setFormat(nvrhi::Format::R32_UINT).setOffset(MeshIndexByteOffset(mesh)));
                        context.GetCommandList()->setGraphicsState(state);
                        context.GetCommandList()->drawIndexed(
                            MeshDrawArguments(mesh, drawItem.LodIndex));
                    }
                });
        }

        void AddPrimitive2DPass()
        {
            Graph.AddPass<Primitive2DPassData>(
                "Primitive2D",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, Primitive2DPassData& data)
                {
                    data.SceneColor = builder.ReadWrite(
                        SceneColorHandle,
                        RenderGraphUsage::RenderTarget);
                    data.SceneDepth = builder.DepthRead(RasterSceneDepthHandle);
                    data.FrameConstantsBuffer = builder.ConstantBuffer(FrameBufferHandle);
                    data.Vertices = builder.VertexBuffer(Primitive2DVerticesHandle);
                    for (const GraphMaterialTexture& texture : GraphMaterialTextures)
                        data.Textures.push_back(builder.ShaderResource(texture.Handle));
                },
                [this](RenderGraphContext& context, const Primitive2DPassData& data)
                {
                    nvrhi::ITexture* sceneColor = context.GetTexture(
                        data.SceneColor,
                        RenderGraphAccess::ReadWrite);
                    nvrhi::ITexture* sceneDepth = context.GetTexture(
                        data.SceneDepth,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.FrameConstantsBuffer, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.Vertices, RenderGraphAccess::Read);
                    for (TextureHandle texture : data.Textures)
                        (void)context.GetTexture(texture, RenderGraphAccess::Read);
                    if (Primitive2DVertices.empty())
                        return;

                    nvrhi::ICommandList* commandList = context.GetCommandList();
                    nvrhi::IFramebuffer* framebuffer =
                        GetOrCreateOpaqueFramebuffer(sceneColor, sceneDepth);
                    const Primitive2DConstants constants{
                        .SamplerIndex = TextureManagerInstance.GetMaterialSamplerIndex(),
                    };
                    for (const Primitive2DBatch& batch : Primitive2DBatches)
                    {
                        nvrhi::GraphicsState state;
                        state.setPipeline(GetOrCreatePrimitive2DPipeline(
                                framebuffer,
                                batch.DepthMode))
                            .setFramebuffer(framebuffer)
                            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                                nvrhi::Viewport(
                                    static_cast<float>(LastWidth),
                                    static_cast<float>(LastHeight))))
                            // 2D primitives are unlit. The lighting variant reads
                            // GTAO, which this pass neither declares nor samples.
                            .addBindingSet(FrameBindingSet)
                            .addBindingSet(Primitive2DBindingSet)
							.addBindingSet(TextureManagerInstance.GetTextureBindingSet())
                            .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                            .addVertexBuffer(nvrhi::VertexBufferBinding()
                                .setBuffer(Primitive2DBufferSlots.at(CurrentFrameSlot).Vertices)
                                .setSlot(0)
                                .setOffset(0));
                        commandList->setGraphicsState(state);
                        commandList->setPushConstants(&constants, sizeof(constants));
                        commandList->draw(nvrhi::DrawArguments()
                            .setVertexCount(batch.VertexCount)
                            .setStartVertexLocation(batch.FirstVertex));
                    }
                });
        }

        void AddTransparentPass()
        {
            Graph.AddPass<TransparentPassData>(
                "TransparentForwardHDR",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, TransparentPassData& data)
                {
                    AddLightDependencies(builder);
                    data.SceneColor = builder.ReadWrite(
                        SceneColorHandle,
                        RenderGraphUsage::RenderTarget);
                    data.SceneDepth = builder.DepthRead(RasterSceneDepthHandle);
                    data.FrameConstantsBuffer = builder.ConstantBuffer(FrameBufferHandle);
                    data.ObjectConstantsBuffer = builder.ConstantBuffer(ObjectBufferHandle);
                    data.GpuMaterials = builder.ShaderResource(GpuMaterialsHandle);
                    data.MaterialParameters = builder.ShaderResource(MaterialParametersHandle);
                    data.BoneMatrices = builder.ShaderResource(BoneMatricesHandle);
                    for (const GraphMaterialTexture& texture : GraphMaterialTextures)
                        data.MaterialTextures.push_back(builder.ShaderResource(texture.Handle));
                    data.DirectionalShadow = builder.ShaderResource(
                        DirectionalShadowHandle,
                        RenderGraphSubresourceRange{
                            .BaseMipLevel = 0,
                            .MipLevelCount = 1,
                            .BaseArraySlice = 0,
                            .ArraySliceCount = DirectionalShadowCascadeCount,
                        });
                    data.Irradiance = builder.ShaderResource(IrradianceHandle);
                    data.Prefilter = builder.ShaderResource(PrefilterHandle);
                    data.BrdfLut = builder.ShaderResource(BrdfLutHandle);
                    for (const GraphGeometry& geometry : GraphGeometryResources)
                    {
                        data.VertexBuffers.push_back(builder.VertexBuffer(geometry.VertexBuffer));
                        data.IndexBuffers.push_back(builder.IndexBuffer(geometry.IndexBuffer));
                    }
                },
                [this](RenderGraphContext& context, const TransparentPassData& data)
                {
                    nvrhi::ITexture* sceneColor = context.GetTexture(
                        data.SceneColor,
                        RenderGraphAccess::ReadWrite);
                    nvrhi::ITexture* sceneDepth = context.GetTexture(
                        data.SceneDepth,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.FrameConstantsBuffer, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.ObjectConstantsBuffer, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.GpuMaterials, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.MaterialParameters, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.BoneMatrices, RenderGraphAccess::Read);
                    for (TextureHandle texture : data.MaterialTextures)
                        (void)context.GetTexture(texture, RenderGraphAccess::Read);
                    nvrhi::ITexture* shadow = context.GetTexture(
                        data.DirectionalShadow,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* irradiance = context.GetTexture(
                        data.Irradiance,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* prefilter = context.GetTexture(
                        data.Prefilter,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* brdfLut = context.GetTexture(
                        data.BrdfLut,
                        RenderGraphAccess::Read);
                    if (TransparentDrawItems.empty())
                        return;

                    nvrhi::IFramebuffer* framebuffer = GetOrCreateOpaqueFramebuffer(
                        sceneColor,
                        sceneDepth);
                    nvrhi::IBindingSet* environmentBindingSet =
                        GetOrCreateEnvironmentBindingSet(
                            shadow,
                            irradiance,
                            prefilter,
                            brdfLut);
                    for (const DrawItem& drawItem : TransparentDrawItems)
                    {
                        const MeshEntry& mesh = MeshCache.at(drawItem.Key);
                        if (mesh.GraphIndex >= data.VertexBuffers.size())
                            throw std::logic_error("Transparent mesh graph index is stale");
                        (void)context.GetBuffer(
                            data.VertexBuffers[mesh.GraphIndex],
                            RenderGraphAccess::Read);
                        (void)context.GetBuffer(
                            data.IndexBuffers[mesh.GraphIndex],
                            RenderGraphAccess::Read);
                        context.GetCommandList()->writeBuffer(
                            ObjectConstantBuffer,
                            &drawItem.Constants,
                            sizeof(drawItem.Constants));

                        nvrhi::GraphicsState state;
                        state.setPipeline(drawItem.MaterialProgram
                                ? GetOrCreateMaterialPipeline(
                                    framebuffer,
                                    *drawItem.MaterialProgram,
                                    MaterialShaderPass::Forward,
                                    drawItem.bSkinned,
                                    drawItem.bDoubleSided,
                                    true)
                                : drawItem.bSkinned
                                    ? GetOrCreateSkinnedPipeline(
                                        framebuffer,
                                        true,
                                        drawItem.bDoubleSided)
                                    : GetOrCreateTransparentPipeline(
                                        framebuffer,
                                        drawItem.bDoubleSided))
                            .setFramebuffer(framebuffer)
                            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                                nvrhi::Viewport(
                                    static_cast<float>(LastWidth),
                                    static_cast<float>(LastHeight))))
                            .addBindingSet(GetLightingFrameBindingSet(context))
                            .addBindingSet(ObjectBindingSet)
                            .addBindingSet(environmentBindingSet)
							.addBindingSet(GetMaterialTextureBindingSet(
								drawItem.Constants.MaterialObjectMeshIndices.x))
                            .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                            .addVertexBuffer(nvrhi::VertexBufferBinding()
                                .setBuffer(mesh.VertexBuffer)
                                .setSlot(0)
                                .setOffset(MeshVertexByteOffset(mesh)))
                            .setIndexBuffer(nvrhi::IndexBufferBinding()
                                .setBuffer(mesh.IndexBuffer)
                                .setFormat(nvrhi::Format::R32_UINT)
                                .setOffset(MeshIndexByteOffset(mesh)));
                        context.GetCommandList()->setGraphicsState(state);
                        context.GetCommandList()->drawIndexed(
                            MeshDrawArguments(mesh, drawItem.LodIndex));
                    }
                });
        }

        void AddEditorOutlinePass()
        {
            Graph.AddPass<OutlinePassData>(
                "EditorOutline",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, OutlinePassData& data)
                {
                    data.SceneColor = builder.ColorAttachment(
                        SceneColorHandle, RenderGraphAttachmentOps{});
                    data.SceneDepth = builder.DepthRead(RasterSceneDepthHandle);
                    data.FrameConstantsBuffer = builder.ConstantBuffer(FrameBufferHandle);
                    data.ObjectConstantsBuffer = builder.ConstantBuffer(ObjectBufferHandle);
                    data.BoneMatrices = builder.ShaderResource(BoneMatricesHandle);
                    data.GpuMaterials = builder.ShaderResource(GpuMaterialsHandle);
                    data.MaterialParameters = builder.ShaderResource(MaterialParametersHandle);
                    for (const GraphMaterialTexture& texture : GraphMaterialTextures)
                        data.MaterialTextures.push_back(builder.ShaderResource(texture.Handle));
                    for (const GraphGeometry& geometry : GraphGeometryResources)
                    {
                        data.VertexBuffers.push_back(builder.VertexBuffer(geometry.VertexBuffer));
                        data.IndexBuffers.push_back(builder.IndexBuffer(geometry.IndexBuffer));
                    }
                },
                [this](RenderGraphContext& context, const OutlinePassData& data)
                {
                    nvrhi::ITexture* sceneColor = context.GetTexture(
                        data.SceneColor,
                        RenderGraphAccess::ReadWrite);
                    nvrhi::ITexture* sceneDepth = context.GetTexture(
                        data.SceneDepth,
                        RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.FrameConstantsBuffer, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.ObjectConstantsBuffer, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.BoneMatrices, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.GpuMaterials, RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.MaterialParameters, RenderGraphAccess::Read);
                    for (TextureHandle texture : data.MaterialTextures)
                        (void)context.GetTexture(texture, RenderGraphAccess::Read);
                    if (OutlineDrawItems.empty())
                        return;

                    nvrhi::ICommandList* commandList = context.GetCommandList();
                    nvrhi::IFramebuffer* framebuffer = GetOrCreateOpaqueFramebuffer(
                        sceneColor,
                        sceneDepth);
                    for (const DrawItem& drawItem : OutlineDrawItems)
                    {
                        const MeshEntry& mesh = MeshCache.at(drawItem.Key);
                        if (mesh.GraphIndex >= data.VertexBuffers.size())
                            throw std::logic_error("Outline mesh graph index is stale");
                        (void)context.GetBuffer(
                            data.VertexBuffers[mesh.GraphIndex],
                            RenderGraphAccess::Read);
                        (void)context.GetBuffer(
                            data.IndexBuffers[mesh.GraphIndex],
                            RenderGraphAccess::Read);
                        commandList->writeBuffer(
                            ObjectConstantBuffer,
                            &drawItem.Constants,
                            sizeof(drawItem.Constants));

                        nvrhi::GraphicsState state;
                        const bool bCustomMask = drawItem.MaterialProgram &&
                            drawItem.MaterialProgram->GetManifest().BlendMode ==
                                MaterialShaderBlendMode::Mask;
                        state.setPipeline(bCustomMask
                                ? GetOrCreateMaterialPipeline(
                                    framebuffer,
                                    *drawItem.MaterialProgram,
                                    MaterialShaderPass::Outline,
                                    drawItem.bSkinned,
                                    drawItem.bDoubleSided,
                                    false)
                                : GetOrCreateOutlinePipeline(
                                    framebuffer,
                                    drawItem.bSkinned))
                            .setFramebuffer(framebuffer)
                            .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                                nvrhi::Viewport(
                                    static_cast<float>(LastWidth),
                                    static_cast<float>(LastHeight))))
                            .addBindingSet(FrameBindingSet)
                            .addBindingSet(ObjectBindingSet)
                            .addVertexBuffer(nvrhi::VertexBufferBinding()
                                .setBuffer(mesh.VertexBuffer)
                                .setSlot(0)
                                .setOffset(MeshVertexByteOffset(mesh)))
                            .setIndexBuffer(nvrhi::IndexBufferBinding()
                                .setBuffer(mesh.IndexBuffer)
                                .setFormat(nvrhi::Format::R32_UINT)
                                .setOffset(MeshIndexByteOffset(mesh)));
                        if (bCustomMask)
                        {
							state.addBindingSet(GetMaterialTextureBindingSet(
								drawItem.Constants.MaterialObjectMeshIndices.x))
                                .addBindingSet(TextureManagerInstance.GetSamplerBindingSet());
                        }
                        commandList->setGraphicsState(state);
                        commandList->drawIndexed(
                            MeshDrawArguments(mesh, drawItem.LodIndex));
                    }
                });
        }

        void AddToneMappingPass()
        {
            Graph.AddPass<ToneMappingPassData>(
                "ToneMapping",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, ToneMappingPassData& data)
                {
                    data.SceneColor = builder.ShaderResource(SceneColorHandle);
                    if (IsSpatialPostAntiAliasingActive())
                    {
                        ToneMappedSceneHandle = builder.CreateTexture(
                            RenderGraphTextureDesc{
                                .DebugName =
                                    "SceneRenderer Tone-Mapped AA Input",
                                .Width = LastOutputWidth,
                                .Height = LastOutputHeight,
                                .Format = nvrhi::Format::RGBA8_UNORM,
                            });
                        data.BackBuffer = builder.RenderTarget(
                            ToneMappedSceneHandle);
                    }
                    else
                    {
                        ToneMappedSceneHandle = BackBufferHandle;
                        data.BackBuffer = builder.RenderTarget(
                            BackBufferHandle);
                    }
                    if (!IsSpatialPostAntiAliasingActive() &&
                        CurrentOutputMode ==
                            SceneRendererOutputMode::OffscreenOnly)
                    {
                        builder.Export(data.BackBuffer);
                    }
                },
                [this](RenderGraphContext& context, const ToneMappingPassData& data)
                {
                    RecordToneMappingPass(context, data);
                });
        }

        void AddRuntimeUiOffscreenPasses(nvrhi::Format outputFormat)
        {
            Graph.AddPass<RuntimeUiBackdropCopyPassData>(
                "RuntimeUiBackdropCopy",
                RenderGraphQueue::Copy,
                [this, outputFormat](
                    RenderGraphBuilder& builder,
                    RuntimeUiBackdropCopyPassData& data)
                {
                    data.Source = builder.CopySource(BackBufferHandle);
                    data.Destination = builder.CreateTexture(
                        RenderGraphTextureDesc{
                            .DebugName = "Runtime UI LDR Backdrop",
                            .Width = LastOutputWidth,
                            .Height = LastOutputHeight,
                            .Format = outputFormat,
                        },
                        RenderGraphResourceLifetime::Persistent);
                    data.Destination = builder.CopyDest(data.Destination);
                    RuntimeUiBackdropHandle = data.Destination;
                },
                [](RenderGraphContext& context, const RuntimeUiBackdropCopyPassData& data)
                {
                    nvrhi::ITexture* source = context.GetTexture(
                        data.Source,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* destination = context.GetTexture(
                        data.Destination,
                        RenderGraphAccess::Write);
                    context.GetCommandList()->copyTexture(
                        destination,
                        nvrhi::TextureSlice(),
                        source,
                        nvrhi::TextureSlice());
                });

            const RuntimeUiRenderGraphResult uiResult = RuntimeUiRenderPass.AddToGraph(
                Graph,
                RuntimeUiRenderGraphInputs{
                    .BackBuffer = BackBufferHandle,
                    .Backdrop = RuntimeUiBackdropHandle,
                    .Width = LastOutputWidth,
                    .Height = LastOutputHeight,
                });
            if (!uiResult.bAdded)
                throw std::runtime_error("Failed to add runtime UI offscreen RenderGraph passes");
        }

        void AddPresentPass()
        {
            Graph.AddPass<PresentPassData>(
                "Present",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, PresentPassData& data)
                {
                    data.BackBuffer = builder.Present(BackBufferHandle);
                    builder.Export(data.BackBuffer);
                    builder.SetSideEffect();
                },
                [](RenderGraphContext& context, const PresentPassData& data)
                {
                    (void)context.GetTexture(data.BackBuffer, RenderGraphAccess::Read);
                });
        }

		void AddClearOutputPass()
		{
			Graph.AddPass<ClearOutputPassData>(
				"ClearOutput",
				RenderGraphQueue::Graphics,
				[this](RenderGraphBuilder& builder, ClearOutputPassData& data)
				{
					data.BackBuffer = builder.RenderTarget(BackBufferHandle);
				},
				[this](RenderGraphContext& context, const ClearOutputPassData& data)
				{
					nvrhi::ITexture* backBuffer = context.GetTexture(
						data.BackBuffer,
						RenderGraphAccess::Write);
					context.GetCommandList()->clearTextureFloat(
						backBuffer,
						nvrhi::AllSubresources,
						nvrhi::Color(0.004f, 0.008f, 0.016f, 1.0f));
				});
		}

        void AddSceneOverlayPass()
        {
            Graph.AddPass<OverlayPassData>(
                "SceneUI",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, OverlayPassData& data)
                {
                    data.BackBuffer = builder.ReadWrite(
                        BackBufferHandle,
                        RenderGraphUsage::RenderTarget);
                },
                [this](RenderGraphContext& context, const OverlayPassData& data)
                {
                    nvrhi::ITexture* backBuffer = context.GetTexture(
                        data.BackBuffer,
                        RenderGraphAccess::ReadWrite);
                    if (!CurrentSceneOverlayRecorder)
                        return;

                    CurrentSceneOverlayRecorder(SceneRendererOverlayContext{
                        .CommandList = context.GetCommandList(),
                        .Framebuffer = GetOrCreateToneFramebuffer(backBuffer),
                        .BackBuffer = backBuffer,
                        .Width = LastOutputWidth,
                        .Height = LastOutputHeight,
                        .FrameSlot = CurrentFrameSlot,
                        .CompletedSubmission = CurrentCompletedSubmission,
                    });
                    Stats.bOverlayPassRecorded = true;
                });
        }

        void AddWindowOverlayPass()
        {
            Graph.AddPass<OverlayPassData>(
                "UI",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, OverlayPassData& data)
                {
                    data.BackBuffer = builder.ReadWrite(
                        BackBufferHandle,
                        RenderGraphUsage::RenderTarget);
                    if (CurrentOutputMode == SceneRendererOutputMode::SwapChainAndOffscreen)
                        data.OffscreenColor = builder.ShaderResource(OffscreenColorHandle);
                },
                [this](RenderGraphContext& context, const OverlayPassData& data)
                {
                    nvrhi::ITexture* backBuffer = context.GetTexture(
                        data.BackBuffer,
                        RenderGraphAccess::ReadWrite);
                    if (data.OffscreenColor)
                    {
                        (void)context.GetTexture(
                            data.OffscreenColor,
                            RenderGraphAccess::Read);
                    }
                    if (!CurrentWindowOverlayRecorder)
                        return;

                    CurrentWindowOverlayRecorder(SceneRendererOverlayContext{
                        .CommandList = context.GetCommandList(),
                        .Framebuffer = GetOrCreateToneFramebuffer(backBuffer),
                        .BackBuffer = backBuffer,
                        .Width = LastOutputWidth,
                        .Height = LastOutputHeight,
                        .FrameSlot = CurrentFrameSlot,
                        .CompletedSubmission = CurrentCompletedSubmission,
                    });
                    Stats.bOverlayPassRecorded = true;
                });
        }

        void AddOffscreenCopyPass()
        {
            if (Graph.GetTextureDesc(BackBufferHandle).Format !=
                Graph.GetTextureDesc(OffscreenColorHandle).Format)
                throw RenderGraphValidationError(
                    "The main offscreen output must match the swapchain color format for a raw image copy");
            Graph.AddPass<OffscreenCopyPassData>(
                "OffscreenOutputCopy",
                RenderGraphQueue::Copy,
                [this](RenderGraphBuilder& builder, OffscreenCopyPassData& data)
                {
                    data.ColorSource = builder.CopySource(BackBufferHandle);
                    data.DepthSource = builder.CopySource(SceneDepthHandle);
                    data.ColorDestination = builder.CopyDest(OffscreenColorHandle);
                    data.DepthDestination = builder.CopyDest(OffscreenDepthHandle);
                    builder.Export(data.ColorDestination);
                    builder.Export(data.DepthDestination);
                },
                [this](RenderGraphContext& context, const OffscreenCopyPassData& data)
                {
                    nvrhi::ITexture* colorSource = context.GetTexture(
                        data.ColorSource,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* depthSource = context.GetTexture(
                        data.DepthSource,
                        RenderGraphAccess::Read);
                    nvrhi::ITexture* colorDestination = context.GetTexture(
                        data.ColorDestination,
                        RenderGraphAccess::Write);
                    nvrhi::ITexture* depthDestination = context.GetTexture(
                        data.DepthDestination,
                        RenderGraphAccess::Write);
                    nvrhi::ICommandList* commandList = context.GetCommandList();
                    commandList->copyTexture(
                        colorDestination,
                        nvrhi::TextureSlice(),
                        colorSource,
                        nvrhi::TextureSlice());
                    commandList->copyTexture(
                        depthDestination,
                        nvrhi::TextureSlice(),
                        depthSource,
                        nvrhi::TextureSlice());
                    Stats.bOffscreenOutputRecorded = true;
                });
        }

        void AddLinearHdrCapturePass()
        {
            Graph.AddPass<LinearHdrCapturePassData>(
                "LinearHdrCapture",
                RenderGraphQueue::Copy,
                [this](
                    RenderGraphBuilder& builder,
                    LinearHdrCapturePassData& data)
                {
                    if (CurrentCaptureSource == FrameCaptureSource::Automatic)
                    {
                        data.SceneColor = builder.CopySource(SceneColorHandle);
                        if (NativeGiDiagnosticInput && IsGlobalIlluminationActive())
                            data.DiagnosticInput = builder.CopySource(NativeGiDiagnosticInput);
                    }
                    else
                    {
                        // Read only attachments already produced by this main view.
                        // Do not enable GI, guides, AA or any otherwise unused pass.
                        TextureHandle selected;
                        switch (CurrentCaptureSource)
                        {
                        case FrameCaptureSource::Depth: selected = SceneDepthHandle; break;
                        case FrameCaptureSource::Motion: selected = RasterSurfaceTargets[3]; break;
                        case FrameCaptureSource::Normal: selected = RasterSurfaceTargets[1]; break;
                        case FrameCaptureSource::SurfaceId: selected = RasterSurfaceTargets[4]; break;
                        case FrameCaptureSource::Reactive: selected = UpscalingPass->GetReactiveMask(); break;
                        case FrameCaptureSource::Composition: selected = UpscalingPass->GetCompositionMask(); break;
                        case FrameCaptureSource::UpscaleMotion: selected = UpscalingPass->GetMotionVectors(); break;
                        case FrameCaptureSource::UpscaleColor: selected = UpscalingPass->GetInputColor(); break;
                        case FrameCaptureSource::UpscaleOpaque: selected = UpscalingPass->GetOpaqueColor(); break;
                        case FrameCaptureSource::DirectionalShadow:
                            if (!IsOfflineViewportRenderingActive()) selected = DirectionalShadowHandle;
                            break;
                        default: throw std::invalid_argument("Invalid native capture source");
                        }
                        if (selected) data.SelectedNativeInput = builder.CopySource(selected);
                        // If settings changed while queued, preserve the normal
                        // frame and let the recorder return a narrow capture error.
                        // No fallback attachment or additional effect is created.
                    }
                    builder.SetSideEffect();
                },
                [this](
                    RenderGraphContext& context,
                    const LinearHdrCapturePassData& data)
                {
                    nvrhi::ITexture* sceneColor = data.SceneColor
                        ? context.GetTexture(data.SceneColor, RenderGraphAccess::Read) : nullptr;
                    if (!CurrentLinearHdrCaptureRecorder)
                        return;
                    CurrentLinearHdrCaptureRecorder(
                        SceneRendererLinearHdrCaptureContext{
                            .CommandList = context.GetCommandList(),
                            .LinearHdr = sceneColor,
                            .DiagnosticInput = data.DiagnosticInput
                                ? context.GetTexture(data.DiagnosticInput, RenderGraphAccess::Read) : nullptr,
                            .SelectedNativeInput = data.SelectedNativeInput
                                ? context.GetTexture(data.SelectedNativeInput, RenderGraphAccess::Read) : nullptr,
                            .Width = LastWidth,
                            .Height = LastHeight,
                            .FrameSlot = CurrentFrameSlot,
                            .CompletedSubmission = CurrentCompletedSubmission,
                        });
                    Stats.bLinearHdrCaptureRecorded = true;
                });
        }

        void AddSpatialAntiAliasingPass()
        {
            if (IsFxaaActive())
            {
                BackBufferHandle = SceneAntiAliasing.AddFxaaPass(
                    Graph,
                    ToneMappedSceneHandle,
                    BackBufferHandle,
                    AaSelection.Effective,
                    LastOutputWidth,
                    LastOutputHeight);
            }
            else if (IsSmaaActive())
            {
                BackBufferHandle = SceneAntiAliasing.AddSmaaPasses(
                    Graph,
                    ToneMappedSceneHandle,
                    BackBufferHandle,
                    AaSelection.Effective,
                    LastOutputWidth,
                    LastOutputHeight);
            }
        }

        void AddCapturePass()
        {
            Graph.AddPass<CapturePassData>(
                "Capture",
                RenderGraphQueue::Copy,
                [this](RenderGraphBuilder& builder, CapturePassData& data)
                {
                    data.BackBuffer = builder.CopySource(BackBufferHandle);
                    // Capture is optional, but the graph owns its transition and
                    // ordering even on frames with no pending request.
                    builder.SetSideEffect();
                },
                [this](RenderGraphContext& context, const CapturePassData& data)
                {
                    nvrhi::ITexture* backBuffer = context.GetTexture(
                        data.BackBuffer,
                        RenderGraphAccess::Read);
                    if (!CurrentCaptureRecorder)
                        return;

                    CurrentCaptureRecorder(SceneRendererCaptureContext{
                        .CommandList = context.GetCommandList(),
                        .BackBuffer = backBuffer,
                        .Width = LastOutputWidth,
                        .Height = LastOutputHeight,
                        .FrameSlot = CurrentFrameSlot,
                        .CompletedSubmission = CurrentCompletedSubmission,
                    });
                    Stats.bCapturePassRecorded = true;
                });
        }

        void RecordDepthPrepass(
            RenderGraphContext& context,
            const DepthPrepassPassData& data)
        {
            nvrhi::ICommandList* commandList = context.GetCommandList();
            nvrhi::ITexture* sceneDepth =
                context.GetTexture(data.SceneDepth, RenderGraphAccess::Write);
            (void)context.GetBuffer(data.FrameConstantsBuffer, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.ObjectConstantsBuffer, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.GpuObjects, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.GpuMaterials, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.MaterialParameters, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.VisibleObjectIndices, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.IndirectArguments, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.BoneMatrices, RenderGraphAccess::Read);
            for (TextureHandle texture : data.MaterialTextures)
                (void)context.GetTexture(texture, RenderGraphAccess::Read);

            commandList->clearDepthStencilTexture(
                sceneDepth,
                nvrhi::AllSubresources,
                true,
                1.0f,
                false,
                0);
            nvrhi::IFramebuffer* framebuffer = GetOrCreateShadowFramebuffer(sceneDepth);
            const nvrhi::ViewportState viewport = nvrhi::ViewportState()
                .addViewportAndScissorRect(nvrhi::Viewport(
                    static_cast<float>(LastWidth),
                    static_cast<float>(LastHeight)));

            if (bGpuBatchingEnabled && !data.bPostTaa)
                RecordGroupedGpuGeometry(context, *DepthIndirectCompaction, data.IndirectArguments,
                    [&](const GpuStateGroup& group)
                    {
                        nvrhi::GraphicsState state;
                        state.setPipeline(GetOrCreateDepthPrepassPipeline(framebuffer, true, false, group.bDoubleSided))
                            .setFramebuffer(framebuffer).setViewport(viewport)
                            .addBindingSet(FrameBindingSet).addBindingSet(GpuObjectBindingSet)
                            .addBindingSet(GetMaterialTextureBindingSet(GpuBatches[group.FirstBatch].MaterialIndex))
                            .addBindingSet(TextureManagerInstance.GetSamplerBindingSet());
                        return state;
                    });

            for (uint32_t batchIndex = 0;
                !data.bPostTaa && !bGpuBatchingEnabled && batchIndex < static_cast<uint32_t>(GpuBatches.size());
                ++batchIndex)
            {
                const GpuBatch& batch = GpuBatches[batchIndex];
                const MeshEntry& mesh = MeshCache.at(batch.Key.Mesh);
                if (mesh.GraphIndex >= data.VertexBuffers.size())
                    throw std::logic_error("Depth-prepass GPU mesh graph index is stale");
                (void)context.GetBuffer(
                    data.VertexBuffers[mesh.GraphIndex],
                    RenderGraphAccess::Read);
                (void)context.GetBuffer(
                    data.IndexBuffers[mesh.GraphIndex],
                    RenderGraphAccess::Read);

                nvrhi::GraphicsState state;
                state.setPipeline(GetOrCreateDepthPrepassPipeline(
                        framebuffer,
                        true,
                        false,
                        batch.Key.bDoubleSided))
                    .setFramebuffer(framebuffer)
                    .setViewport(viewport)
                    .addBindingSet(FrameBindingSet)
                    .addBindingSet(GpuObjectBindingSet)
					.addBindingSet(GetMaterialTextureBindingSet(batch.MaterialIndex))
                    .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                    .addVertexBuffer(nvrhi::VertexBufferBinding()
                        .setBuffer(mesh.VertexBuffer).setSlot(0).setOffset(0))
                    .addVertexBuffer(nvrhi::VertexBufferBinding()
                        .setBuffer(VisibleObjectIndicesBuffer).setSlot(1).setOffset(0))
                    .setIndexBuffer(nvrhi::IndexBufferBinding()
                        .setBuffer(mesh.IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT)
                        .setOffset(0))
                    .setIndirectParams(IndirectArgumentsBuffer);
                commandList->setGraphicsState(state);
                commandList->drawIndexedIndirect(
                    batchIndex * sizeof(nvrhi::DrawIndexedIndirectArguments),
                    1);
            }

            const auto drawCpuItem = [this, &context, commandList, framebuffer, &viewport,
                                         &data](const DrawItem& drawItem, bool bSkinned)
            {
                const MeshEntry& mesh = MeshCache.at(drawItem.Key);
                if (mesh.GraphIndex >= data.VertexBuffers.size())
                    throw std::logic_error("Depth-prepass CPU mesh graph index is stale");
                (void)context.GetBuffer(
                    data.VertexBuffers[mesh.GraphIndex],
                    RenderGraphAccess::Read);
                (void)context.GetBuffer(
                    data.IndexBuffers[mesh.GraphIndex],
                    RenderGraphAccess::Read);
                commandList->writeBuffer(
                    ObjectConstantBuffer,
                    &drawItem.Constants,
                    sizeof(drawItem.Constants));

                nvrhi::GraphicsState state;
                const bool bCustomMask = drawItem.MaterialProgram &&
                    drawItem.MaterialProgram->GetManifest().BlendMode ==
                        MaterialShaderBlendMode::Mask;
                state.setPipeline(bCustomMask
                        ? GetOrCreateMaterialPipeline(
                            framebuffer,
                            *drawItem.MaterialProgram,
                            MaterialShaderPass::Depth,
                            bSkinned,
                            drawItem.bDoubleSided,
                            false)
                        : GetOrCreateDepthPrepassPipeline(
                            framebuffer,
                            false,
                            bSkinned,
                            drawItem.bDoubleSided))
                    .setFramebuffer(framebuffer)
                    .setViewport(viewport)
                    .addBindingSet(FrameBindingSet)
                    .addBindingSet(ObjectBindingSet)
					.addBindingSet(GetMaterialTextureBindingSet(
						drawItem.Constants.MaterialObjectMeshIndices.x))
                    .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                    .addVertexBuffer(nvrhi::VertexBufferBinding()
                        .setBuffer(mesh.VertexBuffer).setSlot(0).setOffset(MeshVertexByteOffset(mesh)))
                    .setIndexBuffer(nvrhi::IndexBufferBinding()
                        .setBuffer(mesh.IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT)
                        .setOffset(MeshIndexByteOffset(mesh)));
                commandList->setGraphicsState(state);
                commandList->drawIndexed(
                    MeshDrawArguments(mesh, drawItem.LodIndex));
            };
            if (data.bPostTaa)
            {
                // The first depth list was culled with a jittered frustum.
                // Draw the complete authoritative input stream by batch instead
                // of replaying each instance through CPU object constants. This
                // retains off-frustum/occluded rows for the unjittered projection.
                for (size_t batchIndex = 0; batchIndex < GpuBatches.size(); ++batchIndex)
                {
                    const auto& batch = GpuBatches[batchIndex];
                    if (batch.MaterialProgram && batch.MaterialProgram->GetManifest().BlendMode ==
                        MaterialShaderBlendMode::Mask)
                    {
                        // Preserve custom alpha/displacement depth programs.
                        for (const auto objectIndex : GpuBatchObjectIndices[batchIndex])
                            drawCpuItem(DrawItem{
                                .Key = batch.Key.Mesh,
                                .LodIndex = batch.Key.LodIndex,
                                .Constants = GpuObjectData[objectIndex],
                                .MaterialProgram = batch.MaterialProgram,
                                .bDoubleSided = batch.Key.bDoubleSided }, false);
                        continue;
                    }
                    const auto& mesh = MeshCache.at(batch.Key.Mesh);
                    (void)context.GetBuffer(data.VertexBuffers.at(mesh.GraphIndex), RenderGraphAccess::Read);
                    (void)context.GetBuffer(data.IndexBuffers.at(mesh.GraphIndex), RenderGraphAccess::Read);
                    nvrhi::GraphicsState state;
                    state.setPipeline(GetOrCreateDepthPrepassPipeline(
                            framebuffer, true, false, batch.Key.bDoubleSided, true))
                        .setFramebuffer(framebuffer).setViewport(viewport)
                        .addBindingSet(FrameBindingSet).addBindingSet(GpuObjectBindingSet)
                        .addBindingSet(GetMaterialTextureBindingSet(batch.MaterialIndex))
                        .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                        .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(mesh.VertexBuffer)
                            .setSlot(0).setOffset(MeshVertexByteOffset(mesh)))
                        .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(GpuDrawDataBuffer)
                            .setSlot(1).setOffset(static_cast<uint64_t>(batch.VisibleBase) * sizeof(GpuDrawData)))
                        .setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(mesh.IndexBuffer)
                            .setFormat(nvrhi::Format::R32_UINT).setOffset(MeshIndexByteOffset(mesh)));
                    commandList->setGraphicsState(state);
                    commandList->drawIndexed(MeshDrawArguments(mesh, batch.Key.LodIndex)
                        .setInstanceCount(batch.ObjectCount));
                }
            }
            for (const DrawItem& drawItem : CpuDrawItems)
                drawCpuItem(drawItem, false);
            for (const DrawItem& drawItem : SkinnedDrawItems)
                drawCpuItem(drawItem, true);
        }

        void RecordDirectionalShadowPass(
            RenderGraphContext& context,
            const DirectionalShadowPassData& data)
        {
            nvrhi::ICommandList* commandList = context.GetCommandList();
            nvrhi::ITexture* shadowDepth =
                context.GetTexture(data.ShadowDepth, RenderGraphAccess::Write);
            DirectionalShadowTextures.insert(shadowDepth);
            (void)context.GetBuffer(data.FrameConstantsBuffer, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.LightConstantsBuffer, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.LightRecords, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.ClusterMasks, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.ObjectConstantsBuffer, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.BoneMatrices, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.PreviousBoneMatrices, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.GpuMaterials, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.MaterialParameters, RenderGraphAccess::Read);
            for (TextureHandle texture : data.MaterialTextures)
                (void)context.GetTexture(texture, RenderGraphAccess::Read);

            Stats.DirectionalShadowDrawCount = 0;
            for (uint32_t cascade = 0;
                cascade < DirectionalShadowCascadeCount;
                ++cascade)
            {
                commandList->clearDepthStencilTexture(
                    shadowDepth,
                    nvrhi::TextureSubresourceSet(0, 1, cascade, 1),
                    true,
                    1.0f,
                    false,
                    0);
            }
            if (!bDirectionalShadowEnabled)
                return;

            const nvrhi::ViewportState viewport = nvrhi::ViewportState()
                .addViewportAndScissorRect(nvrhi::Viewport(
                    static_cast<float>(DirectionalShadowMapSize),
                    static_cast<float>(DirectionalShadowMapSize)));
            for (uint32_t cascade = 0;
                cascade < DirectionalShadowCascadeCount;
                ++cascade)
            {
                nvrhi::IFramebuffer* framebuffer =
                    GetOrCreateShadowFramebuffer(shadowDepth, cascade);
                const ShadowPassConstants shadowPassConstants{
                    .CascadeIndex = cascade,
                };
                Stats.DirectionalShadowDrawCount += RecordGpuShadowView(context, cascade, cascade, framebuffer, viewport, FrameBindingSet);
                for (const DrawItem& drawItem : ShadowDrawItems)
                {
                    if (drawItem.bGpuShadow) continue;
                    const glm::mat4& cascadeMatrix = cascade == 0
                        ? FrameData.LightViewProjection : FrameData.AdditionalLightViewProjections[cascade - 1];
                    if (!IsShadowCasterVisible(drawItem, cascadeMatrix)) continue;
                    ++Stats.DirectionalShadowDrawCount;
                    ++Stats.DirectionalCpuCasterDraws.at(cascade);
                    const MeshEntry& mesh = MeshCache.at(drawItem.Key);
                    if (mesh.GraphIndex >= data.VertexBuffers.size())
                        throw std::logic_error("SceneRenderer shadow mesh graph index is stale");
                    (void)context.GetBuffer(
                        data.VertexBuffers[mesh.GraphIndex], RenderGraphAccess::Read);
                    (void)context.GetBuffer(
                        data.IndexBuffers[mesh.GraphIndex], RenderGraphAccess::Read);
                    commandList->writeBuffer(
                        ObjectConstantBuffer,
                        &drawItem.Constants,
                        sizeof(drawItem.Constants));
                    bObjectConstantsInitialized = true;

                    nvrhi::GraphicsState state;
                    const bool bCustomMask = drawItem.MaterialProgram &&
                        drawItem.MaterialProgram->GetManifest().BlendMode ==
                            MaterialShaderBlendMode::Mask;
                    state.setPipeline(bCustomMask
                            ? GetOrCreateMaterialPipeline(
                                framebuffer,
                                *drawItem.MaterialProgram,
                                MaterialShaderPass::Shadow,
                                drawItem.bSkinned,
                                drawItem.bDoubleSided,
                                false)
                            : GetOrCreateShadowPipeline(
                                framebuffer,
                                drawItem.bSkinned,
                                drawItem.bDoubleSided))
                        .setFramebuffer(framebuffer)
                        .setViewport(viewport)
                        .addBindingSet(FrameBindingSet)
                        .addBindingSet(ObjectBindingSet)
                        .addBindingSet(ShadowPassBindingSet)
						.addBindingSet(GetMaterialTextureBindingSet(
							drawItem.Constants.MaterialObjectMeshIndices.x))
                        .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                        .addVertexBuffer(nvrhi::VertexBufferBinding()
                            .setBuffer(mesh.VertexBuffer)
                            .setSlot(0)
                            .setOffset(MeshVertexByteOffset(mesh)))
                        .setIndexBuffer(nvrhi::IndexBufferBinding()
                            .setBuffer(mesh.IndexBuffer)
                            .setFormat(nvrhi::Format::R32_UINT)
                            .setOffset(MeshIndexByteOffset(mesh)));
                    commandList->setGraphicsState(state);
                    commandList->setPushConstants(
                        &shadowPassConstants,
                        sizeof(shadowPassConstants));
                    commandList->drawIndexed(
                        MeshDrawArguments(mesh, drawItem.LodIndex));
                }
            }
        }

        void RecordOpaquePass(RenderGraphContext& context, const OpaquePassData& data)
        {
            nvrhi::ICommandList* commandList = context.GetCommandList();
            nvrhi::ITexture* sceneColor = context.GetTexture(data.SceneColor, RenderGraphAccess::Write);
            nvrhi::ITexture* sceneDepth = context.GetTexture(data.SceneDepth, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.FrameConstantsBuffer, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.ObjectConstantsBuffer, RenderGraphAccess::Read);
            for (TextureHandle texture : data.MaterialTextures)
                (void)context.GetTexture(texture, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.GpuObjects, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.GpuMaterials, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.MaterialParameters, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.VisibleObjectIndices, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.IndirectArguments, RenderGraphAccess::Read);
            nvrhi::ITexture* directionalShadow =
                context.GetTexture(data.DirectionalShadow, RenderGraphAccess::Read);
            nvrhi::ITexture* irradiance =
                context.GetTexture(data.Irradiance, RenderGraphAccess::Read);
            nvrhi::ITexture* prefilter =
                context.GetTexture(data.Prefilter, RenderGraphAccess::Read);
            nvrhi::ITexture* brdfLut =
                context.GetTexture(data.BrdfLut, RenderGraphAccess::Read);

            nvrhi::IFramebuffer* framebuffer = GetOrCreateOpaqueFramebuffer(sceneColor, sceneDepth);
            nvrhi::IBindingSet* shadowBindingSet =
                GetOrCreateEnvironmentBindingSet(
                    directionalShadow,
                    irradiance,
                    prefilter,
                    brdfLut);
            commandList->clearTextureFloat(
                sceneColor,
                nvrhi::AllSubresources,
                nvrhi::Color(0.004f, 0.008f, 0.016f, 1.0f));

            if (bGpuBatchingEnabled)
                RecordGroupedGpuGeometry(context, *MainIndirectCompaction, data.IndirectArguments,
                    [&](const GpuStateGroup& group)
                    {
                        nvrhi::GraphicsState state;
                        state.setPipeline(GetGpuMaterialPipeline(framebuffer, GpuBatches[group.FirstBatch], MaterialShaderPass::Forward))
                            .setFramebuffer(framebuffer).setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                                nvrhi::Viewport(static_cast<float>(LastWidth), static_cast<float>(LastHeight))))
                            .addBindingSet(GetLightingFrameBindingSet(context)).addBindingSet(GpuObjectBindingSet)
                            .addBindingSet(shadowBindingSet)
                            .addBindingSet(GetMaterialTextureBindingSet(GpuBatches[group.FirstBatch].MaterialIndex))
                            .addBindingSet(TextureManagerInstance.GetSamplerBindingSet());
                        return state;
                    });

            for (uint32_t batchIndex = 0;
                !bGpuBatchingEnabled && batchIndex < static_cast<uint32_t>(GpuBatches.size());
                ++batchIndex)
            {
                const GpuBatch& batch = GpuBatches[batchIndex];
                const MeshEntry& mesh = MeshCache.at(batch.Key.Mesh);
                if (mesh.GraphIndex >= data.VertexBuffers.size())
                    throw std::logic_error("SceneRenderer GPU mesh graph index is stale");
                (void)context.GetBuffer(data.VertexBuffers[mesh.GraphIndex], RenderGraphAccess::Read);
                (void)context.GetBuffer(data.IndexBuffers[mesh.GraphIndex], RenderGraphAccess::Read);

                nvrhi::GraphicsState state;
                state.setPipeline(GetGpuMaterialPipeline(framebuffer, batch, MaterialShaderPass::Forward))
                    .setFramebuffer(framebuffer)
                    .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                        nvrhi::Viewport(static_cast<float>(LastWidth), static_cast<float>(LastHeight))))
                    .addBindingSet(GetLightingFrameBindingSet(context))
                    .addBindingSet(GpuObjectBindingSet)
                    .addBindingSet(shadowBindingSet)
					.addBindingSet(GetMaterialTextureBindingSet(batch.MaterialIndex))
                    .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                    .addVertexBuffer(nvrhi::VertexBufferBinding()
                        .setBuffer(mesh.VertexBuffer)
                        .setSlot(0)
                        .setOffset(0))
                    .addVertexBuffer(nvrhi::VertexBufferBinding()
                        .setBuffer(VisibleObjectIndicesBuffer).setSlot(1).setOffset(0))
                    .setIndexBuffer(nvrhi::IndexBufferBinding()
                        .setBuffer(mesh.IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT)
                        .setOffset(0))
                    .setIndirectParams(IndirectArgumentsBuffer);
                commandList->setGraphicsState(state);
                commandList->drawIndexedIndirect(
                    batchIndex * sizeof(nvrhi::DrawIndexedIndirectArguments),
                    1);
            }

            for (const DrawItem& drawItem : CpuDrawItems)
            {
                const MeshEntry& mesh = MeshCache.at(drawItem.Key);
                if (mesh.GraphIndex >= data.VertexBuffers.size())
                    throw std::logic_error("SceneRenderer mesh graph index is stale");
                (void)context.GetBuffer(data.VertexBuffers[mesh.GraphIndex], RenderGraphAccess::Read);
                (void)context.GetBuffer(data.IndexBuffers[mesh.GraphIndex], RenderGraphAccess::Read);

                commandList->writeBuffer(
                    ObjectConstantBuffer,
                    &drawItem.Constants,
                    sizeof(drawItem.Constants));
                bObjectConstantsInitialized = true;

                nvrhi::GraphicsState state;
                state.setPipeline(drawItem.MaterialProgram
                        ? GetOrCreateMaterialPipeline(
                            framebuffer,
                            *drawItem.MaterialProgram,
                            MaterialShaderPass::Forward,
                            false,
                            drawItem.bDoubleSided,
                            false)
                        : GetOrCreateOpaquePipeline(framebuffer))
                    .setFramebuffer(framebuffer)
                    .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                        nvrhi::Viewport(static_cast<float>(LastWidth), static_cast<float>(LastHeight))))
                    .addBindingSet(GetLightingFrameBindingSet(context))
                    .addBindingSet(ObjectBindingSet)
                    .addBindingSet(shadowBindingSet)
					.addBindingSet(GetMaterialTextureBindingSet(
						drawItem.Constants.MaterialObjectMeshIndices.x))
                    .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                    .addVertexBuffer(nvrhi::VertexBufferBinding()
                        .setBuffer(mesh.VertexBuffer)
                        .setSlot(0)
                        .setOffset(MeshVertexByteOffset(mesh)))
                    .setIndexBuffer(nvrhi::IndexBufferBinding()
                        .setBuffer(mesh.IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT)
                        .setOffset(MeshIndexByteOffset(mesh)));
                commandList->setGraphicsState(state);
                commandList->drawIndexed(MeshDrawArguments(mesh, drawItem.LodIndex));
            }
        }

        void AddGpuCullingReadbackPass()
        {
            Graph.AddPass<GpuCullingReadbackPassData>(
                "GpuCullingReadback",
                RenderGraphQueue::Copy,
                [this](RenderGraphBuilder& builder, GpuCullingReadbackPassData& data)
                {
                    data.IndirectArguments = builder.CopySource(IndirectArgumentsHandle);
                    data.Readback = builder.CopyDest(GpuReadbackHandle);
                    builder.SetSideEffect();
                },
                [this](RenderGraphContext& context, const GpuCullingReadbackPassData& data)
                {
                    nvrhi::IBuffer* indirect =
                        context.GetBuffer(data.IndirectArguments, RenderGraphAccess::Read);
                    nvrhi::IBuffer* readback =
                        context.GetBuffer(data.Readback, RenderGraphAccess::Write);
                    if (GpuBatches.empty())
                        return;
                    context.GetCommandList()->copyBuffer(
                        readback,
                        0,
                        indirect,
                        0,
                        GpuBatches.size() * sizeof(nvrhi::DrawIndexedIndirectArguments));
                });
        }

        void RecordToneMappingPass(RenderGraphContext& context, const ToneMappingPassData& data)
        {
            nvrhi::ICommandList* commandList = context.GetCommandList();
            nvrhi::ITexture* sceneColor = context.GetTexture(data.SceneColor, RenderGraphAccess::Read);
            nvrhi::ITexture* backBuffer = context.GetTexture(data.BackBuffer, RenderGraphAccess::Write);
            nvrhi::IFramebuffer* framebuffer = GetOrCreateToneFramebuffer(backBuffer);
            nvrhi::IBindingSet* bindingSet = GetOrCreateToneBindingSet(sceneColor);
            nvrhi::IGraphicsPipeline* pipeline = GetOrCreateTonePipeline(framebuffer);

            nvrhi::GraphicsState state;
            state.setPipeline(pipeline)
                .setFramebuffer(framebuffer)
                .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                    nvrhi::Viewport(static_cast<float>(backBuffer->getDesc().width), static_cast<float>(backBuffer->getDesc().height))))
                .addBindingSet(bindingSet);
            commandList->setGraphicsState(state);
            const ToneMappingConstants constants{
                .Exposure = Exposure,
                .PbrDebugMode = static_cast<uint32_t>(CurrentPbrDebugMode),
                .GiDebugView = static_cast<uint32_t>(EffectiveGiSettings.DebugView),
                .Upscale = (LastOutputWidth > sceneColor->getDesc().width || LastOutputHeight > sceneColor->getDesc().height) ? 1.0f : 0.0f,
            };
            commandList->setPushConstants(&constants, sizeof(constants));
            commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetOrCreateOpaqueFramebuffer(
            nvrhi::ITexture* color,
            nvrhi::ITexture* depth)
        {
            const OpaqueFramebufferKey key{ color, depth };
            const auto existing = OpaqueFramebuffers.find(key);
            if (existing != OpaqueFramebuffers.end())
                return existing->second;

            nvrhi::FramebufferDesc desc;
            desc.addColorAttachment(color).setDepthAttachment(depth);
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
                throw std::runtime_error("NVRHI failed to create SceneRenderer HDR framebuffer");
            return OpaqueFramebuffers.emplace(key, std::move(framebuffer)).first->second;
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetOrCreateToneFramebuffer(nvrhi::ITexture* backBuffer)
        {
            const auto existing = ToneFramebuffers.find(backBuffer);
            if (existing != ToneFramebuffers.end())
                return existing->second;

            nvrhi::FramebufferDesc desc;
            desc.addColorAttachment(backBuffer);
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
                throw std::runtime_error("NVRHI failed to create SceneRenderer tone-map framebuffer");
            return ToneFramebuffers.emplace(backBuffer, std::move(framebuffer)).first->second;
        }

        void ClearDirectionalShadowResourceCaches()
        {
            if (DirectionalShadowTextures.empty())
                return;

            // Submitted NVRHI command lists retain their framebuffer and binding-set
            // references. Removing only cache entries backed by the old physical
            // shadow arrays lets the resource pool retire those arrays normally.
            std::erase_if(ShadowFramebuffers, [this](const auto& entry)
            {
                return DirectionalShadowTextures.contains(entry.first.Depth);
            });
            std::erase_if(EnvironmentBindingSets, [this](const auto& entry)
            {
                return DirectionalShadowTextures.contains(entry.first.Shadow);
            });
            DirectionalShadowTextures.clear();
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetOrCreateShadowFramebuffer(
            nvrhi::ITexture* shadowDepth,
            uint32_t arraySlice = 0)
        {
            const ShadowFramebufferKey key{
                .Depth = shadowDepth,
                .ArraySlice = arraySlice,
            };
            const auto existing = ShadowFramebuffers.find(key);
            if (existing != ShadowFramebuffers.end())
                return existing->second;

            nvrhi::FramebufferDesc desc;
            desc.setDepthAttachment(
                shadowDepth,
                nvrhi::TextureSubresourceSet(0, 1, arraySlice, 1));
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
                throw std::runtime_error("NVRHI failed to create directional shadow framebuffer");
            return ShadowFramebuffers.emplace(key, std::move(framebuffer)).first->second;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetOrCreateEnvironmentBindingSet(
            nvrhi::ITexture* shadowDepth,
            nvrhi::ITexture* irradiance,
            nvrhi::ITexture* prefilter,
            nvrhi::ITexture* brdfLut)
        {
            const EnvironmentBindingKey key{
                .Shadow = shadowDepth,
                .Irradiance = irradiance,
                .Prefilter = prefilter,
                .BrdfLut = brdfLut,
            };
            const auto existing = EnvironmentBindingSets.find(key);
            if (existing != EnvironmentBindingSets.end())
                return existing->second;

            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                    0,
                    shadowDepth,
                    nvrhi::Format::UNKNOWN,
                    nvrhi::AllSubresources,
                    nvrhi::TextureDimension::Texture2DArray))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    1,
                    irradiance,
                    nvrhi::Format::UNKNOWN,
                    nvrhi::AllSubresources,
                    nvrhi::TextureDimension::TextureCube))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(
                    2,
                    prefilter,
                    nvrhi::Format::UNKNOWN,
                    nvrhi::AllSubresources,
                    nvrhi::TextureDimension::TextureCube))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(10, LocalShadowPass->GetDepthArray(),
                    nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2DArray))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, LocalShadowPass->GetViewRecordBuffer()))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(3, brdfLut))
                .addItem(nvrhi::BindingSetItem::Sampler(0, ShadowSampler))
                .addItem(nvrhi::BindingSetItem::Sampler(1, ShadowComparisonSampler));
            nvrhi::BindingSetHandle bindingSet =
                Device->createBindingSet(desc, ShadowTextureBindingLayout);
            if (!bindingSet)
                throw std::runtime_error("NVRHI failed to bind shadow and IBL textures");
            return EnvironmentBindingSets.emplace(key, std::move(bindingSet)).first->second;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetOrCreateSkyBindingSet(
            nvrhi::ITexture* environment)
        {
            const SkyBindingKey key{ .Environment = environment };
            const auto existing = SkyBindingSets.find(key);
            if (existing != SkyBindingSets.end())
                return existing->second;

            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                    0,
                    environment,
                    nvrhi::Format::UNKNOWN,
                    nvrhi::AllSubresources,
                    nvrhi::TextureDimension::TextureCube))
                .addItem(nvrhi::BindingSetItem::Sampler(0, EnvironmentSampler));
            nvrhi::BindingSetHandle bindingSet =
                Device->createBindingSet(desc, SkyBindingLayout);
            if (!bindingSet)
                throw std::runtime_error("NVRHI failed to create SceneRenderer sky binding set");
            return SkyBindingSets.emplace(key, std::move(bindingSet)).first->second;
        }

        [[nodiscard]] nvrhi::IBindingSet* GetOrCreateToneBindingSet(nvrhi::ITexture* sceneColor)
        {
            const auto existing = ToneBindingSets.find(sceneColor);
            if (existing != ToneBindingSets.end())
                return existing->second;

            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ToneMappingConstants)))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, sceneColor))
                .addItem(nvrhi::BindingSetItem::Sampler(0, ToneSampler));
            nvrhi::BindingSetHandle bindingSet = Device->createBindingSet(desc, ToneBindingLayout);
            if (!bindingSet)
                throw std::runtime_error("NVRHI failed to create SceneRenderer tone-map binding set");
            return ToneBindingSets.emplace(sceneColor, std::move(bindingSet)).first->second;
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateOpaquePipeline(
            nvrhi::IFramebuffer* framebuffer)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(InputLayout)
                .setVertexShader(OpaqueVertexShader)
                .setPixelShader(OpaquePixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(ObjectBindingLayout)
                .addBindingLayout(ShadowTextureBindingLayout)
                .addBindingLayout(TextureManagerInstance.GetTextureBindingLayout())
                .addBindingLayout(TextureManagerInstance.GetSamplerBindingLayout());
            desc.renderState.depthStencilState
                .enableDepthTest()
                .disableDepthWrite()
                .setDepthFunc(nvrhi::ComparisonFunc::Equal);
            desc.renderState.rasterState
                .setCullBack()
                .setFrontCounterClockwise(true)
                .enableDepthClip();
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ 0x53434e4f50415155ull },
                desc,
                framebuffer->getFramebufferInfo(),
                "SceneRenderer static opaque HDR pipeline");
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateMaterialPipeline(
            nvrhi::IFramebuffer* framebuffer,
            const CompiledMaterialShader& program,
            MaterialShaderPass pass,
            bool bSkinned,
            bool bDoubleSided,
            bool bTransparent,
            bool bGpuDriven = false)
        {
            const MaterialShaderVertexPath vertexPath = bSkinned
                ? MaterialShaderVertexPath::Skinned
                : bGpuDriven ? MaterialShaderVertexPath::GpuDriven : MaterialShaderVertexPath::Static;
            const MaterialShaderDescriptorMode descriptorMode = bHeapDirectlyIndexed
                ? MaterialShaderDescriptorMode::HeapDirect
                : MaterialShaderDescriptorMode::Baseline;
            const nvrhi::ShaderHandle vertexShader = program.FindShader(
                MaterialShaderHandleKey{
                    .Pass = pass,
                    .VertexPath = vertexPath,
                    .DescriptorMode = descriptorMode,
                    .Stage = MaterialProgramShaderStage::Vertex,
                });
            const nvrhi::ShaderHandle pixelShader = program.FindShader(
                MaterialShaderHandleKey{
                    .Pass = pass,
                    .VertexPath = vertexPath,
                    .DescriptorMode = descriptorMode,
                    .Stage = MaterialProgramShaderStage::Pixel,
                });
            if (!vertexShader || !pixelShader)
            {
                throw std::logic_error(
                    "Material program is missing a required shader: source='" +
                    program.GetIdentity().SourcePath + "', generation=" +
                    std::to_string(program.GetGeneration()) + ", pass=" + ToString(pass) +
                    ", vertex=" + ToString(vertexPath) + ", descriptor=" + ToString(descriptorMode) +
                    ", missingVS=" + (vertexShader ? "false" : "true") +
                    ", missingPS=" + (pixelShader ? "false" : "true"));
            }

            nvrhi::GraphicsPipelineDesc pipelineDesc;
            pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(bSkinned ? SkinnedInputLayout : bGpuDriven ? GpuInputLayout : InputLayout)
                .setVertexShader(vertexShader)
                .setPixelShader(pixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(bGpuDriven ? GpuObjectBindingLayout : ObjectBindingLayout);
            if (pass == MaterialShaderPass::Forward)
                pipelineDesc.addBindingLayout(ShadowTextureBindingLayout);
            else if (pass == MaterialShaderPass::Shadow)
                pipelineDesc.addBindingLayout(ShadowPassBindingLayout);
            pipelineDesc
                .addBindingLayout(TextureManagerInstance.GetTextureBindingLayout())
                .addBindingLayout(TextureManagerInstance.GetSamplerBindingLayout());

            if (pass == MaterialShaderPass::Forward || pass == MaterialShaderPass::GBuffer)
            {
                pipelineDesc.renderState.depthStencilState
                    .enableDepthTest()
                    .disableDepthWrite()
                    .setDepthFunc(bTransparent
                        ? nvrhi::ComparisonFunc::LessOrEqual
                        : nvrhi::ComparisonFunc::Equal);
                if (bTransparent)
                {
                    pipelineDesc.renderState.blendState.targets[0]
                        .enableBlend()
                        .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                        .setBlendOp(nvrhi::BlendOp::Add)
                        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                        .setBlendOpAlpha(nvrhi::BlendOp::Add);
                }
            }
            else if (pass == MaterialShaderPass::Outline)
            {
                pipelineDesc.renderState.depthStencilState
                    .enableDepthTest()
                    .disableDepthWrite()
                    .setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
            }
            else
            {
                pipelineDesc.renderState.depthStencilState
                    .enableDepthTest()
                    .enableDepthWrite()
                    .setDepthFunc(nvrhi::ComparisonFunc::Less);
            }

            if (pass == MaterialShaderPass::Outline)
                pipelineDesc.renderState.rasterState.setCullFront();
            else if (bDoubleSided)
                pipelineDesc.renderState.rasterState.setCullNone();
            else
                pipelineDesc.renderState.rasterState.setCullBack();
            pipelineDesc.renderState.rasterState
                .setFrontCounterClockwise(true)
                .enableDepthClip();
            if (pass == MaterialShaderPass::Shadow)
            {
                pipelineDesc.renderState.rasterState
                    .setDepthBias(100)
                    .setSlopeScaleDepthBias(1.0f);
            }

            const MaterialShaderManifest& manifest = program.GetManifest();
            const nvrhi::FramebufferInfo framebufferInfo = framebuffer->getFramebufferInfo();
            MaterialPipelineDesc keyDesc;
            keyDesc.Program = {
                .RecursiveContentHash = program.GetIdentity().RecursiveContentHash,
                .ArtifactIdentityHash = program.GetIdentity().ArtifactIdentityHash,
                .Generation = program.GetGeneration(),
                .MaterialAbiVersion = manifest.MaterialAbiVersion,
                .BindingSignature = program.GetIdentity().BindingSignatureHash,
            };
            keyDesc.Pass = pass == MaterialShaderPass::GBuffer
                ? MaterialPipelinePass::GBuffer
                : pass == MaterialShaderPass::Depth
                ? MaterialPipelinePass::Depth
                : pass == MaterialShaderPass::Shadow
                    ? MaterialPipelinePass::Shadow
                    : pass == MaterialShaderPass::Outline
                        ? MaterialPipelinePass::Outline
                    : bTransparent
                        ? MaterialPipelinePass::ForwardTransparent
                        : MaterialPipelinePass::ForwardOpaque;
            keyDesc.VertexPath = bSkinned
                ? MaterialPipelineVertexPath::Skinned
                : bGpuDriven ? MaterialPipelineVertexPath::GpuDriven : MaterialPipelineVertexPath::CpuStatic;
            keyDesc.DescriptorTier = bHeapDirectlyIndexed
                ? MaterialPipelineDescriptorTier::HeapDirect
                : MaterialPipelineDescriptorTier::Baseline;
            keyDesc.InputLayout = MaterialInputLayoutIdentity{
                .Value = bSkinned ? 0x534b494e4e454431ull : bGpuDriven ? 0x475055494e535431ull : 0x5354415449433031ull,
            };
            keyDesc.BlendMode = bTransparent
                ? MaterialPipelineBlendMode::AlphaBlend
                : MaterialPipelineBlendMode::Opaque;
            keyDesc.DepthWrite =
                pass == MaterialShaderPass::Depth ||
                    pass == MaterialShaderPass::Shadow
                ? MaterialPipelineDepthWrite::Enabled
                : MaterialPipelineDepthWrite::Disabled;
            keyDesc.CullMode = pass == MaterialShaderPass::Outline
                ? MaterialPipelineCullMode::Front
                : bDoubleSided
                    ? MaterialPipelineCullMode::None
                    : MaterialPipelineCullMode::Back;
            keyDesc.Sidedness = bDoubleSided
                ? MaterialPipelineSidedness::DoubleSided
                : MaterialPipelineSidedness::SingleSided;
            keyDesc.AlphaMode = manifest.BlendMode == MaterialShaderBlendMode::Mask
                ? MaterialPipelineAlphaMode::Mask
                : manifest.BlendMode == MaterialShaderBlendMode::Transparent
                    ? MaterialPipelineAlphaMode::Blend
                    : MaterialPipelineAlphaMode::Opaque;
            keyDesc.Framebuffer = {
                .ColorFormat = framebufferInfo.colorFormats.size() > 0
                    ? framebufferInfo.colorFormats[0]
                    : nvrhi::Format::UNKNOWN,
                .DepthFormat = framebufferInfo.depthFormat,
                .SampleCount = framebufferInfo.sampleCount,
                .SampleQuality = framebufferInfo.sampleQuality,
            };
            const std::string debugIdentity = "Material " + manifest.SourcePath +
                " generation " + std::to_string(program.GetGeneration()) +
                " pass " + ToString(pass);
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                BuildMaterialPipelineKey(keyDesc),
                pipelineDesc,
                framebufferInfo,
                debugIdentity);
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateSkyPipeline(
            nvrhi::IFramebuffer* framebuffer)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setVertexShader(SkyVertexShader)
                .setPixelShader(SkyPixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(SkyBindingLayout);
            desc.renderState.depthStencilState
                .enableDepthTest()
                .disableDepthWrite()
                .setDepthFunc(nvrhi::ComparisonFunc::Equal);
            desc.renderState.rasterState.setCullNone().enableDepthClip();
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ 0x53434e534b595049ull },
                desc,
                framebuffer->getFramebufferInfo(),
                "SceneRenderer sky HDR pipeline");
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateTransparentPipeline(
            nvrhi::IFramebuffer* framebuffer,
            bool bDoubleSided)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(InputLayout)
                .setVertexShader(OpaqueVertexShader)
                .setPixelShader(OpaquePixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(ObjectBindingLayout)
                .addBindingLayout(ShadowTextureBindingLayout)
                .addBindingLayout(TextureManagerInstance.GetTextureBindingLayout())
                .addBindingLayout(TextureManagerInstance.GetSamplerBindingLayout());
            desc.renderState.depthStencilState
                .enableDepthTest()
                .disableDepthWrite()
                .setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
            if (bDoubleSided)
				desc.renderState.rasterState.setCullNone();
            else
				desc.renderState.rasterState.setCullBack();
			desc.renderState.rasterState
				.setFrontCounterClockwise(true)
				.enableDepthClip();
            desc.renderState.blendState.targets[0]
                .enableBlend()
                .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                .setBlendOp(nvrhi::BlendOp::Add)
                .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                .setBlendOpAlpha(nvrhi::BlendOp::Add);
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ MixKey(
                    0x53434e5452414e53ull,
                    bDoubleSided ? 1u : 0u) },
                desc,
                framebuffer->getFramebufferInfo(),
                bDoubleSided
                    ? "SceneRenderer double-sided transparent HDR pipeline"
                    : "SceneRenderer transparent HDR pipeline");
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateSkinnedPipeline(
            nvrhi::IFramebuffer* framebuffer,
            bool bTransparent,
            bool bDoubleSided)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(SkinnedInputLayout)
                .setVertexShader(SkinnedVertexShader)
                .setPixelShader(SkinnedPixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(ObjectBindingLayout)
                .addBindingLayout(ShadowTextureBindingLayout)
                .addBindingLayout(TextureManagerInstance.GetTextureBindingLayout())
                .addBindingLayout(TextureManagerInstance.GetSamplerBindingLayout());
            desc.renderState.depthStencilState
                .enableDepthTest()
                .setDepthFunc(bTransparent
                    ? nvrhi::ComparisonFunc::LessOrEqual
                    : nvrhi::ComparisonFunc::Equal)
                .disableDepthWrite();
            if (bDoubleSided)
				desc.renderState.rasterState.setCullNone();
            else
				desc.renderState.rasterState.setCullBack();
			desc.renderState.rasterState
				.setFrontCounterClockwise(true)
				.enableDepthClip();
            if (bTransparent)
            {
                desc.renderState.blendState.targets[0]
                    .enableBlend()
                    .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                    .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                    .setBlendOp(nvrhi::BlendOp::Add)
                    .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                    .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                    .setBlendOpAlpha(nvrhi::BlendOp::Add);
            }
            uint64_t key = MixKey(0x53434e534b494e4eull, bTransparent ? 1u : 0u);
            key = MixKey(key, bDoubleSided ? 1u : 0u);
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ key },
                desc,
                framebuffer->getFramebufferInfo(),
                bTransparent
                    ? "SceneRenderer skinned transparent HDR pipeline"
                    : "SceneRenderer skinned opaque HDR pipeline");
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateDepthPrepassPipeline(
            nvrhi::IFramebuffer* framebuffer,
            bool bGpuDriven,
            bool bSkinned,
            bool bDoubleSided,
            bool bUnculledInput = false)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(bSkinned
                    ? SkinnedShadowInputLayout
                    : bGpuDriven
                        ? (bUnculledInput ? GpuUnculledDepthInputLayout : GpuDepthInputLayout)
                        : ShadowInputLayout)
                .setVertexShader(bSkinned
                    ? SkinnedDepthVertexShader
                    : bGpuDriven ? GpuDepthVertexShader : DepthVertexShader)
                .setPixelShader(bSkinned
                    ? SkinnedDepthPixelShader
                    : bGpuDriven ? GpuDepthPixelShader : DepthPixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(bGpuDriven
                    ? GpuObjectBindingLayout
                    : ObjectBindingLayout)
                .addBindingLayout(TextureManagerInstance.GetTextureBindingLayout())
                .addBindingLayout(TextureManagerInstance.GetSamplerBindingLayout());
            desc.renderState.depthStencilState
                .enableDepthTest()
                .enableDepthWrite()
                .setDepthFunc(nvrhi::ComparisonFunc::Less);
            if (bDoubleSided)
				desc.renderState.rasterState.setCullNone();
            else
				desc.renderState.rasterState.setCullBack();
			desc.renderState.rasterState
				.setFrontCounterClockwise(true)
				.enableDepthClip();
            uint64_t key = MixKey(0x53434e4450544850ull, bGpuDriven ? 1u : 0u);
            key = MixKey(key, bSkinned ? 1u : 0u);
            key = MixKey(key, bDoubleSided ? 1u : 0u);
            key = MixKey(key, bUnculledInput ? 1u : 0u);
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ key },
                desc,
                framebuffer->getFramebufferInfo(),
                bSkinned
                    ? "SceneRenderer skinned depth prepass pipeline"
                    : bGpuDriven
                        ? "SceneRenderer GPU-driven depth prepass pipeline"
                        : "SceneRenderer CPU depth prepass pipeline");
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateShadowPipeline(
            nvrhi::IFramebuffer* framebuffer,
            bool bSkinned,
            bool bDoubleSided)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(bSkinned ? SkinnedShadowInputLayout : ShadowInputLayout)
                .setVertexShader(bSkinned ? SkinnedShadowVertexShader : ShadowVertexShader)
                .setPixelShader(bSkinned ? SkinnedShadowPixelShader : ShadowPixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(ObjectBindingLayout)
                .addBindingLayout(ShadowPassBindingLayout)
                .addBindingLayout(TextureManagerInstance.GetTextureBindingLayout())
                .addBindingLayout(TextureManagerInstance.GetSamplerBindingLayout());
            desc.renderState.depthStencilState
                .enableDepthTest()
                .enableDepthWrite()
                .setDepthFunc(nvrhi::ComparisonFunc::Less);
            if (bDoubleSided)
                desc.renderState.rasterState.setCullNone();
            else
                desc.renderState.rasterState.setCullBack();
            desc.renderState.rasterState
                .setFrontCounterClockwise(true)
                .enableDepthClip()
                .setDepthBias(100)
                .setSlopeScaleDepthBias(1.0f);
            uint64_t key = MixKey(0x53434e5348444f57ull, bSkinned ? 1u : 0u);
            key = MixKey(key, bDoubleSided ? 1u : 0u);
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ key },
                desc,
                framebuffer->getFramebufferInfo(),
                bSkinned
                    ? "SceneRenderer skinned directional shadow depth pipeline"
                    : "SceneRenderer directional shadow depth pipeline");
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateOutlinePipeline(
            nvrhi::IFramebuffer* framebuffer,
            bool bSkinned)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(bSkinned ? SkinnedOutlineInputLayout : OutlineInputLayout)
                .setVertexShader(bSkinned ? SkinnedOutlineVertexShader : OutlineVertexShader)
                .setPixelShader(bSkinned ? SkinnedOutlinePixelShader : OutlinePixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(ObjectBindingLayout);
            desc.renderState.depthStencilState
                .enableDepthTest()
                .disableDepthWrite()
                .setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
            // Render only the expanded back faces so the selected object keeps
            // its original material while the visible silhouette surrounds it.
            desc.renderState.rasterState
                .setCullFront()
                .setFrontCounterClockwise(true)
                .enableDepthClip();
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ MixKey(
                    0x53434e4f55544c4eull,
                    bSkinned ? 1u : 0u) },
                desc,
                framebuffer->getFramebufferInfo(),
                bSkinned
                    ? "SceneRenderer skinned editor outline pipeline"
                    : "SceneRenderer editor outline pipeline");
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateTonePipeline(
            nvrhi::IFramebuffer* framebuffer)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setVertexShader(ToneVertexShader)
                .setPixelShader(TonePixelShader)
                .addBindingLayout(ToneBindingLayout);
            desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
            desc.renderState.rasterState.setCullNone().enableDepthClip();
            const uint64_t key = MixKey(
                0x53434e544f4e4550ull,
                static_cast<uint64_t>(framebuffer->getFramebufferInfo().colorFormats[0]));
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ key },
                desc,
                framebuffer->getFramebufferInfo(),
                "SceneRenderer ACES tone-map pipeline");
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreatePrimitive2DPipeline(
            nvrhi::IFramebuffer* framebuffer,
            RenderPrimitiveDepthMode depthMode)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(Primitive2DInputLayout)
                .setVertexShader(Primitive2DVertexShader)
                .setPixelShader(Primitive2DPixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(Primitive2DBindingLayout)
                .addBindingLayout(TextureManagerInstance.GetTextureBindingLayout())
                .addBindingLayout(TextureManagerInstance.GetSamplerBindingLayout());
            if (depthMode == RenderPrimitiveDepthMode::DepthTest)
            {
                desc.renderState.depthStencilState
                    .enableDepthTest()
                    .disableDepthWrite()
                    .setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
            }
            else
            {
                desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
            }
            desc.renderState.rasterState.setCullNone().enableDepthClip();
            desc.renderState.blendState.targets[0]
                .enableBlend()
                .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                .setBlendOp(nvrhi::BlendOp::Add)
                .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                .setBlendOpAlpha(nvrhi::BlendOp::Add);
            const uint64_t key = MixKey(
                0x53434e5052494d50ull,
                static_cast<uint64_t>(depthMode));
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ key },
                desc,
                framebuffer->getFramebufferInfo(),
                depthMode == RenderPrimitiveDepthMode::DepthTest
                    ? "SceneRenderer Primitive2D depth-tested HDR pipeline"
                    : "SceneRenderer Primitive2D overlay HDR pipeline");
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateGpuOpaquePipeline(
            nvrhi::IFramebuffer* framebuffer,
            bool bDoubleSided)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(GpuInputLayout)
                .setVertexShader(GpuOpaqueVertexShader)
                .setPixelShader(GpuOpaquePixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(GpuObjectBindingLayout)
                .addBindingLayout(ShadowTextureBindingLayout)
                .addBindingLayout(TextureManagerInstance.GetTextureBindingLayout())
                .addBindingLayout(TextureManagerInstance.GetSamplerBindingLayout());
            desc.renderState.depthStencilState
                .enableDepthTest()
                .disableDepthWrite()
                .setDepthFunc(nvrhi::ComparisonFunc::Equal);
            if (bDoubleSided)
				desc.renderState.rasterState.setCullNone();
            else
				desc.renderState.rasterState.setCullBack();
			desc.renderState.rasterState
				.setFrontCounterClockwise(true)
				.enableDepthClip();
            return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{ MixKey(
                    0x53434e4750554f50ull,
                    bDoubleSided ? 1u : 0u) },
                desc,
                framebuffer->getFramebufferInfo(),
                "SceneRenderer GPU-driven static opaque HDR pipeline");
        }

        void RecordFrame(const RenderWorld& world, const SceneRendererFrameDesc& outputFrame)
        {
            SceneRendererFrameDesc frame = outputFrame;
            const bool bOutputSizeChanged = LastOutputWidth != frame.Width || LastOutputHeight != frame.Height;
            LastOutputWidth = frame.Width;
            LastOutputHeight = frame.Height;
            // Editor/offscreen depth is consumed by native-resolution picking and
            // external plugins. Keep that contract; cap the main game's 3D only.
            const bool bCanScale = !world.bEditorView &&
                frame.OutputMode == SceneRendererOutputMode::SwapChain &&
                !IsOfflineViewportRenderingActive();
            const bool wasFsrActive = bFsrActiveLastFrame;
            const auto fallback = ResolveRenderExtent(frame.Width, frame.Height,
                bCanScale ? PostProcessSettings.RenderHeight : 0);
            const auto internal = UpscalingPass->Prepare(RequestedUpscaling, {frame.Width, frame.Height},
                fallback, bCanScale && EffectiveGiSettings.DebugView == GlobalIlluminationDebugView::Composite,
                frame.CompletedSubmission);
            if (wasFsrActive != IsFsrActive()) { bGraphDirty = true; bHasPreviousRasterCamera = false; }
            bFsrActiveLastFrame = IsFsrActive();
            frame.Width = internal.Width;
            frame.Height = internal.Height;
            if (LastWidth != frame.Width || LastHeight != frame.Height || bOutputSizeChanged)
            {
                bGraphDirty = true;
                PendingGraphBuildReasonMask |= SceneGraphRebuildReason::Output;
                // Frame uniforms and screen-space preparation below must see
                // the new dimensions on the very first switched frame.
                LastWidth = frame.Width;
                LastHeight = frame.Height;
            }
            Stats.InternalWidth = frame.Width;
            Stats.InternalHeight = frame.Height;
            Stats.OutputWidth = LastOutputWidth;
            Stats.OutputHeight = LastOutputHeight;
            Stats.bUpscalingActive = IsFsrActive();
            Stats.bUpscalingFailed = UpscalingPass->HasFailed();
            Stats.UpscalingState = UpscalingPass->GetStatus();
            Stats.UpscalingFailureReason = Stats.bUpscalingFailed ? UpscalingPass->GetReason() : std::string{};
            if (bOutputSizeChanged) bGraphDirty = true;
            if (!frame.CommandList)
                throw std::invalid_argument("SceneRenderer frame requires a command list");
            const bool bOffscreenOnly =
                frame.OutputMode == SceneRendererOutputMode::OffscreenOnly;
            const bool bNeedsOffscreen =
                frame.OutputMode != SceneRendererOutputMode::SwapChain;
            if (!bOffscreenOnly && !frame.BackBuffer)
                throw std::invalid_argument("SceneRenderer swap-chain output requires a backbuffer");
            if (bNeedsOffscreen && !OffscreenOutput)
                throw std::logic_error("SceneRenderer offscreen output is not configured");
			if (frame.bReuseOffscreenScene &&
				frame.OutputMode != SceneRendererOutputMode::SwapChainAndOffscreen)
			{
				throw std::invalid_argument(
					"SceneRenderer offscreen reuse requires SwapChainAndOffscreen output");
			}
            const auto* activeTechnique = GetActiveIlluminationTechnique();
            if (frame.bReuseOffscreenScene && activeTechnique &&
                activeTechnique->RequiresContinuousUpdate())
            {
                throw std::invalid_argument(
                    "SceneRenderer cannot reuse an offscreen scene while global illumination requires continuous updates");
            }
            if (frame.Width == 0 || frame.Height == 0)
                throw std::invalid_argument("SceneRenderer frame dimensions must be non-zero");
            if (bNeedsOffscreen &&
                (OffscreenOutput.Width != frame.Width || OffscreenOutput.Height != frame.Height))
            {
                throw std::invalid_argument(
                    "SceneRenderer frame dimensions do not match the configured offscreen output");
            }
            if (frame.FrameSlot >= FramesInFlight)
                throw std::out_of_range("SceneRenderer frame slot is out of range");

			ProcessGpuCullingReadback(
				frame.FrameSlot,
				frame.CompletedSubmission,
				false);
            CurrentSceneOverlayRecorder = frame.SceneOverlayRecorder;
            CurrentWindowOverlayRecorder = frame.WindowOverlayRecorder;
            CurrentCaptureRecorder = frame.CaptureRecorder;
            if (bLinearHdrCaptureEnabled != static_cast<bool>(frame.LinearHdrCaptureRecorder))
            {
                bLinearHdrCaptureEnabled = static_cast<bool>(frame.LinearHdrCaptureRecorder);
                bGraphDirty = true;
            }
            const FrameCaptureSource requestedCaptureSource = frame.LinearHdrCaptureRecorder
                ? frame.CaptureSource : FrameCaptureSource::Automatic;
            if (CurrentCaptureSource != requestedCaptureSource)
            {
                CurrentCaptureSource = requestedCaptureSource;
                bGraphDirty = true;
            }
            CurrentLinearHdrCaptureRecorder = frame.LinearHdrCaptureRecorder;
            CurrentFrameSlot = frame.FrameSlot;
            CurrentCompletedSubmission = frame.CompletedSubmission;
            CollectIllumination(frame.CompletedSubmission);
            Features->Collect(frame.CompletedSubmission);
            CurrentOutputMode = frame.OutputMode;
            CurrentSceneOverlayMode = frame.SceneOverlayMode;
            CurrentSceneOverlayTransition = frame.SceneOverlayTransition;
            Stats.bCpuCounterFrameValid = false;
            Stats.DirectionalCpuCasterDraws.fill(0);
            Stats.SceneTableUploadedBytes = 0;
            Stats.CpuFramePreparationMilliseconds = 0.0;
            Stats.CpuGraphRecordMilliseconds = 0.0;
            Stats.CpuGraphBuildMilliseconds = 0.0;
            Stats.GraphBuildReasonMask = 0;
            uint32_t graphBuildReasons = (bGraphDirty ? SceneGraphRebuildReason::ExistingDirty : 0u) |
                PendingGraphBuildReasonMask;
            PendingGraphBuildReasonMask = 0;
            Stats.CpuGraphTimingCount = 0;
            Stats.CpuGraphSubmissionId = 0;
            Stats.bCpuGraphTimingValid = false;
            Stats.CpuMaterialPackingMilliseconds = 0.0;
            Stats.MaterialPackingCallCount = 0;
            Stats.MaterialPackingCacheHitCount = 0;
            Stats.bOverlayPassRecorded = false;
            Stats.bCapturePassRecorded = false;
            Stats.bLinearHdrCaptureRecorded = false;
            Stats.bOffscreenOutputRecorded = false;
			Stats.bSceneReused = frame.bReuseOffscreenScene;
			if (!frame.bReuseOffscreenScene)
            {
                static const RenderWorld noShadowCasters;
                if (LocalShadowPass->PrepareFrame(bShadowsEnabled ? world : noShadowCasters, frame.CompletedSubmission))
                { bGraphDirty = true; graphBuildReasons |= SceneGraphRebuildReason::LocalShadow; }
                const auto& localShadowStats = LocalShadowPass->GetStats();
                Stats.LocalShadowActiveViews = localShadowStats.ActiveViews;
                Stats.LocalShadowDirtyViews = localShadowStats.DirtyViews;
                Stats.LocalShadowCachedViews = localShadowStats.ActiveViews >= localShadowStats.DirtyViews
                    ? localShadowStats.ActiveViews - localShadowStats.DirtyViews : 0u;
                Stats.LocalShadowVisibleCasters = localShadowStats.VisibleCasterObjects;
                LightGrid->SetLocalShadowIndices(LocalShadowPass->GetPointShadowViews(), LocalShadowPass->GetSpotShadowViews());
                PrepareLightGrid(world, frame);
                PrepareAntiAliasingFrame(world, frame);
				PrepareFrameData(world);
                PrepareMaterialPublicationHistory(frame);
                PrepareGpuBatchFrame();
                PrepareGpuShadowFrame();
                GpuOcclusionPass->PrepareFrame(CurrentRasterCamera,
                    static_cast<uint32_t>(GpuDrawDataCpu.size()),
                    static_cast<uint32_t>(GpuBatches.size()), frame.CompletedSubmission);
                const bool bPostReset = world.Camera.bCameraCut ||
                    PostProcessSceneGeneration != world.SceneGeneration;
                const float postDelta = PostProcessTime >= 0.0
                    ? static_cast<float>(std::clamp(world.TimeSeconds - PostProcessTime, 0.0, 0.25))
                    : 1.0f / 60.0f;
                PostProcessTime = world.TimeSeconds;
                PostProcessSceneGeneration = world.SceneGeneration;
                PostProcessPass->PrepareFrame(ResolveViewPostProcess(world), postDelta,
                    Exposure, frame.CompletedSubmission, bPostReset);
                if (PostProcessPass->ConsumeGraphRebuildRequest())
                { bGraphDirty = true; graphBuildReasons |= SceneGraphRebuildReason::PostProcess; }
                Features->PrepareFrame(RenderFeatureFrameContext{
                    .World = &world, .RasterCamera = &CurrentRasterCamera, .Clouds = &CloudQualitySettings,
                    .Width = frame.Width, .Height = frame.Height, .CompletedSubmission = frame.CompletedSubmission,
                    .bOffline = IsOfflineViewportRenderingActive() });
                if (Features->ConsumeGraphRebuildRequest())
                { bGraphDirty = true; graphBuildReasons |= SceneGraphRebuildReason::Feature; }
                AmbientOcclusionPass->PrepareFrame(AmbientOcclusionSettings, CurrentRasterCamera,
                    frame.Width, frame.Height, frame.CompletedSubmission);
                if (AmbientOcclusionPass->ConsumeGraphRebuildRequest())
                { bGraphDirty = true; graphBuildReasons |= SceneGraphRebuildReason::AmbientOcclusion; }
                PrepareSceneSurfaceFrame(world, frame);
                PrepareGlobalIlluminationFrame(world, frame);
                ReflectionPass->PrepareFrame(ReflectionSettings, CurrentRasterCamera, frame.Width,
                    frame.Height, FrameData.IblSettings.x, FrameData.IblSettings.y,
                    PreparedReflectionProbes, frame.CompletedSubmission,
                    1.0f - std::clamp(FrameData.IblSettings.w, 0.0f, 1.0f));
                if (ReflectionPass->ConsumeGraphRebuildRequest())
                { bGraphDirty = true; graphBuildReasons |= SceneGraphRebuildReason::Reflection; }
            }
            glm::vec3 vfxLightDirection(0.0f, 1.0f, 0.0f);
            glm::vec3 vfxLightColor(0.0f);
            if (!world.DirectionalLights.empty())
            {
                const auto& light = world.DirectionalLights.front();
                if (glm::dot(light.Direction, light.Direction) > 1e-8f)
                    // Scene lights store the ray direction; Effekseer's lit shader
                    // dots its direction with the surface normal (toward the light).
                    vfxLightDirection = -glm::normalize(light.Direction);
                vfxLightColor = glm::clamp(light.Color * std::clamp(light.Intensity, 0.0f, 1.0f),
                    glm::vec3(0.0f), glm::vec3(1.0f));
            }
            // Effekseer's lit particles use this value as ambient light directly.
            // Match the scene's IBL strength instead of quartering it, which made
            // textured foliage particles nearly black in moderately lit scenes.
            const glm::vec3 vfxAmbientColor(std::clamp(world.Environment.IblIntensity,
                0.03f, 1.0f));
            CurrentVfxView = EffekseerRenderView{
                .View = world.Camera.View,
                .Projection = CurrentRasterCamera.Projection,
                .CameraPosition = world.Camera.Position,
                .ZNear = world.Camera.NearPlane,
                .ZFar = world.Camera.FarPlane,
                .LightDirection = vfxLightDirection,
                .LightColor = vfxLightColor,
                .LightAmbientColor = vfxAmbientColor,
            };
			if (!frame.bReuseOffscreenScene)
            {
				VfxRenderGraphPass.UpdateView(CurrentVfxView);
				const VFXRenderRequirements vfxRequirements =
					EffekseerRuntime::GetRenderRequirements();
				if (vfxRequirements.bHasActiveEffects != CurrentVfxRequirements.bHasActiveEffects ||
					vfxRequirements.bRequiresDepthSnapshot !=
						CurrentVfxRequirements.bRequiresDepthSnapshot ||
					vfxRequirements.bRequiresBackgroundSnapshot !=
						CurrentVfxRequirements.bRequiresBackgroundSnapshot)
				{
					CurrentVfxRequirements = vfxRequirements;
					bGraphDirty = true;
					graphBuildReasons |= SceneGraphRebuildReason::Vfx;
				}
            }
            UpdatePostTaaView();
            RecordedGpuObjectCounts[frame.FrameSlot] =
                static_cast<uint32_t>(GpuObjectData.size());
            RecordedGpuBatchCounts[frame.FrameSlot] =
                static_cast<uint32_t>(GpuBatches.size());
			RecordedGpuReadbacks[frame.FrameSlot] = !frame.bReuseOffscreenScene;
            if (bGraphDirty || LastWidth != frame.Width || LastHeight != frame.Height ||
                LastBackBufferFormat != (bOffscreenOnly
                    ? OffscreenOutput.ColorFormat
                    : frame.BackBufferFormat) ||
                (!bOffscreenOnly && LastBackBufferInitialState !=
                    frame.BackBufferInitialState) ||
                LastOutputMode != frame.OutputMode ||
				bLastReuseOffscreenScene != frame.bReuseOffscreenScene ||
                LastSceneOverlayMode != frame.SceneOverlayMode)
            {
                graphBuildReasons |= Stats.GraphBuildReasonMask;
                if (!bGraphDirty) graphBuildReasons |= SceneGraphRebuildReason::Output;
                if (graphBuildReasons == 0) graphBuildReasons = SceneGraphRebuildReason::ExistingDirty;
                Stats.GraphBuildReasonMask = graphBuildReasons;
                LastWidth = frame.Width;
                LastHeight = frame.Height;
                const auto graphBuildStart = std::chrono::steady_clock::now();
                BuildGraph(frame);
                Stats.CpuGraphBuildMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - graphBuildStart).count();
            }
            else
            {
                UpdateImportedResources(frame);
            }

            GpuOcclusionPass->Collect(frame.CompletedSubmission);
            GeometryArena->Collect(frame.CompletedSubmission);
            DepthIndirectCompaction->Collect(frame.CompletedSubmission);
            MainIndirectCompaction->Collect(frame.CompletedSubmission);
            CollectPathGraphTransactions(frame.CompletedSubmission);
            ResourcePool.Collect(frame.CompletedSubmission);
            ResourcePool.BeginFrame(frame.FrameSlot, frame.CompletedSubmission);
            try
            {
                RuntimeUiRenderPass.UpdateFrame(RuntimeUiRenderFrameState{
                    .Recorder = CurrentSceneOverlayRecorder
                        ? RuntimeUiRenderRecorder(
                            [this](const RuntimeUiRenderContext& context)
                            {
                                CurrentSceneOverlayRecorder(SceneRendererOverlayContext{
                                    .CommandList = context.CommandList,
                                    .Framebuffer = context.Framebuffer,
                                    .BackBuffer = context.ColorTarget,
                                    .StencilTarget = context.StencilTarget,
                                    .BackdropTexture = context.BackdropTexture,
                                    .Width = context.Width,
                                    .Height = context.Height,
                                    .FrameSlot = context.FrameSlot,
                                    .CompletedSubmission = context.CompletedSubmission,
                                });
                            })
                        : RuntimeUiRenderRecorder{},
                    .Transition = CurrentSceneOverlayTransition,
                    .FrameSlot = frame.FrameSlot,
                    .CompletedSubmission = frame.CompletedSubmission,
                });
                GpuTiming.BeginFrame(frame.FrameSlot, frame.CompletedSubmission, frame.CommandList);
                RefreshGpuTimingStats();
                const auto graphRecordStart = std::chrono::steady_clock::now();
                if (bCpuGraphTimingEnabled) CpuGraphObserver.Begin(Stats);
                Graph.Execute(RenderGraphExecutionParams{
                    .CommandList = frame.CommandList,
                    .ResourcePool = &ResourcePool,
                    .Instrumentation = GpuTiming.IsEnabled() ? &GpuTiming : nullptr,
                    .CpuObserver = bCpuGraphTimingEnabled ? &CpuGraphObserver : nullptr,
                    .FrameSlot = frame.FrameSlot,
                });
                if (bCpuGraphTimingEnabled) CpuGraphObserver.End();
                Stats.CpuGraphRecordMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - graphRecordStart).count();
                Stats.IndirectCompactionUploadedBytes = DepthIndirectCompaction->GetStats().UploadedBytes +
                    MainIndirectCompaction->GetStats().UploadedBytes;
                const auto& gpuShadowStats = ShadowGpuSubmission->GetStats();
                Stats.GpuShadowCompactedViews = gpuShadowStats.CompactedViews;
                Stats.GpuShadowResidentBytes = gpuShadowStats.ResidentBytes + gpuShadowStats.RetiredBytes;
                Stats.GpuShadowUploadedBytes = gpuShadowStats.UploadedBytes;
                Stats.SceneTableUploadedBytes += gpuShadowStats.UploadedBytes;
                Stats.SceneTableUploadedBytes += Stats.IndirectCompactionUploadedBytes;
				if (bNeedsOffscreen && !bOffscreenOutputInitialized)
				{
					bOffscreenOutputInitialized = true;
					bGraphDirty = true;
				}
                RefreshRenderGraphMemoryStats();
                GpuTiming.EndFrame(frame.FrameSlot, frame.CommandList);
                Stats.CompiledPasses = Graph.GetCompiledPassNames();
                RememberPathSwitchFrame(frame);
                CurrentSceneOverlayRecorder = {};
                CurrentWindowOverlayRecorder = {};
                CurrentCaptureRecorder = {};
                CurrentLinearHdrCaptureRecorder = {};
            }
            catch (...)
            {
                UploadedSceneTables.clear();
                GpuTiming.AbortFrame(frame.FrameSlot);
                PostProcessPass->AbortFrame();
                Features->AbortFrame();
                AmbientOcclusionPass->AbortFrame();
                GpuOcclusionPass->AbortFrame();
                DepthIndirectCompaction->AbortFrame();
                MainIndirectCompaction->AbortFrame();
                AbortGeometryUploads();
                LocalShadowPass->AbortFrame();
                ShadowGpuSubmission->AbortFrame();
                ReflectionPass->AbortFrame();
                CurrentSceneOverlayRecorder = {};
                CurrentWindowOverlayRecorder = {};
                CurrentCaptureRecorder = {};
                CurrentLinearHdrCaptureRecorder = {};
                ResourcePool.AbortFrame(frame.FrameSlot);
                throw;
            }
        }

        void Resize(uint32_t width, uint32_t height)
        {
            if (width == 0 || height == 0)
                return;
            for (uint32_t frameSlot = 0; frameSlot < FramesInFlight; ++frameSlot)
            {
                if (ResourcePool.IsFrameActive(frameSlot))
                    throw std::logic_error("Cannot resize SceneRenderer with an active frame slot");
            }
            CollectGpuCullingStats();
            // The caller's existing resize idle barrier has completed these
            // submissions. Release retired swap-chain references before resize.
            InvalidatePathSwitchFrame(LastSubmittedSubmission);
            VfxRenderGraphPass.ClearResourceCaches();
            RuntimeUiRenderPass.ClearResourceCaches();
            ShadowFramebuffers.clear();
            EnvironmentBindingSets.clear();
            DirectionalShadowTextures.clear();
            OpaqueFramebuffers.clear();
            ClearRasterCaches();
            ToneFramebuffers.clear();
            ToneBindingSets.clear();
            Graph.Clear();
            GraphGeometryResources.clear();
			GraphMaterialTextures.clear();
            if (GetActiveIlluminationTechnique())
            {
                GetActiveIlluminationTechnique()->ResetHistory(
                    GlobalIlluminationHistoryResetReason::Resize);
                GetActiveIlluminationTechnique()->Clear(
                    std::numeric_limits<uint64_t>::max());
            }
            if (SurfaceDataPassInstance)
                SurfaceDataPassInstance->ClearCaches();
            if (DepthPyramidPassInstance)
                DepthPyramidPassInstance->ClearCaches();
            if (CompositePassInstance)
                CompositePassInstance->ClearCaches();
            // RenderDevice::WaitForIdle guarantees every prior scene submission
            // is complete, including slots that have not been acquired again to
            // publish their completion query through FrameContext.
            ResourcePool.Collect(LastSubmittedSubmission);
            SceneAntiAliasing.Resize(
                width,
                height,
                LastSubmittedSubmission);
            bHasPreviousRasterCamera = false;
            bRasterCameraFrameRecorded = false;
            LastWidth = width;
            LastHeight = height;
            bGraphDirty = true;
            ResourcePool.Collect(std::numeric_limits<uint64_t>::max());
        }

        void ClearCaches()
        {
            MotionHistory.Reset();
            for (uint32_t frameSlot = 0; frameSlot < FramesInFlight; ++frameSlot)
            {
                if (ResourcePool.IsFrameActive(frameSlot))
                    throw std::logic_error("Cannot clear SceneRenderer caches with an active frame slot");
            }
            InvalidatePathSwitchFrame(LastSubmittedSubmission);
            Graph.Clear();
            GraphGeometryResources.clear();
			GraphMaterialTextures.clear();
            CpuDrawItems.clear();
            TransparentDrawItems.clear();
            SkinnedDrawItems.clear();
            BoneMatricesCpu.clear();
            OutlineDrawItems.clear();
            GpuObjectData.clear();
            GpuMaterialDataCpu.clear();
            MaterialParameterRowsCpu.clear();
            GpuBoundsDataCpu.clear();
            GpuDrawDataCpu.clear();
            GpuIndirectArgumentsCpu.clear();
            GpuBatches.clear();
            GpuBatchObjectIndices.clear();
            ShadowDrawItems.clear();
            MeshOrder.clear();
            MeshCache.clear();
            PendingMeshUploads.clear();
            GraphArenaPages.clear();
            GpuStateGroups.clear();
            GpuGroupMapping.clear();
            GeometryArena->Clear(LastSubmittedSubmission);
            DepthIndirectCompaction->Clear(LastSubmittedSubmission);
            MainIndirectCompaction->Clear(LastSubmittedSubmission);
            MeshLodHistoryBySection.clear();
            GpuMaterialRegistry.clear();
            NextGpuMaterialIndex = 1;
            for (auto& [material, textureSet] : MaterialTextureCache)
            {
                (void)material;
                ReleaseMaterialTextureSet(textureSet, LastSubmittedSubmission);
            }
            MaterialTextureCache.clear();
            for (auto& [path, handle] : StaticLightmapTextureCache)
            {
                (void)path;
                TextureManagerInstance.ReleaseTexture(handle, LastSubmittedSubmission);
            }
            StaticLightmapTextureCache.clear();
            for (auto& [material, textureSet] : MaterialParameterTextureCache)
            {
                (void)material;
                ReleaseMaterialParameterTextureSet(textureSet, LastSubmittedSubmission);
            }
            MaterialParameterTextureCache.clear();
            for (auto& [path, handle] : PrimitiveTextureCache)
            {
                (void)path;
                TextureManagerInstance.ReleaseTexture(handle, LastSubmittedSubmission);
            }
            PrimitiveTextureCache.clear();
            VfxRenderGraphPass.ClearResourceCaches();
            RuntimeUiRenderPass.ClearResourceCaches();
            ShadowFramebuffers.clear();
            EnvironmentBindingSets.clear();
            DirectionalShadowTextures.clear();
            OpaqueFramebuffers.clear();
            ClearRasterCaches();
            ToneFramebuffers.clear();
            ToneBindingSets.clear();
            if (HardwareRayTracingScene)
                HardwareRayTracingScene->Clear(
                    std::numeric_limits<uint64_t>::max());
            if (GetActiveIlluminationTechnique())
                GetActiveIlluminationTechnique()->Clear(
                    std::numeric_limits<uint64_t>::max());
            if (SurfaceDataPassInstance)
                SurfaceDataPassInstance->ClearCaches();
            if (DepthPyramidPassInstance)
                DepthPyramidPassInstance->ClearCaches();
            if (CompositePassInstance)
                CompositePassInstance->ClearCaches();
            SceneAntiAliasing.Clear(
                std::numeric_limits<uint64_t>::max());
            PostProcessPass->Clear(std::numeric_limits<uint64_t>::max());
            UpscalingPass->Clear();
            Features->Clear(std::numeric_limits<uint64_t>::max());
            AmbientOcclusionPass->Clear(std::numeric_limits<uint64_t>::max());
            GpuOcclusionPass->Clear(std::numeric_limits<uint64_t>::max());
            ReflectionPass->Clear(std::numeric_limits<uint64_t>::max());
            PostProcessTime = -1.0;
            MaterialTimeSeconds = -1.0;
            PostProcessSceneGeneration = 0;
            bHasPreviousRasterCamera = false;
            bRasterCameraFrameRecorded = false;
            ResetHardwareRayTracingStats();
            Stats.GiStats = {
                .Technique = EffectiveGiSettings.Technique,
                .Status = IsGlobalIlluminationActive()
                    ? "Caches cleared; awaiting the next GI frame."
                    : GiSelection.Reason,
            };
            Stats.StaticOpaqueDrawCount = 0;
            Stats.DirectionalShadowDrawCount = 0;
            Stats.DirectionalShadowCascadeCount = 0;
            Stats.CachedStaticMeshCount = 0;
            Stats.Primitive2DCount = 0;
            Stats.Primitive2DBatchCount = 0;
            Stats.Primitive2DVertexCount = 0;
            Stats.CpuTransparentDrawCount = 0;
            Stats.CpuFallbackDrawCount = 0;
            Stats.CustomMaterialCpuFallbackDrawCount = 0;
            Stats.MaterialProgramCount = 0;
            Stats.MaterialParameterRowCount = 0;
            Stats.MaterialProgramGenerationMax = 0;
			Stats.MaterialDescriptorPageCount = 0;
			Stats.MaterialDescriptorPeakPageTextures = 0;
			Stats.MaterialBindingSetCreationCount = 0;
            Stats.AlphaMaskDrawCount = 0;
            Stats.DepthPrepassDrawCount = 0;
            Stats.DepthPrepassIndirectDrawCount = 0;
            Stats.DepthPrepassSkinnedDrawCount = 0;
            Stats.DepthPrepassAlphaMaskDrawCount = 0;
            Stats.SkinnedDrawCount = 0;
            Stats.SkinnedBoneMatrixCount = 0;
            Stats.SkinnedNonIdentityBoneMatrixCount = 0;
            Stats.EditorOutlineDrawCount = 0;
            FailedMaterialPrograms.clear();
            MaterialPackingCache.Clear();
            bGraphDirty = true;
            ResourcePool.Collect(std::numeric_limits<uint64_t>::max());
        }

        void ProcessGpuCullingReadback(
            uint32_t frameSlot,
            uint64_t completedSubmission,
            bool bForce)
        {
            GpuReadbackSlot& slot = GpuReadbackSlots.at(frameSlot);
            if (!slot.bPending || (!bForce && completedSubmission < slot.Submission))
                return;

            uint32_t visibleCount = 0;
            uint64_t visibleTriangles = 0;
            uint32_t instancedBatchCount = 0;
            uint32_t maxVisibleInstancesPerBatch = 0;
            if (slot.BatchCount > 0)
            {
                const void* mapped = Device->mapBuffer(slot.Buffer, nvrhi::CpuAccessMode::Read);
                if (!mapped)
                    throw std::runtime_error("NVRHI failed to map GPU culling readback buffer");
                const auto* arguments =
                    static_cast<const nvrhi::DrawIndexedIndirectArguments*>(mapped);
                for (uint32_t batchIndex = 0; batchIndex < slot.BatchCount; ++batchIndex)
                {
                    visibleCount += arguments[batchIndex].instanceCount;
                    visibleTriangles += uint64_t(arguments[batchIndex].instanceCount) *
                        (arguments[batchIndex].indexCount / 3u);
                    maxVisibleInstancesPerBatch = std::max(
                        maxVisibleInstancesPerBatch,
                        arguments[batchIndex].instanceCount);
                    if (arguments[batchIndex].instanceCount > 1)
                        ++instancedBatchCount;
                }
                Device->unmapBuffer(slot.Buffer);
            }
            Stats.GpuVisibleObjectCount = visibleCount;
            Stats.GpuVisibleTriangleCount = visibleTriangles;
            Stats.GpuCullingReadbackSubmission = slot.Submission;
            Stats.GpuCullingReadbackInputObjects = slot.ObjectCount;
            Stats.GpuCullingReadbackBatches = slot.BatchCount;
            Stats.GpuCulledObjectCount = slot.ObjectCount - std::min(slot.ObjectCount, visibleCount);
            Stats.GpuInstancedBatchCount = instancedBatchCount;
            Stats.GpuMaxVisibleInstancesPerBatch = maxVisibleInstancesPerBatch;
            slot.bPending = false;
        }

        void CollectGpuCullingStats()
        {
            for (uint32_t frameSlot = 0; frameSlot < FramesInFlight; ++frameSlot)
            {
                ProcessGpuCullingReadback(
                    frameSlot,
                    std::numeric_limits<uint64_t>::max(),
                    true);
            }
        }

        void RefreshGpuTimingStats()
        {
            Stats.GpuRenderGraphAverageMilliseconds = GpuTiming.GetAverageMilliseconds();
            Stats.GpuRenderGraphLastMilliseconds = GpuTiming.GetLastMilliseconds();
            Stats.GpuRenderGraphSampleCount = GpuTiming.GetFrameSampleCount();
            Stats.GpuRenderGraphLastSubmissionId = GpuTiming.GetLastSubmissionId();
            Stats.GpuFrameSpanMilliseconds = GpuTiming.GetLastPassMilliseconds("FrameGpuSpan");
            Stats.GpuPassTimings = GpuTiming.GetLastFramePasses();
            Stats.bGpuGiTimingValid =
                IsGlobalIlluminationActive() && GpuTiming.IsEnabled() &&
                GpuTiming.GetFrameSampleCount() > 0u;
            Stats.GpuRayTracingBlasBuildEventMilliseconds =
                GpuTiming.SumPeakPassMillisecondsWithPrefix(
                    "HardwareRTBLASBuild[");
            Stats.GpuRayTracingTlasBuildEventMilliseconds =
                GpuTiming.SumPeakPassMillisecondsWithPrefix(
                    "HardwareRTTLASBuild");
            Stats.GpuRayTracingTlasUpdateMilliseconds =
                GpuTiming.GetLastPassMilliseconds("HardwareRTTLASUpdate");
            if (IsOfflineViewportRenderingActive())
            {
                Stats.GpuGiCompositeMilliseconds =
                    GpuTiming.GetLastPassMilliseconds(
                        "OfflineViewportComposite");
                Stats.GpuGiTraceMilliseconds =
                    ActiveOfflineViewportRendererDescriptor.GpuTimingPassName.empty()
                    ? 0.0
                    : GpuTiming.GetLastPassMilliseconds(
                        ActiveOfflineViewportRendererDescriptor.GpuTimingPassName);
                Stats.GpuSvgfMilliseconds = 0.0;
            }
            else
            {
                Stats.GpuGiCompositeMilliseconds =
                    GpuTiming.GetLastPassMilliseconds("GIComposite");
                Stats.GpuGiTraceMilliseconds =
                    IsScreenSpaceGlobalIlluminationActive()
                    ? GpuTiming.GetLastPassMilliseconds("SSGIRayMarch") +
                        GpuTiming.SumLastPassMillisecondsWithPrefix(
                            "DepthPyramid")
                    : GpuTiming.GetLastPassMilliseconds(
                        "RTDirectAndIndirectTrace");
                Stats.GpuSvgfMilliseconds =
                    GpuTiming.GetLastPassMilliseconds("TemporalReprojection") +
                    GpuTiming.GetLastPassMilliseconds("HistoryFix") +
                    GpuTiming.GetLastPassMilliseconds("MomentsAndVariance") +
                    GpuTiming.SumLastPassMillisecondsWithPrefix(
                        "ATrousFilter[") +
                    GpuTiming.GetLastPassMilliseconds("BilateralUpsample");
            }
            Stats.GiStats.LastGpuMilliseconds =
                Stats.GpuGiTraceMilliseconds + Stats.GpuSvgfMilliseconds;
            if (ActiveOfflineViewportRenderer)
            {
                ActiveOfflineViewportRenderer->SetLastGpuMilliseconds(
                    Stats.GpuFrameSpanMilliseconds);
            }
        }

        void CollectGpuTimingStats()
        {
            GpuTiming.CollectAll();
            RefreshGpuTimingStats();
        }

        void ConfigureOffscreenOutput(
            uint32_t width,
            uint32_t height,
            std::string debugName,
            nvrhi::Format colorFormat)
        {
            if (width == 0 || height == 0)
                throw std::invalid_argument("Offscreen output dimensions must be non-zero");
            if (colorFormat != nvrhi::Format::RGBA8_UNORM && colorFormat != nvrhi::Format::BGRA8_UNORM)
                throw std::invalid_argument("Offscreen output requires RGBA8_UNORM or BGRA8_UNORM color");
            for (uint32_t frameSlot = 0; frameSlot < FramesInFlight; ++frameSlot)
            {
                if (ResourcePool.IsFrameActive(frameSlot))
                {
                    throw std::logic_error(
                        "Cannot configure an offscreen output with an active frame slot");
                }
            }
            if (OffscreenOutput && OffscreenOutput.Width == width &&
                OffscreenOutput.Height == height && OffscreenOutput.ColorFormat == colorFormat &&
                OffscreenDebugName == debugName)
            {
                return;
            }

            nvrhi::TextureDesc colorDesc;
            colorDesc.width = width;
            colorDesc.height = height;
            colorDesc.format = colorFormat;
            colorDesc.isShaderResource = true;
            colorDesc.isRenderTarget = true;
			colorDesc.initialState = nvrhi::ResourceStates::Common;
            colorDesc.keepInitialState = false;
            colorDesc.debugName = debugName + " Color";
            nvrhi::TextureHandle color = Device->createTexture(colorDesc);

            nvrhi::TextureDesc depthDesc;
            depthDesc.width = width;
            depthDesc.height = height;
            depthDesc.format = c_SceneDepthFormat;
            depthDesc.isShaderResource = true;
            depthDesc.isRenderTarget = true;
			depthDesc.initialState = nvrhi::ResourceStates::Common;
            depthDesc.keepInitialState = false;
            depthDesc.clearValue = nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f);
            depthDesc.useClearValue = true;
            depthDesc.debugName = debugName + " Depth";
            nvrhi::TextureHandle depth = Device->createTexture(depthDesc);
            if (!color || !depth)
                throw std::runtime_error("NVRHI failed to create SceneRenderer offscreen output");

            Graph.Clear();
            GraphGeometryResources.clear();
            OpaqueFramebuffers.clear();
            ClearRasterCaches();
            ToneFramebuffers.clear();
            OffscreenDebugName = std::move(debugName);
            OffscreenOutput = SceneRendererOffscreenOutput{
                .Color = std::move(color),
                .Depth = std::move(depth),
                .Width = width,
                .Height = height,
                .ColorFormat = colorFormat,
                .DepthFormat = c_SceneDepthFormat,
                .Generation = ++OffscreenGeneration,
            };
			bOffscreenOutputInitialized = false;
            bGraphDirty = true;
        }

        void DisableOffscreenOutput()
        {
            if (!OffscreenOutput)
                return;
            for (uint32_t frameSlot = 0; frameSlot < FramesInFlight; ++frameSlot)
            {
                if (ResourcePool.IsFrameActive(frameSlot))
                {
                    throw std::logic_error(
                        "Cannot disable an offscreen output with an active frame slot");
                }
            }
            Graph.Clear();
            GraphGeometryResources.clear();
            OpaqueFramebuffers.clear();
            ClearRasterCaches();
            ToneFramebuffers.clear();
            OffscreenOutput = {};
			bOffscreenOutputInitialized = false;
            ++OffscreenGeneration;
            bGraphDirty = true;
        }

        nvrhi::DeviceHandle Device;
        ShaderManager ShaderManagerInstance;
        ShaderArtifactResolver ShaderArtifacts;
        std::unique_ptr<MaterialShaderManager> OwnedMaterialShaderManager;
        MaterialShaderManager& MaterialShaderManagerInstance;
        ShaderManager& MaterialShaderHandleManagerInstance;
        PipelineCache PipelineCacheInstance;
		std::uint64_t ObservedMaterialShaderPublicationRevision = 0;
        RenderGraphResourcePool ResourcePool;
        TextureManager TextureManagerInstance;
        EffekseerRenderGraphPass VfxRenderGraphPass;
        RuntimeUiRenderGraphPass RuntimeUiRenderPass;
        SceneAntiAliasingPass SceneAntiAliasing;
        Scope<RayTracingScene> HardwareRayTracingScene;
        struct RetiredIllumination
        {
            uint64_t Submission = 0;
            Scope<RayTracingScene> Scene;
            Scope<SceneSurfaceDataPass> Surface;
            Scope<SvgfDenoiser> Denoiser;
            Scope<DepthPyramidPass> Depth;
            GlobalIlluminationTechniqueLease Realtime;
            OfflineViewportRendererSessionLease Offline;
        };
        std::vector<RetiredIllumination> RetiredIlluminationSets;
        Scope<SceneSurfaceDataPass> SurfaceDataPassInstance;
        Scope<SvgfDenoiser> SvgfDenoiserInstance;
        GlobalIlluminationTechniqueRegistry GiTechniqueRegistry;
        GlobalIlluminationTechniqueLease ActiveGiTechnique;
        OfflineViewportRendererSessionLease ActiveOfflineViewportRenderer;
        OfflineViewportRendererProviderDescriptor
            ActiveOfflineViewportRendererDescriptor;
        std::string ActiveOfflineViewportRendererId;
        GlobalIlluminationTechniqueRequiredInput ActiveGiRequiredInputs =
            GlobalIlluminationTechniqueRequiredInput::None;
        GlobalIlluminationTechniqueCapabilityRequirements
            ActiveGiCapabilityRequirements =
                GlobalIlluminationTechniqueCapabilityRequirements::None;
        Scope<DepthPyramidPass> DepthPyramidPassInstance;
        Scope<GlobalIlluminationCompositePass> CompositePassInstance;
        RenderGraph Graph;
        GpuRenderGraphInstrumentation GpuTiming;
        CpuRenderGraphObserver CpuGraphObserver;
        bool bCpuGraphTimingEnabled = false;
        uint32_t FramesInFlight = 0;
        std::filesystem::path ShaderCacheDirectory;
        std::filesystem::path AssetRootDirectory;
        std::filesystem::path EngineAssetRootDirectory;
        RenderBackend Backend = RenderBackend::DX12;
        RenderingPath Path = RenderingPath::Deferred;
        Scope<SceneReflectionPass> ReflectionPass;
        Scope<SceneGpuOcclusionPass> GpuOcclusionPass;
        bool bGpuOcclusionEnabled = false;
        uint64_t TextureStreamingBudgetBytes = 512ull * 1024 * 1024;
        bool bLodEnabled = false;
        bool bShadowsEnabled = true;
        bool bContinuousPcfEnabled = false;
        float LodMaxPixelError = 1.0f;
        SceneReflectionSettings ReflectionSettings;
        std::vector<SceneReflectionProbe> PreparedReflectionProbes;
        bool bProbeBudgetWarning = false;
        CloudSettings CloudQualitySettings;
        Scope<SceneAmbientOcclusionPass> AmbientOcclusionPass;
        SceneAmbientOcclusionSettings AmbientOcclusionSettings;
        TextureHandle CurrentAmbientOcclusion;
        std::map<std::array<const void*, 3>, nvrhi::BindingSetHandle> LightingFrameBindingSets;
        Scope<ScenePostProcessPass> PostProcessPass;
        Scope<RenderFeaturePlan> Features;
        Scope<SceneLocalShadowPass> LocalShadowPass;
        SceneLocalShadowGraphResources LocalShadowGraphResources;
        nvrhi::BufferHandle LocalShadowFrameConstantBuffer;
        nvrhi::BindingSetHandle LocalShadowFrameBindingSet;
        Scope<SceneLightGrid> LightGrid;
        SceneLightGridGraphResources LightGraphResources;
        ScenePostProcessSettings PostProcessSettings;
        double PostProcessTime = -1.0;
        double MaterialTimeSeconds = -1.0;
        uint64_t PostProcessSceneGeneration = 0;
        RenderCapabilities Capabilities;
        AntiAliasingCapabilities AaCapabilities;
        AntiAliasingSettings RequestedAaSettings;
        UpscalingSettings RequestedUpscaling;
        bool bFsrActiveLastFrame = false;
        Scope<SceneUpscalingPass> UpscalingPass;
        AntiAliasingSelection AaSelection;
        RenderCamera CurrentRasterCamera;
        bool bRasterCameraHistoryReset = true;
        glm::mat4 PreviousRasterView{ 1.0f };
        glm::mat4 PendingRasterView{ 1.0f };
        glm::vec2 PendingRasterJitterPixels{ 0.0f };
        glm::vec2 PreviousRasterJitterPixels{ 0.0f };
        uint64_t PendingRasterWorldFrame = 0;
        uint64_t PreviousRasterWorldFrame = 0;
        uint64_t PreviousRasterSubmission = 0;
        glm::mat4 PreviousRasterViewProjection{ 1.0f };
        glm::mat4 PendingRasterViewProjection{ 1.0f };
        glm::mat4 PreviousTaaNonJitteredViewProjection{ 1.0f };
        glm::mat4 PendingTaaNonJitteredViewProjection{ 1.0f };
        uint64_t PreviousRasterJitterGeneration = 0;
        uint64_t PendingRasterJitterGeneration = 0;
        bool bHasPreviousRasterCamera = false;
        bool bRasterCameraFrameRecorded = false;
        uint64_t PendingMaterialShaderPublicationRevision = 0;
        uint64_t SubmittedMaterialShaderPublicationRevision = 0;
        GlobalIlluminationSettings RequestedGiSettings;
        GlobalIlluminationSettings EffectiveGiSettings;
        GlobalIlluminationSettings CurrentIlluminationTechniqueSettings;
        GlobalIlluminationSelection GiSelection;
        RayTracingSceneRevisions CurrentRtRevisions;
        bool bHasCachedRtWorld = false;
        bool bCachedBasePbrMaterials = false;
        uint64_t CachedRtWorldFrame = 0, CachedRtWorldScene = 0, CachedOfflineSceneFingerprint = 0;
        RayTracingSceneGraphResources CurrentRtGraphResources;
        SceneSurfaceData CurrentSurfaceData;
        DepthPyramidOutputs CurrentDepthPyramid;
        GlobalIlluminationOutputs CurrentGiOutputs;
        bool bHeapDirectlyIndexed = false;

        nvrhi::ShaderHandle OpaqueVertexShader;
        nvrhi::ShaderHandle OpaquePixelShader;
        nvrhi::ShaderHandle GpuOpaqueVertexShader;
        nvrhi::ShaderHandle GpuOpaquePixelShader;
        nvrhi::ShaderHandle GpuCullShader;
        nvrhi::ShaderHandle EnvironmentPrecomputeShader;
        nvrhi::ShaderHandle ShadowVertexShader;
        nvrhi::ShaderHandle ShadowPixelShader;
        nvrhi::ShaderHandle SkinnedShadowVertexShader;
        nvrhi::ShaderHandle SkinnedShadowPixelShader;
        nvrhi::ShaderHandle Primitive2DVertexShader;
        nvrhi::ShaderHandle Primitive2DPixelShader;
        nvrhi::ShaderHandle SkinnedVertexShader;
        nvrhi::ShaderHandle SkinnedPixelShader;
        nvrhi::ShaderHandle DepthVertexShader;
        nvrhi::ShaderHandle DepthPixelShader;
        nvrhi::ShaderHandle GpuDepthVertexShader;
        nvrhi::ShaderHandle GpuDepthPixelShader;
        nvrhi::ShaderHandle SkinnedDepthVertexShader;
        nvrhi::ShaderHandle SkinnedDepthPixelShader;
        nvrhi::ShaderHandle OutlineVertexShader;
        nvrhi::ShaderHandle OutlinePixelShader;
        nvrhi::ShaderHandle SkinnedOutlineVertexShader;
        nvrhi::ShaderHandle SkinnedOutlinePixelShader;
        nvrhi::ShaderHandle ToneVertexShader;
        nvrhi::ShaderHandle TonePixelShader;
        nvrhi::ShaderHandle SkyVertexShader;
        nvrhi::ShaderHandle SkyPixelShader;
        nvrhi::BindingLayoutHandle FrameBindingLayout;
        nvrhi::BindingLayoutHandle ObjectBindingLayout;
        nvrhi::BindingLayoutHandle ShadowPassBindingLayout;
        nvrhi::BindingLayoutHandle GpuObjectBindingLayout;
        nvrhi::BindingLayoutHandle GpuCullingBindingLayout;
        nvrhi::BindingLayoutHandle EnvironmentPrecomputeBindingLayout;
        nvrhi::BindingLayoutHandle ShadowTextureBindingLayout;
        nvrhi::BindingLayoutHandle SkyBindingLayout;
        nvrhi::BindingLayoutHandle ToneBindingLayout;
        nvrhi::BindingLayoutHandle Primitive2DBindingLayout;
        nvrhi::BindingSetHandle FrameBindingSet;
        nvrhi::BindingSetHandle ObjectBindingSet;
        nvrhi::BindingSetHandle ShadowPassBindingSet;
        nvrhi::BindingSetHandle GpuObjectBindingSet;
        nvrhi::BindingSetHandle GpuCullingBindingSet;
        nvrhi::BindingSetHandle Primitive2DBindingSet;
        nvrhi::ComputePipelineHandle GpuCullingPipeline;
        nvrhi::ComputePipelineHandle EnvironmentPrecomputePipeline;
        nvrhi::InputLayoutHandle InputLayout;
        nvrhi::InputLayoutHandle GpuInputLayout;
        nvrhi::InputLayoutHandle GpuDepthInputLayout;
        nvrhi::InputLayoutHandle GpuUnculledDepthInputLayout;
        nvrhi::InputLayoutHandle ShadowInputLayout;
        nvrhi::InputLayoutHandle SkinnedShadowInputLayout;
        nvrhi::InputLayoutHandle Primitive2DInputLayout;
        nvrhi::InputLayoutHandle SkinnedInputLayout;
        nvrhi::InputLayoutHandle OutlineInputLayout;
        nvrhi::InputLayoutHandle SkinnedOutlineInputLayout;
        nvrhi::SamplerHandle ToneSampler;
        nvrhi::SamplerHandle ShadowSampler;
        nvrhi::SamplerHandle ShadowComparisonSampler;
        nvrhi::SamplerHandle EnvironmentSampler;
        nvrhi::SamplerHandle EquirectangularSampler;

        nvrhi::BufferHandle FrameConstantBuffer;
        nvrhi::BufferHandle ObjectConstantBuffer;
        nvrhi::BufferHandle GpuObjectBuffer;
        nvrhi::BufferHandle GpuMaterialBuffer;
        nvrhi::BufferHandle MaterialParameterBuffer;
        nvrhi::BufferHandle BoneMatrixBuffer;
        nvrhi::BufferHandle PreviousBoneMatrixBuffer;
        nvrhi::BufferHandle GpuBoundsBuffer;
        nvrhi::BufferHandle GpuDrawDataBuffer;
        nvrhi::BufferHandle VisibleObjectIndicesBuffer;
        nvrhi::BufferHandle IndirectArgumentsBuffer;
        std::vector<GpuReadbackSlot> GpuReadbackSlots;
        std::vector<Primitive2DBufferSlot> Primitive2DBufferSlots;
        std::vector<uint32_t> RecordedGpuObjectCounts;
        std::vector<uint32_t> RecordedGpuBatchCounts;
		std::vector<bool> RecordedGpuReadbacks;
        nvrhi::TextureHandle DefaultEnvironment;
        nvrhi::TextureHandle DefaultBrdfLut;
        nvrhi::TextureHandle EnvironmentDummyOutputCube;
        nvrhi::TextureHandle EnvironmentDummyOutputBrdf;
        nvrhi::TextureHandle BrdfLut;
        nvrhi::TextureHandle ActiveIrradiance;
        nvrhi::TextureHandle ActivePrefilter;
        nvrhi::TextureHandle ActiveEnvironment;
        nvrhi::TextureHandle ActiveEnvironmentCdf;
        std::shared_ptr<const EnvironmentSamplingProposal> ActiveEnvironmentProposal;
        nvrhi::TextureHandle ActiveBrdfLut;
        SceneRendererOffscreenOutput OffscreenOutput;
		bool bOffscreenOutputInitialized = false;
        std::string OffscreenDebugName = "SceneRenderer Offscreen Output";
        uint64_t OffscreenGeneration = 0;
        bool bFrameConstantsInitialized = false;
        bool bObjectConstantsInitialized = false;
        bool bDefaultEnvironmentUploaded = false;
		bool bLastReuseOffscreenScene = false;
        bool bRunEmptyEffekseerPassForValidation = false;

        std::unordered_map<const SubMesh*, MeshEntry> MeshCache;
        uint64_t CurrentMaterialFrame = 0;
        uint64_t EnvironmentPumpFrame = 0;
        std::unordered_map<const Material*, MaterialTextureSet> MaterialTextureCache;
        std::unordered_map<std::string, ManagedTextureHandle> StaticLightmapTextureCache;
        bool bLoggedUnsupportedStaticLightmaps = false;
        std::unordered_map<const Material*, MaterialParameterTextureSet>
            MaterialParameterTextureCache;
        std::unordered_map<std::string, ManagedTextureHandle> PrimitiveTextureCache;
        std::unordered_map<
            MaterialRegistryKey,
            uint32_t,
            MaterialRegistryKeyHash> GpuMaterialRegistry;
        std::unordered_map<std::string, EnvironmentEntry> EnvironmentCache;
        std::vector<RetiredEnvironmentEntry> RetiredEnvironmentEntries;
        uint64_t EnvironmentSceneGeneration = std::numeric_limits<uint64_t>::max();
        std::string ActiveEnvironmentKey;
        std::unordered_set<std::string> FailedMaterialTextures;
        std::unordered_set<std::string> FailedMaterialPrograms;
        MaterialParameterPackingCache MaterialPackingCache;
        const bool bMaterialPackingCacheEnabled = [] {
#if defined(HE_DEV_AGENT_RUNTIME)
            // Benchmark-only override, sampled once per process. No native API.
            static const bool enabled = [] {
                const char* value = std::getenv("HE_RENDER_DISABLE_MATERIAL_PACK_CACHE");
                return !value || std::string_view(value) != "1";
            }();
            return enabled;
#else
            return true;
#endif
        }();
        std::vector<const SubMesh*> MeshOrder;
        std::vector<GraphGeometry> GraphGeometryResources;
        std::vector<GraphMaterialTexture> GraphMaterialTextures;
        std::vector<DrawItem> CpuDrawItems;
        std::vector<DrawItem> SkinnedDrawItems;
        std::vector<DrawItem> TransparentDrawItems;
        std::vector<DrawItem> ShadowDrawItems;
        std::vector<DrawItem> OutlineDrawItems;
        std::vector<glm::mat4> BoneMatricesCpu;
        std::vector<glm::mat4> PreviousBoneMatricesCpu;
        SubmittedMotionHistory MotionHistory;
        SsgiReactiveBudget ReactiveBudget;
        std::vector<ObjectConstants> GpuObjectData;
        std::vector<GpuMaterialData> GpuMaterialDataCpu;
        std::vector<glm::vec4> MaterialParameterRowsCpu;
        std::vector<GpuBoundsData> GpuBoundsDataCpu;
        std::vector<GpuDrawData> GpuDrawDataCpu;
        std::vector<nvrhi::DrawIndexedIndirectArguments> GpuIndirectArgumentsCpu;
        std::vector<GpuBatch> GpuBatches;
        std::vector<std::vector<uint32_t>> GpuBatchObjectIndices;
		std::vector<MaterialDescriptorPageState> MaterialPages;
		std::vector<RetiredMaterialDescriptorPages> RetiredMaterialPages;
		std::vector<uint32_t> MaterialPageByMaterialIndex;
        std::vector<Primitive2DVertex> Primitive2DVertices;
        std::vector<Primitive2DBatch> Primitive2DBatches;

        std::unordered_map<
            OpaqueFramebufferKey,
            nvrhi::FramebufferHandle,
            OpaqueFramebufferKeyHash> OpaqueFramebuffers;
        std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> ToneFramebuffers;
        std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> ToneBindingSets;
        std::unordered_map<
            ShadowFramebufferKey,
            nvrhi::FramebufferHandle,
            ShadowFramebufferKeyHash> ShadowFramebuffers;
        std::unordered_map<
            EnvironmentBindingKey,
            nvrhi::BindingSetHandle,
            EnvironmentBindingKeyHash> EnvironmentBindingSets;
        // Non-owning keys for per-frame physical directional-shadow arrays.
        std::unordered_set<nvrhi::ITexture*> DirectionalShadowTextures;
        std::unordered_map<
            SkyBindingKey,
            nvrhi::BindingSetHandle,
            SkyBindingKeyHash> SkyBindingSets;

        FrameConstants FrameData;
        EffekseerRenderView CurrentVfxView;
        VFXRenderRequirements CurrentVfxRequirements;
        SceneRendererOverlayRecorder CurrentSceneOverlayRecorder;
        SceneRendererOverlayRecorder CurrentWindowOverlayRecorder;
        SceneRendererCaptureRecorder CurrentCaptureRecorder;
        SceneRendererLinearHdrCaptureRecorder CurrentLinearHdrCaptureRecorder;
        bool bLinearHdrCaptureEnabled = false;
        FrameCaptureSource CurrentCaptureSource = FrameCaptureSource::Automatic;
        TextureHandle NativeGiDiagnosticInput;
        RuntimeUiTransitionParameters CurrentSceneOverlayTransition;
        RuntimeUiRenderMode CurrentSceneOverlayMode = RuntimeUiRenderMode::Direct;
        uint32_t CurrentFrameSlot = 0;
        uint64_t CurrentCompletedSubmission = 0;
        float Exposure = 1.0f;
        TextureHandle BackBufferHandle;
        TextureHandle SceneColorHandle;
        TextureHandle OpaqueDirectHdrHandle;
        TextureHandle ToneMappedSceneHandle;
        TextureHandle SceneDepthHandle;
        TextureHandle RasterSceneDepthHandle;
        bool bPostTaaDepthRequired = false;
        TextureHandle DirectionalShadowHandle;
        TextureHandle IrradianceHandle;
        TextureHandle PrefilterHandle;
        TextureHandle EnvironmentHandle;
        TextureHandle EnvironmentCdfHandle;
        TextureHandle BrdfLutHandle;
        TextureHandle OffscreenColorHandle;
        TextureHandle OffscreenDepthHandle;
        TextureHandle RuntimeUiBackdropHandle;
        BufferHandle FrameBufferHandle;
        BufferHandle ObjectBufferHandle;
        BufferHandle GpuObjectsHandle;
        BufferHandle GpuMaterialsHandle;
        BufferHandle MaterialParametersHandle;
        BufferHandle GpuBoundsHandle;
        BufferHandle GpuDrawDataHandle;
        BufferHandle VisibleObjectIndicesHandle;
        BufferHandle IndirectArgumentsHandle;
        BufferHandle GpuReadbackHandle;
        BufferHandle Primitive2DVerticesHandle;
        BufferHandle BoneMatricesHandle;
        BufferHandle PreviousBoneMatricesHandle;
        uint32_t LastOutputWidth = 0;
        uint32_t LastOutputHeight = 0;
        uint32_t LastWidth = 0;
        uint32_t LastHeight = 0;
        nvrhi::Format LastBackBufferFormat = nvrhi::Format::UNKNOWN;
        nvrhi::ResourceStates LastBackBufferInitialState =
            nvrhi::ResourceStates::Unknown;
        SceneRendererOutputMode CurrentOutputMode = SceneRendererOutputMode::SwapChain;
        SceneRendererOutputMode LastOutputMode = SceneRendererOutputMode::SwapChain;
        RuntimeUiRenderMode LastSceneOverlayMode = RuntimeUiRenderMode::Direct;
        uint64_t LastSubmittedSubmission = 0;
        uint64_t GlobalIlluminationRenderSequence = 0;
        uint64_t GpuSceneGeneration = std::numeric_limits<uint64_t>::max();
        std::unordered_map<nvrhi::IBuffer*, std::vector<uint8_t>> UploadedSceneTables;
        uint32_t NextGpuMaterialIndex = 1;
        uint64_t LastTextureManagerRevision = 0;
        uint32_t PendingGraphBuildReasonMask = 0;
        uint32_t DirectionalShadowCascadeCount = 1;
        uint32_t DirectionalShadowMapSize = c_DefaultDirectionalShadowMapSize;
        bool bGraphDirty = true;
        bool bGlobalIlluminationFrameRecorded = false;
        bool bDirectionalShadowEnabled = false;
        bool bDirectionalShadowCascadeDebug = false;
        ScenePbrDebugMode CurrentPbrDebugMode = ScenePbrDebugMode::Disabled;
        SceneRendererStats Stats;
        #include "SceneRendererSwitch.inl"
    };

    SceneRenderer::SceneRenderer(const SceneRendererDesc& desc)
        : mImpl(std::make_unique<Impl>(desc))
    {
    }

    SceneRenderer::~SceneRenderer() = default;

    MaterialShaderRefreshResult SceneRenderer::RefreshMaterialShaderAtFrameBoundary(
        std::string_view sourcePath)
    {
        return mImpl->RefreshMaterialShaderAtFrameBoundary(sourcePath);
    }

    void SceneRenderer::ClearMaterialShaderPipelineCache()
    {
        mImpl->PipelineCacheInstance.Clear();
		mImpl->ObservedMaterialShaderPublicationRevision =
			mImpl->MaterialShaderManagerInstance.GetPublicationRevision();
    }

    Json SceneRenderer::GetRecordedRasterCaptureMetadata() const
    {
        // Called before submission commits/overwrites the previous raster state.
        if (!mImpl->bRasterCameraFrameRecorded || mImpl->Stats.bSceneReused ||
            mImpl->IsOfflineViewportRenderingActive() || mImpl->LastWidth == 0 || mImpl->LastHeight == 0)
            return Json{ {"valid", false}, {"reason", "no-fresh-main-raster-recording"} };
        const std::array<const glm::mat4*, 9> matrices = {
            &mImpl->CurrentRasterCamera.View, &mImpl->CurrentRasterCamera.Projection,
            &mImpl->FrameData.ViewProjection, &mImpl->FrameData.InverseViewProjection,
            &mImpl->FrameData.PreviousView, &mImpl->FrameData.PreviousViewProjection,
            &mImpl->PendingTaaNonJitteredViewProjection, &mImpl->PreviousTaaNonJitteredViewProjection,
            &mImpl->PendingRasterViewProjection };
        for (const auto* value : matrices)
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    if (!std::isfinite((*value)[column][row]))
                        return Json{ {"valid", false}, {"reason", "nonfinite-raster-matrix"} };
        const auto matrix = [](const glm::mat4& value) {
            Json result = Json::array();
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                {
                    result.push_back(value[column][row]);
                }
            return result;
        };
        const auto vector = [](const glm::vec2& value) { return Json::array({value.x, value.y}); };
        const bool reset = mImpl->bRasterCameraHistoryReset;
        const auto& currentJitter = mImpl->PendingRasterJitterPixels;
        const auto& previousJitter = reset ? currentJitter : mImpl->PreviousRasterJitterPixels;
        const auto uploads = mImpl->TextureManagerInstance.GetUploadStatistics();
        return Json{
            {"valid", true}, {"provenance", "recorded SceneRenderer FrameData shader constants before submission"},
            {"worldFrame", mImpl->PendingRasterWorldFrame},
            {"mainAabbBroadPhase", { {"enabled", mImpl->bGpuTightFrustumEnabled},
                {"testedCandidates", mImpl->Stats.MainAabbTestedCandidates},
                {"rejectedCandidates", mImpl->Stats.MainAabbRejectedCandidates},
                {"rejectedCandidateTriangles", mImpl->Stats.MainAabbRejectedCandidateTriangles},
                {"countDomain", "CPU pre-frustum candidates; not incremental GPU savings"} }},
            {"width", mImpl->LastWidth}, {"height", mImpl->LastHeight},
            {"taaActive", mImpl->IsTaaActive()}, {"historyReset", reset},
            {"upscaling", mImpl->UpscalingPass->GetDiagnostics()},
            {"textureStreaming", {
                {"budgetBytes", uploads.StreamingBudgetBytes},
                {"plannedBytes", uploads.StreamingPlannedBytes},
                {"residentPendingRetiringBytes", uploads.StreamingAccountedBytes},
                {"textures", uploads.StreamableTextures},
                {"transitions", uploads.StreamingTransitions},
                {"budgetStalls", uploads.StreamingBudgetStalls},
                {"descriptorReplacements", uploads.DescriptorReplacements},
                {"descriptorPublicationStalls", uploads.DescriptorPublicationStalls},
                {"minimumOverrun", uploads.bStreamingMinimumOverrun},
                {"pendingUploads", uploads.Pending},
                {"failedUploads", uploads.Failed},
                {"accounting", "texture payload and reserved replacements; not physical VRAM"} }},
            {"jitterSequenceGeneration", mImpl->CurrentRasterCamera.JitterSequenceGeneration},
            {"previousSubmittedFrameValid", !reset && mImpl->PreviousRasterSubmission != 0},
            {"previousWorldFrame", reset ? Json(nullptr) : Json(mImpl->PreviousRasterWorldFrame)},
            {"previousSubmissionId", reset ? Json(nullptr) : Json(mImpl->PreviousRasterSubmission)},
            {"previousMatrixOrigin", reset ? "current-frame-history-reset" : "previous-successfully-submitted-raster-frame"},
            {"currentViewColumnMajor", matrix(mImpl->CurrentRasterCamera.View)},
            {"currentProjectionColumnMajor", matrix(mImpl->CurrentRasterCamera.Projection)},
            {"currentViewProjectionColumnMajor", matrix(mImpl->FrameData.ViewProjection)},
            {"currentInverseViewProjectionColumnMajor", matrix(mImpl->FrameData.InverseViewProjection)},
            {"previousViewColumnMajor", matrix(mImpl->FrameData.PreviousView)},
            {"previousViewProjectionColumnMajor", matrix(mImpl->FrameData.PreviousViewProjection)},
            {"currentNonJitteredViewProjectionColumnMajor", matrix(mImpl->PendingTaaNonJitteredViewProjection)},
            {"previousNonJitteredViewProjectionColumnMajor", matrix(reset
                ? mImpl->PendingTaaNonJitteredViewProjection : mImpl->PreviousTaaNonJitteredViewProjection)},
            {"currentJitterPixels", vector(currentJitter)}, {"previousJitterPixels", vector(previousJitter)},
            {"jitterConvention", "clip translation: x=2*jitterPixels.x/width, y=-2*jitterPixels.y/height"},
            {"matrixConvention", "GLM column-major, column vectors, clip=VP*world; device depth [0,1]"},
            {"depthAttachment", "SceneDepthHandle (main pre-AA raster depth, not post-TAA overlay matching depth)"},
            {"motionAttachment", "RasterSurfaceTargets[3]; use existing shader motion-vector encoding contract"}
        };
    }

#if defined(HE_DEV_AGENT_RUNTIME)
    bool SceneRenderer::InjectUpscalingFailureForValidation(bool duringDispatch)
    {
        return mImpl->UpscalingPass->InjectFailureForValidation(duringDispatch);
    }
#endif

    bool SceneRenderer::IsCaptureSourceAvailable(FrameCaptureSource source) const noexcept
    {
        // Read-only preflight of the last main graph. The actual requested frame
        // checks again during graph building, so queued changes cannot fake data.
        switch (source)
        {
        case FrameCaptureSource::Automatic: return true;
        case FrameCaptureSource::Depth: return static_cast<bool>(mImpl->SceneDepthHandle);
        case FrameCaptureSource::Motion: return static_cast<bool>(mImpl->RasterSurfaceTargets[3]);
        case FrameCaptureSource::Normal: return static_cast<bool>(mImpl->RasterSurfaceTargets[1]);
        case FrameCaptureSource::SurfaceId: return static_cast<bool>(mImpl->RasterSurfaceTargets[4]);
        case FrameCaptureSource::Reactive: return static_cast<bool>(mImpl->UpscalingPass->GetReactiveMask());
        case FrameCaptureSource::Composition: return static_cast<bool>(mImpl->UpscalingPass->GetCompositionMask());
        case FrameCaptureSource::UpscaleMotion: return static_cast<bool>(mImpl->UpscalingPass->GetMotionVectors());
        case FrameCaptureSource::UpscaleColor: return static_cast<bool>(mImpl->UpscalingPass->GetInputColor());
        case FrameCaptureSource::UpscaleOpaque: return static_cast<bool>(mImpl->UpscalingPass->GetOpaqueColor());
        case FrameCaptureSource::DirectionalShadow:
            return !mImpl->IsOfflineViewportRenderingActive() && static_cast<bool>(mImpl->DirectionalShadowHandle);
        }
        return false;
    }

    void SceneRenderer::RecordFrame(const RenderWorld& world, const SceneRendererFrameDesc& frame)
    {
        if (mImpl->ActiveOfflineViewportRenderer &&
            !OfflineViewportRendererProviderRegistry::Get().Contains(
                mImpl->ActiveOfflineViewportRenderer->GetProviderId()))
        {
            // Revocation retires the lease before opening a graph frame slot.
            // Already submitted GPU/export/CPU work remains owned until Collect.
            DeactivateOfflineViewportRenderer();
        }
        if (frame.FrameSlot >= mImpl->FramesInFlight)
            throw std::out_of_range("SceneRenderer frame slot is out of range");
        try
        {
            if (!frame.bReuseOffscreenScene)
            {
                if (frame.WorldSnapshot && frame.WorldSnapshot.get() != &world)
                    throw std::invalid_argument("SceneRenderer snapshot does not match its frame world");
                mImpl->MotionHistory.Stage(frame.WorldSnapshot
                    ? frame.WorldSnapshot : CreateRef<RenderWorld>(world));
            }
            mImpl->RecordFrame(world, frame);
        }
        catch (...)
        {
            // Preparation can fail before the caller marks this view recorded.
            AbortFrame(frame.FrameSlot);
            throw;
        }
    }

    void SceneRenderer::SetDirectionalShadowCascadeDebug(bool bEnabled) noexcept
    {
        mImpl->bDirectionalShadowCascadeDebug = bEnabled;
    }

    void SceneRenderer::SetPbrDebugMode(ScenePbrDebugMode mode) noexcept
    {
        mImpl->bGraphDirty |= mImpl->CurrentPbrDebugMode != mode;
        mImpl->CurrentPbrDebugMode = mode;
    }

    void SceneRenderer::SetAntiAliasingSettings(
        const AntiAliasingSettings& settings)
    {
        ValidateAntiAliasingSettings(settings);
        for (uint32_t frameSlot = 0; frameSlot < mImpl->FramesInFlight;
            ++frameSlot)
        {
            if (mImpl->ResourcePool.IsFrameActive(frameSlot))
            {
                throw std::logic_error(
                    "Cannot configure anti-aliasing with an active frame slot");
            }
        }
        if (mImpl->RequestedAaSettings == settings)
            return;
        mImpl->Graph.Clear();
        mImpl->GraphGeometryResources.clear();
        mImpl->VfxRenderGraphPass.ClearResourceCaches();
        mImpl->RuntimeUiRenderPass.ClearResourceCaches();
        mImpl->OpaqueFramebuffers.clear();
        mImpl->ClearRasterCaches();
        mImpl->ToneFramebuffers.clear();
        mImpl->ToneBindingSets.clear();
        if (mImpl->CompositePassInstance)
            mImpl->CompositePassInstance->ClearCaches();
        mImpl->RequestedAaSettings = settings;
        mImpl->RefreshAntiAliasingSelection();
        mImpl->SceneAntiAliasing.ResetHistory();
        mImpl->bHasPreviousRasterCamera = false;
        if (mImpl->IsOfflineViewportRendererActive())
        {
            mImpl->bGraphDirty = true;
            return;
        }
        mImpl->ConfigureGlobalIllumination(mImpl->RequestedGiSettings);
    }

    const AntiAliasingSettings&
        SceneRenderer::GetAntiAliasingSettings() const noexcept
    {
        return GetRequestedAntiAliasingSettings();
    }

    const AntiAliasingSettings&
        SceneRenderer::GetRequestedAntiAliasingSettings() const noexcept
    {
        return mImpl->RequestedAaSettings;
    }

    const AntiAliasingSettings&
        SceneRenderer::GetEffectiveAntiAliasingSettings() const noexcept
    {
        return mImpl->AaSelection.Effective;
    }

    const AntiAliasingCapabilities&
        SceneRenderer::GetAntiAliasingCapabilities() const noexcept
    {
        return mImpl->AaCapabilities;
    }

    AntiAliasingSelection SceneRenderer::GetAntiAliasingSelection() const
    {
        return mImpl->AaSelection;
    }

    void SceneRenderer::RestartAntiAliasingConvergence() noexcept
    {
        mImpl->SceneAntiAliasing.RestartConvergence();
    }

    void SceneRenderer::SetGlobalIlluminationSettings(
        const GlobalIlluminationSettings& settings)
    {
        for (uint32_t frameSlot = 0; frameSlot < mImpl->FramesInFlight; ++frameSlot)
        {
            if (mImpl->ResourcePool.IsFrameActive(frameSlot))
            {
                throw std::logic_error(
                    "Cannot configure global illumination with an active frame slot");
            }
        }
        if (mImpl->IsOfflineViewportRendererActive())
        {
            // Preserve realtime 3D GI as pending state. The offline provider
            // owns its own settings and remains the sole active renderer.
            mImpl->RequestedGiSettings = settings;
            return;
        }
        const GlobalIlluminationSettings& previous =
            mImpl->RequestedGiSettings;
        const bool bControlOnlyChange =
            previous.Technique == settings.Technique &&
            previous.Denoiser == settings.Denoiser &&
            previous.UnsupportedFallback == settings.UnsupportedFallback &&
            previous.RenderScale == settings.RenderScale &&
            previous.SamplesPerPixel == settings.SamplesPerPixel &&
            previous.MaxBounces == settings.MaxBounces &&
            previous.AtrousIterations == settings.AtrousIterations &&
            previous.bSvgfOptimizedKernels == settings.bSvgfOptimizedKernels &&
            previous.bSsgiLocalReactiveBudget == settings.bSsgiLocalReactiveBudget &&
            previous.bSvgfSharedUncertainty == settings.bSvgfSharedUncertainty &&
            previous.bSsgiEnvironmentMis == settings.bSsgiEnvironmentMis &&
            previous.ScreenSpaceRayCount == settings.ScreenSpaceRayCount &&
            previous.ScreenSpaceStepCount == settings.ScreenSpaceStepCount &&
            previous.ScreenSpaceHiZStartMip == settings.ScreenSpaceHiZStartMip &&
            previous.ScreenSpaceBinaryRefinementSteps ==
                settings.ScreenSpaceBinaryRefinementSteps &&
            previous.ScreenSpaceMaximumRayDistance ==
                settings.ScreenSpaceMaximumRayDistance &&
            previous.ScreenSpaceThickness == settings.ScreenSpaceThickness &&
            previous.ScreenSpaceEdgeFade == settings.ScreenSpaceEdgeFade &&
            previous.ScreenSpaceDiffuseHitDistanceWeight == settings.ScreenSpaceDiffuseHitDistanceWeight &&
            previous.ScreenSpaceTemporalWeight ==
                settings.ScreenSpaceTemporalWeight &&
            previous.bDirectOnly == settings.bDirectOnly &&
            previous.bBilateralUpsample == settings.bBilateralUpsample &&
            previous.DebugView != settings.DebugView;
        mImpl->Graph.Clear();
        mImpl->GraphGeometryResources.clear();
        if (bControlOnlyChange)
        {
            mImpl->RequestedGiSettings.DebugView = settings.DebugView;
            mImpl->EffectiveGiSettings.DebugView = settings.DebugView;
            mImpl->bGraphDirty = true;
            return;
        }
        mImpl->ConfigureGlobalIllumination(settings);
    }

    SceneRendererConfigurationPreparationResult SceneRenderer::PrepareRenderingConfiguration(
        const SceneRendererDesc& target, const RenderWorld& world,
        const SceneRendererFrameDesc* preparationFrame)
    {
        return mImpl->PrepareConfigurationGraphTransaction(target, world, preparationFrame);
    }

    void SceneRenderer::CommitPreparedRenderingConfiguration() noexcept
    {
        mImpl->CommitConfigurationGraphTransaction();
    }

    void SceneRenderer::AbortPreparedRenderingConfiguration() noexcept
    {
        mImpl->PreparedGraphSwitch.reset();
    }

    void SceneRenderer::PrepareRenderingPath(RenderingPath path)
    {
        if (path == RenderingPath::Forward)
            return;
        mImpl->PrepareRasterShaders();
        // Validate shaders, target formats and all built-in PSOs without
        // touching the active graph or its in-flight allocations.
        const std::array<nvrhi::Format, 5> formats = {
            nvrhi::Format::RGBA8_UNORM, nvrhi::Format::RGBA8_UNORM,
            nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA16_FLOAT,
            nvrhi::Format::RG32_UINT };
        std::vector<nvrhi::TextureHandle> targets;
        nvrhi::FramebufferDesc framebufferDesc;
        for (const auto format : formats)
        {
            targets.push_back(mImpl->Device->createTexture(nvrhi::TextureDesc()
                .setWidth(4).setHeight(4).setFormat(format).setIsRenderTarget(true)));
            if (!targets.back())
                throw std::runtime_error("Deferred candidate GBuffer allocation failed");
            framebufferDesc.addColorAttachment(targets.back());
        }
        auto depth = mImpl->Device->createTexture(nvrhi::TextureDesc()
            .setWidth(4).setHeight(4).setFormat(c_SceneDepthFormat).setIsRenderTarget(true));
        framebufferDesc.setDepthAttachment(depth);
        auto framebuffer = mImpl->Device->createFramebuffer(framebufferDesc);
        if (!depth || !framebuffer)
            throw std::runtime_error("Deferred candidate framebuffer preparation failed");
        for (uint32_t vertexPath = 0; vertexPath < 3; ++vertexPath)
            for (bool bDoubleSided : {false, true})
                if (!mImpl->GetRasterPipeline(framebuffer, vertexPath, bDoubleSided))
                    throw std::runtime_error("Deferred candidate PSO preparation failed");
        for (const auto* items : {&mImpl->CpuDrawItems, &mImpl->SkinnedDrawItems})
            for (const auto& item : *items)
                if (item.MaterialProgram)
                    (void)mImpl->GetOrCreateMaterialPipeline(framebuffer, *item.MaterialProgram,
                        MaterialShaderPass::GBuffer, items == &mImpl->SkinnedDrawItems,
                        item.bDoubleSided, false);
    }

    void SceneRenderer::SetReflectionSettings(const SceneReflectionSettings& settings)
    {
        if (mImpl->ReflectionSettings == settings) return;
        mImpl->ReflectionSettings = settings;
        mImpl->bGraphDirty = true;
        mImpl->SceneAntiAliasing.ResetHistory();
    }

    void SceneRenderer::SetGpuOcclusionEnabled(bool bEnabled)
    {
        if (mImpl->bGpuOcclusionEnabled == bEnabled) return;
        mImpl->bGpuOcclusionEnabled = bEnabled;
        mImpl->RefreshAntiAliasingSelection();
        mImpl->bGraphDirty = true;
    }

    void SceneRenderer::SetGpuBatchingEnabled(bool bEnabled)
    {
        if (bEnabled && !SupportsCountedIndirectDraws(mImpl->Capabilities))
        {
            HE_CORE_WARN("GPU compacted draw groups are unavailable; keeping individual GPU-culled indirect draws");
            bEnabled = false;
        }
        if (mImpl->bGpuBatchingEnabled == bEnabled) return;
        mImpl->bGpuBatchingEnabled = bEnabled;
        mImpl->bGraphDirty = true;
    }

    void SceneRenderer::SetGpuShadowBatchingEnabled(bool bEnabled)
    {
        if (bEnabled && !SupportsCountedIndirectDraws(mImpl->Capabilities))
        {
            HE_CORE_WARN("GPU compacted shadow draw groups are unavailable; keeping individual GPU-culled indirect draws");
            bEnabled = false;
        }
        if (mImpl->bGpuShadowBatchingEnabled == bEnabled) return;
        mImpl->bGpuShadowBatchingEnabled = bEnabled;
        mImpl->bGraphDirty = true;
    }

    void SceneRenderer::SetLodSettings(bool bEnabled, float maxPixelError)
    {
        if (!std::isfinite(maxPixelError) || maxPixelError < 0.1f || maxPixelError > 8.0f)
            throw std::invalid_argument("LOD maximum pixel error must be in [0.1, 8]");
        if (mImpl->bLodEnabled == bEnabled && mImpl->LodMaxPixelError == maxPixelError) return;
        mImpl->bLodEnabled = bEnabled;
        mImpl->LodMaxPixelError = maxPixelError;
        mImpl->bGraphDirty = true;
        mImpl->SceneAntiAliasing.ResetHistory();
    }

    void SceneRenderer::PrepareReflectionProbeResources(const RenderWorld& world)
    {
        mImpl->PreparedReflectionProbes.clear();
        if (!mImpl->ReflectionSettings.bLocalProbesEnabled) return;
        std::vector<const RenderReflectionProbe*> candidates;
        for (const auto& probe : world.ReflectionProbes)
            if (!probe.HdrTexturePath.empty()) candidates.push_back(&probe);
        const auto distance = [&](const RenderReflectionProbe* probe)
        {
            const glm::vec3 delta = world.Camera.Position -
                glm::clamp(world.Camera.Position, probe->BoundsMin, probe->BoundsMax);
            return glm::dot(delta, delta);
        };
        std::stable_sort(candidates.begin(), candidates.end(), [&](auto* a, auto* b)
        {
            const float left = distance(a), right = distance(b);
            return left == right ? a->EntityId < b->EntityId : left < right;
        });
        if (candidates.size() > MaximumSceneReflectionProbes && !mImpl->bProbeBudgetWarning)
        {
            HE_CORE_WARN("Reflection probe budget: selecting the nearest {} of {} volumes per view",
                MaximumSceneReflectionProbes, candidates.size());
            mImpl->bProbeBudgetWarning = true;
        }
        for (size_t i = 0; i < std::min(candidates.size(), size_t(MaximumSceneReflectionProbes)); ++i)
        {
            const auto& probe = *candidates[i];
            const auto preparation = mImpl->PrepareEnvironment(probe.HdrTexturePath);
            if (preparation.State != SceneEnvironmentState::Ready) continue;
            const auto found = mImpl->EnvironmentCache.find(preparation.Path);
            if (found == mImpl->EnvironmentCache.end()) continue;
            mImpl->PreparedReflectionProbes.push_back(SceneReflectionProbe{
                .BoundsMin = probe.BoundsMin, .BlendDistance = probe.BlendDistance,
                .BoundsMax = probe.BoundsMax, .Intensity = probe.Intensity,
                .CapturePosition = probe.CapturePosition,
                .PrefilteredEnvironment = found->second.Prefilter,
                .Irradiance = probe.bAffectDiffuse ? found->second.Irradiance : nullptr,
            });
        }
    }

    void SceneRenderer::SetCloudSettings(const CloudSettings& settings)
    {
        mImpl->CloudQualitySettings = ResolveCloudSettings(settings);
    }

    void SceneRenderer::SetAmbientOcclusionSettings(const SceneAmbientOcclusionSettings& settings)
    {
        if (mImpl->AmbientOcclusionSettings == settings) return;
        mImpl->AmbientOcclusionSettings = settings;
        mImpl->bGraphDirty = true;
        mImpl->SceneAntiAliasing.ResetHistory();
    }

    void SceneRenderer::SetTransientCacheBudget(uint64_t bytes) noexcept
    {
        mImpl->ResourcePool.SetTransientCacheBudget(bytes);
    }

    void SceneRenderer::SetTextureStreamingBudget(uint64_t bytes) noexcept
    {
        mImpl->TextureStreamingBudgetBytes = std::clamp<uint64_t>(bytes, 1ull * 1024 * 1024, 8192ull * 1024 * 1024);
    }

    void SceneRenderer::SetUpscalingSharpness(float sharpness)
    {
        mImpl->RequestedUpscaling.Sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    }

    void SceneRenderer::SetPostProcessSettings(const ScenePostProcessSettings& settings)
    {
        if (mImpl->PostProcessSettings.RenderHeight != settings.RenderHeight ||
            mImpl->PostProcessSettings.SkinSubsurfaceEnabled != settings.SkinSubsurfaceEnabled)
            mImpl->bGraphDirty = true;
        mImpl->PostProcessSettings = settings;
        // PrepareFrame detects actual processing topology changes. Constant-only
        // edits preserve the graph, exposure and temporal reconstruction history.
    }

    void SceneRenderer::CommitRenderingPath(RenderingPath path)
    {
        if (mImpl->Path == path)
            return;
        mImpl->Path = path;
        mImpl->PostProcessPass->ResetHistory();
        mImpl->SceneAntiAliasing.ResetHistory();
        mImpl->bHasPreviousRasterCamera = false;
        if (auto* technique = mImpl->GetActiveIlluminationTechnique())
            technique->ResetHistory(GlobalIlluminationHistoryResetReason::CameraCut);
        mImpl->RefreshAntiAliasingSelection();
        mImpl->bGraphDirty = true;
        HE_CORE_INFO("Rendering path committed: {}", ToString(path));
    }

    RenderingPath SceneRenderer::GetRenderingPath() const noexcept
    {
        return mImpl->Path;
    }

    void SceneRenderer::SetRenderSettings(
        const AntiAliasingSettings& antiAliasing,
        const GlobalIlluminationSettings& globalIllumination)
    {
        ValidateAntiAliasingSettings(antiAliasing);
        if (globalIllumination.Denoiser ==
            GlobalIlluminationDenoiserKind::Nrd)
        {
            throw std::invalid_argument(
                "NRD is not available; no approved dependency/adapter is present");
        }
        if (mImpl->RequestedAaSettings == antiAliasing)
        {
            SetGlobalIlluminationSettings(globalIllumination);
            return;
        }

        for (uint32_t frameSlot = 0; frameSlot < mImpl->FramesInFlight;
            ++frameSlot)
        {
            if (mImpl->ResourcePool.IsFrameActive(frameSlot))
            {
                throw std::logic_error(
                    "Cannot configure render settings with an active frame slot");
            }
        }
        // SetAntiAliasingSettings rebuilds the graph and reconfigures GI because
        // raster sample count changes its resources. Seed the new GI request so
        // a batch performs that work exactly once.
        mImpl->RequestedGiSettings = globalIllumination;
        SetAntiAliasingSettings(antiAliasing);
    }

    void SceneRenderer::ResetGlobalIlluminationHistory(
        GlobalIlluminationHistoryResetReason reason)
    {
        if (IGlobalIlluminationTechnique* technique =
                mImpl->GetActiveIlluminationTechnique())
        {
            technique->ResetHistory(reason);
        }
    }

    void SceneRenderer::ResetTemporalHistory()
    {
        mImpl->UpscalingPass->ResetHistory();
        mImpl->SceneAntiAliasing.ResetHistory();
        mImpl->PostProcessPass->ResetHistory();
        mImpl->bHasPreviousRasterCamera = false;
        mImpl->GlobalIlluminationRenderSequence = 0;
        ResetGlobalIlluminationHistory(GlobalIlluminationHistoryResetReason::CameraCut);
    }

    const GlobalIlluminationSettings&
        SceneRenderer::GetGlobalIlluminationSettings() const noexcept
    {
        return GetRequestedGlobalIlluminationSettings();
    }

    const GlobalIlluminationSettings&
        SceneRenderer::GetRequestedGlobalIlluminationSettings() const noexcept
    {
        return mImpl->RequestedGiSettings;
    }

    const GlobalIlluminationSettings&
        SceneRenderer::GetEffectiveGlobalIlluminationSettings() const noexcept
    {
        return mImpl->EffectiveGiSettings;
    }

    GlobalIlluminationSelection
        SceneRenderer::GetGlobalIlluminationSelection() const
    {
        return mImpl->GiSelection;
    }

    std::vector<GlobalIlluminationTechniqueDescriptor>
        SceneRenderer::EnumerateGlobalIlluminationTechniques() const
    {
        return mImpl->GiTechniqueRegistry.Enumerate();
    }

    bool SceneRenderer::RequiresContinuousUpdate() const noexcept
    {
        if (mImpl->IsFsrActive()) return true;
        if (!mImpl->RetiredIlluminationSets.empty()) return true;
        if (mImpl->Features->RequiresContinuousUpdate()) return true;
        if (mImpl->PostProcessPass->RequiresContinuousUpdate()) return true;
        if (mImpl->SceneAntiAliasing.RequiresContinuousUpdate())
            return true;
        const IGlobalIlluminationTechnique* technique =
            mImpl->GetActiveIlluminationTechnique();
        return technique && technique->RequiresContinuousUpdate();
    }

    nvrhi::TextureHandle SceneRenderer::GetOfflineViewportBeautyTexture() const
    {
        if (mImpl->ActiveOfflineViewportRenderer)
        {
            return mImpl->ActiveOfflineViewportRenderer->GetNativeAov(
                OfflineViewportAov::Beauty).Texture;
        }
        return {};
    }

    void SceneRenderer::ActivateOfflineViewportRenderer(std::string providerId)
    {
        for (uint32_t frameSlot = 0; frameSlot < mImpl->FramesInFlight; ++frameSlot)
        {
            if (mImpl->ResourcePool.IsFrameActive(frameSlot))
            {
                throw std::logic_error(
                    "Cannot activate an offline viewport renderer with an active frame slot");
            }
        }
        mImpl->Graph.Clear();
        mImpl->GraphGeometryResources.clear();
        mImpl->ConfigureOfflineViewportRenderer(std::move(providerId));
    }

    void SceneRenderer::DeactivateOfflineViewportRenderer()
    {
        if (!mImpl->IsOfflineViewportRendererActive())
            return;
        for (uint32_t frameSlot = 0; frameSlot < mImpl->FramesInFlight; ++frameSlot)
        {
            if (mImpl->ResourcePool.IsFrameActive(frameSlot))
            {
                throw std::logic_error(
                    "Cannot deactivate an offline viewport renderer with an active frame slot");
            }
        }
        mImpl->Graph.Clear();
        mImpl->GraphGeometryResources.clear();
        mImpl->ClearOfflineViewportRenderer();
    }

    std::string SceneRenderer::GetActiveOfflineViewportRendererId() const
    {
        return mImpl->ActiveOfflineViewportRendererId;
    }

    IOfflineViewportRendererSession*
        SceneRenderer::GetOfflineViewportRendererSession() const noexcept
    {
        return mImpl->ActiveOfflineViewportRenderer.Get();
    }

    std::vector<OfflineViewportRendererProviderDescriptor>
        SceneRenderer::EnumerateOfflineViewportRenderers() const
    {
        return OfflineViewportRendererProviderRegistry::Get().Enumerate();
    }

    SceneEnvironmentPreparation SceneRenderer::PrepareEnvironment(std::string_view path)
    {
        return mImpl->PrepareEnvironment(path);
    }

    SceneEnvironmentPreparation SceneRenderer::GetEnvironmentPreparation(
        std::string_view path) const
    {
        return mImpl->GetEnvironmentPreparation(path);
    }

    bool SceneRenderer::IsEnvironmentReady(std::string_view path) const
    {
        return mImpl->GetEnvironmentPreparation(path).State == SceneEnvironmentState::Ready;
    }

    ManagedTextureHandle SceneRenderer::LoadTexture(const TextureLoadDesc& desc)
    {
        return mImpl->TextureManagerInstance.LoadTexture(desc);
    }

    const ManagedTexture& SceneRenderer::GetTexture(ManagedTextureHandle handle) const
    {
        return mImpl->TextureManagerInstance.GetTexture(handle);
    }

    void SceneRenderer::ReleaseTexture(
        ManagedTextureHandle handle,
        uint64_t retireAfterSubmission)
    {
        mImpl->TextureManagerInstance.ReleaseTexture(handle, retireAfterSubmission);
    }

    size_t SceneRenderer::InvalidateFailedTextureLoads(std::string_view assetPath)
    {
        return mImpl->TextureManagerInstance.InvalidateFailedLoads(assetPath);
    }

    size_t SceneRenderer::RequestTextureReload(std::string_view assetPath)
    {
        const size_t environments = mImpl->InvalidateEnvironmentEntries(assetPath);
        const size_t textures = mImpl->TextureManagerInstance.RequestReload(assetPath);
        (void)mImpl->TextureManagerInstance.InvalidateFailedLoads(assetPath);
        return environments + textures;
    }

    void SceneRenderer::ConfigureOffscreenOutput(
        uint32_t width,
        uint32_t height,
        std::string debugName,
        nvrhi::Format colorFormat)
    {
        mImpl->ConfigureOffscreenOutput(width, height, std::move(debugName), colorFormat);
    }

    void SceneRenderer::DisableOffscreenOutput()
    {
        mImpl->DisableOffscreenOutput();
    }

    SceneRendererOffscreenOutput SceneRenderer::GetOffscreenOutput() const
    {
        return mImpl->OffscreenOutput;
    }

    void SceneRenderer::NotifyFrameSubmitted(uint32_t frameSlot, uint64_t submission)
    {
        mImpl->ResourcePool.EndFrame(frameSlot, submission);
        // Actual graph submission, independent of GPU frame marker submissions and delayed samples.
        if (mImpl->bCpuGraphTimingEnabled && mImpl->Stats.bCpuGraphTimingValid &&
            frameSlot == mImpl->CurrentFrameSlot)
            mImpl->Stats.CpuGraphSubmissionId = submission;
		mImpl->GpuTiming.NotifySubmitted(frameSlot, submission);
        mImpl->LastSubmittedSubmission = std::max(
            mImpl->LastSubmittedSubmission,
            submission);
        if (mImpl->HardwareRayTracingScene)
            mImpl->HardwareRayTracingScene->NotifyFrameSubmitted(submission);
        if (mImpl->IsGlobalIlluminationActive())
        {
            mImpl->GetActiveIlluminationTechnique()->NotifyFrameSubmitted(
                submission);
            if (mImpl->bGlobalIlluminationFrameRecorded)
            {
                ++mImpl->GlobalIlluminationRenderSequence;
                mImpl->bGlobalIlluminationFrameRecorded = false;
            }
            mImpl->RefreshGlobalIlluminationStats();
        }
        mImpl->SceneAntiAliasing.NotifyFrameSubmitted(submission);
        mImpl->UpscalingPass->NotifySubmitted(submission);
        mImpl->LightGrid->NotifyFrameSubmitted(submission);
        mImpl->LocalShadowPass->NotifyFrameSubmitted(submission);
        mImpl->ShadowGpuSubmission->NotifyFrameSubmitted(submission);
        mImpl->PostProcessPass->NotifyFrameSubmitted(submission);
        mImpl->Features->NotifyFrameSubmitted(submission);
        mImpl->AmbientOcclusionPass->NotifyFrameSubmitted(submission);
        mImpl->GpuOcclusionPass->NotifyFrameSubmitted(submission);
        mImpl->DepthIndirectCompaction->NotifyFrameSubmitted(submission);
        mImpl->MainIndirectCompaction->NotifyFrameSubmitted(submission);
        mImpl->GeometryArena->NotifyFrameSubmitted(submission);
        for (const auto* key : mImpl->PendingMeshUploads)
            mImpl->MeshCache.at(key).DeformationHistory.Commit();
        mImpl->PendingMeshUploads.clear();
        mImpl->ReflectionPass->NotifyFrameSubmitted(submission);
        mImpl->ReactiveBudget.Commit();
        if (mImpl->bRasterCameraFrameRecorded)
        {
            mImpl->MotionHistory.Commit();
            mImpl->SubmittedMaterialShaderPublicationRevision =
                mImpl->PendingMaterialShaderPublicationRevision;
            mImpl->PreviousRasterView = mImpl->PendingRasterView;
            mImpl->PreviousRasterJitterPixels = mImpl->PendingRasterJitterPixels;
            mImpl->PreviousRasterWorldFrame = mImpl->PendingRasterWorldFrame;
            mImpl->PreviousRasterSubmission = submission;
            mImpl->PreviousRasterViewProjection =
                mImpl->PendingRasterViewProjection;
            mImpl->PreviousRasterJitterGeneration =
                mImpl->PendingRasterJitterGeneration;
            mImpl->PreviousTaaNonJitteredViewProjection =
                mImpl->PendingTaaNonJitteredViewProjection;
            mImpl->bHasPreviousRasterCamera = true;
            mImpl->bRasterCameraFrameRecorded = false;
        }
		if (!mImpl->RecordedGpuReadbacks.at(frameSlot))
			return;
        SceneRenderer::Impl::GpuReadbackSlot& readback = mImpl->GpuReadbackSlots.at(frameSlot);
        readback.Submission = submission;
        readback.ObjectCount = mImpl->RecordedGpuObjectCounts.at(frameSlot);
        readback.BatchCount = mImpl->RecordedGpuBatchCounts.at(frameSlot);
        readback.bPending = true;
    }

    void SceneRenderer::AbortFrame(uint32_t frameSlot) noexcept
    {
        mImpl->MotionHistory.Abort();
        mImpl->ReactiveBudget.Abort();
        mImpl->UploadedSceneTables.clear();
        mImpl->PostProcessPass->AbortFrame();
        mImpl->Features->AbortFrame();
        mImpl->AmbientOcclusionPass->AbortFrame();
        mImpl->GpuOcclusionPass->AbortFrame();
        mImpl->DepthIndirectCompaction->AbortFrame();
        mImpl->MainIndirectCompaction->AbortFrame();
        mImpl->AbortGeometryUploads();
        mImpl->LocalShadowPass->AbortFrame();
        mImpl->ShadowGpuSubmission->AbortFrame();
        mImpl->ReflectionPass->AbortFrame();
        if (mImpl->HardwareRayTracingScene)
            mImpl->HardwareRayTracingScene->AbortFrame();
        if (mImpl->IsGlobalIlluminationActive())
        {
            mImpl->GetActiveIlluminationTechnique()->AbortFrame();
            mImpl->bGlobalIlluminationFrameRecorded = false;
        }
        mImpl->SceneAntiAliasing.AbortFrame();
        mImpl->UpscalingPass->AbortFrame();
        mImpl->bRasterCameraFrameRecorded = false;
        mImpl->GpuTiming.AbortFrame(frameSlot);
        mImpl->ResourcePool.AbortFrame(frameSlot);
    }

    void SceneRenderer::Resize(uint32_t width, uint32_t height)
    {
        mImpl->Resize(width, height);
    }

    void SceneRenderer::ClearCaches()
    {
        mImpl->ClearCaches();
    }

    void SceneRenderer::CollectGpuCullingStats()
    {
        mImpl->CollectGpuCullingStats();
    }

    void SceneRenderer::CollectGpuTimingStats()
    {
        mImpl->CollectGpuTimingStats();
    }

    const SceneRendererStats& SceneRenderer::GetStats() const noexcept
    {
        mImpl->Stats.CloudCpuRecordMilliseconds = mImpl->Features->GetCpuRecordMilliseconds("builtin.cloud");
        return mImpl->Stats;
    }

    RenderGraphDiagnosticSnapshot
        SceneRenderer::GetRenderGraphDiagnosticSnapshot() const
    {
        return mImpl->Graph.BuildDiagnosticSnapshot();
    }

    std::string SceneRenderer::BuildRenderGraphDebugDump() const
    {
        std::string exclusionReasons;
        if (mImpl->HardwareRayTracingScene)
        {
            const RayTracingSceneStats& rtStats =
                mImpl->HardwareRayTracingScene->GetStats();
            for (uint32_t supportIndex = 0;
                supportIndex < c_RayTracingGeometrySupportCount;
                ++supportIndex)
            {
                const uint32_t count = rtStats.ExcludedBySupport[supportIndex];
                if (count == 0u)
                    continue;
                if (!exclusionReasons.empty())
                    exclusionReasons += ", ";
                exclusionReasons += fmt::format(
                    "{}={}",
                    ToString(static_cast<RayTracingGeometrySupport>(supportIndex)),
                    count);
            }
            if (rtStats.InvalidTransformExcludedCount > 0u)
            {
                if (!exclusionReasons.empty())
                    exclusionReasons += ", ";
                exclusionReasons += fmt::format(
                    "invalid transform={}",
                    rtStats.InvalidTransformExcludedCount);
            }
        }
        if (exclusionReasons.empty())
            exclusionReasons = "none";
        return mImpl->Graph.BuildDebugDump() + "\n" +
            mImpl->GpuTiming.BuildDebugDump() + "\n" +
            fmt::format(
                "GI: requested={} selected={} hardware={} fallback={} "
                "resolution={}x{} scale={:.3f} raysPerPixel={} bounces={} "
                "samples={} historyResets={} lastResetMask=0x{:x}\n"
                "GI timing: valid={} traceMs={:.3f} svgfMs={:.3f} compositeMs={:.3f}\n"
                "GI resources: BLAS(build/reuse)={}/{} TLAS(build/update)={}/{} "
                "instances={} excluded={} excludedReasons=[{}] emissiveTriangles={} rtLogicalBytes={} "
                "rtBufferAllocationRequirementBytes={} rtAsAllocationRequirementBytes={} "
                "rgPersistentBytes={} rgTransientReservedBytes={} "
                "rgPeakFrameTransientBytes={} nvrhiValidation={} reason={} ",
                static_cast<uint32_t>(mImpl->GiSelection.Requested),
                static_cast<uint32_t>(mImpl->GiSelection.Selected),
                mImpl->GiSelection.bUsesHardwareRayTracing ? 1 : 0,
                mImpl->GiSelection.bFallback ? 1 : 0,
                mImpl->LastWidth,
                mImpl->LastHeight,
                mImpl->EffectiveGiSettings.RenderScale,
                mImpl->Stats.GiStats.RaysPerPixel,
                mImpl->Stats.GiStats.BounceCount,
                mImpl->Stats.GiStats.AccumulatedSamples,
                mImpl->Stats.GiStats.HistoryResetCount,
                static_cast<uint32_t>(mImpl->Stats.GiStats.LastResetReasons),
                mImpl->Stats.bGpuGiTimingValid ? 1 : 0,
                mImpl->Stats.GpuGiTraceMilliseconds,
                mImpl->Stats.GpuSvgfMilliseconds,
                mImpl->Stats.GpuGiCompositeMilliseconds,
                mImpl->Stats.RayTracingBlasBuildCount,
                mImpl->Stats.RayTracingBlasReuseCount,
                mImpl->Stats.RayTracingTlasBuildCount,
                mImpl->Stats.RayTracingTlasUpdateCount,
                mImpl->Stats.RayTracingInstanceCount,
                mImpl->Stats.RayTracingExcludedInstanceCount,
                exclusionReasons,
                mImpl->Stats.RayTracingEmissiveTriangleCount,
                mImpl->Stats.RayTracingGeometryBytes +
                    mImpl->Stats.RayTracingSceneTableBytes,
                mImpl->Stats.RayTracingActiveBufferAllocationRequirementBytes,
                mImpl->Stats.RayTracingActiveAsAllocationRequirementBytes,
                mImpl->Stats.RenderGraphPersistentBytes,
                mImpl->Stats.RenderGraphTransientReservedBytes,
                mImpl->Stats.RenderGraphPeakFrameTransientBytes,
                mImpl->Capabilities.bValidationLayer ? "enabled" : "disabled",
                mImpl->GiSelection.Reason);
    }

    std::string SceneRenderer::BuildGpuTimingDebugDump() const
    {
        return mImpl->GpuTiming.BuildDebugDump();
    }
}
