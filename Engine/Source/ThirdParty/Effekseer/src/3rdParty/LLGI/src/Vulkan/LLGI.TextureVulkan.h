
#pragma once

#include "../LLGI.Texture.h"
#include "LLGI.BaseVulkan.h"
#include "LLGI.GraphicsVulkan.h"

namespace LLGI
{

// for Texture2D, RenderTarget, DepthBuffer
class TextureVulkan : public Texture
{
private:
	GraphicsVulkan* graphics_ = nullptr;
	ReferenceObject* owner_ = nullptr;
	vk::Device device_ = nullptr;

	vk::Image image_ = nullptr;
	vk::ImageView view_ = nullptr;
	std::vector<vk::ImageLayout> imageLayouts_;
	vk::DeviceMemory devMem_ = nullptr;
	vk::Format vkTextureFormat_;
	vk::ImageSubresourceRange subresourceRange_;

	Vec3I textureSize;
	TextureParameter parameter_;

	int32_t memorySize = 0;
	vk::DeviceSize cpuBufMemorySize_ = 0;
	std::unique_ptr<InternalBuffer> cpuBuf;
	void* data_ = nullptr;

	bool isExternalResource_ = false;

	void ResetImageLayouts(int32_t count, vk::ImageLayout layout);

public:
	TextureVulkan();
	~TextureVulkan() override;

	bool Initialize(GraphicsVulkan* graphics,
					vk::Device device,
					vk::PhysicalDevice physicalDevice,
					ReferenceObject* owner,
					const TextureParameter& parameter);

	/**
		@brief	initialize as screen
	*/
	bool InitializeAsScreen(const vk::Image& image, const vk::ImageView& imageVew, vk::Format format, const Vec2I& size);

	bool InitializeAsExternal(vk::Device device, const VulkanImageInfo& info, ReferenceObject* owner);

	void* Lock() override;
	void Unlock() override;

	bool GetData(std::vector<uint8_t>& data) override;

	Vec3I GetSize() const { return textureSize; }
	Vec2I GetSizeAs2D() const override;
	const TextureParameter& GetParameter() { return parameter_; }

	const vk::Image& GetImage() const { return image_; }
	const vk::ImageView& GetView() const { return view_; }

	vk::Format GetVulkanFormat() const { return vkTextureFormat_; }
	int32_t GetMemorySize() const { return memorySize; }

	std::vector<vk::ImageLayout> GetImageLayouts() const;

	vk::ImageSubresourceRange GetSubresourceRange() const { return subresourceRange_; }

	void ChangeImageLayout(const vk::ImageLayout& imageLayout);

	void ChangeImageLayout(int32_t mipLevel, const vk::ImageLayout& imageLayout);

	void ResourceBarrier(vk::CommandBuffer& commandBuffer, const vk::ImageLayout& imageLayout);

	void ResourceBarrier(int32_t mipLevel, vk::CommandBuffer& commandBuffer, const vk::ImageLayout& imageLayout);

	/**
		@brief	record commands to generate mipmaps from the top level into a command buffer
		@return	whether commands are recorded
	*/
	bool RecordMipmapGenerationCommands(vk::CommandBuffer& commandBuffer);
};

} // namespace LLGI
