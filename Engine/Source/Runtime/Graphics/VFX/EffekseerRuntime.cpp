#include "Hepch.h"

#include "Runtime/Graphics/VFX/EffekseerRuntime.h"

#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Graphics/VFX/EffekseerPackage.h"
#include "Runtime/Graphics/VFX/EffekseerRegressionManifest.h"
#include "Runtime/Platform/Interop/EffekseerRenderInterop.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Utils/StringUtils/StringUtils.h"

#include "ThirdParty/assimp/contrib/zip/src/zip.h"

#include <Effekseer.h>
#include <EffekseerRendererCommon/EffekseerRenderer.Renderer.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace HEngine
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr int32_t kProductionMaxInstanceCount = 8000;
        constexpr int32_t kStressMaxInstanceCount = 60000;
        constexpr int32_t kProductionMaxSpriteCount = 8000;
        constexpr int32_t kStressMaxSpriteCount = 60000;
        // Upstream allocates GPU particles in 256-particle units. A logical
        // 50,000-particle stress payload therefore needs 50,176 physical slots.
        // This allocation stays opt-in; production admission remains 50,000.
        constexpr uint32_t kGpuParticleCapacity = 50176;
        constexpr uint32_t kProductionGpuParticleBudget = 50000;
        constexpr uint32_t kGpuEmitterCapacity = 128;
        constexpr uint32_t kGpuTrailCapacity = 50000;

        constexpr size_t kMaxPackageEntryCount = 1024;
        constexpr uint64_t kMaxPackageEntryBytes = 64ull * 1024ull * 1024ull;
        constexpr uint64_t kMaxPackageTotalBytes = 256ull * 1024ull * 1024ull;
        constexpr size_t kMaxVirtualPathBytes = 1024;

        constexpr std::chrono::seconds kStatsLogInterval(5);

        // Effekseer 1.8 evaluates authored near/mid/far LOD thresholds per
        // manager layer. GameplayCritical and UI never receive an engine bias;
        // lower quality shifts decorative world effects toward cheaper LODs.
        constexpr std::array<std::array<float, static_cast<size_t>(VFXBudgetCategory::Count)>,
                             static_cast<size_t>(VFXQualityTier::Count)> kLodDistanceBias = {{
            {{ 0.0f, 12.0f, 24.0f, 0.0f }},
            {{ 0.0f,  4.0f, 10.0f, 0.0f }},
            {{ 0.0f,  0.0f,  0.0f, 0.0f }},
        }};

        constexpr std::array<std::array<VFXBudget, static_cast<size_t>(VFXBudgetCategory::Count)>,
                             static_cast<size_t>(VFXQualityTier::Count)> kBudgets = {{
            // Low: gameplay telegraphs remain admitted; ambient is intentionally tight.
            {{
                { 512,  1500,     0,  800,  2048,  60000,  64,     0,  64,  64 },
                { 256,  2500,     0, 1000,  2048,  90000,  96,     0,  64,  64 },
                {  64,   500,     0,  200,   512,  20000,  24,     0,  48,  48 },
                { 128,  1000,     0,  500,  1024,  40000,  48,     0,  48,  48 },
            }},
            // Medium.
            {{
                { 768,  1800, 10000, 1000,  4096, 100000,  96, 20000,  96,  96 },
                { 512,  4000, 20000, 2000,  8192, 180000, 160, 20000,  96,  96 },
                { 128,  1000, 10000,  400,  2048,  50000,  48, 20000,  64,  64 },
                { 256,  1000,     0,  500,  2048,  70000,  64, 20000,  64,  64 },
            }},
            // High. Stress mode expands the manager allocation separately; it never
            // changes these production admission budgets.
            {{
                { 1024,  2000, 20000, 1500,  8192, 160000, 128, kProductionGpuParticleBudget, 128, 128 },
                {  768,  4000, 50000, 2500, 16384, 300000, 256, kProductionGpuParticleBudget, 128, 128 },
                {  256,  1000, 30000,  500,  4096,  90000,  80, kProductionGpuParticleBudget,  96,  96 },
                {  384,  1000,     0,  500,  4096, 100000,  96, kProductionGpuParticleBudget,  96,  96 },
            }},
        }};

        struct EfkPkgContent
        {
            std::string RelativePath;
            std::filesystem::path FullPath;
            std::string MainEffectEntry;
            std::string MainEffectBaseDir;
            std::vector<uint8_t> MainEffectData;
            std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> Files;
        };

        struct ActiveEffectInfo
        {
            std::string RelativePath;
            VFXBudgetCategory Category = VFXBudgetCategory::Combat;
            uint32_t PreviousCpuInstances = 0;
            bool bUsesSceneDepth = false;
            bool bUsesBackground = false;
        };

        struct EffectRenderRequirements
        {
            bool bUsesSceneDepth = false;
            bool bUsesBackground = false;
        };

        class RollingMetricAccumulator final
        {
        public:
            void Add(float value, VFXMetricStatus status = VFXMetricStatus::Exact)
            {
                if (!std::isfinite(value) || status == VFXMetricStatus::Unavailable)
                    return;

                mSamples[mNextSample] = value;
                mNextSample = (mNextSample + 1) % mSamples.size();
                mSampleCount = std::min(mSampleCount + 1, mSamples.size());
                mCurrent = value;
                mStatus = status;
            }

            [[nodiscard]] VFXRollingMetric Snapshot() const
            {
                VFXRollingMetric metric;
                metric.Status = mStatus;
                metric.SampleCount = static_cast<uint32_t>(mSampleCount);
                if (mSampleCount == 0)
                    return metric;

                std::array<float, VFXStats::RollingWindowSamples> sorted = mSamples;
                std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(mSampleCount));

                const auto percentile = [&](float fraction)
                {
                    const size_t index = static_cast<size_t>(
                        std::ceil(fraction * static_cast<float>(mSampleCount - 1)));
                    return sorted[index];
                };

                metric.Current = mCurrent;
                metric.Peak = sorted[mSampleCount - 1];
                metric.P50 = percentile(0.50f);
                metric.P95 = percentile(0.95f);
                metric.P99 = percentile(0.99f);
                return metric;
            }

            [[nodiscard]] float Current() const
            {
                return mCurrent;
            }

            void Reset()
            {
                mSamples = {};
                mNextSample = 0;
                mSampleCount = 0;
                mCurrent = 0.0f;
                mStatus = VFXMetricStatus::Unavailable;
            }

        private:
            std::array<float, VFXStats::RollingWindowSamples> mSamples = {};
            size_t mNextSample = 0;
            size_t mSampleCount = 0;
            float mCurrent = 0.0f;
            VFXMetricStatus mStatus = VFXMetricStatus::Unavailable;
        };

        struct RuntimeMetrics
        {
            RollingMetricAccumulator CpuUpdateMs;
            RollingMetricAccumulator CpuDrawSubmitMs;
            RollingMetricAccumulator EffectLoadMs;
            RollingMetricAccumulator FirstPlayMs;
            RollingMetricAccumulator BatchTransformMs;
            RollingMetricAccumulator GpuSimulationMs;
            RollingMetricAccumulator GpuRenderMs;
            RollingMetricAccumulator GpuBackgroundCopyMs;

            RollingMetricAccumulator ActiveEffects;
            RollingMetricAccumulator ActiveCpuInstances;
            RollingMetricAccumulator ActiveGpuParticles;
            RollingMetricAccumulator EffectsCreatedThisFrame;
            RollingMetricAccumulator EffectsDestroyedThisFrame;
            RollingMetricAccumulator CpuInstancesCreatedThisFrame;
            RollingMetricAccumulator CpuInstancesDestroyedThisFrame;
            RollingMetricAccumulator CachedEffects;
            RollingMetricAccumulator CachedPackages;
            RollingMetricAccumulator DrawCalls;
            RollingMetricAccumulator VertexCount;
            RollingMetricAccumulator ParticleBufferUsed;
            RollingMetricAccumulator ParticleBufferCapacity;
            RollingMetricAccumulator BudgetExceededCount;
            RollingMetricAccumulator BudgetRejectedCount;

            void Reset()
            {
                *this = {};
            }
        };

        bool IsEnvironmentEnabled(const char* name)
        {
            if (name == nullptr)
                return false;

            char* value = nullptr;
            size_t valueLength = 0;
            if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr)
                return false;

            const std::string text(value);
            std::free(value);
            return text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON";
        }

        [[nodiscard]] std::string GetEnvironmentText(const char* name)
        {
            if (name == nullptr)
                return {};

            char* value = nullptr;
            size_t valueLength = 0;
            if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr)
                return {};

            std::string text(value);
            std::free(value);
            return text;
        }

        [[nodiscard]] bool HasDangerousArchivePath(const std::string& rawPath)
        {
            if (rawPath.empty() || rawPath.size() > kMaxVirtualPathBytes ||
                rawPath.front() == '/' || rawPath.front() == '\\')
                return true;

            for (const unsigned char character : rawPath)
            {
                // Colons can address NTFS alternate data streams even when they
                // are not part of a drive prefix. Control characters are not
                // portable archive entry names and can also alias on extraction.
                if (character == ':' || character < 0x20 || character == 0x7f)
                    return true;
            }

            std::string segment;
            const auto isReservedWindowsName = [](const std::string& value)
            {
                const size_t extension = value.find('.');
                std::string stem = value.substr(0, extension);
                std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c)
                {
                    return static_cast<char>(std::toupper(c));
                });

                if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
                    return true;
                if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9')
                    return stem.starts_with("COM") || stem.starts_with("LPT");
                return false;
            };

            for (size_t index = 0; index <= rawPath.size(); ++index)
            {
                if (index == rawPath.size() || rawPath[index] == '/' || rawPath[index] == '\\')
                {
                    if (segment == ".." ||
                        (!segment.empty() && (segment.back() == '.' || segment.back() == ' ')) ||
                        isReservedWindowsName(segment))
                    {
                        return true;
                    }
                    segment.clear();
                }
                else
                {
                    segment.push_back(rawPath[index]);
                }
            }
            return false;
        }

        std::string ToLowerAscii(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            return text;
        }

        std::string NormalizeVirtualPath(const std::string& input)
        {
            if (input.empty())
                return {};

            std::string path = input;
            std::replace(path.begin(), path.end(), '\\', '/');

            if (path.size() > 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
                path = path.substr(2);

            while (!path.empty() && path.front() == '/')
                path.erase(path.begin());

            std::vector<std::string> segments;
            std::string token;
            for (size_t i = 0; i <= path.size(); ++i)
            {
                if (i == path.size() || path[i] == '/')
                {
                    if (token == "..")
                    {
                        if (!segments.empty())
                            segments.pop_back();
                    }
                    else if (!token.empty() && token != ".")
                    {
                        segments.emplace_back(token);
                    }
                    token.clear();
                }
                else
                {
                    token.push_back(path[i]);
                }
            }

            std::string out;
            for (size_t i = 0; i < segments.size(); ++i)
            {
                if (i > 0)
                    out += '/';
                out += segments[i];
            }
            return ToLowerAscii(out);
        }

        std::string GetParentDir(const std::string& path)
        {
            const size_t pos = path.find_last_of('/');
            if (pos == std::string::npos)
                return {};
            return path.substr(0, pos + 1);
        }

        bool EndsWith(const std::string& text, const std::string& suffix)
        {
            if (suffix.size() > text.size())
                return false;
            return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin());
        }

        bool IsAbsoluteVirtualPath(const std::string& path)
        {
            if (path.empty())
                return false;

            if (path.front() == '/' || path.front() == '\\')
                return true;

            return path.size() > 1 &&
                   std::isalpha(static_cast<unsigned char>(path.front())) &&
                   path[1] == ':';
        }

        std::string GetSafeDependencyPath(const std::string& rawPath)
        {
            const std::string normalized = NormalizeVirtualPath(rawPath);
            if (normalized.empty())
                return "<empty>";

            if (!IsAbsoluteVirtualPath(rawPath))
                return normalized;

            const size_t separator = normalized.find_last_of('/');
            return separator == std::string::npos ? normalized : normalized.substr(separator + 1);
        }

        bool IsEfkPkgPath(const std::string& relativePath)
        {
            const auto ext = ToLowerAscii(Utils::PathToUtf8(
                Utils::Utf8ToPath(relativePath).extension()));
            return ext == ".efkpkg";
        }

        bool TryNormalizeEffectPackagePath(
            const std::string& input,
            std::string& outNormalized)
        {
            if (input.size() > kMaxVirtualPathBytes)
            {
                HE_CORE_ERROR(
                    "[Effekseer] Project-relative package path exceeds {0} bytes",
                    kMaxVirtualPathBytes);
                outNormalized.clear();
                return false;
            }
            const Utils::PathValidationResult validation =
                Utils::TryNormalizeProjectRelativePath(input, outNormalized);
            if (!validation)
            {
                HE_CORE_ERROR(
                    "[Effekseer] Invalid project-relative package path '{0}': {1}",
                    input,
                    validation.Message);
                return false;
            }
            if (!IsEfkPkgPath(outNormalized))
            {
                HE_CORE_ERROR("[Effekseer] Only .efkpkg is supported. got='{0}'", input);
                outNormalized.clear();
                return false;
            }
            return true;
        }

        std::shared_ptr<std::vector<uint8_t>> FindPackageFile(const EfkPkgContent& package,
                                                              const std::string& rawPath)
        {
            const std::string normalized = NormalizeVirtualPath(rawPath);
            if (normalized.empty())
                return nullptr;

            auto exact = package.Files.find(normalized);
            if (exact != package.Files.end())
                return exact->second;

            if (!package.MainEffectBaseDir.empty() && !IsAbsoluteVirtualPath(rawPath))
            {
                const std::string prefixed = NormalizeVirtualPath(package.MainEffectBaseDir + rawPath);
                auto prefixedIt = package.Files.find(prefixed);
                if (prefixedIt != package.Files.end())
                    return prefixedIt->second;
            }

            // Older effects can contain author-machine absolute paths. Resolve those only
            // inside the owning package, preferring the longest unique path suffix.
            std::shared_ptr<std::vector<uint8_t>> suffixMatch;
            size_t suffixLength = 0;
            bool bAmbiguous = false;
            for (const auto& [entry, bytes] : package.Files)
            {
                size_t candidateLength = 0;
                if (EndsWith(normalized, "/" + entry))
                    candidateLength = entry.size();
                else if (EndsWith(entry, "/" + normalized))
                    candidateLength = normalized.size();

                if (candidateLength == 0)
                    continue;

                if (candidateLength > suffixLength)
                {
                    suffixMatch = bytes;
                    suffixLength = candidateLength;
                    bAmbiguous = false;
                }
                else if (candidateLength == suffixLength && suffixMatch != bytes)
                {
                    bAmbiguous = true;
                }
            }

            return bAmbiguous ? nullptr : suffixMatch;
        }

        std::shared_ptr<EfkPkgContent>& GetActivePackage()
        {
            // Effekseer performs dependency callbacks synchronously from Effect::Create.
            // Thread-local ownership prevents concurrent loads from selecting each other's package.
            static thread_local std::shared_ptr<EfkPkgContent> activePackage;
            return activePackage;
        }

        class ScopedPackageLookup final
        {
        public:
            explicit ScopedPackageLookup(std::shared_ptr<EfkPkgContent> package)
                : mPrevious(GetActivePackage())
            {
                GetActivePackage() = std::move(package);
            }

            ~ScopedPackageLookup()
            {
                GetActivePackage() = std::move(mPrevious);
            }

            ScopedPackageLookup(const ScopedPackageLookup&) = delete;
            ScopedPackageLookup& operator=(const ScopedPackageLookup&) = delete;

        private:
            std::shared_ptr<EfkPkgContent> mPrevious;
        };

        bool IsCompiledMaterialProbeWithSource(const EfkPkgContent& package, const std::string& rawPath)
        {
            std::string sourcePath = NormalizeVirtualPath(rawPath);
            if (!EndsWith(sourcePath, ".efkmatd"))
                return false;

            sourcePath.pop_back();
            return FindPackageFile(package, sourcePath) != nullptr;
        }

        class MemoryFileReader final : public Effekseer::FileReader
        {
        public:
            explicit MemoryFileReader(std::shared_ptr<std::vector<uint8_t>> data)
                : mData(std::move(data))
            {
            }

            size_t Read(void* buffer, size_t size) override
            {
                if (mData == nullptr || buffer == nullptr || size == 0)
                    return 0;

                const size_t left = mData->size() - mPos;
                const size_t readSize = std::min(left, size);
                if (readSize > 0)
                {
                    memcpy(buffer, mData->data() + mPos, readSize);
                    mPos += readSize;
                }
                return readSize;
            }

            void Seek(int position) override
            {
                if (mData == nullptr)
                {
                    mPos = 0;
                    return;
                }

                const int clamped = std::clamp(position, 0, static_cast<int>(mData->size()));
                mPos = static_cast<size_t>(clamped);
            }

            int GetPosition() const override
            {
                return static_cast<int>(mPos);
            }

            size_t GetLength() const override
            {
                return mData ? mData->size() : 0;
            }

        private:
            std::shared_ptr<std::vector<uint8_t>> mData;
            size_t mPos = 0;
        };

        class PackageFileInterface final : public Effekseer::FileInterface
        {
        public:
            Effekseer::FileReaderRef OpenRead(const char16_t* path) override
            {
                if (path == nullptr)
                    return nullptr;

                const std::wstring rawWide = reinterpret_cast<const wchar_t*>(path);
                const std::string rawPath = Utils::WStringToAnsi(rawWide);
                const auto& package = GetActivePackage();
                auto matched = package != nullptr ? FindPackageFile(*package, rawPath) : nullptr;

                if (matched != nullptr)
                    return Effekseer::MakeRefPtr<MemoryFileReader>(matched);

                // Effekseer probes the precompiled .efkmatd first, then deliberately falls back
                // to the packaged .efkmat source. Do not report that expected probe as missing.
                if (package != nullptr && IsCompiledMaterialProbeWithSource(*package, rawPath))
                    return nullptr;

                const std::string packagePath = package != nullptr ? package->RelativePath : "<no active package>";
                HE_CORE_WARN("[Effekseer] Missing package dependency. package='{0}', dependency='{1}'",
                             packagePath,
                             GetSafeDependencyPath(rawPath));
                return nullptr;
            }

            Effekseer::FileWriterRef OpenWrite(const char16_t* path) override
            {
                (void)path;
                return nullptr;
            }

        };

        std::shared_ptr<EfkPkgContent> LoadEfkPkg(const std::string& relativePath)
        {
            if (HasDangerousArchivePath(relativePath))
            {
                HE_CORE_ERROR("[Effekseer] Rejected unsafe project-relative package path: {0}", relativePath);
                return nullptr;
            }

            if (!IsEfkPkgPath(relativePath))
            {
                HE_CORE_ERROR("[Effekseer] Only .efkpkg is supported. got='{0}'", relativePath);
                return nullptr;
            }

            const std::filesystem::path fullPath = ConfigManager::GetFullPath(relativePath);
            if (!std::filesystem::exists(fullPath))
            {
                HE_CORE_ERROR("[Effekseer] Package file not found: {0}", Utils::PathToUtf8(fullPath));
                return nullptr;
            }

            std::ifstream archiveStream(fullPath, std::ios::binary | std::ios::ate);
            std::streamoff archiveSize = -1;
            if (archiveStream)
                archiveSize = static_cast<std::streamoff>(archiveStream.tellg());
            if (archiveSize <= 0 ||
                static_cast<std::uintmax_t>(archiveSize) > std::numeric_limits<std::size_t>::max())
            {
                HE_CORE_ERROR("[Effekseer] Failed to read package: {0}", Utils::PathToUtf8(fullPath));
                return nullptr;
            }

            std::vector<char> archiveBytes(static_cast<std::size_t>(archiveSize));
            archiveStream.seekg(0, std::ios::beg);
            if (!archiveStream.read(
                    archiveBytes.data(),
                    static_cast<std::streamsize>(archiveSize)))
            {
                HE_CORE_ERROR("[Effekseer] Failed to read package: {0}", Utils::PathToUtf8(fullPath));
                return nullptr;
            }

            struct zip_t* zipArchive = zip_stream_open(
                archiveBytes.data(),
                archiveBytes.size(),
                0,
                'r');
            if (zipArchive == nullptr)
            {
                HE_CORE_ERROR("[Effekseer] Failed to open package: {0}", Utils::PathToUtf8(fullPath));
                return nullptr;
            }

            auto closeZip = [&]()
            {
                zip_stream_close(zipArchive);
            };

            auto pkg = std::make_shared<EfkPkgContent>();
            pkg->RelativePath = relativePath;
            pkg->FullPath = fullPath;

            std::unordered_map<std::string, std::shared_ptr<std::vector<uint8_t>>> rawEntries;
            std::vector<std::string> effectEntries;
            const ssize_t fileCount = zip_entries_total(zipArchive);
            if (fileCount < 0 || static_cast<uint64_t>(fileCount) > kMaxPackageEntryCount)
            {
                HE_CORE_ERROR("[Effekseer] Package entry count is invalid or exceeds budget. package='{0}', entries={1}, limit={2}",
                              relativePath,
                              fileCount,
                              kMaxPackageEntryCount);
                closeZip();
                return nullptr;
            }

            uint64_t totalUncompressedBytes = 0;
            for (size_t i = 0; i < static_cast<size_t>(fileCount); ++i)
            {
                if (zip_entry_openbyindex(zipArchive, i) < 0)
                {
                    HE_CORE_ERROR("[Effekseer] Failed to inspect package entry. package='{0}', index={1}",
                                  relativePath,
                                  i);
                    closeZip();
                    return nullptr;
                }

                const char* entryName = zip_entry_name(zipArchive);
                const std::string entryPathRaw = entryName ? entryName : "";
                if (HasDangerousArchivePath(entryPathRaw))
                {
                    HE_CORE_ERROR("[Effekseer] Rejected dangerous package path. package='{0}', entry='{1}'",
                                  relativePath,
                                  entryPathRaw);
                    zip_entry_close(zipArchive);
                    closeZip();
                    return nullptr;
                }

                const int isDir = zip_entry_isdir(zipArchive);
                if (isDir > 0)
                {
                    zip_entry_close(zipArchive);
                    continue;
                }

                const std::string entryPath = NormalizeVirtualPath(entryPathRaw);
                if (entryPath.empty())
                {
                    HE_CORE_ERROR("[Effekseer] Rejected empty normalized package path. package='{0}', entry='{1}'",
                                  relativePath,
                                  entryPathRaw);
                    zip_entry_close(zipArchive);
                    closeZip();
                    return nullptr;
                }

                if (rawEntries.contains(entryPath))
                {
                    HE_CORE_ERROR("[Effekseer] Rejected duplicate normalized package path. package='{0}', entry='{1}'",
                                  relativePath,
                                  entryPathRaw);
                    zip_entry_close(zipArchive);
                    closeZip();
                    return nullptr;
                }

                const uint64_t uncompressedSize = zip_entry_size(zipArchive);
                if (uncompressedSize > kMaxPackageEntryBytes ||
                    uncompressedSize > kMaxPackageTotalBytes - totalUncompressedBytes)
                {
                    HE_CORE_ERROR("[Effekseer] Package extraction budget exceeded. package='{0}', entry='{1}', entryBytes={2}, totalBefore={3}",
                                  relativePath,
                                  entryPathRaw,
                                  uncompressedSize,
                                  totalUncompressedBytes);
                    zip_entry_close(zipArchive);
                    closeZip();
                    return nullptr;
                }
                totalUncompressedBytes += uncompressedSize;

                const auto size = static_cast<size_t>(uncompressedSize);
                auto bytes = std::make_shared<std::vector<uint8_t>>();
                bytes->resize(size);

                const ssize_t readSize = size > 0 ? zip_entry_noallocread(zipArchive, bytes->data(), size) : 0;
                zip_entry_close(zipArchive);

                if (readSize < 0 || static_cast<size_t>(readSize) != size)
                {
                    HE_CORE_ERROR("[Effekseer] Failed to extract package entry: {0}", entryPathRaw);
                    closeZip();
                    return nullptr;
                }

                rawEntries.emplace(entryPath, bytes);
                pkg->Files.emplace(entryPath, bytes);

                if (EndsWith(entryPath, ".efkefc"))
                    effectEntries.emplace_back(entryPath);
            }

            closeZip();

            auto metafileIt = rawEntries.find("metafile.json");
            if (metafileIt != rawEntries.end() && metafileIt->second != nullptr)
            {
                try
                {
                    std::string metafileText(reinterpret_cast<const char*>(metafileIt->second->data()), metafileIt->second->size());
                    if (metafileText.size() >= 3 &&
                        static_cast<unsigned char>(metafileText[0]) == 0xEF &&
                        static_cast<unsigned char>(metafileText[1]) == 0xBB &&
                        static_cast<unsigned char>(metafileText[2]) == 0xBF)
                    {
                        metafileText.erase(0, 3);
                    }

                    const auto metaJson = nlohmann::json::parse(metafileText);
                    if (metaJson.contains("files") && metaJson["files"].is_object())
                    {
                        for (auto it = metaJson["files"].begin(); it != metaJson["files"].end(); ++it)
                        {
                            const std::string entryKey = NormalizeVirtualPath(it.key());
                            if (entryKey.empty() || !it.value().is_object())
                                continue;

                            auto bytesIt = rawEntries.find(entryKey);
                            if (bytesIt == rawEntries.end() || bytesIt->second == nullptr)
                                continue;

                            if (!it.value().contains("relative_path") || !it.value()["relative_path"].is_string())
                                continue;

                            const std::string relativeAssetPath = NormalizeVirtualPath(it.value()["relative_path"].get<std::string>());
                            if (relativeAssetPath.empty())
                                continue;

                            pkg->Files[relativeAssetPath] = bytesIt->second;

                            if (it.value().contains("type") && it.value()["type"].is_string())
                            {
                                const std::string type = ToLowerAscii(it.value()["type"].get<std::string>());
                                if (type == "effect")
                                    effectEntries.emplace_back(relativeAssetPath);
                            }
                        }
                    }
                }
                catch (const std::exception& ex)
                {
                    HE_CORE_WARN("[Effekseer] Failed to parse package metafile: {0}, error={1}", relativePath, ex.what());
                }
            }

            if (effectEntries.empty())
            {
                HE_CORE_ERROR("[Effekseer] Package has no .efkefc entry: {0}", relativePath);
                return nullptr;
            }

            const std::string packageStem = ToLowerAscii(Utils::PathToUtf8(
                Utils::Utf8ToPath(relativePath).stem()));
            std::string selected = effectEntries.front();
            for (const auto& effectEntry : effectEntries)
            {
                if (ToLowerAscii(Utils::PathToUtf8(
                        Utils::Utf8ToPath(effectEntry).stem())) == packageStem)
                {
                    selected = effectEntry;
                    break;
                }
            }

            auto selectedIt = pkg->Files.find(selected);
            if (selectedIt == pkg->Files.end() || selectedIt->second == nullptr)
            {
                HE_CORE_ERROR("[Effekseer] Main effect entry not found in package: {0}", relativePath);
                return nullptr;
            }

            pkg->MainEffectEntry = selected;
            pkg->MainEffectBaseDir = GetParentDir(selected);
            pkg->MainEffectData = *selectedIt->second;

            HE_CORE_INFO("[Effekseer] Package loaded: {0}, entry={1}", relativePath, selected);
            return pkg;
        }

        class AudioEngineEffekseerSoundData final : public Effekseer::SoundData
        {
        public:
            std::string RelativePath;
            std::filesystem::path FullPath;
            std::shared_ptr<std::vector<uint8_t>> EncodedData;
        };

        class AudioEngineEffekseerSoundLoader final : public Effekseer::SoundLoader
        {
        public:
            Effekseer::SoundDataRef Load(const char16_t* path) override
            {
                if (path == nullptr)
                    return nullptr;

                std::string relativePath = Utils::WStringToAnsi(reinterpret_cast<const wchar_t*>(path));
                if (relativePath.empty())
                    return nullptr;

                auto data = Effekseer::MakeRefPtr<AudioEngineEffekseerSoundData>();
                data->RelativePath = relativePath;

                const auto& package = GetActivePackage();
                if (package != nullptr)
                {
                    auto bytes = FindPackageFile(*package, relativePath);
                    if (bytes != nullptr && !bytes->empty())
                    {
                        data->EncodedData = std::move(bytes);
                        return data;
                    }
                }

#if defined(HE_PACKAGED_GAME)
                HE_CORE_ERROR(
                    "[Effekseer] Packaged sound dependency is missing from its effect package: {0}",
                    GetSafeDependencyPath(relativePath));
                return nullptr;
#else
                data->FullPath = ConfigManager::GetFullPath(relativePath);
                std::error_code errorCode;
                if (!std::filesystem::is_regular_file(data->FullPath, errorCode) || errorCode)
                {
                    HE_CORE_WARN("[Effekseer] Failed to load sound dependency: {0}", GetSafeDependencyPath(relativePath));
                    return nullptr;
                }
                return data;
#endif
            }

            void Unload(Effekseer::SoundDataRef data) override
            {
                data.Reset();
            }
        };

        struct VoiceState
        {
            AudioHandle Handle{};
            Effekseer::SoundTag Tag = nullptr;
        };

        class AudioEngineEffekseerSoundPlayer final : public Effekseer::SoundPlayer
        {
        public:
            Effekseer::SoundHandle Play(Effekseer::SoundTag tag, const Effekseer::SoundPlayer::InstanceParameter& parameter) override
            {
                auto* data = static_cast<AudioEngineEffekseerSoundData*>(parameter.Data.Get());
                if (data == nullptr || (data->EncodedData == nullptr && data->FullPath.empty()))
                    return nullptr;

                AudioPlayParams params;
                params.Bus = AudioBus::Gameplay;
                params.Volume = std::max(0.0f, parameter.Volume);
                params.Pitch = std::max(0.01f, std::exp2(parameter.Pitch));
                params.Pan = std::clamp(parameter.Pan, -1.0f, 1.0f);
                params.bSpatialized = parameter.Mode3D;
                if (parameter.Mode3D)
                    params.Position = glm::vec3(parameter.Position.X, parameter.Position.Y, parameter.Position.Z);

                auto& audio = AudioEngine::GetInstance();
                const AudioHandle handle = data->EncodedData != nullptr
                    ? audio.PlayMemory(data->EncodedData, data->RelativePath, params)
                    : audio.Play(data->FullPath, params);

                if (!handle)
                    return nullptr;

                auto* state = new VoiceState();
                state->Handle = handle;
                state->Tag = tag;
                mVoices.emplace_back(state);
                return reinterpret_cast<Effekseer::SoundHandle>(state);
            }

            void Stop(Effekseer::SoundHandle handle, Effekseer::SoundTag tag) override
            {
                auto* state = reinterpret_cast<VoiceState*>(handle);
                if (state == nullptr)
                    return;
                if (tag != nullptr && state->Tag != tag)
                    return;
                AudioEngine::GetInstance().Stop(state->Handle);
            }

            void Pause(Effekseer::SoundHandle handle, Effekseer::SoundTag tag, bool pause) override
            {
                auto* state = reinterpret_cast<VoiceState*>(handle);
                if (state == nullptr)
                    return;
                if (tag != nullptr && state->Tag != tag)
                    return;
                AudioEngine::GetInstance().Pause(state->Handle, pause);
            }

            bool CheckPlaying(Effekseer::SoundHandle handle, Effekseer::SoundTag tag) override
            {
                auto* state = reinterpret_cast<VoiceState*>(handle);
                if (state == nullptr)
                    return false;
                if (tag != nullptr && state->Tag != tag)
                    return false;
                auto& audio = AudioEngine::GetInstance();
                return audio.IsPlaying(state->Handle) || audio.IsPaused(state->Handle);
            }

            void StopTag(Effekseer::SoundTag tag) override
            {
                for (auto* voice : mVoices)
                {
                    if (voice != nullptr && voice->Tag == tag)
                        AudioEngine::GetInstance().Stop(voice->Handle);
                }
            }

            void PauseTag(Effekseer::SoundTag tag, bool pause) override
            {
                for (auto* voice : mVoices)
                {
                    if (voice != nullptr && voice->Tag == tag)
                        AudioEngine::GetInstance().Pause(voice->Handle, pause);
                }
            }

            bool CheckPlayingTag(Effekseer::SoundTag tag) override
            {
                for (auto* voice : mVoices)
                {
                    if (voice != nullptr && voice->Tag == tag)
                    {
                        auto& audio = AudioEngine::GetInstance();
                        if (audio.IsPlaying(voice->Handle) || audio.IsPaused(voice->Handle))
                            return true;
                    }
                }
                return false;
            }

            void StopAll() override
            {
                for (auto* voice : mVoices)
                {
                    if (voice != nullptr)
                        AudioEngine::GetInstance().Stop(voice->Handle);
                }
            }

            void CollectGarbage()
            {
                auto it = mVoices.begin();
                while (it != mVoices.end())
                {
                    VoiceState* state = *it;
                    auto& audio = AudioEngine::GetInstance();
                    if (state == nullptr || (!audio.IsPlaying(state->Handle) && !audio.IsPaused(state->Handle)))
                    {
                        delete state;
                        it = mVoices.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            ~AudioEngineEffekseerSoundPlayer() override
            {
                StopAll();
                for (auto* voice : mVoices)
                    delete voice;
                mVoices.clear();
            }

        private:
            std::vector<VoiceState*> mVoices;
        };

        // Effekseer only exposes the serialized GPU-emitter flag while it asks
        // its configured factory to create a resource. Keep a non-rendering
        // detector installed so unsupported packages fail at load time instead
        // of being accepted as invisible CPU effects.
        class GpuParticleRequestDetector final : public Effekseer::GpuParticleFactory
        {
        public:
            Effekseer::GpuParticles::ResourceRef CreateResource(
                const Effekseer::GpuParticles::ParamSet&,
                const Effekseer::Effect*) override
            {
                ++mRequestCount;
                return nullptr;
            }

            [[nodiscard]] uint64_t GetRequestCount() const
            {
                return mRequestCount;
            }

        private:
            uint64_t mRequestCount = 0;
        };

        struct EffekseerState
        {
            bool Initialized = false;
            bool bDetailedStatsEnabled = false;
            bool bStatsLogEnabled = false;
            bool bGpuParticlesCapable = false;
            bool bGpuParticlesEnabled = false;
            bool bStressModeEnabled = false;
            bool bValidatePackagesPending = false;
            bool bValidatePackagesActive = false;
            bool bStressEffectPending = false;
            int32_t MaxInstanceCount = kProductionMaxInstanceCount;
            int32_t PreviousCpuInstanceCount = 0;
            uint32_t GpuParticleCapacity = 0;
            uint32_t GpuTrailCapacity = 0;
            float ElapsedTimeSeconds = 0.0f;
            uint32_t EffectsCreatedThisFrame = 0;
            uint32_t EffectsDestroyedThisFrame = 0;
            uint64_t BudgetExceededCount = 0;
            uint64_t BudgetRejectedCount = 0;
            VFXQualityTier QualityTier = VFXQualityTier::High;

            Effekseer::ManagerRef Manager;
            EffekseerRenderer::RendererRef Renderer;
            Effekseer::FileInterfaceRef FileInterface;
            Effekseer::GpuParticleFactoryRef GpuParticleFactory;
            GpuParticleRequestDetector* GpuParticleRequestDetectorTyped = nullptr;
            Effekseer::GpuParticleSystemRef GpuParticleSystem;

            std::unordered_map<std::string, Effekseer::EffectRef> Effects;
            std::unordered_map<std::string, EffectRenderRequirements> EffectRequirements;
            std::unordered_map<std::string, std::shared_ptr<EfkPkgContent>> Packages;
            std::unordered_set<std::string> PlayedEffectPaths;
            std::unordered_map<VFXHandle, ActiveEffectInfo> ActiveEffects;
            std::array<uint32_t, static_cast<size_t>(VFXBudgetCategory::Count)> ActiveByCategory = {};
            std::array<uint32_t, static_cast<size_t>(VFXBudgetCategory::Count)> CpuInstancesByCategory = {};
            std::array<uint32_t, static_cast<size_t>(VFXBudgetCategory::Count)> NewCpuInstancesByCategory = {};
            bool bRejectAmbientForRenderBudget = false;
            bool bRejectCombatForRenderBudget = false;

            Effekseer::SoundLoaderRef SoundLoader;
            Effekseer::SoundPlayerRef SoundPlayer;
            AudioEngineEffekseerSoundPlayer* SoundPlayerTyped = nullptr;

            RuntimeMetrics Metrics;
            Scope<IEffekseerRenderInterop> RenderInterop;
            VFXRenderBackendStatus RenderBackendStatus = VFXRenderBackendStatus::Unconfigured;
            bool bLegacyRenderWarningLogged = false;
            Clock::time_point LastStatsLogTime = Clock::now();
            std::string StressEffectPath;

            glm::mat4 View = glm::mat4(1.0f);
            glm::mat4 Projection = glm::mat4(1.0f);
        };

        EffekseerState& GetState()
        {
            static EffekseerState s;
            return s;
        }

        [[nodiscard]] float ElapsedMilliseconds(Clock::time_point start)
        {
            return std::chrono::duration<float, std::milli>(Clock::now() - start).count();
        }

        [[nodiscard]] size_t CategoryIndex(VFXBudgetCategory category)
        {
            return static_cast<size_t>(category);
        }

        [[nodiscard]] size_t QualityIndex(VFXQualityTier qualityTier)
        {
            return static_cast<size_t>(qualityTier);
        }

        [[nodiscard]] bool IsValidCategory(VFXBudgetCategory category)
        {
            return CategoryIndex(category) < static_cast<size_t>(VFXBudgetCategory::Count);
        }

        [[nodiscard]] bool IsValidQuality(VFXQualityTier qualityTier)
        {
            return QualityIndex(qualityTier) < static_cast<size_t>(VFXQualityTier::Count);
        }

        [[nodiscard]] const VFXBudget& FindBudget(VFXBudgetCategory category, VFXQualityTier qualityTier)
        {
            const VFXBudgetCategory safeCategory = IsValidCategory(category)
                ? category
                : VFXBudgetCategory::Combat;
            const VFXQualityTier safeQuality = IsValidQuality(qualityTier)
                ? qualityTier
                : VFXQualityTier::High;
            return kBudgets[QualityIndex(safeQuality)][CategoryIndex(safeCategory)];
        }

        [[nodiscard]] uint32_t GetSharedEffectCacheBudget(VFXQualityTier qualityTier)
        {
            uint32_t limit = 0;
            for (size_t category = 0; category < static_cast<size_t>(VFXBudgetCategory::Count); ++category)
            {
                limit = std::max(limit, kBudgets[QualityIndex(qualityTier)][category].MaxEffectCache);
            }
            return limit;
        }

        [[nodiscard]] uint32_t GetSharedPackageCacheBudget(VFXQualityTier qualityTier)
        {
            uint32_t limit = 0;
            for (size_t category = 0; category < static_cast<size_t>(VFXBudgetCategory::Count); ++category)
            {
                limit = std::max(limit, kBudgets[QualityIndex(qualityTier)][category].MaxPackageCache);
            }
            return limit;
        }

        [[nodiscard]] uint32_t GetGpuParticleCapacity(VFXQualityTier qualityTier)
        {
            uint32_t limit = 0;
            for (size_t category = 0; category < static_cast<size_t>(VFXBudgetCategory::Count); ++category)
            {
                limit = std::max(limit, kBudgets[QualityIndex(qualityTier)][category].MaxGpuParticles);
            }
            if (limit == 0)
                return 0;

            constexpr uint32_t allocationUnit = 256;
            const uint32_t alignedLimit =
                ((limit + allocationUnit - 1) / allocationUnit) * allocationUnit;
            return std::min(alignedLimit, kGpuParticleCapacity);
        }

        [[nodiscard]] uint32_t GetGpuTrailCapacity(VFXQualityTier qualityTier)
        {
            uint32_t limit = 0;
            for (size_t category = 0; category < static_cast<size_t>(VFXBudgetCategory::Count); ++category)
            {
                limit = std::max(limit, kBudgets[QualityIndex(qualityTier)][category].MaxTrailSegments);
            }
            return std::min(limit, kGpuTrailCapacity);
        }

        EffectRenderRequirements GetEffectRenderRequirements(const Effekseer::EffectRef& effect)
        {
            EffectRenderRequirements requirements;
            if (effect == nullptr)
                return requirements;

            std::vector<Effekseer::EffectNode*> pending;
            if (effect->GetRoot() != nullptr)
                pending.emplace_back(effect->GetRoot());
            while (!pending.empty())
            {
                Effekseer::EffectNode* node = pending.back();
                pending.pop_back();
                const Effekseer::EffectBasicRenderParameter parameter = node->GetBasicRenderParameter();
                requirements.bUsesSceneDepth = requirements.bUsesSceneDepth ||
                    parameter.SoftParticleDistanceFar > 0.0f ||
                    parameter.SoftParticleDistanceNear > 0.0f ||
                    parameter.SoftParticleDistanceNearOffset > 0.0f;
                requirements.bUsesBackground = requirements.bUsesBackground ||
                    parameter.Distortion ||
                    parameter.MaterialType == Effekseer::RendererMaterialType::BackDistortion ||
                    parameter.MaterialType == Effekseer::RendererMaterialType::File;
                for (int32_t index = 0; index < node->GetChildrenCount(); ++index)
                {
                    if (Effekseer::EffectNode* child = node->GetChild(index))
                        pending.emplace_back(child);
                }
            }
            return requirements;
        }

        void SaturatingAdd(uint32_t& destination, uint32_t value)
        {
            destination = value > std::numeric_limits<uint32_t>::max() - destination
                ? std::numeric_limits<uint32_t>::max()
                : destination + value;
        }

        [[nodiscard]] bool ConfigureGpuParticles(EffekseerState& s, bool bEnabled)
        {
            if (bEnabled == s.bGpuParticlesEnabled)
                return true;

            if (!bEnabled)
            {
                // Effekseer GPU-emitter instances call back through the manager's
                // GpuParticleSystem while they are being killed. Removing the
                // system while any effect is alive would turn that into a null
                // dereference. Runtime toggles therefore require a quiescent manager.
                if (s.Manager != nullptr &&
                    (s.Manager->GetTotalInstanceCount() > 0 || !s.ActiveEffects.empty()))
                {
                    HE_CORE_WARN("[Effekseer] GPU particle disable deferred; stop active effects first");
                    return false;
                }
                if (s.Manager != nullptr)
                {
                    s.Manager->SetGpuParticleSystem(nullptr);
                    s.Manager->SetGpuParticleFactory(nullptr);
                }
                s.GpuParticleSystem.Reset();
                s.GpuParticleFactory.Reset();
                s.GpuParticleRequestDetectorTyped = nullptr;
                s.GpuParticleCapacity = 0;
                s.GpuTrailCapacity = 0;
                s.bGpuParticlesEnabled = false;
                return true;
            }

            HE_CORE_ERROR(
                "[Effekseer] GPU particles are unsupported by the current external-command-list "
                "adapter; the request was rejected before creating an Effekseer GPU particle system");
            return false;
        }

        void RemoveTrackedEffect(EffekseerState& s, VFXHandle handle)
        {
            const auto it = s.ActiveEffects.find(handle);
            if (it == s.ActiveEffects.end())
                return;

            if (IsValidCategory(it->second.Category))
            {
                auto& count = s.ActiveByCategory[CategoryIndex(it->second.Category)];
                if (count > 0)
                    --count;
            }
            ++s.EffectsDestroyedThisFrame;
            s.ActiveEffects.erase(it);
        }

        void RemoveFinishedEffectsAndRefreshUsage(EffekseerState& s)
        {
            s.CpuInstancesByCategory = {};
            s.NewCpuInstancesByCategory = {};

            for (auto it = s.ActiveEffects.begin(); it != s.ActiveEffects.end();)
            {
                if (!s.Manager->Exists(static_cast<Effekseer::Handle>(it->first)))
                {
                    if (IsValidCategory(it->second.Category))
                    {
                        auto& count = s.ActiveByCategory[CategoryIndex(it->second.Category)];
                        if (count > 0)
                            --count;
                    }
                    ++s.EffectsDestroyedThisFrame;
                    it = s.ActiveEffects.erase(it);
                    continue;
                }

                if (IsValidCategory(it->second.Category))
                {
                    const size_t category = CategoryIndex(it->second.Category);
                    const int32_t instanceCount = s.Manager->GetInstanceCount(
                        static_cast<Effekseer::Handle>(it->first));
                    const uint32_t currentInstances = static_cast<uint32_t>(std::max(0, instanceCount));
                    SaturatingAdd(s.CpuInstancesByCategory[category], currentInstances);
                    if (currentInstances > it->second.PreviousCpuInstances)
                    {
                        SaturatingAdd(
                            s.NewCpuInstancesByCategory[category],
                            currentInstances - it->second.PreviousCpuInstances);
                    }
                    it->second.PreviousCpuInstances = currentInstances;
                }
                ++it;
            }
        }

        Effekseer::Matrix44 ToEffekseerMatrix(const glm::mat4& m)
        {
            Effekseer::Matrix44 out;
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    out.Values[r][c] = m[r][c];
                }
            }
            return out;
        }
    }

    void EffekseerRuntime::Initialize(const EffekseerRuntimeDesc& desc)
    {
        HE_PROFILE_SECTION("EffekseerRuntime::Initialize");
        auto& s = GetState();
        if (s.Initialized)
        {
            if (s.RenderBackendStatus == VFXRenderBackendStatus::Ready &&
                s.RenderInterop != nullptr &&
                s.RenderInterop->GetStatus().bInitialized)
            {
                return;
            }
            throw std::logic_error("Effekseer runtime is already initialized with a different backend state");
        }
        if (desc.Device == nullptr)
            throw std::invalid_argument("Effekseer runtime requires an NVRHI device");
        if (desc.Device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D12 &&
            desc.Device->getGraphicsAPI() != nvrhi::GraphicsAPI::VULKAN)
        {
            throw std::invalid_argument("Effekseer runtime requires an NVRHI D3D12 or Vulkan device");
        }
        if (desc.FramesInFlight == 0)
            throw std::invalid_argument("Effekseer runtime frames-in-flight must be positive");
        if (IsEnvironmentEnabled("HE_VFX_GPU_PARTICLES"))
        {
            throw std::runtime_error(
                "HE_VFX_GPU_PARTICLES is unsupported by the current Effekseer "
                "external-command-list adapter; GPU particle initialization was rejected");
        }

#if HE_VFX_DETAILED_STATS
        s.bDetailedStatsEnabled = !IsEnvironmentEnabled("HE_VFX_DISABLE_STATS");
        s.bStatsLogEnabled = IsEnvironmentEnabled("HE_VFX_STATS_LOG");
#else
        s.bDetailedStatsEnabled = false;
        s.bStatsLogEnabled = false;
#endif
#if HE_VFX_GPU_STRESS_ALLOWED
        s.bStressModeEnabled = IsEnvironmentEnabled("HE_VFX_STRESS_MODE");
#else
        s.bStressModeEnabled = false;
#endif
        s.bValidatePackagesPending = IsEnvironmentEnabled("HE_VFX_VALIDATE_PACKAGES");
        s.StressEffectPath = s.bStressModeEnabled ? GetEnvironmentText("HE_VFX_STRESS_EFFECT") : std::string{};
        if (!s.StressEffectPath.empty() && HasDangerousArchivePath(s.StressEffectPath))
        {
            HE_CORE_ERROR("[Effekseer][Stress] HE_VFX_STRESS_EFFECT must be a project-relative path");
            s.StressEffectPath.clear();
        }
        s.bStressEffectPending = !s.StressEffectPath.empty();
        s.MaxInstanceCount = s.bStressModeEnabled ? kStressMaxInstanceCount : kProductionMaxInstanceCount;
        const int32_t maxSpriteCount = s.bStressModeEnabled ? kStressMaxSpriteCount : kProductionMaxSpriteCount;

        try
        {
            s.RenderInterop = CreateEffekseerRenderInterop(desc.Device, {
                .ColorFormat = desc.ColorFormat,
                .DepthFormat = desc.DepthFormat,
                .FramesInFlight = desc.FramesInFlight,
                .MaxSpriteCount = maxSpriteCount,
                .bReversedDepth = desc.bReversedDepth,
            });
        }
        catch (...)
        {
            s.RenderInterop.reset();
            s.RenderBackendStatus = VFXRenderBackendStatus::Unconfigured;
            throw;
        }
        s.Renderer = EffekseerRenderer::RendererRef(s.RenderInterop->GetRenderer());
        if (s.Renderer != nullptr)
            s.Renderer->AddRef();
        // CPU sprite/model rendering is supported through the external NVRHI
        // command list. Effekseer 1.80.6 GPU emitters are not: their deferred
        // StandardRenderer flush does not complete on this integration path.
        s.bGpuParticlesCapable = false;
        s.RenderBackendStatus = VFXRenderBackendStatus::Ready;

        s.Manager = Effekseer::Manager::Create(s.MaxInstanceCount);

        if (s.Renderer == nullptr || s.Manager == nullptr)
        {
            HE_CORE_ERROR("[Effekseer] Failed to initialize manager or renderer");
            s.Manager.Reset();
            s.Renderer.Reset();
            s.RenderInterop.reset();
            s.RenderBackendStatus = VFXRenderBackendStatus::Unconfigured;
            s.SoundLoader.Reset();
            s.SoundPlayer.Reset();
            s.SoundPlayerTyped = nullptr;
            s.Effects.clear();
            return;
        }

        s.Manager->SetSpriteRenderer(s.Renderer->CreateSpriteRenderer());
        s.Manager->SetRibbonRenderer(s.Renderer->CreateRibbonRenderer());
        s.Manager->SetRingRenderer(s.Renderer->CreateRingRenderer());
        s.Manager->SetTrackRenderer(s.Renderer->CreateTrackRenderer());
        s.Manager->SetModelRenderer(s.Renderer->CreateModelRenderer());

        s.FileInterface = Effekseer::MakeRefPtr<PackageFileInterface>();
        s.Manager->SetTextureLoader(s.Renderer->CreateTextureLoader(s.FileInterface));
        s.Manager->SetModelLoader(s.Renderer->CreateModelLoader(s.FileInterface));
        s.Manager->SetMaterialLoader(s.Renderer->CreateMaterialLoader(s.FileInterface));
        s.Manager->SetCoordinateSystem(Effekseer::CoordinateSystem::LH);

        auto gpuParticleDetector = Effekseer::MakeRefPtr<GpuParticleRequestDetector>();
        s.GpuParticleFactory = gpuParticleDetector;
        s.GpuParticleRequestDetectorTyped = gpuParticleDetector.Get();
        s.Manager->SetGpuParticleFactory(gpuParticleDetector);

        s.SoundLoader = Effekseer::MakeRefPtr<AudioEngineEffekseerSoundLoader>();
        s.SoundPlayer = Effekseer::MakeRefPtr<AudioEngineEffekseerSoundPlayer>();
        s.SoundPlayerTyped = static_cast<AudioEngineEffekseerSoundPlayer*>(s.SoundPlayer.Get());
        s.Manager->SetSoundLoader(s.SoundLoader);
        s.Manager->SetSoundPlayer(s.SoundPlayer);

        s.Initialized = true;
        HE_CORE_INFO("[Effekseer] Runtime initialized. instances={0}, sprites={1}, stressMode={2}, gpuCapable={3}, gpuEnabled={4}",
                     s.MaxInstanceCount,
                     maxSpriteCount,
                     s.bStressModeEnabled,
                     s.bGpuParticlesCapable,
                     s.bGpuParticlesEnabled);

        // The normal game can remain runtime-paused on its menu, where the
        // render system intentionally skips Update(). Run the explicitly
        // requested bounded package regression here so every registered package is
        // validated even in an unattended menu-idle check.
        if (s.bValidatePackagesPending)
        {
            s.bValidatePackagesPending = false;
            if (!ValidateAllEffectPackages())
            {
                Shutdown();
                throw std::runtime_error(
                    "The explicitly requested Effekseer package regression failed");
            }
        }
    }

    void EffekseerRuntime::Init()
    {
        auto& s = GetState();
        if (s.Initialized || s.bLegacyRenderWarningLogged)
            return;

        s.bLegacyRenderWarningLogged = true;
        HE_CORE_WARN(
            "[Effekseer] Legacy Init() is disabled. Call Initialize(EffekseerRuntimeDesc) "
            "after the NVRHI render device is ready");
    }

    void EffekseerRuntime::Shutdown()
    {
        HE_PROFILE_SECTION("EffekseerRuntime::Shutdown");
        auto& s = GetState();
        if (!s.Initialized)
        {
            s.RenderInterop.reset();
            s.RenderBackendStatus = VFXRenderBackendStatus::Unconfigured;
            return;
        }
        if (s.RenderBackendStatus == VFXRenderBackendStatus::FrameActive)
            throw std::logic_error("Effekseer Shutdown cannot run while a render frame is active");

        if (s.Manager != nullptr)
            s.Manager->StopAllEffects();

        // Effects own renderer resources, so release them before Manager and
        // the platform interop bridge to avoid use-after-release.
        s.Effects.clear();
        s.EffectRequirements.clear();
        s.Packages.clear();
        s.PlayedEffectPaths.clear();
        s.ActiveEffects.clear();
        s.ActiveByCategory = {};
        s.CpuInstancesByCategory = {};
        s.NewCpuInstancesByCategory = {};

        // Keep the GPU particle system attached until Manager has destroyed all
        // emitters. Instance::Kill calls through Manager::GetGpuParticleSystem().
        s.Manager.Reset();
        s.GpuParticleSystem.Reset();
        s.GpuParticleFactory.Reset();
        s.GpuParticleRequestDetectorTyped = nullptr;
        s.GpuParticleCapacity = 0;
        s.GpuTrailCapacity = 0;
        s.Renderer.Reset();
        s.FileInterface.Reset();
        s.SoundLoader.Reset();
        s.SoundPlayer.Reset();
        s.SoundPlayerTyped = nullptr;
        s.Metrics.Reset();
        s.bGpuParticlesCapable = false;
        s.bGpuParticlesEnabled = false;
        s.bRejectAmbientForRenderBudget = false;
        s.bRejectCombatForRenderBudget = false;
        s.bValidatePackagesPending = false;
        s.bValidatePackagesActive = false;
        s.bStressEffectPending = false;
        s.StressEffectPath.clear();
        s.EffectsCreatedThisFrame = 0;
        s.EffectsDestroyedThisFrame = 0;
        s.BudgetExceededCount = 0;
        s.BudgetRejectedCount = 0;
        s.PreviousCpuInstanceCount = 0;
        s.ElapsedTimeSeconds = 0.0f;
        s.Initialized = false;
        s.RenderBackendStatus = VFXRenderBackendStatus::Unconfigured;

        // The RenderDevice owner must already have waited for idle. Release the
        // bridge last so manager/effects cannot reference its renderer resources.
        s.RenderInterop.reset();
    }

    bool EffekseerRuntime::IsInitialized()
    {
        return GetState().Initialized;
    }

    VFXRenderBackendStatus EffekseerRuntime::GetRenderBackendStatus()
    {
        return GetState().RenderBackendStatus;
    }

	bool EffekseerRuntime::HasActiveEffects()
	{
		auto& s = GetState();
		return s.Initialized && s.Manager != nullptr &&
			s.Manager->GetRestInstancesCount() < s.MaxInstanceCount;
	}

    VFXRenderRequirements EffekseerRuntime::GetRenderRequirements()
    {
        auto& s = GetState();
        VFXRenderRequirements requirements;
        requirements.bHasActiveEffects = s.Initialized && !s.ActiveEffects.empty();
        for (const auto& [handle, info] : s.ActiveEffects)
        {
            (void)handle;
            requirements.bRequiresDepthSnapshot =
                requirements.bRequiresDepthSnapshot || info.bUsesSceneDepth;
            requirements.bRequiresBackgroundSnapshot =
                requirements.bRequiresBackgroundSnapshot || info.bUsesBackground;
        }
        return requirements;
    }

    std::filesystem::path EffekseerPackage::ExtractForPreview(const std::string& relativePath)
    {
        const auto package = LoadEfkPkg(relativePath);
        if (package == nullptr)
            return {};

        std::error_code ec;
        const auto lastWriteTime = std::filesystem::last_write_time(package->FullPath, ec);
        if (ec)
        {
            HE_CORE_ERROR("[Effekseer] Failed to read package timestamp: {0}", Utils::PathToUtf8(package->FullPath));
            return {};
        }

        const auto packageSize = std::filesystem::file_size(package->FullPath, ec);
        if (ec)
        {
            HE_CORE_ERROR("[Effekseer] Failed to read package size: {0}", Utils::PathToUtf8(package->FullPath));
            return {};
        }

        const std::string cacheKeySource =
            Utils::PathToUtf8(package->FullPath.lexically_normal()) +
            std::to_string(lastWriteTime.time_since_epoch().count()) +
            std::to_string(packageSize);
        const size_t cacheKey = std::hash<std::string>{}(cacheKeySource);

        std::ostringstream cacheDirectoryName;
        cacheDirectoryName << Utils::PathToUtf8(package->FullPath.stem()) << "-" << std::hex << cacheKey;
        const std::filesystem::path previewDirectory =
            std::filesystem::temp_directory_path(ec) / "HEngine" / "EffekseerPreview" /
            Utils::Utf8ToPath(cacheDirectoryName.str());
        if (ec)
        {
            HE_CORE_ERROR("[Effekseer] Failed to resolve the temporary directory");
            return {};
        }

        std::filesystem::create_directories(previewDirectory, ec);
        if (ec)
        {
            HE_CORE_ERROR("[Effekseer] Failed to create preview directory: {0}", Utils::PathToUtf8(previewDirectory));
            return {};
        }

        for (const auto& [entryPath, data] : package->Files)
        {
            if (entryPath.empty() || data == nullptr)
                continue;

            const std::filesystem::path outputPath = previewDirectory / Utils::Utf8ToPath(entryPath);
            std::filesystem::create_directories(outputPath.parent_path(), ec);
            if (ec)
            {
                HE_CORE_ERROR("[Effekseer] Failed to create preview resource directory: {0}", Utils::PathToUtf8(outputPath.parent_path()));
                return {};
            }

            std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                HE_CORE_ERROR("[Effekseer] Failed to write preview resource: {0}", Utils::PathToUtf8(outputPath));
                return {};
            }

            if (!data->empty())
                output.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
            if (!output)
            {
                HE_CORE_ERROR("[Effekseer] Failed to finish writing preview resource: {0}", Utils::PathToUtf8(outputPath));
                return {};
            }
        }

        const std::filesystem::path mainEffectPath = previewDirectory / Utils::Utf8ToPath(package->MainEffectEntry);
        if (!std::filesystem::exists(mainEffectPath))
        {
            HE_CORE_ERROR("[Effekseer] Extracted preview effect not found: {0}", Utils::PathToUtf8(mainEffectPath));
            return {};
        }

        HE_CORE_INFO("[Effekseer] Preview package extracted: {0}", Utils::PathToUtf8(mainEffectPath));
        return mainEffectPath;
    }

    bool EffekseerRuntime::LoadEffect(const std::string& relativePath)
    {
        return LoadEffectInternal(relativePath, false);
    }

    bool EffekseerRuntime::LoadEffectInternal(const std::string& relativePath, bool bBypassCacheBudget)
    {
        HE_PROFILE_SECTION("EffekseerRuntime::LoadEffect");
        const auto loadStart = Clock::now();
        auto& s = GetState();
        if (!s.Initialized)
            return false;

        std::string packagePath;
        if (!TryNormalizeEffectPackagePath(relativePath, packagePath))
            return false;
        const std::string packageKey = Utils::MakeProjectPathCacheKey(packagePath);

        if (s.Effects.find(packageKey) != s.Effects.end())
            return true;

        const bool bNeedsPackageCacheEntry = !s.Packages.contains(packageKey);
        if (!bBypassCacheBudget &&
            (s.Effects.size() >= GetSharedEffectCacheBudget(s.QualityTier) ||
             (bNeedsPackageCacheEntry &&
              s.Packages.size() >= GetSharedPackageCacheBudget(s.QualityTier))))
        {
            ++s.BudgetExceededCount;
            ++s.BudgetRejectedCount;
            HE_CORE_WARN("[Effekseer] Load rejected by shared cache budget. effect='{0}', effectCache={1}/{2}, packageCache={3}/{4}",
                         packagePath,
                         s.Effects.size(),
                         GetSharedEffectCacheBudget(s.QualityTier),
                         s.Packages.size(),
                         GetSharedPackageCacheBudget(s.QualityTier));
            return false;
        }

        class EffectLoadCacheTransaction final
        {
        public:
            EffectLoadCacheTransaction(EffekseerState& state, const std::string& key)
                : mState(state), mKey(key)
            {
            }

            ~EffectLoadCacheTransaction()
            {
                if (mCommitted)
                    return;
                if (mEffectInserted)
                {
                    mState.Effects.erase(mKey);
                    mState.EffectRequirements.erase(mKey);
                }
                if (mPackageInserted)
                    mState.Packages.erase(mKey);
            }

            void MarkPackageInserted() { mPackageInserted = true; }
            void MarkEffectInserted() { mEffectInserted = true; }
            void Commit() { mCommitted = true; }

        private:
            EffekseerState& mState;
            const std::string& mKey;
            bool mPackageInserted = false;
            bool mEffectInserted = false;
            bool mCommitted = false;
        } cacheTransaction(s, packageKey);

        std::shared_ptr<EfkPkgContent> pkg;
        auto pkgIt = s.Packages.find(packageKey);
        if (pkgIt == s.Packages.end())
        {
            pkg = LoadEfkPkg(packagePath);
            if (pkg == nullptr)
                return false;
            s.Packages[packageKey] = pkg;
            cacheTransaction.MarkPackageInserted();
        }
        else
        {
            pkg = pkgIt->second;
        }

        if (pkg == nullptr || pkg->MainEffectData.empty())
        {
            HE_CORE_ERROR("[Effekseer] Package effect data is empty: {0}", packagePath);
            return false;
        }

        std::wstring materialPathW;
        materialPathW.reserve(pkg->MainEffectBaseDir.size());
        for (char c : pkg->MainEffectBaseDir)
            materialPathW.push_back(static_cast<wchar_t>(c));

        ScopedPackageLookup packageLookup(pkg);
        const uint64_t gpuParticleRequestsBefore = s.GpuParticleRequestDetectorTyped != nullptr
            ? s.GpuParticleRequestDetectorTyped->GetRequestCount()
            : 0;
        auto effect = Effekseer::Effect::Create(s.Manager,
                                                pkg->MainEffectData.data(),
                                                static_cast<int32_t>(pkg->MainEffectData.size()),
                                                1.0f,
                                                reinterpret_cast<const char16_t*>(materialPathW.c_str()));
        if (effect == nullptr)
        {
            HE_CORE_ERROR("[Effekseer] Load effect failed: {0}", packagePath);
            return false;
        }
        if (s.GpuParticleRequestDetectorTyped != nullptr &&
            s.GpuParticleRequestDetectorTyped->GetRequestCount() != gpuParticleRequestsBefore)
        {
            HE_CORE_ERROR(
                "[Effekseer] Package rejected because it contains GPU particles, which are unsupported "
                "by the current DX12 external-command-list adapter: {0}",
                packagePath);
            return false;
        }

        const bool bEffectInserted = s.Effects.emplace(packageKey, effect).second;
        if (!bEffectInserted)
            return true;
        cacheTransaction.MarkEffectInserted();
        s.EffectRequirements[packageKey] = GetEffectRenderRequirements(effect);
        s.Metrics.EffectLoadMs.Add(ElapsedMilliseconds(loadStart));
        cacheTransaction.Commit();
        return true;
    }

    VFXHandle EffekseerRuntime::PlayEffect(const std::string& relativePath, const glm::vec3& position)
    {
        VFXPlayParams params;
        params.Position = position;
        return PlayEffect(relativePath, params);
    }

    VFXHandle EffekseerRuntime::PlayEffect(const std::string& relativePath, const VFXPlayParams& params)
    {
        HE_PROFILE_SECTION("EffekseerRuntime::PlayEffect");
        const auto playStart = Clock::now();
        auto& s = GetState();
        if (!s.Initialized)
        {
            HE_CORE_WARN("[Effekseer] Play skipped, runtime not initialized");
            return -1;
        }

        std::string packagePath;
        if (!TryNormalizeEffectPackagePath(relativePath, packagePath))
            return -1;
        const std::string packageKey = Utils::MakeProjectPathCacheKey(packagePath);

        const VFXBudgetCategory category = IsValidCategory(params.Category)
            ? params.Category
            : VFXBudgetCategory::Combat;
        const auto& budget = FindBudget(category, s.QualityTier);
        const size_t categoryIndex = CategoryIndex(category);
        const uint32_t activeInCategory = s.ActiveByCategory[CategoryIndex(category)];
        const uint32_t cpuInstancesInCategory = s.CpuInstancesByCategory[categoryIndex];
        const uint32_t newCpuInstancesInCategory = s.NewCpuInstancesByCategory[categoryIndex];
        const uint32_t effectCacheLimit = GetSharedEffectCacheBudget(s.QualityTier);
        const uint32_t packageCacheLimit = GetSharedPackageCacheBudget(s.QualityTier);
        const bool bNeedsEffectCacheEntry = !s.Effects.contains(packageKey);
        const bool bNeedsPackageCacheEntry = !s.Packages.contains(packageKey);
        const bool bCacheBudgetExceeded =
            (bNeedsEffectCacheEntry && s.Effects.size() >= effectCacheLimit) ||
            (bNeedsPackageCacheEntry && s.Packages.size() >= packageCacheLimit);
        const bool bRenderBudgetExceeded =
            category == VFXBudgetCategory::Ambient
                ? s.bRejectAmbientForRenderBudget
                : category == VFXBudgetCategory::Combat && s.bRejectCombatForRenderBudget;
        const bool bAdmissionBudgetExceeded =
            activeInCategory >= budget.MaxActiveEffects ||
            cpuInstancesInCategory >= budget.MaxCpuInstances ||
            newCpuInstancesInCategory >= budget.MaxNewParticlesPerFrame ||
            bCacheBudgetExceeded ||
            bRenderBudgetExceeded;

        if (bAdmissionBudgetExceeded)
        {
            ++s.BudgetExceededCount;
            if (category != VFXBudgetCategory::GameplayCritical)
            {
                ++s.BudgetRejectedCount;
                HE_CORE_WARN("[Effekseer] Play rejected by stable {0} budget. effect='{1}', active={2}/{3}, cpuInstances={4}/{5}, newInstances={6}/{7}, effectCache={8}/{9}, packageCache={10}/{11}, renderPressure={12}",
                             static_cast<int>(category),
                             packagePath,
                             activeInCategory,
                             budget.MaxActiveEffects,
                             cpuInstancesInCategory,
                             budget.MaxCpuInstances,
                             newCpuInstancesInCategory,
                             budget.MaxNewParticlesPerFrame,
                             s.Effects.size(),
                             effectCacheLimit,
                             s.Packages.size(),
                             packageCacheLimit,
                             bRenderBudgetExceeded);
                return -1;
            }
        }

        const bool bFirstPlay = !s.PlayedEffectPaths.contains(packageKey);
        if (!LoadEffectInternal(
                packagePath,
                category == VFXBudgetCategory::GameplayCritical))
            return -1;

        auto it = s.Effects.find(packageKey);
        if (it == s.Effects.end())
            return -1;

        const Effekseer::Vector3D pos(params.Position.x, params.Position.y, params.Position.z);
        const auto handle = s.Manager->Play(it->second, pos);
        if (handle < 0)
        {
            HE_CORE_WARN("[Effekseer] Play failed: {0}", packagePath);
            return -1;
        }

        const glm::vec3 dir = glm::dot(params.Forward, params.Forward) > 1e-8f
            ? glm::normalize(params.Forward)
            : glm::vec3(0.0f, 0.0f, -1.0f);
        const float yaw = std::atan2(-dir.x, -dir.z);
        const float pitch = std::asin(glm::clamp(dir.y, -1.0f, 1.0f));
        s.Manager->SetRotation(handle, pitch, yaw, 0.0f);

        const float clampedScale = glm::max(params.Scale, 0.001f);
        s.Manager->SetScale(handle, clampedScale, clampedScale, clampedScale);
        s.Manager->SetSpeed(handle, glm::max(params.Speed, 0.01f));

        const VFXHandle publicHandle = static_cast<VFXHandle>(handle);
        s.Manager->SetLayer(handle, static_cast<int32_t>(categoryIndex));
        const auto requirementsIt = s.EffectRequirements.find(packageKey);
        const EffectRenderRequirements requirements = requirementsIt != s.EffectRequirements.end()
            ? requirementsIt->second
            : EffectRenderRequirements{};
        s.ActiveEffects[publicHandle] = {
            packagePath,
            category,
            0,
            requirements.bUsesSceneDepth,
            requirements.bUsesBackground,
        };
        ++s.ActiveByCategory[categoryIndex];
        ++s.EffectsCreatedThisFrame;
        if (bFirstPlay)
        {
            s.PlayedEffectPaths.emplace(packageKey);
            s.Metrics.FirstPlayMs.Add(ElapsedMilliseconds(playStart));
        }
        return publicHandle;
    }

    VFXHandle EffekseerRuntime::PlayEffectAdvanced(const std::string& relativePath,
                                                   const glm::vec3& position,
                                                   const glm::vec3& forward,
                                                   float scale,
                                                   float speed)
    {
        VFXPlayParams params;
        params.Position = position;
        params.Forward = forward;
        params.Scale = scale;
        params.Speed = speed;
        return PlayEffect(relativePath, params);
    }

    bool EffekseerRuntime::SetEffectTransform(VFXHandle handle,
                                              const glm::vec3& position,
                                              const glm::vec3& forward)
    {
        auto& s = GetState();
        if (!s.Initialized || s.Manager == nullptr || handle < 0 || !s.Manager->Exists(handle))
            return false;

        const glm::vec3 dir = glm::dot(forward, forward) > 1e-8f
            ? glm::normalize(forward)
            : glm::vec3(0.0f, 0.0f, -1.0f);
        const float yaw = std::atan2(-dir.x, -dir.z);
        const float pitch = std::asin(glm::clamp(dir.y, -1.0f, 1.0f));
        s.Manager->SetLocation(handle, position.x, position.y, position.z);
        s.Manager->SetRotation(handle, pitch, yaw, 0.0f);
        return true;
    }

    void EffekseerRuntime::SetEffectTransforms(std::span<const EffectTransformCommand> commands,
                                               std::vector<uint8_t>& outResults)
    {
        HE_PROFILE_SECTION("EffekseerRuntime::SetEffectTransforms");
        const auto start = Clock::now();
        DispatchEffectTransformBatch(
            commands,
            nullptr,
            [](void*, const EffectTransformCommand& command)
            {
                return EffekseerRuntime::SetEffectTransform(
                    command.Handle,
                    glm::vec3(command.PositionX, command.PositionY, command.PositionZ),
                    glm::vec3(command.ForwardX, command.ForwardY, command.ForwardZ));
            },
            outResults);
        GetState().Metrics.BatchTransformMs.Add(ElapsedMilliseconds(start));
    }

    bool EffekseerRuntime::IsEffectPlaying(VFXHandle handle)
    {
        auto& s = GetState();
        return s.Initialized && s.Manager != nullptr && handle >= 0 && s.Manager->Exists(handle);
    }

    void EffekseerRuntime::StopEffect(VFXHandle handle)
    {
        auto& s = GetState();
        if (s.Initialized && s.Manager != nullptr && handle >= 0 && s.Manager->Exists(handle))
        {
            s.Manager->StopEffect(handle);
            RemoveTrackedEffect(s, handle);
        }
    }

    void EffekseerRuntime::StopAllEffects()
    {
        auto& s = GetState();
        if (s.Initialized && s.Manager != nullptr)
        {
            s.Manager->StopAllEffects();
            s.EffectsDestroyedThisFrame += static_cast<uint32_t>(std::min<size_t>(
                s.ActiveEffects.size(),
                std::numeric_limits<uint32_t>::max() - s.EffectsDestroyedThisFrame));
            s.ActiveEffects.clear();
            s.ActiveByCategory = {};
            s.CpuInstancesByCategory = {};
            s.NewCpuInstancesByCategory = {};
        }
    }

    VFXStats EffekseerRuntime::GetStats()
    {
        auto& s = GetState();
        VFXStats stats;
        stats.CpuUpdateMs = s.Metrics.CpuUpdateMs.Snapshot();
        stats.CpuDrawSubmitMs = s.Metrics.CpuDrawSubmitMs.Snapshot();
        stats.EffectLoadMs = s.Metrics.EffectLoadMs.Snapshot();
        stats.FirstPlayMs = s.Metrics.FirstPlayMs.Snapshot();
        stats.BatchTransformMs = s.Metrics.BatchTransformMs.Snapshot();
        stats.GpuSimulationMs = s.Metrics.GpuSimulationMs.Snapshot();
        stats.GpuRenderMs = s.Metrics.GpuRenderMs.Snapshot();
        stats.GpuBackgroundCopyMs = s.Metrics.GpuBackgroundCopyMs.Snapshot();
        // Effekseer exposes one combined transparent render pass. A separate,
        // exact transparent-only timestamp is not available from the public API.
        stats.GpuTransparentRenderMs.Status = VFXMetricStatus::Unavailable;

        stats.ActiveEffects = s.Metrics.ActiveEffects.Snapshot();
        stats.ActiveCpuInstances = s.Metrics.ActiveCpuInstances.Snapshot();
        stats.ActiveGpuParticles = s.Metrics.ActiveGpuParticles.Snapshot();
        stats.EffectsCreatedThisFrame = s.Metrics.EffectsCreatedThisFrame.Snapshot();
        stats.EffectsDestroyedThisFrame = s.Metrics.EffectsDestroyedThisFrame.Snapshot();
        stats.CpuInstancesCreatedThisFrame = s.Metrics.CpuInstancesCreatedThisFrame.Snapshot();
        stats.CpuInstancesDestroyedThisFrame = s.Metrics.CpuInstancesDestroyedThisFrame.Snapshot();
        stats.CachedEffects = s.Metrics.CachedEffects.Snapshot();
        stats.CachedPackages = s.Metrics.CachedPackages.Snapshot();
        stats.DrawCalls = s.Metrics.DrawCalls.Snapshot();
        stats.VertexCount = s.Metrics.VertexCount.Snapshot();
        // Renderer counters cover the CPU renderer path, but Effekseer's GPU
        // particle backend submits through its own backend path. Do not expose
        // the partial counters as an exact combined total while GPU particles
        // are enabled.
        if (s.bGpuParticlesEnabled)
        {
            stats.DrawCalls.Status = VFXMetricStatus::Unavailable;
            stats.VertexCount.Status = VFXMetricStatus::Unavailable;
        }
        // Aggregate live allocation is unavailable for the same reason as the
        // live GPU particle count; capacity is an exact configured constant.
        stats.ParticleBufferUsed.Status = VFXMetricStatus::Unavailable;
        stats.ParticleBufferCapacity = s.Metrics.ParticleBufferCapacity.Snapshot();
        stats.BudgetExceededCount = s.Metrics.BudgetExceededCount.Snapshot();
        stats.BudgetRejectedCount = s.Metrics.BudgetRejectedCount.Snapshot();

        if (s.Manager != nullptr)
        {
            uint32_t highestInstances = 0;
            for (const auto& [handle, info] : s.ActiveEffects)
            {
                const int32_t instances = s.Manager->GetInstanceCount(static_cast<Effekseer::Handle>(handle));
                if (instances <= 0 || static_cast<uint32_t>(instances) <= highestInstances)
                    continue;

                highestInstances = static_cast<uint32_t>(instances);
                stats.HighestCostEffectPath = info.RelativePath;
            }

            if (!stats.HighestCostEffectPath.empty())
            {
                stats.HighestCostEffectEstimatedInstances = highestInstances;
                // Instance count is exact, but using it as an ordering proxy for
                // CPU cost is an estimate, not per-effect timer data.
                stats.HighestCostEffectStatus = VFXMetricStatus::Estimated;
            }
        }

        stats.QualityTier = s.QualityTier;
        stats.bDetailedStatsEnabled = s.bDetailedStatsEnabled;
        stats.bGpuParticlesCapable = s.bGpuParticlesCapable;
        stats.bGpuParticlesEnabled = s.bGpuParticlesEnabled;
        return stats;
    }

    VFXBudget EffekseerRuntime::GetBudget(VFXBudgetCategory category, VFXQualityTier qualityTier)
    {
        return FindBudget(category, qualityTier);
    }

    VFXQualityTier EffekseerRuntime::GetQualityTier()
    {
        return GetState().QualityTier;
    }

    void EffekseerRuntime::SetQualityTier(VFXQualityTier qualityTier)
    {
        if (!IsValidQuality(qualityTier))
        {
            HE_CORE_WARN("[Effekseer] Ignored invalid VFX quality tier: {0}", static_cast<int>(qualityTier));
            return;
        }
        auto& s = GetState();
        s.QualityTier = qualityTier;
        const uint32_t desiredGpuCapacity = GetGpuParticleCapacity(qualityTier);
        if (s.bGpuParticlesEnabled && s.GpuParticleCapacity != desiredGpuCapacity)
        {
            if (ConfigureGpuParticles(s, false))
            {
                if (desiredGpuCapacity == 0)
                {
                    HE_CORE_INFO("[Effekseer] GPU particles disabled by Low quality tier");
                }
                else if (!ConfigureGpuParticles(s, true))
                {
                    HE_CORE_WARN("[Effekseer] Failed to recreate GPU particle buffers for the selected quality tier");
                }
            }
            else
            {
                HE_CORE_WARN("[Effekseer] VFX quality changed while effects are active; GPU buffer resize requires a quiescent manager");
            }
        }
    }

    bool EffekseerRuntime::IsGpuParticleCapable()
    {
        return GetState().bGpuParticlesCapable;
    }

    bool EffekseerRuntime::IsGpuParticlesEnabled()
    {
        return GetState().bGpuParticlesEnabled;
    }

    bool EffekseerRuntime::SetGpuParticlesEnabled(bool bEnabled)
    {
        auto& s = GetState();
        if (!s.Initialized)
            return false;
        return ConfigureGpuParticles(s, bEnabled);
    }

    bool EffekseerRuntime::ValidateEffectPackages(
        std::span<const std::string> relativePaths)
    {
        HE_PROFILE_SECTION("EffekseerRuntime::ValidateEffectPackages");
        auto& s = GetState();
        if (!s.Initialized || s.Manager == nullptr || s.bValidatePackagesActive)
            return false;
        if (relativePaths.size() > EffekseerRegressionManifestLimits::MaxPackages)
        {
            HE_CORE_ERROR(
                "[Effekseer][Regression] Package count {0} exceeds the bounded maximum {1}",
                relativePaths.size(),
                EffekseerRegressionManifestLimits::MaxPackages);
            return false;
        }

        std::vector<std::string> normalizedPaths;
        normalizedPaths.reserve(relativePaths.size());
        std::unordered_set<std::string> normalizedKeys;
        normalizedKeys.reserve(relativePaths.size());
        for (const std::string& path : relativePaths)
        {
            std::string normalizedPath;
            if (!TryNormalizeEffectPackagePath(path, normalizedPath))
                return false;

            const std::string packageKey =
                Utils::MakeProjectPathCacheKey(normalizedPath);
            if (!normalizedKeys.emplace(packageKey).second)
            {
                HE_CORE_ERROR(
                    "[Effekseer][Regression] Duplicate package path: '{0}'",
                    normalizedPath);
                return false;
            }
            normalizedPaths.emplace_back(std::move(normalizedPath));
        }

        std::vector<VFXHandle> validationHandles;
        validationHandles.reserve(normalizedPaths.size() * 2);
        std::vector<std::string> transientEffectKeys;
        std::vector<std::string> transientPackageKeys;
        std::vector<std::string> transientPlayedKeys;
        transientEffectKeys.reserve(normalizedPaths.size());
        transientPackageKeys.reserve(normalizedPaths.size());
        transientPlayedKeys.reserve(normalizedPaths.size());

        s.bValidatePackagesActive = true;
        class ValidationScope final
        {
        public:
            ValidationScope(
                EffekseerState& state,
                std::vector<VFXHandle>& handles,
                std::vector<std::string>& effectKeys,
                std::vector<std::string>& packageKeys,
                std::vector<std::string>& playedKeys)
                : mState(state),
                  mHandles(handles),
                  mEffectKeys(effectKeys),
                  mPackageKeys(packageKeys),
                  mPlayedKeys(playedKeys)
            {
            }

            ~ValidationScope()
            {
                for (const VFXHandle handle : mHandles)
                {
                    EffekseerRuntime::StopEffect(handle);
                    if (EffekseerRuntime::IsEffectPlaying(handle) && mState.Manager != nullptr)
                        mState.Manager->UpdateHandle(handle, 0.0f);
                    if (!EffekseerRuntime::IsEffectPlaying(handle))
                        RemoveTrackedEffect(mState, handle);
                }
                for (const std::string& key : mEffectKeys)
                {
                    mState.Effects.erase(key);
                    mState.EffectRequirements.erase(key);
                }
                for (const std::string& key : mPackageKeys)
                    mState.Packages.erase(key);
                for (const std::string& key : mPlayedKeys)
                    mState.PlayedEffectPaths.erase(key);
                mState.bValidatePackagesActive = false;
            }

        private:
            EffekseerState& mState;
            std::vector<VFXHandle>& mHandles;
            std::vector<std::string>& mEffectKeys;
            std::vector<std::string>& mPackageKeys;
            std::vector<std::string>& mPlayedKeys;
        } validationScope(
            s,
            validationHandles,
            transientEffectKeys,
            transientPackageKeys,
            transientPlayedKeys);

        bool bAllPassed = true;
        uint32_t passed = 0;
        uint32_t failed = 0;

        VFXPlayParams validationParams;
        validationParams.Category = VFXBudgetCategory::GameplayCritical;
        const auto stopValidationHandle = [&s](VFXHandle handle)
        {
            if (handle < 0)
                return false;
            StopEffect(handle);
            if (IsEffectPlaying(handle) && s.Manager != nullptr)
                s.Manager->UpdateHandle(handle, 0.0f);
            const bool bStopped = !IsEffectPlaying(handle);
            if (bStopped)
                RemoveTrackedEffect(s, handle);
            return bStopped;
        };

        for (const std::string& path : normalizedPaths)
        {
            const std::string packageKey = Utils::MakeProjectPathCacheKey(path);
            const bool bHadEffect = s.Effects.contains(packageKey);
            const bool bHadPackage = s.Packages.contains(packageKey);
            const bool bHadPlayed = s.PlayedEffectPaths.contains(packageKey);
            if (!bHadEffect)
                transientEffectKeys.emplace_back(packageKey);
            if (!bHadPackage)
                transientPackageKeys.emplace_back(packageKey);
            if (!bHadPlayed)
                transientPlayedKeys.emplace_back(packageKey);

            // Explicit bounded regression runs must not produce false failures
            // merely because the normal shared runtime cache budget is smaller
            // than the project's configured package set.
            bool bPassed = LoadEffectInternal(path, true);
            VFXHandle firstHandle = -1;
            VFXHandle repeatedHandle = -1;
            if (bPassed)
            {
                firstHandle = PlayEffect(path, validationParams);
                if (firstHandle >= 0)
                    validationHandles.emplace_back(firstHandle);
                const bool bStarted = firstHandle >= 0 && IsEffectPlaying(firstHandle);
                const bool bTransformPassed = bStarted && SetEffectTransform(
                    firstHandle,
                    glm::vec3(0.0f),
                    glm::vec3(0.0f, 0.0f, -1.0f));
                const bool bStopped = stopValidationHandle(firstHandle);
                bPassed = bStarted && bTransformPassed && bStopped;
            }
            if (bPassed)
            {
                repeatedHandle = PlayEffect(path, validationParams);
                if (repeatedHandle >= 0)
                    validationHandles.emplace_back(repeatedHandle);
                const bool bRepeated =
                    repeatedHandle >= 0 && IsEffectPlaying(repeatedHandle);
                const bool bRepeatedStopped = stopValidationHandle(repeatedHandle);
                bPassed = bRepeated && bRepeatedStopped;
            }

            if (!bHadEffect)
            {
                s.Effects.erase(packageKey);
                s.EffectRequirements.erase(packageKey);
            }
            if (!bHadPackage)
                s.Packages.erase(packageKey);
            if (!bHadPlayed)
                s.PlayedEffectPaths.erase(packageKey);

            if (bPassed)
            {
                ++passed;
                HE_CORE_INFO("[Effekseer][Regression] PASS package='{0}'", path);
            }
            else
            {
                ++failed;
                bAllPassed = false;
                HE_CORE_ERROR("[Effekseer][Regression] FAIL package='{0}'", path);
            }
        }

        HE_CORE_INFO("[Effekseer][Regression] {0} expected={1} passed={2} failed={3}",
                     bAllPassed ? "PASS" : "FAIL",
                     normalizedPaths.size(),
                     passed,
                     failed);
        return bAllPassed;
    }

    bool EffekseerRuntime::ValidateAllEffectPackages()
    {
        HE_PROFILE_SECTION("EffekseerRuntime::ValidateAllEffectPackages");
        const auto& s = GetState();
        if (!s.Initialized || s.Manager == nullptr || s.bValidatePackagesActive)
            return false;

        std::string manifestPath = GetEnvironmentText("HE_VFX_VALIDATION_MANIFEST");
        if (manifestPath.empty())
        {
            manifestPath = std::string(
                EffekseerRegressionManifestLimits::DefaultProjectPath);
        }

        try
        {
            const EffekseerRegressionManifest manifest =
                LoadEffekseerRegressionManifest(
                    ConfigManager::GetInstance().GetRootFolder(),
                    manifestPath);
            HE_CORE_INFO(
                "[Effekseer][Regression] Loaded project manifest='{0}', packages={1}",
                manifestPath,
                manifest.Packages.size());
            return ValidateEffectPackages(manifest.Packages);
        }
        catch (const std::exception& exception)
        {
            HE_CORE_ERROR(
                "[Effekseer][Regression] Project validation failed for manifest='{0}': {1}",
                manifestPath,
                exception.what());
            return false;
        }
    }

    void EffekseerRuntime::Update(Timestep ts)
    {
        HE_PROFILE_SECTION("EffekseerRuntime::Update");
        auto& s = GetState();
        if (!s.Initialized || s.Manager == nullptr)
            return;

        if (s.bValidatePackagesPending && !s.bValidatePackagesActive)
        {
            s.bValidatePackagesPending = false;
            if (!ValidateAllEffectPackages())
            {
                throw std::runtime_error(
                    "The explicitly requested Effekseer package regression failed");
            }
        }

        if (s.bStressEffectPending)
        {
            s.bStressEffectPending = false;
            VFXPlayParams params;
            params.Category = VFXBudgetCategory::Ambient;
            const VFXHandle handle = PlayEffect(s.StressEffectPath, params);
            if (handle >= 0)
            {
                HE_CORE_INFO("[Effekseer][Stress] Auto-played effect='{0}', handle={1}",
                             s.StressEffectPath,
                             handle);
            }
            else
            {
                HE_CORE_ERROR("[Effekseer][Stress] Failed to auto-play effect='{0}'",
                              s.StressEffectPath);
            }
        }

        Effekseer::Manager::UpdateParameter param;
        const float deltaSeconds = std::max(0.0f, ts.GetSeconds());
        param.DeltaFrame = deltaSeconds * 60.0f;
        param.UpdateInterval = 0.0f;

        const auto updateStart = Clock::now();
        s.Manager->Update(param);
        s.Metrics.CpuUpdateMs.Add(ElapsedMilliseconds(updateStart));
        s.ElapsedTimeSeconds += deltaSeconds;
        if (s.Renderer != nullptr)
            s.Renderer->SetTime(s.ElapsedTimeSeconds);

        if (s.bGpuParticlesEnabled)
        {
            // GPU particle commands must be emitted while the RenderGraph VFX
            // pass owns an NVRHI command list. Render() performs Compute()
            // immediately before Draw() inside that explicit interop frame.
        }

        RemoveFinishedEffectsAndRefreshUsage(s);
        if (s.ActiveEffects.empty())
        {
            s.bRejectAmbientForRenderBudget = false;
            s.bRejectCombatForRenderBudget = false;
        }

        const int32_t cpuInstances = std::max(0, s.Manager->GetTotalInstanceCount());
        const int32_t cpuInstancesCreated = std::max(0, cpuInstances - s.PreviousCpuInstanceCount);
        const int32_t cpuInstancesDestroyed = std::max(0, s.PreviousCpuInstanceCount - cpuInstances);
        s.PreviousCpuInstanceCount = cpuInstances;

        bool bObservedBudgetExceeded =
            s.Effects.size() > GetSharedEffectCacheBudget(s.QualityTier) ||
            s.Packages.size() > GetSharedPackageCacheBudget(s.QualityTier);
        for (size_t category = 0; category < static_cast<size_t>(VFXBudgetCategory::Count); ++category)
        {
            const auto& budget = kBudgets[QualityIndex(s.QualityTier)][category];
            bObservedBudgetExceeded = bObservedBudgetExceeded ||
                s.ActiveByCategory[category] > budget.MaxActiveEffects ||
                s.CpuInstancesByCategory[category] > budget.MaxCpuInstances ||
                s.NewCpuInstancesByCategory[category] > budget.MaxNewParticlesPerFrame;
        }
        if (bObservedBudgetExceeded)
            ++s.BudgetExceededCount;

        s.Metrics.ActiveEffects.Add(static_cast<float>(s.ActiveEffects.size()));
        s.Metrics.ActiveCpuInstances.Add(static_cast<float>(cpuInstances));
        s.Metrics.EffectsCreatedThisFrame.Add(static_cast<float>(s.EffectsCreatedThisFrame));
        s.Metrics.EffectsDestroyedThisFrame.Add(static_cast<float>(s.EffectsDestroyedThisFrame));
        // Public APIs expose snapshots, not individual birth/death events. The
        // positive frame-to-frame deltas are therefore explicitly estimated.
        s.Metrics.CpuInstancesCreatedThisFrame.Add(
            static_cast<float>(cpuInstancesCreated), VFXMetricStatus::Estimated);
        s.Metrics.CpuInstancesDestroyedThisFrame.Add(
            static_cast<float>(cpuInstancesDestroyed), VFXMetricStatus::Estimated);
        s.Metrics.CachedEffects.Add(static_cast<float>(s.Effects.size()));
        s.Metrics.CachedPackages.Add(static_cast<float>(s.Packages.size()));
        s.Metrics.ParticleBufferCapacity.Add(static_cast<float>(
            s.bGpuParticlesEnabled ? s.GpuParticleCapacity : 0));
        s.Metrics.BudgetExceededCount.Add(static_cast<float>(s.BudgetExceededCount));
        s.Metrics.BudgetRejectedCount.Add(static_cast<float>(s.BudgetRejectedCount));
        s.EffectsCreatedThisFrame = 0;
        s.EffectsDestroyedThisFrame = 0;

        if (s.SoundPlayerTyped)
            s.SoundPlayerTyped->CollectGarbage();

        if (s.bStatsLogEnabled && Clock::now() - s.LastStatsLogTime >= kStatsLogInterval)
        {
            const VFXStats stats = GetStats();
            HE_CORE_INFO("[Effekseer][Stats] effects={0} cpuInstances={1} gpuParticles={2}({3}) updateMs={4:.3f}/{5:.3f} drawSubmitMs={6:.3f}/{7:.3f} gpuSimMs={8:.3f}({9}) gpuRenderMs={10:.3f}({11}) draws={12} vertices={13} cache={14}/{15} budgetExceeded={16} budgetRejected={17} highCost='{18}'({19})",
                         stats.ActiveEffects.Current,
                         stats.ActiveCpuInstances.Current,
                         stats.ActiveGpuParticles.Current,
                         static_cast<int>(stats.ActiveGpuParticles.Status),
                         stats.CpuUpdateMs.Current,
                         stats.CpuUpdateMs.P95,
                         stats.CpuDrawSubmitMs.Current,
                         stats.CpuDrawSubmitMs.P95,
                         stats.GpuSimulationMs.Current,
                         static_cast<int>(stats.GpuSimulationMs.Status),
                         stats.GpuRenderMs.Current,
                         static_cast<int>(stats.GpuRenderMs.Status),
                         stats.DrawCalls.Current,
                         stats.VertexCount.Current,
                         stats.CachedEffects.Current,
                         stats.CachedPackages.Current,
                         stats.BudgetExceededCount.Current,
                         stats.BudgetRejectedCount.Current,
                         stats.HighestCostEffectPath,
                         static_cast<int>(stats.HighestCostEffectStatus));
            s.LastStatsLogTime = Clock::now();
        }

        HE_PROFILE_PLOT("VFX CPU Update (ms)", s.Metrics.CpuUpdateMs.Current());
        HE_PROFILE_PLOT("VFX CPU Instances", static_cast<float>(cpuInstances));
    }

    void EffekseerRuntime::SetCamera(const glm::mat4& view, const glm::mat4& projection)
    {
        auto& s = GetState();
        s.View = view;
        s.Projection = projection;
    }

    void EffekseerRuntime::Render(const EffekseerRenderDesc& desc)
    {
        HE_PROFILE_SECTION("EffekseerRuntime::Render");
        auto& s = GetState();
        if (!s.Initialized || s.Manager == nullptr || s.Renderer == nullptr || s.RenderInterop == nullptr)
            throw std::logic_error("Effekseer Render called before NVRHI renderer initialization");
        if (s.RenderBackendStatus != VFXRenderBackendStatus::Ready)
            throw std::logic_error("Effekseer Render requires an idle, ready render backend");
        if (desc.CommandList == nullptr || desc.Framebuffer == nullptr)
            throw std::invalid_argument("Effekseer Render requires a declared NVRHI command list and framebuffer");

        const auto drawStart = Clock::now();
        if (s.Manager->GetRestInstancesCount() >= s.MaxInstanceCount)
        {
            s.Metrics.CpuDrawSubmitMs.Add(ElapsedMilliseconds(drawStart));
            s.Metrics.DrawCalls.Add(0.0f);
            s.Metrics.VertexCount.Add(0.0f);
            return;
        }

        s.Renderer->SetCameraMatrix(ToEffekseerMatrix(s.View));
        s.Renderer->SetProjectionMatrix(ToEffekseerMatrix(s.Projection));

        for (size_t category = 0;
             category < static_cast<size_t>(VFXBudgetCategory::Count);
             ++category)
        {
            Effekseer::Manager::LayerParameter layerParam;
            layerParam.ViewerPosition = {
                desc.CameraPosition.x,
                desc.CameraPosition.y,
                desc.CameraPosition.z
            };
            layerParam.DistanceBias = kLodDistanceBias[QualityIndex(s.QualityTier)][category];
            s.Manager->SetLayerParameter(static_cast<int32_t>(category), layerParam);
        }

        Effekseer::Manager::DrawParameter drawParam;
        drawParam.ViewProjectionMatrix = s.Renderer->GetCameraProjectionMatrix();
        drawParam.CameraCullingMask = s.Manager->GetCameraCullingMaskToShowAllEffects();
        drawParam.ZNear = desc.ZNear;
        drawParam.ZFar = desc.ZFar;
        drawParam.CameraPosition = {
            desc.CameraPosition.x,
            desc.CameraPosition.y,
            desc.CameraPosition.z
        };

        s.Renderer->ResetDrawCallCount();
        s.Renderer->ResetDrawVertexCount();

        bool bUsesSceneDepth = false;
        bool bUsesBackground = false;
        for (const auto& [handle, info] : s.ActiveEffects)
        {
            (void)handle;
            bUsesSceneDepth = bUsesSceneDepth || info.bUsesSceneDepth;
            bUsesBackground = bUsesBackground || info.bUsesBackground;
        }
        if (bUsesSceneDepth && desc.DepthSampleTexture == nullptr)
            throw std::invalid_argument("Effekseer active effects require a declared depth snapshot");
        if (bUsesBackground && desc.BackgroundTexture == nullptr)
            throw std::invalid_argument("Effekseer active effects require a declared background snapshot");

        EffekseerDepthReconstruction depthReconstruction;
        depthReconstruction.DepthBufferScale = 1.0f;
        depthReconstruction.DepthBufferOffset = 0.0f;
        depthReconstruction.ProjectionMatrix33 = s.Projection[2][2];
        depthReconstruction.ProjectionMatrix34 = s.Projection[3][2];
        depthReconstruction.ProjectionMatrix43 = s.Projection[2][3];
        depthReconstruction.ProjectionMatrix44 = s.Projection[3][3];

        s.RenderBackendStatus = VFXRenderBackendStatus::FrameActive;
        try
        {
            s.RenderInterop->BeginFrame({
                .CommandList = desc.CommandList,
                .Framebuffer = desc.Framebuffer,
                .DepthSampleTexture = desc.DepthSampleTexture,
                .BackgroundTexture = desc.BackgroundTexture,
                .DepthReconstruction = depthReconstruction,
            });
            if (s.bGpuParticlesEnabled)
                s.Manager->Compute();
            s.RenderInterop->BeginRendering();
            s.Manager->Draw(drawParam);
            s.RenderInterop->EndRendering();
            s.RenderInterop->EndFrame();
            s.RenderBackendStatus = VFXRenderBackendStatus::Ready;
        }
        catch (...)
        {
            // The bridge owns precise partial-frame unwinding. Shutdown is the
            // only safe recovery after native command recording fails.
            s.RenderInterop->Shutdown();
            s.RenderBackendStatus = VFXRenderBackendStatus::Unconfigured;
            throw;
        }
        s.Metrics.CpuDrawSubmitMs.Add(ElapsedMilliseconds(drawStart));
        const uint64_t drawCalls = static_cast<uint64_t>(std::max(0, s.Renderer->GetDrawCallCount()));
        const uint64_t vertexCount = static_cast<uint64_t>(std::max(0, s.Renderer->GetDrawVertexCount()));
        s.Metrics.DrawCalls.Add(static_cast<float>(drawCalls));
        s.Metrics.VertexCount.Add(static_cast<float>(vertexCount));

        // Effekseer exposes aggregate render counts only. Keep UI independent by
        // applying the world gate only when no UI effect participates in this
        // combined pass. Ambient admission is denied first; Combat is denied only
        // after Ambient is absent and the world remains above the combined budget.
        if (s.ActiveByCategory[CategoryIndex(VFXBudgetCategory::UI)] == 0)
        {
            uint64_t worldDrawCallBudget = 0;
            uint64_t worldVertexBudget = 0;
            constexpr std::array worldCategories = {
                VFXBudgetCategory::GameplayCritical,
                VFXBudgetCategory::Combat,
                VFXBudgetCategory::Ambient,
            };
            for (const VFXBudgetCategory category : worldCategories)
            {
                const auto& budget = FindBudget(category, s.QualityTier);
                worldDrawCallBudget += budget.MaxDrawCalls;
                worldVertexBudget += budget.MaxTransparentVertices;
            }

            const bool bWorldRenderBudgetExceeded =
                drawCalls > worldDrawCallBudget || vertexCount > worldVertexBudget;
            s.bRejectAmbientForRenderBudget = bWorldRenderBudgetExceeded;
            s.bRejectCombatForRenderBudget = bWorldRenderBudgetExceeded &&
                s.ActiveByCategory[CategoryIndex(VFXBudgetCategory::Ambient)] == 0;
            if (bWorldRenderBudgetExceeded)
                ++s.BudgetExceededCount;
        }
        else
        {
            s.bRejectAmbientForRenderBudget = false;
            s.bRejectCombatForRenderBudget = false;
        }

        HE_PROFILE_PLOT("VFX CPU Draw Submit (ms)", s.Metrics.CpuDrawSubmitMs.Current());
        HE_PROFILE_PLOT("VFX Draw Calls", static_cast<float>(s.Renderer->GetDrawCallCount()));
    }

    void EffekseerRuntime::Render(float, float, const glm::vec3&)
    {
        auto& s = GetState();
        if (!s.bLegacyRenderWarningLogged)
        {
            s.bLegacyRenderWarningLogged = true;
            HE_CORE_ERROR(
                "[Effekseer] Legacy immediate Render() has no backend. "
                "Record EffekseerRenderDesc from the RenderGraph VFX pass");
        }
    }

}
