#pragma once

#include <iostream>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <algorithm>
#include <functional>

#include <string>
#include <sstream>
#include <array>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include <concepts>
#include <type_traits>

#include "Runtime/Core/Log/Log.h"
#include "Runtime/Debug/Instrumentor.h"

#ifdef HE_PLATFORM_WINDOWS
	#include <Windows.h>
#endif

