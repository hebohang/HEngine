#pragma once

#include "BasePanel.h"

#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/Renderer/AntiAliasing.h"

#include <string>

namespace HEngine
{
    class GlobalIlluminationPanel final : public BasePanel
    {
        REGISTER_PANEL(GlobalIlluminationPanel)

    public:
        GlobalIlluminationPanel(
            EditorLayer* parent,
            std::string panelName,
            bool* pOpen);

    protected:
        void OnImGuiRenderImpl() override;

    private:
        void InitializePendingSettings();
        void ApplyQualityPreset(int preset);

        GlobalIlluminationSettings mApplied;
        GlobalIlluminationSettings mPending;
        AntiAliasingSettings mAppliedAntiAliasing;
        AntiAliasingSettings mPendingAntiAliasing;
        std::string mLastApplyError;
        std::string mLastAntiAliasingApplyError;
        int mQualityPreset = 1;
        bool bInitialized = false;
        bool bDirty = false;
        bool bAntiAliasingDirty = false;
    };
}
