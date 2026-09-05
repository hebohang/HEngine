#pragma once

#define HE_TEST_REFLECT(...) __attribute__((annotate(#__VA_ARGS__)))

namespace BrokenFixture
{
    class HE_TEST_REFLECT(Fields) Broken
    {
    public:
        int m_value = ;
    };
}
