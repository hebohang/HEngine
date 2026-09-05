#pragma once

namespace HEngine
{
    // Platform windows opt into rendering through this marker. Native handles
    // are recovered only by Runtime/Platform backends.
    class RenderSurface
    {
    public:
        virtual ~RenderSurface() = default;
    };
}
