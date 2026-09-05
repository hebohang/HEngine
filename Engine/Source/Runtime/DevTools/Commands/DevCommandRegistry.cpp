#include "Hepch.h"

#include "Runtime/DevTools/Commands/DevCommandRegistry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace HEngine
{
	bool DevCommandRegistry::Register(DevCommandDescription description, Handler handler)
	{
		if (!handler || !ValidateDescription(description))
			return false;

		const std::string commandName = description.Name;
		auto [iterator, bInserted] = mCommands.emplace(
			commandName,
			Command{ std::move(description), std::move(handler) });
		(void)iterator;
		if (bInserted)
			++mRevision;
		return bInserted;
	}

	bool DevCommandRegistry::Unregister(std::string_view name)
	{
		const auto command = mCommands.find(name);
		if (command == mCommands.end())
			return false;
		mCommands.erase(command);
		++mRevision;
		return true;
	}

	std::size_t DevCommandRegistry::UnregisterOwner(std::string_view owner)
	{
		std::size_t removed = 0;
		for (auto command = mCommands.begin(); command != mCommands.end();)
		{
			if (command->second.Description.Owner == owner)
			{
				command = mCommands.erase(command);
				++removed;
			}
			else
			{
				++command;
			}
		}
		if (removed > 0)
			++mRevision;
		return removed;
	}

	const DevCommandDescription* DevCommandRegistry::Find(std::string_view name) const
	{
		const auto command = mCommands.find(name);
		return command == mCommands.end() ? nullptr : &command->second.Description;
	}

	std::vector<DevCommandDescription> DevCommandRegistry::GetDescriptions() const
	{
		std::vector<DevCommandDescription> descriptions;
		descriptions.reserve(mCommands.size());
		for (const auto& [name, command] : mCommands)
		{
			(void)name;
			descriptions.push_back(command.Description);
		}
		return descriptions;
	}

	DevCommandResult DevCommandRegistry::ValidateArguments(
		std::string_view name,
		const Json& arguments,
		Json& outValidatedArguments) const
	{
		outValidatedArguments = Json::object();
		const auto command = mCommands.find(name);
		if (command == mCommands.end())
			return DevCommandResult::Failure("command_not_found", "请求的命令尚未注册。");
		if (!arguments.is_object())
			return DevCommandResult::Failure("invalid_arguments", "命令参数必须是 JSON 对象。");

		const DevCommandDescription& description = command->second.Description;
		if (!description.bUseTypedArguments)
		{
			outValidatedArguments = arguments;
			return DevCommandResult::Success();
		}

		for (const auto& [argumentName, value] : arguments.items())
		{
			(void)value;
			const auto schema = std::find_if(
				description.Arguments.begin(),
				description.Arguments.end(),
				[&argumentName](const DevCommandArgument& candidate)
				{
					return candidate.Name == argumentName;
				});
			if (schema == description.Arguments.end())
			{
				return DevCommandResult::Failure(
					"invalid_arguments",
					"命令参数包含未知字段：" + argumentName);
			}
		}

		for (const DevCommandArgument& schema : description.Arguments)
		{
			if (!arguments.contains(schema.Name))
			{
				if (!schema.DefaultValue.is_null())
				{
					outValidatedArguments[schema.Name] = schema.DefaultValue;
					continue;
				}
				if (schema.bRequired)
				{
					return DevCommandResult::Failure(
						"invalid_arguments",
						"缺少必填命令参数：" + schema.Name);
				}
				continue;
			}

			DevCommandResult validation = ValidateValue(schema, arguments[schema.Name]);
			if (!validation.bSuccess)
				return validation;
			outValidatedArguments[schema.Name] = arguments[schema.Name];
		}
		return DevCommandResult::Success();
	}

	DevCommandResult DevCommandRegistry::ExecuteValidated(
		std::string_view name,
		DevCommandExecutionContext& context) const
	{
		const auto command = mCommands.find(name);
		if (command == mCommands.end())
			return DevCommandResult::Failure("command_not_found", "请求的命令尚未注册。");
		try
		{
			return command->second.Invoke(context);
		}
		catch (const DevCommandError& error)
		{
			return DevCommandResult::Failure(error.GetCode(), error.what());
		}
		catch (const std::invalid_argument& error)
		{
			return DevCommandResult::Failure("invalid_arguments", error.what());
		}
		catch (...)
		{
			return DevCommandResult::Failure(
				"internal_error",
				"命令执行失败，内部状态未暴露。");
		}
	}

	bool DevCommandRegistry::IsValidIdentifier(
		std::string_view value,
		std::size_t maxLength,
		bool bAllowDot)
	{
		if (value.empty() || value.size() > maxLength)
			return false;
		return std::all_of(value.begin(), value.end(), [bAllowDot](unsigned char character)
		{
			return std::isalnum(character) != 0 || character == '_' || character == '-' ||
				(bAllowDot && character == '.');
		});
	}

	DevCommandResult DevCommandRegistry::ValidateValue(
		const DevCommandArgument& schema,
		const Json& value)
	{
		const std::string prefix = "命令参数“" + schema.Name + "”";
		switch (schema.Type)
		{
			case DevCommandArgumentType::Boolean:
				if (!value.is_boolean())
					return DevCommandResult::Failure("invalid_arguments", prefix + "必须是布尔值。");
				break;
			case DevCommandArgumentType::Integer:
				if (!value.is_number_integer() && !value.is_number_unsigned())
					return DevCommandResult::Failure("invalid_arguments", prefix + "必须是整数。");
				if (value.is_number_unsigned() &&
					value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				{
					return DevCommandResult::Failure("invalid_arguments", prefix + "超出支持的整数范围。");
				}
				break;
			case DevCommandArgumentType::Number:
				if (!value.is_number())
					return DevCommandResult::Failure("invalid_arguments", prefix + "必须是数值。");
				break;
			case DevCommandArgumentType::String:
			case DevCommandArgumentType::Enumeration:
				if (!value.is_string())
					return DevCommandResult::Failure("invalid_arguments", prefix + "必须是字符串。");
				if (value.get_ref<const std::string&>().size() > schema.MaxLength)
					return DevCommandResult::Failure("invalid_arguments", prefix + "长度超过限制。");
				if (schema.Type == DevCommandArgumentType::Enumeration &&
					std::find(schema.Choices.begin(), schema.Choices.end(),
						value.get_ref<const std::string&>()) == schema.Choices.end())
				{
					return DevCommandResult::Failure("invalid_arguments", prefix + "不是允许的选项。");
				}
				break;
		}

		if ((schema.Minimum || schema.Maximum) && value.is_number())
		{
			const double number = value.get<double>();
			if (!std::isfinite(number))
				return DevCommandResult::Failure("invalid_arguments", prefix + "必须是有限数值。");
			if (schema.Minimum && number < *schema.Minimum)
				return DevCommandResult::Failure("invalid_arguments", prefix + "低于最小值。");
			if (schema.Maximum && number > *schema.Maximum)
				return DevCommandResult::Failure("invalid_arguments", prefix + "高于最大值。");
		}
		return DevCommandResult::Success();
	}

	bool DevCommandRegistry::ValidateDescription(const DevCommandDescription& description)
	{
		if (!IsValidIdentifier(description.Name, 128, true) ||
			!IsValidIdentifier(description.Owner, 128, true) ||
			description.DisplayName.size() > 128 || description.Summary.size() > 1024 ||
			description.Category.size() > 128 || description.Scope.size() > 128)
		{
			return false;
		}

		std::set<std::string, std::less<>> argumentNames;
		for (const DevCommandArgument& argument : description.Arguments)
		{
			if (!IsValidIdentifier(argument.Name, 64, false) ||
				!argumentNames.insert(argument.Name).second ||
				argument.DisplayName.size() > 128 || argument.Summary.size() > 1024 ||
				argument.MaxLength == 0 || argument.MaxLength > 65536 ||
				(argument.Minimum && argument.Maximum && *argument.Minimum > *argument.Maximum) ||
				(argument.Type == DevCommandArgumentType::Enumeration && argument.Choices.empty()) ||
				(!argument.ChoiceLabels.empty() &&
					argument.ChoiceLabels.size() != argument.Choices.size()))
			{
				return false;
			}
			if (std::any_of(
				argument.ChoiceLabels.begin(),
				argument.ChoiceLabels.end(),
				[](const std::string& label) { return label.empty() || label.size() > 128; }))
			{
				return false;
			}
			if (!argument.DefaultValue.is_null() &&
				!ValidateValue(argument, argument.DefaultValue).bSuccess)
			{
				return false;
			}
		}

		std::set<std::string, std::less<>> domainNames;
		for (const std::string& domain : description.StateDomains)
		{
			if (!IsValidIdentifier(domain, 128, true) || !domainNames.insert(domain).second)
				return false;
		}
		return true;
	}
}
