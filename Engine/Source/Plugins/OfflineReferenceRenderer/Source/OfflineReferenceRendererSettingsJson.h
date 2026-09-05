#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"

namespace HEngine
{
    [[nodiscard]] Json SerializeOfflineReferenceRendererSettings(
        const OfflineViewportRendererSettings& settings);
    [[nodiscard]] OfflineViewportRendererSettings
        DeserializeOfflineReferenceRendererSettings(
            const Json& json,
            const OfflineViewportRendererSettings& defaults = {});
}
