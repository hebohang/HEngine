
#pragma once

#include "../LLGI.Query.h"
#include "LLGI.BaseVulkan.h"
#include "LLGI.GraphicsVulkan.h"

namespace LLGI
{

class QueryVulkan : public Query
{
private:
	std::shared_ptr<GraphicsVulkan> graphics_;
	vk::QueryPool queryPool_;
	QueryType queryType_{};
	uint32_t queryCount_{};

public:
	bool Initialize(GraphicsVulkan* graphics, QueryType queryType, uint32_t queryCount);

	QueryVulkan();
	~QueryVulkan() override;

	vk::QueryPool& GetQueryPool() { return queryPool_; }

	uint32_t GetQueryCount() const { return queryCount_; }

	uint64_t GetQueryResult(uint32_t queryIndex) override;
};

} // namespace LLGI
