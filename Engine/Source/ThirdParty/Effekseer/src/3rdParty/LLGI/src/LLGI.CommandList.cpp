
#include "LLGI.CommandList.h"
#include "LLGI.Buffer.h"
#include "LLGI.PipelineState.h"
#include "LLGI.Texture.h"

namespace LLGI
{

void CommandList::GetCurrentVertexBuffer(BindingVertexBuffer& buffer, bool& isDirtied)
{
	buffer = bindingVertexBuffer;
	isDirtied = isVertexBufferDirtied;
}

void CommandList::GetCurrentIndexBuffer(BindingIndexBuffer& buffer, bool& isDirtied)
{
	buffer = bindingIndexBuffer;
	isDirtied = isCurrentIndexBufferDirtied;
}

void CommandList::GetCurrentPipelineState(PipelineState*& pipelineState, bool& isDirtied)
{
	pipelineState = currentPipelineState;
	isDirtied = isPipelineDirtied;
}

void CommandList::GetCurrentStorageBuffer(int32_t unit, BindingStorageBuffer& buffer) { buffer = storageBuffers_[unit]; }

void CommandList::RegisterReferencedObject(ReferenceObject* referencedObject)
{
	if (referencedObject == nullptr)
		return;

	assert(swapIndex_ >= 0);
	SafeAddRef(referencedObject);
	swapObjects[swapIndex_].referencedObjects.push_back(referencedObject);
}

bool CommandList::ValidateDrawState(const char* backendName,
									int32_t primitiveCount,
									int32_t instanceCount,
									BindingVertexBuffer& vertexBuffer,
									BindingIndexBuffer& indexBuffer,
									PipelineState*& pipelineState,
									bool& outIsVertexBufferDirtied,
									bool& outIsIndexBufferDirtied,
									bool& outIsPipelineDirtied)
{
	const auto prefix = std::string(backendName != nullptr ? backendName : "CommandList") + "::Draw skipped: ";
	// When the command list wraps an external one, the render pass state is managed
	// outside of LLGI, so it cannot be validated.
	if (!isInRenderPass_ && !doesBeginWithPlatform_)
	{
		Log(LogType::Warning, prefix + "Draw must be called inside RenderPass.");
		return false;
	}
	if (primitiveCount <= 0 || instanceCount <= 0)
	{
		Log(LogType::Warning,
			prefix + "invalid draw count. primitives=" + std::to_string(primitiveCount) +
				", instances=" + std::to_string(instanceCount));
		return false;
	}

	GetCurrentVertexBuffer(vertexBuffer, outIsVertexBufferDirtied);
	GetCurrentIndexBuffer(indexBuffer, outIsIndexBufferDirtied);
	GetCurrentPipelineState(pipelineState, outIsPipelineDirtied);

	if (vertexBuffer.vertexBuffer == nullptr)
	{
		Log(LogType::Error, prefix + "vertex buffer is not specified.");
		return false;
	}
	if (indexBuffer.indexBuffer == nullptr)
	{
		Log(LogType::Error, prefix + "index buffer is not specified.");
		return false;
	}
	if (pipelineState == nullptr)
	{
		Log(LogType::Error, prefix + "pipeline state is not specified.");
		return false;
	}
	if (GetIndexCountPerPrimitive(pipelineState->Topology) == 0)
	{
		Log(LogType::Error, prefix + "unsupported topology. topology=" + to_string(pipelineState->Topology));
		return false;
	}
	if (vertexBuffer.stride <= 0)
	{
		Log(LogType::Error, prefix + "invalid vertex stride. stride=" + std::to_string(vertexBuffer.stride));
		return false;
	}
	if (vertexBuffer.offset < 0 || indexBuffer.offset < 0)
	{
		Log(LogType::Error,
			prefix + "invalid buffer offset. vertexOffset=" + std::to_string(vertexBuffer.offset) +
				", indexOffset=" + std::to_string(indexBuffer.offset));
		return false;
	}
	if (indexBuffer.stride != 2 && indexBuffer.stride != 4)
	{
		Log(LogType::Error, prefix + "unsupported index stride. stride=" + std::to_string(indexBuffer.stride));
		return false;
	}

	return true;
}

bool CommandList::ValidateDispatchState(const char* backendName,
										int32_t groupX,
										int32_t groupY,
										int32_t groupZ,
										int32_t threadX,
										int32_t threadY,
										int32_t threadZ,
										PipelineState*& pipelineState,
										bool& outIsPipelineDirtied)
{
	const auto prefix = std::string(backendName != nullptr ? backendName : "CommandList") + "::Dispatch skipped: ";
	// When the command list wraps an external one, the render pass state is managed
	// outside of LLGI, so it cannot be validated.
	if (isInRenderPass_ && !doesBeginWithPlatform_)
	{
		Log(LogType::Error, prefix + "Dispatch must be called outside RenderPass.");
		return false;
	}
	if (groupX <= 0 || groupY <= 0 || groupZ <= 0)
	{
		Log(LogType::Error,
			prefix + "invalid group count. groups=(" + std::to_string(groupX) + ", " + std::to_string(groupY) + ", " +
				std::to_string(groupZ) + "), threads=(" + std::to_string(threadX) + ", " + std::to_string(threadY) + ", " +
				std::to_string(threadZ) + ")");
		return false;
	}
	if (threadX <= 0 || threadY <= 0 || threadZ <= 0)
	{
		Log(LogType::Error,
			prefix + "invalid thread count. groups=(" + std::to_string(groupX) + ", " + std::to_string(groupY) + ", " +
				std::to_string(groupZ) + "), threads=(" + std::to_string(threadX) + ", " + std::to_string(threadY) + ", " +
				std::to_string(threadZ) + ")");
		return false;
	}

	GetCurrentPipelineState(pipelineState, outIsPipelineDirtied);
	if (pipelineState == nullptr)
	{
		Log(LogType::Error, prefix + "pipeline state is not specified.");
		return false;
	}

	return true;
}

CommandList::CommandList(int32_t swapCount) : swapCount_(swapCount)
{
	constantBuffers_.fill(nullptr);

	for (auto& cbs : storageBuffers_)
	{
		cbs.storageBuffer = nullptr;
	}

	for (auto& t : currentTextures_)
	{
		t.texture = nullptr;
		t.isBound = false;
	}

	swapObjects.resize(swapCount_);
}

CommandList::~CommandList()
{
	for (auto& c : constantBuffers_)
	{
		SafeRelease(c);
	}

	for (auto& t : currentTextures_)
	{
		SafeRelease(t.texture);
	}

	for (auto& so : swapObjects)
	{
		for (auto& o : so.referencedObjects)
		{
			o->Release();
		}
		so.referencedObjects.clear();
	}

	for (auto& cb : storageBuffers_)
	{
		SafeRelease(cb.storageBuffer);
	}
}

void CommandList::Begin()
{
	bindingVertexBuffer.vertexBuffer = nullptr;
	bindingIndexBuffer.indexBuffer = nullptr;
	currentPipelineState = nullptr;
	isVertexBufferDirtied = true;
	isCurrentIndexBufferDirtied = true;
	isPipelineDirtied = true;
	ResetTextures();
	ResetStorageBuffers();

	swapIndex_ = (swapIndex_ + 1) % swapCount_;

	for (auto& o : swapObjects[swapIndex_].referencedObjects)
	{
		o->Release();
	}
	swapObjects[swapIndex_].referencedObjects.clear();

	isInBegin_ = true;
}

bool CommandList::BeginWithPlatform(void* platformContextPtr)
{
	bindingVertexBuffer.vertexBuffer = nullptr;
	bindingIndexBuffer.indexBuffer = nullptr;
	currentPipelineState = nullptr;
	isVertexBufferDirtied = true;
	isCurrentIndexBufferDirtied = true;
	isPipelineDirtied = true;
	ResetTextures();
	ResetStorageBuffers();

	swapIndex_ = (swapIndex_ + 1) % swapCount_;

	for (auto& o : swapObjects[swapIndex_].referencedObjects)
	{
		o->Release();
	}
	swapObjects[swapIndex_].referencedObjects.clear();
	doesBeginWithPlatform_ = true;

	isInBegin_ = true;
	return true;
}

void CommandList::End()
{
	isInBegin_ = false;

	if (GetIsInRenderPass())
	{
		Log(LogType::Error, "Please call End outside of RenderPass");
	}

	if (doesBeginWithPlatform_)
	{
		Log(LogType::Error, "CommandList begins with platform.");
	}
}

void CommandList::EndWithPlatform()
{
	isInBegin_ = false;

	if (!doesBeginWithPlatform_)
	{
		Log(LogType::Error, "CommandList doesn't begin with platform.");
	}
	doesBeginWithPlatform_ = false;
}

void CommandList::SetScissor(int32_t x, int32_t y, int32_t width, int32_t height) {}

void CommandList::Draw(int32_t primitiveCount, int32_t instanceCount)
{
	isVertexBufferDirtied = false;
	isCurrentIndexBufferDirtied = false;
	isPipelineDirtied = false;
}

void CommandList::SetVertexBuffer(Buffer* vertexBuffer, int32_t stride, int32_t offset)
{
	isVertexBufferDirtied |=
		bindingVertexBuffer.vertexBuffer != vertexBuffer || bindingVertexBuffer.stride != stride || bindingVertexBuffer.offset != offset;
	bindingVertexBuffer.vertexBuffer = vertexBuffer;
	bindingVertexBuffer.stride = stride;
	bindingVertexBuffer.offset = offset;

	RegisterReferencedObject(vertexBuffer);
}

void CommandList::SetIndexBuffer(Buffer* indexBuffer, int32_t stride, int32_t offset)
{
	isCurrentIndexBufferDirtied |= bindingIndexBuffer.indexBuffer != indexBuffer || bindingIndexBuffer.offset != offset;
	bindingIndexBuffer.indexBuffer = indexBuffer;
	bindingIndexBuffer.stride = stride;
	bindingIndexBuffer.offset = offset;

	RegisterReferencedObject(indexBuffer);
}

void CommandList::SetPipelineState(PipelineState* pipelineState)
{
	currentPipelineState = pipelineState;
	isPipelineDirtied = true;

	RegisterReferencedObject(pipelineState);
}

void CommandList::SetConstantBuffer(Buffer* constantBuffer, int32_t unit)
{
	if (unit < 0 || unit >= NumConstantBuffer)
	{
		Log(LogType::Error,
			std::string("Constant buffer slot is out of range. slot=") + std::to_string(unit) +
				", max=" + std::to_string(NumConstantBuffer - 1));
		return;
	}

	SafeAssign(constantBuffers_[unit], constantBuffer);

	RegisterReferencedObject(constantBuffer);
}

void CommandList::SetStorageBuffer(
	Buffer* storageBuffer, int32_t stride, int32_t unit, ShaderResourceAccess access, StorageBufferViewType viewType)
{
	ShaderResourceBinding binding;
	binding.ResourceType = ShaderResourceType::StorageBuffer;
	binding.Access = access;
	binding.StorageBufferView = viewType;
	binding.Slot = unit;
	binding.ElementStride = stride;
	SetStorageBuffer(storageBuffer, binding);
}

void CommandList::SetStorageBuffer(Buffer* storageBuffer, const ShaderResourceBinding& binding)
{
	if (binding.Slot < 0 || binding.Slot >= NumStorageBuffer)
	{
		Log(LogType::Error,
			std::string("Storage buffer slot is out of range. slot=") + std::to_string(binding.Slot) +
				", max=" + std::to_string(NumStorageBuffer - 1) + ", group=" + std::to_string(binding.Group) +
				", stride=" + std::to_string(binding.ElementStride));
		return;
	}

	auto normalizedBinding = binding;
	normalizedBinding.ResourceType = ShaderResourceType::StorageBuffer;

	auto& storageBinding = storageBuffers_[binding.Slot];
	SafeAssign(storageBinding.storageBuffer, storageBuffer);
	storageBinding.binding = normalizedBinding;
	RegisterReferencedObject(storageBuffer);
}

void CommandList::SetTexture(Texture* texture, TextureWrapMode wrapMode, TextureMinMagFilter minmagFilter, int32_t unit)
{
	if (unit < 0 || unit >= NumTexture)
	{
		Log(LogType::Error,
			std::string("Texture slot is out of range. slot=") + std::to_string(unit) +
				", max=" + std::to_string(NumTexture - 1));
		return;
	}

	SafeAssign(currentTextures_[unit].texture, texture);
	currentTextures_[unit].wrapMode = wrapMode;
	currentTextures_[unit].minMagFilter = minmagFilter;
	currentTextures_[unit].isBound = true;

	RegisterReferencedObject(texture);
}

void CommandList::ResetTextures()
{
	for (auto& texture : currentTextures_)
	{
		SafeRelease(texture.texture);
		texture.wrapMode = TextureWrapMode::Clamp;
		texture.minMagFilter = TextureMinMagFilter::Nearest;
		texture.isBound = false;
	}
}

void CommandList::BeginRenderPass(RenderPass* renderPass)
{
	isVertexBufferDirtied = true;
	isCurrentIndexBufferDirtied = true;
	isPipelineDirtied = true;
	isInRenderPass_ = true;
}

bool CommandList::BeginRenderPassWithPlatformPtr(void* platformPtr)
{
	isVertexBufferDirtied = true;
	isCurrentIndexBufferDirtied = true;
	isPipelineDirtied = true;
	isInRenderPass_ = true;
	return true;
}

void CommandList::Dispatch(int32_t groupX, int32_t groupY, int32_t groupZ, int32_t threadX, int32_t threadY, int32_t threadZ)
{
	isPipelineDirtied = false;
}

void CommandList::ResetStorageBuffers()
{
	for (auto& cb : storageBuffers_)
	{
		SafeRelease(cb.storageBuffer);
		cb.binding = ShaderResourceBinding();
	}
}

void CommandList::SetImageData2D(Texture* texture, int32_t x, int32_t y, int32_t width, int32_t height, const void* data)
{
	assert(0); // TODO: Not implemented.
}

void CommandList::WaitUntilCompleted()
{
	assert(0); // TODO: Not implemented.
}

bool CommandList::GetIsInRenderPass() const { return isInRenderPass_; }

} // namespace LLGI
