#pragma once

#include "Runtime/Core/UUID.h"
#include "Runtime/EcsFramework/System/System.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <optional>

namespace HEngine
{
#if WITH_EDITOR
	struct EmbreeEditorPickHit
	{
		UUID EntityID;
		std::uint32_t SubMeshIndex = 0;
		std::uint32_t MaterialIndex = 0;
	};
#endif

	class EmbreeSystem : public System
	{
	public:
		EmbreeSystem(Level* level);
		~EmbreeSystem() override;
	public:
		void OnRuntiemStart() override;
		void OnUpdateRuntime(Timestep ts) override;
		void OnRuntimeStop() override;
#if WITH_EDITOR
		void OnUpdateEditor(Timestep ts, EditorCamera& camera) override;

		static void InitializeSharedContext();
		static void ResetSharedContext();
		static void ShutdownSharedContext() noexcept;
		[[nodiscard]] static bool IsSharedContextInitialized() noexcept;

		void OnEntityRemoved(const UUID& entityID) noexcept;
		[[nodiscard]] std::optional<EmbreeEditorPickHit> Raycast(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float nearDistance = 0.0f,
			float farDistance = std::numeric_limits<float>::infinity());
#endif

	private:
#if WITH_EDITOR
		struct EditorState;
		void RemoveBinding(const UUID& entityID) noexcept;
		void ClearBindings() noexcept;
		bool SynchronizeEditorPickScene();

		Scope<EditorState> mEditorState;
#endif
	};
}
