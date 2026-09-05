#include "Hepch.h"

#include "Runtime/DevTools/Commands/DevCommandExecutor.h"

#include <exception>
#include <map>
#include <stdexcept>
#include <utility>

namespace HEngine
{
	DevCommandExecutor::DevCommandExecutor(
		DevCommandRegistry& commands,
		GMStateDomainRegistry& stateDomains,
		std::size_t historyLimit)
		: mCommands(commands),
		  mStateDomains(stateDomains),
		  mHistoryLimit(historyLimit)
	{
		if (mHistoryLimit == 0 || mHistoryLimit > 1000)
			throw std::invalid_argument("开发命令历史上限必须在 1..1000 范围内。");
	}

	DevCommandResult DevCommandExecutor::Execute(
		std::string_view commandName,
		const Json& arguments,
		bool bConfirmed,
		std::string_view source)
	{
		const DevCommandDescription* description = mCommands.Find(commandName);
		if (!description)
		{
			return Finish(
				commandName,
				source,
				DevCommandResult::Failure("command_not_found", "请求的命令尚未注册。"));
		}
		if (description->Risk != DevCommandRisk::Safe && !bConfirmed)
		{
			return Finish(
				commandName,
				source,
				DevCommandResult::Failure(
					"confirmation_required",
					"该命令具有风险，需要显式确认。",
					Json{ { "risk", ToString(description->Risk) } }),
				false);
		}

		Json validatedArguments;
		DevCommandResult validation = mCommands.ValidateArguments(
			commandName,
			arguments,
			validatedArguments);
		if (!validation.bSuccess)
			return Finish(commandName, source, std::move(validation));

		std::vector<const GMStateDomain*> domains;
		domains.reserve(description->StateDomains.size());
		for (const std::string& domainName : description->StateDomains)
		{
			const GMStateDomain* domain = mStateDomains.Find(domainName);
			if (!domain)
			{
				return Finish(
					commandName,
					source,
					DevCommandResult::Failure(
						"state_domain_not_found",
						"所需状态域尚未注册：" + domainName));
			}
			try
			{
				if (domain->Callbacks.Availability)
				{
					const GMStateDomainAvailability availability = domain->Callbacks.Availability();
					if (!availability.bAvailable)
					{
						return Finish(
							commandName,
							source,
							DevCommandResult::Failure(
								"state_domain_unavailable",
								availability.Reason.empty()
									? "所需状态域当前不可用：" + domainName
									: availability.Reason));
					}
				}
			}
			catch (...)
			{
				return Finish(
					commandName,
					source,
					DevCommandResult::Failure(
						"state_domain_unavailable",
						"状态域可用性检查失败：" + domainName));
			}
			domains.push_back(domain);
		}

		DevCommandExecutionContext context;
		context.Arguments = std::move(validatedArguments);
		context.Source = std::string(source.substr(0, 128));
		std::map<std::string, Json, std::less<>> originalStates;
		try
		{
			for (const GMStateDomain* domain : domains)
			{
				Json original = domain->Callbacks.Snapshot();
				originalStates.emplace(domain->Description.Name, original);
				context.StagedDomains.emplace(domain->Description.Name, std::move(original));
			}
		}
		catch (...)
		{
			return Finish(
				commandName,
				source,
				DevCommandResult::Failure(
					"state_snapshot_failed",
					"所需状态域无法创建事务快照。"));
		}

		DevCommandResult result = mCommands.ExecuteValidated(commandName, context);
		if (!result.bSuccess)
			return Finish(commandName, source, std::move(result));

		for (const GMStateDomain* domain : domains)
		{
			try
			{
				const auto staged = context.StagedDomains.find(domain->Description.Name);
				if (staged == context.StagedDomains.end())
					throw std::logic_error("命令移除了暂存状态域。");
				DevCommandResult commit = domain->Callbacks.Commit(staged->second);
				if (!commit.bSuccess)
				{
					RestoreDomains(domains, originalStates);
					RefreshDomains(domains);
					return Finish(
						commandName,
						source,
						DevCommandResult::Failure(
							"transaction_commit_failed",
							commit.Message.empty()
								? "状态域提交失败：" + domain->Description.Name
								: commit.Message,
							Json{ { "domain", domain->Description.Name }, { "cause", commit.ToJson() } }));
				}
			}
			catch (...)
			{
				RestoreDomains(domains, originalStates);
				RefreshDomains(domains);
				return Finish(
					commandName,
					source,
					DevCommandResult::Failure(
						"transaction_commit_failed",
						"状态域提交抛出异常：" + domain->Description.Name,
						Json{ { "domain", domain->Description.Name } }));
			}
		}

		result.bCommitted = !domains.empty();
		RefreshDomains(domains);
		if (description->bTaintsCurrentSession)
		{
			mCurrentSessionTainted = true;
			result.bTainted = true;
			for (const GMStateDomain* domain : domains)
			{
				try
				{
					if (domain->Callbacks.MarkTainted)
						domain->Callbacks.MarkTainted();
				}
				catch (...)
				{
					DevCommandResult taintFailure = DevCommandResult::Failure(
						"taint_callback_failed",
						"命令已提交，但状态域污染标记回调失败。",
						Json{ { "domain", domain->Description.Name }, { "commandResult", result.Data } });
					taintFailure.bCommitted = result.bCommitted;
					taintFailure.bTainted = true;
					return Finish(commandName, source, std::move(taintFailure));
				}
			}
		}
		result.bTainted = result.bTainted || mCurrentSessionTainted;
		return Finish(commandName, source, std::move(result));
	}

	DevCommandResult DevCommandExecutor::Finish(
		std::string_view commandName,
		std::string_view source,
		DevCommandResult result,
		bool bRecordHistory)
	{
		result.ReceiptId = mNextReceiptId++;
		result.bTainted = result.bTainted || mCurrentSessionTainted;
		if (!bRecordHistory)
			return result;

		mHistory.push_back(DevCommandHistoryEntry{
			.ReceiptId = result.ReceiptId,
			.Command = std::string(commandName.substr(0, 128)),
			.Source = std::string(source.substr(0, 128)),
			.Result = result
		});
		while (mHistory.size() > mHistoryLimit)
			mHistory.pop_front();
		return result;
	}

	void DevCommandExecutor::RefreshDomains(
		const std::vector<const GMStateDomain*>& domains) const noexcept
	{
		for (const GMStateDomain* domain : domains)
		{
			try
			{
				if (domain->Callbacks.Refresh)
					domain->Callbacks.Refresh();
			}
			catch (...)
			{
			}
		}
	}

	void DevCommandExecutor::RestoreDomains(
		const std::vector<const GMStateDomain*>& domains,
		const std::map<std::string, Json, std::less<>>& originalStates) const noexcept
	{
		for (auto domain = domains.rbegin(); domain != domains.rend(); ++domain)
		{
			try
			{
				const auto original = originalStates.find((*domain)->Description.Name);
				if (original != originalStates.end() && (*domain)->Callbacks.Restore)
					(void)(*domain)->Callbacks.Restore(original->second);
			}
			catch (...)
			{
			}
		}
	}
}
