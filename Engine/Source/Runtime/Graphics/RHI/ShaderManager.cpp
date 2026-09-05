#include "Hepch.h"

#include "ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace HEngine
{
    namespace
    {
        constexpr uint64_t FnvOffset = 14695981039346656037ull;
        constexpr uint64_t FnvPrime = 1099511628211ull;

        void HashBytes(uint64_t& hash, const void* data, size_t size) noexcept
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t index = 0; index < size; ++index)
            {
                hash ^= bytes[index];
                hash *= FnvPrime;
            }
        }

        void HashString(uint64_t& hash, std::string_view value) noexcept
        {
            HashBytes(hash, value.data(), value.size());
            const uint8_t terminator = 0xff;
            HashBytes(hash, &terminator, sizeof(terminator));
        }

        [[nodiscard]] uint64_t Fingerprint(std::span<const std::byte> bytecode) noexcept
        {
            uint64_t hash = FnvOffset;
            HashBytes(hash, bytecode.data(), bytecode.size());
            return hash;
        }

        [[nodiscard]] const char* StageName(nvrhi::ShaderType stage) noexcept
        {
            switch (stage)
            {
                case nvrhi::ShaderType::Compute: return "Compute";
                case nvrhi::ShaderType::Vertex: return "Vertex";
                case nvrhi::ShaderType::Hull: return "Hull";
                case nvrhi::ShaderType::Domain: return "Domain";
                case nvrhi::ShaderType::Geometry: return "Geometry";
                case nvrhi::ShaderType::Pixel: return "Pixel";
                case nvrhi::ShaderType::Amplification: return "Amplification";
                case nvrhi::ShaderType::Mesh: return "Mesh";
                case nvrhi::ShaderType::RayGeneration: return "RayGeneration";
                case nvrhi::ShaderType::AnyHit: return "AnyHit";
                case nvrhi::ShaderType::ClosestHit: return "ClosestHit";
                case nvrhi::ShaderType::Miss: return "Miss";
                case nvrhi::ShaderType::Intersection: return "Intersection";
                case nvrhi::ShaderType::Callable: return "Callable";
                default: return "Invalid";
            }
        }

        [[nodiscard]] std::string Describe(const ShaderCacheKey& key)
        {
            std::ostringstream stream;
            stream << "source='" << key.Source
                << "', variant='" << (key.Variant.empty() ? "<default>" : key.Variant)
                << "', stage=" << StageName(key.Stage)
                << " (" << static_cast<uint32_t>(key.Stage) << ')'
                << ", entry='" << key.EntryPoint
                << "', profile='" << key.TargetProfile
                << "', hash='" << key.ContentHash
                << "', backend=" << ToString(key.Backend)
                << ", format=" << ToString(key.BinaryFormat);
            return stream.str();
        }

        [[nodiscard]] std::string Describe(const ShaderLibraryCacheKey& key)
        {
            std::ostringstream stream;
            stream << "source='" << key.Source
                << "', variant='" << (key.Variant.empty() ? "<default>" : key.Variant)
                << "', profile='" << key.TargetProfile
                << "', hash='" << key.ContentHash
                << "', backend=" << ToString(key.Backend)
                << ", format=" << ToString(key.BinaryFormat);
            return stream.str();
        }

        void ValidateArtifactTarget(
            RenderBackend backend,
            ShaderBinaryFormat binaryFormat,
            std::string_view description)
        {
            if (!IsShaderArtifactTargetCompatible(backend, binaryFormat))
            {
                throw std::invalid_argument(
                    "Shader binary format does not match its backend target (" +
                    std::string(description) + ')');
            }
        }

        void ValidateKey(const ShaderCacheKey& key)
        {
            if (key.Source.empty())
                throw std::invalid_argument("Shader cache key source must not be empty");
            if (key.EntryPoint.empty())
                throw std::invalid_argument("Shader cache key entry point must not be empty (" + Describe(key) + ')');
            if (key.TargetProfile.empty())
                throw std::invalid_argument("Shader cache key target profile must not be empty (" + Describe(key) + ')');
            if (key.ContentHash.empty())
                throw std::invalid_argument("Shader cache key content hash must not be empty (" + Describe(key) + ')');

            const uint32_t stage = static_cast<uint32_t>(key.Stage);
            if (stage == 0 || (stage & (stage - 1)) != 0 ||
                std::string_view(StageName(key.Stage)) == "Invalid")
                throw std::invalid_argument("Shader cache key must contain exactly one shader stage (" + Describe(key) + ')');

            ValidateArtifactTarget(key.Backend, key.BinaryFormat, Describe(key));
        }

        void ValidateLibraryKey(const ShaderLibraryCacheKey& key)
        {
            if (key.Source.empty())
                throw std::invalid_argument("Shader library cache key source must not be empty");
            if (key.TargetProfile.empty())
            {
                throw std::invalid_argument(
                    "Shader library cache key target profile must not be empty (" +
                    Describe(key) + ')');
            }
            if (key.ContentHash.empty())
            {
                throw std::invalid_argument(
                    "Shader library cache key content hash must not be empty (" +
                    Describe(key) + ')');
            }
            ValidateArtifactTarget(key.Backend, key.BinaryFormat, Describe(key));
        }

        void ValidateDeviceTarget(
            nvrhi::IDevice* device,
            RenderBackend backend,
            std::string_view description)
        {
            const nvrhi::GraphicsAPI graphicsApi = device->getGraphicsAPI();
            const bool bMatches =
                (backend == RenderBackend::DX12 && graphicsApi == nvrhi::GraphicsAPI::D3D12) ||
                (backend == RenderBackend::Vulkan && graphicsApi == nvrhi::GraphicsAPI::VULKAN);
            if (!bMatches)
            {
                throw std::invalid_argument(
                    "Shader backend target does not match the NVRHI device (" +
                    std::string(description) + ')');
            }
        }

        [[nodiscard]] bool IsRayTracingStage(nvrhi::ShaderType stage) noexcept
        {
            switch (stage)
            {
                case nvrhi::ShaderType::RayGeneration:
                case nvrhi::ShaderType::Miss:
                case nvrhi::ShaderType::ClosestHit:
                case nvrhi::ShaderType::AnyHit:
                case nvrhi::ShaderType::Intersection:
                case nvrhi::ShaderType::Callable:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] std::string EffectiveDebugName(
            const ShaderCacheKey& key,
            std::string_view debugName)
        {
            if (!debugName.empty())
                return std::string(debugName);

            std::string result = key.Source;
            if (!key.Variant.empty())
                result += "[" + key.Variant + "]";
            result += ":" + key.EntryPoint + "/" + key.TargetProfile;
            return result;
        }
    }

    ShaderManager::ShaderManager(nvrhi::IDevice* device)
        : mDevice(device)
    {
        if (!mDevice)
            throw std::invalid_argument("ShaderManager requires a valid NVRHI device");
    }

    nvrhi::ShaderHandle ShaderManager::LoadShaderFile(
        const ShaderCacheKey& key,
        const std::filesystem::path& binaryPath,
        std::string_view debugName)
    {
        ValidateKey(key);
        ValidateDeviceTarget(mDevice, key.Backend, Describe(key));
        if (binaryPath.empty())
            throw std::invalid_argument("Shader binary path must not be empty (" + Describe(key) + ')');

        if (nvrhi::ShaderHandle cached = Find(key))
            return cached;

        std::ifstream stream(binaryPath, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            throw std::runtime_error(
                "Failed to open shader binary file '" + Utils::PathToUtf8(binaryPath) +
                "' (" + Describe(key) + ')');
        }

        const std::streamoff byteCount = stream.tellg();
        if (byteCount <= 0 ||
            static_cast<uint64_t>(byteCount) > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        {
            throw std::runtime_error(
                "Shader binary file is empty or too large '" + Utils::PathToUtf8(binaryPath) +
                "' (" + Describe(key) + ')');
        }

        std::vector<std::byte> bytecode(static_cast<size_t>(byteCount));
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(bytecode.data()), byteCount);
        if (!stream)
        {
            throw std::runtime_error(
                "Failed to read shader binary file '" + Utils::PathToUtf8(binaryPath) +
                "' (" + Describe(key) + ')');
        }

        return CreateShader(key, bytecode, debugName);
    }

    nvrhi::ShaderHandle ShaderManager::CreateShader(
        const ShaderCacheKey& key,
        std::span<const std::byte> binary,
        std::string_view debugName)
    {
        const ShaderBinaryCreateRequest request{
            .Key = key,
            .Binary = binary,
            .DebugName = std::string(debugName),
        };
        std::vector<nvrhi::ShaderHandle> shaders = CreateShaderBatch(
            std::span<const ShaderBinaryCreateRequest>(&request, 1));
        return std::move(shaders.front());
    }

    std::vector<nvrhi::ShaderHandle> ShaderManager::CreateShaderBatch(
        std::span<const ShaderBinaryCreateRequest> requests)
    {
        for (const ShaderBinaryCreateRequest& request : requests)
        {
            ValidateKey(request.Key);
            ValidateDeviceTarget(mDevice, request.Key.Backend, Describe(request.Key));
            if (request.Binary.empty())
            {
                throw std::invalid_argument(
                    "Shader binary must not be empty (" + Describe(request.Key) + ')');
            }
        }

        std::vector<nvrhi::ShaderHandle> shaders;
        std::vector<size_t> insertedRequestIndices;
        shaders.reserve(requests.size());
        insertedRequestIndices.reserve(requests.size());

        std::scoped_lock lock(mMutex);
        try
        {
            for (size_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex)
            {
                const ShaderBinaryCreateRequest& request = requests[requestIndex];
                const uint64_t bytecodeFingerprint = Fingerprint(request.Binary);
                if (const auto it = mShaders.find(request.Key); it != mShaders.end())
                {
#ifdef HE_DEBUG
                    if (it->second.BytecodeFingerprint != bytecodeFingerprint)
                    {
                        throw std::logic_error(
                            "ShaderManager cache key collision with different binary bytes (" +
                            Describe(request.Key) + ')');
                    }
#endif
                    shaders.push_back(it->second.Shader);
                    continue;
                }

                const std::string resolvedDebugName = EffectiveDebugName(
                    request.Key, request.DebugName);
                nvrhi::ShaderDesc desc;
                desc.shaderType = request.Key.Stage;
                desc.entryName = request.Key.EntryPoint;
                desc.debugName = resolvedDebugName;

                nvrhi::ShaderHandle shader;
                try
                {
                    shader = mDevice->createShader(
                        desc, request.Binary.data(), request.Binary.size());
                }
                catch (const std::exception& exception)
                {
                    throw std::runtime_error(
                        "NVRHI threw while creating shader (" + Describe(request.Key) +
                        ", debugName='" + resolvedDebugName + "'): " + exception.what());
                }
                catch (...)
                {
                    throw std::runtime_error(
                        "NVRHI threw an unknown exception while creating shader (" +
                        Describe(request.Key) + ", debugName='" + resolvedDebugName + "')");
                }

                if (!shader)
                {
                    throw std::runtime_error(
                        "NVRHI failed to create shader (" + Describe(request.Key) +
                        ", debugName='" + resolvedDebugName + "')");
                }

                mShaders.emplace(
                    request.Key,
                    CacheEntry{
                        .Shader = shader,
                        .BytecodeFingerprint = bytecodeFingerprint,
                    });
                insertedRequestIndices.push_back(requestIndex);
                shaders.push_back(std::move(shader));
            }
        }
        catch (...)
        {
            for (const size_t requestIndex : insertedRequestIndices)
                mShaders.erase(requests[requestIndex].Key);
            throw;
        }
        return shaders;
    }

    nvrhi::ShaderLibraryHandle ShaderManager::LoadShaderLibraryFile(
        const ShaderLibraryCacheKey& key,
        const std::filesystem::path& binaryPath)
    {
        ValidateLibraryKey(key);
        ValidateDeviceTarget(mDevice, key.Backend, Describe(key));
        if (binaryPath.empty())
        {
            throw std::invalid_argument(
                "Shader library binary path must not be empty (" + Describe(key) + ')');
        }

        if (nvrhi::ShaderLibraryHandle cached = FindLibrary(key))
            return cached;

        std::ifstream stream(binaryPath, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            throw std::runtime_error(
                "Failed to open shader library binary file '" +
                Utils::PathToUtf8(binaryPath) + "' (" + Describe(key) + ')');
        }

        const std::streamoff byteCount = stream.tellg();
        if (byteCount <= 0 ||
            static_cast<uint64_t>(byteCount) > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        {
            throw std::runtime_error(
                "Shader library binary file is empty or too large '" +
                Utils::PathToUtf8(binaryPath) + "' (" + Describe(key) + ')');
        }

        std::vector<std::byte> binary(static_cast<size_t>(byteCount));
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(binary.data()), byteCount);
        if (!stream)
        {
            throw std::runtime_error(
                "Failed to read shader library binary file '" +
                Utils::PathToUtf8(binaryPath) + "' (" + Describe(key) + ')');
        }
        return CreateShaderLibrary(key, binary);
    }

    nvrhi::ShaderLibraryHandle ShaderManager::CreateShaderLibrary(
        const ShaderLibraryCacheKey& key,
        std::span<const std::byte> binary)
    {
        ValidateLibraryKey(key);
        ValidateDeviceTarget(mDevice, key.Backend, Describe(key));
        if (binary.empty())
        {
            throw std::invalid_argument(
                "Shader library binary must not be empty (" + Describe(key) + ')');
        }

        const uint64_t bytecodeFingerprint = Fingerprint(binary);
        std::scoped_lock lock(mMutex);
        if (const auto it = mLibraries.find(key); it != mLibraries.end())
        {
#ifdef HE_DEBUG
            if (it->second.BytecodeFingerprint != bytecodeFingerprint)
            {
                throw std::logic_error(
                    "ShaderManager library cache key collision with different binary bytes (" +
                    Describe(key) + ')');
            }
#endif
            return it->second.Library;
        }

        nvrhi::ShaderLibraryHandle library;
        try
        {
            library = mDevice->createShaderLibrary(binary.data(), binary.size());
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                "NVRHI threw while creating shader library (" + Describe(key) +
                "): " + exception.what());
        }
        catch (...)
        {
            throw std::runtime_error(
                "NVRHI threw an unknown exception while creating shader library (" +
                Describe(key) + ')');
        }

        if (!library)
        {
            throw std::runtime_error(
                "NVRHI failed to create shader library (" + Describe(key) + ')');
        }

        mLibraries.emplace(
            key,
            LibraryCacheEntry{
                .Library = library,
                .BytecodeFingerprint = bytecodeFingerprint,
            });
        return library;
    }

    nvrhi::ShaderHandle ShaderManager::GetShaderFromLibrary(
        const ShaderLibraryCacheKey& key,
        std::string_view entryPoint,
        nvrhi::ShaderType stage) const
    {
        ValidateLibraryKey(key);
        ValidateDeviceTarget(mDevice, key.Backend, Describe(key));
        if (entryPoint.empty())
        {
            throw std::invalid_argument(
                "Shader library entry point must not be empty (" + Describe(key) + ')');
        }
        if (!IsRayTracingStage(stage))
        {
            throw std::invalid_argument(
                "Shader library entry must use one RayGeneration/Miss/ClosestHit/AnyHit/"
                "Intersection/Callable stage (" + Describe(key) + ')');
        }

        nvrhi::ShaderLibraryHandle library = FindLibrary(key);
        if (!library)
            throw std::out_of_range("Shader library is not loaded (" + Describe(key) + ')');

        const std::string entry(entryPoint);
        nvrhi::ShaderHandle shader;
        try
        {
            shader = library->getShader(entry.c_str(), stage);
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                "NVRHI threw while resolving shader library entry '" + entry + "' (" +
                Describe(key) + "): " + exception.what());
        }
        catch (...)
        {
            throw std::runtime_error(
                "NVRHI threw an unknown exception while resolving shader library entry '" +
                entry + "' (" + Describe(key) + ')');
        }
        if (!shader)
        {
            throw std::runtime_error(
                "NVRHI failed to resolve shader library entry '" + entry + "' (" +
                Describe(key) + ')');
        }
        return shader;
    }

    nvrhi::ShaderHandle ShaderManager::LoadDxilFile(
        const ShaderCacheKey& key,
        const std::filesystem::path& dxilPath,
        std::string_view debugName)
    {
        if (key.Backend != RenderBackend::DX12 ||
            key.BinaryFormat != ShaderBinaryFormat::Dxil)
        {
            throw std::invalid_argument(
                "LoadDxilFile requires a DX12/DXIL shader key (" + Describe(key) + ')');
        }
        return LoadShaderFile(key, dxilPath, debugName);
    }

    nvrhi::ShaderHandle ShaderManager::CreateDxil(
        const ShaderCacheKey& key,
        std::span<const std::byte> dxil,
        std::string_view debugName)
    {
        if (key.Backend != RenderBackend::DX12 ||
            key.BinaryFormat != ShaderBinaryFormat::Dxil)
        {
            throw std::invalid_argument(
                "CreateDxil requires a DX12/DXIL shader key (" + Describe(key) + ')');
        }
        return CreateShader(key, dxil, debugName);
    }

    std::vector<nvrhi::ShaderHandle> ShaderManager::CreateDxilBatch(
        std::span<const ShaderDxilCreateRequest> requests)
    {
        std::vector<ShaderBinaryCreateRequest> neutralRequests;
        neutralRequests.reserve(requests.size());
        for (const ShaderDxilCreateRequest& request : requests)
        {
            if (request.Key.Backend != RenderBackend::DX12 ||
                request.Key.BinaryFormat != ShaderBinaryFormat::Dxil)
            {
                throw std::invalid_argument(
                    "CreateDxilBatch requires DX12/DXIL shader keys (" +
                    Describe(request.Key) + ')');
            }
            neutralRequests.push_back({
                .Key = request.Key,
                .Binary = request.Dxil,
                .DebugName = request.DebugName,
            });
        }
        return CreateShaderBatch(neutralRequests);
    }

    bool ShaderManager::Contains(const ShaderCacheKey& key) const
    {
        std::scoped_lock lock(mMutex);
        return mShaders.contains(key);
    }

    size_t ShaderManager::Size() const
    {
        std::scoped_lock lock(mMutex);
        return mShaders.size();
    }

    RenderBackend ShaderManager::GetBackend() const noexcept
    {
        return mDevice->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN
            ? RenderBackend::Vulkan
            : RenderBackend::DX12;
    }

    ShaderBinaryFormat ShaderManager::GetBinaryFormat() const noexcept
    {
        return GetShaderBinaryFormat(GetBackend());
    }

    void ShaderManager::RemoveSourceExcept(
        std::string_view source,
        std::span<const ShaderCacheKey> retainedKeys)
    {
        const std::string canonicalSource = Utils::MakeProjectPathCacheKey(source);
        std::scoped_lock lock(mMutex);
        for (auto iterator = mShaders.begin(); iterator != mShaders.end();)
        {
            const bool bRetained = std::ranges::any_of(
                retainedKeys,
                [&cachedKey = iterator->first](const ShaderCacheKey& retainedKey)
                {
                    return ShaderCacheKeysMatchCanonicalIdentity(cachedKey, retainedKey);
                });
            if (Utils::MakeProjectPathCacheKey(iterator->first.Source) == canonicalSource &&
                !bRetained)
            {
                iterator = mShaders.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
    }

    bool ShaderCacheKeysMatchCanonicalIdentity(
        const ShaderCacheKey& left,
        const ShaderCacheKey& right)
    {
        return Utils::MakeProjectPathCacheKey(left.Source) ==
                Utils::MakeProjectPathCacheKey(right.Source) &&
            left.Variant == right.Variant &&
            left.Stage == right.Stage &&
            left.EntryPoint == right.EntryPoint &&
            left.TargetProfile == right.TargetProfile &&
            left.ContentHash == right.ContentHash &&
            left.Backend == right.Backend &&
            left.BinaryFormat == right.BinaryFormat;
    }

    void ShaderManager::Clear()
    {
        std::scoped_lock lock(mMutex);
        mShaders.clear();
        mLibraries.clear();
    }

    size_t ShaderCacheKeyHash::operator()(const ShaderCacheKey& key) const noexcept
    {
        uint64_t hash = FnvOffset;
        HashString(hash, key.Source);
        HashString(hash, key.Variant);
        const uint32_t stage = static_cast<uint32_t>(key.Stage);
        HashBytes(hash, &stage, sizeof(stage));
        HashString(hash, key.EntryPoint);
        HashString(hash, key.TargetProfile);
        HashString(hash, key.ContentHash);
        const uint8_t backend = static_cast<uint8_t>(key.Backend);
        HashBytes(hash, &backend, sizeof(backend));
        const uint8_t binaryFormat = static_cast<uint8_t>(key.BinaryFormat);
        HashBytes(hash, &binaryFormat, sizeof(binaryFormat));
        return static_cast<size_t>(hash);
    }

    size_t ShaderLibraryCacheKeyHash::operator()(
        const ShaderLibraryCacheKey& key) const noexcept
    {
        uint64_t hash = FnvOffset;
        HashString(hash, key.Source);
        HashString(hash, key.Variant);
        HashString(hash, key.TargetProfile);
        HashString(hash, key.ContentHash);
        const uint8_t backend = static_cast<uint8_t>(key.Backend);
        HashBytes(hash, &backend, sizeof(backend));
        const uint8_t binaryFormat = static_cast<uint8_t>(key.BinaryFormat);
        HashBytes(hash, &binaryFormat, sizeof(binaryFormat));
        return static_cast<size_t>(hash);
    }

    nvrhi::ShaderHandle ShaderManager::Find(const ShaderCacheKey& key) const
    {
        std::scoped_lock lock(mMutex);
        const auto it = mShaders.find(key);
        return it == mShaders.end() ? nvrhi::ShaderHandle{} : it->second.Shader;
    }

    nvrhi::ShaderLibraryHandle ShaderManager::FindLibrary(
        const ShaderLibraryCacheKey& key) const
    {
        std::scoped_lock lock(mMutex);
        const auto it = mLibraries.find(key);
        return it == mLibraries.end()
            ? nvrhi::ShaderLibraryHandle{}
            : it->second.Library;
    }
}
