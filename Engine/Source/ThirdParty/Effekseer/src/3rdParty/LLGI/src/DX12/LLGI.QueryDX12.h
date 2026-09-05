
#pragma once

#include "../LLGI.Query.h"
#include "LLGI.BaseDX12.h"
#include "LLGI.GraphicsDX12.h"

namespace LLGI
{

class QueryDX12 : public Query
{
private:
	ID3D12QueryHeap* queryHeap_ = nullptr;
	ID3D12Resource* buffer_ = nullptr;
	D3D12_QUERY_TYPE queryTypeDX12_{};
	QueryType queryType_{};
	uint32_t queryCount_{};

public:
	bool Initialize(GraphicsDX12* graphics, QueryType queryType, uint32_t queryCount);

	QueryDX12();
	~QueryDX12() override;

	ID3D12QueryHeap* GetQueryHeap() { return queryHeap_; }

	ID3D12Resource* GetBuffer() { return buffer_; }

	D3D12_QUERY_TYPE GetQueryTypeDX12() const { return queryTypeDX12_; }

	uint32_t GetQueryCount() const { return queryCount_; }

	uint64_t GetQueryResult(uint32_t queryIndex) override;
};

} // namespace LLGI
