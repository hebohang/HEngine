#include "Hepch.h"

#include "Runtime/EcsFramework/System/Camera/CameraFollowSystem.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/Input/MouseCodes.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/FollowCameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/SpringArmComponent.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/UUID.h"

#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Math/MathTypes.h>

#include <cmath>
#include <unordered_map>
#include <algorithm>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

namespace HEngine
{
    namespace
    {
        static bool IsRuntimeCursorCaptured()
        {
            return Application::GetWindow().IsMouseCursorCaptured();
        }

        static glm::vec3 ForwardFromYawPitch(float yawRadians, float pitchRadians)
        {
            const float cosPitch = std::cos(pitchRadians);
            return glm::normalize(glm::vec3(
                std::sin(yawRadians) * cosPitch,
                std::sin(pitchRadians),
                std::cos(yawRadians) * cosPitch));
        }

        static float InterpAlpha(float speed, float dt)
        {
            if (speed <= 0.0f || dt <= 0.0f)
                return 1.0f;

            return 1.0f - std::exp(-speed * dt);
        }

		static void UpdateFollowCameraEntity(
			Level* level,
			Entity cameraEntity,
			FollowCameraComponent& followComp,
			SpringArmComponent& springArm,
			float dt,
            bool bRuntime,
            std::unordered_map<UUID, bool>* lastCursorCapturedByCamera)
        {
            if (!followComp.bHasTarget)
                return;

            if (bRuntime)
            {
                followComp.RuntimeMissingTargetWarnCooldown = std::max(0.0f, followComp.RuntimeMissingTargetWarnCooldown - dt);
                followComp.RuntimeMissingTransformWarnCooldown = std::max(0.0f, followComp.RuntimeMissingTransformWarnCooldown - dt);
            }

            Entity targetEntity = level->TryGetEntityByUUID(followComp.TargetUUID);
            if (!targetEntity)
            {
                if (bRuntime && followComp.RuntimeMissingTargetWarnCooldown <= 0.0f)
                {
                    HE_CORE_WARN("CameraFollowSystem: target missing for camera entity={0}", static_cast<uint32_t>(cameraEntity));
                    followComp.RuntimeMissingTargetWarnCooldown = 5.0f;
                }
                return;
            }

            if (!targetEntity.HasComponent<TransformComponent>())
            {
                if (bRuntime && followComp.RuntimeMissingTransformWarnCooldown <= 0.0f)
                {
                    HE_CORE_WARN("CameraFollowSystem: target has no TransformComponent, camera entity={0}", static_cast<uint32_t>(cameraEntity));
                    followComp.RuntimeMissingTransformWarnCooldown = 5.0f;
                }
				return;
			}

			glm::vec3 cameraWorldTranslation(0.0f);
			glm::vec3 cameraWorldRotation(0.0f);
			glm::vec3 cameraWorldScale(1.0f);
			if (!level->GetWorldTransformComponents(cameraEntity, cameraWorldTranslation, cameraWorldRotation, cameraWorldScale))
				return;

			const glm::vec3 targetWorldTranslation = level->GetWorldTranslation(targetEntity);
			const glm::vec3 targetWorldRotation = level->GetWorldRotation(targetEntity);
			const glm::vec3 pivot = targetWorldTranslation + springArm.TargetOffset;

			if (!followComp.bRuntimeInitialized)
			{
                followComp.RuntimeLastMousePos = Input::GetMousePosition();
                if (followComp.Mode == FollowCameraMode::TopDown)
                {
                    followComp.RuntimeYawRadians = glm::radians(followComp.TopDownYawDegrees);
                    followComp.RuntimePitchRadians = glm::radians(followComp.TopDownPitchDegrees);
				}
				else
				{
					followComp.RuntimeYawRadians = targetWorldRotation.y;
					followComp.RuntimePitchRadians = glm::radians(followComp.InitialPitchDegrees);
				}
                springArm.RuntimeCurrentDistance = -1.0f;
                followComp.bRuntimeInitialized = true;
            }

            glm::vec2 mouseDelta = glm::vec2(0.0f);
            if (bRuntime)
            {
                const glm::vec2 mousePos = Input::GetMousePosition();

                bool bRotateInputEnabled =
                    (!followComp.bRequireHoldToRotate) || Input::IsMouseButtonPressed(Mouse::ButtonRight);

                if (followComp.Mode == FollowCameraMode::ThirdPerson)
                {
                    const bool bCursorCaptured = IsRuntimeCursorCaptured();
                    if (bCursorCaptured)
                        bRotateInputEnabled = true;

                    if (lastCursorCapturedByCamera)
                    {
                        const UUID cameraUUID = cameraEntity.UUID();
                        const auto previousCaptureIt = lastCursorCapturedByCamera->find(cameraUUID);
                        const bool bWasCaptured = previousCaptureIt != lastCursorCapturedByCamera->end() ? previousCaptureIt->second : bCursorCaptured;
                        if (!bWasCaptured && bCursorCaptured)
                            followComp.RuntimeLastMousePos = mousePos;
                        (*lastCursorCapturedByCamera)[cameraUUID] = bCursorCaptured;
                    }
                }

                if (bRotateInputEnabled)
                    mouseDelta = mousePos - followComp.RuntimeLastMousePos;

                followComp.RuntimeLastMousePos = mousePos;

                if (followComp.Mode == FollowCameraMode::TopDown)
                {
                    if (followComp.bAllowYawInput && bRotateInputEnabled)
                    {
                        followComp.RuntimeYawRadians += mouseDelta.x * followComp.MouseSensitivity;
                        followComp.RuntimeYawRadians = std::remainder(
                            followComp.RuntimeYawRadians,
                            glm::two_pi<float>());
                    }
                }
				else
				{
					if (followComp.bFollowTargetRotation)
						followComp.RuntimeYawRadians = targetWorldRotation.y;

					if (bRotateInputEnabled)
                    {
                        if (followComp.bAllowYawInput)
                            followComp.RuntimeYawRadians -= mouseDelta.x * followComp.MouseSensitivity;
                        if (followComp.bAllowPitchInput)
                            followComp.RuntimePitchRadians -= mouseDelta.y * followComp.MouseSensitivity;
                    }
                }
            }
            else
            {
                if (followComp.Mode == FollowCameraMode::TopDown)
                {
					followComp.RuntimeYawRadians = glm::radians(followComp.TopDownYawDegrees);
					followComp.RuntimePitchRadians = glm::radians(followComp.TopDownPitchDegrees);
				}
				else if (followComp.bFollowTargetRotation)
				{
					followComp.RuntimeYawRadians = targetWorldRotation.y;
				}
			}

            const float minPitchRadians = glm::radians(followComp.MinPitchDegrees);
            const float maxPitchRadians = glm::radians(followComp.MaxPitchDegrees);
            followComp.RuntimePitchRadians = glm::clamp(followComp.RuntimePitchRadians, minPitchRadians, maxPitchRadians);

            const glm::quat yawRotation = glm::angleAxis(followComp.RuntimeYawRadians, glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::vec3 socketOffset = yawRotation * springArm.SocketOffset;

            const glm::vec3 forward = ForwardFromYawPitch(followComp.RuntimeYawRadians, followComp.RuntimePitchRadians);
            const float targetDistance = (followComp.Mode == FollowCameraMode::TopDown)
                ? std::max(0.1f, followComp.TopDownDistance)
                : std::max(0.1f, springArm.ArmLength);

            if (springArm.RuntimeCurrentDistance < 0.0f)
                springArm.RuntimeCurrentDistance = targetDistance;

            float collisionDistance = targetDistance;
            const bool bCollisionEnabledForMode = springArm.bEnableCollision &&
                (followComp.Mode != FollowCameraMode::TopDown || springArm.bEnableTopDownCollision);

            if (bRuntime && bCollisionEnabledForMode)
            {
                if (auto* physicsSystem = level->TryGetSystem<PhysicSystem3D>())
                {
                    if (auto* joltSystem = physicsSystem->GetPhysicsSystem())
                    {
                        const float probeRadius = std::max(0.02f, springArm.ProbeRadius);
                        const glm::vec3 startPos = pivot + socketOffset;
                        const glm::vec3 targetPos = startPos - forward * targetDistance;

                        struct IgnoreTargetBodyFilter final : JPH::BodyFilter
                        {
                            const JPH::BodyID* IgnoreBody = nullptr;

                            bool ShouldCollide(const JPH::BodyID& inBodyID) const override
                            {
                                return !(IgnoreBody && inBodyID == *IgnoreBody);
                            }
                        };

                        IgnoreTargetBodyFilter bodyFilter;
                        if (targetEntity.HasComponent<Rigidbody3DComponent>())
                        {
                            const auto& targetRb = targetEntity.GetComponent<Rigidbody3DComponent>();
                            bodyFilter.IgnoreBody = PhysicSystem3D::TryGetBodyID(targetRb.RuntimeBody);
                        }

                        const JPH::RVec3 castStart = Utils::GlmToJoltRVec3(startPos);
                        const JPH::Vec3 castDirection = Utils::GlmToJoltVec3(targetPos - startPos);

                        JPH::SphereShape probeShape(probeRadius);
                        JPH::RShapeCast shapeCast(
                            &probeShape,
                            JPH::Vec3::sReplicate(1.0f),
                            JPH::RMat44::sTranslation(castStart),
                            castDirection);

                        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> castCollector;
                        joltSystem->GetNarrowPhaseQuery().CastShape(
                            shapeCast,
                            JPH::ShapeCastSettings(),
                            JPH::RVec3::sZero(),
                            castCollector,
                            joltSystem->GetDefaultBroadPhaseLayerFilter(PhysicSystem3D::ObjectLayerMoving),
                            joltSystem->GetDefaultLayerFilter(PhysicSystem3D::ObjectLayerMoving),
                            bodyFilter,
                            JPH::ShapeFilter());

                        if (castCollector.HadHit())
                        {
                            collisionDistance = targetDistance * castCollector.mHit.mFraction;
                        }
                        else
                        {
                            JPH::RRayCast rayCast(castStart, castDirection);
                            JPH::RayCastResult rayResult;
                            if (joltSystem->GetNarrowPhaseQuery().CastRay(
                                rayCast,
                                rayResult,
                                joltSystem->GetDefaultBroadPhaseLayerFilter(PhysicSystem3D::ObjectLayerMoving),
                                joltSystem->GetDefaultLayerFilter(PhysicSystem3D::ObjectLayerMoving),
                                bodyFilter))
                            {
                                const JPH::RVec3 hitPoint = rayCast.GetPointOnRay(rayResult.mFraction);
                                const glm::vec3 hit = Utils::JoltRVec3ToGlm(hitPoint);
                                collisionDistance = glm::length(hit - startPos);
                            }
                        }

                        collisionDistance = std::max(springArm.MinArmLength, collisionDistance - probeRadius);
                    }
                }
            }

            if (bRuntime)
            {
                const float distanceLerpSpeed = (collisionDistance < springArm.RuntimeCurrentDistance)
                    ? springArm.CollisionSnapSpeed
                    : springArm.CollisionRecoverSpeed;
                springArm.RuntimeCurrentDistance = glm::mix(
                    springArm.RuntimeCurrentDistance,
                    collisionDistance,
                    InterpAlpha(distanceLerpSpeed, dt));
            }
            else
            {
                springArm.RuntimeCurrentDistance = collisionDistance;
            }

            const float finalDistance = std::clamp(
                springArm.RuntimeCurrentDistance,
                std::max(0.05f, springArm.MinArmLength),
                targetDistance);

			const glm::vec3 desiredPos = pivot + socketOffset - forward * finalDistance;

			const float alpha = (bRuntime && springArm.bEnableLag) ? InterpAlpha(springArm.LagSpeed, dt) : 1.0f;
			cameraWorldTranslation = glm::mix(cameraWorldTranslation, desiredPos, alpha);

			const glm::vec3 up = followComp.bLockToWorldUp ? glm::vec3(0.0f, 1.0f, 0.0f) : (yawRotation * glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::mat4 viewMatrix = glm::lookAt(cameraWorldTranslation, pivot, up);
			const glm::quat worldRotation = glm::quat_cast(glm::inverse(viewMatrix));
			cameraWorldRotation = glm::eulerAngles(worldRotation);
			level->SetWorldTransformComponents(cameraEntity, cameraWorldTranslation, cameraWorldRotation, cameraWorldScale);
		}
	}

    void CameraFollowSystem::OnUpdateRuntime(Timestep ts)
    {
		HE_PROFILE;

        const float dt = static_cast<float>(ts);
        static std::unordered_map<UUID, bool> sLastCursorCapturedByCamera;

        auto view = mLevel->mRegistry.view<TransformComponent, CameraComponent, FollowCameraComponent, SpringArmComponent>();
		for (auto e : view)
		{
			Entity cameraEntity = { e, mLevel };
			auto [followComp, springArm] = view.get<FollowCameraComponent, SpringArmComponent>(e);

            if (mLevel->IsRuntimePaused())
            {
                followComp.RuntimeLastMousePos = Input::GetMousePosition();
                sLastCursorCapturedByCamera[cameraEntity.UUID()] = IsRuntimeCursorCaptured();
                continue;
            }

			UpdateFollowCameraEntity(
				mLevel,
				cameraEntity,
				followComp,
				springArm,
                dt,
                true,
                &sLastCursorCapturedByCamera);
        }
    }

#if WITH_EDITOR
    void CameraFollowSystem::OnUpdateEditor(Timestep ts, EditorCamera& camera)
    {
        (void)camera;

        const float dt = static_cast<float>(ts);
        auto view = mLevel->mRegistry.view<TransformComponent, CameraComponent, FollowCameraComponent, SpringArmComponent>();
		for (auto e : view)
		{
			Entity cameraEntity = { e, mLevel };
			auto [followComp, springArm] = view.get<FollowCameraComponent, SpringArmComponent>(e);

			UpdateFollowCameraEntity(
				mLevel,
				cameraEntity,
				followComp,
				springArm,
                dt,
                false,
                nullptr);
        }
    }
#endif
}
