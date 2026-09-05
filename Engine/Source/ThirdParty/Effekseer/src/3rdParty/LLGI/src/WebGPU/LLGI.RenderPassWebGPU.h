#pragma once

#include "../LLGI.Graphics.h"
#include "LLGI.BaseWebGPU.h"

namespace LLGI
{
class TextureWebGPU;

class RenderPassWebGPU : public RenderPass
{
	wgpu::RenderPassDescriptor descriptor_;
	std::array<wgpu::RenderPassColorAttachment, RenderTargetMax> colorAttachments_;
	wgpu::RenderPassDepthStencilAttachment depthStencilAttachiment_;

public:
	bool
	Initialize(Texture** textures, int textureCount, Texture* depthTexture, Texture* resolvedRenderTexture, Texture* resolvedDepthTexture);

	void RefreshDescriptor();

	const wgpu::RenderPassDescriptor& GetDescriptor() const { return descriptor_; }
};

} // namespace LLGI
