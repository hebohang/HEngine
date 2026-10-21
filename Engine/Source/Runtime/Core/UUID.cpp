#include "Hepch.h"
#include "UUID.h"

namespace HEngine
{
    UUID::UUID()
        : mGuid(xg::newGuid())
    {
    }

    UUID::UUID(const std::array<unsigned char, 16>& bytes) : mGuid(bytes)
    {
    }

    // create a guid from array of bytes
    UUID::UUID(const unsigned char* bytes) : mGuid(bytes)
    {
    }

    UUID::UUID(const std::string& fromString) : mGuid(fromString)
    {
    }

    UUID::UUID(const UUID& other) : mGuid(other.mGuid)
    {
    }

    UUID& UUID::operator=(const UUID& other)
    {
        UUID(other).swap(*this);
        return *this;
    }

    bool UUID::operator==(const UUID& other) const
    {
        return (mGuid == other.mGuid);
    }

    // overload inequality operator
    bool UUID::operator!=(const UUID& other) const
    {
        return (mGuid != other.mGuid);
    }

    std::string UUID::str() const
    {
        return mGuid.str();
    }

    UUID::operator std::string() const
    {
        return mGuid.str();
    }

    void UUID::swap(UUID& other)
    {
        mGuid.swap(other.mGuid);
    }

    bool UUID::isValid() const
    {
        return mGuid.isValid();
    }

    template<>
    void Serializer::Serialize(Json& json, const UUID& obj)
    {
        json = obj.str();
    }
    template<>
    void Serializer::Deserialize(const Json& json, UUID& obj)
    {
        HE_CORE_ASSERT(obj.isValid(), "uuid is not valid!");
        obj = std::string(json);
    }
}

namespace std
{
    template <>
	void swap(HEngine::UUID& lhs, HEngine::UUID& rhs)
    {
        lhs.swap(rhs);
    }
}
