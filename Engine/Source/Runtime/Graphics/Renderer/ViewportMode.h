#pragma once

#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"

#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    enum class SceneDimension : uint8_t
    {
        TwoD,
        ThreeD,
    };

    struct ViewportMode
    {
        std::string Id;
        std::string DisplayName;
        SceneDimension Dimension = SceneDimension::ThreeD;
        std::string RendererProviderId;
        bool bAvailable = true;
        std::string UnavailableReason;

        [[nodiscard]] bool UsesRasterRenderer() const noexcept
        {
            return RendererProviderId.empty();
        }
    };

    struct ViewportModeTransition
    {
        ViewportMode Previous;
        ViewportMode Current;
        bool bChanged = false;
    };

    class ViewportModeController final
    {
    public:
        ViewportModeController();

        [[nodiscard]] std::vector<ViewportMode> EnumerateModes(
            const RenderCapabilities& capabilities) const;
        [[nodiscard]] const ViewportMode& GetCurrentMode() const noexcept;
        [[nodiscard]] ViewportModeTransition SetMode(
            std::string_view id,
            const RenderCapabilities& capabilities,
            bool bEditorEditState);
        [[nodiscard]] ViewportModeTransition PrepareForPlay();
        [[nodiscard]] bool IsOfflineRendererActive() const noexcept;

    private:
        ViewportMode mCurrent;
    };
}
