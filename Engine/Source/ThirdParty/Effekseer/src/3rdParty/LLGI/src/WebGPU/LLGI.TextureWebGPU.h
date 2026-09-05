#pragma once

#include "../LLGI.Graphics.h"
#include "../LLGI.Texture.h"
#include "LLGI.BaseWebGPU.h"
#include <unordered_map>
#include <vector>

namespace LLGI
{

class TextureWebGPU : public Texture
{
	wgpu::Device device_;
	wgpu::Instance instance_;

	wgpu::Texture texture_;
	wgpu::TextureView textureView_;
	wgpu::BindGroupLayout mipmapBindGroupLayout_;
	wgpu::PipelineLayout mipmapPipelineLayout_;
	wgpu::Sampler mipmapSampler_;
	wgpu::ShaderModule mipmapShaderModule_;
	std::unordered_map<wgpu::TextureFormat, wgpu::RenderPipeline> mipmapPipelines_;
	std::vector<wgpu::Texture> mipmapRenderTextures_;
	bool mipmapsGeneratedFromLockedData_ = false;
	TextureParameter parameter_;
	std::vector<uint8_t> temp_buffer_;

	bool CreateMipmapResources();
	wgpu::RenderPipeline GetMipmapPipeline(wgpu::TextureFormat format);

public:
	bool Initialize(wgpu::Device& device, const TextureParameter& parameter, wgpu::Instance instance = nullptr);
	bool InitializeFromSurfaceTexture(wgpu::Device& device, wgpu::Texture texture, const TextureParameter& parameter);
	void* Lock() override;
	void Unlock() override;
	void GenerateMipMaps(wgpu::CommandEncoder& commandEncoder);
	bool GetData(std::vector<uint8_t>& data) override;
	Vec2I GetSizeAs2D() const override;

	const TextureParameter& GetParameter() const { return parameter_; }

	wgpu::Texture GetTexture() const { return texture_; }

	wgpu::TextureView GetTextureView() const { return textureView_; }
};

} // namespace LLGI
