#include "Hepch.h"

#include "Runtime/Graphics/RHI/TemporalUpscaler.h"

#if HE_FSR31
#include <Windows.h>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#if HE_FSR31_DX12
#include <directx/d3d12.h>
// SDK 1.1.4 declares identically named frame-generation enumerators in
// both backend headers. Isolate those declarations without changing the SDK.
namespace FsrDx12
{
#include <ffx_api/dx12/ffx_api_dx12.h>
}
#endif
#if HE_FSR31_VULKAN
#include "Runtime/Platform/Vulkan/VulkanNativeContext.h"
namespace FsrVk
{
#include <ffx_api/vk/ffx_api_vk.h>
}
#endif
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>

namespace HEngine
{
#if HE_FSR31
    namespace
    {
#if HE_FSR31_VULKAN
        VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetFsrVulkanDeviceProcAddr(VkDevice device, const char* name)
        {
            auto function = vkGetDeviceProcAddr(device, name);
            // HEngine enables Vulkan 1.3 core functionality. SDK 1.1.4 still
            // requests the old extension spelling; loaders may legitimately
            // return null when the promoted extension is not enabled by name.
            if (!function && std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)
                function = vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
            return function;
        }
#endif
        uint32_t GetFsrFormat(nvrhi::Format format)
        {
            switch (format)
            {
            case nvrhi::Format::RGBA16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
            case nvrhi::Format::RGBA32_FLOAT: return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
            case nvrhi::Format::RG16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
            case nvrhi::Format::RG32_FLOAT: return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
            case nvrhi::Format::R32_FLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
            case nvrhi::Format::R16_FLOAT: return FFX_API_SURFACE_FORMAT_R16_FLOAT;
            case nvrhi::Format::R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
            case nvrhi::Format::RG8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8_UNORM;
            case nvrhi::Format::RGBA8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
            case nvrhi::Format::R11G11B10_FLOAT: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
            default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
            }
        }

        class Fsr31Upscaler final : public ITemporalUpscaler
        {
        public:
            ~Fsr31Upscaler() override
            {
                if (mContext && mDestroy) mDestroy(&mContext, nullptr);
                if (mModule) FreeLibrary(mModule);
            }

            bool Initialize(nvrhi::IDevice* device, const TemporalUpscalerDesc& desc, std::string& error)
            {
                mDesc = desc;
                mApi = device->getGraphicsAPI();
                const wchar_t* filename = nullptr;
#if HE_FSR31_DX12
                if (mApi == nvrhi::GraphicsAPI::D3D12)
                {
                    filename = L"amd_fidelityfx_dx12.dll";
                    mDx12Desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
                    mDx12Desc.device = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
                    mCreateDesc.header.pNext = &mDx12Desc.header;
                }
#endif
#if HE_FSR31_VULKAN
                if (mApi == nvrhi::GraphicsAPI::VULKAN)
                {
                    const auto* native = GetVulkanNativeContext(device);
                    if (!native)
                    {
                        error = "FSR 3.1: Vulkan native context is unavailable";
                        return false;
                    }
                    filename = L"amd_fidelityfx_vk.dll";
                    mVkDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
                    mVkDesc.vkDevice = native->Device;
                    mVkDesc.vkPhysicalDevice = native->PhysicalDevice;
                    mVkDesc.vkDeviceProcAddr = GetFsrVulkanDeviceProcAddr;
                    if (!GetFsrVulkanDeviceProcAddr(native->Device, "vkGetBufferMemoryRequirements2KHR"))
                    {
                        error = "FSR 3.1: Vulkan buffer memory requirements query is unavailable";
                        return false;
                    }
                    mCreateDesc.header.pNext = &mVkDesc.header;
                }
#endif
                if (!filename)
                {
                    error = "FSR 3.1: unsupported graphics API";
                    return false;
                }
                // Resolve only the executable's deployed runtime, never the CWD
                // or PATH. Each backend is dynamically loaded independently.
                std::array<wchar_t, 32768> executable{};
                const DWORD length = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
                if (length == 0 || length >= executable.size())
                {
                    error = "FSR 3.1: cannot resolve executable directory";
                    return false;
                }
                const auto path = std::filesystem::path(executable.data()).parent_path() / filename;
                mModule = LoadLibraryExW(path.c_str(), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!mModule)
                {
                    error = "FSR 3.1: cannot load runtime " + path.string()
                        + " (Windows error " + std::to_string(GetLastError()) + ")";
                    return false;
                }
                const auto create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(mModule, "ffxCreateContext"));
                mDestroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(mModule, "ffxDestroyContext"));
                mDispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(mModule, "ffxDispatch"));
                mQuery = reinterpret_cast<PfnFfxQuery>(GetProcAddress(mModule, "ffxQuery"));
                if (!create || !mDestroy || !mDispatch || !mQuery)
                {
                    error = "FSR 3.1: runtime API exports are incomplete";
                    return false;
                }
                mCreateDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
                mCreateDesc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE
                    | (desc.bJitteredMotionVectors ? FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0)
                    | (desc.bReversedDepth ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0)
                    | (desc.bInfiniteDepth ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0)
                    | (desc.bAutoExposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0);
                mCreateDesc.maxRenderSize = {desc.RenderWidth, desc.RenderHeight};
                mCreateDesc.maxUpscaleSize = {desc.OutputWidth, desc.OutputHeight};
                if (!Check(create(&mContext, &mCreateDesc.header, nullptr), "create", error)) return false;
                ffxQueryGetProviderVersion version{};
                version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
                if (!Check(mQuery(&mContext, &version.header), "version query", error)) return false;
                mVersion = version.versionName ? version.versionName : "unknown";
                if (!mVersion.starts_with("3.1."))
                {
                    error = "FSR 3.1: unexpected provider version " + mVersion;
                    return false;
                }
                return true;
            }

            bool GetJitter(uint32_t frameIndex, uint32_t renderWidth,
                float& x, float& y, std::string& error) const override
            {
                if (renderWidth == 0 || renderWidth > mDesc.RenderWidth)
                {
                    error = "FSR 3.1: invalid jitter render width";
                    return false;
                }
                int32_t phaseCount = 0;
                ffxQueryDescUpscaleGetJitterPhaseCount phases{};
                phases.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
                phases.renderWidth = renderWidth;
                phases.displayWidth = mDesc.OutputWidth;
                phases.pOutPhaseCount = &phaseCount;
                if (!Check(mQuery(&mContext, &phases.header), "jitter phases", error) || phaseCount <= 0) return false;
                ffxQueryDescUpscaleGetJitterOffset offset{};
                offset.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
                offset.index = int32_t(frameIndex % uint32_t(phaseCount));
                offset.phaseCount = phaseCount;
                offset.pOutX = &x;
                offset.pOutY = &y;
                return Check(mQuery(&mContext, &offset.header), "jitter offset", error);
            }

            bool Dispatch(const TemporalUpscalerFrame& frame, std::string& error) override
            {
                if (!frame.CommandList || !frame.Color || !frame.Depth || !frame.MotionVectors || !frame.Output
                    || frame.RenderWidth == 0 || frame.RenderHeight == 0
                    || frame.RenderWidth != mDesc.RenderWidth || frame.RenderHeight != mDesc.RenderHeight
                    || !std::isfinite(frame.PreExposure) || frame.PreExposure <= 0.0f
                    || !std::isfinite(frame.Sharpness) || frame.Sharpness < 0.0f || frame.Sharpness > 1.0f
                    || !std::isfinite(frame.FrameTimeMilliseconds) || frame.FrameTimeMilliseconds < 0.0f
                    || !std::isfinite(frame.JitterX) || !std::isfinite(frame.JitterY)
                    || !std::isfinite(frame.MotionScaleX) || !std::isfinite(frame.MotionScaleY)
                    || !std::isfinite(frame.CameraNear) || frame.CameraNear <= 0.0f
                    || !std::isfinite(frame.CameraFar) || frame.CameraFar <= frame.CameraNear
                    || !std::isfinite(frame.VerticalFovRadians) || frame.VerticalFovRadians <= 0.0f
                    || frame.VerticalFovRadians >= 3.1415927f
                    || frame.Depth->getDesc().format != nvrhi::Format::R32_FLOAT
                    || frame.Output->getDesc().width != mDesc.OutputWidth
                    || frame.Output->getDesc().height != mDesc.OutputHeight || !frame.Output->getDesc().isUAV
                    || frame.Output->getDesc().sampleCount != 1
                    || GetFsrFormat(frame.Output->getDesc().format) == FFX_API_SURFACE_FORMAT_UNKNOWN)
                {
                    error = "FSR 3.1: invalid dispatch resources, dimensions or pre-exposure";
                    return false;
                }
                const std::array inputs{frame.Color, frame.Depth, frame.MotionVectors,
                    frame.Exposure, frame.Reactive, frame.Composition};
                for (auto* texture : inputs)
                {
                    if (texture && (GetFsrFormat(texture->getDesc().format) == FFX_API_SURFACE_FORMAT_UNKNOWN
                        || texture->getDesc().sampleCount != 1 || texture == frame.Output
                        || texture->getDesc().dimension != nvrhi::TextureDimension::Texture2D
                        || (texture != frame.Exposure && (texture->getDesc().width < frame.RenderWidth
                            || texture->getDesc().height < frame.RenderHeight))))
                    {
                        error = "FSR 3.1: unsupported input format, MSAA or output alias";
                        return false;
                    }
                }
                auto* commands = frame.CommandList;
                ffxDispatchDescUpscale dispatch{};
                dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
                dispatch.commandList = commands->getNativeObject(mApi == nvrhi::GraphicsAPI::D3D12
                    ? nvrhi::ObjectTypes::D3D12_GraphicsCommandList : nvrhi::ObjectTypes::VK_CommandBuffer).pointer;
                if (!dispatch.commandList)
                {
                    error = "FSR 3.1: native command list is unavailable";
                    return false;
                }
                for (auto* texture : inputs)
                    if (texture) commands->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
                commands->setTextureState(frame.Output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
                commands->commitBarriers();
                dispatch.color = Resource(frame.Color);
                dispatch.depth = Resource(frame.Depth);
                dispatch.motionVectors = Resource(frame.MotionVectors);
                dispatch.exposure = Resource(frame.Exposure);
                dispatch.reactive = Resource(frame.Reactive);
                dispatch.transparencyAndComposition = Resource(frame.Composition);
                dispatch.output = Resource(frame.Output, true);
                dispatch.jitterOffset = {frame.JitterX, frame.JitterY};
                dispatch.motionVectorScale = {frame.MotionScaleX, frame.MotionScaleY};
                dispatch.renderSize = {frame.RenderWidth, frame.RenderHeight};
                dispatch.upscaleSize = {mDesc.OutputWidth, mDesc.OutputHeight};
                dispatch.enableSharpening = frame.Sharpness > 0.0f;
                dispatch.sharpness = std::clamp(frame.Sharpness, 0.0f, 1.0f);
                dispatch.frameTimeDelta = frame.FrameTimeMilliseconds;
                dispatch.preExposure = frame.PreExposure;
                dispatch.reset = frame.bReset;
                dispatch.cameraNear = frame.CameraNear;
                dispatch.cameraFar = mDesc.bInfiniteDepth ? std::numeric_limits<float>::max() : frame.CameraFar;
                if (mDesc.bReversedDepth) std::swap(dispatch.cameraNear, dispatch.cameraFar);
                dispatch.cameraFovAngleVertical = frame.VerticalFovRadians;
                dispatch.viewSpaceToMetersFactor = 1.0f;
                const auto result = mDispatch(&mContext, &dispatch.header);
                // The SDK unregisters external resources and restores their
                // incoming states. Its descriptor heaps/pipelines are native.
                commands->clearState();
                return Check(result, "dispatch", error);
            }

            const std::string& GetVersion() const noexcept override { return mVersion; }

        private:
            static bool Check(ffxReturnCode_t result, const char* operation, std::string& error)
            {
                if (result == FFX_API_RETURN_OK) return true;
                error = std::string("FSR 3.1 ") + operation + " failed: " + std::to_string(result);
                return false;
            }

            FfxApiResource Resource(nvrhi::ITexture* texture, bool bOutput = false) const
            {
                if (!texture) return {};
                const auto& desc = texture->getDesc();
                FfxApiResource resource{};
                resource.resource = texture->getNativeObject(mApi == nvrhi::GraphicsAPI::D3D12
                    ? nvrhi::ObjectTypes::D3D12_Resource : nvrhi::ObjectTypes::VK_Image).pointer;
                resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
                resource.description.format = GetFsrFormat(desc.format);
                resource.description.width = desc.width;
                resource.description.height = desc.height;
                resource.description.depth = 1;
                resource.description.mipCount = desc.mipLevels;
                resource.description.usage = desc.isUAV ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
                resource.state = bOutput ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
                return resource;
            }

            HMODULE mModule = nullptr;
            mutable ffxContext mContext = nullptr;
            PfnFfxDestroyContext mDestroy = nullptr;
            PfnFfxDispatch mDispatch = nullptr;
            PfnFfxQuery mQuery = nullptr;
            ffxCreateContextDescUpscale mCreateDesc{};
#if HE_FSR31_DX12
            FsrDx12::ffxCreateBackendDX12Desc mDx12Desc{};
#endif
#if HE_FSR31_VULKAN
            FsrVk::ffxCreateBackendVKDesc mVkDesc{};
#endif
            TemporalUpscalerDesc mDesc;
            nvrhi::GraphicsAPI mApi = nvrhi::GraphicsAPI::D3D12;
            std::string mVersion;
        };
    }
#endif

    std::unique_ptr<ITemporalUpscaler> CreateFsr31Upscaler(
        nvrhi::IDevice* device, const TemporalUpscalerDesc& desc, std::string& error)
    {
        error.clear();
#if HE_FSR31
        if (!device || desc.RenderWidth == 0 || desc.RenderHeight == 0
            || desc.OutputWidth < desc.RenderWidth || desc.OutputHeight < desc.RenderHeight)
        {
            error = "FSR 3.1: invalid device or context dimensions";
            return nullptr;
        }
        auto result = std::make_unique<Fsr31Upscaler>();
        if (result->Initialize(device, desc, error)) return result;
#else
        (void)device;
        (void)desc;
        error = "FSR 3.1 runtime support was disabled at build time";
#endif
        return nullptr;
    }
}
