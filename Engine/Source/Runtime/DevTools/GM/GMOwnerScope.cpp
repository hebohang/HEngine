#include "Hepch.h"

#include "Runtime/DevTools/GM/GMOwnerScope.h"
#include "Runtime/DevTools/GM/GMService.h"

#include <utility>

namespace HEngine
{
	GMOwnerScope::GMOwnerScope(std::string owner)
		: mOwner(std::move(owner))
	{
	}

	GMOwnerScope::~GMOwnerScope()
	{
		Release();
	}

	GMOwnerScope::GMOwnerScope(GMOwnerScope&& other) noexcept
		: mOwner(std::move(other.mOwner))
	{
		other.mOwner.clear();
	}

	GMOwnerScope& GMOwnerScope::operator=(GMOwnerScope&& other) noexcept
	{
		if (this != &other)
		{
			Release();
			mOwner = std::move(other.mOwner);
			other.mOwner.clear();
		}
		return *this;
	}

	void GMOwnerScope::Release()
	{
		if (mOwner.empty())
			return;
		(void)GMService::GetInstance().UnregisterOwner(mOwner);
		mOwner.clear();
	}
}
