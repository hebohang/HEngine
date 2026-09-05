#include "LLGI.CommandListMetal.h"
#include "LLGI.BufferMetal.h"
#include "LLGI.GraphicsMetal.h"
#include "LLGI.Metal_Impl.h"
#include "LLGI.PipelineStateMetal.h"
#include "LLGI.RenderPassMetal.h"
#include "LLGI.TextureMetal.h"
#include "LLGI.QueryMetal.h"

#import <MetalKit/MetalKit.h>
#include <TargetConditionals.h>
#include <algorithm>

namespace LLGI
{
namespace
{
static constexpr uint32_t InitialVisibilityResultBufferCount = 1024;

id<MTLTexture> CreateFallbackSampledTexture(id<MTLDevice> device)
{
	MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
																								  width:1
																								 height:1
																							  mipmapped:NO];
	textureDescriptor.usage = MTLTextureUsageShaderRead;

	auto texture = [device newTextureWithDescriptor:textureDescriptor];
	if (texture == nullptr)
	{
		return nullptr;
	}

	uint8_t textureData[4] = {255, 255, 255, 255};
	MTLRegion region = MTLRegionMake2D(0, 0, 1, 1);
	[texture replaceRegion:region mipmapLevel:0 withBytes:textureData bytesPerRow:4];
	return texture;
}
}

bool CommandListMetal::EnsureVisibilityResultBuffer(uint32_t queryCount)
{
	if (visibilityResultBuffer_ != nullptr && visibilityResultBufferCount_ >= queryCount)
	{
		return true;
	}

	if (visibilityResultBuffer_ != nullptr)
	{
		[visibilityResultBuffer_ release];
		visibilityResultBuffer_ = nullptr;
		visibilityResultBufferCount_ = 0;
	}

	visibilityResultBuffer_ = [graphics_->GetDevice() newBufferWithLength:sizeof(uint64_t) * queryCount
																   options:MTLResourceStorageModeShared];
	if (visibilityResultBuffer_ == nil)
	{
		return false;
	}

	visibilityResultBufferCount_ = queryCount;
	return true;
}

CommandListMetal::CommandListMetal(Graphics* graphics)
{
	auto g = static_cast<GraphicsMetal*>(graphics);
	SafeAddRef(g);
	graphics_ = g;

	// Sampler
	for (int w = 0; w < 3; w++)
	{
		for (int f = 0; f < 2; f++)
		{
			for (int m = 0; m < 3; m++)
			{
				MTLSamplerAddressMode ws[3];
				ws[0] = MTLSamplerAddressModeClampToEdge;
				ws[1] = MTLSamplerAddressModeRepeat;
				ws[2] = MTLSamplerAddressModeMirrorRepeat;

				MTLSamplerMinMagFilter fsmin[2];
				fsmin[0] = MTLSamplerMinMagFilterNearest;
				fsmin[1] = MTLSamplerMinMagFilterLinear;

				MTLSamplerMipFilter msmip[3];
				msmip[0] = MTLSamplerMipFilterNotMipmapped;
				msmip[1] = MTLSamplerMipFilterLinear;
				msmip[2] = MTLSamplerMipFilterNearest;

				MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
				samplerDescriptor.minFilter = fsmin[f];
				samplerDescriptor.magFilter = fsmin[f];
				samplerDescriptor.mipFilter = msmip[m];
				samplerDescriptor.sAddressMode = ws[w];
				samplerDescriptor.tAddressMode = ws[w];

				samplers_[w][f][m] = samplerDescriptor;
				samplerStates_[w][f][m] = [g->GetDevice() newSamplerStateWithDescriptor:samplerDescriptor];
			}
		}
	}

	fence_ = [g->GetDevice() newFence];
	fallbackSampledTexture_ = CreateFallbackSampledTexture(g->GetDevice());
}

CommandListMetal::~CommandListMetal()
{
	if (isInRenderPass_)
	{
		EndRenderPass();
	}

	if (isInBegin_)
	{
		End();
	}

	WaitUntilCompleted();

	for (int w = 0; w < 3; w++)
	{
		for (int f = 0; f < 2; f++)
		{
			for (int m = 0; m < 3; m++)
			{
				[samplers_[w][f][m] release];
				[samplerStates_[w][f][m] release];
			}
		}
	}

	if (commandBuffer_ != nullptr)
	{
		[commandBuffer_ release];
		commandBuffer_ = nullptr;
	}

	if (renderEncoder_ != nullptr)
	{
		[renderEncoder_ release];
	}

	if (fence_ != nullptr)
	{
		[fence_ release];
	}

	if (visibilityResultBuffer_ != nullptr)
	{
		[visibilityResultBuffer_ release];
		visibilityResultBuffer_ = nullptr;
	}

	if (fallbackSampledTexture_ != nullptr)
	{
		[fallbackSampledTexture_ release];
		fallbackSampledTexture_ = nullptr;
	}

	SafeRelease(graphics_);
}

void CommandListMetal::Begin()
{
	@autoreleasepool
	{
		if (commandBuffer_ != nullptr)
		{
			[commandBuffer_ release];
			commandBuffer_ = nullptr;
		}

		commandBuffer_ = [graphics_->GetCommandQueue() commandBuffer];
		[commandBuffer_ retain];

		auto t = this;

		[commandBuffer_ addCompletedHandler:^(id buffer) {
		  t->isCompleted_ = true;
		}];

		CommandList::Begin();
	}
}

void CommandListMetal::End() { CommandList::End(); }

void CommandListMetal::SetScissor(int32_t x, int32_t y, int32_t width, int32_t height)
{
	MTLScissorRect rect;
	rect.x = x;
	rect.y = y;
	rect.width = width;
	rect.height = height;
	[renderEncoder_ setScissorRect:rect];
}

void CommandListMetal::Draw(int32_t primitiveCount, int32_t instanceCount)
{
	BindingVertexBuffer bvb;
	BindingIndexBuffer bib;
	PipelineState* bpip = nullptr;

	const int mipmapFilter = 1;

	bool isVBDirtied = false;
	bool isIBDirtied = false;
	bool isPipDirtied = false;

	if (!ValidateDrawState("CommandListMetal", primitiveCount, instanceCount, bvb, bib, bpip, isVBDirtied, isIBDirtied, isPipDirtied))
	{
		return;
	}

	if (renderEncoder_ == nullptr)
	{
		Log(LogType::Error, "CommandListMetal::Draw skipped: render encoder is null.");
		return;
	}

	auto vb = static_cast<BufferMetal*>(bvb.vertexBuffer);
	auto ib = static_cast<BufferMetal*>(bib.indexBuffer);
	auto pip = static_cast<PipelineStateMetal*>(bpip);
	if (pip->GetRenderPipelineState() == nullptr)
	{
		Log(LogType::Error, "CommandListMetal::Draw skipped: render pipeline state is null.");
		return;
	}

	// set cull mode
	if (pip->Culling == LLGI::CullingMode::Clockwise)
	{
		[renderEncoder_ setCullMode:MTLCullModeFront];
	}
	else if (pip->Culling == LLGI::CullingMode::CounterClockwise)
	{
		[renderEncoder_ setCullMode:MTLCullModeBack];
	}
	else
	{
		[renderEncoder_ setCullMode:MTLCullModeNone];
	}

	[renderEncoder_ setFrontFacingWinding:MTLWindingCounterClockwise];

	if (isVBDirtied)
	{
		[renderEncoder_ setVertexBuffer:vb->GetBuffer() offset:bvb.offset atIndex:VertexBufferIndex];
	}

	// assign constant buffers
    for(size_t i = 0; i < constantBuffers_.size(); i++)
    {
        auto cb = static_cast<BufferMetal*>(constantBuffers_[i]);
        if(cb != nullptr)
        {
            [renderEncoder_ setVertexBuffer:cb->GetBuffer() offset:cb->GetOffset() atIndex:i];
            [renderEncoder_ setFragmentBuffer:cb->GetBuffer() offset:cb->GetOffset() atIndex:i];
        }
    }
    
    // Assign textures
    for (int unit_ind = 0; unit_ind < currentTextures_.size(); unit_ind++)
    {
        auto texture = (TextureMetal*)currentTextures_[unit_ind].texture;
        auto nativeTexture = texture != nullptr ? texture->GetTexture() : fallbackSampledTexture_;
        if (nativeTexture == nullptr)
            continue;
        auto wm = (int32_t)currentTextures_[unit_ind].wrapMode;
        auto mm = (int32_t)currentTextures_[unit_ind].minMagFilter;
        auto pm = 0;
        if (nativeTexture.mipmapLevelCount >= 2)
        {
            pm = mipmapFilter;
        }
        
        [renderEncoder_ setVertexTexture:nativeTexture atIndex:unit_ind];
        [renderEncoder_ setVertexSamplerState:samplerStates_[wm][mm][pm] atIndex:unit_ind];
        [renderEncoder_ setFragmentTexture:nativeTexture atIndex:unit_ind];
        [renderEncoder_ setFragmentSamplerState:samplerStates_[wm][mm][pm] atIndex:unit_ind];
    }
	
    const int compute_offset = 10;
    
    for (int unit_ind = 0; unit_ind < NumStorageBuffer; unit_ind++)
    {
        BindingStorageBuffer bcb;
        GetCurrentStorageBuffer(unit_ind, bcb);
        if (bcb.storageBuffer == nullptr)
            continue;
			
        auto cb = static_cast<BufferMetal*>(bcb.storageBuffer);
        [renderEncoder_ setVertexBuffer:cb->GetBuffer() offset:cb->GetOffset() atIndex:unit_ind + compute_offset];
        [renderEncoder_ setFragmentBuffer:cb->GetBuffer() offset:cb->GetOffset() atIndex:unit_ind + compute_offset];
    }

	if (isPipDirtied)
	{
		[renderEncoder_ setRenderPipelineState:pip->GetRenderPipelineState()];

		if (pip->GetDepthStencilState() != nullptr)
		{
			[renderEncoder_ setDepthStencilState:pip->GetDepthStencilState()];
		}

		[renderEncoder_ setStencilReferenceValue:pip->StencilRef];
	}

	// draw
	MTLPrimitiveType topology = MTLPrimitiveTypeTriangle;
	MTLIndexType indexType = MTLIndexTypeUInt32;
	const auto indexPerPrim = GetIndexCountPerPrimitive(bpip->Topology);
	if (indexPerPrim == 0)
	{
		Log(LogType::Error, "CommandListMetal::Draw skipped: unsupported topology. topology=" + to_string(bpip->Topology));
		return;
	}

	if (bpip->Topology == TopologyType::Triangle)
	{
		topology = MTLPrimitiveTypeTriangle;
	}
	else if (bpip->Topology == TopologyType::Line)
	{
		topology = MTLPrimitiveTypeLine;
	}
	else if (bpip->Topology == TopologyType::Point)
	{
		topology = MTLPrimitiveTypePoint;
	}

	if (bib.stride == 2)
	{
		indexType = MTLIndexTypeUInt16;
	}

	[renderEncoder_ drawIndexedPrimitives:topology
							   indexCount:primitiveCount * indexPerPrim
								indexType:indexType
							  indexBuffer:ib->GetBuffer()
						indexBufferOffset:bib.offset
							instanceCount:instanceCount];

	CommandList::Draw(primitiveCount, instanceCount);
}

void CommandListMetal::CopyTexture(Texture* src, Texture* dst)
{
	auto srcTex = static_cast<TextureMetal*>(src);
	CopyTexture(src, dst, {0, 0, 0}, {0, 0, 0}, srcTex->GetParameter().Size, 0, 0);
}

void CommandListMetal::CopyTexture(
	Texture* src, Texture* dst, const Vec3I& srcPos, const Vec3I& dstPos, const Vec3I& size, int srcLayer, int dstLayer)
{
	@autoreleasepool
	{
		if (isInRenderPass_ && !doesBeginWithPlatform_)
		{
			Log(LogType::Error, "Please call CopyTexture outside of RenderPass");
			return;
		}

		auto srcTex = static_cast<TextureMetal*>(src);
		auto dstTex = static_cast<TextureMetal*>(dst);

		id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer_ blitCommandEncoder];

		MTLRegion region = {{(uint32_t)srcPos[0], (uint32_t)srcPos[1], (uint32_t)srcPos[2]},
							{(uint32_t)size[0], (uint32_t)size[1], (uint32_t)size[2]}};

		[blitEncoder copyFromTexture:srcTex->GetTexture()
						 sourceSlice:srcLayer
						 sourceLevel:0
						sourceOrigin:region.origin
						  sourceSize:region.size
						   toTexture:dstTex->GetTexture()
					destinationSlice:dstLayer
					destinationLevel:0
				   destinationOrigin:{(uint32_t)dstPos[0], (uint32_t)dstPos[1], (uint32_t)dstPos[2]}];
		[blitEncoder endEncoding];

		RegisterReferencedObject(src);
		RegisterReferencedObject(dst);
	}
}

void CommandListMetal::GenerateMipMap(Texture* src)
{
	auto srcTex = static_cast<TextureMetal*>(src);
	const auto& parameter = srcTex->GetParameter();

	if (!parameter.IsMipmapGenerationEnabled || IsBlockCompressedFormat(parameter.Format))
	{
		return;
	}

	id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer_ blitCommandEncoder];
	[blitEncoder generateMipmapsForTexture:srcTex->GetTexture()];
	[blitEncoder endEncoding];
}

void CommandListMetal::BeginRenderPass(RenderPass* renderPass)
{
	@autoreleasepool
	{
		auto rp = static_cast<RenderPassMetal*>(renderPass);
		auto rpd = rp->GetRenderPassDescriptor();

		if (EnsureVisibilityResultBuffer(InitialVisibilityResultBufferCount))
		{
			memset([visibilityResultBuffer_ contents], 0, sizeof(uint64_t) * visibilityResultBufferCount_);
			visibilityResultOffset_ = 0;
			pendingOcclusionQueries_.clear();
			rpd.visibilityResultBuffer = visibilityResultBuffer_;
		}

		for (size_t i = 0; i < rp->pixelFormats.size(); i++)
		{
			if (rp->isColorCleared)
			{
				auto r_ = rp->clearColor.R / 255.0;
				auto g_ = rp->clearColor.G / 255.0;
				auto b_ = rp->clearColor.B / 255.0;
				auto a_ = rp->clearColor.A / 255.0;

				rpd.colorAttachments[i].loadAction = MTLLoadActionClear;
				rpd.colorAttachments[i].clearColor = MTLClearColorMake(r_, g_, b_, a_);
			}
			else
			{
				rpd.colorAttachments[i].loadAction = MTLLoadActionLoad;
			}
		}

		if (rp->isDepthCleared)
		{
			rpd.depthAttachment.loadAction = MTLLoadActionClear;
			rpd.depthAttachment.clearDepth = 1.0;

			if (rp->depthStencilFormat == MTLPixelFormatDepth32Float_Stencil8
#if !(TARGET_OS_IPHONE) && !(TARGET_OS_SIMULATOR)
				|| rp->depthStencilFormat == MTLPixelFormatDepth24Unorm_Stencil8
#endif
			)
			{
				rpd.stencilAttachment.loadAction = MTLLoadActionClear;
				rpd.stencilAttachment.clearStencil = 0;
			}
		}
		else
		{
			rpd.depthAttachment.loadAction = MTLLoadActionLoad;
			rpd.stencilAttachment.loadAction = MTLLoadActionLoad;
		}

		renderEncoder_ = [commandBuffer_ renderCommandEncoderWithDescriptor:rpd];
		[renderEncoder_ retain];
		[renderEncoder_ waitForFence:fence_ beforeStages:MTLRenderStageVertex];

		CommandList::BeginRenderPass(renderPass);
	}
}

void CommandListMetal::EndRenderPass()
{
	if (renderEncoder_)
	{
		[renderEncoder_ updateFence:fence_ afterStages:MTLRenderStageFragment];
		[renderEncoder_ endEncoding];
		[renderEncoder_ release];
		renderEncoder_ = nullptr;
	}

	if (!pendingOcclusionQueries_.empty())
	{
		auto blitEncoder = [commandBuffer_ blitCommandEncoder];
		for (auto& pending : pendingOcclusionQueries_)
		{
			if (pending.query == nullptr || pending.query->GetOcclusionBuffer() == nil)
			{
				continue;
			}

			[blitEncoder copyFromBuffer:visibilityResultBuffer_
						   sourceOffset:sizeof(uint64_t) * pending.visibilityIndex
							   toBuffer:pending.query->GetOcclusionBuffer()
					  destinationOffset:sizeof(uint64_t) * pending.queryIndex
								   size:sizeof(uint64_t)];
		}
		[blitEncoder endEncoding];
		pendingOcclusionQueries_.clear();
	}

	CommandList::EndRenderPass();
}

void CommandListMetal::WaitUntilCompleted()
{
	if (commandBuffer_ != nullptr)
	{
		auto status = [commandBuffer_ status];
		if (status == MTLCommandBufferStatusNotEnqueued)
		{
			return;
		}

		[commandBuffer_ waitUntilCompleted];
	}
}

bool CommandListMetal::BeginWithPlatform(void* platformContextPtr) { return CommandList::BeginWithPlatform(platformContextPtr); }

void CommandListMetal::EndWithPlatform() { CommandList::EndWithPlatform(); }

bool CommandListMetal::BeginRenderPassWithPlatformPtr(void* platformPtr)
{
	auto pp = reinterpret_cast<CommandListMetalPlatformRenderPassContext*>(platformPtr);

	this->renderEncoder_ = pp->RenderEncoder;

	if (this->renderEncoder_)
	{
		[this->renderEncoder_ retain];
		// TODO : make correct. wait can do only once per encorder
		// [this->renderEncoder waitForFence:fence beforeStages:MTLRenderStageVertex];
	}

	return CommandList::BeginRenderPassWithPlatformPtr(platformPtr);
}

bool CommandListMetal::EndRenderPassWithPlatformPtr()
{
	if (renderEncoder_)
	{
		// TODO : make correct. wait can do only once per encorder
		// [renderEncoder updateFence:fence afterStages:MTLRenderStageFragment];
		[renderEncoder_ release];
		renderEncoder_ = nullptr;
	}

	return CommandList::EndRenderPassWithPlatformPtr();
}

void CommandListMetal::BeginComputePass()
{
	computeEncoder_ = [commandBuffer_ computeCommandEncoder];
	[computeEncoder_ retain];
}

void CommandListMetal::EndComputePass()
{
	if (computeEncoder_)
	{
		[computeEncoder_ endEncoding];
		[computeEncoder_ release];
		computeEncoder_ = nullptr;
	}
}

bool CommandListMetal::BeginComputePassWithPlatformPtr(void* platformPtr)
{
	auto pp = reinterpret_cast<CommandListMetalPlatformComputePassContext*>(platformPtr);

	this->computeEncoder_ = pp->ComputeEncoder;
	
	if (this->computeEncoder_)
	{
		[this->computeEncoder_ retain];
	}

	return CommandList::BeginComputePassWithPlatformPtr(platformPtr);
}

bool CommandListMetal::EndComputePassWithPlatformPtr()
{
	if (computeEncoder_)
	{
		[computeEncoder_ release];
		computeEncoder_ = nullptr;
	}

	return CommandList::EndComputePassWithPlatformPtr();
}

void CommandListMetal::Dispatch(int32_t groupX, int32_t groupY, int32_t groupZ, int32_t threadX, int32_t threadY, int32_t threadZ)
{
    const int mipmapFilter = 1;

	PipelineState* bpip = nullptr;

	bool isPipDirtied = false;

	if (!ValidateDispatchState("CommandListMetal", groupX, groupY, groupZ, threadX, threadY, threadZ, bpip, isPipDirtied))
	{
		return;
	}

	if (computeEncoder_ == nullptr)
	{
		Log(LogType::Error, "CommandListMetal::Dispatch skipped: compute encoder is null.");
		return;
	}

	auto pip = static_cast<PipelineStateMetal*>(bpip);
	if (pip->GetComputePipelineState() == nullptr)
	{
		Log(LogType::Error, "CommandListMetal::Dispatch skipped: compute pipeline state is null.");
		return;
	}

    const int compute_offset = 10;
    
    // assign constant buffers
    for(size_t i = 0; i < constantBuffers_.size(); i++)
    {
        auto cb = static_cast<BufferMetal*>(constantBuffers_[i]);
        if(cb != nullptr)
        {
            [computeEncoder_ setBuffer:cb->GetBuffer() offset:cb->GetOffset() atIndex:i];
        }
    }

	// Assign textures
    for (int unit_ind = 0; unit_ind < currentTextures_.size(); unit_ind++)
    {
        auto texture = (TextureMetal*)currentTextures_[unit_ind].texture;
        auto nativeTexture = texture != nullptr ? texture->GetTexture() : fallbackSampledTexture_;
        if (nativeTexture == nullptr)
            continue;
        auto wm = (int32_t)currentTextures_[unit_ind].wrapMode;
        auto mm = (int32_t)currentTextures_[unit_ind].minMagFilter;
        auto pm = 0;
        if (nativeTexture.mipmapLevelCount >= 2)
        {
            pm = mipmapFilter;
        }
        
        [computeEncoder_ setTexture:nativeTexture atIndex:unit_ind];
        [computeEncoder_ setSamplerState:samplerStates_[wm][mm][pm] atIndex:unit_ind];
    }

    for (int unit_ind = 0; unit_ind < NumStorageBuffer; unit_ind++)
    {
        BindingStorageBuffer bcb;
        GetCurrentStorageBuffer(unit_ind, bcb);
        if (bcb.storageBuffer == nullptr)
            continue;
        
        auto cb = static_cast<BufferMetal*>(bcb.storageBuffer);
        [computeEncoder_ setBuffer:cb->GetBuffer() offset:cb->GetOffset() atIndex:compute_offset + unit_ind];
    }
    
	if (isPipDirtied)
	{
		[computeEncoder_ setComputePipelineState:pip->GetComputePipelineState()];
	}

	[computeEncoder_ dispatchThreadgroups:{(uint32_t)groupX, (uint32_t)groupY, (uint32_t)groupZ} threadsPerThreadgroup:{(uint32_t)threadX, (uint32_t)threadY, (uint32_t)threadZ}];

	CommandList::Dispatch(groupX, groupY, groupZ, threadX, threadY, threadZ);
}

void CommandListMetal::CopyBuffer(Buffer* src, Buffer* dst)
{
	auto srcBuf = static_cast<BufferMetal*>(src);
	auto srcGpuBuf = srcBuf->GetBuffer();

	auto dstBuf = static_cast<BufferMetal*>(dst);
	auto dstGpuBuf = dstBuf->GetBuffer();

	auto encoder = [commandBuffer_ blitCommandEncoder];
	[encoder retain];
	[encoder copyFromBuffer:srcGpuBuf
			   sourceOffset:srcBuf->GetOffset()
				   toBuffer:dstGpuBuf
		  destinationOffset:dstBuf->GetOffset()
					   size:std::min(srcBuf->GetSize(), dstBuf->GetSize())];
	[encoder endEncoding];
	[encoder release];

	RegisterReferencedObject(src);
	RegisterReferencedObject(dst);
}


bool CommandListMetal::ResetQuery(Query* query)
{
	auto query_ = static_cast<QueryMetal*>(query);
	if (query_ == nullptr)
	{
		return false;
	}

	if (query_->GetQueryType() == QueryType::Occulusion)
	{
		auto buffer = query_->GetOcclusionBuffer();
		if (buffer == nil)
		{
			return false;
		}

		memset([buffer contents], 0, sizeof(uint64_t) * query_->GetQueryCount());
	}

	return true;
}

bool CommandListMetal::BeginQuery(Query* query, uint32_t queryIndex)
{
	auto query_ = static_cast<QueryMetal*>(query);
	if (query_ == nullptr || queryIndex >= query_->GetQueryCount())
	{
		return false;
	}

	if (query_->GetQueryType() == QueryType::Occulusion)
	{
		if (renderEncoder_ == nullptr || query_->GetOcclusionBuffer() == nil || visibilityResultBuffer_ == nil ||
			visibilityResultOffset_ >= visibilityResultBufferCount_)
		{
			return false;
		}

		const auto visibilityIndex = visibilityResultOffset_++;
		[renderEncoder_ setVisibilityResultMode:MTLVisibilityResultModeCounting offset:sizeof(uint64_t) * visibilityIndex];

		PendingOcclusionQuery pending;
		pending.query = query_;
		pending.queryIndex = queryIndex;
		pending.visibilityIndex = visibilityIndex;
		pendingOcclusionQueries_.push_back(pending);
		RegisterReferencedObject(query);
		return true;
	}

	return false;
}

bool CommandListMetal::EndQuery(Query* query, uint32_t queryIndex)
{
	auto query_ = static_cast<QueryMetal*>(query);
	if (query_ == nullptr || queryIndex >= query_->GetQueryCount())
	{
		return false;
	}

	if (query_->GetQueryType() == QueryType::Occulusion)
	{
		if (renderEncoder_ == nullptr)
		{
			return false;
		}

		[renderEncoder_ setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
		return true;
	}

	return false;
}

bool CommandListMetal::RecordTimestamp(Query* query, uint32_t queryIndex)
{
	auto query_ = static_cast<QueryMetal*>(query);
	if (query_ == nullptr)
	{
		return false;
	}

	if (query_->GetQueryType() == QueryType::Timestamp)
	{
		if (queryIndex >= query_->GetQueryCount())
		{
			return false;
		}

		id<MTLCounterSampleBuffer> buffer = query_->GetTimestampBuffer();
		if (buffer == nil)
		{
			return false;
		}
		if (computeEncoder_)
		{
			[computeEncoder_ sampleCountersInBuffer:buffer atSampleIndex:queryIndex withBarrier:TRUE];
		}
		else if (renderEncoder_)
		{
			[renderEncoder_ sampleCountersInBuffer:buffer atSampleIndex:queryIndex withBarrier:TRUE];
		}
		return true;
	}
	return false;
}


}
