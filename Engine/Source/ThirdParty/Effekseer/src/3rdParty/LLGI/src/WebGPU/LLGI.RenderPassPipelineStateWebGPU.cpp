#include "LLGI.RenderPassPipelineStateWebGPU.h"

namespace LLGI
{

void RenderPassPipelineStateWebGPU::SetKey(const RenderPassPipelineStateKey& key)
{
	Key = key;
	pixelFormats_.resize(key.RenderTargetFormats.size());

	for (size_t i = 0; i < pixelFormats_.size(); i++)
	{
		pixelFormats_.at(i) = ConvertFormat(key.RenderTargetFormats.at(i));
	}

	depthStencilFormat_ = ConvertFormat(key.DepthFormat);
}

} // namespace LLGI