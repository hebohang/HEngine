#pragma once

#include "Runtime/Core/Meta/Json.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace HEngine
{
	struct DevCommandResult
	{
		bool bSuccess = false;
		std::string Code;
		std::string Message;
		Json Data = Json::object();
		bool bCommitted = false;
		bool bTainted = false;
		std::uint64_t ReceiptId = 0;

		[[nodiscard]] static DevCommandResult Success(
			Json data = Json::object(),
			std::string message = "命令执行完成。");
		[[nodiscard]] static DevCommandResult Failure(
			std::string code,
			std::string message,
			Json data = Json::object());
		[[nodiscard]] Json ToJson() const;
	};
}
