#pragma once

namespace HEngine
{
    // Stores the current game-viewport screen rect so Python UI scripts can
    // position their windows relative to the viewport instead of the whole
    // editor screen.  Updated every frame by EditorLayer (play mode) and
    // GameLayer before they call Level::OnImGuiRender().
    struct UIViewportContext
    {
        static inline float X = 0.f;
        static inline float Y = 0.f;
        static inline float W = 0.f;
        static inline float H = 0.f;
        static inline float UIScale = 1.f;

        static void Set(float x, float y, float w, float h, float uiScale)
        {
            X = x; Y = y; W = w; H = h; UIScale = uiScale;
        }

        static void Set(float x, float y, float w, float h)
        {
            Set(x, y, w, h, 1.0f);
        }
    };
}
