#pragma once
#include "Runtime/Core/BuildFeatures.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <concepts>
#include <memory>
#include <type_traits>

namespace HEngine
{
    class ComponentBase
    {
    public:
        static constexpr BuildFeature RequiredBuildFeature = BuildFeature::None;
        ComponentBase() = default;
        ComponentBase(const ComponentBase&) {}
        ComponentBase& operator=(const ComponentBase& other)
        {
            if (this != &other) mReferenceLifetime.reset();
            return *this;
        }
        ComponentBase(ComponentBase&&) noexcept = default;
        ComponentBase& operator=(ComponentBase&&) noexcept = default;
        virtual ~ComponentBase() {}

        // Allocated only when a guarded external reference is first requested.
        // Moves preserve component identity; copies/replacement create a new identity.
        std::weak_ptr<const unsigned char> ReferenceLifetime() const
        {
            if (!mReferenceLifetime) mReferenceLifetime = std::make_shared<const unsigned char>(0);
            return mReferenceLifetime;
        }
    private:
        mutable std::shared_ptr<const unsigned char> mReferenceLifetime;
    };

    inline void to_json(Json& json, const ComponentBase& obj) {}
    inline void from_json(const Json& json, ComponentBase& obj) {}

    template<class T>
    concept Component = std::is_base_of_v<ComponentBase, T>;

    template<Component... C>
    struct ComponentGroup
    {
        static const int size = sizeof...(C);
    };

    enum { MaxComponentNum = 64 };

    template <int N>
    struct Rank : Rank<N - 1> {};

    template <>
    struct Rank<0> {};

    template <class List, class T>
    struct Append;

    template <class... Ts, class T>
    struct Append<ComponentGroup<Ts...>, T> 
    {
        typedef ComponentGroup<Ts..., T> type;
    };

    inline ComponentGroup<> GetTypes(Rank<0>) { return {}; }

#define _GET_COMPONENT_REGISTERED_TYPES() \
    decltype(GetTypes(Rank<MaxComponentNum>()))

#define REGISTER_COMPONENT_TYPE(Type)                                    \
    inline Append<_GET_COMPONENT_REGISTERED_TYPES(), Type>::type         \
    GetTypes(Rank<_GET_COMPONENT_REGISTERED_TYPES()::size + 1>) {        \
        return {};                                                       \
    }                                                                    

#define REFLECTION_COMPONENT_BODY(Type) REFLECTION_BODY(Type)
}
