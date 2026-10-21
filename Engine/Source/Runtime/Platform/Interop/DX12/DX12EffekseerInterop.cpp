#include "Hepch.h"

#include "DX12EffekseerInterop.h"

#include "Runtime/Platform/Interop/EffekseerFrameResources.h"

#include <EffekseerRendererDX12.h>
#include <nvrhi/d3d12.h>

#include <directx/d3d12.h>

#include <climits>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] bool IsDepthFormat(nvrhi::Format format) noexcept
        {
            switch (format)
            {
            case nvrhi::Format::D16:
            case nvrhi::Format::D24S8:
            case nvrhi::Format::D32:
            case nvrhi::Format::D32S8:
                return true;
            default:
                return false;
            }
        }

        // nvrhi::d3d12::convertFormat returns the SRV format. Effekseer uses
        // this value in its PSO, so depth attachments need their DSV format.
        [[nodiscard]] DXGI_FORMAT ConvertDepthFormat(nvrhi::Format format) noexcept
        {
            switch (format)
            {
            case nvrhi::Format::UNKNOWN: return DXGI_FORMAT_UNKNOWN;
            case nvrhi::Format::D16: return DXGI_FORMAT_D16_UNORM;
            case nvrhi::Format::D24S8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case nvrhi::Format::D32: return DXGI_FORMAT_D32_FLOAT;
            case nvrhi::Format::D32S8: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        [[nodiscard]] ID3D12Device* GetNativeDevice(nvrhi::IDevice* device)
        {
            return device != nullptr
                ? device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device)
                : nullptr;
        }

        [[nodiscard]] ID3D12CommandQueue* GetNativeGraphicsQueue(nvrhi::IDevice* device)
        {
            return device != nullptr
                ? device->getNativeQueue(
                    nvrhi::ObjectTypes::D3D12_CommandQueue,
                    nvrhi::CommandQueue::Graphics)
                : nullptr;
        }

        [[nodiscard]] ID3D12GraphicsCommandList* GetNativeCommandList(
            nvrhi::ICommandList* commandList)
        {
            return commandList != nullptr
                ? commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList)
                : nullptr;
        }

        [[nodiscard]] ID3D12Resource* GetNativeTexture(nvrhi::ITexture* texture)
        {
            return texture != nullptr
                ? texture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource)
                : nullptr;
        }

        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetRenderTargetView(
            const nvrhi::FramebufferAttachment& attachment)
        {
            if (!attachment.valid())
                return {};

            return D3D12_CPU_DESCRIPTOR_HANDLE{ attachment.texture->getNativeView(
                nvrhi::ObjectTypes::D3D12_RenderTargetViewDescriptor,
                attachment.format,
                attachment.subresources).integer };
        }

        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE GetDepthView(
            const nvrhi::FramebufferAttachment& attachment)
        {
            if (!attachment.valid())
                return {};

            return D3D12_CPU_DESCRIPTOR_HANDLE{ attachment.texture->getNativeView(
                nvrhi::ObjectTypes::D3D12_DepthStencilViewDescriptor,
                attachment.format,
                attachment.subresources,
                nvrhi::TextureDimension::Unknown,
                attachment.isReadOnly).integer };
        }

        [[nodiscard]] EffekseerRenderer::DepthReconstructionParameter ToEffekseer(
            const DX12EffekseerDepthReconstruction& source) noexcept
        {
            EffekseerRenderer::DepthReconstructionParameter result = {};
            result.DepthBufferScale = source.DepthBufferScale;
            result.DepthBufferOffset = source.DepthBufferOffset;
            result.ProjectionMatrix33 = source.ProjectionMatrix33;
            result.ProjectionMatrix34 = source.ProjectionMatrix34;
            result.ProjectionMatrix43 = source.ProjectionMatrix43;
            result.ProjectionMatrix44 = source.ProjectionMatrix44;
            return result;
        }
    }

    struct DX12EffekseerInterop::Impl
    {
        nvrhi::DeviceHandle Device;
        nvrhi::CommandListHandle ActiveCommandList;
        nvrhi::FramebufferHandle ActiveFramebuffer;
        nvrhi::TextureHandle ActiveDepthSample;
        nvrhi::TextureHandle ActiveBackground;

        Effekseer::Backend::GraphicsDeviceRef GraphicsDevice;
        EffekseerRenderer::RendererRef Renderer;
        EffekseerFrameResources FrameResources;
        Effekseer::RefPtr<EffekseerRenderer::SingleFrameMemoryPool> MemoryPool;
        Effekseer::RefPtr<EffekseerRenderer::CommandList> CommandList;
        uint64_t RecordingStartDrawCount = 0;
        uint64_t RecordingStartDispatchCount = 0;
        Effekseer::Backend::TextureRef DepthTexture;
        Effekseer::Backend::TextureRef BackgroundTexture;

        nvrhi::Format DepthFormat = nvrhi::Format::UNKNOWN;
        DX12EffekseerInteropStatus Status;
    };

    DX12EffekseerInterop::DX12EffekseerInterop()
        : mImpl(std::make_unique<Impl>())
    {
    }

    DX12EffekseerInterop::~DX12EffekseerInterop()
    {
        Shutdown();
    }

    void DX12EffekseerInterop::Initialize(
        nvrhi::IDevice* device,
        const DX12EffekseerInteropDesc& desc)
    {
        if (mImpl->Status.bInitialized)
            throw std::logic_error("DX12 Effekseer interop is already initialized");
        if (device == nullptr)
            throw std::invalid_argument("DX12 Effekseer interop requires a valid NVRHI device");
        if (device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D12)
            throw std::invalid_argument("DX12 Effekseer interop requires an NVRHI D3D12 device");
        if (desc.ColorFormat == nvrhi::Format::UNKNOWN || IsDepthFormat(desc.ColorFormat))
            throw std::invalid_argument("DX12 Effekseer interop requires a valid color target format");
        if (desc.DepthFormat != nvrhi::Format::UNKNOWN && !IsDepthFormat(desc.DepthFormat))
            throw std::invalid_argument("DX12 Effekseer interop depth format is not a depth-stencil format");
        if (desc.FramesInFlight == 0 || desc.FramesInFlight > INT32_MAX)
            throw std::invalid_argument("DX12 Effekseer interop frames-in-flight is out of range");
        if (desc.MaxSpriteCount <= 0)
            throw std::invalid_argument("DX12 Effekseer interop max sprite count must be positive");

        ID3D12Device* nativeDevice = GetNativeDevice(device);
        ID3D12CommandQueue* nativeQueue = GetNativeGraphicsQueue(device);
        if (nativeDevice == nullptr || nativeQueue == nullptr)
        {
            throw std::runtime_error(
                "NVRHI did not expose the existing DX12 device and graphics queue for Effekseer interop");
        }

        try
        {
            mImpl->Device = device;
            mImpl->GraphicsDevice = EffekseerRendererDX12::CreateGraphicsDevice(
                nativeDevice,
                nativeQueue,
                static_cast<int32_t>(desc.FramesInFlight));
            if (mImpl->GraphicsDevice == nullptr)
                throw std::runtime_error("Effekseer DX12 failed to wrap the existing device and graphics queue");

            DXGI_FORMAT colorFormat = nvrhi::d3d12::convertFormat(desc.ColorFormat);
            const DXGI_FORMAT depthFormat = ConvertDepthFormat(desc.DepthFormat);
            if (colorFormat == DXGI_FORMAT_UNKNOWN
                || (desc.DepthFormat != nvrhi::Format::UNKNOWN && depthFormat == DXGI_FORMAT_UNKNOWN))
            {
                throw std::runtime_error("Effekseer DX12 could not convert the requested NVRHI framebuffer formats");
            }

            mImpl->Renderer = EffekseerRendererDX12::Create(
                mImpl->GraphicsDevice,
                &colorFormat,
                1,
                depthFormat,
                desc.bReversedDepth,
                desc.MaxSpriteCount);
            if (mImpl->Renderer == nullptr)
                throw std::runtime_error("Effekseer DX12 renderer creation failed");

            mImpl->FrameResources.Initialize(desc.FramesInFlight);

            mImpl->DepthFormat = desc.DepthFormat;
            mImpl->Status.bInitialized = true;
        }
        catch (...)
        {
            Shutdown();
            throw;
        }
    }

    void DX12EffekseerInterop::BeginHostFrame(uint32_t slot, uint64_t completed)
    {
        mImpl->FrameResources.Begin(slot, completed);
    }

    void DX12EffekseerInterop::NotifyHostFrameSubmitted(uint32_t slot, uint64_t submission) noexcept
    {
        mImpl->FrameResources.Submitted(slot, submission);
    }

    void DX12EffekseerInterop::AbortHostFrame(uint32_t slot) noexcept
    {
        mImpl->FrameResources.Abort(slot);
    }

    void DX12EffekseerInterop::AbortRecording() noexcept
    {
        // Unwind native recording while the host command list is still open.
        // Keep all banks and their in-flight references until GPU completion.
        try
        {
            if (mImpl->Status.bRendering)
                mImpl->Renderer->EndRendering();
        }
        catch (...) {}
        mImpl->Status.bRendering = false;
        try
        {
            if (mImpl->Status.bFrameActive)
                EndFrame();
        }
        catch (...) {}
    }

    void DX12EffekseerInterop::Shutdown() noexcept
    {
        if (mImpl == nullptr)
            return;

        if (mImpl->Status.bRendering && mImpl->Renderer != nullptr)
            mImpl->Renderer->EndRendering();
        if (mImpl->Status.bFrameActive)
        {
            if (mImpl->Renderer != nullptr)
                mImpl->Renderer->SetCommandList(nullptr);
            if (mImpl->CommandList != nullptr)
                EffekseerRendererDX12::EndCommandList(mImpl->CommandList);
            if (mImpl->ActiveCommandList != nullptr)
                mImpl->ActiveCommandList->clearState();
        }

        mImpl->Status = {};
        mImpl->DepthTexture.Reset();
        mImpl->BackgroundTexture.Reset();
        mImpl->ActiveDepthSample = nullptr;
        mImpl->ActiveBackground = nullptr;
        mImpl->ActiveFramebuffer = nullptr;
        mImpl->ActiveCommandList = nullptr;
        mImpl->CommandList.Reset();
        mImpl->MemoryPool.Reset();
        if (mImpl->Renderer != nullptr)
            mImpl->Renderer->GetImpl()->InternalVertexBuffer.reset();
        mImpl->FrameResources.Clear();
        mImpl->Renderer.Reset();
        mImpl->GraphicsDevice.Reset();
        mImpl->Device = nullptr;
        mImpl->DepthFormat = nvrhi::Format::UNKNOWN;
    }

    void DX12EffekseerInterop::BeginComputeFrame(nvrhi::ICommandList* commandList)
    {
        if (!mImpl->Status.bInitialized || mImpl->Status.bFrameActive)
            throw std::logic_error("DX12 Effekseer compute requires initialized, idle interop");
        auto* nativeCommandList = GetNativeCommandList(commandList);
        if (!nativeCommandList)
            throw std::invalid_argument("DX12 Effekseer compute requires an open native command list");
        bool bCommandListActive = false;
        try
        {
            auto& record = mImpl->FrameResources.Acquire(*mImpl->Renderer.Get(), mImpl->GraphicsDevice);
            mImpl->MemoryPool = record.MemoryPool;
            mImpl->CommandList = record.CommandList;
            mImpl->Renderer->GetImpl()->InternalVertexBuffer = record.VertexBuffer;
            mImpl->MemoryPool->NewFrame();
            commandList->commitBarriers();
            EffekseerRendererDX12::BeginCommandList(mImpl->CommandList, nativeCommandList);
            bCommandListActive = true;
            const auto counts = EffekseerRenderer::GetRecordedCommandCounts(mImpl->CommandList);
            mImpl->RecordingStartDrawCount = counts.Draw;
            mImpl->RecordingStartDispatchCount = counts.Dispatch;
            mImpl->Renderer->SetCommandList(mImpl->CommandList);
            mImpl->Renderer->SetDepth(nullptr, {});
            mImpl->Renderer->SetBackground(nullptr);
            mImpl->ActiveCommandList = commandList;
            mImpl->ActiveFramebuffer = nullptr;
            mImpl->Status.bFrameActive = true;
            mImpl->Status.bDepthSampleBound = false;
            mImpl->Status.bBackgroundBound = false;
        }
        catch (...)
        {
            mImpl->Renderer->SetCommandList(nullptr);
            if (bCommandListActive) EffekseerRendererDX12::EndCommandList(mImpl->CommandList);
            mImpl->CommandList.Reset();
            commandList->clearState();
            throw;
        }
    }

    void DX12EffekseerInterop::BeginFrame(const DX12EffekseerFrameDesc& frame)
    {
        if (!mImpl->Status.bInitialized)
            throw std::logic_error("DX12 Effekseer interop BeginFrame called before Initialize");
        if (mImpl->Status.bFrameActive)
            throw std::logic_error("DX12 Effekseer interop frame is already active");
        if (frame.CommandList == nullptr || frame.Framebuffer == nullptr)
            throw std::invalid_argument("DX12 Effekseer interop requires a command list and framebuffer");

        ID3D12GraphicsCommandList* nativeCommandList = GetNativeCommandList(frame.CommandList);
        if (nativeCommandList == nullptr)
            throw std::runtime_error("NVRHI did not expose an open DX12 graphics command list to Effekseer");

        const nvrhi::FramebufferDesc& framebufferDesc = frame.Framebuffer->getDesc();
        if (framebufferDesc.colorAttachments.size() != 1
            || !framebufferDesc.colorAttachments[0].valid())
        {
            throw std::invalid_argument("Effekseer DX12 interop currently requires exactly one color attachment");
        }
        if (frame.Framebuffer->getFramebufferInfo().sampleCount != 1)
            throw std::invalid_argument("Effekseer DX12 interop currently requires single-sample targets");
        if (frame.Framebuffer->getFramebufferInfo().depthFormat != mImpl->DepthFormat)
        {
            throw std::invalid_argument(
                "Effekseer DX12 interop framebuffer depth format does not match renderer initialization");
        }

        if (frame.DepthSampleTexture != nullptr)
        {
            const nvrhi::TextureDesc& depthSampleDesc = frame.DepthSampleTexture->getDesc();
            if (depthSampleDesc.format != nvrhi::Format::R32_FLOAT
                || depthSampleDesc.sampleCount != 1
                || depthSampleDesc.dimension != nvrhi::TextureDimension::Texture2D)
            {
                throw std::invalid_argument(
                    "Effekseer DX12 depth sample must be a single-sample R32_FLOAT Texture2D snapshot");
            }
            if (!framebufferDesc.depthAttachment.valid())
            {
                throw std::invalid_argument(
                    "Effekseer DX12 depth sampling requires a framebuffer depth attachment for depth testing");
            }
            if (!framebufferDesc.depthAttachment.isReadOnly)
            {
                throw std::invalid_argument(
                    "Effekseer DX12 depth sampling requires a read-only framebuffer depth attachment");
            }
        }
        if (frame.BackgroundTexture != nullptr)
        {
            const nvrhi::TextureDesc& backgroundDesc = frame.BackgroundTexture->getDesc();
            if (backgroundDesc.sampleCount != 1
                || backgroundDesc.dimension != nvrhi::TextureDimension::Texture2D)
            {
                throw std::invalid_argument(
                    "Effekseer DX12 background must be a single-sample Texture2D snapshot");
            }
        }

        frame.CommandList->setTextureState(
            framebufferDesc.colorAttachments[0].texture,
            framebufferDesc.colorAttachments[0].subresources,
            nvrhi::ResourceStates::RenderTarget);
        if (framebufferDesc.depthAttachment.valid())
        {
            frame.CommandList->setTextureState(
                framebufferDesc.depthAttachment.texture,
                framebufferDesc.depthAttachment.subresources,
                framebufferDesc.depthAttachment.isReadOnly
                    ? nvrhi::ResourceStates::DepthRead
                    : nvrhi::ResourceStates::DepthWrite);
        }
        if (frame.DepthSampleTexture != nullptr)
        {
            frame.CommandList->setTextureState(
                frame.DepthSampleTexture,
                nvrhi::AllSubresources,
                nvrhi::ResourceStates::ShaderResource);
        }
        if (frame.BackgroundTexture != nullptr)
        {
            frame.CommandList->setTextureState(
                frame.BackgroundTexture,
                nvrhi::AllSubresources,
                nvrhi::ResourceStates::ShaderResource);
        }
        frame.CommandList->commitBarriers();

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetRenderTargetView(
            framebufferDesc.colorAttachments[0]);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = GetDepthView(
            framebufferDesc.depthAttachment);
        if (rtv.ptr == 0 || (framebufferDesc.depthAttachment.valid() && dsv.ptr == 0))
            throw std::runtime_error("NVRHI did not expose DX12 framebuffer descriptors to Effekseer");

        bool bEffekseerCommandListActive = false;
        try
        {
            auto& record = mImpl->FrameResources.Acquire(*mImpl->Renderer.Get(), mImpl->GraphicsDevice);
            mImpl->MemoryPool = record.MemoryPool;
            mImpl->CommandList = record.CommandList;
            mImpl->Renderer->GetImpl()->InternalVertexBuffer = record.VertexBuffer;
            mImpl->MemoryPool->NewFrame();
            EffekseerRendererDX12::BeginCommandList(mImpl->CommandList, nativeCommandList);
            const auto counts = EffekseerRenderer::GetRecordedCommandCounts(mImpl->CommandList);
            mImpl->RecordingStartDrawCount = counts.Draw;
            mImpl->RecordingStartDispatchCount = counts.Dispatch;
            bEffekseerCommandListActive = true;
            mImpl->Renderer->SetCommandList(mImpl->CommandList);

            nativeCommandList->OMSetRenderTargets(
                1,
                &rtv,
                FALSE,
                framebufferDesc.depthAttachment.valid() ? &dsv : nullptr);

            if (frame.DepthSampleTexture != nullptr)
            {
                ID3D12Resource* nativeDepth = GetNativeTexture(frame.DepthSampleTexture);
                if (nativeDepth == nullptr)
                    throw std::runtime_error("NVRHI did not expose the DX12 depth-sample resource");
                mImpl->DepthTexture = EffekseerRendererDX12::CreateTexture(
                    mImpl->GraphicsDevice,
                    nativeDepth);
                if (mImpl->DepthTexture == nullptr)
                    throw std::runtime_error("Effekseer DX12 failed to wrap the depth-sample resource");
                mImpl->Renderer->SetDepth(
                    mImpl->DepthTexture,
                    ToEffekseer(frame.DepthReconstruction));
            }
            else
            {
                mImpl->Renderer->SetDepth(nullptr, {});
            }

            if (frame.BackgroundTexture != nullptr)
            {
                ID3D12Resource* nativeBackground = GetNativeTexture(frame.BackgroundTexture);
                if (nativeBackground == nullptr)
                    throw std::runtime_error("NVRHI did not expose the DX12 Effekseer background resource");
                mImpl->BackgroundTexture = EffekseerRendererDX12::CreateTexture(
                    mImpl->GraphicsDevice,
                    nativeBackground);
                if (mImpl->BackgroundTexture == nullptr)
                    throw std::runtime_error("Effekseer DX12 failed to wrap the background resource");
                mImpl->Renderer->SetBackground(mImpl->BackgroundTexture);
            }
            else
            {
                mImpl->Renderer->SetBackground(nullptr);
            }

            mImpl->ActiveCommandList = frame.CommandList;
            mImpl->ActiveFramebuffer = frame.Framebuffer;
            mImpl->ActiveDepthSample = frame.DepthSampleTexture;
            mImpl->ActiveBackground = frame.BackgroundTexture;
            mImpl->Status.bFrameActive = true;
            mImpl->Status.bDepthSampleBound = frame.DepthSampleTexture != nullptr;
            mImpl->Status.bBackgroundBound = frame.BackgroundTexture != nullptr;
        }
        catch (...)
        {
            mImpl->Renderer->SetDepth(nullptr, {});
            mImpl->Renderer->SetBackground(nullptr);
            mImpl->Renderer->SetCommandList(nullptr);
            if (bEffekseerCommandListActive)
                EffekseerRendererDX12::EndCommandList(mImpl->CommandList);
            frame.CommandList->clearState();
            mImpl->DepthTexture.Reset();
            mImpl->BackgroundTexture.Reset();
            throw;
        }
    }

    void DX12EffekseerInterop::BeginRendering()
    {
        if (!mImpl->ActiveFramebuffer)
            throw std::logic_error("DX12 Effekseer compute-only recording cannot draw");
        if (!mImpl->Status.bFrameActive)
            throw std::logic_error("DX12 Effekseer rendering requires an active interop frame");
        if (mImpl->Status.bRendering)
            throw std::logic_error("DX12 Effekseer rendering is already active");
        if (!mImpl->Renderer->BeginRendering())
            throw std::runtime_error("Effekseer DX12 BeginRendering failed");
        mImpl->Status.bRendering = true;
    }

    void DX12EffekseerInterop::EndRendering()
    {
        if (!mImpl->Status.bRendering)
            throw std::logic_error("DX12 Effekseer EndRendering called without BeginRendering");
        if (!mImpl->Renderer->EndRendering())
            throw std::runtime_error("Effekseer DX12 EndRendering failed");
        mImpl->Status.bRendering = false;
    }

    void DX12EffekseerInterop::EndFrame()
    {
        if (!mImpl->Status.bFrameActive)
            throw std::logic_error("DX12 Effekseer EndFrame called without BeginFrame");
        if (mImpl->Status.bRendering)
            throw std::logic_error("DX12 Effekseer EndFrame called while rendering is active");

        mImpl->Renderer->SetDepth(nullptr, {});
        mImpl->Renderer->SetBackground(nullptr);
        mImpl->Renderer->SetCommandList(nullptr);
        EffekseerRendererDX12::EndCommandList(mImpl->CommandList);

        const auto counts = EffekseerRenderer::GetRecordedCommandCounts(mImpl->CommandList);
        mImpl->Status.RecordedDrawCount = counts.Draw - mImpl->RecordingStartDrawCount;
        mImpl->Status.RecordedDispatchCount = counts.Dispatch - mImpl->RecordingStartDispatchCount;
        ++mImpl->Status.RecordingSerial;

        mImpl->ActiveCommandList->clearState();
        mImpl->DepthTexture.Reset();
        mImpl->BackgroundTexture.Reset();
        mImpl->ActiveDepthSample = nullptr;
        mImpl->ActiveBackground = nullptr;
        mImpl->ActiveFramebuffer = nullptr;
        mImpl->ActiveCommandList = nullptr;
        mImpl->Status.bFrameActive = false;
        mImpl->Status.bDepthSampleBound = false;
        mImpl->Status.bBackgroundBound = false;
    }

    bool DX12EffekseerInterop::IsInitialized() const noexcept
    {
        return mImpl != nullptr && mImpl->Status.bInitialized;
    }

    EffekseerRenderer::Renderer* DX12EffekseerInterop::GetRenderer() const noexcept
    {
        return mImpl != nullptr ? mImpl->Renderer.Get() : nullptr;
    }

    const DX12EffekseerInteropStatus& DX12EffekseerInterop::GetStatus() const noexcept
    {
        return mImpl->Status;
    }
}
