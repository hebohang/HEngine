#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceWarmupSchema.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceRegistry.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
	enum class RuntimeResourceWarmupGroupState : std::uint8_t
	{
		Idle,
		Queued,
		Loading,
		Ready,
		Failed,
		Released
	};

	struct RuntimeResourceWarmupTarget
	{
		std::string Path;
		RuntimeCatalogResourceType Type = RuntimeCatalogResourceType::Data;
		bool bExplicitRoot = false;
	};

	struct RuntimeResourceWarmupGroupPlan
	{
		std::string Name;
		std::string Trigger;
		std::int32_t Priority = 0;
		RuntimeResourceWarmupExpand Expand = RuntimeResourceWarmupExpand::HardRuntime;
		RuntimeResourceWarmupResidency Residency = RuntimeResourceWarmupResidency::Session;
		std::vector<RuntimeResourceWarmupRoot> Roots;
		std::vector<RuntimeResourceWarmupTarget> Targets;
	};

	struct RuntimeResourceWarmupPlan
	{
		std::filesystem::path ProjectRoot;
		std::shared_ptr<const RuntimeResourceCatalogSnapshot> Catalog;
		std::vector<RuntimeResourceWarmupGroupPlan> Groups;
		bool bPackaged = false;
	};

	// Invoked by Pump on the manager's owning main thread. The composition root
	// chooses the concrete resource adapter and must pin the returned key into group.
	using RuntimeResourceWarmupDispatch = std::function<RuntimeResourceKey(
		const RuntimeResourceWarmupTarget& target,
		RuntimeResourceGroup& group)>;

	[[nodiscard]] RuntimeResourceWarmupPlan BuildRuntimeWarmupPlan(
		const Json& gameObject,
		std::shared_ptr<const RuntimeResourceCatalogSnapshot> snapshot,
		const std::filesystem::path& projectRoot);

	struct RuntimeResourceWarmupGroupProgress
	{
		RuntimeResourceWarmupGroupState State = RuntimeResourceWarmupGroupState::Idle;
		std::size_t Total = 0;
		std::size_t Undispatched = 0;
		std::size_t Dispatched = 0;
		std::size_t Ready = 0;
		std::size_t Failed = 0;
		std::size_t Loading = 0;
		std::size_t Skipped = 0;
		float Ratio = 0.0f;
	};

	struct RuntimeResourceWarmupTriggerProgress
	{
		std::size_t TotalGroups = 0;
		std::size_t IdleGroups = 0;
		std::size_t QueuedGroups = 0;
		std::size_t LoadingGroups = 0;
		std::size_t ReadyGroups = 0;
		std::size_t FailedGroups = 0;
		std::size_t ReleasedGroups = 0;
		std::size_t Total = 0;
		std::size_t Undispatched = 0;
		std::size_t Dispatched = 0;
		std::size_t Ready = 0;
		std::size_t Failed = 0;
		std::size_t Loading = 0;
		std::size_t Skipped = 0;
		float Ratio = 0.0f;
	};

	// The manager is main-thread owned. It never pumps the registry and never
	// invalidates shared resource generations. Ready groups retain their pins
	// until explicitly released or Shutdown is called.
	class RuntimeResourceWarmupManager final
	{
	public:
		RuntimeResourceWarmupManager();
		~RuntimeResourceWarmupManager();

		RuntimeResourceWarmupManager(const RuntimeResourceWarmupManager&) = delete;
		RuntimeResourceWarmupManager& operator=(const RuntimeResourceWarmupManager&) = delete;

		[[nodiscard]] static RuntimeResourceWarmupManager& GetInstance();

		void Initialize(
			RuntimeResourceRegistry& registry,
			RuntimeResourceWarmupPlan plan,
			RuntimeResourceWarmupDispatch dispatch);
		[[nodiscard]] bool IsInitialized() const noexcept;

		[[nodiscard]] bool QueueGroup(std::string_view groupName);
		[[nodiscard]] std::size_t QueueTrigger(std::string_view trigger);
		void Pump();

		[[nodiscard]] std::optional<RuntimeResourceWarmupGroupProgress> GetGroupProgress(
			std::string_view groupName) const;
		[[nodiscard]] std::optional<RuntimeResourceWarmupTriggerProgress> GetTriggerProgress(
			std::string_view trigger) const;
		void RethrowFailure(std::string_view groupName) const;

		[[nodiscard]] bool ReleaseGroup(std::string_view groupName) noexcept;
		[[nodiscard]] std::size_t ReleaseResidency(RuntimeResourceWarmupResidency residency) noexcept;
		void Shutdown() noexcept;

	private:
		struct Impl;
		Scope<Impl> mImpl;
	};
}
