#pragma once

#include "LLGI.Base.h"

namespace LLGI
{

class Query : public ReferenceObject
{
protected:
	QueryType queryType_ = QueryType::Timestamp;

public:
	Query() = default;
	~Query() override = default;

	QueryType GetQueryType() const { return queryType_; }

	virtual uint64_t GetQueryResult(uint32_t queryIndex) { return 0; }
};

} // namespace LLGI
