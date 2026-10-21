
#ifndef __EFFEKSEERRENDERER_VULKAN_RENDERER_H__
#define __EFFEKSEERRENDERER_VULKAN_RENDERER_H__

#include "EffekseerRendererVulkan.Base.Pre.h"
#include <EffekseerRendererCommon/EffekseerRenderer.Renderer.h>
#include <functional>

namespace EffekseerRendererVulkan
{

struct VulkanImageInfo
{
	VkImage image;
	VkImageAspectFlags aspect;
	VkFormat format;
};

struct RenderPassInformation
{
	bool DoesPresentToScreen = false;
	std::array<VkFormat, 8> RenderTextureFormats;
	int32_t RenderTextureCount = 1;
	VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
};

::Effekseer::Backend::GraphicsDeviceRef CreateGraphicsDevice(
	VkPhysicalDevice physicalDevice, VkDevice device, VkQueue transfarQueue, VkCommandPool transfarCommandPool, int32_t swapBufferCount);

::EffekseerRenderer::RendererRef
Create(::Effekseer::Backend::GraphicsDeviceRef graphicsDevice, RenderPassInformation renderPassInformation, int32_t squareMaxCount);

/**
@brief	Create an instance
@param transfarQueue   Used in short-time command buffer for immediate data transfer.
@param transfarCommandPool   Used in short-time command buffer for immediate data transfer.
@param squareMaxCount	the number of maximum sprites
@return	instance
*/
::EffekseerRenderer::RendererRef Create(VkPhysicalDevice physicalDevice,
										VkDevice device,
										VkQueue transfarQueue,
										VkCommandPool transfarCommandPool,
										int32_t swapBufferCount,
										RenderPassInformation renderPassInformation,
										int32_t squareMaxCount);

Effekseer::Backend::TextureRef CreateTexture(::Effekseer::Backend::GraphicsDeviceRef graphicsDevice, const VulkanImageInfo& info);

// Set false when recording compute/upload commands before the host render pass.
void BeginCommandList(Effekseer::RefPtr<EffekseerRenderer::CommandList> commandList, VkCommandBuffer nativeCommandList, bool insideRenderPass = true);

void BeginExternalRenderPass(Effekseer::RefPtr<EffekseerRenderer::CommandList> commandList);
void EndExternalRenderPass(Effekseer::RefPtr<EffekseerRenderer::CommandList> commandList);

void EndCommandList(Effekseer::RefPtr<EffekseerRenderer::CommandList> commandList);

} // namespace EffekseerRendererVulkan

#endif
