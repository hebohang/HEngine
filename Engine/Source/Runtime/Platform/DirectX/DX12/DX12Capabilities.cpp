#include "Hepch.h"

#include "DX12Capabilities.h"

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <nvrhi/nvrhi.h>

#include <Windows.h>
#include <winternl.h>

#include <array>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef HE_NVRHI_NVAPI_ENABLED
#define HE_NVRHI_NVAPI_ENABLED 0
#endif

#ifndef HE_NVRHI_NATIVE_OMM_ENABLED
#define HE_NVRHI_NATIVE_OMM_ENABLED 0
#endif

namespace HEngine
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr std::array c_ReportedMsaaSampleCounts{ 1u, 2u, 4u, 8u };
        constexpr std::array c_QueriedMsaaSampleCounts{ 2u, 4u, 8u };

        [[nodiscard]] constexpr uint32_t MsaaSampleBit(uint32_t sampleCount) noexcept
        {
            return 1u << sampleCount;
        }

        [[nodiscard]] bool SupportsMsaaFormat(
            ID3D12Device* device,
            DXGI_FORMAT format,
            uint32_t sampleCount) noexcept
        {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS query = {};
            query.Format = format;
            query.SampleCount = sampleCount;
            query.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
            const HRESULT result = device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                &query,
                sizeof(query));
            // NumQualityLevels > 0 guarantees that the engine's quality index 0
            // is valid. A failed optional query only disables that sample count.
            return SUCCEEDED(result) && query.NumQualityLevels > 0;
        }

        [[nodiscard]] uint32_t QuerySceneMsaaSampleMask(ID3D12Device* device) noexcept
        {
            uint32_t mask = MsaaSampleBit(1u);
            for (const uint32_t sampleCount : c_QueriedMsaaSampleCounts)
            {
                const bool bColorSupported = SupportsMsaaFormat(
                    device,
                    DXGI_FORMAT_R16G16B16A16_FLOAT,
                    sampleCount);
                const bool bDepthSupported = SupportsMsaaFormat(
                    device,
                    DXGI_FORMAT_D32_FLOAT,
                    sampleCount);
                if (bColorSupported && bDepthSupported)
                    mask |= MsaaSampleBit(sampleCount);
            }
            return mask;
        }

        [[nodiscard]] Json SupportedMsaaSamplesToJson(uint32_t mask)
        {
            Json samples = Json::array();
            for (const uint32_t sampleCount : c_ReportedMsaaSampleCounts)
            {
                if ((mask & MsaaSampleBit(sampleCount)) != 0)
                    samples.push_back(sampleCount);
            }
            return samples;
        }

        [[nodiscard]] std::string DescribeSupportedMsaaSamples(uint32_t mask)
        {
            std::string result;
            for (const uint32_t sampleCount : c_ReportedMsaaSampleCounts)
            {
                if ((mask & MsaaSampleBit(sampleCount)) == 0)
                    continue;
                if (!result.empty())
                    result += ',';
                result += std::to_string(sampleCount) + 'x';
            }
            return result;
        }

        [[nodiscard]] std::string HResultToString(HRESULT result)
        {
            std::ostringstream stream;
            stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<uint32_t>(result);

            char* message = nullptr;
            const DWORD messageLength = FormatMessageA(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,
                result,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                reinterpret_cast<char*>(&message),
                0,
                nullptr);
            if (messageLength != 0 && message != nullptr)
            {
                std::string text(message, messageLength);
                while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
                    text.pop_back();
                stream << " (" << text << ')';
            }
            if (message != nullptr)
                LocalFree(message);
            return stream.str();
        }

        void CheckHResult(std::string_view operation, HRESULT result)
        {
            if (FAILED(result))
            {
                throw std::runtime_error(
                    std::string(operation) + " failed with " + HResultToString(result));
            }
        }

        [[nodiscard]] std::string WideToUtf8(const wchar_t* text)
        {
            if (text == nullptr || text[0] == L'\0')
                return {};

            const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (length <= 1)
                return {};

            std::string result(static_cast<size_t>(length), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
            result.pop_back();
            return result;
        }

        [[nodiscard]] uint64_t PackLuid(LUID luid)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32u) |
                static_cast<uint32_t>(luid.LowPart);
        }

        [[nodiscard]] std::string LuidToString(uint64_t luid)
        {
            std::ostringstream stream;
            stream << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << luid;
            return stream.str();
        }

        [[nodiscard]] std::string QueryDriverVersion(IDXGIAdapter* adapter)
        {
            LARGE_INTEGER version = {};
            const HRESULT result = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version);
            if (FAILED(result))
                return "unavailable (" + HResultToString(result) + ')';

            const uint32_t high = static_cast<uint32_t>(version.HighPart);
            const uint32_t low = version.LowPart;
            return std::to_string(HIWORD(high)) + '.' +
                std::to_string(LOWORD(high)) + '.' +
                std::to_string(HIWORD(low)) + '.' +
                std::to_string(LOWORD(low));
        }

        [[nodiscard]] DX12AdapterSelectionResult DescribeAdapter(
            IDXGIAdapter1* candidate,
            uint32_t preferenceIndex)
        {
            DX12AdapterSelectionResult result;
            CheckHResult(
                "IDXGIAdapter1::QueryInterface(IDXGIAdapter4)",
                candidate->QueryInterface(IID_PPV_ARGS(&result.Adapter)));
            CheckHResult("IDXGIAdapter4::GetDesc3", result.Adapter->GetDesc3(&result.Description));
            result.PreferenceIndex = preferenceIndex;
            result.DriverVersion = QueryDriverVersion(candidate);
            return result;
        }

        void LogAdapterCandidate(
            const DX12AdapterSelectionResult& candidate,
            std::string_view result)
        {
            HE_CORE_INFO(
                "DX12 adapter candidate [{0}]: {1}; VendorId=0x{2:04X}, DeviceId=0x{3:04X}, "
                "LUID={4}, dedicated VRAM={5} MiB, driver={6}; FL11_0 probe: {7}",
                candidate.PreferenceIndex,
                WideToUtf8(candidate.Description.Description),
                candidate.Description.VendorId,
                candidate.Description.DeviceId,
                LuidToString(PackLuid(candidate.Description.AdapterLuid)),
                candidate.Description.DedicatedVideoMemory / (1024ull * 1024ull),
                candidate.DriverVersion,
                result);
        }

        [[nodiscard]] bool MatchesSelection(
            const DX12AdapterSelectionResult& candidate,
            const RenderAdapterSelectionOptions& selection)
        {
            if (selection.GpuIndex && candidate.PreferenceIndex != *selection.GpuIndex)
                return false;
            if (selection.AdapterLuid &&
                PackLuid(candidate.Description.AdapterLuid) != *selection.AdapterLuid)
            {
                return false;
            }
            return true;
        }

        [[nodiscard]] const char* FeatureLevelToString(uint32_t value)
        {
            switch (static_cast<D3D_FEATURE_LEVEL>(value))
            {
            case D3D_FEATURE_LEVEL_11_0: return "11_0";
            case D3D_FEATURE_LEVEL_11_1: return "11_1";
            case D3D_FEATURE_LEVEL_12_0: return "12_0";
            case D3D_FEATURE_LEVEL_12_1: return "12_1";
            case D3D_FEATURE_LEVEL_12_2: return "12_2";
            default: return "unknown";
            }
        }

        [[nodiscard]] std::string ShaderModelToString(uint32_t value)
        {
            std::ostringstream stream;
            stream << ((value >> 4u) & 0x0fu) << '_' << (value & 0x0fu);
            return stream.str();
        }

        [[nodiscard]] const char* RootSignatureVersionToString(uint32_t value)
        {
            switch (static_cast<D3D_ROOT_SIGNATURE_VERSION>(value))
            {
            case D3D_ROOT_SIGNATURE_VERSION_1_0: return "1_0";
            case D3D_ROOT_SIGNATURE_VERSION_1_1: return "1_1";
            default: return "unknown";
            }
        }

        [[nodiscard]] std::string TierToString(uint32_t value)
        {
            return value == 0 ? "not supported" : std::to_string(value);
        }

        [[nodiscard]] std::string RayTracingTierToString(uint32_t value)
        {
            switch (static_cast<D3D12_RAYTRACING_TIER>(value))
            {
            case D3D12_RAYTRACING_TIER_NOT_SUPPORTED: return "not supported";
            case D3D12_RAYTRACING_TIER_1_0: return "1_0";
            case D3D12_RAYTRACING_TIER_1_1: return "1_1";
            default: return std::to_string(value);
            }
        }

        [[nodiscard]] std::string MeshShaderTierToString(uint32_t value)
        {
            switch (static_cast<D3D12_MESH_SHADER_TIER>(value))
            {
            case D3D12_MESH_SHADER_TIER_NOT_SUPPORTED: return "not supported";
            case D3D12_MESH_SHADER_TIER_1: return "1_0";
            default: return std::to_string(value);
            }
        }

        [[nodiscard]] std::string SamplerFeedbackTierToString(uint32_t value)
        {
            switch (static_cast<D3D12_SAMPLER_FEEDBACK_TIER>(value))
            {
            case D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED: return "not supported";
            case D3D12_SAMPLER_FEEDBACK_TIER_0_9: return "0_9";
            case D3D12_SAMPLER_FEEDBACK_TIER_1_0: return "1_0";
            default: return std::to_string(value);
            }
        }

        void EnableNativeFeature(RenderFeatureStatus& status, std::string reason)
        {
            status.bSupported = true;
            status.bEnabled = true;
            status.Reason = std::move(reason);
        }

        void DisableFeature(RenderFeatureStatus& status, std::string reason)
        {
            status.bSupported = false;
            status.bEnabled = false;
            status.Reason = std::move(reason);
        }

        void FinalizeNvrhiFeature(
            RenderFeatureStatus& status,
            bool bNvrhiSupported,
            std::string_view featureName,
            std::string_view fallback)
        {
            if (!status.bSupported)
            {
                status.bEnabled = false;
                return;
            }

            status.bEnabled = bNvrhiSupported;
            if (bNvrhiSupported)
            {
                status.Reason = std::string(featureName) +
                    " is supported by both D3D12 and NVRHI and is available to renderer path selection.";
            }
            else
            {
                status.Reason = std::string(featureName) +
                    " is reported by D3D12 but not exposed by the active NVRHI device; " +
                    std::string(fallback);
            }
        }

        [[nodiscard]] Json FeatureStatusToJson(const RenderFeatureStatus& status)
        {
            Json result;
            result["supported"] = status.bSupported;
            result["enabled"] = status.bEnabled;
            result["reason"] = status.Reason;
            return result;
        }

        struct WindowsVersionInformation
        {
            uint32_t Major = 0;
            uint32_t Minor = 0;
            uint32_t Build = 0;
            uint32_t Revision = 0;
            std::string QueryError;
            bool bAvailable = false;
            bool bRevisionAvailable = false;
        };

        [[nodiscard]] WindowsVersionInformation QueryWindowsVersion()
        {
            WindowsVersionInformation result;
            using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
            const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            const auto rtlGetVersion = ntdll == nullptr ? nullptr :
                reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion"));
            if (rtlGetVersion == nullptr)
            {
                result.QueryError = "RtlGetVersion unavailable";
                return result;
            }

            RTL_OSVERSIONINFOW version = {};
            version.dwOSVersionInfoSize = sizeof(version);
            const LONG status = rtlGetVersion(&version);
            if (status != 0)
            {
                result.QueryError = "RtlGetVersion failed with NTSTATUS " + std::to_string(status);
                return result;
            }

            result.Major = version.dwMajorVersion;
            result.Minor = version.dwMinorVersion;
            result.Build = version.dwBuildNumber;
            result.bAvailable = true;

            DWORD revision = 0;
            DWORD revisionSize = sizeof(revision);
            const LSTATUS revisionStatus = RegGetValueW(
                HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                L"UBR",
                RRF_RT_REG_DWORD,
                nullptr,
                &revision,
                &revisionSize);
            if (revisionStatus == ERROR_SUCCESS)
            {
                result.Revision = revision;
                result.bRevisionAvailable = true;
            }
            return result;
        }

        [[nodiscard]] Json QueryOsInformation()
        {
            Json result;
#if defined(_M_ARM64)
            result["architecture"] = "arm64";
#else
            result["architecture"] = "x64";
#endif
            const WindowsVersionInformation version = QueryWindowsVersion();
            if (!version.bAvailable)
            {
                result["name"] = "Windows";
                result["versionQuery"] = version.QueryError;
                return result;
            }

            result["name"] = version.Build >= 22000 ? "Windows 11" : "Windows 10";
            result["major"] = version.Major;
            result["minor"] = version.Minor;
            result["build"] = version.Build;
            if (version.bRevisionAvailable)
                result["revision"] = version.Revision;
            else
                result["revisionQuery"] = "Windows UBR registry value unavailable";
            return result;
        }

        [[nodiscard]] Json CapabilitiesToJson(
            const RenderCapabilities& capabilities,
            const DX12NativeCapabilities& nativeCapabilities)
        {
            Json document;
            document["schemaVersion"] = 1;
            document["backend"] = "DX12/NVRHI";
            document["os"] = QueryOsInformation();
            document["adapter"] = {
                { "name", capabilities.AdapterName },
                { "highPerformancePreferenceIndex", capabilities.AdapterIndex },
                { "vendorId", capabilities.AdapterVendorId },
                { "deviceId", capabilities.AdapterDeviceId },
                { "luid", LuidToString(capabilities.AdapterLuid) },
                { "dedicatedVideoMemoryBytes", capabilities.DedicatedVideoMemory },
                { "driverVersion", capabilities.AdapterDriverVersion },
                { "software", capabilities.bSoftwareAdapter },
            };
            document["d3d12"] = {
                { "deviceCreationMinimumFeatureLevel", "11_0" },
                { "maximumFeatureLevel", FeatureLevelToString(nativeCapabilities.FeatureLevel) },
                { "shaderModel", ShaderModelToString(nativeCapabilities.ShaderModel) },
                { "rootSignatureVersion", RootSignatureVersionToString(nativeCapabilities.RootSignatureVersion) },
                { "resourceBindingTier", TierToString(nativeCapabilities.ResourceBindingTier) },
                { "rayTracingTier", RayTracingTierToString(nativeCapabilities.RayTracingTier) },
                { "meshShaderTier", MeshShaderTierToString(nativeCapabilities.MeshShaderTier) },
                { "variableRateShadingTier", TierToString(nativeCapabilities.VariableRateShadingTier) },
                { "samplerFeedbackTier", SamplerFeedbackTierToString(nativeCapabilities.SamplerFeedbackTier) },
                { "heapDirectlyIndexed", nativeCapabilities.bHeapDirectlyIndexed },
                { "enhancedBarriers", nativeCapabilities.bEnhancedBarriers },
                { "supportedSceneMsaaSampleMask", capabilities.SupportedMsaaSampleMask },
                { "drawIndirectFirstInstance", capabilities.bDrawIndirectFirstInstance },
                { "multiDrawIndirect", capabilities.bMultiDrawIndirect },
                { "drawIndirectCount", capabilities.bDrawIndirectCount },
                { "supportedSceneMsaaSamples", SupportedMsaaSamplesToJson(
                    capabilities.SupportedMsaaSampleMask) },
            };
            document["nvrhi"] = {
                { "RayTracingAccelStruct", capabilities.bNvrhiRayTracingAccelStruct },
                { "RayTracingPipeline", capabilities.bNvrhiRayTracingPipeline },
                { "RayQuery", capabilities.bNvrhiRayQuery },
                { "Meshlets", capabilities.bNvrhiMeshlets },
                { "VariableRateShading", capabilities.bNvrhiVariableRateShading },
                { "HeapDirectlyIndexed", capabilities.bNvrhiHeapDirectlyIndexed },
                { "EnhancedBarriers", capabilities.bNvrhiEnhancedBarriers },
                { "ShaderExecutionReordering", capabilities.bNvrhiShaderExecutionReordering },
                { "RayTracingOpacityMicromap", capabilities.bNvrhiRayTracingOpacityMicromap },
            };
            document["nvrhiBuildSupport"] = {
                { "NVAPI", capabilities.bNvrhiNvapiBuildSupport },
                { "NativeOpacityMicromap", capabilities.bNvrhiNativeOpacityMicromapBuildSupport },
            };
            document["queues"] = {
                { "graphics", capabilities.bGraphicsQueue },
                { "compute", capabilities.bComputeQueue },
                { "copy", capabilities.bCopyQueue },
            };
            document["rendererProfile"] = ToString(capabilities.Profile);
            document["rendererProfileNote"] =
                "The selected profile is raster-only unless a hardware-ray-tracing technique is explicitly registered.";
            document["hardwareRayTracingReadiness"] = {
                { "ready", capabilities.bHardwareRayTracingRendererReady },
                { "pathEnabled", capabilities.HardwareRayTracingRendererPath.bEnabled },
                { "reason", capabilities.HardwareRayTracingRendererPath.Reason },
            };

            Json& features = document["features"];
            features["BaselineRaster"] = FeatureStatusToJson(capabilities.BaselineRaster);
            features["ShaderModel6_6"] = FeatureStatusToJson(capabilities.ShaderModel6_6);
            features["ResourceBindingTier3"] = FeatureStatusToJson(capabilities.ResourceBindingTier3);
            features["HeapDirectlyIndexed"] = FeatureStatusToJson(capabilities.HeapDirectlyIndexed);
            features["EnhancedBarriers"] = FeatureStatusToJson(capabilities.EnhancedBarriers);
            features["RayTracingAccelStruct"] = FeatureStatusToJson(capabilities.RayTracingAccelStruct);
            features["RayTracingPipeline"] = FeatureStatusToJson(capabilities.RayTracingPipeline);
            features["RayQuery"] = FeatureStatusToJson(capabilities.RayQuery);
            features["MeshShader"] = FeatureStatusToJson(capabilities.MeshShader);
            features["VariableRateShading"] = FeatureStatusToJson(capabilities.VariableRateShading);
            features["SamplerFeedback"] = FeatureStatusToJson(capabilities.SamplerFeedback);
            features["ShaderExecutionReordering"] = FeatureStatusToJson(capabilities.ShaderExecutionReordering);
            features["RayTracingOpacityMicromap"] = FeatureStatusToJson(capabilities.RayTracingOpacityMicromap);
            features["HardwareRayTracingRendererPath"] =
                FeatureStatusToJson(capabilities.HardwareRayTracingRendererPath);

            document["degradations"] = Json::array();
            const std::array<std::pair<const char*, const RenderFeatureStatus*>, 13> advancedFeatures = {{
                { "ShaderModel6_6", &capabilities.ShaderModel6_6 },
                { "ResourceBindingTier3", &capabilities.ResourceBindingTier3 },
                { "HeapDirectlyIndexed", &capabilities.HeapDirectlyIndexed },
                { "EnhancedBarriers", &capabilities.EnhancedBarriers },
                { "RayTracingAccelStruct", &capabilities.RayTracingAccelStruct },
                { "RayTracingPipeline", &capabilities.RayTracingPipeline },
                { "RayQuery", &capabilities.RayQuery },
                { "MeshShader", &capabilities.MeshShader },
                { "VariableRateShading", &capabilities.VariableRateShading },
                { "SamplerFeedback", &capabilities.SamplerFeedback },
                { "ShaderExecutionReordering", &capabilities.ShaderExecutionReordering },
                { "RayTracingOpacityMicromap", &capabilities.RayTracingOpacityMicromap },
                { "HardwareRayTracingRendererPath", &capabilities.HardwareRayTracingRendererPath },
            }};
            for (const auto& [name, status] : advancedFeatures)
            {
                if (!status->bEnabled)
                {
                    document["degradations"].push_back({
                        { "feature", name },
                        { "reason", status->Reason },
                    });
                }
            }
            return document;
        }

        class ProbeMessageCallback final : public nvrhi::IMessageCallback
        {
        public:
            void message(nvrhi::MessageSeverity severity, const char* messageText) override
            {
                const char* text = messageText != nullptr ? messageText : "<empty NVRHI probe message>";
                switch (severity)
                {
                case nvrhi::MessageSeverity::Info:
                    HE_CORE_INFO("NVRHI probe: {0}", text);
                    break;
                case nvrhi::MessageSeverity::Warning:
                    HE_CORE_WARN("NVRHI probe: {0}", text);
                    break;
                case nvrhi::MessageSeverity::Error:
                case nvrhi::MessageSeverity::Fatal:
                    HE_CORE_ERROR("NVRHI probe: {0}", text);
                    break;
                }
            }
        };

        [[nodiscard]] ComPtr<ID3D12CommandQueue> CreateProbeQueue(
            ID3D12Device* device,
            D3D12_COMMAND_LIST_TYPE type)
        {
            D3D12_COMMAND_QUEUE_DESC description = {};
            description.Type = type;
            description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

            ComPtr<ID3D12CommandQueue> queue;
            CheckHResult(
                "ID3D12Device::CreateCommandQueue(capability probe)",
                device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)));
            return queue;
        }
    }

    void RequireDX12OperatingSystemBaseline()
    {
        constexpr uint32_t c_Windows10_1909Build = 18363;
        constexpr uint32_t c_Windows10_1909MinimumRevision = 1350;
        constexpr uint32_t c_Windows10_2004Build = 19041;
        constexpr uint32_t c_Windows10_20H2Build = 19042;
        constexpr uint32_t c_Windows10_2004MinimumRevision = 789;
        const WindowsVersionInformation version = QueryWindowsVersion();
        if (!version.bAvailable)
        {
            HE_CORE_WARN(
                "Could not verify the Windows 10 1909 DX12 baseline: {0}. "
                "Device creation will provide the final OS/runtime diagnostic.",
                version.QueryError);
            return;
        }
        if (version.Major < 10 || version.Build < c_Windows10_1909Build)
        {
            throw std::runtime_error(
                "HEngine DX12 requires Windows 10 1909 (build 18363) or newer; detected Windows " +
                std::to_string(version.Major) + '.' + std::to_string(version.Minor) +
                " build " + std::to_string(version.Build) + '.');
        }

        uint32_t minimumRevision = 0;
        if (version.Build == c_Windows10_1909Build)
            minimumRevision = c_Windows10_1909MinimumRevision;
        else if (version.Build == c_Windows10_2004Build ||
            version.Build == c_Windows10_20H2Build)
        {
            minimumRevision = c_Windows10_2004MinimumRevision;
        }

        if (minimumRevision == 0)
            return;
        if (!version.bRevisionAvailable)
        {
            HE_CORE_WARN(
                "Windows build {0} requires cumulative update revision {1}+ for the DX12 Agility loader, "
                "but the UBR registry value is unavailable. D3D12CreateDevice will provide the final diagnostic.",
                version.Build,
                minimumRevision);
            return;
        }
        if (version.Revision < minimumRevision)
        {
            throw std::runtime_error(
                "The DirectX 12 Agility SDK loader requires Windows build " +
                std::to_string(version.Build) + '.' + std::to_string(minimumRevision) +
                " or newer; detected " + std::to_string(version.Major) + '.' +
                std::to_string(version.Minor) + " build " +
                std::to_string(version.Build) + '.' + std::to_string(version.Revision) +
                ". Install the latest Windows cumulative update.");
        }
    }

    DX12AdapterSelectionResult SelectDX12Adapter(
        IDXGIFactory6* factory,
        const RenderAdapterSelectionOptions& selection)
    {
        if (factory == nullptr)
            throw std::invalid_argument("SelectDX12Adapter requires a DXGI factory");
        if (selection.bUseWarp && (selection.GpuIndex || selection.AdapterLuid))
        {
            throw std::invalid_argument(
                "WARP selection cannot be combined with a hardware GPU index or adapter LUID");
        }

        if (selection.bUseWarp)
        {
            ComPtr<IDXGIAdapter1> warp;
            CheckHResult("IDXGIFactory6::EnumWarpAdapter", factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
            DX12AdapterSelectionResult result = DescribeAdapter(warp.Get(), 0);
            const HRESULT probeResult = D3D12CreateDevice(
                result.Adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr);
            LogAdapterCandidate(result, SUCCEEDED(probeResult) ? "PASS (WARP)" : HResultToString(probeResult));
            if (FAILED(probeResult))
            {
                throw std::runtime_error(
                    "The DXGI WARP adapter could not create a DX12 FL11_0 device: " +
                    HResultToString(probeResult));
            }
            return result;
        }

        for (uint32_t adapterIndex = 0; ; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumResult = factory->EnumAdapterByGpuPreference(
                adapterIndex,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&candidate));
            if (enumResult == DXGI_ERROR_NOT_FOUND)
                break;
            CheckHResult("IDXGIFactory6::EnumAdapterByGpuPreference", enumResult);

            DX12AdapterSelectionResult described = DescribeAdapter(candidate.Get(), adapterIndex);
            if ((described.Description.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0)
            {
                LogAdapterCandidate(described, "SKIP (software adapter; request WARP explicitly)");
                continue;
            }
            if (!MatchesSelection(described, selection))
            {
                LogAdapterCandidate(described, "SKIP (does not match requested GPU selector)");
                continue;
            }

            const HRESULT probeResult = D3D12CreateDevice(
                described.Adapter.Get(),
                D3D_FEATURE_LEVEL_11_0,
                __uuidof(ID3D12Device),
                nullptr);
            LogAdapterCandidate(described, SUCCEEDED(probeResult) ? "PASS" : HResultToString(probeResult));
            if (SUCCEEDED(probeResult))
                return described;
        }

        std::string selector;
        if (selection.GpuIndex)
            selector += " GPU index " + std::to_string(*selection.GpuIndex);
        if (selection.AdapterLuid)
            selector += " adapter LUID " + LuidToString(*selection.AdapterLuid);
        throw std::runtime_error(
            "No matching hardware adapter" + selector +
            " could create a Direct3D 12 feature-level 11_0 device. Verify Windows 10 1909+ or "
            "Windows 11, the D3D12 runtime/Agility SDK installation, and the graphics driver.");
    }

    void QueryNativeDX12Capabilities(
        ID3D12Device* device,
        const DX12AdapterSelectionResult& adapter,
        RenderCapabilities& capabilities,
        DX12NativeCapabilities& nativeCapabilities)
    {
        if (device == nullptr || !adapter.Adapter)
            throw std::invalid_argument("QueryNativeDX12Capabilities requires a device and adapter");

        nativeCapabilities = {};
        capabilities.AdapterName = WideToUtf8(adapter.Description.Description);
        capabilities.AdapterDriverVersion = adapter.DriverVersion;
        capabilities.AdapterIndex = adapter.PreferenceIndex;
        capabilities.AdapterVendorId = adapter.Description.VendorId;
        capabilities.AdapterDeviceId = adapter.Description.DeviceId;
        capabilities.AdapterLuid = PackLuid(adapter.Description.AdapterLuid);
        capabilities.DedicatedVideoMemory = adapter.Description.DedicatedVideoMemory;
        capabilities.bSoftwareAdapter =
            (adapter.Description.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0;
        capabilities.SupportedMsaaSampleMask = QuerySceneMsaaSampleMask(device);
        capabilities.bDrawIndirectFirstInstance = true;
        capabilities.bMultiDrawIndirect = true;
        capabilities.bDrawIndirectCount = true;

        constexpr std::array featureLevels = {
            D3D_FEATURE_LEVEL_12_2,
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevelData = {};
        featureLevelData.NumFeatureLevels = static_cast<uint32_t>(featureLevels.size());
        featureLevelData.pFeatureLevelsRequested = featureLevels.data();
        featureLevelData.MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        CheckHResult(
            "ID3D12Device::CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS)",
            device->CheckFeatureSupport(
                D3D12_FEATURE_FEATURE_LEVELS,
                &featureLevelData,
                sizeof(featureLevelData)));
        nativeCapabilities.FeatureLevel =
            static_cast<uint32_t>(featureLevelData.MaxSupportedFeatureLevel);

        constexpr std::array shaderModels = {
            // Numeric casts keep the query source-compatible with older
            // DirectX-Headers while the runtime E_INVALIDARG fallback handles
            // OS versions that do not recognize these newer enum values.
            static_cast<D3D_SHADER_MODEL>(0x69),
            static_cast<D3D_SHADER_MODEL>(0x68),
            D3D_SHADER_MODEL_6_7,
            D3D_SHADER_MODEL_6_6,
            D3D_SHADER_MODEL_6_5,
            D3D_SHADER_MODEL_6_4,
            D3D_SHADER_MODEL_6_3,
            D3D_SHADER_MODEL_6_2,
            D3D_SHADER_MODEL_6_1,
            D3D_SHADER_MODEL_6_0,
            D3D_SHADER_MODEL_5_1,
        };
        HRESULT shaderModelResult = E_INVALIDARG;
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModelData = {};
        for (D3D_SHADER_MODEL requestedModel : shaderModels)
        {
            shaderModelData.HighestShaderModel = requestedModel;
            shaderModelResult = device->CheckFeatureSupport(
                D3D12_FEATURE_SHADER_MODEL,
                &shaderModelData,
                sizeof(shaderModelData));
            if (SUCCEEDED(shaderModelResult))
                break;
        }
        CheckHResult(
            "ID3D12Device::CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL)",
            shaderModelResult);
        nativeCapabilities.ShaderModel =
            static_cast<uint32_t>(shaderModelData.HighestShaderModel);

        D3D12_FEATURE_DATA_ROOT_SIGNATURE rootSignatureData = { D3D_ROOT_SIGNATURE_VERSION_1_1 };
        HRESULT rootSignatureResult = device->CheckFeatureSupport(
            D3D12_FEATURE_ROOT_SIGNATURE,
            &rootSignatureData,
            sizeof(rootSignatureData));
        if (FAILED(rootSignatureResult))
        {
            rootSignatureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
            rootSignatureResult = device->CheckFeatureSupport(
                D3D12_FEATURE_ROOT_SIGNATURE,
                &rootSignatureData,
                sizeof(rootSignatureData));
        }
        CheckHResult(
            "ID3D12Device::CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE)",
            rootSignatureResult);
        nativeCapabilities.RootSignatureVersion =
            static_cast<uint32_t>(rootSignatureData.HighestVersion);

        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        const HRESULT optionsResult = device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
        if (SUCCEEDED(optionsResult))
            nativeCapabilities.ResourceBindingTier =
                static_cast<uint32_t>(options.ResourceBindingTier);

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
        const HRESULT options5Result = device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
        if (SUCCEEDED(options5Result))
            nativeCapabilities.RayTracingTier =
                static_cast<uint32_t>(options5.RaytracingTier);

        D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6 = {};
        const HRESULT options6Result = device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6));
        if (SUCCEEDED(options6Result))
        {
            nativeCapabilities.VariableRateShadingTier =
                static_cast<uint32_t>(options6.VariableShadingRateTier);
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
        const HRESULT options7Result = device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
        if (SUCCEEDED(options7Result))
        {
            nativeCapabilities.MeshShaderTier =
                static_cast<uint32_t>(options7.MeshShaderTier);
            nativeCapabilities.SamplerFeedbackTier =
                static_cast<uint32_t>(options7.SamplerFeedbackTier);
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
        const HRESULT options12Result = device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12));

        ComPtr<ID3D12Device2> device2;
        ComPtr<ID3D12Device5> device5;
        ComPtr<ID3D12Device8> device8;
        ComPtr<ID3D12Device10> device10;
        const bool bDevice2 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device2)));
        const bool bDevice5 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5)));
        const bool bDevice8 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device8)));
        const bool bDevice10 = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device10)));

        const bool bShaderModel6 =
            nativeCapabilities.ShaderModel >= D3D_SHADER_MODEL_6_0;
        const bool bBaseline =
            nativeCapabilities.FeatureLevel >= D3D_FEATURE_LEVEL_11_0 && bShaderModel6;
        capabilities.BaselineRaster.bSupported = bBaseline;
        capabilities.BaselineRaster.bEnabled = bBaseline;
        capabilities.BaselineRaster.Reason = bBaseline
            ? "FL11_0+ and Shader Model 6.0+ are available; SM6.0 shaders, descriptor tables, and legacy barriers are valid."
            : "The device was created at FL11_0, but the baseline SM6.0 shader requirement is not available (highest Shader Model " +
                ShaderModelToString(nativeCapabilities.ShaderModel) + ").";

        if (nativeCapabilities.ShaderModel >= D3D_SHADER_MODEL_6_6)
        {
            EnableNativeFeature(
                capabilities.ShaderModel6_6,
                "Shader Model " + ShaderModelToString(nativeCapabilities.ShaderModel) +
                " is available; explicitly tagged SM6.6 shader variants may be selected.");
        }
        else
        {
            DisableFeature(
                capabilities.ShaderModel6_6,
                "Highest Shader Model is " + ShaderModelToString(nativeCapabilities.ShaderModel) +
                "; baseline shaders remain on SM6.0.");
        }

        if (SUCCEEDED(optionsResult) &&
            options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3)
        {
            EnableNativeFeature(
                capabilities.ResourceBindingTier3,
                "D3D12 Resource Binding Tier 3 is available; bindless-capable paths may opt in independently.");
        }
        else
        {
            DisableFeature(
                capabilities.ResourceBindingTier3,
                FAILED(optionsResult)
                    ? "D3D12_OPTIONS query failed with " + HResultToString(optionsResult) +
                        "; traditional descriptor tables selected."
                    : "Resource Binding Tier is " + TierToString(nativeCapabilities.ResourceBindingTier) +
                        "; traditional descriptor tables selected.");
        }

        nativeCapabilities.bHeapDirectlyIndexed =
            nativeCapabilities.RootSignatureVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1 &&
            nativeCapabilities.ShaderModel >= D3D_SHADER_MODEL_6_6 &&
            nativeCapabilities.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
        if (nativeCapabilities.bHeapDirectlyIndexed)
        {
            capabilities.HeapDirectlyIndexed.bSupported = true;
            capabilities.HeapDirectlyIndexed.Reason =
                "SM6.6 and Resource Binding Tier 3 satisfy D3D12 heap-direct indexing prerequisites.";
        }
        else
        {
            std::string missingPrerequisites;
            const auto addMissingPrerequisite = [&missingPrerequisites](std::string_view prerequisite)
            {
                if (!missingPrerequisites.empty())
                    missingPrerequisites += ", ";
                missingPrerequisites += prerequisite;
            };
            if (nativeCapabilities.RootSignatureVersion < D3D_ROOT_SIGNATURE_VERSION_1_1)
                addMissingPrerequisite("Root Signature 1.1");
            if (nativeCapabilities.ShaderModel < D3D_SHADER_MODEL_6_6)
                addMissingPrerequisite("Shader Model 6.6");
            if (nativeCapabilities.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
                addMissingPrerequisite("Resource Binding Tier 3");
            DisableFeature(
                capabilities.HeapDirectlyIndexed,
                "Heap-direct indexing is missing " + missingPrerequisites +
                "; traditional descriptor tables selected.");
        }

        const bool bRayTracing10 = SUCCEEDED(options5Result) && bDevice5 &&
            options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
        if (bRayTracing10)
        {
            capabilities.RayTracingAccelStruct.bSupported = true;
            capabilities.RayTracingPipeline.bSupported = true;
            capabilities.RayTracingAccelStruct.Reason = "D3D12 reports DXR Tier 1.0+ and ID3D12Device5.";
            capabilities.RayTracingPipeline.Reason = capabilities.RayTracingAccelStruct.Reason;
        }
        else
        {
            const std::string reason = FAILED(options5Result)
                ? "D3D12_OPTIONS5 query failed with " + HResultToString(options5Result) +
                    "; raster rendering selected."
                : "DXR Tier 1.0 and ID3D12Device5 are not both available; raster rendering selected.";
            DisableFeature(capabilities.RayTracingAccelStruct, reason);
            DisableFeature(capabilities.RayTracingPipeline, reason);
        }

        const bool bRayQuery = SUCCEEDED(options5Result) && bDevice5 &&
            options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
        if (bRayQuery)
        {
            capabilities.RayQuery.bSupported = true;
            capabilities.RayQuery.Reason = "D3D12 reports DXR Tier 1.1+, which permits inline RayQuery.";
        }
        else
        {
            DisableFeature(
                capabilities.RayQuery,
                "RayQuery requires DXR Tier 1.1 and ID3D12Device5; the raster or DXR 1.0 path remains available.");
        }

        const bool bMeshShader = SUCCEEDED(options7Result) && bDevice2 &&
            options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;
        if (bMeshShader)
        {
            capabilities.MeshShader.bSupported = true;
            capabilities.MeshShader.Reason = "D3D12 Mesh Shader Tier 1 and ID3D12Device2 are available.";
        }
        else
        {
            DisableFeature(
                capabilities.MeshShader,
                FAILED(options7Result)
                    ? "D3D12_OPTIONS7 query failed with " + HResultToString(options7Result) +
                        "; conventional vertex/primitive processing selected."
                    : "Mesh Shader Tier 1 and ID3D12Device2 are not both available; conventional vertex/primitive processing selected.");
        }

        const bool bVrs = SUCCEEDED(options6Result) &&
            options6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1;
        if (bVrs)
        {
            capabilities.VariableRateShading.bSupported = true;
            capabilities.VariableRateShading.Reason =
                "D3D12 Variable Rate Shading Tier " +
                TierToString(nativeCapabilities.VariableRateShadingTier) + " is available.";
        }
        else
        {
            DisableFeature(
                capabilities.VariableRateShading,
                FAILED(options6Result)
                    ? "D3D12_OPTIONS6 query failed with " + HResultToString(options6Result) +
                        "; full-rate shading selected."
                    : "Variable Rate Shading is not supported; full-rate shading selected.");
        }

        const bool bSamplerFeedback = SUCCEEDED(options7Result) && bDevice8 &&
            options7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9;
        if (bSamplerFeedback)
        {
            capabilities.SamplerFeedback.bSupported = true;
            capabilities.SamplerFeedback.Reason =
                "D3D12 Sampler Feedback Tier " +
                SamplerFeedbackTierToString(nativeCapabilities.SamplerFeedbackTier) +
                " and ID3D12Device8 are available.";
        }
        else
        {
            DisableFeature(
                capabilities.SamplerFeedback,
                "Sampler Feedback Tier 0.9 and ID3D12Device8 are not both available; sampler feedback remains disabled.");
        }

        nativeCapabilities.bEnhancedBarriers = SUCCEEDED(options12Result) && bDevice10 &&
            options12.EnhancedBarriersSupported == TRUE;
        if (nativeCapabilities.bEnhancedBarriers)
        {
            capabilities.EnhancedBarriers.bSupported = true;
            capabilities.EnhancedBarriers.Reason =
                "D3D12_OPTIONS12 and ID3D12Device10 report Enhanced Barriers support.";
        }
        else
        {
            DisableFeature(
                capabilities.EnhancedBarriers,
                FAILED(options12Result)
                    ? "D3D12_OPTIONS12 query is unavailable (" + HResultToString(options12Result) +
                        "); legacy resource barriers selected."
                    : "Enhanced Barriers and ID3D12Device10 are not both available; legacy resource barriers selected.");
        }

        DisableFeature(
            capabilities.ShaderExecutionReordering,
            "Awaiting the independent NVRHI ShaderExecutionReordering feature query.");
        DisableFeature(
            capabilities.RayTracingOpacityMicromap,
            "Awaiting the independent NVRHI RayTracingOpacityMicromap feature query.");
    }

    void ConfigureNvrhiDX12Capabilities(
        const DX12NativeCapabilities& nativeCapabilities,
        nvrhi::d3d12::DeviceDesc& deviceDescription)
    {
        deviceDescription.enableHeapDirectlyIndexed =
            nativeCapabilities.bHeapDirectlyIndexed;
        deviceDescription.enableEnhancedBarriers =
            nativeCapabilities.bEnhancedBarriers;
    }

    void QueryNvrhiDX12Capabilities(
        nvrhi::IDevice* device,
        RenderCapabilities& capabilities)
    {
        if (device == nullptr)
            throw std::invalid_argument("QueryNvrhiDX12Capabilities requires an NVRHI device");

        capabilities.bNvrhiRayTracingAccelStruct =
            device->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct);
        capabilities.bNvrhiRayTracingPipeline =
            device->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline);
        capabilities.bNvrhiRayQuery = device->queryFeatureSupport(nvrhi::Feature::RayQuery);
        capabilities.bNvrhiMeshlets = device->queryFeatureSupport(nvrhi::Feature::Meshlets);
        capabilities.bNvrhiVariableRateShading =
            device->queryFeatureSupport(nvrhi::Feature::VariableRateShading);
        capabilities.bNvrhiHeapDirectlyIndexed =
            device->queryFeatureSupport(nvrhi::Feature::HeapDirectlyIndexed);
        capabilities.bNvrhiEnhancedBarriers =
            device->queryFeatureSupport(nvrhi::Feature::EnhancedBarriers);
        capabilities.bNvrhiShaderExecutionReordering =
            device->queryFeatureSupport(nvrhi::Feature::ShaderExecutionReordering);
        capabilities.bNvrhiRayTracingOpacityMicromap =
            device->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap);
        capabilities.bNvrhiNvapiBuildSupport = HE_NVRHI_NVAPI_ENABLED != 0;
        capabilities.bNvrhiNativeOpacityMicromapBuildSupport =
            HE_NVRHI_NATIVE_OMM_ENABLED != 0;
        capabilities.bComputeQueue = device->queryFeatureSupport(nvrhi::Feature::ComputeQueue);
        capabilities.bCopyQueue = device->queryFeatureSupport(nvrhi::Feature::CopyQueue);

        FinalizeNvrhiFeature(
            capabilities.HeapDirectlyIndexed,
            capabilities.bNvrhiHeapDirectlyIndexed,
            "Heap-direct indexing",
            "traditional descriptor tables selected.");
        FinalizeNvrhiFeature(
            capabilities.EnhancedBarriers,
            capabilities.bNvrhiEnhancedBarriers,
            "Enhanced Barriers",
            "legacy resource barriers selected.");
        FinalizeNvrhiFeature(
            capabilities.RayTracingAccelStruct,
            capabilities.bNvrhiRayTracingAccelStruct,
            "Ray-tracing acceleration structures",
            "raster rendering selected.");
        FinalizeNvrhiFeature(
            capabilities.RayTracingPipeline,
            capabilities.bNvrhiRayTracingPipeline,
            "Ray-tracing pipelines",
            "raster rendering selected.");
        FinalizeNvrhiFeature(
            capabilities.RayQuery,
            capabilities.bNvrhiRayQuery,
            "RayQuery",
            "pipeline DXR or raster rendering remains available.");
        FinalizeNvrhiFeature(
            capabilities.MeshShader,
            capabilities.bNvrhiMeshlets,
            "Mesh shaders",
            "conventional vertex/primitive processing selected.");
        FinalizeNvrhiFeature(
            capabilities.VariableRateShading,
            capabilities.bNvrhiVariableRateShading,
            "Variable Rate Shading",
            "full-rate shading selected. NVRHI currently exposes its VRS feature at Tier 2.");
        FinalizeNvrhiFeature(
            capabilities.SamplerFeedback,
            device->queryFeatureSupport(nvrhi::Feature::SamplerFeedback),
            "Sampler Feedback",
            "sampler feedback remains disabled.");

        capabilities.ShaderExecutionReordering.bSupported =
            capabilities.bNvrhiShaderExecutionReordering;
        capabilities.ShaderExecutionReordering.bEnabled =
            capabilities.bNvrhiShaderExecutionReordering;
        capabilities.ShaderExecutionReordering.Reason = capabilities.bNvrhiShaderExecutionReordering
            ? "NVRHI reports ShaderExecutionReordering; the capability is available to a future DXR path."
            : capabilities.bNvrhiNvapiBuildSupport
                ? "NVRHI queried NVAPI but the active hardware/driver does not report ShaderExecutionReordering."
                : "NVRHI was built without the optional NVAPI SDK, so SER hardware support was not queried; the feature remains disabled.";

        capabilities.RayTracingOpacityMicromap.bSupported =
            capabilities.bNvrhiRayTracingOpacityMicromap;
        capabilities.RayTracingOpacityMicromap.bEnabled =
            capabilities.bNvrhiRayTracingOpacityMicromap;
        capabilities.RayTracingOpacityMicromap.Reason = capabilities.bNvrhiRayTracingOpacityMicromap
            ? "NVRHI reports RayTracingOpacityMicromap; the capability is available to a future DXR path."
            : (capabilities.bNvrhiNativeOpacityMicromapBuildSupport ||
                capabilities.bNvrhiNvapiBuildSupport)
                ? "NVRHI queried the enabled native/NVAPI OMM path, but the active hardware/driver does not report opacity micromaps."
                : "NVRHI was built without native or NVAPI OMM support, so hardware support was not queried; the feature remains disabled.";

        ResolveRendererCapabilityPolicy(capabilities);
    }

    void RequireBaselineDX12Capabilities(
        const DX12NativeCapabilities& nativeCapabilities)
    {
        if (nativeCapabilities.FeatureLevel < D3D_FEATURE_LEVEL_11_0)
        {
            throw std::runtime_error(
                "The selected adapter does not meet the DX12 baseline Feature Level 11_0 requirement.");
        }
        if (nativeCapabilities.ShaderModel < D3D_SHADER_MODEL_6_0)
        {
            throw std::runtime_error(
                "The DX12 device was created successfully at Feature Level " +
                std::string(FeatureLevelToString(nativeCapabilities.FeatureLevel)) +
                ", but the baseline shader profile requires Shader Model 6.0 and the driver reports " +
                ShaderModelToString(nativeCapabilities.ShaderModel) +
                ". Update Windows and the graphics driver; DXR and SM6.6 are not required.");
        }
    }

    void LogDX12Capabilities(
        const RenderCapabilities& capabilities,
        const DX12NativeCapabilities& nativeCapabilities)
    {
        HE_CORE_INFO(
            "DX12 selected adapter [{0}]: {1}; VendorId=0x{2:04X}, DeviceId=0x{3:04X}, LUID={4}, "
            "dedicated VRAM={5} MiB, driver={6}, software={7}",
            capabilities.AdapterIndex,
            capabilities.AdapterName,
            capabilities.AdapterVendorId,
            capabilities.AdapterDeviceId,
            LuidToString(capabilities.AdapterLuid),
            capabilities.DedicatedVideoMemory / (1024ull * 1024ull),
            capabilities.AdapterDriverVersion,
            capabilities.bSoftwareAdapter);
        HE_CORE_INFO(
            "DX12 scene MSAA support (RGBA16_FLOAT + D32, quality 0): {0}",
            DescribeSupportedMsaaSamples(capabilities.SupportedMsaaSampleMask));
        HE_CORE_INFO(
            "DX12 native capabilities: maximum FL={0}, SM={1}, Root Signature={2}, Binding Tier={3}, "
            "DXR Tier={4}, Mesh Shader Tier={5}, VRS Tier={6}, Sampler Feedback Tier={7}, "
            "heap-direct={8}, Enhanced Barriers={9}",
            FeatureLevelToString(nativeCapabilities.FeatureLevel),
            ShaderModelToString(nativeCapabilities.ShaderModel),
            RootSignatureVersionToString(nativeCapabilities.RootSignatureVersion),
            TierToString(nativeCapabilities.ResourceBindingTier),
            RayTracingTierToString(nativeCapabilities.RayTracingTier),
            MeshShaderTierToString(nativeCapabilities.MeshShaderTier),
            TierToString(nativeCapabilities.VariableRateShadingTier),
            SamplerFeedbackTierToString(nativeCapabilities.SamplerFeedbackTier),
            nativeCapabilities.bHeapDirectlyIndexed ? "yes" : "no",
            nativeCapabilities.bEnhancedBarriers ? "yes" : "no");
        HE_CORE_INFO(
            "NVRHI capabilities: RayTracingAccelStruct={0}, RayTracingPipeline={1}, RayQuery={2}, "
            "Meshlets={3}, VariableRateShading={4}, HeapDirectlyIndexed={5}, EnhancedBarriers={6}, "
            "ShaderExecutionReordering={7}, RayTracingOpacityMicromap={8}",
            capabilities.bNvrhiRayTracingAccelStruct,
            capabilities.bNvrhiRayTracingPipeline,
            capabilities.bNvrhiRayQuery,
            capabilities.bNvrhiMeshlets,
            capabilities.bNvrhiVariableRateShading,
            capabilities.bNvrhiHeapDirectlyIndexed,
            capabilities.bNvrhiEnhancedBarriers,
            capabilities.bNvrhiShaderExecutionReordering,
            capabilities.bNvrhiRayTracingOpacityMicromap);

        const std::array<std::pair<const char*, const RenderFeatureStatus*>, 14> statuses = {{
            { "BaselineRaster", &capabilities.BaselineRaster },
            { "ShaderModel6_6", &capabilities.ShaderModel6_6 },
            { "ResourceBindingTier3", &capabilities.ResourceBindingTier3 },
            { "HeapDirectlyIndexed", &capabilities.HeapDirectlyIndexed },
            { "EnhancedBarriers", &capabilities.EnhancedBarriers },
            { "RayTracingAccelStruct", &capabilities.RayTracingAccelStruct },
            { "RayTracingPipeline", &capabilities.RayTracingPipeline },
            { "RayQuery", &capabilities.RayQuery },
            { "MeshShader", &capabilities.MeshShader },
            { "VariableRateShading", &capabilities.VariableRateShading },
            { "SamplerFeedback", &capabilities.SamplerFeedback },
            { "ShaderExecutionReordering", &capabilities.ShaderExecutionReordering },
            { "RayTracingOpacityMicromap", &capabilities.RayTracingOpacityMicromap },
            { "HardwareRayTracingRendererPath", &capabilities.HardwareRayTracingRendererPath },
        }};
        for (const auto& [name, status] : statuses)
        {
            HE_CORE_INFO(
                "DX12 feature {0}: supported={1}, enabled={2}; {3}",
                name,
                status->bSupported,
                status->bEnabled,
                status->Reason);
        }
        HE_CORE_INFO("DX12 renderer capability profile: {0}", ToString(capabilities.Profile));
    }

    void RunDX12CapabilityProbe(
        const std::filesystem::path& outputPath,
        const RenderAdapterSelectionOptions& selection)
    {
        if (outputPath.empty())
            throw std::invalid_argument("The DX12 capability probe output path cannot be empty");

        RequireDX12OperatingSystemBaseline();

        ComPtr<IDXGIFactory6> factory;
        CheckHResult(
            "CreateDXGIFactory2(DX12 capability probe; verify the Windows DXGI runtime)",
            CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        const DX12AdapterSelectionResult adapter = SelectDX12Adapter(factory.Get(), selection);

        ComPtr<ID3D12Device> nativeDevice;
        CheckHResult(
            "D3D12CreateDevice(FL11_0 capability probe; verify OS, Agility SDK/runtime, and driver)",
            D3D12CreateDevice(
                adapter.Adapter.Get(),
                D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&nativeDevice)));

        RenderCapabilities capabilities;
        capabilities.Backend = RenderBackend::DX12;
        DX12NativeCapabilities nativeCapabilities;
        QueryNativeDX12Capabilities(
            nativeDevice.Get(),
            adapter,
            capabilities,
            nativeCapabilities);

        ComPtr<ID3D12CommandQueue> graphicsQueue =
            CreateProbeQueue(nativeDevice.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        ComPtr<ID3D12CommandQueue> computeQueue =
            CreateProbeQueue(nativeDevice.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE);
        ComPtr<ID3D12CommandQueue> copyQueue =
            CreateProbeQueue(nativeDevice.Get(), D3D12_COMMAND_LIST_TYPE_COPY);

        ProbeMessageCallback messageCallback;
        nvrhi::d3d12::DeviceDesc deviceDescription;
        deviceDescription.errorCB = &messageCallback;
        deviceDescription.pDevice = nativeDevice.Get();
        deviceDescription.pGraphicsCommandQueue = graphicsQueue.Get();
        deviceDescription.pComputeCommandQueue = computeQueue.Get();
        deviceDescription.pCopyCommandQueue = copyQueue.Get();
        ConfigureNvrhiDX12Capabilities(nativeCapabilities, deviceDescription);

        nvrhi::d3d12::DeviceHandle nvrhiDevice = nvrhi::d3d12::createDevice(deviceDescription);
        if (!nvrhiDevice)
        {
            throw std::runtime_error(
                "nvrhi::d3d12::createDevice failed during the headless capability probe. "
                "Check the D3D12 runtime/driver and the logged heap-direct/Enhanced Barriers decisions.");
        }

        capabilities.bGraphicsQueue = true;
        QueryNvrhiDX12Capabilities(nvrhiDevice.Get(), capabilities);
        LogDX12Capabilities(capabilities, nativeCapabilities);

        if (!nvrhiDevice->waitForIdle())
            throw std::runtime_error("NVRHI failed to make the headless DX12 capability probe device idle");

        const std::filesystem::path absoluteOutput = outputPath.is_absolute()
            ? outputPath.lexically_normal()
            : std::filesystem::absolute(outputPath).lexically_normal();
        const std::filesystem::path parent = absoluteOutput.parent_path();
        if (!parent.empty())
        {
            std::error_code directoryError;
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError)
            {
                throw std::runtime_error(
                    "Could not create the DX12 probe output directory '" + parent.string() +
                    "': " + directoryError.message());
            }
        }
        Utils::SaveJsonToFile(
            CapabilitiesToJson(capabilities, nativeCapabilities),
            absoluteOutput);
        HE_CORE_INFO("DX12 capability probe wrote {0}", absoluteOutput.string());

        // The JSON remains available for diagnosis even when the current
        // baseline shader profile cannot run on the selected adapter.
        RequireBaselineDX12Capabilities(nativeCapabilities);
    }
}
