#include "Hepch.h"

#include "Runtime/DevTools/GM/GMStateDomainRegistry.h"
#include "Runtime/DevTools/Commands/DevCommandRegistry.h"

#include <utility>

namespace HEngine
{
	bool GMStateDomainRegistry::Register(
		GMStateDomainDescription description,
		GMStateDomainCallbacks callbacks)
	{
		if (!DevCommandRegistry::IsValidIdentifier(description.Name, 128, true) ||
			!DevCommandRegistry::IsValidIdentifier(description.Owner, 128, true) ||
			description.DisplayName.size() > 128 || description.Summary.size() > 1024 ||
			!callbacks.Snapshot || !callbacks.Commit)
		{
			return false;
		}

		const std::string domainName = description.Name;
		auto [iterator, bInserted] = mDomains.emplace(
			domainName,
			GMStateDomain{ std::move(description), std::move(callbacks) });
		(void)iterator;
		if (bInserted)
			++mRevision;
		return bInserted;
	}

	bool GMStateDomainRegistry::Unregister(std::string_view name)
	{
		const auto domain = mDomains.find(name);
		if (domain == mDomains.end())
			return false;
		mDomains.erase(domain);
		++mRevision;
		return true;
	}

	std::size_t GMStateDomainRegistry::UnregisterOwner(std::string_view owner)
	{
		std::size_t removed = 0;
		for (auto domain = mDomains.begin(); domain != mDomains.end();)
		{
			if (domain->second.Description.Owner == owner)
			{
				domain = mDomains.erase(domain);
				++removed;
			}
			else
			{
				++domain;
			}
		}
		if (removed > 0)
			++mRevision;
		return removed;
	}

	const GMStateDomain* GMStateDomainRegistry::Find(std::string_view name) const
	{
		const auto domain = mDomains.find(name);
		return domain == mDomains.end() ? nullptr : &domain->second;
	}

	std::vector<GMStateDomainDescription> GMStateDomainRegistry::GetDescriptions() const
	{
		std::vector<GMStateDomainDescription> descriptions;
		descriptions.reserve(mDomains.size());
		for (const auto& [name, domain] : mDomains)
		{
			(void)name;
			descriptions.push_back(domain.Description);
		}
		return descriptions;
	}
}
