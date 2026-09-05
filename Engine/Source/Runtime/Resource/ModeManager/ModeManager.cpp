#include "Hepch.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

namespace HEngine
{
    int ModeManager::b3DMode = 1;
    bool ModeManager::bEditState = true;
    EditMode ModeManager::mEditMode = EditMode::Select;
    bool ModeManager::bShowPhysicsColliders = false;
    PhysicsDebugDrawModeFlag ModeManager::mPhysicsDebugDrawModeFlag =
        PhysicsDebugDrawModeFlag::DrawShape | PhysicsDebugDrawModeFlag::DrawShapeWireframe;
#if WITH_EDITOR
	bool ModeManager::bRealtimePreview = false;
#endif
}
