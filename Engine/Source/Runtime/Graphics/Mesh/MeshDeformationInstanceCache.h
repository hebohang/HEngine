#pragma once

#include "Runtime/Graphics/Mesh/Mesh.h"

#include <cstdint>
#include <stdexcept>
#include <unordered_map>

namespace HEngine
{
    // Retain one private deformable instance per live owner and source. Restoring
    // an appearance reuses its vertex buffers instead of cloning a new mesh key.
    class MeshDeformationInstanceCache final
    {
    public:
        [[nodiscard]] Ref<Mesh> Acquire(uint64_t owner, const Ref<Mesh>& source)
        {
            if (!owner || !source) throw std::invalid_argument("Mesh instance cache requires an owner and source.");
            auto& entry = mEntries[owner];
            if (!entry.Instance || entry.Source != source)
            {
                auto instance = CreateRef<Mesh>(*source);
                instance->EnsureUniqueDeformation();
                entry = { source, std::move(instance) };
            }
            return entry.Instance;
        }

        template<class IsLive>
        void RetainLiveOwners(IsLive&& isLive)
        {
            std::erase_if(mEntries, [&](const auto& entry) { return !isLive(entry.first); });
        }

    private:
        struct Entry { Ref<Mesh> Source; Ref<Mesh> Instance; };
        std::unordered_map<uint64_t, Entry> mEntries;
    };
}
