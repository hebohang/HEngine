#pragma once

#include "HEngine/Core/Layer.h"

#include "HEngine/Events/ApplicationEvent.h"
#include "HEngine/Events/KeyEvent.h"
#include "HEngine/Events/MouseEvent.h"

namespace HEngine
{
    class HENGINE_API ImGuiLayer : public Layer
    {
    public:
        ImGuiLayer();
        ~ImGuiLayer();

        virtual void OnAttach() override;
        virtual void OnDetach() override;
        virtual void OnEvent(Event& e) override;

        void Begin();
        void End();

        void BlockEvents(bool block) { m_BlockEvents = block; }
    private:
        bool m_BlockEvents = true;
        float m_Time = 0.0f;
    };
}