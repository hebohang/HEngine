#define _CRT_SECURE_NO_WARNINGS

#pragma once

#include "Runtime/ImGui/ImGuiLayer.h"

namespace HEngine
{
	class OpenGLImGuiLayer : public ImGuiLayer
	{
	public:
		OpenGLImGuiLayer() = default;
		OpenGLImGuiLayer(const std::string& name) : ImGuiLayer(name) {}
		virtual ~OpenGLImGuiLayer();

		virtual void Begin() override;
		virtual void End() override;

		virtual void OnAttach() override;
		virtual void OnDetach() override;

	private:
		float m_Time = 0.0f;

	};
}