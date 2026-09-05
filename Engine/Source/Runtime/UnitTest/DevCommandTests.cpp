#include "Hepch.h"

#include "Runtime/UnitTest/DevCommandTests.h"
#include "Runtime/DevTools/Commands/DevCommandExecutor.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
	namespace
	{
		void Require(bool condition, std::string_view message)
		{
			if (!condition)
				throw std::runtime_error(std::string(message));
		}
	}

	void DevCommandPlatformContract()
	{
		DevCommandRegistry commands;
		GMStateDomainRegistry domains;
		DevCommandExecutor executor(commands, domains, 3);

		DevCommandArgument localizedChoice;
		localizedChoice.Name = "category";
		localizedChoice.DisplayName = "类别";
		localizedChoice.Type = DevCommandArgumentType::Enumeration;
		localizedChoice.Choices = { "characters", "weapons", "trinkets" };
		localizedChoice.ChoiceLabels = { "角色", "武器", "奇物" };
		const Json localizedSchema = ToJson(localizedChoice);
		Require(
			localizedSchema["choices"][0] == "characters" &&
			localizedSchema["choiceLabels"][0] == "角色",
			"Localized enum labels changed their stable values or were not discovered.");

		DevCommandDescription invalidLocalizedCommand;
		invalidLocalizedCommand.Name = "test.invalid_labels";
		invalidLocalizedCommand.DisplayName = "Invalid Labels";
		invalidLocalizedCommand.Owner = "test.owner";
		localizedChoice.ChoiceLabels.pop_back();
		invalidLocalizedCommand.Arguments.push_back(std::move(localizedChoice));
		Require(!commands.Register(
			std::move(invalidLocalizedCommand),
			[](DevCommandExecutionContext& context)
			{
				(void)context;
				return DevCommandResult::Success();
			}),
			"A mismatched localized enum label list was accepted.");

		Json persisted = Json{ { "value", 0 } };
		GMStateDomainDescription domainDescription;
		domainDescription.Name = "test.profile";
		domainDescription.DisplayName = "Test Profile";
		domainDescription.Owner = "test.owner";
		domainDescription.bPersistent = true;
		GMStateDomainCallbacks callbacks;
		callbacks.Snapshot = [&persisted]() { return persisted; };
		callbacks.Commit = [&persisted](const Json& staged)
		{
			if (staged.value("value", 0) == 13)
				return DevCommandResult::Failure("fixture_rejected", "Fixture rejected value 13.");
			persisted = staged;
			return DevCommandResult::Success();
		};
		callbacks.Restore = [&persisted](const Json& original)
		{
			persisted = original;
			return DevCommandResult::Success();
		};
		Require(domains.Register(std::move(domainDescription), std::move(callbacks)),
			"A valid GM state domain was rejected.");

		DevCommandDescription description;
		description.Name = "test.profile.set";
		description.DisplayName = "Set Test Profile";
		description.Category = "Tests";
		description.Scope = "persistent-profile";
		description.Owner = "test.owner";
		description.Risk = DevCommandRisk::Confirm;
		description.StateDomains = { "test.profile" };
		description.bTaintsCurrentSession = true;
		DevCommandArgument amount;
		amount.Name = "amount";
		amount.DisplayName = "Amount";
		amount.Type = DevCommandArgumentType::Integer;
		amount.bRequired = true;
		amount.Minimum = 0.0;
		amount.Maximum = 100.0;
		description.Arguments.push_back(std::move(amount));
		Require(commands.Register(
			std::move(description),
			[](DevCommandExecutionContext& context)
			{
				Json& profile = context.RequireStateDomain("test.profile");
				profile["value"] = context.Arguments["amount"];
				return DevCommandResult::Success(Json{ { "value", profile["value"] } });
			}),
			"A valid typed GM command was rejected.");

		DevCommandResult confirmation = executor.Execute(
			"test.profile.set", Json{ { "amount", 5 } }, false, "unit-test");
		Require(!confirmation.bSuccess && confirmation.Code == "confirmation_required" &&
			persisted["value"] == 0 && executor.GetHistory().empty(),
			"Risk confirmation changed state or polluted command history.");

		DevCommandResult invalid = executor.Execute(
			"test.profile.set", Json{ { "amount", 101 } }, true, "unit-test");
		Require(!invalid.bSuccess && invalid.Code == "invalid_arguments" && persisted["value"] == 0,
			"Typed range validation accepted an out-of-range value.");

		DevCommandResult success = executor.Execute(
			"test.profile.set", Json{ { "amount", 5 } }, true, "unit-test");
		Require(success.bSuccess && success.bCommitted && success.bTainted &&
			persisted["value"] == 5 && executor.IsCurrentSessionTainted(),
			"A valid transactional command did not commit and taint its session.");

		DevCommandResult rejectedCommit = executor.Execute(
			"test.profile.set", Json{ { "amount", 13 } }, true, "unit-test");
		Require(!rejectedCommit.bSuccess && rejectedCommit.Code == "transaction_commit_failed" &&
			persisted["value"] == 5,
			"A failed state-domain commit was not rolled back to its original snapshot.");
		Require(executor.GetHistory().size() == 3,
			"Bounded command history did not retain the configured number of receipts.");

		Require(commands.UnregisterOwner("test.owner") == 1,
			"Command Owner cleanup removed the wrong number of handlers.");
		Require(domains.UnregisterOwner("test.owner") == 1,
			"State-domain Owner cleanup removed the wrong number of callbacks.");
		Require(commands.Find("test.profile.set") == nullptr && domains.Find("test.profile") == nullptr,
			"Owner cleanup left a hot-reload-unsafe registration behind.");
	}
}
