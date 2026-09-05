#pragma once

#include "../LLGI.Base.h"
#include "../LLGI.Graphics.h"
#include "LLGI.BaseWebGPU.h"

namespace LLGI
{
class RenderPassPipelineStateWebGPU;

class GraphicsWebGPU : public Graphics
{
private:
	//! cached
	std::unordered_map<RenderPassPipelineStateKey, std::shared_ptr<RenderPassPipelineStateWebGPU>, RenderPassPipelineStateKey::Hash>
		renderPassPipelineStates_;

	wgpu::Device device_;
	wgpu::Instance instance_;
	wgpu::Queue queue_;

public:

	GraphicsWebGPU(wgpu::Device device);
	GraphicsWebGPU(wgpu::Device device, wgpu::Instance instance);

	void SetWindowSize(const Vec2I& windowSize) override;

	void Execute(CommandList* commandList) override;

	void WaitFinish() override;

	Buffer* CreateBuffer(BufferUsageType usage, int32_t size) override;

	Shader* CreateShader(DataStructure* data, int32_t count) override;

	PipelineState* CreatePiplineState() override;

	SingleFrameMemoryPool* CreateSingleFrameMemoryPool(int32_t constantBufferPoolSize, int32_t drawingCount) override;

	CommandList* CreateCommandList(SingleFrameMemoryPool* memoryPool) override;

	RenderPass* CreateRenderPass(Texture** textures, int32_t textureCount, Texture* depthTexture) override;

	RenderPass* CreateRenderPass(Texture* texture, Texture* resolvedTexture, Texture* depthTexture, Texture* resolvedDepthTexture) override;

	Texture* CreateTexture(const TextureParameter& parameter) override;

	Texture* CreateTexture(const TextureInitializationParameter& parameter) override;

	Texture* CreateRenderTexture(const RenderTextureInitializationParameter& parameter) override;
	
	Texture* CreateDepthTexture(const DepthTextureInitializationParameter& parameter) override;

	Texture* CreateTexture(uint64_t id) override;

	std::vector<uint8_t> CaptureRenderTarget(Texture* renderTarget) override;

	bool GetIsMipmapGenerationSupportedOnTextureLoad() const override { return true; }

	RenderPassPipelineState* CreateRenderPassPipelineState(RenderPass* renderPass) override;

	RenderPassPipelineState* CreateRenderPassPipelineState(const RenderPassPipelineStateKey& key) override;

	wgpu::Device& GetDevice() { return device_; }
	wgpu::Queue& GetQueue() { return queue_; }

	bool IsResolvedDepthSupported() const override { return false; }
};

} // namespace LLGI
