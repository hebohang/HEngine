#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Templates/PublicSingleton.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    class Material;

    // Canonical runtime ownership boundary for serialized material resources.
    // Entries are weak so unused materials are never kept alive by the cache.
    class MaterialResourceCache final : public PublicSingleton<MaterialResourceCache>
    {
    public:
        MaterialResourceCache() = default;

        [[nodiscard]] Ref<Material> Get(std::string_view materialPath);
        [[nodiscard]] Ref<Material> Reload(std::string_view materialPath);
        // Publishes an Editor draft or other already-deserialized instance.
        // An existing live cached object is updated in place. When none is
        // live, the draft stays private and a future Get reloads saved content.
        [[nodiscard]] Ref<Material> Publish(
            std::string_view materialPath,
            const Ref<Material>& material);
        [[nodiscard]] bool Invalidate(std::string_view materialPath);
        void Clear();

        // Returns strong references captured under the cache lock. Callers may
        // safely inspect or refresh the returned materials after the lock is gone.
        [[nodiscard]] std::vector<Ref<Material>> GetLiveMaterialsForShader(
            std::string_view shaderPath);
        // Returns the number of successfully refreshed materials. Diagnostics
        // contain only path-validation or per-material failures.
        [[nodiscard]] std::size_t RefreshShaderMaterials(
            std::string_view shaderPath,
            std::vector<std::string>* diagnostics = nullptr);

    private:
        [[nodiscard]] Ref<Material> PublishInPlace(
            const std::string& key,
            const Ref<Material>& material,
            bool bCacheWhenMissing);

        mutable std::mutex mMutex;
        std::unordered_map<std::string, std::weak_ptr<Material>> mMaterials;
    };
}
