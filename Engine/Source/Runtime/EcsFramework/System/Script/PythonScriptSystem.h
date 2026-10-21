#pragma once

#include "Runtime/EcsFramework/System/System.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <exception>
#include <optional>
#include <vector>
#include <unordered_set>
#include "Runtime/Scripting/ScriptEngine.h"

namespace HEngine
{
	class Entity;
	class PythonScriptComponent;

	class PythonScriptSystem : public System
	{
	public:
		PythonScriptSystem(Level* level);
		~PythonScriptSystem() noexcept override;
	public:
		static Level* GetActiveLevel() { return sActiveLevel; }
		[[nodiscard]] static PythonScriptSystem* GetActiveSystem(Level* level) noexcept;
		static const UUID* GetCurrentScriptOwner();
		static void SetCurrentScriptOwner(const UUID& owner);
		static void ClearCurrentScriptOwner();
		[[nodiscard]] bool StartRuntimeOwner(Entity entity);

		void OnRuntiemStart() override;
		void OnUpdateRuntime(Timestep ts) override;
		void OnFixedUpdate(Timestep ts);
		void OnRuntimeStop() override;
		void OnImGuiRender() override;
		[[nodiscard]] bool ShouldUpdateWhenRuntimePaused() const override { return true; }
#if WITH_EDITOR
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;
#endif
	private:
		static void OnScriptComponentAdded(Entity entity, PythonScriptComponent& component);
		static void OnScriptComponentRemoving(Level* level, const UUID& owner) noexcept;
		[[nodiscard]] bool StartRuntimeOwner(Entity entity, bool bRefreshModule);
		[[nodiscard]] std::exception_ptr StopRuntimeOwner(
			const UUID& owner,
			bool bInvokeStopCallback) noexcept;
		[[nodiscard]] std::exception_ptr CleanupRuntimeOwnerState(
			const UUID& owner) noexcept;
		void ReleaseRuntimeOwner(const UUID& owner) noexcept;

		void CallAll(LegacyScriptCallback callback, float seconds = 0);

		static Level* sActiveLevel;
		static PythonScriptSystem* sActiveSystem;
		static std::optional<UUID> sCurrentScriptOwner;
		std::vector<UUID> mStartedScriptOwners;
		std::unordered_set<UUID> mFaultedOwners;
		bool mRuntimeActive = false;

		friend class Level;
	};
}
