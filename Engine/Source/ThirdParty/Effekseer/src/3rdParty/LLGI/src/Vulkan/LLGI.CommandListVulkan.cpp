#include "LLGI.CommandListVulkan.h"
#include "LLGI.BufferVulkan.h"
#include "LLGI.GraphicsVulkan.h"
#include "LLGI.PipelineStateVulkan.h"
#include "LLGI.TextureVulkan.h"
#include "LLGI.QueryVulkan.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace LLGI
{

DescriptorPoolVulkan::DescriptorPoolVulkan(
	std::shared_ptr<GraphicsVulkan> graphics, int32_t slot_size_max, int32_t constant_size, int32_t texture_size, int32_t storage_size)
	: graphics_(graphics), slotSizeMax_(slot_size_max)
{
	{
		std::array<vk::DescriptorPoolSize, 3> poolSizes;
		poolSizes[0].type = vk::DescriptorType::eUniformBufferDynamic;
		poolSizes[0].descriptorCount = slotSizeMax_ * constant_size;
		poolSizes[1].type = vk::DescriptorType::eCombinedImageSampler;
		poolSizes[1].descriptorCount = slotSizeMax_ * texture_size;
		poolSizes[2].type = vk::DescriptorType::eStorageBufferDynamic;
		poolSizes[2].descriptorCount = slotSizeMax_ * storage_size;

		vk::DescriptorPoolCreateInfo poolInfo;
		poolInfo.poolSizeCount = 3;
		poolInfo.pPoolSizes = poolSizes.data();
		poolInfo.maxSets = slotSizeMax_ * 3;

		descriptorPool_ = graphics_->GetDevice().createDescriptorPool(poolInfo);
	}

	{
		std::array<vk::DescriptorPoolSize, 4> poolSizes;
		poolSizes[0].type = vk::DescriptorType::eUniformBufferDynamic;
		poolSizes[0].descriptorCount = slotSizeMax_ * constant_size;
		poolSizes[1].type = vk::DescriptorType::eCombinedImageSampler;
		poolSizes[1].descriptorCount = slotSizeMax_ * texture_size;
		poolSizes[2].type = vk::DescriptorType::eStorageBufferDynamic;
		poolSizes[2].descriptorCount = slotSizeMax_ * storage_size;
		poolSizes[3].type = vk::DescriptorType::eStorageImage;
		poolSizes[3].descriptorCount = slotSizeMax_ * texture_size;

		vk::DescriptorPoolCreateInfo poolInfo;
		poolInfo.poolSizeCount = 4;
		poolInfo.pPoolSizes = poolSizes.data();
		poolInfo.maxSets = slotSizeMax_ * 4;

		computeDescriptorPool_ = graphics_->GetDevice().createDescriptorPool(poolInfo);
	}
}

DescriptorPoolVulkan ::~DescriptorPoolVulkan()
{
	if (descriptorPool_)
	{
		graphics_->GetDevice().destroyDescriptorPool(descriptorPool_);
		descriptorPool_ = nullptr;
	}

	if (computeDescriptorPool_)
	{
		graphics_->GetDevice().destroyDescriptorPool(computeDescriptorPool_);
		computeDescriptorPool_ = nullptr;
	}
	// Destroy all cached sets before releasing the pipelines owning the layouts
	// used to allocate them. Owners are bounded by the existing pool slot count.
	cache.clear();
	computeCache.clear();
}

const std::vector<vk::DescriptorSet>& DescriptorPoolVulkan::Get(PipelineStateVulkan* pip)
{
	if (cache.size() > static_cast<size_t>(offset))
	{
		offset++;
		return cache[offset - 1].Sets;
	}

	if (offset >= slotSizeMax_)
	{
		Log(LogType::Warning, "Lack of allocated memory.");
		return dummySet_;
	}

	// TODO : improve it
	auto& layout = pip->GetDescriptorSetLayout();
	vk::DescriptorSetAllocateInfo allocateInfo;
	allocateInfo.descriptorPool = descriptorPool_;
	allocateInfo.descriptorSetCount = static_cast<int>(layout.size());
	allocateInfo.pSetLayouts = layout.data();

	std::vector<vk::DescriptorSet> descriptorSets = graphics_->GetDevice().allocateDescriptorSets(allocateInfo);
	// Graphics layouts have a fixed binding schema in PipelineStateVulkan;
	// compatible later pipelines may reuse these sets, but their original layout
	// owner must survive every subsequent descriptor write.
	cache.push_back({CreateSharedPtr(pip, true), std::move(descriptorSets)});
	offset++;
	return cache[offset - 1].Sets;
}

const std::vector<vk::DescriptorSet>& DescriptorPoolVulkan::GetCompute(PipelineStateVulkan* pip)
{
	if (computeCache.size() > static_cast<size_t>(computeOffset))
	{
		computeOffset++;
		return computeCache[computeOffset - 1].Sets;
	}

	if (computeOffset >= slotSizeMax_)
	{
		Log(LogType::Warning, "Lack of allocated memory.");
		return dummySet_;
	}

	// TODO : improve it
	auto& layout = pip->GetComputeDescriptorSetLayout();
	vk::DescriptorSetAllocateInfo allocateInfo;
	allocateInfo.descriptorPool = computeDescriptorPool_;
	allocateInfo.descriptorSetCount = static_cast<int>(layout.size());
	allocateInfo.pSetLayouts = layout.data();

	std::vector<vk::DescriptorSet> descriptorSets = graphics_->GetDevice().allocateDescriptorSets(allocateInfo);
	// Compute layouts likewise use a fixed schema, separate from graphics.
	computeCache.push_back({CreateSharedPtr(pip, true), std::move(descriptorSets)});
	computeOffset++;
	return computeCache[computeOffset - 1].Sets;
}

void DescriptorPoolVulkan::Reset()
{
	offset = 0;
	computeOffset = 0;
}

void CommandListVulkan::AssignConstantBuffersToCommandList(const vk::DescriptorSet& descriptorSet,
														   vk::WriteDescriptorSet* descriptorSets,
														   int& descriptorSetOffset,
														   vk::DescriptorBufferInfo* descBuffers,
														   int& descBufferOffset)
{
	for (size_t unit_ind = 0; unit_ind < constantBuffers_.size(); unit_ind++)
	{
		auto cb = static_cast<BufferVulkan*>(constantBuffers_[unit_ind]);
		if (cb == nullptr)
		{
			continue;
		}

		descBuffers[descBufferOffset].buffer = cb->GetBuffer();
		descBuffers[descBufferOffset].offset = cb->GetOffset();
		descBuffers[descBufferOffset].range = cb->GetSize();

		vk::WriteDescriptorSet desc;
		desc.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
		desc.dstSet = descriptorSet;
		desc.dstBinding = static_cast<int>(unit_ind);
		desc.dstArrayElement = 0;
		desc.pBufferInfo = &(descBuffers[descBufferOffset]);
		desc.descriptorCount = 1;

		descriptorSets[descriptorSetOffset] = desc;

		descBufferOffset++;
		descriptorSetOffset++;
	}
}

void CommandListVulkan::AssignStorageBuffersToCommandList(const vk::DescriptorSet& descriptorSet,
														  vk::WriteDescriptorSet* descriptorSets,
														  int& descriptorSetOffset,
														  vk::DescriptorBufferInfo* descBuffers,
														  int& descBufferOffset)
{
	for (int unit_ind = 0; unit_ind < NumStorageBuffer; unit_ind++)
	{
		BindingStorageBuffer cb_;
		GetCurrentStorageBuffer(unit_ind, cb_);

		if (cb_.storageBuffer == nullptr)
			continue;

		auto cb = static_cast<BufferVulkan*>(cb_.storageBuffer);

		descBuffers[descBufferOffset].buffer = cb->GetBuffer();
		descBuffers[descBufferOffset].offset = cb->GetOffset();
		descBuffers[descBufferOffset].range = cb->GetSize();

		vk::WriteDescriptorSet desc;
		desc.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
		desc.dstSet = descriptorSet;
		desc.dstBinding = static_cast<int>(unit_ind);
		desc.dstArrayElement = 0;
		desc.pBufferInfo = &(descBuffers[descBufferOffset]);
		desc.descriptorCount = 1;

		descriptorSets[descriptorSetOffset] = desc;

		descBufferOffset++;
		descriptorSetOffset++;
	}
}

void CommandListVulkan::AssignTexturesToCommandList(const vk::DescriptorSet& descriptorSet,
													vk::WriteDescriptorSet* writeDescriptorSets,
													int& writeDescriptorSetOffset,
													vk::DescriptorImageInfo* descImages,
													int& descImageOffset,
													const std::function<bool(TextureUsageType)>& filter)
{
	for (int32_t unit_ind = 0; unit_ind < static_cast<int32_t>(currentTextures_.size()); unit_ind++)
	{
		auto texture = static_cast<TextureVulkan*>(currentTextures_[unit_ind].texture);
		const auto usage = texture != nullptr ? texture->GetUsage() : TextureUsageType::NoneFlag;
		if (!filter(usage))
		{
			continue;
		}

		if (texture == nullptr)
		{
			texture = fallbackSampledTexture_;
		}

		if (texture == nullptr)
		{
			continue;
		}

		auto wm = (int32_t)currentTextures_[unit_ind].wrapMode;
		auto mm = (int32_t)currentTextures_[unit_ind].minMagFilter;
		vk::DescriptorImageInfo imageInfo;

		if (texture->GetType() == TextureType::Depth)
		{
			//	texture->ResourceBarrier(cmdBuffer, vk::ImageLayout::eDepthStencilReadOnlyOptimal);
			imageInfo.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
		}
		else
		{
			//	texture->ResourceBarrier(cmdBuffer, vk::ImageLayout::eShaderReadOnlyOptimal);
			imageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		}

		imageInfo.imageView = texture->GetView();
		imageInfo.sampler = samplers_[wm][mm];
		if (earlySamplerDiagnostics_ && !earlySamplerAssigned_)
		{
			earlySamplerAssigned_ = true;
			std::fprintf(stderr, "EARLY_LLGI_SAMPLER_ASSIGN this=%p slot=%d wrap=%d filter=%d sampler=%p table00=%p texture=%p swap=%d textures=%zu\n",
				static_cast<void*>(this), unit_ind, wm, mm,
				reinterpret_cast<void*>(static_cast<VkSampler>(imageInfo.sampler)),
				reinterpret_cast<void*>(static_cast<VkSampler>(samplers_[0][0])),
				static_cast<void*>(texture), currentSwapBufferIndex_, currentTextures_.size());
			std::fflush(stderr);
		}
		descImages[descImageOffset] = imageInfo;

		vk::WriteDescriptorSet desc;
		desc.dstSet = descriptorSet;
		desc.dstBinding = unit_ind;
		desc.dstArrayElement = 0;
		desc.pImageInfo = &descImages[descImageOffset];
		desc.descriptorCount = 1;
		desc.descriptorType = vk::DescriptorType::eCombinedImageSampler;

		writeDescriptorSets[writeDescriptorSetOffset] = desc;

		descImageOffset++;
		writeDescriptorSetOffset++;
	}
}

void CommandListVulkan::PrepareStorageBuffersForGraphics()
{
	for (int unit_ind = 0; unit_ind < NumStorageBuffer; unit_ind++)
	{
		BindingStorageBuffer cb_;
		GetCurrentStorageBuffer(unit_ind, cb_);

		if (cb_.storageBuffer == nullptr)
			continue;

		auto cb = static_cast<BufferVulkan*>(cb_.storageBuffer);
		cb->ResourceBarrier(currentCommandBuffer_, BufferVulkanAccess::ShaderRead);
	}
}

CommandListVulkan::~CommandListVulkan()
{
	if (commandBuffers_.size() > 0)
	{
		graphics_->GetDevice().freeCommandBuffers(graphics_->GetCommandPool(), commandBuffers_);
	}
	commandBuffers_.clear();

	descriptorPools.clear();

	for (size_t i = 0; i < fences_.size(); i++)
	{
		if (fences_[i])
		{
			graphics_->GetDevice().destroyFence(fences_[i]);
		}
	}
	fences_.clear();

	for (int w = 0; w < 3; w++)
	{
		for (int f = 0; f < 2; f++)
		{
			if (samplers_[w][f])
			{
				graphics_->GetDevice().destroySampler(samplers_[w][f]);
			}
		}
	}

	SafeRelease(fallbackSampledTexture_);
}

bool CommandListVulkan::CreateFallbackSampledTexture()
{
	TextureParameter fallbackTextureParameter;
	auto fallbackTexture = static_cast<TextureVulkan*>(graphics_->CreateTexture(fallbackTextureParameter));
	if (fallbackTexture == nullptr)
	{
		return false;
	}

	auto fallbackTextureData = static_cast<uint8_t*>(fallbackTexture->Lock());
	if (fallbackTextureData == nullptr)
	{
		SafeRelease(fallbackTexture);
		return false;
	}

	fallbackTextureData[0] = 255;
	fallbackTextureData[1] = 255;
	fallbackTextureData[2] = 255;
	fallbackTextureData[3] = 255;
	fallbackTexture->Unlock();

	fallbackSampledTexture_ = fallbackTexture;
	return true;
}

bool CommandListVulkan::Initialize(GraphicsVulkan* graphics, int32_t drawingCount)
{
	#if defined(HE_EFFEKSEER_DEV_TEST_HOOKS) && HE_EFFEKSEER_DEV_TEST_HOOKS
	const char* diagnostic = std::getenv("HE_VFX_EARLY_SMOKE");
#else
	const char* diagnostic = nullptr; // No environment-controlled SDK diagnostics in shipping.
#endif
	static std::atomic<unsigned> diagnosticLists{0};
	earlySamplerDiagnostics_ = diagnostic && diagnostic[0] == '1' && diagnostic[1] == '\0' &&
		diagnosticLists.fetch_add(1) < 32;
	SafeAddRef(graphics);
	graphics_ = CreateSharedPtr(graphics);

	vk::CommandBufferAllocateInfo allocInfo;
	allocInfo.commandPool = graphics->GetCommandPool();
	allocInfo.level = vk::CommandBufferLevel::ePrimary;
	allocInfo.commandBufferCount = graphics->GetSwapBufferCount();
	commandBuffers_ = graphics->GetDevice().allocateCommandBuffers(allocInfo);

	for (size_t i = 0; i < static_cast<size_t>(graphics_->GetSwapBufferCount()); i++)
	{
		auto dp = std::make_shared<DescriptorPoolVulkan>(graphics_, drawingCount, 4, 8, 8);
		descriptorPools.push_back(dp);

		fences_.emplace_back(vk::Fence{});
	}

	// Sampler
	for (int w = 0; w < 3; w++)
	{
		for (int f = 0; f < 2; f++)
		{
			vk::Filter filters[2];
			filters[0] = vk::Filter::eNearest;
			filters[1] = vk::Filter::eLinear;

			vk::SamplerAddressMode am[3];
			am[0] = vk::SamplerAddressMode::eClampToEdge;
			am[1] = vk::SamplerAddressMode::eRepeat;
			am[2] = vk::SamplerAddressMode::eMirroredRepeat;

			vk::SamplerCreateInfo samplerInfo;
			samplerInfo.magFilter = filters[f];
			samplerInfo.minFilter = filters[f];
			samplerInfo.anisotropyEnable = false;
			samplerInfo.maxAnisotropy = 1;
			samplerInfo.addressModeU = am[w];
			samplerInfo.addressModeV = am[w];
			samplerInfo.addressModeW = am[w];
			samplerInfo.borderColor = vk::BorderColor::eIntOpaqueBlack;
			samplerInfo.unnormalizedCoordinates = false;
			samplerInfo.compareEnable = false;
			samplerInfo.compareOp = vk::CompareOp::eAlways;
			samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
			samplerInfo.mipLodBias = 0.0f;
			samplerInfo.minLod = 0.0f;
			samplerInfo.maxLod = 8.0f;

			samplers_[w][f] = graphics_->GetDevice().createSampler(samplerInfo);
			if (earlySamplerDiagnostics_)
			{
				std::fprintf(stderr, "EARLY_LLGI_SAMPLER_CREATE this=%p wrap=%d filter=%d sampler=%p\n",
					static_cast<void*>(this), w, f, reinterpret_cast<void*>(static_cast<VkSampler>(samplers_[w][f])));
				std::fflush(stderr);
			}
		}
	}

	if (!CreateFallbackSampledTexture())
	{
		return false;
	}

	currentSwapBufferIndex_ = -1;
	return true;
}

void CommandListVulkan::Begin()
{
	currentSwapBufferIndex_++;
	currentSwapBufferIndex_ %= commandBuffers_.size();

	if (fences_[currentSwapBufferIndex_])
	{
		WaitUntilCompleted();
	}
	else
	{
		fences_[currentSwapBufferIndex_] = graphics_->GetDevice().createFence(vk::FenceCreateFlags());
	}

	const auto recetFencesResult = graphics_->GetDevice().resetFences(1, &(fences_[currentSwapBufferIndex_]));
	if (recetFencesResult != vk::Result::eSuccess)
	{
		LLGI::Log(LogType::Error, "Failed to resetFences");
		return;
	}

	currentCommandBuffer_ = commandBuffers_[currentSwapBufferIndex_];
	currentCommandBuffer_.reset(vk::CommandBufferResetFlagBits::eReleaseResources);
	vk::CommandBufferBeginInfo cmdBufInfo;
	currentCommandBuffer_.begin(cmdBufInfo);

	auto& dp = descriptorPools[currentSwapBufferIndex_];
	dp->Reset();

	CommandList::Begin();
}

void CommandListVulkan::End()
{
	currentCommandBuffer_.end();
	CommandList::End();
}

bool CommandListVulkan::BeginWithPlatform(void* platformContextPtr)
{
	auto ptr = reinterpret_cast<PlatformContextVulkan*>(platformContextPtr);

	currentSwapBufferIndex_++;
	currentSwapBufferIndex_ %= commandBuffers_.size();

	currentCommandBuffer_ = vk::CommandBuffer(ptr->commandBuffer);

	auto& dp = descriptorPools[currentSwapBufferIndex_];
	dp->Reset();

	return CommandList::BeginWithPlatform(platformContextPtr);
}

void CommandListVulkan::EndWithPlatform()
{
	currentCommandBuffer_ = vk::CommandBuffer();
	CommandList::EndWithPlatform();
}

bool CommandListVulkan::BeginRenderPassWithPlatformPtr(void* platformPtr)
{
	isInValidRenderPass_ = true;
	return CommandList::BeginRenderPassWithPlatformPtr(platformPtr);
}

bool CommandListVulkan::EndRenderPassWithPlatformPtr()
{
	isInValidRenderPass_ = false;
	CommandList::EndRenderPass();
	return true;
}

void CommandListVulkan::SetScissor(int32_t x, int32_t y, int32_t width, int32_t height)
{
	if (!isInValidRenderPass_)
	{
		Log(LogType::Warning, "SetScissor must be called in RenderPass.");
		return;
	}

	vk::Rect2D scissor = vk::Rect2D(vk::Offset2D(x, y), vk::Extent2D(width, height));
	currentCommandBuffer_.setScissor(0, scissor);
}

void CommandListVulkan::Draw(int32_t primitiveCount, int32_t instanceCount)
{
	if (!isInValidRenderPass_)
	{
		Log(LogType::Warning, "Draw must be called in RenderPass.");
		return;
	}

	BindingVertexBuffer vb_;
	BindingIndexBuffer ib_;
	PipelineState* pip_ = nullptr;

	bool isVBDirtied = false;
	bool isIBDirtied = false;
	bool isPipDirtied = false;

	if (!ValidateDrawState("CommandListVulkan", primitiveCount, instanceCount, vb_, ib_, pip_, isVBDirtied, isIBDirtied, isPipDirtied))
	{
		return;
	}

	auto vb = static_cast<BufferVulkan*>(vb_.vertexBuffer);
	auto ib = static_cast<BufferVulkan*>(ib_.indexBuffer);
	auto pip = static_cast<PipelineStateVulkan*>(pip_);

	if (renderPass_ != nullptr && pip->GetRenderPassPipelineState()->Key != renderPass_->GetKey())
	{
		Log(LogType::Warning, "Pipeline states between Pipeline state and render pass is different.");
		return;
	}

	// assign a vertex buffer
	if (isVBDirtied)
	{
		vk::DeviceSize vertexOffsets = vb_.offset;
		vk::Buffer vkBuf = vb->GetBuffer();
		currentCommandBuffer_.bindVertexBuffers(0, 1, &(vkBuf), &vertexOffsets);
	}

	// assign an index vuffer
	if (isIBDirtied)
	{
		vk::DeviceSize indexOffset = ib_.offset;
		vk::IndexType indexType = vk::IndexType::eUint16;

		if (ib_.stride == 2)
		{
			indexType = vk::IndexType::eUint16;
		}
		else if (ib_.stride == 4)
		{
			indexType = vk::IndexType::eUint32;
		}

		currentCommandBuffer_.bindIndexBuffer(ib->GetBuffer(), indexOffset, indexType);
	}

	auto& dp = descriptorPools[currentSwapBufferIndex_];

	const auto& descriptorSets = dp->Get(pip);
	if (descriptorSets.size() == 0)
	{
		return;
	}

	std::array<vk::WriteDescriptorSet, NumStorageBuffer + NumTexture + NumConstantBuffer> writeDescriptorSets;
	int writeDescriptorIndex = 0;

	std::array<vk::DescriptorBufferInfo, NumStorageBuffer + NumTexture + NumConstantBuffer> descriptorBufferInfos;
	int descriptorBufferIndex = 0;

	std::array<vk::DescriptorImageInfo, NumTexture> descriptorImageInfos;
	int descriptorImageIndex = 0;

	AssignConstantBuffersToCommandList(
		descriptorSets[0], writeDescriptorSets.data(), writeDescriptorIndex, descriptorBufferInfos.data(), descriptorBufferIndex);

	AssignTexturesToCommandList(descriptorSets[1],
								writeDescriptorSets.data(),
								writeDescriptorIndex,
								descriptorImageInfos.data(),
								descriptorImageIndex,
								[](TextureUsageType t) -> bool { return true; });

	const unsigned diagnosticDraw = earlySamplerDiagnostics_ ? ++earlySamplerDrawCount_ : 0;
	const auto diagnoseDrawDescriptors = [&](const char* phase) {
		if (!earlySamplerDiagnostics_ || diagnosticDraw > 16) return;
		if (diagnosticDraw == 1)
			std::fprintf(stderr, "EARLY_LLGI_DRAW_ABI phase=%s writeSize=%zu/%zu imageSize=%zu/%zu imagePtrOffset=%zu/%zu samplerOffset=%zu/%zu\n",
				phase, sizeof(vk::WriteDescriptorSet), sizeof(VkWriteDescriptorSet),
				sizeof(vk::DescriptorImageInfo), sizeof(VkDescriptorImageInfo),
				offsetof(vk::WriteDescriptorSet, pImageInfo), offsetof(VkWriteDescriptorSet, pImageInfo),
				offsetof(vk::DescriptorImageInfo, sampler), offsetof(VkDescriptorImageInfo, sampler));
		for (int i = 0; i < writeDescriptorIndex; ++i)
		{
			const auto& write = writeDescriptorSets[i];
			if (write.descriptorType != vk::DescriptorType::eCombinedImageSampler) continue;
			const auto& textureBinding = currentTextures_[write.dstBinding];
			std::fprintf(stderr, "EARLY_LLGI_DRAW_WRITE phase=%s this=%p ordinal=%u pipeline=%p dstSet=%p write=%d binding=%u wrap=%d filter=%d sampler=%p nativeSampler=%p imageInfo=%p imageBase=%p baseSampler=%p writes=%d buffers=%d images=%d\n",
				phase, static_cast<void*>(this), diagnosticDraw, static_cast<void*>(pip),
				reinterpret_cast<void*>(static_cast<VkDescriptorSet>(write.dstSet)), i, write.dstBinding,
				static_cast<int>(textureBinding.wrapMode), static_cast<int>(textureBinding.minMagFilter),
				reinterpret_cast<void*>(static_cast<VkSampler>(write.pImageInfo->sampler)),
				reinterpret_cast<void*>(reinterpret_cast<const VkDescriptorImageInfo*>(write.pImageInfo)->sampler),
				static_cast<const void*>(write.pImageInfo), static_cast<const void*>(descriptorImageInfos.data()),
				reinterpret_cast<void*>(static_cast<VkSampler>(descriptorImageInfos[0].sampler)),
				writeDescriptorIndex, descriptorBufferIndex, descriptorImageIndex);
		}
		std::fflush(stderr);
	};
	diagnoseDrawDescriptors("after-textures");

	AssignStorageBuffersToCommandList(
		descriptorSets[2], writeDescriptorSets.data(), writeDescriptorIndex, descriptorBufferInfos.data(), descriptorBufferIndex);

	if (writeDescriptorIndex > 0)
	{
		diagnoseDrawDescriptors("before-vkUpdateDescriptorSets");
		graphics_->GetDevice().updateDescriptorSets(writeDescriptorIndex, writeDescriptorSets.data(), 0, nullptr);
		if (earlySamplerDiagnostics_ && diagnosticDraw <= 16)
		{
			std::fprintf(stderr, "EARLY_LLGI_DRAW_API_RETURN this=%p ordinal=%u dstSet=%p\n",
				static_cast<void*>(this), diagnosticDraw,
				reinterpret_cast<void*>(static_cast<VkDescriptorSet>(descriptorSets[1])));
			std::fflush(stderr);
		}
	}

	std::array<uint32_t, 12> offsets;
	offsets.fill(0);

	currentCommandBuffer_.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics, pip->GetPipelineLayout(), 0, 3, descriptorSets.data(), 12, offsets.data());

	// assign a pipeline
	if (isPipDirtied)
	{
		currentCommandBuffer_.bindPipeline(vk::PipelineBindPoint::eGraphics, pip->GetPipeline());
	}

	// draw
	const auto indexPerPrim = GetIndexCountPerPrimitive(pip->Topology);
	if (indexPerPrim == 0)
	{
		Log(LogType::Error, "CommandListVulkan::Draw skipped: unsupported topology. topology=" + to_string(pip->Topology));
		return;
	}

	currentCommandBuffer_.drawIndexed(indexPerPrim * primitiveCount, instanceCount, 0, 0, 0);

	CommandList::Draw(primitiveCount, instanceCount);
}

void CommandListVulkan::CopyTexture(Texture* src, Texture* dst)
{
	auto srcTex = static_cast<TextureVulkan*>(src);
	CopyTexture(src, dst, {0, 0, 0}, {0, 0, 0}, srcTex->GetParameter().Size, 0, 0);
}

void CommandListVulkan::CopyTexture(
	Texture* src, Texture* dst, const Vec3I& srcPos, const Vec3I& dstPos, const Vec3I& size, int srcLayer, int dstLayer)
{
	if (isInRenderPass_ && !doesBeginWithPlatform_)
	{
		Log(LogType::Error, "Please call CopyTexture outside of RenderPass");
		return;
	}

	auto srcTex = static_cast<TextureVulkan*>(src);
	auto dstTex = static_cast<TextureVulkan*>(dst);

	if (srcTex->GetMipmapCount() != dstTex->GetMipmapCount())
	{
		Log(LogType::Error, "CopyTexture : MipLevel is different.");
		return;
	}

	if (srcTex->GetMipmapCount() != 1)
	{
		Log(LogType::Error, "CopyTexture : MipLevel is not supported.");
		return;
	}

	std::array<vk::ImageCopy, 1> imageCopy;
	imageCopy[0].dstOffset = vk::Offset3D(dstPos[0], dstPos[1], dstPos[2]);
	imageCopy[0].srcOffset = vk::Offset3D(srcPos[0], srcPos[1], srcPos[2]);
	imageCopy[0].extent.width = size[0];
	imageCopy[0].extent.height = size[1];
	imageCopy[0].extent.depth = size[2];
	imageCopy[0].srcSubresource.aspectMask = srcTex->GetSubresourceRange().aspectMask;
	imageCopy[0].srcSubresource.layerCount = 1;
	imageCopy[0].srcSubresource.baseArrayLayer = srcLayer;
	imageCopy[0].dstSubresource.aspectMask = dstTex->GetSubresourceRange().aspectMask;
	imageCopy[0].dstSubresource.layerCount = 1;
	imageCopy[0].dstSubresource.baseArrayLayer = dstLayer;

	srcTex->ResourceBarrier(currentCommandBuffer_, vk::ImageLayout::eTransferSrcOptimal);
	dstTex->ResourceBarrier(currentCommandBuffer_, vk::ImageLayout::eTransferDstOptimal);
	currentCommandBuffer_.copyImage(
		srcTex->GetImage(), srcTex->GetImageLayouts()[0], dstTex->GetImage(), dstTex->GetImageLayouts()[0], imageCopy);
	dstTex->ResourceBarrier(currentCommandBuffer_, vk::ImageLayout::eShaderReadOnlyOptimal);
	srcTex->ResourceBarrier(currentCommandBuffer_, vk::ImageLayout::eShaderReadOnlyOptimal);

	RegisterReferencedObject(src);
	RegisterReferencedObject(dst);
}

void CommandListVulkan::GenerateMipMap(Texture* src)
{
	auto srcTex = static_cast<TextureVulkan*>(src);
	const auto& parameter = srcTex->GetParameter();

	if (!parameter.IsMipmapGenerationEnabled || IsBlockCompressedFormat(parameter.Format))
	{
		return;
	}
	if (BitwiseContains(parameter.Usage, TextureUsageType::Array))
	{
		Log(LogType::Error, "CommandListVulkan::GenerateMipMap skipped: mipmap generation for texture arrays is currently not supported by LLGI.");
		return;
	}

	if (srcTex->RecordMipmapGenerationCommands(currentCommandBuffer_))
	{
		// Flushing queued mipmaps releases the queue's texture reference. A
		// rejected/unloaded effect may have no other owner, so retain the image
		// with this recording until command-list reuse after host retirement.
		RegisterReferencedObject(src);
	}
}

void CommandListVulkan::CopyBuffer(Buffer* src, Buffer* dst)
{
	auto srcBuf = static_cast<BufferVulkan*>(src);
	auto srcGpuBuf = srcBuf->GetBuffer();

	auto dstBuf = static_cast<BufferVulkan*>(dst);
	auto dstGpuBuf = dstBuf->GetBuffer();

	vk::BufferCopy copyRegion;
	copyRegion.srcOffset = srcBuf->GetOffset();
	copyRegion.dstOffset = dstBuf->GetOffset();
	copyRegion.size = std::min(srcBuf->GetSize(), dstBuf->GetSize());

	srcBuf->ResourceBarrier(currentCommandBuffer_, BufferVulkanAccess::TransferRead);
	dstBuf->ResourceBarrier(currentCommandBuffer_, BufferVulkanAccess::TransferWrite);
	currentCommandBuffer_.copyBuffer(srcGpuBuf, dstGpuBuf, copyRegion);
	dstBuf->ResourceBarrier(currentCommandBuffer_, BufferVulkanAccess::ShaderRead);

	RegisterReferencedObject(src);
	RegisterReferencedObject(dst);
}

void CommandListVulkan::BeginRenderPass(RenderPass* renderPass)
{
	renderPass_ = static_cast<RenderPassVulkan*>(renderPass);
	if (!renderPass_->GetIsValid())
	{
		CommandList::BeginRenderPass(renderPass);
		return;
	}

		PrepareStorageBuffersForGraphics();

	vk::ClearColorValue clearColor(std::array<float, 4>{renderPass_->GetClearColor().R / 255.0f,
														renderPass_->GetClearColor().G / 255.0f,
														renderPass_->GetClearColor().B / 255.0f,
														renderPass_->GetClearColor().A / 255.0f});
	vk::ClearDepthStencilValue clearDepth(1.0f, 0);

	vk::ImageSubresourceRange colorSubRange;
	colorSubRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	colorSubRange.levelCount = 1;
	colorSubRange.layerCount = 1;

	vk::ImageSubresourceRange depthSubRange;
	depthSubRange.aspectMask = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
	depthSubRange.levelCount = 1;
	depthSubRange.layerCount = 1;

	vk::ClearValue clear_values[RenderTargetMax + 1];
	int clearValueCount = 0;

	if (renderPass->GetIsColorCleared())
	{
		for (int32_t i = 0; i < renderPass_->GetRenderTextureCount(); i++)
		{
			clear_values[i].color = clearColor;
			clearValueCount++;
		}
	}

	if (renderPass_->GetHasDepthTexture() && renderPass->GetIsDepthCleared())
	{
		clear_values[renderPass_->GetRenderTextureCount()].depthStencil = clearDepth;
		clearValueCount++;
	}

	clear_values[renderPass_->GetRenderTextureCount()].depthStencil = clearDepth;

	if (renderPass_->GetHasDepthTexture())
	{
		auto t = static_cast<TextureVulkan*>(renderPass_->GetDepthTexture());
		t->ResourceBarrier(currentCommandBuffer_, vk::ImageLayout::eDepthStencilAttachmentOptimal);
	}

	// begin renderpass
	vk::RenderPassBeginInfo renderPassBeginInfo;
	renderPassBeginInfo.framebuffer = renderPass_->frameBuffer_;
	renderPassBeginInfo.renderPass = renderPass_->renderPassPipelineState->GetRenderPass();
	renderPassBeginInfo.renderArea.extent = vk::Extent2D(renderPass_->GetImageSize().X, renderPass_->GetImageSize().Y);
	renderPassBeginInfo.clearValueCount = clearValueCount;
	renderPassBeginInfo.pClearValues = clear_values;
	currentCommandBuffer_.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);

	vk::Viewport viewport = vk::Viewport(
		0.0f, 0.0f, static_cast<float>(renderPass_->GetImageSize().X), static_cast<float>(renderPass_->GetImageSize().Y), 0.0f, 1.0f);
	currentCommandBuffer_.setViewport(0, viewport);

	vk::Rect2D scissor = vk::Rect2D(vk::Offset2D(), vk::Extent2D(renderPass_->GetImageSize().X, renderPass_->GetImageSize().Y));
	currentCommandBuffer_.setScissor(0, scissor);

	auto layoutOffset = 0;
	for (int32_t i = 0; i < renderPass_->GetRenderTextureCount(); i++)
	{
		auto t = static_cast<TextureVulkan*>(renderPass_->GetRenderTexture(i));
		t->ChangeImageLayout(renderPass_->renderPassPipelineState->finalLayouts_.at(i));
	}

	layoutOffset += renderPass_->GetRenderTextureCount();

	if (renderPass_->GetHasDepthTexture())
	{
		auto t = static_cast<TextureVulkan*>(renderPass_->GetDepthTexture());
		t->ChangeImageLayout(renderPass_->renderPassPipelineState->finalLayouts_.at(layoutOffset));
	}

	if (renderPass_->GetHasDepthTexture())
	{
		layoutOffset += 1;
	}

	if (auto t = static_cast<TextureVulkan*>(renderPass_->GetResolvedRenderTexture()))
	{
		t->ChangeImageLayout(renderPass_->renderPassPipelineState->finalLayouts_.at(layoutOffset));
	}

	isInValidRenderPass_ = true;
	CommandList::BeginRenderPass(renderPass);
}

void CommandListVulkan::EndRenderPass()
{
	// end renderpass
	if (isInValidRenderPass_)
	{
		currentCommandBuffer_.endRenderPass();
	}
	isInValidRenderPass_ = false;
	renderPass_ = nullptr;
	CommandList::EndRenderPass();
}

vk::CommandBuffer CommandListVulkan::GetCommandBuffer() const { return currentCommandBuffer_; }

vk::Fence CommandListVulkan::GetFence() const { return fences_[currentSwapBufferIndex_]; }

bool CommandListVulkan::ResetQuery(Query* query)
{
	auto query_ = static_cast<QueryVulkan*>(query);
	if (query_ == nullptr)
	{
		return false;
	}

	currentCommandBuffer_.resetQueryPool(query_->GetQueryPool(), 0, query_->GetQueryCount());

	return true;
}

bool CommandListVulkan::BeginQuery(Query* query, uint32_t queryIndex)
{
	auto query_ = static_cast<QueryVulkan*>(query);
	if (query_ == nullptr)
	{
		return false;
	}

	currentCommandBuffer_.beginQuery(query_->GetQueryPool(), queryIndex, vk::QueryControlFlagBits::ePrecise);

	return true;
}

bool CommandListVulkan::EndQuery(Query* query, uint32_t queryIndex)
{
	auto query_ = static_cast<QueryVulkan*>(query);
	if (query_ == nullptr)
	{
		return false;
	}

	currentCommandBuffer_.endQuery(query_->GetQueryPool(), queryIndex);

	return true;
}

bool CommandListVulkan::RecordTimestamp(Query* query, uint32_t queryIndex)
{
	auto query_ = static_cast<QueryVulkan*>(query);
	if (query_ == nullptr)
	{
		return false;
	}

	auto pipelineStage = (queryIndex % 2 == 0) ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eBottomOfPipe;
	currentCommandBuffer_.writeTimestamp(pipelineStage, query_->GetQueryPool(), queryIndex);

	return true;
}

void CommandListVulkan::BeginComputePass() {}

void CommandListVulkan::EndComputePass() {}

void CommandListVulkan::Dispatch(int32_t groupX, int32_t groupY, int32_t groupZ, int32_t threadX, int32_t threadY, int32_t threadZ)
{
	PipelineState* pip_ = nullptr;

	bool isPipDirtied = false;

	if (!ValidateDispatchState("CommandListVulkan", groupX, groupY, groupZ, threadX, threadY, threadZ, pip_, isPipDirtied))
	{
		return;
	}

	auto pip = static_cast<PipelineStateVulkan*>(pip_);

	auto& dp = descriptorPools[currentSwapBufferIndex_];

	const auto& descriptorSets = dp->GetCompute(pip);
	if (descriptorSets.size() == 0)
	{
		return;
	}

	std::array<vk::WriteDescriptorSet, NumConstantBuffer + NumTexture * 2 + NumStorageBuffer> writeDescriptorSets;
	int writeDescriptorIndex = 0;

	std::array<vk::DescriptorBufferInfo, NumConstantBuffer + NumTexture * 2 + NumStorageBuffer> descriptorBufferInfos;
	int descriptorBufferIndex = 0;

	std::array<vk::DescriptorImageInfo, NumTexture> descriptorImageInfos;
	int descriptorImageIndex = 0;

	std::array<vk::DescriptorImageInfo, NumTexture> descriptorImageStorageInfos;
	int descriptorImageStorageIndex = 0;

	AssignConstantBuffersToCommandList(
		descriptorSets[0], writeDescriptorSets.data(), writeDescriptorIndex, descriptorBufferInfos.data(), descriptorBufferIndex);

	AssignTexturesToCommandList(descriptorSets[1],
								writeDescriptorSets.data(),
								writeDescriptorIndex,
								descriptorImageInfos.data(),
								descriptorImageIndex,
								[](TextureUsageType t) -> bool { return !BitwiseContains(t, TextureUsageType::Storage); });

	// Assign textures
	for (int unit_ind = 0; unit_ind < static_cast<int32_t>(currentTextures_.size()); unit_ind++)
	{
		if (currentTextures_[unit_ind].texture == nullptr ||
			!BitwiseContains(currentTextures_[unit_ind].texture->GetUsage(), TextureUsageType::Storage))
		{
			continue;
		}

		const auto targetImageLayout = vk::ImageLayout::eGeneral;
		auto texture = (TextureVulkan*)currentTextures_[unit_ind].texture;
		texture->ResourceBarrier(currentCommandBuffer_, targetImageLayout);

		vk::DescriptorImageInfo imageInfo;
		imageInfo.imageLayout = targetImageLayout;
		imageInfo.imageView = texture->GetView();
		descriptorImageStorageInfos[descriptorImageStorageIndex] = imageInfo;

		vk::WriteDescriptorSet desc;
		desc.dstSet = descriptorSets[3];
		desc.dstBinding = unit_ind;
		desc.dstArrayElement = 0;
		desc.pImageInfo = &descriptorImageStorageInfos[descriptorImageStorageIndex];
		desc.descriptorCount = 1;
		desc.descriptorType = vk::DescriptorType::eStorageImage;

		writeDescriptorSets[writeDescriptorIndex] = desc;

		descriptorImageStorageIndex++;
		writeDescriptorIndex++;
	}

	AssignStorageBuffersToCommandList(
		descriptorSets[2], writeDescriptorSets.data(), writeDescriptorIndex, descriptorBufferInfos.data(), descriptorBufferIndex);

	// Assign storage buffers
	for (int unit_ind = 0; unit_ind < NumStorageBuffer; unit_ind++)
	{
		BindingStorageBuffer cb_;
		GetCurrentStorageBuffer(unit_ind, cb_);

		if (cb_.storageBuffer == nullptr)
			continue;

		auto cb = static_cast<BufferVulkan*>(cb_.storageBuffer);
		cb->ResourceBarrier(
			currentCommandBuffer_, IsReadOnly(cb_.binding.Access) ? BufferVulkanAccess::ShaderRead : BufferVulkanAccess::ShaderReadWrite);
	}

	if (writeDescriptorIndex > 0)
	{
		if (earlySamplerDiagnostics_ && !earlySamplerSubmitted_)
		{
			earlySamplerSubmitted_ = true;
			for (int i = 0; i < writeDescriptorIndex; ++i)
			{
				const auto& write = writeDescriptorSets[i];
				if (write.descriptorType != vk::DescriptorType::eCombinedImageSampler) continue;
				std::fprintf(stderr, "EARLY_LLGI_SAMPLER_WRITE this=%p write=%d binding=%u sampler=%p imageInfo=%p count=%d groups=%d,%d,%d\n",
					static_cast<void*>(this), i, write.dstBinding,
					reinterpret_cast<void*>(static_cast<VkSampler>(write.pImageInfo->sampler)),
					static_cast<const void*>(write.pImageInfo), writeDescriptorIndex, groupX, groupY, groupZ);
				break;
			}
			std::fflush(stderr);
		}
		graphics_->GetDevice().updateDescriptorSets(writeDescriptorIndex, writeDescriptorSets.data(), 0, nullptr);
	}

	std::array<uint32_t, 12> offsets;
	offsets.fill(0);

	currentCommandBuffer_.bindDescriptorSets(
		vk::PipelineBindPoint::eCompute, pip->GetComputePipelineLayout(), 0, 4, descriptorSets.data(), 12, offsets.data());

	// assign a pipeline
	if (isPipDirtied)
	{
		currentCommandBuffer_.bindPipeline(vk::PipelineBindPoint::eCompute, pip->GetComputePipeline());
	}

	currentCommandBuffer_.dispatch(groupX, groupY, groupZ);

	// External render passes cannot contain compute-to-vertex dependencies.
	// Finish each compute write while still outside the host render pass.
	PrepareStorageBuffersForGraphics();

	CommandList::Dispatch(groupX, groupY, groupZ, threadX, threadY, threadZ);
}

void CommandListVulkan::WaitUntilCompleted()
{
	if (currentSwapBufferIndex_ >= 0)
	{
		vk::Result fenceRes =
			graphics_->GetDevice().waitForFences(fences_[currentSwapBufferIndex_], VK_TRUE, std::numeric_limits<int>::max());
		if (fenceRes != vk::Result::eSuccess)
		{
			throw "Invalid waitForFences";
		}
	}
}

} // namespace LLGI
