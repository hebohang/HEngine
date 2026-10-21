#pragma once

#include <memory>

namespace HEngine
{
    class Window;
    class ImGuiPlatform
    {
    public:
        virtual ~ImGuiPlatform() = default;
        virtual void Initialize(bool installCallbacks) = 0;
        virtual void Shutdown() = 0;
        virtual void NewFrame() = 0;
        [[nodiscard]] virtual float GetContentScale() const = 0;
    };
    [[nodiscard]] std::unique_ptr<ImGuiPlatform> CreateImGuiPlatform(Window& window);
}
