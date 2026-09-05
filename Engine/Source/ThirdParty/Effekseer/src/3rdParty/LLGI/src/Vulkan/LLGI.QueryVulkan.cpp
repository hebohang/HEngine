#include "LLGI.QueryVulkan.h"
#include "LLGI.SingleFrameMemoryPoolVulkan.h"

namespace LLGI
{

QueryVulkan::QueryVulkan() {}

QueryVulkan::~QueryVulkan()
{
	if (queryPool_)
	{
		graphics_->GetDevice().destroyQueryPool(queryPool_);
		queryPool_ = nullptr;
	}
}

bool QueryVulkan::Initialize(GraphicsVulkan* graphics, QueryType queryType, uint32_t queryCount)
{
	graphics_ = CreateSharedPtr(graphics);
	queryType_ = queryType;
	queryCount_ = queryCount;

	vk::QueryPoolCreateInfo queryInfo;
	queryInfo.setQueryCount(queryCount);

	switch (queryType)
	{
	case QueryType::Timestamp:
		queryInfo.setQueryType(vk::QueryType::eTimestamp);
		break;
	case QueryType::Occulusion:
		queryInfo.setQueryType(vk::QueryType::eOcclusion);
		break;
	default:
		return false;
	}

	queryPool_ = graphics->GetDevice().createQueryPool(queryInfo);

	return true;
}

uint64_t QueryVulkan::GetQueryResult(uint32_t queryIndex)
{
	uint64_t value = 0;
	vk::Result result = graphics_->GetDevice().getQueryPoolResults(
		queryPool_, queryIndex, 1, sizeof(uint64_t), &value, sizeof(uint64_t),
		vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);

	if (result == vk::Result::eSuccess)
	{
		return value;
	}

	return 0;
}

} // namespace LLGI
