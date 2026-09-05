#include "Hepch.h"

#include "Runtime/Graphics/Material/MaterialResourceCache.h"

#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    namespace
    {
        struct CanonicalPath
        {
            std::string Value;
            std::string Key;
        };

        [[nodiscard]] std::string ToLowerCopy(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](char character)
            {
                if (character >= 'A' && character <= 'Z')
                    return static_cast<char>(character - 'A' + 'a');
                return character;
            });
            return value;
        }

        [[nodiscard]] CanonicalPath CanonicalizeMaterialPath(std::string_view materialPath)
        {
            std::string normalized;
            const Utils::PathValidationResult validation =
                Utils::TryNormalizeProjectRelativePath(materialPath, normalized);
            if (!validation)
            {
                throw std::invalid_argument(
                    "Material resource path is not a safe content-relative path: " +
                    validation.Message);
            }

            if (ToLowerCopy(Utils::PathToUtf8(
                    Utils::Utf8ToPath(normalized).extension())) != ".hmat")
            {
                throw std::invalid_argument(
                    "Material resource path must use the .hmat extension: " + normalized);
            }

            const std::string key = Utils::MakeProjectPathCacheKey(normalized);
            return {
                .Value = std::move(normalized),
                .Key = key,
            };
        }

        [[nodiscard]] CanonicalPath CanonicalizeShaderPath(std::string_view shaderPath)
        {
            std::string normalized;
            const Utils::PathValidationResult validation =
                Utils::TryNormalizeProjectRelativePath(shaderPath, normalized);
            if (!validation)
            {
                throw std::invalid_argument(
                    "Material shader path is not a safe content-relative path: " +
                    validation.Message);
            }

            const std::string lower = ToLowerCopy(normalized);
            if (lower.starts_with("assets/shaders/"))
                normalized.erase(0, std::string_view("Assets/").size());

            const std::string canonicalLower = ToLowerCopy(normalized);
            if (!canonicalLower.starts_with("shaders/") ||
                ToLowerCopy(Utils::PathToUtf8(
                    Utils::Utf8ToPath(normalized).extension())) != ".hlsl")
            {
                throw std::invalid_argument(
                    "Material shader path must be a Shaders-relative .hlsl path: " +
                    normalized);
            }

            const std::string key = Utils::MakeProjectPathCacheKey(normalized);
            return {
                .Value = std::move(normalized),
                .Key = key,
            };
        }

        void AddDiagnostic(
            std::vector<std::string>* diagnostics,
            std::string message)
        {
            if (diagnostics)
                diagnostics->push_back(std::move(message));
        }
    }

    Ref<Material> MaterialResourceCache::Get(std::string_view materialPath)
    {
        const CanonicalPath path = CanonicalizeMaterialPath(materialPath);
        {
            std::scoped_lock lock(mMutex);
            if (const auto iterator = mMaterials.find(path.Key);
                iterator != mMaterials.end())
            {
                if (Ref<Material> material = iterator->second.lock())
                    return material;
                mMaterials.erase(iterator);
            }
        }

        // Deserialization and physical-root validation happen at Material's
        // load boundary without serializing unrelated cache requests.
        Ref<Material> material = CreateRef<Material>(path.Value);
        std::scoped_lock lock(mMutex);
        if (const auto iterator = mMaterials.find(path.Key);
            iterator != mMaterials.end())
        {
            if (Ref<Material> published = iterator->second.lock())
                return published;
            mMaterials.erase(iterator);
        }
        mMaterials.emplace(path.Key, material);
        return material;
    }

    Ref<Material> MaterialResourceCache::Reload(std::string_view materialPath)
    {
        const CanonicalPath path = CanonicalizeMaterialPath(materialPath);
        // Load first so a failed reload preserves the live cached state and so
        // no filesystem IO occurs under the cache lock.
        Ref<Material> material = CreateRef<Material>(path.Value);
        return PublishInPlace(path.Key, material, true);
    }

    Ref<Material> MaterialResourceCache::Publish(
        std::string_view materialPath,
        const Ref<Material>& material)
    {
        if (!material)
            throw std::invalid_argument("Cannot publish a null material resource.");

        const CanonicalPath path = CanonicalizeMaterialPath(materialPath);
        const CanonicalPath source =
            CanonicalizeMaterialPath(material->GetResourcePath());
        if (source.Key != path.Key)
        {
            throw std::invalid_argument(
                "Published material resource identity does not match its cache path.");
        }
        // Editor drafts remain privately owned when no live cache object
        // exists. This prevents later unsaved edits from leaking through Get;
        // the next request will deserialize the just-saved disk document.
        return PublishInPlace(path.Key, material, false);
    }

    Ref<Material> MaterialResourceCache::PublishInPlace(
        const std::string& key,
        const Ref<Material>& material,
        bool bCacheWhenMissing)
    {
        std::scoped_lock lock(mMutex);
        if (const auto iterator = mMaterials.find(key);
            iterator != mMaterials.end())
        {
            if (Ref<Material> live = iterator->second.lock())
            {
                if (live != material)
                    live->PublishRuntimeStateFrom(*material);
                return live;
            }
            mMaterials.erase(iterator);
        }

        if (bCacheWhenMissing)
            mMaterials.emplace(key, material);
        return material;
    }

    bool MaterialResourceCache::Invalidate(std::string_view materialPath)
    {
        try
        {
            const CanonicalPath path = CanonicalizeMaterialPath(materialPath);
            std::scoped_lock lock(mMutex);
            return mMaterials.erase(path.Key) != 0;
        }
        catch (const std::invalid_argument&)
        {
            return false;
        }
    }

    void MaterialResourceCache::Clear()
    {
        std::scoped_lock lock(mMutex);
        mMaterials.clear();
    }

    std::vector<Ref<Material>> MaterialResourceCache::GetLiveMaterialsForShader(
        std::string_view shaderPath)
    {
        const CanonicalPath shader = CanonicalizeShaderPath(shaderPath);
        std::vector<Ref<Material>> liveMaterials;

        {
            std::scoped_lock lock(mMutex);
            for (auto iterator = mMaterials.begin(); iterator != mMaterials.end();)
            {
                Ref<Material> material = iterator->second.lock();
                if (!material)
                {
                    iterator = mMaterials.erase(iterator);
                    continue;
                }

                liveMaterials.push_back(std::move(material));
                ++iterator;
            }
        }

        std::vector<Ref<Material>> materials;
        materials.reserve(liveMaterials.size());
        for (Ref<Material>& material : liveMaterials)
        {
            if (Utils::MakeProjectPathCacheKey(material->GetShaderPath()) == shader.Key)
                materials.push_back(std::move(material));
        }
        return materials;
    }

    std::size_t MaterialResourceCache::RefreshShaderMaterials(
        std::string_view shaderPath,
        std::vector<std::string>* diagnostics)
    {
        if (diagnostics)
            diagnostics->clear();

        std::vector<Ref<Material>> materials;
        try
        {
            materials = GetLiveMaterialsForShader(shaderPath);
        }
        catch (const std::exception& exception)
        {
            AddDiagnostic(diagnostics, exception.what());
            return 0;
        }

        std::size_t refreshedCount = 0;
        for (const Ref<Material>& material : materials)
        {
            std::string error;
            if (material->RefreshShaderLayoutFromManifest(&error))
            {
                ++refreshedCount;
                continue;
            }

            AddDiagnostic(
                diagnostics,
                material->GetResourcePath() + ": " +
                    (error.empty() ? "Material shader layout refresh failed." : error));
        }
        return refreshedCount;
    }
}
