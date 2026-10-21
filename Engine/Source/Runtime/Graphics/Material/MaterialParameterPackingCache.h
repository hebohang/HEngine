#pragma once

#include "MaterialParameterPacking.h"
#include "MaterialShaderManager.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace HEngine
{
    // CPU-only, pre-descriptor-fixup cache. Scene-generation ownership and
    // pruning belong to the renderer; entries never own GPU descriptors.
    class MaterialParameterPackingCache
    {
    public:
        [[nodiscard]] const MaterialParameterPackingResult* Find(uint32_t materialId,
            const void* owner, uint64_t revision, const MaterialShaderProgramIdentity& program,
            uint64_t generation) const
        {
            if (revision == 0) return nullptr;
            const auto found = mEntries.find(materialId);
            if (found == mEntries.end()) return nullptr;
            const auto& entry = found->second;
            return entry.Owner.get() == owner && entry.Revision == revision &&
                entry.Generation == generation && entry.Program == program ? &entry.Packing : nullptr;
        }

        void Store(uint32_t materialId, std::shared_ptr<const void> owner, uint64_t revision,
            const MaterialShaderProgramIdentity& program, uint64_t generation,
            const MaterialParameterPackingResult& packing)
        {
            if (revision == 0 || !packing.Succeeded()) return;
            mEntries.insert_or_assign(materialId, Entry{
                std::move(owner), revision, program, generation, packing });
        }

        void Retain(const std::unordered_set<uint32_t>& liveMaterials)
        {
            std::erase_if(mEntries, [&](const auto& entry) { return !liveMaterials.contains(entry.first); });
        }
        void Clear() noexcept { mEntries.clear(); }

    private:
        struct Entry
        {
            std::shared_ptr<const void> Owner;
            uint64_t Revision;
            MaterialShaderProgramIdentity Program;
            uint64_t Generation;
            MaterialParameterPackingResult Packing;
        };
        std::unordered_map<uint32_t, Entry> mEntries;
    };
}
