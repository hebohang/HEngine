#pragma once

struct GLFWwindow;

namespace HEngine
{
    // Only valid synchronously inside the GLFW mouse button callback.
    bool TryGetMacMouseButtonPosition(GLFWwindow* window, float& x, float& y);
}
