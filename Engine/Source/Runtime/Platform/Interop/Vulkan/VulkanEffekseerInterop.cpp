#include "Hepch.h"

#include "VulkanEffekseerInterop.h"

#include "Runtime/Platform/Interop/EffekseerFrameResources.h"
#include "Runtime/Platform/Vulkan/VulkanNativeContext.h"

#include <EffekseerRendererVulkan.h>
#include <nvrhi/vulkan.h>

#include <array>
#include <climits>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace HEngine
{
    namespace
    {
        template<typename Handle>
        [[nodiscard]] Handle GetNativeHandle(nvrhi::Object object) noexcept
        {
            if constexpr (std::is_pointer_v<Handle>)
                return static_cast<Handle>(object.pointer);
            else
                return static_cast<Handle>(object.integer);
        }

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

        [[nodiscard]] bool HasStencil(nvrhi::Format format) noexcept
        {
            return format == nvrhi::Format::D24S8 || format == nvrhi::Format::D32S8;
        }

        [[nodiscard]] VkImage GetNativeImage(nvrhi::ITexture* texture) noexcept
        {
            return texture != nullptr
                ? GetNativeHandle<VkImage>(texture->getNativeObject(nvrhi::ObjectTypes::VK_Image))
                : VK_NULL_HANDLE;
        }

        [[nodiscard]] VkImageView GetNativeAttachmentView(
            const nvrhi::FramebufferAttachment& attachment) noexcept
        {
            if (!attachment.valid())
                return VK_NULL_HANDLE;

            return GetNativeHandle<VkImageView>(attachment.texture->getNativeView(
                nvrhi::ObjectTypes::VK_ImageView,
                attachment.format,
                attachment.subresources,
                nvrhi::TextureDimension::Unknown,
                attachment.isReadOnly));
        }

        [[nodiscard]] EffekseerRenderer::DepthReconstructionParameter ToEffekseer(
            const EffekseerDepthReconstruction& source) noexcept
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

        [[noreturn]] void ThrowVulkanFailure(const char* operation, VkResult result)
        {
            throw std::runtime_error(
                std::string("Effekseer Vulkan ") + operation + " failed: " +
                nvrhi::vulkan::resultToString(result));
        }
    }

    struct VulkanEffekseerInterop::Impl
    {
        struct CacheUse
        {
            uint64_t Generation = 0;
            uint64_t LastSubmission = 0;
        };

        struct FramebufferCacheEntry
        {
            CacheUse Use;
            nvrhi::FramebufferHandle Owner;
            VkFramebuffer Framebuffer = VK_NULL_HANDLE;
            uint32_t Width = 0;
            uint32_t Height = 0;
        };

        struct TextureCacheEntry
        {
            CacheUse Use;
            nvrhi::TextureHandle Owner;
            Effekseer::Backend::TextureRef Texture;
            VkFormat Format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags Aspect = 0;
        };

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

        VkDevice NativeDevice = VK_NULL_HANDLE;
        VkRenderPass RenderPass = VK_NULL_HANDLE;
        VkCommandBuffer ActiveNativeCommandBuffer = VK_NULL_HANDLE;
        VkFramebuffer ActiveNativeFramebuffer = VK_NULL_HANDLE;
        VkExtent2D ActiveExtent = {};
        std::vector<FramebufferCacheEntry> Framebuffers;
        std::vector<TextureCacheEntry> ExternalTextures;
        uint64_t CacheGeneration = 0;
        uint64_t LastCacheSubmission = 0;
        uint32_t CacheRetentionFrames = 1;
        uint32_t CacheFrameSlot = 0;
        bool bCacheFrameActive = false;
        bool bLogCacheRetirement = false;

        void CollectCaches(uint64_t completed)
        {
            const auto expired = [&](const CacheUse& use)
            {
                return use.LastSubmission <= completed &&
                    CacheGeneration > use.Generation &&
                    CacheGeneration - use.Generation > CacheRetentionFrames;
            };
            // No active recording uses these entries. Native framebuffer
            // destruction precedes releasing its owning attachment image views.
            const size_t oldFramebufferCount = Framebuffers.size();
            const size_t oldTextureCount = ExternalTextures.size();
            std::erase_if(Framebuffers, [&](const FramebufferCacheEntry& entry)
            {
                if (!expired(entry.Use)) return false;
                vkDestroyFramebuffer(NativeDevice, entry.Framebuffer, nullptr);
                return true;
            });
            std::erase_if(ExternalTextures, [&](const TextureCacheEntry& entry)
            {
                return expired(entry.Use);
            });
            const size_t removedFramebuffers = oldFramebufferCount - Framebuffers.size();
            const size_t removedTextures = oldTextureCount - ExternalTextures.size();
            if (bLogCacheRetirement && (removedFramebuffers || removedTextures))
            {
                // These are cache Owner counts, not LLGI reference or VRAM totals.
                HE_CORE_INFO("[Effekseer][VulkanCacheRetirement] generation={} completed={} "
                    "removedFramebuffers={} remainingFramebuffers={} removedTextures={} remainingTextures={} ownersOnly=1",
                    CacheGeneration, completed, removedFramebuffers, Framebuffers.size(),
                    removedTextures, ExternalTextures.size());
            }
        }

        nvrhi::Format ColorFormat = nvrhi::Format::UNKNOWN;
        nvrhi::Format DepthFormat = nvrhi::Format::UNKNOWN;
        EffekseerRenderInteropStatus Status;
        bool bNativeRenderPassActive = false;
    };

    VulkanEffekseerInterop::VulkanEffekseerInterop()
        : mImpl(std::make_unique<Impl>())
    {
    }

    VulkanEffekseerInterop::~VulkanEffekseerInterop()
    {
        Shutdown();
    }

    void VulkanEffekseerInterop::Initialize(
        nvrhi::IDevice* device,
        const EffekseerRenderInteropDesc& desc)
    {
        if (mImpl->Status.bInitialized)
            throw std::logic_error("Vulkan Effekseer interop is already initialized");
        if (device == nullptr)
            throw std::invalid_argument("Vulkan Effekseer interop requires a valid NVRHI device");
        if (device->getGraphicsAPI() != nvrhi::GraphicsAPI::VULKAN)
            throw std::invalid_argument("Vulkan Effekseer interop requires an NVRHI Vulkan device");
        if (desc.ColorFormat == nvrhi::Format::UNKNOWN || IsDepthFormat(desc.ColorFormat))
            throw std::invalid_argument("Vulkan Effekseer interop requires a valid color target format");
        if (desc.DepthFormat != nvrhi::Format::UNKNOWN && !IsDepthFormat(desc.DepthFormat))
            throw std::invalid_argument("Vulkan Effekseer interop depth format is not a depth-stencil format");
        if (desc.FramesInFlight == 0 || desc.FramesInFlight > INT32_MAX)
            throw std::invalid_argument("Vulkan Effekseer interop frames-in-flight is out of range");
        if (desc.MaxSpriteCount <= 0)
            throw std::invalid_argument("Vulkan Effekseer interop max sprite count must be positive");
        if (desc.bReversedDepth)
        {
            throw std::invalid_argument(
                "EffekseerRendererVulkan 1.80.6 does not expose reversed-depth initialization");
        }

        const VulkanNativeContext* native = GetVulkanNativeContext(device);
        if (native == nullptr || native->PhysicalDevice == VK_NULL_HANDLE ||
            native->Device == VK_NULL_HANDLE || native->GraphicsQueue == VK_NULL_HANDLE ||
            native->InteropCommandPool == VK_NULL_HANDLE)
        {
            throw std::runtime_error(
                "Vulkan backend did not expose the existing device, graphics queue, and interop command pool");
        }

        try
        {
            mImpl->Device = device;
            mImpl->NativeDevice = native->Device;
            mImpl->ColorFormat = desc.ColorFormat;
            mImpl->DepthFormat = desc.DepthFormat;

            mImpl->GraphicsDevice = EffekseerRendererVulkan::CreateGraphicsDevice(
                native->PhysicalDevice,
                native->Device,
                native->GraphicsQueue,
                native->InteropCommandPool,
                static_cast<int32_t>(desc.FramesInFlight));
            if (mImpl->GraphicsDevice == nullptr)
                throw std::runtime_error("Effekseer Vulkan failed to wrap the existing device and graphics queue");

            const VkFormat colorFormat = nvrhi::vulkan::convertFormat(desc.ColorFormat);
            const VkFormat depthFormat = nvrhi::vulkan::convertFormat(desc.DepthFormat);
            if (colorFormat == VK_FORMAT_UNDEFINED ||
                (desc.DepthFormat != nvrhi::Format::UNKNOWN && depthFormat == VK_FORMAT_UNDEFINED))
            {
                throw std::runtime_error("Effekseer Vulkan could not convert the requested framebuffer formats");
            }

            EffekseerRendererVulkan::RenderPassInformation rendererPass = {};
            rendererPass.DoesPresentToScreen = false;
            rendererPass.RenderTextureFormats[0] = colorFormat;
            rendererPass.RenderTextureCount = 1;
            rendererPass.DepthFormat = depthFormat;
            mImpl->Renderer = EffekseerRendererVulkan::Create(
                mImpl->GraphicsDevice,
                rendererPass,
                desc.MaxSpriteCount);
            if (mImpl->Renderer == nullptr)
                throw std::runtime_error("Effekseer Vulkan renderer creation failed");

            mImpl->FrameResources.Initialize(desc.FramesInFlight);
            mImpl->CacheRetentionFrames = desc.FramesInFlight;
            mImpl->bLogCacheRetirement = false;
#if defined(HE_DEV_AGENT_RUNTIME)
            if (const char* value = std::getenv("HE_VFX_CACHE_RETIREMENT_LOG"))
                mImpl->bLogCacheRetirement = std::string_view(value) == "1";
#endif

            std::array<VkAttachmentDescription, 2> attachments = {};
            attachments[0].format = colorFormat;
            attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkAttachmentReference depthReference = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            const bool bHasDepth = desc.DepthFormat != nvrhi::Format::UNKNOWN;
            if (bHasDepth)
            {
                attachments[1].format = depthFormat;
                attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
                attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachments[1].stencilLoadOp = HasStencil(desc.DepthFormat)
                    ? VK_ATTACHMENT_LOAD_OP_LOAD
                    : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachments[1].stencilStoreOp = HasStencil(desc.DepthFormat)
                    ? VK_ATTACHMENT_STORE_OP_STORE
                    : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            }

            VkSubpassDescription subpass = {};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorReference;
            subpass.pDepthStencilAttachment = bHasDepth ? &depthReference : nullptr;

            VkRenderPassCreateInfo renderPassInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            renderPassInfo.attachmentCount = bHasDepth ? 2u : 1u;
            renderPassInfo.pAttachments = attachments.data();
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            const VkResult renderPassResult = vkCreateRenderPass(
                mImpl->NativeDevice,
                &renderPassInfo,
                nullptr,
                &mImpl->RenderPass);
            if (renderPassResult != VK_SUCCESS)
                ThrowVulkanFailure("render-pass creation", renderPassResult);

            mImpl->Status.bInitialized = true;
        }
        catch (...)
        {
            Shutdown();
            throw;
        }
    }

    void VulkanEffekseerInterop::BeginHostFrame(uint32_t slot, uint64_t completed)
    {
        mImpl->FrameResources.Begin(slot, completed);
        ++mImpl->CacheGeneration;
        mImpl->CacheFrameSlot = slot;
        mImpl->CollectCaches(completed);
        mImpl->bCacheFrameActive = true;
    }

    void VulkanEffekseerInterop::NotifyHostFrameSubmitted(uint32_t slot, uint64_t submission) noexcept
    {
        // All views in the host scope share this publication. A duplicate from
        // an earlier frame must not stamp or close a newer recording scope.
        if (mImpl->bCacheFrameActive && slot == mImpl->CacheFrameSlot &&
            submission > mImpl->LastCacheSubmission)
        {
            for (auto& entry : mImpl->Framebuffers)
                if (entry.Use.Generation == mImpl->CacheGeneration)
                    entry.Use.LastSubmission = submission;
            for (auto& entry : mImpl->ExternalTextures)
                if (entry.Use.Generation == mImpl->CacheGeneration)
                    entry.Use.LastSubmission = submission;
            mImpl->LastCacheSubmission = submission;
            mImpl->bCacheFrameActive = false;
        }
        mImpl->FrameResources.Submitted(slot, submission);
    }

    void VulkanEffekseerInterop::AbortHostFrame(uint32_t slot) noexcept
    {
        // Do not erase LastSubmission: an entry can also belong to an older,
        // still-pending view recording. Aborted uses only age out the CPU cache.
        if (mImpl->bCacheFrameActive && slot == mImpl->CacheFrameSlot)
            mImpl->bCacheFrameActive = false;
        mImpl->FrameResources.Abort(slot);
    }

    void VulkanEffekseerInterop::AbortRecording() noexcept
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

    void VulkanEffekseerInterop::Shutdown() noexcept
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
                EffekseerRendererVulkan::EndCommandList(mImpl->CommandList);
            if (mImpl->bNativeRenderPassActive && mImpl->ActiveNativeCommandBuffer != VK_NULL_HANDLE)
                vkCmdEndRenderPass(mImpl->ActiveNativeCommandBuffer);
            if (mImpl->ActiveCommandList != nullptr)
                mImpl->ActiveCommandList->clearState();
        }

        mImpl->Status = {};
        mImpl->bNativeRenderPassActive = false;
        mImpl->ActiveNativeCommandBuffer = VK_NULL_HANDLE;
        mImpl->DepthTexture.Reset();
        mImpl->BackgroundTexture.Reset();
        mImpl->ActiveDepthSample = nullptr;
        mImpl->ActiveBackground = nullptr;
        mImpl->ActiveFramebuffer = nullptr;
        mImpl->ActiveCommandList = nullptr;
        // Remaining cache entries may own in-flight image views until this
        // GPU-idle shutdown; ordinary stale entries retire at BeginHostFrame.
        mImpl->ExternalTextures.clear();
        mImpl->CommandList.Reset();
        mImpl->MemoryPool.Reset();
        if (mImpl->Renderer != nullptr)
            mImpl->Renderer->GetImpl()->InternalVertexBuffer.reset();
        mImpl->FrameResources.Clear();
        mImpl->Renderer.Reset();
        mImpl->GraphicsDevice.Reset();
        mImpl->Device = nullptr;

        if (mImpl->NativeDevice != VK_NULL_HANDLE)
        {
            for (const Impl::FramebufferCacheEntry& entry : mImpl->Framebuffers)
            {
                if (entry.Framebuffer != VK_NULL_HANDLE)
                    vkDestroyFramebuffer(mImpl->NativeDevice, entry.Framebuffer, nullptr);
            }
            if (mImpl->RenderPass != VK_NULL_HANDLE)
                vkDestroyRenderPass(mImpl->NativeDevice, mImpl->RenderPass, nullptr);
        }
        mImpl->Framebuffers.clear();
        mImpl->CacheGeneration = 0;
        mImpl->LastCacheSubmission = 0;
        mImpl->bCacheFrameActive = false;
        mImpl->RenderPass = VK_NULL_HANDLE;
        mImpl->NativeDevice = VK_NULL_HANDLE;
        mImpl->ColorFormat = nvrhi::Format::UNKNOWN;
        mImpl->DepthFormat = nvrhi::Format::UNKNOWN;
    }

    void VulkanEffekseerInterop::BeginComputeFrame(nvrhi::ICommandList* commandList)
    {
        if (!mImpl->Status.bInitialized || mImpl->Status.bFrameActive || !mImpl->bCacheFrameActive)
            throw std::logic_error("Vulkan Effekseer compute requires an initialized idle interop and host scope");
        if (!commandList)
            throw std::invalid_argument("Vulkan Effekseer compute requires a command list");
        const VkCommandBuffer nativeCommandBuffer = GetNativeHandle<VkCommandBuffer>(
            commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer));
        if (nativeCommandBuffer == VK_NULL_HANDLE)
            throw std::invalid_argument("Vulkan Effekseer compute requires an open native command buffer");
        bool bCommandListActive = false;
        try
        {
            auto& record = mImpl->FrameResources.Acquire(*mImpl->Renderer.Get(), mImpl->GraphicsDevice);
            mImpl->MemoryPool = record.MemoryPool;
            mImpl->CommandList = record.CommandList;
            mImpl->Renderer->GetImpl()->InternalVertexBuffer = record.VertexBuffer;
            mImpl->MemoryPool->NewFrame();
            commandList->commitBarriers();
            EffekseerRendererVulkan::BeginCommandList(mImpl->CommandList, nativeCommandBuffer, false);
            bCommandListActive = true;
            const auto counts = EffekseerRenderer::GetRecordedCommandCounts(mImpl->CommandList);
            mImpl->RecordingStartDrawCount = counts.Draw;
            mImpl->RecordingStartDispatchCount = counts.Dispatch;
            mImpl->Renderer->SetCommandList(mImpl->CommandList);
            mImpl->Renderer->SetDepth(nullptr, {});
            mImpl->Renderer->SetBackground(nullptr);
            mImpl->ActiveCommandList = commandList;
            mImpl->ActiveFramebuffer = nullptr;
            mImpl->ActiveNativeCommandBuffer = nativeCommandBuffer;
            mImpl->ActiveNativeFramebuffer = VK_NULL_HANDLE;
            mImpl->bNativeRenderPassActive = false;
            mImpl->Status.bFrameActive = true;
            mImpl->Status.bDepthSampleBound = false;
            mImpl->Status.bBackgroundBound = false;
        }
        catch (...)
        {
            mImpl->Renderer->SetCommandList(nullptr);
            if (bCommandListActive) EffekseerRendererVulkan::EndCommandList(mImpl->CommandList);
            mImpl->CommandList.Reset();
            commandList->clearState();
            throw;
        }
    }

    void VulkanEffekseerInterop::BeginFrame(const EffekseerRenderFrameDesc& frame)
    {
        if (!mImpl->Status.bInitialized)
            throw std::logic_error("Vulkan Effekseer interop BeginFrame called before Initialize");
        if (!mImpl->bCacheFrameActive)
            throw std::logic_error("Vulkan Effekseer recording requires a host frame scope");
        if (mImpl->Status.bFrameActive)
            throw std::logic_error("Vulkan Effekseer interop frame is already active");
        if (frame.CommandList == nullptr || frame.Framebuffer == nullptr)
            throw std::invalid_argument("Vulkan Effekseer interop requires a command list and framebuffer");

        const VkCommandBuffer nativeCommandBuffer = GetNativeHandle<VkCommandBuffer>(
            frame.CommandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer));
        if (nativeCommandBuffer == VK_NULL_HANDLE)
            throw std::runtime_error("NVRHI did not expose an open Vulkan graphics command buffer to Effekseer");

        const nvrhi::FramebufferDesc& framebufferDesc = frame.Framebuffer->getDesc();
        const nvrhi::FramebufferInfoEx& framebufferInfo = frame.Framebuffer->getFramebufferInfo();
        if (framebufferDesc.colorAttachments.size() != 1 ||
            !framebufferDesc.colorAttachments[0].valid())
        {
            throw std::invalid_argument("Effekseer Vulkan interop currently requires exactly one color attachment");
        }
        if (framebufferInfo.sampleCount != 1 || framebufferInfo.arraySize != 1)
            throw std::invalid_argument("Effekseer Vulkan interop requires single-sample, non-array targets");
        if (framebufferInfo.width == 0 || framebufferInfo.height == 0)
            throw std::invalid_argument("Effekseer Vulkan interop framebuffer extent must be non-zero");
        if (framebufferInfo.colorFormats[0] != mImpl->ColorFormat ||
            framebufferInfo.depthFormat != mImpl->DepthFormat)
        {
            throw std::invalid_argument(
                "Effekseer Vulkan interop framebuffer formats do not match renderer initialization");
        }

        if (frame.DepthSampleTexture != nullptr)
        {
            const nvrhi::TextureDesc& depthSampleDesc = frame.DepthSampleTexture->getDesc();
            if (depthSampleDesc.format != nvrhi::Format::R32_FLOAT ||
                depthSampleDesc.sampleCount != 1 ||
                depthSampleDesc.dimension != nvrhi::TextureDimension::Texture2D)
            {
                throw std::invalid_argument(
                    "Effekseer Vulkan depth sample must be a single-sample R32_FLOAT Texture2D snapshot");
            }
            if (!framebufferDesc.depthAttachment.valid() || !framebufferDesc.depthAttachment.isReadOnly)
            {
                throw std::invalid_argument(
                    "Effekseer Vulkan depth sampling requires a read-only framebuffer depth attachment");
            }
        }
        if (frame.BackgroundTexture != nullptr)
        {
            const nvrhi::TextureDesc& backgroundDesc = frame.BackgroundTexture->getDesc();
            if (backgroundDesc.sampleCount != 1 ||
                backgroundDesc.dimension != nvrhi::TextureDimension::Texture2D)
            {
                throw std::invalid_argument(
                    "Effekseer Vulkan background must be a single-sample Texture2D snapshot");
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
                nvrhi::ResourceStates::DepthRead);
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

        VkFramebuffer nativeFramebuffer = VK_NULL_HANDLE;
        for (Impl::FramebufferCacheEntry& entry : mImpl->Framebuffers)
        {
            if (entry.Owner.Get() == frame.Framebuffer)
            {
                nativeFramebuffer = entry.Framebuffer;
                entry.Use.Generation = mImpl->CacheGeneration;
                break;
            }
        }
        if (nativeFramebuffer == VK_NULL_HANDLE)
        {
            std::array<VkImageView, 2> views = {
                GetNativeAttachmentView(framebufferDesc.colorAttachments[0]),
                GetNativeAttachmentView(framebufferDesc.depthAttachment),
            };
            if (views[0] == VK_NULL_HANDLE ||
                (framebufferDesc.depthAttachment.valid() && views[1] == VK_NULL_HANDLE))
            {
                throw std::runtime_error("NVRHI did not expose Vulkan framebuffer image views to Effekseer");
            }

            VkFramebufferCreateInfo createInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            createInfo.renderPass = mImpl->RenderPass;
            createInfo.attachmentCount = framebufferDesc.depthAttachment.valid() ? 2u : 1u;
            createInfo.pAttachments = views.data();
            createInfo.width = framebufferInfo.width;
            createInfo.height = framebufferInfo.height;
            createInfo.layers = 1;
            const VkResult framebufferResult = vkCreateFramebuffer(
                mImpl->NativeDevice,
                &createInfo,
                nullptr,
                &nativeFramebuffer);
            if (framebufferResult != VK_SUCCESS)
                ThrowVulkanFailure("framebuffer creation", framebufferResult);

            try
            {
                mImpl->Framebuffers.push_back({
                    .Use = {.Generation = mImpl->CacheGeneration},
                    .Owner = frame.Framebuffer,
                    .Framebuffer = nativeFramebuffer,
                    .Width = framebufferInfo.width,
                    .Height = framebufferInfo.height,
                });
            }
            catch (...)
            {
                vkDestroyFramebuffer(mImpl->NativeDevice, nativeFramebuffer, nullptr);
                throw;
            }
        }

        bool bEffekseerCommandListActive = false;
        try
        {
            auto& record = mImpl->FrameResources.Acquire(*mImpl->Renderer.Get(), mImpl->GraphicsDevice);
            mImpl->MemoryPool = record.MemoryPool;
            mImpl->CommandList = record.CommandList;
            mImpl->Renderer->GetImpl()->InternalVertexBuffer = record.VertexBuffer;
            mImpl->MemoryPool->NewFrame();

            const VkViewport viewport = {
                0.0f,
                0.0f,
                static_cast<float>(framebufferInfo.width),
                static_cast<float>(framebufferInfo.height),
                0.0f,
                1.0f,
            };
            const VkRect2D scissor = { { 0, 0 }, { framebufferInfo.width, framebufferInfo.height } };
            vkCmdSetViewport(nativeCommandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(nativeCommandBuffer, 0, 1, &scissor);

            EffekseerRendererVulkan::BeginCommandList(mImpl->CommandList, nativeCommandBuffer, false);
            const auto counts = EffekseerRenderer::GetRecordedCommandCounts(mImpl->CommandList);
            mImpl->RecordingStartDrawCount = counts.Draw;
            mImpl->RecordingStartDispatchCount = counts.Dispatch;
            bEffekseerCommandListActive = true;
            mImpl->Renderer->SetCommandList(mImpl->CommandList);

            if (frame.DepthSampleTexture != nullptr)
            {
                const VkImage nativeDepth = GetNativeImage(frame.DepthSampleTexture);
                if (nativeDepth == VK_NULL_HANDLE)
                    throw std::runtime_error("NVRHI did not expose the Vulkan depth-sample image");
                for (Impl::TextureCacheEntry& entry : mImpl->ExternalTextures)
                {
                    if (entry.Owner.Get() == frame.DepthSampleTexture &&
                        entry.Format == VK_FORMAT_R32_SFLOAT &&
                        entry.Aspect == VK_IMAGE_ASPECT_COLOR_BIT)
                    {
                        mImpl->DepthTexture = entry.Texture;
                        entry.Use.Generation = mImpl->CacheGeneration;
                        break;
                    }
                }
                if (mImpl->DepthTexture == nullptr)
                {
                    mImpl->DepthTexture = EffekseerRendererVulkan::CreateTexture(
                        mImpl->GraphicsDevice,
                        { nativeDepth, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_R32_SFLOAT });
                    if (mImpl->DepthTexture != nullptr)
                    {
                        mImpl->ExternalTextures.push_back({
                            .Use = {.Generation = mImpl->CacheGeneration},
                            .Owner = frame.DepthSampleTexture,
                            .Texture = mImpl->DepthTexture,
                            .Format = VK_FORMAT_R32_SFLOAT,
                            .Aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                        });
                    }
                }
                if (mImpl->DepthTexture == nullptr)
                    throw std::runtime_error("Effekseer Vulkan failed to wrap the depth-sample image");
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
                const VkImage nativeBackground = GetNativeImage(frame.BackgroundTexture);
                const VkFormat backgroundFormat = nvrhi::vulkan::convertFormat(
                    frame.BackgroundTexture->getDesc().format);
                if (nativeBackground == VK_NULL_HANDLE || backgroundFormat == VK_FORMAT_UNDEFINED)
                    throw std::runtime_error("NVRHI did not expose a usable Vulkan Effekseer background image");
                for (Impl::TextureCacheEntry& entry : mImpl->ExternalTextures)
                {
                    if (entry.Owner.Get() == frame.BackgroundTexture &&
                        entry.Format == backgroundFormat &&
                        entry.Aspect == VK_IMAGE_ASPECT_COLOR_BIT)
                    {
                        mImpl->BackgroundTexture = entry.Texture;
                        entry.Use.Generation = mImpl->CacheGeneration;
                        break;
                    }
                }
                if (mImpl->BackgroundTexture == nullptr)
                {
                    mImpl->BackgroundTexture = EffekseerRendererVulkan::CreateTexture(
                        mImpl->GraphicsDevice,
                        { nativeBackground, VK_IMAGE_ASPECT_COLOR_BIT, backgroundFormat });
                    if (mImpl->BackgroundTexture != nullptr)
                    {
                        mImpl->ExternalTextures.push_back({
                            .Use = {.Generation = mImpl->CacheGeneration},
                            .Owner = frame.BackgroundTexture,
                            .Texture = mImpl->BackgroundTexture,
                            .Format = backgroundFormat,
                            .Aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                        });
                    }
                }
                if (mImpl->BackgroundTexture == nullptr)
                    throw std::runtime_error("Effekseer Vulkan failed to wrap the background image");
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
            mImpl->ActiveNativeCommandBuffer = nativeCommandBuffer;
            mImpl->ActiveNativeFramebuffer = nativeFramebuffer;
            mImpl->ActiveExtent = { framebufferInfo.width, framebufferInfo.height };
            mImpl->bNativeRenderPassActive = false;
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
                EffekseerRendererVulkan::EndCommandList(mImpl->CommandList);
            frame.CommandList->clearState();
            mImpl->DepthTexture.Reset();
            mImpl->BackgroundTexture.Reset();
            throw;
        }
    }

    void VulkanEffekseerInterop::BeginRendering()
    {
        if (!mImpl->ActiveFramebuffer)
            throw std::logic_error("Vulkan Effekseer compute-only recording cannot draw");
        if (!mImpl->Status.bFrameActive)
            throw std::logic_error("Vulkan Effekseer rendering requires an active interop frame");
        if (mImpl->Status.bRendering)
            throw std::logic_error("Vulkan Effekseer rendering is already active");
        // Uploads and compute-to-graphics barriers must finish outside the
        // native render pass, including CPU-only frames with pending assets.
        mImpl->GraphicsDevice->BeginComputePass();
        mImpl->GraphicsDevice->EndComputePass();
        VkRenderPassBeginInfo beginInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        beginInfo.renderPass = mImpl->RenderPass;
        beginInfo.framebuffer = mImpl->ActiveNativeFramebuffer;
        beginInfo.renderArea.extent = mImpl->ActiveExtent;
        vkCmdBeginRenderPass(mImpl->ActiveNativeCommandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        mImpl->bNativeRenderPassActive = true;
        EffekseerRendererVulkan::BeginExternalRenderPass(mImpl->CommandList);
        if (!mImpl->Renderer->BeginRendering())
            throw std::runtime_error("Effekseer Vulkan BeginRendering failed");
        mImpl->Status.bRendering = true;
    }

    void VulkanEffekseerInterop::EndRendering()
    {
        if (!mImpl->Status.bRendering)
            throw std::logic_error("Vulkan Effekseer EndRendering called without BeginRendering");
        if (!mImpl->Renderer->EndRendering())
            throw std::runtime_error("Effekseer Vulkan EndRendering failed");
        EffekseerRendererVulkan::EndExternalRenderPass(mImpl->CommandList);
        vkCmdEndRenderPass(mImpl->ActiveNativeCommandBuffer);
        mImpl->bNativeRenderPassActive = false;
        mImpl->Status.bRendering = false;
    }

    void VulkanEffekseerInterop::EndFrame()
    {
        if (!mImpl->Status.bFrameActive)
            throw std::logic_error("Vulkan Effekseer EndFrame called without BeginFrame");
        if (mImpl->Status.bRendering)
            throw std::logic_error("Vulkan Effekseer EndFrame called while rendering is active");

        if (!mImpl->ActiveFramebuffer)
        {
            mImpl->GraphicsDevice->BeginComputePass();
            mImpl->GraphicsDevice->EndComputePass();
        }

        mImpl->Renderer->SetDepth(nullptr, {});
        mImpl->Renderer->SetBackground(nullptr);
        mImpl->Renderer->SetCommandList(nullptr);
        EffekseerRendererVulkan::EndCommandList(mImpl->CommandList);

        const auto counts = EffekseerRenderer::GetRecordedCommandCounts(mImpl->CommandList);
        mImpl->Status.RecordedDrawCount = counts.Draw - mImpl->RecordingStartDrawCount;
        mImpl->Status.RecordedDispatchCount = counts.Dispatch - mImpl->RecordingStartDispatchCount;
        ++mImpl->Status.RecordingSerial;
        if (mImpl->bNativeRenderPassActive)
            vkCmdEndRenderPass(mImpl->ActiveNativeCommandBuffer);

        mImpl->ActiveCommandList->clearState();
        mImpl->DepthTexture.Reset();
        mImpl->BackgroundTexture.Reset();
        mImpl->ActiveDepthSample = nullptr;
        mImpl->ActiveBackground = nullptr;
        mImpl->ActiveFramebuffer = nullptr;
        mImpl->ActiveCommandList = nullptr;
        mImpl->ActiveNativeCommandBuffer = VK_NULL_HANDLE;
        mImpl->bNativeRenderPassActive = false;
        mImpl->Status.bFrameActive = false;
        mImpl->Status.bDepthSampleBound = false;
        mImpl->Status.bBackgroundBound = false;
    }

    bool VulkanEffekseerInterop::IsInitialized() const noexcept
    {
        return mImpl != nullptr && mImpl->Status.bInitialized;
    }

    EffekseerRenderer::Renderer* VulkanEffekseerInterop::GetRenderer() const noexcept
    {
        return mImpl != nullptr ? mImpl->Renderer.Get() : nullptr;
    }

    const EffekseerRenderInteropStatus& VulkanEffekseerInterop::GetStatus() const noexcept
    {
        return mImpl->Status;
    }
}
