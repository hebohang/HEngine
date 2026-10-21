#pragma once

#include <glm/vec2.hpp>
#include <optional>

struct GLFWwindow;

namespace HEngine
{
    bool TryGetGlfwMouseEventPosition(glm::vec2& point);

    // A queued button event may be delivered after the physical pointer moved.
    // Keep synchronous event consumers on the event's logical client position.
    class ScopedGlfwMouseEventPosition final
    {
    public:
        explicit ScopedGlfwMouseEventPosition(GLFWwindow* window);
        ~ScopedGlfwMouseEventPosition();
        ScopedGlfwMouseEventPosition(const ScopedGlfwMouseEventPosition&) = delete;
        ScopedGlfwMouseEventPosition& operator=(const ScopedGlfwMouseEventPosition&) = delete;

    private:
        std::optional<glm::vec2> mPrevious;
    };
}
