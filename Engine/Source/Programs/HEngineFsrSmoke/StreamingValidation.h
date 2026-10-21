#pragma once

#include "Runtime/Graphics/RHI/RenderDevice.h"

#include <filesystem>

void VerifyTextureStreaming(nvrhi::IDevice* device, HEngine::RenderBackend backend,
    const std::filesystem::path& evidence);
