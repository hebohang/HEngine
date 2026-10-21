#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include <crossguid/Guid.hpp>
#include <functional>

namespace HEngine
{
	class UUID
	{
	public:
		UUID(const std::array<unsigned char, 16>& bytes);
		UUID(const unsigned char* bytes);
		UUID(const std::string& fromString);
		UUID();
		UUID(const UUID& other);
		UUID& operator=(const UUID& other);
		bool operator==(const UUID& other) const;
		bool operator!=(const UUID& other) const;

		std::string str() const;
		operator std::string() const;
		void swap(UUID& other);
		bool isValid() const;
	private:
		xg::Guid mGuid;

		friend std::ostream& operator<<(std::ostream& s, const UUID& guid);
	};

	template<>
	void Serializer::Serialize(Json& json, const UUID& obj);
	template<>
	void Serializer::Deserialize(const Json& json, UUID& obj);
}

namespace std
{
	template <>
	void swap(HEngine::UUID& uuid0, HEngine::UUID& uuid1);

	template <>
	struct hash<HEngine::UUID>
	{
		typedef HEngine::UUID argument_type;
		typedef std::size_t result_type;

		result_type operator()(argument_type const& uuid) const
		{
			std::hash<std::string> hasher;
			return static_cast<result_type>(hasher(uuid.str()));
		}
	};
}
