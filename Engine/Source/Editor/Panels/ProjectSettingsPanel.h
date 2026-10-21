#pragma once

#include "BasePanel.h"

#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Resource/ResType/InputMap.h"
#include "Runtime/Resource/ResType/NavigationSettings.h"

#include <array>
#include <string>

namespace HEngine
{
    class ProjectSettingsPanel final : public BasePanel
    {
        REGISTER_PANEL(ProjectSettingsPanel)

    public:
        ProjectSettingsPanel(
            EditorLayer* parent,
            std::string panelName,
            bool* pOpen);

        void OnImGuiRender() override;

    protected:
        void OnImGuiRenderImpl() override;

    private:
        void Refresh(bool bDiscardPending);
        void DrawNavigation();
        void DrawSettings();
        void DrawInputSettings();
        void DrawNavigationSettings();
        void DrawPipelineProfiles();
        void DrawSetting(const RenderSettingDescriptor& descriptor);
        void DrawFooter();
        void DrawDiagnostics();
        void ApplyPending(SettingsLayer layer, bool bPersist);
        void QueueCurrentPageReset();
        [[nodiscard]] Json GetDisplayedValue(std::string_view propertyId) const;
        [[nodiscard]] bool IsDescriptorOnSelectedPage(
            const RenderSettingDescriptor& descriptor) const;
        [[nodiscard]] bool MatchesFilter(
            const RenderSettingDescriptor& descriptor) const;
        [[nodiscard]] const char* GetSelectedPageTitle() const;
        [[nodiscard]] const char* GetSelectedPageDescription() const;

        ResolvedRenderSettings mSnapshot;
        ResolvedRenderSettings mBaseSnapshot;
        std::string mEditedView;
        Json mPendingPatch = Json::object();
        std::array<char, 160> mSearch{};
        std::array<char, 128> mProfileName{};
        std::string mSelectedProfileAsset;
        std::string mSelectedPage = "Rendering.PostProcess";
        std::string mLastMessage;
        SettingsLayer mTargetLayer = SettingsLayer::Session;
        bool bShowAdvanced = false;
        bool bInitialized = false;
        InputMapRes mInputDraft;
        std::string mInputProject, mInputMessage;
        NavigationBuildSettings mNavigationDraft;
        std::string mNavigationProject, mNavigationMessage;
    };
}
