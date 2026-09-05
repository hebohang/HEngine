
#include "LLGI.TextureVulkan.h"

namespace LLGI
{

void TextureVulkan::ResetImageLayouts(int32_t count, vk::ImageLayout layout)
{
	imageLayouts_.resize(count);

	for (int32_t i = 0; i < count; i++)
	{
		imageLayouts_[i] = layout;
	}
}

TextureVulkan::TextureVulkan() {}

TextureVulkan::~TextureVulkan()
{
	if (view_ && type_ != TextureType::Screen)
	{
		device_.destroyImageView(view_);
		view_ = nullptr;
	}

	if (image_)
	{
		if (type_ != TextureType::Screen && !isExternalResource_)
		{
			device_.destroyImage(image_);
			device_.freeMemory(devMem_);
			image_ = nullptr;
		}
	}

	SafeRelease(owner_);
}

bool TextureVulkan::Initialize(GraphicsVulkan* graphics,
							   vk::Device device,
							   vk::PhysicalDevice physicalDevice,
							   ReferenceObject* owner,
							   const TextureParameter& parameter)
{
	if (parameter.Dimension < 2)
	{
		return false;
	}

	owner_ = owner;
	SafeAddRef(owner_);
	device_ = device;
	graphics_ = graphics;

	type_ = TextureType::Color;
	vk::ImageType dimension = vk::ImageType::e2D;

	if (parameter.Dimension == 2)
	{
		dimension = vk::ImageType::e2D;
	}
	else if (parameter.Dimension == 3)
	{
		dimension = vk::ImageType::e3D;
	}

	auto vkFormat = (vk::Format)VulkanHelper::TextureFormatToVkFormat(parameter.Format);
	format_ = parameter.Format;

	vk::ImageUsageFlags resourceUsage = {};

	vk::ImageAspectFlags aspect = {};

	if (IsDepthFormat(parameter.Format))
	{
		resourceUsage =
			resourceUsage | vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;

		aspect = vk::ImageAspectFlagBits::eDepth;

		if (HasStencil(format_))
		{
			aspect = aspect | vk::ImageAspectFlagBits::eStencil;
		}

		type_ = TextureType::Depth;

		vk::FormatProperties formatProps = physicalDevice.getFormatProperties((vk::Format)vkFormat);
		if (!(formatProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment))
		{
			return false;
		}
	}
	else
	{
		aspect = vk::ImageAspectFlagBits::eColor;

		resourceUsage =
			resourceUsage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
	}

	if ((parameter.Usage & TextureUsageType::RenderTarget) != TextureUsageType::NoneFlag)
	{
		resourceUsage = resourceUsage | vk::ImageUsageFlagBits::eColorAttachment;
		type_ = TextureType::Render;
	}

	if (BitwiseContains(parameter.Usage, TextureUsageType::Storage))
	{
		resourceUsage = resourceUsage | vk::ImageUsageFlagBits::eStorage;
	}

	samplingCount_ = parameter.SampleCount;

	int mipmapCount = parameter.MipLevelCount;

	// check whether is mipmap enabled?
	auto properties = physicalDevice.getFormatProperties((vk::Format)vkFormat);
	if (parameter.IsMipmapGenerationEnabled && !(properties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear))
	{
		mipmapCount = 1;
	}

	parameter_ = parameter;
	parameter_.MipLevelCount = mipmapCount;

	auto isArray = (parameter.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag;
	// image
	vk::ImageCreateInfo imageCreateInfo;

	imageCreateInfo.imageType = dimension;
	imageCreateInfo.extent.width = parameter.Size.X;
	imageCreateInfo.extent.height = parameter.Size.Y;
	imageCreateInfo.extent.depth = isArray ? 1 : parameter.Size.Z;
	imageCreateInfo.mipLevels = mipmapCount;
	imageCreateInfo.arrayLayers = isArray ? parameter.Size.Z : 1;
	imageCreateInfo.format = vkFormat;
	imageCreateInfo.tiling = vk::ImageTiling::eOptimal;
	imageCreateInfo.initialLayout = vk::ImageLayout::eUndefined;
	imageCreateInfo.usage = resourceUsage;

	imageCreateInfo.sharingMode = vk::SharingMode::eExclusive;
	imageCreateInfo.samples = (vk::SampleCountFlagBits)samplingCount_;
	imageCreateInfo.flags = (vk::ImageCreateFlagBits)0;

	image_ = device.createImage(imageCreateInfo);

	// calculate size
	const auto preserveDepth = (parameter.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag;
	memorySize = GetTextureMemorySize(format_, parameter.Size, mipmapCount, preserveDepth);

	// create a buffer on cpu
	if (!IsDepthFormat(parameter.Format))
	{
		cpuBuf = std::unique_ptr<InternalBuffer>(new InternalBuffer(graphics_));
		vk::BufferCreateInfo bufferInfo;
		bufferInfo.size = memorySize;
		bufferInfo.usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
		vk::Buffer buffer = device.createBuffer(bufferInfo);

		vk::MemoryRequirements memReqs = device.getBufferMemoryRequirements(buffer);
		cpuBufMemorySize_ = memReqs.size;
		vk::MemoryAllocateInfo memAlloc;
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = GetMemoryTypeIndex(physicalDevice, memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible);
		vk::DeviceMemory devMem = device.allocateMemory(memAlloc);
		device.bindBufferMemory(buffer, devMem, 0);

		cpuBuf->Attach(buffer, devMem);
	}

	// create a buffer on gpu
	{
		vk::MemoryRequirements memReqs = device.getImageMemoryRequirements(image_);
		vk::MemoryAllocateInfo memAlloc;
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = GetMemoryTypeIndex(physicalDevice, memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
		devMem_ = device.allocateMemory(memAlloc);
		device.bindImageMemory(image_, devMem_, 0);
	}

	// create a texture view
	{
		vk::ImageViewCreateInfo imageViewInfo;
		imageViewInfo.image = image_;

		if (parameter.Dimension == 3)
		{
			imageViewInfo.viewType = vk::ImageViewType::e3D;
		}
		else if (isArray)
		{
			imageViewInfo.viewType = vk::ImageViewType::e2DArray;
		}
		else
		{
			imageViewInfo.viewType = vk::ImageViewType::e2D;
		}

		imageViewInfo.format = vkFormat;
		imageViewInfo.subresourceRange.aspectMask = aspect;
		imageViewInfo.subresourceRange.baseMipLevel = 0;
		imageViewInfo.subresourceRange.levelCount = mipmapCount;
		imageViewInfo.subresourceRange.baseArrayLayer = 0;
		imageViewInfo.subresourceRange.layerCount = isArray ? parameter_.Size.Z : 1;
		subresourceRange_ = imageViewInfo.subresourceRange;
		view_ = device.createImageView(imageViewInfo);
	}

	textureSize = parameter.Size;
	mipmapCount_ = mipmapCount;
	vkTextureFormat_ = imageCreateInfo.format;
	format_ = VulkanHelper::VkFormatToTextureFormat(static_cast<VkFormat>(vkTextureFormat_));
	usage_ = parameter_.Usage;

	ResetImageLayouts(mipmapCount_, imageCreateInfo.initialLayout);

	if (!IsDepthFormat(parameter.Format) && graphics_ != nullptr)
	{
		vk::CommandBufferAllocateInfo cmdBufInfo;
		cmdBufInfo.commandPool = graphics_->GetCommandPool();
		cmdBufInfo.level = vk::CommandBufferLevel::ePrimary;
		cmdBufInfo.commandBufferCount = 1;
		auto cmdBuffers = graphics_->GetDevice().allocateCommandBuffersUnique(cmdBufInfo);

		// a texture state must starts from undefined, so the states must be changed with a command buffer
		vk::CommandBufferBeginInfo cmdBufferBeginInfo;
		cmdBuffers[0]->begin(cmdBufferBeginInfo);
		ResourceBarrier(cmdBuffers[0].get(), vk::ImageLayout::eShaderReadOnlyOptimal);
		cmdBuffers[0]->end();
		std::array<vk::SubmitInfo, 1> submitInfos;
		submitInfos[0].commandBufferCount = 1;
		submitInfos[0].pCommandBuffers = &(cmdBuffers[0].get());

		const auto submitResult = graphics_->GetQueue().submit(static_cast<uint32_t>(submitInfos.size()), submitInfos.data(), vk::Fence());

		if (submitResult != vk::Result::eSuccess)
		{
			LLGI::Log(LogType::Error, "Failed to submit");
			return false;
		}

		graphics_->GetQueue().waitIdle();
	}

	return true;
}

bool TextureVulkan::InitializeAsScreen(const vk::Image& image, const vk::ImageView& imageVew, vk::Format format, const Vec2I& size)
{
	type_ = TextureType::Screen;

	this->image_ = image;
	this->view_ = imageVew;
	vkTextureFormat_ = format;
	textureSize = {size.X, size.Y, 1};
	format_ = VulkanHelper::VkFormatToTextureFormat(static_cast<VkFormat>(vkTextureFormat_));
	memorySize = GetTextureMemorySize(format_, {size.X, size.Y, 1});

	subresourceRange_.aspectMask = vk::ImageAspectFlagBits::eColor;
	subresourceRange_.baseArrayLayer = 0;
	subresourceRange_.levelCount = 1;
	subresourceRange_.baseMipLevel = 0;
	subresourceRange_.layerCount = 1;

	isExternalResource_ = true;

	mipmapCount_ = 1;
	ResetImageLayouts(mipmapCount_, vk::ImageLayout::eUndefined);

	return true;
}

bool TextureVulkan::InitializeAsExternal(vk::Device device, const VulkanImageInfo& info, ReferenceObject* owner)
{
	type_ = TextureType::Color;
	textureSize = Vec3I{0, 0, 0};
	device_ = device;

	owner_ = owner;
	SafeAddRef(owner_);

	image_ = static_cast<vk::Image>(info.image);

	vk::ImageViewCreateInfo viewCreateInfo;
	viewCreateInfo.viewType = vk::ImageViewType::e2D;
	viewCreateInfo.format = (vk::Format)info.format;
	viewCreateInfo.components = {vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA};
	viewCreateInfo.subresourceRange.aspectMask = (vk::ImageAspectFlagBits)info.aspect;
	viewCreateInfo.subresourceRange.levelCount = 1;
	viewCreateInfo.subresourceRange.layerCount = 1;
	viewCreateInfo.image = image_;
	view_ = device.createImageView(viewCreateInfo);

	if (!view_)
	{
		return false;
	}

	mipmapCount_ = 1;
	ResetImageLayouts(mipmapCount_, vk::ImageLayout::eUndefined);

	isExternalResource_ = true;

	return true;
}

void* TextureVulkan::Lock()
{
	if (graphics_ == nullptr)
		return nullptr;

	data_ = graphics_->GetDevice().mapMemory(cpuBuf->devMem(), 0, cpuBufMemorySize_, vk::MemoryMapFlags());
	return data_;
}

void TextureVulkan::Unlock()
{
	if (graphics_ == nullptr)
	{
		return;
	}

	vk::MappedMemoryRange mappedRange;
	mappedRange.memory = cpuBuf->devMem();
	mappedRange.offset = 0;
	mappedRange.size = VK_WHOLE_SIZE;
	const auto flushResult = graphics_->GetDevice().flushMappedMemoryRanges(1, &mappedRange);
	if (flushResult != vk::Result::eSuccess)
	{
		LLGI::Log(LogType::Error, "Failed to flush texture upload memory");
		graphics_->GetDevice().unmapMemory(cpuBuf->devMem());
		data_ = nullptr;
		return;
	}
	graphics_->GetDevice().unmapMemory(cpuBuf->devMem());
	data_ = nullptr;

	// copy buffer
	vk::CommandBufferAllocateInfo cmdBufInfo;
	cmdBufInfo.commandPool = graphics_->GetCommandPool();
	cmdBufInfo.level = vk::CommandBufferLevel::ePrimary;
	cmdBufInfo.commandBufferCount = 1;
	vk::CommandBuffer copyCommandBuffer = graphics_->GetDevice().allocateCommandBuffers(cmdBufInfo)[0];

	vk::CommandBufferBeginInfo cmdBufferBeginInfo;

	copyCommandBuffer.begin(cmdBufferBeginInfo);

	auto isArray = (parameter_.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag;

	std::vector<vk::BufferImageCopy> imageRegions;
	size_t srcOffset = 0;
	for (int32_t mipLevel = 0; mipLevel < mipmapCount_; mipLevel++)
	{
		auto mipDataSize = GetTextureMipSize(parameter_.Size, mipLevel, isArray);
		auto mipImageSize = mipDataSize;
		if (isArray)
		{
			mipImageSize.Z = 1;
		}

		vk::BufferImageCopy imageRegion;
		imageRegion.bufferOffset = srcOffset;
		imageRegion.bufferRowLength = 0;
		imageRegion.bufferImageHeight = 0;
		imageRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		imageRegion.imageSubresource.mipLevel = mipLevel;
		imageRegion.imageSubresource.baseArrayLayer = 0;
		imageRegion.imageSubresource.layerCount = isArray ? parameter_.Size.Z : 1;
		imageRegion.imageOffset = vk::Offset3D(0, 0, 0);
		imageRegion.imageExtent = vk::Extent3D(
			static_cast<uint32_t>(mipImageSize.X),
			static_cast<uint32_t>(mipImageSize.Y),
			static_cast<uint32_t>(mipImageSize.Z));
		imageRegions.emplace_back(imageRegion);

		srcOffset += GetTextureMemorySize(format_, mipDataSize);
	}

	vk::ImageSubresourceRange colorSubRange;
	colorSubRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	colorSubRange.levelCount = 1;
	colorSubRange.layerCount = isArray ? parameter_.Size.Z : 1;

	vk::ImageLayout imageLayout = vk::ImageLayout::eTransferDstOptimal;
	ResourceBarrier(copyCommandBuffer, imageLayout);
	copyCommandBuffer.copyBufferToImage(cpuBuf->buffer(), image_, imageLayout, static_cast<uint32_t>(imageRegions.size()), imageRegions.data());
	ResourceBarrier(copyCommandBuffer, vk::ImageLayout::eShaderReadOnlyOptimal);

	// GetIsMipmapGenerationSupportedOnTextureLoad of GraphicsVulkan returns true,
	// so mipmaps must be generated when texture data is transferred.
	RecordMipmapGenerationCommands(copyCommandBuffer);

	copyCommandBuffer.end();

	// submit and wait to execute command
	std::array<vk::SubmitInfo, 1> copySubmitInfos;
	copySubmitInfos[0].commandBufferCount = 1;
	copySubmitInfos[0].pCommandBuffers = &copyCommandBuffer;

	const auto submitResult =
		graphics_->GetQueue().submit(static_cast<uint32_t>(copySubmitInfos.size()), copySubmitInfos.data(), vk::Fence());
	if (submitResult != vk::Result::eSuccess)
	{
		LLGI::Log(LogType::Error, "Failed to submit");
		return;
	}

	graphics_->GetQueue().waitIdle();

	graphics_->GetDevice().freeCommandBuffers(graphics_->GetCommandPool(), copyCommandBuffer);
}

bool TextureVulkan::RecordMipmapGenerationCommands(vk::CommandBuffer& commandBuffer)
{
	if (!parameter_.IsMipmapGenerationEnabled || GetMipmapCount() <= 1 || IsBlockCompressedFormat(parameter_.Format) ||
		BitwiseContains(parameter_.Usage, TextureUsageType::Array))
	{
		return false;
	}

	int32_t mipWidth = GetSizeAs2D().X;
	int32_t mipHeight = GetSizeAs2D().Y;

	for (int32_t i = 1; i < GetMipmapCount(); i++)
	{
		ResourceBarrier(i - 1, commandBuffer, vk::ImageLayout::eTransferSrcOptimal);
		ResourceBarrier(i, commandBuffer, vk::ImageLayout::eTransferDstOptimal);

		vk::ImageBlit blit{};
		blit.srcOffsets[0] = vk::Offset3D(0, 0, 0);
		blit.srcOffsets[1] = vk::Offset3D(mipWidth, mipHeight, 1);
		blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		blit.srcSubresource.mipLevel = i - 1;
		blit.srcSubresource.baseArrayLayer = 0;
		blit.srcSubresource.layerCount = 1;
		blit.dstOffsets[0] = vk::Offset3D(0, 0, 0);
		blit.dstOffsets[1] = vk::Offset3D(mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1);
		blit.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		blit.dstSubresource.mipLevel = i;
		blit.dstSubresource.baseArrayLayer = 0;
		blit.dstSubresource.layerCount = 1;

		commandBuffer.blitImage(image_,
								vk::ImageLayout::eTransferSrcOptimal,
								image_,
								vk::ImageLayout::eTransferDstOptimal,
								1,
								&blit,
								vk::Filter::eLinear);

		mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
		mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
	}

	ResourceBarrier(commandBuffer, vk::ImageLayout::eShaderReadOnlyOptimal);
	return true;
}

bool TextureVulkan::GetData(std::vector<uint8_t>& data)
{
	if (graphics_ == nullptr)
	{
		return false;
	}

	// copy buffer
	vk::CommandBufferAllocateInfo cmdBufInfo;
	cmdBufInfo.commandPool = graphics_->GetCommandPool();
	cmdBufInfo.level = vk::CommandBufferLevel::ePrimary;
	cmdBufInfo.commandBufferCount = 1;
	vk::CommandBuffer copyCommandBuffer = graphics_->GetDevice().allocateCommandBuffers(cmdBufInfo)[0];

	vk::CommandBufferBeginInfo cmdBufferBeginInfo;

	copyCommandBuffer.begin(cmdBufferBeginInfo);

	auto isArray = (parameter_.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag;

	vk::BufferImageCopy imageRegion;

	imageRegion.bufferOffset = 0;
	imageRegion.bufferRowLength = 0;
	imageRegion.bufferImageHeight = 0;

	imageRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
	imageRegion.imageSubresource.mipLevel = 0;
	imageRegion.imageSubresource.baseArrayLayer = 0;
	imageRegion.imageSubresource.layerCount = isArray ? parameter_.Size.Z : 1;

	imageRegion.imageOffset = vk::Offset3D(0, 0, 0);
	imageRegion.imageExtent =
		vk::Extent3D(static_cast<uint32_t>(GetSizeAs2D().X), static_cast<uint32_t>(GetSizeAs2D().Y), isArray ? 1 : parameter_.Size.Z);

	vk::ImageSubresourceRange colorSubRange;
	colorSubRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	colorSubRange.levelCount = 1;
	colorSubRange.layerCount = isArray ? parameter_.Size.Z : 1;

	vk::ImageLayout imageLayout = vk::ImageLayout::eTransferSrcOptimal;
	ResourceBarrier(copyCommandBuffer, imageLayout);
	copyCommandBuffer.copyImageToBuffer(image_, imageLayout, cpuBuf->buffer(), imageRegion);
	ResourceBarrier(copyCommandBuffer, vk::ImageLayout::eShaderReadOnlyOptimal);
	copyCommandBuffer.end();

	// submit and wait to execute command
	std::array<vk::SubmitInfo, 1> copySubmitInfos;
	copySubmitInfos[0].commandBufferCount = 1;
	copySubmitInfos[0].pCommandBuffers = &copyCommandBuffer;

	const auto submitResult =
		graphics_->GetQueue().submit(static_cast<uint32_t>(copySubmitInfos.size()), copySubmitInfos.data(), vk::Fence());
	if (submitResult != vk::Result::eSuccess)
	{
		LLGI::Log(LogType::Error, "Failed to submit");
		return false;
	}

	graphics_->GetQueue().waitIdle();

	graphics_->GetDevice().freeCommandBuffers(graphics_->GetCommandPool(), copyCommandBuffer);

	auto datatemp = graphics_->GetDevice().mapMemory(cpuBuf->devMem(), 0, memorySize, vk::MemoryMapFlags());

	if (datatemp != nullptr)
	{
		const auto baseMemorySize = GetTextureMemorySize(format_, textureSize);
		data.resize(baseMemorySize);
		memcpy(data.data(), datatemp, baseMemorySize);
	}

	graphics_->GetDevice().unmapMemory(cpuBuf->devMem());

	return true;
}

Vec2I TextureVulkan::GetSizeAs2D() const { return {textureSize.X, textureSize.Y}; }

std::vector<vk::ImageLayout> TextureVulkan::GetImageLayouts() const { return imageLayouts_; }

void TextureVulkan::ChangeImageLayout(const vk::ImageLayout& imageLayout)
{
	for (int32_t i = 0; i < mipmapCount_; i++)
	{
		ChangeImageLayout(i, imageLayout);
	}
}

void TextureVulkan::ChangeImageLayout(int32_t mipLevel, const vk::ImageLayout& imageLayout) { imageLayouts_[mipLevel] = imageLayout; }

void TextureVulkan::ResourceBarrier(vk::CommandBuffer& commandBuffer, const vk::ImageLayout& imageLayout)
{
	for (int32_t i = 0; i < mipmapCount_; i++)
	{
		ResourceBarrier(i, commandBuffer, imageLayout);
	}
}

void TextureVulkan::ResourceBarrier(int32_t mipLevel, vk::CommandBuffer& commandBuffer, const vk::ImageLayout& imageLayout)
{
	if (imageLayouts_[mipLevel] == imageLayout)
	{
		return;
	}

	auto subresourceRange = subresourceRange_;
	subresourceRange.baseMipLevel = mipLevel;
	subresourceRange.levelCount = 1;
	SetImageLayout(commandBuffer, image_, imageLayouts_[mipLevel], imageLayout, subresourceRange);
	ChangeImageLayout(mipLevel, imageLayout);
}

} // namespace LLGI
