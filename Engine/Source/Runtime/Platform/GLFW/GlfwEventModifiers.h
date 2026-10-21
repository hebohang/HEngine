#pragma once

namespace HEngine
{
    // Aggregate modifiers belonging to the dispatched event, not a later poll.
    bool TryGetGlfwEventModifiers(int& modifiers);

    class ScopedGlfwEventModifiers final
    {
    public:
        explicit ScopedGlfwEventModifiers(int modifiers);
        ~ScopedGlfwEventModifiers();
        ScopedGlfwEventModifiers(const ScopedGlfwEventModifiers&) = delete;
        ScopedGlfwEventModifiers& operator=(const ScopedGlfwEventModifiers&) = delete;

    private:
        int mPrevious = 0;
        bool mWasActive = false;
    };
}
