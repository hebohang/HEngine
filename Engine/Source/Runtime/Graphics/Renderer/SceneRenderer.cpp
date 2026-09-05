#include "Hepch.h"

#include "SceneRenderer.h"

#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialPipelineKey.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationCompositePass.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationTechniqueRegistry.h"
#include "Runtime/Graphics/GlobalIllumination/DepthPyramidPass.h"
#include "Runtime/Graphics/GlobalIllumination/RealtimeRayTracedGlobalIllumination.h"
#include "Runtime/Graphics/GlobalIllumination/SceneSurfaceDataPass.h"
#include "Runtime/Graphics/GlobalIllumination/ScreenSpaceGlobalIllumination.h"
#include "Runtime/Graphics/GlobalIllumination/SvgfDenoiser.h"
#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/RayTracing/RayTracingScene.h"
#include "Runtime/Graphics/Renderer/EnvironmentMapSizes.h"
#include "Runtime/Graphics/Renderer/SceneAntiAliasingPass.h"
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
#include <cmath>
#include <cstddef>
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
        constexpr float c_DirectionalShadowCascadeBlendFraction = 0.10f;
        constexpr float c_DirectionalShadowFarFadeFraction = 0.10f;
        constexpr float c_DirectionalShadowCasterDepthPadding = 20.0f;
        constexpr uint32_t c_MaxGpuDrivenObjects = 16384;
        constexpr uint32_t c_MaxGpuDrivenBatches = 4096;
        constexpr uint32_t c_MaxGpuMaterials = 4096;
		// Vulkan implements volatile constant buffers as an aligned version ring.
		// CPU fallback objects can be written by depth, shadow cascades, forward,
		// transparent and outline passes in the same command list.
		constexpr uint32_t c_MaxObjectConstantWritesPerFrame = 32768;
        constexpr uint32_t c_MaxMaterialParameterRows =
            c_MaxGpuMaterials * static_cast<uint32_t>(MaterialParameterMaximumPropertyCount);
        constexpr uint32_t c_MaxPrimitive2DVertices = 65536;
        constexpr uint32_t c_MaxSkinnedDraws = 256;
        constexpr uint32_t c_MaxBonesPerPalette = 100;
        constexpr uint32_t c_EnvironmentSampleCount = 256;
        constexpr uint32_t c_MaterialFlagAlphaMask = 1u << 0u;
        constexpr uint32_t c_MaterialFlagAlphaBlend = 1u << 1u;
        constexpr uint32_t c_MaterialFlagDoubleSided = 1u << 2u;

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
            // x: cascade debug, y: PBR debug mode, z: first point falloff exponent,
            // w: sanitized camera near plane used by the first cascade blend span.
            glm::vec4 DirectionalShadowDebugSettings{ 0.0f };
            // x: monotonic presentation time in seconds, yzw reserved.
            glm::vec4 MaterialTimeAndPadding{ 0.0f };
        };

        struct alignas(16) ObjectConstants
        {
            glm::mat4 World{ 1.0f };
            glm::mat4 WorldInverseTranspose{ 1.0f };
            // x: GPU material, y: object, z: mesh, w: reserved.
            glm::uvec4 MaterialObjectMeshIndices{ 0u };
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
        };

        struct alignas(16) ToneMappingConstants
        {
            float Exposure = 1.0f;
            uint32_t PbrDebugMode = 0;
            float Padding[2]{};
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

        struct alignas(16) GpuDrawConstants
        {
            uint32_t VisibleBase = 0;
            uint32_t Padding[3]{};
        };

        struct alignas(16) ShadowPassConstants
        {
            uint32_t CascadeIndex = 0;
            uint32_t Padding[3]{};
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

        static_assert(sizeof(FrameConstants) == 624);
        static_assert(offsetof(FrameConstants, ShadowCameraView) == 304);
        static_assert(offsetof(FrameConstants, AdditionalLightViewProjections) == 368);
        static_assert(offsetof(FrameConstants, DirectionalShadowCascadeSplits) == 560);
        static_assert(offsetof(FrameConstants, DirectionalShadowCascadeSettings) == 576);
        static_assert(offsetof(FrameConstants, DirectionalShadowDebugSettings) == 592);
        static_assert(sizeof(ObjectConstants) == 144);
        static_assert(sizeof(GpuMaterialData) == 96);
        static_assert(sizeof(ToneMappingConstants) == 16);
        static_assert(sizeof(GpuBoundsData) == 16);
        static_assert(sizeof(GpuDrawData) == 16);
        static_assert(sizeof(CullConstants) == 80);
        static_assert(sizeof(GpuDrawConstants) == 16);
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
                    HashValue(hash, section.MaterialInstance.Revision);
                    HashValue(hash, section.MaterialInstance.BaseColor);
                    HashValue(hash, section.MaterialInstance.Emissive);
                    HashValue(hash, section.MaterialInstance.Metallic);
                    HashValue(hash, section.MaterialInstance.Roughness);
                }
            }
            return hash == 0 ? 1 : hash;
        }

        [[nodiscard]] RayTracingSceneRevisions BuildRayTracingSceneRevisions(
            const RenderWorld& world)
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
                        ? ClassifyRayTracingGeometry(object, section)
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
                    const auto& material = section.MaterialInstance;
                    HashString(materials, material.AssetPath);
                    HashString(materials, material.ShaderPath);
                    HashValue(materials, material.Revision);
                    HashValue(materials, material.BaseColor);
                    HashValue(materials, material.Emissive);
                    HashValue(materials, material.Metallic);
                    HashValue(materials, material.Roughness);
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
                HashValue(lights, light.FalloffExponent);
            }
            for (const RenderDirectionalLight& light : world.DirectionalLights)
            {
                HashValue(lights, light.Direction);
                HashValue(lights, light.Intensity);
                HashValue(lights, light.Color);
            }
            HashString(lights, world.Environment.HdrTexturePath);
            HashValue(lights, world.Environment.IblIntensity);
            revisions.Scene = std::max(world.SceneGeneration, 1ull);
            revisions.GeometryTopology = topology == 0 ? 1 : topology;
            revisions.Transform = transforms == 0 ? 1 : transforms;
            revisions.Material = materials == 0 ? 1 : materials;
            revisions.Light = lights == 0 ? 1 : lights;
            return revisions;
        }

        [[nodiscard]] float SanitizeDirectionalShadowNearPlane(float nearPlane) noexcept
        {
            return std::isfinite(nearPlane) ? std::max(nearPlane, 0.001f) : 0.001f;
        }

        [[nodiscard]] float ComputeDirectionalShadowCascadeSplit(
            float nearPlane,
            float farPlane,
            float ratio,
            float lambda)
        {
            const float logarithmicSplit = nearPlane * std::pow(farPlane / nearPlane, ratio);
            const float uniformSplit = nearPlane + (farPlane - nearPlane) * ratio;
            return glm::mix(uniformSplit, logarithmicSplit, lambda);
        }

        [[nodiscard]] std::array<glm::vec3, 8> ComputeDirectionalShadowFrustumCorners(
            const glm::mat4& view,
            const glm::mat4& projection,
            float nearSplit,
            float farSplit)
        {
            const glm::mat4 inverseViewProjection = glm::inverse(projection * view);
            const glm::vec4 projectedNear =
                projection * glm::vec4(0.0f, 0.0f, -nearSplit, 1.0f);
            const glm::vec4 projectedFar =
                projection * glm::vec4(0.0f, 0.0f, -farSplit, 1.0f);
            const float ndcNear = projectedNear.z / projectedNear.w;
            const float ndcFar = projectedFar.z / projectedFar.w;

            std::array<glm::vec3, 8> corners{};
            uint32_t cornerIndex = 0;
            for (float x : { -1.0f, 1.0f })
            {
                for (float y : { -1.0f, 1.0f })
                {
                    for (float z : { ndcNear, ndcFar })
                    {
                        const glm::vec4 corner =
                            inverseViewProjection * glm::vec4(x, y, z, 1.0f);
                        corners[cornerIndex++] = glm::vec3(corner) / corner.w;
                    }
                }
            }
            return corners;
        }

        [[nodiscard]] glm::mat4 ComputeDirectionalShadowCascadeMatrix(
            const std::array<glm::vec3, 8>& frustumCorners,
            const glm::vec3& lightDirection,
            uint32_t shadowMapSize)
        {
            glm::vec3 center(0.0f);
            for (const glm::vec3& corner : frustumCorners)
                center += corner;
            center /= static_cast<float>(frustumCorners.size());

            float radius = 0.0f;
            for (const glm::vec3& corner : frustumCorners)
                radius = std::max(radius, glm::length(corner - center));
            // Quantizing the square extent avoids tiny floating-point changes
            // in the fitted frustum from continuously rescaling the projection.
            radius = std::max(std::ceil(radius * 16.0f) / 16.0f, 1.0f / 16.0f);

            const glm::vec3 up =
                std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f
                ? glm::vec3(0.0f, 0.0f, 1.0f)
                : glm::vec3(0.0f, 1.0f, 0.0f);
            const float eyeDistance =
                radius * 2.0f + c_DirectionalShadowCasterDepthPadding;
            const glm::mat4 lightView = glm::lookAt(
                center - lightDirection * eyeDistance,
                center,
                up);
            glm::mat4 lightProjection = glm::orthoZO(
                -radius,
                radius,
                -radius,
                radius,
                0.0f,
                radius * 4.0f + c_DirectionalShadowCasterDepthPadding * 2.0f);

            // Align the projection origin to an integer shadow texel. The
            // square extent above keeps the world-units-per-texel stable.
            const glm::mat4 lightViewProjection = lightProjection * lightView;
            glm::vec4 shadowOrigin =
                lightViewProjection * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            const float halfResolution =
                std::max(static_cast<float>(shadowMapSize) * 0.5f, 1.0f);
            shadowOrigin *= halfResolution;
            const glm::vec2 roundedOrigin = glm::round(glm::vec2(shadowOrigin));
            const glm::vec2 roundingOffset =
                (roundedOrigin - glm::vec2(shadowOrigin)) / halfResolution;
            lightProjection[3][0] += roundingOffset.x;
            lightProjection[3][1] += roundingOffset.y;
            return lightProjection * lightView;
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

            const float nearPlane = SanitizeDirectionalShadowNearPlane(world.Camera.NearPlane);
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
                splits[cascade] = ComputeDirectionalShadowCascadeSplit(
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
                float cascadeNear = cascade == 0 ? nearPlane : splits[cascade - 1u];
                if (cascade > 0)
                {
                    const float previousNear = cascade == 1
                        ? nearPlane
                        : splits[cascade - 2u];
                    cascadeNear -= (cascadeNear - previousNear) *
                        c_DirectionalShadowCascadeBlendFraction;
                }
                const auto corners = ComputeDirectionalShadowFrustumCorners(
                    world.Camera.View,
                    world.Camera.Projection,
                    cascadeNear,
                    splits[cascade]);
                data.LightViewProjections[cascade] =
                    ComputeDirectionalShadowCascadeMatrix(
                        corners,
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
            BufferHandle ObjectConstantsBuffer;
            BufferHandle BoneMatrices;
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
            nvrhi::TextureHandle Environment;
            nvrhi::TextureHandle Irradiance;
            nvrhi::TextureHandle Prefilter;
        };

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
            Ref<SubMesh> Resource;
            nvrhi::BufferHandle VertexBuffer;
            nvrhi::BufferHandle IndexBuffer;
            uint32_t IndexCount = 0;
            uint32_t GraphIndex = InvalidRenderGraphIndex;
            bool bUploaded = false;
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
            ObjectConstants Constants;
            std::shared_ptr<const CompiledMaterialShader> MaterialProgram;
            TransparentDrawSortKey SortKey;
            bool bDoubleSided = false;
            bool bSkinned = false;
        };

        struct GpuBatchKey
        {
            const SubMesh* Mesh = nullptr;
            const Material* MaterialResource = nullptr;
            bool bDoubleSided = false;

            [[nodiscard]] bool operator==(const GpuBatchKey&) const = default;
        };

        struct GpuBatchKeyHash
        {
            [[nodiscard]] size_t operator()(const GpuBatchKey& key) const noexcept
            {
                uint64_t value = reinterpret_cast<uintptr_t>(key.Mesh);
                value = MixKey(value, reinterpret_cast<uintptr_t>(key.MaterialResource));
                value = MixKey(value, key.bDoubleSided ? 1u : 0u);
                return static_cast<size_t>(value);
            }
        };

        struct GpuBatch
        {
            GpuBatchKey Key;
            uint32_t VisibleBase = 0;
            uint32_t ObjectCount = 0;
			uint32_t MaterialIndex = 0;
        };

        struct MaterialTextureSet
        {
            std::string Signature;
            std::array<ManagedTextureHandle, 5> Handles{};
            std::array<uint32_t, 5> ShaderIndices{};
        };

        struct MaterialParameterTextureSet
        {
            std::string Signature;
            std::vector<ManagedTextureHandle> Handles;
            std::vector<uint32_t> ShaderIndices;
			std::vector<DefaultBindlessTexture> Fallbacks;
			std::vector<uint32_t> RowOffsets;
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

            void BeginFrame(uint32_t frameSlot, uint64_t completedSubmission)
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
            }

            void EndFrame(uint32_t frameSlot)
            {
                if (!bEnabled)
                    return;
                Slot& slot = mSlots.at(frameSlot);
                if (!slot.bRecording || mCurrentFrameSlot != frameSlot)
                    throw std::logic_error("SceneRenderer GPU timing frame is not recording");
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
                for (uint32_t index = 0; index < slot.UsedQueryCount; ++index)
                {
                    TimerRecord& query = slot.Queries[index];
                    if (!query.Handle)
                        continue;
                    const double milliseconds = static_cast<double>(
                        mDevice->getTimerQueryTime(query.Handle)) * 1000.0;
                    frameMilliseconds += milliseconds;
                    if (bCompleteSample)
                        RecordPassTiming(query.PassName, milliseconds);
                    mDevice->resetTimerQuery(query.Handle);
                }

                if (bCompleteSample)
                {
                    mLastFrameMilliseconds = frameMilliseconds;
                    mTotalFrameMilliseconds += frameMilliseconds;
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
            std::unordered_map<std::string, size_t> mPassTimingIndices;
            uint32_t mCurrentFrameSlot = InvalidRenderGraphIndex;
            double mTotalFrameMilliseconds = 0.0;
            double mLastFrameMilliseconds = 0.0;
            uint64_t mFrameSampleCount = 0;
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
            , ResourcePool(desc.Device, desc.FramesInFlight)
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
            , Capabilities(desc.Capabilities)
            , AaCapabilities(BuildAntiAliasingCapabilities(
                desc.Backend,
                desc.Capabilities))
            , RequestedAaSettings(desc.AntiAliasing)
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
            CreateImmutableState();
            CreateDefaultResources();
            ActiveIrradiance = DefaultEnvironment;
            ActivePrefilter = DefaultEnvironment;
            ActiveEnvironment = DefaultEnvironment;
            ActiveBrdfLut = DefaultBrdfLut;
            Stats.SceneColorFormat = c_SceneColorFormat;
            Stats.SceneDepthFormat = c_SceneDepthFormat;
            Stats.bGpuTimingEnabled = desc.bEnableGpuTiming;
            RegisterGlobalIlluminationTechniques();
            ConfigureGlobalIllumination(desc.GlobalIllumination);
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
                    GlobalIlluminationTechniqueRequiredInput::FrameBuffer |
                    GlobalIlluminationTechniqueRequiredInput::LightBuffer |
                    GlobalIlluminationTechniqueRequiredInput::GeometryBuffer,
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

        [[nodiscard]] bool RequiresSceneSurfaceData() const noexcept
        {
            return IsTaaActive() || RequiresGiInput(
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
            return RequiresSceneSurfaceData() ||
                RequiresGiInput(GlobalIlluminationTechniqueRequiredInput::SceneTlas) ||
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
            if (!AaSelection.bBypassed &&
                AaSelection.Effective.Mode == AntiAliasingMode::Msaa &&
                EffectiveGiSettings.DebugView !=
                    GlobalIlluminationDebugView::Composite)
            {
                AaSelection.Effective.Mode = AntiAliasingMode::None;
                AaSelection.bFallback = true;
                AaSelection.Reason =
                    "MSAA is disabled for non-composite GI debug views.";
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
                !bHasPreviousRasterCamera;
            if (bViewChanged && !bReset)
                SceneAntiAliasing.RestartConvergence();
            if (IsTaaActive())
            {
                const glm::vec2 jitterPixels = GetTaaJitterPixels(
                    SceneAntiAliasing.GetTemporalFrameIndex(),
                    world.Camera.JitterSequenceGeneration);
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
                PendingTaaNonJitteredViewProjection =
                    world.Camera.NonJitteredViewProjection;
                bRasterCameraFrameRecorded = true;
            }
            else
            {
                PendingRasterViewProjection = world.Camera.ViewProjection;
                PendingRasterJitterGeneration =
                    world.Camera.JitterSequenceGeneration;
                bRasterCameraFrameRecorded = false;
            }
            SceneAntiAliasing.PrepareFrame(
                AaSelection.Effective,
                frame.Width,
                frame.Height,
                frame.CompletedSubmission,
                bReset);
            if (SceneAntiAliasing.ConsumeGraphRebuildRequest())
                bGraphDirty = true;
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

            if (ActiveOfflineViewportRenderer)
            {
                ActiveOfflineViewportRenderer->Clear(
                    std::numeric_limits<uint64_t>::max());
                ActiveOfflineViewportRenderer.Release();
            }
            if (ActiveGiTechnique)
            {
                ActiveGiTechnique->Clear(
                    std::numeric_limits<uint64_t>::max());
                ActiveGiTechnique.Release();
            }
            SvgfDenoiserInstance.reset();
            SurfaceDataPassInstance.reset();
            DepthPyramidPassInstance.reset();
            HardwareRayTracingScene.reset();
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
            ActiveOfflineViewportRenderer->Clear(
                std::numeric_limits<uint64_t>::max());
            ActiveOfflineViewportRenderer.Release();
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
            if (ActiveGiTechnique)
            {
                ActiveGiTechnique->Clear(
                    std::numeric_limits<uint64_t>::max());
                ActiveGiTechnique.Release();
            }
            SvgfDenoiserInstance.reset();
            SurfaceDataPassInstance.reset();
            DepthPyramidPassInstance.reset();
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
            if (RequiresSceneSurfaceData())
            {
                SurfaceDataPassInstance = CreateScope<SceneSurfaceDataPass>(
                    SceneSurfaceDataPassDesc{
                        .Device = Device,
                        .SharedShaderManager = &MaterialShaderHandleManagerInstance,
                        .Scene = HardwareRayTracingScene.get(),
                        .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(),
                        .Backend = Backend,
                    });
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
            if (!HardwareRayTracingScene)
                return;
            CurrentRtRevisions = BuildRayTracingSceneRevisions(world);
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
                FrameData.IblSettings.x = 0.0f;
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
                .bCameraCut = world.Camera.bCameraCut,
            };
            GetActiveIlluminationTechnique()->Collect(frame.CompletedSubmission);
            const GlobalIlluminationPrepareResult prepareResult =
                GetActiveIlluminationTechnique()->PrepareFrame(
                    GlobalIlluminationFrameContext{
                        .Camera = techniqueCamera,
                        .HistorySignature = historySignature,
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
                            ? FingerprintOfflineViewportSceneState(world)
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
                .Environment = EnvironmentHandle,
                .OpaqueDirectHdr = OpaqueDirectHdrHandle,
                .DepthPyramid = CurrentDepthPyramid.Depth,
                .Width = LastWidth,
                .Height = LastHeight,
            };
        }

        void AddGlobalIlluminationPasses()
        {
            if (!IsGlobalIlluminationActive())
                return;
            const GlobalIlluminationInputs inputs =
                BuildGlobalIlluminationInputs();
            TextureHandle compositeInput;
            GlobalIlluminationCompositeMode compositeMode =
                GlobalIlluminationCompositeMode::Additive;
            bool bScalarCompositeInput = false;
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
                        bScalarCompositeInput = true;
                        break;
                    case GlobalIlluminationDebugView::HistoryLength:
                        compositeInput = CurrentGiOutputs.HistoryLength;
                        bScalarCompositeInput = true;
                        break;
                    case GlobalIlluminationDebugView::Confidence:
                        compositeInput = CurrentGiOutputs.Confidence;
                        bScalarCompositeInput = true;
                        break;
                    case GlobalIlluminationDebugView::Coverage:
                        compositeInput = CurrentGiOutputs.CoverageMask;
                        bScalarCompositeInput = true;
                        break;
                    case GlobalIlluminationDebugView::Variance:
                        compositeInput = CurrentGiOutputs.Variance;
                        bScalarCompositeInput = true;
                        break;
                    default:
                        compositeInput = CurrentGiOutputs.CompositeRadiance;
                        break;
                    }
                }
            }
            if (!compositeInput)
                throw std::logic_error("Global illumination produced no composite input");
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
                    bScalarCompositeInput);
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
                    bScalarCompositeInput);
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
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
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
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
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
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::PushConstants(1, sizeof(GpuDrawConstants)));
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
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
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
            EnvironmentSampler = Device->createSampler(samplerDesc);
            samplerDesc.setAddressU(nvrhi::SamplerAddressMode::Wrap)
                .setAddressV(nvrhi::SamplerAddressMode::Clamp)
                .setAddressW(nvrhi::SamplerAddressMode::Clamp);
            EquirectangularSampler = Device->createSampler(samplerDesc);

            const std::array<nvrhi::VertexAttributeDesc, 5> attributes = {
                nvrhi::VertexAttributeDesc()
                    .setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(StaticVertex, Pos)))
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
            };
            InputLayout = Device->createInputLayout(
                attributes.data(),
                static_cast<uint32_t>(attributes.size()),
                OpaqueVertexShader);
            const std::array<nvrhi::VertexAttributeDesc, 7> skinnedAttributes = {
                nvrhi::VertexAttributeDesc().setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Pos)))
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
                nvrhi::VertexAttributeDesc().setName("BONEIDS")
                    .setFormat(nvrhi::Format::RGBA32_SINT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mBoneIDs)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BONEWEIGHTS")
                    .setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mWeights)))
                    .setElementStride(sizeof(SkinnedVertex)),
            };
            SkinnedInputLayout = Device->createInputLayout(
                skinnedAttributes.data(),
                static_cast<uint32_t>(skinnedAttributes.size()),
                SkinnedVertexShader);
            GpuInputLayout = Device->createInputLayout(
                attributes.data(),
                static_cast<uint32_t>(attributes.size()),
                GpuOpaqueVertexShader);
            const std::array<nvrhi::VertexAttributeDesc, 2> shadowAttributes = {
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
            };
            ShadowInputLayout = Device->createInputLayout(
                shadowAttributes.data(),
                static_cast<uint32_t>(shadowAttributes.size()),
                ShadowVertexShader);
            const std::array<nvrhi::VertexAttributeDesc, 4> skinnedShadowAttributes = {
                nvrhi::VertexAttributeDesc().setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, Pos)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("TEXCOORD")
                    .setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, TexCoord)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BONEIDS")
                    .setFormat(nvrhi::Format::RGBA32_SINT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mBoneIDs)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BONEWEIGHTS")
                    .setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mWeights)))
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
                nvrhi::VertexAttributeDesc().setName("BONEIDS")
                    .setFormat(nvrhi::Format::RGBA32_SINT).setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(SkinnedVertex, mBoneIDs)))
                    .setElementStride(sizeof(SkinnedVertex)),
                nvrhi::VertexAttributeDesc().setName("BONEWEIGHTS")
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
                !ToneSampler || !ShadowSampler || !EnvironmentSampler ||
                !EquirectangularSampler ||
                !InputLayout || !GpuInputLayout || !ShadowInputLayout ||
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
                .setMaxVersions(std::max(FramesInFlight, 2u))
                .setInitialState(nvrhi::ResourceStates::Common)
                .setKeepInitialState(false)
                .setDebugName("SceneRenderer Volatile Frame Constants");
            FrameConstantBuffer = Device->createBuffer(frameBufferDesc);
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
                .setByteSize(static_cast<uint64_t>(c_MaxSkinnedDraws) *
                    c_MaxBonesPerPalette * sizeof(glm::mat4))
                .setStructStride(sizeof(glm::mat4))
                .setCanHaveUAVs(false)
                .setIsDrawIndirectArgs(false)
                .setDebugName("SceneRenderer Skinned Bone Matrices");
            BoneMatrixBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuDrivenObjects) * sizeof(GpuBoundsData))
                .setStructStride(sizeof(GpuBoundsData))
                .setDebugName("SceneRenderer GPU Scene Bounds");
            GpuBoundsBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuDrivenObjects) * sizeof(GpuDrawData))
                .setStructStride(sizeof(GpuDrawData))
                .setDebugName("SceneRenderer GPU Scene Draw Data");
            GpuDrawDataBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuDrivenObjects) * sizeof(uint32_t))
                .setStructStride(sizeof(uint32_t))
                .setCanHaveUAVs(true)
                .setDebugName("SceneRenderer GPU Visible Object Indices");
            VisibleObjectIndicesBuffer = Device->createBuffer(gpuBufferDesc);
            gpuBufferDesc
                .setByteSize(static_cast<uint64_t>(c_MaxGpuDrivenBatches) *
                    sizeof(nvrhi::DrawIndexedIndirectArguments))
                .setStructStride(sizeof(nvrhi::DrawIndexedIndirectArguments))
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
                !BoneMatrixBuffer ||
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
            frameBindingDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, FrameConstantBuffer));
            FrameBindingSet = PipelineCacheInstance.GetOrCreateStaticBindingSet(
                StaticBindingSetKey{ 0x53434e4652414d53ull },
                frameBindingDesc,
                FrameBindingLayout,
                "SceneRenderer frame binding set");

            nvrhi::BindingSetDesc objectBindingDesc;
            objectBindingDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                    0, ObjectConstantBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, GpuMaterialBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, BoneMatrixBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, MaterialParameterBuffer));
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
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, VisibleObjectIndicesBuffer))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, GpuMaterialBuffer))
                .addItem(nvrhi::BindingSetItem::PushConstants(1, sizeof(GpuDrawConstants)));
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

        MeshEntry& GetOrCreateMesh(const Ref<SubMesh>& subMesh)
        {
            const SubMesh* key = subMesh.get();
            const auto existing = MeshCache.find(key);
            if (existing != MeshCache.end())
                return existing->second;

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
            nvrhi::BufferDesc indexDesc;
            indexDesc.setByteSize(
                    static_cast<uint64_t>(subMesh->GetIndices().size()) * sizeof(uint32_t))
                .setIsIndexBuffer(true)
                .setCanHaveRawViews(true)
                .setIsAccelStructBuildInput(true)
                .setInitialState(nvrhi::ResourceStates::Common)
                .setKeepInitialState(false)
                .setDebugName("SceneRenderer Static Mesh Indices");

            MeshEntry entry;
            entry.Resource = subMesh;
            entry.VertexBuffer = Device->createBuffer(vertexDesc);
            entry.IndexBuffer = Device->createBuffer(indexDesc);
            entry.IndexCount = static_cast<uint32_t>(subMesh->GetIndices().size());
            entry.bSkinned = bSkinned;
            if (!entry.VertexBuffer || !entry.IndexBuffer)
                throw std::runtime_error("NVRHI failed to create SceneRenderer mesh buffers");

            MeshOrder.push_back(key);
            const auto [iterator, bInserted] = MeshCache.emplace(key, std::move(entry));
            (void)bInserted;
            bGraphDirty = true;
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

            auto [iterator, bInserted] = EnvironmentCache.try_emplace(key);
            EnvironmentEntry& entry = iterator->second;
            if (!bInserted && entry.State != SceneEnvironmentState::Unrequested)
                return GetEnvironmentPreparation(key);

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
					.bGenerateMipmaps = false,
                    .DebugName = "SceneRenderer HDR Equirectangular: " + key,
                });
                entry.SourceEquirectangular =
                    TextureManagerInstance.GetTexture(entry.SourceAsset).Texture;

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

                const bool bCreateBrdfLut = !BrdfLut;
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
                Device->executeCommandList(commandList, nvrhi::CommandQueue::Graphics);
                if (!Device->waitForIdle())
                    throw std::runtime_error("GPU wait failed during environment IBL preparation");

                entry.State = SceneEnvironmentState::Ready;
                bGraphDirty = true;
            }
            catch (const std::exception& exception)
            {
                entry.SourceAsset = {};
                entry.SourceEquirectangular = nullptr;
                entry.Environment = nullptr;
                entry.Irradiance = nullptr;
                entry.Prefilter = nullptr;
                entry.Error = exception.what();
                entry.State = SceneEnvironmentState::Failed;
            }
            catch (...)
            {
                entry.SourceAsset = {};
                entry.SourceEquirectangular = nullptr;
                entry.Environment = nullptr;
                entry.Irradiance = nullptr;
                entry.Prefilter = nullptr;
                entry.Error = "Unknown environment IBL preparation failure";
                entry.State = SceneEnvironmentState::Failed;
            }
            return GetEnvironmentPreparation(key);
        }

        void SelectEnvironment(const RenderEnvironment& environment)
        {
            const std::string key = NormalizeEnvironmentKey(environment.HdrTexturePath);
            nvrhi::TextureHandle irradiance = DefaultEnvironment;
            nvrhi::TextureHandle prefilter = DefaultEnvironment;
            nvrhi::TextureHandle environmentMap = DefaultEnvironment;
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
                    brdf = BrdfLut;
                    bReady = true;
                }
            }

            if (ActiveIrradiance != irradiance || ActivePrefilter != prefilter ||
                ActiveEnvironment != environmentMap ||
                ActiveBrdfLut != brdf)
            {
                ActiveIrradiance = std::move(irradiance);
                ActivePrefilter = std::move(prefilter);
                ActiveEnvironment = std::move(environmentMap);
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
							.bCreateBindlessDescriptor = bHeapDirectlyIndexed,
                            .bPreserveAlphaCoverage =
                                index == 0 && bPreserveAlphaCoverage,
                            .AlphaCoverageCutoff = alphaCoverageCutoff,
                            .DebugName = "Material texture: " + reference.AssetPath,
                        });
					if (bHeapDirectlyIndexed)
					{
						textureSet.ShaderIndices[index] = TextureManagerInstance.GetTexture(
							textureSet.Handles[index]).ShaderDescriptorIndex;
						if (textureSet.ShaderIndices[index] == InvalidDescriptorIndex)
							throw std::logic_error("Material texture has no heap-direct descriptor");
					}
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
                return existing->second;
            }

            MaterialTextureSet replacement = BuildMaterialTextureSet(
                material,
                std::move(signature),
                bPreserveAlphaCoverage,
                alphaCoverageCutoff);
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
							.bCreateBindlessDescriptor = bHeapDirectlyIndexed,
                            .bPreserveAlphaCoverage =
                                alphaMode == RenderMaterialAlphaMode::Mask &&
                                fixup.Semantic == MaterialShaderTextureSemantic::BaseColor,
                            .AlphaCoverageCutoff = std::clamp(alphaCutoff, 0.0f, 1.0f),
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
                return existing->second;
            }

            MaterialParameterTextureSet replacement = BuildMaterialParameterTextureSet(
                packing,
                alphaMode,
                alphaCutoff);
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
            std::stable_sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right)
            {
                return std::tie(left->Category, left->DepthMode, left->Layer) <
                    std::tie(right->Category, right->DepthMode, right->Layer);
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

        void PrepareFrameData(const RenderWorld& world)
        {
			const std::uint64_t publicationRevision =
				MaterialShaderManagerInstance.GetPublicationRevision();
			if (publicationRevision != ObservedMaterialShaderPublicationRevision)
			{
				PipelineCacheInstance.Clear();
				ObservedMaterialShaderPublicationRevision = publicationRevision;
			}
            TextureManagerInstance.Collect(CurrentCompletedSubmission);
            CpuDrawItems.clear();
            TransparentDrawItems.clear();
            SkinnedDrawItems.clear();
            BoneMatricesCpu.clear();
            Stats.SkinnedNonIdentityBoneMatrixCount = 0;
            ShadowDrawItems.clear();
            OutlineDrawItems.clear();
            GpuObjectData.clear();
            GpuBoundsDataCpu.clear();
            GpuDrawDataCpu.clear();
            GpuIndirectArgumentsCpu.clear();
            GpuBatches.clear();
            GpuBatchObjectIndices.clear();
			MaterialPages.clear();
			RetiredMaterialPages.clear();
			MaterialPageByMaterialIndex.clear();
            Primitive2DVertices.clear();
            Primitive2DBatches.clear();
            GpuMaterialDataCpu.clear();
            MaterialParameterRowsCpu.clear();
            GpuMaterialRegistry.clear();
            NextGpuMaterialIndex = 1;
            if (FailedMaterialPrograms.size() > 256)
                FailedMaterialPrograms.clear();
            GpuMaterialDataCpu.resize(1);
            PopulateMaterialConstants(GpuMaterialDataCpu.front(), nullptr);
            std::unordered_map<GpuBatchKey, uint32_t, GpuBatchKeyHash> batchLookup;
            std::unordered_set<const Material*> frameMaterials;
            std::unordered_set<const Material*> frameLegacyTextureMaterials;
            std::unordered_set<const Material*> frameCustomParameterMaterials;
            std::unordered_set<uint32_t> frameGpuMaterials;
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
            FrameData.MaterialTimeAndPadding.x = std::isfinite(world.TimeSeconds)
                ? static_cast<float>(std::clamp(
                    world.TimeSeconds,
                    0.0,
                    static_cast<double>(std::numeric_limits<float>::max())))
                : 0.0f;
            FrameData.ViewProjection = CurrentRasterCamera.ViewProjection;
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
                2.0f,
                SanitizeDirectionalShadowNearPlane(world.Camera.NearPlane));
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
                if (light.bCastShadows && light.Intensity > 0.0f && shadowStrength > 0.0f)
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
            if (!world.PointLights.empty())
            {
                const RenderPointLight& light = world.PointLights.front();
                FrameData.PointPositionAndIntensity =
                    glm::vec4(light.Position, std::max(light.Intensity, 0.0f));
                FrameData.PointColorAndRadius =
                    glm::vec4(light.Color, std::max(light.Radius, 0.0f));
                FrameData.DirectionalShadowDebugSettings.z =
                    std::isfinite(light.FalloffExponent)
                        ? std::clamp(light.FalloffExponent, 0.0f, 8.0f)
                        : 2.0f;
            }

            for (const RenderObject& object : world.Objects)
            {
                if (!object.bVisible)
                    continue;

                uint32_t bonePaletteOffset = 0;
                const bool bObjectSkinned = object.Path == RenderPath::CpuSkinned ||
                    object.Path == RenderPath::CpuSkinnedTransparent;
                if (bObjectSkinned)
                {
                    if (BoneMatricesCpu.size() + c_MaxBonesPerPalette >
                        static_cast<size_t>(c_MaxSkinnedDraws) * c_MaxBonesPerPalette)
                    {
                        throw std::length_error("SceneRenderer skinned palette capacity exceeded");
                    }
                    bonePaletteOffset = static_cast<uint32_t>(BoneMatricesCpu.size());
                    for (uint32_t boneIndex = 0; boneIndex < c_MaxBonesPerPalette; ++boneIndex)
                    {
                        const bool bHasBone = boneIndex < object.BonePalette.size();
                        const glm::mat4 boneMatrix = bHasBone
                            ? object.BonePalette[boneIndex]
                            : glm::mat4(1.0f);
                        BoneMatricesCpu.push_back(boneMatrix);
                        if (bHasBone && !IsIdentityMatrix(boneMatrix))
                            ++Stats.SkinnedNonIdentityBoneMatrixCount;
                    }
                }

                glm::mat4 inverseTranspose(1.0f);
                if (std::abs(glm::determinant(object.WorldTransform)) > 1e-8f)
                    inverseTranspose = glm::transpose(glm::inverse(object.WorldTransform));

                for (std::size_t renderSectionIndex = 0;
                    renderSectionIndex < object.Sections.size();
                    ++renderSectionIndex)
                {
                    const RenderMeshSection& section = object.Sections[renderSectionIndex];
                    RenderPath sectionPath = section.Path;
                    // Synthetic/legacy snapshots created before section-level
                    // classification inherit the explicit object path.
                    if (sectionPath == RenderPath::CpuOpaque &&
                        !section.MaterialInstance.bCustomSurface &&
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
                    if (section.MaterialResource && !section.MaterialInstance.bValid)
                    {
                        throw std::invalid_argument(
                            "RenderMeshSection with a MaterialResource requires an immutable material snapshot");
                    }
                    if (section.MaterialInstance.bCustomSurface)
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
                    ObjectConstants constants;
                    constants.World = object.WorldTransform;
                    constants.WorldInverseTranspose = inverseTranspose;
                    const uint32_t gpuMaterialIndex = GetOrCreateGpuMaterialIndex(section);
                    constants.MaterialObjectMeshIndices = glm::uvec4(
                        gpuMaterialIndex,
                        object.ObjectGpuIndex == InvalidGpuSceneIndex
                            ? 0u
                            : object.ObjectGpuIndex,
                        object.MeshGpuIndex == InvalidGpuSceneIndex
                            ? 0u
                            : object.MeshGpuIndex,
                        bonePaletteOffset);

                    if (GpuMaterialDataCpu.size() <= gpuMaterialIndex)
                        GpuMaterialDataCpu.resize(gpuMaterialIndex + 1u);
					if (frameMaterialDescriptorInputs.size() <= gpuMaterialIndex)
						frameMaterialDescriptorInputs.resize(gpuMaterialIndex + 1u);
                    GpuMaterialData materialData;
                    PopulateMaterialConstants(
                        materialData,
                        &section.MaterialInstance,
                        section.MaterialResource.get(),
                        !section.MaterialInstance.bCustomSurface,
                        section.AlphaMode,
                        section.AlphaCutoff,
                        section.bDoubleSided);
                    if (section.MaterialResource &&
                        !section.MaterialInstance.bCustomSurface)
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

                    std::shared_ptr<const CompiledMaterialShader> materialProgram;
                    if (section.MaterialInstance.bCustomSurface)
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
                                .SourcePath = section.MaterialInstance.ShaderPath,
                                .RecursiveContentHash =
                                    section.MaterialInstance.ExpectedRecursiveSourceHash,
                                .MaterialAbiVersion =
                                    section.MaterialInstance.ExpectedMaterialAbiVersion,
                                .BindingSignatureHash =
                                    section.MaterialInstance.ExpectedBindingSignature,
                                .ArtifactIdentityHash =
                                    section.MaterialInstance.ExpectedArtifactIdentityHash,
                            };
                            materialProgram = MaterialShaderManagerInstance.FindCompatible(
                                expectedIdentity);

                            MaterialShaderRefreshResult refresh;
                            if (RequiresMaterialShaderFrameBoundaryRefresh(materialProgram))
                            {
                                refresh = RefreshMaterialShaderAtFrameBoundary(
                                    section.MaterialInstance.ShaderPath);
                                materialProgram = MaterialShaderManagerInstance.FindCompatible(
                                    expectedIdentity);
                            }
                            for (const MaterialShaderDiagnostic& diagnostic : refresh.Diagnostics)
                            {
                                if (diagnostic.Severity != MaterialShaderDiagnosticSeverity::Error)
                                    continue;
                                const std::string diagnosticKey =
                                    section.MaterialInstance.ShaderPath + ':' +
                                    diagnostic.Code + ':' + diagnostic.Message;
                                if (FailedMaterialPrograms.insert(diagnosticKey).second)
                                {
                                    HE_CORE_WARN(
                                        "Material shader '{}' retained last-known-good or skipped: [{}] {}",
                                        section.MaterialInstance.ShaderPath,
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
                                    section.MaterialInstance,
                                    programIdentity,
                                    materialProgram->GetGeneration());
                            if (!bSnapshotProgramCompatible)
                            {
                                const std::string diagnosticKey =
                                    section.MaterialInstance.AssetPath + ":snapshot-program-mismatch";
                                if (FailedMaterialPrograms.insert(diagnosticKey).second)
                                {
                                    HE_CORE_WARN(
                                        "Material '{}' snapshot does not match shader program '{}' generation {} (expected {}); draw deferred until an atomic layout migration",
                                        section.MaterialInstance.AssetPath,
                                        section.MaterialInstance.ShaderPath,
                                        materialProgram->GetGeneration(),
                                        section.MaterialInstance.ExpectedProgramGeneration);
                                }
                                continue;
                            }

                            const MaterialShaderManifest& programManifest =
                                materialProgram->GetManifest();
                            const MaterialShaderBlendMode programBlend = programManifest.BlendMode;
                            const bool bSnapshotStateMatchesSection =
                                section.MaterialInstance.AlphaMode == section.AlphaMode &&
                                section.MaterialInstance.bDoubleSided == section.bDoubleSided;
                            if (!IsMaterialSnapshotRenderStateCompatible(
                                    section.MaterialInstance,
                                    programManifest) ||
                                !bSnapshotStateMatchesSection)
                            {
                                const std::string diagnosticKey =
                                    section.MaterialInstance.ShaderPath + ":render-state-mismatch";
                                if (FailedMaterialPrograms.insert(diagnosticKey).second)
                                {
                                    HE_CORE_WARN(
                                        "Material '{}' snapshot render state does not match shader manifest (blend='{}', cull='{}', depthWrite={}); draw skipped",
                                        section.MaterialInstance.AssetPath,
                                        ToString(programBlend),
                                        ToString(programManifest.CullMode),
                                        programManifest.bDepthWrite ? 1 : 0);
                                }
                                continue;
                            }

                            MaterialParameterPackingResult packing = PackMaterialParameters(
                                materialProgram->GetManifest(),
                                section.MaterialInstance.ParameterValues);
                            if (!packing.Succeeded())
                            {
                                for (const MaterialShaderDiagnostic& diagnostic : packing.Diagnostics)
                                {
                                    const std::string diagnosticKey =
                                        section.MaterialInstance.ShaderPath + ':' +
                                        diagnostic.Code + ':' + diagnostic.Message;
                                    if (FailedMaterialPrograms.insert(diagnosticKey).second)
                                    {
                                        HE_CORE_WARN(
                                            "Material parameters for '{}' are invalid: [{}] {}",
                                            section.MaterialInstance.ShaderPath,
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
                                    textureIndex >= parameterTextures.ShaderIndices.size())
                                {
                                    throw std::logic_error(
                                        "Material texture fixup escaped its packed parameter block");
                                }
                                packing.PackedFloat4Rows[rowOffset][0] = std::bit_cast<float>(
                                    parameterTextures.ShaderIndices[textureIndex]);
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

                    if (object.bSelected)
                    {
                        OutlineDrawItems.push_back(DrawItem{
                            .Key = mesh.Resource.get(),
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
                            .Constants = constants,
                            .MaterialProgram = materialProgram,
                            .bDoubleSided = section.bDoubleSided,
                        });
                    }
                    else if (sectionPath == RenderPath::Transparent)
                    {
                        TransparentDrawItems.push_back(DrawItem{
                            .Key = mesh.Resource.get(),
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
                        bDirectionalShadowEnabled && object.bCastShadow &&
                        (!materialProgram || materialProgram->GetManifest().bCastsShadow))
                    {
                        ShadowDrawItems.push_back(DrawItem{
                            .Key = mesh.Resource.get(),
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
                    if (!bounds.bValid && section.SubMeshResource->HasBounds())
                    {
                        const glm::vec3 localMinimum = section.SubMeshResource->GetBoundsMin();
                        const glm::vec3 localMaximum = section.SubMeshResource->GetBoundsMax();
                        const glm::vec3 localCenter = (localMinimum + localMaximum) * 0.5f;
                        const glm::vec3 localExtents = (localMaximum - localMinimum) * 0.5f;
                        bounds.Center = glm::vec3(object.WorldTransform * glm::vec4(localCenter, 1.0f));
                        const float maximumScale = std::max({
                            glm::length(glm::vec3(object.WorldTransform[0])),
                            glm::length(glm::vec3(object.WorldTransform[1])),
                            glm::length(glm::vec3(object.WorldTransform[2])),
                        });
                        bounds.Radius = glm::length(localExtents) * maximumScale;
                        bounds.bValid = true;
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
							.MaterialIndex = gpuMaterialIndex,
						});
                        GpuBatchObjectIndices.emplace_back();
                        batch = batchLookup.emplace(batchKey, batchIndex).first;
                    }
                    GpuBatchObjectIndices[batch->second].push_back(objectIndex);
                }
            }

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
                    .setIndexCount(mesh.IndexCount)
                    .setInstanceCount(0)
                    .setStartIndexLocation(0)
                    .setBaseVertexLocation(0)
                    .setStartInstanceLocation(0));
                for (const uint32_t objectIndex : GpuBatchObjectIndices[batchIndex])
                {
                    GpuDrawDataCpu.push_back(GpuDrawData{
                        .ObjectIndex = objectIndex,
                        .BatchIndex = batchIndex,
                        .VisibleBase = visibleBase,
                    });
                }
                visibleBase += batch.ObjectCount;
            }

            for (auto iterator = MaterialTextureCache.begin();
                iterator != MaterialTextureCache.end();)
            {
                if (frameLegacyTextureMaterials.contains(iterator->first))
                {
                    ++iterator;
                    continue;
                }
                ReleaseMaterialTextureSet(iterator->second, LastSubmittedSubmission);
                iterator = MaterialTextureCache.erase(iterator);
            }
            for (auto iterator = MaterialParameterTextureCache.begin();
                iterator != MaterialParameterTextureCache.end();)
            {
                if (frameCustomParameterMaterials.contains(iterator->first))
                {
                    ++iterator;
                    continue;
                }
                ReleaseMaterialParameterTextureSet(
                    iterator->second,
                    LastSubmittedSubmission);
                iterator = MaterialParameterTextureCache.erase(iterator);
            }

			std::erase_if(frameMaterialDescriptorInputs,
				[](const FrameMaterialDescriptorInput& input)
				{
					return input.Textures.empty();
				});
			BuildBaselineMaterialDescriptorPages(
				frameMaterialDescriptorInputs);
            PreparePrimitive2DData(world);

			const uint64_t objectConstantWrites =
				static_cast<uint64_t>(CpuDrawItems.size()) * 2ull +
				static_cast<uint64_t>(SkinnedDrawItems.size()) * 2ull +
				static_cast<uint64_t>(TransparentDrawItems.size()) +
				static_cast<uint64_t>(OutlineDrawItems.size()) +
				static_cast<uint64_t>(ShadowDrawItems.size()) *
					DirectionalShadowCascadeCount;
			if (objectConstantWrites > c_MaxObjectConstantWritesPerFrame)
			{
				throw std::length_error(
					"SceneRenderer requires " +
					std::to_string(objectConstantWrites) +
					" volatile object-constant writes in one frame, exceeding the explicit capacity of " +
					std::to_string(c_MaxObjectConstantWritesPerFrame));
			}

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
                    program ? program->GetGeneration() : 0ull);
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
            Stats.BindlessTextureCount = static_cast<uint32_t>(
                TextureManagerInstance.GetBindlessTextures().size());
            if (LastTextureManagerRevision != TextureManagerInstance.GetRevision())
            {
                LastTextureManagerRevision = TextureManagerInstance.GetRevision();
                bGraphDirty = true;
            }
        }

        void BuildGraph(const SceneRendererFrameDesc& frame)
        {
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
                    .Width = frame.Width,
                    .Height = frame.Height,
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
                    .ByteSize = static_cast<uint64_t>(c_MaxSkinnedDraws) *
                        c_MaxBonesPerPalette * sizeof(glm::mat4),
                    .StructStride = sizeof(glm::mat4),
                },
                BoneMatrixBuffer,
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Common);

			std::vector<nvrhi::TextureHandle> frameMaterialTextures =
				TextureManagerInstance.GetBindlessTextures();
			if (!bHeapDirectlyIndexed)
			{
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

            for (const SubMesh* key : MeshOrder)
            {
                MeshEntry& mesh = MeshCache.at(key);
                mesh.GraphIndex = static_cast<uint32_t>(GraphGeometryResources.size());
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
                            .ByteSize = static_cast<uint64_t>(mesh.Resource->GetIndices().size()) *
                                sizeof(uint32_t),
                            .StructStride = sizeof(uint32_t),
                        },
                        mesh.IndexBuffer,
                        initialIndexState,
                        nvrhi::ResourceStates::IndexBuffer),
                });
            }

            AddSceneUploadPass();
            CurrentRtGraphResources = {};
            CurrentSurfaceData = {};
            CurrentDepthPyramid = {};
            CurrentGiOutputs = {};
            OpaqueDirectHdrHandle = {};
            ToneMappedSceneHandle = {};
            if (HardwareRayTracingScene && HardwareRayTracingScene->IsReady())
            {
                CurrentRtGraphResources =
                    HardwareRayTracingScene->AddBuildPasses(Graph);
            }
            AddDirectionalShadowPass();
            AddGpuCullingPass();
            AddGpuCullingReadbackPass();
            AddDepthPrepass();
            if (IsMsaaActive())
                AddDepthPrepass(true);
            if (SurfaceDataPassInstance)
            {
                CurrentSurfaceData = SurfaceDataPassInstance->AddPasses(
                    Graph,
                    SceneDepthHandle,
                    CurrentRtGraphResources);
            }
            AddOpaquePass();
            AddSkyPass();
            AddSkinnedPass();
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
            if (IsGlobalIlluminationActive())
                AddGlobalIlluminationPasses();
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
                AddPrimitive2DPass();
                AddTransparentPass();
                AddEditorOutlinePass();
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
                            .SceneDepth = SceneDepthHandle,
                            .Width = frame.Width,
                            .Height = frame.Height,
                            .View = CurrentVfxView.View,
                            .Projection = CurrentVfxView.Projection,
                            .CameraPosition = CurrentVfxView.CameraPosition,
                            .ZNear = CurrentVfxView.ZNear,
                            .ZFar = CurrentVfxView.ZFar,
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
            if (IsGlobalIlluminationActive())
                AddLinearHdrCapturePass();
            AddToneMappingPass();
            AddSpatialAntiAliasingPass();
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
                    data.FrameConstantsBuffer = builder.CopyDest(FrameBufferHandle);
                    for (const GraphGeometry& geometry : GraphGeometryResources)
                    {
                        const MeshEntry& mesh = MeshCache.at(geometry.Key);
                        if (!mesh.bUploaded)
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
                        if (mesh.bUploaded)
                            continue;

                        (void)context.GetBuffer(data.VertexBuffers.at(uploadIndex), RenderGraphAccess::Write);
                        (void)context.GetBuffer(data.IndexBuffers.at(uploadIndex), RenderGraphAccess::Write);
                        commandList->writeBuffer(
                            mesh.VertexBuffer,
                            mesh.bSkinned
                                ? static_cast<const void*>(mesh.Resource->mSkinnedVertices.data())
                                : static_cast<const void*>(mesh.Resource->mStaticVertices.data()),
                            mesh.bSkinned
                                ? mesh.Resource->mSkinnedVertices.size() * sizeof(SkinnedVertex)
                                : mesh.Resource->mStaticVertices.size() * sizeof(StaticVertex));
                        commandList->writeBuffer(
                            mesh.IndexBuffer,
                            mesh.Resource->GetIndices().data(),
                            mesh.Resource->GetIndices().size() * sizeof(uint32_t));
                        mesh.bUploaded = true;
                        ++uploadIndex;
                        ++Stats.UploadedMeshCount;
                        bGraphDirty = true;
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
                    if (!GpuObjectData.empty())
                    {
                        commandList->writeBuffer(
                            GpuObjectBuffer,
                            GpuObjectData.data(),
                            GpuObjectData.size() * sizeof(ObjectConstants));
                        commandList->writeBuffer(
                            GpuBoundsBuffer,
                            GpuBoundsDataCpu.data(),
                            GpuBoundsDataCpu.size() * sizeof(GpuBoundsData));
                        commandList->writeBuffer(
                            GpuDrawDataBuffer,
                            GpuDrawDataCpu.data(),
                            GpuDrawDataCpu.size() * sizeof(GpuDrawData));
                        commandList->writeBuffer(
                            IndirectArgumentsBuffer,
                            GpuIndirectArgumentsCpu.data(),
                            GpuIndirectArgumentsCpu.size() *
                                sizeof(nvrhi::DrawIndexedIndirectArguments));
                    }
                    if (!GpuMaterialDataCpu.empty())
                    {
                        commandList->writeBuffer(
                            GpuMaterialBuffer,
                            GpuMaterialDataCpu.data(),
                            GpuMaterialDataCpu.size() * sizeof(GpuMaterialData));
                    }
                    if (!MaterialParameterRowsCpu.empty())
                    {
                        commandList->writeBuffer(
                            MaterialParameterBuffer,
                            MaterialParameterRowsCpu.data(),
                            MaterialParameterRowsCpu.size() * sizeof(glm::vec4));
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
                        commandList->writeBuffer(
                            BoneMatrixBuffer,
                            BoneMatricesCpu.data(),
                            BoneMatricesCpu.size() * sizeof(glm::mat4));
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
                    DirectionalShadowHandle = data.ShadowDepth;
                },
                [this](RenderGraphContext& context, const DirectionalShadowPassData& data)
                {
                    RecordDirectionalShadowPass(context, data);
                });
        }

        void AddDepthPrepass(bool bMultisample = false)
        {
            Graph.AddPass<DepthPrepassPassData>(
                bMultisample ? "SceneDepthPrepassMSAA" : "SceneDepthPrepass",
                RenderGraphQueue::Graphics,
                [this, bMultisample](
                    RenderGraphBuilder& builder,
                    DepthPrepassPassData& data)
                {
                    data.SceneDepth = !bMultisample &&
                            CurrentOutputMode == SceneRendererOutputMode::OffscreenOnly
                        ? OffscreenDepthHandle
                        : builder.CreateTexture(RenderGraphTextureDesc{
                            .DebugName = bMultisample
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
                    data.VisibleObjectIndices = builder.ShaderResource(VisibleObjectIndicesHandle);
                    data.IndirectArguments = builder.IndirectArgument(IndirectArgumentsHandle);
                    data.BoneMatrices = builder.ShaderResource(BoneMatricesHandle);
                    for (const GraphMaterialTexture& texture : GraphMaterialTextures)
                        data.MaterialTextures.push_back(builder.ShaderResource(texture.Handle));
                    for (const GraphGeometry& geometry : GraphGeometryResources)
                    {
                        data.VertexBuffers.push_back(builder.VertexBuffer(geometry.VertexBuffer));
                        data.IndexBuffers.push_back(builder.IndexBuffer(geometry.IndexBuffer));
                    }
                    if (bMultisample)
                        RasterSceneDepthHandle = data.SceneDepth;
                    else
                    {
                        SceneDepthHandle = data.SceneDepth;
                        RasterSceneDepthHandle = data.SceneDepth;
                    }
                    if (!bMultisample &&
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

        void AddOpaquePass()
        {
            Graph.AddPass<OpaquePassData>(
                "OpaqueForwardHDR",
                RenderGraphQueue::Graphics,
                [this](RenderGraphBuilder& builder, OpaquePassData& data)
                {
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
                    data.VisibleObjectIndices = builder.ShaderResource(VisibleObjectIndicesHandle);
                    data.IndirectArguments = builder.IndirectArgument(IndirectArgumentsHandle);
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
                            .addBindingSet(FrameBindingSet)
                            .addBindingSet(ObjectBindingSet)
                            .addBindingSet(environmentBindingSet)
							.addBindingSet(GetMaterialTextureBindingSet(
								drawItem.Constants.MaterialObjectMeshIndices.x))
                            .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                            .addVertexBuffer(nvrhi::VertexBufferBinding()
                                .setBuffer(mesh.VertexBuffer).setSlot(0).setOffset(0))
                            .setIndexBuffer(nvrhi::IndexBufferBinding()
                                .setBuffer(mesh.IndexBuffer)
                                .setFormat(nvrhi::Format::R32_UINT).setOffset(0));
                        context.GetCommandList()->setGraphicsState(state);
                        context.GetCommandList()->drawIndexed(
                            nvrhi::DrawArguments().setVertexCount(mesh.IndexCount));
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
                            .addBindingSet(FrameBindingSet)
                            .addBindingSet(ObjectBindingSet)
                            .addBindingSet(environmentBindingSet)
							.addBindingSet(GetMaterialTextureBindingSet(
								drawItem.Constants.MaterialObjectMeshIndices.x))
                            .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                            .addVertexBuffer(nvrhi::VertexBufferBinding()
                                .setBuffer(mesh.VertexBuffer)
                                .setSlot(0)
                                .setOffset(0))
                            .setIndexBuffer(nvrhi::IndexBufferBinding()
                                .setBuffer(mesh.IndexBuffer)
                                .setFormat(nvrhi::Format::R32_UINT)
                                .setOffset(0));
                        context.GetCommandList()->setGraphicsState(state);
                        context.GetCommandList()->drawIndexed(
                            nvrhi::DrawArguments().setVertexCount(mesh.IndexCount));
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
                    data.SceneColor = builder.ReadWrite(
                        SceneColorHandle,
                        RenderGraphUsage::RenderTarget);
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
                                .setOffset(0))
                            .setIndexBuffer(nvrhi::IndexBufferBinding()
                                .setBuffer(mesh.IndexBuffer)
                                .setFormat(nvrhi::Format::R32_UINT)
                                .setOffset(0));
                        if (bCustomMask)
                        {
							state.addBindingSet(GetMaterialTextureBindingSet(
								drawItem.Constants.MaterialObjectMeshIndices.x))
                                .addBindingSet(TextureManagerInstance.GetSamplerBindingSet());
                        }
                        commandList->setGraphicsState(state);
                        commandList->drawIndexed(
                            nvrhi::DrawArguments().setVertexCount(mesh.IndexCount));
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
                                .Width = LastWidth,
                                .Height = LastHeight,
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
                            .Width = LastWidth,
                            .Height = LastHeight,
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
                    .Width = LastWidth,
                    .Height = LastHeight,
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
                        .Width = LastWidth,
                        .Height = LastHeight,
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
                        .Width = LastWidth,
                        .Height = LastHeight,
                        .FrameSlot = CurrentFrameSlot,
                        .CompletedSubmission = CurrentCompletedSubmission,
                    });
                    Stats.bOverlayPassRecorded = true;
                });
        }

        void AddOffscreenCopyPass()
        {
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
                    data.SceneColor = builder.CopySource(SceneColorHandle);
                    builder.SetSideEffect();
                },
                [this](
                    RenderGraphContext& context,
                    const LinearHdrCapturePassData& data)
                {
                    nvrhi::ITexture* sceneColor = context.GetTexture(
                        data.SceneColor,
                        RenderGraphAccess::Read);
                    if (!CurrentLinearHdrCaptureRecorder)
                        return;
                    CurrentLinearHdrCaptureRecorder(
                        SceneRendererLinearHdrCaptureContext{
                            .CommandList = context.GetCommandList(),
                            .LinearHdr = sceneColor,
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
                    LastWidth,
                    LastHeight);
            }
            else if (IsSmaaActive())
            {
                BackBufferHandle = SceneAntiAliasing.AddSmaaPasses(
                    Graph,
                    ToneMappedSceneHandle,
                    BackBufferHandle,
                    AaSelection.Effective,
                    LastWidth,
                    LastHeight);
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
                        .Width = LastWidth,
                        .Height = LastHeight,
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

            for (uint32_t batchIndex = 0;
                batchIndex < static_cast<uint32_t>(GpuBatches.size());
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
                    .setIndexBuffer(nvrhi::IndexBufferBinding()
                        .setBuffer(mesh.IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT)
                        .setOffset(0))
                    .setIndirectParams(IndirectArgumentsBuffer);
                commandList->setGraphicsState(state);
                const GpuDrawConstants drawConstants{ .VisibleBase = batch.VisibleBase };
                commandList->setPushConstants(&drawConstants, sizeof(drawConstants));
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
                        .setBuffer(mesh.VertexBuffer).setSlot(0).setOffset(0))
                    .setIndexBuffer(nvrhi::IndexBufferBinding()
                        .setBuffer(mesh.IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT)
                        .setOffset(0));
                commandList->setGraphicsState(state);
                commandList->drawIndexed(
                    nvrhi::DrawArguments().setVertexCount(mesh.IndexCount));
            };
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
            (void)context.GetBuffer(data.ObjectConstantsBuffer, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.BoneMatrices, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.GpuMaterials, RenderGraphAccess::Read);
            (void)context.GetBuffer(data.MaterialParameters, RenderGraphAccess::Read);
            for (TextureHandle texture : data.MaterialTextures)
                (void)context.GetTexture(texture, RenderGraphAccess::Read);

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
                for (const DrawItem& drawItem : ShadowDrawItems)
                {
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
                            .setOffset(0))
                        .setIndexBuffer(nvrhi::IndexBufferBinding()
                            .setBuffer(mesh.IndexBuffer)
                            .setFormat(nvrhi::Format::R32_UINT)
                            .setOffset(0));
                    commandList->setGraphicsState(state);
                    commandList->setPushConstants(
                        &shadowPassConstants,
                        sizeof(shadowPassConstants));
                    commandList->drawIndexed(
                        nvrhi::DrawArguments().setVertexCount(mesh.IndexCount));
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

            for (uint32_t batchIndex = 0;
                batchIndex < static_cast<uint32_t>(GpuBatches.size());
                ++batchIndex)
            {
                const GpuBatch& batch = GpuBatches[batchIndex];
                const MeshEntry& mesh = MeshCache.at(batch.Key.Mesh);
                if (mesh.GraphIndex >= data.VertexBuffers.size())
                    throw std::logic_error("SceneRenderer GPU mesh graph index is stale");
                (void)context.GetBuffer(data.VertexBuffers[mesh.GraphIndex], RenderGraphAccess::Read);
                (void)context.GetBuffer(data.IndexBuffers[mesh.GraphIndex], RenderGraphAccess::Read);

                nvrhi::GraphicsState state;
                state.setPipeline(GetOrCreateGpuOpaquePipeline(
                        framebuffer,
                        batch.Key.bDoubleSided))
                    .setFramebuffer(framebuffer)
                    .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                        nvrhi::Viewport(static_cast<float>(LastWidth), static_cast<float>(LastHeight))))
                    .addBindingSet(FrameBindingSet)
                    .addBindingSet(GpuObjectBindingSet)
                    .addBindingSet(shadowBindingSet)
					.addBindingSet(GetMaterialTextureBindingSet(batch.MaterialIndex))
                    .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                    .addVertexBuffer(nvrhi::VertexBufferBinding()
                        .setBuffer(mesh.VertexBuffer)
                        .setSlot(0)
                        .setOffset(0))
                    .setIndexBuffer(nvrhi::IndexBufferBinding()
                        .setBuffer(mesh.IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT)
                        .setOffset(0))
                    .setIndirectParams(IndirectArgumentsBuffer);
                commandList->setGraphicsState(state);
                const GpuDrawConstants drawConstants{ .VisibleBase = batch.VisibleBase };
                commandList->setPushConstants(&drawConstants, sizeof(drawConstants));
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
                    .addBindingSet(FrameBindingSet)
                    .addBindingSet(ObjectBindingSet)
                    .addBindingSet(shadowBindingSet)
					.addBindingSet(GetMaterialTextureBindingSet(
						drawItem.Constants.MaterialObjectMeshIndices.x))
                    .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                    .addVertexBuffer(nvrhi::VertexBufferBinding()
                        .setBuffer(mesh.VertexBuffer)
                        .setSlot(0)
                        .setOffset(0))
                    .setIndexBuffer(nvrhi::IndexBufferBinding()
                        .setBuffer(mesh.IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT)
                        .setOffset(0));
                commandList->setGraphicsState(state);
                commandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(mesh.IndexCount));
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
                    nvrhi::Viewport(static_cast<float>(LastWidth), static_cast<float>(LastHeight))))
                .addBindingSet(bindingSet);
            commandList->setGraphicsState(state);
            const ToneMappingConstants constants{
                .Exposure = Exposure,
                .PbrDebugMode = static_cast<uint32_t>(CurrentPbrDebugMode),
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
                .addItem(nvrhi::BindingSetItem::Texture_SRV(3, brdfLut))
                .addItem(nvrhi::BindingSetItem::Sampler(0, ShadowSampler));
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
            bool bTransparent)
        {
            const MaterialShaderVertexPath vertexPath = bSkinned
                ? MaterialShaderVertexPath::Skinned
                : MaterialShaderVertexPath::Static;
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
                    "Material program is missing a required pass/vertex/tier shader");
            }

            nvrhi::GraphicsPipelineDesc pipelineDesc;
            pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(bSkinned ? SkinnedInputLayout : InputLayout)
                .setVertexShader(vertexShader)
                .setPixelShader(pixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(ObjectBindingLayout);
            if (pass == MaterialShaderPass::Forward)
                pipelineDesc.addBindingLayout(ShadowTextureBindingLayout);
            else if (pass == MaterialShaderPass::Shadow)
                pipelineDesc.addBindingLayout(ShadowPassBindingLayout);
            pipelineDesc
                .addBindingLayout(TextureManagerInstance.GetTextureBindingLayout())
                .addBindingLayout(TextureManagerInstance.GetSamplerBindingLayout());

            if (pass == MaterialShaderPass::Forward)
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
            keyDesc.Pass = pass == MaterialShaderPass::Depth
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
                : MaterialPipelineVertexPath::CpuStatic;
            keyDesc.DescriptorTier = bHeapDirectlyIndexed
                ? MaterialPipelineDescriptorTier::HeapDirect
                : MaterialPipelineDescriptorTier::Baseline;
            keyDesc.InputLayout = MaterialInputLayoutIdentity{
                .Value = bSkinned ? 0x534b494e4e454431ull : 0x5354415449433031ull,
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
            bool bDoubleSided)
        {
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(bSkinned
                    ? SkinnedShadowInputLayout
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
                static_cast<uint64_t>(LastBackBufferFormat));
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

        void RecordFrame(const RenderWorld& world, const SceneRendererFrameDesc& frame)
        {
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
            if (frame.bReuseOffscreenScene && IsGlobalIlluminationActive())
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
            CurrentLinearHdrCaptureRecorder = frame.LinearHdrCaptureRecorder;
            CurrentFrameSlot = frame.FrameSlot;
            CurrentCompletedSubmission = frame.CompletedSubmission;
            CurrentOutputMode = frame.OutputMode;
            CurrentSceneOverlayMode = frame.SceneOverlayMode;
            CurrentSceneOverlayTransition = frame.SceneOverlayTransition;
            Stats.bOverlayPassRecorded = false;
            Stats.bCapturePassRecorded = false;
            Stats.bLinearHdrCaptureRecorded = false;
            Stats.bOffscreenOutputRecorded = false;
			Stats.bSceneReused = frame.bReuseOffscreenScene;
			if (!frame.bReuseOffscreenScene)
            {
                PrepareAntiAliasingFrame(world, frame);
				PrepareFrameData(world);
                PrepareSceneSurfaceFrame(world, frame);
                PrepareGlobalIlluminationFrame(world, frame);
            }
            CurrentVfxView = EffekseerRenderView{
                .View = world.Camera.View,
                .Projection = CurrentRasterCamera.Projection,
                .CameraPosition = world.Camera.Position,
                .ZNear = world.Camera.NearPlane,
                .ZFar = world.Camera.FarPlane,
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
				}
            }
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
                LastWidth = frame.Width;
                LastHeight = frame.Height;
                BuildGraph(frame);
            }
            else
            {
                UpdateImportedResources(frame);
            }

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
                GpuTiming.BeginFrame(frame.FrameSlot, frame.CompletedSubmission);
                RefreshGpuTimingStats();
                Graph.Execute(RenderGraphExecutionParams{
                    .CommandList = frame.CommandList,
                    .ResourcePool = &ResourcePool,
                    .Instrumentation = GpuTiming.IsEnabled() ? &GpuTiming : nullptr,
                    .FrameSlot = frame.FrameSlot,
                });
				if (bNeedsOffscreen && !bOffscreenOutputInitialized)
				{
					bOffscreenOutputInitialized = true;
					bGraphDirty = true;
				}
                RefreshRenderGraphMemoryStats();
                GpuTiming.EndFrame(frame.FrameSlot);
                Stats.CompiledPasses = Graph.GetCompiledPassNames();
                CurrentSceneOverlayRecorder = {};
                CurrentWindowOverlayRecorder = {};
                CurrentCaptureRecorder = {};
                CurrentLinearHdrCaptureRecorder = {};
            }
            catch (...)
            {
                GpuTiming.AbortFrame(frame.FrameSlot);
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
            VfxRenderGraphPass.ClearResourceCaches();
            RuntimeUiRenderPass.ClearResourceCaches();
            ShadowFramebuffers.clear();
            EnvironmentBindingSets.clear();
            DirectionalShadowTextures.clear();
            OpaqueFramebuffers.clear();
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
            for (uint32_t frameSlot = 0; frameSlot < FramesInFlight; ++frameSlot)
            {
                if (ResourcePool.IsFrameActive(frameSlot))
                    throw std::logic_error("Cannot clear SceneRenderer caches with an active frame slot");
            }
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
            GpuMaterialRegistry.clear();
            NextGpuMaterialIndex = 1;
            for (auto& [material, textureSet] : MaterialTextureCache)
            {
                (void)material;
                ReleaseMaterialTextureSet(textureSet, LastSubmittedSubmission);
            }
            MaterialTextureCache.clear();
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
                    maxVisibleInstancesPerBatch = std::max(
                        maxVisibleInstancesPerBatch,
                        arguments[batchIndex].instanceCount);
                    if (arguments[batchIndex].instanceCount > 1)
                        ++instancedBatchCount;
                }
                Device->unmapBuffer(slot.Buffer);
            }
            Stats.GpuVisibleObjectCount = visibleCount;
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
                    Stats.GiStats.LastGpuMilliseconds);
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
            std::string debugName)
        {
            if (width == 0 || height == 0)
                throw std::invalid_argument("Offscreen output dimensions must be non-zero");
            for (uint32_t frameSlot = 0; frameSlot < FramesInFlight; ++frameSlot)
            {
                if (ResourcePool.IsFrameActive(frameSlot))
                {
                    throw std::logic_error(
                        "Cannot configure an offscreen output with an active frame slot");
                }
            }
            if (OffscreenOutput && OffscreenOutput.Width == width &&
                OffscreenOutput.Height == height && OffscreenDebugName == debugName)
            {
                return;
            }

            nvrhi::TextureDesc colorDesc;
            colorDesc.width = width;
            colorDesc.height = height;
            colorDesc.format = nvrhi::Format::RGBA8_UNORM;
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
            ToneFramebuffers.clear();
            OffscreenDebugName = std::move(debugName);
            OffscreenOutput = SceneRendererOffscreenOutput{
                .Color = std::move(color),
                .Depth = std::move(depth),
                .Width = width,
                .Height = height,
                .ColorFormat = nvrhi::Format::RGBA8_UNORM,
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
        uint32_t FramesInFlight = 0;
        std::filesystem::path ShaderCacheDirectory;
        std::filesystem::path AssetRootDirectory;
        std::filesystem::path EngineAssetRootDirectory;
        RenderBackend Backend = RenderBackend::DX12;
        RenderCapabilities Capabilities;
        AntiAliasingCapabilities AaCapabilities;
        AntiAliasingSettings RequestedAaSettings;
        AntiAliasingSelection AaSelection;
        RenderCamera CurrentRasterCamera;
        glm::mat4 PreviousRasterViewProjection{ 1.0f };
        glm::mat4 PendingRasterViewProjection{ 1.0f };
        glm::mat4 PreviousTaaNonJitteredViewProjection{ 1.0f };
        glm::mat4 PendingTaaNonJitteredViewProjection{ 1.0f };
        uint64_t PreviousRasterJitterGeneration = 0;
        uint64_t PendingRasterJitterGeneration = 0;
        bool bHasPreviousRasterCamera = false;
        bool bRasterCameraFrameRecorded = false;
        GlobalIlluminationSettings RequestedGiSettings;
        GlobalIlluminationSettings EffectiveGiSettings;
        GlobalIlluminationSettings CurrentIlluminationTechniqueSettings;
        GlobalIlluminationSelection GiSelection;
        RayTracingSceneRevisions CurrentRtRevisions;
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
        nvrhi::InputLayoutHandle ShadowInputLayout;
        nvrhi::InputLayoutHandle SkinnedShadowInputLayout;
        nvrhi::InputLayoutHandle Primitive2DInputLayout;
        nvrhi::InputLayoutHandle SkinnedInputLayout;
        nvrhi::InputLayoutHandle OutlineInputLayout;
        nvrhi::InputLayoutHandle SkinnedOutlineInputLayout;
        nvrhi::SamplerHandle ToneSampler;
        nvrhi::SamplerHandle ShadowSampler;
        nvrhi::SamplerHandle EnvironmentSampler;
        nvrhi::SamplerHandle EquirectangularSampler;

        nvrhi::BufferHandle FrameConstantBuffer;
        nvrhi::BufferHandle ObjectConstantBuffer;
        nvrhi::BufferHandle GpuObjectBuffer;
        nvrhi::BufferHandle GpuMaterialBuffer;
        nvrhi::BufferHandle MaterialParameterBuffer;
        nvrhi::BufferHandle BoneMatrixBuffer;
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
        std::unordered_map<const Material*, MaterialTextureSet> MaterialTextureCache;
        std::unordered_map<const Material*, MaterialParameterTextureSet>
            MaterialParameterTextureCache;
        std::unordered_map<std::string, ManagedTextureHandle> PrimitiveTextureCache;
        std::unordered_map<
            MaterialRegistryKey,
            uint32_t,
            MaterialRegistryKeyHash> GpuMaterialRegistry;
        std::unordered_map<std::string, EnvironmentEntry> EnvironmentCache;
        std::string ActiveEnvironmentKey;
        std::unordered_set<std::string> FailedMaterialTextures;
        std::unordered_set<std::string> FailedMaterialPrograms;
        std::vector<const SubMesh*> MeshOrder;
        std::vector<GraphGeometry> GraphGeometryResources;
        std::vector<GraphMaterialTexture> GraphMaterialTextures;
        std::vector<DrawItem> CpuDrawItems;
        std::vector<DrawItem> SkinnedDrawItems;
        std::vector<DrawItem> TransparentDrawItems;
        std::vector<DrawItem> ShadowDrawItems;
        std::vector<DrawItem> OutlineDrawItems;
        std::vector<glm::mat4> BoneMatricesCpu;
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
        TextureHandle DirectionalShadowHandle;
        TextureHandle IrradianceHandle;
        TextureHandle PrefilterHandle;
        TextureHandle EnvironmentHandle;
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
        uint32_t NextGpuMaterialIndex = 1;
        uint64_t LastTextureManagerRevision = 0;
        uint32_t DirectionalShadowCascadeCount = 1;
        uint32_t DirectionalShadowMapSize = c_DefaultDirectionalShadowMapSize;
        bool bGraphDirty = true;
        bool bGlobalIlluminationFrameRecorded = false;
        bool bDirectionalShadowEnabled = false;
        bool bDirectionalShadowCascadeDebug = false;
        ScenePbrDebugMode CurrentPbrDebugMode = ScenePbrDebugMode::Disabled;
        SceneRendererStats Stats;
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

    void SceneRenderer::RecordFrame(const RenderWorld& world, const SceneRendererFrameDesc& frame)
    {
        mImpl->RecordFrame(world, frame);
    }

    void SceneRenderer::SetDirectionalShadowCascadeDebug(bool bEnabled) noexcept
    {
        mImpl->bDirectionalShadowCascadeDebug = bEnabled;
    }

    void SceneRenderer::SetPbrDebugMode(ScenePbrDebugMode mode) noexcept
    {
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
            previous.ScreenSpaceRayCount == settings.ScreenSpaceRayCount &&
            previous.ScreenSpaceStepCount == settings.ScreenSpaceStepCount &&
            previous.ScreenSpaceHiZStartMip == settings.ScreenSpaceHiZStartMip &&
            previous.ScreenSpaceBinaryRefinementSteps ==
                settings.ScreenSpaceBinaryRefinementSteps &&
            previous.ScreenSpaceMaximumRayDistance ==
                settings.ScreenSpaceMaximumRayDistance &&
            previous.ScreenSpaceThickness == settings.ScreenSpaceThickness &&
            previous.ScreenSpaceEdgeFade == settings.ScreenSpaceEdgeFade &&
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

    void SceneRenderer::ConfigureOffscreenOutput(
        uint32_t width,
        uint32_t height,
        std::string debugName)
    {
        mImpl->ConfigureOffscreenOutput(width, height, std::move(debugName));
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
        if (mImpl->bRasterCameraFrameRecorded)
        {
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
        if (mImpl->HardwareRayTracingScene)
            mImpl->HardwareRayTracingScene->AbortFrame();
        if (mImpl->IsGlobalIlluminationActive())
        {
            mImpl->GetActiveIlluminationTechnique()->AbortFrame();
            mImpl->bGlobalIlluminationFrameRecorded = false;
        }
        mImpl->SceneAntiAliasing.AbortFrame();
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
