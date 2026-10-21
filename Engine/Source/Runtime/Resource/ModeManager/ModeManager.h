#pragma once

#include "Runtime/Core/Templates/PublicSingleton.h"

#include <cstdint>

namespace HEngine
{
    enum class PhysicsDebugDrawModeFlag : uint32_t
    {
        None = 0,
        DrawShape = 1 << 0,
        DrawShapeWireframe = 1 << 1,
        DrawBoundingBox = 1 << 2,
        DrawCenterOfMassTransform = 1 << 3,
        DrawWorldTransform = 1 << 4,
        DrawVelocity = 1 << 5,
        DrawMassAndInertia = 1 << 6,
        DrawSleepStats = 1 << 7,
        DrawConstraints = 1 << 8,
        DrawConstraintLimits = 1 << 9,
        DrawConstraintReferenceFrame = 1 << 10,
        CharacterVirtualDrawConstraints = 1 << 11,
        CharacterVirtualDrawWalkStairs = 1 << 12,
        CharacterVirtualDrawStickToFloor = 1 << 13
    };

    inline PhysicsDebugDrawModeFlag operator|(PhysicsDebugDrawModeFlag lhs, PhysicsDebugDrawModeFlag rhs)
    {
        return static_cast<PhysicsDebugDrawModeFlag>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    inline PhysicsDebugDrawModeFlag operator&(PhysicsDebugDrawModeFlag lhs, PhysicsDebugDrawModeFlag rhs)
    {
        return static_cast<PhysicsDebugDrawModeFlag>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
    }

    inline PhysicsDebugDrawModeFlag operator~(PhysicsDebugDrawModeFlag value)
    {
        return static_cast<PhysicsDebugDrawModeFlag>(~static_cast<uint32_t>(value));
    }

    inline PhysicsDebugDrawModeFlag& operator|=(PhysicsDebugDrawModeFlag& lhs, PhysicsDebugDrawModeFlag rhs)
    {
        lhs = lhs | rhs;
        return lhs;
    }

    inline PhysicsDebugDrawModeFlag& operator&=(PhysicsDebugDrawModeFlag& lhs, PhysicsDebugDrawModeFlag rhs)
    {
        lhs = lhs & rhs;
        return lhs;
    }

    class ModeManager final : public PublicSingleton<ModeManager>
    {
    public:
        static bool IsEditState() { return ModeManager::GetInstance().bEditState; }
        static void ChangeState() { ModeManager::GetInstance().bEditState = !ModeManager::GetInstance().bEditState; }
    public:
        static int b3DMode;  // 0 for 2D, 1 for 3D
        static bool bShowPhysicsColliders;
        static PhysicsDebugDrawModeFlag mPhysicsDebugDrawModeFlag;
#if WITH_EDITOR
		static bool bRealtimePreview;
#endif
    private:
        static bool bEditState;
    };
}
