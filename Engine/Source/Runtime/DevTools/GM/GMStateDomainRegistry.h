#pragma once

#include "Runtime/DevTools/Commands/DevCommandResult.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
	struct GMStateDomainAvailability
	{
		bool bAvailable = true;
		std::string Reason;
	};

	struct GMStateDomainDescription
	{
		std::string Name;
		std::string DisplayName;
		std::string Summary;
		std::string Owner;
		bool bPersistent = false;
	};

	struct GMStateDomainCallbacks
	{
		std::function<GMStateDomainAvailability()> Availability;
		std::function<Json()> Snapshot;
		std::function<DevCommandResult(const Json& stagedState)> Commit;
		std::function<DevCommandResult(const Json& originalState)> Restore;
		std::function<void()> Refresh;
		std::function<void()> MarkTainted;
	};

	struct GMStateDomain
	{
		GMStateDomainDescription Description;
		GMStateDomainCallbacks Callbacks;
	};

	class GMStateDomainRegistry final
	{
	public:
		[[nodiscard]] bool Register(
			GMStateDomainDescription description,
			GMStateDomainCallbacks callbacks);
		[[nodiscard]] bool Unregister(std::string_view name);
		[[nodiscard]] std::size_t UnregisterOwner(std::string_view owner);
		[[nodiscard]] const GMStateDomain* Find(std::string_view name) const;
		[[nodiscard]] std::vector<GMStateDomainDescription> GetDescriptions() const;
		[[nodiscard]] std::uint64_t GetRevision() const { return mRevision; }

	private:
		std::map<std::string, GMStateDomain, std::less<>> mDomains;
		std::uint64_t mRevision = 0;
	};
}
