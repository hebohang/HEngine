#include "hepch.h"
#include "OpenGLContext.h"

#include <GLFW/glfw3.h>
#include <glad/glad.h>

namespace HEngine
{
    OpenGLContext::OpenGLContext(GLFWwindow* windowHandle)
        : m_WindowHandle(windowHandle)
    {
        HE_CORE_ASSERT(windowHandle, "Window handle is null!");
    }

    void OpenGLContext::Init()
    {
        glfwMakeContextCurrent(m_WindowHandle);
        int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
        HE_CORE_ASSERT(status, "Failed to initialize Glad!");

        HE_CORE_INFO("OpenGL Info:");
        HE_CORE_INFO("  Vendeor: {0}", glGetString(GL_VENDOR));
        HE_CORE_INFO("  Renderer: {0}", glGetString(GL_RENDERER));
        HE_CORE_INFO("  Version: {0}", glGetString(GL_VERSION));
    }

    void OpenGLContext::SwapBuffers()
    {
        glfwSwapBuffers(m_WindowHandle);
    }
}