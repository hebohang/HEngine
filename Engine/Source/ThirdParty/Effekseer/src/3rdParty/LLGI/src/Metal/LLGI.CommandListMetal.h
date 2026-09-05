#pragma once

#include "../LLGI.CommandList.h"
#include "../LLGI.Buffer.h"
#import <MetalKit/MetalKit.h>

#include <memory>
#include <vector>

namespace LLGI
{

class GraphicsMetal;
class IndexBuffer;
class QueryMetal;

struct CommandListMetalPlatformRenderPassContext
{
	id<MTLRenderCommandEncoder> RenderEncoder = nullptr;
};

struct CommandListMetalPlatformComputePassContext
{
	id<MTLComputeCommandEncoder> ComputeEncoder = nullptr;
};

class CommandListMetal : public CommandList
{
	GraphicsMetal* graphics_ = nullptr;

	MTLSamplerDescriptor* samplers_[3][2][3];
	id<MTLSamplerState> samplerStates_[3][2][3];

	id<MTLCommandBuffer> commandBuffer_ = nullptr;
	id<MTLRenderCommandEncoder> renderEncoder_ = nullptr;
	id<MTLComputeCommandEncoder> computeEncoder_ = nullptr;
	id<MTLFence> fence_ = nullptr;
	id<MTLTexture> fallbackSampledTexture_ = nullptr;
	id<MTLBuffer> visibilityResultBuffer_ = nullptr;
	uint32_t visibilityResultBufferCount_ = 0;
	uint32_t visibilityResultOffset_ = 0;
	bool isCompleted_ = true;

	struct PendingOcclusionQuery
	{
		QueryMetal* query = nullptr;
		uint32_t queryIndex = 0;
		uint32_t visibilityIndex = 0;
	};
	std::vector<PendingOcclusionQuery> pendingOcclusionQueries_;

	bool EnsureVisibilityResultBuffer(uint32_t queryCount);

public:
	CommandListMetal(Graphics* graphics);
	~CommandListMetal() override;

	void Begin() override;
	void End() override;
	void SetScissor(int32_t x, int32_t y, int32_t width, int32_t height) override;
	void Draw(int32_t primitiveCount, int32_t instanceCount) override;
	void CopyTexture(Texture* src, Texture* dst) override;
	void CopyTexture(
		Texture* src, Texture* dst, const Vec3I& srcPos, const Vec3I& dstPos, const Vec3I& size, int srcLayer, int dstLayer) override;

	void GenerateMipMap(Texture* src) override;
	void BeginRenderPass(RenderPass* renderPass) override;
	void EndRenderPass() override;

	void WaitUntilCompleted() override;

	bool BeginWithPlatform(void* platformContextPtr) override;
	void EndWithPlatform() override;

	bool BeginRenderPassWithPlatformPtr(void* platformPtr) override;
	bool EndRenderPassWithPlatformPtr() override;

	bool ResetQuery(Query* query) override;
	bool BeginQuery(Query* query, uint32_t queryIndex) override;
	bool EndQuery(Query* query, uint32_t queryIndex) override;
	bool RecordTimestamp(Query* query, uint32_t queryIndex) override;

	void BeginComputePass() override;
	void EndComputePass() override;
	
	bool BeginComputePassWithPlatformPtr(void* platformPtr) override;
	bool EndComputePassWithPlatformPtr() override;

	void Dispatch(int32_t groupX, int32_t groupY, int32_t groupZ, int32_t threadX, int32_t threadY, int32_t threadZ) override;

	bool GetIsCompleted() { return isCompleted_; }

    void CopyBuffer(Buffer* src, Buffer* dst) override;
    
	void ResetCompleted() { isCompleted_ = false; }

	id<MTLCommandBuffer>& GetCommandBuffer() { return commandBuffer_; }
	id<MTLRenderCommandEncoder>& GetRenderCommandEncorder() { return renderEncoder_; }
	id<MTLComputeCommandEncoder>& GetComputeCommandEncorder() { return computeEncoder_; }
};

} // namespace LLGI
