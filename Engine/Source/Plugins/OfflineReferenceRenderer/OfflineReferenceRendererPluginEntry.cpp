#include "Runtime/Core/Plugins/PluginApi.h"
#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSession.h"
#include "Runtime/Graphics/RHI/ShaderArtifact.h"
#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <array>
#include <filesystem>
#include <string>

namespace HEngine
{
    namespace
    {
        constexpr const char* c_OfflineReferenceProviderId =
            "he.viewport.path-tracing";

        [[nodiscard]] Scope<IOfflineViewportRendererSession>
            CreateOfflineReferenceSession(
                const OfflineViewportRendererCreateContext& context,
                const std::filesystem::path& offlineSettingsPath)
        {
            return CreateScope<ReferencePathTracer>(ReferencePathTracerDesc{
                .Device = context.Device,
                .SharedShaderManager = context.SharedShaderManager,
                .Scene = context.Scene,
                .ShaderArtifactCacheRoot = context.ShaderArtifactCacheRoot,
                .OfflineSettingsPath = offlineSettingsPath,
                .Backend = context.Backend,
                .FramesInFlight = context.FramesInFlight,
                .bOfflineViewportSession = true,
            });
        }

        [[nodiscard]] OfflineViewportRendererSupport QueryOfflineSupport(
            const RenderCapabilities& capabilities,
            const std::filesystem::path& shaderArtifactCacheRoot)
        {
            if (!capabilities.RayTracingAccelStruct.bSupported ||
                !capabilities.RayTracingAccelStruct.bEnabled ||
                !capabilities.RayTracingPipeline.bSupported ||
                !capabilities.RayTracingPipeline.bEnabled)
            {
                return {
                    false,
                    "Hardware ray-tracing acceleration structures and pipelines are unavailable.",
                };
            }
            constexpr std::array requiredArtifacts = {
                "HardwareRayTracingReference__lib_6_5.lib",
                "OfflineReferencePreview_FLOAT4.cs",
                "OfflineReferencePreview_SCALAR.cs",
                "OfflineReferencePreview_UINT.cs",
                "OfflineReferencePreview_TONEMAP.cs",
            };
            const std::filesystem::path dxilDirectory =
                shaderArtifactCacheRoot / "DXIL";
            for (const char* artifactName : requiredArtifacts)
            {
                const std::filesystem::path artifactPath =
                    dxilDirectory / artifactName;
                if (!std::filesystem::is_regular_file(artifactPath) ||
                    !std::filesystem::is_regular_file(
                        MakeShaderCacheKeyPath(artifactPath)))
                {
                    return {
                        false,
                        "Offline Reference Renderer shader artifact is missing: " +
                            std::string(artifactName),
                    };
                }
            }
            return {
                true,
                "Available for static opaque triangles with constant Base Color, "
                "Metalness diffuse suppression, Emissive, interpolated vertex normals, "
                "and scene-environment miss lighting. Roughness/GGX specular, material "
                "textures and normal maps, Mask/Blend alpha, skinned meshes, VFX, "
                "volumes, and motion blur are not reference-supported yet.",
            };
        }

        class OfflineReferenceRendererPluginModule final : public IPluginModule
        {
        public:
            void Start(PluginContext& context) override
            {
                const std::filesystem::path cacheRoot =
                    ConfigManager::GetEngineGeneratedContentFullPath(
                        "Plugins/OfflineReferenceRenderer/Shaders/CACHES");
                const std::filesystem::path offlineSettingsPath =
                    HasPluginHost(
                        context.GetEnvironment().Host,
                        PluginHost::Editor)
                    ? ConfigManager::GetProjectFullPath(
                        "Config/DevOnly/OfflineReferenceRenderer.json")
                    : std::filesystem::path{};
                mProviderRegistration =
                    OfflineViewportRendererProviderRegistry::Get().Register(
                        OfflineViewportRendererProviderDescriptor{
                            .Id = c_OfflineReferenceProviderId,
                            .DisplayName = "Path Tracing",
                            .OwnerPluginId = "he.offline-reference-renderer",
                            .SortOrder = 100,
                            .CapabilityRequirements =
                                GlobalIlluminationTechniqueCapabilityRequirements::
                                    GraphicsQueue |
                                GlobalIlluminationTechniqueCapabilityRequirements::
                                    RayTracingAccelerationStructure |
                                GlobalIlluminationTechniqueCapabilityRequirements::
                                    RayTracingPipeline,
                            .RequiredInputs =
                                GlobalIlluminationTechniqueRequiredInput::SceneTlas |
                                GlobalIlluminationTechniqueRequiredInput::
                                    RayTracingVertexBuffer |
                                GlobalIlluminationTechniqueRequiredInput::
                                    RayTracingIndexBuffer |
                                GlobalIlluminationTechniqueRequiredInput::
                                    RayTracingInstanceBuffer |
                                GlobalIlluminationTechniqueRequiredInput::MaterialBuffer |
                                GlobalIlluminationTechniqueRequiredInput::GeometryBuffer |
                                GlobalIlluminationTechniqueRequiredInput::LightBuffer,
                            .OutputSemantics = {
                                .Outputs =
                                    GlobalIlluminationTechniqueOutput::
                                        DiffuseIndirectRadiance |
                                    GlobalIlluminationTechniqueOutput::
                                        DirectDiffuseRadiance |
                                    GlobalIlluminationTechniqueOutput::HistoryLength,
                            },
                            .SupportedDebugViews = {
                                GlobalIlluminationDebugView::Composite,
                                GlobalIlluminationDebugView::RawDirectDiffuse,
                                GlobalIlluminationDebugView::RawIndirectDiffuse,
                                GlobalIlluminationDebugView::Albedo,
                                GlobalIlluminationDebugView::Normal,
                                GlobalIlluminationDebugView::HitDistance,
                                GlobalIlluminationDebugView::HistoryLength,
                            },
                            .ShaderArtifactCacheRoot = cacheRoot,
                            .GpuTimingPassName = "ReferencePathTrace",
                            .QuerySupport = &QueryOfflineSupport,
                            .Factory = [offlineSettingsPath](
                                const OfflineViewportRendererCreateContext&
                                    createContext)
                            {
                                return CreateOfflineReferenceSession(
                                    createContext,
                                    offlineSettingsPath);
                            },
                        });
            }

            void Stop(PluginContext&) override
            {
                mProviderRegistration.Release();
            }

        private:
            OfflineViewportRendererProviderRegistration mProviderRegistration;
        };
    }

    Scope<IPluginModule> HEngineCreateOfflineReferenceRendererPluginModule()
    {
        return CreateScope<OfflineReferenceRendererPluginModule>();
    }
}
