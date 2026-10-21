#pragma once
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Resource/ResType/Mesh.h"

namespace HEngine
{
    // Both parsing frontends emit the same tightly packed native accessor layout.
    inline int AddAccessor(MeshRes& meshRes, int bufferIndex, size_t byteOffset,
        size_t byteLength, int componentType, const std::string& type, size_t count)
    {
        MeshBufferView view;
        view.bufferIndex = bufferIndex;
        view.byteOffset = byteOffset;
        view.byteLength = byteLength;
        const int viewIndex = static_cast<int>(meshRes.bufferViewVec.size());
        meshRes.bufferViewVec.push_back(view);
        Accessor accessor;
        accessor.bufferViewIndex = viewIndex;
        accessor.componentType = componentType;
        accessor.count = count;
        accessor.type = type;
        const int index = static_cast<int>(meshRes.accessorVec.size());
        meshRes.accessorVec.push_back(accessor);
        return index;
    }
}
