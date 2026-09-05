#pragma once

#include "Runtime/Core/AppFramework/Layer/Layer.h"

#include "Runtime/Events/ApplicationEvent.h"
#include "Runtime/Events/KeyEvent.h"
#include "Runtime/Events/MouseEvent.h"

#include <imgui.h>

namespace HEngine
{
    struct NvrhiImGuiLayerDesc;

    class ImGuiLayer : public Layer
    {
    public:
        ImGuiLayer();
        ImGuiLayer(const std::string& name);
        ~ImGuiLayer();

        virtual void OnEvent(Event& e) override;

        virtual void Begin() {};
        virtual void End() {};
        virtual void OnImGuiRender() {}

        // The no-argument factory is retained until Application injects its
        // RenderDevice. It never selects a legacy renderer backend.
        static ImGuiLayer* Create();
        static ImGuiLayer* CreateNvrhi(const NvrhiImGuiLayerDesc& desc);

        // if block is true, then imgui layer doesn't handle events
        void BlockEvents(bool block) { mBlockEvents = block; }
        float GetUIScale() const { return mCurrentDpiScale; }
        float GetFontScale() const { return mFontScale; }
        void SetFontScale(float fontScale);

        void SetDarkThemeColors();

        //static void ReloadFont() { ImGuiLayer::bReloadFont = true; }
    protected:
        void ImguiInit();
        void ImguiInit(float initialDpiScale, bool bEnableDocking, bool bEnableViewports);
        void ImguiEnd();
        void UpdateUIScaleFromWindow();
        void ApplyUIScale(float dpiScale);
    private:
        void ImguiInitFont(float fontSize);
    private:
        bool mBlockEvents = true;
        float mCurrentDpiScale = 0.0f;
        float mFontScale = 1.0f;
        float mBaseFontSize = 20.0f;
        ImGuiStyle mBaseStyle{};
        //static bool bReloadFont;
    };
}
