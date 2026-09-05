#pragma once

#include "../LLGI.PipelineState.h"
#include "LLGI.BaseWebGPU.h"
#include "LLGI.ShaderWebGPU.h"
#include <array>
#include <vector>

namespace LLGI
{

class PipelineStateWebGPU : public PipelineState
{
	std::array<Shader*, static_cast<int>(ShaderStageType::Max)> shaders_;

	wgpu::Device device_;

	wgpu::RenderPipeline renderPipeline_;
	wgpu::ComputePipeline computePipeline_;
	std::array<wgpu::BindGroupLayout, 3> bindGroupLayouts_;
	wgpu::PipelineLayout pipelineLayout_;
	std::vector<ShaderBindingWebGPU> bindings_;
	bool hasBindingReflection_ = false;

public:
	PipelineStateWebGPU(wgpu::Device device);
	~PipelineStateWebGPU() override;

	void SetShader(ShaderStageType stage, Shader* shader) override;

	bool Compile() override;

	wgpu::RenderPipeline GetRenderPipeline() { return renderPipeline_; }
	wgpu::ComputePipeline GetComputePipeline() { return computePipeline_; }
	bool HasBinding(uint32_t group, uint32_t binding) const;
	bool HasBinding(uint32_t group, uint32_t binding, ShaderResourceType resourceType) const;
	bool HasBinding(uint32_t group, uint32_t binding, ShaderResourceType resourceType, ShaderResourceAccess access) const;
};

} // namespace LLGI
