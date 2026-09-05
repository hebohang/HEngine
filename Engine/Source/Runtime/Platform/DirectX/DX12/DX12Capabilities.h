#pragma once

#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <nvrhi/d3d12.h>

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace HEngine
{
    struct DX12AdapterSelectionResult
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter4> Adapter;
        DXGI_ADAPTER_DESC3 Description = {};
        uint32_t PreferenceIndex = 0;
        std::string DriverVersion;
    };

    // DX12-only diagnostics and device-creation switches. These never cross
    // the platform boundary through RenderCapabilities.
    struct DX12NativeCapabilities
    {
        uint32_t FeatureLevel = 0;
        uint32_t ShaderModel = 0;
        uint32_t RootSignatureVersion = 0;
        uint32_t ResourceBindingTier = 0;
        uint32_t RayTracingTier = 0;
        uint32_t MeshShaderTier = 0;
        uint32_t VariableRateShadingTier = 0;
        uint32_t SamplerFeedbackTier = 0;
        bool bHeapDirectlyIndexed = false;
        bool bEnhancedBarriers = false;
    };

    void RequireDX12OperatingSystemBaseline();

    [[nodiscard]] DX12AdapterSelectionResult SelectDX12Adapter(
        IDXGIFactory6* factory,
        const RenderAdapterSelectionOptions& selection);

    void QueryNativeDX12Capabilities(
        ID3D12Device* device,
        const DX12AdapterSelectionResult& adapter,
        RenderCapabilities& capabilities,
        DX12NativeCapabilities& nativeCapabilities);

    void ConfigureNvrhiDX12Capabilities(
        const DX12NativeCapabilities& nativeCapabilities,
        nvrhi::d3d12::DeviceDesc& deviceDescription);

    void QueryNvrhiDX12Capabilities(
        nvrhi::IDevice* device,
        RenderCapabilities& capabilities);

    void RequireBaselineDX12Capabilities(
        const DX12NativeCapabilities& nativeCapabilities);
    void LogDX12Capabilities(
        const RenderCapabilities& capabilities,
        const DX12NativeCapabilities& nativeCapabilities);
    void RunDX12CapabilityProbe(
        const std::filesystem::path& outputPath,
        const RenderAdapterSelectionOptions& selection = {});
}
