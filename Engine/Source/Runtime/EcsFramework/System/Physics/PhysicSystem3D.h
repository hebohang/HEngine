#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/Graphics/Renderer2D.h"
#include "Runtime/Resource/ModeManager/ModeManager.h"

#if HE_ENABLE_PHYSICS_3D
#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Physics/Body/BodyManager.h>
#include "Runtime/EcsFramework/System/Physics/JoltDebugRendererAdapter.h"
#endif

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

namespace HEngine
{
	class ScriptContactListener3D;
	namespace Utils
	{
		inline glm::vec3 JoltVec3ToGlm(JPH::Vec3Arg src)
		{
			return glm::vec3(src.GetX(), src.GetY(), src.GetZ());
		}

		inline glm::vec3 JoltRVec3ToGlm(JPH::RVec3Arg src)
		{
			return glm::vec3(static_cast<float>(src.GetX()), static_cast<float>(src.GetY()), static_cast<float>(src.GetZ()));
		}

		inline JPH::Vec3 GlmToJoltVec3(const glm::vec3& src)
		{
			return JPH::Vec3(src.x, src.y, src.z);
		}

		inline JPH::RVec3 GlmToJoltRVec3(const glm::vec3& src)
		{
			return JPH::RVec3(src.x, src.y, src.z);
		}
	}

    class PhysicSystem3D : public System
    {
	public:
		PhysicSystem3D(Level* level);
		~PhysicSystem3D() noexcept override;

		static constexpr JPH::ObjectLayer ObjectLayerStatic = 0;
		static constexpr JPH::ObjectLayer ObjectLayerMoving = 1;
    public:
		void OnRuntiemStart() override;
		void OnUpdateRuntime(Timestep ts) override;
		void OnRuntimeStop() override;
#if WITH_EDITOR
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
		void ReleaseEditorDebugResources();
#endif

		static void SetDebugMode(PhysicsDebugDrawModeFlag flags);

		JPH::PhysicsSystem* GetPhysicsSystem() const { return mPhysicsSystem.get(); }
		JPH::TempAllocator* GetTempAllocator() const { return mTempAllocator.get(); }

		bool SetBodyLinearVelocity(void* runtimeBody, const glm::vec3& velocity);
        bool GetBodyLinearVelocity(void* runtimeBody, glm::vec3& velocity);
        bool ApplyBodyForce(void* runtimeBody, const glm::vec3& force, bool impulse);
		bool SnapEntityToFloor(Entity entity);
		[[nodiscard]] bool RaycastFloorHeight(
			const glm::vec3& position,
			float castDistanceAbove,
			float castDistanceBelow,
			float& outFloorHeight) const;

		static const JPH::BodyID* TryGetBodyID(void* runtimeBody);
		static const JPH::BodyID* TryGetBodyID(const void* runtimeBody);

	private:
		enum class RuntimeLifecycleState : std::uint8_t
		{
			Stopped,
			Starting,
			Started,
			Stopping
		};

		void EnsureJoltInitialized();
		void ShutdownJoltIfNeeded() noexcept;
		void ReleaseRuntimeJoltIfNeeded() noexcept;
		void EnsureEditorDebugJoltInitialized();
		void ReleaseEditorDebugJoltIfNeeded() noexcept;
		void BuildRuntimeRigidBodies();
		void OnBodyRemoving(Registry& registry, EntityHandle entity);
		entt::scoped_connection mBodyConnection;
		void ClearRuntimeRigidBodies();
		void CleanupRuntimeState();
		void DrawDebugBodies();
		void ApplyCharacterVirtualDebugFlags() const;
		#ifdef JPH_DEBUG_RENDERER
		JPH::BodyManager::DrawSettings BuildJoltDrawSettings() const;
		#endif

		std::unique_ptr<JPH::PhysicsSystem> mPhysicsSystem;
		std::unique_ptr<ScriptContactListener3D> mContactListener;
		std::unique_ptr<JPH::TempAllocatorImpl> mTempAllocator;
		std::unique_ptr<JPH::JobSystemThreadPool> mJobSystem;

		std::unique_ptr<JPH::BroadPhaseLayerInterfaceTable> mBroadPhaseLayerInterface;
		std::unique_ptr<JPH::ObjectLayerPairFilterTable> mObjectLayerPairFilter;
		std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> mObjectVsBroadPhaseLayerFilter;

		RuntimeLifecycleState mRuntimeState = RuntimeLifecycleState::Stopped;
		bool mRuntimeJoltInitialized = false;
		bool mEditorJoltInitialized = false;
		#ifdef JPH_DEBUG_RENDERER
		std::unique_ptr<JoltDebugRendererAdapter> mDebugRenderer;
		#endif

		static PhysicsDebugDrawModeFlag mDebugMode;
		static int sJoltInitRefCount;
    };
}

#else
namespace HEngine
{
    // Kept as a queryable system type; Level never registers it when disabled.
    class PhysicSystem3D : public System
    {
    public:
        explicit PhysicSystem3D(Level* level) : System(level) {}
        void* GetPhysicsSystem() const { return nullptr; }
        static void SetDebugMode(PhysicsDebugDrawModeFlag) {}
        void ReleaseEditorDebugResources() {}
        bool SetBodyLinearVelocity(void*, const glm::vec3&) { return false; }
        bool GetBodyLinearVelocity(void*, glm::vec3&) { return false; }
        bool ApplyBodyForce(void*, const glm::vec3&, bool) { return false; }
        bool SnapEntityToFloor(Entity) ;
        bool RaycastFloorHeight(const glm::vec3&, float, float, float&) const { return false; }
    };
}
#endif
