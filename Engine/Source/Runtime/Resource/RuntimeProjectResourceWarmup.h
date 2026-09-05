#pragma once

#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceWarmupManager.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace HEngine
{
	// Project-level composition service for the resource warm-up plan. Application
	// owns its lifetime; gameplay and Editor PIE only queue or observe groups.
	class RuntimeProjectResourceWarmup final
	{
	public:
		RuntimeProjectResourceWarmup() = default;
		~RuntimeProjectResourceWarmup();

		RuntimeProjectResourceWarmup(const RuntimeProjectResourceWarmup&) = delete;
		RuntimeProjectResourceWarmup& operator=(const RuntimeProjectResourceWarmup&) = delete;

		void Initialize(
			RuntimeResourceRegistry& registry,
			const std::filesystem::path& projectRoot,
			const std::filesystem::path& engineContentRoot,
			const std::filesystem::path& gameConfigPath,
			std::shared_ptr<const RuntimeResourceCatalogSnapshot> catalog);
		[[nodiscard]] bool IsInitialized() const noexcept;

		void Pump();
		[[nodiscard]] bool QueueGroup(std::string_view groupName);
		[[nodiscard]] std::optional<RuntimeResourceWarmupGroupProgress> GetGroupProgress(
			std::string_view groupName) const;
		void RethrowFailure(std::string_view groupName) const;
		[[nodiscard]] std::size_t ReleaseResidency(
			RuntimeResourceWarmupResidency residency) noexcept;
		void Shutdown() noexcept;

		[[nodiscard]] const std::string& GetDefaultScene() const;

	private:
		RuntimeResourceWarmupManager* mManager = nullptr;
		std::string mDefaultScene;
		bool mInitialized = false;
	};
}
