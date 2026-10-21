#include "Runtime/Platform/macOS/MacInputEvents.h"
#include "Runtime/Core/Log/Log.h"

#import <AppKit/AppKit.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include <cmath>
#include <cstdlib>
#include <string_view>

namespace HEngine
{
    bool TryGetMacMouseButtonPosition(GLFWwindow* window, float& x, float& y)
    {
        NSWindow* nativeWindow = glfwGetCocoaWindow(window);
        NSEvent* event = NSApp.currentEvent;
        static const bool trace = [] { const char* value = std::getenv("HE_TRACE_UI_INPUT"); return value && std::string_view(value) == "1"; }();
        if (trace)
            HE_CORE_INFO("[UIInput] Cocoa current type={} eventWindow={} inputWindow={}",
                event ? static_cast<unsigned long>(event.type) : 0,
                event.window ? static_cast<long>(event.window.windowNumber) : 0,
                nativeWindow ? static_cast<long>(nativeWindow.windowNumber) : 0);
        if (!nativeWindow || !event || event.window != nativeWindow)
            return false;
        switch (event.type)
        {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:
            break;
        default:
            return false;
        }
        NSView* view = nativeWindow.contentView;
        const NSPoint point = [view convertPoint:event.locationInWindow fromView:nil];
        x = static_cast<float>(point.x);
        y = static_cast<float>(view.isFlipped ? point.y : view.bounds.size.height - point.y);
        if (trace)
        {
            double polledX = 0, polledY = 0;
            glfwGetCursorPos(window, &polledX, &polledY);
            HE_CORE_INFO("[UIInput] Cocoa button event={},{} polled={},{} type={} window={}",
                x, y, polledX, polledY, static_cast<unsigned long>(event.type), static_cast<long>(nativeWindow.windowNumber));
        }
        return std::isfinite(x) && std::isfinite(y);
    }
}
