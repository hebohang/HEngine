#pragma once

#include "Common.h"

#include <vector>

namespace CycleFixture
{
    class A;

    class HE_TEST_REFLECT(Fields) B
    {
    public:
        std::vector<A> values;
    };
}
