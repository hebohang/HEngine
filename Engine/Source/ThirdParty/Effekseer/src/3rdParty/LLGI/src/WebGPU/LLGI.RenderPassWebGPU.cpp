#include "LLGI.RenderPassWebGPU.h"
#include "LLGI.TextureWebGPU.h"

namespace LLGI
{

void RenderPassWebGPU::RefreshDescriptor()
{
	for (int i = 0; i < descriptor_.colorAttachmentCount; i++)
	{
		if (GetIsColorCleared())
		{
			colorAttachments_[i].loadOp = wgpu::LoadOp::Clear;
			colorAttachments_[i].storeOp = wgpu::StoreOp::Store;
			colorAttachments_[i].clearValue = {
				GetClearColor().R / 255.0, GetClearColor().G / 255.0, GetClearColor().B / 255.0, GetClearColor().A / 255.0};
		}
		else
		{
			colorAttachments_[i].loadOp = wgpu::LoadOp::Load;
			colorAttachments_[i].storeOp = wgpu::StoreOp::Store;
			colorAttachments_[i].clearValue = {0, 0, 0, 1};
		}
	}

	if (descriptor_.depthStencilAttachment != nullptr)
	{
		if (GetIsDepthCleared())
		{
			depthStencilAttachiment_.depthLoadOp = wgpu::LoadOp::Clear;
			depthStencilAttachiment_.depthStoreOp = wgpu::StoreOp::Store;
			depthStencilAttachiment_.depthClearValue = 1.0f;
		}
		else
		{
			depthStencilAttachiment_.depthLoadOp = wgpu::LoadOp::Load;
			depthStencilAttachiment_.depthStoreOp = wgpu::StoreOp::Store;
			depthStencilAttachiment_.depthClearValue = 1.0f;
		}
	}
}

bool RenderPassWebGPU::Initialize(
	Texture** textures, int textureCount, Texture* depthTexture, Texture* resolvedRenderTexture, Texture* resolvedDepthTexture)
{
	if (resolvedDepthTexture != nullptr)
	{
		Log(LogType::Error, "RenderPassWebGPU : Resolved depth is not supported.");
		return false;
	}

	if (!assignRenderTextures(textures, textureCount))
	{
		return false;
	}

	if (!assignDepthTexture(depthTexture))
	{
		return false;
	}

	if (!assignResolvedRenderTexture(resolvedRenderTexture))
	{
		return false;
	}

	if (!assignResolvedDepthTexture(resolvedDepthTexture))
	{
		return false;
	}

	if (!getSize(screenSize_, (const Texture**)textures, textureCount, depthTexture))
	{
		return false;
	}

	std::array<TextureWebGPU*, RenderTargetMax> texturesImpl;
	texturesImpl.fill(nullptr);
	TextureWebGPU* depthTextureImpl = nullptr;

	for (int32_t i = 0; i < textureCount; i++)
	{
		if (textures[i] == nullptr)
			continue;

		texturesImpl.at(i) = reinterpret_cast<TextureWebGPU*>(textures[i]);
	}

	if (depthTexture != nullptr)
	{
		depthTextureImpl = reinterpret_cast<TextureWebGPU*>(depthTexture);
	}

	TextureWebGPU* resolvedTextureImpl = nullptr;
	TextureWebGPU* resolvedDepthTextureImpl = nullptr;

	if (resolvedRenderTexture != nullptr)
	{
		resolvedTextureImpl = reinterpret_cast<TextureWebGPU*>(resolvedRenderTexture);
	}

	if (resolvedDepthTexture != nullptr)
	{
		resolvedDepthTextureImpl = reinterpret_cast<TextureWebGPU*>(resolvedDepthTexture);
	}

	descriptor_.colorAttachmentCount = textureCount;
	descriptor_.colorAttachments = colorAttachments_.data();

	for (int i = 0; i < textureCount; i++)
	{
		colorAttachments_[i].view = texturesImpl[i]->GetTextureView();

		if (resolvedTextureImpl != nullptr)
		{
			colorAttachments_[i].resolveTarget = resolvedTextureImpl->GetTextureView();
			colorAttachments_[i].storeOp = wgpu::StoreOp::Store;
		}
	}

	if (depthTexture != nullptr)
	{
		depthStencilAttachiment_.view = depthTextureImpl->GetTextureView();

		if (depthTextureImpl->GetFormat() == TextureFormatType::D24S8 || depthTextureImpl->GetFormat() == TextureFormatType::D32S8)
		{
			depthStencilAttachiment_.stencilLoadOp = GetIsDepthCleared() ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
			depthStencilAttachiment_.stencilStoreOp = wgpu::StoreOp::Store;
			depthStencilAttachiment_.stencilClearValue = 0;
		}

		if (resolvedDepthTextureImpl != nullptr)
		{
			// ?
		}

		descriptor_.depthStencilAttachment = &depthStencilAttachiment_;
	}

	RefreshDescriptor();

	return true;
}

} // namespace LLGI
