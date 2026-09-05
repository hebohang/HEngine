
#pragma once

#include "LLGI.Base.h"
#include "LLGI.Shader.h"

namespace LLGI
{
static constexpr int NumConstantBuffer = 4;
static constexpr int NumTexture = TextureSlotMax;
static constexpr int NumStorageBuffer = TextureSlotMax;

class VertexBuffer;
class IndexBuffer;

/**
	@brief	command list
	@note
	CommandList has a swap buffer. So you don't need to consider buffering.
	CommandList has references related to rendering.
	References are released after the number of swapCount has passed.

	Limitation :
	Begin and End are need to call only once in one frame.
	Command list must not be released after finishing rendering.
*/
class CommandList : public ReferenceObject
{
protected:
	struct BindingVertexBuffer
	{
		Buffer* vertexBuffer = nullptr;
		int32_t stride = 0;
		int32_t offset = 0;
	};

	struct BindingIndexBuffer
	{
		Buffer* indexBuffer = nullptr;
		int32_t stride = 0;
		int32_t offset = 0;
	};

	struct BindingTexture
	{
		Texture* texture = nullptr;
		TextureWrapMode wrapMode = TextureWrapMode::Clamp;
		TextureMinMagFilter minMagFilter = TextureMinMagFilter::Nearest;
		bool isBound = false;
	};

	struct BindingStorageBuffer
	{
		Buffer* storageBuffer = nullptr;
		ShaderResourceBinding binding;
	};

private:
	struct SwapObject
	{
		std::vector<ReferenceObject*> referencedObjects;
	};

	int32_t swapIndex_ = -1;
	int32_t swapCount_ = 0;
	std::vector<SwapObject> swapObjects;

	BindingVertexBuffer bindingVertexBuffer;
	BindingIndexBuffer bindingIndexBuffer;

	PipelineState* currentPipelineState = nullptr;

	bool isVertexBufferDirtied = true;
	bool isCurrentIndexBufferDirtied = true;
	bool isPipelineDirtied = true;

protected:
	//! whether the command list wraps an external one whose render pass state is managed outside of LLGI
	bool doesBeginWithPlatform_ = false;
	bool isInRenderPass_ = false;
	bool isInBegin_ = false;

	std::array<Buffer*, NumConstantBuffer> constantBuffers_;
	std::array<BindingTexture, NumTexture> currentTextures_;
	std::array<BindingStorageBuffer, NumStorageBuffer> storageBuffers_;

protected:
	void GetCurrentVertexBuffer(BindingVertexBuffer& buffer, bool& isDirtied);
	void GetCurrentIndexBuffer(BindingIndexBuffer& buffer, bool& isDirtied);
	void GetCurrentPipelineState(PipelineState*& pipelineState, bool& isDirtied);
	void GetCurrentStorageBuffer(int32_t unit, BindingStorageBuffer& buffer);
	void RegisterReferencedObject(ReferenceObject* referencedObject);
	bool ValidateDrawState(const char* backendName,
						   int32_t primitiveCount,
						   int32_t instanceCount,
						   BindingVertexBuffer& vertexBuffer,
						   BindingIndexBuffer& indexBuffer,
						   PipelineState*& pipelineState,
						   bool& outIsVertexBufferDirtied,
						   bool& outIsIndexBufferDirtied,
						   bool& outIsPipelineDirtied);
	bool ValidateDispatchState(const char* backendName,
							   int32_t groupX,
							   int32_t groupY,
							   int32_t groupZ,
							   int32_t threadX,
							   int32_t threadY,
							   int32_t threadZ,
							   PipelineState*& pipelineState,
							   bool& outIsPipelineDirtied);

public:
	CommandList(int32_t swapCount = 3);
	~CommandList() override;

	/**
	@brief
	added a command into supecified context.
	This function can be called once by a frame.
	*/
	virtual void Begin();

	/**
		@brief
		added a command into supecified context. This function is supported in some platform.
		Internal context is not used if platformContextPtr is null in Metal.
		This function can be called once by a frame.
	*/
	virtual bool BeginWithPlatform(void* platformContextPtr);

	virtual void End();
	virtual void EndWithPlatform();

	virtual void SetScissor(int32_t x, int32_t y, int32_t width, int32_t height);
	virtual void Draw(int32_t primitiveCount, int32_t instanceCount = 1);
	virtual void SetVertexBuffer(Buffer* vertexBuffer, int32_t stride, int32_t offset);
	virtual void SetIndexBuffer(Buffer* indexBuffer, int32_t stride, int32_t offset = 0);
	virtual void SetPipelineState(PipelineState* pipelineState);
	virtual void SetConstantBuffer(Buffer* constantBuffer, int32_t unit);

	/**
		@brief specify a storage buffer.
		@param stride logical element stride for structured buffer access.
		@note On DirectX12, ByteAddressBuffer/RWByteAddressBuffer bindings still receive the logical stride here.
		      The DX12 backend converts those bindings to raw buffer views internally.
	*/
	virtual void SetStorageBuffer(Buffer* storageBuffer,
								  int32_t stride,
								  int32_t unit,
								  ShaderResourceAccess access,
								  StorageBufferViewType viewType = StorageBufferViewType::Structured);
	virtual void SetStorageBuffer(Buffer* storageBuffer, const ShaderResourceBinding& binding);

	/**
		@brief	copy a texture
	*/
	virtual void CopyTexture(Texture* src, Texture* dst) {}

	/**
		@brief	copy a texture
	*/
	virtual void
	CopyTexture(Texture* src, Texture* dst, const Vec3I& srcPos, const Vec3I& dstPos, const Vec3I& size, int srcLayer, int dstLayer)
	{
	}

	/**
		@brief specify textures
	*/
	virtual void SetTexture(Texture* texture, TextureWrapMode wrapMode, TextureMinMagFilter minmagFilter, int32_t unit);

	/**
		@brief generate mipmap
		@note
		use this method before use mipmap required texture.
	*/
	virtual void GenerateMipMap(Texture* src) {}

	/**
		@brief	reset textures and set null.
	*/
	virtual void ResetTextures();

	virtual void BeginRenderPass(RenderPass* renderPass);

	/**
		@brief
		added a command into supecified renderpass. This function is supported in some platform.
	*/
	virtual bool BeginRenderPassWithPlatformPtr(void* platformPtr);

	virtual void EndRenderPass() { isInRenderPass_ = false; }

	/**
		@brief
		The pair of BeginRenderPassWithPlatformPtr
	*/
	virtual bool EndRenderPassWithPlatformPtr() { return false; }

	virtual bool ResetQuery(Query* query) { return false; }
	virtual bool BeginQuery(Query* query, uint32_t queryIndex) { return false; }
	virtual bool EndQuery(Query* query, uint32_t queryIndex) { return false; }
	virtual bool RecordTimestamp(Query* query, uint32_t queryIndex) { return false; }

	virtual void ResetStorageBuffers();
	virtual void BeginComputePass() {}
	virtual void EndComputePass() {}
	virtual bool BeginComputePassWithPlatformPtr(void* platformPtr) { return false; }
	virtual bool EndComputePassWithPlatformPtr() { return false; }

	virtual void Dispatch(int32_t groupX, int32_t groupY, int32_t groupZ, int32_t threadX, int32_t threadY, int32_t threadZ);

	virtual void CopyBuffer(Buffer* src, Buffer* dst) {}

	/**
		@brief	send a memory in specified texture from cpu to gpu
	*/
	virtual void SetImageData2D(Texture* texture, int32_t x, int32_t y, int32_t width, int32_t height, const void* data);

	/**
		@brief wait until this command is completed.
	*/
	virtual void WaitUntilCompleted();

	bool GetIsInRenderPass() const;
};

} // namespace LLGI
