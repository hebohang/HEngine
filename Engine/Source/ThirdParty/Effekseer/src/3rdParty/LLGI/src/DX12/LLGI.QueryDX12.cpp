#include "LLGI.QueryDX12.h"
#include "LLGI.SingleFrameMemoryPoolDX12.h"

namespace LLGI
{

QueryDX12::QueryDX12() {}

QueryDX12::~QueryDX12()
{
	SafeRelease(buffer_);
	SafeRelease(queryHeap_);
}

bool QueryDX12::Initialize(GraphicsDX12* graphics, QueryType queryType, uint32_t queryCount)
{
	queryType_ = queryType;
	queryCount_ = queryCount;

	{
		D3D12_QUERY_HEAP_DESC heapDesc = {};
		heapDesc.Count = static_cast<UINT>(queryCount);

		switch (queryType)
		{
		case QueryType::Timestamp:
			heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			queryTypeDX12_ = D3D12_QUERY_TYPE_TIMESTAMP;
			break;
		case QueryType::Occulusion:
			heapDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
			queryTypeDX12_ = D3D12_QUERY_TYPE_OCCLUSION;
			break;
		default:
			return false;
		}

		graphics->GetDevice()->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&queryHeap_));
	}

	{
		D3D12_RESOURCE_DESC resDesc{};
		resDesc.SampleDesc.Count = 1;
		resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resDesc.Width = sizeof(uint64_t) * queryCount;
		resDesc.Height = 1;
		resDesc.MipLevels = 1;
		resDesc.DepthOrArraySize = 1;
		resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		D3D12_HEAP_PROPERTIES heapProp{};
		heapProp.Type = D3D12_HEAP_TYPE_READBACK;

		graphics->GetDevice()->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer_));
	}

	return true;
}

uint64_t QueryDX12::GetQueryResult(uint32_t queryIndex)
{
	D3D12_RANGE range{queryIndex * sizeof(uint64_t), (queryIndex + 1) * sizeof(uint64_t)};
	void* ptr = nullptr;
	buffer_->Map(0, &range, &ptr);
	auto data = reinterpret_cast<uint64_t*>(ptr);
	uint64_t result = data[queryIndex];
	buffer_->Unmap(0, nullptr);

	return result;
}

} // namespace LLGI
