#pragma once

#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <span>

namespace HEngine
{
    struct LightTexture
    {
        std::string AssetPath;
        int Width = 0;
        int Height = 0;
        std::vector<unsigned char> Pixels;
    };

    struct LightRenderCapabilities
    {
        bool TexturedSprites = true;
        bool Depth = true;
        bool StraightAlpha = true;
    };

    // No device, GPU handles, descriptor indices or desktop graph obligations.
    class LightRenderer
    {
    public:
        virtual ~LightRenderer() = default;
        virtual void Init(const LightTexture& texture) = 0;
        virtual void Resize(int width, int height) = 0;
        virtual void Submit(const RenderCamera& camera, std::span<const RenderPrimitive2D> primitives) = 0;
        virtual LightRenderCapabilities Capabilities() const = 0;
        virtual void Shutdown() = 0;
    };
    Scope<LightRenderer> CreateLightRenderer();
}
