#include "Hepch.h"
#include "CharacterMovementSystem.h"
#include "Runtime/Core/BuildFeatures.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

namespace HEngine
{
    void CharacterMovementSystem::OnUpdateRuntime(Timestep) {}
    void CharacterMovementSystem::CaptureInputFrame(bool) {}
    void CharacterMovementSystem::OnRuntimeStop() {}
    bool CharacterMovementSystem::Teleport(Entity, const glm::vec3&)
    {
        RequireBuildFeature(BuildFeature::Physics3D, "Character teleport");
        return false;
    }
    bool PhysicSystem3D::SnapEntityToFloor(Entity) { return false; }
}
