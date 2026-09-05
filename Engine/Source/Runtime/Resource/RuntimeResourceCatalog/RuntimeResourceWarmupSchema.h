#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
	inline constexpr std::int64_t RuntimeResourceWarmupSchemaVersion = 1;

	enum class RuntimeResourceWarmupExpand : std::uint8_t
	{
		None,
		HardRuntime,
		Runtime
	};

	enum class RuntimeResourceWarmupResidency : std::uint8_t
	{
		Startup,
		Session
	};

	struct RuntimeResourceWarmupRoot
	{
		std::string Path;
		RuntimeResourceWarmupExpand Expand = RuntimeResourceWarmupExpand::HardRuntime;
	};

	struct RuntimeResourceWarmupSchemaGroup
	{
		std::string Name;
		std::string Trigger;
		std::int32_t Priority = 0;
		RuntimeResourceWarmupExpand Expand = RuntimeResourceWarmupExpand::HardRuntime;
		RuntimeResourceWarmupResidency Residency = RuntimeResourceWarmupResidency::Session;
		std::vector<RuntimeResourceWarmupRoot> Roots;
	};

	struct RuntimeResourceWarmupSchema
	{
		std::vector<RuntimeResourceWarmupSchemaGroup> Groups;
	};

	using RuntimeResourceWarmupRootNormalizer =
		std::function<std::string(std::string_view path, std::string_view context)>;

	// Parses only the shared ResourceWarmup schema. Filesystem resolution and
	// dependency closure remain consumer-specific through normalizeRootPath.
	[[nodiscard]] RuntimeResourceWarmupSchema ParseRuntimeResourceWarmupSchema(
		const nlohmann::json& gameObject,
		const RuntimeResourceWarmupRootNormalizer& normalizeRootPath);
}
