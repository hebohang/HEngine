#include "Hepch.h"

#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeUiResource.h"

#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace HEngine
{
	namespace
	{
		[[nodiscard]] size_t SaturatingSize(uint64_t value) noexcept
		{
			constexpr uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
			return value > maxSize ? std::numeric_limits<size_t>::max() : static_cast<size_t>(value);
		}

		[[nodiscard]] size_t SaturatingAdd(size_t lhs, size_t rhs) noexcept
		{
			const size_t maxSize = std::numeric_limits<size_t>::max();
			return rhs > maxSize - lhs ? maxSize : lhs + rhs;
		}

		[[nodiscard]] size_t GetResidentBytes(
			const std::shared_ptr<const RuntimeResourceCatalogSnapshot>& snapshot,
			std::string_view projectPath,
			const std::filesystem::path& fullPath)
		{
			if (const RuntimeResourceCatalogEntry* entry = snapshot ? snapshot->Find(projectPath) : nullptr)
				return SaturatingSize(entry->CpuBytes);

			std::error_code error;
			const uintmax_t fileSize = std::filesystem::file_size(fullPath, error);
			return error ? 0 : SaturatingSize(static_cast<uint64_t>(fileSize));
		}

		[[nodiscard]] bool IsInsideRoot(
			const std::filesystem::path& canonicalRoot,
			const std::filesystem::path& candidate)
		{
			const std::filesystem::path relative = candidate.lexically_relative(canonicalRoot);
			if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
				return false;
			for (const std::filesystem::path& component : relative)
			{
				if (component == "..")
					return false;
			}
			return true;
		}

		void ResolveSiblingPath(
			const std::filesystem::path& projectRoot,
			const std::string& huiProjectPath,
			std::string_view extension,
			std::filesystem::path& outFullPath,
			std::string& outProjectPath)
		{
			std::filesystem::path siblingProjectPath = Utils::Utf8ToPath(huiProjectPath);
			siblingProjectPath.replace_extension(std::string(extension));
			const Utils::PathValidationResult result = Utils::TryResolveProjectRelativePath(
				projectRoot,
				Utils::PathToUtf8(siblingProjectPath),
				outFullPath,
				outProjectPath);
			if (!result)
				throw std::invalid_argument(result.Message);
		}

		void ValidateGeneratedPath(
			std::string_view generatedPath,
			std::string_view expectedProjectPath,
			std::string_view fieldName)
		{
			std::string normalizedPath;
			const Utils::PathValidationResult result =
				Utils::TryNormalizeProjectRelativePath(generatedPath, normalizedPath);
			if (!result)
			{
				throw std::runtime_error(
					"UI resource field " + std::string(fieldName) + " is invalid: " + result.Message);
			}

			if (Utils::MakeProjectPathCacheKey(normalizedPath) !=
				Utils::MakeProjectPathCacheKey(expectedProjectPath))
			{
				throw std::runtime_error(
					"UI resource field " + std::string(fieldName) +
					" must reference its generated sibling: " + std::string(expectedProjectPath));
			}
		}

		[[nodiscard]] UIRes LoadAndValidateDefinition(const RuntimeUiPath& path)
		{
			UIRes definition;
			std::string error;
			if (!LoadUIRes(path.HuiFullPath, definition, &error))
			{
				throw std::runtime_error(
					"Failed to load UI resource " + Utils::PathToUtf8(path.HuiFullPath) + ": " + error);
			}

			ValidateGeneratedPath(
				definition.GeneratedRmlPath,
				path.RmlProjectPath,
				"GeneratedRmlPath");
			ValidateGeneratedPath(
				definition.GeneratedRcssPath,
				path.RcssProjectPath,
				"GeneratedRcssPath");

			std::error_code errorCode;
			if (!std::filesystem::is_regular_file(path.RmlFullPath, errorCode) || errorCode)
				throw std::runtime_error("Generated RmlUi document is missing: " + Utils::PathToUtf8(path.RmlFullPath));
			errorCode.clear();
			if (!std::filesystem::is_regular_file(path.RcssFullPath, errorCode) || errorCode)
				throw std::runtime_error("Generated RmlUi stylesheet is missing: " + Utils::PathToUtf8(path.RcssFullPath));

			return definition;
		}
	}

	RuntimeUiDefinition::RuntimeUiDefinition(UIRes definition)
		: mDefinition(std::move(definition))
	{
	}

	const UIRes& RuntimeUiDefinition::GetDefinition() const noexcept
	{
		return mDefinition;
	}

	RuntimeUiPath ResolveRuntimeUiPath(
		const std::filesystem::path& projectRoot,
		std::string_view projectPath)
	{
		RuntimeUiPath path;
		std::string normalizedPath;
		const Utils::PathValidationResult result = Utils::TryResolveProjectRelativePath(
			projectRoot,
			projectPath,
			path.HuiFullPath,
			normalizedPath);
		if (!result)
			throw std::invalid_argument(result.Message);

		path.Key = RuntimeResourceKey::FromProjectPath(RuntimeResourceKind::Ui, normalizedPath);
		if (!path.Key.CachePath.ends_with(".hui"))
			throw std::invalid_argument("UI resource path must use the .hui extension.");
		path.HuiProjectPath = path.Key.Path;

		ResolveSiblingPath(
			projectRoot,
			path.HuiProjectPath,
			".rml",
			path.RmlFullPath,
			path.RmlProjectPath);
		ResolveSiblingPath(
			projectRoot,
			path.HuiProjectPath,
			".rcss",
			path.RcssFullPath,
			path.RcssProjectPath);

		const std::shared_ptr<const RuntimeResourceCatalogSnapshot> snapshot =
			RuntimeResourceCatalog::GetInstance().GetSnapshot();
		path.ResidentBytes = GetResidentBytes(snapshot, path.HuiProjectPath, path.HuiFullPath);
		path.ResidentBytes = SaturatingAdd(
			path.ResidentBytes,
			GetResidentBytes(snapshot, path.RmlProjectPath, path.RmlFullPath));
		path.ResidentBytes = SaturatingAdd(
			path.ResidentBytes,
			GetResidentBytes(snapshot, path.RcssProjectPath, path.RcssFullPath));
		return path;
	}

	RuntimeUiPath ResolveRuntimeUiWrittenPath(
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& writtenPath)
	{
		if (!writtenPath.is_absolute())
			return ResolveRuntimeUiPath(projectRoot, Utils::PathToUtf8(writtenPath));

		std::error_code error;
		const std::filesystem::path absoluteRoot = std::filesystem::absolute(projectRoot, error);
		if (error)
			throw std::invalid_argument("Cannot make the project root absolute: " + error.message());
		const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
		if (error || !std::filesystem::is_directory(canonicalRoot, error) || error)
			throw std::invalid_argument("Cannot resolve the project root for UI resource save.");

		const std::filesystem::path canonicalWritten =
			std::filesystem::weakly_canonical(writtenPath, error);
		if (error)
			throw std::invalid_argument("Cannot resolve the UI resource save path: " + error.message());
		if (!IsInsideRoot(canonicalRoot, canonicalWritten))
			throw std::invalid_argument("UI resource save path resolves outside the project root.");

		const std::filesystem::path relative = canonicalWritten.lexically_relative(canonicalRoot);
		return ResolveRuntimeUiPath(canonicalRoot, Utils::PathToUtf8(relative));
	}

	RuntimeResourceHandle<RuntimeUiDefinition> AcquireRuntimeUiDefinition(
		RuntimeResourceRegistry& registry,
		const RuntimeUiPath& path)
	{
		return registry.AcquireBlocking<RuntimeUiDefinition>(
			path.Key,
			[path]
			{
				return std::make_shared<RuntimeUiDefinition>(LoadAndValidateDefinition(path));
			},
			path.ResidentBytes);
	}

	bool QueueRuntimeUiDefinition(
		RuntimeResourceRegistry& registry,
		const RuntimeUiPath& path,
		RuntimeResourceGroup& group)
	{
		if (!group)
			throw std::logic_error("UI runtime resource queue requires a valid resource group.");

		return registry.QueueAsyncPinned<RuntimeUiDefinition, UIRes>(
			path.Key,
			group,
			[path](std::stop_token stopToken)
			{
				if (stopToken.stop_requested())
					throw std::runtime_error("UI resource load was cancelled: " + Utils::PathToUtf8(path.HuiFullPath));
				UIRes definition = LoadAndValidateDefinition(path);
				if (stopToken.stop_requested())
					throw std::runtime_error("UI resource load was cancelled: " + Utils::PathToUtf8(path.HuiFullPath));
				return definition;
			},
			[](UIRes&& definition)
			{
				return std::make_shared<RuntimeUiDefinition>(std::move(definition));
			},
			path.ResidentBytes);
	}

	bool SaveRuntimeUiDefinition(
		RuntimeResourceRegistry& registry,
		const RuntimeUiPath& path,
		const UIRes& definition,
		std::string* outError)
	{
		if (outError)
			outError->clear();

		UIRes persistedDefinition = definition;
		persistedDefinition.GeneratedRmlPath = path.RmlProjectPath;
		persistedDefinition.GeneratedRcssPath = path.RcssProjectPath;
		if (!SaveUIRes(persistedDefinition, path.HuiFullPath, outError))
			return false;

		if (registry.IsInitialized())
			(void)registry.Invalidate(path.Key);
		return true;
	}
}
