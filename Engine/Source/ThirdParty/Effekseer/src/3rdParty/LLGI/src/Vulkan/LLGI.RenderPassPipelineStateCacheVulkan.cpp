#include "LLGI.RenderPassPipelineStateCacheVulkan.h"

namespace LLGI
{

RenderPassPipelineStateCacheVulkan::RenderPassPipelineStateCacheVulkan(vk::Device device, ReferenceObject* owner)
	: device_(device), owner_(owner)
{
	SafeAddRef(owner_);
}

RenderPassPipelineStateCacheVulkan::~RenderPassPipelineStateCacheVulkan()
{
	renderPassPipelineStates_.clear();
	SafeRelease(owner_);
}

RenderPassPipelineStateVulkan* RenderPassPipelineStateCacheVulkan::Create(const RenderPassPipelineStateKey key)
{
	// already?
	{
		auto it = renderPassPipelineStates_.find(key);

		if (it != renderPassPipelineStates_.end())
		{
			auto ret = it->second;

			if (ret != nullptr)
			{
				auto retptr = ret.get();
				SafeAddRef(retptr);
				return retptr;
			}
		}
	}

	// settings
	bool hasDepth = key.DepthFormat != TextureFormatType::Unknown;
	FixedSizeVector<vk::AttachmentDescription, RenderTargetMax + 1> attachmentDescs;
	FixedSizeVector<vk::AttachmentReference, RenderTargetMax + 1> attachmentRefs;
	FixedSizeVector<vk::ImageLayout, RenderTargetMax + 1> finalLayouts;

	int colorCount = static_cast<int32_t>(key.RenderTargetFormats.size());
	int depthCount = key.DepthFormat != TextureFormatType::Unknown ? 1 : 0;

	attachmentDescs.resize(colorCount + depthCount);
	attachmentRefs.resize(colorCount + depthCount);

	// color buffer

	for (int i = 0; i < colorCount; i++)
	{
		attachmentDescs.at(i).format = (vk::Format)VulkanHelper::TextureFormatToVkFormat(key.RenderTargetFormats.at(i));
		attachmentDescs.at(i).samples = (vk::SampleCountFlagBits)key.SamplingCount;

		if (key.IsColorCleared)
			attachmentDescs.at(i).loadOp = vk::AttachmentLoadOp::eClear;
		else
			attachmentDescs.at(i).loadOp = vk::AttachmentLoadOp::eDontCare;

		attachmentDescs.at(i).storeOp = vk::AttachmentStoreOp::eStore;
		attachmentDescs.at(i).stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
		attachmentDescs.at(i).stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
	}

	if (key.IsPresent)
	{
		// When clearing, the initialLayout does not matter.
		attachmentDescs.at(0).initialLayout = (key.IsColorCleared) ? vk::ImageLayout::eUndefined : vk::ImageLayout::ePresentSrcKHR;
		attachmentDescs.at(0).finalLayout = vk::ImageLayout::ePresentSrcKHR;
	}
	else
	{
		for (int i = 0; i < colorCount; i++)
		{
			// When clearing, the initialLayout does not matter.
			attachmentDescs.at(i).initialLayout =
				(key.IsColorCleared) ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal;
			attachmentDescs.at(i).finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		}
	}

	// depth buffer
	if (hasDepth)
	{
		attachmentDescs.at(colorCount).format = (vk::Format)VulkanHelper::TextureFormatToVkFormat(key.DepthFormat);
		attachmentDescs.at(colorCount).samples = (vk::SampleCountFlagBits)key.SamplingCount;

		if (key.IsDepthCleared)
		{
			attachmentDescs.at(colorCount).loadOp = vk::AttachmentLoadOp::eClear;
			attachmentDescs.at(colorCount).stencilLoadOp = vk::AttachmentLoadOp::eClear;
		}
		else
		{
			attachmentDescs.at(colorCount).loadOp = vk::AttachmentLoadOp::eDontCare;
			attachmentDescs.at(colorCount).stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
		}

		attachmentDescs.at(colorCount).storeOp = vk::AttachmentStoreOp::eStore;
		attachmentDescs.at(colorCount).stencilStoreOp = HasStencil(key.DepthFormat) ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare;

		// When clearing, the initialLayout does not matter.
		attachmentDescs.at(colorCount).initialLayout =
			(key.IsDepthCleared) ? vk::ImageLayout::eUndefined : vk::ImageLayout::eDepthStencilAttachmentOptimal;
		attachmentDescs.at(colorCount).finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
	}

	// resolve
	int32_t resolveIndex = -1;
	if (key.HasResolvedRenderTarget)
	{
		attachmentDescs.resize(attachmentDescs.size() + 1);
		auto& desc = attachmentDescs.at(attachmentDescs.size() - 1);
		resolveIndex = static_cast<int32_t>(attachmentDescs.size()) - 1;

		desc.format = (vk::Format)VulkanHelper::TextureFormatToVkFormat(key.RenderTargetFormats.at(0));
		desc.samples = vk::SampleCountFlagBits::e1;
		desc.loadOp = vk::AttachmentLoadOp::eDontCare;
		desc.storeOp = vk::AttachmentStoreOp::eStore;
		desc.initialLayout = vk::ImageLayout::eUndefined;
		desc.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	}

	// int32_t resolveDepthIndex = -1;
	if (key.HasResolvedDepthTarget)
	{
		// Wait 1.2
		// attachmentDescs.resize(attachmentDescs.size() + 1);
		// auto& desc = attachmentDescs.at(attachmentDescs.size() - 1);
		// resolveDepthIndex = static_cast<int32_t>(attachmentDescs.size()) - 1;
		//
		// desc.format = (vk::Format)VulkanHelper::TextureFormatToVkFormat(key.DepthFormat);
		// desc.samples = vk::SampleCountFlagBits::e1;
		// desc.loadOp = vk::AttachmentLoadOp::eDontCare;
		// desc.storeOp = vk::AttachmentStoreOp::eStore;
		// desc.initialLayout = vk::ImageLayout::eUndefined;
		// desc.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
	}

	for (int i = 0; i < colorCount; i++)
	{
		vk::AttachmentReference& colorReference = attachmentRefs.at(i);
		colorReference.attachment = i;
		colorReference.layout = vk::ImageLayout::eColorAttachmentOptimal;
	}

	if (hasDepth)
	{
		vk::AttachmentReference& depthReference = attachmentRefs.at(colorCount);
		depthReference.attachment = colorCount;
		depthReference.layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
	}

	if (key.HasResolvedRenderTarget)
	{
		attachmentRefs.resize(attachmentRefs.size() + 1);
		auto& ref = attachmentRefs.at(attachmentRefs.size() - 1);
		ref.attachment = resolveIndex;
		ref.layout = vk::ImageLayout::eColorAttachmentOptimal;
	}

	if (key.HasResolvedDepthTarget)
	{
		// Wait 1.2
		// attachmentRefs.resize(attachmentRefs.size() + 1);
		// auto& ref = attachmentRefs.at(attachmentRefs.size() - 1);
		// ref.attachment = resolveDepthIndex;
		// ref.layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
	}

	finalLayouts.resize(attachmentDescs.size());

	for (size_t i = 0; i < attachmentDescs.size(); i++)
	{
		finalLayouts.at(i) = attachmentDescs.at(i).finalLayout;
	}

	std::array<vk::SubpassDescription, 1> subpasses;
	{
		vk::SubpassDescription& subpass = subpasses[0];
		subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
		subpass.colorAttachmentCount = colorCount;
		subpass.pColorAttachments = &attachmentRefs.at(0);

		if (hasDepth)
		{
			subpass.pDepthStencilAttachment = &attachmentRefs.at(colorCount);
		}
		else
		{
			subpass.pDepthStencilAttachment = nullptr;
		}

		if (key.HasResolvedRenderTarget)
		{
			subpass.pResolveAttachments = &attachmentRefs.at(resolveIndex);
		}

		if (key.HasResolvedDepthTarget)
		{
			// Wait 1.2
		}
	}

	std::array<vk::SubpassDependency, RenderTargetMax * 2 + 2> dependencies;
	int32_t dependencyCount = 0;

	if (!key.IsPresent)
	{
		for (int i = 0; i < colorCount; i++)
		{
			auto& before = dependencies[dependencyCount++];
			before.srcSubpass = VK_SUBPASS_EXTERNAL;
			before.dstSubpass = 0;
			before.srcStageMask = (vk::PipelineStageFlags)VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			before.dstStageMask = (vk::PipelineStageFlags)VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			before.srcAccessMask = (vk::AccessFlags)VK_ACCESS_SHADER_READ_BIT;
			before.dstAccessMask = (vk::AccessFlags)VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			before.dependencyFlags = (vk::DependencyFlags)VK_DEPENDENCY_BY_REGION_BIT;

			auto& after = dependencies[dependencyCount++];
			after.srcSubpass = 0;
			after.dstSubpass = VK_SUBPASS_EXTERNAL;
			after.srcStageMask = (vk::PipelineStageFlags)VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			after.dstStageMask = (vk::PipelineStageFlags)VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			after.srcAccessMask = (vk::AccessFlags)VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			after.dstAccessMask = (vk::AccessFlags)VK_ACCESS_SHADER_READ_BIT;
			after.dependencyFlags = (vk::DependencyFlags)VK_DEPENDENCY_BY_REGION_BIT;
		}

		if (hasDepth)
		{
			auto& before = dependencies[dependencyCount++];
			before.srcSubpass = VK_SUBPASS_EXTERNAL;
			before.dstSubpass = 0;
			before.srcStageMask = (vk::PipelineStageFlags)VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			before.dstStageMask =
				(vk::PipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
			before.srcAccessMask = (vk::AccessFlags)VK_ACCESS_SHADER_READ_BIT;
			before.dstAccessMask =
				(vk::AccessFlags)(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
			before.dependencyFlags = (vk::DependencyFlags)VK_DEPENDENCY_BY_REGION_BIT;

			auto& after = dependencies[dependencyCount++];
			after.srcSubpass = 0;
			after.dstSubpass = VK_SUBPASS_EXTERNAL;
			after.srcStageMask =
				(vk::PipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
			after.dstStageMask = (vk::PipelineStageFlags)VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			after.srcAccessMask =
				(vk::AccessFlags)(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
			after.dstAccessMask = (vk::AccessFlags)VK_ACCESS_SHADER_READ_BIT;
			after.dependencyFlags = (vk::DependencyFlags)VK_DEPENDENCY_BY_REGION_BIT;
		}
	}

	{
		vk::RenderPassCreateInfo renderPassInfo;
		renderPassInfo.attachmentCount = (uint32_t)attachmentDescs.size();
		renderPassInfo.pAttachments = attachmentDescs.data();

		renderPassInfo.subpassCount = (uint32_t)subpasses.size();
		renderPassInfo.pSubpasses = subpasses.data();

		if (!key.IsPresent)
		{
			renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencyCount);
			renderPassInfo.pDependencies = dependencies.data();
		}
		else
		{
			renderPassInfo.dependencyCount = 0;
			renderPassInfo.pDependencies = nullptr;
		}

		auto renderPass = device_.createRenderPass(renderPassInfo);
		if (!renderPass)
		{
			return nullptr;
		}

		std::shared_ptr<RenderPassPipelineStateVulkan> ret = CreateSharedPtr(new RenderPassPipelineStateVulkan(device_, owner_));
		ret->renderPass_ = renderPass;
		ret->finalLayouts_ = finalLayouts;
		renderPassPipelineStates_[key] = ret;

		auto retptr = ret.get();
		SafeAddRef(retptr);

		retptr->RenderTargetCount = colorCount;

		retptr->Key = key;

		return retptr;
	}
}

} // namespace LLGI
