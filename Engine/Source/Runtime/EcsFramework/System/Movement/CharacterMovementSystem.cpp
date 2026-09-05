#include "Hepch.h"

#include "Runtime/EcsFramework/System/Movement/CharacterMovementSystem.h"
#include "Runtime/Core/Input/Input.h"
#include "Runtime/Core/Input/KeyCodes.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Camera/FollowCameraComponent.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/CapsuleCollider3DComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"

#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

namespace HEngine
{
		namespace
		{
		struct RuntimeCharacterController
		{
			JPH::RefConst<JPH::Shape> Shape;
			JPH::Ref<JPH::CharacterVirtualSettings> Settings;
			std::unique_ptr<JPH::CharacterVirtual> Character;
			JPH::CharacterVirtual::ExtendedUpdateSettings ExtendedSettings;
		};

		class EmptyBodyFilter final : public JPH::BodyFilter
		{
		};

		class EmptyShapeFilter final : public JPH::ShapeFilter
		{
		};

		EmptyBodyFilter gEmptyBodyFilter;
		EmptyShapeFilter gEmptyShapeFilter;

		static float MoveTowards(float current, float target, float maxDelta)
		{
			if (glm::abs(target - current) <= maxDelta)
				return target;

			return current + glm::sign(target - current) * maxDelta;
		}

		static float MoveTowardsAngle(float current, float target, float maxDelta)
		{
			const float delta = std::remainder(target - current, glm::two_pi<float>());
			if (glm::abs(delta) <= maxDelta)
				return target;

			return current + glm::sign(delta) * maxDelta;
		}

		static glm::vec3 FlatForwardFromYaw(float yaw)
		{
			return glm::normalize(glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)));
		}

		static float ResolveControlYaw(Level* level, Entity controlledEntity)
		{
			auto followView = level->mRegistry.view<TransformComponent, FollowCameraComponent>();
			for (auto e : followView)
			{
				auto [cameraTransform, follow] = followView.get<TransformComponent, FollowCameraComponent>(e);
				if (follow.bHasTarget && follow.TargetUUID == controlledEntity.UUID())
					return follow.RuntimeYawRadians;
			}

			return level->GetWorldRotation(controlledEntity).y;
		}

		static RuntimeCharacterController* GetRuntimeController(CharacterMovementComponent& movement)
		{
			return reinterpret_cast<RuntimeCharacterController*>(movement.RuntimeCharacter);
		}

		static void DestroyRuntimeController(CharacterMovementComponent& movement)
		{
			auto* runtime = GetRuntimeController(movement);
			if (!runtime)
				return;
			delete runtime;
			movement.RuntimeCharacter = nullptr;
			movement.RuntimeVelocity = glm::vec3(0.0f);
			movement.RuntimeDesiredVelocity = glm::vec3(0.0f);
			movement.RuntimeHasExternalVelocity = false;
			movement.RuntimeJumpHeld = false;
		}

		static bool EnsureRuntimeController(Entity entity, PhysicSystem3D* physicsSystem)
		{
			auto& movement = entity.GetComponent<CharacterMovementComponent>();
			auto* runtime = GetRuntimeController(movement);
			if (runtime)
				return true;

			if (!entity.HasComponent<CapsuleCollider3DComponent>())
				return false;

			auto& capsule = entity.GetComponent<CapsuleCollider3DComponent>();
			Level* level = entity.GetLevel();
			const glm::vec3 worldScale = level->GetWorldScale(entity);
			const glm::vec3 worldPosition = level->GetWorldTranslation(entity);
			const glm::vec3 worldRotation = level->GetWorldRotation(entity);

			const float radius = std::max(0.05f, capsule.Radius * std::max(worldScale.x, worldScale.z));
			const float scaledHeight = std::max(2.0f * radius + 0.01f, capsule.Height * worldScale.y);
			const float halfCylinderHeight = std::max(0.01f, (scaledHeight - 2.0f * radius) * 0.5f);

			JPH::CapsuleShapeSettings shapeSettings(halfCylinderHeight, radius);
			auto shapeResult = shapeSettings.Create();
			if (!shapeResult.IsValid())
			{
				HE_CORE_ERROR("CharacterMovementSystem: failed to create character capsule for entity='{0}', error='{1}'",
					entity.GetName(), shapeResult.GetError().c_str());
				return false;
			}

			auto* created = new RuntimeCharacterController();
			created->Shape = shapeResult.Get();
			created->Settings = new JPH::CharacterVirtualSettings();
			created->Settings->mShape = created->Shape;
			created->Settings->mMaxSlopeAngle = glm::radians(glm::clamp(movement.MaxSlopeAngleDegrees, 1.0f, 89.0f));
			created->Settings->mMass = std::max(1.0f, 70.0f);
			created->Settings->mShapeOffset = Utils::GlmToJoltVec3(capsule.Center);
			created->Settings->mEnhancedInternalEdgeRemoval = true;

			created->ExtendedSettings.mStickToFloorStepDown = JPH::Vec3(0.0f, -std::max(0.0f, movement.StickToFloorDistance), 0.0f);
			created->ExtendedSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, std::max(0.0f, movement.StepUpHeight), 0.0f);

			created->Character = std::make_unique<JPH::CharacterVirtual>(
				created->Settings.GetPtr(),
				Utils::GlmToJoltRVec3(worldPosition),
				JPH::Quat::sEulerAngles(Utils::GlmToJoltVec3(worldRotation)),
				physicsSystem->GetPhysicsSystem());

			movement.RuntimeCharacter = created;
			return true;
		}

		static glm::vec2 ReadMoveInput()
		{
			glm::vec2 input = glm::vec2(0.0f);
			if (Input::IsKeyPressed(HE_KEY_A)) input.x -= 1.0f;
			if (Input::IsKeyPressed(HE_KEY_D)) input.x += 1.0f;
			if (Input::IsKeyPressed(HE_KEY_W)) input.y += 1.0f;
			if (Input::IsKeyPressed(HE_KEY_S)) input.y -= 1.0f;

			if (glm::length2(input) > 1.0f)
				input = glm::normalize(input);

			return input;
		}

		static bool IsSprintInputPressed()
		{
			return Input::IsKeyPressed(HE_KEY_LEFT_SHIFT) || Input::IsKeyPressed(HE_KEY_RIGHT_SHIFT);
		}

		static float ComputeLocomotionSpeedParam(float planarSpeed, float walkSpeed, float runSpeed)
		{
			const float safeWalk = std::max(0.01f, walkSpeed);
			if (runSpeed <= safeWalk + 0.001f)
				return planarSpeed / safeWalk;

			if (planarSpeed <= safeWalk)
				return planarSpeed / safeWalk;

			const float runRange = std::max(0.01f, runSpeed - safeWalk);
			return 1.0f + (planarSpeed - safeWalk) / runRange;
		}
	}

	bool CharacterMovementSystem::Teleport(Entity entity, const glm::vec3& worldPosition)
	{
		if (!entity || entity.GetLevel() != mLevel ||
			!entity.HasComponent<TransformComponent>() ||
			!entity.HasComponent<CharacterMovementComponent>())
		{
			return false;
		}

		auto& movement = entity.GetComponent<CharacterMovementComponent>();
		auto* runtime = GetRuntimeController(movement);
		if (!runtime || !runtime->Character)
			return false;

		glm::vec3 currentWorldPosition(0.0f);
		glm::vec3 worldRotation(0.0f);
		glm::vec3 worldScale(1.0f);
		if (!mLevel->GetWorldTransformComponents(entity, currentWorldPosition, worldRotation, worldScale))
			return false;

		runtime->Character->SetPosition(Utils::GlmToJoltRVec3(worldPosition));
		runtime->Character->SetLinearVelocity(JPH::Vec3::sZero());
		movement.RuntimeVelocity = glm::vec3(0.0f);
		movement.RuntimeDesiredVelocity = glm::vec3(0.0f);
		movement.RuntimeHasExternalVelocity = false;
		movement.RuntimeHasLastInputYaw = false;
		movement.RuntimeJumpHeld = false;
		mLevel->SetWorldTransformComponents(entity, worldPosition, worldRotation, worldScale);
		return true;
	}

	void CharacterMovementSystem::OnUpdateRuntime(Timestep ts)
	{
		HE_PROFILE;

		const float dt = static_cast<float>(ts);
		if (dt <= 0.0f)
			return;

		auto* physicsSystem = mLevel->TryGetSystem<PhysicSystem3D>();
		if (!physicsSystem || !physicsSystem->GetPhysicsSystem() || !physicsSystem->GetTempAllocator())
			return;

		auto view = mLevel->mRegistry.view<TransformComponent, CharacterMovementComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& movement = view.get<CharacterMovementComponent>(e);

			if (!EnsureRuntimeController(entity, physicsSystem))
				continue;

			auto* runtime = GetRuntimeController(movement);
			if (!runtime || !runtime->Character)
				continue;

			const glm::vec2 input = movement.bPlayerInputEnabled ? ReadMoveInput() : glm::vec2(0.0f);
			const bool bHasInput = glm::length2(input) > 0.0001f;
			const bool bSprint = bHasInput && movement.bEnableSprint && IsSprintInputPressed();

			const float controlYaw = ResolveControlYaw(mLevel, entity);
			const glm::vec3 forward = FlatForwardFromYaw(controlYaw);
			const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
			const glm::vec3 desiredMoveDirection = bHasInput ? glm::normalize(right * input.x + forward * input.y) : glm::vec3(0.0f);

			const float sprintSpeedMultiplier = std::max(1.0f, movement.SprintSpeedMultiplier);
			const float targetMaxSpeed = movement.MaxSpeed * (bSprint ? sprintSpeedMultiplier : 1.0f);
			glm::vec3 desiredVelocity = desiredMoveDirection * targetMaxSpeed;
			if (movement.RuntimeHasExternalVelocity)
			{
				desiredVelocity = movement.RuntimeDesiredVelocity;
				movement.RuntimeHasExternalVelocity = false;
			}

			const float sprintAccelMultiplier = std::max(1.0f, movement.SprintAccelerationMultiplier);
			const float accelRate = movement.Acceleration * (bSprint ? sprintAccelMultiplier : 1.0f);
			const float maxDelta = (bHasInput ? accelRate : movement.Deceleration) * dt;
			movement.RuntimeVelocity.x = MoveTowards(movement.RuntimeVelocity.x, desiredVelocity.x, maxDelta);
			movement.RuntimeVelocity.z = MoveTowards(movement.RuntimeVelocity.z, desiredVelocity.z, maxDelta);

			const JPH::CharacterBase::EGroundState groundState = runtime->Character->GetGroundState();
			const bool bIsGrounded = groundState == JPH::CharacterBase::EGroundState::OnGround || groundState == JPH::CharacterBase::EGroundState::OnSteepGround;
			const bool bJumpPressed = movement.bPlayerInputEnabled && Input::IsKeyPressed(HE_KEY_SPACE);
			const bool bStartJump = movement.bEnableJump && bJumpPressed && !movement.RuntimeJumpHeld && bIsGrounded;
			movement.RuntimeJumpHeld = bJumpPressed;
			if (bStartJump)
				movement.RuntimeVelocity.y = std::max(0.0f, movement.JumpSpeed);
			else if (bIsGrounded)
				movement.RuntimeVelocity.y = 0.0f;
			else
				movement.RuntimeVelocity.y += movement.Gravity * dt;

			runtime->Settings->mMaxSlopeAngle = glm::radians(glm::clamp(movement.MaxSlopeAngleDegrees, 1.0f, 89.0f));
			runtime->Character->SetMaxSlopeAngle(runtime->Settings->mMaxSlopeAngle);
			runtime->ExtendedSettings.mStickToFloorStepDown = bStartJump
				? JPH::Vec3::sZero()
				: JPH::Vec3(0.0f, -std::max(0.0f, movement.StickToFloorDistance), 0.0f);
			runtime->ExtendedSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, std::max(0.0f, movement.StepUpHeight), 0.0f);

			runtime->Character->SetLinearVelocity(Utils::GlmToJoltVec3(movement.RuntimeVelocity));
			runtime->Character->ExtendedUpdate(
				dt,
				JPH::Vec3(0.0f, movement.Gravity, 0.0f),
				runtime->ExtendedSettings,
				physicsSystem->GetPhysicsSystem()->GetDefaultBroadPhaseLayerFilter(PhysicSystem3D::ObjectLayerMoving),
				physicsSystem->GetPhysicsSystem()->GetDefaultLayerFilter(PhysicSystem3D::ObjectLayerMoving),
				gEmptyBodyFilter,
				gEmptyShapeFilter,
				*physicsSystem->GetTempAllocator());

			movement.RuntimeVelocity = Utils::JoltVec3ToGlm(runtime->Character->GetLinearVelocity());

			glm::vec3 worldTranslation(0.0f);
			glm::vec3 worldRotation(0.0f);
			glm::vec3 worldScale(1.0f);
			mLevel->GetWorldTransformComponents(entity, worldTranslation, worldRotation, worldScale);
			worldTranslation = Utils::JoltRVec3ToGlm(runtime->Character->GetPosition());

			const glm::vec2 planarVelocity = glm::vec2(movement.RuntimeVelocity.x, movement.RuntimeVelocity.z);
			if (movement.bOrientRotationToMovement && glm::length2(planarVelocity) > 0.0001f)
			{
				const float targetYaw = std::atan2(planarVelocity.x, planarVelocity.y);
				const float maxYawDelta = movement.TurnRate * dt;
				const float yawDelta = std::remainder(targetYaw - worldRotation.y, glm::two_pi<float>());
				const float clampedDelta = glm::clamp(yawDelta, -maxYawDelta, maxYawDelta);
				worldRotation.y += clampedDelta;
			}

			mLevel->SetWorldTransformComponents(entity, worldTranslation, worldRotation, worldScale);

			if (mLevel->mRegistry.all_of<AnimatorComponent>(e))
			{
				auto& animator = mLevel->mRegistry.get<AnimatorComponent>(e);
				const float speed = glm::length(planarVelocity);
				const float sprintSpeedMultiplier = std::max(1.0f, movement.SprintSpeedMultiplier);
				const float runSpeed = movement.MaxSpeed * sprintSpeedMultiplier;
				const float speedParam = ComputeLocomotionSpeedParam(speed, movement.MaxSpeed, runSpeed);

				float targetDirection = 0.0f;
				if (bHasInput)
				{
					const float inputYaw = std::atan2(input.x, input.y);
					if (!movement.RuntimeHasLastInputYaw)
					{
						movement.RuntimeLastInputYaw = inputYaw;
						movement.RuntimeHasLastInputYaw = true;
					}

					const float deltaYaw = std::remainder(inputYaw - movement.RuntimeLastInputYaw, glm::two_pi<float>());
					movement.RuntimeLastInputYaw = inputYaw;

					const float turnNorm = deltaYaw / glm::half_pi<float>();
					targetDirection = glm::clamp(turnNorm, -1.0f, 1.0f);
				}
				else
				{
					movement.RuntimeHasLastInputYaw = false;
				}

				const float directionLerpRate = glm::max(8.0f, movement.TurnRate);
				movement.RuntimeAnimDirection = MoveTowards(movement.RuntimeAnimDirection, targetDirection, directionLerpRate * dt);
				const bool bInAir = runtime->Character->GetGroundState() == JPH::CharacterBase::EGroundState::InAir;

				if (animator.HasParameter("Speed"))
					animator.SetFloat("Speed", speedParam);
				if (animator.HasParameter("Direction"))
					animator.SetFloat("Direction", movement.RuntimeAnimDirection);
				if (animator.HasParameter("IsRunning"))
					animator.SetBool("IsRunning", bSprint);
				if (animator.HasParameter("IsInAir"))
					animator.SetBool("IsInAir", bInAir);
				if (animator.HasParameter("StartMove") && bHasInput && speed > 0.1f)
					animator.SetTrigger("StartMove");
			}
		}
	}

	void CharacterMovementSystem::OnRuntimeStop()
	{
		auto view = mLevel->mRegistry.view<CharacterMovementComponent>();
		for (auto e : view)
		{
			auto& movement = view.get<CharacterMovementComponent>(e);
			DestroyRuntimeController(movement);
		}
	}
}
