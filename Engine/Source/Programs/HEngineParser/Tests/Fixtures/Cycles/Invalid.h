#pragma once

#include "Common.h"

namespace CycleFixture
{
    class InvalidB;

    class HE_TEST_REFLECT(Fields) InvalidA : public InvalidB
    {
    };

    class HE_TEST_REFLECT(Fields) InvalidB : public InvalidA
    {
    };
}
