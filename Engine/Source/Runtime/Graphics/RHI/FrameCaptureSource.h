#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace HEngine
{
    // Owned selection carried through the capture request, never inferred from
    // filenames (except output encoding), GI settings or environment variables.
    enum class FrameCaptureSource : uint8_t
    {
        Automatic, Depth, Motion, Normal, SurfaceId,
        // Raw existing directional shadow array; the caller selects its slice.
        DirectionalShadow,
        Reactive, Composition,
        UpscaleMotion,
        UpscaleColor, UpscaleOpaque,
    };

    [[nodiscard]] constexpr const char* FrameCaptureSourceName(FrameCaptureSource source) noexcept
    {
        switch (source)
        {
        case FrameCaptureSource::Automatic: return "automatic";
        case FrameCaptureSource::Depth: return "depth";
        case FrameCaptureSource::Motion: return "motion";
        case FrameCaptureSource::Normal: return "normal";
        case FrameCaptureSource::SurfaceId: return "surface-id";
        case FrameCaptureSource::DirectionalShadow: return "directional-shadow";
        case FrameCaptureSource::Reactive: return "reactive";
        case FrameCaptureSource::Composition: return "composition";
        case FrameCaptureSource::UpscaleMotion: return "upscale-motion";
        case FrameCaptureSource::UpscaleColor: return "upscale-color";
        case FrameCaptureSource::UpscaleOpaque: return "upscale-opaque";
        }
        return "invalid";
    }

    class CaptureSourceUnavailable final : public std::runtime_error
    {
    public:
        explicit CaptureSourceUnavailable(FrameCaptureSource source)
            : std::runtime_error(std::string("capture_source_unavailable: ") + FrameCaptureSourceName(source)) {}
    };
}
