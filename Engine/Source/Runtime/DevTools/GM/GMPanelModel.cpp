#include "Hepch.h"

#include "Runtime/DevTools/GM/GMPanelModel.h"

#include <sstream>
#include <string_view>

namespace HEngine
{
	namespace
	{
		[[nodiscard]] std::string_view DisplayRisk(DevCommandRisk risk)
		{
			switch (risk)
			{
				case DevCommandRisk::Safe: return "安全";
				case DevCommandRisk::Confirm: return "需确认";
				case DevCommandRisk::Dangerous: return "危险";
			}
			return "未知";
		}

		[[nodiscard]] std::string_view DisplayScope(std::string_view scope)
		{
			if (scope == "read-only")
				return "只读";
			if (scope == "developer-ui")
				return "开发界面";
			if (scope == "persistent-profile")
				return "持久化档案";
			if (scope == "current-session")
				return "当前游戏局";
			return scope;
		}
	}

	std::string GMPanelModel::BuildCommandList(
		const Json& discovery,
		std::vector<std::string>& outCommandNames)
	{
		outCommandNames.clear();
		std::ostringstream markup;
		if (!discovery.contains("commands") || !discovery["commands"].is_array())
			return {};

		std::string currentCategory;
		for (const Json& command : discovery["commands"])
		{
			const std::string name = command.value("name", "");
			if (name.empty())
				continue;
			const std::string category = command.value("category", "其他");
			if (category != currentCategory)
			{
				currentCategory = category;
				markup << "<div class=\"gm-category\">" << Escape(category) << "</div>";
			}

			const std::size_t index = outCommandNames.size();
			outCommandNames.push_back(name);
			const bool bAvailable = command.value("available", true);
			const std::string risk = command.value("risk", "safe");
			markup << "<button id=\"gm_cmd_" << index << "\" class=\"gm-command risk-"
				<< Escape(risk);
			if (!bAvailable)
				markup << " unavailable";
			markup << "\"><span class=\"gm-command-title\">"
				<< Escape(command.value("displayName", name))
				<< "</span><span class=\"gm-command-name\">" << Escape(name)
				<< "</span></button>";
		}
		return markup.str();
	}

	std::string GMPanelModel::BuildCommandForm(const DevCommandDescription& description)
	{
		std::ostringstream markup;
		markup << "<div class=\"gm-form-header\"><h2>" << Escape(
			description.DisplayName.empty() ? description.Name : description.DisplayName)
			<< "</h2><span class=\"gm-risk risk-" << Escape(ToString(description.Risk))
			<< "\">" << Escape(DisplayRisk(description.Risk)) << "</span></div>"
			<< "<p class=\"gm-summary\">" << Escape(description.Summary) << "</p>"
			<< "<p class=\"gm-scope\">" << Escape(description.Name) << " · "
			<< Escape(DisplayScope(description.Scope)) << "</p>";

		for (std::size_t index = 0; index < description.Arguments.size(); ++index)
		{
			const DevCommandArgument& argument = description.Arguments[index];
			const std::string value = argument.DefaultValue.is_null()
				? std::string{}
				: ScalarText(argument.DefaultValue);
			markup << "<div class=\"gm-field\"><label for=\"gm_arg_" << index << "\">"
				<< Escape(argument.DisplayName.empty() ? argument.Name : argument.DisplayName);
			if (argument.bRequired)
				markup << " *";
			markup << "</label>";

			if (argument.Type == DevCommandArgumentType::Boolean)
			{
				markup << "<select id=\"gm_arg_" << index << "\">"
					<< "<option value=\"false\"" << (value == "false" ? " selected=\"selected\"" : "")
					<< ">否</option><option value=\"true\""
					<< (value == "true" ? " selected=\"selected\"" : "") << ">是</option></select>";
			}
			else if (argument.Type == DevCommandArgumentType::Enumeration)
			{
				markup << "<select id=\"gm_arg_" << index << "\">";
				for (std::size_t choiceIndex = 0; choiceIndex < argument.Choices.size(); ++choiceIndex)
				{
					const std::string& choice = argument.Choices[choiceIndex];
					const std::string& label = argument.ChoiceLabels.empty()
						? choice
						: argument.ChoiceLabels[choiceIndex];
					markup << "<option value=\"" << Escape(choice) << "\""
						<< (choice == value ? " selected=\"selected\"" : "") << ">"
						<< Escape(label) << "</option>";
				}
				markup << "</select>";
			}
			else
			{
				markup << "<input id=\"gm_arg_" << index << "\" type=\""
					<< (argument.Type == DevCommandArgumentType::String ? "text" : "number")
					<< "\" value=\"" << Escape(value) << "\"";
				if (argument.Minimum)
					markup << " min=\"" << *argument.Minimum << "\"";
				if (argument.Maximum)
					markup << " max=\"" << *argument.Maximum << "\"";
				markup << " />";
			}
			if (!argument.Summary.empty())
				markup << "<p class=\"gm-field-help\">" << Escape(argument.Summary) << "</p>";
			markup << "</div>";
		}

		markup << "<div class=\"gm-actions\"><button id=\"gm_execute\" class=\"gm-primary\">执行</button>"
			<< "<button id=\"gm_confirm\" class=\"gm-danger hidden\">确认执行</button></div>";
		return markup.str();
	}

	std::string GMPanelModel::BuildHistory(const Json& history)
	{
		if (!history.is_array() || history.empty())
			return "<p class=\"gm-empty\">暂无命令执行记录。</p>";

		std::ostringstream markup;
		for (auto entry = history.rbegin(); entry != history.rend(); ++entry)
		{
			const Json& result = entry->value("result", Json::object());
			const bool bSuccess = result.value("success", false);
			markup << "<div class=\"gm-history-entry " << (bSuccess ? "success" : "failure")
				<< "\"><span class=\"gm-receipt\">#" << entry->value("receiptId", 0ull)
				<< "</span><span class=\"gm-history-command\">" << Escape(entry->value("command", ""))
				<< "</span><span class=\"gm-history-code\">" << Escape(result.value("code", ""))
				<< "</span></div>";
		}
		return markup.str();
	}

	std::string GMPanelModel::Escape(std::string_view value)
	{
		std::string result;
		result.reserve(value.size());
		for (const char character : value)
		{
			switch (character)
			{
				case '&': result += "&amp;"; break;
				case '<': result += "&lt;"; break;
				case '>': result += "&gt;"; break;
				case '"': result += "&quot;"; break;
				case '\'': result += "&#39;"; break;
				default: result += character; break;
			}
		}
		return result;
	}

	std::string GMPanelModel::ScalarText(const Json& value)
	{
		if (value.is_string())
			return value.get<std::string>();
		if (value.is_boolean())
			return value.get<bool>() ? "true" : "false";
		if (value.is_number())
			return value.dump();
		return {};
	}
}
