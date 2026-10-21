#pragma once

#include "Runtime/Core/Base/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace HEngine
{
    struct SceneGeometryArenaDesc
    {
        nvrhi::IDevice* Device = nullptr;
        uint64_t VertexPageBytes = 48ull * 1024ull * 1024ull;
        uint64_t IndexPageBytes = 16ull * 1024ull * 1024ull;
    };
    struct SceneGeometryArenaAllocation
    {
        uint32_t PageIndex = ~0u;
        uint32_t FirstVertex = 0;
        uint32_t FirstIndex = 0;
        uint32_t VertexCount = 0;
        uint32_t IndexCount = 0;
        uint64_t Generation = 0;
        [[nodiscard]] explicit operator bool() const noexcept { return PageIndex != ~0u; }
    };
    struct SceneGeometryArenaPage
    {
        nvrhi::BufferHandle VertexBuffer;
        nvrhi::BufferHandle IndexBuffer;
        uint32_t VertexStride = 0;
        uint32_t VertexCapacity = 0;
        uint32_t IndexCapacity = 0;
        uint32_t UsedVertices = 0;
        uint32_t UsedIndices = 0;
        bool bSubmitted = false;
    };
    struct SceneGeometryArenaStats
    {
        uint64_t ReservedBytes = 0;
        uint64_t UsedBytes = 0;
        uint64_t RetiredBytes = 0;
        uint64_t UploadedBytes = 0; // lifetime total, including repeated uploads after abort
        uint32_t PageCount = 0;
    };
    // Allocation ownership remains in the authoritative MeshEntry. Pages never
    // move old offsets; scene reset retires complete pages after their fence.
    // The caller imports page buffers into its existing SceneUpload graph pass
    // as CopyDest, then declares VertexBuffer/IndexBuffer use before drawing.
    class SceneGeometryArena final
    {
    public:
        explicit SceneGeometryArena(const SceneGeometryArenaDesc& desc);
        ~SceneGeometryArena();
        [[nodiscard]] SceneGeometryArenaAllocation Allocate(
            uint32_t vertexCount, uint32_t indexCount, uint32_t vertexStride);
        void WriteAllocation(nvrhi::ICommandList* commands, const SceneGeometryArenaAllocation& allocation,
            std::span<const std::byte> vertices, std::span<const uint32_t> indices);
        [[nodiscard]] std::span<const SceneGeometryArenaPage> GetPages() const noexcept;
        [[nodiscard]] SceneGeometryArenaStats GetStats() const noexcept;
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void Collect(uint64_t completedSubmission);
        void Clear(uint64_t completedSubmission);
    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
