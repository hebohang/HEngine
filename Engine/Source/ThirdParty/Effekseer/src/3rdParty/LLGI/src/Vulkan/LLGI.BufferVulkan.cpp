#include "LLGI.BufferVulkan.h"
#include "LLGI.SingleFrameMemoryPoolVulkan.h"

namespace LLGI
{
namespace
{
vk::PipelineStageFlags GetPipelineStageFlags(vk::AccessFlags accessFlags)
{
	vk::PipelineStageFlags stageFlags = {};

	if (accessFlags & (vk::AccessFlagBits::eHostRead | vk::AccessFlagBits::eHostWrite))
	{
		stageFlags |= vk::PipelineStageFlagBits::eHost;
	}

	if (accessFlags & (vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite))
	{
		stageFlags |= vk::PipelineStageFlagBits::eTransfer;
	}

	if (accessFlags & (vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite))
	{
		stageFlags |= vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader |
					  vk::PipelineStageFlagBits::eComputeShader;
	}

	if (!stageFlags)
	{
		stageFlags = vk::PipelineStageFlagBits::eTopOfPipe;
	}

	return stageFlags;
}

vk::AccessFlags ToAccessFlags(BufferVulkanAccess access)
{
	switch (access)
	{
	case BufferVulkanAccess::TransferRead:
		return vk::AccessFlagBits::eTransferRead;
	case BufferVulkanAccess::TransferWrite:
		return vk::AccessFlagBits::eTransferWrite;
	case BufferVulkanAccess::ShaderRead:
		return vk::AccessFlagBits::eShaderRead;
	case BufferVulkanAccess::ShaderReadWrite:
		return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	}

	return {};
}
} // namespace

BufferVulkan::BufferVulkan() {}

BufferVulkan::~BufferVulkan() {}

bool BufferVulkan::Initialize(GraphicsVulkan* graphics, BufferUsageType usage, int32_t size)
{
	if (!VerifyUsage(usage))
	{
		return false;
	}

	SafeAddRef(graphics);
	graphics_ = CreateSharedPtr(graphics);

	buffer_ = std::unique_ptr<InternalBuffer>(new InternalBuffer(graphics));

	usage_ = usage;
	size_ = size;
	actualSize_ = size;

	vk::BufferUsageFlags vkUsage = {};

	vk::MemoryPropertyFlags memoryProperty = vk::MemoryPropertyFlagBits::eDeviceLocal;
	if (BitwiseContains(usage, BufferUsageType::MapWrite))
	{
		memoryProperty = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
	}

	if (BitwiseContains(usage, BufferUsageType::MapRead))
	{
		memoryProperty = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
	}

	if (BitwiseContains(usage, BufferUsageType::CopyDst))
	{
		vkUsage |= vk::BufferUsageFlagBits::eTransferDst;
	}

	if (BitwiseContains(usage, BufferUsageType::CopySrc))
	{
		vkUsage |= vk::BufferUsageFlagBits::eTransferSrc;
	}

	if (BitwiseContains(usage, BufferUsageType::Index))
	{
		vkUsage |= vk::BufferUsageFlagBits::eIndexBuffer;
	}

	if (BitwiseContains(usage, BufferUsageType::Vertex))
	{
		vkUsage |= vk::BufferUsageFlagBits::eVertexBuffer;
	}

	if (BitwiseContains(usage, BufferUsageType::StorageRead) || BitwiseContains(usage, BufferUsageType::StorageWrite))
	{
		vkUsage |= vk::BufferUsageFlagBits::eStorageBuffer;
	}

	if (BitwiseContains(usage, BufferUsageType::Constant))
	{
		vkUsage |= vk::BufferUsageFlagBits::eUniformBuffer;
		actualSize_ = static_cast<int32_t>(GetAlignedSize(size, 256)); // buffer size should be multiple of 256
	}

	{
		vk::BufferCreateInfo storageBufferInfo;
		storageBufferInfo.size = actualSize_;
		storageBufferInfo.usage = vkUsage;
		vk::Buffer buffer = graphics_->GetDevice().createBuffer(storageBufferInfo);

		vk::MemoryRequirements memReqs = graphics_->GetDevice().getBufferMemoryRequirements(buffer);
		vk::MemoryAllocateInfo memAlloc;
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = graphics_->GetMemoryTypeIndex(memReqs.memoryTypeBits, memoryProperty);
		vk::DeviceMemory devMem = graphics_->GetDevice().allocateMemory(memAlloc);
		graphics_->GetDevice().bindBufferMemory(buffer, devMem, 0);

		buffer_->Attach(buffer, devMem);
	}

	return true;
}

bool BufferVulkan::InitializeAsShortTime(GraphicsVulkan* graphics, SingleFrameMemoryPoolVulkan* memoryPool, int32_t size)
{
	if (buffer_ == nullptr /* || readbackBuffer_ == nullptr || stagingBuffer_ == nullptr*/)
	{
		SafeAddRef(graphics);
		graphics_ = CreateSharedPtr(graphics);

		if (buffer_ == nullptr)
			buffer_ = std::unique_ptr<InternalBuffer>(new InternalBuffer(graphics_.get()));
	}

	auto alignedSize = static_cast<int32_t>(GetAlignedSize(size, 256));
	BufferVulkan* poolBuffer;
	if (memoryPool->GetConstantBuffer(alignedSize, poolBuffer, offset_))
	{
		buffer_->Attach(poolBuffer->buffer_->buffer(), poolBuffer->buffer_->devMem(), true);
		size_ = size;
		actualSize_ = alignedSize;

		return true;
	}
	else
	{
		return false;
	}
}

void* BufferVulkan::Lock()
{
	data = graphics_->GetDevice().mapMemory(buffer_->devMem(), offset_, actualSize_, vk::MemoryMapFlags());
	return data;
}

void* BufferVulkan::Lock(int32_t offset, int32_t size)
{
	data = graphics_->GetDevice().mapMemory(buffer_->devMem(), offset_ + offset, size, vk::MemoryMapFlags());
	return data;
}

void BufferVulkan::Unlock()
{
	graphics_->GetDevice().unmapMemory(buffer_->devMem());
	if (BitwiseContains(usage_, BufferUsageType::MapWrite))
	{
		accessFlag_ = vk::AccessFlagBits::eHostWrite;
	}
}

int32_t BufferVulkan::GetSize() { return size_; }

void BufferVulkan::ResourceBarrier(vk::CommandBuffer& commandBuffer, BufferVulkanAccess access)
{
	const auto accessFlag = ToAccessFlags(access);
	if (accessFlag_ == accessFlag && !(accessFlag & vk::AccessFlagBits::eShaderWrite))
	{
		return;
	}

	vk::BufferMemoryBarrier bufferBarrier(
		accessFlag_, accessFlag, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, buffer_->buffer(), 0, VK_WHOLE_SIZE);

	commandBuffer.pipelineBarrier(
		GetPipelineStageFlags(accessFlag_), GetPipelineStageFlags(accessFlag), vk::DependencyFlags(), 0, nullptr, 0, &bufferBarrier, 0, nullptr);

	accessFlag_ = accessFlag;
}

} // namespace LLGI
