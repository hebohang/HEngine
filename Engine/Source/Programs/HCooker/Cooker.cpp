#include "Cooker.h"

#include "Runtime/Graphics/Material/MaterialShaderManifest.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceWarmupSchema.h"
#include "Runtime/Utils/JsonUtils/CookedJsonFormat.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include "Effekseer/Effekseer/Material/Effekseer.CompiledMaterial.h"
#include "EffekseerMaterialCompiler/DirectX12/EffekseerMaterialCompilerDX12.h"
#include "EffekseerRendererVulkan/EffekseerMaterialCompilerVulkan.h"
#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace HEngine::Cooker
{
	namespace
	{
		using Json = nlohmann::json;

		constexpr std::uint64_t FnvOffsetBasis = 14695981039346656037ull;
		constexpr std::uint64_t FnvPrime = 1099511628211ull;
		constexpr std::uint64_t MaxEntrySize = 2ull * 1024ull * 1024ull * 1024ull;
		constexpr std::uint64_t MaxContentSize = 16ull * 1024ull * 1024ull * 1024ull;
		constexpr std::size_t MaxFileCount =
			static_cast<std::size_t>(RuntimeResourceCatalogLimits::MaxEntries);
		constexpr std::size_t MaxManifestSize = 32ull * 1024ull * 1024ull;

		enum class DependencyKind
		{
			HardRuntime,
			SoftRuntime,
			PackagingOnly
		};

		enum class DependencySource
		{
			Structured,
			Heuristic,
			ExplicitDynamic
		};

		enum class SeedSource
		{
			RequiredInvariant,
			GameConfig,
			EntryScene,
			ProjectRoot,
			EngineRoot,
			WarmupRoot,
			ExplicitDynamicAssetSet,
			IncludeDirectoryFallback
		};

		struct DependencyEvidence
		{
			std::string Owner;
			std::string Target;
			std::string RawReference;
			DependencyKind Kind = DependencyKind::PackagingOnly;
			DependencySource Source = DependencySource::Heuristic;
			std::string Provider;
			std::string Schema;
			std::string Location;
			std::string Reason;
		};

		struct ResourceDependency
		{
			std::string Path;
			DependencyKind Kind = DependencyKind::PackagingOnly;
			DependencySource Source = DependencySource::Heuristic;
			std::vector<DependencyEvidence> Evidence;
		};

		struct DependencyDiagnostic
		{
			std::string Owner;
			std::string Target;
			std::string RawReference;
			DependencyKind Kind = DependencyKind::PackagingOnly;
			DependencySource Source = DependencySource::Heuristic;
			std::string Provider;
			std::string Schema;
			std::string Location;
			std::string Reason;
		};

		struct SeedRecord
		{
			std::string Id;
			std::string Path;
			SeedSource Source = SeedSource::ProjectRoot;
			std::string Origin;
			std::string Group;
			std::string Reason;
		};

		struct DynamicAssetSetConfig
		{
			std::string Name;
			std::string Manifest;
		};

		struct CookConfig
		{
			std::string GameName;
			std::string ContentVersion;
			std::string EntryScene;
			std::vector<std::string> RootFiles;
			std::vector<std::string> ProjectRootFiles;
			std::vector<std::string> EngineRootFiles;
			std::vector<std::string> IncludeDirectories;
			std::set<std::string> CookedJsonExtensions;
			std::set<std::string> ExcludedDirectories;
			std::set<std::string> ExcludedExtensions;
			std::set<std::string> VirtualFiles;
			std::vector<RuntimeResourceWarmupSchemaGroup> WarmupGroups;
			std::vector<DynamicAssetSetConfig> DynamicAssetSets;
			std::map<std::string, std::string> DynamicAssetSetNamesByManifest;
			int CompressionLevel = ZIP_DEFAULT_COMPRESSION_LEVEL;
		};

		struct IndexedFile
		{
			std::string LogicalPath;
			std::filesystem::path PhysicalPath;
			std::uint64_t Size = 0;
		};

		struct ProjectIndex
		{
			std::map<std::string, IndexedFile> Files;
			std::set<std::string> KnownExtensions;
			std::set<std::string> VirtualFiles;
			std::vector<std::string> ExcludedByRule;
		};

		struct CookedFile
		{
			std::string LogicalPath;
			std::uint64_t SourceSize = 0;
			std::vector<std::uint8_t> Data;
			std::vector<ResourceDependency> Dependencies;
			std::vector<DependencyDiagnostic> VirtualDependencies;
			std::vector<DependencyDiagnostic> UnresolvedCandidates;
			std::set<std::string> Tags;
			std::string Type;
			std::uint32_t CookFormatVersion = 0;
			std::uint64_t CpuBytes = 0;
			std::uint64_t GpuBytes = 0;
			std::uint64_t Hash = 0;
			std::uint64_t CompressedSize = 0;
			bool bCookedJson = false;
		};

		[[noreturn]] void Fail(const std::string& message)
		{
			throw std::runtime_error(message);
		}

		std::string ToLowerAscii(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
			return value;
		}

		std::string Trim(std::string value)
		{
			const auto bIsSpace = [](unsigned char character) { return std::isspace(character) != 0; };
			value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), bIsSpace));
			value.erase(std::find_if_not(value.rbegin(), value.rend(), bIsSpace).base(), value.end());
			return value;
		}

		std::string NormalizeExtension(std::string extension)
		{
			extension = ToLowerAscii(Trim(std::move(extension)));
			if (!extension.empty() && extension.front() != '.')
				extension.insert(extension.begin(), '.');
			return extension;
		}

		bool HasParentTraversal(std::string_view path)
		{
			std::string normalized(path);
			std::replace(normalized.begin(), normalized.end(), '\\', '/');
			std::istringstream stream(normalized);
			std::string component;
			while (std::getline(stream, component, '/'))
			{
				if (component == "..")
					return true;
			}
			return false;
		}

		bool IsAbsoluteAssetPath(std::string_view value)
		{
			if (value.empty())
				return false;
			if (value.front() == '/' || value.front() == '\\')
				return true;
			if (value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0])) &&
				value[1] == ':' && (value[2] == '/' || value[2] == '\\'))
				return true;
			return std::filesystem::path(value).is_absolute();
		}

		std::string NormalizeLogicalPath(
			std::string value,
			const std::string& context,
			bool bValidateArchivePath = true)
		{
			value = Trim(std::move(value));
			std::replace(value.begin(), value.end(), '\\', '/');
			if (value.empty())
				Fail(context + ": path is empty");
			if (IsAbsoluteAssetPath(value))
				Fail(context + ": absolute paths are forbidden: " + value);
			if (HasParentTraversal(value))
				Fail(context + ": parent traversal is forbidden: " + value);

			std::string result;
			std::istringstream stream(value);
			std::string component;
			while (std::getline(stream, component, '/'))
			{
				if (component.empty() || component == ".")
					continue;
				if (bValidateArchivePath && component.find(':') != std::string::npos)
					Fail(context + ": invalid path component: " + component);
				if (!result.empty())
					result.push_back('/');
				result += component;
			}
			if (result.empty())
				Fail(context + ": path resolves to an empty value");
			if (bValidateArchivePath && result.size() > RuntimeResourceCatalogLimits::MaxPathLength)
				Fail(context + ": path exceeds the packaged runtime limit");

			if (bValidateArchivePath)
			{
				std::istringstream resultStream(result);
				while (std::getline(resultStream, component, '/'))
				{
					if (component.empty() || component.size() > 255 || component.back() == '.' || component.back() == ' ' ||
						component.find_first_of("<>:\"|?*") != std::string::npos ||
						std::any_of(component.begin(), component.end(), [](unsigned char character) { return character < 32; }))
					{
						Fail(context + ": invalid Windows path component: " + component);
					}
					if (Utils::IsWindowsReservedPathComponent(component))
						Fail(context + ": reserved Windows path component: " + component);
				}
			}
			if (bValidateArchivePath && ToLowerAscii(result) == ".package-marker")
				Fail(context + ": path is reserved by the packaged runtime: " + result);
			return result;
		}

		std::string FoldPath(const std::string& logicalPath)
		{
			return ToLowerAscii(logicalPath);
		}

		const char* ToString(DependencyKind kind)
		{
			switch (kind)
			{
			case DependencyKind::HardRuntime: return "HardRuntime";
			case DependencyKind::SoftRuntime: return "SoftRuntime";
			case DependencyKind::PackagingOnly: return "PackagingOnly";
			}
			Fail("Unknown resource dependency kind");
		}

		const char* ToString(DependencySource source)
		{
			switch (source)
			{
			case DependencySource::Structured: return "Structured";
			case DependencySource::Heuristic: return "Heuristic";
			case DependencySource::ExplicitDynamic: return "ExplicitDynamic";
			}
			Fail("Unknown resource dependency source");
		}

		const char* ToManifestString(DependencySource source)
		{
			return source == DependencySource::Heuristic ? "Heuristic" : "Structured";
		}

		const char* ToString(SeedSource source)
		{
			switch (source)
			{
			case SeedSource::RequiredInvariant: return "RequiredInvariant";
			case SeedSource::GameConfig: return "GameConfig";
			case SeedSource::EntryScene: return "EntryScene";
			case SeedSource::ProjectRoot: return "ProjectRoot";
			case SeedSource::EngineRoot: return "EngineRoot";
			case SeedSource::WarmupRoot: return "WarmupRoot";
			case SeedSource::ExplicitDynamicAssetSet: return "ExplicitDynamicAssetSet";
			case SeedSource::IncludeDirectoryFallback: return "IncludeDirectoryFallback";
			}
			Fail("Unknown resource seed source");
		}

		int SeedSourceRank(SeedSource source)
		{
			return static_cast<int>(source);
		}

		std::optional<std::string> NormalizeDependencyCandidate(std::string value, const std::string& context)
		{
			std::replace(value.begin(), value.end(), '\\', '/');
			const std::filesystem::path path = std::filesystem::path(value).lexically_normal();
			if (path.is_absolute() || path.has_root_name())
				Fail(context + ": absolute paths are forbidden: " + value);
			for (const auto& component : path)
			{
				if (component == "..")
					return std::nullopt;
			}
			const std::string normalized = path.generic_string();
			if (normalized.empty() || normalized == ".")
				return std::nullopt;
			return NormalizeLogicalPath(normalized, context, false);
		}

		bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& candidate)
		{
			const std::filesystem::path relative = candidate.lexically_relative(root);
			if (relative.empty())
				return candidate == root;
			if (relative.is_absolute() || relative.has_root_name())
				return false;
			for (const auto& component : relative)
			{
				if (component == "..")
					return false;
			}
			return true;
		}

		std::filesystem::path ResolveWithin(
			const std::filesystem::path& projectRoot,
			const std::string& logicalPath,
			const std::string& context)
		{
			std::error_code error;
			const std::filesystem::path resolved = std::filesystem::weakly_canonical(projectRoot / logicalPath, error);
			if (error)
				Fail(context + ": cannot resolve path '" + logicalPath + "': " + error.message());
			if (!IsWithin(projectRoot, resolved))
				Fail(context + ": path escapes the project root: " + logicalPath);
			return resolved;
		}

		Json ReadTextJson(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				Fail("Cannot open JSON file: " + path.generic_string());
			try
			{
				Json json;
				stream >> json;
				return json;
			}
			catch (const std::exception& exception)
			{
				Fail("Invalid JSON file '" + path.generic_string() + "': " + exception.what());
			}
		}

		std::vector<std::string> ReadStringArray(const Json& object, const char* key)
		{
			if (!object.contains(key))
				return {};
			if (!object[key].is_array())
				Fail(std::string("Config field '") + key + "' must be an array");

			std::vector<std::string> values;
			for (const Json& value : object[key])
			{
				if (!value.is_string())
					Fail(std::string("Config field '") + key + "' must contain only strings");
				values.push_back(value.get<std::string>());
			}
			return values;
		}

		CookConfig ParseCookConfig(const Json& root)
		{
			if (!root.contains("Game") || !root["Game"].is_object())
				Fail("Game config must contain a Game object");
			if (!root.contains("Cook") || !root["Cook"].is_object())
				Fail("Game config must contain a Cook object");

			const Json& game = root["Game"];
			const Json& cook = root["Cook"];
			CookConfig config;
			config.GameName = game.value("Name", "HEngine Game");
			config.EntryScene = game.value("DefaultScene", "");
			config.ContentVersion = cook.value("ContentVersion", "1");
			config.CompressionLevel = cook.value("CompressionLevel", ZIP_DEFAULT_COMPRESSION_LEVEL);
			config.RootFiles = ReadStringArray(cook, "RootFiles");
			config.ProjectRootFiles = config.RootFiles;
			config.IncludeDirectories = ReadStringArray(cook, "IncludeDirectories");
			if (cook.contains("DynamicAssetSets"))
			{
				if (!cook["DynamicAssetSets"].is_array())
					Fail("Cook.DynamicAssetSets must be an array");
				std::set<std::string> names;
				std::set<std::string> manifests;
				for (std::size_t index = 0; index < cook["DynamicAssetSets"].size(); ++index)
				{
					const Json& value = cook["DynamicAssetSets"][index];
					const std::string context = "Cook.DynamicAssetSets[" + std::to_string(index) + "]";
					if (!value.is_object())
						Fail(context + " must be an object");
					for (const auto& [key, child] : value.items())
					{
						(void)child;
						if (key != "Name" && key != "Manifest")
							Fail(context + " contains an unknown field: " + key);
					}
					if (!value.contains("Name") || !value["Name"].is_string() ||
						!value.contains("Manifest") || !value["Manifest"].is_string())
					{
						Fail(context + " requires string Name and Manifest fields");
					}
					DynamicAssetSetConfig assetSet;
					assetSet.Name = Trim(value["Name"].get<std::string>());
					if (assetSet.Name.empty() || assetSet.Name.size() > 128 ||
						std::any_of(assetSet.Name.begin(), assetSet.Name.end(), [](unsigned char character)
						{
							return character < 32 || character == ':';
						}))
					{
						Fail(context + ".Name must be 1-128 characters without ':' or control characters");
					}
					assetSet.Manifest = NormalizeLogicalPath(
						value["Manifest"].get<std::string>(), context + ".Manifest");
					if (!names.insert(FoldPath(assetSet.Name)).second)
						Fail("Cook.DynamicAssetSets contains a duplicate Name: " + assetSet.Name);
					if (!manifests.insert(FoldPath(assetSet.Manifest)).second)
						Fail("Cook.DynamicAssetSets contains a duplicate Manifest: " + assetSet.Manifest);
					config.DynamicAssetSetNamesByManifest.emplace(
						FoldPath(assetSet.Manifest), assetSet.Name);
					config.DynamicAssetSets.push_back(std::move(assetSet));
				}
			}
			RuntimeResourceWarmupSchema warmupSchema = ParseRuntimeResourceWarmupSchema(
				game,
				[](std::string_view path, std::string_view context)
				{
					return NormalizeLogicalPath(
						std::string(path),
						std::string(context));
				});
			config.WarmupGroups = std::move(warmupSchema.Groups);

			if (config.EntryScene.empty())
				Fail("Game config is missing Game.DefaultScene");
			if (config.ContentVersion.empty() || config.ContentVersion.size() > 64 ||
				!std::all_of(config.ContentVersion.begin(), config.ContentVersion.end(), [](unsigned char character)
				{
					return (character >= 'a' && character <= 'z') ||
						(character >= 'A' && character <= 'Z') ||
						(character >= '0' && character <= '9') ||
						character == '.' || character == '_' || character == '-';
				}))
			{
				Fail("Cook.ContentVersion must use 1-64 ASCII letters, digits, dots, underscores, or hyphens");
			}
			config.ContentVersion = ToLowerAscii(std::move(config.ContentVersion));
			if (config.CompressionLevel < 1 || config.CompressionLevel > 9)
				Fail("Cook.CompressionLevel must be between 1 and 9");

			for (std::string extension : ReadStringArray(cook, "CookedJsonExtensions"))
			{
				extension = NormalizeExtension(std::move(extension));
				if (!extension.empty())
					config.CookedJsonExtensions.insert(extension);
			}
			for (std::string directory : ReadStringArray(cook, "ExcludeDirectories"))
				config.ExcludedDirectories.insert(FoldPath(NormalizeLogicalPath(std::move(directory), "Cook.ExcludeDirectories")));
			for (std::string extension : ReadStringArray(cook, "ExcludeExtensions"))
			{
				extension = NormalizeExtension(std::move(extension));
				if (!extension.empty())
					config.ExcludedExtensions.insert(extension);
			}
			for (std::string virtualFile : ReadStringArray(cook, "VirtualFiles"))
				config.VirtualFiles.insert(FoldPath(NormalizeLogicalPath(std::move(virtualFile), "Cook.VirtualFiles")));

			config.ExcludedDirectories.insert("__pycache__");
			config.ExcludedExtensions.insert(".pyc");
			return config;
		}

		void MergeEngineCookConfig(CookConfig& config, const Json& root)
		{
			if (!root.is_object() || !root.contains("SchemaVersion") ||
				!root["SchemaVersion"].is_number_unsigned() ||
				root["SchemaVersion"].get<std::uint64_t>() != 1)
			{
				Fail("Engine cook config must be an object with SchemaVersion 1");
			}
			for (const auto& [key, value] : root.items())
			{
				if (key != "SchemaVersion" && key != "RootFiles" &&
					key != "IncludeDirectories" && key != "VirtualFiles")
				{
					Fail("Engine cook config contains an unknown field: " + key);
				}
			}

			std::vector<std::string> rootFiles = ReadStringArray(root, "RootFiles");
			std::vector<std::string> includeDirectories =
				ReadStringArray(root, "IncludeDirectories");
			config.RootFiles.insert(
				config.RootFiles.end(), rootFiles.begin(), rootFiles.end());
			config.EngineRootFiles.insert(
				config.EngineRootFiles.end(), rootFiles.begin(), rootFiles.end());
			config.IncludeDirectories.insert(
				config.IncludeDirectories.end(),
				includeDirectories.begin(),
				includeDirectories.end());
			for (std::string virtualFile : ReadStringArray(root, "VirtualFiles"))
			{
				config.VirtualFiles.insert(FoldPath(NormalizeLogicalPath(
					std::move(virtualFile), "EngineCook.VirtualFiles")));
			}
		}

		bool IsExcludedDirectory(const std::string& logicalPath, const CookConfig& config)
		{
			const std::string folded = FoldPath(logicalPath);
			for (const std::string& excluded : config.ExcludedDirectories)
			{
				if (folded == excluded || folded.starts_with(excluded + "/"))
					return true;
				std::istringstream stream(folded);
				std::string component;
				while (std::getline(stream, component, '/'))
				{
					if (component == excluded)
						return true;
				}
			}
			return false;
		}

		ProjectIndex BuildProjectIndex(const std::filesystem::path& projectRoot, const CookConfig& config)
		{
			ProjectIndex result;
			result.VirtualFiles = config.VirtualFiles;
			std::error_code error;
			std::filesystem::recursive_directory_iterator iterator(
				projectRoot,
				std::filesystem::directory_options::skip_permission_denied,
				error);
			if (error)
				Fail("Cannot enumerate project root: " + error.message());

			const std::filesystem::recursive_directory_iterator end;
			for (; iterator != end; iterator.increment(error))
			{
				if (error)
					Fail("Cannot enumerate project files: " + error.message());

				const auto& entry = *iterator;
				const std::filesystem::path relativePath = entry.path().lexically_relative(projectRoot);
				const std::string logicalPath = relativePath.generic_string();
				if (entry.is_symlink(error))
				{
					if (entry.is_directory(error))
						iterator.disable_recursion_pending();
					result.ExcludedByRule.push_back(logicalPath);
					continue;
				}
				if (entry.is_directory(error))
				{
					if (IsExcludedDirectory(logicalPath, config))
					{
						iterator.disable_recursion_pending();
						result.ExcludedByRule.push_back(logicalPath + "/");
					}
					continue;
				}
				if (!entry.is_regular_file(error))
					continue;

				const std::string extension = ToLowerAscii(entry.path().extension().string());
				if (IsExcludedDirectory(logicalPath, config) || config.ExcludedExtensions.contains(extension))
				{
					result.ExcludedByRule.push_back(logicalPath);
					continue;
				}

				const std::filesystem::path resolved = std::filesystem::weakly_canonical(entry.path(), error);
				if (error || !IsWithin(projectRoot, resolved))
					Fail("Project file escapes the project root: " + logicalPath);

				const std::string normalized = NormalizeLogicalPath(logicalPath, "Project file");
				const std::string key = FoldPath(normalized);
				if (key == FoldPath("Config/UserEngine.json") ||
					key == FoldPath("Saved/Config/UserEngine.json"))
				{
					result.ExcludedByRule.push_back(normalized);
					continue;
				}
				if (result.Files.contains(key))
					Fail("Case-insensitive project path collision: " + result.Files.at(key).LogicalPath + " and " + normalized);

				result.Files.emplace(key, IndexedFile{
					.LogicalPath = normalized,
					.PhysicalPath = resolved,
					.Size = static_cast<std::uint64_t>(entry.file_size(error))
				});
				if (error)
					Fail("Cannot read file size for: " + normalized);
				if (!extension.empty())
					result.KnownExtensions.insert(extension);
			}

			std::sort(result.ExcludedByRule.begin(), result.ExcludedByRule.end());
			return result;
		}

		const IndexedFile& RequireIndexedFile(const ProjectIndex& index, const std::string& logicalPath, const std::string& context)
		{
			const auto found = index.Files.find(FoldPath(logicalPath));
			if (found == index.Files.end())
				Fail(context + ": file does not exist or is excluded: " + logicalPath);
			return found->second;
		}

		std::vector<std::uint8_t> ReadBinaryFile(const IndexedFile& file)
		{
			std::ifstream stream(file.PhysicalPath, std::ios::binary);
			if (!stream)
				Fail("Cannot open project file: " + file.LogicalPath);
			std::vector<std::uint8_t> data(static_cast<std::size_t>(file.Size));
			if (!data.empty())
			{
				stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
				if (!stream)
					Fail("Cannot read project file: " + file.LogicalPath);
			}
			return data;
		}

		void CollectJsonStrings(const Json& value, std::vector<std::string>& strings)
		{
			if (value.is_string())
			{
				strings.push_back(value.get<std::string>());
				return;
			}
			if (value.is_array())
			{
				for (const Json& child : value)
					CollectJsonStrings(child, strings);
				return;
			}
			if (value.is_object())
			{
				for (const auto& [key, child] : value.items())
					CollectJsonStrings(child, strings);
			}
		}

		bool IsProjectRootedReference(std::string_view value)
		{
			std::string normalized(value);
			std::replace(normalized.begin(), normalized.end(), '\\', '/');
			const std::string folded = ToLowerAscii(normalized);
			return folded.starts_with("assets/") || folded.starts_with("config/") ||
				folded.starts_with("scripts/") || folded.starts_with("shaders/") ||
				folded.starts_with("scripts.");
		}

		std::optional<std::string> ResolveReference(
			std::string rawReference,
			const std::string& ownerPath,
			const ProjectIndex& index,
			std::string* outVirtualPath = nullptr,
			bool* outUnresolvedCandidate = nullptr)
		{
			if (outVirtualPath)
				outVirtualPath->clear();
			if (outUnresolvedCandidate)
				*outUnresolvedCandidate = false;
			rawReference = Trim(std::move(rawReference));
			if (rawReference.empty() || rawReference.starts_with("data:") || rawReference.starts_with("http://") ||
				rawReference.starts_with("https://"))
				return std::nullopt;

			if (IsAbsoluteAssetPath(rawReference))
				Fail(ownerPath + ": absolute asset reference is forbidden: " + rawReference);

			const bool bRootedReference = IsProjectRootedReference(rawReference);
			std::string dottedScriptCandidate;
			if (rawReference.starts_with("Scripts.") && rawReference.ends_with(".py") &&
				rawReference.find_first_of("/\\") == std::string::npos)
			{
				dottedScriptCandidate = rawReference.substr(0, rawReference.size() - 3);
				std::replace(dottedScriptCandidate.begin(), dottedScriptCandidate.end(), '.', '/');
				dottedScriptCandidate += ".py";
			}

			std::vector<std::string> candidates;
			if (!dottedScriptCandidate.empty())
				candidates.push_back(dottedScriptCandidate);
			std::string normalized = rawReference;
			std::replace(normalized.begin(), normalized.end(), '\\', '/');
			const std::string ownerDirectory = std::filesystem::path(ownerPath).parent_path().generic_string();
			if (!bRootedReference && !ownerDirectory.empty())
				candidates.push_back(ownerDirectory + "/" + normalized);
			const std::string foldedOwnerPath = FoldPath(ownerPath);
			if (!bRootedReference && foldedOwnerPath.starts_with("shaders/"))
				candidates.push_back("Shaders/" + normalized);
			candidates.push_back(normalized);

			for (std::string& candidate : candidates)
			{
				const std::optional<std::string> safeCandidate = NormalizeDependencyCandidate(
					std::move(candidate), ownerPath + " reference");
				if (!safeCandidate)
					continue;
				if (index.VirtualFiles.contains(FoldPath(*safeCandidate)))
				{
					if (outVirtualPath)
						*outVirtualPath = *safeCandidate;
					return std::nullopt;
				}
				const auto found = index.Files.find(FoldPath(*safeCandidate));
				if (found != index.Files.end())
					return found->second.LogicalPath;
			}

			const std::filesystem::path referencePath(normalized);
			const std::string extension = ToLowerAscii(referencePath.extension().string());
			const bool bLooksLikeKnownFile = !referencePath.stem().empty() && index.KnownExtensions.contains(extension);
			if (bRootedReference || bLooksLikeKnownFile)
				Fail(ownerPath + ": referenced file does not exist or is excluded: " + rawReference);
			if (outUnresolvedCandidate &&
				(rawReference.find_first_of("/\\") != std::string::npos ||
					(!referencePath.extension().empty() && !referencePath.stem().empty())))
			{
				*outUnresolvedCandidate = true;
			}
			return std::nullopt;
		}

		using DependencyMap = std::map<std::string, ResourceDependency>;

		struct DependencyDiagnostics
		{
			std::vector<DependencyDiagnostic> VirtualDependencies;
			std::vector<DependencyDiagnostic> UnresolvedCandidates;
			std::string Witness;
		};

		int DependencyStrength(DependencyKind kind)
		{
			switch (kind)
			{
			case DependencyKind::HardRuntime: return 3;
			case DependencyKind::SoftRuntime: return 2;
			case DependencyKind::PackagingOnly: return 1;
			}
			return 0;
		}

		int DependencySourceStrength(DependencySource source)
		{
			switch (source)
			{
			case DependencySource::ExplicitDynamic: return 3;
			case DependencySource::Structured: return 2;
			case DependencySource::Heuristic: return 1;
			}
			return 0;
		}

		bool EvidenceLess(const DependencyEvidence& left, const DependencyEvidence& right)
		{
			return std::tie(
				left.Owner, left.Target, left.Provider, left.Schema, left.Location,
				left.Reason, left.RawReference, left.Source, left.Kind) <
				std::tie(
					right.Owner, right.Target, right.Provider, right.Schema, right.Location,
					right.Reason, right.RawReference, right.Source, right.Kind);
		}

		void AddDependency(
			DependencyMap& dependencies,
			const std::string& ownerPath,
			std::string dependencyPath,
			DependencyKind kind,
			DependencySource source,
			std::string_view rawReference = {},
			std::string_view provider = {},
			std::string_view schema = {},
			std::string_view location = {},
			std::string_view reason = {})
		{
			if (source == DependencySource::Heuristic && kind != DependencyKind::PackagingOnly)
				Fail(ownerPath + ": heuristic dependencies must remain PackagingOnly");
			if (FoldPath(ownerPath) == FoldPath(dependencyPath))
				return;

			DependencyEvidence evidence{
				.Owner = ownerPath,
				.Target = dependencyPath,
				.RawReference = rawReference.empty() ? dependencyPath : std::string(rawReference),
				.Kind = kind,
				.Source = source,
				.Provider = std::string(provider),
				.Schema = std::string(schema),
				.Location = std::string(location),
				.Reason = std::string(reason)
			};
			const std::string key = FoldPath(dependencyPath);
			const auto found = dependencies.find(key);
			if (found == dependencies.end())
			{
				dependencies.emplace(key, ResourceDependency{
					.Path = std::move(dependencyPath),
					.Kind = kind,
					.Source = source,
					.Evidence = { std::move(evidence) }
				});
				return;
			}

			ResourceDependency& existing = found->second;
			if (DependencyStrength(kind) > DependencyStrength(existing.Kind))
				existing.Kind = kind;
			if (DependencySourceStrength(source) > DependencySourceStrength(existing.Source))
				existing.Source = source;
			if (std::find_if(
					existing.Evidence.begin(), existing.Evidence.end(),
					[&](const DependencyEvidence& value)
					{
						return !EvidenceLess(value, evidence) && !EvidenceLess(evidence, value);
					}) == existing.Evidence.end())
			{
				existing.Evidence.push_back(std::move(evidence));
				std::sort(existing.Evidence.begin(), existing.Evidence.end(), EvidenceLess);
			}
		}

		struct StrictReferenceOptions
		{
			bool bAllowEmpty = true;
			bool bAllowNone = true;
			bool bAllowDataUri = false;
			bool bAllowHttpUri = false;
			bool bAllowBuiltinTextureToken = false;
		};

		std::string BuildStrictDependencyError(
			std::string_view message,
			std::string_view rawReference,
			const std::string& ownerPath,
			DependencyKind kind,
			std::string_view provider,
			std::string_view location,
			const DependencyDiagnostics* diagnostics)
		{
			std::ostringstream stream;
			stream << ownerPath << ": " << message << " '" << rawReference << "'"
				<< " [provider=" << provider << ", location=" << location
				<< ", kind=" << ToString(kind) << ']';
			if (diagnostics && !diagnostics->Witness.empty())
				stream << " [witness=" << diagnostics->Witness << ']';
			return stream.str();
		}

		std::optional<std::string> ResolveStrictReference(
			std::string rawReference,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyKind kind,
			DependencySource source,
			std::string_view provider,
			std::string_view schema,
			std::string_view location,
			std::string_view reason,
			const StrictReferenceOptions& options,
			DependencyDiagnostics* diagnostics)
		{
			rawReference = Trim(std::move(rawReference));
			const std::string foldedReference = ToLowerAscii(rawReference);
			if (rawReference.empty())
			{
				if (options.bAllowEmpty)
					return std::nullopt;
				Fail(BuildStrictDependencyError(
					"structured dependency path is empty", rawReference, ownerPath,
					kind, provider, location, diagnostics));
			}
			if (options.bAllowNone && foldedReference == "none")
				return std::nullopt;
			if (options.bAllowBuiltinTextureToken &&
				(foldedReference == "white" || foldedReference == "black" || foldedReference == "normal"))
			{
				return std::nullopt;
			}
			if (foldedReference.starts_with("data:"))
			{
				if (options.bAllowDataUri)
					return std::nullopt;
				Fail(BuildStrictDependencyError(
					"external URI is not allowed for structured dependency", rawReference,
					ownerPath, kind, provider, location, diagnostics));
			}
			if (foldedReference.starts_with("http://") || foldedReference.starts_with("https://"))
			{
				if (options.bAllowHttpUri)
					return std::nullopt;
				Fail(BuildStrictDependencyError(
					"external URI is not allowed for structured dependency", rawReference,
					ownerPath, kind, provider, location, diagnostics));
			}

			const std::size_t scheme = rawReference.find(':');
			if (scheme != std::string::npos && scheme > 0 &&
				std::all_of(
					rawReference.begin(), rawReference.begin() + static_cast<std::ptrdiff_t>(scheme),
					[](unsigned char character)
					{
						return std::isalnum(character) || character == '+' || character == '-' || character == '.';
					}))
			{
				Fail(BuildStrictDependencyError(
					"unsupported URI scheme in structured dependency", rawReference,
					ownerPath, kind, provider, location, diagnostics));
			}
			if (IsAbsoluteAssetPath(rawReference) || HasParentTraversal(rawReference))
			{
				Fail(BuildStrictDependencyError(
					"unsafe structured dependency path", rawReference, ownerPath,
					kind, provider, location, diagnostics));
			}

			const bool bRootedReference = IsProjectRootedReference(rawReference);
			std::string dottedScriptCandidate;
			if (rawReference.starts_with("Scripts.") && rawReference.ends_with(".py") &&
				rawReference.find_first_of("/\\") == std::string::npos)
			{
				dottedScriptCandidate = rawReference.substr(0, rawReference.size() - 3);
				std::replace(dottedScriptCandidate.begin(), dottedScriptCandidate.end(), '.', '/');
				dottedScriptCandidate += ".py";
			}

			std::vector<std::string> candidates;
			if (!dottedScriptCandidate.empty())
				candidates.push_back(dottedScriptCandidate);
			std::string normalizedReference = rawReference;
			std::replace(normalizedReference.begin(), normalizedReference.end(), '\\', '/');
			const std::string ownerDirectory = std::filesystem::path(ownerPath).parent_path().generic_string();
			if (!bRootedReference && !ownerDirectory.empty())
				candidates.push_back(ownerDirectory + "/" + normalizedReference);
			if (!bRootedReference && FoldPath(ownerPath).starts_with("shaders/"))
				candidates.push_back("Shaders/" + normalizedReference);
			candidates.push_back(normalizedReference);

			std::set<std::string> visited;
			for (std::string candidate : candidates)
			{
				candidate = NormalizeLogicalPath(
					std::move(candidate), ownerPath + " structured dependency");
				if (!visited.insert(FoldPath(candidate)).second)
					continue;
				if (index.VirtualFiles.contains(FoldPath(candidate)))
				{
					if (diagnostics)
					{
						diagnostics->VirtualDependencies.push_back({
							.Owner = ownerPath,
							.Target = candidate,
							.RawReference = rawReference,
							.Kind = kind,
							.Source = source,
							.Provider = std::string(provider),
							.Schema = std::string(schema),
							.Location = std::string(location),
							.Reason = std::string(reason)
						});
					}
					return std::nullopt;
				}
				const auto found = index.Files.find(FoldPath(candidate));
				if (found != index.Files.end())
					return found->second.LogicalPath;
			}

			Fail(BuildStrictDependencyError(
				"structured dependency does not exist or is excluded", rawReference,
				ownerPath, kind, provider, location, diagnostics));
		}

		void ResolveAndAddDependency(
			std::string rawReference,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyKind kind,
			DependencySource source,
			DependencyDiagnostics* diagnostics = nullptr,
			std::string_view provider = "HeuristicScanner",
			std::string_view schema = {},
			std::string_view location = {},
			std::string_view reason = {})
		{
			const std::string originalReference = rawReference;
			std::string virtualPath;
			bool bUnresolvedCandidate = false;
			if (const auto resolved = ResolveReference(
					std::move(rawReference), ownerPath, index,
					&virtualPath, &bUnresolvedCandidate))
			{
				AddDependency(
					dependencies, ownerPath, *resolved, kind, source, originalReference,
					provider, schema, location, reason);
			}
			else if (diagnostics && !virtualPath.empty())
			{
				diagnostics->VirtualDependencies.push_back({
					.Owner = ownerPath,
					.Target = virtualPath,
					.RawReference = originalReference,
					.Kind = kind,
					.Source = source,
					.Provider = std::string(provider),
					.Schema = std::string(schema),
					.Location = std::string(location),
					.Reason = std::string(reason)
				});
			}
			else if (diagnostics && bUnresolvedCandidate)
			{
				diagnostics->UnresolvedCandidates.push_back({
					.Owner = ownerPath,
					.RawReference = originalReference,
					.Kind = kind,
					.Source = source,
					.Provider = std::string(provider),
					.Schema = std::string(schema),
					.Location = std::string(location),
					.Reason = std::string(reason)
				});
			}
		}

		void ResolveAndAddStrictDependency(
			std::string rawReference,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyKind kind,
			DependencySource source,
			std::string_view provider,
			std::string_view schema,
			std::string_view location,
			std::string_view reason,
			const StrictReferenceOptions& options = {},
			DependencyDiagnostics* diagnostics = nullptr)
		{
			const std::string originalReference = rawReference;
			if (const auto resolved = ResolveStrictReference(
					std::move(rawReference), ownerPath, index, kind, source,
					provider, schema, location, reason, options, diagnostics))
			{
				AddDependency(
					dependencies, ownerPath, *resolved, kind, source, originalReference,
					provider, schema, location, reason);
			}
		}

		void AddStructuredField(
			const Json& object,
			const char* field,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyKind kind,
			DependencyDiagnostics* diagnostics = nullptr,
			std::string_view provider = "StructuredJson",
			std::string_view schema = {},
			std::string location = {},
			StrictReferenceOptions options = {})
		{
			if (!object.is_object() || !object.contains(field))
				return;
			if (!object[field].is_string())
				Fail(ownerPath + ": structured resource field '" + field + "' must be a string");
			if (location.empty())
				location = "/" + std::string(field);
			ResolveAndAddStrictDependency(
				object[field].get<std::string>(),
				ownerPath,
				index,
				dependencies,
				kind,
				DependencySource::Structured,
				provider,
				schema,
				location,
				std::string("structured field ") + field,
				options,
				diagnostics);
		}

		void AddStructuredStringArrayField(
			const Json& object,
			const char* field,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyKind kind,
			DependencyDiagnostics* diagnostics = nullptr,
			std::string_view provider = "StructuredJson",
			std::string_view schema = {},
			std::string location = {},
			StrictReferenceOptions options = {})
		{
			if (!object.is_object() || !object.contains(field))
				return;
			const Json& values = object[field];
			if (!values.is_array())
				Fail(ownerPath + ": structured resource field '" + field + "' must be an array");
			for (std::size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex)
			{
				const Json& value = values[valueIndex];
				if (!value.is_string())
					Fail(ownerPath + ": structured resource field '" + field + "' entries must be strings");
				const std::string dependencyPath = value.get<std::string>();
				if (dependencyPath.empty())
					continue;
				ResolveAndAddStrictDependency(
					dependencyPath,
					ownerPath,
					index,
					dependencies,
					kind,
					DependencySource::Structured,
					provider,
					schema,
					location.empty()
						? "/" + std::string(field) + "/" + std::to_string(valueIndex)
						: location + "/" + std::to_string(valueIndex),
					std::string("structured array field ") + field,
					options,
					diagnostics);
			}
		}

		void VisitNamedStructuredField(
			const Json& value,
			const char* field,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyKind kind,
			DependencyDiagnostics* diagnostics = nullptr,
			std::string_view provider = "StructuredJson",
			std::string_view schema = {},
			std::string location = {},
			StrictReferenceOptions options = {})
		{
			if (value.is_array())
			{
				for (std::size_t indexValue = 0; indexValue < value.size(); ++indexValue)
				{
					VisitNamedStructuredField(
						value[indexValue], field, ownerPath, index, dependencies, kind,
						diagnostics, provider, schema, location + "/" + std::to_string(indexValue), options);
				}
				return;
			}
			if (!value.is_object())
				return;

			AddStructuredField(
				value, field, ownerPath, index, dependencies, kind,
				diagnostics, provider, schema, location + "/" + field, options);
			for (const auto& [childKey, child] : value.items())
			{
				VisitNamedStructuredField(
					child, field, ownerPath, index, dependencies, kind,
					diagnostics, provider, schema, location + "/" + childKey, options);
			}
		}

		void VisitSceneStructuredDependencies(
			const Json& value,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyDiagnostics* diagnostics = nullptr,
			std::string location = {})
		{
			if (value.is_array())
			{
				for (std::size_t childIndex = 0; childIndex < value.size(); ++childIndex)
				{
					VisitSceneStructuredDependencies(
						value[childIndex], ownerPath, index, dependencies, diagnostics,
						location + "/" + std::to_string(childIndex));
				}
				return;
			}
			if (!value.is_object())
				return;

			for (const auto& [key, child] : value.items())
			{
				const std::string childLocation = location + "/" + key;
				if (key == "MeshComponent")
				{
					AddStructuredField(child, "Path", ownerPath, index, dependencies, DependencyKind::HardRuntime,
						diagnostics, "SceneComponent", "MeshComponent", childLocation + "/Path");
					AddStructuredField(child, "meshResPath", ownerPath, index, dependencies, DependencyKind::HardRuntime,
						diagnostics, "SceneComponent", "MeshComponent", childLocation + "/meshResPath");
					AddStructuredStringArrayField(
						child,
						"MaterialOverrides",
						ownerPath,
						index,
						dependencies,
						DependencyKind::HardRuntime,
						diagnostics, "SceneComponent", "MeshComponent",
						childLocation + "/MaterialOverrides");
				}
				else if (key == "SpriteRendererComponent")
				{
					AddStructuredField(child, "Texture", ownerPath, index, dependencies, DependencyKind::HardRuntime,
						diagnostics, "SceneComponent", "SpriteRendererComponent", childLocation + "/Texture");
				}
				else if (key == "PythonScriptComponent")
				{
					AddStructuredField(child, "Path", ownerPath, index, dependencies, DependencyKind::HardRuntime,
						diagnostics, "SceneComponent", "PythonScriptComponent", childLocation + "/Path");
				}
				else if (key == "AnimatorComponent")
				{
					AddStructuredField(child, "animResPath", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
						diagnostics, "SceneComponent", "AnimatorComponent", childLocation + "/animResPath");
					AddStructuredField(child, "controllerPath", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
						diagnostics, "SceneComponent", "AnimatorComponent", childLocation + "/controllerPath");
				}
				else if (key == "SoundComponent")
				{
					AddStructuredField(child, "Path", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
						diagnostics, "SceneComponent", "SoundComponent", childLocation + "/Path");
				}
				else if (key == "BehaviorTreeComponent")
				{
					AddStructuredField(child, "btResPath", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
						diagnostics, "SceneComponent", "BehaviorTreeComponent", childLocation + "/btResPath");
				}
				else if (key == "EnvironmentLightComponent")
				{
					AddStructuredField(child, "HdrTexturePath", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
						diagnostics, "SceneComponent", "EnvironmentLightComponent", childLocation + "/HdrTexturePath");
				}
				else if (key == "__SceneSettings")
				{
					AddStructuredField(child, "EnvironmentHdrPath", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
						diagnostics, "SceneDocument", "SceneSettings", childLocation + "/EnvironmentHdrPath");
				}
				VisitSceneStructuredDependencies(
					child, ownerPath, index, dependencies, diagnostics, childLocation);
			}
		}

		void CollectMaterialShaderManifestDependencies(
			const Json& json,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyDiagnostics* diagnostics = nullptr)
		{
			(void)diagnostics;
			const MaterialShaderManifestLoadResult loadResult =
				ParseMaterialShaderManifest(json, ownerPath);
			if (!loadResult.Succeeded())
			{
				std::ostringstream message;
				message << ownerPath << ": material shader manifest validation failed";
				for (const MaterialShaderDiagnostic& diagnostic : loadResult.Diagnostics)
				{
					if (diagnostic.Severity != MaterialShaderDiagnosticSeverity::Error)
						continue;
					message << "\n  [" << diagnostic.Code << "] " << diagnostic.Message;
				}
				Fail(message.str());
			}

			const auto collectArtifact = [&](const MaterialShaderArtifact& artifact)
			{
				const std::string artifactPath = NormalizeLogicalPath(
					"Shaders/CACHES/" + artifact.RelativePath,
					ownerPath + " artifact " + artifact.Permutation + "/" + artifact.Stage);
				const IndexedFile& indexedArtifact = RequireIndexedFile(
					index,
					artifactPath,
					ownerPath + " material shader artifact");
				std::string actualHash;
				std::string hashError;
				if (!ComputeMaterialShaderFileSha256(
						indexedArtifact.PhysicalPath,
						actualHash,
						hashError))
				{
					Fail(ownerPath + ": cannot hash material shader artifact " +
						indexedArtifact.LogicalPath + ": " + hashError);
				}
				const std::string expectedHash = ToLowerAscii(artifact.ContentHash);
				if (actualHash != expectedHash)
				{
					Fail(
						ownerPath + ": material shader artifact hash mismatch for " +
						indexedArtifact.LogicalPath + " (expected " + expectedHash +
						", actual " + actualHash + ")");
				}
				AddDependency(
					dependencies,
					ownerPath,
					indexedArtifact.LogicalPath,
					DependencyKind::HardRuntime,
					DependencySource::Structured,
					artifact.RelativePath,
					"MaterialShaderManifest",
					"hengine.material-shader-manifest",
					"/ArtifactSets/Artifacts/RelativePath",
					"validated material shader artifact");
			};
			if (loadResult.Manifest.ArtifactSets.empty())
			{
				for (const MaterialShaderArtifact& artifact : loadResult.Manifest.Artifacts)
					collectArtifact(artifact);
			}
			else
			{
				for (const MaterialShaderArtifactSet& set : loadResult.Manifest.ArtifactSets)
				{
					for (const MaterialShaderArtifact& artifact : set.Artifacts)
						collectArtifact(artifact);
				}
			}
		}

		void CollectStructuredJsonDependencies(
			const Json& json,
			const std::string& extension,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyDiagnostics* diagnostics = nullptr)
		{
			if (extension == ".hmesh")
			{
				if (json.contains("bufferVec") && json["bufferVec"].is_array())
				{
					for (std::size_t bufferIndex = 0; bufferIndex < json["bufferVec"].size(); ++bufferIndex)
					{
						StrictReferenceOptions options;
						options.bAllowDataUri = true;
						AddStructuredField(
							json["bufferVec"][bufferIndex], "uri", ownerPath, index, dependencies,
							DependencyKind::HardRuntime, diagnostics, "MeshResource", "MeshRes",
							"/bufferVec/" + std::to_string(bufferIndex) + "/uri", options);
					}
				}
				if (json.contains("primitives") && json["primitives"].is_array())
				{
					for (std::size_t primitiveIndex = 0; primitiveIndex < json["primitives"].size(); ++primitiveIndex)
					{
						AddStructuredField(
							json["primitives"][primitiveIndex], "materialResPath", ownerPath, index,
							dependencies, DependencyKind::HardRuntime, diagnostics, "MeshResource", "MeshRes",
							"/primitives/" + std::to_string(primitiveIndex) + "/materialResPath");
					}
				}
				AddStructuredField(
					json, "skeletonPath", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
					diagnostics, "MeshResource", "MeshRes", "/skeletonPath");
			}
			else if (extension == ".hmat")
			{
				AddStructuredField(
					json, "ShaderPath", ownerPath, index, dependencies, DependencyKind::HardRuntime,
					diagnostics, "MaterialResource", "MaterialRes", "/ShaderPath");
				const int schemaVersion = json.value("SchemaVersion", 1);
				if (schemaVersion >= 2)
				{
					if (json.contains("Textures") && !json["Textures"].is_object())
						Fail(ownerPath + ": V2 material Textures must be an object");
					if (json.contains("Textures"))
					{
						for (const auto& [name, texture] : json["Textures"].items())
						{
							if (!texture.is_object())
								Fail(ownerPath + ": V2 texture property '" + name + "' must be an object");
							if (!texture.contains("Type") || !texture["Type"].is_string() ||
								ToLowerAscii(texture["Type"].get<std::string>()) != "texture2d")
							{
								Fail(ownerPath + ": V2 texture property '" + name + "' must have Type Texture2D");
							}
							StrictReferenceOptions options;
							options.bAllowBuiltinTextureToken = true;
							AddStructuredField(
								texture, "Path", ownerPath, index, dependencies,
								DependencyKind::SoftRuntime, diagnostics, "MaterialResource", "MaterialResV2",
								"/Textures/" + name + "/Path", options);
						}
					}

					const std::string type = json.value("Type", std::string("Default"));
					if (ToLowerAscii(type) != "default" && json.contains("ShaderPath") &&
						json["ShaderPath"].is_string())
					{
						std::string shaderPath = NormalizeLogicalPath(
							json["ShaderPath"].get<std::string>(), ownerPath + " ShaderPath");
						constexpr std::string_view shaderPrefix = "Shaders/";
						if (!shaderPath.starts_with(shaderPrefix) ||
							ToLowerAscii(std::filesystem::path(shaderPath).extension().string()) != ".hlsl")
						{
							Fail(ownerPath + ": V2 custom material ShaderPath must be Shaders-relative .hlsl");
						}
						const std::string manifestPath = "Shaders/CACHES/Materials/" +
							shaderPath.substr(shaderPrefix.size()) + ".material.json";
						ResolveAndAddStrictDependency(
							manifestPath, ownerPath, index, dependencies,
							DependencyKind::HardRuntime, DependencySource::Structured,
							"MaterialResource", "MaterialResV2", "/ShaderPath",
							"custom material artifact manifest", {}, diagnostics);
					}
				}
				else
				{
					StrictReferenceOptions textureOptions;
					textureOptions.bAllowBuiltinTextureToken = true;
					VisitNamedStructuredField(
						json, "textureSourcePath", ownerPath, index, dependencies,
						DependencyKind::SoftRuntime, diagnostics, "MaterialResource", "MaterialResV1", {},
						textureOptions);
					if (json.contains("Properties") && json["Properties"].is_object() &&
						json["Properties"].contains("ExposedProperties") &&
						json["Properties"]["ExposedProperties"].is_object())
					{
						for (const Json& value : json["Properties"]["ExposedProperties"])
						{
							if (value.is_string())
							{
								ResolveAndAddDependency(
									value.get<std::string>(), ownerPath, index, dependencies,
									DependencyKind::PackagingOnly, DependencySource::Heuristic,
									diagnostics, "LegacyMaterialExposedProperties", "MaterialResV1",
									"/Properties/ExposedProperties", "legacy untyped exposed string");
							}
						}
					}
				}
			}
			else if (extension == ".json" && FoldPath(ownerPath).ends_with(".material.json"))
			{
				CollectMaterialShaderManifestDependencies(json, ownerPath, index, dependencies, diagnostics);
			}
			else if (extension == ".he" || extension == ".hprefab")
			{
				VisitSceneStructuredDependencies(json, ownerPath, index, dependencies, diagnostics);
			}
			else if (extension == ".hui")
			{
				AddStructuredField(json, "GeneratedRmlPath", ownerPath, index, dependencies, DependencyKind::HardRuntime,
					diagnostics, "UiResource", "UIRes", "/GeneratedRmlPath");
				AddStructuredField(json, "GeneratedRcssPath", ownerPath, index, dependencies, DependencyKind::HardRuntime,
					diagnostics, "UiResource", "UIRes", "/GeneratedRcssPath");
				VisitNamedStructuredField(json, "FontPath", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
					diagnostics, "UiResource", "UIRes");
				VisitNamedStructuredField(json, "ImagePath", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
					diagnostics, "UiResource", "UIRes");
				VisitNamedStructuredField(json, "BackgroundImage", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
					diagnostics, "UiResource", "UIRes");
			}
			else if (extension == ".hanim")
			{
				AddStructuredField(json, "meshResPath", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
					diagnostics, "AnimationResource", "AnimRes", "/meshResPath");
				AddStructuredField(json, "targetSkeletonPath", ownerPath, index, dependencies, DependencyKind::HardRuntime,
					diagnostics, "AnimationResource", "AnimRes", "/targetSkeletonPath");
			}
			else if (extension == ".hanimctrl")
			{
				AddStructuredField(json, "targetSkeletonPath", ownerPath, index, dependencies, DependencyKind::HardRuntime,
					diagnostics, "AnimationControllerResource", "AnimControllerRes", "/targetSkeletonPath");
				if (json.contains("nodes") && json["nodes"].is_array())
				{
					for (std::size_t nodeIndex = 0; nodeIndex < json["nodes"].size(); ++nodeIndex)
					{
						AddStructuredField(
							json["nodes"][nodeIndex], "clipPath", ownerPath, index, dependencies,
							DependencyKind::HardRuntime, diagnostics, "AnimationControllerResource",
							"AnimControllerRes", "/nodes/" + std::to_string(nodeIndex) + "/clipPath");
					}
				}
			}
		}

		void CollectTextReferences(
			const std::string& text,
			std::vector<std::string>& references,
			bool bCollectQuotedStrings)
		{
			if (bCollectQuotedStrings)
			{
				static const std::regex quotedString(R"hecook("([^"\r\n]+)"|'([^'\r\n]+)')hecook");
				for (std::sregex_iterator iterator(text.begin(), text.end(), quotedString), end; iterator != end; ++iterator)
					references.push_back((*iterator)[1].matched ? (*iterator)[1].str() : (*iterator)[2].str());
			}

			static const std::regex rootedPath(R"hecook((?:Assets|Config|Scripts|Shaders)[/\\][A-Za-z0-9_@.+/\\-]+)hecook");
			for (std::sregex_iterator iterator(text.begin(), text.end(), rootedPath), end; iterator != end; ++iterator)
				references.push_back(iterator->str());
		}

		void CollectShaderIncludeDependencies(
			const std::string& text,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyDiagnostics* diagnostics)
		{
			static const std::regex includePattern(
				R"hecook(^\s*#\s*include\s*[<\"]([^>\"]+)[>\"])hecook");
			std::istringstream stream(text);
			std::string line;
			std::size_t lineNumber = 0;
			while (std::getline(stream, line))
			{
				++lineNumber;
				std::smatch match;
				if (!std::regex_search(line, match, includePattern))
					continue;
				ResolveAndAddStrictDependency(
					match[1].str(), ownerPath, index, dependencies,
					DependencyKind::PackagingOnly, DependencySource::Structured,
					"ShaderInclude", "HLSL", "/line/" + std::to_string(lineNumber),
					"shader include", {}, diagnostics);
			}
		}

		void CollectRmlDependencies(
			const std::string& text,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyDiagnostics* diagnostics)
		{
			static const std::regex tagPattern(
				R"hecook(<\s*(?:link|img|svg|lottie|include|script)\b[^>]*>)hecook",
				std::regex_constants::icase);
			static const std::regex attributePattern(
				R"hecook(\b(?:src|href)\s*=\s*(?:"([^"]+)"|'([^']+)'|([^\s>]+)))hecook",
				std::regex_constants::icase);
			StrictReferenceOptions options;
			options.bAllowDataUri = true;
			for (std::sregex_iterator tag(text.begin(), text.end(), tagPattern), end; tag != end; ++tag)
			{
				const std::string tagText = tag->str();
				for (std::sregex_iterator attribute(tagText.begin(), tagText.end(), attributePattern);
					attribute != end; ++attribute)
				{
					const std::string reference = (*attribute)[1].matched ? (*attribute)[1].str()
						: ((*attribute)[2].matched ? (*attribute)[2].str() : (*attribute)[3].str());
					if (reference.empty() || reference.front() == '#')
						continue;
					const std::size_t offset = static_cast<std::size_t>(tag->position() + attribute->position());
					ResolveAndAddStrictDependency(
						reference, ownerPath, index, dependencies,
						DependencyKind::SoftRuntime, DependencySource::Structured,
						"RmlMarkup", "RML", "/byte/" + std::to_string(offset),
						"RML resource attribute", options, diagnostics);
				}
			}
		}

		void CollectRcssDependencies(
			const std::string& text,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyDiagnostics* diagnostics)
		{
			static const std::regex functionPattern(
				R"hecook(\b(url|image)\s*\(\s*(?:"([^"]+)"|'([^']+)'|([^\s\)]+)))hecook",
				std::regex_constants::icase);
			StrictReferenceOptions options;
			options.bAllowDataUri = true;
			for (std::sregex_iterator match(text.begin(), text.end(), functionPattern), end;
				match != end; ++match)
			{
				const std::string reference = (*match)[2].matched ? (*match)[2].str()
					: ((*match)[3].matched ? (*match)[3].str() : (*match)[4].str());
				if (reference.empty() || reference.front() == '#' ||
					((*match)[1].str() == "image" && !(*match)[2].matched && !(*match)[3].matched &&
						reference.find_first_of("/\\.") == std::string::npos))
				{
					continue;
				}
				ResolveAndAddStrictDependency(
					reference, ownerPath, index, dependencies,
					DependencyKind::SoftRuntime, DependencySource::Structured,
					"RcssResource", "RCSS", "/byte/" + std::to_string(match->position()),
					"RCSS url/image resource", options, diagnostics);
			}
		}

		bool IsPythonCommentLine(const std::string& text, std::size_t position)
		{
			const std::size_t lineStart = position == 0 ? 0 : text.rfind('\n', position - 1) + 1;
			std::size_t firstCharacter = lineStart;
			while (firstCharacter < position &&
				(text[firstCharacter] == ' ' || text[firstCharacter] == '\t' || text[firstCharacter] == '\r'))
			{
				++firstCharacter;
			}
			return firstCharacter < position && text[firstCharacter] == '#';
		}

		bool HasPythonRawStringPrefix(const std::string& text, std::size_t stringContentPosition)
		{
			if (stringContentPosition == 0 ||
				(text[stringContentPosition - 1] != '\'' && text[stringContentPosition - 1] != '"'))
			{
				return false;
			}

			const char quote = text[stringContentPosition - 1];
			std::size_t quoteStart = stringContentPosition;
			std::size_t quoteCount = 0;
			while (quoteStart > 0 && text[quoteStart - 1] == quote && quoteCount < 3)
			{
				--quoteStart;
				++quoteCount;
			}
			if (quoteCount != 1 && quoteCount != 3)
				return false;

			std::size_t prefixStart = quoteStart;
			while (prefixStart > 0 && quoteStart - prefixStart < 3 &&
				std::isalpha(static_cast<unsigned char>(text[prefixStart - 1])))
			{
				--prefixStart;
			}
			for (std::size_t index = prefixStart; index < quoteStart; ++index)
			{
				if (text[index] == 'r' || text[index] == 'R')
					return true;
			}
			return false;
		}

		void LintTextPayload(
			const std::string& text,
			const std::string& logicalPath,
			bool bPython)
		{
			const auto bCanReportAt = [&](std::size_t position)
			{
				return !bPython || !IsPythonCommentLine(text, position);
			};

			for (std::size_t index = 0; index + 2 < text.size(); ++index)
			{
				const unsigned char character = static_cast<unsigned char>(text[index]);
				const bool bBoundary = index == 0 ||
					(!std::isalnum(static_cast<unsigned char>(text[index - 1])) && text[index - 1] != '_');
				if (bBoundary && std::isalpha(character) && text[index + 1] == ':' &&
					(text[index + 2] == '/' || text[index + 2] == '\\') && bCanReportAt(index))
				{
					Fail(logicalPath + ": absolute Windows path is forbidden in cooked text payloads");
				}

				const bool bUncBoundary = index == 0 ||
					(!std::isalnum(static_cast<unsigned char>(text[index - 1])) &&
						text[index - 1] != '_' && text[index - 1] != '-' && text[index - 1] != '.' &&
						text[index - 1] != '$' && text[index - 1] != '\\');
				if (!bUncBoundary || text[index] != '\\' || text[index + 1] != '\\' || !bCanReportAt(index))
					continue;

				std::size_t serverStart = index + 2;
				while (serverStart < text.size() && text[serverStart] == '\\')
					++serverStart;
				const std::size_t leadingSlashCount = serverStart - index;
				if (bPython && leadingSlashCount < 4 && !HasPythonRawStringPrefix(text, index))
					continue;
				std::size_t serverEnd = serverStart;
				while (serverEnd < text.size())
				{
					const unsigned char serverCharacter = static_cast<unsigned char>(text[serverEnd]);
					if (!std::isalnum(serverCharacter) && text[serverEnd] != '_' &&
						text[serverEnd] != '-' && text[serverEnd] != '.' && text[serverEnd] != '$')
					{
						break;
					}
					++serverEnd;
				}
				if (serverEnd == serverStart || serverEnd >= text.size() || text[serverEnd] != '\\')
					continue;

				std::size_t shareStart = serverEnd;
				while (shareStart < text.size() && text[shareStart] == '\\')
					++shareStart;
				if (shareStart < text.size() && !std::isspace(static_cast<unsigned char>(text[shareStart])) &&
					text[shareStart] != '\'' && text[shareStart] != '"')
				{
					Fail(logicalPath + ": UNC path is forbidden in cooked text payloads");
				}
			}
		}

		bool ContainsAsciiCaseInsensitive(
			const std::vector<std::uint8_t>& data,
			const std::string& needle)
		{
			if (needle.empty() || data.size() < needle.size())
				return false;
			const auto bEquals = [](unsigned char left, unsigned char right)
			{
				const auto lower = [](unsigned char character)
				{
					return character >= 'A' && character <= 'Z'
						? static_cast<unsigned char>(character - 'A' + 'a')
						: character;
				};
				return lower(left) == lower(right);
			};
			return std::search(
				data.begin(),
				data.end(),
				needle.begin(),
				needle.end(),
				bEquals) != data.end();
		}

#if defined(_WIN32)
		bool ContainsUtf16LeCaseInsensitive(
			const std::vector<std::uint8_t>& data,
			const std::wstring& needle)
		{
			if (needle.empty() || data.size() < needle.size() * 2)
				return false;
			const auto foldAscii = [](wchar_t character)
			{
				return character >= L'A' && character <= L'Z'
					? static_cast<wchar_t>(character - L'A' + L'a')
					: character;
			};
			for (std::size_t offset = 0; offset + needle.size() * 2 <= data.size(); ++offset)
			{
				bool bMatches = true;
				for (std::size_t index = 0; index < needle.size(); ++index)
				{
					const wchar_t value = static_cast<wchar_t>(
						static_cast<std::uint16_t>(data[offset + index * 2]) |
						(static_cast<std::uint16_t>(data[offset + index * 2 + 1]) << 8));
					if (foldAscii(value) != foldAscii(needle[index]))
					{
						bMatches = false;
						break;
					}
				}
				if (bMatches)
					return true;
			}
			return false;
		}
#endif

		bool ContainsRootLeak(
			const std::vector<std::uint8_t>& data,
			const std::filesystem::path& root)
		{
			std::set<std::string> narrowVariants;
			const auto appendNarrowVariants = [&](std::string value)
			{
				if (value.empty())
					return;
				narrowVariants.insert(value);
				std::replace(value.begin(), value.end(), '\\', '/');
				narrowVariants.insert(value);
				std::replace(value.begin(), value.end(), '/', '\\');
				narrowVariants.insert(value);
			};
			appendNarrowVariants(root.string());
			appendNarrowVariants(root.generic_string());
			appendNarrowVariants(root.lexically_normal().string());
			appendNarrowVariants(root.lexically_normal().generic_string());
			const auto appendUtf8Variants = [&](const std::u8string& value)
			{
				appendNarrowVariants(std::string(
					reinterpret_cast<const char*>(value.data()),
					value.size()));
			};
			appendUtf8Variants(root.u8string());
			appendUtf8Variants(root.generic_u8string());
			appendUtf8Variants(root.lexically_normal().u8string());
			appendUtf8Variants(root.lexically_normal().generic_u8string());
			for (const std::string& variant : narrowVariants)
			{
				if (ContainsAsciiCaseInsensitive(data, variant))
					return true;
			}

#if defined(_WIN32)
			std::set<std::wstring> wideVariants;
			const auto appendWideVariants = [&](std::wstring value)
			{
				if (value.empty())
					return;
				wideVariants.insert(value);
				std::replace(value.begin(), value.end(), L'\\', L'/');
				wideVariants.insert(value);
				std::replace(value.begin(), value.end(), L'/', L'\\');
				wideVariants.insert(value);
			};
			appendWideVariants(root.wstring());
			appendWideVariants(root.lexically_normal().wstring());
			for (const std::wstring& variant : wideVariants)
			{
				if (ContainsUtf16LeCaseInsensitive(data, variant))
					return true;
			}
#endif
			return false;
		}

		void LintAbsoluteRootLeaks(
			const std::vector<std::uint8_t>& data,
			const std::string& logicalPath,
			const std::filesystem::path& projectRoot,
			const std::vector<std::filesystem::path>& forbiddenRoots)
		{
			if (ContainsRootLeak(data, projectRoot))
			{
				Fail(logicalPath + ": project-root path leakage is forbidden in cooked payloads");
			}

			for (const std::filesystem::path& forbiddenRoot : forbiddenRoots)
			{
				if (ContainsRootLeak(data, forbiddenRoot))
				{
					Fail(logicalPath + ": forbidden-root path leakage is forbidden in cooked payloads: " +
						forbiddenRoot.generic_string());
				}
			}
		}

		struct EfkPackageEntry
		{
			std::string ArchivePath;
			std::string LogicalPath;
			std::string Type;
			std::string MetadataKey;
			std::vector<std::uint8_t> Data;
		};

		struct EfkPackage
		{
			std::map<std::string, EfkPackageEntry> Entries;
			Json Metadata;
			std::string MetadataPath;
			bool bHasMetadata = false;
			bool bMetadataChanged = false;
		};

		std::string NormalizeEfkLookupPath(std::string path)
		{
			std::replace(path.begin(), path.end(), '\\', '/');
			if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
				path.erase(0, 2);

			std::vector<std::string> segments;
			std::istringstream stream(path);
			std::string segment;
			while (std::getline(stream, segment, '/'))
			{
				if (segment.empty() || segment == ".")
					continue;
				if (segment == "..")
				{
					if (!segments.empty())
						segments.pop_back();
					continue;
				}
				segments.push_back(ToLowerAscii(std::move(segment)));
			}

			std::string result;
			for (const std::string& current : segments)
			{
				if (!result.empty())
					result.push_back('/');
				result += current;
			}
			return result;
		}

		std::string GetEfkEntryExtension(const EfkPackageEntry& entry)
		{
			return ToLowerAscii(std::filesystem::path(entry.LogicalPath).extension().string());
		}

		EfkPackageEntry* FindEfkPackageEntry(EfkPackage& package, const std::string& archivePath)
		{
			const std::string foldedPath = FoldPath(archivePath);
			for (auto& [path, entry] : package.Entries)
			{
				if (FoldPath(path) == foldedPath)
					return &entry;
			}
			return nullptr;
		}

		EfkPackage ReadEfkPackage(
			const std::vector<std::uint8_t>& data,
			const std::string& ownerPath)
		{
			zip_t* archive = zip_stream_open(
				reinterpret_cast<const char*>(data.data()), data.size(), 0, 'r');
			if (archive == nullptr)
				Fail(ownerPath + ": cannot open Effekseer package ZIP");

			EfkPackage package;
			std::set<std::string> foldedArchivePaths;
			try
			{
				const ssize_t entryCount = zip_entries_total(archive);
				if (entryCount < 0 || static_cast<std::size_t>(entryCount) > MaxFileCount)
					Fail(ownerPath + ": Effekseer package has an invalid entry count");

				for (std::size_t index = 0; index < static_cast<std::size_t>(entryCount); ++index)
				{
					if (zip_entry_openbyindex(archive, index) != 0)
						Fail(ownerPath + ": cannot open Effekseer package entry");

					if (zip_entry_isdir(archive) != 0)
					{
						if (zip_entry_close(archive) != 0)
							Fail(ownerPath + ": cannot close Effekseer package directory entry");
						continue;
					}

					const char* rawEntryName = zip_entry_name(archive);
					if (rawEntryName == nullptr)
						Fail(ownerPath + ": Effekseer package entry has no name");
					const std::string archivePath = NormalizeLogicalPath(
						rawEntryName, ownerPath + " Effekseer package entry");
					if (!foldedArchivePaths.insert(FoldPath(archivePath)).second)
						Fail(ownerPath + ": case-insensitive Effekseer package entry collision: " + archivePath);

					const auto rawEntrySize = zip_entry_size(archive);
					if (rawEntrySize < 0 || static_cast<std::uint64_t>(rawEntrySize) > MaxEntrySize)
						Fail(ownerPath + ": Effekseer package entry is too large: " + archivePath);
					const std::size_t entrySize = static_cast<std::size_t>(rawEntrySize);
					std::vector<std::uint8_t> entryData(entrySize);
					const ssize_t readSize = entrySize > 0
						? zip_entry_noallocread(archive, entryData.data(), entrySize)
						: 0;
					if (zip_entry_close(archive) != 0 || readSize < 0 || static_cast<std::size_t>(readSize) != entrySize)
						Fail(ownerPath + ": cannot read Effekseer package entry: " + archivePath);

					package.Entries.emplace(archivePath, EfkPackageEntry{
						.ArchivePath = archivePath,
						.LogicalPath = archivePath,
						.Data = std::move(entryData)
					});
				}
				zip_stream_close(archive);
				archive = nullptr;
			}
			catch (...)
			{
				if (archive != nullptr)
					zip_stream_close(archive);
				throw;
			}

			for (auto& [archivePath, entry] : package.Entries)
			{
				if (FoldPath(archivePath) != "metafile.json")
					continue;
				package.MetadataPath = archivePath;
				std::string metadataText(entry.Data.begin(), entry.Data.end());
				if (metadataText.size() >= 3 &&
					static_cast<unsigned char>(metadataText[0]) == 0xef &&
					static_cast<unsigned char>(metadataText[1]) == 0xbb &&
					static_cast<unsigned char>(metadataText[2]) == 0xbf)
				{
					metadataText.erase(0, 3);
				}
				try
				{
					package.Metadata = Json::parse(metadataText);
				}
				catch (const std::exception& exception)
				{
					Fail(ownerPath + ": invalid Effekseer metafile.json: " + exception.what());
				}
				package.bHasMetadata = true;
				break;
			}

			if (package.bHasMetadata && package.Metadata.contains("files"))
			{
				if (!package.Metadata["files"].is_object())
					Fail(ownerPath + ": Effekseer metafile.json files field must be an object");

				for (auto iterator = package.Metadata["files"].begin();
					iterator != package.Metadata["files"].end(); ++iterator)
				{
					if (!iterator.value().is_object() ||
						!iterator.value().contains("relative_path") ||
						!iterator.value()["relative_path"].is_string())
					{
						continue;
					}

					const std::string metadataKey = NormalizeLogicalPath(
						iterator.key(), ownerPath + " Effekseer metadata key");
					EfkPackageEntry* entry = FindEfkPackageEntry(package, metadataKey);
					if (entry == nullptr)
						continue;
					entry->LogicalPath = NormalizeLogicalPath(
						iterator.value()["relative_path"].get<std::string>(),
						ownerPath + " Effekseer metadata relative_path");
					entry->MetadataKey = iterator.key();
					if (iterator.value().contains("type") && iterator.value()["type"].is_string())
						entry->Type = ToLowerAscii(iterator.value()["type"].get<std::string>());
				}
			}

			std::map<std::string, std::string> logicalPaths;
			for (const auto& [archivePath, entry] : package.Entries)
			{
				if (FoldPath(archivePath) == "metafile.json")
					continue;
				const std::string foldedLogicalPath = FoldPath(entry.LogicalPath);
				const auto [found, bInserted] = logicalPaths.emplace(foldedLogicalPath, archivePath);
				if (!bInserted)
				{
					Fail(ownerPath + ": case-insensitive Effekseer logical path collision: " +
						found->second + " and " + archivePath);
				}
			}

			return package;
		}

		std::size_t RemoveEfkPackageEntries(
			EfkPackage& package,
			const std::vector<std::string>& archivePaths,
			std::uint64_t* removedBytes = nullptr)
		{
			std::vector<std::string> removedMetadataKeys;
			std::set<std::string> removedMetadataKeyFolds;
			for (const std::string& archivePath : archivePaths)
			{
				const auto entry = package.Entries.find(archivePath);
				if (entry == package.Entries.end())
					continue;
				if (removedBytes != nullptr)
					*removedBytes += static_cast<std::uint64_t>(entry->second.Data.size());
				if (!entry->second.MetadataKey.empty())
				{
					removedMetadataKeys.push_back(entry->second.MetadataKey);
					removedMetadataKeyFolds.insert(FoldPath(entry->second.MetadataKey));
				}
				package.Entries.erase(entry);
			}

			if (!removedMetadataKeys.empty() && package.bHasMetadata &&
				package.Metadata.contains("files") && package.Metadata["files"].is_object())
			{
				Json& files = package.Metadata["files"];
				for (const std::string& key : removedMetadataKeys)
					files.erase(key);

				for (auto& [key, file] : files.items())
				{
					(void)key;
					if (!file.is_object() || !file.contains("dependencies") || !file["dependencies"].is_array())
						continue;
					Json retainedDependencies = Json::array();
					for (const Json& dependency : file["dependencies"])
					{
						if (!dependency.is_string() ||
							!removedMetadataKeyFolds.contains(FoldPath(dependency.get<std::string>())))
						{
							retainedDependencies.push_back(dependency);
						}
					}
					file["dependencies"] = std::move(retainedDependencies);
				}
				package.bMetadataChanged = true;
			}

			return archivePaths.size();
		}

		std::size_t RemoveEfkEditorSources(
			EfkPackage& package,
			std::uint64_t& removedBytes)
		{
			std::vector<std::string> editorSourcePaths;
			for (const auto& [archivePath, entry] : package.Entries)
			{
				if (GetEfkEntryExtension(entry) == ".fbx")
					editorSourcePaths.push_back(archivePath);
			}
			return RemoveEfkPackageEntries(package, editorSourcePaths, &removedBytes);
		}

		std::uint32_t ReadLittleEndian32(const std::vector<std::uint8_t>& data, std::size_t offset)
		{
			return static_cast<std::uint32_t>(data[offset]) |
				(static_cast<std::uint32_t>(data[offset + 1]) << 8u) |
				(static_cast<std::uint32_t>(data[offset + 2]) << 16u) |
				(static_cast<std::uint32_t>(data[offset + 3]) << 24u);
		}

		std::uint16_t ReadLittleEndian16(const std::vector<std::uint8_t>& data, std::size_t offset)
		{
			return static_cast<std::uint16_t>(data[offset]) |
				static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
		}

		void WriteLittleEndian16(std::vector<std::uint8_t>& data, std::size_t offset, std::uint16_t value)
		{
			data[offset] = static_cast<std::uint8_t>(value & 0xffu);
			data[offset + 1] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
		}

		bool IsWindowsDrivePath(const std::vector<std::uint16_t>& path)
		{
			return path.size() >= 3 &&
				((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) &&
				path[1] == ':' && (path[2] == '/' || path[2] == '\\');
		}

		std::string RequireAsciiEfkPath(
			const std::vector<std::uint16_t>& path,
			const std::string& ownerPath)
		{
			std::string result;
			result.reserve(path.size());
			for (const std::uint16_t character : path)
			{
				if (character < 0x20u || character > 0x7eu)
					Fail(ownerPath + ": absolute Effekseer path cannot be safely rewritten as ASCII");
				result.push_back(static_cast<char>(character));
			}
			return result;
		}

		const EfkPackageEntry& FindEfkAbsolutePathTarget(
			const EfkPackage& package,
			const std::string& absolutePath,
			const std::string& ownerPath)
		{
			const std::string normalizedAbsolutePath = NormalizeEfkLookupPath(absolutePath);
			const EfkPackageEntry* matchedEntry = nullptr;
			std::size_t matchedLength = 0;
			bool bAmbiguous = false;

			for (const auto& [archivePath, entry] : package.Entries)
			{
				if (FoldPath(archivePath) == "metafile.json" || GetEfkEntryExtension(entry) == ".fbx")
					continue;
				const std::string candidate = NormalizeEfkLookupPath(entry.LogicalPath);
				if (candidate.empty() ||
					(normalizedAbsolutePath != candidate && !normalizedAbsolutePath.ends_with("/" + candidate)))
				{
					continue;
				}

				if (candidate.size() > matchedLength)
				{
					matchedEntry = &entry;
					matchedLength = candidate.size();
					bAmbiguous = false;
				}
				else if (candidate.size() == matchedLength)
				{
					bAmbiguous = true;
				}
			}

			if (matchedEntry == nullptr || bAmbiguous)
			{
				Fail(ownerPath + ": absolute Effekseer dependency has no unique package suffix match: " +
					absolutePath);
			}
			return *matchedEntry;
		}

		std::string BuildEqualLengthEfkRelativePath(
			const std::string& relativePath,
			std::size_t requiredLength,
			const std::string& ownerPath)
		{
			if (!std::all_of(relativePath.begin(), relativePath.end(), [](unsigned char character)
				{
					return character >= 0x20u && character <= 0x7eu;
				}))
			{
				Fail(ownerPath + ": matched Effekseer package path is not ASCII and cannot be rewritten in place");
			}

			std::string paddedPath = relativePath;
			std::replace(paddedPath.begin(), paddedPath.end(), '\\', '/');
			if (paddedPath.size() > requiredLength)
				Fail(ownerPath + ": matched Effekseer package path is longer than its absolute source path");

			std::size_t padding = requiredLength - paddedPath.size();
			std::string prefix;
			if ((padding & 1u) != 0u)
			{
				if (padding < 3u)
					Fail(ownerPath + ": cannot safely pad an odd-length Effekseer relative path");
				prefix = ".//";
				padding -= 3u;
			}
			for (std::size_t index = 0; index < padding / 2u; ++index)
				prefix += "./";
			paddedPath.insert(0, prefix);
			if (paddedPath.size() != requiredLength ||
				NormalizeEfkLookupPath(paddedPath) != NormalizeEfkLookupPath(relativePath))
			{
				Fail(ownerPath + ": cannot construct an equal-length safe Effekseer relative path");
			}
			return paddedPath;
		}

		std::size_t RewriteEfkAbsolutePaths(
			EfkPackage& package,
			const std::string& ownerPath)
		{
			std::size_t rewrittenPathCount = 0;
			for (auto& [archivePath, entry] : package.Entries)
			{
				const bool bEffectEntry = GetEfkEntryExtension(entry) == ".efkefc" || entry.Type == "effect";
				if (!bEffectEntry)
					continue;

				std::size_t offset = 0;
				while (offset + 10u <= entry.Data.size())
				{
					const std::uint32_t unitCount = ReadLittleEndian32(entry.Data, offset);
					const std::size_t stringOffset = offset + 4u;
					if (unitCount < 3u || unitCount > 4096u ||
						unitCount > (entry.Data.size() - stringOffset) / 2u)
					{
						++offset;
						continue;
					}

					std::vector<std::uint16_t> utf16Path(unitCount);
					for (std::size_t index = 0; index < unitCount; ++index)
						utf16Path[index] = ReadLittleEndian16(entry.Data, stringOffset + index * 2u);
					const bool bNullTerminated = !utf16Path.empty() && utf16Path.back() == 0;
					if (bNullTerminated)
						utf16Path.pop_back();
					if (!IsWindowsDrivePath(utf16Path))
					{
						++offset;
						continue;
					}

					const std::string absolutePath = RequireAsciiEfkPath(utf16Path, ownerPath);
					const EfkPackageEntry& target = FindEfkAbsolutePathTarget(package, absolutePath, ownerPath);
					const std::string replacement = BuildEqualLengthEfkRelativePath(
						target.LogicalPath, utf16Path.size(), ownerPath);
					for (std::size_t index = 0; index < replacement.size(); ++index)
						WriteLittleEndian16(entry.Data, stringOffset + index * 2u, replacement[index]);
					++rewrittenPathCount;
					offset = stringOffset + static_cast<std::size_t>(unitCount) * 2u;
				}
			}
			return rewrittenPathCount;
		}

		std::vector<std::uint8_t> CopyEfkShaderData(
			Effekseer::CompiledMaterialBinary& binary,
			Effekseer::MaterialShaderType shaderType,
			bool bVertexShader,
			const char* platformName,
			const std::string& ownerPath)
		{
			const int32_t size = bVertexShader
				? binary.GetVertexShaderSize(shaderType)
				: binary.GetPixelShaderSize(shaderType);
			const std::uint8_t* data = bVertexShader
				? binary.GetVertexShaderData(shaderType)
				: binary.GetPixelShaderData(shaderType);
			if (size < 0 || (size > 0 && data == nullptr))
				Fail(ownerPath + ": Effekseer " + platformName + " material compiler returned invalid shader data");
			return size > 0 ? std::vector<std::uint8_t>(data, data + size) : std::vector<std::uint8_t>{};
		}

		void AddEfkCompiledMaterialPlatform(
			Effekseer::CompiledMaterial& compiled,
			Effekseer::CompiledMaterialBinary& binary,
			Effekseer::CompiledMaterialPlatformType platform,
			const char* platformName,
			const std::string& ownerPath)
		{
			const auto standardVs = CopyEfkShaderData(
				binary, Effekseer::MaterialShaderType::Standard, true, platformName, ownerPath);
			const auto standardPs = CopyEfkShaderData(
				binary, Effekseer::MaterialShaderType::Standard, false, platformName, ownerPath);
			const auto modelVs = CopyEfkShaderData(
				binary, Effekseer::MaterialShaderType::Model, true, platformName, ownerPath);
			const auto modelPs = CopyEfkShaderData(
				binary, Effekseer::MaterialShaderType::Model, false, platformName, ownerPath);
			const auto refractionVs = CopyEfkShaderData(
				binary, Effekseer::MaterialShaderType::Refraction, true, platformName, ownerPath);
			const auto refractionPs = CopyEfkShaderData(
				binary, Effekseer::MaterialShaderType::Refraction, false, platformName, ownerPath);
			const auto modelRefractionVs = CopyEfkShaderData(
				binary, Effekseer::MaterialShaderType::RefractionModel, true, platformName, ownerPath);
			const auto modelRefractionPs = CopyEfkShaderData(
				binary, Effekseer::MaterialShaderType::RefractionModel, false, platformName, ownerPath);

			compiled.UpdateData(
				standardVs,
				standardPs,
				modelVs,
				modelPs,
				refractionVs,
				refractionPs,
				modelRefractionVs,
				modelRefractionPs,
				platform);
		}

		std::vector<std::uint8_t> CompileEfkMaterial(
			const EfkPackageEntry& source,
			const std::string& ownerPath)
		{
			if (source.Data.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
				Fail(ownerPath + ": Effekseer material source is too large: " + source.LogicalPath);

			Effekseer::MaterialFile material;
			if (!material.Load(source.Data.data(), static_cast<int32_t>(source.Data.size())))
				Fail(ownerPath + ": cannot parse Effekseer material: " + source.LogicalPath);

			Effekseer::MaterialCompilerDX12 compiler;
			auto binary = Effekseer::CreateUniqueReference(compiler.Compile(&material));
			if (!binary)
				Fail(ownerPath + ": cannot compile Effekseer DX12 material: " + source.LogicalPath);

			Effekseer::CompiledMaterial compiled;
			AddEfkCompiledMaterialPlatform(
				compiled,
				*binary,
				Effekseer::CompiledMaterialPlatformType::DirectX12,
				"DX12",
				ownerPath);

			Effekseer::MaterialCompilerVulkan vulkanCompiler;
			auto vulkanBinary = Effekseer::CreateUniqueReference(vulkanCompiler.Compile(&material));
			if (!vulkanBinary)
				Fail(ownerPath + ": cannot compile Effekseer Vulkan material: " + source.LogicalPath);
			AddEfkCompiledMaterialPlatform(
				compiled,
				*vulkanBinary,
				Effekseer::CompiledMaterialPlatformType::Vulkan,
				"Vulkan",
				ownerPath);

			std::vector<std::uint8_t> originalData = source.Data;
			std::vector<std::uint8_t> compiledData;
			compiled.Save(compiledData, material.GetGUID(), originalData);

			Effekseer::CompiledMaterial verification;
			if (compiledData.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()) ||
				!verification.Load(compiledData.data(), static_cast<int32_t>(compiledData.size())) ||
				!verification.GetHasValue(Effekseer::CompiledMaterialPlatformType::DirectX12) ||
				!verification.GetHasValue(Effekseer::CompiledMaterialPlatformType::Vulkan))
			{
				Fail(ownerPath + ": generated Effekseer DX12/Vulkan material failed verification: " + source.LogicalPath);
			}
			return compiledData;
		}

		std::size_t CompileEfkMaterials(EfkPackage& package, const std::string& ownerPath)
		{
			std::vector<std::string> materialPaths;
			for (const auto& [archivePath, entry] : package.Entries)
			{
				if (GetEfkEntryExtension(entry) == ".efkmat")
					materialPaths.push_back(archivePath);
			}

			for (const std::string& materialPath : materialPaths)
			{
				const EfkPackageEntry& source = package.Entries.at(materialPath);
				const std::string compiledLogicalPath = NormalizeLogicalPath(
					source.LogicalPath + "d", ownerPath + " compiled Effekseer material");
				std::vector<std::uint8_t> compiledData = CompileEfkMaterial(source, ownerPath);

				EfkPackageEntry* existingCompiled = nullptr;
				for (auto& [path, entry] : package.Entries)
				{
					(void)path;
					if (FoldPath(entry.LogicalPath) == FoldPath(compiledLogicalPath))
					{
						if (existingCompiled != nullptr)
							Fail(ownerPath + ": duplicate Effekseer compiled material path: " + compiledLogicalPath);
						existingCompiled = &entry;
					}
				}

				if (existingCompiled != nullptr)
				{
					existingCompiled->Data = std::move(compiledData);
					continue;
				}

				if (FindEfkPackageEntry(package, compiledLogicalPath) != nullptr)
					Fail(ownerPath + ": compiled Effekseer material archive collision: " + compiledLogicalPath);
				package.Entries.emplace(compiledLogicalPath, EfkPackageEntry{
					.ArchivePath = compiledLogicalPath,
					.LogicalPath = compiledLogicalPath,
					.Type = "material",
					.Data = std::move(compiledData)
				});
			}
			RemoveEfkPackageEntries(package, materialPaths);
			return materialPaths.size();
		}

		std::vector<std::uint8_t> WriteEfkPackage(
			EfkPackage& package,
			const std::string& ownerPath,
			int compressionLevel)
		{
			if (package.bMetadataChanged)
			{
				EfkPackageEntry* metadataEntry = FindEfkPackageEntry(package, package.MetadataPath);
				if (metadataEntry == nullptr)
					Fail(ownerPath + ": Effekseer metadata entry disappeared while cooking");
				const std::string metadataText = package.Metadata.dump();
				metadataEntry->Data.assign(metadataText.begin(), metadataText.end());
			}

			zip_t* archive = zip_stream_open(nullptr, 0, compressionLevel, 'w');
			if (archive == nullptr)
				Fail(ownerPath + ": cannot create cooked Effekseer package ZIP");

			void* outputBuffer = nullptr;
			try
			{
				for (const auto& [archivePath, entry] : package.Entries)
				{
					if (zip_entry_open(archive, archivePath.c_str()) != 0)
						Fail(ownerPath + ": cannot create cooked Effekseer package entry: " + archivePath);
					if (!entry.Data.empty() && zip_entry_write(archive, entry.Data.data(), entry.Data.size()) != 0)
						Fail(ownerPath + ": cannot write cooked Effekseer package entry: " + archivePath);
					if (zip_entry_close(archive) != 0)
						Fail(ownerPath + ": cannot close cooked Effekseer package entry: " + archivePath);
				}

				size_t outputSize = 0;
				const ssize_t copiedSize = zip_stream_copy(archive, &outputBuffer, &outputSize);
				if (copiedSize < 0 || static_cast<std::size_t>(copiedSize) != outputSize ||
					(outputSize > 0 && outputBuffer == nullptr))
				{
					Fail(ownerPath + ": cannot finalize cooked Effekseer package ZIP");
				}

				const auto* outputBytes = static_cast<const std::uint8_t*>(outputBuffer);
				std::vector<std::uint8_t> result;
				if (outputSize > 0)
					result.assign(outputBytes, outputBytes + outputSize);
				std::free(outputBuffer);
				outputBuffer = nullptr;
				zip_stream_close(archive);
				archive = nullptr;
				return result;
			}
			catch (...)
			{
				std::free(outputBuffer);
				if (archive != nullptr)
					zip_stream_close(archive);
				throw;
			}
		}

		std::vector<std::uint8_t> CookEfkPackage(
			const std::vector<std::uint8_t>& sourceData,
			const std::string& ownerPath,
			const std::filesystem::path& projectRoot,
			const std::vector<std::filesystem::path>& forbiddenRoots,
			int compressionLevel)
		{
			EfkPackage package = ReadEfkPackage(sourceData, ownerPath);
			std::uint64_t removedBytes = 0;
			const std::size_t removedSourceCount = RemoveEfkEditorSources(package, removedBytes);
			const std::size_t rewrittenPathCount = RewriteEfkAbsolutePaths(package, ownerPath);
			const std::size_t compiledMaterialCount = CompileEfkMaterials(package, ownerPath);

			for (const auto& [archivePath, entry] : package.Entries)
			{
				LintAbsoluteRootLeaks(
					entry.Data,
					ownerPath + "!/" + archivePath,
					projectRoot,
					forbiddenRoots);
			}

			std::vector<std::uint8_t> cookedData = WriteEfkPackage(package, ownerPath, compressionLevel);
			if (cookedData.size() > MaxEntrySize)
				Fail(ownerPath + ": cooked Effekseer package exceeds the packaged runtime entry limit");
			std::printf(
				"HCooker Effekseer: %s, removed %zu FBX (%llu bytes), rewrote %zu absolute paths, "
				"compiled %zu DX12 materials, %zu -> %zu bytes.\n",
				ownerPath.c_str(),
				removedSourceCount,
				static_cast<unsigned long long>(removedBytes),
				rewrittenPathCount,
				compiledMaterialCount,
				sourceData.size(),
				cookedData.size());
			return cookedData;
		}

		bool IsLogicalPathWithin(std::string_view root, std::string_view candidate)
		{
			const std::string foldedRoot = FoldPath(std::string(root));
			const std::string foldedCandidate = FoldPath(std::string(candidate));
			return foldedCandidate == foldedRoot ||
				foldedCandidate.starts_with(foldedRoot + "/");
		}

		std::filesystem::path PythonModulePath(std::string module)
		{
			std::replace(module.begin(), module.end(), '.', '/');
			return std::filesystem::path(module);
		}

		void AddIndexedPythonDependency(
			const ProjectIndex& index,
			DependencyMap& dependencies,
			const std::string& ownerPath,
			const std::filesystem::path& candidate)
		{
			const std::string normalized = NormalizeLogicalPath(
				candidate.lexically_normal().generic_string(),
				ownerPath + ": Python import");
			const auto found = index.Files.find(FoldPath(normalized));
			if (found == index.Files.end())
				return;

			AddDependency(
				dependencies,
				ownerPath,
				found->second.LogicalPath,
				DependencyKind::PackagingOnly,
				DependencySource::Heuristic);
		}

		bool AddPythonModuleDependencies(
			const std::filesystem::path& moduleBase,
			const std::filesystem::path& searchRoot,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyDiagnostics* diagnostics = nullptr)
		{
			const std::string normalizedBase = NormalizeLogicalPath(
				moduleBase.lexically_normal().generic_string(),
				ownerPath + ": Python import");
			const std::filesystem::path basePath(normalizedBase);
			if (!searchRoot.empty() && !IsLogicalPathWithin(
				searchRoot.generic_string(), normalizedBase))
			{
				Fail(ownerPath + ": Python import escapes its search root");
			}

			const std::array candidates = {
				std::filesystem::path(normalizedBase + ".py"),
				basePath / "__init__.py"
			};
			const IndexedFile* resolvedModule = nullptr;
			for (const std::filesystem::path& candidate : candidates)
			{
				const auto found = index.Files.find(FoldPath(candidate.generic_string()));
				if (found != index.Files.end())
				{
					resolvedModule = &found->second;
					break;
				}
			}
			if (resolvedModule == nullptr)
				return false;

			AddDependency(
				dependencies,
				ownerPath,
				resolvedModule->LogicalPath,
				DependencyKind::PackagingOnly,
				DependencySource::Heuristic);

			for (std::filesystem::path package = basePath.parent_path(); !package.empty(); package = package.parent_path())
			{
				if (!searchRoot.empty() && FoldPath(package.generic_string()) ==
					FoldPath(searchRoot.generic_string()))
				{
					break;
				}
				AddIndexedPythonDependency(index, dependencies, ownerPath, package / "__init__.py");
			}
			return true;
		}

		bool AddAbsolutePythonModuleDependencies(
			const std::string& module,
			const std::filesystem::path& ownerDirectory,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyDiagnostics* diagnostics = nullptr)
		{
			if (module.empty())
				return false;
			const std::filesystem::path scriptsRoot("Scripts");
			const std::filesystem::path modulePath = PythonModulePath(module);
			const std::array searchBases = {
				std::pair{ modulePath, std::filesystem::path{} },
				std::pair{ scriptsRoot / modulePath, scriptsRoot },
				std::pair{ ownerDirectory / modulePath, std::filesystem::path{} }
			};
			for (const auto& [moduleBase, searchRoot] : searchBases)
			{
				if (AddPythonModuleDependencies(
					moduleBase, searchRoot, ownerPath, index, dependencies))
				{
					return true;
				}
			}
			return false;
		}

		std::filesystem::path ResolveRelativePythonBase(
			std::string_view leadingDots,
			const std::filesystem::path& ownerDirectory,
			const std::string& ownerPath)
		{
			const std::filesystem::path scriptsRoot("Scripts");
			if (leadingDots.empty())
				Fail(ownerPath + ": relative Python import has no leading dot");
			if (!IsLogicalPathWithin(scriptsRoot.generic_string(), ownerDirectory.generic_string()))
				Fail(ownerPath + ": relative Python import is outside the Scripts root");

			std::filesystem::path relativeBase = ownerDirectory;
			for (std::size_t level = 1; level < leadingDots.size(); ++level)
			{
				if (FoldPath(relativeBase.generic_string()) == FoldPath(scriptsRoot.generic_string()))
					Fail(ownerPath + ": relative Python import escapes the Scripts root");
				relativeBase = relativeBase.parent_path();
			}
			if (!IsLogicalPathWithin(scriptsRoot.generic_string(), relativeBase.generic_string()))
				Fail(ownerPath + ": relative Python import escapes the Scripts root");
			return relativeBase;
		}

		std::vector<std::string> CollectPythonImportStatements(const std::string& text)
		{
			static const std::regex importStartPattern(R"hecook(^(import|from)\s+)hecook");
			std::vector<std::string> statements;
			std::istringstream stream(text);
			std::string statement;
			std::string line;
			int parenthesisDepth = 0;
			while (std::getline(stream, line))
			{
				const std::size_t comment = line.find('#');
				if (comment != std::string::npos)
					line.erase(comment);
				line = Trim(std::move(line));
				if (statement.empty() && !std::regex_search(line, importStartPattern))
					continue;

				bool bBackslashContinuation = !line.empty() && line.back() == '\\';
				if (bBackslashContinuation)
				{
					line.pop_back();
					line = Trim(std::move(line));
				}
				if (!line.empty())
				{
					if (!statement.empty())
						statement.push_back(' ');
					statement += line;
					parenthesisDepth += static_cast<int>(std::count(line.begin(), line.end(), '('));
					parenthesisDepth -= static_cast<int>(std::count(line.begin(), line.end(), ')'));
				}

				if (!statement.empty() && parenthesisDepth <= 0 && !bBackslashContinuation)
				{
					statements.push_back(std::move(statement));
					statement.clear();
					parenthesisDepth = 0;
				}
			}
			return statements;
		}

		std::vector<std::string> ParsePythonImportItems(std::string items, bool bAllowDottedNames)
		{
			items = Trim(std::move(items));
			if (items.size() >= 2 && items.front() == '(' && items.back() == ')')
				items = items.substr(1, items.size() - 2);

			const std::regex itemPattern(bAllowDottedNames
				? R"hecook(^([A-Za-z_][A-Za-z0-9_.]*)(?:\s+as\s+[A-Za-z_][A-Za-z0-9_]*)?$)hecook"
				: R"hecook(^([A-Za-z_][A-Za-z0-9_]*|\*)(?:\s+as\s+[A-Za-z_][A-Za-z0-9_]*)?$)hecook");
			std::vector<std::string> result;
			std::istringstream stream(items);
			std::string item;
			while (std::getline(stream, item, ','))
			{
				item = Trim(std::move(item));
				std::smatch match;
				if (std::regex_match(item, match, itemPattern))
					result.push_back(match[1].str());
			}
			return result;
		}

		void CollectPythonImports(
			const std::string& text,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies)
		{
			static const std::regex importPattern(R"hecook(^import\s+(.+)$)hecook");
			static const std::regex fromPattern(
				R"hecook(^from\s+(\.*)([A-Za-z_][A-Za-z0-9_.]*)?\s+import\s+(.+)$)hecook");
			const std::filesystem::path scriptsRoot("Scripts");
			const std::filesystem::path ownerDirectory = std::filesystem::path(ownerPath).parent_path();
			for (const std::string& statement : CollectPythonImportStatements(text))
			{
				std::smatch match;
				if (std::regex_match(statement, match, importPattern))
				{
					for (const std::string& module : ParsePythonImportItems(match[1].str(), true))
					{
						(void)AddAbsolutePythonModuleDependencies(
							module, ownerDirectory, ownerPath, index, dependencies);
					}
					continue;
				}
				if (!std::regex_match(statement, match, fromPattern))
					continue;

				const std::string leadingDots = match[1].str();
				const std::string module = match[2].matched ? match[2].str() : std::string{};
				const std::vector<std::string> importedItems =
					ParsePythonImportItems(match[3].str(), false);
				if (leadingDots.empty())
				{
					if (module.empty())
						continue;
					(void)AddAbsolutePythonModuleDependencies(
						module, ownerDirectory, ownerPath, index, dependencies);
					for (const std::string& importedItem : importedItems)
					{
						if (importedItem != "*")
							(void)AddAbsolutePythonModuleDependencies(
								module + "." + importedItem,
								ownerDirectory,
								ownerPath,
								index,
								dependencies);
					}
					continue;
				}

				const std::filesystem::path relativeBase = ResolveRelativePythonBase(
					leadingDots, ownerDirectory, ownerPath);
				const std::filesystem::path moduleBase = module.empty()
					? relativeBase
					: relativeBase / PythonModulePath(module);
				(void)AddPythonModuleDependencies(
					moduleBase,
					scriptsRoot,
					ownerPath,
					index,
					dependencies);
				for (const std::string& importedItem : importedItems)
				{
					if (importedItem != "*")
						(void)AddPythonModuleDependencies(
							moduleBase / PythonModulePath(importedItem),
							scriptsRoot,
							ownerPath,
							index,
							dependencies);
				}
			}
		}

		DependencyKind ParseDynamicDependencyKind(
			const Json& asset,
			const std::string& ownerPath,
			std::size_t assetIndex)
		{
			if (!asset.contains("Kind"))
				return DependencyKind::HardRuntime;
			if (!asset["Kind"].is_string())
				Fail(ownerPath + ": /Assets/" + std::to_string(assetIndex) + "/Kind must be a string");
			const std::string kind = asset["Kind"].get<std::string>();
			if (kind == "HardRuntime") return DependencyKind::HardRuntime;
			if (kind == "SoftRuntime") return DependencyKind::SoftRuntime;
			if (kind == "PackagingOnly") return DependencyKind::PackagingOnly;
			Fail(ownerPath + ": /Assets/" + std::to_string(assetIndex) +
				"/Kind must be HardRuntime, SoftRuntime, or PackagingOnly");
		}

		void CollectDynamicAssetSetDependencies(
			const Json& json,
			const std::string& ownerPath,
			const std::string& assetSetName,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyDiagnostics* diagnostics)
		{
			if (!json.is_object())
				Fail(ownerPath + ": runtime asset set root must be an object");
			for (const auto& [key, value] : json.items())
			{
				(void)value;
				if (key != "Schema" && key != "SchemaVersion" && key != "Assets")
					Fail(ownerPath + ": runtime asset set contains an unknown field: " + key);
			}
			if (!json.contains("Schema") || !json["Schema"].is_string() ||
				json["Schema"].get<std::string>() != "hengine.runtime-asset-set" ||
				!json.contains("SchemaVersion") || !json["SchemaVersion"].is_number_unsigned() ||
				json["SchemaVersion"].get<std::uint64_t>() != 1 ||
				!json.contains("Assets") || !json["Assets"].is_array())
			{
				Fail(ownerPath + ": runtime asset set must use Schema hengine.runtime-asset-set, SchemaVersion 1, and an Assets array");
			}

			std::set<std::string> keys;
			std::pair<std::string, std::string> previousAsset;
			for (std::size_t assetIndex = 0; assetIndex < json["Assets"].size(); ++assetIndex)
			{
				const Json& asset = json["Assets"][assetIndex];
				const std::string location = "/Assets/" + std::to_string(assetIndex);
				if (!asset.is_object())
					Fail(ownerPath + ": " + location + " must be an object");
				for (const auto& [key, value] : asset.items())
				{
					(void)value;
					if (key != "Key" && key != "Path" && key != "Kind" && key != "Reason")
						Fail(ownerPath + ": " + location + " contains an unknown field: " + key);
				}
				if (!asset.contains("Key") || !asset["Key"].is_string() ||
					!asset.contains("Path") || !asset["Path"].is_string())
				{
					Fail(ownerPath + ": " + location + " requires string Key and Path fields");
				}
				const std::string key = Trim(asset["Key"].get<std::string>());
				const std::string path = Trim(asset["Path"].get<std::string>());
				if (key.empty() || key.size() > 256 || key.front() == '/' || key.back() == '/' ||
					std::any_of(key.begin(), key.end(), [](unsigned char character)
					{
						return !((character >= 'a' && character <= 'z') ||
							(character >= '0' && character <= '9') ||
							character == '/' || character == '_' || character == '-');
					}))
				{
					Fail(ownerPath + ": " + location +
						"/Key must use lowercase ASCII a-z, 0-9, '/', '_', or '-' without leading/trailing '/'");
				}
				if (!keys.insert(key).second)
					Fail(ownerPath + ": duplicate runtime asset Key: " + key);
				const std::pair<std::string, std::string> currentAsset{ key, path };
				if (assetIndex != 0 && !(previousAsset < currentAsset))
					Fail(ownerPath + ": Assets must be in canonical Key, Path order");
				previousAsset = currentAsset;
				if (asset.contains("Reason") && !asset["Reason"].is_string())
					Fail(ownerPath + ": " + location + "/Reason must be a string");
				const std::string reason = asset.contains("Reason")
					? asset["Reason"].get<std::string>()
					: "explicit dynamic asset '" + key + "' from set '" + assetSetName + "'";
				StrictReferenceOptions options;
				options.bAllowEmpty = false;
				options.bAllowNone = false;
				ResolveAndAddStrictDependency(
					path, ownerPath, index, dependencies,
					ParseDynamicDependencyKind(asset, ownerPath, assetIndex),
					DependencySource::ExplicitDynamic,
					"RuntimeAssetSet", "hengine.runtime-asset-set",
					location + "/Path", reason, options, diagnostics);
			}
		}

		void CollectGameConfigDependencies(
			const Json& json,
			const CookConfig& config,
			const std::string& ownerPath,
			const ProjectIndex& index,
			DependencyMap& dependencies,
			DependencyDiagnostics* diagnostics)
		{
			if (!json.contains("Game") || !json["Game"].is_object())
				return;
			const Json& game = json["Game"];
			AddStructuredField(
				game, "DefaultScene", ownerPath, index, dependencies, DependencyKind::HardRuntime,
				diagnostics, "GameConfig", "Game", "/Game/DefaultScene");
			AddStructuredField(
				game, "Icon", ownerPath, index, dependencies, DependencyKind::SoftRuntime,
				diagnostics, "GameConfig", "Game", "/Game/Icon");
			for (std::size_t setIndex = 0; setIndex < config.DynamicAssetSets.size(); ++setIndex)
			{
				const DynamicAssetSetConfig& assetSet = config.DynamicAssetSets[setIndex];
				ResolveAndAddStrictDependency(
					assetSet.Manifest, ownerPath, index, dependencies,
					DependencyKind::HardRuntime, DependencySource::Structured,
					"GameConfig", "Cook.DynamicAssetSets",
					"/Cook/DynamicAssetSets/" + std::to_string(setIndex) + "/Manifest",
					"runtime asset set manifest '" + assetSet.Name + "'", {}, diagnostics);
			}
		}

		std::uint64_t HashBytes(std::uint64_t hash, const std::uint8_t* data, std::size_t size)
		{
			for (std::size_t index = 0; index < size; ++index)
			{
				hash ^= data[index];
				hash *= FnvPrime;
			}
			return hash;
		}

		std::uint64_t HashBytes(const std::vector<std::uint8_t>& data)
		{
			return HashBytes(FnvOffsetBasis, data.data(), data.size());
		}

		std::uint64_t HashString(std::uint64_t hash, std::string_view value)
		{
			return HashBytes(hash, reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
		}

		std::uint64_t HashLittleEndian64(std::uint64_t hash, std::uint64_t value)
		{
			std::uint8_t bytes[8]{};
			for (std::size_t index = 0; index < 8; ++index)
				bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xffu);
			return HashBytes(hash, bytes, 8);
		}

		std::string Hex64(std::uint64_t value)
		{
			std::ostringstream stream;
			stream << std::hex << std::setfill('0') << std::setw(16) << value;
			return stream.str();
		}

		std::string ClassifyResourceType(const std::string& extension)
		{
			if (extension == ".he") return "Scene";
			if (extension == ".hprefab") return "Prefab";
			if (extension == ".hmesh") return "Mesh";
			if (extension == ".hmat") return "Material";
			if (extension == ".hskel") return "Skeleton";
			if (extension == ".hanim") return "AnimationClip";
			if (extension == ".hanimctrl") return "AnimationController";
			if (extension == ".hui") return "UiDocument";
			if (extension == ".hbt") return "BehaviorTree";
			if (extension == ".rml" || extension == ".rcss") return "UiMarkup";
			if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
				extension == ".bmp" || extension == ".tga" || extension == ".hdr" || extension == ".dds")
			{
				return "Texture";
			}
			if (extension == ".hlsl" || extension == ".hlsli") return "ShaderSource";
			if (extension == ".vs" || extension == ".ps" || extension == ".cs")
				return "ShaderBinary";
			if (extension == ".py") return "Script";
			if (extension == ".wav" || extension == ".mp3" || extension == ".ogg" || extension == ".flac")
				return "Audio";
			if (extension == ".ttf" || extension == ".otf") return "Font";
			if (extension == ".efkpkg") return "EffectPackage";
			if (extension == ".efkefc") return "Effect";
			if (extension == ".fbx" || extension == ".obj" || extension == ".gltf" || extension == ".glb")
				return "MeshSource";
			if (extension == ".json" || extension == ".ini") return "Config";
			if (extension == ".bin") return "Binary";
			return "Data";
		}

		bool TryMultiplyBytes(std::uint64_t left, std::uint64_t right, std::uint64_t& result)
		{
			if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
				return false;
			result = left * right;
			return true;
		}

		std::uint32_t ReadBigEndian32(const std::vector<std::uint8_t>& data, std::size_t offset)
		{
			return (static_cast<std::uint32_t>(data[offset]) << 24) |
				(static_cast<std::uint32_t>(data[offset + 1]) << 16) |
				(static_cast<std::uint32_t>(data[offset + 2]) << 8) |
				static_cast<std::uint32_t>(data[offset + 3]);
		}

		std::int64_t ReadLittleEndianSigned32(const std::vector<std::uint8_t>& data, std::size_t offset)
		{
			const std::uint32_t value = ReadLittleEndian32(data, offset);
			return value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
				? static_cast<std::int64_t>(value)
				: static_cast<std::int64_t>(value) - (1ll << 32);
		}

		std::uint64_t EstimateTextureGpuBytes(
			const std::string& extension,
			const std::vector<std::uint8_t>& data)
		{
			std::uint64_t width = 0;
			std::uint64_t height = 0;
			if (extension == ".png" && data.size() >= 24)
			{
				static constexpr std::uint8_t PngSignature[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
				if (std::equal(std::begin(PngSignature), std::end(PngSignature), data.begin()))
				{
					width = ReadBigEndian32(data, 16);
					height = ReadBigEndian32(data, 20);
				}
			}
			else if (extension == ".bmp" && data.size() >= 26 && data[0] == 'B' && data[1] == 'M')
			{
				const std::int64_t signedWidth = ReadLittleEndianSigned32(data, 18);
				const std::int64_t signedHeight = ReadLittleEndianSigned32(data, 22);
				if (signedWidth <= 0 || signedHeight == 0)
					return 0;
				width = static_cast<std::uint64_t>(signedWidth);
				height = static_cast<std::uint64_t>(
					signedHeight < 0 ? -signedHeight : signedHeight);
			}
			if (width == 0 || height == 0)
				return 0;

			std::uint64_t pixelCount = 0;
			std::uint64_t byteCount = 0;
			if (!TryMultiplyBytes(width, height, pixelCount) || !TryMultiplyBytes(pixelCount, 4, byteCount))
				return RuntimeResourceCatalogLimits::MaxResourceEstimate + 1;
			return byteCount;
		}

		std::uint64_t EstimateMeshGpuBytes(const Json& json)
		{
			const Json* entries = nullptr;
			if (json.contains("bufferViewVec") && json["bufferViewVec"].is_array())
				entries = &json["bufferViewVec"];
			else if (json.contains("bufferVec") && json["bufferVec"].is_array())
				entries = &json["bufferVec"];
			if (!entries)
				return 0;

			std::uint64_t result = 0;
			for (const Json& entry : *entries)
			{
				if (!entry.is_object() || !entry.contains("byteLength") || !entry["byteLength"].is_number_unsigned())
					continue;
				const std::uint64_t bytes = entry["byteLength"].get<std::uint64_t>();
				if (bytes > std::numeric_limits<std::uint64_t>::max() - result)
					return RuntimeResourceCatalogLimits::MaxResourceEstimate + 1;
				result += bytes;
			}
			return result;
		}

		CookedFile CookFile(
			const IndexedFile& source,
			const CookConfig& config,
			const ProjectIndex& index,
			const std::string& configLogicalPath,
			const std::filesystem::path& projectRoot,
			const std::vector<std::filesystem::path>& forbiddenRoots,
			std::string witness = {})
		{
			CookedFile result;
			result.LogicalPath = source.LogicalPath;
			result.SourceSize = source.Size;
			if (source.Size > MaxEntrySize)
				Fail(source.LogicalPath + ": source file exceeds the packaged runtime entry limit");
			const std::vector<std::uint8_t> sourceData = ReadBinaryFile(source);
			result.Data = sourceData;

			const std::string extension = ToLowerAscii(source.PhysicalPath.extension().string());
			result.Type = ClassifyResourceType(extension);
			const bool bCookedJson = config.CookedJsonExtensions.contains(extension);

			if (extension == ".efkpkg")
				result.Data = CookEfkPackage(
					sourceData,
					source.LogicalPath,
					projectRoot,
					forbiddenRoots,
					config.CompressionLevel);

			const bool bJsonFile = extension == ".json" || config.CookedJsonExtensions.contains(extension);
			DependencyMap dependencies;
			DependencyDiagnostics diagnostics;
			diagnostics.Witness = std::move(witness);
			std::optional<Json> resourceJson;
			if (bJsonFile)
			{
				const std::string text(sourceData.begin(), sourceData.end());
				LintTextPayload(text, source.LogicalPath, false);
				try
				{
					resourceJson = Json::parse(sourceData.begin(), sourceData.end());
				}
				catch (const std::exception& exception)
				{
					Fail("Invalid JSON resource '" + source.LogicalPath + "': " + exception.what());
				}

				CollectStructuredJsonDependencies(
					*resourceJson, extension, source.LogicalPath, index, dependencies, &diagnostics);
				if (source.LogicalPath == configLogicalPath)
				{
					CollectGameConfigDependencies(
						*resourceJson, config, source.LogicalPath, index, dependencies, &diagnostics);
				}
				const auto dynamicSet = config.DynamicAssetSetNamesByManifest.find(FoldPath(source.LogicalPath));
				const bool bDynamicAssetSet = dynamicSet != config.DynamicAssetSetNamesByManifest.end();
				if (bDynamicAssetSet)
				{
					CollectDynamicAssetSetDependencies(
						*resourceJson, source.LogicalPath, dynamicSet->second,
						index, dependencies, &diagnostics);
				}

				const bool bTypedMaterial = extension == ".hmat" &&
					resourceJson->value("SchemaVersion", 1) >= 2;
				const bool bMaterialManifest = extension == ".json" &&
					FoldPath(source.LogicalPath).ends_with(".material.json");
				if (!bTypedMaterial && !bMaterialManifest && !bDynamicAssetSet)
				{
					std::vector<std::string> references;
					if (source.LogicalPath == configLogicalPath && resourceJson->contains("Game"))
						CollectJsonStrings((*resourceJson)["Game"], references);
					else
						CollectJsonStrings(*resourceJson, references);
					for (std::string& reference : references)
						ResolveAndAddDependency(
							std::move(reference), source.LogicalPath, index, dependencies,
							DependencyKind::PackagingOnly, DependencySource::Heuristic,
							&diagnostics, "JsonHeuristicScanner", {}, {}, "JSON string candidate");
				}

				if (bCookedJson)
				{
					const std::vector<std::uint8_t> messagePack = Json::to_msgpack(*resourceJson);
					result.Data.assign(Utils::CookedJsonMagic.begin(), Utils::CookedJsonMagic.end());
					result.Data.insert(result.Data.end(), messagePack.begin(), messagePack.end());
					result.bCookedJson = true;
				}
			}
			else
			{
				static const std::set<std::string> TextExtensions = {
					".py", ".rml", ".rcss", ".hlsl", ".hlsli", ".ini", ".txt", ".meta"
				};
				if (TextExtensions.contains(extension))
				{
					const std::string text(sourceData.begin(), sourceData.end());
					LintTextPayload(text, source.LogicalPath, extension == ".py");
					if (extension == ".hlsl" || extension == ".hlsli")
					{
						CollectShaderIncludeDependencies(
							text, source.LogicalPath, index, dependencies, &diagnostics);
					}
					else if (extension == ".rml")
					{
						CollectRmlDependencies(
							text, source.LogicalPath, index, dependencies, &diagnostics);
					}
					else if (extension == ".rcss")
					{
						CollectRcssDependencies(
							text, source.LogicalPath, index, dependencies, &diagnostics);
					}
					std::vector<std::string> references;
					CollectTextReferences(text, references, extension != ".py");
					for (std::string& reference : references)
						ResolveAndAddDependency(
							std::move(reference), source.LogicalPath, index, dependencies,
							DependencyKind::PackagingOnly, DependencySource::Heuristic,
							&diagnostics, "TextHeuristicScanner", {}, {}, "text reference candidate");
					if (extension == ".py")
						CollectPythonImports(text, source.LogicalPath, index, dependencies);
				}
			}

			// Scan the final encoded payload. In particular, JSON escape sequences such as
			// "\\u0043" are decoded by MessagePack conversion and must not bypass the
			// absolute-root leak check performed on shipping bytes.
			LintAbsoluteRootLeaks(result.Data, source.LogicalPath, projectRoot, forbiddenRoots);

			if (result.Data.size() > MaxEntrySize)
				Fail(source.LogicalPath + ": cooked file exceeds the packaged runtime entry limit");
			for (auto& dependency : dependencies)
				result.Dependencies.push_back(std::move(dependency.second));
			result.VirtualDependencies = std::move(diagnostics.VirtualDependencies);
			result.UnresolvedCandidates = std::move(diagnostics.UnresolvedCandidates);
			// Payload encoding version only; resource schema versions remain inside each payload.
			result.CookFormatVersion = result.bCookedJson || extension == ".efkpkg" ? 1u : 0u;
			result.CpuBytes = static_cast<std::uint64_t>(result.Data.size());
			if (extension == ".hmesh" && resourceJson)
				result.GpuBytes = EstimateMeshGpuBytes(*resourceJson);
			else if (result.Type == "Texture")
				result.GpuBytes = EstimateTextureGpuBytes(extension, result.Data);
			result.Hash = HashBytes(result.Data);
			return result;
		}

		void WriteJsonFile(const std::filesystem::path& path, const Json& json)
		{
			const std::filesystem::path temporaryPath = path.string() + ".tmp";
			{
				std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
				if (!stream)
					Fail("Cannot write JSON output: " + temporaryPath.generic_string());
				stream << json.dump(2) << '\n';
				if (!stream)
					Fail("Cannot finish JSON output: " + temporaryPath.generic_string());
			}
			std::error_code error;
			std::filesystem::remove(path, error);
			error.clear();
			std::filesystem::rename(temporaryPath, path, error);
			if (error)
				Fail("Cannot replace JSON output '" + path.generic_string() + "': " + error.message());
		}

		void WritePak(
			const std::filesystem::path& pakPath,
			std::map<std::string, CookedFile>& files,
			int compressionLevel)
		{
			const std::filesystem::path temporaryPath = pakPath.string() + ".tmp";
			std::error_code error;
			std::filesystem::remove(temporaryPath, error);

			zip_t* archive = zip_open(temporaryPath.string().c_str(), compressionLevel, 'w');
			if (!archive)
				Fail("Cannot create pak: " + temporaryPath.generic_string());

			try
			{
				for (auto& [path, file] : files)
				{
					if (zip_entry_open(archive, path.c_str()) != 0)
						Fail("Cannot open pak entry: " + path);
					if (!file.Data.empty() && zip_entry_write(archive, file.Data.data(), file.Data.size()) != 0)
						Fail("Cannot write pak entry: " + path);
					if (zip_entry_close(archive) != 0)
						Fail("Cannot close pak entry: " + path);
					file.CompressedSize = static_cast<std::uint64_t>(zip_entry_comp_size(archive));
				}
				zip_close(archive);
				archive = nullptr;
			}
			catch (...)
			{
				if (archive)
					zip_close(archive);
				std::filesystem::remove(temporaryPath, error);
				throw;
			}

			std::filesystem::remove(pakPath, error);
			error.clear();
			std::filesystem::rename(temporaryPath, pakPath, error);
			if (error)
				Fail("Cannot replace pak '" + pakPath.generic_string() + "': " + error.message());
		}

		struct PakVerificationContext
		{
			std::uint64_t Hash = FnvOffsetBasis;
			std::uint64_t Bytes = 0;
			bool bInvalidOffset = false;
		};

		size_t VerifyPakChunk(void* argument, std::uint64_t offset, const void* data, size_t size)
		{
			auto& context = *static_cast<PakVerificationContext*>(argument);
			if (offset != context.Bytes)
			{
				context.bInvalidOffset = true;
				return 0;
			}
			context.Hash = HashBytes(
				context.Hash,
				static_cast<const std::uint8_t*>(data),
				size);
			context.Bytes += static_cast<std::uint64_t>(size);
			return size;
		}

		void VerifyPak(
			const std::filesystem::path& pakPath,
			const std::map<std::string, CookedFile>& files)
		{
			zip_t* archive = zip_open(pakPath.string().c_str(), 0, 'r');
			if (!archive)
				Fail("Cannot reopen generated pak: " + pakPath.generic_string());

			try
			{
				const auto entryCount = zip_entries_total(archive);
				if (entryCount < 0 || static_cast<std::size_t>(entryCount) != files.size())
					Fail("Generated pak has an unexpected entry count");

				std::size_t entryIndex = 0;
				for (const auto& [path, file] : files)
				{
					if (zip_entry_openbyindex(archive, entryIndex) != 0)
						Fail("Cannot reopen generated pak entry: " + path);
					const char* entryName = zip_entry_name(archive);
					if (!entryName || path != entryName || zip_entry_isdir(archive) != 0 ||
						zip_entry_size(archive) != file.Data.size() ||
						zip_entry_comp_size(archive) != file.CompressedSize)
					{
						Fail("Generated pak entry metadata is invalid: " + path);
					}

					PakVerificationContext context;
					if (zip_entry_extract(archive, VerifyPakChunk, &context) != 0 ||
						context.bInvalidOffset || context.Bytes != file.Data.size() || context.Hash != file.Hash)
					{
						Fail("Generated pak entry failed decompression verification: " + path);
					}
					if (zip_entry_close(archive) != 0)
						Fail("Cannot close verified pak entry: " + path);
					++entryIndex;
				}
				zip_close(archive);
				archive = nullptr;
			}
			catch (...)
			{
				if (archive)
					zip_close(archive);
				throw;
			}
		}

		bool PathsReferToSameLocation(
			const std::filesystem::path& first,
			const std::filesystem::path& second)
		{
			std::error_code firstError;
			std::error_code secondError;
			const auto resolveOutputPath = [](const std::filesystem::path& path, std::error_code& error)
			{
				const std::filesystem::path parent = std::filesystem::weakly_canonical(
					path.parent_path(),
					error);
				return error ? std::filesystem::path{} : (parent / path.filename()).lexically_normal();
			};
			std::string firstPath = resolveOutputPath(first, firstError).generic_string();
			std::string secondPath = resolveOutputPath(second, secondError).generic_string();
			if (firstError || secondError)
				Fail("Cannot resolve cooker output paths");
#if defined(_WIN32)
			firstPath = ToLowerAscii(std::move(firstPath));
			secondPath = ToLowerAscii(std::move(secondPath));
#endif
			return firstPath == secondPath;
		}

		Json BuildDependencyJson(const std::vector<ResourceDependency>& dependencies)
		{
			Json result = Json::array();
			for (const ResourceDependency& dependency : dependencies)
			{
				result.push_back({
					{ "Path", dependency.Path },
					{ "Kind", ToString(dependency.Kind) },
					{ "Source", ToManifestString(dependency.Source) }
				});
			}
			return result;
		}

		std::vector<std::string> BuildDependencyPathList(const std::vector<ResourceDependency>& dependencies)
		{
			std::vector<std::string> result;
			result.reserve(dependencies.size());
			for (const ResourceDependency& dependency : dependencies)
				result.push_back(dependency.Path);
			return result;
		}

		bool ShouldExpandDependency(RuntimeResourceWarmupExpand expand, DependencyKind kind)
		{
			if (expand == RuntimeResourceWarmupExpand::None)
				return false;
			if (kind == DependencyKind::HardRuntime)
				return true;
			return expand == RuntimeResourceWarmupExpand::Runtime &&
				kind == DependencyKind::SoftRuntime;
		}

		void ApplyWarmupTags(
			const CookConfig& config,
			std::map<std::string, CookedFile>& files)
		{
			for (const RuntimeResourceWarmupSchemaGroup& group : config.WarmupGroups)
			{
				const std::string tag = "Warmup:" + group.Name;
				for (const RuntimeResourceWarmupRoot& root : group.Roots)
				{
					std::set<std::string> visited;
					std::set<std::string> pending = { root.Path };
					while (!pending.empty())
					{
						const std::string path = *pending.begin();
						pending.erase(pending.begin());
						if (!visited.insert(path).second)
							continue;

						const auto found = files.find(path);
						if (found == files.end())
							Fail("Warm-up closure is missing packaged resource: " + path);
						found->second.Tags.insert(tag);
						for (const ResourceDependency& dependency : found->second.Dependencies)
						{
							if (ShouldExpandDependency(root.Expand, dependency.Kind))
								pending.insert(dependency.Path);
						}
					}
				}
			}
		}

		void ValidateCatalogLimits(const std::map<std::string, CookedFile>& files)
		{
			if (files.empty() || files.size() > RuntimeResourceCatalogLimits::MaxEntries)
				Fail("Resource catalog exceeds the packaged runtime entry limit");

			std::uint64_t totalDependencies = 0;
			std::uint64_t totalTags = 0;
			std::uint64_t totalCpuBytes = 0;
			std::uint64_t totalGpuBytes = 0;
			for (const auto& [path, file] : files)
			{
				const std::uint64_t dependencyCount =
					static_cast<std::uint64_t>(file.Dependencies.size());
				if (file.Dependencies.size() > RuntimeResourceCatalogLimits::MaxDependenciesPerEntry ||
					totalDependencies > RuntimeResourceCatalogLimits::MaxDependencies - dependencyCount)
				{
					Fail(path + ": resource catalog dependency limit exceeded");
				}
				totalDependencies += dependencyCount;

				const std::uint64_t tagCount = static_cast<std::uint64_t>(file.Tags.size());
				if (file.Tags.size() > RuntimeResourceCatalogLimits::MaxTagsPerEntry ||
					totalTags > RuntimeResourceCatalogLimits::MaxTags - tagCount)
				{
					Fail(path + ": resource catalog tag limit exceeded");
				}
				totalTags += tagCount;
				for (const std::string& tag : file.Tags)
				{
					if (tag.empty() || tag.size() > RuntimeResourceCatalogLimits::MaxTagLength ||
						std::any_of(tag.begin(), tag.end(), [](unsigned char character) { return character < 32; }))
					{
						Fail(path + ": resource catalog contains an invalid tag");
					}
				}

				if (file.CpuBytes > RuntimeResourceCatalogLimits::MaxResourceEstimate ||
					file.GpuBytes > RuntimeResourceCatalogLimits::MaxResourceEstimate ||
					totalCpuBytes > RuntimeResourceCatalogLimits::MaxCatalogEstimate - file.CpuBytes ||
					totalGpuBytes > RuntimeResourceCatalogLimits::MaxCatalogEstimate - file.GpuBytes)
				{
					Fail(path + ": resource catalog estimate limit exceeded");
				}
				totalCpuBytes += file.CpuBytes;
				totalGpuBytes += file.GpuBytes;
			}
		}

		std::uint64_t ComputeContentHash(const std::map<std::string, CookedFile>& files)
		{
			std::uint64_t hash = FnvOffsetBasis;
			for (const auto& [path, file] : files)
			{
				hash = HashLittleEndian64(hash, static_cast<std::uint64_t>(path.size()));
				hash = HashString(hash, path);
				hash = HashLittleEndian64(hash, static_cast<std::uint64_t>(file.Data.size()));
				hash = HashBytes(hash, file.Data.data(), file.Data.size());
			}
			return hash;
		}

		std::uint64_t HashLengthPrefixedString(std::uint64_t hash, std::string_view value)
		{
			hash = HashLittleEndian64(hash, static_cast<std::uint64_t>(value.size()));
			return HashString(hash, value);
		}

		std::uint64_t ComputeRegistryHash(const std::map<std::string, CookedFile>& files)
		{
			std::uint64_t hash = FnvOffsetBasis;
			hash = HashLengthPrefixedString(hash, RuntimeResourceCatalogLimits::RegistryHashDomain);
			hash = HashLittleEndian64(hash, static_cast<std::uint64_t>(files.size()));
			for (const auto& [path, file] : files)
			{
				hash = HashLengthPrefixedString(hash, path);
				hash = HashLengthPrefixedString(hash, file.Type);
				hash = HashLittleEndian64(hash, file.CookFormatVersion);
				hash = HashLittleEndian64(hash, static_cast<std::uint64_t>(file.Dependencies.size()));
				for (const ResourceDependency& dependency : file.Dependencies)
				{
					hash = HashLengthPrefixedString(hash, dependency.Path);
					hash = HashLengthPrefixedString(hash, ToString(dependency.Kind));
					hash = HashLengthPrefixedString(hash, ToManifestString(dependency.Source));
				}
				hash = HashLittleEndian64(hash, static_cast<std::uint64_t>(file.Tags.size()));
				for (const std::string& tag : file.Tags)
					hash = HashLengthPrefixedString(hash, tag);
				hash = HashLittleEndian64(hash, file.CpuBytes);
				hash = HashLittleEndian64(hash, file.GpuBytes);
			}
			return hash;
		}

		Json BuildManifest(
			const CookConfig& config,
			const std::map<std::string, CookedFile>& files,
			std::uint64_t contentHash,
			std::uint64_t registryHash,
			std::uint64_t pakSize)
		{
			Json fileList = Json::array();
			for (const auto& [path, file] : files)
			{
				fileList.push_back({
					{ "Path", path },
					{ "SourceSize", file.SourceSize },
					{ "StoredSize", file.Data.size() },
					{ "CompressedSize", file.CompressedSize },
					{ "CookedJson", file.bCookedJson },
					{ "Hash", Hex64(file.Hash) },
					{ "Type", file.Type },
					{ "CookFormatVersion", file.CookFormatVersion },
					{ "Dependencies", BuildDependencyJson(file.Dependencies) },
					{ "Tags", file.Tags },
					{ "CpuBytes", file.CpuBytes },
					{ "GpuBytes", file.GpuBytes }
				});
			}

			return Json{
				{ "ManifestVersion", 1 },
				{ "ResourceCatalogVersion", RuntimeResourceCatalogLimits::SchemaVersion },
				{ "ContentVersion", config.ContentVersion },
				{ "GameName", config.GameName },
				{ "EntryScene", config.EntryScene },
				{ "PakFile", "Game.pak" },
				{ "PakFormat", "zip" },
				{ "Compression", "deflate" },
				{ "CookedJsonMagic", std::string(Utils::CookedJsonMagicText) },
				{ "CookedJsonFormat", "messagepack" },
				{ "ContentHashAlgorithm", "fnv1a64" },
				{ "ContentHash", Hex64(contentHash) },
				{ "RegistryHashAlgorithm", std::string(RuntimeResourceCatalogLimits::RegistryHashAlgorithm) },
				{ "RegistryHash", Hex64(registryHash) },
				{ "FileCount", files.size() },
				{ "PakSize", pakSize },
				{ "Files", std::move(fileList) }
			};
		}

		using ReachablePaths = std::set<std::string>;

		ReachablePaths ComputeReachablePaths(
			const std::map<std::string, CookedFile>& files,
			const std::vector<const SeedRecord*>& selectedSeeds,
			bool bSkipHeuristic = false)
		{
			ReachablePaths reachable;
			std::set<std::string> pending;
			for (const SeedRecord* seed : selectedSeeds)
			{
				if (seed && files.contains(seed->Path))
					pending.insert(seed->Path);
			}
			while (!pending.empty())
			{
				const std::string path = *pending.begin();
				pending.erase(pending.begin());
				if (!reachable.insert(path).second)
					continue;
				const auto file = files.find(path);
				if (file == files.end())
					continue;
				for (const ResourceDependency& dependency : file->second.Dependencies)
				{
					if (!bSkipHeuristic || dependency.Source != DependencySource::Heuristic)
						pending.insert(dependency.Path);
				}
			}
			return reachable;
		}

		struct WitnessValue
		{
			std::size_t Distance = 0;
			const SeedRecord* Seed = nullptr;
			std::vector<std::string> Chain;
		};

		bool WitnessLess(const WitnessValue& left, const WitnessValue& right)
		{
			const int leftRank = left.Seed ? SeedSourceRank(left.Seed->Source) : std::numeric_limits<int>::max();
			const int rightRank = right.Seed ? SeedSourceRank(right.Seed->Source) : std::numeric_limits<int>::max();
			const std::string_view leftId = left.Seed ? std::string_view(left.Seed->Id) : std::string_view{};
			const std::string_view rightId = right.Seed ? std::string_view(right.Seed->Id) : std::string_view{};
			return std::tie(left.Distance, leftRank, leftId, left.Chain) <
				std::tie(right.Distance, rightRank, rightId, right.Chain);
		}

		std::map<std::string, WitnessValue> BuildRootWitnesses(
			const std::map<std::string, CookedFile>& files,
			const std::vector<SeedRecord>& seeds)
		{
			struct FrontierLess
			{
				bool operator()(const std::pair<std::string, WitnessValue>& left,
					const std::pair<std::string, WitnessValue>& right) const
				{
					if (WitnessLess(left.second, right.second)) return true;
					if (WitnessLess(right.second, left.second)) return false;
					return left.first < right.first;
				}
			};

			std::map<std::string, WitnessValue> witnesses;
			std::set<std::pair<std::string, WitnessValue>, FrontierLess> frontier;
			for (const SeedRecord& seed : seeds)
			{
				if (!files.contains(seed.Path))
					continue;
				WitnessValue candidate{ 0, &seed, { seed.Path } };
				const auto existing = witnesses.find(seed.Path);
				if (existing == witnesses.end() || WitnessLess(candidate, existing->second))
				{
					witnesses[seed.Path] = candidate;
					frontier.emplace(seed.Path, std::move(candidate));
				}
			}

			while (!frontier.empty())
			{
				auto current = *frontier.begin();
				frontier.erase(frontier.begin());
				const auto best = witnesses.find(current.first);
				if (best == witnesses.end() || WitnessLess(best->second, current.second))
					continue;
				const auto owner = files.find(current.first);
				if (owner == files.end())
					continue;
				for (const ResourceDependency& dependency : owner->second.Dependencies)
				{
					WitnessValue candidate = current.second;
					++candidate.Distance;
					candidate.Chain.push_back(dependency.Path);
					const auto existing = witnesses.find(dependency.Path);
					if (existing == witnesses.end() || WitnessLess(candidate, existing->second))
					{
						witnesses[dependency.Path] = candidate;
						frontier.emplace(dependency.Path, std::move(candidate));
					}
				}
			}
			return witnesses;
		}

		Json BuildDiagnosticJson(const DependencyDiagnostic& diagnostic)
		{
			return Json{
				{ "Owner", diagnostic.Owner },
				{ "Target", diagnostic.Target },
				{ "RawReference", diagnostic.RawReference },
				{ "Kind", ToString(diagnostic.Kind) },
				{ "Source", ToString(diagnostic.Source) },
				{ "Provider", diagnostic.Provider },
				{ "Schema", diagnostic.Schema },
				{ "Location", diagnostic.Location },
				{ "Reason", diagnostic.Reason }
			};
		}

		Json BuildEvidenceJson(const DependencyEvidence& evidence)
		{
			return Json{
				{ "Owner", evidence.Owner },
				{ "Target", evidence.Target },
				{ "RawReference", evidence.RawReference },
				{ "Kind", ToString(evidence.Kind) },
				{ "Source", ToString(evidence.Source) },
				{ "Provider", evidence.Provider },
				{ "Schema", evidence.Schema },
				{ "Location", evidence.Location },
				{ "Reason", evidence.Reason }
			};
		}

		Json BuildByteSummary(
			const ReachablePaths& paths,
			const std::map<std::string, CookedFile>& files)
		{
			std::uint64_t sourceBytes = 0;
			std::uint64_t cookedBytes = 0;
			std::uint64_t compressedBytes = 0;
			for (const std::string& path : paths)
			{
				const CookedFile& file = files.at(path);
				sourceBytes += file.SourceSize;
				cookedBytes += static_cast<std::uint64_t>(file.Data.size());
				compressedBytes += file.CompressedSize;
			}
			return Json{
				{ "FileCount", paths.size() },
				{ "SourceBytes", sourceBytes },
				{ "CookedBytes", cookedBytes },
				{ "CompressedBytes", compressedBytes }
			};
		}

		Json BuildContributionJson(
			std::string id,
			const std::vector<const SeedRecord*>& contributionSeeds,
			const std::vector<const SeedRecord*>& allSeeds,
			const ReachablePaths& allReachable,
			const std::map<std::string, CookedFile>& files)
		{
			std::set<std::string> removedSeedIds;
			for (const SeedRecord* seed : contributionSeeds)
				removedSeedIds.insert(seed->Id);
			std::vector<const SeedRecord*> remainingSeeds;
			for (const SeedRecord* seed : allSeeds)
			{
				if (!removedSeedIds.contains(seed->Id))
					remainingSeeds.push_back(seed);
			}
			const ReachablePaths reachable = ComputeReachablePaths(files, contributionSeeds);
			const ReachablePaths without = ComputeReachablePaths(files, remainingSeeds);
			ReachablePaths exclusive;
			std::set_difference(
				allReachable.begin(), allReachable.end(), without.begin(), without.end(),
				std::inserter(exclusive, exclusive.end()));

			Json removedFiles = Json::array();
			for (const std::string& path : exclusive)
				removedFiles.push_back(path);
			const SeedRecord& first = *contributionSeeds.front();
			const Json exclusiveBytes = BuildByteSummary(exclusive, files);
			return Json{
				{ "Id", std::move(id) },
				{ "SeedSource", ToString(first.Source) },
				{ "Origin", first.Origin },
				{ "Group", first.Group },
				{ "SeedCount", contributionSeeds.size() },
				{ "Reachable", BuildByteSummary(reachable, files) },
				{ "Exclusive", exclusiveBytes },
				{ "RemovalSimulation", {
					{ "RemovedFileCount", exclusive.size() },
					{ "SourceBytes", exclusiveBytes["SourceBytes"] },
					{ "CookedBytes", exclusiveBytes["CookedBytes"] },
					{ "CompressedBytes", exclusiveBytes["CompressedBytes"] },
					{ "Files", std::move(removedFiles) }
				} }
			};
		}

		enum class TopFileMetric
		{
			Source,
			Cooked,
			Compressed
		};

		Json BuildTopFiles(
			const std::map<std::string, CookedFile>& files,
			TopFileMetric metric)
		{
			std::vector<const CookedFile*> sorted;
			sorted.reserve(files.size());
			for (const auto& [path, file] : files)
			{
				(void)path;
				sorted.push_back(&file);
			}
			const auto bytes = [metric](const CookedFile& file)
			{
				switch (metric)
				{
				case TopFileMetric::Source: return file.SourceSize;
				case TopFileMetric::Cooked: return static_cast<std::uint64_t>(file.Data.size());
				case TopFileMetric::Compressed: return file.CompressedSize;
				}
				return std::uint64_t{ 0 };
			};
			std::sort(sorted.begin(), sorted.end(), [&](const CookedFile* left, const CookedFile* right)
			{
				if (bytes(*left) != bytes(*right))
					return bytes(*left) > bytes(*right);
				return left->LogicalPath < right->LogicalPath;
			});
			Json result = Json::array();
			for (std::size_t index = 0; index < std::min<std::size_t>(50, sorted.size()); ++index)
			{
				result.push_back({
					{ "Path", sorted[index]->LogicalPath },
					{ "Type", sorted[index]->Type },
					{ "Bytes", bytes(*sorted[index]) }
				});
			}
			return result;
		}

		Json BuildDuplicateContentGroups(const std::map<std::string, CookedFile>& files)
		{
			std::map<std::pair<std::uint64_t, std::size_t>, std::vector<const CookedFile*>> candidates;
			for (const auto& [path, file] : files)
			{
				(void)path;
				candidates[{ file.Hash, file.Data.size() }].push_back(&file);
			}
			Json result = Json::array();
			for (const auto& [identity, candidateFiles] : candidates)
			{
				if (candidateFiles.size() < 2)
					continue;
				std::vector<std::vector<const CookedFile*>> exactGroups;
				for (const CookedFile* candidate : candidateFiles)
				{
					auto group = std::find_if(exactGroups.begin(), exactGroups.end(), [&](const auto& value)
					{
						return value.front()->Data == candidate->Data;
					});
					if (group == exactGroups.end())
						exactGroups.push_back({ candidate });
					else
						group->push_back(candidate);
				}
				for (const auto& group : exactGroups)
				{
					if (group.size() < 2)
						continue;
					Json paths = Json::array();
					std::uint64_t sourceBytes = 0;
					std::uint64_t compressedBytes = 0;
					for (const CookedFile* file : group)
					{
						paths.push_back(file->LogicalPath);
						sourceBytes += file->SourceSize;
						compressedBytes += file->CompressedSize;
					}
					result.push_back({
						{ "Hash", Hex64(identity.first) },
						{ "CookedBytesPerFile", identity.second },
						{ "Paths", std::move(paths) },
						{ "PossibleSourceSavings", sourceBytes - group.front()->SourceSize },
						{ "PossibleCookedSavings", identity.second * (group.size() - 1) },
						{ "PossibleCompressedSavings", compressedBytes - group.front()->CompressedSize }
					});
				}
			}
			return result;
		}

		Json BuildReport(
			const CookConfig& config,
			const ProjectIndex& index,
			const std::map<std::string, CookedFile>& files,
			const std::vector<SeedRecord>& seeds,
			std::uint64_t pakSize,
			std::uint64_t contentHash,
			std::uint64_t registryHash,
			std::string_view runId)
		{
			struct TypeBytes
			{
				std::size_t Count = 0;
				std::uint64_t Source = 0;
				std::uint64_t Cooked = 0;
				std::uint64_t Compressed = 0;
			};
			std::uint64_t sourceBytes = 0;
			std::uint64_t storedBytes = 0;
			std::uint64_t prunedBytes = 0;
			std::uint64_t estimatedCpuBytes = 0;
			std::uint64_t estimatedGpuBytes = 0;
			std::size_t cookedJsonCount = 0;
			std::size_t warmupTaggedFileCount = 0;
			std::map<std::string, std::size_t> resourceTypeCounts;
			std::map<std::string, std::size_t> dependencyKindCounts = {
				{ "HardRuntime", 0 },
				{ "SoftRuntime", 0 },
				{ "PackagingOnly", 0 }
			};
			std::map<std::string, std::size_t> dependencySourceCounts = {
				{ "Structured", 0 },
				{ "Heuristic", 0 }
			};
			std::map<std::string, std::size_t> evidenceSourceCounts = {
				{ "Structured", 0 },
				{ "Heuristic", 0 },
				{ "ExplicitDynamic", 0 }
			};
			std::map<std::string, std::size_t> warmupGroupFileCounts;
			std::map<std::string, TypeBytes> resourceTypeBytes;
			std::map<std::string, std::vector<DependencyEvidence>> directReferrers;
			std::map<std::string, std::set<std::string>> reverseDependencies;
			Json dependencyList = Json::array();
			Json edges = Json::array();
			Json virtualDependencies = Json::array();
			Json unresolvedCandidates = Json::array();
			Json unreferenced = Json::array();
			for (const auto& [path, file] : files)
			{
				sourceBytes += file.SourceSize;
				storedBytes += static_cast<std::uint64_t>(file.Data.size());
				if (file.CpuBytes > std::numeric_limits<std::uint64_t>::max() - estimatedCpuBytes ||
					file.GpuBytes > std::numeric_limits<std::uint64_t>::max() - estimatedGpuBytes)
				{
					Fail("Cook report resource byte estimate overflow: " + path);
				}
				estimatedCpuBytes += file.CpuBytes;
				estimatedGpuBytes += file.GpuBytes;
				cookedJsonCount += file.bCookedJson ? 1u : 0u;
				++resourceTypeCounts[file.Type];
				TypeBytes& typeBytes = resourceTypeBytes[file.Type];
				++typeBytes.Count;
				typeBytes.Source += file.SourceSize;
				typeBytes.Cooked += static_cast<std::uint64_t>(file.Data.size());
				typeBytes.Compressed += file.CompressedSize;
				bool bWarmupTagged = false;
				for (const std::string& tag : file.Tags)
				{
					if (tag.starts_with("Warmup:"))
					{
						bWarmupTagged = true;
						++warmupGroupFileCounts[tag.substr(7)];
					}
				}
				warmupTaggedFileCount += bWarmupTagged ? 1u : 0u;
				for (const ResourceDependency& dependency : file.Dependencies)
				{
					++dependencyKindCounts[ToString(dependency.Kind)];
					++dependencySourceCounts[ToManifestString(dependency.Source)];
					reverseDependencies[dependency.Path].insert(path);
					for (const DependencyEvidence& evidence : dependency.Evidence)
					{
						++evidenceSourceCounts[ToString(evidence.Source)];
						directReferrers[dependency.Path].push_back(evidence);
						edges.push_back(BuildEvidenceJson(evidence));
					}
				}
				for (const DependencyDiagnostic& diagnostic : file.VirtualDependencies)
					virtualDependencies.push_back(BuildDiagnosticJson(diagnostic));
				for (const DependencyDiagnostic& diagnostic : file.UnresolvedCandidates)
					unresolvedCandidates.push_back(BuildDiagnosticJson(diagnostic));
				dependencyList.push_back({
					{ "Path", path },
					{ "Dependencies", BuildDependencyPathList(file.Dependencies) },
					{ "CatalogDependencies", BuildDependencyJson(file.Dependencies) }
				});
			}
			for (auto& [path, referrers] : directReferrers)
			{
				(void)path;
				std::sort(referrers.begin(), referrers.end(), EvidenceLess);
			}
			for (const auto& [key, file] : index.Files)
			{
				if (!files.contains(file.LogicalPath))
				{
					prunedBytes += file.Size;
					unreferenced.push_back(file.LogicalPath);
				}
			}

			std::vector<const SeedRecord*> allSeeds;
			allSeeds.reserve(seeds.size());
			for (const SeedRecord& seed : seeds)
				allSeeds.push_back(&seed);
			const ReachablePaths allReachable = ComputeReachablePaths(files, allSeeds);
			std::vector<const SeedRecord*> nonDirectorySeeds;
			std::vector<const SeedRecord*> nonExplicitRootSeeds;
			for (const SeedRecord* seed : allSeeds)
			{
				if (seed->Source != SeedSource::IncludeDirectoryFallback)
					nonDirectorySeeds.push_back(seed);
				if (seed->Source != SeedSource::ProjectRoot && seed->Source != SeedSource::EngineRoot)
					nonExplicitRootSeeds.push_back(seed);
			}
			const ReachablePaths reachableWithoutDirectories =
				ComputeReachablePaths(files, nonDirectorySeeds);
			const ReachablePaths reachableWithoutExplicitRoots =
				ComputeReachablePaths(files, nonExplicitRootSeeds);
			const ReachablePaths reachableWithoutHeuristics =
				ComputeReachablePaths(files, allSeeds, true);
			const std::map<std::string, WitnessValue> witnesses = BuildRootWitnesses(files, seeds);

			Json fileAnalysis = Json::array();
			for (const auto& [path, file] : files)
			{
				Json inclusionReasons = Json::array();
				for (const SeedRecord& seed : seeds)
				{
					if (seed.Path != path)
						continue;
					inclusionReasons.push_back({
						{ "Type", "Seed" },
						{ "SeedId", seed.Id },
						{ "SeedSource", ToString(seed.Source) },
						{ "Origin", seed.Origin },
						{ "Group", seed.Group },
						{ "Reason", seed.Reason }
					});
				}

				Json referrers = Json::array();
				const auto incoming = directReferrers.find(path);
				if (incoming != directReferrers.end())
				{
					for (const DependencyEvidence& evidence : incoming->second)
					{
						Json value = {
							{ "Owner", evidence.Owner },
							{ "Kind", ToString(evidence.Kind) },
							{ "Source", ToString(evidence.Source) },
							{ "Provider", evidence.Provider },
							{ "Schema", evidence.Schema },
							{ "Location", evidence.Location },
							{ "Reason", evidence.Reason }
						};
						referrers.push_back(value);
						value["Type"] = "Dependency";
						inclusionReasons.push_back(std::move(value));
					}
				}

				Json witnessJson = nullptr;
				const auto witness = witnesses.find(path);
				if (witness != witnesses.end() && witness->second.Seed)
				{
					witnessJson = {
						{ "SeedId", witness->second.Seed->Id },
						{ "SeedSource", ToString(witness->second.Seed->Source) },
						{ "Origin", witness->second.Seed->Origin },
						{ "Group", witness->second.Seed->Group },
						{ "Chain", witness->second.Chain }
					};
				}

				fileAnalysis.push_back({
					{ "Path", path },
					{ "SourceSize", file.SourceSize },
					{ "CookedSize", file.Data.size() },
					{ "CompressedSize", file.CompressedSize },
					{ "Hash", Hex64(file.Hash) },
					{ "Type", file.Type },
					{ "InclusionReasons", std::move(inclusionReasons) },
					{ "RootWitness", std::move(witnessJson) },
					{ "DirectReferrers", std::move(referrers) },
					{ "OnlyIncludedByDirectory", !reachableWithoutDirectories.contains(path) },
					{ "OnlyIncludedByExplicitRoot", !reachableWithoutExplicitRoots.contains(path) },
					{ "HeuristicOnly", !reachableWithoutHeuristics.contains(path) }
				});
			}

			Json reverseDependencyJson = Json::array();
			for (const auto& [path, owners] : reverseDependencies)
				reverseDependencyJson.push_back({ { "Path", path }, { "Referrers", owners } });

			std::map<std::string, std::vector<const SeedRecord*>> rootGroups;
			std::map<std::string, std::vector<const SeedRecord*>> directoryGroups;
			for (const SeedRecord* seed : allSeeds)
			{
				if (seed->Source == SeedSource::IncludeDirectoryFallback)
				{
					directoryGroups[seed->Origin].push_back(seed);
					continue;
				}
				const std::string id = seed->Source == SeedSource::WarmupRoot
					? std::string(ToString(seed->Source)) + ":" + seed->Group
					: std::string(ToString(seed->Source)) + ":" + seed->Origin;
				rootGroups[id].push_back(seed);
			}
			Json rootContributions = Json::array();
			for (const auto& [id, groupSeeds] : rootGroups)
				rootContributions.push_back(BuildContributionJson(id, groupSeeds, allSeeds, allReachable, files));
			Json directoryContributions = Json::array();
			for (const auto& [directory, groupSeeds] : directoryGroups)
			{
				directoryContributions.push_back(BuildContributionJson(
					"IncludeDirectoryFallback:" + directory, groupSeeds, allSeeds, allReachable, files));
			}

			Json typeBytesJson = Json::object();
			for (const auto& [type, value] : resourceTypeBytes)
			{
				typeBytesJson[type] = {
					{ "FileCount", value.Count },
					{ "SourceBytes", value.Source },
					{ "CookedBytes", value.Cooked },
					{ "CompressedBytes", value.Compressed }
				};
			}

			return Json{
				{ "ReportVersion", 2 },
				{ "Succeeded", true },
				{ "RunId", std::string(runId) },
				{ "Status", "Succeeded" },
				{ "OutcomeScope", "Cook" },
				{ "ContentHash", Hex64(contentHash) },
				{ "RegistryHash", Hex64(registryHash) },
				{ "GameName", config.GameName },
				{ "IndexedFileCount", index.Files.size() },
				{ "IncludedFileCount", files.size() },
				{ "CookedJsonFileCount", cookedJsonCount },
				{ "PrunedFileCount", index.Files.size() - files.size() },
				{ "ExcludedByRuleCount", index.ExcludedByRule.size() },
				{ "IncludedSourceBytes", sourceBytes },
				{ "CookedPayloadBytes", storedBytes },
				{ "EstimatedCpuBytes", estimatedCpuBytes },
				{ "EstimatedGpuBytes", estimatedGpuBytes },
				{ "PrunedBytes", prunedBytes },
				{ "PakBytes", pakSize },
				{ "ResourceTypeCounts", resourceTypeCounts },
				{ "DependencyKindCounts", dependencyKindCounts },
				{ "DependencySourceCounts", dependencySourceCounts },
				{ "DependencyEvidenceSourceCounts", evidenceSourceCounts },
				{ "WarmupTaggedFileCount", warmupTaggedFileCount },
				{ "WarmupGroupFileCounts", warmupGroupFileCounts },
				{ "ExcludedByRule", index.ExcludedByRule },
				{ "UnreferencedFiles", std::move(unreferenced) },
				{ "Dependencies", std::move(dependencyList) },
				{ "Files", std::move(fileAnalysis) },
				{ "Edges", std::move(edges) },
				{ "ReverseDependencies", std::move(reverseDependencyJson) },
				{ "VirtualDependencies", std::move(virtualDependencies) },
				{ "UnresolvedCandidates", std::move(unresolvedCandidates) },
				{ "RootContributions", std::move(rootContributions) },
				{ "IncludeDirectoryContributions", std::move(directoryContributions) },
				{ "ResourceTypeBytes", std::move(typeBytesJson) },
				{ "LargestSourceFiles", BuildTopFiles(files, TopFileMetric::Source) },
				{ "LargestCookedFiles", BuildTopFiles(files, TopFileMetric::Cooked) },
				{ "LargestCompressedFiles", BuildTopFiles(files, TopFileMetric::Compressed) },
				{ "DuplicateContentGroups", BuildDuplicateContentGroups(files) }
			};
		}
	}

	int Run(const CookOptions& options)
	{
		std::error_code error;
		const std::filesystem::path projectRoot = std::filesystem::weakly_canonical(options.ProjectRoot, error);
		if (error || !std::filesystem::is_directory(projectRoot))
			Fail("Invalid project root: " + options.ProjectRoot.generic_string());

		const auto buildRootKey = [](const std::filesystem::path& root)
		{
			std::string key = root.generic_string();
#if defined(_WIN32)
			key = ToLowerAscii(std::move(key));
#endif
			return key;
		};
		std::vector<std::filesystem::path> forbiddenRoots;
		std::set<std::string> forbiddenRootKeys{ buildRootKey(projectRoot) };
		const auto appendForbiddenRoot = [&](const std::filesystem::path& root)
		{
			if (forbiddenRootKeys.insert(buildRootKey(root)).second)
				forbiddenRoots.push_back(root);
		};
		for (const std::filesystem::path& rawForbiddenRoot : options.ForbiddenRoots)
		{
			if (!rawForbiddenRoot.is_absolute())
			{
				Fail("--forbid-root must be an absolute directory: " +
					rawForbiddenRoot.generic_string());
			}

			error.clear();
			const std::filesystem::path canonicalForbiddenRoot =
				std::filesystem::weakly_canonical(rawForbiddenRoot, error);
			if (error || !std::filesystem::is_directory(canonicalForbiddenRoot))
			{
				Fail("Invalid --forbid-root directory: " + rawForbiddenRoot.generic_string());
			}

			appendForbiddenRoot(rawForbiddenRoot);
			appendForbiddenRoot(rawForbiddenRoot.lexically_normal());
			appendForbiddenRoot(canonicalForbiddenRoot);
		}
		const std::filesystem::path outputDirectory = std::filesystem::weakly_canonical(
			std::filesystem::absolute(options.OutputDirectory), error);
		if (error)
			Fail("Cannot resolve cooker output directory: " + options.OutputDirectory.generic_string());
		const std::filesystem::path pakPath = outputDirectory / "Game.pak";
		const std::filesystem::path manifestPath = outputDirectory / "Game.manifest.json";
		const std::filesystem::path reportPath = options.ReportPath.empty()
			? outputDirectory / "CookReport.json"
			: std::filesystem::absolute(options.ReportPath);
		const std::string runId = options.RunId.empty() ? "manual" : options.RunId;
		if (runId.empty() || runId.size() > 128 ||
			!std::all_of(runId.begin(), runId.end(), [](unsigned char character)
			{
				return (character >= 'a' && character <= 'z') ||
					(character >= 'A' && character <= 'Z') ||
					(character >= '0' && character <= '9') ||
					character == '.' || character == '_' || character == '-';
			}))
		{
			Fail("--run-id must use 1-128 ASCII letters, digits, dots, underscores, or hyphens");
		}

		const std::filesystem::path rawConfigPath = options.ConfigPath.is_absolute()
			? options.ConfigPath
			: projectRoot / options.ConfigPath;
		const std::filesystem::path configPath = std::filesystem::weakly_canonical(rawConfigPath, error);
		if (error || !IsWithin(projectRoot, configPath) || !std::filesystem::is_regular_file(configPath))
			Fail("Cook config must be a file inside the project root: " + options.ConfigPath.generic_string());
		const std::string configLogicalPath = NormalizeLogicalPath(
			configPath.lexically_relative(projectRoot).generic_string(), "Cook config");
		std::filesystem::create_directories(reportPath.parent_path(), error);
		if (error)
			Fail("Cannot create report output directory: " + error.message());
		if (PathsReferToSameLocation(reportPath, pakPath) || PathsReferToSameLocation(reportPath, manifestPath))
			Fail("Cook report path must not overwrite Game.pak or Game.manifest.json");
		WriteJsonFile(reportPath, Json{
			{ "ReportVersion", 2 },
			{ "Succeeded", false },
			{ "RunId", runId },
			{ "Status", "Running" },
			{ "OutcomeScope", "Cook" }
		});

		const Json configJson = ReadTextJson(configPath);
		CookConfig config = ParseCookConfig(configJson);
		if (!options.EngineConfigPath.empty())
		{
			if (!options.EngineConfigPath.is_absolute())
				Fail("--engine-config must be an absolute file path");
			error.clear();
			const std::filesystem::path engineConfigPath =
				std::filesystem::weakly_canonical(options.EngineConfigPath, error);
			if (error || !std::filesystem::is_regular_file(engineConfigPath))
			{
				Fail("Invalid engine cook config: " +
					options.EngineConfigPath.generic_string());
			}
			MergeEngineCookConfig(config, ReadTextJson(engineConfigPath));
		}
		config.EntryScene = NormalizeLogicalPath(config.EntryScene, "Game.DefaultScene");

		ProjectIndex projectIndex = BuildProjectIndex(projectRoot, config);
		std::set<std::string> pending;
		std::vector<SeedRecord> seeds;
		std::set<std::string> seedIds;
		struct DiscoveryWitness
		{
			std::size_t Distance = 0;
			std::string Text;
		};
		std::map<std::string, DiscoveryWitness> discoveryWitnesses;
		const auto addSeed = [&](
			std::string rawPath,
			SeedSource source,
			std::string origin,
			std::string group,
			std::string reason,
			std::string_view context) -> std::string
		{
			const std::string normalized = NormalizeLogicalPath(std::move(rawPath), std::string(context));
			const std::string path = RequireIndexedFile(projectIndex, normalized, std::string(context)).LogicalPath;
			const std::string id = std::string(ToString(source)) + ":" + origin + ":" + path;
			if (seedIds.insert(id).second)
			{
				seeds.push_back({
					.Id = id,
					.Path = path,
					.Source = source,
					.Origin = std::move(origin),
					.Group = std::move(group),
					.Reason = std::move(reason)
				});
			}
			pending.insert(path);
			const DiscoveryWitness candidate{ 0, "[" + id + "] " + path };
			const auto found = discoveryWitnesses.find(path);
			if (found == discoveryWitnesses.end() || candidate.Text < found->second.Text)
				discoveryWitnesses[path] = candidate;
			return path;
		};

		for (const std::string_view invariant : {
				std::string_view("Config/DefaultEngine.json"),
				std::string_view("Config/BaseEngine.json") })
		{
			(void)addSeed(
				std::string(invariant), SeedSource::RequiredInvariant, std::string(invariant), {},
				"required packaged runtime configuration", "RequiredInvariant");
		}
		(void)addSeed(
			configLogicalPath, SeedSource::GameConfig, configLogicalPath, {},
			"selected game configuration", "GameConfig");
		(void)addSeed(
			config.EntryScene, SeedSource::EntryScene, config.EntryScene, {},
			"Game.DefaultScene", "Game.DefaultScene");
		for (const std::string& rootFile : config.ProjectRootFiles)
		{
			(void)addSeed(
				rootFile, SeedSource::ProjectRoot, rootFile, {},
				"Cook.RootFiles", "Cook.RootFiles");
		}
		for (const std::string& rootFile : config.EngineRootFiles)
		{
			(void)addSeed(
				rootFile, SeedSource::EngineRoot, rootFile, {},
				"EngineCook.RootFiles", "EngineCook.RootFiles");
		}
		for (RuntimeResourceWarmupSchemaGroup& group : config.WarmupGroups)
		{
			for (RuntimeResourceWarmupRoot& root : group.Roots)
			{
				root.Path = addSeed(
					root.Path, SeedSource::WarmupRoot, group.Name, group.Name,
					"Game.ResourceWarmup group '" + group.Name + "'",
					"Game.ResourceWarmup");
			}
		}
		for (const DynamicAssetSetConfig& assetSet : config.DynamicAssetSets)
		{
			(void)addSeed(
				assetSet.Manifest, SeedSource::ExplicitDynamicAssetSet, assetSet.Name, assetSet.Name,
				"Cook.DynamicAssetSets manifest", "Cook.DynamicAssetSets");
		}
		for (std::string directory : config.IncludeDirectories)
		{
			directory = NormalizeLogicalPath(std::move(directory), "Cook.IncludeDirectories");
			ResolveWithin(projectRoot, directory, "Cook.IncludeDirectories");
			const std::string prefix = FoldPath(directory) + "/";
			std::size_t matchCount = 0;
			for (const auto& [key, file] : projectIndex.Files)
			{
				if (key.starts_with(prefix))
				{
					(void)addSeed(
						file.LogicalPath, SeedSource::IncludeDirectoryFallback, directory, {},
						"Cook.IncludeDirectories fallback '" + directory + "'",
						"Cook.IncludeDirectories");
					++matchCount;
				}
			}
			if (matchCount == 0)
				Fail("Cook.IncludeDirectories contains no cookable files: " + directory);
		}
		std::sort(seeds.begin(), seeds.end(), [](const SeedRecord& left, const SeedRecord& right)
		{
			return std::tie(left.Source, left.Origin, left.Group, left.Path, left.Id) <
				std::tie(right.Source, right.Origin, right.Group, right.Path, right.Id);
		});

		std::map<std::string, CookedFile> cookedFiles;
		std::uint64_t cookedContentSize = 0;
		while (!pending.empty())
		{
			const std::string logicalPath = *pending.begin();
			pending.erase(pending.begin());
			if (cookedFiles.contains(logicalPath))
				continue;
			if (cookedFiles.size() >= MaxFileCount)
				Fail("Cooked content exceeds the packaged runtime file-count limit");

			const IndexedFile& source = RequireIndexedFile(projectIndex, logicalPath, "Dependency closure");
			const auto ownerWitness = discoveryWitnesses.find(logicalPath);
			CookedFile cooked = CookFile(
				source,
				config,
				projectIndex,
				configLogicalPath,
				projectRoot,
				forbiddenRoots,
				ownerWitness == discoveryWitnesses.end() ? std::string{} : ownerWitness->second.Text);
			if (cooked.Data.size() > MaxContentSize - cookedContentSize)
				Fail("Cooked content exceeds the packaged runtime total-size limit");
			cookedContentSize += static_cast<std::uint64_t>(cooked.Data.size());
			for (const ResourceDependency& dependency : cooked.Dependencies)
			{
				if (!cookedFiles.contains(dependency.Path))
					pending.insert(dependency.Path);
				if (ownerWitness != discoveryWitnesses.end())
				{
					DiscoveryWitness candidate{
						ownerWitness->second.Distance + 1,
						ownerWitness->second.Text + " -> " + dependency.Path
					};
					const auto existing = discoveryWitnesses.find(dependency.Path);
					if (existing == discoveryWitnesses.end() ||
						std::tie(candidate.Distance, candidate.Text) <
							std::tie(existing->second.Distance, existing->second.Text))
					{
						discoveryWitnesses[dependency.Path] = std::move(candidate);
					}
				}
			}
			cookedFiles.emplace(cooked.LogicalPath, std::move(cooked));
		}
		ApplyWarmupTags(config, cookedFiles);
		ValidateCatalogLimits(cookedFiles);

		std::filesystem::create_directories(outputDirectory, error);
		if (error)
			Fail("Cannot create output directory: " + error.message());

		WritePak(pakPath, cookedFiles, config.CompressionLevel);
		VerifyPak(pakPath, cookedFiles);
		const std::uint64_t pakSize = static_cast<std::uint64_t>(std::filesystem::file_size(pakPath, error));
		if (error)
			Fail("Cannot read generated pak size: " + error.message());

		const std::uint64_t contentHash = ComputeContentHash(cookedFiles);
		const std::uint64_t registryHash = ComputeRegistryHash(cookedFiles);
		const Json manifest = BuildManifest(config, cookedFiles, contentHash, registryHash, pakSize);
		if (manifest.dump(2).size() + 1 > MaxManifestSize)
			Fail("Game.manifest.json exceeds the packaged runtime size limit");
		WriteJsonFile(manifestPath, manifest);
		WriteJsonFile(reportPath, BuildReport(
			config,
			projectIndex,
			cookedFiles,
			seeds,
			pakSize,
			contentHash,
			registryHash,
			runId));

		std::printf(
			"HCooker succeeded: %zu files, %zu bytes, content hash %s.\n",
			cookedFiles.size(),
			static_cast<std::size_t>(pakSize),
			Hex64(contentHash).c_str());
		return 0;
	}

	void MarkRunFailed(const CookOptions& options, const std::string& error) noexcept
	{
		try
		{
			const std::filesystem::path reportPath = options.ReportPath.empty()
				? std::filesystem::absolute(options.OutputDirectory) / "CookReport.json"
				: std::filesystem::absolute(options.ReportPath);
			std::error_code statusError;
			if (!std::filesystem::is_regular_file(reportPath, statusError) || statusError)
				return;
			Json report = ReadTextJson(reportPath);
			const std::string runId = options.RunId.empty() ? "manual" : options.RunId;
			if (!report.is_object() || !report.contains("RunId") || !report["RunId"].is_string() ||
				report["RunId"].get<std::string>() != runId ||
				!report.contains("Status") || !report["Status"].is_string() ||
				report["Status"].get<std::string>() != "Running" ||
				report.value("OutcomeScope", std::string{}) != "Cook")
			{
				return;
			}
			report["Succeeded"] = false;
			report["Status"] = "Failed";
			report["Error"] = error;
			WriteJsonFile(reportPath, report);
		}
		catch (...)
		{
		}
	}

	int RunSelfTests()
	{
		const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
		const std::filesystem::path testRoot = std::filesystem::temp_directory_path() /
			("HEngine-HCooker-SelfTests-" + std::to_string(timestamp));
		std::error_code error;
		if (!std::filesystem::create_directories(testRoot, error) || error)
			Fail("Cannot create HCooker self-test directory: " + error.message());

		const auto cleanup = [&]() noexcept
		{
			std::error_code cleanupError;
			std::filesystem::remove_all(testRoot, cleanupError);
		};
		const auto writeText = [](const std::filesystem::path& path, std::string_view text)
		{
			std::error_code directoryError;
			std::filesystem::create_directories(path.parent_path(), directoryError);
			if (directoryError)
				Fail("Cannot create HCooker self-test fixture directory: " + directoryError.message());
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
				Fail("Cannot create HCooker self-test fixture: " + path.generic_string());
			stream << text;
			if (!stream)
				Fail("Cannot finish HCooker self-test fixture: " + path.generic_string());
		};
		const auto writeProjectConfig = [&](const std::filesystem::path& projectRoot, std::string_view rootScript)
		{
			const Json config = {
				{ "Game", {
					{ "Name", "HCooker Python Import Contract" },
					{ "DefaultScene", "Assets/Main.scene" }
				} },
				{ "Cook", {
					{ "ContentVersion", "self-test" },
					{ "CompressionLevel", 1 },
					{ "CookedJsonExtensions", Json::array({ ".hmat" }) },
					{ "RootFiles", Json::array({ std::string(rootScript) }) }
				} }
			};
			writeText(projectRoot / "Config/Game.json", config.dump(2));
			writeText(
				projectRoot / "Config/DefaultEngine.json",
				R"({"Schema":"hengine.render-settings","SchemaVersion":2,"Rendering":{}})");
			writeText(
				projectRoot / "Config/BaseEngine.json",
				R"({"Schema":"hengine.render-settings","SchemaVersion":2})");
			writeText(projectRoot / "Assets/Main.scene", "scene\n");
		};

		try
		{
			const std::filesystem::path materialRoot = testRoot / "MaterialContract";
			writeText(
				materialRoot / "Shaders/Custom.hlsl",
				"#include \"Include/Common.hlsli\"\n// material shader\n");
			writeText(
				materialRoot / "Shaders/Include/Common.hlsli",
				"#include \"Math/Constants.hlsli\"\n");
			writeText(
				materialRoot / "Shaders/Math/Constants.hlsli",
				"static const float HE_PI = 3.14159265f;\n");
			writeText(materialRoot / "Assets/Textures/real.png", "real texture\n");
			writeText(materialRoot / "Assets/Textures/not-a-texture.png", "legacy texture\n");
			writeText(materialRoot / "Assets/Meshes/Shared.hmesh", "{}\n");
			writeText(materialRoot / "Assets/Materials/Override.hmat", "{}\n");

			MaterialShaderManifest expectedManifest;
			expectedManifest.SourcePath = "Custom.hlsl";
			expectedManifest.RecursiveSourceHash = std::string(64, 'a');
			expectedManifest.BindingSignatureHash =
				BuildMaterialShaderBindingSignatureHash(expectedManifest);
			for (const MaterialShaderVertexPath vertexPath : {
					MaterialShaderVertexPath::Static,
					MaterialShaderVertexPath::Skinned })
			{
				const std::string vertexToken = vertexPath == MaterialShaderVertexPath::Static
					? "static" : "skinned";
				const std::string permutationVertex = vertexPath == MaterialShaderVertexPath::Static
					? "CPU_STATIC" : "SKINNED";
				for (const MaterialShaderDescriptorMode descriptorMode : {
						MaterialShaderDescriptorMode::Baseline,
						MaterialShaderDescriptorMode::HeapDirect })
				{
					const std::string descriptorToken =
						descriptorMode == MaterialShaderDescriptorMode::Baseline
						? "baseline" : "heap-direct";
					const std::string permutationDescriptor =
						descriptorMode == MaterialShaderDescriptorMode::Baseline
						? "BASELINE" : "HEAP_DIRECT";
					for (const std::string_view stage : {
							std::string_view("vs"), std::string_view("ps") })
					{
						const std::string relativePath = "DXIL/Materials/program/Custom-" +
							vertexToken + "-" + descriptorToken + "." + std::string(stage);
						writeText(materialRoot / "Shaders/CACHES" / relativePath, "abc");
						expectedManifest.Artifacts.push_back(MaterialShaderArtifact{
							.Pass = MaterialShaderPass::Forward,
							.Stage = std::string(stage),
							.EntryPoint = stage == "vs" ? "HEMaterialVS" : "HEMaterialPS",
							.TargetProfile = stage == "vs" ||
								descriptorMode == MaterialShaderDescriptorMode::Baseline
								? std::string(stage) + "_6_0"
								: "ps_6_6",
							.Permutation = "FORWARD_" + permutationVertex + "_" +
								permutationDescriptor,
							.DescriptorMode = descriptorMode,
							.VertexPath = vertexPath,
							.RelativePath = relativePath,
							.ContentHash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
						});
					}
				}
			}
			expectedManifest.ArtifactSets.push_back(MaterialShaderArtifactSet{
				.Backend = RenderBackend::DX12,
				.BinaryFormat = ShaderBinaryFormat::Dxil,
				.Artifacts = expectedManifest.Artifacts,
			});
			MaterialShaderArtifactSet vulkanArtifacts{
				.Backend = RenderBackend::Vulkan,
				.BinaryFormat = ShaderBinaryFormat::SpirV,
			};
			for (const MaterialShaderArtifact& dxilArtifact : expectedManifest.Artifacts)
			{
				if (dxilArtifact.DescriptorMode != MaterialShaderDescriptorMode::Baseline)
					continue;
				MaterialShaderArtifact spirVArtifact = dxilArtifact;
				spirVArtifact.RelativePath.replace(0, 4, "SPIRV");
				writeText(
					materialRoot / "Shaders/CACHES" / spirVArtifact.RelativePath,
					"abc");
				vulkanArtifacts.Artifacts.push_back(std::move(spirVArtifact));
			}
			expectedManifest.ArtifactSets.push_back(std::move(vulkanArtifacts));
			const Json materialManifest = MaterialShaderManifestToJson(expectedManifest);
			writeText(
				materialRoot / "Shaders/CACHES/Materials/Custom.hlsl.material.json",
				materialManifest.dump(2));

			const ProjectIndex materialIndex = BuildProjectIndex(materialRoot, CookConfig{});
			const std::optional<std::string> shaderRootInclude = ResolveReference(
				"Math/Constants.hlsli",
				"Shaders/Include/Common.hlsli",
				materialIndex);
			if (!shaderRootInclude || *shaderRootInclude != "Shaders/Math/Constants.hlsli")
				Fail("Shader-root-relative include dependency was not resolved");

			const Json typedMaterial = {
				{ "SchemaVersion", 2 },
				{ "Type", "Custom" },
				{ "ShaderPath", "Shaders/Custom.hlsl" },
				{ "Parameters", {
					{ "label", {
						{ "Type", "string" },
						{ "Value", "Assets/Textures/not-a-texture.png" }
					} }
				} },
				{ "Textures", {
					{ "surface", {
						{ "Type", "texture2d" },
						{ "Path", "Assets/Textures/real.png" }
					} }
				} },
				{ "Properties", {
					{ "legacyTexture", {
						{ "textureSourcePath", "Assets/Textures/not-a-texture.png" }
					} }
				} }
			};
			DependencyMap materialDependencies;
			CollectStructuredJsonDependencies(
				typedMaterial,
				".hmat",
				"Assets/Test.hmat",
				materialIndex,
				materialDependencies);
			if (!materialDependencies.contains(FoldPath("Shaders/Custom.hlsl")) ||
				!materialDependencies.contains(FoldPath("Assets/Textures/real.png")) ||
				!materialDependencies.contains(FoldPath("Shaders/CACHES/Materials/Custom.hlsl.material.json")) ||
				materialDependencies.contains(FoldPath("Assets/Textures/not-a-texture.png")))
			{
				Fail("V2 material typed dependency collection is incorrect");
			}

			const Json materialOverrideScene = {
				{ "entity", {
					{ "MeshComponent", {
						{ "Path", "Assets/Meshes/Shared.hmesh" },
						{ "meshResPath", "Assets/Meshes/Shared.hmesh" },
						{ "MaterialOverrides", Json::array({
							"Assets/Materials/Override.hmat", "" }) },
					} },
				} },
			};
			DependencyMap sceneDependencies;
			CollectStructuredJsonDependencies(
				materialOverrideScene,
				".he",
				"Assets/MaterialOverride.he",
				materialIndex,
				sceneDependencies);
			if (!sceneDependencies.contains(FoldPath("Assets/Meshes/Shared.hmesh")) ||
				!sceneDependencies.contains(FoldPath("Assets/Materials/Override.hmat")))
			{
				Fail("Scene material override dependency collection is incorrect");
			}

			const Json legacyMaterial = {
				{ "ShaderPath", "Shaders/Custom.hlsl" },
				{ "Properties", {
					{ "legacyTexture", {
						{ "textureSourcePath", "Assets/Textures/not-a-texture.png" }
					} }
				} }
			};
			DependencyMap legacyDependencies;
			CollectStructuredJsonDependencies(
				legacyMaterial,
				".hmat",
				"Assets/Legacy.hmat",
				materialIndex,
				legacyDependencies);
			if (!legacyDependencies.contains(FoldPath("Assets/Textures/not-a-texture.png")))
				Fail("V1 material legacy textureSourcePath dependency was not recorded");

			DependencyMap manifestDependencies;
			CollectStructuredJsonDependencies(
				materialManifest,
				".json",
				"Shaders/CACHES/Materials/Custom.hlsl.material.json",
				materialIndex,
				manifestDependencies);
			const std::size_t expectedArtifactDependencyCount = std::accumulate(
				expectedManifest.ArtifactSets.begin(),
				expectedManifest.ArtifactSets.end(),
				std::size_t{ 0 },
				[](std::size_t count, const MaterialShaderArtifactSet& set)
				{
					return count + set.Artifacts.size();
				});
			if (manifestDependencies.size() != expectedArtifactDependencyCount ||
				!manifestDependencies.contains(FoldPath(
					"Shaders/CACHES/DXIL/Materials/program/Custom-static-baseline.ps")) ||
				!manifestDependencies.contains(FoldPath(
					"Shaders/CACHES/SPIRV/Materials/program/Custom-static-baseline.ps")))
				Fail("Material manifest dual-backend dependencies were not recorded");

			Json mismatchedManifest = materialManifest;
			mismatchedManifest["ArtifactSets"][0]["Artifacts"][0]["ContentHash"] =
				std::string(64, '0');
			bool bHashMismatchRejected = false;
			try
			{
				DependencyMap ignoredDependencies;
				CollectStructuredJsonDependencies(
					mismatchedManifest,
					".json",
					"Shaders/CACHES/Materials/Custom.hlsl.material.json",
					materialIndex,
					ignoredDependencies);
			}
			catch (const std::runtime_error& exception)
			{
				bHashMismatchRejected = std::string(exception.what()).find(
					"material shader artifact hash mismatch") != std::string::npos;
			}
			if (!bHashMismatchRejected)
				Fail("Material shader artifact hash mismatch was not rejected");

			Json malformedManifest = materialManifest;
			malformedManifest.erase("ParameterBlock");
			bool bMalformedManifestRejected = false;
			try
			{
				DependencyMap ignoredDependencies;
				CollectStructuredJsonDependencies(
					malformedManifest,
					".json",
					"Shaders/CACHES/Materials/Custom.hlsl.material.json",
					materialIndex,
					ignoredDependencies);
			}
			catch (const std::runtime_error& exception)
			{
				bMalformedManifestRejected = std::string(exception.what()).find(
					"Manifest.ParameterBlock") != std::string::npos;
			}
			if (!bMalformedManifestRejected)
				Fail("Malformed material shader manifest bypassed full validation");

			writeProjectConfig(materialRoot, "Assets/Test.hmat");
			writeText(materialRoot / "Assets/Test.hmat", typedMaterial.dump(2));
			CookOptions materialOptions;
			materialOptions.ProjectRoot = materialRoot;
			materialOptions.OutputDirectory = testRoot / "MaterialOutput";
			std::filesystem::create_directories(materialOptions.OutputDirectory);
			if (Run(materialOptions) != 0)
				Fail("Valid typed material dependency cook returned a non-zero result");

			const Json materialCookManifest = ReadTextJson(
				materialOptions.OutputDirectory / "Game.manifest.json");
			const auto materialCookContains = [&](std::string_view path)
			{
				return std::any_of(
					materialCookManifest.at("Files").begin(),
					materialCookManifest.at("Files").end(),
					[path](const Json& file)
					{
						return file.at("Path").get<std::string>() == path;
					});
			};
			if (!materialCookContains("Assets/Textures/real.png") ||
				materialCookContains("Assets/Textures/not-a-texture.png") ||
				!materialCookContains("Shaders/CACHES/Materials/Custom.hlsl.material.json"))
			{
				Fail("Typed material cook did not preserve its dependency boundary");
			}
			for (const MaterialShaderArtifactSet& set : expectedManifest.ArtifactSets)
			{
				for (const MaterialShaderArtifact& artifact : set.Artifacts)
				{
					if (!materialCookContains("Shaders/CACHES/" + artifact.RelativePath))
						Fail("Typed material cook omitted a validated shader artifact: " + artifact.RelativePath);
				}
			}

			const std::filesystem::path validProject = testRoot / "ValidProject";
			writeProjectConfig(validProject, "Scripts/entry.py");
			writeText(validProject / "Scripts/entry.py",
				"import FirstModule, SecondModule as second_alias\n"
				"from FirstModule import VALUE\n"
				"from TestPackage import imported_submodule, SYMBOL\n"
				"from TestPackage import (\n"
				"    continued_submodule,\n"
				"    EXPORTED_VALUE,\n"
				")\n"
				"from TestPackage import \\\n"
				"    backslash_submodule\n"
				"from TestPackage.absolute import VALUE\n");
			writeText(validProject / "Scripts/FirstModule.py", "VALUE = 1\n");
			writeText(validProject / "Scripts/SecondModule.py", "VALUE = 2\n");
			writeText(validProject / "Scripts/TestPackage/__init__.py",
				"SYMBOL = 3\nEXPORTED_VALUE = 4\n");
			writeText(validProject / "Scripts/TestPackage/absolute.py",
				"from .relative import VALUE\n"
				"from . import relative_layout\n");
			writeText(validProject / "Scripts/TestPackage/relative.py", "VALUE = 7\n");
			writeText(validProject / "Scripts/TestPackage/relative_layout.py", "VALUE = 8\n");
			writeText(validProject / "Scripts/TestPackage/imported_submodule.py", "VALUE = 9\n");
			writeText(validProject / "Scripts/TestPackage/continued_submodule.py", "VALUE = 10\n");
			writeText(validProject / "Scripts/TestPackage/backslash_submodule.py", "VALUE = 11\n");

			const std::filesystem::path validOutput = testRoot / "ValidOutput";
			std::filesystem::create_directories(validOutput);
			CookOptions validOptions;
			validOptions.ProjectRoot = validProject;
			validOptions.OutputDirectory = validOutput;
			if (Run(validOptions) != 0)
				Fail("Valid Python import closure cook returned a non-zero result");

			const Json manifest = ReadTextJson(validOutput / "Game.manifest.json");
			const auto findFile = [&](std::string_view path) -> const Json*
			{
				for (const Json& file : manifest.at("Files"))
				{
					if (file.at("Path").get<std::string>() == path)
						return &file;
				}
				return nullptr;
			};
			const auto hasDependency = [](const Json& file, std::string_view path)
			{
				return std::any_of(
					file.at("Dependencies").begin(),
					file.at("Dependencies").end(),
					[path](const Json& dependency)
					{
						return dependency.at("Path").get<std::string>() == path;
					});
			};

			const Json* entry = findFile("Scripts/entry.py");
			const Json* absolute = findFile("Scripts/TestPackage/absolute.py");
			if (entry == nullptr || absolute == nullptr ||
				findFile("Scripts/FirstModule.py") == nullptr ||
				findFile("Scripts/SecondModule.py") == nullptr ||
				findFile("Scripts/TestPackage/__init__.py") == nullptr ||
				findFile("Scripts/TestPackage/relative.py") == nullptr ||
				findFile("Scripts/TestPackage/relative_layout.py") == nullptr ||
				findFile("Scripts/TestPackage/imported_submodule.py") == nullptr ||
				findFile("Scripts/TestPackage/continued_submodule.py") == nullptr ||
				findFile("Scripts/TestPackage/backslash_submodule.py") == nullptr)
			{
				Fail("Python import closure is incomplete");
			}
			if (!hasDependency(*entry, "Scripts/FirstModule.py") ||
				!hasDependency(*entry, "Scripts/SecondModule.py"))
			{
				Fail("A comma-separated Python import was not recorded as a dependency");
			}
			if (!hasDependency(*entry, "Scripts/TestPackage/absolute.py"))
				Fail("Scripts-root absolute Python import was not recorded as a dependency");
			if (!hasDependency(*entry, "Scripts/TestPackage/imported_submodule.py"))
				Fail("A from-import submodule was not recorded as a dependency");
			if (!hasDependency(*entry, "Scripts/TestPackage/continued_submodule.py"))
				Fail("A parenthesized from-import submodule was not recorded as a dependency");
			if (!hasDependency(*entry, "Scripts/TestPackage/backslash_submodule.py"))
				Fail("A backslash-continued from-import submodule was not recorded as a dependency");
			if (!hasDependency(*absolute, "Scripts/TestPackage/relative.py"))
				Fail("A leading-dot relative module import was not recorded as a dependency");
			if (!hasDependency(*absolute, "Scripts/TestPackage/relative_layout.py"))
				Fail("A 'from . import module' dependency was not recorded");

			const std::filesystem::path invalidProject = testRoot / "InvalidProject";
			writeProjectConfig(invalidProject, "Scripts/TestPackage/escape.py");
			writeText(invalidProject / "Scripts/TestPackage/escape.py",
				"from ...outside import VALUE\n");
			writeText(invalidProject / "outside.py", "VALUE = 9\n");
			CookOptions invalidOptions;
			invalidOptions.ProjectRoot = invalidProject;
			invalidOptions.OutputDirectory = testRoot / "InvalidOutput";
			std::filesystem::create_directories(invalidOptions.OutputDirectory);
			bool bEscapeRejected = false;
			try
			{
				(void)Run(invalidOptions);
			}
			catch (const std::runtime_error& exception)
			{
				bEscapeRejected = std::string(exception.what()).find(
					"relative Python import escapes the Scripts root") != std::string::npos;
			}
			if (!bEscapeRejected)
				Fail("A relative Python import escaped the Scripts root");

			const std::filesystem::path graphRoot = testRoot / "DependencyGraphContract";
			writeText(graphRoot / "Config/DefaultEngine.json",
				R"({"Schema":"hengine.render-settings","SchemaVersion":2,"Rendering":{}})");
			writeText(graphRoot / "Config/BaseEngine.json",
				R"({"Schema":"hengine.render-settings","SchemaVersion":2})");
			writeText(graphRoot / "Assets/Icon.png", "icon\n");
			writeText(graphRoot / "Assets/Mesh.hmesh", "{}\n");
			writeText(graphRoot / "Assets/Skeleton.hskel", "{}\n");
			writeText(graphRoot / "Assets/Background.png", "background\n");
			writeText(graphRoot / "Assets/Explicit.png", "explicit\n");
			writeText(graphRoot / "Assets/Fallback/Shared.bin", "shared\n");
			writeText(graphRoot / "Assets/Hud.rml", "<rml><head><link href=\"Hud.rcss\" /></head></rml>\n");
			writeText(
				graphRoot / "Assets/Hud.rcss",
				"body { background-image: url('Background.png'); }\n");
			writeText(graphRoot / "Assets/Animation.hanim", Json{
				{ "version", 1 },
				{ "name", "SelfTest" },
				{ "meshResPath", "Assets/Mesh.hmesh" },
				{ "targetSkeletonPath", "Assets/Skeleton.hskel" },
				{ "boneInfos", Json::array() },
				{ "rootNode", Json::object() },
				{ "clips", Json::array() }
			}.dump(2));
			writeText(graphRoot / "Assets/Hud.hui", Json{
				{ "Version", 1 },
				{ "Name", "SelfTest" },
				{ "GeneratedRmlPath", "Assets/Hud.rml" },
				{ "GeneratedRcssPath", "Assets/Hud.rcss" },
				{ "Nodes", Json::array({ Json{
					{ "ImagePath", "" },
					{ "Style", {
						{ "FontPath", "None" },
						{ "BackgroundImage", "Assets/Background.png" }
					} }
				} }) }
			}.dump(2));
			writeText(graphRoot / "Assets/Main.he", Json{
				{ "first", { { "SpriteRendererComponent", { { "Texture", "None" } } } } },
				{ "second", { { "SpriteRendererComponent", { { "Texture", "Assets/Virtual.png" } } } } }
			}.dump(2));
			const Json runtimeAssetSet = {
				{ "Schema", "hengine.runtime-asset-set" },
				{ "SchemaVersion", 1 },
				{ "Assets", Json::array({
					Json{ { "Key", "asset/explicit" }, { "Path", "Assets/Explicit.png" },
						{ "Kind", "HardRuntime" }, { "Reason", "explicit self-test asset" } },
					Json{ { "Key", "asset/shared-a" }, { "Path", "Assets/Fallback/Shared.bin" },
						{ "Kind", "PackagingOnly" }, { "Reason", "first shared owner" } },
					Json{ { "Key", "asset/shared-b" }, { "Path", "Assets/Fallback/Shared.bin" },
						{ "Kind", "SoftRuntime" }, { "Reason", "second shared owner" } }
				}) }
			};
			writeText(graphRoot / "Config/RuntimeAssets.json", runtimeAssetSet.dump(2));
			const Json graphConfig = {
				{ "Game", {
					{ "Name", "HCooker Dependency Graph Contract" },
					{ "DefaultScene", "Assets/Main.he" },
					{ "Icon", "Assets/Icon.png" }
				} },
				{ "Cook", {
					{ "ContentVersion", "self-test" },
					{ "CompressionLevel", 1 },
					{ "CookedJsonExtensions", Json::array({ ".he", ".hanim", ".hui" }) },
					{ "RootFiles", Json::array({ "Assets/Animation.hanim", "Assets/Hud.hui" }) },
					{ "IncludeDirectories", Json::array({ "Assets/Fallback" }) },
					{ "VirtualFiles", Json::array({ "Assets/Virtual.png" }) },
					{ "DynamicAssetSets", Json::array({ Json{
						{ "Name", "SelfTest" },
						{ "Manifest", "Config/RuntimeAssets.json" }
					} }) }
				} }
			};
			writeText(graphRoot / "Config/Game.json", graphConfig.dump(2));

			const ProjectIndex graphIndex = BuildProjectIndex(graphRoot, ParseCookConfig(graphConfig));
			bool bMissingStrictTargetRejected = false;
			try
			{
				DependencyMap ignoredDependencies;
				DependencyDiagnostics strictDiagnostics;
				strictDiagnostics.Witness = "[ProjectRoot:self-test] Assets/Main.he";
				StrictReferenceOptions strictOptions;
				strictOptions.bAllowEmpty = false;
				strictOptions.bAllowNone = false;
				ResolveAndAddStrictDependency(
					"Assets/Missing.never-seen", "Assets/Main.he", graphIndex,
					ignoredDependencies, DependencyKind::HardRuntime, DependencySource::Structured,
					"SelfTestProvider", "SelfTestSchema", "/Missing", "strict missing target",
					strictOptions, &strictDiagnostics);
			}
			catch (const std::runtime_error& exception)
			{
				const std::string message = exception.what();
				bMissingStrictTargetRejected = message.find("Assets/Missing.never-seen") != std::string::npos &&
					message.find("SelfTestProvider") != std::string::npos &&
					message.find("/Missing") != std::string::npos &&
					message.find("HardRuntime") != std::string::npos &&
					message.find("witness=") != std::string::npos;
			}
			if (!bMissingStrictTargetRejected)
				Fail("Strict structured dependency did not fail closed with explanatory context");

			CookOptions graphOptions;
			graphOptions.ProjectRoot = graphRoot;
			graphOptions.OutputDirectory = testRoot / "DependencyGraphOutput";
			graphOptions.RunId = "graph-contract";
			if (Run(graphOptions) != 0)
				Fail("Dependency graph contract cook returned a non-zero result");
			const Json graphReport = ReadTextJson(graphOptions.OutputDirectory / "CookReport.json");
			if (graphReport.value("ReportVersion", 0) != 2 || !graphReport.value("Succeeded", false) ||
				graphReport.value("RunId", std::string{}) != graphOptions.RunId ||
				graphReport.value("Status", std::string{}) != "Succeeded" ||
				graphReport["VirtualDependencies"].empty() || graphReport["Edges"].empty() ||
				graphReport["RootContributions"].empty() || graphReport["IncludeDirectoryContributions"].empty())
			{
				Fail("Dependency graph ReportVersion 2 contract is incomplete");
			}
			const auto findReportFile = [&](std::string_view path) -> const Json*
			{
				for (const Json& file : graphReport["Files"])
				{
					if (file.value("Path", std::string{}) == path)
						return &file;
				}
				return nullptr;
			};
			const Json* meshAnalysis = findReportFile("Assets/Mesh.hmesh");
			const Json* backgroundAnalysis = findReportFile("Assets/Background.png");
			const Json* sharedAnalysis = findReportFile("Assets/Fallback/Shared.bin");
			if (!meshAnalysis || !backgroundAnalysis || !sharedAnalysis ||
				meshAnalysis->at("RootWitness").at("Chain").size() < 2 ||
				backgroundAnalysis->at("DirectReferrers").empty() ||
				sharedAnalysis->at("DirectReferrers").size() < 2 ||
				sharedAnalysis->value("OnlyIncludedByDirectory", true))
			{
				Fail("Dependency graph witness, reverse edge, or overlap analysis is incomplete");
			}

			const std::filesystem::path failureRoot = testRoot / "FailureReportContract";
			writeProjectConfig(failureRoot, "Assets/MissingRoot.never-seen");
			const std::filesystem::path failureOutput = testRoot / "FailureReportOutput";
			std::filesystem::create_directories(failureOutput);
			writeText(failureOutput / "CookReport.json",
				R"({"ReportVersion":1,"Succeeded":true,"RunId":"old-run","Status":"Succeeded"})");
			CookOptions failureOptions;
			failureOptions.ProjectRoot = failureRoot;
			failureOptions.OutputDirectory = failureOutput;
			failureOptions.RunId = "fresh-failure";
			bool bFailureObserved = false;
			try
			{
				(void)Run(failureOptions);
			}
			catch (const std::runtime_error& exception)
			{
				bFailureObserved = true;
				MarkRunFailed(failureOptions, exception.what());
			}
			const Json failedReport = ReadTextJson(failureOutput / "CookReport.json");
			if (!bFailureObserved || failedReport.value("RunId", std::string{}) != "fresh-failure" ||
				failedReport.value("Status", std::string{}) != "Failed" ||
				failedReport.value("Succeeded", true) || !failedReport.contains("Error"))
			{
				Fail("A failed cook did not replace a stale success report for the same run");
			}
		}
		catch (...)
		{
			cleanup();
			throw;
		}

		cleanup();
		std::printf("HCooker dependency graph/report self-test PASS\n");
		return 0;
	}
}
