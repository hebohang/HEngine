#include "hepch.h"

#include "Runtime/ImGui/ImGuiLayer.h"
#include "Runtime/Renderer/RendererAPI.h"
#include "Runtime/Platform/OpenGL/OpenGLImGuiLayer.h"
#include "Runtime/Platform/Vulkan/VulkanImGuiLayer.h"
#include "Runtime/Platform/DirectX11/Dx11ImGuiLayer.h"

#include <imgui.h>

namespace HEngine
{
    ImGuiLayer::ImGuiLayer()
        : Layer("ImGuiLayer")
    {
    }

    ImGuiLayer::ImGuiLayer(const std::string& name)
        : Layer(name)
    {
    }

    ImGuiLayer::~ImGuiLayer()
    {
    }

    void ImGuiLayer::OnEvent(Event& e)
    {
        if (mBlockEvents)
        {
            ImGuiIO& io = ImGui::GetIO();
            e.Handled |= e.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
            e.Handled |= e.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
        }
    }

    ImGuiLayer* ImGuiLayer::Create()
    {
        switch (RendererAPI::Current())
        {
        case RendererAPI::RendererAPIType::None:    return nullptr;
        case RendererAPI::RendererAPIType::OpenGL:  return new OpenGLImGuiLayer("OpenGLImGuiLayer");
        case RendererAPI::RendererAPIType::Vulkan:  return new VulkanImGuiLayer("VulkanImGuiLayer");
        case RendererAPI::RendererAPIType::DX11:    return new Dx11ImGuiLayer("Dx11ImGuiLayer");
        case RendererAPI::RendererAPIType::DX12:    return nullptr;
        }

        HE_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    void ImGuiLayer::SetDarkThemeColors()
    {
        auto& colors = ImGui::GetStyle().Colors;
        colors[ImGuiCol_WindowBg] = ImVec4{ 0.2f, 0.205f, 0.21f, 1.0f };

        // Headers
        colors[ImGuiCol_Header] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };
        colors[ImGuiCol_HeaderHovered] = ImVec4{ 0.4f, 0.405f, 0.41f, 1.0f };
        colors[ImGuiCol_HeaderActive] = ImVec4{ 0.25f, 0.2505f, 0.251f, 1.0f };

        // Buttons
        colors[ImGuiCol_Button] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };
        colors[ImGuiCol_ButtonHovered] = ImVec4{ 0.4f, 0.405f, 0.41f, 1.0f };
        colors[ImGuiCol_ButtonActive] = ImVec4{ 0.25f, 0.2505f, 0.251f, 1.0f };

        // Frame BG
        colors[ImGuiCol_FrameBg] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };
        colors[ImGuiCol_FrameBgHovered] = ImVec4{ 0.4f, 0.405f, 0.41f, 1.0f };
        colors[ImGuiCol_FrameBgActive] = ImVec4{ 0.25f, 0.2505f, 0.251f, 1.0f };

        // Tabs
        colors[ImGuiCol_Tab] = ImVec4{ 0.25f, 0.2505f, 0.251f, 1.0f };
        colors[ImGuiCol_TabHovered] = ImVec4{ 0.48f, 0.4805f, 0.481f, 1.0f };
        colors[ImGuiCol_TabActive] = ImVec4{ 0.38f, 0.3805f, 0.381f, 1.0f };
        colors[ImGuiCol_TabUnfocused] = ImVec4{ 0.25f, 0.2505f, 0.251f, 1.0f };
        colors[ImGuiCol_TabUnfocusedActive] = ImVec4{ 0.3f, 0.305f, 0.31f, 1.0f };

        // Title
        colors[ImGuiCol_TitleBg] = ImVec4{ 0.25f, 0.2505f, 0.251f, 1.0f };
        colors[ImGuiCol_TitleBgActive] = ImVec4{ 0.25f, 0.2505f, 0.251f, 1.0f };
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4{ 0.25f, 0.2505f, 0.251f, 1.0f };

        // Resize Grip
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.91f, 0.91f, 0.91f, 0.25f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.81f, 0.81f, 0.81f, 0.67f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.46f, 0.46f, 0.46f, 0.95f);

        // Scrollbar
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.12f, 0.12f, 0.12f, 0.53f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.41f, 0.41f, 0.41f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.51f, 0.51f, 0.51f, 1.0f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.61f, 0.61f, 0.61f, 1.0f);

        // Check Mark
        colors[ImGuiCol_CheckMark] = ImVec4(0.94f, 0.94f, 0.94f, 1.0f);

        // Slider
        colors[ImGuiCol_SliderGrab] = ImVec4(0.51f, 0.51f, 0.51f, 0.7f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.66f, 0.66f, 0.66f, 1.0f);
    }
}
