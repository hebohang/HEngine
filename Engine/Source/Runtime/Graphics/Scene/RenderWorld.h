#pragma once

#include "Runtime/Resource/ResType/TextureSampling.h"

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/Material/MaterialParameterPacking.h"
#include "Runtime/Graphics/Renderer/VolumetricCloud.h"
#include "Runtime/Core/Meta/Json.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace HEngine
{
    class Material;
    class Mesh;
    class SubMesh;

    inline constexpr uint32_t InvalidGpuSceneIndex = std::numeric_limits<uint32_t>::max();

    enum class RenderPath : uint8_t
    {
        GpuDrivenOpaque,
        CpuOpaque,
        CpuSkinned,
        CpuSkinnedTransparent,
        Transparent,
        EditorSpecial,
        ThirdPartyVfx,
        Ui,
    };

    enum class RenderMaterialAlphaMode : uint8_t
    {
        Opaque,
        Mask,
        Blend,
    };

    enum class RenderPrimitive2DKind : uint8_t
    {
        Quad,
        Circle,
        Line,
    };

    enum class RenderPrimitive2DCategory : uint8_t
    {
        Scene,
        Debug,
        Editor,
    };

    enum class RenderPrimitiveDepthMode : uint8_t
    {
        DepthTest,
        Overlay,
    };

    enum class RenderPrimitiveTextureFallback : uint8_t
    {
        White,
        Black,
        FlatNormal,
    };

    struct RenderCamera
    {
        Json RenderOverrides = Json::object();
        glm::mat4 View{ 1.0f };
        glm::mat4 Projection{ 1.0f };
        glm::mat4 ViewProjection{ 1.0f };
        glm::mat4 PreviousViewProjection{ 1.0f };
        glm::vec3 Position{ 0.0f };
        float NearPlane = 0.01f;
        float FarPlane = 1000.0f;
        // The current renderer has no temporal jitter, so extraction assigns
        // these to ViewProjection. Temporal techniques consume these fields
        // without assuming that remains true when TAA is introduced.
        glm::mat4 NonJitteredViewProjection{ 1.0f };
        glm::mat4 PreviousNonJitteredViewProjection{ 1.0f };
        uint64_t JitterSequenceGeneration = 1;
        uint64_t ProjectionGeneration = 1;
        bool bCameraCut = false;
    };

    struct RenderBounds
    {
        glm::vec3 Center{ 0.0f };
        float Radius = 0.0f;
        glm::vec3 Extents{ 0.0f };
        bool bValid = false;
    };

    // Owned CPU snapshot consumed by a later render pass. TextureAssetPath is
    // project-root-relative (for example Assets/Textures/a.png); an empty path
    // selects TextureFallback explicitly.
    struct RenderPrimitive2D
    {
        RenderPrimitive2DKind Kind = RenderPrimitive2DKind::Quad;
        RenderPrimitive2DCategory Category = RenderPrimitive2DCategory::Scene;
        RenderPrimitiveDepthMode DepthMode = RenderPrimitiveDepthMode::DepthTest;
        RenderPrimitiveTextureFallback TextureFallback = RenderPrimitiveTextureFallback::White;
        glm::mat4 Transform{ 1.0f };
        glm::vec3 LineStart{ 0.0f };
        glm::vec3 LineEnd{ 0.0f };
        glm::vec4 Color{ 1.0f };
        glm::vec2 UvMin{ 0.0f };
        glm::vec2 UvMax{ 1.0f };
        std::string TextureAssetPath;
        float TilingFactor = 1.0f;
        float Thickness = 1.0f;
        float Fade = 0.005f;
        float LineWidth = 2.0f;
        int EntityId = -1;
        int Layer = 0;
    };

    struct RenderMeshSection
    {
        struct MaterialInstanceSnapshot
        {
            struct TextureReference
            {
                std::string AssetPath;
                TextureSampling Sampling;
                std::string Fallback;
                bool bEnabled = false;
            };

            std::string AssetPath;
            std::string ShaderPath;
            // Manifest identity captured with ParameterValues. The renderer
            // must not combine this snapshot with an incompatible hot-reload
            // generation before the owning Material has migrated its layout.
            std::string ExpectedRecursiveSourceHash;
            std::string ExpectedBindingSignature;
            uint32_t ExpectedMaterialAbiVersion = 0;
            std::string ExpectedArtifactIdentityHash;
            uint64_t ExpectedProgramGeneration = 0;
            uint64_t Revision = 0;
            std::vector<MaterialParameterValue> ParameterValues;
            glm::vec4 BaseColor{ 1.0f };
            glm::vec3 Emissive{ 0.0f };
            float Metallic = 1.0f;
            float Roughness = 1.0f;
            float AoStrength = 1.0f;
            float NormalScale = 1.0f;
            std::array<TextureReference, 5> LegacyTextures;
            RenderMaterialAlphaMode AlphaMode = RenderMaterialAlphaMode::Opaque;
            float AlphaCutoff = 0.5f;
            bool bDoubleSided = false;
            bool bDepthWrite = true;
            bool bCustomSurface = false;
            bool bValid = false;
        };

        Ref<SubMesh> SubMeshResource;
        Ref<Material> MaterialResource;
        // Owned immutable values captured before RenderGraph construction.
        MaterialInstanceSnapshot MaterialInstance;
        // Extraction shares immutable material payloads between instances. The
        // inline value remains available for explicitly authored/test snapshots.
        Ref<const MaterialInstanceSnapshot> SharedMaterialInstance;
        [[nodiscard]] const MaterialInstanceSnapshot& GetMaterialInstance() const noexcept
        {
            return SharedMaterialInstance ? *SharedMaterialInstance : MaterialInstance;
        }
        uint32_t MaterialGpuIndex = InvalidGpuSceneIndex;
        RenderPath Path = RenderPath::CpuOpaque;
        RenderMaterialAlphaMode AlphaMode = RenderMaterialAlphaMode::Opaque;
        float AlphaCutoff = 0.5f;
        bool bDoubleSided = false;
    };

    struct RenderObject
    {
        int EntityId = -1;
        Ref<Mesh> MeshResource;
        std::string MeshAssetPath;
        // Per-instance immutable lightmap snapshot; no ECS access in render passes.
        std::string StaticLightmapPath;
        glm::vec2 StaticLightmapUvScale{ 1.0f };
        glm::vec2 StaticLightmapUvBias{ 0.0f };
        glm::vec4 StaticLightmapScale0{ 1.0f };
        glm::vec4 StaticLightmapAdd0{ 0.0f };
        glm::vec4 StaticLightmapScale1{ 1.0f };
        glm::vec4 StaticLightmapAdd1{ 0.0f };
        float StaticLightmapBlend = 1.0f;
        int StaticLightmapEncoding = 0;
        glm::mat4 WorldTransform{ 1.0f };
        glm::mat4 PreviousWorldTransform{ 1.0f };
        RenderBounds Bounds;
        std::vector<RenderMeshSection> Sections;
        // Immutable animation snapshot. RenderGraph passes never query the
        // AnimatorComponent or mutable Mesh animator state.
        std::vector<glm::mat4> BonePalette;
        // Previous evaluated pose, using the same bone indices as BonePalette.
        // New objects and incompatible/reloaded poses use the current palette.
        std::vector<glm::mat4> PreviousBonePalette;
        uint32_t ObjectGpuIndex = InvalidGpuSceneIndex;
        uint32_t MeshGpuIndex = InvalidGpuSceneIndex;
        RenderPath Path = RenderPath::CpuOpaque;
        bool bVisible = true;
        bool bSelected = false;
        bool bCastShadow = true;
    };

    struct RenderPointLight
    {
        glm::vec3 Position{ 0.0f };
        float Intensity = 0.0f;
        glm::vec3 Color{ 1.0f };
        float Radius = 0.0f;
    };

    struct RenderDirectionalLight
    {
        glm::vec3 Direction{ 0.0f, -1.0f, 0.0f };
        float Intensity = 0.0f;
        glm::vec3 Color{ 1.0f };
        bool bCastShadows = false;
        int CascadeCount = 4;
        int ShadowMapSize = 2048;
        float ShadowDistance = 150.0f;
        float CascadeLambda = 0.75f;
        float DepthBias = 0.0015f;
        float NormalBias = 0.05f;
        float PcfRadius = 1.5f;
        float ShadowStrength = 1.0f;
        bool bUsePcf = true;
    };

    struct RenderSpotLight
    {
        glm::vec3 Position{ 0.0f };
        float Intensity = 0.0f;
        glm::vec3 Color{ 1.0f };
        float Radius = 0.0f;
        glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
        float InnerConeCosine = 0.94f;
        float OuterConeCosine = 0.866f;
    };

    struct RenderReflectionProbe
    {
        int EntityId = -1;
        std::string HdrTexturePath;
        glm::vec3 BoundsMin{ -5.0f };
        glm::vec3 BoundsMax{ 5.0f };
        glm::vec3 CapturePosition{ 0.0f };
        float BlendDistance = 1.0f;
        float Intensity = 1.0f;
        bool bAffectDiffuse = true;
    };

    struct RenderBakedIrradianceVolume
    {
        int EntityId = -1;
        glm::vec3 BoundsMin{ -5.0f };
        glm::vec3 BoundsMax{ 5.0f };
        std::array<glm::vec3, 9> IrradianceSH{};
        float BlendDistance = 1.0f;
        float Intensity = 1.0f;
    };

    struct RenderEnvironment
    {
        std::string HdrTexturePath;
        float Exposure = 1.0f / 16384.0f;
        float IblIntensity = 0.0f;
        float SkyExposureCompensation = 0.0f;
        glm::vec3 SkyColorTint{ 1.0f };
        bool bFogEnabled = false;
        glm::vec3 FogColor{ 0.5f, 0.55f, 0.6f };
        float FogDensity = 0.0f;
        float FogStartDistance = 0.0f;
        float FogBaseHeight = 0.0f;
        float FogHeightFalloff = 0.0f;
        glm::vec3 SecondaryFogColor{ 0.0f };
        float SecondaryFogDensity = 0.0f;
        float SecondaryFogStartDistance = 0.0f;
        float SecondaryFogBaseHeight = 0.0f;
        float SecondaryFogHeightFalloff = 0.0f;
        glm::vec3 FogDirectionalColor{ 1.0f };
        float FogDirectionalIntensity = 0.0f;
        float FogDirectionalExponent = 4.0f;
    };

    struct RenderPostProcessSettings
    {
        float Exposure = 1.0f / 16384.0f;
        float BloomIntensity = 0.05f;
        float Contrast = 1.0f;
        float Saturation = 1.0f;
        float Temperature = 0.0f;
        float VignetteIntensity = 0.0f;
        glm::vec3 ColorGain{ 1.0f };
        glm::vec3 ColorOffset{ 0.0f };
        // View-independent affine Volume blend. The renderer supplies each view's base.
        float BloomBaseWeight = 1.0f;
        float BloomContribution = 0.0f;
        float ContrastBaseWeight = 1.0f;
        float ContrastContribution = 0.0f;
        float SaturationBaseWeight = 1.0f;
        float SaturationContribution = 0.0f;
        float TemperatureBaseWeight = 1.0f;
        float TemperatureContribution = 0.0f;
        float VignetteBaseWeight = 1.0f;
        float VignetteContribution = 0.0f;
        float ColorGainBaseWeight = 1.0f;
        glm::vec3 ColorGainContribution{ 0.0f };
        float ColorOffsetBaseWeight = 1.0f;
        glm::vec3 ColorOffsetContribution{ 0.0f };

        [[nodiscard]] bool operator==(
            const RenderPostProcessSettings&) const = default;
    };

    // Kept by the caller's lease; the scene render system only holds a weak
    // reference so disposing content removes its explicit residency request.
    struct RenderResourceWarmupRequest
    {
        Ref<Mesh> MeshResource;
    };

    struct RenderWorld
    {
        Json RenderOverrides = Json::object();
        RenderCamera Camera;
        std::vector<RenderObject> Objects;
        // Immutable resource-only snapshots: no visibility, shadow, GI or motion contribution.
        std::vector<Ref<const RenderObject>> ResourceWarmupObjects;
        // Distinct lightmap assets from the full scene, including culled meshes.
        // The renderer may prewarm them before measured traversal frames.
        std::vector<std::string> StaticLightmapAssets;
        std::vector<RenderPrimitive2D> Primitives2D;
        std::vector<RenderPointLight> PointLights;
        std::vector<RenderDirectionalLight> DirectionalLights;
        std::vector<RenderSpotLight> SpotLights;
        std::vector<RenderReflectionProbe> ReflectionProbes;
        std::vector<RenderBakedIrradianceVolume> BakedIrradianceVolumes;
        RenderEnvironment Environment;
        RenderCloudLayer Clouds;
        // Final immutable Project -> World -> Volume -> Camera result. Renderer
        // passes consume this value and never query ECS components or settings.
        RenderPostProcessSettings PostProcess;
        uint64_t FrameNumber = 0;
        // Scene reload identity is distinct from ordinary object transforms;
        // moving objects rely on motion/disocclusion instead of resetting GI.
        uint64_t SceneGeneration = 1;
        // Monotonic presentation time in seconds, captured with this snapshot.
        double TimeSeconds = 0.0;
        bool bEditorView = false;

        void ClearFrameData()
        {
            Objects.clear();
            ResourceWarmupObjects.clear();
            StaticLightmapAssets.clear();
            Primitives2D.clear();
            PointLights.clear();
            DirectionalLights.clear();
            SpotLights.clear();
            ReflectionProbes.clear();
            BakedIrradianceVolumes.clear();
            Environment = {};
            Clouds = {};
            PostProcess = {};
        }
    };
}
