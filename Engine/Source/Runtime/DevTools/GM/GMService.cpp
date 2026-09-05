#include "Hepch.h"

#include "Runtime/DevTools/GM/GMService.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace HEngine
{
	GMService::GMService()
		: mExecutor(mCommands, mStateDomains, 100)
	{
		DevCommandDescription status;
		status.Name = "engine.gm.status";
		status.DisplayName = "GM 状态";
		status.Summary = "查看当前 GM 污染标记和有界历史记录数量。";
		status.Category = "引擎";
		status.Scope = "read-only";
		status.Owner = "engine.gm";
		const bool bRegistered = mCommands.Register(
			std::move(status),
			[this](DevCommandExecutionContext& context)
			{
				(void)context;
				return DevCommandResult::Success(Json{
					{ "tainted", mExecutor.IsCurrentSessionTainted() },
					{ "historySize", mExecutor.GetHistory().size() }
				});
			});
		if (!bRegistered)
			throw std::logic_error("无法注册 engine.gm.status。");
	}

	DevCommandResult GMService::Execute(
		std::string_view commandName,
		const Json& arguments,
		bool bConfirmed,
		std::string_view source)
	{
		return mExecutor.Execute(commandName, arguments, bConfirmed, source);
	}

	Json GMService::Discover() const
	{
		Json commands = Json::array();
		for (const DevCommandDescription& description : mCommands.GetDescriptions())
		{
			Json command = ToJson(description);
			bool bAvailable = true;
			std::string reason;
			for (const std::string& domainName : description.StateDomains)
			{
				const GMStateDomain* domain = mStateDomains.Find(domainName);
				if (!domain)
				{
					bAvailable = false;
					reason = "所需状态域尚未注册：" + domainName;
					break;
				}
				try
				{
					if (domain->Callbacks.Availability)
					{
						const GMStateDomainAvailability availability = domain->Callbacks.Availability();
						if (!availability.bAvailable)
						{
							bAvailable = false;
							reason = availability.Reason;
							break;
						}
					}
				}
				catch (...)
				{
					bAvailable = false;
					reason = "状态域可用性检查失败：" + domainName;
					break;
				}
			}
			command["available"] = bAvailable;
			command["availabilityReason"] = reason;
			commands.push_back(std::move(command));
		}

		Json domains = Json::array();
		for (const GMStateDomainDescription& description : mStateDomains.GetDescriptions())
		{
			domains.push_back(Json{
				{ "name", description.Name },
				{ "displayName", description.DisplayName },
				{ "summary", description.Summary },
				{ "owner", description.Owner },
				{ "persistent", description.bPersistent }
			});
		}

		return Json{
			{ "commands", std::move(commands) },
			{ "stateDomains", std::move(domains) },
			{ "commandRevision", mCommands.GetRevision() },
			{ "stateDomainRevision", mStateDomains.GetRevision() },
			{ "sessionTainted", mExecutor.IsCurrentSessionTainted() },
			{ "history", GetHistoryJson(25) }
		};
	}

	Json GMService::GetHistoryJson(std::size_t limit) const
	{
		Json history = Json::array();
		const auto& entries = mExecutor.GetHistory();
		limit = std::min(limit, entries.size());
		const std::size_t first = entries.size() - limit;
		for (std::size_t index = first; index < entries.size(); ++index)
		{
			const DevCommandHistoryEntry& entry = entries[index];
			history.push_back(Json{
				{ "receiptId", entry.ReceiptId },
				{ "command", entry.Command },
				{ "source", entry.Source },
				{ "result", entry.Result.ToJson() }
			});
		}
		return history;
	}

	std::size_t GMService::UnregisterOwner(std::string_view owner)
	{
		const std::size_t commandCount = mCommands.UnregisterOwner(owner);
		const std::size_t domainCount = mStateDomains.UnregisterOwner(owner);
		return commandCount + domainCount;
	}

	void GMService::MarkPythonOwner(std::string owner)
	{
		if (DevCommandRegistry::IsValidIdentifier(owner, 128, true))
			mPythonOwners.insert(std::move(owner));
	}

	void GMService::ShutdownPythonOwners()
	{
		for (const std::string& owner : mPythonOwners)
			(void)UnregisterOwner(owner);
		mPythonOwners.clear();
	}
}
