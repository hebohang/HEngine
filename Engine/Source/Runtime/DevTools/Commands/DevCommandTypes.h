#pragma once

#include "Runtime/DevTools/Commands/DevCommandResult.h"

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HEngine
{
	enum class DevCommandArgumentType
	{
		Boolean,
		Integer,
		Number,
		String,
		Enumeration
	};

	enum class DevCommandRisk
	{
		Safe,
		Confirm,
		Dangerous
	};

	struct DevCommandArgument
	{
		std::string Name;
		std::string DisplayName;
		std::string Summary;
		DevCommandArgumentType Type = DevCommandArgumentType::String;
		bool bRequired = false;
		Json DefaultValue = nullptr;
		std::optional<double> Minimum;
		std::optional<double> Maximum;
		std::size_t MaxLength = 1024;
		std::vector<std::string> Choices;
		std::vector<std::string> ChoiceLabels;
	};

	struct DevCommandDescription
	{
		std::string Name;
		std::string DisplayName;
		std::string Summary;
		std::string Category;
		std::string Scope = "read-only";
		std::string Owner;
		DevCommandRisk Risk = DevCommandRisk::Safe;
		std::vector<DevCommandArgument> Arguments;
		std::vector<std::string> StateDomains;
		bool bTaintsCurrentSession = false;
		bool bUseTypedArguments = true;
		Json RawParamsSchema = Json::object();
	};

	struct DevCommandExecutionContext
	{
		Json Arguments = Json::object();
		std::map<std::string, Json, std::less<>> StagedDomains;
		std::string Source;

		[[nodiscard]] Json& RequireStateDomain(std::string_view name);
		[[nodiscard]] const Json& RequireStateDomain(std::string_view name) const;
	};

	class DevCommandError : public std::runtime_error
	{
	public:
		DevCommandError(std::string code, std::string message)
			: std::runtime_error(std::move(message)), mCode(std::move(code))
		{
		}

		[[nodiscard]] const std::string& GetCode() const { return mCode; }

	private:
		std::string mCode;
	};

	[[nodiscard]] std::string_view ToString(DevCommandArgumentType type);
	[[nodiscard]] std::string_view ToString(DevCommandRisk risk);
	[[nodiscard]] std::optional<DevCommandArgumentType> ParseDevCommandArgumentType(
		std::string_view value);
	[[nodiscard]] std::optional<DevCommandRisk> ParseDevCommandRisk(std::string_view value);
	[[nodiscard]] Json ToJson(const DevCommandArgument& argument);
	[[nodiscard]] Json ToJson(const DevCommandDescription& description);
}
