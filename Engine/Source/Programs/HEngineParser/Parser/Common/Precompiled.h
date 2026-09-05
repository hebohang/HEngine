#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <clang-c/Index.h>

namespace fs = std::filesystem;

#include "Meta/MetaDataConfig.h"
#include "Meta/MetaUtils.h"

#include "mustache.hpp"

namespace Mustache = kainjow::mustache;
