#include "Hepch.h"

#include "SceneGeometryArena.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    struct SceneGeometryArena::Impl
    {
        struct RetiredPages
        {
            uint64_t Submission = 0;
            uint64_t Bytes = 0;
            std::vector<SceneGeometryArenaPage> Pages;
        };
        explicit Impl(const SceneGeometryArenaDesc& desc) : Device(desc.Device), Desc(desc)
        {
            if (!Device || desc.VertexPageBytes == 0 || desc.IndexPageBytes < sizeof(uint32_t) ||
                desc.VertexPageBytes > uint64_t(std::numeric_limits<int32_t>::max()) ||
                desc.IndexPageBytes > uint64_t(std::numeric_limits<int32_t>::max()))
                throw std::invalid_argument("Geometry arena requires a device and nonzero page budgets below 2 GiB");
        }
        nvrhi::DeviceHandle Device;
        SceneGeometryArenaDesc Desc;
        std::vector<SceneGeometryArenaPage> Pages;
        std::vector<bool> RecordedUploads;
        std::vector<RetiredPages> Retired;
        uint64_t LastSubmission = 0;
        uint64_t Generation = 1;
        uint64_t ReservedBytes = 0;
        uint64_t UsedBytes = 0;
        uint64_t UploadedBytes = 0;
    };

    SceneGeometryArena::SceneGeometryArena(const SceneGeometryArenaDesc& desc) : mImpl(CreateScope<Impl>(desc)) {}
    SceneGeometryArena::~SceneGeometryArena() = default;

    SceneGeometryArenaAllocation SceneGeometryArena::Allocate(
        uint32_t vertexCount, uint32_t indexCount, uint32_t vertexStride)
    {
        auto& impl = *mImpl;
        if (vertexCount == 0 || indexCount == 0 || vertexStride == 0 || vertexStride > 65536u ||
            vertexCount > uint32_t(std::numeric_limits<int32_t>::max()) ||
            uint64_t(vertexCount) * vertexStride > uint64_t(std::numeric_limits<int32_t>::max()) ||
            uint64_t(indexCount) * sizeof(uint32_t) > uint64_t(std::numeric_limits<int32_t>::max()))
            throw std::invalid_argument("Geometry arena allocation exceeds the indexed draw or page byte range");
        uint32_t pageIndex = ~0u;
        for (size_t index = impl.Pages.size(); index > 0; --index)
        {
            const auto& page = impl.Pages[index - 1];
            if (page.VertexStride == vertexStride && vertexCount <= page.VertexCapacity - page.UsedVertices &&
                indexCount <= page.IndexCapacity - page.UsedIndices)
            { pageIndex = static_cast<uint32_t>(index - 1); break; }
        }
        if (pageIndex == ~0u)
        {
            impl.Pages.reserve(impl.Pages.size() + 1u);
            impl.RecordedUploads.reserve(impl.RecordedUploads.size() + 1u);
            SceneGeometryArenaPage page;
            page.VertexStride = vertexStride;
            page.VertexCapacity = static_cast<uint32_t>(std::max(uint64_t(vertexCount), impl.Desc.VertexPageBytes / vertexStride));
            page.IndexCapacity = static_cast<uint32_t>(std::max(uint64_t(indexCount), impl.Desc.IndexPageBytes / sizeof(uint32_t)));
            const uint64_t vertexBytes = uint64_t(page.VertexCapacity) * vertexStride;
            const uint64_t indexBytes = uint64_t(page.IndexCapacity) * sizeof(uint32_t);
            page.VertexBuffer = impl.Device->createBuffer(nvrhi::BufferDesc().setByteSize(vertexBytes)
                .setIsVertexBuffer(true).setCanHaveRawViews(true).setIsAccelStructBuildInput(true)
                .setInitialState(nvrhi::ResourceStates::Common)
                .setDebugName("Resident static geometry arena vertices"));
            page.IndexBuffer = impl.Device->createBuffer(nvrhi::BufferDesc().setByteSize(indexBytes)
                .setIsIndexBuffer(true).setCanHaveRawViews(true).setIsAccelStructBuildInput(true)
                .setInitialState(nvrhi::ResourceStates::Common)
                .setDebugName("Resident static geometry arena indices"));
            if (!page.VertexBuffer || !page.IndexBuffer)
                throw std::runtime_error("NVRHI geometry arena page allocation failed");
            pageIndex = static_cast<uint32_t>(impl.Pages.size());
            impl.Pages.push_back(std::move(page));
            impl.RecordedUploads.push_back(false);
            impl.ReservedBytes += vertexBytes + indexBytes;
        }
        auto& page = impl.Pages[pageIndex];
        const SceneGeometryArenaAllocation result{ .PageIndex = pageIndex,
            .FirstVertex = page.UsedVertices, .FirstIndex = page.UsedIndices,
            .VertexCount = vertexCount, .IndexCount = indexCount, .Generation = impl.Generation };
        page.UsedVertices += vertexCount;
        page.UsedIndices += indexCount;
        impl.UsedBytes += uint64_t(vertexCount) * vertexStride + uint64_t(indexCount) * sizeof(uint32_t);
        return result;
    }

    void SceneGeometryArena::WriteAllocation(nvrhi::ICommandList* commands,
        const SceneGeometryArenaAllocation& allocation, std::span<const std::byte> vertices,
        std::span<const uint32_t> indices)
    {
        auto& impl = *mImpl;
        if (!commands || allocation.Generation != impl.Generation || allocation.PageIndex >= impl.Pages.size())
            throw std::invalid_argument("Geometry arena upload uses a stale allocation or invalid command list");
        const auto& page = impl.Pages[allocation.PageIndex];
        if (vertices.size_bytes() != uint64_t(allocation.VertexCount) * page.VertexStride ||
            indices.size() != allocation.IndexCount ||
            uint64_t(allocation.FirstVertex) + allocation.VertexCount > page.UsedVertices ||
            uint64_t(allocation.FirstIndex) + allocation.IndexCount > page.UsedIndices)
            throw std::invalid_argument("Geometry arena upload does not match its allocated range");
        commands->writeBuffer(page.VertexBuffer, vertices.data(), vertices.size_bytes(),
            uint64_t(allocation.FirstVertex) * page.VertexStride);
        commands->writeBuffer(page.IndexBuffer, indices.data(), indices.size_bytes(),
            uint64_t(allocation.FirstIndex) * sizeof(uint32_t));
        impl.UploadedBytes += vertices.size_bytes() + indices.size_bytes();
        impl.RecordedUploads[allocation.PageIndex] = true;
    }

    std::span<const SceneGeometryArenaPage> SceneGeometryArena::GetPages() const noexcept { return mImpl->Pages; }
    SceneGeometryArenaStats SceneGeometryArena::GetStats() const noexcept
    {
        SceneGeometryArenaStats stats{ .ReservedBytes = mImpl->ReservedBytes, .UsedBytes = mImpl->UsedBytes,
            .UploadedBytes = mImpl->UploadedBytes, .PageCount = static_cast<uint32_t>(mImpl->Pages.size()) };
        for (const auto& retired : mImpl->Retired) stats.RetiredBytes += retired.Bytes;
        return stats;
    }
    void SceneGeometryArena::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        mImpl->LastSubmission = submission;
        for (size_t page = 0; page < mImpl->Pages.size(); ++page)
            if (mImpl->RecordedUploads[page]) mImpl->Pages[page].bSubmitted = true;
        AbortFrame();
    }
    void SceneGeometryArena::AbortFrame() noexcept
    { std::fill(mImpl->RecordedUploads.begin(), mImpl->RecordedUploads.end(), false); }
    void SceneGeometryArena::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mImpl->Retired, [completedSubmission](const Impl::RetiredPages& pages)
            { return pages.Submission <= completedSubmission; });
    }
    void SceneGeometryArena::Clear(uint64_t completedSubmission)
    {
        if (!mImpl->Pages.empty())
            mImpl->Retired.push_back({ mImpl->LastSubmission, mImpl->ReservedBytes, std::move(mImpl->Pages) });
        mImpl->Pages.clear();
        mImpl->RecordedUploads.clear();
        mImpl->ReservedBytes = mImpl->UsedBytes = 0;
        if (++mImpl->Generation == 0) mImpl->Generation = 1;
        Collect(completedSubmission);
    }
}
