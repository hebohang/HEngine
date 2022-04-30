#include "hepch.h"

#include "Runtime/Platform/OpenGL/OpenGLContext.h"

#include <GLFW/glfw3.h>
#include <glad/glad.h>

namespace HEngine
{
    OpenGLContext::OpenGLContext(GLFWwindow* windowHandle)
        : mWindowHandle(windowHandle)
    {
        HE_CORE_ASSERT(windowHandle, "Window handle is null!");
    }

    void OpenGLContext::Init()
    {
        HE_PROFILE_FUNCTION();

        glfwMakeContextCurrent(mWindowHandle);
        int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
        HE_CORE_ASSERT(status, "Failed to initialize Glad!");

        HE_CORE_INFO("OpenGL Info:");
        HE_CORE_INFO("  Vendeor: {0}", glGetString(GL_VENDOR));
        HE_CORE_INFO("  Renderer: {0}", glGetString(GL_RENDERER));
        HE_CORE_INFO("  Version: {0}", glGetString(GL_VERSION));
    }

    void OpenGLContext::SwapBuffers()
    {
        HE_PROFILE_FUNCTION();

        glfwSwapBuffers(mWindowHandle);
    }
}