#pragma once

#include "../LLGI.CommandList.h"
#include "LLGI.BaseWebGPU.h"
#include <array>
#include <vector>

namespace LLGI
{

class CommandListWebGPU : public CommandList
{
	struct BindGroupEntryKey
	{
		uint32_t binding = 0;
		const void* resource = nullptr;
		uint64_t offset = 0;
		uint64_t size = 0;
		int32_t wrapMode = 0;
		int32_t minMagFilter = 0;
	};

	struct BindGroupCache
	{
		const void* pipeline = nullptr;
		std::vector<BindGroupEntryKey> entries;
		wgpu::BindGroup bindGroup = nullptr;
	};

	wgpu::Device device_;
	wgpu::CommandBuffer commandBuffer_;
	wgpu::CommandEncoder commandEncorder_;
	wgpu::RenderPassEncoder renderPassEncorder_;
	wgpu::ComputePassEncoder computePassEncorder_;
	wgpu::Sampler samplers_[3][2];
	wgpu::Texture fallbackTexture_;
	wgpu::TextureView fallbackTextureView_;
	std::array<BindGroupCache, 3> renderBindGroupCaches_;
	std::array<BindGroupCache, 3> computeBindGroupCaches_;

	static bool Equals(const std::vector<BindGroupEntryKey>& lhs, const std::vector<BindGroupEntryKey>& rhs);
	void ResetRenderBindGroupCaches();
	void ResetComputeBindGroupCaches();
	void SetRenderBindGroup(
		uint32_t index, const void* pipeline, const std::vector<BindGroupEntryKey>& entries, const wgpu::BindGroupDescriptor& desc);
	void SetComputeBindGroup(
		uint32_t index, const void* pipeline, const std::vector<BindGroupEntryKey>& entries, const wgpu::BindGroupDescriptor& desc);
	bool PrepareStandaloneCommand(const char* commandName);

public:
	CommandListWebGPU(wgpu::Device device);

	void Begin() override;

	bool BeginWithPlatform(void* platformContextPtr) override;

	void End() override;

	void EndWithPlatform() override;

	void BeginRenderPass(RenderPass* renderPass) override;

	void EndRenderPass() override;

	bool BeginRenderPassWithPlatformPtr(void* platformPtr) override;

	bool EndRenderPassWithPlatformPtr() override;

	void Draw(int32_t primitiveCount, int32_t instanceCount) override;

	void BeginComputePass() override;

	void EndComputePass() override;

	void Dispatch(int32_t groupX, int32_t groupY, int32_t groupZ, int32_t threadX, int32_t threadY, int32_t threadZ) override;

	void SetScissor(int32_t x, int32_t y, int32_t width, int32_t height) override;

	void CopyTexture(Texture* src, Texture* dst) override;

	void CopyTexture(
		Texture* src, Texture* dst, const Vec3I& srcPos, const Vec3I& dstPos, const Vec3I& size, int srcLayer, int dstLayer) override;

	void GenerateMipMap(Texture* src) override;

	void CopyBuffer(Buffer* src, Buffer* dst) override;

	void WaitUntilCompleted() override;

	const wgpu::CommandBuffer& GetCommandBuffer() const { return commandBuffer_; }
};

} // namespace LLGI
