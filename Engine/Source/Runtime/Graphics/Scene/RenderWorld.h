#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/Material/MaterialParameterPacking.h"

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
        glm::mat4 WorldTransform{ 1.0f };
        glm::mat4 PreviousWorldTransform{ 1.0f };
        RenderBounds Bounds;
        std::vector<RenderMeshSection> Sections;
        // Immutable animation snapshot. RenderGraph passes never query the
        // AnimatorComponent or mutable Mesh animator state.
        std::vector<glm::mat4> BonePalette;
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
        float FalloffExponent = 2.0f;
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

    struct RenderEnvironment
    {
        std::string HdrTexturePath;
        float Exposure = 0.8f;
        float IblIntensity = 0.0f;
    };

    struct RenderPostProcessSettings
    {
        float Exposure = 0.8f;

        [[nodiscard]] bool operator==(
            const RenderPostProcessSettings&) const = default;
    };

    struct RenderWorld
    {
        RenderCamera Camera;
        std::vector<RenderObject> Objects;
        std::vector<RenderPrimitive2D> Primitives2D;
        std::vector<RenderPointLight> PointLights;
        std::vector<RenderDirectionalLight> DirectionalLights;
        RenderEnvironment Environment;
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
            Primitives2D.clear();
            PointLights.clear();
            DirectionalLights.clear();
            Environment = {};
            PostProcess = {};
        }
    };
}
