#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <concepts>
#include <type_traits>

namespace HEngine
{
    template<class T>
    concept Component = std::is_base_of_v<ComponentBase, T>;
}