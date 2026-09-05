#include "LLGI.GraphicsWebGPU.h"

#include "LLGI.BufferWebGPU.h"
#include "LLGI.CommandListWebGPU.h"
#include "LLGI.PipelineStateWebGPU.h"
#include "LLGI.RenderPassPipelineStateWebGPU.h"
#include "LLGI.RenderPassWebGPU.h"
#include "LLGI.ShaderWebGPU.h"
#include "LLGI.TextureWebGPU.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace LLGI
{

namespace
{
uint32_t AlignTo(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

uint32_t GetFormatBytesPerPixel(TextureFormatType format)
{
	switch (format)
	{
	case TextureFormatType::R8_UNORM:
		return 1;
	case TextureFormatType::R32G32B32A32_FLOAT:
		return 16;
	default:
		return 4;
	}
}

#if defined(__EMSCRIPTEN__)
void WaitForQueue(wgpu::Queue& queue)
{
	bool completed = false;
	bool succeeded = false;
	queue.OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
							  [&completed, &succeeded](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
								  succeeded = status == wgpu::QueueWorkDoneStatus::Success;
								  completed = true;
							  });

	const double waitStart = emscripten_get_now();
	while (!completed)
	{
		emscripten_sleep(1);
		if (emscripten_get_now() - waitStart > 5000.0)
		{
			break;
		}
	}

	if (!succeeded)
	{
		Log(LogType::Warning, "Timed out or failed while waiting for WebGPU queue completion.");
	}
}
#else
void WaitForQueue(wgpu::Instance& instance, wgpu::Queue& queue)
{
	if (instance == nullptr)
	{
		return;
	}

	bool succeeded = false;
	auto future = queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
											[&succeeded](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
												succeeded = status == wgpu::QueueWorkDoneStatus::Success;
											});
	instance.WaitAny(future, std::numeric_limits<uint64_t>::max());

	if (!succeeded)
	{
		Log(LogType::Warning, "Failed while waiting for WebGPU queue completion.");
	}
}
#endif
} // namespace

class SingleFrameMemoryPoolWebGPU : public SingleFrameMemoryPool
{
	wgpu::Device device_;

	Buffer* CreateBufferInternal(int32_t size) override
	{
		auto obj = new BufferWebGPU();
		if (!obj->Initialize(device_, BufferUsageType::Constant, size))
		{
			SafeRelease(obj);
			return nullptr;
		}
		return obj;
	}

	Buffer* ReinitializeBuffer(Buffer* cb, int32_t size) override
	{
		if (cb != nullptr && cb->GetSize() >= size)
		{
			return cb;
		}
		return CreateBufferInternal(size);
	}

public:
	SingleFrameMemoryPoolWebGPU(wgpu::Device device, int32_t swapBufferCount)
		: SingleFrameMemoryPool(swapBufferCount), device_(device)
	{
	}
};

GraphicsWebGPU::GraphicsWebGPU(wgpu::Device device) : device_(device) { queue_ = device.GetQueue(); }

GraphicsWebGPU::GraphicsWebGPU(wgpu::Device device, wgpu::Instance instance) : device_(device), instance_(instance)
{
	queue_ = device.GetQueue();
}

void GraphicsWebGPU::SetWindowSize(const Vec2I& windowSize) {}

void GraphicsWebGPU::Execute(CommandList* commandList)
{
	auto commandListWgpu = static_cast<CommandListWebGPU*>(commandList);
	auto cb = commandListWgpu->GetCommandBuffer();
	queue_.Submit(1, &cb);
}

void GraphicsWebGPU::WaitFinish()
{
#if defined(__EMSCRIPTEN__)
	if (queue_ != nullptr)
	{
		WaitForQueue(queue_);
	}
#else
	if (queue_ != nullptr && instance_ != nullptr)
	{
		WaitForQueue(instance_, queue_);
	}
	else if (device_ != nullptr)
	{
		device_.Tick();
	}
#endif
}

Buffer* GraphicsWebGPU::CreateBuffer(BufferUsageType usage, int32_t size)
{
	auto obj = new BufferWebGPU();
	if (!obj->Initialize(GetDevice(), usage, size, instance_))
	{
		SafeRelease(obj);
		return nullptr;
	}

	return obj;
}

Shader* GraphicsWebGPU::CreateShader(DataStructure* data, int32_t count)
{
	auto obj = new ShaderWebGPU();
	if (!obj->Initialize(GetDevice(), data, count))
	{
		SafeRelease(obj);
		return nullptr;
	}
	return obj;
}

PipelineState* GraphicsWebGPU::CreatePiplineState()
{
	auto pipelineState = new PipelineStateWebGPU(GetDevice());

	// TODO : error check
	return pipelineState;
}

SingleFrameMemoryPool* GraphicsWebGPU::CreateSingleFrameMemoryPool(int32_t constantBufferPoolSize, int32_t drawingCount)
{
	return new SingleFrameMemoryPoolWebGPU(GetDevice(), 1);
}

CommandList* GraphicsWebGPU::CreateCommandList(SingleFrameMemoryPool* memoryPool)
{
	auto commandList = new CommandListWebGPU(GetDevice());

	// TODO : error check
	return commandList;
}

RenderPass* GraphicsWebGPU::CreateRenderPass(Texture** textures, int32_t textureCount, Texture* depthTexture)
{
	assert(textures != nullptr);
	if (textures == nullptr)
		return nullptr;

	for (int32_t i = 0; i < textureCount; i++)
	{
		assert(textures[i] != nullptr);
		if (textures[i] == nullptr)
			return nullptr;
	}

	auto dt = static_cast<TextureWebGPU*>(depthTexture);

	auto renderPass = new RenderPassWebGPU();
	if (!renderPass->Initialize(textures, textureCount, dt, nullptr, nullptr))
	{
		SafeRelease(renderPass);
	}

	return renderPass;
}

RenderPass*
GraphicsWebGPU::CreateRenderPass(Texture* texture, Texture* resolvedTexture, Texture* depthTexture, Texture* resolvedDepthTexture)
{
	if (texture == nullptr)
		return nullptr;

	auto dt = static_cast<const TextureWebGPU*>(depthTexture);
	auto rt = static_cast<const TextureWebGPU*>(resolvedTexture);
	auto rdt = static_cast<const TextureWebGPU*>(resolvedDepthTexture);

	auto renderPass = new RenderPassWebGPU();
	if (!renderPass->Initialize((&texture), 1, (TextureWebGPU*)dt, (TextureWebGPU*)rt, (TextureWebGPU*)rdt))
	{
		SafeRelease(renderPass);
	}

	return renderPass;
}

Texture* GraphicsWebGPU::CreateTexture(uint64_t id) { return nullptr; }

Texture* GraphicsWebGPU::CreateTexture(const TextureParameter& parameter)
{
	auto obj = new TextureWebGPU();
	if (!obj->Initialize(GetDevice(), parameter, instance_))
	{
		SafeRelease(obj);
		return nullptr;
	}
	return obj;
}

Texture* GraphicsWebGPU::CreateTexture(const TextureInitializationParameter& parameter)
{
	TextureParameter param;
	param.Dimension = 2;
	param.Format = parameter.Format;
	param.MipLevelCount = parameter.MipMapCount;
	param.IsMipmapGenerationEnabled = parameter.MipMapCount > 1;
	param.SampleCount = 1;
	param.Size = {parameter.Size.X, parameter.Size.Y, 1};
	return CreateTexture(param);
}

Texture* GraphicsWebGPU::CreateRenderTexture(const RenderTextureInitializationParameter& parameter)
{
	TextureParameter param;
	param.Dimension = 2;
	param.Format = parameter.Format;
	param.MipLevelCount = 1;
	param.SampleCount = parameter.SamplingCount;
	param.Size = {parameter.Size.X, parameter.Size.Y, 1};
	param.Usage = TextureUsageType::RenderTarget;
	return CreateTexture(param);
}

Texture* GraphicsWebGPU::CreateDepthTexture(const DepthTextureInitializationParameter& parameter)
{
	auto format = TextureFormatType::D32;
	if (parameter.Mode == DepthTextureMode::DepthStencil)
	{
		format = TextureFormatType::D24S8;
	}

	TextureParameter param;
	param.Dimension = 2;
	param.Format = format;
	param.MipLevelCount = 1;
	param.SampleCount = parameter.SamplingCount;
	param.Size = {parameter.Size.X, parameter.Size.Y, 1};
	param.Usage = TextureUsageType::RenderTarget;
	return CreateTexture(param);
}

std::vector<uint8_t> GraphicsWebGPU::CaptureRenderTarget(Texture* renderTarget)
{
	auto texture = static_cast<TextureWebGPU*>(renderTarget);
	if (texture == nullptr)
	{
		return std::vector<uint8_t>();
	}

	const auto size = texture->GetSizeAs2D();
	const auto bytesPerPixel = GetFormatBytesPerPixel(texture->GetFormat());
	const auto unalignedBytesPerRow = static_cast<uint32_t>(size.X) * bytesPerPixel;
	const auto bytesPerRow = AlignTo(unalignedBytesPerRow, 256);
	const auto bufferSize = static_cast<uint64_t>(bytesPerRow) * static_cast<uint32_t>(size.Y);

	wgpu::BufferDescriptor bufferDesc{};
	bufferDesc.size = bufferSize;
	bufferDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
	auto readbackBuffer = device_.CreateBuffer(&bufferDesc);

	wgpu::CommandEncoderDescriptor encoderDesc{};
	auto encoder = device_.CreateCommandEncoder(&encoderDesc);

	wgpu::TexelCopyTextureInfo src{};
	src.texture = texture->GetTexture();
	src.aspect = wgpu::TextureAspect::All;

	wgpu::TexelCopyBufferInfo dst{};
	dst.buffer = readbackBuffer;
	dst.layout.offset = 0;
	dst.layout.bytesPerRow = bytesPerRow;
	dst.layout.rowsPerImage = static_cast<uint32_t>(size.Y);

	wgpu::Extent3D extent{};
	extent.width = static_cast<uint32_t>(size.X);
	extent.height = static_cast<uint32_t>(size.Y);
	extent.depthOrArrayLayers = 1;
	encoder.CopyTextureToBuffer(&src, &dst, &extent);

	auto commandBuffer = encoder.Finish();
	queue_.Submit(1, &commandBuffer);

	bool completed = false;
	bool succeeded = false;
	auto future = readbackBuffer.MapAsync(wgpu::MapMode::Read,
										  0,
										  bufferSize,
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

	std::vector<uint8_t> ret(static_cast<size_t>(unalignedBytesPerRow) * static_cast<size_t>(size.Y));
	if (!succeeded)
	{
		Log(LogType::Warning, "Timed out or failed while waiting for WebGPU readback.");
		return ret;
	}

	auto mapped = static_cast<const uint8_t*>(readbackBuffer.GetConstMappedRange(0, bufferSize));
	for (int32_t y = 0; y < size.Y; y++)
	{
		memcpy(ret.data() + static_cast<size_t>(y) * unalignedBytesPerRow,
			   mapped + static_cast<size_t>(y) * bytesPerRow,
			   unalignedBytesPerRow);
	}
	readbackBuffer.Unmap();
	return ret;
}

RenderPassPipelineState* GraphicsWebGPU::CreateRenderPassPipelineState(RenderPass* renderPass)
{
	return CreateRenderPassPipelineState(renderPass->GetKey());
}

RenderPassPipelineState* GraphicsWebGPU::CreateRenderPassPipelineState(const RenderPassPipelineStateKey& key)
{
	// already?
	{
		auto it = renderPassPipelineStates_.find(key);

		if (it != renderPassPipelineStates_.end())
		{
			auto ret = it->second;

			if (ret != nullptr)
			{
				auto ptr = ret.get();
				SafeAddRef(ptr);
				return ptr;
			}
		}
	}

	std::shared_ptr<RenderPassPipelineStateWebGPU> ret = LLGI::CreateSharedPtr<>(new RenderPassPipelineStateWebGPU());
	ret->SetKey(key);

	renderPassPipelineStates_[key] = ret;

	{
		auto ptr = ret.get();
		SafeAddRef(ptr);
		return ptr;
	}
}

} // namespace LLGI
