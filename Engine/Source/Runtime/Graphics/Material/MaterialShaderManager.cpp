#include "Hepch.h"

#include "MaterialShaderManager.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        class Sha256 final
        {
        public:
            void Update(std::span<const std::uint8_t> bytes)
            {
                if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - mTotalBytes)
                    throw std::overflow_error("SHA-256 input is too large");
                mTotalBytes += static_cast<std::uint64_t>(bytes.size());

                std::size_t offset = 0;
                if (mBufferedBytes != 0)
                {
                    const std::size_t copied = std::min(
                        bytes.size(), mBuffer.size() - mBufferedBytes);
                    std::copy_n(bytes.data(), copied, mBuffer.data() + mBufferedBytes);
                    mBufferedBytes += copied;
                    offset += copied;
                    if (mBufferedBytes == mBuffer.size())
                    {
                        Transform(mBuffer.data());
                        mBufferedBytes = 0;
                    }
                }

                while (bytes.size() - offset >= mBuffer.size())
                {
                    Transform(bytes.data() + offset);
                    offset += mBuffer.size();
                }
                if (offset != bytes.size())
                {
                    mBufferedBytes = bytes.size() - offset;
                    std::copy_n(bytes.data() + offset, mBufferedBytes, mBuffer.data());
                }
            }

            [[nodiscard]] std::array<std::uint8_t, 32> Finalize()
            {
                if (mTotalBytes > std::numeric_limits<std::uint64_t>::max() / 8)
                    throw std::overflow_error("SHA-256 input is too large");
                const std::uint64_t bitLength = mTotalBytes * 8;

                mBuffer[mBufferedBytes++] = 0x80;
                if (mBufferedBytes > 56)
                {
                    std::fill(mBuffer.begin() + mBufferedBytes, mBuffer.end(), 0);
                    Transform(mBuffer.data());
                    mBufferedBytes = 0;
                }
                std::fill(mBuffer.begin() + mBufferedBytes, mBuffer.begin() + 56, 0);
                for (std::size_t index = 0; index < 8; ++index)
                {
                    mBuffer[56 + index] = static_cast<std::uint8_t>(
                        (bitLength >> ((7 - index) * 8)) & 0xffu);
                }
                Transform(mBuffer.data());

                std::array<std::uint8_t, 32> digest{};
                for (std::size_t word = 0; word < mState.size(); ++word)
                {
                    for (std::size_t byte = 0; byte < 4; ++byte)
                    {
                        digest[word * 4 + byte] = static_cast<std::uint8_t>(
                            (mState[word] >> ((3 - byte) * 8)) & 0xffu);
                    }
                }
                return digest;
            }

        private:
            static constexpr std::array<std::uint32_t, 64> c_RoundConstants = {
                0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
                0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
                0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
                0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
                0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
                0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
                0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
                0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
                0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
            };

            [[nodiscard]] static std::uint32_t RotateRight(
                std::uint32_t value,
                std::uint32_t count) noexcept
            {
                return (value >> count) | (value << (32u - count));
            }

            void Transform(const std::uint8_t* block) noexcept
            {
                std::array<std::uint32_t, 64> words{};
                for (std::size_t index = 0; index < 16; ++index)
                {
                    words[index] =
                        (static_cast<std::uint32_t>(block[index * 4]) << 24u) |
                        (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16u) |
                        (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8u) |
                        static_cast<std::uint32_t>(block[index * 4 + 3]);
                }
                for (std::size_t index = 16; index < words.size(); ++index)
                {
                    const std::uint32_t sigma0 = RotateRight(words[index - 15], 7) ^
                        RotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3u);
                    const std::uint32_t sigma1 = RotateRight(words[index - 2], 17) ^
                        RotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10u);
                    words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
                }

                std::uint32_t a = mState[0];
                std::uint32_t b = mState[1];
                std::uint32_t c = mState[2];
                std::uint32_t d = mState[3];
                std::uint32_t e = mState[4];
                std::uint32_t f = mState[5];
                std::uint32_t g = mState[6];
                std::uint32_t h = mState[7];
                for (std::size_t index = 0; index < words.size(); ++index)
                {
                    const std::uint32_t sum1 = RotateRight(e, 6) ^
                        RotateRight(e, 11) ^ RotateRight(e, 25);
                    const std::uint32_t choose = (e & f) ^ (~e & g);
                    const std::uint32_t temporary1 = h + sum1 + choose +
                        c_RoundConstants[index] + words[index];
                    const std::uint32_t sum0 = RotateRight(a, 2) ^
                        RotateRight(a, 13) ^ RotateRight(a, 22);
                    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                    const std::uint32_t temporary2 = sum0 + majority;
                    h = g;
                    g = f;
                    f = e;
                    e = d + temporary1;
                    d = c;
                    c = b;
                    b = a;
                    a = temporary1 + temporary2;
                }
                mState[0] += a;
                mState[1] += b;
                mState[2] += c;
                mState[3] += d;
                mState[4] += e;
                mState[5] += f;
                mState[6] += g;
                mState[7] += h;
            }

            std::array<std::uint32_t, 8> mState = {
                0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
            };
            std::array<std::uint8_t, 64> mBuffer{};
            std::uint64_t mTotalBytes = 0;
            std::size_t mBufferedBytes = 0;
        };

        struct SourceResolution
        {
            std::string CanonicalSourcePath;
            std::string ManifestSourcePath;
            std::string CacheKey;
            std::filesystem::path ManifestPath;
        };

        struct LoadedShader
        {
            MaterialShaderHandleKey Key;
            ShaderCacheKey CacheKey;
            std::vector<std::byte> Bytecode;
            std::string DebugName;
            nvrhi::ShaderHandle Handle;
        };

        struct LoadedArtifactFile
        {
            std::vector<std::byte> Bytes;
            std::string ContentHash;
        };

        struct MaterialProgramCandidate
        {
            MaterialShaderManifest Manifest;
            MaterialShaderProgramIdentity Identity;
            std::vector<LoadedShader> Shaders;
            std::vector<MaterialShaderDiagnostic> Diagnostics;
        };

        struct FileStamp
        {
            std::filesystem::file_time_type WriteTime{};
            std::uintmax_t Size = 0;
            bool bValid = false;

            [[nodiscard]] bool operator==(const FileStamp&) const = default;
        };

        void AddError(
            std::vector<MaterialShaderDiagnostic>& diagnostics,
            std::string code,
            std::string message,
            std::string source = {})
        {
            diagnostics.push_back({
                .Severity = MaterialShaderDiagnosticSeverity::Error,
                .Code = std::move(code),
                .Message = std::move(message),
                .Source = std::move(source),
            });
        }

        [[nodiscard]] bool HasErrors(
            const std::vector<MaterialShaderDiagnostic>& diagnostics) noexcept
        {
            return std::ranges::any_of(diagnostics, [](const MaterialShaderDiagnostic& diagnostic)
            {
                return diagnostic.Severity == MaterialShaderDiagnosticSeverity::Error;
            });
        }

        [[nodiscard]] std::string ToLowerAscii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
            {
                if (character >= 'A' && character <= 'Z')
                    return static_cast<char>(character - 'A' + 'a');
                return static_cast<char>(character);
            });
            return value;
        }

        [[nodiscard]] std::filesystem::path NormalizeRoot(
            const std::filesystem::path& root)
        {
            if (root.empty())
                throw std::invalid_argument("MaterialShaderManager requires a CACHES root");

            std::error_code error;
            std::filesystem::path normalized = std::filesystem::absolute(root, error);
            if (error)
                throw std::invalid_argument("Failed to resolve material shader CACHES root");
            const std::filesystem::path canonical =
                std::filesystem::weakly_canonical(normalized, error);
            return error ? normalized.lexically_normal() : canonical;
        }

        [[nodiscard]] bool IsWithinRoot(
            const std::filesystem::path& root,
            const std::filesystem::path& candidate)
        {
            const std::filesystem::path relative = candidate.lexically_relative(root);
            if (relative.empty())
                return candidate == root;
            if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
                return false;
            return std::ranges::none_of(relative, [](const std::filesystem::path& component)
            {
                return component == "..";
            });
        }

        [[nodiscard]] std::optional<std::filesystem::path> ResolveCachesPath(
            const std::filesystem::path& cachesRoot,
            std::string_view relativePath,
            std::vector<MaterialShaderDiagnostic>& diagnostics,
            std::string_view diagnosticCode,
            std::string_view source)
        {
            std::string normalizedRelative;
            const Utils::PathValidationResult validation =
                Utils::TryNormalizeProjectRelativePath(relativePath, normalizedRelative);
            if (!validation)
            {
                AddError(
                    diagnostics,
                    std::string(diagnosticCode),
                    "Unsafe CACHES-relative path: " + validation.Message,
                    std::string(source));
                return std::nullopt;
            }

            std::error_code error;
            const std::filesystem::path candidate = std::filesystem::weakly_canonical(
                cachesRoot / Utils::Utf8ToPath(normalizedRelative), error);
            const std::filesystem::path normalizedCandidate = error
                ? (cachesRoot / Utils::Utf8ToPath(normalizedRelative)).lexically_normal()
                : candidate;
            if (!IsWithinRoot(cachesRoot, normalizedCandidate))
            {
                AddError(
                    diagnostics,
                    std::string(diagnosticCode),
                    "CACHES-relative path resolves outside the configured root.",
                    std::string(source));
                return std::nullopt;
            }
            return normalizedCandidate;
        }

        [[nodiscard]] std::optional<SourceResolution> ResolveSource(
            const std::filesystem::path& cachesRoot,
            std::string_view sourcePath,
            std::vector<MaterialShaderDiagnostic>& diagnostics)
        {
            std::string normalized;
            const Utils::PathValidationResult validation =
                Utils::TryNormalizeProjectRelativePath(sourcePath, normalized);
            if (!validation)
            {
                AddError(diagnostics, "MaterialProgram.SourcePath",
                    "Unsafe material shader source path: " + validation.Message,
                    std::string(sourcePath));
                return std::nullopt;
            }

            const std::size_t separator = normalized.find('/');
            if (separator == std::string::npos ||
                ToLowerAscii(normalized.substr(0, separator)) != "shaders")
            {
                AddError(diagnostics, "MaterialProgram.SourcePath",
                    "Material shader source path must be Shaders/<relative>.hlsl.",
                    normalized);
                return std::nullopt;
            }
            const std::string manifestSource = normalized.substr(separator + 1);
            if (manifestSource.empty() ||
                ToLowerAscii(std::filesystem::path(manifestSource).extension().string()) != ".hlsl")
            {
                AddError(diagnostics, "MaterialProgram.SourcePath",
                    "Material shader source path must end in .hlsl.", normalized);
                return std::nullopt;
            }

            std::filesystem::path manifestRelative =
                std::filesystem::path("Materials") / Utils::Utf8ToPath(manifestSource);
            manifestRelative += ".material.json";
            const std::string manifestRelativeText = Utils::PathToUtf8(manifestRelative);
            const std::optional<std::filesystem::path> manifestPath = ResolveCachesPath(
                cachesRoot,
                manifestRelativeText,
                diagnostics,
                "MaterialProgram.ManifestPath",
                normalized);
            if (!manifestPath)
                return std::nullopt;

            const std::string canonicalSource = "Shaders/" + manifestSource;
            return SourceResolution{
                .CanonicalSourcePath = canonicalSource,
                .ManifestSourcePath = manifestSource,
                .CacheKey = Utils::MakeProjectPathCacheKey(canonicalSource),
                .ManifestPath = *manifestPath,
            };
        }

        [[nodiscard]] FileStamp ReadFileStamp(const std::filesystem::path& path) noexcept
        {
            FileStamp stamp;
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || error)
                return stamp;
            stamp.Size = std::filesystem::file_size(path, error);
            if (error)
                return {};
            stamp.WriteTime = std::filesystem::last_write_time(path, error);
            if (error)
                return {};
            stamp.bValid = true;
            return stamp;
        }

        [[nodiscard]] std::string ToHex(std::span<const std::uint8_t> bytes)
        {
            constexpr char c_HexDigits[] = "0123456789abcdef";
            std::string result(bytes.size() * 2, '0');
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                result[index * 2] = c_HexDigits[bytes[index] >> 4u];
                result[index * 2 + 1] = c_HexDigits[bytes[index] & 0x0fu];
            }
            return result;
        }

        void HashUnsigned64(Sha256& hash, std::uint64_t value)
        {
            std::array<std::uint8_t, 8> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                bytes[index] = static_cast<std::uint8_t>(
                    (value >> ((7u - index) * 8u)) & 0xffu);
            }
            hash.Update(bytes);
        }

        void HashString(Sha256& hash, std::string_view value)
        {
            HashUnsigned64(hash, static_cast<std::uint64_t>(value.size()));
            hash.Update(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
        }

        [[nodiscard]] std::optional<LoadedArtifactFile> LoadArtifactFile(
            const std::filesystem::path& path,
            std::vector<MaterialShaderDiagnostic>& diagnostics,
            std::string_view source)
        {
            std::ifstream stream(path, std::ios::in | std::ios::binary | std::ios::ate);
            if (!stream)
            {
                AddError(diagnostics, "MaterialProgram.ArtifactOpen",
                    "Failed to open compiled material shader artifact.", std::string(source));
                return std::nullopt;
            }

            const std::streamoff byteCount = stream.tellg();
            if (byteCount <= 0 ||
                static_cast<std::uint64_t>(byteCount) >
                    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
                byteCount > std::numeric_limits<std::streamsize>::max())
            {
                AddError(diagnostics, "MaterialProgram.ArtifactEmpty",
                    "Compiled material shader artifact is empty or too large.", std::string(source));
                return std::nullopt;
            }

            LoadedArtifactFile artifact;
            artifact.Bytes.resize(static_cast<std::size_t>(byteCount));
            stream.seekg(0, std::ios::beg);
            stream.read(
                reinterpret_cast<char*>(artifact.Bytes.data()),
                static_cast<std::streamsize>(artifact.Bytes.size()));
            if (!stream)
            {
                AddError(diagnostics, "MaterialProgram.ArtifactRead",
                    "Failed while reading compiled material shader artifact.", std::string(source));
                return std::nullopt;
            }

            Sha256 hash;
            hash.Update(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(artifact.Bytes.data()),
                artifact.Bytes.size()));
            artifact.ContentHash = ToHex(hash.Finalize());
            return artifact;
        }

        [[nodiscard]] std::optional<std::pair<MaterialProgramShaderStage, nvrhi::ShaderType>>
            ParseStage(std::string_view stage) noexcept
        {
            if (stage == "vs")
                return std::pair{ MaterialProgramShaderStage::Vertex, nvrhi::ShaderType::Vertex };
            if (stage == "ps")
                return std::pair{ MaterialProgramShaderStage::Pixel, nvrhi::ShaderType::Pixel };
            if (stage == "cs")
                return std::pair{ MaterialProgramShaderStage::Compute, nvrhi::ShaderType::Compute };
            return std::nullopt;
        }

        [[nodiscard]] std::string BuildArtifactIdentityHash(
            const MaterialShaderManifest& manifest,
            RenderBackend backend,
            ShaderBinaryFormat binaryFormat)
        {
            const std::span<const MaterialShaderArtifact> artifacts =
                GetMaterialShaderArtifacts(manifest, backend, binaryFormat);
            std::vector<std::string> identities;
            identities.reserve(artifacts.size());
            for (const MaterialShaderArtifact& artifact : artifacts)
            {
                identities.push_back(
                    std::string(ToString(artifact.Pass)) + "\n" +
                    ToString(artifact.VertexPath) + "\n" +
                    ToString(artifact.DescriptorMode) + "\n" +
                    artifact.Stage + "\n" + artifact.EntryPoint + "\n" +
                    artifact.TargetProfile + "\n" + artifact.Permutation + "\n" +
                    artifact.RelativePath + "\n" + ToLowerAscii(artifact.ContentHash));
            }
            std::sort(identities.begin(), identities.end());

            Sha256 hash;
            HashString(hash, "HEngine.MaterialShader.ArtifactSet.v2");
            HashString(hash, ToString(backend));
            HashString(hash, ToString(binaryFormat));
            HashUnsigned64(hash, static_cast<std::uint64_t>(identities.size()));
            for (const std::string& identity : identities)
                HashString(hash, identity);
            return ToHex(hash.Finalize());
        }

        [[nodiscard]] std::string BuildCombinedArtifactIdentityHash(
            const MaterialShaderManifest& manifest)
        {
            std::vector<std::string> targetHashes;
            if (manifest.ArtifactSets.empty())
            {
                targetHashes.push_back(
                    std::string(ToString(RenderBackend::DX12)) + "\n" +
                    std::string(ToString(ShaderBinaryFormat::Dxil)) + "\n" +
                    BuildArtifactIdentityHash(
                        manifest,
                        RenderBackend::DX12,
                        ShaderBinaryFormat::Dxil));
            }
            else
            {
                targetHashes.reserve(manifest.ArtifactSets.size());
                for (const MaterialShaderArtifactSet& set : manifest.ArtifactSets)
                {
                    targetHashes.push_back(
                        std::string(ToString(set.Backend)) + "\n" +
                        std::string(ToString(set.BinaryFormat)) + "\n" +
                        BuildArtifactIdentityHash(
                            manifest, set.Backend, set.BinaryFormat));
                }
                std::sort(targetHashes.begin(), targetHashes.end());
            }

            Sha256 hash;
            HashString(hash, "HEngine.MaterialShader.AllArtifactSets.v1");
            HashUnsigned64(hash, static_cast<std::uint64_t>(targetHashes.size()));
            for (const std::string& targetHash : targetHashes)
                HashString(hash, targetHash);
            return ToHex(hash.Finalize());
        }

        [[nodiscard]] std::string BuildShaderVariantIdentity(
            const MaterialShaderArtifact& artifact)
        {
            return std::string(ToString(artifact.Pass)) + "/" +
                ToString(artifact.VertexPath) + "/" +
                ToString(artifact.DescriptorMode) + "/" + artifact.Permutation;
        }

        [[nodiscard]] MaterialProgramCandidate LoadCandidate(
            ShaderManager* shaderManager,
            const std::filesystem::path& cachesRoot,
            const SourceResolution& source,
            RenderBackend backend,
            ShaderBinaryFormat binaryFormat)
        {
            MaterialProgramCandidate candidate;
            MaterialShaderManifestLoadResult loaded =
                LoadMaterialShaderManifest(source.ManifestPath);
            candidate.Diagnostics = std::move(loaded.Diagnostics);
            if (HasErrors(candidate.Diagnostics))
                return candidate;

            candidate.Manifest = std::move(loaded.Manifest);
            const std::span<const MaterialShaderArtifact> artifacts =
                GetMaterialShaderArtifacts(candidate.Manifest, backend, binaryFormat);
            if (artifacts.empty())
            {
                AddError(candidate.Diagnostics, "MaterialProgram.ArtifactTarget",
                    "Material shader manifest has no artifact set for the active renderer backend.",
                    source.CanonicalSourcePath);
                return candidate;
            }

            std::string normalizedManifestSource;
            const Utils::PathValidationResult manifestSourceValidation =
                Utils::TryNormalizeProjectRelativePath(
                    candidate.Manifest.SourcePath, normalizedManifestSource);
            if (!manifestSourceValidation ||
                Utils::MakeProjectPathCacheKey(normalizedManifestSource) !=
                    Utils::MakeProjectPathCacheKey(source.ManifestSourcePath))
            {
                AddError(candidate.Diagnostics, "MaterialProgram.SourceMismatch",
                    "Material shader manifest SourcePath does not match the requested shader.",
                    source.CanonicalSourcePath);
                return candidate;
            }

            std::unordered_map<
                MaterialShaderHandleKey,
                std::size_t,
                MaterialShaderHandleKeyHash> typedKeys;
            candidate.Shaders.reserve(artifacts.size());
            for (const MaterialShaderArtifact& artifact : artifacts)
            {
                const auto stage = ParseStage(artifact.Stage);
                if (!stage)
                {
                    AddError(candidate.Diagnostics, "MaterialProgram.ArtifactStage",
                        "Material shader runtime supports vs, ps and cs artifacts only.",
                        artifact.RelativePath);
                    continue;
                }

                const MaterialShaderHandleKey handleKey{
                    .Pass = artifact.Pass,
                    .VertexPath = artifact.VertexPath,
                    .DescriptorMode = artifact.DescriptorMode,
                    .Stage = stage->first,
                };
                if (!typedKeys.emplace(handleKey, typedKeys.size()).second)
                {
                    AddError(candidate.Diagnostics, "MaterialProgram.ArtifactDuplicate",
                        "More than one artifact maps to the same typed material shader key.",
                        artifact.RelativePath);
                    continue;
                }

                const std::optional<std::filesystem::path> artifactPath = ResolveCachesPath(
                    cachesRoot,
                    artifact.RelativePath,
                    candidate.Diagnostics,
                    "MaterialProgram.ArtifactPath",
                    artifact.RelativePath);
                if (!artifactPath)
                    continue;

                std::error_code error;
                if (!std::filesystem::is_regular_file(*artifactPath, error) || error)
                {
                    AddError(candidate.Diagnostics, "MaterialProgram.ArtifactMissing",
                        "Compiled material shader artifact is missing.", artifact.RelativePath);
                    continue;
                }
                std::optional<LoadedArtifactFile> artifactFile = LoadArtifactFile(
                    *artifactPath, candidate.Diagnostics, artifact.RelativePath);
                if (!artifactFile)
                    continue;
                if (ToLowerAscii(artifactFile->ContentHash) !=
                    ToLowerAscii(artifact.ContentHash))
                {
                    AddError(candidate.Diagnostics, "MaterialProgram.ArtifactHash",
                        "Compiled material shader artifact content hash does not match the manifest.",
                        artifact.RelativePath);
                    continue;
                }

                const std::string variantIdentity = BuildShaderVariantIdentity(artifact);
                candidate.Shaders.push_back({
                    .Key = handleKey,
                    .CacheKey = {
                        .Source = source.CanonicalSourcePath,
                        .Variant = variantIdentity,
                        .Stage = stage->second,
                        .EntryPoint = artifact.EntryPoint,
                        .TargetProfile = artifact.TargetProfile,
                        .ContentHash = artifact.ContentHash,
                        .Backend = backend,
                        .BinaryFormat = binaryFormat,
                    },
                    .Bytecode = std::move(artifactFile->Bytes),
                    .DebugName = "Material shader " + source.CanonicalSourcePath + " " +
                        variantIdentity + " " + artifact.Stage,
                });
            }
            if (HasErrors(candidate.Diagnostics) ||
                candidate.Shaders.size() != artifacts.size())
            {
                return candidate;
            }

            if (shaderManager)
            {
                std::vector<ShaderBinaryCreateRequest> requests;
                requests.reserve(candidate.Shaders.size());
                for (const LoadedShader& shader : candidate.Shaders)
                {
                    requests.push_back({
                        .Key = shader.CacheKey,
                        .Binary = shader.Bytecode,
                        .DebugName = shader.DebugName,
                    });
                }

                try
                {
                    std::vector<nvrhi::ShaderHandle> handles =
                        shaderManager->CreateShaderBatch(requests);
                    if (handles.size() != candidate.Shaders.size())
                        throw std::logic_error("ShaderManager returned an incomplete shader batch");
                    for (std::size_t index = 0; index < handles.size(); ++index)
                        candidate.Shaders[index].Handle = std::move(handles[index]);
                }
                catch (const std::exception& exception)
                {
                    AddError(candidate.Diagnostics, "MaterialProgram.ShaderCreate",
                        "Failed to create material ShaderHandle set: " +
                            std::string(exception.what()),
                        source.CanonicalSourcePath);
                    return candidate;
                }
            }

            candidate.Identity = {
                .SourcePath = source.CanonicalSourcePath,
                .RecursiveContentHash = candidate.Manifest.RecursiveSourceHash,
                .MaterialAbiVersion = candidate.Manifest.MaterialAbiVersion,
                .BindingSignatureHash = candidate.Manifest.BindingSignatureHash,
                .ArtifactIdentityHash =
                    BuildCombinedArtifactIdentityHash(candidate.Manifest),
            };
            return candidate;
        }
    }

    std::string BuildMaterialShaderArtifactIdentityHash(
        const MaterialShaderManifest& manifest)
    {
        return BuildCombinedArtifactIdentityHash(manifest);
    }

    struct MaterialShaderManager::Impl
    {
        struct CacheEntry
        {
            std::shared_ptr<const CompiledMaterialShader> Current;
            std::deque<std::shared_ptr<const CompiledMaterialShader>> History;
            std::vector<MaterialShaderDiagnostic> LastDiagnostics;
            FileStamp ManifestStamp;
            bool bInvalidated = true;
        };

        ShaderManager* ShaderManagerInstance = nullptr;
        std::filesystem::path CachesRoot;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;
        std::mutex RefreshMutex;
        mutable std::mutex Mutex;
        std::unordered_map<std::string, CacheEntry> Entries;
        std::uint64_t PublicationRevision = 0;
    };

    std::size_t MaterialShaderHandleKeyHash::operator()(
        const MaterialShaderHandleKey& key) const noexcept
    {
        std::uint64_t value = 14695981039346656037ull;
        const auto add = [&value](std::uint64_t field)
        {
            for (std::uint32_t byteIndex = 0; byteIndex < sizeof(field); ++byteIndex)
            {
                value ^= static_cast<std::uint8_t>(field & 0xffu);
                value *= 1099511628211ull;
                field >>= 8u;
            }
        };
        add(static_cast<std::uint64_t>(key.Pass));
        add(static_cast<std::uint64_t>(key.VertexPath));
        add(static_cast<std::uint64_t>(key.DescriptorMode));
        add(static_cast<std::uint64_t>(key.Stage));
        return static_cast<std::size_t>(value);
    }

    CompiledMaterialShader::CompiledMaterialShader(
        MaterialShaderManifest manifest,
        MaterialShaderProgramIdentity identity,
        std::uint64_t generation,
        ShaderMap shaders,
        std::vector<ShaderCacheKey> shaderCacheKeys)
        : mManifest(std::move(manifest))
        , mIdentity(std::move(identity))
        , mGeneration(generation)
        , mShaders(std::move(shaders))
        , mShaderCacheKeys(std::move(shaderCacheKeys))
    {
    }

    const MaterialShaderManifest& CompiledMaterialShader::GetManifest() const noexcept
    {
        return mManifest;
    }

    const MaterialShaderProgramIdentity& CompiledMaterialShader::GetIdentity() const noexcept
    {
        return mIdentity;
    }

    std::uint64_t CompiledMaterialShader::GetGeneration() const noexcept
    {
        return mGeneration;
    }

    bool CompiledMaterialShader::HasShader(const MaterialShaderHandleKey& key) const noexcept
    {
        return mShaders.contains(key);
    }

    nvrhi::ShaderHandle CompiledMaterialShader::FindShader(
        const MaterialShaderHandleKey& key) const
    {
        const auto shader = mShaders.find(key);
        return shader == mShaders.end() ? nvrhi::ShaderHandle{} : shader->second;
    }

    std::span<const ShaderCacheKey> CompiledMaterialShader::GetShaderCacheKeys() const noexcept
    {
        return mShaderCacheKeys;
    }

    bool MaterialShaderRefreshResult::Succeeded() const noexcept
    {
        return Program && !HasErrors(Diagnostics);
    }

    MaterialShaderManager::MaterialShaderManager(
        ShaderManager* shaderManager,
        std::filesystem::path cachesRoot)
        : mImpl(std::make_unique<Impl>())
    {
        mImpl->ShaderManagerInstance = shaderManager;
        mImpl->CachesRoot = NormalizeRoot(cachesRoot);
        if (shaderManager)
        {
            mImpl->Backend = shaderManager->GetBackend();
            mImpl->BinaryFormat = shaderManager->GetBinaryFormat();
        }
    }

    MaterialShaderManager::~MaterialShaderManager() = default;

    MaterialShaderRefreshResult MaterialShaderManager::Refresh(std::string_view sourcePath)
    {
        return RefreshInternal(sourcePath, true, {});
    }

    bool MaterialShaderManager::Invalidate(std::string_view sourcePath)
    {
        std::vector<MaterialShaderDiagnostic> diagnostics;
        const std::optional<SourceResolution> source = ResolveSource(
            mImpl->CachesRoot, sourcePath, diagnostics);
        if (!source)
            return false;

        std::scoped_lock refreshLock(mImpl->RefreshMutex);
        std::scoped_lock lock(mImpl->Mutex);
        mImpl->Entries[source->CacheKey].bInvalidated = true;
        return true;
    }

    MaterialShaderRefreshResult MaterialShaderManager::RefreshIfChangedAtFrameBoundary(
        std::string_view sourcePath,
        PublishBarrier beforePublish)
    {
        return RefreshInternal(sourcePath, false, beforePublish);
    }

    MaterialShaderRefreshResult MaterialShaderManager::RefreshInternal(
        std::string_view sourcePath,
        bool bForce,
        const PublishBarrier& beforePublish)
    {
        MaterialShaderRefreshResult result;
        const std::optional<SourceResolution> source = ResolveSource(
            mImpl->CachesRoot, sourcePath, result.Diagnostics);
        if (!source)
            return result;

        std::unique_lock refreshLock(mImpl->RefreshMutex);
        const FileStamp beforeStamp = ReadFileStamp(source->ManifestPath);
        if (!bForce)
        {
            std::scoped_lock lock(mImpl->Mutex);
            const auto existing = mImpl->Entries.find(source->CacheKey);
            if (existing != mImpl->Entries.end() && existing->second.Current &&
                !existing->second.bInvalidated && existing->second.ManifestStamp == beforeStamp)
            {
                result.Program = existing->second.Current;
                result.Diagnostics = existing->second.LastDiagnostics;
                return result;
            }
        }

        MaterialProgramCandidate candidate = LoadCandidate(
            mImpl->ShaderManagerInstance,
            mImpl->CachesRoot,
            *source,
            mImpl->Backend,
            mImpl->BinaryFormat);
        const FileStamp afterStamp = ReadFileStamp(source->ManifestPath);
        result.Diagnostics = candidate.Diagnostics;
        {
            std::scoped_lock lock(mImpl->Mutex);
            Impl::CacheEntry& entry = mImpl->Entries[source->CacheKey];
            entry.ManifestStamp = afterStamp;
            entry.bInvalidated = beforeStamp != afterStamp;
            entry.LastDiagnostics = candidate.Diagnostics;

            if (HasErrors(candidate.Diagnostics))
            {
                result.Program = entry.Current;
                return result;
            }
            if (entry.Current && entry.Current->GetIdentity() == candidate.Identity)
            {
                result.Program = entry.Current;
                return result;
            }
        }

        CompiledMaterialShader::ShaderMap shaders;
        std::vector<ShaderCacheKey> shaderCacheKeys;
        shaders.reserve(candidate.Shaders.size());
        shaderCacheKeys.reserve(candidate.Shaders.size());
        for (LoadedShader& shader : candidate.Shaders)
        {
            shaders.emplace(shader.Key, std::move(shader.Handle));
            shaderCacheKeys.push_back(std::move(shader.CacheKey));
        }

        std::shared_ptr<const CompiledMaterialShader> previousCurrent;
        std::uint64_t generation = 1;
        {
            std::scoped_lock lock(mImpl->Mutex);
            previousCurrent = mImpl->Entries[source->CacheKey].Current;
            if (previousCurrent)
                generation = previousCurrent->GetGeneration() + 1u;
        }
        auto program = std::shared_ptr<const CompiledMaterialShader>(
            new CompiledMaterialShader(
                std::move(candidate.Manifest),
                std::move(candidate.Identity),
                generation,
                std::move(shaders),
                std::move(shaderCacheKeys)));

        if (previousCurrent && beforePublish)
        {
            bool bBarrierSucceeded = false;
            std::string barrierFailure;
            try
            {
                bBarrierSucceeded = beforePublish();
                if (!bBarrierSucceeded)
                    barrierFailure = "The GPU publication barrier did not reach a safe state.";
            }
            catch (const std::exception& exception)
            {
                barrierFailure =
                    "The GPU publication barrier threw an exception: " +
                    std::string(exception.what());
            }
            catch (...)
            {
                barrierFailure = "The GPU publication barrier threw an unknown exception.";
            }

            if (!bBarrierSucceeded)
            {
                AddError(
                    result.Diagnostics,
                    "MaterialProgram.PublishBarrier",
                    barrierFailure + " The last-known-good program remains current.",
                    source->CanonicalSourcePath);
                std::scoped_lock lock(mImpl->Mutex);
                Impl::CacheEntry& entry = mImpl->Entries[source->CacheKey];
                entry.LastDiagnostics = result.Diagnostics;
                entry.bInvalidated = true;
                result.Program = entry.Current;
                return result;
            }
        }

        {
            std::scoped_lock lock(mImpl->Mutex);
            Impl::CacheEntry& entry = mImpl->Entries[source->CacheKey];
            if (entry.Current)
            {
                entry.History.push_front(entry.Current);
                while (entry.History.size() > MaterialShaderGenerationHistoryCapacity)
                    entry.History.pop_back();
            }
            entry.Current = program;
            entry.LastDiagnostics = result.Diagnostics;
            ++mImpl->PublicationRevision;
            if (mImpl->PublicationRevision == 0)
                mImpl->PublicationRevision = 1;
        }
        result.Program = std::move(program);
        result.bPublished = true;
        return result;
    }

    std::uint64_t MaterialShaderManager::GetPublicationRevision() const
    {
        std::scoped_lock lock(mImpl->Mutex);
        return mImpl->PublicationRevision;
    }

    std::shared_ptr<const CompiledMaterialShader> MaterialShaderManager::GetCurrent(
        std::string_view sourcePath) const
    {
        std::vector<MaterialShaderDiagnostic> diagnostics;
        const std::optional<SourceResolution> source = ResolveSource(
            mImpl->CachesRoot, sourcePath, diagnostics);
        if (!source)
            return {};

        std::scoped_lock lock(mImpl->Mutex);
        const auto entry = mImpl->Entries.find(source->CacheKey);
        return entry == mImpl->Entries.end()
            ? std::shared_ptr<const CompiledMaterialShader>{}
            : entry->second.Current;
    }

    std::shared_ptr<const CompiledMaterialShader> MaterialShaderManager::FindCompatible(
        const MaterialShaderProgramIdentity& identity) const
    {
        std::vector<MaterialShaderDiagnostic> diagnostics;
        const std::optional<SourceResolution> source = ResolveSource(
            mImpl->CachesRoot, identity.SourcePath, diagnostics);
        if (!source)
            return {};

        const auto matches = [&identity, &source](
            const std::shared_ptr<const CompiledMaterialShader>& program)
        {
            if (!program)
                return false;
            const MaterialShaderProgramIdentity& actual = program->GetIdentity();
            return Utils::MakeProjectPathCacheKey(actual.SourcePath) == source->CacheKey &&
                actual.RecursiveContentHash == identity.RecursiveContentHash &&
                actual.MaterialAbiVersion == identity.MaterialAbiVersion &&
                actual.BindingSignatureHash == identity.BindingSignatureHash &&
                actual.ArtifactIdentityHash == identity.ArtifactIdentityHash;
        };

        std::scoped_lock lock(mImpl->Mutex);
        const auto entry = mImpl->Entries.find(source->CacheKey);
        if (entry == mImpl->Entries.end())
            return {};
        if (matches(entry->second.Current))
            return entry->second.Current;
        const auto compatible = std::ranges::find_if(entry->second.History, matches);
        return compatible == entry->second.History.end()
            ? std::shared_ptr<const CompiledMaterialShader>{}
            : *compatible;
    }

    std::vector<MaterialShaderDiagnostic> MaterialShaderManager::GetLastDiagnostics(
        std::string_view sourcePath) const
    {
        std::vector<MaterialShaderDiagnostic> diagnostics;
        const std::optional<SourceResolution> source = ResolveSource(
            mImpl->CachesRoot, sourcePath, diagnostics);
        if (!source)
            return diagnostics;

        std::scoped_lock lock(mImpl->Mutex);
        const auto entry = mImpl->Entries.find(source->CacheKey);
        return entry == mImpl->Entries.end()
            ? std::vector<MaterialShaderDiagnostic>{}
            : entry->second.LastDiagnostics;
    }

    bool RequiresMaterialShaderFrameBoundaryRefresh(
        const std::shared_ptr<const CompiledMaterialShader>& compatibleProgram) noexcept
    {
        return !compatibleProgram;
    }
}
