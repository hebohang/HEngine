
#pragma once

#include "../LLGI.Buffer.h"
#include "LLGI.BaseVulkan.h"
#include "LLGI.GraphicsVulkan.h"

namespace LLGI
{
class SingleFrameMemoryPoolVulkan;

enum class BufferVulkanAccess
{
	TransferRead,
	TransferWrite,
	ShaderRead,
	ShaderReadWrite,
};

class BufferVulkan : public Buffer
{
private:
	std::shared_ptr<GraphicsVulkan> graphics_;
	std::unique_ptr<InternalBuffer> buffer_;
	void* data = nullptr;
	int32_t size_ = 0;
	int32_t actualSize_ = 0;
	int32_t offset_ = 0;

	// Should specify None for the first time.
	vk::AccessFlags accessFlag_ = vk::AccessFlagBits::eHostRead;

public:
	bool Initialize(GraphicsVulkan* graphics, BufferUsageType usage, int32_t size);
	bool InitializeAsShortTime(GraphicsVulkan* graphics, SingleFrameMemoryPoolVulkan* memoryPool, int32_t size);

	BufferVulkan();
	~BufferVulkan() override;

	void* Lock() override;
	void* Lock(int32_t offset, int32_t size) override;
	void Unlock() override;

	int32_t GetSize() override;
	int32_t GetActualSize() const { return actualSize_; }
	int32_t GetOffset() const { return offset_; }

	vk::Buffer GetBuffer() { return buffer_->buffer(); }

	void ResourceBarrier(vk::CommandBuffer& commandBuffer, BufferVulkanAccess access);
};

} // namespace LLGI
