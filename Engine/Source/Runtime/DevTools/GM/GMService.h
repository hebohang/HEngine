#pragma once

#include "Runtime/Core/Templates/PublicSingleton.h"
#include "Runtime/DevTools/Commands/DevCommandExecutor.h"

#include <set>
#include <string>
#include <string_view>

namespace HEngine
{
	class GMService final : public PublicSingleton<GMService>
	{
	public:
		GMService();

		[[nodiscard]] DevCommandRegistry& GetCommandRegistry() { return mCommands; }
		[[nodiscard]] const DevCommandRegistry& GetCommandRegistry() const { return mCommands; }
		[[nodiscard]] GMStateDomainRegistry& GetStateDomainRegistry() { return mStateDomains; }
		[[nodiscard]] const GMStateDomainRegistry& GetStateDomainRegistry() const { return mStateDomains; }
		[[nodiscard]] DevCommandExecutor& GetExecutor() { return mExecutor; }
		[[nodiscard]] const DevCommandExecutor& GetExecutor() const { return mExecutor; }

		[[nodiscard]] DevCommandResult Execute(
			std::string_view commandName,
			const Json& arguments,
			bool bConfirmed,
			std::string_view source);
		[[nodiscard]] Json Discover() const;
		[[nodiscard]] Json GetHistoryJson(std::size_t limit = 25) const;
		[[nodiscard]] std::size_t UnregisterOwner(std::string_view owner);
		void MarkPythonOwner(std::string owner);
		void ShutdownPythonOwners();

	private:
		DevCommandRegistry mCommands;
		GMStateDomainRegistry mStateDomains;
		DevCommandExecutor mExecutor;
		std::set<std::string, std::less<>> mPythonOwners;
	};
}
