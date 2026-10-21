#include "Hepch.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

namespace HEngine
{
    int ModeManager::b3DMode = 1;
    bool ModeManager::bEditState = true;
    bool ModeManager::bShowPhysicsColliders = false;
    PhysicsDebugDrawModeFlag ModeManager::mPhysicsDebugDrawModeFlag =
        PhysicsDebugDrawModeFlag::DrawShape | PhysicsDebugDrawModeFlag::DrawShapeWireframe;
#if WITH_EDITOR
	bool ModeManager::bRealtimePreview = false;
#endif
}
