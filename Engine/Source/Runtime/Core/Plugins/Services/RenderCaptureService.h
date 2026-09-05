#pragma once

#include <string>

namespace HEngine
{
    inline constexpr const char* c_RenderCaptureServiceId =
        "he.service.render-capture";

    class IRenderCaptureService
    {
    public:
        virtual ~IRenderCaptureService() = default;

        [[nodiscard]] virtual bool IsAvailable() const noexcept = 0;
        [[nodiscard]] virtual std::string GetUnavailableReason() const = 0;
        virtual void Capture() = 0;
    };
}
