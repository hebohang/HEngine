#include "hepch.h"
#include "Runtime/Core/Input.h"

#include "Runtime/Core/Application.h"
#include <GLFW/glfw3.h>

namespace HEngine
{
    bool Input::IsKeyPressed(KeyCode key)
    {
        auto window = static_cast<GLFWwindow*>(Application::GetInstance().GetWindow().GetNativeWindow());
        auto state = glfwGetKey(window, static_cast<int32_t>(key));
        return state == GLFW_PRESS || state == GLFW_REPEAT;
    }
    bool Input::IsMouseButtonPressed(MouseCode button)
    {
        auto window = static_cast<GLFWwindow*>(Application::GetInstance().GetWindow().GetNativeWindow());
        auto state = glfwGetMouseButton(window, static_cast<int32_t>(button));
        return state == GLFW_PRESS;
    }
    glm::vec2 Input::GetMousePosition()
    {
        auto window = static_cast<GLFWwindow*>(Application::GetInstance().GetWindow().GetNativeWindow());
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);

        return { (float)xpos, (float)ypos };
    }
    float Input::GetMouseX()
    {
        return GetMousePosition().x;
    }
    float Input::GetMouseY()
    {
        return GetMousePosition().y;
    }
}
