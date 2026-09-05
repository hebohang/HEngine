#pragma once

#include "Game/AgentRuntime/AgentProtocol.h"
#include "Runtime/DevTools/Commands/DevCommandRegistry.h"

#include <functional>
#include <string>
#include <string_view>

namespace HEngine
{
	using AgentCommandError = DevCommandError;

	struct AgentCommandDescription
	{
		std::string Name;
		std::string Summary;
		std::string Scope;
		Json ParamsSchema = Json::object();
	};

	class AgentCommandRegistry final
	{
	public:
		using Handler = std::function<Json(const Json& params)>;

		[[nodiscard]] bool Register(AgentCommandDescription description, Handler handler);
		[[nodiscard]] Json Dispatch(const Json& request, std::string_view expectedToken) const;
		[[nodiscard]] Json Discover() const;

		[[nodiscard]] static Json MakeError(
			const Json& requestId,
			std::string_view code,
			std::string_view message);

	private:
		[[nodiscard]] static bool ConstantTimeEquals(std::string_view lhs, std::string_view rhs);
		[[nodiscard]] static bool IsValidIdentifier(std::string_view value, std::size_t maxLength, bool bAllowDot);

		DevCommandRegistry mCommands;
	};
}
