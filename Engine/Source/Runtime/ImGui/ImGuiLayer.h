#pragma once

#include "Runtime/Core/AppFramework/Layer/Layer.h"

#include "Runtime/Events/ApplicationEvent.h"
#include "Runtime/Events/KeyEvent.h"
#include "Runtime/Events/MouseEvent.h"

namespace HEngine
{
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

        static ImGuiLayer* Create();

        void BlockEvents(bool block) { mBlockEvents = block; }

        void SetDarkThemeColors();
    private:
        bool mBlockEvents = true;
    };
}
