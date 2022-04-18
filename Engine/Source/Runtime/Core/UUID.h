#pragma once

#include <xhash>

namespace HEngine
{
	class UUID
	{
	public:
		UUID();
		UUID(uint64_t uuid);
		UUID(const UUID& other); 

		operator uint64_t() { return mUUID; }
		operator const uint64_t() const { return mUUID; }
	private:
		uint64_t mUUID;
	};
}

namespace std
{
	template<>
	struct hash<HEngine::UUID>
	{
		std::size_t operator()(const HEngine::UUID& uuid) const
		{
			return hash<uint64_t>()((uint64_t)uuid);
		}
	};
}
