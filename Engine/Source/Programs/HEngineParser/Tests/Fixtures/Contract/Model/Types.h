#pragma once

#include "A/Foo.h"
#include "B/Foo.h"
#include "Common.h"

namespace Model
{
    class HE_TEST_REFLECT(Fields) LocalValue
    {
    public:
        int m_local = 0;
    };

    class HE_TEST_REFLECT(Fields) Left
    {
    public:
        int m_left = 0;
    };

    class HE_TEST_REFLECT(Fields) Right
    {
    public:
        int m_right = 0;
    };

    class HE_TEST_REFLECT(Fields) Child : public Left, public Right
    {
    public:
        Alpha::Foo m_foo;
        std::vector<Beta::Foo> m_items;
        LocalValue m_local;
        std::vector<LocalValue, std::Alloc<LocalValue>> m_locals;
        HE_TEST_PROPERTY(serializedName : "display:key",
                         default : "a,b:c",
                         QualifiedType : Alpha::Foo,
                         Range : [0,1],
                         FactoryValue : Factory(1,2),
                         NestedValue : Wrapper(Alpha::Foo, Factory(1,2), [0,1], {3,4}),
                         EscapedQuote : "say \"hello\"")
        std::string m_label;
        HE_TEST_PROPERTY(serializedName : "a&b")
        int m_escaped = 0;
        HE_TEST_PROPERTY(serializedName : "escaped\"quote")
        int m_escaped_quote = 0;
        HE_TEST_PROPERTY(serializedName : "comma,key")
        int m_comma_key = 0;
        HE_TEST_PROPERTY(serializedName : Alpha::Foo)
        int m_namespace_key = 0;
        HE_TEST_PROPERTY(serializedName : [0,1])
        int m_range_key = 0;
        HE_TEST_PROPERTY(serializedName : Factory(1,2))
        int m_factory_key = 0;
        HE_TEST_PROPERTY(serializedName : Nested(Alpha::Foo, Factory(1,2), [0,1], {3,4}))
        int m_nested_key = 0;
    };

    class HE_TEST_REFLECT(Fields) Empty
    {};
}
