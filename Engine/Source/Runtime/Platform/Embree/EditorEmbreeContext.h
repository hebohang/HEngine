#pragma once

#if defined(WITH_EDITOR)

#include "Runtime/Core/Base/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace HEngine
{
	struct EditorEmbreeRayHit
	{
		std::uint32_t InstanceID = 0;
		std::uint32_t PrimitiveID = 0;
	};

	class EditorEmbreeInstance final
	{
	public:
		~EditorEmbreeInstance();

		EditorEmbreeInstance(const EditorEmbreeInstance&) = delete;
		EditorEmbreeInstance& operator=(const EditorEmbreeInstance&) = delete;

		[[nodiscard]] std::uint32_t GetInstanceID() const noexcept;
		[[nodiscard]] std::uint64_t GetContextGeneration() const noexcept;

	private:
		struct Impl;
		explicit EditorEmbreeInstance(Scope<Impl> impl);

		Scope<Impl> mImpl;

		friend class EditorEmbreeContext;
	};

	// Owns the process-wide Embree device and the active editor pick scene.
	// Native Embree handles stay behind this API; callers retain only scoped,
	// generation-tagged instance bindings.
	class EditorEmbreeContext final
	{
	public:
		static void Initialize();
		static void Reset();
		static void Shutdown() noexcept;

		[[nodiscard]] static bool IsInitialized() noexcept;
		[[nodiscard]] static std::uint64_t GetGeneration() noexcept;

		[[nodiscard]] static Scope<EditorEmbreeInstance> CreateInstance(
			std::span<const glm::vec3> vertices,
			std::span<const std::uint32_t> triangleIndices,
			const glm::mat4& transform);
		[[nodiscard]] static bool UpdateInstanceTransform(
			EditorEmbreeInstance& instance,
			const glm::mat4& transform);
		[[nodiscard]] static bool Commit() noexcept;

		[[nodiscard]] static std::optional<EditorEmbreeRayHit> Raycast(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float nearDistance,
			float farDistance);
	};
}

#endif
