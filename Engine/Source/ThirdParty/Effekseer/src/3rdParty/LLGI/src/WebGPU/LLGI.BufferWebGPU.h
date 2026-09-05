#pragma once

#include "../LLGI.Buffer.h"
#include "LLGI.BaseWebGPU.h"

namespace LLGI
{
/**
 * TODO : Implement short time buffer
*/
class BufferWebGPU : public Buffer
{
	wgpu::Buffer buffer_ = nullptr;
	wgpu::Device device_ = nullptr;
	wgpu::Instance instance_ = nullptr;
	std::vector<uint8_t> lockedBuffer_;
	int32_t lockedOffset_ = 0;
	int32_t lockedSize_ = 0;
	int32_t size_ = 0;
	int32_t allocatedSize_ = 0;
	int32_t offset_ = 0;

public:
	bool Initialize(wgpu::Device& device, const BufferUsageType usage, const int32_t size, wgpu::Instance instance = nullptr);
	void* Lock() override;
	void* Lock(int32_t offset, int32_t size) override;
	void Unlock() override;

	int32_t GetSize() override;

	int32_t GetOffset() const { return offset_; }
	int32_t GetAllocatedSize() const { return allocatedSize_; }

	wgpu::Buffer& GetBuffer();
};

} // namespace LLGI
