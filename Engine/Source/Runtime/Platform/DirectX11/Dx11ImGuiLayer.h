#define _CRT_SECURE_NO_WARNINGS

#pragma once

#include "Runtime/ImGui/ImGuiLayer.h"

namespace HEngine
{
	class Dx11ImGuiLayer : public ImGuiLayer
	{
	public:
		Dx11ImGuiLayer() = default;
		Dx11ImGuiLayer(const std::string& name) : ImGuiLayer(name) {}
		virtual ~Dx11ImGuiLayer();

		virtual void Begin() override;
		virtual void End() override;

		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnImGuiRender() override;

	private:
		float m_Time = 0.0f;

	};
}