#include "Hepch.h"

#include "Runtime/Scripting/Binding/GMBinding.hpp"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/DevTools/GM/GMService.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace py = pybind11;

namespace HEngine
{
	namespace
	{
		Json PyToJson(const py::handle& value)
		{
			if (value.is_none())
				return nullptr;
			if (py::isinstance<py::bool_>(value))
				return value.cast<bool>();
			if (py::isinstance<py::int_>(value))
			{
				try
				{
					return value.cast<std::int64_t>();
				}
				catch (const py::cast_error&)
				{
					throw std::invalid_argument("GM Python 整数超出有符号 64 位范围。");
				}
			}
			if (py::isinstance<py::float_>(value))
			{
				const double number = value.cast<double>();
				if (!std::isfinite(number))
					throw std::invalid_argument("GM Python 数值必须是有限值。");
				return number;
			}
			if (py::isinstance<py::str>(value))
				return value.cast<std::string>();
			if (py::isinstance<py::dict>(value))
			{
				Json object = Json::object();
				for (const auto& [key, item] : value.cast<py::dict>())
				{
					if (!py::isinstance<py::str>(key))
						throw std::invalid_argument("GM Python 字典的键必须是字符串。");
					object[key.cast<std::string>()] = PyToJson(item);
				}
				return object;
			}
			if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value))
			{
				Json array = Json::array();
				for (const py::handle item : value)
					array.push_back(PyToJson(item));
				return array;
			}
			throw std::invalid_argument("GM Python 值必须兼容 JSON。");
		}

		py::object JsonToPy(const Json& value)
		{
			if (value.is_null())
				return py::none();
			if (value.is_boolean())
				return py::bool_(value.get<bool>());
			if (value.is_number_unsigned())
				return py::int_(value.get<std::uint64_t>());
			if (value.is_number_integer())
				return py::int_(value.get<std::int64_t>());
			if (value.is_number_float())
				return py::float_(value.get<double>());
			if (value.is_string())
				return py::str(value.get_ref<const std::string&>());
			if (value.is_array())
			{
				py::list result;
				for (const Json& item : value)
					result.append(JsonToPy(item));
				return std::move(result);
			}
			py::dict result;
			for (const auto& [key, item] : value.items())
				result[py::str(key)] = JsonToPy(item);
			return std::move(result);
		}

		std::string GetString(
			const Json& object,
			std::string_view name,
			bool bRequired = true)
		{
			if (!object.contains(name))
			{
				if (!bRequired)
					return {};
				throw std::invalid_argument("GM 描述缺少字符串字段：" + std::string(name));
			}
			if (!object[name].is_string())
				throw std::invalid_argument("GM 描述字段必须是字符串：" + std::string(name));
			return object[name].get<std::string>();
		}

		DevCommandArgument ParseArgument(const Json& value)
		{
			if (!value.is_object())
				throw std::invalid_argument("GM 命令参数必须是描述对象。");
			DevCommandArgument argument;
			argument.Name = GetString(value, "name");
			argument.DisplayName = value.value("displayName", argument.Name);
			argument.Summary = value.value("summary", "");
			const std::optional<DevCommandArgumentType> type =
				ParseDevCommandArgumentType(GetString(value, "type"));
			if (!type)
				throw std::invalid_argument("GM 命令参数类型不受支持：" + argument.Name);
			argument.Type = *type;
			argument.bRequired = value.value("required", false);
			if (value.contains("default"))
				argument.DefaultValue = value["default"];
			if (value.contains("minimum"))
			{
				if (!value["minimum"].is_number())
					throw std::invalid_argument("GM 参数 minimum 必须是数值：" + argument.Name);
				argument.Minimum = value["minimum"].get<double>();
			}
			if (value.contains("maximum"))
			{
				if (!value["maximum"].is_number())
					throw std::invalid_argument("GM 参数 maximum 必须是数值：" + argument.Name);
				argument.Maximum = value["maximum"].get<double>();
			}
			argument.MaxLength = value.value("maxLength", static_cast<std::size_t>(1024));
			if (value.contains("choices"))
			{
				if (!value["choices"].is_array())
					throw std::invalid_argument("GM 参数 choices 必须是数组：" + argument.Name);
				for (const Json& choice : value["choices"])
				{
					if (!choice.is_string())
						throw std::invalid_argument("GM 参数 choices 必须包含字符串：" + argument.Name);
					argument.Choices.push_back(choice.get<std::string>());
				}
			}
			if (value.contains("choiceLabels"))
			{
				if (!value["choiceLabels"].is_array())
					throw std::invalid_argument("GM 参数 choiceLabels 必须是数组：" + argument.Name);
				for (const Json& label : value["choiceLabels"])
				{
					if (!label.is_string())
						throw std::invalid_argument("GM 参数 choiceLabels 必须包含字符串：" + argument.Name);
					argument.ChoiceLabels.push_back(label.get<std::string>());
				}
			}
			return argument;
		}

		DevCommandDescription ParseCommandDescription(std::string owner, const py::dict& descriptor)
		{
			const Json value = PyToJson(descriptor);
			DevCommandDescription description;
			description.Name = GetString(value, "name");
			description.DisplayName = value.value("displayName", description.Name);
			description.Summary = value.value("summary", "");
			description.Category = value.value("category", "项目");
			description.Scope = value.value("scope", "current-session");
			description.Owner = std::move(owner);
		const std::optional<DevCommandRisk> risk = ParseDevCommandRisk(value.value("risk", "safe"));
			if (!risk)
				throw std::invalid_argument("GM 命令风险级别不受支持：" + description.Name);
			description.Risk = *risk;
			description.bTaintsCurrentSession = value.value("taintsCurrentSession", false);
			if (value.contains("arguments"))
			{
				if (!value["arguments"].is_array())
					throw std::invalid_argument("GM 命令 arguments 必须是数组。");
				for (const Json& argument : value["arguments"])
					description.Arguments.push_back(ParseArgument(argument));
			}
			if (value.contains("stateDomains"))
			{
				if (!value["stateDomains"].is_array())
					throw std::invalid_argument("GM 命令 stateDomains 必须是数组。");
				for (const Json& domain : value["stateDomains"])
				{
					if (!domain.is_string())
						throw std::invalid_argument("GM 命令 stateDomains 必须包含字符串。");
					description.StateDomains.push_back(domain.get<std::string>());
				}
			}
			return description;
		}

		DevCommandResult ParsePythonResult(const py::handle& value)
		{
			if (value.is_none())
				return DevCommandResult::Success();
			Json result = PyToJson(value);
			if (!result.is_object() || !result.contains("success"))
				return DevCommandResult::Success(std::move(result));
			if (!result["success"].is_boolean())
				throw std::invalid_argument("GM Python 结果的 success 字段必须是布尔值。");
			const bool bSuccess = result["success"].get<bool>();
			const std::string message = result.value(
				"message",
				bSuccess ? "命令执行完成。" : "命令执行失败。");
			Json data = result.contains("data") ? result["data"] : Json::object();
			return bSuccess
				? DevCommandResult::Success(std::move(data), message)
				: DevCommandResult::Failure(result.value("code", "handler_failed"), message, std::move(data));
		}

		GMStateDomainAvailability ParseAvailability(const py::handle& value)
		{
			if (py::isinstance<py::bool_>(value))
				return GMStateDomainAvailability{ .bAvailable = value.cast<bool>() };
			const Json result = PyToJson(value);
			if (!result.is_object())
				throw std::invalid_argument("GM 状态可用性回调必须返回 bool 或 dict。");
			return GMStateDomainAvailability{
				.bAvailable = result.value("available", true),
				.Reason = result.value("reason", "")
			};
		}

		py::function OptionalFunction(const py::object& value, std::string_view name)
		{
			if (value.is_none())
				return {};
			if (!PyCallable_Check(value.ptr()))
				throw std::invalid_argument("GM 状态回调必须可调用：" + std::string(name));
			return value.cast<py::function>();
		}

		void LoadProjectCommands(bool bReload)
		{
			const std::filesystem::path packagePath = ConfigManager::GetProjectFullPath(
				"Scripts/DevOnly/GM/__init__.py");
			if (!std::filesystem::is_regular_file(packagePath))
				return;

			py::gil_scoped_acquire acquire;
			try
			{
				py::module_ package = py::module_::import("Scripts.DevOnly.GM");
				const char* functionName = bReload && py::hasattr(package, "reload_commands")
					? "reload_commands"
					: "register";
				if (!py::hasattr(package, functionName))
					throw std::runtime_error("Project GM 包必须定义 register()。");
				package.attr(functionName)();
				HE_CORE_INFO(bReload
					? "Project GM commands reloaded."
					: "Project GM commands registered.");
			}
			catch (py::error_already_set& error)
			{
				HE_CORE_ERROR("Project GM command registration failed: {0}", error.what());
				error.restore();
				PyErr_Clear();
			}
			catch (const std::exception& error)
			{
				HE_CORE_ERROR("Project GM command registration failed: {0}", error.what());
			}
		}
	}

	void GMBinding(py::module_& engineModule)
	{
		py::module_ gm = engineModule.def_submodule("GM");
		gm.def("RegisterCommand", [](const std::string& owner, const py::dict& descriptor, py::function handler)
		{
			DevCommandDescription description = ParseCommandDescription(owner, descriptor);
			const std::string commandName = description.Name;
			const bool bRegistered = GMService::GetInstance().GetCommandRegistry().Register(
				std::move(description),
				[handler = std::move(handler)](DevCommandExecutionContext& context) mutable
				{
					py::gil_scoped_acquire acquire;
					py::dict staged;
					for (const auto& [name, value] : context.StagedDomains)
						staged[py::str(name)] = JsonToPy(value);
					py::object result = handler(JsonToPy(context.Arguments), staged);
					const Json stagedJson = PyToJson(staged);
					for (auto& [name, value] : context.StagedDomains)
					{
						if (!stagedJson.contains(name))
							throw std::invalid_argument("GM Python Handler 移除了暂存状态域：" + name);
						value = stagedJson[name];
					}
					return ParsePythonResult(result);
				});
			if (!bRegistered)
				throw std::invalid_argument("GM 命令注册被拒绝：" + commandName);
			GMService::GetInstance().MarkPythonOwner(owner);
			return true;
		}, py::arg("owner"), py::arg("descriptor"), py::arg("handler"));

		gm.def(
			"RegisterStateDomain",
			[](const std::string& owner,
				const py::dict& descriptor,
				py::function snapshot,
				py::function commit,
				py::object availability,
				py::object restore,
				py::object refresh,
				py::object markTainted)
			{
				const Json value = PyToJson(descriptor);
				GMStateDomainDescription description;
				description.Name = GetString(value, "name");
				description.DisplayName = value.value("displayName", description.Name);
				description.Summary = value.value("summary", "");
				description.Owner = owner;
				description.bPersistent = value.value("persistent", false);
				const std::string domainName = description.Name;

				py::function availabilityFunction = OptionalFunction(availability, "availability");
				py::function restoreFunction = OptionalFunction(restore, "restore");
				py::function refreshFunction = OptionalFunction(refresh, "refresh");
				py::function taintFunction = OptionalFunction(markTainted, "mark_tainted");
				GMStateDomainCallbacks callbacks;
				if (availabilityFunction)
				{
					callbacks.Availability = [function = std::move(availabilityFunction)]() mutable
					{
						py::gil_scoped_acquire acquire;
						return ParseAvailability(function());
					};
				}
				callbacks.Snapshot = [function = std::move(snapshot)]() mutable
				{
					py::gil_scoped_acquire acquire;
					return PyToJson(function());
				};
				callbacks.Commit = [function = std::move(commit)](const Json& state) mutable
				{
					py::gil_scoped_acquire acquire;
					return ParsePythonResult(function(JsonToPy(state)));
				};
				if (restoreFunction)
				{
					callbacks.Restore = [function = std::move(restoreFunction)](const Json& state) mutable
					{
						py::gil_scoped_acquire acquire;
						return ParsePythonResult(function(JsonToPy(state)));
					};
				}
				if (refreshFunction)
				{
					callbacks.Refresh = [function = std::move(refreshFunction)]() mutable
					{
						py::gil_scoped_acquire acquire;
						function();
					};
				}
				if (taintFunction)
				{
					callbacks.MarkTainted = [function = std::move(taintFunction)]() mutable
					{
						py::gil_scoped_acquire acquire;
						function();
					};
				}

				const bool bRegistered = GMService::GetInstance().GetStateDomainRegistry().Register(
					std::move(description),
					std::move(callbacks));
				if (!bRegistered)
					throw std::invalid_argument("GM 状态域注册被拒绝：" + domainName);
				GMService::GetInstance().MarkPythonOwner(owner);
				return true;
			},
			py::arg("owner"),
			py::arg("descriptor"),
			py::arg("snapshot"),
			py::arg("commit"),
			py::arg("availability") = py::none(),
			py::arg("restore") = py::none(),
			py::arg("refresh") = py::none(),
			py::arg("mark_tainted") = py::none());

		gm.def("UnregisterOwner", [](const std::string& owner)
		{
			return GMService::GetInstance().UnregisterOwner(owner);
		});
		gm.def("Discover", []()
		{
			return JsonToPy(GMService::GetInstance().Discover());
		});
		gm.def(
			"Execute",
			[](const std::string& command, const py::dict& arguments, bool bConfirm)
			{
				return JsonToPy(GMService::GetInstance().Execute(
					command,
					PyToJson(arguments),
					bConfirm,
					"python").ToJson());
			},
			py::arg("command"),
			py::arg("arguments") = py::dict(),
			py::arg("confirm") = false);
		gm.def("ReloadProjectCommands", &ReloadProjectGMCommands);
		gm.def("IsCurrentSessionTainted", []()
		{
			return GMService::GetInstance().GetExecutor().IsCurrentSessionTainted();
		});
	}

	void InitializeProjectGMCommands()
	{
		LoadProjectCommands(false);
	}

	void ReloadProjectGMCommands()
	{
		LoadProjectCommands(true);
	}

	void ShutdownProjectGMCommands()
	{
		if (Py_IsInitialized() == 0)
			return;
		py::gil_scoped_acquire acquire;
		GMService::GetInstance().ShutdownPythonOwners();
	}
}
