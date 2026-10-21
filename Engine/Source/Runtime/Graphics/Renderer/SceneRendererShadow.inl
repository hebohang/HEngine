// Shared mesh/material/skin data is consumed by each budgeted local shadow view.
struct ShadowGpuBatch
{
    const SubMesh* Mesh = nullptr;
    bool bDoubleSided = false;
    uint32_t LodIndex = 0;
    uint32_t VisibleBase = 0;
    std::vector<uint32_t> Objects;
};
Scope<SceneShadowGpuSubmission> ShadowGpuSubmission;
std::vector<ShadowGpuBatch> ShadowGpuBatches;
std::vector<SceneShadowGpuViewResources> ShadowGpuGraphViews;
std::vector<nvrhi::BindingSetHandle> ShadowGpuObjectBindings;
nvrhi::BindingLayoutHandle ShadowGpuObjectLayout;
nvrhi::ShaderHandle ShadowGpuVertexShader;
uint64_t ShadowLodSignature = 0;
bool bGpuShadowBatchingEnabled = false;
struct ShadowGpuStateGroup
{
    uint32_t Page = 0, Base = 0, Count = 0;
    bool bDoubleSided = false;
};
std::vector<ShadowGpuStateGroup> ShadowGpuGroups;

void PrepareGpuShadowFrame()
{
    uint64_t lodSignature = 14695981039346656037ull;
    for (const auto& item : ShadowDrawItems)
    {
        lodSignature = MixKey(lodSignature, item.Constants.MaterialObjectMeshIndices.y);
        lodSignature = MixKey(lodSignature, item.Constants.MaterialObjectMeshIndices.z);
        lodSignature = MixKey(lodSignature, item.LodIndex);
    }
    if (lodSignature != ShadowLodSignature)
        LocalShadowPass->InvalidateGeometry();
    ShadowLodSignature = lodSignature;
    ShadowGpuBatches.clear();
    std::unordered_map<uint64_t, uint32_t> gpuRows;
    for (uint32_t i = 0; i < GpuObjectData.size(); ++i)
    {
        const auto& id = GpuObjectData[i].MaterialObjectMeshIndices;
        gpuRows.emplace((uint64_t(id.y) << 32u) | id.z, i);
    }
    std::map<std::tuple<const SubMesh*, bool, uint32_t>, uint32_t> batches;
    for (auto& item : ShadowDrawItems)
    {
        item.bGpuShadow = false;
        if (item.bSkinned || item.MaterialProgram) continue;
        const auto& id = item.Constants.MaterialObjectMeshIndices;
        if ((GpuMaterialDataCpu[id.x].EmissiveSamplerFlags.z & 1u) != 0u) continue;
        const auto row = gpuRows.find((uint64_t(id.y) << 32u) | id.z);
        if (row == gpuRows.end()) continue;
        const auto [batch, inserted] = batches.try_emplace(
            std::tuple(item.Key, item.bDoubleSided, item.LodIndex), static_cast<uint32_t>(ShadowGpuBatches.size()));
        if (inserted) ShadowGpuBatches.push_back(ShadowGpuBatch{ item.Key, item.bDoubleSided, item.LodIndex });
        ShadowGpuBatches[batch->second].Objects.push_back(row->second);
        item.bGpuShadow = true;
    }
    std::vector<SceneShadowGpuDraw> draws;
    std::vector<nvrhi::DrawIndexedIndirectArguments> arguments;
    std::vector<SceneIndirectGroupMapping> groupMapping(ShadowGpuBatches.size());
    std::map<std::pair<uint32_t, bool>, uint32_t> groupLookup;
    ShadowGpuGroups.clear();
    for (uint32_t i = 0; i < ShadowGpuBatches.size(); ++i)
    {
        auto& batch = ShadowGpuBatches[i];
        batch.VisibleBase = static_cast<uint32_t>(draws.size());
        for (const uint32_t row : batch.Objects)
            draws.push_back({ row, i, batch.VisibleBase, 0 });
        nvrhi::DrawIndexedIndirectArguments args{};
        const auto& mesh = MeshCache.at(batch.Mesh);
        const auto& range = mesh.Lods[batch.LodIndex];
        if (!mesh.ArenaAllocation) throw std::logic_error("GPU shadow static geometry lacks an arena allocation");
        args.indexCount = range.IndexCount;
        args.startIndexLocation = mesh.ArenaAllocation.FirstIndex + range.FirstIndex;
        args.baseVertexLocation = static_cast<int32_t>(mesh.ArenaAllocation.FirstVertex);
        args.startInstanceLocation = batch.VisibleBase;
        args.instanceCount = 0;
        arguments.push_back(args);
        const auto [group, inserted] = groupLookup.try_emplace(
            std::pair(mesh.ArenaAllocation.PageIndex, batch.bDoubleSided), static_cast<uint32_t>(ShadowGpuGroups.size()));
        if (inserted) ShadowGpuGroups.push_back({ .Page = mesh.ArenaAllocation.PageIndex, .bDoubleSided = batch.bDoubleSided });
        ++ShadowGpuGroups[group->second].Count;
        groupMapping[i].GroupIndex = group->second;
    }
    uint32_t groupBase = 0;
    for (auto& group : ShadowGpuGroups) { group.Base = groupBase; groupBase += group.Count; }
    for (auto& group : groupMapping) group.GroupBase = ShadowGpuGroups[group.GroupIndex].Base;
    Stats.GpuShadowIndirectDrawCount = 0;
    Stats.GpuShadowRecordedViews = 0;
    Stats.GpuShadowMultiDrawGroupCount = bGpuShadowBatchingEnabled ? static_cast<uint32_t>(ShadowGpuGroups.size()) : 0;
    constexpr uint32_t viewCount = 24;
    if (ShadowGpuSubmission->PrepareFrame(draws, arguments, viewCount, CurrentCompletedSubmission,
        groupMapping, static_cast<uint32_t>(ShadowGpuGroups.size())))
    {
        ShadowGpuObjectBindings.clear();
        for (uint32_t i = 0; i < viewCount; ++i)
        {
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, GpuObjectBuffer));
            auto binding = Device->createBindingSet(desc, ShadowGpuObjectLayout);
            if (!binding) throw std::runtime_error("GPU shadow object binding failed");
            ShadowGpuObjectBindings.push_back(binding);
        }
        bGraphDirty = true;
    }
    for (uint32_t i = 0; i < DirectionalShadowCascadeCount; ++i)
        ShadowGpuSubmission->SetView(i, i == 0 ? FrameData.LightViewProjection :
            FrameData.AdditionalLightViewProjections[i - 1], bDirectionalShadowEnabled);
    const auto localViews = LocalShadowPass->GetViews();
    for (const uint32_t i : LocalShadowPass->GetDirtyViewIndices())
        ShadowGpuSubmission->SetView(4 + localViews[i].Layer, localViews[i].ViewProjection);
    const uint64_t cpuShadowDraws = static_cast<uint64_t>(std::count_if(
        ShadowDrawItems.begin(), ShadowDrawItems.end(), [](const auto& item) { return !item.bGpuShadow; }));
    const uint64_t objectWrites = uint64_t(CpuDrawItems.size() + SkinnedDrawItems.size()) * 4u +
        TransparentDrawItems.size() + OutlineDrawItems.size() + cpuShadowDraws *
        (DirectionalShadowCascadeCount + LocalShadowPass->GetStats().DirtyViews);
    if (objectWrites > c_MaxObjectConstantWritesPerFrame)
        throw std::length_error("Scene exceeds the explicit volatile object-constant write budget");
}

void DeclareGpuShadowResources(RenderGraphBuilder& builder, uint32_t first, uint32_t count)
{
    (void)builder.ShaderResource(GpuObjectsHandle);
    for (uint32_t i = first; i < first + count; ++i)
    {
        (void)builder.VertexBuffer(ShadowGpuGraphViews[i].VisibleObjects);
        (void)builder.IndirectArgument(bGpuShadowBatchingEnabled
            ? ShadowGpuGraphViews[i].CompactedArguments : ShadowGpuGraphViews[i].IndirectArguments);
    }
}

uint32_t RecordGpuShadowView(RenderGraphContext& context, uint32_t view, uint32_t cascade,
    nvrhi::IFramebuffer* framebuffer, const nvrhi::ViewportState& viewport,
    nvrhi::IBindingSet* frameBinding)
{
    auto* commandList = context.GetCommandList();
    (void)context.GetBuffer(ShadowGpuGraphViews[view].VisibleObjects, RenderGraphAccess::Read);
    (void)context.GetBuffer(bGpuShadowBatchingEnabled ? ShadowGpuGraphViews[view].CompactedArguments :
        ShadowGpuGraphViews[view].IndirectArguments, RenderGraphAccess::Read);
    auto* arguments = bGpuShadowBatchingEnabled ? ShadowGpuSubmission->GetCompactedBuffer(view) :
        ShadowGpuSubmission->GetIndirectBuffer(view);
    const uint32_t count = static_cast<uint32_t>(bGpuShadowBatchingEnabled ? ShadowGpuGroups.size() : ShadowGpuBatches.size());
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t pageIndex = bGpuShadowBatchingEnabled ? ShadowGpuGroups[i].Page :
            MeshCache.at(ShadowGpuBatches[i].Mesh).ArenaAllocation.PageIndex;
        const bool bDoubleSided = bGpuShadowBatchingEnabled ? ShadowGpuGroups[i].bDoubleSided : ShadowGpuBatches[i].bDoubleSided;
        const auto& page = GeometryArena->GetPages()[pageIndex];
        (void)context.GetBuffer(GraphArenaPages[pageIndex].Vertices, RenderGraphAccess::Read);
        (void)context.GetBuffer(GraphArenaPages[pageIndex].Indices, RenderGraphAccess::Read);
        nvrhi::GraphicsPipelineDesc pipeline;
        pipeline.setPrimType(nvrhi::PrimitiveType::TriangleList).setInputLayout(GpuDepthInputLayout)
            .setVertexShader(ShadowGpuVertexShader)
            .addBindingLayout(FrameBindingLayout).addBindingLayout(ShadowGpuObjectLayout)
            .addBindingLayout(ShadowPassBindingLayout);
        pipeline.renderState.depthStencilState.enableDepthTest().enableDepthWrite()
            .setDepthFunc(nvrhi::ComparisonFunc::Less);
        pipeline.renderState.rasterState.setCullMode(bDoubleSided
            ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back)
            .setFrontCounterClockwise(true).enableDepthClip()
            .setDepthBias(100).setSlopeScaleDepthBias(1.0f);
        auto pso = PipelineCacheInstance.GetOrCreateGraphicsPipeline(
            GraphicsPipelineKey{ MixKey(0x534844475055ull, bDoubleSided) },
            pipeline, framebuffer->getFramebufferInfo(), "GPU submitted static shadow");
        nvrhi::GraphicsState state;
        state.setPipeline(pso).setFramebuffer(framebuffer).setViewport(viewport)
            .addBindingSet(frameBinding).addBindingSet(ShadowGpuObjectBindings[view])
            .addBindingSet(ShadowPassBindingSet)
            .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(page.VertexBuffer).setSlot(0).setOffset(0))
            .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(ShadowGpuSubmission->GetVisibleBuffer(view)).setSlot(1).setOffset(0))
            .setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(page.IndexBuffer)
                .setFormat(nvrhi::Format::R32_UINT).setOffset(0))
            .setIndirectParams(arguments);
        if (bGpuShadowBatchingEnabled) state.setIndirectCountBuffer(arguments);
        commandList->setGraphicsState(state);
        const ShadowPassConstants constants{ .CascadeIndex = cascade };
        commandList->setPushConstants(&constants, sizeof(constants));
        if (bGpuShadowBatchingEnabled)
            commandList->drawIndexedIndirectCount(ShadowGpuGroups[i].Base * sizeof(nvrhi::DrawIndexedIndirectArguments),
                ShadowGpuSubmission->GetCountOffset(i), ShadowGpuGroups[i].Count);
        else
            commandList->drawIndexedIndirect(i * sizeof(nvrhi::DrawIndexedIndirectArguments), 1);
    }
    Stats.GpuShadowIndirectDrawCount += count;
    if (count > 0) ++Stats.GpuShadowRecordedViews;
    return count;
}

template<typename T>
bool IsShadowCasterVisible(const T& item, const glm::mat4& viewProjection) const
{
    // Bind-pose bounds cannot conservatively reject a deformed character.
    if (item.bSkinned || !item.Key->HasBounds()) return true;
    const glm::vec3 center = (item.Key->GetBoundsMin() + item.Key->GetBoundsMax()) * 0.5f;
    const glm::vec3 extents = (item.Key->GetBoundsMax() - item.Key->GetBoundsMin()) * 0.5f;
    const glm::mat4 clip = glm::transpose(viewProjection * item.Constants.World);
    const std::array<glm::vec4, 6> planes = {
        clip[3] + clip[0], clip[3] - clip[0], clip[3] + clip[1],
        clip[3] - clip[1], clip[2], clip[3] - clip[2] };
    for (const auto& plane : planes)
        if (glm::dot(glm::vec3(plane), center) + plane.w +
            glm::dot(glm::abs(glm::vec3(plane)), extents) < 0.0f) return false;
    return true;
}

void AddLocalShadowPasses()
{
    const auto localFrame = Graph.ImportBuffer(RenderGraphBufferDesc{
        .DebugName = "Local shadow volatile view constants", .ByteSize = sizeof(FrameConstants),
    }, LocalShadowFrameConstantBuffer, nvrhi::ResourceStates::ConstantBuffer,
        nvrhi::ResourceStates::ConstantBuffer);
    LocalShadowGraphResources = LocalShadowPass->AddPasses(Graph, SceneLocalShadowCallbacks{
        .DeclareCasterResources = [this, localFrame](RenderGraphBuilder& builder)
        {
            DeclareGpuShadowResources(builder, 4, 20);
            (void)builder.ConstantBuffer(localFrame);
            (void)builder.ConstantBuffer(ObjectBufferHandle);
            (void)builder.ShaderResource(BoneMatricesHandle);
            (void)builder.ShaderResource(GpuMaterialsHandle);
            (void)builder.ShaderResource(MaterialParametersHandle);
            for (const auto& texture : GraphMaterialTextures)
                (void)builder.ShaderResource(texture.Handle);
            for (const auto& geometry : GraphGeometryResources)
            {
                (void)builder.VertexBuffer(geometry.VertexBuffer);
                (void)builder.IndexBuffer(geometry.IndexBuffer);
            }
        },
        .DrawView = [this](RenderGraphContext& context, const SceneLocalShadowView& view,
            nvrhi::IFramebuffer* framebuffer)
        {
            auto constants = FrameData;
            constants.LightViewProjection = view.ViewProjection;
            auto* commandList = context.GetCommandList();
            commandList->writeBuffer(LocalShadowFrameConstantBuffer, &constants, sizeof(constants));
            const auto viewport = nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(view.Resolution), static_cast<float>(view.Resolution)));
            const ShadowPassConstants passConstants{ .CascadeIndex = 0 };
            RecordGpuShadowView(context, 4 + view.Layer, 0, framebuffer, viewport, LocalShadowFrameBindingSet);
            for (const auto& item : ShadowDrawItems)
            {
                if (item.bGpuShadow) continue;
                if (!view.IsObjectVisible(item.Constants.MaterialObjectMeshIndices.y)) continue;
                const auto& mesh = MeshCache.at(item.Key);
                commandList->writeBuffer(ObjectConstantBuffer, &item.Constants, sizeof(item.Constants));
                bObjectConstantsInitialized = true;
                const bool bCustomMask = item.MaterialProgram &&
                    item.MaterialProgram->GetManifest().BlendMode == MaterialShaderBlendMode::Mask;
                nvrhi::GraphicsState state;
                state.setPipeline(bCustomMask
                        ? GetOrCreateMaterialPipeline(framebuffer, *item.MaterialProgram,
                            MaterialShaderPass::Shadow, item.bSkinned, item.bDoubleSided, false)
                        : GetOrCreateShadowPipeline(framebuffer, item.bSkinned, item.bDoubleSided))
                    .setFramebuffer(framebuffer).setViewport(viewport)
                    .addBindingSet(LocalShadowFrameBindingSet).addBindingSet(ObjectBindingSet)
                    .addBindingSet(ShadowPassBindingSet)
                    .addBindingSet(GetMaterialTextureBindingSet(item.Constants.MaterialObjectMeshIndices.x))
                    .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                    .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(mesh.VertexBuffer).setSlot(0).setOffset(MeshVertexByteOffset(mesh)))
                    .setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(mesh.IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT).setOffset(MeshIndexByteOffset(mesh)));
                commandList->setGraphicsState(state);
                commandList->setPushConstants(&passConstants, sizeof(passConstants));
                commandList->drawIndexed(MeshDrawArguments(mesh, item.LodIndex));
            }
        },
    });
}
