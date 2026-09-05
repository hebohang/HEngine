#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceWarmupSchema.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace HEngine
{
	namespace
	{
		constexpr std::size_t MaxRuntimeWarmupIdentifierLength = 128;

		[[nodiscard]] std::string TrimAscii(std::string value)
		{
			const auto bWhitespace = [](unsigned char character)
			{
				return character == ' ' || character == '\t' ||
					character == '\r' || character == '\n';
			};
			const auto first = std::find_if_not(value.begin(), value.end(), bWhitespace);
			const auto last = std::find_if_not(value.rbegin(), value.rend(), bWhitespace).base();
			return first < last ? std::string(first, last) : std::string{};
		}

		[[nodiscard]] std::string FoldAscii(std::string_view value)
		{
			std::string folded(value);
			for (char& character : folded)
			{
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character - 'A' + 'a');
			}
			return folded;
		}

		[[nodiscard]] bool HasControlCharacter(std::string_view value) noexcept
		{
			return std::any_of(value.begin(), value.end(), [](unsigned char character)
			{
				return character < 32;
			});
		}

		void RequireOnlyFields(
			const nlohmann::json& object,
			std::initializer_list<std::string_view> fields,
			const std::string& context)
		{
			if (!object.is_object())
				throw std::invalid_argument(context + " must be an object.");

			for (auto field = object.begin(); field != object.end(); ++field)
			{
				const bool bKnown = std::any_of(
					fields.begin(),
					fields.end(),
					[&field](std::string_view known)
					{
						return field.key() == known;
					});
				if (!bKnown)
					throw std::invalid_argument(
						context + " contains unknown field '" + field.key() + "'.");
			}
		}

		[[nodiscard]] const nlohmann::json& RequireField(
			const nlohmann::json& object,
			const char* field,
			const std::string& context)
		{
			const auto found = object.find(field);
			if (found == object.end())
				throw std::invalid_argument(context + " is missing field '" + field + "'.");
			return *found;
		}

		[[nodiscard]] std::string ParseIdentifier(
			const nlohmann::json& value,
			const std::string& context)
		{
			if (!value.is_string())
				throw std::invalid_argument(context + " must be a string.");

			std::string identifier = TrimAscii(value.get<std::string>());
			if (identifier.empty() || identifier.size() > MaxRuntimeWarmupIdentifierLength ||
				identifier.find(':') != std::string::npos || HasControlCharacter(identifier))
			{
				throw std::invalid_argument(
					context + " must be 1-128 characters without ':' or control characters.");
			}
			return identifier;
		}

		[[nodiscard]] RuntimeResourceWarmupExpand ParseExpand(
			const nlohmann::json& value,
			const std::string& context)
		{
			if (!value.is_string())
				throw std::invalid_argument(
					context + " must be one of None, HardRuntime, or Runtime.");

			const std::string expand = value.get<std::string>();
			if (expand == "None")
				return RuntimeResourceWarmupExpand::None;
			if (expand == "HardRuntime")
				return RuntimeResourceWarmupExpand::HardRuntime;
			if (expand == "Runtime")
				return RuntimeResourceWarmupExpand::Runtime;
			throw std::invalid_argument(
				context + " must be one of None, HardRuntime, or Runtime.");
		}

		[[nodiscard]] RuntimeResourceWarmupResidency ParseResidency(
			const nlohmann::json& value,
			const std::string& context)
		{
			if (!value.is_string())
				throw std::invalid_argument(context + " must be one of Startup or Session.");

			const std::string residency = value.get<std::string>();
			if (residency == "Startup")
				return RuntimeResourceWarmupResidency::Startup;
			if (residency == "Session")
				return RuntimeResourceWarmupResidency::Session;
			throw std::invalid_argument(context + " must be one of Startup or Session.");
		}

		[[nodiscard]] std::int32_t ParsePriority(
			const nlohmann::json& group,
			const std::string& context)
		{
			const auto found = group.find("Priority");
			if (found == group.end())
				return 0;
			if (!found->is_number_integer())
				throw std::invalid_argument(context + ".Priority must be a 32-bit integer.");

			try
			{
				if (found->is_number_unsigned())
				{
					const std::uint64_t priority = found->get<std::uint64_t>();
					if (priority > static_cast<std::uint64_t>(
						std::numeric_limits<std::int32_t>::max()))
					{
						throw std::invalid_argument(
							context + ".Priority must be a 32-bit integer.");
					}
					return static_cast<std::int32_t>(priority);
				}

				const std::int64_t priority = found->get<std::int64_t>();
				if (priority < std::numeric_limits<std::int32_t>::min() ||
					priority > std::numeric_limits<std::int32_t>::max())
				{
					throw std::invalid_argument(
						context + ".Priority must be a 32-bit integer.");
				}
				return static_cast<std::int32_t>(priority);
			}
			catch (const std::invalid_argument&)
			{
				throw;
			}
			catch (const std::exception&)
			{
				throw std::invalid_argument(context + ".Priority must be a 32-bit integer.");
			}
		}
	}

	RuntimeResourceWarmupSchema ParseRuntimeResourceWarmupSchema(
		const nlohmann::json& gameObject,
		const RuntimeResourceWarmupRootNormalizer& normalizeRootPath)
	{
		if (!gameObject.is_object())
			throw std::invalid_argument("Game must be an object when parsing ResourceWarmup.");
		if (!normalizeRootPath)
			throw std::invalid_argument("ResourceWarmup root path normalizer cannot be empty.");

		RuntimeResourceWarmupSchema schema;
		const auto warmupIt = gameObject.find("ResourceWarmup");
		if (warmupIt == gameObject.end())
			return schema;

		const nlohmann::json& warmup = *warmupIt;
		RequireOnlyFields(warmup, { "Version", "Groups" }, "Game.ResourceWarmup");
		const nlohmann::json& version = RequireField(
			warmup,
			"Version",
			"Game.ResourceWarmup");
		if (!version.is_number_integer())
			throw std::invalid_argument("Game.ResourceWarmup.Version must be integer 1.");
		bool bSupportedVersion = false;
		try
		{
			bSupportedVersion = version.is_number_unsigned()
				? version.get<std::uint64_t>() ==
					static_cast<std::uint64_t>(RuntimeResourceWarmupSchemaVersion)
				: version.get<std::int64_t>() == RuntimeResourceWarmupSchemaVersion;
		}
		catch (const std::exception&)
		{
			throw std::invalid_argument("Game.ResourceWarmup.Version must be integer 1.");
		}
		if (!bSupportedVersion)
			throw std::invalid_argument("Game.ResourceWarmup.Version must be integer 1.");

		const nlohmann::json& groups = RequireField(
			warmup,
			"Groups",
			"Game.ResourceWarmup");
		if (!groups.is_array())
			throw std::invalid_argument("Game.ResourceWarmup.Groups must be an array.");

		std::unordered_set<std::string> groupNames;
		groupNames.reserve(groups.size());
		schema.Groups.reserve(groups.size());
		for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex)
		{
			const nlohmann::json& groupJson = groups[groupIndex];
			const std::string context =
				"Game.ResourceWarmup.Groups[" + std::to_string(groupIndex) + "]";
			RequireOnlyFields(
				groupJson,
				{ "Name", "Trigger", "Priority", "Expand", "Residency", "Roots" },
				context);

			RuntimeResourceWarmupSchemaGroup group;
			group.Name = ParseIdentifier(
				RequireField(groupJson, "Name", context),
				context + ".Name");
			if (!groupNames.emplace(FoldAscii(group.Name)).second)
			{
				throw std::invalid_argument(
					"Game.ResourceWarmup group names must be unique: " + group.Name);
			}
			group.Trigger = groupJson.contains("Trigger")
				? ParseIdentifier(groupJson["Trigger"], context + ".Trigger")
				: group.Name;
			group.Priority = ParsePriority(groupJson, context);
			group.Expand = groupJson.contains("Expand")
				? ParseExpand(groupJson["Expand"], context + ".Expand")
				: RuntimeResourceWarmupExpand::HardRuntime;
			group.Residency = groupJson.contains("Residency")
				? ParseResidency(groupJson["Residency"], context + ".Residency")
				: RuntimeResourceWarmupResidency::Session;

			const nlohmann::json& roots = RequireField(groupJson, "Roots", context);
			if (!roots.is_array())
				throw std::invalid_argument(context + ".Roots must be an array.");

			std::unordered_set<std::string> rootPaths;
			rootPaths.reserve(roots.size());
			group.Roots.reserve(roots.size());
			for (std::size_t rootIndex = 0; rootIndex < roots.size(); ++rootIndex)
			{
				const nlohmann::json& rootJson = roots[rootIndex];
				const std::string rootContext =
					context + ".Roots[" + std::to_string(rootIndex) + "]";
				RequireOnlyFields(rootJson, { "Path", "Expand" }, rootContext);
				const nlohmann::json& pathValue = RequireField(rootJson, "Path", rootContext);
				if (!pathValue.is_string())
					throw std::invalid_argument(rootContext + ".Path must be a string.");

				RuntimeResourceWarmupRoot root;
				root.Path = normalizeRootPath(
					pathValue.get_ref<const std::string&>(),
					rootContext + ".Path");
				if (root.Path.empty())
					throw std::invalid_argument(rootContext + ".Path cannot normalize to an empty path.");
				root.Expand = rootJson.contains("Expand")
					? ParseExpand(rootJson["Expand"], rootContext + ".Expand")
					: group.Expand;
				if (!rootPaths.emplace(FoldAscii(root.Path)).second)
					throw std::invalid_argument(context + " contains duplicate root: " + root.Path);
				group.Roots.emplace_back(std::move(root));
			}

			schema.Groups.emplace_back(std::move(group));
		}
		return schema;
	}
}
