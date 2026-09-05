#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"

namespace HEngine
{
    [[nodiscard]] Json SerializeGlobalIlluminationSettings(
        const GlobalIlluminationSettings& settings);
    [[nodiscard]] GlobalIlluminationSettings DeserializeGlobalIlluminationSettings(
        const Json& json,
        const GlobalIlluminationSettings& defaults = {});
}
