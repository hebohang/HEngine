#pragma once

#include <iostream>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <algorithm>
#include <functional>
#include <regex>
#if !defined(HE_WECHAT)
#include <execution>
#endif
#include <filesystem>

#include <string>
#include <sstream>
#include <fstream>
#include <array>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <concepts>
#include <type_traits>

#include "Runtime/Core/Log/Log.h"
#include "Runtime/Profile/Profile.h"

#ifdef HE_PLATFORM_WINDOWS
	#include <Windows.h>
#endif

