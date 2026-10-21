#include "Runtime/Core/Plugins/PluginApi.h"
#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSession.h"
#include "Runtime/Graphics/RHI/ShaderArtifact.h"
#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <mutex>
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
            struct CachedSupport
            {
                std::filesystem::path Root;
                std::chrono::steady_clock::time_point Checked{};
                OfflineViewportRendererSupport Support;
            };
            static std::mutex cacheMutex;
            static std::array<CachedSupport, 2> cached;
            const std::scoped_lock lock(cacheMutex);
            auto& cache = cached[capabilities.Backend == RenderBackend::Vulkan ? 1 : 0];
            const auto now = std::chrono::steady_clock::now();
            if (cache.Root == shaderArtifactCacheRoot && now - cache.Checked < std::chrono::seconds(1))
                return cache.Support;
            const auto remember = [&](OfflineViewportRendererSupport support)
            {
                cache = {shaderArtifactCacheRoot, now, support};
                return support;
            };
            constexpr std::array requiredArtifacts = {
                "HardwareRayTracingReference__lib_6_5.lib",
                "OfflineReferencePreview_FLOAT4.cs",
                "OfflineReferencePreview_SCALAR.cs",
                "OfflineReferencePreview_UINT.cs",
                "OfflineReferencePreview_TONEMAP.cs",
            };
            const std::filesystem::path dxilDirectory = shaderArtifactCacheRoot /
                (capabilities.Backend == RenderBackend::Vulkan ? "SPIRV" : "DXIL");
            for (const char* artifactName : requiredArtifacts)
            {
                const std::filesystem::path artifactPath =
                    dxilDirectory / artifactName;
                if (!std::filesystem::is_regular_file(artifactPath) ||
                    !std::filesystem::is_regular_file(
                        MakeShaderCacheKeyPath(artifactPath)))
                {
                    return remember({
                        false,
                        "Offline Reference Renderer shader artifact is missing: " +
                            std::string(artifactName),
                    });
                }
            }
            return remember({
                true,
                "Static opaque/masked BasePBR triangles, smooth normals, base color/normal/metallic/roughness/emission textures, "
                "GGX reflection, directional/point/spot lights, emissive mesh and environment NEE/MIS. "
                "Alpha blend, transmission, custom shaders, skinning, VFX and volumes remain unsupported. "
                "Up to 512 material textures and 256 analytic lights; reference textures use mip zero. "
                "Denoising availability is reported by the session.",
            });
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
                            .bBasePbrMaterials = true,
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
