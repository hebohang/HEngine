#pragma once

#include "HEngine/Layer.h"

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
        virtual void OnImGuiRender() override;

        void Begin();
        void End();
    private:
        float m_Time = 0.0f;
    };
}