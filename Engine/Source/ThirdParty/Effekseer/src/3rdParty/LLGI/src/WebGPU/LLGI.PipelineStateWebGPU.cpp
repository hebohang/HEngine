#include "LLGI.PipelineStateWebGPU.h"
#include "LLGI.RenderPassPipelineStateWebGPU.h"
#include <algorithm>
#include <array>
#include <limits>

namespace LLGI
{
namespace
{
bool BuildBindGroupLayoutEntry(const ShaderBindingWebGPU& binding, wgpu::ShaderStage visibility, wgpu::BindGroupLayoutEntry& entry)
{
	entry = {};
	entry.binding = binding.Binding;
	entry.visibility = visibility;

	switch (binding.Resource.ResourceType)
	{
	case ShaderResourceType::UniformBuffer:
		entry.buffer.type = wgpu::BufferBindingType::Uniform;
		entry.buffer.hasDynamicOffset = false;
		entry.buffer.minBindingSize = 0;
		return true;
	case ShaderResourceType::StorageBuffer:
		entry.buffer.type = IsReadOnly(binding.Resource.Access) ? wgpu::BufferBindingType::ReadOnlyStorage : wgpu::BufferBindingType::Storage;
		entry.buffer.hasDynamicOffset = false;
		entry.buffer.minBindingSize = 0;
		return true;
	case ShaderResourceType::Texture:
		entry.texture.sampleType = binding.TextureSampleType;
		entry.texture.viewDimension = binding.TextureViewDimension;
		entry.texture.multisampled = false;
		return true;
	case ShaderResourceType::StorageTexture:
		entry.storageTexture.access =
			binding.StorageTextureAccess != wgpu::StorageTextureAccess::Undefined ? binding.StorageTextureAccess : wgpu::StorageTextureAccess::WriteOnly;
		entry.storageTexture.format =
			binding.StorageTextureFormat != wgpu::TextureFormat::Undefined ? binding.StorageTextureFormat : wgpu::TextureFormat::RGBA32Float;
		entry.storageTexture.viewDimension = binding.TextureViewDimension;
		return true;
	case ShaderResourceType::Sampler:
		entry.sampler.type = wgpu::SamplerBindingType::Filtering;
		return true;
	default:
		return false;
	}
}

bool ContainsBinding(const std::vector<ShaderBindingWebGPU>& bindings, const ShaderBindingWebGPU& binding)
{
	for (const auto& existingBinding : bindings)
	{
		if (existingBinding.Group != binding.Group || existingBinding.Binding != binding.Binding ||
			existingBinding.Resource.ResourceType != binding.Resource.ResourceType ||
			existingBinding.Resource.Access != binding.Resource.Access)
		{
			continue;
		}

		if ((binding.Resource.ResourceType == ShaderResourceType::Texture ||
			 binding.Resource.ResourceType == ShaderResourceType::StorageTexture) &&
			existingBinding.TextureViewDimension != binding.TextureViewDimension)
		{
			continue;
		}

		if (binding.Resource.ResourceType == ShaderResourceType::Texture &&
			existingBinding.TextureSampleType != binding.TextureSampleType)
		{
			continue;
		}

		if (binding.Resource.ResourceType == ShaderResourceType::StorageTexture &&
			(existingBinding.StorageTextureFormat != binding.StorageTextureFormat ||
			 existingBinding.StorageTextureAccess != binding.StorageTextureAccess))
		{
			continue;
		}

		return true;
	}

	return false;
}

wgpu::PipelineLayout CreatePipelineLayout(wgpu::Device& device,
										  const std::vector<ShaderBindingWebGPU>& bindings,
										  wgpu::ShaderStage visibility,
										  std::array<wgpu::BindGroupLayout, 3>& bindGroupLayouts)
{
	std::array<std::vector<wgpu::BindGroupLayoutEntry>, 3> bindGroupLayoutEntries;
	uint32_t bindGroupLayoutCount = 0;
	for (const auto& binding : bindings)
	{
		if (binding.Group >= bindGroupLayoutEntries.size())
		{
			continue;
		}

		wgpu::BindGroupLayoutEntry entry{};
		if (BuildBindGroupLayoutEntry(binding, visibility, entry))
		{
			bindGroupLayoutEntries[binding.Group].push_back(entry);
			bindGroupLayoutCount = std::max(bindGroupLayoutCount, binding.Group + 1);
		}
	}

	for (uint32_t i = 0; i < bindGroupLayoutCount; i++)
	{
		wgpu::BindGroupLayoutDescriptor bindGroupLayoutDesc{};
		bindGroupLayoutDesc.entryCount = static_cast<uint32_t>(bindGroupLayoutEntries[i].size());
		bindGroupLayoutDesc.entries = bindGroupLayoutEntries[i].data();
		bindGroupLayouts[i] = device.CreateBindGroupLayout(&bindGroupLayoutDesc);
	}

	wgpu::PipelineLayoutDescriptor pipelineLayoutDesc{};
	pipelineLayoutDesc.bindGroupLayoutCount = bindGroupLayoutCount;
	pipelineLayoutDesc.bindGroupLayouts = bindGroupLayouts.data();
	return device.CreatePipelineLayout(&pipelineLayoutDesc);
}
} // namespace

PipelineStateWebGPU::PipelineStateWebGPU(wgpu::Device device) : device_(device) { shaders_.fill(nullptr); }

PipelineStateWebGPU::~PipelineStateWebGPU()
{
	for (auto& shader : shaders_)
	{
		SafeRelease(shader);
	}
}

void PipelineStateWebGPU::SetShader(ShaderStageType stage, Shader* shader)
{
	SafeAddRef(shader);
	SafeRelease(shaders_[static_cast<int>(stage)]);
	shaders_[static_cast<int>(stage)] = shader;
}

bool PipelineStateWebGPU::HasBinding(uint32_t group, uint32_t binding) const
{
	if (!hasBindingReflection_)
	{
		return true;
	}

	for (const auto& b : bindings_)
	{
		if (b.Group == group && b.Binding == binding)
		{
			return true;
		}
	}

	return false;
}

bool PipelineStateWebGPU::HasBinding(uint32_t group, uint32_t binding, ShaderResourceType resourceType) const
{
	return HasBinding(group, binding, resourceType, ShaderResourceAccess::ReadOnly);
}

bool PipelineStateWebGPU::HasBinding(uint32_t group, uint32_t binding, ShaderResourceType resourceType, ShaderResourceAccess access) const
{
	if (!hasBindingReflection_)
	{
		return true;
	}

	for (const auto& b : bindings_)
	{
		const bool resourceTypeMatches = b.Resource.ResourceType == resourceType || b.Resource.ResourceType == ShaderResourceType::Unknown;
		const bool accessMatches = b.Resource.ResourceType == ShaderResourceType::Unknown ||
								   resourceType != ShaderResourceType::StorageBuffer ||
								   IsReadOnly(b.Resource.Access) == IsReadOnly(access);
		if (b.Group == group && b.Binding == binding && resourceTypeMatches && accessMatches)
		{
			return true;
		}
	}

	return false;
}

bool PipelineStateWebGPU::Compile()
{
	bindings_.clear();
	hasBindingReflection_ = false;
	for (auto shader : shaders_)
	{
		auto shaderWebGPU = static_cast<ShaderWebGPU*>(shader);
		if (shaderWebGPU == nullptr || !shaderWebGPU->HasBindingReflection())
		{
			continue;
		}

		hasBindingReflection_ = true;
		for (const auto& reflectedBinding : shaderWebGPU->GetBindings())
		{
			if (!ContainsBinding(bindings_, reflectedBinding))
			{
				bindings_.push_back(reflectedBinding);
			}
		}
	}

	const char* entryPointName = "main";
	auto computeShader = static_cast<ShaderWebGPU*>(shaders_[static_cast<int>(ShaderStageType::Compute)]);
	if (computeShader != nullptr)
	{
		wgpu::ComputePipelineDescriptor desc{};
		desc.layout = nullptr;
		if (hasBindingReflection_)
		{
			pipelineLayout_ = CreatePipelineLayout(device_, bindings_, wgpu::ShaderStage::Compute, bindGroupLayouts_);
			desc.layout = pipelineLayout_;
		}
		desc.compute.module = computeShader->GetShaderModule();
		desc.compute.entryPoint = entryPointName;
		computePipeline_ = device_.CreateComputePipeline(&desc);
		return computePipeline_ != nullptr;
	}

	wgpu::RenderPipelineDescriptor desc{};

	desc.primitive.topology = Convert(Topology);
	desc.primitive.stripIndexFormat = wgpu::IndexFormat::Undefined; // is it correct?
	desc.primitive.frontFace = wgpu::FrontFace::CW;
	desc.primitive.cullMode = Convert(Culling);
	desc.multisample.count = static_cast<uint32_t>(renderPassPipelineState_->Key.SamplingCount);
	desc.multisample.mask = std::numeric_limits<int32_t>::max();
	desc.multisample.alphaToCoverageEnabled = false;
	desc.layout = nullptr; // is it correct?
	if (hasBindingReflection_)
	{
		pipelineLayout_ = CreatePipelineLayout(device_, bindings_, wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment, bindGroupLayouts_);
		desc.layout = pipelineLayout_;
	}

	auto vertexShader = static_cast<ShaderWebGPU*>(shaders_[static_cast<int>(ShaderStageType::Vertex)]);

	desc.vertex.module = vertexShader->GetShaderModule();
	desc.vertex.entryPoint = entryPointName;

	desc.vertex.bufferCount = 1;
	std::array<wgpu::VertexBufferLayout, 1> bufferLayouts{};
	desc.vertex.buffers = bufferLayouts.data();

	bufferLayouts[0].attributeCount = VertexLayoutCount;
	bufferLayouts[0].arrayStride = 0;
	bufferLayouts[0].stepMode = wgpu::VertexStepMode::Vertex;

	std::array<wgpu::VertexAttribute, VertexLayoutMax> attributes{};
	bufferLayouts[0].attributes = attributes.data();

	int offset = 0;
	for (int i = 0; i < VertexLayoutCount; i++)
	{
		attributes[i].format = Convert(VertexLayouts[i]);
		attributes[i].offset = offset;
		attributes[i].shaderLocation = i;
		offset += GetSize(VertexLayouts[i]);
	}
	bufferLayouts[0].arrayStride = VertexBufferStride > 0 ? VertexBufferStride : offset;

	auto pixelShader = static_cast<ShaderWebGPU*>(shaders_[static_cast<int>(ShaderStageType::Pixel)]);

	// TODO : support blend enabled
	wgpu::BlendState blendState{};
	blendState.color.srcFactor = Convert(BlendSrcFunc);
	blendState.color.dstFactor = Convert(BlendDstFunc);
	blendState.color.operation = Convert(BlendEquationRGB);
	blendState.alpha.srcFactor = Convert(BlendSrcFuncAlpha);
	blendState.alpha.dstFactor = Convert(BlendDstFuncAlpha);
	blendState.alpha.operation = Convert(BlendEquationAlpha);

	std::array<wgpu::ColorTargetState, RenderTargetMax> colorTargetStates{};

	for (size_t i = 0; i < renderPassPipelineState_->Key.RenderTargetFormats.size(); i++)
	{
		colorTargetStates[i].blend = IsBlendEnabled ? &blendState : nullptr;
		colorTargetStates[i].format = ConvertFormat(renderPassPipelineState_->Key.RenderTargetFormats.at(i));
		colorTargetStates[i].writeMask = wgpu::ColorWriteMask::All;
	}

	wgpu::FragmentState fragmentState = {};
	fragmentState.targetCount = static_cast<uint32_t>(renderPassPipelineState_->Key.RenderTargetFormats.size());
	fragmentState.targets = colorTargetStates.data();
	fragmentState.entryPoint = entryPointName;
	fragmentState.module = pixelShader->GetShaderModule();

	desc.fragment = &fragmentState;

	const bool hasDepth = renderPassPipelineState_->Key.DepthFormat != TextureFormatType::Unknown;
	const bool hasStencil = HasStencil(renderPassPipelineState_->Key.DepthFormat);

	wgpu::DepthStencilState depthStencilState = {};
	depthStencilState.depthWriteEnabled = hasDepth && IsDepthWriteEnabled;

	if (IsDepthTestEnabled)
	{
		depthStencilState.depthCompare = Convert(DepthFunc);
	}
	else
	{
		depthStencilState.depthCompare = wgpu::CompareFunction::Always;
	}

	if (hasStencil && IsStencilTestEnabled)
	{
		wgpu::StencilFaceState fs{};

		fs.compare = Convert(StencilCompareFunc);
		fs.depthFailOp = Convert(StencilDepthFailOp);
		fs.failOp = Convert(StencilFailOp);
		fs.passOp = Convert(StencilPassOp);

		depthStencilState.stencilFront = fs;
		depthStencilState.stencilBack = fs;

		depthStencilState.stencilWriteMask = StencilWriteMask;
		depthStencilState.stencilReadMask = StencilReadMask;
	}
	else
	{
		wgpu::StencilFaceState fs{};

		fs.depthFailOp = wgpu::StencilOperation::Keep;
		fs.failOp = wgpu::StencilOperation::Keep;
		fs.compare = wgpu::CompareFunction::Always;
		fs.passOp = wgpu::StencilOperation::Keep;

		depthStencilState.stencilFront = fs;
		depthStencilState.stencilBack = fs;

		depthStencilState.stencilWriteMask = 0xff;
		depthStencilState.stencilReadMask = 0xff;
	}

	if (hasDepth)
	{
		depthStencilState.format = ConvertFormat(renderPassPipelineState_->Key.DepthFormat);
		desc.depthStencil = &depthStencilState;
	}

	renderPipeline_ = device_.CreateRenderPipeline(&desc);

	return renderPipeline_ != nullptr;
}

} // namespace LLGI
