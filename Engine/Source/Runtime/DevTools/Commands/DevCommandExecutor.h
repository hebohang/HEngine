#pragma once

#include "Runtime/DevTools/Commands/DevCommandRegistry.h"
#include "Runtime/DevTools/GM/GMStateDomainRegistry.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
	struct DevCommandHistoryEntry
	{
		std::uint64_t ReceiptId = 0;
		std::string Command;
		std::string Source;
		DevCommandResult Result;
	};

	class DevCommandExecutor final
	{
	public:
		DevCommandExecutor(
			DevCommandRegistry& commands,
			GMStateDomainRegistry& stateDomains,
			std::size_t historyLimit = 100);

		[[nodiscard]] DevCommandResult Execute(
			std::string_view commandName,
			const Json& arguments,
			bool bConfirmed,
			std::string_view source);
		[[nodiscard]] const std::deque<DevCommandHistoryEntry>& GetHistory() const
		{
			return mHistory;
		}
		[[nodiscard]] bool IsCurrentSessionTainted() const { return mCurrentSessionTainted; }

	private:
		[[nodiscard]] DevCommandResult Finish(
			std::string_view commandName,
			std::string_view source,
			DevCommandResult result,
			bool bRecordHistory = true);
		void RefreshDomains(const std::vector<const GMStateDomain*>& domains) const noexcept;
		void RestoreDomains(
			const std::vector<const GMStateDomain*>& domains,
			const std::map<std::string, Json, std::less<>>& originalStates) const noexcept;

		DevCommandRegistry& mCommands;
		GMStateDomainRegistry& mStateDomains;
		std::size_t mHistoryLimit = 100;
		std::uint64_t mNextReceiptId = 1;
		bool mCurrentSessionTainted = false;
		std::deque<DevCommandHistoryEntry> mHistory;
	};
}
