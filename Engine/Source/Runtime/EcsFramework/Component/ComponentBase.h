#pragma once
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <concepts>
#include <type_traits>
#include <pybind11/embed.h>

namespace HEngine
{
    class ComponentBase
    {
    public:
        ComponentBase() = default;
        virtual ~ComponentBase() {}
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

    enum { MaxComponentNum = 30 };

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

#define REFLECTION_COMPONENT_BODY(Type)         \
    REFLECTION_BODY(Type)                       \
public:                                         \
    static void PyBinding(pybind11::module_& HEngineModule);
}