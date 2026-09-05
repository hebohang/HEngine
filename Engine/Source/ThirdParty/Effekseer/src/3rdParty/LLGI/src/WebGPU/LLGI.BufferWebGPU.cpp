#include "LLGI.BufferWebGPU.h"

#include <chrono>
#include <thread>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace LLGI
{

namespace
{
int32_t AlignTo(int32_t value, int32_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}
} // namespace

bool BufferWebGPU::Initialize(wgpu::Device& device, const BufferUsageType usage, const int32_t size, wgpu::Instance instance)
{
	device_ = device;
	instance_ = instance;

	wgpu::BufferDescriptor desc{};
	allocatedSize_ = BitwiseContains(usage, BufferUsageType::Constant) ? AlignTo(size, 16) : size;
	desc.size = allocatedSize_;
	if (BitwiseContains(usage, BufferUsageType::MapRead))
	{
		desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
	}
	else
	{
		desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc;
	}

	if ((usage & BufferUsageType::Vertex) == BufferUsageType::Vertex)
	{
		desc.usage |= wgpu::BufferUsage::Vertex;
	}

	if ((usage & BufferUsageType::Index) == BufferUsageType::Index)
	{
		desc.usage |= wgpu::BufferUsage::Index;
	}

	if ((usage & BufferUsageType::Constant) == BufferUsageType::Constant)
	{
		desc.usage |= wgpu::BufferUsage::Uniform;
	}

	if ((usage & BufferUsageType::StorageRead) == BufferUsageType::StorageRead ||
		(usage & BufferUsageType::StorageWrite) == BufferUsageType::StorageWrite)
	{
		desc.usage |= wgpu::BufferUsage::Storage;
	}

	buffer_ = device.CreateBuffer(&desc);
	size_ = size;
	usage_ = usage;
	return buffer_ != nullptr;
}

void* BufferWebGPU::Lock() { return Lock(0, GetSize()); }

void* BufferWebGPU::Lock(int32_t offset, int32_t size)
{
	lockedOffset_ = offset;
	lockedSize_ = size;

	if (BitwiseContains(usage_, BufferUsageType::MapRead))
	{
		bool completed = false;
		bool succeeded = false;
		auto future = buffer_.MapAsync(wgpu::MapMode::Read,
									   offset,
									   size,
#if defined(__EMSCRIPTEN__)
									   wgpu::CallbackMode::AllowSpontaneous,
#else
									   instance_ != nullptr ? wgpu::CallbackMode::WaitAnyOnly : wgpu::CallbackMode::AllowProcessEvents,
#endif
									   [&completed, &succeeded](wgpu::MapAsyncStatus status, wgpu::StringView) {
										   succeeded = status == wgpu::MapAsyncStatus::Success;
										   completed = true;
									   });

		if (instance_ != nullptr)
		{
			instance_.WaitAny(future, 5ULL * 1000ULL * 1000ULL * 1000ULL);
		}
		else
		{
#if defined(__EMSCRIPTEN__)
			const double waitStart = emscripten_get_now();
			while (!completed)
			{
				emscripten_sleep(1);
				if (emscripten_get_now() - waitStart > 5000.0)
				{
					break;
				}
			}
#else
			const auto waitStart = std::chrono::steady_clock::now();
			while (!completed)
			{
				device_.Tick();
				if (std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(5))
				{
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
#endif
		}

		return succeeded ? const_cast<void*>(buffer_.GetConstMappedRange(offset, size)) : nullptr;
	}

	lockedBuffer_.resize(size);
	return lockedBuffer_.data();
}

void BufferWebGPU::Unlock()
{
	if (lockedBuffer_.empty())
	{
		if (BitwiseContains(usage_, BufferUsageType::MapRead))
		{
			buffer_.Unmap();
		}
		return;
	}

	device_.GetQueue().WriteBuffer(buffer_, lockedOffset_, lockedBuffer_.data(), lockedSize_);
	lockedBuffer_.clear();
	lockedOffset_ = 0;
	lockedSize_ = 0;
}

int32_t BufferWebGPU::GetSize() { return size_; }

wgpu::Buffer& BufferWebGPU::GetBuffer() { return buffer_; }

} // namespace LLGI
