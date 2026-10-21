#pragma once

// The pinned LLGI BaseVulkan header assumes every non-Windows/Apple target is
// XCB. Load the platform-neutral Vulkan declarations before that legacy header.
// HEngine owns presentation, so LLGI needs no platform surface declarations.
#include <vulkan/vulkan.hpp>
