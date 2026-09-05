#pragma once

#include "Runtime/Core/Meta/Json.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace HEngine
{
	namespace AgentProtocol
	{
		inline constexpr std::uint32_t Version = 1;
		inline constexpr std::size_t MaxRequestBytes = 64 * 1024;
		inline constexpr std::size_t MaxResponseBytes = 256 * 1024;
		inline constexpr std::size_t MaxRequestsPerTick = 8;
		inline constexpr std::size_t MaxRequestIdLength = 64;
		inline constexpr std::size_t MaxMethodLength = 96;
	}

	struct AgentTransportMessage
	{
		std::string Handle;
		Json Payload;
		std::string ErrorCode;
		std::string ErrorMessage;

		[[nodiscard]] bool HasTransportError() const { return !ErrorCode.empty(); }
	};
}
