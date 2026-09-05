#include "Hepch.h"

#include "Runtime/Graphics/Renderer/ViewportMode.h"

#include <algorithm>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] ViewportMode Raster2D()
        {
            return {
                .Id = "raster.2d",
                .DisplayName = "2D",
                .Dimension = SceneDimension::TwoD,
            };
        }

        [[nodiscard]] ViewportMode Raster3D()
        {
            return {
                .Id = "raster.3d",
                .DisplayName = "3D",
                .Dimension = SceneDimension::ThreeD,
            };
        }
    }

    ViewportModeController::ViewportModeController()
        : mCurrent(Raster3D())
    {
    }

    std::vector<ViewportMode> ViewportModeController::EnumerateModes(
        const RenderCapabilities& capabilities) const
    {
        std::vector<ViewportMode> modes{ Raster2D(), Raster3D() };
        for (const OfflineViewportRendererProviderDescriptor& provider :
            OfflineViewportRendererProviderRegistry::Get().Enumerate())
        {
            const OfflineViewportRendererSupport support =
                OfflineViewportRendererProviderRegistry::Get().QuerySupport(
                    provider.Id,
                    capabilities);
            modes.push_back(ViewportMode{
                .Id = provider.Id,
                .DisplayName = provider.DisplayName,
                .Dimension = SceneDimension::ThreeD,
                .RendererProviderId = provider.Id,
                .bAvailable = support.bSupported,
                .UnavailableReason = support.Reason,
            });
        }
        return modes;
    }

    const ViewportMode& ViewportModeController::GetCurrentMode() const noexcept
    {
        return mCurrent;
    }

    ViewportModeTransition ViewportModeController::SetMode(
        std::string_view id,
        const RenderCapabilities& capabilities,
        bool bEditorEditState)
    {
        const std::vector<ViewportMode> modes = EnumerateModes(capabilities);
        const auto selected = std::ranges::find_if(
            modes,
            [id](const ViewportMode& mode)
            {
                return mode.Id == id;
            });
        if (selected == modes.end())
            throw std::out_of_range("Unknown viewport renderer mode");
        if (!selected->bAvailable)
            throw std::runtime_error(selected->UnavailableReason);
        if (!bEditorEditState && !selected->UsesRasterRenderer())
        {
            throw std::logic_error(
                "Offline viewport renderers are only available in Editor Edit state");
        }

        ViewportModeTransition transition{
            .Previous = mCurrent,
            .Current = *selected,
            .bChanged = mCurrent.Id != selected->Id,
        };
        if (transition.bChanged)
            mCurrent = *selected;
        return transition;
    }

    ViewportModeTransition ViewportModeController::PrepareForPlay()
    {
        const ViewportMode raster = Raster3D();
        ViewportModeTransition transition{
            .Previous = mCurrent,
            .Current = raster,
            .bChanged = mCurrent.Id != raster.Id,
        };
        mCurrent = raster;
        return transition;
    }

    bool ViewportModeController::IsOfflineRendererActive() const noexcept
    {
        return !mCurrent.UsesRasterRenderer();
    }
}
