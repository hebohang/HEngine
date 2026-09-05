#pragma once

#include "BasePanel.h"

#include <string>

namespace HEngine
{
    class BaseSettingsPanel final : public BasePanel
    {
        REGISTER_PANEL(BaseSettingsPanel)

    public:
        BaseSettingsPanel(EditorLayer* parent, std::string panelName, bool* pOpen);

        void OnImGuiRender() override;
        void OnImGuiRenderImpl() override;

    private:
        enum class Category
        {
            Appearance,
            StartupAndScenes,
            ViewportCamera
        };

        void DrawHeader() const;
        void DrawNavigation();
        void DrawSelectedCategory();
        void DrawAppearanceSettings();
        void DrawStartupAndSceneSettings();
        void DrawViewportCameraSettings();
        void SelectDefaultScene();

        [[nodiscard]] const char* GetCategoryTitle(Category category) const;
        [[nodiscard]] const char* GetCategoryDescription(Category category) const;

        Category mSelectedCategory = Category::Appearance;
        std::string mStartupMessage;
        bool bStartupMessageError = false;
    };
}
