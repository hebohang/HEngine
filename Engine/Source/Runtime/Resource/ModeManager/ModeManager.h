#pragma once

#include "Runtime/Core/Base/PublicSingleton.h"

#include <magic_enum.hpp>

namespace HEngine
{
    // From UE4.27.2
    // TODO
    enum class EditMode
    {
        Select = 0,
        Landscape = 1,
        Foliage = 2,
        BrushEditing = 3,
        MeshPaint = 4
    };

    enum class SceneMode
    {
        None = 0,
        EnvironmentHdr = 1,
        SkyBox = 2,
    };

    enum class PhysicsDebugDrawModeFlag
    {
        HEngine_NoDebug = 0,
        HEngine_DrawWireframe = 1,
        HEngine_DrawAabb = 2,
        HEngine_DrawFeaturesText = 4,
        HEngine_DrawContactPoints = 8,
        HEngine_NoDeactivation = 16,
        HEngine_NoHelpText = 32,
        HEngine_DrawText = 64,
        HEngine_ProfileTimings = 128,
        HEngine_EnableSatComparison = 256,
        HEngine_DisableBulletLCP = 512,
        HEngine_EnableCCD = 1024,
        HEngine_DrawConstraints = (1 << 11),
        HEngine_DrawConstraintLimits = (1 << 12),
        HEngine_FastWireframe = (1 << 13),
        HEngine_DrawNormals = (1 << 14),
        HEngine_DrawFrames = (1 << 15),
        HEngine_MAX_DEBUG_DRAW_MODE
    };

    class ModeManager final : public PublicSingleton<ModeManager>
    {
    public:
        static bool IsEditState() { return ModeManager::GetInstance().bEditState; }
        static void ChangeState() { ModeManager::GetInstance().bEditState = !ModeManager::GetInstance().bEditState; }
    public:
        static int b3DMode;  // 0 for 2D, 1 for 3D
        static bool bHdrUse;
        static SceneMode mSceneMode;
        static bool bShowPhysicsColliders;
        static PhysicsDebugDrawModeFlag mPhysicsDebugDrawModeFlag;
    private:
        static bool bEditState;
        static EditMode mEditMode;
    };
}