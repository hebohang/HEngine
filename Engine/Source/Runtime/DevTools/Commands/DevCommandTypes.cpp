#include "Hepch.h"

#include "Runtime/DevTools/Commands/DevCommandTypes.h"

#include <stdexcept>

namespace HEngine
{
	Json& DevCommandExecutionContext::RequireStateDomain(std::string_view name)
	{
		const auto domain = StagedDomains.find(name);
		if (domain == StagedDomains.end())
			throw std::logic_error("Command handler requested an unstaged state domain.");
		return domain->second;
	}

	const Json& DevCommandExecutionContext::RequireStateDomain(std::string_view name) const
	{
		const auto domain = StagedDomains.find(name);
		if (domain == StagedDomains.end())
			throw std::logic_error("Command handler requested an unstaged state domain.");
		return domain->second;
	}

	std::string_view ToString(DevCommandArgumentType type)
	{
		switch (type)
		{
			case DevCommandArgumentType::Boolean: return "boolean";
			case DevCommandArgumentType::Integer: return "integer";
			case DevCommandArgumentType::Number: return "number";
			case DevCommandArgumentType::String: return "string";
			case DevCommandArgumentType::Enumeration: return "enum";
		}
		return "unknown";
	}

	std::string_view ToString(DevCommandRisk risk)
	{
		switch (risk)
		{
			case DevCommandRisk::Safe: return "safe";
			case DevCommandRisk::Confirm: return "confirm";
			case DevCommandRisk::Dangerous: return "dangerous";
		}
		return "unknown";
	}

	std::optional<DevCommandArgumentType> ParseDevCommandArgumentType(std::string_view value)
	{
		if (value == "boolean" || value == "bool")
			return DevCommandArgumentType::Boolean;
		if (value == "integer" || value == "int")
			return DevCommandArgumentType::Integer;
		if (value == "number" || value == "float")
			return DevCommandArgumentType::Number;
		if (value == "string")
			return DevCommandArgumentType::String;
		if (value == "enum")
			return DevCommandArgumentType::Enumeration;
		return std::nullopt;
	}

	std::optional<DevCommandRisk> ParseDevCommandRisk(std::string_view value)
	{
		if (value == "safe")
			return DevCommandRisk::Safe;
		if (value == "confirm" || value == "caution")
			return DevCommandRisk::Confirm;
		if (value == "dangerous" || value == "danger")
			return DevCommandRisk::Dangerous;
		return std::nullopt;
	}

	Json ToJson(const DevCommandArgument& argument)
	{
		Json result{
			{ "name", argument.Name },
			{ "displayName", argument.DisplayName },
			{ "summary", argument.Summary },
			{ "type", ToString(argument.Type) },
			{ "required", argument.bRequired },
			{ "maxLength", argument.MaxLength }
		};
		if (!argument.DefaultValue.is_null())
			result["default"] = argument.DefaultValue;
		if (argument.Minimum)
			result["minimum"] = *argument.Minimum;
		if (argument.Maximum)
			result["maximum"] = *argument.Maximum;
		if (!argument.Choices.empty())
			result["choices"] = argument.Choices;
		if (!argument.ChoiceLabels.empty())
			result["choiceLabels"] = argument.ChoiceLabels;
		return result;
	}

	Json ToJson(const DevCommandDescription& description)
	{
		Json arguments = Json::array();
		for (const DevCommandArgument& argument : description.Arguments)
			arguments.push_back(ToJson(argument));

		return Json{
			{ "name", description.Name },
			{ "displayName", description.DisplayName },
			{ "summary", description.Summary },
			{ "category", description.Category },
			{ "scope", description.Scope },
			{ "owner", description.Owner },
			{ "risk", ToString(description.Risk) },
			{ "arguments", std::move(arguments) },
			{ "stateDomains", description.StateDomains },
			{ "taintsCurrentSession", description.bTaintsCurrentSession }
		};
	}
}
