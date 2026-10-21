#include "Hepch.h"

#if HE_ENABLE_PHYSICS_2D
#include "Runtime/EcsFramework/System/Physics/PhysicSystem2D.h"
#endif
#if HE_ENABLE_PHYSICS_3D
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#endif

#if HE_ENABLE_PHYSICS_2D
#include <box2d/b2_body.h>
#include <box2d/b2_world.h>
#endif
#if HE_ENABLE_PHYSICS_3D
#include <Jolt/Physics/Body/BodyInterface.h>
#endif

#include <cmath>

namespace HEngine
{
#if HE_ENABLE_PHYSICS_3D
    bool PhysicSystem3D::GetBodyLinearVelocity(void* runtimeBody, glm::vec3& velocity)
    {
        if (!mPhysicsSystem || !runtimeBody) return false;
        const auto* resolved = TryGetBodyID(runtimeBody);
        if (!resolved) return false;
        const auto bodyID = *resolved;
        auto& bodies = mPhysicsSystem->GetBodyInterface();
        if (!bodies.IsAdded(bodyID)) return false;
        const auto value = bodies.GetLinearVelocity(bodyID);
        velocity = {value.GetX(), value.GetY(), value.GetZ()};
        return true;
    }

    bool PhysicSystem3D::ApplyBodyForce(void* runtimeBody, const glm::vec3& force, bool impulse)
    {
        if (!mPhysicsSystem || !runtimeBody || !std::isfinite(force.x) || !std::isfinite(force.y) || !std::isfinite(force.z)) return false;
        const auto* resolved = TryGetBodyID(runtimeBody);
        if (!resolved) return false;
        const auto bodyID = *resolved;
        auto& bodies = mPhysicsSystem->GetBodyInterface();
        if (!bodies.IsAdded(bodyID) || bodies.GetMotionType(bodyID) != JPH::EMotionType::Dynamic) return false;
        const JPH::Vec3 value(force.x, force.y, force.z);
        if (impulse) bodies.AddImpulse(bodyID, value);
        else bodies.AddForce(bodyID, value);
        return true;
    }
#endif

#if HE_ENABLE_PHYSICS_2D
    bool PhysicSystem2D::GetBodyLinearVelocity(void* runtimeBody, glm::vec2& velocity)
    {
        if (!mPhysicsWorld || !runtimeBody) return false;
        for (auto* body = mPhysicsWorld->GetBodyList(); body; body = body->GetNext())
            if (body == runtimeBody && body->IsEnabled())
            {
                const auto value = body->GetLinearVelocity();
                velocity = {value.x, value.y};
                return true;
            }
        return false;
    }

    bool PhysicSystem2D::ApplyBodyForce(void* runtimeBody, const glm::vec2& force, bool impulse)
    {
        if (!mPhysicsWorld || !runtimeBody || !std::isfinite(force.x) || !std::isfinite(force.y)) return false;
        for (auto* body = mPhysicsWorld->GetBodyList(); body; body = body->GetNext())
            if (body == runtimeBody)
            {
                if (!body->IsEnabled() || body->GetType() != b2_dynamicBody) return false;
                if (impulse) body->ApplyLinearImpulseToCenter({force.x, force.y}, true);
                else body->ApplyForceToCenter({force.x, force.y}, true);
                return true;
            }
        return false;
    }
#endif
}
