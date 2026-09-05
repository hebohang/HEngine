#pragma once 

#include "Runtime/Core/Templates/PublicSingleton.h"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace HEngine
{
    template <template <typename> typename Derived, typename LibType>
    class LibraryBase : public PublicSingleton<Derived<LibType>>
    {
    public:
        void Add(const std::string& name, const Ref<LibType>& mem)
        {
            HE_CORE_ASSERT(mLibrary.find(name) == mLibrary.end() && "Already have this member in Library!");
            mLibrary[name] = mem;
        }
        void Set(const std::string& name, const Ref<LibType>& mem)
        {
            HE_CORE_ASSERT(mLibrary.find(name) != mLibrary.end() && "Can't find this member in Library!");
            mLibrary[name] = mem;
        }
        [[nodiscard]] Ref<LibType> Get(const std::string& name)
        {
            const auto it = mLibrary.find(name);
            HE_CORE_ASSERT(it != mLibrary.end() && "Can't find this member in Library!");
            return it != mLibrary.end() ? it->second : nullptr;
        }
        [[nodiscard]] bool Contains(const std::string& name) const
        {
            return mLibrary.contains(name);
        }
        bool Remove(const std::string& name)
        {
            return mLibrary.erase(name) != 0;
        }
        void Clear()
        {
            mLibrary.clear();
        }
        [[nodiscard]] std::size_t Size() const
        {
            return mLibrary.size();
        }

    protected:
        std::unordered_map<std::string, Ref<LibType>> mLibrary;
    };

    template <typename LibType>
    class Library : public LibraryBase<Library, LibType>
    {
    public:
        Library();
    };
}
