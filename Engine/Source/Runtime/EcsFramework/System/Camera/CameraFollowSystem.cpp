#include "Hepch.h"

#include "Runtime/EcsFramework/System/Camera/CameraFollowSystem.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/Input/InputActions.h"
#include "Runtime/Camera/CameraSpring.h"
#include "Runtime/Core/Input/MouseCodes.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/FollowCameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/SpringArmComponent.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/EcsFramework/System/Movement/CharacterPresentation.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/Utils/MathUtils/Rotation.h"

#if HE_ENABLE_PHYSICS_3D
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Math/MathTypes.h>
#endif

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

			glm::vec3 targetWorldTranslation = level->GetWorldTranslation(targetEntity);
            if (bRuntime)
                if (const auto offset = GetCharacterPresentationOffset(*level, targetEntity))
                    targetWorldTranslation += *offset;
			const glm::vec3 targetWorldRotation = level->GetWorldRotation(targetEntity);
			glm::vec3 pivot = targetWorldTranslation + springArm.TargetOffset;

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

            if (bRuntime && springArm.bEnableLag && springArm.RuntimeCurrentDistance >= 0.0f &&
                glm::distance(pivot, springArm.RuntimePivot) < std::max(0.1f, springArm.LagResetDistance))
            {
                AdvanceCameraSpring(springArm.RuntimePivot, springArm.RuntimePivotVelocity, pivot,
                    glm::vec3(springArm.LagSpeed, springArm.VerticalLagSpeed, springArm.LagSpeed), dt);
                pivot = springArm.RuntimePivot;
            }
            else springArm.RuntimePivotVelocity = glm::vec3(0.0f);
            springArm.RuntimePivot = pivot;

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

                // Look is an angular rate, independent of mouse capture/hold.
                // Reuse camera sensitivity (including gameplay aim scaling) at
                // an equivalent full-stick rate of 600 mouse pixels per second.
                const glm::vec2 stickDelta = InputActions::Get().ReadOrDefault("Look").Value *
                    (600.0f * followComp.MouseSensitivity * std::max(0.0f, dt));

                if (followComp.Mode == FollowCameraMode::TopDown)
                {
                    if (followComp.bAllowYawInput)
                    {
                        followComp.RuntimeYawRadians += mouseDelta.x * followComp.MouseSensitivity + stickDelta.x;
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
                    if (followComp.bAllowYawInput)
                        followComp.RuntimeYawRadians -= stickDelta.x;
                    if (followComp.bAllowPitchInput)
                        followComp.RuntimePitchRadians += stickDelta.y;
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

            const bool bInitializeArm = springArm.RuntimeCurrentDistance < 0.0f;

            float collisionDistance = targetDistance;
            const bool bCollisionEnabledForMode = springArm.bEnableCollision &&
                (followComp.Mode != FollowCameraMode::TopDown || springArm.bEnableTopDownCollision);

#if HE_ENABLE_PHYSICS_3D
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

#endif
            if (bRuntime)
            {
                if (bInitializeArm)
                {
                    // Start at the collision-safe distance rather than sweeping
                    // the camera through nearby geometry on the first frames.
                    springArm.RuntimeCurrentDistance = collisionDistance;
                }
                else
                {
                    const float distanceLerpSpeed = (collisionDistance < springArm.RuntimeCurrentDistance)
                        ? springArm.CollisionSnapSpeed
                        : springArm.CollisionRecoverSpeed;
                    springArm.RuntimeCurrentDistance = collisionDistance < springArm.RuntimeCurrentDistance
                        ? collisionDistance
                        : glm::mix(springArm.RuntimeCurrentDistance, collisionDistance, InterpAlpha(distanceLerpSpeed, dt));
                }
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

            // Orbit the filtered pivot directly. A second position filter changes
            // view direction on stairs and can drag the camera through collision.
            cameraWorldTranslation = desiredPos;

			const glm::vec3 up = followComp.bLockToWorldUp ? glm::vec3(0.0f, 1.0f, 0.0f) : (yawRotation * glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::vec3 lookAtTarget = pivot + yawRotation * springArm.LookAtOffset;
			const glm::mat4 viewMatrix = glm::lookAt(cameraWorldTranslation, lookAtTarget, up);
			const glm::quat worldRotation = glm::quat_cast(glm::inverse(viewMatrix));
			cameraWorldRotation = Math::StableEulerAngles(worldRotation);
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

}
