#include "Runtime/Platform/macOS/MacInputEvents.h"
#include "Runtime/Platform/GLFW/GlfwInputEvents.h"
#include "Runtime/Platform/GLFW/GlfwEventModifiers.h"
#include "Runtime/Core/Log/Log.h"

#import <AppKit/AppKit.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include <cassert>
#include <cmath>
#include <iostream>
#include <thread>

// Drive the exact native-event boundary without moving the user's physical
// pointer or activating a visible window. NSView performs the real conversion.
@interface HEInputContractApplication : NSApplication
@property(nonatomic, strong) NSEvent* inputEvent;
@end
@implementation HEInputContractApplication
- (NSEvent*)currentEvent { return self.inputEvent; }
@end

int main()
{
    @autoreleasepool
    {
        auto* app = [HEInputContractApplication sharedApplication];
        HEngine::Log::Init();
        glfwInitHint(GLFW_COCOA_MENUBAR, GLFW_FALSE);
        glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);
        assert(glfwInit());
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        GLFWwindow* window = glfwCreateWindow(640, 480, "HE input contract", nullptr, nullptr);
        GLFWwindow* other = glfwCreateWindow(640, 480, "HE other input contract", nullptr, nullptr);
        assert(window && other);
        NSWindow* native = glfwGetCocoaWindow(window);
        auto setEvent = [&](NSEventType type, float x, float y)
        {
            const NSPoint point = [native.contentView convertPoint:NSMakePoint(x, native.contentView.bounds.size.height - y) toView:nil];
            ((HEInputContractApplication*)app).inputEvent = [NSEvent mouseEventWithType:type
                location:point modifierFlags:0 timestamp:1 windowNumber:native.windowNumber
                context:nil eventNumber:1 clickCount:1 pressure:1];
        };
        glm::vec2 position;
        assert(!HEngine::TryGetGlfwMouseEventPosition(position));
        setEvent(NSEventTypeLeftMouseDown, 90.25f, 120.5f);
        assert(HEngine::TryGetMacMouseButtonPosition(window, position.x, position.y));
        assert(std::abs(position.x - 90.25f) < 0.001f && std::abs(position.y - 120.5f) < 0.001f);
        assert(!HEngine::TryGetMacMouseButtonPosition(other, position.x, position.y));
        {
            HEngine::ScopedGlfwMouseEventPosition outer(window);
            assert(HEngine::TryGetGlfwMouseEventPosition(position) && position.x == 90.25f);
            bool otherThreadHasEvent = true;
            std::thread([&] { glm::vec2 point; otherThreadHasEvent = HEngine::TryGetGlfwMouseEventPosition(point); }).join();
            assert(!otherThreadHasEvent);
            setEvent(NSEventTypeLeftMouseUp, 310.f, 240.f);
            {
                HEngine::ScopedGlfwMouseEventPosition inner(window);
                assert(HEngine::TryGetGlfwMouseEventPosition(position) && position.x == 310.f && position.y == 240.f);
            }
            assert(HEngine::TryGetGlfwMouseEventPosition(position) && position.x == 90.25f && position.y == 120.5f);
        }
        assert(!HEngine::TryGetGlfwMouseEventPosition(position));
        setEvent(NSEventTypeMouseMoved, 20.f, 30.f);
        assert(!HEngine::TryGetMacMouseButtonPosition(window, position.x, position.y));
        ((HEInputContractApplication*)app).inputEvent = nil;
        assert(!HEngine::TryGetMacMouseButtonPosition(window, position.x, position.y));
        int modifiers = -1;
        assert(!HEngine::TryGetGlfwEventModifiers(modifiers));
        {
            HEngine::ScopedGlfwEventModifiers outer(GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_SHIFT);
            assert(HEngine::TryGetGlfwEventModifiers(modifiers));
            assert((modifiers & GLFW_MOD_CONTROL) && (modifiers & GLFW_MOD_SUPER) && (modifiers & GLFW_MOD_SHIFT));
            assert(!(modifiers & GLFW_MOD_ALT));
            bool otherThreadHasModifiers = true;
            std::thread([&] { int value; otherThreadHasModifiers = HEngine::TryGetGlfwEventModifiers(value); }).join();
            assert(!otherThreadHasModifiers);
            {
                HEngine::ScopedGlfwEventModifiers inner(0);
                assert(HEngine::TryGetGlfwEventModifiers(modifiers) && modifiers == 0);
            }
            assert(HEngine::TryGetGlfwEventModifiers(modifiers) && modifiers == (GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_SHIFT));
        }
        assert(!HEngine::TryGetGlfwEventModifiers(modifiers));
        glfwDestroyWindow(other);
        glfwDestroyWindow(window);
        glfwTerminate();
        std::cout << "PASS: Cocoa logical event coordinates, window/type isolation, nested/thread-local position and modifier restoration\n";
    }
}
