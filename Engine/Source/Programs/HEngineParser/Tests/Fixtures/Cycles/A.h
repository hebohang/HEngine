#pragma once

#include "Common.h"

#include <vector>

namespace CycleFixture
{
    class B;

    class HE_TEST_REFLECT(Fields) A
    {
    public:
        std::vector<B> values;
    };
}
