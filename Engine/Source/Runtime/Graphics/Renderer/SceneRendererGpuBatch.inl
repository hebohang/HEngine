// Included inside SceneRenderer::Impl. Scene rows and material pages remain
// authoritative; this groups only compatible command recording state.
struct GraphArenaPage { BufferHandle Vertices, Indices; };
struct GpuStateGroup
{
    uint32_t FirstBatch = 0;
    uint32_t GeometryPage = 0;
    uint32_t DescriptorPage = 0;
    uint32_t ArgumentBase = 0;
    uint32_t MaxDrawCount = 0;
    bool bDoubleSided = false;
    bool bAlphaMask = false;
};
Scope<SceneGeometryArena> GeometryArena;
Scope<SceneIndirectCompactionPass> DepthIndirectCompaction;
Scope<SceneIndirectCompactionPass> MainIndirectCompaction;
std::vector<GraphArenaPage> GraphArenaPages;
std::vector<GpuStateGroup> GpuStateGroups;
std::vector<SceneIndirectGroupMapping> GpuGroupMapping;
BufferHandle DepthCompactedArgumentsHandle;
BufferHandle MainCompactedArgumentsHandle;
bool bGpuBatchingEnabled = false;
bool bGpuTightFrustumEnabled = false;
std::vector<const SubMesh*> PendingMeshUploads;
// Reuse scratch storage across meshes; writeBuffer copies it during recording.
std::vector<StaticVertex> DeformedStaticUpload;
std::vector<SkinnedVertex> DeformedSkinnedUpload;

static uint64_t MeshVertexByteOffset(const MeshEntry& mesh)
{ return mesh.ArenaAllocation ? uint64_t(mesh.ArenaAllocation.FirstVertex) * sizeof(StaticVertex) : 0u; }
static uint64_t MeshIndexByteOffset(const MeshEntry& mesh)
{ return mesh.ArenaAllocation ? uint64_t(mesh.ArenaAllocation.FirstIndex) * sizeof(uint32_t) : 0u; }

void PrepareGpuBatchFrame()
{
    UpdateRasterGuideRequirements();
    PendingMeshUploads.reserve(MeshOrder.size());
    GpuStateGroups.clear();
    GpuGroupMapping.resize(GpuBatches.size());
    std::map<std::tuple<uint32_t, uint32_t, bool, bool, const CompiledMaterialShader*>, uint32_t> groups;
    for (uint32_t i = 0; i < GpuBatches.size(); ++i)
    {
        const auto& batch = GpuBatches[i];
        const auto& mesh = MeshCache.at(batch.Key.Mesh);
        if (!mesh.ArenaAllocation) throw std::logic_error("Static GPU batch has no resident geometry arena allocation");
        const uint32_t descriptorPage = bHeapDirectlyIndexed ? 0u : MaterialPageByMaterialIndex.at(batch.MaterialIndex);
        const bool bAlphaMask = (GpuMaterialDataCpu.at(batch.MaterialIndex).EmissiveSamplerFlags.z & c_MaterialFlagAlphaMask) != 0;
        const auto [group, inserted] = groups.try_emplace(
            std::tuple(mesh.ArenaAllocation.PageIndex, descriptorPage, batch.Key.bDoubleSided, bAlphaMask, batch.MaterialProgram.get()),
            static_cast<uint32_t>(GpuStateGroups.size()));
        if (inserted)
            GpuStateGroups.push_back({ .FirstBatch = i, .GeometryPage = mesh.ArenaAllocation.PageIndex,
                .DescriptorPage = descriptorPage, .bDoubleSided = batch.Key.bDoubleSided, .bAlphaMask = bAlphaMask });
        ++GpuStateGroups[group->second].MaxDrawCount;
        GpuGroupMapping[i].GroupIndex = group->second;
    }
    Stats.CustomMaterialGpuDrawCount = 0;
    Stats.CustomMaterialGpuObjectCount = 0;
    for (const auto& batch : GpuBatches)
        if (batch.MaterialProgram)
        {
            Stats.CustomMaterialGpuObjectCount += batch.ObjectCount;
            if (!bGpuBatchingEnabled) ++Stats.CustomMaterialGpuDrawCount;
        }
    if (bGpuBatchingEnabled)
        for (const auto& group : GpuStateGroups)
            if (GpuBatches[group.FirstBatch].MaterialProgram) ++Stats.CustomMaterialGpuDrawCount;
    uint32_t base = 0;
    for (auto& group : GpuStateGroups)
    { group.ArgumentBase = base; base += group.MaxDrawCount; }
    for (auto& mapping : GpuGroupMapping)
        mapping.GroupBase = GpuStateGroups[mapping.GroupIndex].ArgumentBase;
    if (DepthIndirectCompaction->PrepareFrame(GpuGroupMapping, static_cast<uint32_t>(GpuStateGroups.size()), CurrentCompletedSubmission))
        bGraphDirty = true;
    if (MainIndirectCompaction->PrepareFrame(GpuGroupMapping, static_cast<uint32_t>(GpuStateGroups.size()), CurrentCompletedSubmission))
        bGraphDirty = true;
    const auto arena = GeometryArena->GetStats();
    Stats.GeometryArenaReservedBytes = arena.ReservedBytes + arena.RetiredBytes;
    Stats.GeometryArenaUsedBytes = arena.UsedBytes;
    Stats.IndirectCompactionResidentBytes = DepthIndirectCompaction->GetStats().ResidentBytes +
        MainIndirectCompaction->GetStats().ResidentBytes + DepthIndirectCompaction->GetStats().RetiredBytes +
        MainIndirectCompaction->GetStats().RetiredBytes;
    Stats.GpuMultiDrawGroupCount = bGpuBatchingEnabled ? static_cast<uint32_t>(GpuStateGroups.size()) : 0;
    if (bGpuBatchingEnabled)
    {
        Stats.GpuIndirectDrawCount = Stats.GpuMultiDrawGroupCount;
        Stats.DepthPrepassIndirectDrawCount = Stats.GpuMultiDrawGroupCount;
        Stats.StaticOpaqueDrawCount = Stats.CpuFallbackDrawCount + Stats.GpuIndirectDrawCount;
        Stats.DepthPrepassDrawCount = static_cast<uint32_t>(CpuDrawItems.size() + SkinnedDrawItems.size()) + Stats.GpuMultiDrawGroupCount;
    }
}

void AbortGeometryUploads() noexcept
{
    GeometryArena->AbortFrame();
    for (const auto* key : PendingMeshUploads)
        if (auto found = MeshCache.find(key); found != MeshCache.end())
        {
            found->second.bUploaded = false;
            found->second.DeformationHistory.Abort();
        }
    PendingMeshUploads.clear();
}

void ImportGeometryArenaPages()
{
    GraphArenaPages.clear();
    for (const auto& page : GeometryArena->GetPages())
    {
        GraphArenaPages.push_back({
            Graph.ImportBuffer(RenderGraphBufferDesc{ .DebugName = "Static arena page vertices",
                .ByteSize = uint64_t(page.VertexCapacity) * page.VertexStride, .StructStride = page.VertexStride },
                page.VertexBuffer, page.bSubmitted ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::VertexBuffer),
            Graph.ImportBuffer(RenderGraphBufferDesc{ .DebugName = "Static arena page indices",
                .ByteSize = uint64_t(page.IndexCapacity) * sizeof(uint32_t), .StructStride = sizeof(uint32_t) },
                page.IndexBuffer, page.bSubmitted ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::IndexBuffer),
        });
    }
}

template<typename StateFactory>
void RecordGroupedGpuGeometry(RenderGraphContext& context, SceneIndirectCompactionPass& compaction,
    BufferHandle arguments, StateFactory&& makeState)
{
    (void)context.GetBuffer(arguments, RenderGraphAccess::Read);
    (void)context.GetBuffer(VisibleObjectIndicesHandle, RenderGraphAccess::Read);
    auto* commands = context.GetCommandList();
    const auto pages = GeometryArena->GetPages();
    for (const auto& group : GpuStateGroups)
    {
        const auto& page = pages[group.GeometryPage];
        const auto& graphPage = GraphArenaPages[group.GeometryPage];
        (void)context.GetBuffer(graphPage.Vertices, RenderGraphAccess::Read);
        (void)context.GetBuffer(graphPage.Indices, RenderGraphAccess::Read);
        auto state = makeState(group);
        state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(page.VertexBuffer).setSlot(0).setOffset(0))
            .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(VisibleObjectIndicesBuffer).setSlot(1).setOffset(0))
            .setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(page.IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0))
            .setIndirectParams(compaction.GetBuffer()).setIndirectCountBuffer(compaction.GetBuffer());
        commands->setGraphicsState(state);
        commands->drawIndexedIndirectCount(compaction.GetArgumentOffset(group.ArgumentBase),
            compaction.GetCountOffset(static_cast<uint32_t>(&group - GpuStateGroups.data())), group.MaxDrawCount);
    }
}

// Shared by individual indirect draws and compaction groups. Program identity
// includes source, artifact generation and layout in the material PSO cache.
nvrhi::IGraphicsPipeline* GetGpuMaterialPipeline(nvrhi::IFramebuffer* framebuffer,
    const GpuBatch& batch, MaterialShaderPass pass)
{
    if (batch.MaterialProgram)
        return GetOrCreateMaterialPipeline(framebuffer, *batch.MaterialProgram,
            pass, false, batch.Key.bDoubleSided, false, true);
    return pass == MaterialShaderPass::GBuffer
        ? GetRasterPipeline(framebuffer, 2, batch.Key.bDoubleSided)
        : GetOrCreateGpuOpaquePipeline(framebuffer, batch.Key.bDoubleSided);
}
