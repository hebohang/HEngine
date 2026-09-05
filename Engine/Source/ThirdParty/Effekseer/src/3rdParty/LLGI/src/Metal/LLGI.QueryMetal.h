#pragma once

#include "../LLGI.Graphics.h"
#include "../LLGI.Query.h"
#import <MetalKit/MetalKit.h>
#include <functional>
#include <memory>
#include <unordered_map>

namespace LLGI
{

class QueryMetal : public Query
{
private:
	id<MTLCounterSampleBuffer> timestampBuffer_ = nullptr;
	id<MTLBuffer> occlusionBuffer_ = nullptr;
	uint32_t queryCount_ = 0;

public:
	QueryMetal();
	~QueryMetal() override;
    
	bool Initialize(Graphics* graphics, QueryType queryType, uint32_t queryCount);

	uint32_t GetQueryCount() const { return queryCount_; }

	uint64_t GetQueryResult(uint32_t queryIndex) override;

	id<MTLCounterSampleBuffer> GetTimestampBuffer() { return timestampBuffer_; }
	id<MTLBuffer> GetOcclusionBuffer() { return occlusionBuffer_; }
};

} // namespace LLGI
