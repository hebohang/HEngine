#include "Hepch.h"

#include "PackagedProject.h"
#include "ProjectIdentity.h"

#include "Runtime/Platform/PlatformPaths.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <HEngine/ProjectBuildConfig.h>
#if HE_PORTABLE_PLAYER
#include <HEngine/PlayerBuildConfig.h>
#endif

#if defined(HE_PACKAGED_GAME)
#include <nlohmann/json.hpp>
#include <unzip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#else
#include <stdexcept>
#endif

namespace HEngine
{
#if defined(HE_PACKAGED_GAME)
    namespace
    {
        constexpr std::uint64_t Fnv1aOffsetBasis = 14695981039346656037ull;
        constexpr std::uint64_t Fnv1aPrime = 1099511628211ull;
        constexpr std::uint64_t MaxManifestSize = 32ull * 1024ull * 1024ull;
        constexpr std::uint64_t MaxEntrySize = 2ull * 1024ull * 1024ull * 1024ull;
        constexpr std::uint64_t MaxContentSize = 16ull * 1024ull * 1024ull * 1024ull;
        constexpr std::uint64_t MaxFileCount = RuntimeResourceCatalogLimits::MaxEntries;
        constexpr std::uint64_t MaxResourceEstimate = RuntimeResourceCatalogLimits::MaxResourceEstimate;
        constexpr std::uint64_t MaxCatalogEstimate = RuntimeResourceCatalogLimits::MaxCatalogEstimate;
        constexpr std::uint64_t MaxCatalogDependencies = RuntimeResourceCatalogLimits::MaxDependencies;
        constexpr std::uint64_t MaxCatalogTags = RuntimeResourceCatalogLimits::MaxTags;
        constexpr std::size_t MaxDependenciesPerFile =
            RuntimeResourceCatalogLimits::MaxDependenciesPerEntry;
        constexpr std::size_t MaxTagsPerFile = RuntimeResourceCatalogLimits::MaxTagsPerEntry;
        constexpr std::size_t MaxTagLength = RuntimeResourceCatalogLimits::MaxTagLength;
        constexpr std::array<const char*, 6> CatalogFileFields = {
            "Type",
            "CookFormatVersion",
            "Dependencies",
            "Tags",
            "CpuBytes",
            "GpuBytes"
        };

        class Fnv1a64 final
        {
        public:
            void Update(const void* data, std::size_t size)
            {
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                for (std::size_t index = 0; index < size; ++index)
                {
                    mValue ^= bytes[index];
                    mValue *= Fnv1aPrime;
                }
            }

            void Update(std::string_view value)
            {
                Update(value.data(), value.size());
            }

            void UpdateUint64LittleEndian(std::uint64_t value)
            {
                std::uint8_t bytes[sizeof(value)]{};
                for (std::size_t index = 0; index < sizeof(value); ++index)
                    bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xffu);
                Update(bytes, sizeof(bytes));
            }

            [[nodiscard]] std::string Hex() const
            {
                std::ostringstream stream;
                stream << std::hex << std::setfill('0') << std::setw(16) << std::nouppercase << mValue;
                return stream.str();
            }

        private:
            std::uint64_t mValue = Fnv1aOffsetBasis;
        };

        struct SafeArchivePath
        {
            std::string key;
            std::filesystem::path relativePath;
        };

        struct ManifestFile
        {
            SafeArchivePath path;
            std::uint64_t storedSize = 0;
            std::string hash;
        };

        struct PackageManifest
        {
            ProjectIdentity project{
                .ProjectId = HE_PROJECT_ID,
                .DisplayName = HE_PROJECT_DISPLAY_NAME,
                .ExecutableName = HE_PROJECT_EXECUTABLE_NAME,
                .GameConfig = HE_PROJECT_GAME_CONFIG,
                .LegacySavedDirectory = HE_PROJECT_LEGACY_SAVED_DIRECTORY,
            };
            std::filesystem::path pakPath;
            std::uint64_t pakSize = 0;
            std::string contentVersion;
            std::string contentHash;
            std::string registryHash;
            std::string marker;
            std::vector<ManifestFile> files;
            std::unordered_map<std::string, std::size_t> fileIndices;
            std::shared_ptr<const RuntimeResourceCatalogSnapshot> resourceCatalog =
                RuntimeResourceCatalogSnapshot::Empty();
        };

        static std::string ToLowerAscii(std::string value)
        {
            for (char& character : value)
            {
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            }
            return value;
        }

        static bool IsBytewiseLess(std::string_view lhs, std::string_view rhs)
        {
            return std::lexicographical_compare(
                lhs.begin(),
                lhs.end(),
                rhs.begin(),
                rhs.end(),
                [](char lhsCharacter, char rhsCharacter)
                {
                    return static_cast<unsigned char>(lhsCharacter) < static_cast<unsigned char>(rhsCharacter);
                });
        }

        static SafeArchivePath ValidateArchivePath(const std::string& rawPath)
        {
            if (rawPath.empty() || rawPath.size() > RuntimeResourceCatalogLimits::MaxPathLength)
                throw std::runtime_error("The package contains an invalid empty or oversized path.");
            if (rawPath.find('\0') != std::string::npos)
                throw std::runtime_error("The package contains a path with an embedded null byte.");
            if (rawPath.front() == '/' || rawPath.front() == '\\')
                throw std::runtime_error("The package contains an absolute path: " + rawPath);
            if (rawPath.size() >= 2 && std::isalpha(static_cast<unsigned char>(rawPath[0])) && rawPath[1] == ':')
                throw std::runtime_error("The package contains a drive-qualified path: " + rawPath);
            if (rawPath.find('\\') != std::string::npos)
                throw std::runtime_error("The package contains a non-normalized path: " + rawPath);
            if (rawPath.back() == '/')
                throw std::runtime_error("The package contains an unexpected directory entry: " + rawPath);

            SafeArchivePath result;
            std::size_t start = 0;
            while (start <= rawPath.size())
            {
                const std::size_t slash = rawPath.find('/', start);
                const std::size_t end = slash == std::string::npos ? rawPath.size() : slash;
                const std::string component = rawPath.substr(start, end - start);
                if (component.empty() || component == "." || component == "..")
                    throw std::runtime_error("The package contains a traversing or non-normalized path: " + rawPath);
                if (component.size() > 255 || component.back() == '.' || component.back() == ' ')
                    throw std::runtime_error("The package contains an invalid Windows path component: " + rawPath);
                if (component.find_first_of("<>:\"|?*") != std::string::npos ||
                    std::any_of(component.begin(), component.end(), [](unsigned char character) { return character < 32; }))
                {
                    throw std::runtime_error("The package contains an invalid Windows path component: " + rawPath);
                }
                if (Utils::IsWindowsReservedPathComponent(component))
                    throw std::runtime_error("The package contains a reserved Windows path component: " + rawPath);

                if (!result.key.empty())
                    result.key.push_back('/');
                result.key += component;
                result.relativePath /= std::filesystem::u8path(component);

                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }

            if (result.relativePath.empty() || result.relativePath.is_absolute() || result.relativePath.has_root_name())
                throw std::runtime_error("The package contains an unsafe path: " + rawPath);
            if (ToLowerAscii(result.key) == ".package-marker")
                throw std::runtime_error("The package contains the reserved path .package-marker.");
            return result;
        }

        static std::string NormalizeContentVersion(std::string value)
        {
            if (value.empty() || value.size() > 64 ||
                !std::all_of(value.begin(), value.end(), [](unsigned char character)
                {
                    return (character >= 'a' && character <= 'z') ||
                        (character >= 'A' && character <= 'Z') ||
                        (character >= '0' && character <= '9') ||
                        character == '.' || character == '_' || character == '-';
                }))
            {
                throw std::runtime_error("Invalid ContentVersion in Game.manifest.json.");
            }
            return ToLowerAscii(std::move(value));
        }

        static std::string NormalizeHash(const std::string& value, const char* fieldName)
        {
            if (value.size() != 16 || !std::all_of(value.begin(), value.end(), [](unsigned char character)
                {
                    return std::isxdigit(character) != 0;
                }))
            {
                throw std::runtime_error(std::string("Invalid ") + fieldName + " in Game.manifest.json.");
            }
            return ToLowerAscii(value);
        }

        static std::uint64_t ReadBoundedUnsigned(
            const nlohmann::json& value,
            std::uint64_t maximum,
            const std::string& context)
        {
            if (!value.is_number_unsigned())
                throw std::runtime_error(context + " must be an unsigned integer.");
            const std::uint64_t result = value.get<std::uint64_t>();
            if (result > maximum)
                throw std::runtime_error(context + " exceeds the packaged runtime limit.");
            return result;
        }

        static bool HasCatalogSchema(const nlohmann::json& manifestJson)
        {
            if (!manifestJson.contains("ResourceCatalogVersion"))
            {
                if (manifestJson.contains("RegistryHash") ||
                    manifestJson.contains("RegistryHashAlgorithm"))
                {
                    throw std::runtime_error(
                        "Game.manifest.json resource catalog is missing ResourceCatalogVersion.");
                }
                return false;
            }

            const nlohmann::json& version = manifestJson.at("ResourceCatalogVersion");
            if (!version.is_number_unsigned() ||
                version.get<std::uint64_t>() != RuntimeResourceCatalogLimits::SchemaVersion)
            {
                throw std::runtime_error("Unsupported Game.manifest.json resource catalog version.");
            }
            return true;
        }

        static RuntimeResourceCatalogEntry ParseCatalogEntry(
            const nlohmann::json& fileJson,
            const std::string& path,
            std::uint64_t& totalDependencies,
            std::uint64_t& totalTags,
            std::uint64_t& totalCpuBytes,
            std::uint64_t& totalGpuBytes)
        {
            for (const char* field : CatalogFileFields)
            {
                if (!fileJson.contains(field))
                    throw std::runtime_error("Game.manifest.json has an incomplete catalog entry: " + path);
            }

            RuntimeResourceCatalogEntry entry;
            entry.Path = path;

            if (!fileJson.at("Type").is_string() ||
                !TryParseRuntimeCatalogResourceType(
                    fileJson.at("Type").get_ref<const std::string&>(),
                    entry.Type))
            {
                throw std::runtime_error("Game.manifest.json has an invalid resource Type: " + path);
            }

            entry.CookFormatVersion = static_cast<std::uint32_t>(ReadBoundedUnsigned(
                fileJson.at("CookFormatVersion"),
                std::numeric_limits<std::uint32_t>::max(),
                "CookFormatVersion for " + path));

            const nlohmann::json& dependencies = fileJson.at("Dependencies");
            if (!dependencies.is_array() || dependencies.size() > MaxDependenciesPerFile ||
                totalDependencies > MaxCatalogDependencies - dependencies.size())
            {
                throw std::runtime_error("Game.manifest.json has too many catalog dependencies: " + path);
            }
            totalDependencies += static_cast<std::uint64_t>(dependencies.size());
            entry.Dependencies.reserve(dependencies.size());
            std::unordered_set<std::string> dependencyKeys;
            std::string previousDependencyKey;
            for (const nlohmann::json& dependencyJson : dependencies)
            {
                if (!dependencyJson.is_object() || dependencyJson.size() != 3 ||
                    !dependencyJson.contains("Path") || !dependencyJson.contains("Kind") ||
                    !dependencyJson.contains("Source") || !dependencyJson.at("Path").is_string() ||
                    !dependencyJson.at("Kind").is_string() || !dependencyJson.at("Source").is_string())
                {
                    throw std::runtime_error("Game.manifest.json has an invalid catalog dependency: " + path);
                }

                RuntimeCatalogDependency dependency;
                dependency.Path = ValidateArchivePath(
                    dependencyJson.at("Path").get_ref<const std::string&>()).key;
                dependency.CacheKey = ToLowerAscii(dependency.Path);
                if (!dependencyKeys.emplace(dependency.CacheKey).second)
                    throw std::runtime_error("Game.manifest.json has a duplicate catalog dependency: " + path);
                if (!previousDependencyKey.empty() && !(previousDependencyKey < dependency.CacheKey))
                    throw std::runtime_error("Game.manifest.json catalog dependencies are not in canonical order: " + path);
                previousDependencyKey = dependency.CacheKey;

                if (!TryParseRuntimeCatalogDependencyKind(
                        dependencyJson.at("Kind").get_ref<const std::string&>(),
                        dependency.Kind) ||
                    !TryParseRuntimeCatalogDependencySource(
                        dependencyJson.at("Source").get_ref<const std::string&>(),
                        dependency.Source))
                {
                    throw std::runtime_error("Game.manifest.json has an invalid catalog dependency enum: " + path);
                }
                if (dependency.Source == RuntimeCatalogDependencySource::Heuristic &&
                    dependency.Kind != RuntimeCatalogDependencyKind::PackagingOnly)
                {
                    throw std::runtime_error("Heuristic catalog dependencies must be packaging-only: " + path);
                }
                entry.Dependencies.push_back(std::move(dependency));
            }

            const nlohmann::json& tags = fileJson.at("Tags");
            if (!tags.is_array() || tags.size() > MaxTagsPerFile ||
                totalTags > MaxCatalogTags - tags.size())
            {
                throw std::runtime_error("Game.manifest.json has too many catalog tags: " + path);
            }
            totalTags += static_cast<std::uint64_t>(tags.size());
            entry.Tags.reserve(tags.size());
            std::string previousTag;
            for (const nlohmann::json& tagJson : tags)
            {
                if (!tagJson.is_string())
                    throw std::runtime_error("Game.manifest.json contains a non-string catalog tag: " + path);
                const std::string tag = tagJson.get<std::string>();
                if (tag.empty() || tag.size() > MaxTagLength ||
                    std::any_of(tag.begin(), tag.end(), [](unsigned char character) { return character < 32; }))
                {
                    throw std::runtime_error("Game.manifest.json contains an invalid catalog tag: " + path);
                }
                if (!previousTag.empty() && !(previousTag < tag))
                    throw std::runtime_error("Game.manifest.json catalog tags are duplicated or not in canonical order: " + path);
                previousTag = tag;
                entry.Tags.push_back(tag);
            }

            entry.CpuBytes = ReadBoundedUnsigned(
                fileJson.at("CpuBytes"), MaxResourceEstimate, "CpuBytes for " + path);
            entry.GpuBytes = ReadBoundedUnsigned(
                fileJson.at("GpuBytes"), MaxResourceEstimate, "GpuBytes for " + path);
            if (totalCpuBytes > MaxCatalogEstimate - entry.CpuBytes ||
                totalGpuBytes > MaxCatalogEstimate - entry.GpuBytes)
            {
                throw std::runtime_error("Game.manifest.json resource estimates exceed the catalog limit.");
            }
            totalCpuBytes += entry.CpuBytes;
            totalGpuBytes += entry.GpuBytes;
            return entry;
        }

        static std::string ReadTextFile(const std::filesystem::path& path, std::uint64_t maximumSize)
        {
            std::error_code error;
            const std::uint64_t size = std::filesystem::file_size(path, error);
            if (error)
                throw std::runtime_error("Could not read required file: " + Utils::PathToUtf8(path));
            if (size > maximumSize)
                throw std::runtime_error("Required file is unexpectedly large: " + Utils::PathToUtf8(path));

            std::ifstream stream(PlatformPaths::ToFileIoPath(path), std::ios::binary);
            if (!stream)
                throw std::runtime_error("Could not open required file: " + Utils::PathToUtf8(path));
            std::string content(static_cast<std::size_t>(size), '\0');
            if (size != 0 && !stream.read(content.data(), static_cast<std::streamsize>(size)))
                throw std::runtime_error("Could not completely read required file: " + Utils::PathToUtf8(path));
            return content;
        }

        static void WriteTextFile(const std::filesystem::path& path, std::string_view content)
        {
            std::ofstream stream(PlatformPaths::ToFileIoPath(path), std::ios::binary | std::ios::trunc);
            if (!stream)
                throw std::runtime_error("Could not create package marker: " + Utils::PathToUtf8(path));
            stream.write(content.data(), static_cast<std::streamsize>(content.size()));
            stream.flush();
            if (!stream)
                throw std::runtime_error("Could not write package marker: " + Utils::PathToUtf8(path));
        }

        static void EnsureDirectory(const std::filesystem::path& path)
        {
            std::error_code error;
            const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
            if (!error && std::filesystem::exists(status))
            {
                if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
                    throw std::runtime_error("A required game data directory is unsafe: " + Utils::PathToUtf8(path));
                return;
            }

            error.clear();
            if (!std::filesystem::create_directories(path, error) && error)
                throw std::runtime_error("Could not create game data directory: " + Utils::PathToUtf8(path));

            error.clear();
            const std::filesystem::file_status createdStatus = std::filesystem::symlink_status(path, error);
            if (error || std::filesystem::is_symlink(createdStatus) || !std::filesystem::is_directory(createdStatus))
                throw std::runtime_error("A required game data directory is unsafe: " + Utils::PathToUtf8(path));
        }

        static bool IsExistingDirectory(const std::filesystem::path& path)
        {
            std::error_code error;
            const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
            if (error || !std::filesystem::exists(status))
                return false;
            if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
                throw std::runtime_error("A game data path is unsafe: " + Utils::PathToUtf8(path));
            return true;
        }

        static bool IsPackageCheckId(std::string_view value)
        {
            if (value.size() != 32)
                return false;
            return std::all_of(value.begin(), value.end(), [](const char character)
            {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
            });
        }

        static void CreateFreshPackageCheckRoot(const std::filesystem::path& path)
        {
            std::error_code error;
            const bool bCreated = std::filesystem::create_directory(path, error);
            if (error)
            {
                throw std::runtime_error(
                    "Could not create the isolated package check root: " + Utils::PathToUtf8(path));
            }
            if (!bCreated)
            {
                throw std::runtime_error(
                    "The isolated package check root already exists: " + Utils::PathToUtf8(path));
            }

            error.clear();
            const std::filesystem::file_status status =
                std::filesystem::symlink_status(path, error);
            if (error || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_directory(status))
            {
                throw std::runtime_error(
                    "The isolated package check root is unsafe: " + Utils::PathToUtf8(path));
            }
        }

        static void RemoveScopedDirectory(
            const std::filesystem::path& path,
            const std::filesystem::path& expectedParent)
        {
            if (path.parent_path() != expectedParent)
                throw std::runtime_error("Refusing to remove an out-of-scope game data path.");
            if (!IsExistingDirectory(path))
                return;

            std::error_code error;
            std::filesystem::remove_all(path, error);
            if (error)
                throw std::runtime_error("Could not clean stale game data: " + Utils::PathToUtf8(path));
        }

        static void QuarantineInvalidContent(
            const std::filesystem::path& contentRoot,
            const std::filesystem::path& productRoot,
            const std::string& expectedDirectoryName)
        {
            if (contentRoot.parent_path() != productRoot ||
                contentRoot.filename() != std::filesystem::u8path(expectedDirectoryName))
            {
                throw std::runtime_error("Refusing to quarantine an out-of-scope game content path.");
            }
            if (!IsExistingDirectory(contentRoot))
                return;

            const std::filesystem::path quarantineRoot = productRoot / ".Content.quarantine";
            EnsureDirectory(quarantineRoot);

            const auto nonce = static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            for (std::uint32_t attempt = 0; attempt < 100; ++attempt)
            {
                std::ostringstream suffix;
                suffix << std::hex << nonce << '-' << attempt;
                const std::filesystem::path quarantinePath = quarantineRoot /
                    (expectedDirectoryName + ".invalid-" + suffix.str());

                std::error_code error;
                if (std::filesystem::exists(quarantinePath, error))
                    continue;
                if (error)
                    throw std::runtime_error("Could not inspect the game content quarantine path.");

                std::filesystem::rename(contentRoot, quarantinePath, error);
                if (error)
                    throw std::runtime_error("Could not quarantine invalid packaged game content.");
                return;
            }

            throw std::runtime_error("Could not allocate a unique game content quarantine path.");
        }

        class PackageLock final
        {
        public:
            explicit PackageLock(std::filesystem::path path)
                : mPath(std::move(path))
            {
            }

            PackageLock(const PackageLock&) = delete;
            PackageLock& operator=(const PackageLock&) = delete;

            ~PackageLock()
            {
                std::error_code error;
                std::filesystem::remove(mPath, error);
            }

        private:
            std::filesystem::path mPath;
        };

        static PackageLock AcquirePackageLock(
            const std::filesystem::path& lockPath,
            const std::filesystem::path& productRoot)
        {
            if (lockPath.parent_path() != productRoot)
                throw std::runtime_error("Invalid package lock location.");

            std::error_code error;
            if (std::filesystem::create_directory(lockPath, error))
                return PackageLock(lockPath);

            if (error)
                throw std::runtime_error("Could not create the game content preparation lock.");

            const std::filesystem::file_status status = std::filesystem::symlink_status(lockPath, error);
            if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
                throw std::runtime_error("The game content preparation lock is unsafe.");

            const auto writeTime = std::filesystem::last_write_time(lockPath, error);
            if (!error && std::filesystem::file_time_type::clock::now() - writeTime > std::chrono::minutes(5))
            {
                error.clear();
                if (std::filesystem::remove(lockPath, error) && !error)
                {
                    error.clear();
                    if (std::filesystem::create_directory(lockPath, error))
                        return PackageLock(lockPath);
                }
            }

            throw std::runtime_error("Another game instance is preparing the packaged content. Please try again shortly.");
        }

        static PackageManifest LoadManifest(const std::filesystem::path& executableDirectory)
        {
            const std::filesystem::path manifestPath = executableDirectory / "Game.manifest.json";
            const std::string manifestText = ReadTextFile(manifestPath, MaxManifestSize);

            nlohmann::json json;
            try
            {
                json = nlohmann::json::parse(manifestText);
            }
            catch (const nlohmann::json::exception& exception)
            {
                throw std::runtime_error(std::string("Game.manifest.json is invalid: ") + exception.what());
            }

            try
            {
                if (json.at("ManifestVersion").get<std::uint32_t>() != 1)
                    throw std::runtime_error("Unsupported Game.manifest.json version.");
                if (json.at("PakFile").get<std::string>() != "Game.pak" ||
                    json.at("PakFormat").get<std::string>() != "zip" ||
                    json.at("Compression").get<std::string>() != "deflate")
                {
                    throw std::runtime_error("Unsupported packaged game archive format.");
                }
                if (json.at("ContentHashAlgorithm").get<std::string>() != "fnv1a64")
                    throw std::runtime_error("Unsupported packaged game content hash.");
                if (!json.at("GameName").is_string() || !json.at("EntryScene").is_string())
                    throw std::runtime_error("Game.manifest.json is missing game identity fields.");

                PackageManifest manifest;
#if HE_PORTABLE_PLAYER
                manifest.project = ReadPlayerIdentity(json.at("Player"),
                    HE_PROJECT_ENGINE_VERSION, HE_PLAYER_RUNTIME_FINGERPRINT);
#endif
                manifest.pakPath = executableDirectory / "Game.pak";
                manifest.pakSize = json.at("PakSize").get<std::uint64_t>();
                manifest.contentVersion = NormalizeContentVersion(json.at("ContentVersion").get<std::string>());
                manifest.contentHash = NormalizeHash(json.at("ContentHash").get<std::string>(), "ContentHash");

                const auto& files = json.at("Files");
                const std::uint64_t declaredFileCount = json.at("FileCount").get<std::uint64_t>();
                if (!files.is_array() || declaredFileCount == 0 || declaredFileCount > MaxFileCount ||
                    declaredFileCount != files.size())
                {
                    throw std::runtime_error("Game.manifest.json has an invalid file list.");
                }

                const bool bHasCatalog = HasCatalogSchema(json);
                if (bHasCatalog)
                {
                    if (!json.contains("RegistryHashAlgorithm") || !json.contains("RegistryHash") ||
                        !json.at("RegistryHashAlgorithm").is_string() ||
                        std::string_view(json.at("RegistryHashAlgorithm").get_ref<const std::string&>()) !=
                            RuntimeResourceCatalogLimits::RegistryHashAlgorithm ||
                        !json.at("RegistryHash").is_string())
                    {
                        throw std::runtime_error("Game.manifest.json has an incomplete resource catalog hash.");
                    }
                    manifest.registryHash = NormalizeHash(
                        json.at("RegistryHash").get_ref<const std::string&>(),
                        "RegistryHash");
                }

                std::uint64_t totalSize = 0;
                std::uint64_t totalCatalogDependencies = 0;
                std::uint64_t totalCatalogTags = 0;
                std::uint64_t totalCatalogCpuBytes = 0;
                std::uint64_t totalCatalogGpuBytes = 0;
                std::unordered_set<std::string> caseFoldedPaths;
                std::string previousPath;
                manifest.files.reserve(files.size());
                std::vector<RuntimeResourceCatalogEntry> catalogEntries;
                if (bHasCatalog)
                    catalogEntries.reserve(files.size());
                for (const auto& fileJson : files)
                {
                    ManifestFile file;
                    file.path = ValidateArchivePath(fileJson.at("Path").get<std::string>());
                    file.storedSize = fileJson.at("StoredSize").get<std::uint64_t>();
                    file.hash = NormalizeHash(fileJson.at("Hash").get<std::string>(), "file Hash");
                    if (file.storedSize > MaxEntrySize || totalSize > MaxContentSize - file.storedSize)
                        throw std::runtime_error("The packaged game content exceeds its safety limit.");
                    totalSize += file.storedSize;

                    const std::string caseFolded = ToLowerAscii(file.path.key);
                    if (!caseFoldedPaths.emplace(caseFolded).second)
                        throw std::runtime_error("Game.manifest.json contains duplicate Windows paths.");
                    if (!previousPath.empty() && !IsBytewiseLess(previousPath, file.path.key))
                        throw std::runtime_error("Game.manifest.json file paths are not in canonical byte order.");
                    previousPath = file.path.key;
                    const std::size_t index = manifest.files.size();
                    if (!manifest.fileIndices.emplace(file.path.key, index).second)
                        throw std::runtime_error("Game.manifest.json contains duplicate paths.");
                    if (bHasCatalog)
                    {
                        catalogEntries.push_back(ParseCatalogEntry(
                            fileJson,
                            file.path.key,
                            totalCatalogDependencies,
                            totalCatalogTags,
                            totalCatalogCpuBytes,
                            totalCatalogGpuBytes));
                    }
                    manifest.files.emplace_back(std::move(file));
                }

                if (bHasCatalog)
                {
                    for (const RuntimeResourceCatalogEntry& entry : catalogEntries)
                    {
                        for (const RuntimeCatalogDependency& dependency : entry.Dependencies)
                        {
                            const auto target = manifest.fileIndices.find(dependency.Path);
                            if (target == manifest.fileIndices.end() ||
                                manifest.files[target->second].path.key != dependency.Path)
                            {
                                throw std::runtime_error(
                                    "Game.manifest.json catalog dependency is not a packaged canonical path: " +
                                    dependency.Path);
                            }
                        }
                    }

                    try
                    {
                        manifest.resourceCatalog = RuntimeResourceCatalogSnapshot::Create(
                            std::move(catalogEntries),
                            manifest.registryHash);
                    }
                    catch (const std::invalid_argument& exception)
                    {
                        throw std::runtime_error(
                            std::string("Game.manifest.json resource catalog is invalid: ") + exception.what());
                    }
                }

                nlohmann::ordered_json markerJson;
#if HE_PORTABLE_PLAYER
                if (!manifest.fileIndices.contains(manifest.project.GameConfig))
                    throw std::runtime_error("Player GameConfig is not included in the cooked package.");
                markerJson["Player"] = json.at("Player");
#endif
                markerJson["ContentVersion"] = manifest.contentVersion;
                markerJson["ContentHash"] = manifest.contentHash;
                if (!manifest.registryHash.empty())
                    markerJson["RegistryHash"] = manifest.registryHash;
                manifest.marker = markerJson.dump() + "\n";
                return manifest;
            }
            catch (const nlohmann::json::exception& exception)
            {
                throw std::runtime_error(std::string("Game.manifest.json has an invalid schema: ") + exception.what());
            }
        }

        struct ArchiveStream
        {
            explicit ArchiveStream(const std::filesystem::path& path)
                : stream(PlatformPaths::ToFileIoPath(path), std::ios::binary)
            {
            }

            std::ifstream stream;
        };

        static voidpf ZCALLBACK OpenArchive(voidpf, const void* filename, int mode)
        {
            if ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) != ZLIB_FILEFUNC_MODE_READ || filename == nullptr)
                return nullptr;

            const auto* path = static_cast<const std::filesystem::path*>(filename);
            auto archiveStream = std::make_unique<ArchiveStream>(*path);
            if (!archiveStream->stream)
                return nullptr;
            return archiveStream.release();
        }

        static uLong ZCALLBACK ReadArchive(voidpf, voidpf stream, void* buffer, uLong size)
        {
            auto* archiveStream = static_cast<ArchiveStream*>(stream);
            archiveStream->stream.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
            return static_cast<uLong>(archiveStream->stream.gcount());
        }

        static uLong ZCALLBACK WriteArchive(voidpf, voidpf, const void*, uLong)
        {
            return 0;
        }

        static ZPOS64_T ZCALLBACK TellArchive(voidpf, voidpf stream)
        {
            auto* archiveStream = static_cast<ArchiveStream*>(stream);
            const std::streampos position = archiveStream->stream.tellg();
            if (position < 0)
                return static_cast<ZPOS64_T>(-1);
            return static_cast<ZPOS64_T>(position);
        }

        static long ZCALLBACK SeekArchive(voidpf, voidpf stream, ZPOS64_T offset, int origin)
        {
            if (offset > static_cast<ZPOS64_T>(std::numeric_limits<std::streamoff>::max()))
                return -1;

            std::ios_base::seekdir direction;
            switch (origin)
            {
            case ZLIB_FILEFUNC_SEEK_SET:
                direction = std::ios_base::beg;
                break;
            case ZLIB_FILEFUNC_SEEK_CUR:
                direction = std::ios_base::cur;
                break;
            case ZLIB_FILEFUNC_SEEK_END:
                direction = std::ios_base::end;
                break;
            default:
                return -1;
            }

            auto* archiveStream = static_cast<ArchiveStream*>(stream);
            archiveStream->stream.clear();
            archiveStream->stream.seekg(static_cast<std::streamoff>(offset), direction);
            return archiveStream->stream ? 0 : -1;
        }

        static int ZCALLBACK CloseArchive(voidpf, voidpf stream)
        {
            delete static_cast<ArchiveStream*>(stream);
            return 0;
        }

        static int ZCALLBACK ArchiveError(voidpf, voidpf stream)
        {
            const auto* archiveStream = static_cast<const ArchiveStream*>(stream);
            return archiveStream->stream.bad() ? 1 : 0;
        }

        class ZipArchive final
        {
        public:
            explicit ZipArchive(const std::filesystem::path& path)
            {
                zlib_filefunc64_def callbacks{};
                callbacks.zopen64_file = &OpenArchive;
                callbacks.zread_file = &ReadArchive;
                callbacks.zwrite_file = &WriteArchive;
                callbacks.ztell64_file = &TellArchive;
                callbacks.zseek64_file = &SeekArchive;
                callbacks.zclose_file = &CloseArchive;
                callbacks.zerror_file = &ArchiveError;
                mArchive = unzOpen2_64(&path, &callbacks);
                if (mArchive == nullptr)
                    throw std::runtime_error("Game.pak is missing or is not a valid ZIP archive.");
            }

            ZipArchive(const ZipArchive&) = delete;
            ZipArchive& operator=(const ZipArchive&) = delete;

            ~ZipArchive()
            {
                if (mArchive != nullptr)
                    unzClose(mArchive);
            }

            [[nodiscard]] unzFile Get() const { return mArchive; }

        private:
            unzFile mArchive = nullptr;
        };

        class CurrentZipEntry final
        {
        public:
            explicit CurrentZipEntry(unzFile archive)
                : mArchive(archive)
            {
                if (unzOpenCurrentFile(mArchive) != UNZ_OK)
                    throw std::runtime_error("Could not open an entry in Game.pak.");
                bOpen = true;
            }

            CurrentZipEntry(const CurrentZipEntry&) = delete;
            CurrentZipEntry& operator=(const CurrentZipEntry&) = delete;

            ~CurrentZipEntry()
            {
                if (bOpen)
                    unzCloseCurrentFile(mArchive);
            }

            int Close()
            {
                bOpen = false;
                return unzCloseCurrentFile(mArchive);
            }

        private:
            unzFile mArchive = nullptr;
            bool bOpen = false;
        };

        static std::string GetCurrentEntryName(unzFile archive, unz_file_info64& fileInfo)
        {
            if (unzGetCurrentFileInfo64(archive, &fileInfo, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
                throw std::runtime_error("Could not inspect an entry in Game.pak.");
            if (fileInfo.size_filename == 0 || fileInfo.size_filename > 4096)
                throw std::runtime_error("Game.pak contains an invalid entry name.");

            std::vector<char> filename(static_cast<std::size_t>(fileInfo.size_filename) + 1, '\0');
            if (unzGetCurrentFileInfo64(
                    archive,
                    &fileInfo,
                    filename.data(),
                    static_cast<uLong>(filename.size()),
                    nullptr,
                    0,
                    nullptr,
                    0) != UNZ_OK)
            {
                throw std::runtime_error("Could not read an entry name from Game.pak.");
            }
            return std::string(filename.data(), static_cast<std::size_t>(fileInfo.size_filename));
        }

        static void ExtractPackage(const PackageManifest& manifest, const std::filesystem::path& stagingRoot)
        {
            std::error_code error;
            const std::uint64_t actualPakSize = std::filesystem::file_size(manifest.pakPath, error);
            if (error || actualPakSize != manifest.pakSize)
                throw std::runtime_error("Game.pak does not match Game.manifest.json.");

            ZipArchive archive(manifest.pakPath);
            unz_global_info64 globalInfo{};
            if (unzGetGlobalInfo64(archive.Get(), &globalInfo) != UNZ_OK ||
                globalInfo.number_entry != manifest.files.size())
            {
                throw std::runtime_error("Game.pak has an unexpected file count.");
            }
            if (unzGoToFirstFile(archive.Get()) != UNZ_OK)
                throw std::runtime_error("Game.pak does not contain readable entries.");

            std::unordered_set<std::string> extractedPaths;
            std::vector<char> buffer(256 * 1024);
            for (std::uint64_t entryIndex = 0; entryIndex < globalInfo.number_entry; ++entryIndex)
            {
                unz_file_info64 fileInfo{};
                const SafeArchivePath archivePath = ValidateArchivePath(GetCurrentEntryName(archive.Get(), fileInfo));
                const auto manifestIt = manifest.fileIndices.find(archivePath.key);
                if (manifestIt == manifest.fileIndices.end() || !extractedPaths.emplace(archivePath.key).second)
                    throw std::runtime_error("Game.pak contains an undeclared or duplicate entry: " + archivePath.key);

                const ManifestFile& expectedFile = manifest.files[manifestIt->second];
                if ((fileInfo.flag & 1u) != 0 || (fileInfo.compression_method != 0 && fileInfo.compression_method != Z_DEFLATED))
                    throw std::runtime_error("Game.pak contains an unsupported entry: " + archivePath.key);
                if (fileInfo.uncompressed_size != expectedFile.storedSize || fileInfo.uncompressed_size > MaxEntrySize)
                    throw std::runtime_error("Game.pak entry size does not match its manifest: " + archivePath.key);

                const std::filesystem::path outputPath = stagingRoot / archivePath.relativePath;
                EnsureDirectory(outputPath.parent_path());
                const std::filesystem::file_status outputStatus = std::filesystem::symlink_status(outputPath, error);
                if (!error && std::filesystem::exists(outputStatus))
                    throw std::runtime_error("Game.pak contains colliding output paths: " + archivePath.key);

                std::ofstream output(PlatformPaths::ToFileIoPath(outputPath), std::ios::binary | std::ios::trunc);
                if (!output)
                    throw std::runtime_error("Could not create cooked game file: " + archivePath.key);

                CurrentZipEntry currentEntry(archive.Get());
                Fnv1a64 fileHash;
                std::uint64_t writtenSize = 0;
                for (;;)
                {
                    const int readSize = unzReadCurrentFile(
                        archive.Get(),
                        buffer.data(),
                        static_cast<unsigned>(buffer.size()));
                    if (readSize < 0)
                        throw std::runtime_error("Could not decompress Game.pak entry: " + archivePath.key);
                    if (readSize == 0)
                        break;
                    if (writtenSize > expectedFile.storedSize ||
                        static_cast<std::uint64_t>(readSize) > expectedFile.storedSize - writtenSize)
                        throw std::runtime_error("Game.pak entry exceeds its declared size: " + archivePath.key);

                    output.write(buffer.data(), readSize);
                    if (!output)
                        throw std::runtime_error("Could not write cooked game file: " + archivePath.key);
                    fileHash.Update(buffer.data(), static_cast<std::size_t>(readSize));
                    writtenSize += static_cast<std::uint64_t>(readSize);
                }

                output.flush();
                if (!output || writtenSize != expectedFile.storedSize)
                    throw std::runtime_error("Cooked game file is incomplete: " + archivePath.key);
                if (currentEntry.Close() != UNZ_OK)
                    throw std::runtime_error("Game.pak entry failed its CRC check: " + archivePath.key);
                if (fileHash.Hex() != expectedFile.hash)
                    throw std::runtime_error("Game.pak entry failed its manifest hash check: " + archivePath.key);

                if (entryIndex + 1 < globalInfo.number_entry && unzGoToNextFile(archive.Get()) != UNZ_OK)
                    throw std::runtime_error("Game.pak ended before all manifest entries were extracted.");
            }

            if (extractedPaths.size() != manifest.files.size())
                throw std::runtime_error("Game.pak is missing one or more manifest entries.");

            Fnv1a64 contentHash;
            for (const ManifestFile& file : manifest.files)
            {
                contentHash.UpdateUint64LittleEndian(static_cast<std::uint64_t>(file.path.key.size()));
                contentHash.Update(file.path.key);
                contentHash.UpdateUint64LittleEndian(file.storedSize);
                std::ifstream input(PlatformPaths::ToFileIoPath(stagingRoot / file.path.relativePath), std::ios::binary);
                if (!input)
                    throw std::runtime_error("Could not verify cooked game file: " + file.path.key);
                while (input)
                {
                    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const std::streamsize readSize = input.gcount();
                    if (readSize > 0)
                        contentHash.Update(buffer.data(), static_cast<std::size_t>(readSize));
                }
                if (!input.eof())
                    throw std::runtime_error("Could not verify cooked game file: " + file.path.key);
            }
            if (contentHash.Hex() != manifest.contentHash)
                throw std::runtime_error("The extracted game content failed its manifest hash check.");
        }

        static bool HasMatchingMarker(
            const std::filesystem::path& contentRoot,
            const std::string& expectedMarker)
        {
            if (!IsExistingDirectory(contentRoot))
                return false;

            const std::filesystem::path markerPath = contentRoot / ".package-marker";
            std::error_code error;
            const std::filesystem::file_status markerStatus = std::filesystem::symlink_status(markerPath, error);
            if (error || std::filesystem::is_symlink(markerStatus) || !std::filesystem::is_regular_file(markerStatus))
                return false;

            try
            {
                return ReadTextFile(markerPath, 4096) == expectedMarker;
            }
            catch (...)
            {
                return false;
            }
        }
    }
#endif

    PackagedProjectPaths PackagedProject::Prepare(std::string_view packageCheckId)
    {
#if !defined(HE_PACKAGED_GAME)
        (void)packageCheckId;
        throw std::logic_error("PackagedProject is only available in packaged game builds.");
#else
        const std::filesystem::path executableDirectory = PlatformPaths::GetApplicationContentDirectory();
        const PackageManifest manifest = LoadManifest(executableDirectory);

        std::error_code error;
        const std::uint64_t actualPakSize = std::filesystem::file_size(manifest.pakPath, error);
        if (error || actualPakSize != manifest.pakSize)
            throw std::runtime_error("Game.pak does not match Game.manifest.json.");

        const bool bPackageCheck = !packageCheckId.empty();
        std::filesystem::path engineRoot;
        std::filesystem::path productRoot;
        if (bPackageCheck)
        {
            if (!IsPackageCheckId(packageCheckId))
                throw std::invalid_argument("Invalid isolated package check identifier.");

            error.clear();
            const std::filesystem::path temporaryRoot =
                std::filesystem::temp_directory_path(error);
            if (error || temporaryRoot.empty() || !temporaryRoot.is_absolute())
                throw std::runtime_error("Could not locate the temporary directory for package checking.");

            engineRoot = temporaryRoot / "HEnginePackageChecks" /
                std::filesystem::u8path(u8"Unicode-\u96EA-\U0001F680 Path");
            productRoot = engineRoot / std::string(packageCheckId);
        }
        else
        {
            const std::filesystem::path localAppData =
                PlatformPaths::GetLocalAppDataDirectory();
            engineRoot = localAppData / "HEngine";
            productRoot = engineRoot / "Projects" / manifest.project.ProjectId;
        }
        const std::string contentDirectoryName =
            "Content-v" + manifest.contentVersion + "-" + manifest.contentHash;
        const std::filesystem::path contentRoot = productRoot / contentDirectoryName;
        std::filesystem::path savedRoot = productRoot / "Saved";
        const std::string legacySavedDirectory =
            manifest.project.LegacySavedDirectory;
        if (!bPackageCheck && !legacySavedDirectory.empty())
        {
            const std::filesystem::path legacySavedRoot =
                engineRoot / legacySavedDirectory / "Saved";
            if (!IsExistingDirectory(savedRoot) && IsExistingDirectory(legacySavedRoot))
                savedRoot = legacySavedRoot;
        }
        const std::filesystem::path stagingRoot =
            productRoot / ("." + contentDirectoryName + ".extracting");
        const std::filesystem::path lockPath = productRoot / ".Content.prepare.lock";

        EnsureDirectory(engineRoot);
        if (bPackageCheck)
            CreateFreshPackageCheckRoot(productRoot);
        else
            EnsureDirectory(productRoot);
        EnsureDirectory(savedRoot);

        if (HasMatchingMarker(contentRoot, manifest.marker))
            return { contentRoot, savedRoot, manifest.resourceCatalog, manifest.project.GameConfig };

        PackageLock packageLock = AcquirePackageLock(lockPath, productRoot);
        if (HasMatchingMarker(contentRoot, manifest.marker))
            return { contentRoot, savedRoot, manifest.resourceCatalog, manifest.project.GameConfig };

        RemoveScopedDirectory(stagingRoot, productRoot);
        if (IsExistingDirectory(contentRoot))
            QuarantineInvalidContent(contentRoot, productRoot, contentDirectoryName);

        EnsureDirectory(stagingRoot);
        try
        {
            ExtractPackage(manifest, stagingRoot);
            WriteTextFile(stagingRoot / ".package-marker", manifest.marker);

            error.clear();
            std::filesystem::rename(stagingRoot, contentRoot, error);
            if (error)
            {
                if (HasMatchingMarker(contentRoot, manifest.marker))
                {
                    RemoveScopedDirectory(stagingRoot, productRoot);
                    return { contentRoot, savedRoot, manifest.resourceCatalog, manifest.project.GameConfig };
                }
                throw std::runtime_error("Could not activate the extracted packaged game content.");
            }
        }
        catch (...)
        {
            try
            {
                RemoveScopedDirectory(stagingRoot, productRoot);
            }
            catch (...)
            {
            }
            throw;
        }

        return { contentRoot, savedRoot, manifest.resourceCatalog, manifest.project.GameConfig };
#endif
    }
}
