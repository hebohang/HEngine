#include "LLGI.TextureWebGPU.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace LLGI
{

namespace
{
constexpr uint32_t TextureBytesPerRowAlignment = 256;

uint32_t AlignTo(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

const char* MipmapShaderWGSL = R"(
struct VSOutput {
	@builtin(position) position : vec4f,
	@location(0) uv : vec2f,
};

@vertex
fn VSMain(@builtin(vertex_index) vertexId : u32) -> VSOutput {
	var positions = array<vec2f, 3>(
		vec2f(-1.0, -1.0),
		vec2f(-1.0, 3.0),
		vec2f(3.0, -1.0));
	var uvs = array<vec2f, 3>(
		vec2f(0.0, 1.0),
		vec2f(0.0, -1.0),
		vec2f(2.0, 1.0));

	var output : VSOutput;
	output.position = vec4f(positions[vertexId], 0.0, 1.0);
	output.uv = uvs[vertexId];
	return output;
}

@group(0) @binding(0) var srcSampler : sampler;
@group(0) @binding(1) var srcTexture : texture_2d<f32>;

@fragment
fn PSMain(input : VSOutput) -> @location(0) vec4f {
	return textureSampleLevel(srcTexture, srcSampler, input.uv, 0.0);
}
)";

struct MipmapSize
{
	uint32_t Width = 1;
	uint32_t Height = 1;
};

bool CanGenerateMipMaps(const TextureParameter& parameter)
{
	return parameter.IsMipmapGenerationEnabled &&
		   parameter.MipLevelCount > 1 &&
		   parameter.Dimension == 2 &&
		   parameter.Size.Z == 1 &&
		   !IsBlockCompressedFormat(parameter.Format) &&
		   !IsDepthFormat(parameter.Format) &&
		   parameter.SampleCount == 1;
}

MipmapSize GetMipmapSize(const TextureParameter& parameter, uint32_t mipLevel)
{
	MipmapSize size;
	size.Width = std::max<uint32_t>(static_cast<uint32_t>(parameter.Size.X) >> mipLevel, 1);
	size.Height = std::max<uint32_t>(static_cast<uint32_t>(parameter.Size.Y) >> mipLevel, 1);
	return size;
}

uint32_t GetTextureRowsPerImage(TextureFormatType format, Vec3I size)
{
	if (size.Y <= 0)
	{
		return 0;
	}

	if (IsBlockCompressedFormat(format))
	{
		return static_cast<uint32_t>((size.Y + 3) / 4);
	}

	return static_cast<uint32_t>(size.Y);
}

struct TextureUploadData
{
	const uint8_t* Source = nullptr;
	size_t SourceSize = 0;
	std::vector<uint8_t> AlignedData;

	const uint8_t* Data() const
	{
		return AlignedData.empty() ? Source : AlignedData.data();
	}

	size_t Size() const
	{
		return AlignedData.empty() ? SourceSize : AlignedData.size();
	}
};

TextureUploadData CreateTextureUploadData(const uint8_t* src, uint32_t rowPitch, uint32_t rowCount, uint32_t alignedRowPitch)
{
	TextureUploadData uploadData;
	uploadData.Source = src;
	uploadData.SourceSize = static_cast<size_t>(rowPitch) * rowCount;

	if (rowPitch == alignedRowPitch)
	{
		return uploadData;
	}

	uploadData.AlignedData.resize(static_cast<size_t>(alignedRowPitch) * rowCount);
	for (uint32_t row = 0; row < rowCount; row++)
	{
		memcpy(
			uploadData.AlignedData.data() + static_cast<size_t>(alignedRowPitch) * row,
			src + static_cast<size_t>(rowPitch) * row,
			rowPitch);
	}
	return uploadData;
}

void WriteTextureMipLevel(
	wgpu::Device& device,
	wgpu::Texture texture,
	TextureFormatType format,
	uint32_t mipLevel,
	Vec3I size,
	const uint8_t* data)
{
	const auto rowPitch = static_cast<uint32_t>(GetTextureRowPitch(format, size));
	const auto rowCount = static_cast<uint32_t>(GetTextureRowCount(format, size));
	const auto rowsPerImage = GetTextureRowsPerImage(format, size);
	const auto alignedRowPitch = AlignTo(rowPitch, TextureBytesPerRowAlignment);
	const auto uploadData = CreateTextureUploadData(data, rowPitch, rowCount, alignedRowPitch);

	wgpu::TexelCopyTextureInfo dst{};
	dst.texture = texture;
	dst.mipLevel = mipLevel;
	dst.aspect = wgpu::TextureAspect::All;

	wgpu::TexelCopyBufferLayout layout{};
	layout.bytesPerRow = alignedRowPitch;
	layout.rowsPerImage = rowsPerImage;

	wgpu::Extent3D extent{};
	extent.width = size.X;
	extent.height = size.Y;
	extent.depthOrArrayLayers = size.Z;
	device.GetQueue().WriteTexture(&dst, uploadData.Data(), uploadData.Size(), &layout, &extent);
}

wgpu::Texture CreateMipmapRenderTexture(wgpu::Device& device, wgpu::TextureFormat format, const MipmapSize& size)
{
	wgpu::TextureDescriptor desc{};
	desc.dimension = wgpu::TextureDimension::e2D;
	desc.format = format;
	desc.mipLevelCount = 1;
	desc.sampleCount = 1;
	desc.size.width = size.Width;
	desc.size.height = size.Height;
	desc.size.depthOrArrayLayers = 1;
	desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
	return device.CreateTexture(&desc);
}

wgpu::TextureViewDescriptor CreateMipmapTextureViewDesc(wgpu::TextureFormat format, uint32_t mipLevel, wgpu::TextureUsage usage)
{
	wgpu::TextureViewDescriptor desc{};
	desc.format = format;
	desc.dimension = wgpu::TextureViewDimension::e2D;
	desc.baseMipLevel = mipLevel;
	desc.mipLevelCount = 1;
	desc.baseArrayLayer = 0;
	desc.arrayLayerCount = 1;
	desc.aspect = wgpu::TextureAspect::All;
	desc.usage = usage;
	return desc;
}

wgpu::TexelCopyTextureInfo CreateTextureCopyInfo(wgpu::Texture texture, uint32_t mipLevel)
{
	wgpu::TexelCopyTextureInfo info{};
	info.texture = texture;
	info.mipLevel = mipLevel;
	info.aspect = wgpu::TextureAspect::All;
	return info;
}

wgpu::Extent3D CreateTextureCopySize(const MipmapSize& size)
{
	wgpu::Extent3D copySize{};
	copySize.width = size.Width;
	copySize.height = size.Height;
	copySize.depthOrArrayLayers = 1;
	return copySize;
}

wgpu::RenderPassColorAttachment CreateMipmapColorAttachment(wgpu::TextureView view)
{
	wgpu::RenderPassColorAttachment attachment{};
	attachment.view = view;
	attachment.loadOp = wgpu::LoadOp::Clear;
	attachment.storeOp = wgpu::StoreOp::Store;
	attachment.clearValue = {0, 0, 0, 0};
	return attachment;
}

} // namespace

bool TextureWebGPU::CreateMipmapResources()
{
	if (mipmapShaderModule_ != nullptr)
	{
		return true;
	}

	wgpu::BindGroupLayoutEntry entries[2]{};
	entries[0].binding = 0;
	entries[0].visibility = wgpu::ShaderStage::Fragment;
	entries[0].sampler.type = wgpu::SamplerBindingType::Filtering;
	entries[1].binding = 1;
	entries[1].visibility = wgpu::ShaderStage::Fragment;
	entries[1].texture.sampleType = wgpu::TextureSampleType::Float;
	entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
	entries[1].texture.multisampled = false;

	wgpu::BindGroupLayoutDescriptor bindGroupLayoutDesc{};
	bindGroupLayoutDesc.entryCount = 2;
	bindGroupLayoutDesc.entries = entries;
	mipmapBindGroupLayout_ = device_.CreateBindGroupLayout(&bindGroupLayoutDesc);
	if (mipmapBindGroupLayout_ == nullptr)
	{
		return false;
	}

	wgpu::PipelineLayoutDescriptor pipelineLayoutDesc{};
	pipelineLayoutDesc.bindGroupLayoutCount = 1;
	pipelineLayoutDesc.bindGroupLayouts = &mipmapBindGroupLayout_;
	mipmapPipelineLayout_ = device_.CreatePipelineLayout(&pipelineLayoutDesc);
	if (mipmapPipelineLayout_ == nullptr)
	{
		return false;
	}

	wgpu::SamplerDescriptor samplerDesc{};
	samplerDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
	samplerDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
	samplerDesc.addressModeW = wgpu::AddressMode::ClampToEdge;
	samplerDesc.magFilter = wgpu::FilterMode::Linear;
	samplerDesc.minFilter = wgpu::FilterMode::Linear;
	samplerDesc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
	samplerDesc.lodMinClamp = 0.0f;
	samplerDesc.lodMaxClamp = 32.0f;
	samplerDesc.maxAnisotropy = 1;
	mipmapSampler_ = device_.CreateSampler(&samplerDesc);
	if (mipmapSampler_ == nullptr)
	{
		return false;
	}

	wgpu::ShaderSourceWGSL wgslDesc{};
	wgslDesc.code = wgpu::StringView(MipmapShaderWGSL, strlen(MipmapShaderWGSL));
	wgpu::ShaderModuleDescriptor shaderDesc{};
	shaderDesc.nextInChain = reinterpret_cast<wgpu::ChainedStruct*>(&wgslDesc);
	mipmapShaderModule_ = device_.CreateShaderModule(&shaderDesc);
	return mipmapShaderModule_ != nullptr;
}

wgpu::RenderPipeline TextureWebGPU::GetMipmapPipeline(wgpu::TextureFormat format)
{
	auto found = mipmapPipelines_.find(format);
	if (found != mipmapPipelines_.end())
	{
		return found->second;
	}

	if (!CreateMipmapResources())
	{
		return nullptr;
	}

	wgpu::ColorTargetState colorTargetState{};
	colorTargetState.format = format;
	colorTargetState.writeMask = wgpu::ColorWriteMask::All;

	wgpu::FragmentState fragmentState{};
	fragmentState.module = mipmapShaderModule_;
	fragmentState.entryPoint = "PSMain";
	fragmentState.targetCount = 1;
	fragmentState.targets = &colorTargetState;

	wgpu::RenderPipelineDescriptor desc{};
	desc.layout = mipmapPipelineLayout_;
	desc.vertex.module = mipmapShaderModule_;
	desc.vertex.entryPoint = "VSMain";
	desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
	desc.primitive.frontFace = wgpu::FrontFace::CW;
	desc.primitive.cullMode = wgpu::CullMode::None;
	desc.fragment = &fragmentState;
	desc.multisample.count = 1;
	desc.multisample.mask = UINT32_MAX;
	desc.multisample.alphaToCoverageEnabled = false;

	auto pipeline = device_.CreateRenderPipeline(&desc);
	if (pipeline != nullptr)
	{
		mipmapPipelines_[format] = pipeline;
	}
	return pipeline;
}

bool TextureWebGPU::Initialize(wgpu::Device& device, const TextureParameter& parameter, wgpu::Instance instance)
{
	device_ = device;
	instance_ = instance;
	parameter_ = parameter;

	const auto getDimension = [](int dimension)
	{
		if (dimension == 1)
			return wgpu::TextureDimension::e1D;

		if (dimension == 2)
			return wgpu::TextureDimension::e2D;

		if (dimension == 3)
			return wgpu::TextureDimension::e3D;

		throw "Not implemented";
	};

	const auto getViewDimension = [](int dimension)
	{
		if (dimension == 1)
			return wgpu::TextureViewDimension::e1D;

		if (dimension == 2)
			return wgpu::TextureViewDimension::e2D;

		if (dimension == 3)
			return wgpu::TextureViewDimension::e3D;

		throw "Not implemented";
	};

	{
		wgpu::TextureDescriptor texDesc{};

		texDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc;
		if (IsDepthFormat(parameter.Format) || (parameter.Usage & TextureUsageType::RenderTarget) != TextureUsageType::NoneFlag)
		{
			texDesc.usage |= wgpu::TextureUsage::RenderAttachment;
		}

		if (CanGenerateMipMaps(parameter))
		{
			texDesc.usage |= wgpu::TextureUsage::RenderAttachment;
		}

		if (BitwiseContains(parameter.Usage, TextureUsageType::Storage))
		{
			texDesc.usage |= wgpu::TextureUsage::StorageBinding;
		}

		if ((parameter.Usage & TextureUsageType::External) != TextureUsageType::NoneFlag)
		{
			throw "Not implemented";
			// texDesc.usage |= dawn::platform::kPresentTextureUsage;
		}

		bool isArray = false;
		if ((parameter.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag)
		{
			isArray = true;
		}

		texDesc.dimension = getDimension(parameter.Dimension);
		texDesc.format = ConvertFormat(parameter.Format);
		texDesc.mipLevelCount = parameter.MipLevelCount;
		texDesc.sampleCount = parameter.SampleCount;
		texDesc.size.width = parameter.Size.X;
		texDesc.size.height = parameter.Size.Y;
		texDesc.size.depthOrArrayLayers = parameter.Size.Z;

		texture_ = device.CreateTexture(&texDesc);
		if (texture_ == nullptr)
		{
			return false;
		}

		wgpu::TextureViewDescriptor texViewDesc{};
		texViewDesc.format = texDesc.format;
		texViewDesc.dimension = isArray && parameter.Dimension == 2 ? wgpu::TextureViewDimension::e2DArray : getViewDimension(parameter.Dimension);
		texViewDesc.baseMipLevel = 0;
		texViewDesc.mipLevelCount = texDesc.mipLevelCount;
		texViewDesc.baseArrayLayer = 0;
		texViewDesc.arrayLayerCount = isArray ? parameter.Size.Z : 1;
		texViewDesc.aspect = wgpu::TextureAspect::All;

		textureView_ = texture_.CreateView(&texViewDesc);
	}

	format_ = parameter.Format;
	usage_ = parameter.Usage;
	samplingCount_ = parameter.SampleCount;
	mipmapCount_ = parameter.MipLevelCount;
	type_ = TextureType::Color;
	if (IsDepthFormat(parameter.Format))
	{
		type_ = TextureType::Depth;
	}
	else if (BitwiseContains(parameter.Usage, TextureUsageType::RenderTarget))
	{
		type_ = TextureType::Render;
	}

	return texture_ != nullptr && textureView_ != nullptr;
}

bool TextureWebGPU::InitializeFromSurfaceTexture(wgpu::Device& device, wgpu::Texture texture, const TextureParameter& parameter)
{
	device_ = device;
	parameter_ = parameter;
	texture_ = texture;
	if (texture_ == nullptr)
	{
		return false;
	}

	wgpu::TextureViewDescriptor texViewDesc{};
	texViewDesc.format = ConvertFormat(parameter.Format);
	texViewDesc.dimension = wgpu::TextureViewDimension::e2D;
	texViewDesc.baseMipLevel = 0;
	texViewDesc.mipLevelCount = 1;
	texViewDesc.baseArrayLayer = 0;
	texViewDesc.arrayLayerCount = 1;
	texViewDesc.aspect = wgpu::TextureAspect::All;
	textureView_ = texture_.CreateView(&texViewDesc);

	format_ = parameter.Format;
	usage_ = parameter.Usage;
	samplingCount_ = parameter.SampleCount;
	mipmapCount_ = parameter.MipLevelCount;
	type_ = TextureType::Screen;

	return textureView_ != nullptr;
}

void* TextureWebGPU::Lock()
{
	const auto preserveDepth = (parameter_.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag;
	auto cpuMemorySize = GetTextureMemorySize(format_, parameter_.Size, parameter_.MipLevelCount, preserveDepth);
	temp_buffer_.resize(cpuMemorySize);
	return temp_buffer_.data();
}

void TextureWebGPU::Unlock()
{
	const auto preserveDepth = (parameter_.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag;
	size_t offset = 0;
	for (int32_t mipLevel = 0; mipLevel < parameter_.MipLevelCount; mipLevel++)
	{
		auto mipSize = GetTextureMipSize(parameter_.Size, mipLevel, preserveDepth);
		WriteTextureMipLevel(device_, texture_, format_, mipLevel, mipSize, temp_buffer_.data() + offset);
		offset += GetTextureMemorySize(format_, mipSize);
	}
	mipmapsGeneratedFromLockedData_ = false;

	if (CanGenerateMipMaps(parameter_))
	{
		wgpu::CommandEncoderDescriptor encoderDesc{};
		auto encoder = device_.CreateCommandEncoder(&encoderDesc);
		GenerateMipMaps(encoder);
		auto commandBuffer = encoder.Finish();
		device_.GetQueue().Submit(1, &commandBuffer);
	}
}

void TextureWebGPU::GenerateMipMaps(wgpu::CommandEncoder& commandEncoder)
{
	if (!CanGenerateMipMaps(parameter_))
	{
		return;
	}

	if (!temp_buffer_.empty() && mipmapsGeneratedFromLockedData_)
	{
		return;
	}

	const auto format = ConvertFormat(parameter_.Format);
	auto pipeline = GetMipmapPipeline(format);
	if (pipeline == nullptr)
	{
		return;
	}

	mipmapRenderTextures_.clear();

	for (uint32_t mipLevel = 1; mipLevel < static_cast<uint32_t>(parameter_.MipLevelCount); mipLevel++)
	{
		const auto srcViewDesc = CreateMipmapTextureViewDesc(format, mipLevel - 1, wgpu::TextureUsage::TextureBinding);
		auto srcView = texture_.CreateView(&srcViewDesc);

		const auto dstSize = GetMipmapSize(parameter_, mipLevel);
		auto mipmapRenderTexture = CreateMipmapRenderTexture(device_, format, dstSize);
		if (mipmapRenderTexture == nullptr)
		{
			return;
		}
		mipmapRenderTextures_.push_back(mipmapRenderTexture);

		const auto dstViewDesc = CreateMipmapTextureViewDesc(format, 0, wgpu::TextureUsage::RenderAttachment);
		auto dstView = mipmapRenderTexture.CreateView(&dstViewDesc);

		wgpu::BindGroupEntry bindGroupEntries[2]{};
		bindGroupEntries[0].binding = 0;
		bindGroupEntries[0].sampler = mipmapSampler_;
		bindGroupEntries[1].binding = 1;
		bindGroupEntries[1].textureView = srcView;

		wgpu::BindGroupDescriptor bindGroupDesc{};
		bindGroupDesc.layout = mipmapBindGroupLayout_;
		bindGroupDesc.entryCount = 2;
		bindGroupDesc.entries = bindGroupEntries;
		auto bindGroup = device_.CreateBindGroup(&bindGroupDesc);

		auto colorAttachment = CreateMipmapColorAttachment(dstView);

		wgpu::RenderPassDescriptor renderPassDesc{};
		renderPassDesc.colorAttachmentCount = 1;
		renderPassDesc.colorAttachments = &colorAttachment;

		auto passEncoder = commandEncoder.BeginRenderPass(&renderPassDesc);
		passEncoder.SetViewport(0.0f, 0.0f, static_cast<float>(dstSize.Width), static_cast<float>(dstSize.Height), 0.0f, 1.0f);
		passEncoder.SetScissorRect(0, 0, dstSize.Width, dstSize.Height);
		passEncoder.SetPipeline(pipeline);
		passEncoder.SetBindGroup(0, bindGroup);
		passEncoder.Draw(3);
		passEncoder.End();

		const auto copySrc = CreateTextureCopyInfo(mipmapRenderTexture, 0);
		const auto copyDst = CreateTextureCopyInfo(texture_, mipLevel);
		const auto copySize = CreateTextureCopySize(dstSize);
		commandEncoder.CopyTextureToTexture(&copySrc, &copyDst, &copySize);
	}

	if (!temp_buffer_.empty())
	{
		mipmapsGeneratedFromLockedData_ = true;
	}
}

bool TextureWebGPU::GetData(std::vector<uint8_t>& data)
{
	const auto bytesPerRowUnaligned = static_cast<uint32_t>(GetTextureRowPitch(format_, parameter_.Size));
	const auto bytesPerRow = AlignTo(bytesPerRowUnaligned, TextureBytesPerRowAlignment);
	const auto rowCount = static_cast<uint32_t>(GetTextureRowCount(format_, parameter_.Size));
	const auto rowsPerImage = GetTextureRowsPerImage(format_, parameter_.Size);
	const auto height = static_cast<uint32_t>(parameter_.Size.Y);
	const auto depth = static_cast<uint32_t>(parameter_.Size.Z);
	const auto bufferSize = static_cast<uint64_t>(bytesPerRow) * rowCount;

	wgpu::BufferDescriptor bufferDesc{};
	bufferDesc.size = bufferSize;
	bufferDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
	auto readbackBuffer = device_.CreateBuffer(&bufferDesc);

	wgpu::CommandEncoderDescriptor encoderDesc{};
	auto encoder = device_.CreateCommandEncoder(&encoderDesc);

	wgpu::TexelCopyTextureInfo src{};
	src.texture = texture_;
	src.aspect = wgpu::TextureAspect::All;

	wgpu::TexelCopyBufferInfo dst{};
	dst.buffer = readbackBuffer;
	dst.layout.bytesPerRow = bytesPerRow;
	dst.layout.rowsPerImage = rowsPerImage;

	wgpu::Extent3D extent{};
	extent.width = static_cast<uint32_t>(parameter_.Size.X);
	extent.height = height;
	extent.depthOrArrayLayers = depth;
	encoder.CopyTextureToBuffer(&src, &dst, &extent);

	auto commandBuffer = encoder.Finish();
	device_.GetQueue().Submit(1, &commandBuffer);

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

	if (!succeeded)
	{
		return false;
	}

	data.resize(static_cast<size_t>(bytesPerRowUnaligned) * height * depth);
	const auto mapped = static_cast<const uint8_t*>(readbackBuffer.GetConstMappedRange(0, bufferSize));
	for (uint32_t z = 0; z < depth; z++)
	{
		for (uint32_t y = 0; y < height; y++)
		{
			memcpy(data.data() + (static_cast<size_t>(z) * height + y) * bytesPerRowUnaligned,
				   mapped + (static_cast<size_t>(z) * height + y) * bytesPerRow,
				   bytesPerRowUnaligned);
		}
	}
	readbackBuffer.Unmap();
	return true;
}

Vec2I TextureWebGPU::GetSizeAs2D() const { return Vec2I(parameter_.Size.X, parameter_.Size.Y); }

} // namespace LLGI
