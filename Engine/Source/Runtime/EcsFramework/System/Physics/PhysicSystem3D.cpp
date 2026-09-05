#include "Hepch.h"

#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/CapsuleCollider3DComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/ScopeExit.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Physics/Character/CharacterVirtual.h>
#endif

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <vector>

#include <glm/gtx/quaternion.hpp>

namespace HEngine
{
		namespace
		{
			constexpr uint32_t kMaxBodies = 65536;
		constexpr uint32_t kNumBodyMutexes = 0;
		constexpr uint32_t kMaxBodyPairs = 65536;
		constexpr uint32_t kMaxContactConstraints = 10240;
		constexpr size_t kTempAllocatorSize = 64 * 1024 * 1024;
			constexpr uint32_t kJobSystemMaxJobs = 4096;
			constexpr uint32_t kJobSystemMaxBarriers = 32;

			constexpr float kMaxPhysicsDeltaTime = 1.0f / 30.0f;
			constexpr float kEditorFloorSnapLiftPadding = 0.1f;
			constexpr float kEditorFloorSnapMinLiftDistance = 1.0f;
			constexpr float kEditorFloorSnapMaxDistance = 100000.0f;
			constexpr float kEditorFloorSnapMoveEpsilon = 0.0005f;

		static void* EncodeRuntimeBodyHandle(JPH::BodyID bodyID)
		{
			if (bodyID.IsInvalid())
				return nullptr;

			const uintptr_t encoded = static_cast<uintptr_t>(bodyID.GetIndexAndSequenceNumber()) + 1u;
			return reinterpret_cast<void*>(encoded);
		}

		static bool DecodeRuntimeBodyHandle(const void* runtimeBody, JPH::BodyID& outBodyID)
		{
			if (!runtimeBody)
				return false;

			const uintptr_t raw = reinterpret_cast<uintptr_t>(runtimeBody);
			if (raw == 0)
				return false;

			outBodyID = JPH::BodyID(static_cast<JPH::uint32>(raw - 1u));
			return !outBodyID.IsInvalid();
		}

		class EmptyBodyFilter final : public JPH::BodyFilter
		{
		};

		class EmptyShapeFilter final : public JPH::ShapeFilter
		{
		};

		EmptyBodyFilter gEmptyBodyFilter;
		EmptyShapeFilter gEmptyShapeFilter;

		static bool GetEntityWorldTransformComponents(Entity entity, glm::vec3& outTranslation, glm::vec3& outRotation, glm::vec3& outScale)
		{
			Level* level = entity.GetLevel();
			return level && level->GetWorldTransformComponents(entity, outTranslation, outRotation, outScale);
		}

		void JoltTraceImpl(const char* inFmt, ...)
		{
			char buffer[1024] = { 0 };
			va_list args;
			va_start(args, inFmt);
			vsnprintf(buffer, sizeof(buffer), inFmt, args);
			va_end(args);
			HE_CORE_INFO("Jolt: {0}", buffer);
		}

		void LogPhysicsCleanupFailure(const char* stage, const std::exception_ptr& failure) noexcept
		{
			if (!failure)
				return;

			try
			{
				std::rethrow_exception(failure);
			}
			catch (const std::exception& e)
			{
				try
				{
					HE_CORE_ERROR("Jolt: cleanup stage '{0}' failed: {1}", stage, e.what());
				}
				catch (...)
				{
				}
			}
			catch (...)
			{
				try
				{
					HE_CORE_ERROR("Jolt: cleanup stage '{0}' failed with an unknown exception", stage);
				}
				catch (...)
				{
				}
			}
		}

		bool JoltAssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, JPH::uint inLine)
		{
			if (inMessage)
				HE_CORE_ERROR("Jolt Assert: expr='{0}' message='{1}' file='{2}' line={3}", inExpression, inMessage, inFile, inLine);
			else
				HE_CORE_ERROR("Jolt Assert: expr='{0}' file='{1}' line={2}", inExpression, inFile, inLine);
			return true;
		}

		JPH::EMotionType ToJoltMotionType(Rigidbody3DComponent::Body3DType type)
		{
			switch (type)
			{
			case Rigidbody3DComponent::Body3DType::Static:
				return JPH::EMotionType::Static;
			case Rigidbody3DComponent::Body3DType::Kinematic:
				return JPH::EMotionType::Kinematic;
			case Rigidbody3DComponent::Body3DType::Dynamic:
			default:
				return JPH::EMotionType::Dynamic;
			}
		}

		JPH::BodyCreationSettings CreateBodySettings(const JPH::Shape* shape, Entity entity, const Rigidbody3DComponent& rb3d)
		{
			const glm::vec3 worldTranslation = entity.GetLevel()->GetWorldTranslation(entity);
			const glm::vec3 worldRotation = entity.GetLevel()->GetWorldRotation(entity);
			const JPH::RVec3 position = Utils::GlmToJoltRVec3(worldTranslation);
			const JPH::Quat rotation = JPH::Quat::sEulerAngles(Utils::GlmToJoltVec3(worldRotation));
			const JPH::EMotionType motionType = ToJoltMotionType(rb3d.Type);
			const JPH::ObjectLayer layer = rb3d.Type == Rigidbody3DComponent::Body3DType::Static ? PhysicSystem3D::ObjectLayerStatic : PhysicSystem3D::ObjectLayerMoving;

			JPH::BodyCreationSettings settings(shape, position, rotation, motionType, layer);
			settings.mLinearDamping = rb3d.linearDamping;
			settings.mAngularDamping = rb3d.angularDamping;
			settings.mRestitution = rb3d.restitution;
			settings.mFriction = rb3d.friction;
			settings.mIsSensor = rb3d.IsTrigger;
			settings.mAllowSleeping = rb3d.AllowSleeping;
			settings.mMotionQuality = rb3d.EnableCCD ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;

			if (rb3d.Type == Rigidbody3DComponent::Body3DType::Dynamic)
			{
				float clampedMass = std::max(0.001f, rb3d.mass);
				JPH::MassProperties shapeMassProps = shape->GetMassProperties();
				float shapeMass = shapeMassProps.mMass > 0.001f ? shapeMassProps.mMass : 1.0f;
				settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
				settings.mMassPropertiesOverride.mMass = clampedMass;
				settings.mMassPropertiesOverride.mInertia = shapeMassProps.mInertia * (clampedMass / shapeMass);
			}

			if (rb3d.Type == Rigidbody3DComponent::Body3DType::Kinematic)
				settings.mAllowDynamicOrKinematic = true;

			return settings;
		}

			bool HasFloorSnapSourceCollision(Entity entity)
			{
				if (!entity || !entity.HasComponent<TransformComponent>())
					return false;

				if (entity.HasComponent<CapsuleCollider3DComponent>())
					return true;

				if (!entity.HasComponent<Rigidbody3DComponent>())
					return false;

				return entity.GetComponent<Rigidbody3DComponent>().Shape != CollisionShape::None;
			}

			bool IsFloorSnapBlockingTarget(Entity entity)
			{
				if (!HasFloorSnapSourceCollision(entity))
					return false;

				if (entity.HasComponent<Rigidbody3DComponent>() && entity.GetComponent<Rigidbody3DComponent>().IsTrigger)
					return false;

				if (entity.HasComponent<CapsuleCollider3DComponent>() && entity.GetComponent<CapsuleCollider3DComponent>().IsTrigger)
					return false;

				return true;
			}

			JPH::RefConst<JPH::Shape> BuildQueryShapeForEntity(Entity entity)
			{
				const Rigidbody3DComponent* rb3d = entity.HasComponent<Rigidbody3DComponent>() ? &entity.GetComponent<Rigidbody3DComponent>() : nullptr;
				const CapsuleCollider3DComponent* capsuleCollider = entity.HasComponent<CapsuleCollider3DComponent>() ? &entity.GetComponent<CapsuleCollider3DComponent>() : nullptr;
				if (!rb3d && !capsuleCollider)
					return nullptr;

				const glm::vec3 worldScale = entity.GetLevel()->GetWorldScale(entity);

				const glm::vec3 colliderScale = rb3d ? rb3d->ColliderScale : glm::vec3(1.0f);
				const glm::vec3 combinedScale = glm::abs(worldScale * colliderScale);
				const glm::vec3 safeScale(
					std::max(0.01f, combinedScale.x),
					std::max(0.01f, combinedScale.y),
					std::max(0.01f, combinedScale.z));

				auto applyLocalColliderTransform = [&](const JPH::RefConst<JPH::Shape>& baseShape) -> JPH::RefConst<JPH::Shape>
				{
					if (!baseShape)
						return nullptr;

					glm::vec3 localOffset = rb3d ? rb3d->ColliderOffset : glm::vec3(0.0f);
					if (capsuleCollider)
						localOffset += capsuleCollider->Center;

					const glm::vec3 localRotation = rb3d ? rb3d->ColliderRotation : glm::vec3(0.0f);
					const bool bHasOffset = glm::dot(localOffset, localOffset) > 0.000001f;
					const bool bHasRotation = glm::dot(localRotation, localRotation) > 0.000001f;
					if (!bHasOffset && !bHasRotation)
						return baseShape;

				JPH::RotatedTranslatedShapeSettings shapeSettings(
					Utils::GlmToJoltVec3(localOffset),
					JPH::Quat::sEulerAngles(Utils::GlmToJoltVec3(localRotation)),
					baseShape.GetPtr());
				auto result = shapeSettings.Create();
				if (result.IsValid())
					return result.Get();

				HE_CORE_ERROR("Jolt: create rotated translated shape failed: {0}", result.GetError().c_str());
				return baseShape;
			};

				if (capsuleCollider)
				{
					const float radius = std::max(0.01f, capsuleCollider->Radius * std::max(safeScale.x, safeScale.z));
					const float scaledHeight = std::max(2.0f * radius + 0.01f, capsuleCollider->Height * safeScale.y);
					const float halfCylinderHeight = std::max(0.005f, (scaledHeight - 2.0f * radius) * 0.5f);
					JPH::CapsuleShapeSettings shapeSettings(halfCylinderHeight, radius);
					auto result = shapeSettings.Create();
					if (result.IsValid())
						return applyLocalColliderTransform(result.Get());
					HE_CORE_ERROR("Jolt: create capsule shape failed: {0}", result.GetError().c_str());
					return nullptr;
				}

				if (!rb3d || rb3d->Shape == CollisionShape::None)
					return nullptr;

				if (rb3d->Shape == CollisionShape::Sphere)
				{
					const float radius = std::max(0.01f, std::max(safeScale.x, std::max(safeScale.y, safeScale.z)));
					JPH::SphereShapeSettings shapeSettings(radius);
				auto result = shapeSettings.Create();
				if (result.IsValid())
					return applyLocalColliderTransform(result.Get());
				HE_CORE_ERROR("Jolt: create sphere shape failed: {0}", result.GetError().c_str());
				return nullptr;
			}

				if (rb3d->Shape == CollisionShape::ConvexHull && entity.HasComponent<MeshComponent>())
				{
					auto& meshComp = entity.GetComponent<MeshComponent>();
					JPH::Array<JPH::Vec3> points;
					if (meshComp.mMesh)
					{
						for (const auto& submesh : meshComp.mMesh->mSubMeshes)
						{
							for (const auto& vertex : submesh->mStaticVertices)
							{
								points.push_back(JPH::Vec3(
									vertex.Pos.x * safeScale.x,
									vertex.Pos.y * safeScale.y,
									vertex.Pos.z * safeScale.z));
							}

							for (const auto& vertex : submesh->mSkinnedVertices)
							{
								points.push_back(JPH::Vec3(
									vertex.Pos.x * safeScale.x,
									vertex.Pos.y * safeScale.y,
									vertex.Pos.z * safeScale.z));
							}
						}
					}

				if (!points.empty())
				{
					JPH::ConvexHullShapeSettings shapeSettings(points);
					auto result = shapeSettings.Create();
					if (result.IsValid())
						return applyLocalColliderTransform(result.Get());
					HE_CORE_ERROR("Jolt: create convex hull shape failed: {0}", result.GetError().c_str());
				}
			}

			JPH::AABox bounds;
			if (entity.HasComponent<MeshComponent>())
			{
				auto& meshComp = entity.GetComponent<MeshComponent>();
				if (meshComp.mMesh)
				{
					for (const auto& submesh : meshComp.mMesh->mSubMeshes)
					{
						for (const auto& vertex : submesh->mStaticVertices)
						{
							bounds.Encapsulate(JPH::Vec3(
								vertex.Pos.x * safeScale.x,
								vertex.Pos.y * safeScale.y,
								vertex.Pos.z * safeScale.z));
						}

						for (const auto& vertex : submesh->mSkinnedVertices)
						{
							bounds.Encapsulate(JPH::Vec3(
								vertex.Pos.x * safeScale.x,
								vertex.Pos.y * safeScale.y,
								vertex.Pos.z * safeScale.z));
						}
					}
				}
			}

			if (!bounds.IsValid())
				bounds = JPH::AABox(JPH::Vec3(-0.5f, -0.5f, -0.5f), JPH::Vec3(0.5f, 0.5f, 0.5f));

			JPH::Vec3 halfExtent = bounds.GetExtent();
			halfExtent = JPH::Vec3(
				std::max(0.01f, halfExtent.GetX()),
				std::max(0.01f, halfExtent.GetY()),
				std::max(0.01f, halfExtent.GetZ()));

			JPH::BoxShapeSettings shapeSettings(halfExtent);
			auto result = shapeSettings.Create();
			if (result.IsValid())
				return applyLocalColliderTransform(result.Get());

				HE_CORE_ERROR("Jolt: create box shape failed: {0}", result.GetError().c_str());
				return nullptr;
			}

#if WITH_EDITOR && !defined(JPH_DEBUG_RENDERER)
		glm::vec3 TransformDebugPoint(const glm::mat4& transform, const glm::vec3& point)
		{
			return glm::vec3(transform * glm::vec4(point, 1.0f));
		}

		void DrawDebugSegment(const glm::mat4& transform, const glm::vec3& from, const glm::vec3& to, const glm::vec4& color)
		{
			Renderer2D::DrawLine(TransformDebugPoint(transform, from), TransformDebugPoint(transform, to), color);
		}

		void DrawDebugBox(const glm::mat4& transform, const glm::vec3& minimum, const glm::vec3& maximum, const glm::vec4& color)
		{
			const glm::vec3 corners[8] = {
				{ minimum.x, minimum.y, minimum.z }, { maximum.x, minimum.y, minimum.z },
				{ maximum.x, maximum.y, minimum.z }, { minimum.x, maximum.y, minimum.z },
				{ minimum.x, minimum.y, maximum.z }, { maximum.x, minimum.y, maximum.z },
				{ maximum.x, maximum.y, maximum.z }, { minimum.x, maximum.y, maximum.z }
			};
			constexpr int edges[12][2] = {
				{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
				{ 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
				{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
			};
			for (const auto& edge : edges)
				DrawDebugSegment(transform, corners[edge[0]], corners[edge[1]], color);
		}

		void DrawDebugCircle(const glm::mat4& transform, float radius, int axis, const glm::vec3& center, const glm::vec4& color)
		{
			constexpr int segmentCount = 32;
			constexpr float twoPi = 6.28318530717958647692f;
			for (int segment = 0; segment < segmentCount; ++segment)
			{
				const float angle0 = twoPi * static_cast<float>(segment) / static_cast<float>(segmentCount);
				const float angle1 = twoPi * static_cast<float>(segment + 1) / static_cast<float>(segmentCount);
				glm::vec3 point0 = center;
				glm::vec3 point1 = center;
				const int axis0 = (axis + 1) % 3;
				const int axis1 = (axis + 2) % 3;
				point0[axis0] += std::cos(angle0) * radius;
				point0[axis1] += std::sin(angle0) * radius;
				point1[axis0] += std::cos(angle1) * radius;
				point1[axis1] += std::sin(angle1) * radius;
				DrawDebugSegment(transform, point0, point1, color);
			}
		}

		void DrawDebugSphere(const glm::mat4& transform, float radius, const glm::vec4& color)
		{
			DrawDebugCircle(transform, radius, 0, glm::vec3(0.0f), color);
			DrawDebugCircle(transform, radius, 1, glm::vec3(0.0f), color);
			DrawDebugCircle(transform, radius, 2, glm::vec3(0.0f), color);
		}

		void DrawDebugCapsule(const glm::mat4& transform, float radius, float halfCylinderHeight, const glm::vec4& color)
		{
			const glm::vec3 topCenter(0.0f, halfCylinderHeight, 0.0f);
			const glm::vec3 bottomCenter(0.0f, -halfCylinderHeight, 0.0f);
			DrawDebugCircle(transform, radius, 1, topCenter, color);
			DrawDebugCircle(transform, radius, 1, bottomCenter, color);

			constexpr int arcSegments = 16;
			constexpr float pi = 3.14159265358979323846f;
			for (int plane = 0; plane < 2; ++plane)
			{
				for (int segment = 0; segment < arcSegments; ++segment)
				{
					const float angle0 = pi * static_cast<float>(segment) / static_cast<float>(arcSegments);
					const float angle1 = pi * static_cast<float>(segment + 1) / static_cast<float>(arcSegments);
					glm::vec3 top0(0.0f, halfCylinderHeight + std::sin(angle0) * radius, 0.0f);
					glm::vec3 top1(0.0f, halfCylinderHeight + std::sin(angle1) * radius, 0.0f);
					glm::vec3 bottom0(0.0f, -halfCylinderHeight - std::sin(angle0) * radius, 0.0f);
					glm::vec3 bottom1(0.0f, -halfCylinderHeight - std::sin(angle1) * radius, 0.0f);
					if (plane == 0)
					{
						top0.x = std::cos(angle0) * radius; top1.x = std::cos(angle1) * radius;
						bottom0.x = std::cos(angle0) * radius; bottom1.x = std::cos(angle1) * radius;
					}
					else
					{
						top0.z = std::cos(angle0) * radius; top1.z = std::cos(angle1) * radius;
						bottom0.z = std::cos(angle0) * radius; bottom1.z = std::cos(angle1) * radius;
					}
					DrawDebugSegment(transform, top0, top1, color);
					DrawDebugSegment(transform, bottom0, bottom1, color);
				}
			}

			DrawDebugSegment(transform, { radius, -halfCylinderHeight, 0.0f }, { radius, halfCylinderHeight, 0.0f }, color);
			DrawDebugSegment(transform, { -radius, -halfCylinderHeight, 0.0f }, { -radius, halfCylinderHeight, 0.0f }, color);
			DrawDebugSegment(transform, { 0.0f, -halfCylinderHeight, radius }, { 0.0f, halfCylinderHeight, radius }, color);
			DrawDebugSegment(transform, { 0.0f, -halfCylinderHeight, -radius }, { 0.0f, halfCylinderHeight, -radius }, color);
		}

		bool GetMeshDebugBounds(Entity entity, glm::vec3& outMinimum, glm::vec3& outMaximum)
		{
			if (!entity.HasComponent<MeshComponent>())
				return false;
			const auto& meshComponent = entity.GetComponent<MeshComponent>();
			if (!meshComponent.mMesh)
				return false;

			bool bHasVertex = false;
			outMinimum = glm::vec3(std::numeric_limits<float>::max());
			outMaximum = glm::vec3(std::numeric_limits<float>::lowest());
			for (const auto& submesh : meshComponent.mMesh->mSubMeshes)
			{
				if (!submesh)
					continue;
				for (const auto& vertex : submesh->mStaticVertices)
				{
					outMinimum = glm::min(outMinimum, vertex.Pos);
					outMaximum = glm::max(outMaximum, vertex.Pos);
					bHasVertex = true;
				}
				for (const auto& vertex : submesh->mSkinnedVertices)
				{
					outMinimum = glm::min(outMinimum, vertex.Pos);
					outMaximum = glm::max(outMaximum, vertex.Pos);
					bHasVertex = true;
				}
			}
			return bHasVertex;
		}

		void DrawEditorColliderWireframe(Entity entity)
		{
			const Rigidbody3DComponent* rigidbody = entity.HasComponent<Rigidbody3DComponent>()
				? &entity.GetComponent<Rigidbody3DComponent>() : nullptr;
			const CapsuleCollider3DComponent* capsule = entity.HasComponent<CapsuleCollider3DComponent>()
				? &entity.GetComponent<CapsuleCollider3DComponent>() : nullptr;
			if ((!rigidbody || rigidbody->Shape == CollisionShape::None) && !capsule)
				return;

			glm::vec3 worldTranslation(0.0f);
			glm::vec3 worldRotation(0.0f);
			glm::vec3 worldScale(1.0f);
			if (!entity.GetLevel()->GetWorldTransformComponents(entity, worldTranslation, worldRotation, worldScale))
				return;

			const glm::vec3 colliderScale = rigidbody ? rigidbody->ColliderScale : glm::vec3(1.0f);
			const glm::vec3 safeScale = glm::max(glm::abs(worldScale * colliderScale), glm::vec3(0.01f));
			glm::vec3 localOffset = rigidbody ? rigidbody->ColliderOffset : glm::vec3(0.0f);
			if (capsule)
				localOffset += capsule->Center;
			const glm::vec3 localRotation = rigidbody ? rigidbody->ColliderRotation : glm::vec3(0.0f);
			const glm::mat4 colliderTransform =
				glm::translate(glm::mat4(1.0f), worldTranslation) * glm::toMat4(glm::quat(worldRotation)) *
				glm::translate(glm::mat4(1.0f), localOffset) * glm::toMat4(glm::quat(localRotation));
			const glm::vec4 color = (rigidbody && rigidbody->IsTrigger) || (capsule && capsule->IsTrigger)
				? glm::vec4(1.0f, 0.55f, 0.15f, 1.0f) : glm::vec4(0.20f, 1.0f, 0.35f, 1.0f);

			if (capsule)
			{
				const float radius = std::max(0.01f, capsule->Radius * std::max(safeScale.x, safeScale.z));
				const float scaledHeight = std::max(radius * 2.0f + 0.01f, capsule->Height * safeScale.y);
				DrawDebugCapsule(colliderTransform, radius, (scaledHeight - radius * 2.0f) * 0.5f, color);
				return;
			}

			if (rigidbody->Shape == CollisionShape::Sphere)
			{
				DrawDebugSphere(colliderTransform, std::max(safeScale.x, std::max(safeScale.y, safeScale.z)), color);
				return;
			}

			glm::vec3 boundsMinimum(-0.5f);
			glm::vec3 boundsMaximum(0.5f);
			GetMeshDebugBounds(entity, boundsMinimum, boundsMaximum);
			DrawDebugBox(colliderTransform, boundsMinimum * safeScale, boundsMaximum * safeScale, color);
		}

		void DrawEditorColliderWireframes(Level* level)
		{
			auto rigidbodyView = level->mRegistry.view<TransformComponent, Rigidbody3DComponent>();
			for (auto handle : rigidbodyView)
			{
				Entity entity{ handle, level };
				if (level->IsEntityVisibleInEditor(entity))
					DrawEditorColliderWireframe(entity);
			}

			auto characterView = level->mRegistry.view<TransformComponent, CharacterMovementComponent, CapsuleCollider3DComponent>();
			for (auto handle : characterView)
			{
				if (level->mRegistry.all_of<Rigidbody3DComponent>(handle))
					continue;
				Entity entity{ handle, level };
				if (level->IsEntityVisibleInEditor(entity))
					DrawEditorColliderWireframe(entity);
			}
		}
#endif

		#ifdef JPH_DEBUG_RENDERER
		JPH::RefConst<JPH::Shape> BuildCharacterShapeForEntity(Entity entity)
		{
			if (!entity.HasComponent<CharacterMovementComponent>() || !entity.HasComponent<CapsuleCollider3DComponent>())
				return nullptr;

			const auto& capsule = entity.GetComponent<CapsuleCollider3DComponent>();
			const glm::vec3 absScale = glm::abs(entity.GetLevel()->GetWorldScale(entity));
			const float radius = std::max(0.05f, capsule.Radius * std::max(absScale.x, absScale.z));
			const float scaledHeight = std::max(2.0f * radius + 0.01f, capsule.Height * absScale.y);
			const float halfCylinderHeight = std::max(0.01f, (scaledHeight - 2.0f * radius) * 0.5f);

			JPH::CapsuleShapeSettings shapeSettings(halfCylinderHeight, radius);
			auto shapeResult = shapeSettings.Create();
			if (!shapeResult.IsValid())
			{
				HE_CORE_ERROR("Jolt: create character capsule debug shape failed for entity='{0}', error='{1}'",
					entity.GetName(), shapeResult.GetError().c_str());
				return nullptr;
			}

			if (glm::dot(capsule.Center, capsule.Center) <= 0.000001f)
				return shapeResult.Get();

			JPH::RotatedTranslatedShapeSettings transformedShapeSettings(
				Utils::GlmToJoltVec3(capsule.Center),
				JPH::Quat::sIdentity(),
				shapeResult.Get().GetPtr());
			auto transformedResult = transformedShapeSettings.Create();
			if (transformedResult.IsValid())
				return transformedResult.Get();

			HE_CORE_ERROR("Jolt: create character rotated translated debug shape failed for entity='{0}', error='{1}'",
				entity.GetName(), transformedResult.GetError().c_str());
			return shapeResult.Get();
		}

		JPH::RMat44 BuildEntityCenterOfMassTransform(Entity entity, const JPH::Shape& shape)
		{
			const glm::vec3 worldTranslation = entity.GetLevel()->GetWorldTranslation(entity);
			const glm::vec3 worldRotation = entity.GetLevel()->GetWorldRotation(entity);
			return JPH::RMat44::sRotationTranslation(
				JPH::Quat::sEulerAngles(Utils::GlmToJoltVec3(worldRotation)),
				Utils::GlmToJoltRVec3(worldTranslation)).PreTranslated(shape.GetCenterOfMass());
		}

		void DrawEntityDebugShape(Entity entity, JoltDebugRendererAdapter& debugRenderer, bool bDrawShape, bool bDrawWireframe, bool bDrawBoundingBox, JPH::ColorArg color)
		{
			if (!bDrawShape && !bDrawBoundingBox)
				return;

			JPH::RefConst<JPH::Shape> shape;
			if (entity.HasComponent<Rigidbody3DComponent>())
				shape = BuildQueryShapeForEntity(entity);
			else
				shape = BuildCharacterShapeForEntity(entity);

			if (!shape)
				return;

			const JPH::RMat44 worldTransform = BuildEntityCenterOfMassTransform(entity, *shape);
			if (bDrawBoundingBox)
				debugRenderer.DrawWireBox(shape->GetWorldSpaceBounds(worldTransform, JPH::Vec3::sOne()), color);

			if (bDrawShape)
				shape->Draw(&debugRenderer, worldTransform, JPH::Vec3::sOne(), color, false, bDrawWireframe);
		}

		void DrawCharacterDebugShapes(Level* level, JoltDebugRendererAdapter& debugRenderer, bool bDrawShape, bool bDrawWireframe, bool bDrawBoundingBox, JPH::ColorArg color)
		{
			auto characterView = level->mRegistry.view<TransformComponent, CharacterMovementComponent, CapsuleCollider3DComponent>();
			for (auto e : characterView)
			{
				if (level->mRegistry.all_of<Rigidbody3DComponent>(e))
					continue;

				DrawEntityDebugShape({ e, level }, debugRenderer, bDrawShape, bDrawWireframe, bDrawBoundingBox, color);
			}
		}
		#endif
	}

	PhysicsDebugDrawModeFlag PhysicSystem3D::mDebugMode = PhysicsDebugDrawModeFlag::DrawShape | PhysicsDebugDrawModeFlag::DrawShapeWireframe;
	int PhysicSystem3D::sJoltInitRefCount = 0;

	PhysicSystem3D::~PhysicSystem3D() noexcept
	{
		try
		{
			CleanupRuntimeState();
		}
		catch (...)
		{
			LogPhysicsCleanupFailure("runtime destruction", std::current_exception());
		}

		#ifdef JPH_DEBUG_RENDERER
		mDebugRenderer.reset();
		#endif
		ReleaseEditorDebugJoltIfNeeded();
	}

	void PhysicSystem3D::SetDebugMode(PhysicsDebugDrawModeFlag flags)
	{
		mDebugMode = flags;
	}

	void PhysicSystem3D::ApplyCharacterVirtualDebugFlags() const
	{
		#ifdef JPH_DEBUG_RENDERER
		const bool bDrawConstraints = (mDebugMode & PhysicsDebugDrawModeFlag::CharacterVirtualDrawConstraints) != PhysicsDebugDrawModeFlag::None;
		const bool bDrawWalkStairs = (mDebugMode & PhysicsDebugDrawModeFlag::CharacterVirtualDrawWalkStairs) != PhysicsDebugDrawModeFlag::None;
		const bool bDrawStickToFloor = (mDebugMode & PhysicsDebugDrawModeFlag::CharacterVirtualDrawStickToFloor) != PhysicsDebugDrawModeFlag::None;

		JPH::CharacterVirtual::sDrawConstraints = bDrawConstraints;
		JPH::CharacterVirtual::sDrawWalkStairs = bDrawWalkStairs;
		JPH::CharacterVirtual::sDrawStickToFloor = bDrawStickToFloor;
		#endif
	}

	#ifdef JPH_DEBUG_RENDERER
	JPH::BodyManager::DrawSettings PhysicSystem3D::BuildJoltDrawSettings() const
	{
		JPH::BodyManager::DrawSettings settings;
		settings.mDrawShape = (mDebugMode & PhysicsDebugDrawModeFlag::DrawShape) != PhysicsDebugDrawModeFlag::None;
		settings.mDrawShapeWireframe = (mDebugMode & PhysicsDebugDrawModeFlag::DrawShapeWireframe) != PhysicsDebugDrawModeFlag::None;
		settings.mDrawBoundingBox = (mDebugMode & PhysicsDebugDrawModeFlag::DrawBoundingBox) != PhysicsDebugDrawModeFlag::None;
		settings.mDrawCenterOfMassTransform = (mDebugMode & PhysicsDebugDrawModeFlag::DrawCenterOfMassTransform) != PhysicsDebugDrawModeFlag::None;
		settings.mDrawWorldTransform = (mDebugMode & PhysicsDebugDrawModeFlag::DrawWorldTransform) != PhysicsDebugDrawModeFlag::None;
		settings.mDrawVelocity = (mDebugMode & PhysicsDebugDrawModeFlag::DrawVelocity) != PhysicsDebugDrawModeFlag::None;
		settings.mDrawMassAndInertia = (mDebugMode & PhysicsDebugDrawModeFlag::DrawMassAndInertia) != PhysicsDebugDrawModeFlag::None;
		settings.mDrawSleepStats = (mDebugMode & PhysicsDebugDrawModeFlag::DrawSleepStats) != PhysicsDebugDrawModeFlag::None;
		return settings;
	}
	#endif

	void PhysicSystem3D::EnsureJoltInitialized()
	{
		if (sJoltInitRefCount == 0)
		{
			JPH::Trace = JoltTraceImpl;
#ifdef JPH_ENABLE_ASSERTS
			JPH::AssertFailed = JoltAssertFailedImpl;
#endif
			JPH::RegisterDefaultAllocator();
			auto factory = std::make_unique<JPH::Factory>();
			JPH::Factory::sInstance = factory.get();
			try
			{
				JPH::RegisterTypes();
			}
			catch (...)
			{
				const std::exception_ptr initializationFailure = std::current_exception();
				try
				{
					JPH::UnregisterTypes();
				}
				catch (...)
				{
					LogPhysicsCleanupFailure("Jolt type registration rollback", std::current_exception());
				}
				JPH::Factory::sInstance = nullptr;
				std::rethrow_exception(initializationFailure);
			}
			factory.release();
		}
		++sJoltInitRefCount;
	}

	void PhysicSystem3D::EnsureEditorDebugJoltInitialized()
	{
		if (mRuntimeState != RuntimeLifecycleState::Stopped || mRuntimeJoltInitialized || mEditorJoltInitialized)
			return;

		EnsureJoltInitialized();
		mEditorJoltInitialized = true;
	}

	void PhysicSystem3D::ShutdownJoltIfNeeded() noexcept
	{
		if (sJoltInitRefCount <= 0)
			return;

		--sJoltInitRefCount;
		if (sJoltInitRefCount == 0)
		{
			try
			{
				JPH::UnregisterTypes();
			}
			catch (...)
			{
				LogPhysicsCleanupFailure("Jolt type shutdown", std::current_exception());
			}

			try
			{
				delete JPH::Factory::sInstance;
			}
			catch (...)
			{
				LogPhysicsCleanupFailure("Jolt factory shutdown", std::current_exception());
			}
			JPH::Factory::sInstance = nullptr;
		}
	}

	void PhysicSystem3D::ReleaseRuntimeJoltIfNeeded() noexcept
	{
		if (!mRuntimeJoltInitialized)
			return;

		mRuntimeJoltInitialized = false;
		ShutdownJoltIfNeeded();
	}

	void PhysicSystem3D::ReleaseEditorDebugJoltIfNeeded() noexcept
	{
		if (!mEditorJoltInitialized)
			return;

		mEditorJoltInitialized = false;
		ShutdownJoltIfNeeded();
	}

	void PhysicSystem3D::OnRuntiemStart()
	{
		HE_PROFILE;

		if (mRuntimeState == RuntimeLifecycleState::Started)
			return;
		if (mRuntimeState != RuntimeLifecycleState::Stopped)
			throw std::logic_error("Jolt runtime startup requested during a lifecycle transition");

		const bool bHasPartialRuntimeState =
			mRuntimeJoltInitialized ||
			mPhysicsSystem ||
			mTempAllocator ||
			mJobSystem ||
			mBroadPhaseLayerInterface ||
			mObjectLayerPairFilter ||
			mObjectVsBroadPhaseLayerFilter;
		if (bHasPartialRuntimeState)
			CleanupRuntimeState();

		mRuntimeState = RuntimeLifecycleState::Starting;
		try
		{
			EnsureJoltInitialized();
			mRuntimeJoltInitialized = true;

			mTempAllocator = std::make_unique<JPH::TempAllocatorImpl>(kTempAllocatorSize);
			mJobSystem = std::make_unique<JPH::JobSystemThreadPool>(kJobSystemMaxJobs, kJobSystemMaxBarriers, -1);
			mPhysicsSystem = std::make_unique<JPH::PhysicsSystem>();

			mBroadPhaseLayerInterface = std::make_unique<JPH::BroadPhaseLayerInterfaceTable>(2, 2);
			mBroadPhaseLayerInterface->MapObjectToBroadPhaseLayer(ObjectLayerStatic, JPH::BroadPhaseLayer(0));
			mBroadPhaseLayerInterface->MapObjectToBroadPhaseLayer(ObjectLayerMoving, JPH::BroadPhaseLayer(1));

			mObjectLayerPairFilter = std::make_unique<JPH::ObjectLayerPairFilterTable>(2);
			mObjectLayerPairFilter->EnableCollision(ObjectLayerStatic, ObjectLayerMoving);
			mObjectLayerPairFilter->EnableCollision(ObjectLayerMoving, ObjectLayerMoving);

			mObjectVsBroadPhaseLayerFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
				*mBroadPhaseLayerInterface,
				mBroadPhaseLayerInterface->GetNumBroadPhaseLayers(),
				*mObjectLayerPairFilter,
				mObjectLayerPairFilter->GetNumObjectLayers());

			mPhysicsSystem->Init(
				kMaxBodies,
				kNumBodyMutexes,
				kMaxBodyPairs,
				kMaxContactConstraints,
				*mBroadPhaseLayerInterface,
				*mObjectVsBroadPhaseLayerFilter,
				*mObjectLayerPairFilter);
			mPhysicsSystem->SetGravity(JPH::Vec3(0.0f, -9.8f, 0.0f));
			mDebugMode = ModeManager::mPhysicsDebugDrawModeFlag;

			BuildRuntimeRigidBodies();
			ApplyCharacterVirtualDebugFlags();
			mRuntimeState = RuntimeLifecycleState::Started;
		}
		catch (...)
		{
			const std::exception_ptr startupFailure = std::current_exception();
			try
			{
				CleanupRuntimeState();
			}
			catch (...)
			{
				LogPhysicsCleanupFailure("runtime startup rollback", std::current_exception());
			}
			std::rethrow_exception(startupFailure);
		}
	}

	void PhysicSystem3D::BuildRuntimeRigidBodies()
	{
		HE_PROFILE;

		auto& bodyInterface = mPhysicsSystem->GetBodyInterface();
		auto view = mLevel->mRegistry.view<TransformComponent, Rigidbody3DComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();
			rb3d.RuntimeBody = nullptr;

			JPH::RefConst<JPH::Shape> shape = BuildQueryShapeForEntity(entity);
			if (!shape)
				continue;

			JPH::BodyCreationSettings settings = CreateBodySettings(shape, entity, rb3d);
			JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(
				settings,
				rb3d.Type == Rigidbody3DComponent::Body3DType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);

			if (bodyID.IsInvalid())
			{
				HE_CORE_ERROR("Jolt: failed to create body for entity={0}", static_cast<uint32_t>(e));
				continue;
			}

			rb3d.RuntimeBody = EncodeRuntimeBodyHandle(bodyID);
		}
	}

	void PhysicSystem3D::ClearRuntimeRigidBodies()
	{
		HE_PROFILE;

		if (!mLevel)
			return;

		std::exception_ptr firstFailure;
		auto recordFailure = [&](const char* stage)
		{
			const std::exception_ptr failure = std::current_exception();
			if (!firstFailure)
				firstFailure = failure;
			else
				LogPhysicsCleanupFailure(stage, failure);
		};

		JPH::BodyIDVector destroyedBodyIDs;
		auto view = mLevel->mRegistry.view<Rigidbody3DComponent>();
		for (auto e : view)
		{
			auto& rb3d = view.get<Rigidbody3DComponent>(e);
			JPH::BodyID bodyID;
			if (!DecodeRuntimeBodyHandle(rb3d.RuntimeBody, bodyID))
			{
				rb3d.RuntimeBody = nullptr;
				continue;
			}
			rb3d.RuntimeBody = nullptr;

			if (!mPhysicsSystem)
				continue;

			const bool bAlreadyDestroyed = std::find(destroyedBodyIDs.begin(), destroyedBodyIDs.end(), bodyID) != destroyedBodyIDs.end();
			if (bAlreadyDestroyed)
				continue;

			try
			{
				destroyedBodyIDs.push_back(bodyID);
				auto& bodyInterface = mPhysicsSystem->GetBodyInterface();
				if (bodyInterface.IsAdded(bodyID))
					bodyInterface.RemoveBody(bodyID);
				bodyInterface.DestroyBody(bodyID);
			}
			catch (...)
			{
				recordFailure("runtime body cleanup");
			}
		}

		if (firstFailure)
			std::rethrow_exception(firstFailure);
	}

	void PhysicSystem3D::OnUpdateRuntime(Timestep ts)
	{
		HE_PROFILE;

		if (mRuntimeState != RuntimeLifecycleState::Started || !mPhysicsSystem)
			return;

		const float clampedDt = std::min(static_cast<float>(ts), kMaxPhysicsDeltaTime);
		mPhysicsSystem->Update(clampedDt, 1, mTempAllocator.get(), mJobSystem.get());

		auto& bodyInterface = mPhysicsSystem->GetBodyInterface();
		auto view = mLevel->mRegistry.view<TransformComponent, Rigidbody3DComponent>();
		for (auto e : view)
		{
			Entity entity = { e, mLevel };
			auto& rb3d = view.get<Rigidbody3DComponent>(e);
			JPH::BodyID bodyID;
			if (!DecodeRuntimeBodyHandle(rb3d.RuntimeBody, bodyID))
			{
				rb3d.RuntimeBody = nullptr;
				continue;
			}

			if (!bodyInterface.IsAdded(bodyID))
			{
				rb3d.RuntimeBody = nullptr;
				continue;
			}

			const JPH::RVec3 position = bodyInterface.GetPosition(bodyID);
			const JPH::Quat rotation = bodyInterface.GetRotation(bodyID);

			glm::vec3 worldTranslation(0.0f);
			glm::vec3 worldRotation(0.0f);
			glm::vec3 worldScale(1.0f);
			GetEntityWorldTransformComponents(entity, worldTranslation, worldRotation, worldScale);
			worldTranslation = Utils::JoltRVec3ToGlm(position);
			worldRotation = glm::eulerAngles(glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()));
			mLevel->SetWorldTransformComponents(entity, worldTranslation, worldRotation, worldScale);
		}

		if (ModeManager::bShowPhysicsColliders)
		{
			Entity camera = mLevel->GetPrimaryCameraEntity();
			if (camera)
			{
				auto& cameraComp = camera.GetComponent<CameraComponent>();
				Renderer2D::BeginScene(cameraComp.Camera, mLevel->GetWorldTransform(camera));
				if (mDebugMode != PhysicsDebugDrawModeFlag::None)
					DrawDebugBodies();
				Renderer2D::EndScene();
			}
		}
	}

	void PhysicSystem3D::DrawDebugBodies()
	{
		if (!mPhysicsSystem)
			return;

		ApplyCharacterVirtualDebugFlags();

		#ifdef JPH_DEBUG_RENDERER
		if (!mDebugRenderer)
		{
			mDebugRenderer = std::make_unique<JoltDebugRendererAdapter>();
			mDebugRenderer->Reserve(8192);
		}

		if (mDebugRenderer)
		{
			Entity camera = mLevel->GetPrimaryCameraEntity();
			if (camera)
				mDebugRenderer->SetCameraPos(Utils::GlmToJoltRVec3(mLevel->GetWorldTranslation(camera)));

			const JPH::BodyManager::DrawSettings drawSettings = BuildJoltDrawSettings();
			const bool bDrawShape = drawSettings.mDrawShape;
			const bool bDrawWireframe = drawSettings.mDrawShapeWireframe;
			const bool bDrawBoundingBox = drawSettings.mDrawBoundingBox;
			mDebugRenderer->Clear();
			mPhysicsSystem->DrawBodies(drawSettings, mDebugRenderer.get());
			DrawCharacterDebugShapes(mLevel, *mDebugRenderer, bDrawShape, bDrawWireframe, bDrawBoundingBox, JPH::Color::sGreen);

			if ((mDebugMode & PhysicsDebugDrawModeFlag::DrawConstraints) != PhysicsDebugDrawModeFlag::None)
				mPhysicsSystem->DrawConstraints(mDebugRenderer.get());
			if ((mDebugMode & PhysicsDebugDrawModeFlag::DrawConstraintLimits) != PhysicsDebugDrawModeFlag::None)
				mPhysicsSystem->DrawConstraintLimits(mDebugRenderer.get());
			if ((mDebugMode & PhysicsDebugDrawModeFlag::DrawConstraintReferenceFrame) != PhysicsDebugDrawModeFlag::None)
				mPhysicsSystem->DrawConstraintReferenceFrame(mDebugRenderer.get());

			mDebugRenderer->FlushToRenderer2D();
			mDebugRenderer->NextFrame();
		}
		#endif
	}

	void PhysicSystem3D::OnRuntimeStop()
	{
		CleanupRuntimeState();
	}

	void PhysicSystem3D::CleanupRuntimeState()
	{
		const bool bHasRuntimeState =
			mRuntimeState != RuntimeLifecycleState::Stopped ||
			mRuntimeJoltInitialized ||
			mPhysicsSystem ||
			mTempAllocator ||
			mJobSystem ||
			mBroadPhaseLayerInterface ||
			mObjectLayerPairFilter ||
			mObjectVsBroadPhaseLayerFilter;
		if (!bHasRuntimeState)
			return;

		mRuntimeState = RuntimeLifecycleState::Stopping;
		std::exception_ptr firstFailure;
		auto cleanupStage = [&](const char* stage, auto&& cleanup)
		{
			try
			{
				cleanup();
			}
			catch (...)
			{
				const std::exception_ptr failure = std::current_exception();
				if (!firstFailure)
					firstFailure = failure;
				else
					LogPhysicsCleanupFailure(stage, failure);
			}
		};

		cleanupStage("runtime bodies", [&]() { ClearRuntimeRigidBodies(); });
		cleanupStage("runtime debug flags", [&]()
		{
			#ifdef JPH_DEBUG_RENDERER
			JPH::CharacterVirtual::sDrawConstraints = false;
			JPH::CharacterVirtual::sDrawWalkStairs = false;
			JPH::CharacterVirtual::sDrawStickToFloor = false;
			#endif
		});
		cleanupStage("runtime debug renderer", [&]()
		{
			#ifdef JPH_DEBUG_RENDERER
			mDebugRenderer.reset();
			#endif
		});
		cleanupStage("runtime physics system", [&]() { mPhysicsSystem.reset(); });
		cleanupStage("runtime job system", [&]() { mJobSystem.reset(); });
		cleanupStage("runtime temp allocator", [&]() { mTempAllocator.reset(); });
		cleanupStage("runtime broad phase filter", [&]() { mObjectVsBroadPhaseLayerFilter.reset(); });
		cleanupStage("runtime object layer filter", [&]() { mObjectLayerPairFilter.reset(); });
		cleanupStage("runtime broad phase interface", [&]() { mBroadPhaseLayerInterface.reset(); });
		cleanupStage("runtime Jolt reference", [&]() { ReleaseRuntimeJoltIfNeeded(); });

		mRuntimeState = RuntimeLifecycleState::Stopped;
		if (firstFailure)
			std::rethrow_exception(firstFailure);
	}

#if WITH_EDITOR
	void PhysicSystem3D::OnUpdateEditor(Timestep ts, EditorCamera& camera)
	{
		(void)ts;

		if (!ModeManager::bShowPhysicsColliders)
		{
			ReleaseEditorDebugResources();
			return;
		}

		if (mDebugMode == PhysicsDebugDrawModeFlag::None)
		{
			ReleaseEditorDebugResources();
			return;
		}

		#ifdef JPH_DEBUG_RENDERER
		EnsureEditorDebugJoltInitialized();
		#endif

		Renderer2D::BeginScene(camera);

		#ifdef JPH_DEBUG_RENDERER
		if (!mDebugRenderer)
		{
			mDebugRenderer = std::make_unique<JoltDebugRendererAdapter>();
			mDebugRenderer->Reserve(8192);
		}

		if (mDebugRenderer)
		{
			const bool bDrawShape = (mDebugMode & PhysicsDebugDrawModeFlag::DrawShape) != PhysicsDebugDrawModeFlag::None;
			const bool bDrawWireframe = (mDebugMode & PhysicsDebugDrawModeFlag::DrawShapeWireframe) != PhysicsDebugDrawModeFlag::None;
			const bool bDrawBoundingBox = (mDebugMode & PhysicsDebugDrawModeFlag::DrawBoundingBox) != PhysicsDebugDrawModeFlag::None;

			mDebugRenderer->SetCameraPos(Utils::GlmToJoltRVec3(camera.GetPosition()));
			mDebugRenderer->Clear();

			auto rigidbodyView = mLevel->mRegistry.view<TransformComponent, Rigidbody3DComponent>();
			for (auto e : rigidbodyView)
			{
				Entity entity = { e, mLevel };
				if (mLevel->IsEntityVisibleInEditor(entity))
					DrawEntityDebugShape(entity, *mDebugRenderer, bDrawShape, bDrawWireframe, bDrawBoundingBox, JPH::Color::sGreen);
			}

			DrawCharacterDebugShapes(mLevel, *mDebugRenderer, bDrawShape, bDrawWireframe, bDrawBoundingBox, JPH::Color::sGreen);

			mDebugRenderer->FlushToRenderer2D();
			mDebugRenderer->NextFrame();
		}
		#endif

		#ifndef JPH_DEBUG_RENDERER
		DrawEditorColliderWireframes(mLevel);
		#endif

		Renderer2D::EndScene();
	}
#endif

	bool PhysicSystem3D::SetBodyLinearVelocity(void* runtimeBody, const glm::vec3& velocity)
	{
		if (!mPhysicsSystem || !runtimeBody)
			return false;

		JPH::BodyID bodyID;
		if (!DecodeRuntimeBodyHandle(runtimeBody, bodyID))
			return false;

		auto& bodyInterface = mPhysicsSystem->GetBodyInterface();
		if (!bodyInterface.IsAdded(bodyID))
			return false;

		bodyInterface.SetLinearVelocity(bodyID, Utils::GlmToJoltVec3(velocity));
		return true;
	}

	bool PhysicSystem3D::RaycastFloorHeight(
		const glm::vec3& position,
		float castDistanceAbove,
		float castDistanceBelow,
		float& outFloorHeight) const
	{
		if (!mPhysicsSystem)
			return false;

		const float safeDistanceAbove = std::max(0.0f, castDistanceAbove);
		const float safeDistanceBelow = std::max(0.0f, castDistanceBelow);
		const float castDistance = safeDistanceAbove + safeDistanceBelow;
		if (castDistance <= 0.0001f)
			return false;

		auto& bodyInterface = mPhysicsSystem->GetBodyInterface();
		struct StaticFloorBodyFilter final : JPH::BodyFilter
		{
			const JPH::BodyInterface* Interface = nullptr;

			bool ShouldCollide(const JPH::BodyID& bodyID) const override
			{
				return Interface &&
					Interface->GetObjectLayer(bodyID) == PhysicSystem3D::ObjectLayerStatic &&
					!Interface->IsSensor(bodyID);
			}
		};

		StaticFloorBodyFilter bodyFilter;
		bodyFilter.Interface = &bodyInterface;
		const glm::vec3 castStart = position + glm::vec3(0.0f, safeDistanceAbove, 0.0f);
		const JPH::RRayCast rayCast(
			Utils::GlmToJoltRVec3(castStart),
			JPH::Vec3(0.0f, -castDistance, 0.0f));
		JPH::RayCastResult result;
		if (!mPhysicsSystem->GetNarrowPhaseQuery().CastRay(
			rayCast,
			result,
			mPhysicsSystem->GetDefaultBroadPhaseLayerFilter(ObjectLayerMoving),
			mPhysicsSystem->GetDefaultLayerFilter(ObjectLayerMoving),
			bodyFilter))
		{
			return false;
		}

		outFloorHeight = static_cast<float>(rayCast.GetPointOnRay(result.mFraction).GetY());
		return std::isfinite(outFloorHeight);
	}

	bool PhysicSystem3D::SnapEntityToFloor(Entity entity)
	{
		if (!entity || entity.GetLevel() != mLevel || !HasFloorSnapSourceCollision(entity))
			return false;

		EnsureJoltInitialized();
		JPH::ScopeExit joltRefGuard([this]() noexcept { ShutdownJoltIfNeeded(); });

		JPH::RefConst<JPH::Shape> sourceShape = BuildQueryShapeForEntity(entity);
		if (!sourceShape)
			return false;

		auto broadPhaseLayerInterface = std::make_unique<JPH::BroadPhaseLayerInterfaceTable>(2, 2);
		broadPhaseLayerInterface->MapObjectToBroadPhaseLayer(ObjectLayerStatic, JPH::BroadPhaseLayer(0));
		broadPhaseLayerInterface->MapObjectToBroadPhaseLayer(ObjectLayerMoving, JPH::BroadPhaseLayer(1));

		auto objectLayerPairFilter = std::make_unique<JPH::ObjectLayerPairFilterTable>(2);
		objectLayerPairFilter->EnableCollision(ObjectLayerStatic, ObjectLayerMoving);
		objectLayerPairFilter->EnableCollision(ObjectLayerMoving, ObjectLayerMoving);

		auto objectVsBroadPhaseLayerFilter = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
			*broadPhaseLayerInterface,
			broadPhaseLayerInterface->GetNumBroadPhaseLayers(),
			*objectLayerPairFilter,
			objectLayerPairFilter->GetNumObjectLayers());

		JPH::PhysicsSystem queryPhysicsSystem;

		queryPhysicsSystem.Init(
			kMaxBodies,
			kNumBodyMutexes,
			kMaxBodyPairs,
			kMaxContactConstraints,
			*broadPhaseLayerInterface,
			*objectVsBroadPhaseLayerFilter,
			*objectLayerPairFilter);

		auto& bodyInterface = queryPhysicsSystem.GetBodyInterface();
		std::vector<JPH::BodyID> queryBodyIDs;
		queryBodyIDs.reserve(64);

		auto destroyQueryBody = [&](JPH::BodyID bodyID, const char* stage) noexcept
		{
			try
			{
				if (bodyInterface.IsAdded(bodyID))
					bodyInterface.RemoveBody(bodyID);
				bodyInterface.DestroyBody(bodyID);
			}
			catch (...)
			{
				LogPhysicsCleanupFailure(stage, std::current_exception());
			}
		};

		auto cleanupQueryBodies = [&]() noexcept
		{
			for (JPH::BodyID bodyID : queryBodyIDs)
				destroyQueryBody(bodyID, "floor-snap query body cleanup");
			queryBodyIDs.clear();
		};
		JPH::ScopeExit queryBodiesGuard([&]() noexcept { cleanupQueryBodies(); });

		auto rigidbodyView = mLevel->mRegistry.view<TransformComponent>();
		for (auto e : rigidbodyView)
		{
			Entity candidate = { e, mLevel };
			if (!candidate || candidate == entity)
				continue;

			if (mLevel->IsEntityDescendantOf(candidate, entity) || mLevel->IsEntityDescendantOf(entity, candidate))
				continue;

			if (!IsFloorSnapBlockingTarget(candidate))
				continue;

			JPH::RefConst<JPH::Shape> targetShape = BuildQueryShapeForEntity(candidate);
			if (!targetShape)
				continue;

			const glm::vec3 targetTranslation = mLevel->GetWorldTranslation(candidate);
			const glm::vec3 targetRotation = mLevel->GetWorldRotation(candidate);
			JPH::BodyCreationSettings settings(
				targetShape,
				Utils::GlmToJoltRVec3(targetTranslation),
				JPH::Quat::sEulerAngles(Utils::GlmToJoltVec3(targetRotation)),
				JPH::EMotionType::Static,
				ObjectLayerStatic);

			JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
			if (bodyID.IsInvalid())
				continue;

			try
			{
				queryBodyIDs.push_back(bodyID);
			}
			catch (...)
			{
				const std::exception_ptr registrationFailure = std::current_exception();
				destroyQueryBody(bodyID, "unregistered floor-snap query body cleanup");
				std::rethrow_exception(registrationFailure);
			}
		}

		if (queryBodyIDs.empty())
			return false;

		queryPhysicsSystem.OptimizeBroadPhase();

		const glm::vec3 sourceTranslation = mLevel->GetWorldTranslation(entity);
		const glm::vec3 sourceRotation = mLevel->GetWorldRotation(entity);
		const JPH::AABox sourceBounds = sourceShape->GetLocalBounds();
		const float liftDistance = std::max(
			kEditorFloorSnapMinLiftDistance,
			static_cast<float>(sourceBounds.GetExtent().GetY()) + kEditorFloorSnapLiftPadding);
		const float castDistance = liftDistance + kEditorFloorSnapMaxDistance;

		JPH::ShapeCastSettings castSettings;
		castSettings.mReturnDeepestPoint = true;

		const glm::vec3 castStartTranslation = sourceTranslation + glm::vec3(0.0f, liftDistance, 0.0f);
		const JPH::RMat44 castStartWorldTransform = JPH::RMat44::sRotationTranslation(
			JPH::Quat::sEulerAngles(Utils::GlmToJoltVec3(sourceRotation)),
			Utils::GlmToJoltRVec3(castStartTranslation));

		const JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(
			sourceShape.GetPtr(),
			JPH::Vec3::sReplicate(1.0f),
			castStartWorldTransform,
			JPH::Vec3(0.0f, -castDistance, 0.0f));

		JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> castCollector;
		queryPhysicsSystem.GetNarrowPhaseQuery().CastShape(
			shapeCast,
			castSettings,
			JPH::RVec3::sZero(),
			castCollector,
			queryPhysicsSystem.GetDefaultBroadPhaseLayerFilter(ObjectLayerMoving),
			queryPhysicsSystem.GetDefaultLayerFilter(ObjectLayerMoving),
			gEmptyBodyFilter,
			gEmptyShapeFilter);

		bool bMoved = false;
		if (castCollector.HadHit())
		{
			glm::vec3 snappedTranslation = sourceTranslation;
			snappedTranslation.y = sourceTranslation.y + liftDistance - castDistance * castCollector.mHit.mFraction;

			if (glm::abs(snappedTranslation.y - sourceTranslation.y) > kEditorFloorSnapMoveEpsilon)
				bMoved = mLevel->SetWorldTranslation(entity, snappedTranslation);
		}

		return bMoved;
	}

	#if WITH_EDITOR
	void PhysicSystem3D::ReleaseEditorDebugResources()
	{
		#ifdef JPH_DEBUG_RENDERER
		mDebugRenderer.reset();
		#endif
		ReleaseEditorDebugJoltIfNeeded();
	}
	#endif

	const JPH::BodyID* PhysicSystem3D::TryGetBodyID(void* runtimeBody)
	{
		thread_local JPH::BodyID sBodyID;
		if (!DecodeRuntimeBodyHandle(runtimeBody, sBodyID))
			return nullptr;
		return &sBodyID;
	}

	const JPH::BodyID* PhysicSystem3D::TryGetBodyID(const void* runtimeBody)
	{
		thread_local JPH::BodyID sBodyID;
		if (!DecodeRuntimeBodyHandle(runtimeBody, sBodyID))
			return nullptr;
		return &sBodyID;
	}
}
