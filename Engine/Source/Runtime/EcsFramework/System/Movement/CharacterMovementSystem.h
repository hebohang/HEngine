#pragma once

#include "Runtime/EcsFramework/System/System.h"

#include <glm/glm.hpp>

namespace HEngine
{
    class Entity;

    class FixedActionLatch final
    {
    public:
        void Capture(bool bHeld, bool bPressed, bool bPaused) noexcept
        {
            if (bPaused) { Reset(); return; }
            mHeld = bHeld;
            mPendingPress |= bPressed;
        }
        [[nodiscard]] bool ConsumeTick() noexcept
        {
            const bool bActive = mHeld || mPendingPress;
            mPendingPress = false;
            return bActive;
        }
        void Reset() noexcept { mHeld = false; mPendingPress = false; }

    private:
        bool mHeld = false;
        bool mPendingPress = false;
    };

    class CharacterMovementSystem : public System
    {
    public:
        CharacterMovementSystem(Level* level)
            : System(level)
        {
        }

        virtual ~CharacterMovementSystem() = default;

        void OnUpdateRuntime(Timestep ts) override;
        void OnRuntimeStop() override;
        void CaptureInputFrame(bool bPaused);
        [[nodiscard]] bool Teleport(Entity entity, const glm::vec3& worldPosition);
    private:
        // One presentation-frame snapshot is replayed by each fixed tick.
        // The edge is consumed only by the first tick, including when a frame
        // has no tick and must carry the edge forward.
        glm::vec2 mMoveInput{0.0f};
        bool mSprintHeld = false;
        bool mCrouchHeld = false;
        FixedActionLatch mJump;
    };
}
