#pragma once

#include "Runtime/Core/Base/Base.h"

#include <concepts>

namespace HEngine
{
    // refer to https://github.com/mateeeeeee/Adria-DX12/tree/master/Adria/Graphics/Releasable.h
    // license: MIT
    template<typename T>
    concept Releasable = requires (T t)
    {
        {t.Release()};
    };

    struct ReleaseDeleter
    {
        template<Releasable T>
        void operator()(T* allocation)
        {
            allocation->Release();
        }
    };

    template<Releasable T>
    using ReleasablePtr = std::unique_ptr<T, ReleaseDeleter>;

    struct ReleasableObject
    {
        virtual void Release() = 0;
        virtual ~ReleasableObject() = default;
    };

    template<Releasable T>
    struct ReleasableResource : ReleasableObject
    {
        ReleasableResource(T* r)
        {
            resource = r;
        }

        virtual ~ReleasableResource()
        {
            Release();
        }

        virtual void Release() override
        {
            if (resource) resource->Release();
        }

        T* resource;
    };

    struct ReleasableItem
    {
        std::unique_ptr<ReleasableObject> obj;
        UINT64 fence_value;

        ReleasableItem(ReleasableObject* obj, UINT64 fence_value) : obj(obj), fence_value(fence_value) {}
    };
}
