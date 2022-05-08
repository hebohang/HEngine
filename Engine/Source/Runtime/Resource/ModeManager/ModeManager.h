#pragma once

#include "Runtime/Core/Base/PublicSingleton.h"

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

    class ModeManager final : public PublicSingleton<ModeManager>
    {
    public:
        static bool IsEditState() { return ModeManager::GetInstance().bEditState; }
        static void ChangeState() { ModeManager::GetInstance().bEditState = !ModeManager::GetInstance().bEditState; }
    public:
        static int b3DMode;  // 0 for 2D, 1 for 3D
        static bool bHdrUse;
        static SceneMode mSceneMode;
    private:
        static bool bEditState;
        static EditMode mEditMode;
    };
}