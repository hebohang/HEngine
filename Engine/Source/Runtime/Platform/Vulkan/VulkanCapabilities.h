#pragma once

#include "Runtime/Graphics/RHI/RenderBackend.h"

namespace HEngine
{
    void RunVulkanCapabilityProbe(
        const std::filesystem::path& outputPath,
        const RenderAdapterSelectionOptions& selection = {});
}
