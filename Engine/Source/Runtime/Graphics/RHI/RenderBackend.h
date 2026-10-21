#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace HEngine
{
    enum class RenderBackend : uint8_t
    {
        DX12,
        Vulkan,
    };

    [[nodiscard]] const char* ToString(RenderBackend backend);
    [[nodiscard]] RenderBackend ParseRenderBackend(std::string_view name);
    [[nodiscard]] bool IsRenderBackendCompiled(RenderBackend backend) noexcept;
    [[nodiscard]] std::string GetRenderBackendAvailabilityReason(RenderBackend backend);

    enum class RendererProfile : uint8_t
    {
        BaselineRaster,
        ModernRaster,
        // Reserved for the future renderer registration point. Capability
        // detection alone never selects this profile.
        HardwareRayTracing,
    };

    [[nodiscard]] const char* ToString(RendererProfile profile);

    struct RenderAdapterSelectionOptions
    {
        // The index follows DXGI's high-performance preference order.
        std::optional<uint32_t> GpuIndex;
        std::optional<uint64_t> AdapterLuid;
        bool bUseWarp = false;
    };

    struct RenderFeatureStatus
    {
        bool bSupported = false;
        // Enabled means that device creation exposed the feature to renderer
        // path selection. It does not claim that an effect using it was drawn.
        bool bEnabled = false;
        std::string Reason;
    };

    struct RenderCapabilities
    {
        RenderBackend Backend = RenderBackend::DX12;
        std::string AdapterName;
        std::string AdapterDriverVersion;
        std::string AdapterUuid;
        uint32_t AdapterIndex = 0;
        uint32_t AdapterVendorId = 0;
        uint32_t AdapterDeviceId = 0;
        uint64_t AdapterLuid = 0;
        uint64_t DedicatedVideoMemory = 0;
        bool bSoftwareAdapter = false;

        // Vulkan-native observations are kept as data-only diagnostics so no
        // native Vulkan types cross the Runtime/Platform boundary.
        uint32_t VulkanApiVersion = 0;
        uint32_t VulkanDriverVersion = 0;
        bool bVulkanTimelineSemaphore = false;
        bool bVulkanDynamicRendering = false;
        bool bVulkanSynchronization2 = false;
        bool bVulkanScalarBlockLayout = false;
        bool bVulkanSampledImageArrayNonUniformIndexing = false;
        uint32_t VulkanMaxPerStageSampledImages = 0;
        uint32_t VulkanMaxPerStageSamplers = 0;
        uint32_t VulkanMaxBoundDescriptorSets = 0;

        // Bit N means that N raster samples are supported for the engine's
        // RGBA16_FLOAT scene color and D32 scene depth pair. Bit 1 is always set.
        uint32_t SupportedMsaaSampleMask = 1u << 1u;

        // GPU Scene uses first-instance addressing and optional counted batches.
        bool bDrawIndirectFirstInstance = false;
        bool bMultiDrawIndirect = false;
        bool bDrawIndirectCount = false;

        // Backend-neutral NVRHI feature observations. Native API feature
        // levels and tier diagnostics remain inside the platform backend.
        bool bNvrhiRayTracingAccelStruct = false;
        bool bNvrhiRayTracingPipeline = false;
        bool bNvrhiRayQuery = false;
        bool bNvrhiMeshlets = false;
        bool bNvrhiVariableRateShading = false;
        bool bNvrhiHeapDirectlyIndexed = false;
        bool bNvrhiEnhancedBarriers = false;
        bool bNvrhiShaderExecutionReordering = false;
        bool bNvrhiRayTracingOpacityMicromap = false;
        bool bNvrhiNvapiBuildSupport = false;
        bool bNvrhiNativeOpacityMicromapBuildSupport = false;
        bool bHardwareRayTracingRendererReady = false;

        RendererProfile Profile = RendererProfile::BaselineRaster;
        RenderFeatureStatus BaselineRaster;
        RenderFeatureStatus ShaderModel6_6;
        RenderFeatureStatus ResourceBindingTier3;
        RenderFeatureStatus HeapDirectlyIndexed;
        RenderFeatureStatus EnhancedBarriers;
        RenderFeatureStatus RayTracingAccelStruct;
        RenderFeatureStatus RayTracingPipeline;
        RenderFeatureStatus RayQuery;
        RenderFeatureStatus MeshShader;
        RenderFeatureStatus VariableRateShading;
        RenderFeatureStatus SamplerFeedback;
        RenderFeatureStatus ShaderExecutionReordering;
        RenderFeatureStatus RayTracingOpacityMicromap;
        RenderFeatureStatus HardwareRayTracingRendererPath;

        bool bGraphicsQueue = false;
        bool bComputeQueue = false;
        bool bCopyQueue = false;
        bool bValidationLayer = false;
        uint32_t FramesInFlight = 0;
    };

    [[nodiscard]] inline bool SupportsCountedIndirectDraws(const RenderCapabilities& capabilities) noexcept
    {
        return capabilities.bDrawIndirectFirstInstance && capabilities.bMultiDrawIndirect &&
            capabilities.bDrawIndirectCount;
    }

    // Pure policy resolution for synthetic/headless capability contracts.
    // Hardware-ray-tracing readiness is reported independently until a
    // renderer technique is registered.
    void ResolveRendererCapabilityPolicy(
        RenderCapabilities& capabilities,
        bool bHardwareRayTracingRendererRegistered = false);

    // Runs without creating a window or swap chain and throws on probe failure.
    void RunRenderCapabilityProbe(
        RenderBackend backend,
        const std::filesystem::path& outputPath,
        const RenderAdapterSelectionOptions& selection = {});
}
