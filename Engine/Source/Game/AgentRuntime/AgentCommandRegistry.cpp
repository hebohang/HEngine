#include "Game/AgentRuntime/AgentCommandRegistry.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace HEngine
{
	bool AgentCommandRegistry::Register(AgentCommandDescription description, Handler handler)
	{
		if (!handler || !IsValidIdentifier(description.Name, AgentProtocol::MaxMethodLength, true))
			return false;

		DevCommandDescription command;
		command.Name = std::move(description.Name);
		command.DisplayName = command.Name;
		command.Summary = std::move(description.Summary);
		command.Category = "Agent";
		command.Scope = std::move(description.Scope);
		command.Owner = "engine.agent";
		command.bUseTypedArguments = false;
		command.RawParamsSchema = std::move(description.ParamsSchema);
		return mCommands.Register(
			std::move(command),
			[handler = std::move(handler)](DevCommandExecutionContext& context) mutable
			{
				return DevCommandResult::Success(handler(context.Arguments));
			});
	}

	Json AgentCommandRegistry::Dispatch(const Json& request, std::string_view expectedToken) const
	{
		Json requestId = nullptr;
		if (!request.is_object())
			return MakeError(requestId, "invalid_request", "Request must be a JSON object.");

		for (const auto& [name, value] : request.items())
		{
			(void)value;
			if (name != "protocol" && name != "id" && name != "method" && name != "token" && name != "params")
				return MakeError(requestId, "invalid_request", "Request contains an unknown field.");
		}

		if (!request.contains("protocol") || !request["protocol"].is_number_unsigned() ||
			request["protocol"].get<std::uint32_t>() != AgentProtocol::Version)
		{
			return MakeError(requestId, "unsupported_protocol", "Unsupported or missing protocol version.");
		}

		if (!request.contains("id") || !request["id"].is_string())
			return MakeError(requestId, "invalid_request", "Request id is required.");

		const std::string id = request["id"].get<std::string>();
		if (!IsValidIdentifier(id, AgentProtocol::MaxRequestIdLength, true))
			return MakeError(requestId, "invalid_request", "Request id has an invalid format.");
		requestId = id;

		if (!request.contains("token") || !request["token"].is_string() ||
			!ConstantTimeEquals(request["token"].get_ref<const std::string&>(), expectedToken))
		{
			return MakeError(requestId, "unauthorized", "Session token is invalid.");
		}

		if (!request.contains("method") || !request["method"].is_string())
			return MakeError(requestId, "invalid_request", "Request method is required.");

		const std::string& method = request["method"].get_ref<const std::string&>();
		if (!IsValidIdentifier(method, AgentProtocol::MaxMethodLength, true))
			return MakeError(requestId, "invalid_request", "Request method has an invalid format.");

		Json params = Json::object();
		if (request.contains("params"))
		{
			if (!request["params"].is_object())
				return MakeError(requestId, "invalid_params", "Request params must be a JSON object.");
			params = request["params"];
		}

		Json validatedParams;
		DevCommandResult validation = mCommands.ValidateArguments(method, params, validatedParams);
		if (!validation.bSuccess)
		{
			return MakeError(
				requestId,
				validation.Code == "command_not_found" ? "method_not_found" : "invalid_params",
				validation.Message);
		}

		DevCommandExecutionContext context;
		context.Arguments = std::move(validatedParams);
		context.Source = "agent";
		DevCommandResult result = mCommands.ExecuteValidated(method, context);
		if (!result.bSuccess)
		{
			return MakeError(
				requestId,
				result.Code == "invalid_arguments" ? "invalid_params" : result.Code,
				result.Message);
		}
		return Json{
			{ "protocol", AgentProtocol::Version },
			{ "id", requestId },
			{ "ok", true },
			{ "result", std::move(result.Data) }
		};
	}

	Json AgentCommandRegistry::Discover() const
	{
		Json commands = Json::array();
		for (const DevCommandDescription& command : mCommands.GetDescriptions())
		{
			commands.push_back({
				{ "name", command.Name },
				{ "summary", command.Summary },
				{ "scope", command.Scope },
				{ "params", command.RawParamsSchema }
			});
		}

		return Json{
			{ "protocolVersion", AgentProtocol::Version },
			{ "commands", std::move(commands) }
		};
	}

	Json AgentCommandRegistry::MakeError(
		const Json& requestId,
		std::string_view code,
		std::string_view message)
	{
		return Json{
			{ "protocol", AgentProtocol::Version },
			{ "id", requestId },
			{ "ok", false },
			{ "error", {
				{ "code", std::string(code) },
				{ "message", std::string(message) }
			} }
		};
	}

	bool AgentCommandRegistry::ConstantTimeEquals(std::string_view lhs, std::string_view rhs)
	{
		std::size_t difference = lhs.size() ^ rhs.size();
		const std::size_t length = std::max(lhs.size(), rhs.size());
		for (std::size_t index = 0; index < length; ++index)
		{
			const unsigned char left = index < lhs.size() ? static_cast<unsigned char>(lhs[index]) : 0;
			const unsigned char right = index < rhs.size() ? static_cast<unsigned char>(rhs[index]) : 0;
			difference |= left ^ right;
		}
		return difference == 0;
	}

	bool AgentCommandRegistry::IsValidIdentifier(std::string_view value, std::size_t maxLength, bool bAllowDot)
	{
		return DevCommandRegistry::IsValidIdentifier(value, maxLength, bAllowDot);
	}
}
