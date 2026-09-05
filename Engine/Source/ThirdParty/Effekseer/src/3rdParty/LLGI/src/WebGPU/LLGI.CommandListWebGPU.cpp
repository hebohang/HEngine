#include "LLGI.CommandListWebGPU.h"
#include "LLGI.BufferWebGPU.h"
#include "LLGI.PipelineStateWebGPU.h"
#include "LLGI.RenderPassWebGPU.h"
#include "LLGI.TextureWebGPU.h"

#include <algorithm>
#include <array>
#include <string>

namespace LLGI
{
namespace
{
constexpr uint32_t TextureBytesPerRowAlignment = 256;
constexpr std::array<wgpu::FilterMode, 2> TextureFilterModes = {
	wgpu::FilterMode::Nearest,
	wgpu::FilterMode::Linear,
};
constexpr std::array<wgpu::AddressMode, 3> TextureAddressModes = {
	wgpu::AddressMode::ClampToEdge,
	wgpu::AddressMode::Repeat,
	wgpu::AddressMode::MirrorRepeat,
};

ShaderResourceType GetTextureBindingResourceType(TextureWebGPU* texture)
{
	if (texture != nullptr && BitwiseContains(texture->GetParameter().Usage, TextureUsageType::Storage))
	{
		return ShaderResourceType::StorageTexture;
	}
	return ShaderResourceType::Texture;
}

bool NeedsTextureSampler(TextureWebGPU* texture)
{
	return texture == nullptr || !BitwiseContains(texture->GetParameter().Usage, TextureUsageType::Storage);
}
} // namespace

bool CommandListWebGPU::Equals(const std::vector<BindGroupEntryKey>& lhs, const std::vector<BindGroupEntryKey>& rhs)
{
	if (lhs.size() != rhs.size())
	{
		return false;
	}

	for (size_t i = 0; i < lhs.size(); i++)
	{
		if (lhs[i].binding != rhs[i].binding || lhs[i].resource != rhs[i].resource || lhs[i].offset != rhs[i].offset ||
			lhs[i].size != rhs[i].size || lhs[i].wrapMode != rhs[i].wrapMode || lhs[i].minMagFilter != rhs[i].minMagFilter)
		{
			return false;
		}
	}

	return true;
}

void CommandListWebGPU::ResetRenderBindGroupCaches()
{
	for (auto& cache : renderBindGroupCaches_)
	{
		cache.pipeline = nullptr;
		cache.entries.clear();
		cache.bindGroup = nullptr;
	}
}

void CommandListWebGPU::ResetComputeBindGroupCaches()
{
	for (auto& cache : computeBindGroupCaches_)
	{
		cache.pipeline = nullptr;
		cache.entries.clear();
		cache.bindGroup = nullptr;
	}
}

void CommandListWebGPU::SetRenderBindGroup(
	uint32_t index, const void* pipeline, const std::vector<BindGroupEntryKey>& entries, const wgpu::BindGroupDescriptor& desc)
{
	auto& cache = renderBindGroupCaches_[index];
	if (cache.bindGroup == nullptr || cache.pipeline != pipeline || !Equals(cache.entries, entries))
	{
		cache.pipeline = pipeline;
		cache.entries = entries;
		cache.bindGroup = device_.CreateBindGroup(&desc);
	}

	renderPassEncorder_.SetBindGroup(index, cache.bindGroup);
}

void CommandListWebGPU::SetComputeBindGroup(
	uint32_t index, const void* pipeline, const std::vector<BindGroupEntryKey>& entries, const wgpu::BindGroupDescriptor& desc)
{
	auto& cache = computeBindGroupCaches_[index];
	if (cache.bindGroup == nullptr || cache.pipeline != pipeline || !Equals(cache.entries, entries))
	{
		cache.pipeline = pipeline;
		cache.entries = entries;
		cache.bindGroup = device_.CreateBindGroup(&desc);
	}

	computePassEncorder_.SetBindGroup(index, cache.bindGroup);
}

bool CommandListWebGPU::PrepareStandaloneCommand(const char* commandName)
{
	if (isInRenderPass_ && !doesBeginWithPlatform_)
	{
		Log(LogType::Error, std::string("Please call ") + commandName + " outside of RenderPass");
		return false;
	}

	EndComputePass();
	return true;
}

CommandListWebGPU::CommandListWebGPU(wgpu::Device device) : device_(device)
{
	wgpu::TextureDescriptor fallbackTextureDesc{};
	fallbackTextureDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
	fallbackTextureDesc.dimension = wgpu::TextureDimension::e2D;
	fallbackTextureDesc.format = wgpu::TextureFormat::RGBA8Unorm;
	fallbackTextureDesc.mipLevelCount = 1;
	fallbackTextureDesc.sampleCount = 1;
	fallbackTextureDesc.size.width = 1;
	fallbackTextureDesc.size.height = 1;
	fallbackTextureDesc.size.depthOrArrayLayers = 1;
	fallbackTexture_ = device_.CreateTexture(&fallbackTextureDesc);
	fallbackTextureView_ = fallbackTexture_.CreateView();

	std::array<uint8_t, TextureBytesPerRowAlignment> fallbackTextureData{};
	fallbackTextureData[0] = 255;
	fallbackTextureData[1] = 255;
	fallbackTextureData[2] = 255;
	fallbackTextureData[3] = 255;
	wgpu::TexelCopyTextureInfo fallbackDst{};
	fallbackDst.texture = fallbackTexture_;
	fallbackDst.aspect = wgpu::TextureAspect::All;
	wgpu::TexelCopyBufferLayout fallbackLayout{};
	fallbackLayout.bytesPerRow = TextureBytesPerRowAlignment;
	fallbackLayout.rowsPerImage = 1;
	wgpu::Extent3D fallbackExtent{};
	fallbackExtent.width = 1;
	fallbackExtent.height = 1;
	fallbackExtent.depthOrArrayLayers = 1;
	device_.GetQueue().WriteTexture(
		&fallbackDst,
		fallbackTextureData.data(),
		fallbackTextureData.size(),
		&fallbackLayout,
		&fallbackExtent);

	for (int w = 0; w < 3; w++)
	{
		for (int f = 0; f < 2; f++)
		{
			wgpu::SamplerDescriptor samplerDesc{};

			samplerDesc.magFilter = TextureFilterModes[f];
			samplerDesc.minFilter = TextureFilterModes[f];
			// LLGI exposes min/mag filtering only, so keep mip selection point-filtered.
			samplerDesc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
			samplerDesc.lodMinClamp = 0.0f;
			samplerDesc.lodMaxClamp = 32.0f;
			samplerDesc.maxAnisotropy = 1;
			samplerDesc.addressModeU = TextureAddressModes[w];
			samplerDesc.addressModeV = TextureAddressModes[w];
			samplerDesc.addressModeW = TextureAddressModes[w];
			samplers_[w][f] = device.CreateSampler(&samplerDesc);
		}
	}
}

void CommandListWebGPU::Begin()
{
	wgpu::CommandEncoderDescriptor desc = {};
	commandEncorder_ = device_.CreateCommandEncoder(&desc);
	ResetRenderBindGroupCaches();
	ResetComputeBindGroupCaches();

	CommandList::Begin();
}

bool CommandListWebGPU::BeginWithPlatform(void* platformContextPtr)
{
	if (platformContextPtr == nullptr)
	{
		Begin();
		return true;
	}

	commandBuffer_ = nullptr;
	commandEncorder_ = wgpu::CommandEncoder::Acquire(reinterpret_cast<WGPUCommandEncoder>(platformContextPtr));
	ResetRenderBindGroupCaches();
	ResetComputeBindGroupCaches();

	return CommandList::BeginWithPlatform(platformContextPtr);
}

void CommandListWebGPU::End()
{
	commandBuffer_ = commandEncorder_.Finish();
	commandEncorder_ = nullptr;

	CommandList::End();
}

void CommandListWebGPU::EndWithPlatform()
{
	commandEncorder_ = nullptr;
	commandBuffer_ = nullptr;

	CommandList::EndWithPlatform();
}

void CommandListWebGPU::BeginRenderPass(RenderPass* renderPass)
{
	EndComputePass();

	auto rp = static_cast<RenderPassWebGPU*>(renderPass);
	rp->RefreshDescriptor();
	const auto& desc = rp->GetDescriptor();

	renderPassEncorder_ = commandEncorder_.BeginRenderPass(&desc);
	ResetRenderBindGroupCaches();
	renderPassEncorder_.SetViewport(0.0f, 0.0f, static_cast<float>(rp->GetScreenSize().X), static_cast<float>(rp->GetScreenSize().Y), 0.0f, 1.0f);

	CommandList::BeginRenderPass(renderPass);
}

void CommandListWebGPU::EndRenderPass()
{
	if (renderPassEncorder_ != nullptr)
	{
		renderPassEncorder_.End();
		renderPassEncorder_ = nullptr;
	}
	CommandList::EndRenderPass();
}

bool CommandListWebGPU::BeginRenderPassWithPlatformPtr(void* platformPtr)
{
	if (platformPtr == nullptr)
	{
		return false;
	}

	EndComputePass();
	renderPassEncorder_ = wgpu::RenderPassEncoder::Acquire(reinterpret_cast<WGPURenderPassEncoder>(platformPtr));
	ResetRenderBindGroupCaches();

	return CommandList::BeginRenderPassWithPlatformPtr(platformPtr);
}

bool CommandListWebGPU::EndRenderPassWithPlatformPtr()
{
	if (renderPassEncorder_ != nullptr)
	{
		renderPassEncorder_ = nullptr;
	}

	CommandList::EndRenderPass();
	return true;
}

void CommandListWebGPU::BeginComputePass()
{
	if (computePassEncorder_ != nullptr)
	{
		return;
	}

	wgpu::ComputePassDescriptor desc{};
	computePassEncorder_ = commandEncorder_.BeginComputePass(&desc);
	ResetComputeBindGroupCaches();
}

void CommandListWebGPU::EndComputePass()
{
	if (computePassEncorder_ != nullptr)
	{
		computePassEncorder_.End();
		computePassEncorder_ = nullptr;
	}
}

void CommandListWebGPU::Draw(int32_t primitiveCount, int32_t instanceCount)
{
	BindingVertexBuffer bvb;
	BindingIndexBuffer bib;
	PipelineState* bpip = nullptr;

	bool isVBDirtied = false;
	bool isIBDirtied = false;
	bool isPipDirtied = false;

	GetCurrentVertexBuffer(bvb, isVBDirtied);
	GetCurrentIndexBuffer(bib, isIBDirtied);
	GetCurrentPipelineState(bpip, isPipDirtied);

	assert(bvb.vertexBuffer != nullptr);
	assert(bib.indexBuffer != nullptr);
	assert(bpip != nullptr);

	auto vb = static_cast<BufferWebGPU*>(bvb.vertexBuffer);
	auto ib = static_cast<BufferWebGPU*>(bib.indexBuffer);
	auto pip = static_cast<PipelineStateWebGPU*>(bpip);

	if (vb != nullptr && isVBDirtied)
	{
		renderPassEncorder_.SetVertexBuffer(0, vb->GetBuffer(), bvb.offset, bvb.vertexBuffer->GetSize() - bvb.offset);
	}

	if (ib != nullptr && isIBDirtied)
	{
		const auto format = bib.stride == 2 ? wgpu::IndexFormat::Uint16 : wgpu::IndexFormat::Uint32;
		renderPassEncorder_.SetIndexBuffer(ib->GetBuffer(), format, bib.offset, ib->GetSize() - bib.offset);
	}

	if (pip != nullptr && isPipDirtied)
	{
		renderPassEncorder_.SetPipeline(pip->GetRenderPipeline());
		renderPassEncorder_.SetStencilReference(pip->StencilRef);
	}

	std::vector<wgpu::BindGroupEntry> constantBindGroupEntries;
	std::vector<BindGroupEntryKey> constantBindGroupEntryKeys;

	for (size_t unit_ind = 0; unit_ind < constantBuffers_.size(); unit_ind++)
	{
		auto cb = static_cast<BufferWebGPU*>(constantBuffers_[unit_ind]);
		if (cb == nullptr)
		{
			continue;
		}
		if (!pip->HasBinding(0, static_cast<uint32_t>(unit_ind), ShaderResourceType::UniformBuffer))
		{
			continue;
		}

		wgpu::BindGroupEntry entry = {};
		entry.binding = static_cast<uint32_t>(unit_ind);
		entry.buffer = cb->GetBuffer();
		entry.size = cb->GetAllocatedSize() - cb->GetOffset();
		entry.offset = cb->GetOffset();
		constantBindGroupEntries.push_back(entry);
		constantBindGroupEntryKeys.push_back(
			{static_cast<uint32_t>(unit_ind), cb, static_cast<uint64_t>(entry.offset), static_cast<uint64_t>(entry.size), 0, 0});
	}

	if (!constantBindGroupEntries.empty())
	{
		wgpu::BindGroupDescriptor constantBindGroupDesc = {};
		constantBindGroupDesc.layout = pip->GetRenderPipeline().GetBindGroupLayout(0);
		constantBindGroupDesc.entries = constantBindGroupEntries.data();
		constantBindGroupDesc.entryCount = constantBindGroupEntries.size();
		SetRenderBindGroup(0, pip, constantBindGroupEntryKeys, constantBindGroupDesc);
	}

	std::vector<wgpu::BindGroupEntry> textureGroupEntries;
	std::vector<wgpu::BindGroupEntry> samplerGroupEntries;
	std::vector<BindGroupEntryKey> textureGroupEntryKeys;
	std::vector<BindGroupEntryKey> samplerGroupEntryKeys;

	for (int unit_ind = 0; unit_ind < static_cast<int32_t>(currentTextures_.size()); unit_ind++)
	{
		auto texture = static_cast<TextureWebGPU*>(currentTextures_[unit_ind].texture);
		const auto resourceType = GetTextureBindingResourceType(texture);
		if (!pip->HasBinding(1, static_cast<uint32_t>(unit_ind), resourceType))
			continue;
		auto wm = (int32_t)currentTextures_[unit_ind].wrapMode;
		auto mm = (int32_t)currentTextures_[unit_ind].minMagFilter;

		wgpu::BindGroupEntry textureEntry = {};
		textureEntry.binding = unit_ind;
		textureEntry.textureView = texture != nullptr ? texture->GetTextureView() : fallbackTextureView_;
		textureGroupEntries.push_back(textureEntry);
		textureGroupEntryKeys.push_back(
			{static_cast<uint32_t>(unit_ind), texture, 0, 0, static_cast<int32_t>(wm), static_cast<int32_t>(mm)});

		wgpu::BindGroupEntry samplerEntry = {};
		if (NeedsTextureSampler(texture))
		{
			if (!pip->HasBinding(2, static_cast<uint32_t>(unit_ind), ShaderResourceType::Sampler))
			{
				continue;
			}
			samplerEntry.binding = unit_ind;
			samplerEntry.sampler = samplers_[wm][mm];
			samplerGroupEntries.push_back(samplerEntry);
			samplerGroupEntryKeys.push_back({static_cast<uint32_t>(unit_ind), nullptr, 0, 0, wm, mm});
		}
	}

	for (int unit_ind = 0; unit_ind < static_cast<int32_t>(storageBuffers_.size()); unit_ind++)
	{
		if (storageBuffers_[unit_ind].storageBuffer == nullptr)
		{
			continue;
		}
		if (!pip->HasBinding(1, static_cast<uint32_t>(unit_ind), ShaderResourceType::StorageBuffer, storageBuffers_[unit_ind].binding.Access))
		{
			continue;
		}

		auto buffer = static_cast<BufferWebGPU*>(storageBuffers_[unit_ind].storageBuffer);
		wgpu::BindGroupEntry bufferEntry = {};
		bufferEntry.binding = static_cast<uint32_t>(unit_ind);
		bufferEntry.buffer = buffer->GetBuffer();
		bufferEntry.offset = buffer->GetOffset();
		bufferEntry.size = buffer->GetSize();
		textureGroupEntries.push_back(bufferEntry);
		textureGroupEntryKeys.push_back({static_cast<uint32_t>(unit_ind),
										 buffer,
										 static_cast<uint64_t>(bufferEntry.offset),
										 static_cast<uint64_t>(bufferEntry.size),
										 0,
										 0});
	}

	if (!textureGroupEntries.empty())
	{
		wgpu::BindGroupDescriptor textureBindGroupDesc = {};
		textureBindGroupDesc.layout = pip->GetRenderPipeline().GetBindGroupLayout(1);
		textureBindGroupDesc.entries = textureGroupEntries.data();
		textureBindGroupDesc.entryCount = textureGroupEntries.size();
		SetRenderBindGroup(1, pip, textureGroupEntryKeys, textureBindGroupDesc);
	}

	if (!samplerGroupEntries.empty())
	{
		wgpu::BindGroupDescriptor samplerBindGroupDesc = {};
		samplerBindGroupDesc.layout = pip->GetRenderPipeline().GetBindGroupLayout(2);
		samplerBindGroupDesc.entries = samplerGroupEntries.data();
		samplerBindGroupDesc.entryCount = samplerGroupEntries.size();
		SetRenderBindGroup(2, pip, samplerGroupEntryKeys, samplerBindGroupDesc);
	}

	int indexPerPrim = 0;

	if (pip->Topology == TopologyType::Triangle)
	{
		indexPerPrim = 3;
	}
	else if (pip->Topology == TopologyType::Line)
	{
		indexPerPrim = 2;
	}
	else if (pip->Topology == TopologyType::Point)
	{
		indexPerPrim = 1;
	}
	else
	{
		assert(0);
	}

	renderPassEncorder_.DrawIndexed(primitiveCount * indexPerPrim, instanceCount, 0, 0, 0);
	CommandList::Draw(primitiveCount, instanceCount);
}

void CommandListWebGPU::Dispatch(int32_t groupX, int32_t groupY, int32_t groupZ, int32_t threadX, int32_t threadY, int32_t threadZ)
{
	PipelineState* bpip = nullptr;
	bool isPipDirtied = false;
	GetCurrentPipelineState(bpip, isPipDirtied);
	auto pip = static_cast<PipelineStateWebGPU*>(bpip);
	if (pip == nullptr)
	{
		return;
	}

	bool isComputePassStarted = false;
	if (computePassEncorder_ == nullptr)
	{
		BeginComputePass();
		isComputePassStarted = true;
	}

	if (isComputePassStarted || isPipDirtied)
	{
		computePassEncorder_.SetPipeline(pip->GetComputePipeline());
	}

	std::vector<wgpu::BindGroupEntry> constantBindGroupEntries;
	std::vector<BindGroupEntryKey> constantBindGroupEntryKeys;
	for (size_t unit_ind = 0; unit_ind < constantBuffers_.size(); unit_ind++)
	{
		auto cb = static_cast<BufferWebGPU*>(constantBuffers_[unit_ind]);
		if (cb == nullptr)
		{
			continue;
		}
		if (!pip->HasBinding(0, static_cast<uint32_t>(unit_ind), ShaderResourceType::UniformBuffer))
		{
			continue;
		}

		wgpu::BindGroupEntry entry{};
		entry.binding = static_cast<uint32_t>(unit_ind);
		entry.buffer = cb->GetBuffer();
		entry.size = cb->GetAllocatedSize() - cb->GetOffset();
		entry.offset = cb->GetOffset();
		constantBindGroupEntries.push_back(entry);
		constantBindGroupEntryKeys.push_back(
			{static_cast<uint32_t>(unit_ind), cb, static_cast<uint64_t>(entry.offset), static_cast<uint64_t>(entry.size), 0, 0});
	}

	if (!constantBindGroupEntries.empty())
	{
		wgpu::BindGroupDescriptor desc{};
		desc.layout = pip->GetComputePipeline().GetBindGroupLayout(0);
		desc.entries = constantBindGroupEntries.data();
		desc.entryCount = constantBindGroupEntries.size();
		SetComputeBindGroup(0, pip, constantBindGroupEntryKeys, desc);
	}

	std::vector<wgpu::BindGroupEntry> textureGroupEntries;
	std::vector<wgpu::BindGroupEntry> samplerAndBufferGroupEntries;
	std::vector<BindGroupEntryKey> textureGroupEntryKeys;
	std::vector<BindGroupEntryKey> samplerAndBufferGroupEntryKeys;

	for (int unit_ind = 0; unit_ind < static_cast<int32_t>(currentTextures_.size()); unit_ind++)
	{
		auto texture = static_cast<TextureWebGPU*>(currentTextures_[unit_ind].texture);
		const auto resourceType = GetTextureBindingResourceType(texture);
		if (!pip->HasBinding(1, static_cast<uint32_t>(unit_ind), resourceType))
		{
			continue;
		}

		auto wm = (int32_t)currentTextures_[unit_ind].wrapMode;
		auto mm = (int32_t)currentTextures_[unit_ind].minMagFilter;

		wgpu::BindGroupEntry textureEntry{};
		textureEntry.binding = unit_ind;
		textureEntry.textureView = texture != nullptr ? texture->GetTextureView() : fallbackTextureView_;
		textureGroupEntries.push_back(textureEntry);
		textureGroupEntryKeys.push_back(
			{static_cast<uint32_t>(unit_ind), texture, 0, 0, static_cast<int32_t>(wm), static_cast<int32_t>(mm)});

		if (NeedsTextureSampler(texture))
		{
			if (!pip->HasBinding(2, static_cast<uint32_t>(unit_ind), ShaderResourceType::Sampler))
			{
				continue;
			}
			wgpu::BindGroupEntry samplerEntry{};
			samplerEntry.binding = unit_ind;
			samplerEntry.sampler = samplers_[wm][mm];
			samplerAndBufferGroupEntries.push_back(samplerEntry);
			samplerAndBufferGroupEntryKeys.push_back({static_cast<uint32_t>(unit_ind), nullptr, 0, 0, wm, mm});
		}
	}

	for (int unit_ind = 0; unit_ind < static_cast<int32_t>(storageBuffers_.size()); unit_ind++)
	{
		if (storageBuffers_[unit_ind].storageBuffer == nullptr)
		{
			continue;
		}
		if (!pip->HasBinding(2, static_cast<uint32_t>(unit_ind), ShaderResourceType::StorageBuffer, storageBuffers_[unit_ind].binding.Access))
		{
			continue;
		}

		auto buffer = static_cast<BufferWebGPU*>(storageBuffers_[unit_ind].storageBuffer);
		wgpu::BindGroupEntry entry{};
		entry.binding = static_cast<uint32_t>(unit_ind);
		entry.buffer = buffer->GetBuffer();
		entry.offset = buffer->GetOffset();
		entry.size = buffer->GetSize();
		samplerAndBufferGroupEntries.push_back(entry);
		samplerAndBufferGroupEntryKeys.push_back({static_cast<uint32_t>(unit_ind),
												  buffer,
												  static_cast<uint64_t>(entry.offset),
												  static_cast<uint64_t>(entry.size),
												  0,
												  0});
	}

	if (!textureGroupEntries.empty())
	{
		wgpu::BindGroupDescriptor desc{};
		desc.layout = pip->GetComputePipeline().GetBindGroupLayout(1);
		desc.entries = textureGroupEntries.data();
		desc.entryCount = textureGroupEntries.size();
		SetComputeBindGroup(1, pip, textureGroupEntryKeys, desc);
	}

	if (!samplerAndBufferGroupEntries.empty())
	{
		wgpu::BindGroupDescriptor desc{};
		desc.layout = pip->GetComputePipeline().GetBindGroupLayout(2);
		desc.entries = samplerAndBufferGroupEntries.data();
		desc.entryCount = samplerAndBufferGroupEntries.size();
		SetComputeBindGroup(2, pip, samplerAndBufferGroupEntryKeys, desc);
	}

	computePassEncorder_.DispatchWorkgroups(groupX, groupY, groupZ);
	CommandList::Dispatch(groupX, groupY, groupZ, threadX, threadY, threadZ);
}

void CommandListWebGPU::SetScissor(int32_t x, int32_t y, int32_t width, int32_t height)
{
	renderPassEncorder_.SetScissorRect(x, y, width, height);
}

void CommandListWebGPU::CopyTexture(Texture* src, Texture* dst)
{
	auto srcTex = static_cast<TextureWebGPU*>(src);
	CopyTexture(src, dst, {0, 0, 0}, {0, 0, 0}, srcTex->GetParameter().Size, 0, 0);
}

void CommandListWebGPU::CopyTexture(
	Texture* src, Texture* dst, const Vec3I& srcPos, const Vec3I& dstPos, const Vec3I& size, int srcLayer, int dstLayer)
{
	if (!PrepareStandaloneCommand("CopyTexture"))
		return;

	auto srcTex = static_cast<TextureWebGPU*>(src);
	auto dstTex = static_cast<TextureWebGPU*>(dst);

	wgpu::TexelCopyTextureInfo srcTexCopy{};
	wgpu::TexelCopyTextureInfo dstTexCopy{};
	wgpu::Extent3D extend3d{};

	srcTexCopy.texture = srcTex->GetTexture();
	srcTexCopy.origin = {static_cast<uint32_t>(srcPos.X), static_cast<uint32_t>(srcPos.Y), static_cast<uint32_t>(srcLayer + srcPos.Z)};
	srcTexCopy.aspect = wgpu::TextureAspect::All;

	dstTexCopy.texture = dstTex->GetTexture();
	dstTexCopy.origin = {static_cast<uint32_t>(dstPos.X), static_cast<uint32_t>(dstPos.Y), static_cast<uint32_t>(dstLayer + dstPos.Z)};
	dstTexCopy.aspect = wgpu::TextureAspect::All;

	extend3d.width = size.X;
	extend3d.height = size.Y;
	extend3d.depthOrArrayLayers = size.Z;

	commandEncorder_.CopyTextureToTexture(&srcTexCopy, &dstTexCopy, &extend3d);
}

void CommandListWebGPU::GenerateMipMap(Texture* src)
{
	if (!PrepareStandaloneCommand("GenerateMipMap"))
		return;

	auto srcTex = static_cast<TextureWebGPU*>(src);
	if (srcTex == nullptr)
	{
		return;
	}

	srcTex->GenerateMipMaps(commandEncorder_);
}

void CommandListWebGPU::CopyBuffer(Buffer* src, Buffer* dst)
{
	if (!PrepareStandaloneCommand("CopyBuffer"))
		return;

	auto srcBuffer = static_cast<BufferWebGPU*>(src);
	auto dstBuffer = static_cast<BufferWebGPU*>(dst);
	if (srcBuffer == nullptr || dstBuffer == nullptr)
	{
		return;
	}

	commandEncorder_.CopyBufferToBuffer(srcBuffer->GetBuffer(), 0, dstBuffer->GetBuffer(), 0, std::min(srcBuffer->GetSize(), dstBuffer->GetSize()));

	RegisterReferencedObject(src);
	RegisterReferencedObject(dst);
}

void CommandListWebGPU::WaitUntilCompleted()
{
#if !defined(__EMSCRIPTEN__)
	if (device_ != nullptr)
	{
		device_.Tick();
	}
#endif
}

} // namespace LLGI
