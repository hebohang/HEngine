#include "Sandbox2D.h"
#include "imgui/imgui.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <chrono>

Sandbox2D::Sandbox2D()
	: Layer("Sandbox2D"), m_CameraController(1280.0f / 720.0f)
{
}

void Sandbox2D::OnAttach()
{
    m_CheckerboardTexture = HEngine::Texture2D::Create("assets/textures/Checkerboard.png");
}

void Sandbox2D::OnDetach()
{
}

void Sandbox2D::OnUpdate(HEngine::Timestep ts)
{
	HE_PROFILE_FUNCTION();

    // Update
	{
		HE_PROFILE_SCOPE("CameraController::OnUpdate");
		m_CameraController.OnUpdate(ts);	
	}

    // Render
	{
		HE_PROFILE_SCOPE("Renderer Prep");
		HEngine::RenderCommand::SetClearColor({ 0.1f, 0.1f, 0.1f, 1 });
		HEngine::RenderCommand::Clear();
	}

	{
		HE_PROFILE_SCOPE("Renderer Draw");
		HEngine::Renderer2D::BeginScene(m_CameraController.GetCamera());
		//HEngine::Renderer2D::DrawRotatedQuad({ -1.0f, 0.0f }, { 0.8f, 0.8f }, glm::radians(-45.0f), { 0.8f, 0.2f, 0.3f, 1.0f });
		HEngine::Renderer2D::DrawQuad({ -1.0f, 0.0f }, { 0.8f, 0.8f }, { 0.8f, 0.2f, 0.3f, 1.0f });
		HEngine::Renderer2D::DrawQuad({ 0.5f, -0.5f }, { 0.5f, 0.75f }, { 0.2f, 0.3f, 0.8f, 1.0f });
		HEngine::Renderer2D::DrawQuad({ -5.0f,  -5.0f, -0.1f }, { 10.0f, 10.0f }, m_CheckerboardTexture, 10.0f);
		HEngine::Renderer2D::DrawQuad({ -0.5f,  -0.5f,  0.0f }, { 1.0f, 1.0f }, m_CheckerboardTexture, 20.0f);
		HEngine::Renderer2D::EndScene();
	}
}

void Sandbox2D::OnImGuiRender()
{
	HE_PROFILE_FUNCTION();

	ImGui::Begin("Settings");
    ImGui::ColorEdit4("Square Color", glm::value_ptr(m_SquareColor));

    ImGui::End();
}

void Sandbox2D::OnEvent(HEngine::Event& e)
{
    m_CameraController.OnEvent(e);
}
