#pragma once

#include <string>

namespace HEngine
{
	class GMService;

	class GMOwnerScope final
	{
	public:
		explicit GMOwnerScope(std::string owner);
		~GMOwnerScope();
		GMOwnerScope(const GMOwnerScope&) = delete;
		GMOwnerScope& operator=(const GMOwnerScope&) = delete;
		GMOwnerScope(GMOwnerScope&& other) noexcept;
		GMOwnerScope& operator=(GMOwnerScope&& other) noexcept;

		[[nodiscard]] const std::string& GetOwner() const { return mOwner; }
		void Release();

	private:
		std::string mOwner;
	};
}
