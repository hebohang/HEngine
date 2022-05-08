#include "hepch.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

namespace HEngine
{
    int ModeManager::b3DMode = 1;
    bool ModeManager::bHdrUse = false;
    bool ModeManager::bEditState = true;
    SceneMode ModeManager::mSceneMode = SceneMode::None;
    EditMode ModeManager::mEditMode = EditMode::Select;
}