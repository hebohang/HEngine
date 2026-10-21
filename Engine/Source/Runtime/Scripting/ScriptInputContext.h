#pragma once

#include <cstdint>

namespace HEngine
{
    // Host-client coordinates, in the same units as Input::GetMousePosition().
    // Game hosts use the full window; Editor restricts input to its game viewport.
    struct ScriptInputRegion
    {
        bool Restricted = false;
        bool Enabled = true;
        bool KeyboardEnabled = true;
        float X = 0, Y = 0, Width = 0, Height = 0;

        bool Contains(float x, float y) const
        {
            return Enabled && (!Restricted || (Width > 0 && Height > 0 &&
                x >= X && y >= Y && x < X + Width && y < Y + Height));
        }
    };

    struct ScriptInputContext
    {
        static inline ScriptInputRegion Region;
    };

    // Events retain complete clicks between frames; polling must not count them
    // again or turn a press originating in Editor UI into a gameplay drag.
    class ScriptPointerState
    {
    public:
        void Update(bool pressed, bool accepted)
        {
            if (pressed && !mPressed)
            {
                mAccepted = accepted;
                if (accepted) ++mSequence;
            }
            mPressed = pressed;
            if (!pressed) mAccepted = false;
        }
        bool IsPressed(bool accepted) const { return mPressed && mAccepted && accepted; }
        std::uint64_t Sequence() const { return mSequence; }

    private:
        bool mPressed = false;
        bool mAccepted = false;
        std::uint64_t mSequence = 0;
    };
}
