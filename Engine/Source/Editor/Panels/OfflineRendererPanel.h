#pragma once

#include "BasePanel.h"

#include "Runtime/Graphics/Renderer/ViewportRendererExtension.h"

#include <array>
#include <string>

namespace HEngine
{
    class OfflineRendererPanel final : public BasePanel
    {
        REGISTER_PANEL(OfflineRendererPanel)

    public:
        OfflineRendererPanel(
            EditorLayer* parent,
            std::string panelName,
            bool* pOpen);

    protected:
        void OnImGuiRenderImpl() override;

    private:
        void InitializeFromSession(IOfflineViewportRendererSession& session);
        void PollExport(IOfflineViewportRendererSession& session);

        OfflineViewportRendererSettings mApplied;
        OfflineViewportRendererSettings mPending;
        TextureReadbackHandle mExport;
        std::array<char, 512> mOutputDirectory{};
        std::array<char, 128> mOutputFileName{};
        std::string mProviderId;
        std::string mStatus;
        bool bInitialized = false;
        bool bDirty = false;
    };
}
