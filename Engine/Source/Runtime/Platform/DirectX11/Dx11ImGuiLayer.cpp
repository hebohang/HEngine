#include "hepch.h"

#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Platform/DirectX11/Dx11ImGuiLayer.h"
#include "Runtime/Platform/DirectX11/Dx11ContextWrapper.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Platform/DirectX11/DXTrace.h"

#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_WIN32 1
#include <GLFW/glfw3native.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

#include <d3d11.h>

#include <ImGuizmo.h>

namespace HEngine
{
    Dx11ImGuiLayer::~Dx11ImGuiLayer()
    {
    }

    void Dx11ImGuiLayer::OnAttach()
    {
        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
        //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows
        //io.ConfigViewportsNoAutoMerge = true;
        //io.ConfigViewportsNoTaskBarIcon = true;

        float fontSize = 18.0f;
        io.Fonts->AddFontFromFileTTF(AssetManager::GetFullPath("Assets/fonts/opensans/OpenSans-Bold.ttf").string().c_str(), fontSize);
        io.FontDefault = io.Fonts->AddFontFromFileTTF(AssetManager::GetFullPath("Assets/fonts/opensans/OpenSans-Regular.ttf").string().c_str(), fontSize);

        // Setup Dear ImGui style
        ImGui::StyleColorsDark();
        //ImGui::StyleColorsClassic();

        // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
        ImGuiStyle& style = ImGui::GetStyle();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }

        SetDarkThemeColors();

        Application& app = Application::GetInstance();
        GLFWwindow* window = static_cast<GLFWwindow*>(app.GetWindow().GetNativeWindow());
        HWND hWnd = glfwGetWin32Window(window);

        // Setup Platform/Renderer backends
        ImGui_ImplGlfw_InitForOther(window, true);
        //ImGui_ImplWin32_Init(hWnd);
        ImGui_ImplDX11_Init(Dx11ContextWrapper::GetDevice().Get(), Dx11ContextWrapper::GetContext().Get());

        Dx11ContextWrapper::OnResize();
    }

    void Dx11ImGuiLayer::OnDetach()
    {
        // Cleanup
        ImGui_ImplDX11_Shutdown();
        //ImGui_ImplWin32_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    void Dx11ImGuiLayer::OnImGuiRender()
    {
        static bool pOpen = true;
        ImGui::ShowDemoWindow(&pOpen);
    }

    void Dx11ImGuiLayer::Begin()
    {
        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        //ImGui_ImplWin32_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuizmo::BeginFrame();
    }

    void Dx11ImGuiLayer::End()
    {
        ImGuiIO& io = ImGui::GetIO();
        Application& app = Application::GetInstance();
        io.DisplaySize = ImVec2((float)app.GetWindow().GetWidth(), (float)app.GetWindow().GetHeight());

        // Rendering
        ImGui::Render();
        ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
        //Dx11ContextWrapper::GetContext()->OMSetRenderTargets(1, Dx11ContextWrapper::GetRTV().GetAddressOf(), NULL);
        ImDrawData* main_draw_data = ImGui::GetDrawData();
        ImGui_ImplDX11_RenderDrawData(main_draw_data);

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }
    }
}