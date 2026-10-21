#include "CommonWindow.h"
#include "Runtime/Platform/GLFW/GlfwInputEvents.h"
#include "Runtime/Platform/GLFW/GlfwEventModifiers.h"
#include "Runtime/ImGui/ImGuiPlatform.h"
#include "Runtime/Core/Log/Log.h"

#include <backends/imgui_impl_glfw.h>
#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <stdexcept>
#if defined(__APPLE__)
#include <unordered_map>
#endif

namespace HEngine
{
    class GlfwImGuiPlatform final : public ImGuiPlatform
    {
    public:
        explicit GlfwImGuiPlatform(GLFWwindow* window) : mWindow(window) {}
        void Initialize(bool installCallbacks) override
        {
            if (!ImGui_ImplGlfw_InitForOther(mWindow, installCallbacks))
                throw std::runtime_error("Failed to initialize the ImGui GLFW platform backend");
#if defined(__APPLE__)
            if (installCallbacks) InstallMouseEventCallbacks();
#endif
        }
        void Shutdown() override
        {
#if defined(__APPLE__)
            if (mCallbacksInstalled)
            {
                glfwSetMouseButtonCallback(mWindow, mMouseButtonCallback);
                glfwSetCursorPosCallback(mWindow, mCursorPositionCallback);
                glfwSetCursorEnterCallback(mWindow, mCursorEnterCallback);
                glfwSetWindowFocusCallback(mWindow, mFocusCallback);
                glfwSetKeyCallback(mWindow, mKeyCallback);
                sInstances.erase(mWindow);
                mCallbacksInstalled = false;
            }
#endif
            ImGui_ImplGlfw_Shutdown();
        }
        void NewFrame() override
        {
#if defined(__APPLE__)
            // Keyboard navigation may explicitly warp the pointer. Otherwise,
            // keep queued native event positions authoritative over GLFW's
            // outside-of-event-stream fallback poll (which can belong elsewhere).
            if (ImGui::GetIO().WantSetMousePos) mHasEventPosition = false;
#endif
            ImGui_ImplGlfw_NewFrame();
#if defined(__APPLE__)
            if (TraceInput() && mTraceFrames > 0)
                HE_CORE_INFO("[UIInput] ImGui platform NewFrame remembered={} value={},{} oldIo={},{} focusLost={} noMouse={}",
                    mHasEventPosition, mEventPosition.x, mEventPosition.y, ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y,
                    ImGui::GetIO().AppFocusLost, (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouse) != 0);
            if (mHasEventPosition) SubmitEventPosition();
            if (mTraceFrames > 0) --mTraceFrames;
#endif
        }
        float GetContentScale() const override
        {
#if defined(__APPLE__)
            // GLFW uses logical points on macOS. ImGui's GLFW backend supplies
            // DisplayFramebufferScale for Retina pixels; applying that factor
            // to widget/font sizes again would double the editor's UI size.
            return 1.f;
#else
            float x = 1, y = 1;
            glfwGetWindowContentScale(mWindow, &x, &y);
            return std::clamp((x + y) * 0.5f, 0.75f, 3.0f);
#endif
        }
    private:
#if defined(__APPLE__)
        static bool TraceInput()
        {
            static const bool enabled = [] { const char* value = std::getenv("HE_TRACE_UI_INPUT"); return value && std::string_view(value) == "1"; }();
            return enabled;
        }

        void SubmitEventPosition()
        {
            ImVec2 position = mEventPosition;
            ImGuiIO& io = ImGui::GetIO();
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            {
                int x = 0, y = 0;
                glfwGetWindowPos(mWindow, &x, &y);
                position.x += static_cast<float>(x);
                position.y += static_cast<float>(y);
            }
            if (TraceInput() && mTraceFrames > 0)
                HE_CORE_INFO("[UIInput] ImGui submit event={},{} queued={},{} priorIo={},{} mainViewport={},{} flags={} display={}x{} framebufferScale={},{}",
                    mEventPosition.x, mEventPosition.y, position.x, position.y, io.MousePos.x, io.MousePos.y,
                    ImGui::GetMainViewport()->Pos.x, ImGui::GetMainViewport()->Pos.y, io.ConfigFlags,
                    io.DisplaySize.x, io.DisplaySize.y, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            io.AddMousePosEvent(position.x, position.y);
        }

        void InstallMouseEventCallbacks()
        {
            sInstances[mWindow] = this;
            mMouseButtonCallback = glfwSetMouseButtonCallback(mWindow, [](GLFWwindow* window, int button, int action, int mods)
            {
                auto& platform = *sInstances.at(window);
                if (TraceInput())
                {
                    platform.mTraceFrames = 6;
                    HE_CORE_INFO("[UIInput] ImGui button button={} action={} mods={}", button, action, mods);
                }
                const ScopedGlfwMouseEventPosition eventScope(window);
                const ScopedGlfwEventModifiers eventModifiers(mods);
                glm::vec2 position;
                if (TryGetGlfwMouseEventPosition(position))
                {
                    platform.mEventPosition = ImVec2(position.x, position.y);
                    platform.mHasEventPosition = true;
                    // Queue coordinates before the backend queues the button.
                    platform.SubmitEventPosition();
                }
                if (platform.mMouseButtonCallback) platform.mMouseButtonCallback(window, button, action, mods);
            });
            mCursorPositionCallback = glfwSetCursorPosCallback(mWindow, [](GLFWwindow* window, double x, double y)
            {
                auto& platform = *sInstances.at(window);
                platform.mEventPosition = ImVec2(static_cast<float>(x), static_cast<float>(y));
                platform.mHasEventPosition = true;
                if (platform.mCursorPositionCallback) platform.mCursorPositionCallback(window, x, y);
            });
            mCursorEnterCallback = glfwSetCursorEnterCallback(mWindow, [](GLFWwindow* window, int entered)
            {
                auto& platform = *sInstances.at(window);
                if (!entered) platform.mHasEventPosition = false;
                if (platform.mCursorEnterCallback) platform.mCursorEnterCallback(window, entered);
            });
            mFocusCallback = glfwSetWindowFocusCallback(mWindow, [](GLFWwindow* window, int focused)
            {
                auto& platform = *sInstances.at(window);
                if (!focused) platform.mHasEventPosition = false;
                if (platform.mFocusCallback) platform.mFocusCallback(window, focused);
            });
            mKeyCallback = glfwSetKeyCallback(mWindow, [](GLFWwindow* window, int key, int scancode, int action, int mods)
            {
                auto& platform = *sInstances.at(window);
                const ScopedGlfwEventModifiers eventModifiers(mods);
                if (TraceInput()) HE_CORE_INFO("[UIInput] GLFW key={} action={} eventModifiers={}", key, action, mods);
                if (platform.mKeyCallback) platform.mKeyCallback(window, key, scancode, action, mods);
            });
            mCallbacksInstalled = true;
        }

        static inline std::unordered_map<GLFWwindow*, GlfwImGuiPlatform*> sInstances;
        GLFWmousebuttonfun mMouseButtonCallback = nullptr;
        GLFWcursorposfun mCursorPositionCallback = nullptr;
        GLFWcursorenterfun mCursorEnterCallback = nullptr;
        GLFWwindowfocusfun mFocusCallback = nullptr;
        GLFWkeyfun mKeyCallback = nullptr;
        ImVec2 mEventPosition;
        bool mHasEventPosition = false;
        bool mCallbacksInstalled = false;
        unsigned mTraceFrames = 0;
#endif
        GLFWwindow* mWindow;
    };

    std::unique_ptr<ImGuiPlatform> CreateImGuiPlatform(Window& window)
    {
        auto* nativeWindow = dynamic_cast<CommonWindow*>(&window);
        if (!nativeWindow || !nativeWindow->GetGlfwWindow())
            throw std::invalid_argument("ImGui requires a live GLFW window");
        return std::make_unique<GlfwImGuiPlatform>(nativeWindow->GetGlfwWindow());
    }
}
