#include "Hepch.h"

#include "DX12RenderBackend.h"
#include "DX12Capabilities.h"

#include "Runtime/Platform/GLFW/CommonWindow.h"
#include "Runtime/Profile/RenderFrameTiming.h"

#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_WIN32 1
#include <GLFW/glfw3native.h>

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr uint32_t c_BackBufferCount = 3;
        constexpr DXGI_FORMAT c_BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

        void CALLBACK DX12MessageCallback(
            D3D12_MESSAGE_CATEGORY,
            D3D12_MESSAGE_SEVERITY severity,
            D3D12_MESSAGE_ID,
            LPCSTR description,
            void*)
        {
            const char* text = description != nullptr ? description : "<empty DX12 debug message>";
            switch (severity)
            {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:
                HE_CORE_ERROR("DX12 Debug Layer: {0}", text);
				Log::WriteFallbackError("DX12 Debug Layer", text ? text : "unknown error");
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                HE_CORE_WARN("DX12 Debug Layer: {0}", text);
				Log::WriteFallbackError("DX12 Debug Layer warning", text ? text : "unknown warning");
                break;
            case D3D12_MESSAGE_SEVERITY_INFO:
            case D3D12_MESSAGE_SEVERITY_MESSAGE:
                HE_CORE_TRACE("DX12 Debug Layer: {0}", text);
                break;
            }
        }

        std::string HResultToString(HRESULT result)
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

        [[noreturn]] void ThrowHResultFailure(
            std::string_view operation,
            HRESULT result,
            ID3D12Device* device = nullptr)
        {
            std::ostringstream stream;
            stream << operation << " failed with " << HResultToString(result);

            if (device != nullptr)
            {
                const HRESULT removalReason = device->GetDeviceRemovedReason();
                if (FAILED(removalReason))
                {
                    stream << "; device removed reason: " << HResultToString(removalReason);
                    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
                    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred))))
                    {
                        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
                        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
                        {
                            uint32_t nodes = 0;
                            for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node && nodes < 6;
                                node = node->pNext, ++nodes)
                            {
                                const uint32_t completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                                stream << "; DRED list=" << (node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "unnamed")
                                    << " completed=" << completed << '/' << node->BreadcrumbCount << " nearbyOps=";
                                const uint32_t begin = completed > 3 ? completed - 3 : 0;
                                const uint32_t end = std::min(node->BreadcrumbCount, completed + 5);
                                if (node->pCommandHistory)
                                    for (uint32_t index = begin; index < end; ++index)
                                        stream << index << ':' << static_cast<uint32_t>(node->pCommandHistory[index]) << ',';
                            }
                        }
                        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
                        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA != 0)
                            stream << "; DRED pageFaultVA=" << fault.PageFaultVA;
                    }
                }
            }

            throw std::runtime_error(stream.str());
        }

        void CheckHResult(std::string_view operation, HRESULT result, ID3D12Device* device = nullptr)
        {
            if (FAILED(result))
                ThrowHResultFailure(operation, result, device);
        }

        void CheckDeviceHealth(std::string_view operation, ID3D12Device* device)
        {
            if (device == nullptr)
                return;

            const HRESULT result = device->GetDeviceRemovedReason();
            if (FAILED(result))
                ThrowHResultFailure(operation, result, device);
        }

        void SetDebugName(ID3D12Object* object, const wchar_t* name)
        {
            if (object != nullptr)
                CheckHResult("ID3D12Object::SetName", object->SetName(name));
        }

        ComPtr<ID3D12CommandQueue> CreateCommandQueue(
            ID3D12Device* device,
            D3D12_COMMAND_LIST_TYPE type,
            const wchar_t* debugName)
        {
            D3D12_COMMAND_QUEUE_DESC description = {};
            description.Type = type;
            description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            description.NodeMask = 0;

            ComPtr<ID3D12CommandQueue> queue;
            CheckHResult("ID3D12Device::CreateCommandQueue", device->CreateCommandQueue(
                &description, IID_PPV_ARGS(&queue)), device);
            SetDebugName(queue.Get(), debugName);
            return queue;
        }

        void SignalAndWaitForQueue(
            ID3D12CommandQueue* queue,
            ID3D12Fence* fence,
            HANDLE fenceEvent,
            uint64_t& fenceValue,
            ID3D12Device* device)
        {
            if (queue == nullptr || fence == nullptr || fenceEvent == nullptr)
                throw std::logic_error("DX12 queue-drain synchronization is not initialized");
            if (fenceValue == std::numeric_limits<uint64_t>::max())
                throw std::overflow_error("DX12 queue-drain fence value overflow");

            ++fenceValue;
            CheckHResult("ID3D12CommandQueue::Signal", queue->Signal(fence, fenceValue), device);
            if (fence->GetCompletedValue() < fenceValue)
            {
                CheckHResult("ID3D12Fence::SetEventOnCompletion", fence->SetEventOnCompletion(
                    fenceValue, fenceEvent), device);
                const DWORD waitResult = WaitForSingleObject(fenceEvent, INFINITE);
                if (waitResult != WAIT_OBJECT_0)
                {
                    const HRESULT result = waitResult == WAIT_FAILED
                        ? HRESULT_FROM_WIN32(GetLastError())
                        : E_FAIL;
                    ThrowHResultFailure("WaitForSingleObject(DX12 queue-drain fence)", result, device);
                }
            }
            CheckDeviceHealth("Waiting for the DX12 graphics queue to drain", device);
        }

        class DX12SwapChain final : public SwapChain
        {
        public:
            DX12SwapChain(
                GLFWwindow* window,
                IDXGIFactory6* factory,
                ID3D12Device* nativeDevice,
                ID3D12CommandQueue* graphicsQueue,
                nvrhi::IDevice* nvrhiDevice,
                uint32_t framesInFlight)
                : mNativeDevice(nativeDevice)
                , mNvrhiDevice(nvrhiDevice)
                , mBufferCount(framesInFlight)
            {
                HWND windowHandle = glfwGetWin32Window(window);
                if (windowHandle == nullptr)
                    throw std::runtime_error("glfwGetWin32Window returned a null HWND for the DX12 swap chain");

                int framebufferWidth = 0;
                int framebufferHeight = 0;
                glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
                mWidth = static_cast<uint32_t>(std::max(framebufferWidth, 1));
                mHeight = static_cast<uint32_t>(std::max(framebufferHeight, 1));

                BOOL allowTearing = FALSE;
                if (SUCCEEDED(factory->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &allowTearing,
                    sizeof(allowTearing))))
                {
                    mAllowTearing = allowTearing == TRUE;
                }

                mSwapChainFlags = mAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

                DXGI_SWAP_CHAIN_DESC1 description = {};
                description.Width = mWidth;
                description.Height = mHeight;
                description.Format = c_BackBufferFormat;
                description.Stereo = FALSE;
                description.SampleDesc.Count = 1;
                description.SampleDesc.Quality = 0;
                description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                description.BufferCount = mBufferCount;
                description.Scaling = DXGI_SCALING_STRETCH;
                description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                description.Flags = mSwapChainFlags;

                ComPtr<IDXGISwapChain1> swapChain;
                CheckHResult("IDXGIFactory6::CreateSwapChainForHwnd", factory->CreateSwapChainForHwnd(
                    graphicsQueue,
                    windowHandle,
                    &description,
                    nullptr,
                    nullptr,
                    &swapChain), nativeDevice);
                CheckHResult("IDXGISwapChain1::QueryInterface(IDXGISwapChain4)", swapChain.As(&mSwapChain), nativeDevice);
                CheckHResult("IDXGIFactory6::MakeWindowAssociation", factory->MakeWindowAssociation(
                    windowHandle, DXGI_MWA_NO_ALT_ENTER), nativeDevice);
                const char swapChainName[] = "HEngine DX12 Swap Chain";
                CheckHResult("IDXGISwapChain4::SetPrivateData", mSwapChain->SetPrivateData(
                    WKPDID_D3DDebugObjectName,
                    static_cast<uint32_t>(sizeof(swapChainName) - 1),
                    swapChainName), nativeDevice);

                ImportBackBuffers();
            }

            ~DX12SwapChain() override
            {
                ReleaseBackBuffers();
            }

            void Resize(uint32_t width, uint32_t height) override
            {
                if (width == 0 || height == 0)
                    return;
                if (width == mWidth && height == mHeight)
                    return;

                ReleaseBackBuffers();
                CheckHResult("IDXGISwapChain4::ResizeBuffers", mSwapChain->ResizeBuffers(
                    mBufferCount,
                    width,
                    height,
                    c_BackBufferFormat,
                    mSwapChainFlags), mNativeDevice);

                mWidth = width;
                mHeight = height;
                ImportBackBuffers();
            }

            [[nodiscard]] uint32_t AcquireNextImage() override
            {
                mCurrentBackBuffer = mSwapChain->GetCurrentBackBufferIndex();
                if (mCurrentBackBuffer >= mBufferCount)
                    throw std::runtime_error("DXGI returned an invalid swap-chain backbuffer index");
                return mCurrentBackBuffer;
            }

            void Present(bool bVSync) override
            {
                const uint32_t syncInterval = bVSync ? 1u : 0u;
                const uint32_t presentFlags = !bVSync && mAllowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;
                CheckHResult("IDXGISwapChain4::Present", mSwapChain->Present(syncInterval, presentFlags), mNativeDevice);
                mPresentMode = bVSync
                    ? PresentMode::VSync
                    : PresentMode::Immediate;
            }

            [[nodiscard]] uint32_t GetWidth() const override { return mWidth; }
            [[nodiscard]] uint32_t GetHeight() const override { return mHeight; }
            [[nodiscard]] uint32_t GetBufferCount() const override { return mBufferCount; }
            [[nodiscard]] nvrhi::Format GetFormat() const override { return nvrhi::Format::RGBA8_UNORM; }
            [[nodiscard]] PresentMode GetPresentMode() const override { return mPresentMode; }
            [[nodiscard]] uint32_t GetCurrentImageIndex() const override { return mCurrentBackBuffer; }
            [[nodiscard]] nvrhi::ResourceStates GetCurrentBackBufferInitialState() const override
            {
                return nvrhi::ResourceStates::Present;
            }

            [[nodiscard]] nvrhi::ITexture* GetCurrentBackBuffer() const override
            {
                return mBackBuffers[mCurrentBackBuffer];
            }

            [[nodiscard]] nvrhi::IFramebuffer* GetCurrentFramebuffer() const override
            {
                return mFramebuffers[mCurrentBackBuffer];
            }

        private:
            void ImportBackBuffers()
            {
                for (uint32_t bufferIndex = 0; bufferIndex < mBufferCount; ++bufferIndex)
                {
                    CheckHResult("IDXGISwapChain4::GetBuffer", mSwapChain->GetBuffer(
                        bufferIndex, IID_PPV_ARGS(&mNativeBackBuffers[bufferIndex])), mNativeDevice);

                    const std::wstring nativeName = L"HEngine DX12 Backbuffer " + std::to_wstring(bufferIndex);
                    SetDebugName(mNativeBackBuffers[bufferIndex].Get(), nativeName.c_str());

                    nvrhi::TextureDesc textureDescription;
                    textureDescription.width = mWidth;
                    textureDescription.height = mHeight;
                    textureDescription.format = nvrhi::Format::RGBA8_UNORM;
                    textureDescription.dimension = nvrhi::TextureDimension::Texture2D;
                    textureDescription.isShaderResource = false;
                    textureDescription.isRenderTarget = true;
                    textureDescription.debugName = "HEngine DX12 Backbuffer " + std::to_string(bufferIndex);
                    textureDescription.enableAutomaticStateTracking(nvrhi::ResourceStates::Present);

                    mBackBuffers[bufferIndex] = mNvrhiDevice->createHandleForNativeTexture(
                        nvrhi::ObjectTypes::D3D12_Resource,
                        mNativeBackBuffers[bufferIndex].Get(),
                        textureDescription);
                    if (!mBackBuffers[bufferIndex])
                        throw std::runtime_error("NVRHI failed to import a DX12 swap-chain backbuffer");

                    nvrhi::FramebufferDesc framebufferDescription;
                    framebufferDescription.addColorAttachment(mBackBuffers[bufferIndex]);
                    mFramebuffers[bufferIndex] = mNvrhiDevice->createFramebuffer(framebufferDescription);
                    if (!mFramebuffers[bufferIndex])
                        throw std::runtime_error("NVRHI failed to create a framebuffer for a DX12 backbuffer");
                }

                mCurrentBackBuffer = mSwapChain->GetCurrentBackBufferIndex();
            }

            void ReleaseBackBuffers()
            {
                for (uint32_t bufferIndex = 0; bufferIndex < mBufferCount; ++bufferIndex)
                {
                    mFramebuffers[bufferIndex] = nullptr;
                    mBackBuffers[bufferIndex] = nullptr;
                    mNativeBackBuffers[bufferIndex].Reset();
                }
            }

            ID3D12Device* mNativeDevice = nullptr;
            nvrhi::IDevice* mNvrhiDevice = nullptr;
            ComPtr<IDXGISwapChain4> mSwapChain;
            std::array<ComPtr<ID3D12Resource>, c_BackBufferCount> mNativeBackBuffers;
            std::array<nvrhi::TextureHandle, c_BackBufferCount> mBackBuffers;
            std::array<nvrhi::FramebufferHandle, c_BackBufferCount> mFramebuffers;
            uint32_t mBufferCount = 3;
            uint32_t mWidth = 0;
            uint32_t mHeight = 0;
            uint32_t mCurrentBackBuffer = 0;
            uint32_t mSwapChainFlags = 0;
            bool mAllowTearing = false;
            PresentMode mPresentMode = PresentMode::VSync;
        };

        void NameActiveCommandList(const FrameContext& frame)
        {
            const std::wstring commandListName = L"HEngine DX12 Graphics Command List "
                + std::to_wstring(frame.FrameIndex);
            ID3D12GraphicsCommandList* nativeCommandList = frame.GraphicsCommandList->getNativeObject(
                nvrhi::ObjectTypes::D3D12_GraphicsCommandList);
            SetDebugName(nativeCommandList, commandListName.c_str());

            const std::wstring allocatorName = L"HEngine DX12 Graphics Command Allocator "
                + std::to_wstring(frame.FrameIndex);
            ID3D12CommandAllocator* nativeAllocator = frame.GraphicsCommandList->getNativeObject(
                nvrhi::ObjectTypes::D3D12_CommandAllocator);
            SetDebugName(nativeAllocator, allocatorName.c_str());
        }
    }

    struct DX12RenderBackend::Impl
    {
        DX12NativeCapabilities NativeCapabilities;
        ComPtr<ID3D12Debug> DebugController;
        ComPtr<IDXGIFactory6> Factory;
        ComPtr<IDXGIAdapter4> Adapter;
        ComPtr<ID3D12Device> NativeDevice;
        ComPtr<ID3D12InfoQueue1> InfoQueue;
        DWORD InfoQueueCallbackCookie = 0;
        ComPtr<ID3D12CommandQueue> GraphicsQueue;
        ComPtr<ID3D12CommandQueue> ComputeQueue;
        ComPtr<ID3D12CommandQueue> CopyQueue;
        ComPtr<ID3D12Fence> PresentDrainFence;
        HANDLE PresentDrainEvent = nullptr;
        uint64_t PresentDrainFenceValue = 0;
        nvrhi::d3d12::DeviceHandle NativeNvrhiDevice;
        nvrhi::DeviceHandle Device;
        Scope<DX12SwapChain> SwapChain;
        std::array<FrameContext, c_BackBufferCount> Frames;
        std::array<bool, c_BackBufferCount> CompletionQueryArmed = {};
        FrameContext* ActiveFrame = nullptr;
        std::string TerminalFrameError;
        bool bInitialized = false;
    };

    DX12RenderBackend::DX12RenderBackend()
        : mImpl(CreateScope<Impl>())
    {
    }

    DX12RenderBackend::~DX12RenderBackend()
    {
        try
        {
            Shutdown();
        }
        catch (const std::exception& exception)
        {
            HE_CORE_ERROR("DX12 backend shutdown failed: {0}", exception.what());
        }
    }

    void DX12RenderBackend::Initialize(
        RenderSurface& surface,
        bool bEnableValidation,
        const RenderAdapterSelectionOptions& selection,
        uint32_t framesInFlight)
    {
        if (mImpl->bInitialized)
            throw std::logic_error("DX12RenderBackend is already initialized");
        const auto* windowSurface = dynamic_cast<const CommonWindow*>(&surface);
        if (windowSurface == nullptr || windowSurface->GetGlfwWindow() == nullptr)
            throw std::invalid_argument("DX12RenderBackend requires a GLFW CommonWindow render surface");
        GLFWwindow* window = windowSurface->GetGlfwWindow();

        if (framesInFlight < 2 || framesInFlight > 3)
            throw std::invalid_argument("Frames in flight must be 2 or 3.");

        try
        {
            RequireDX12OperatingSystemBaseline();
            mCapabilities.Backend = RenderBackend::DX12;

            uint32_t factoryFlags = 0;
            if (bEnableValidation)
            {
                ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
                {
                    dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                    dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                }
                const HRESULT debugInterfaceResult = D3D12GetDebugInterface(
                    IID_PPV_ARGS(&mImpl->DebugController));
                if (SUCCEEDED(debugInterfaceResult))
                {
                    mImpl->DebugController->EnableDebugLayer();
                    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                }
                else
                {
                    HE_CORE_WARN(
                        "D3D12 debug layer is unavailable ({}); continuing with NVRHI validation only. Install the Windows Graphics Tools optional feature for native validation.",
                        HResultToString(debugInterfaceResult));
                }
            }

            CheckHResult("CreateDXGIFactory2", CreateDXGIFactory2(
                factoryFlags, IID_PPV_ARGS(&mImpl->Factory)));
            const DX12AdapterSelectionResult selectedAdapter =
                SelectDX12Adapter(mImpl->Factory.Get(), selection);
            mImpl->Adapter = selectedAdapter.Adapter;

            CheckHResult(
                "D3D12CreateDevice(FL11_0 minimum; verify OS, app-local Agility SDK/runtime, and driver)",
                D3D12CreateDevice(
                    mImpl->Adapter.Get(),
                    D3D_FEATURE_LEVEL_11_0,
                    IID_PPV_ARGS(&mImpl->NativeDevice)));
            SetDebugName(mImpl->NativeDevice.Get(), L"HEngine DX12 Device");
            QueryNativeDX12Capabilities(
                mImpl->NativeDevice.Get(),
                selectedAdapter,
                mCapabilities,
                mImpl->NativeCapabilities);

            if (bEnableValidation && SUCCEEDED(mImpl->NativeDevice.As(&mImpl->InfoQueue)))
            {
                D3D12_MESSAGE_SEVERITY deniedSeverities[] = {
                    D3D12_MESSAGE_SEVERITY_INFO,
                    D3D12_MESSAGE_SEVERITY_MESSAGE,
                };
                D3D12_INFO_QUEUE_FILTER filter = {};
                filter.DenyList.NumSeverities = static_cast<uint32_t>(std::size(deniedSeverities));
                filter.DenyList.pSeverityList = deniedSeverities;
                CheckHResult("ID3D12InfoQueue1::PushStorageFilter", mImpl->InfoQueue->PushStorageFilter(&filter),
                    mImpl->NativeDevice.Get());
                CheckHResult("ID3D12InfoQueue1::RegisterMessageCallback", mImpl->InfoQueue->RegisterMessageCallback(
                    DX12MessageCallback,
                    D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                    nullptr,
                    &mImpl->InfoQueueCallbackCookie), mImpl->NativeDevice.Get());
            }

            mImpl->GraphicsQueue = CreateCommandQueue(
                mImpl->NativeDevice.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, L"HEngine DX12 Graphics Queue");
            mImpl->ComputeQueue = CreateCommandQueue(
                mImpl->NativeDevice.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, L"HEngine DX12 Compute Queue");
            mImpl->CopyQueue = CreateCommandQueue(
                mImpl->NativeDevice.Get(), D3D12_COMMAND_LIST_TYPE_COPY, L"HEngine DX12 Copy Queue");
            CheckHResult("ID3D12Device::CreateFence", mImpl->NativeDevice->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mImpl->PresentDrainFence)), mImpl->NativeDevice.Get());
            SetDebugName(mImpl->PresentDrainFence.Get(), L"HEngine DX12 Present Drain Fence");
            mImpl->PresentDrainEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (mImpl->PresentDrainEvent == nullptr)
                ThrowHResultFailure("CreateEventW(DX12 present-drain fence)", HRESULT_FROM_WIN32(GetLastError()));

            nvrhi::d3d12::DeviceDesc deviceDescription;
            deviceDescription.errorCB = this;
            deviceDescription.pDevice = mImpl->NativeDevice.Get();
            deviceDescription.pGraphicsCommandQueue = mImpl->GraphicsQueue.Get();
            deviceDescription.pComputeCommandQueue = mImpl->ComputeQueue.Get();
            deviceDescription.pCopyCommandQueue = mImpl->CopyQueue.Get();
            deviceDescription.renderTargetViewHeapSize = 4096;
            deviceDescription.depthStencilViewHeapSize = 2048;
            deviceDescription.shaderResourceViewHeapSize = 65536;
            deviceDescription.samplerHeapSize = 2048;
            deviceDescription.maxTimerQueries = 1024;
            ConfigureNvrhiDX12Capabilities(
                mImpl->NativeCapabilities,
                deviceDescription);

            mImpl->NativeNvrhiDevice = nvrhi::d3d12::createDevice(deviceDescription);
            if (!mImpl->NativeNvrhiDevice)
                throw std::runtime_error("nvrhi::d3d12::createDevice failed");

            if (bEnableValidation)
            {
                mImpl->Device = nvrhi::validation::createValidationLayer(mImpl->NativeNvrhiDevice);
                if (!mImpl->Device)
                    throw std::runtime_error("nvrhi::validation::createValidationLayer failed");
            }
            else
            {
                mImpl->Device = mImpl->NativeNvrhiDevice;
            }

            mCapabilities.bGraphicsQueue = true;
            QueryNvrhiDX12Capabilities(mImpl->NativeNvrhiDevice.Get(), mCapabilities);
            RequireBaselineDX12Capabilities(mImpl->NativeCapabilities);
            LogDX12Capabilities(mCapabilities, mImpl->NativeCapabilities);

            mImpl->SwapChain = CreateScope<DX12SwapChain>(
                window,
                mImpl->Factory.Get(),
                mImpl->NativeDevice.Get(),
                mImpl->GraphicsQueue.Get(),
                mImpl->Device.Get(),
                framesInFlight);

            for (uint32_t frameIndex = 0; frameIndex < framesInFlight; ++frameIndex)
            {
                FrameContext& frame = mImpl->Frames[frameIndex];
                frame.FrameIndex = frameIndex;
                frame.GraphicsCommandList = mImpl->Device->createCommandList(
                    nvrhi::CommandListParameters()
                        .setEnableImmediateExecution(false)
                        .setQueueType(nvrhi::CommandQueue::Graphics));
                frame.CompletionQuery = mImpl->Device->createEventQuery();
                if (!frame.GraphicsCommandList || !frame.CompletionQuery)
                    throw std::runtime_error("NVRHI failed to create the DX12 frame command list or completion query");
            }

            mCapabilities.bGraphicsQueue = true;
            mCapabilities.bComputeQueue = true;
            mCapabilities.bCopyQueue = true;
            mCapabilities.bValidationLayer = bEnableValidation;
            mCapabilities.FramesInFlight = framesInFlight;
            mImpl->bInitialized = true;

            HE_CORE_INFO(
                "Initialized DX12/NVRHI on {0} ({1} MiB dedicated VRAM), {2} frame(s) in flight, validation: {3}",
                mCapabilities.AdapterName,
                mCapabilities.DedicatedVideoMemory / (1024ull * 1024ull),
                framesInFlight,
                bEnableValidation ? "enabled" : "disabled");
        }
        catch (...)
        {
            mImpl->SwapChain.reset();
            for (FrameContext& frame : mImpl->Frames)
                frame = {};
            mImpl->CompletionQueryArmed.fill(false);
            mImpl->TerminalFrameError.clear();
            mImpl->Device = nullptr;
            mImpl->NativeNvrhiDevice = nullptr;
            mImpl->CopyQueue.Reset();
            mImpl->ComputeQueue.Reset();
            mImpl->GraphicsQueue.Reset();
            mImpl->PresentDrainFence.Reset();
            if (mImpl->PresentDrainEvent != nullptr)
            {
                CloseHandle(mImpl->PresentDrainEvent);
                mImpl->PresentDrainEvent = nullptr;
            }
            if (mImpl->InfoQueue && mImpl->InfoQueueCallbackCookie != 0)
            {
                mImpl->InfoQueue->UnregisterMessageCallback(mImpl->InfoQueueCallbackCookie);
                mImpl->InfoQueueCallbackCookie = 0;
            }
            mImpl->InfoQueue.Reset();
            mImpl->NativeDevice.Reset();
            mImpl->Adapter.Reset();
            mImpl->Factory.Reset();
            mImpl->DebugController.Reset();
            mCapabilities = {};
            throw;
        }
    }

    RenderMemoryBudget DX12RenderBackend::QueryMemoryBudget() const
    {
        RenderMemoryBudget result;
        if (!mImpl || !mImpl->Adapter) return result;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        if (FAILED(mImpl->Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
            return result;
        result.bAvailable = true;
        result.bPerProcess = true;
        result.LocalBudgetBytes = local.Budget;
        result.LocalUsageBytes = local.CurrentUsage;
        result.LocalReservationBytes = local.CurrentReservation;
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
        if (SUCCEEDED(mImpl->Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal)))
        {
            result.bNonLocalAvailable = true;
            result.NonLocalBudgetBytes = nonLocal.Budget;
            result.NonLocalUsageBytes = nonLocal.CurrentUsage;
        }
        return result;
    }

    void DX12RenderBackend::Shutdown()
    {
        if (!mImpl->bInitialized && !mImpl->Device)
            return;

        std::exception_ptr waitError;
        if (mImpl->ActiveFrame != nullptr)
        {
            try
            {
                mImpl->ActiveFrame->GraphicsCommandList->close();
            }
            catch (...)
            {
                waitError = std::current_exception();
            }
            mImpl->ActiveFrame = nullptr;
        }

        if (mImpl->Device)
        {
            try
            {
                if (!mImpl->Device->waitForIdle())
                    CheckDeviceHealth("NVRHI waitForIdle during DX12 shutdown", mImpl->NativeDevice.Get());
                SignalAndWaitForQueue(
                    mImpl->GraphicsQueue.Get(),
                    mImpl->PresentDrainFence.Get(),
                    mImpl->PresentDrainEvent,
                    mImpl->PresentDrainFenceValue,
                    mImpl->NativeDevice.Get());
                CheckDeviceHealth("DX12 shutdown", mImpl->NativeDevice.Get());
                mImpl->Device->runGarbageCollection();
            }
            catch (...)
            {
                if (!waitError)
                    waitError = std::current_exception();
            }
        }

        mImpl->SwapChain.reset();
        for (FrameContext& frame : mImpl->Frames)
            frame = {};
        mImpl->CompletionQueryArmed.fill(false);
        mImpl->Device = nullptr;
        mImpl->NativeNvrhiDevice = nullptr;
        if (mImpl->InfoQueue && mImpl->InfoQueueCallbackCookie != 0)
        {
            mImpl->InfoQueue->UnregisterMessageCallback(mImpl->InfoQueueCallbackCookie);
            mImpl->InfoQueueCallbackCookie = 0;
        }
        mImpl->InfoQueue.Reset();
        mImpl->CopyQueue.Reset();
        mImpl->ComputeQueue.Reset();
        mImpl->PresentDrainFence.Reset();
        if (mImpl->PresentDrainEvent != nullptr)
        {
            CloseHandle(mImpl->PresentDrainEvent);
            mImpl->PresentDrainEvent = nullptr;
        }
        mImpl->GraphicsQueue.Reset();
        mImpl->NativeDevice.Reset();
        mImpl->Adapter.Reset();
        mImpl->Factory.Reset();
        mImpl->DebugController.Reset();
        mImpl->TerminalFrameError.clear();
        mImpl->bInitialized = false;
        mCapabilities = {};

        if (waitError)
            std::rethrow_exception(waitError);
    }

    FrameContext& DX12RenderBackend::BeginFrame()
    {
        if (!mImpl->bInitialized)
            throw std::logic_error("DX12RenderBackend is not initialized");
        if (!mImpl->TerminalFrameError.empty())
        {
            throw std::runtime_error(
                "DX12RenderBackend cannot begin another frame after a terminal frame failure: " +
                mImpl->TerminalFrameError);
        }
        if (mImpl->ActiveFrame != nullptr)
            throw std::logic_error("BeginFrame called before the previous DX12 frame was submitted");

        uint32_t frameIndex = 0;
        {
            HE_PROFILE_SECTION("DX12::AcquireImage");
            ScopedRenderFrameCpuTiming timing(RenderFrameCpuPhase::AcquireImage);
            frameIndex = mImpl->SwapChain->AcquireNextImage();
        }
        FrameContext& frame = mImpl->Frames[frameIndex];
        frame.SwapChainImageIndex = frameIndex;
        if (frame.bInFlight)
        {
            if (!mImpl->CompletionQueryArmed[frameIndex])
            {
                throw std::logic_error(
                    "DX12 frame is in flight without an armed completion query");
            }
            {
                HE_PROFILE_SECTION("DX12::FrameSlotWait");
                ScopedRenderFrameCpuTiming timing(RenderFrameCpuPhase::FrameSlotWait);
                mImpl->Device->waitEventQuery(frame.CompletionQuery);
            }
            CheckDeviceHealth("Waiting for the DX12 frame-in-flight completion query", mImpl->NativeDevice.Get());
            mImpl->Device->resetEventQuery(frame.CompletionQuery);
            mImpl->CompletionQueryArmed[frameIndex] = false;
            frame.bInFlight = false;
            frame.CompletedSubmissionInstance = frame.SubmissionInstance;
        }

        {
            HE_PROFILE_SECTION("DX12::GarbageCollect");
            ScopedRenderFrameCpuTiming timing(RenderFrameCpuPhase::GarbageCollect);
            mImpl->Device->runGarbageCollection();
        }
        frame.bSubmissionIssued = false;
        try
        {
            {
                HE_PROFILE_SECTION("DX12::CommandListOpen");
                ScopedRenderFrameCpuTiming timing(RenderFrameCpuPhase::CommandListSetup);
                frame.GraphicsCommandList->open();
            }
            mImpl->ActiveFrame = &frame;
            NameActiveCommandList(frame);
        }
        catch (const std::exception& exception)
        {
            if (mImpl->ActiveFrame == &frame)
            {
                try
                {
                    frame.GraphicsCommandList->close();
                }
                catch (...)
                {
                }
                mImpl->ActiveFrame = nullptr;
            }
            mImpl->TerminalFrameError =
                "BeginFrame command-list setup failed: " + std::string(exception.what());
            throw;
        }
        catch (...)
        {
            if (mImpl->ActiveFrame == &frame)
            {
                try
                {
                    frame.GraphicsCommandList->close();
                }
                catch (...)
                {
                }
                mImpl->ActiveFrame = nullptr;
            }
            mImpl->TerminalFrameError = "BeginFrame command-list setup failed with an unknown exception";
            throw;
        }
        return frame;
    }

    void DX12RenderBackend::AbortFrame(FrameContext& frame) noexcept
    {
        if (mImpl->ActiveFrame != &frame || frame.bSubmissionIssued)
            return;

        try
        {
            frame.GraphicsCommandList->close();
            // NVRHI command lists are expected to be executed before they are
            // reopened. Aborting must not submit partially recorded work, so
            // replace the closed list instead of reusing it on this frame slot.
            nvrhi::CommandListHandle replacement = mImpl->Device->createCommandList(
                nvrhi::CommandListParameters()
                    .setEnableImmediateExecution(false)
                    .setQueueType(nvrhi::CommandQueue::Graphics));
            if (!replacement)
            {
                throw std::runtime_error(
                    "NVRHI failed to replace an aborted DX12 frame command list");
            }
            frame.GraphicsCommandList = std::move(replacement);
        }
        catch (const std::exception& exception)
        {
            mImpl->TerminalFrameError =
                "AbortFrame failed to close the unsubmitted command list: " +
                std::string(exception.what());
            HE_CORE_ERROR("{0}", mImpl->TerminalFrameError);
        }
        catch (...)
        {
            mImpl->TerminalFrameError =
                "AbortFrame failed to close the unsubmitted command list with an unknown exception";
            HE_CORE_ERROR("{0}", mImpl->TerminalFrameError);
        }
        mImpl->ActiveFrame = nullptr;
    }

    void DX12RenderBackend::SubmitAndPresent(FrameContext& frame, bool bVSync)
    {
        if (!mImpl->bInitialized)
            throw std::logic_error("DX12RenderBackend is not initialized");
        if (mImpl->ActiveFrame != &frame)
            throw std::logic_error("SubmitAndPresent received a FrameContext that is not the active DX12 frame");

        frame.GraphicsCommandList->setTextureState(
            mImpl->SwapChain->GetCurrentBackBuffer(),
            nvrhi::AllSubresources,
            nvrhi::ResourceStates::Present);
        try
        {
            {
                HE_PROFILE_SECTION("DX12::CommandListClose");
                ScopedRenderFrameCpuTiming timing(RenderFrameCpuPhase::CommandListSetup);
                if (frame.FullFrameTimer)
                {
                    // Include the final Present transition before writing end.
                    frame.GraphicsCommandList->commitBarriers();
                    frame.GraphicsCommandList->endTimerQuery(frame.FullFrameTimer);
                    frame.bFullFrameTimerEnded = true;
                }
                frame.GraphicsCommandList->close();
            }
            mImpl->ActiveFrame = nullptr;

            {
                HE_PROFILE_SECTION("DX12::GraphicsSubmit");
                ScopedRenderFrameCpuTiming timing(RenderFrameCpuPhase::Submit);
                frame.SubmissionInstance = mImpl->Device->executeCommandList(
                    frame.GraphicsCommandList, nvrhi::CommandQueue::Graphics);
                // This flag is the transaction commit point. Everything after it
                // must preserve the submitted resource lifetimes even on failure.
                frame.bSubmissionIssued = true;
                frame.bInFlight = true;
                CheckDeviceHealth("Submitting the DX12 graphics command list", mImpl->NativeDevice.Get());
                mImpl->Device->setEventQuery(frame.CompletionQuery, nvrhi::CommandQueue::Graphics);
                mImpl->CompletionQueryArmed[frame.FrameIndex] = true;
            }
            {
                HE_PROFILE_SECTION("DX12::Present");
                ScopedRenderFrameCpuTiming timing(RenderFrameCpuPhase::Present);
                mImpl->SwapChain->Present(bVSync);
            }
            {
                HE_PROFILE_SECTION("DX12::GarbageCollect");
                ScopedRenderFrameCpuTiming timing(RenderFrameCpuPhase::GarbageCollect);
                mImpl->Device->runGarbageCollection();
            }
        }
        catch (const std::exception& exception)
        {
            const std::string phase = frame.bSubmissionIssued
                ? "after graphics submission " + std::to_string(frame.SubmissionInstance)
                : "before graphics submission";

            if (frame.bSubmissionIssued &&
                !mImpl->CompletionQueryArmed[frame.FrameIndex] && mImpl->Device)
            {
                try
                {
                    if (mImpl->Device->waitForIdle())
                    {
                        frame.bInFlight = false;
                        frame.CompletedSubmissionInstance = frame.SubmissionInstance;
                    }
                }
                catch (...)
                {
                    // The backend is terminal below; Shutdown/WaitForIdle owns
                    // the final best-effort drain without reusing this slot.
                }
            }

            mImpl->TerminalFrameError =
                "SubmitAndPresent failed " + phase + ": " + exception.what();
            throw std::runtime_error(mImpl->TerminalFrameError);
        }
        catch (...)
        {
            const std::string phase = frame.bSubmissionIssued
                ? "after graphics submission " + std::to_string(frame.SubmissionInstance)
                : "before graphics submission";
            mImpl->TerminalFrameError =
                "SubmitAndPresent failed " + phase + " with an unknown exception";
            throw std::runtime_error(mImpl->TerminalFrameError);
        }
    }

    void DX12RenderBackend::Resize(uint32_t width, uint32_t height)
    {
        if (!mImpl->bInitialized)
            throw std::logic_error("DX12RenderBackend is not initialized");
        if (!mImpl->TerminalFrameError.empty())
        {
            throw std::runtime_error(
                "Cannot resize DX12 after a terminal frame failure: " +
                mImpl->TerminalFrameError);
        }
        if (width == 0 || height == 0)
            return;
        if (mImpl->ActiveFrame != nullptr)
            throw std::logic_error("Cannot resize the DX12 swap chain while a frame is being recorded");

        WaitForIdle();
        HE_CORE_INFO("DX12 resize: device idle, releasing and resizing swap chain to {0}x{1}", width, height);
        mImpl->SwapChain->Resize(width, height);
        HE_CORE_INFO("DX12 resize: swap-chain backbuffers reimported at {0}x{1}", width, height);
        mImpl->Device->runGarbageCollection();
    }

    void DX12RenderBackend::WaitForIdle()
    {
        if (!mImpl->Device)
            return;
        if (mImpl->ActiveFrame != nullptr)
            throw std::logic_error("Cannot wait for DX12 idle while a frame is being recorded");

        if (!mImpl->Device->waitForIdle())
        {
            CheckDeviceHealth("NVRHI waitForIdle", mImpl->NativeDevice.Get());
            throw std::runtime_error("NVRHI failed to wait for the DX12 device to become idle");
        }
        SignalAndWaitForQueue(
            mImpl->GraphicsQueue.Get(),
            mImpl->PresentDrainFence.Get(),
            mImpl->PresentDrainEvent,
            mImpl->PresentDrainFenceValue,
            mImpl->NativeDevice.Get());
        CheckDeviceHealth("Waiting for the DX12 device to become idle", mImpl->NativeDevice.Get());

        for (FrameContext& frame : mImpl->Frames)
        {
            if (!frame.bInFlight)
                continue;
            if (mImpl->CompletionQueryArmed[frame.FrameIndex])
            {
                mImpl->Device->resetEventQuery(frame.CompletionQuery);
                mImpl->CompletionQueryArmed[frame.FrameIndex] = false;
            }
            frame.bInFlight = false;
            frame.CompletedSubmissionInstance = frame.SubmissionInstance;
        }
        mImpl->Device->runGarbageCollection();
    }

    nvrhi::IDevice* DX12RenderBackend::GetDevice() const
    {
        return mImpl->Device;
    }

    SwapChain& DX12RenderBackend::GetSwapChain()
    {
        if (!mImpl->SwapChain)
            throw std::logic_error("DX12 swap chain is not initialized");
        return *mImpl->SwapChain;
    }

    void DX12RenderBackend::message(nvrhi::MessageSeverity severity, const char* messageText)
    {
        const char* text = messageText != nullptr ? messageText : "<empty NVRHI message>";
        switch (severity)
        {
        case nvrhi::MessageSeverity::Info:
            HE_CORE_INFO("NVRHI: {0}", text);
            break;
        case nvrhi::MessageSeverity::Warning:
            HE_CORE_WARN("NVRHI: {0}", text);
            break;
        case nvrhi::MessageSeverity::Error:
        case nvrhi::MessageSeverity::Fatal:
            HE_CORE_ERROR("NVRHI: {0}", text);
            Log::WriteFallbackError("NVRHI validation error", text);
            break;
        }
    }

}
