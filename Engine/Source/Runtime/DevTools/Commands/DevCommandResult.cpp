#include "Hepch.h"

#include "Runtime/DevTools/Commands/DevCommandResult.h"

#include <utility>

namespace HEngine
{
	DevCommandResult DevCommandResult::Success(Json data, std::string message)
	{
		return DevCommandResult{
			.bSuccess = true,
			.Code = "ok",
			.Message = std::move(message),
			.Data = std::move(data)
		};
	}

	DevCommandResult DevCommandResult::Failure(
		std::string code,
		std::string message,
		Json data)
	{
		return DevCommandResult{
			.bSuccess = false,
			.Code = std::move(code),
			.Message = std::move(message),
			.Data = std::move(data)
		};
	}

	Json DevCommandResult::ToJson() const
	{
		return Json{
			{ "receiptId", ReceiptId },
			{ "success", bSuccess },
			{ "code", Code },
			{ "message", Message },
			{ "data", Data },
			{ "committed", bCommitted },
			{ "tainted", bTainted }
		};
	}
}
