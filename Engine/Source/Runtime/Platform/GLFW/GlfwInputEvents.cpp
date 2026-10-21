#include "Runtime/Platform/GLFW/GlfwInputEvents.h"
#include "Runtime/Platform/GLFW/GlfwEventModifiers.h"
#if defined(__APPLE__)
#include "Runtime/Platform/macOS/MacInputEvents.h"
#endif

namespace HEngine
{
    namespace
    {
        thread_local std::optional<glm::vec2> sMouseEventPosition;
        thread_local int sEventModifiers = 0;
        thread_local bool sHasEventModifiers = false;
    }

    bool TryGetGlfwEventModifiers(int& modifiers)
    {
        if (!sHasEventModifiers) return false;
        modifiers = sEventModifiers;
        return true;
    }

    ScopedGlfwEventModifiers::ScopedGlfwEventModifiers(int modifiers)
        : mPrevious(sEventModifiers), mWasActive(sHasEventModifiers)
    {
#if defined(__APPLE__)
        sEventModifiers = modifiers;
        sHasEventModifiers = true;
#else
        (void)modifiers;
#endif
    }

    ScopedGlfwEventModifiers::~ScopedGlfwEventModifiers()
    {
        sEventModifiers = mPrevious;
        sHasEventModifiers = mWasActive;
    }

    bool TryGetGlfwMouseEventPosition(glm::vec2& point)
    {
        if (!sMouseEventPosition) return false;
        point = *sMouseEventPosition;
        return true;
    }

    ScopedGlfwMouseEventPosition::ScopedGlfwMouseEventPosition(GLFWwindow* window)
        : mPrevious(sMouseEventPosition)
    {
#if defined(__APPLE__)
        glm::vec2 point;
        if (TryGetMacMouseButtonPosition(window, point.x, point.y)) sMouseEventPosition = point;
#else
        (void)window;
#endif
    }

    ScopedGlfwMouseEventPosition::~ScopedGlfwMouseEventPosition()
    {
        sMouseEventPosition = mPrevious;
    }
}
