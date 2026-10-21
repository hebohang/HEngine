#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/Window/Window.h"
#include "Runtime/ImGui/ImGuiPlatform.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>

namespace HEngine
{
    class AndroidImGuiPlatform final : public ImGuiPlatform
    {
    public:
        explicit AndroidImGuiPlatform(Window& window) : mWindow(window) {}
        void Initialize(bool) override
        {
            auto& io = ImGui::GetIO();
            io.BackendPlatformName = "he_android";
            io.IniFilename = nullptr;
        }
        void Shutdown() override { ImGui::GetIO().BackendPlatformName = nullptr; }
        float GetContentScale() const override { return 1.0f; }
        void NewFrame() override
        {
            auto& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(float(mWindow.GetWidth()), float(mWindow.GetHeight()));
            const auto now = std::chrono::steady_clock::now();
            io.DeltaTime = std::clamp(std::chrono::duration<float>(now - mPrevious).count(), 0.001f, 0.1f);
            mPrevious = now;
            const auto pointer = Input::GetMousePosition();
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(pointer.x, pointer.y);
            io.AddMouseButtonEvent(0, Input::IsMouseButtonPressed(MouseCode::ButtonLeft));
        }
    private:
        Window& mWindow;
        std::chrono::steady_clock::time_point mPrevious = std::chrono::steady_clock::now();
    };
    std::unique_ptr<ImGuiPlatform> CreateImGuiPlatform(Window& window)
    {
        return std::make_unique<AndroidImGuiPlatform>(window);
    }
}
