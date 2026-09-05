#pragma once

#include "Runtime/DevTools/Commands/DevCommandTypes.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
	class DevCommandRegistry final
	{
	public:
		using Handler = std::function<DevCommandResult(DevCommandExecutionContext& context)>;

		[[nodiscard]] bool Register(DevCommandDescription description, Handler handler);
		[[nodiscard]] bool Unregister(std::string_view name);
		[[nodiscard]] std::size_t UnregisterOwner(std::string_view owner);
		[[nodiscard]] const DevCommandDescription* Find(std::string_view name) const;
		[[nodiscard]] std::vector<DevCommandDescription> GetDescriptions() const;
		[[nodiscard]] DevCommandResult ValidateArguments(
			std::string_view name,
			const Json& arguments,
			Json& outValidatedArguments) const;
		[[nodiscard]] DevCommandResult ExecuteValidated(
			std::string_view name,
			DevCommandExecutionContext& context) const;
		[[nodiscard]] std::uint64_t GetRevision() const { return mRevision; }

		[[nodiscard]] static bool IsValidIdentifier(
			std::string_view value,
			std::size_t maxLength,
			bool bAllowDot);

	private:
		struct Command
		{
			DevCommandDescription Description;
			Handler Invoke;
		};

		[[nodiscard]] static DevCommandResult ValidateValue(
			const DevCommandArgument& schema,
			const Json& value);
		[[nodiscard]] static bool ValidateDescription(const DevCommandDescription& description);

		std::map<std::string, Command, std::less<>> mCommands;
		std::uint64_t mRevision = 0;
	};
}
