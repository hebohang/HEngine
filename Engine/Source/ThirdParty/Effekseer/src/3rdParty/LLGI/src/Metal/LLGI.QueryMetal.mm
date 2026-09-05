#include "LLGI.QueryMetal.h"
#include "LLGI.CommandListMetal.h"
#include "LLGI.GraphicsMetal.h"
#include "LLGI.Metal_Impl.h"
#include "LLGI.PipelineStateMetal.h"
#include "LLGI.RenderPassMetal.h"
#include "LLGI.ShaderMetal.h"
#include "LLGI.SingleFrameMemoryPoolMetal.h"
#include "LLGI.TextureMetal.h"

#import <MetalKit/MetalKit.h>

namespace LLGI
{

QueryMetal::QueryMetal()
{
}

QueryMetal::~QueryMetal()
{
	if (timestampBuffer_ != nullptr)
	{
		[timestampBuffer_ release];
		timestampBuffer_ = nullptr;
	}

	if (occlusionBuffer_ != nullptr)
	{
		[occlusionBuffer_ release];
		occlusionBuffer_ = nullptr;
	}
}

bool QueryMetal::Initialize(Graphics* graphics, QueryType queryType, uint32_t queryCount)
{
	queryType_ = queryType;
	queryCount_ = queryCount;

	auto device = static_cast<GraphicsMetal*>(graphics)->GetDevice();
	
	if (queryType == QueryType::Timestamp)
	{
		MTLCounterSampleBufferDescriptor *descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
		descriptor.label = @"Timestamps";
		descriptor.storageMode = MTLStorageModeShared;
		descriptor.sampleCount = queryCount;
		for (id<MTLCounterSet> counterSet in device.counterSets)
		{
			if ([counterSet.name isEqualToString:MTLCommonCounterSetTimestamp])
			{
				descriptor.counterSet = counterSet;
			}
		}
		if (descriptor.counterSet != nil)
		{
			timestampBuffer_ = [device newCounterSampleBufferWithDescriptor:descriptor error:nil];
			[descriptor release];
			return (timestampBuffer_ != nil);
		}
		[descriptor release];
	}
	else if (queryType == QueryType::Occulusion)
	{
		occlusionBuffer_ = [device newBufferWithLength:sizeof(uint64_t) * queryCount options:MTLResourceStorageModeShared];
		return (occlusionBuffer_ != nil);
	}
	return false;
}

uint64_t QueryMetal::GetQueryResult(uint32_t queryIndex)
{
	if (queryIndex >= queryCount_)
	{
		return 0;
	}

	if (queryType_ == QueryType::Occulusion)
	{
		if (occlusionBuffer_ == nil)
		{
			return 0;
		}

		auto data = static_cast<const uint64_t*>([occlusionBuffer_ contents]);
		return data[queryIndex];
	}

	if (timestampBuffer_ == nil)
	{
		return 0;
	}

	uint64_t result = 0;
	NSRange range = { queryIndex, 1 };
	NSData * counterData = [timestampBuffer_ resolveCounterRange:range];
	if (counterData)
	{
		memcpy(&result, [counterData bytes], sizeof(uint64_t));
	}
	return result;
}

}
