#pragma once

#include "../LLGI.Graphics.h"
#include "LLGI.BaseWebGPU.h"

namespace LLGI
{
class RenderPassPipelineStateWebGPU : public RenderPassPipelineState
{
private:
	FixedSizeVector<wgpu::TextureFormat, RenderTargetMax> pixelFormats_;
	wgpu::TextureFormat depthStencilFormat_ = wgpu::TextureFormat::Undefined;

public:
	void SetKey(const RenderPassPipelineStateKey& key);

	const FixedSizeVector<wgpu::TextureFormat, RenderTargetMax>& GetPixelFormats() const { return pixelFormats_; }
	const wgpu::TextureFormat& GetDepthStencilFormat() const { return depthStencilFormat_; }
};

} // namespace LLGI