// Included inside SceneRenderer::Impl. Uses the renderer's authoritative mesh,
// material and object tables; no separate raster scene is constructed.
template<typename T>
void UploadSceneRows(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer,
    const std::vector<T>& rows)
{
    static_assert(std::is_trivially_copyable_v<T>);
    auto& previous = UploadedSceneTables[buffer];
    const auto* bytes = reinterpret_cast<const uint8_t*>(rows.data());
    const size_t byteCount = rows.size() * sizeof(T);
    size_t row = 0;
    while (row < rows.size())
    {
        const auto changed = [&](size_t index)
        {
            const size_t offset = index * sizeof(T);
            return offset + sizeof(T) > previous.size() ||
                std::memcmp(previous.data() + offset, bytes + offset, sizeof(T)) != 0;
        };
        if (!changed(row)) { ++row; continue; }
        const size_t first = row++;
        while (row < rows.size() && changed(row)) ++row;
        const size_t offset = first * sizeof(T);
        const size_t count = (row - first) * sizeof(T);
        commandList->writeBuffer(buffer, bytes + offset, count, offset);
        Stats.SceneTableUploadedBytes += count;
    }
    previous.resize(byteCount);
    if (byteCount) std::memcpy(previous.data(), bytes, byteCount);
}

void AddLightDependencies(RenderGraphBuilder& builder)
{
    (void)builder.ConstantBuffer(LightGraphResources.Constants);
    (void)builder.ShaderResource(LightGraphResources.Lights);
    (void)builder.ShaderResource(LightGraphResources.ClusterMasks);
    (void)builder.ShaderResource(LocalShadowGraphResources.DepthArray);
    (void)builder.ShaderResource(LocalShadowGraphResources.ViewRecords);
    if (CurrentAmbientOcclusion)
        (void)builder.ShaderResource(CurrentAmbientOcclusion);
}

nvrhi::BindingSetHandle GetLightingFrameBindingSet(RenderGraphContext& context)
{
    if (!CurrentAmbientOcclusion) return FrameBindingSet;
    auto* ao = context.GetTexture(CurrentAmbientOcclusion, RenderGraphAccess::Read);
    const std::array<const void*, 3> key = {
        ao, LightGrid->GetLightBuffer(), LightGrid->GetClusterMaskBuffer() };
    if (const auto found = LightingFrameBindingSets.find(key); found != LightingFrameBindingSets.end())
        return found->second;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, FrameConstantBuffer))
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, LightGrid->GetConstantsBuffer()))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, LightGrid->GetLightBuffer()))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, LightGrid->GetClusterMaskBuffer()))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(2, ao));
    auto binding = Device->createBindingSet(desc, FrameBindingLayout);
    if (!binding) throw std::runtime_error("Scene AO lighting binding creation failed");
    LightingFrameBindingSets.emplace(key, binding);
    return binding;
}

void PrepareLightGrid(const RenderWorld& world, const SceneRendererFrameDesc& frame)
{
    if (LightGrid->PrepareFrame(world, frame.Width, frame.Height, frame.CompletedSubmission))
    {
        nvrhi::BindingSetDesc desc;
        desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, FrameConstantBuffer))
            .addItem(nvrhi::BindingSetItem::ConstantBuffer(1, LightGrid->GetConstantsBuffer()))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, LightGrid->GetLightBuffer()))
            .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, LightGrid->GetClusterMaskBuffer()))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(2,
                TextureManagerInstance.GetDefaultTexture(DefaultBindlessTexture::White).Texture));
        auto candidate = Device->createBindingSet(desc, FrameBindingLayout);
        if (!candidate) throw std::runtime_error("Scene light binding preparation failed");
        FrameBindingSet = std::move(candidate);
        desc.bindings[0] = nvrhi::BindingSetItem::ConstantBuffer(0, LocalShadowFrameConstantBuffer);
        LocalShadowFrameBindingSet = Device->createBindingSet(desc, FrameBindingLayout);
        if (!LocalShadowFrameBindingSet) throw std::runtime_error("Local shadow view binding failed");
        bGraphDirty = true;
    }
}
struct RasterSurfacePassData
{
    std::array<TextureHandle, 6> Targets;
    TextureHandle Depth;
    bool bGuidesOnly = false;
    std::vector<BufferHandle> Buffers;
    std::vector<TextureHandle> Textures;
};

struct DeferredLightingPassData
{
    std::array<TextureHandle, 11> Inputs;
    TextureHandle Color;
    TextureHandle SkinDiffuse;
    BufferHandle Frame;
};

struct SkinSubsurfacePassData
{
    std::array<TextureHandle, 7> Inputs;
    TextureHandle Output;
    uint32_t Operation = 0;
};

struct SkinSubsurfaceConstants
{
    glm::mat4 InverseViewProjection{ 1.0f };
    glm::vec4 CameraRadius{ 0.0f };
    glm::vec4 PrimaryFog{ 0.0f };
    glm::vec4 SecondaryFog{ 0.0f };
    glm::uvec2 Dimensions{ 0u };
    uint32_t Operation = 0;
    uint32_t Quality = 1;
};
static_assert(sizeof(SkinSubsurfaceConstants) == 128);

struct RasterUnpackPassData
{
    std::array<TextureHandle, 6> Inputs;
    std::array<TextureHandle, 8> Outputs;
    bool bTaaOnly = false;
};

struct RasterUnpackConstants
{
    glm::mat4 ViewFromClip;
    glm::mat4 PreviousSkyClipFromCurrentClip;
};
static_assert(sizeof(RasterUnpackConstants) == 128);

std::array<TextureHandle, 6> RasterSurfaceTargets;
bool bRasterGuidesOnly = false;
std::array<nvrhi::ShaderHandle, 6> RasterVertexShaders;
std::array<nvrhi::ShaderHandle, 6> RasterPixelShaders;
nvrhi::ShaderHandle DeferredVertexShader;
nvrhi::ShaderHandle DeferredPixelShader;
nvrhi::ShaderHandle DeferredSkinPixelShader;
nvrhi::ComputePipelineHandle SkinSubsurfacePipeline;
nvrhi::BindingLayoutHandle SkinSubsurfaceBindingLayout;
TextureHandle SkinDiffuseHandle;
nvrhi::ShaderHandle RasterUnpackShader;
nvrhi::BindingLayoutHandle DeferredBindingLayout;
nvrhi::BindingLayoutHandle RasterUnpackBindingLayout;
nvrhi::ComputePipelineHandle RasterUnpackPipeline;
nvrhi::ComputePipelineHandle RasterTaaGuidePipeline;
nvrhi::BindingLayoutHandle RasterTaaGuideBindingLayout;
std::map<std::array<nvrhi::ITexture*, 7>, nvrhi::FramebufferHandle> RasterFramebuffers;
std::map<std::array<nvrhi::ITexture*, 11>, nvrhi::BindingSetHandle> DeferredBindingSets;
std::map<std::array<nvrhi::ITexture*, 14>, nvrhi::BindingSetHandle> RasterUnpackBindingSets;
std::map<std::array<nvrhi::ITexture*, 2>, nvrhi::FramebufferHandle> DeferredSkinFramebuffers;
std::map<std::array<nvrhi::ITexture*, 8>, nvrhi::BindingSetHandle> SkinSubsurfaceBindingSets;

void ClearRasterCaches()
{
    LightingFrameBindingSets.clear();
    RasterFramebuffers.clear();
    DeferredBindingSets.clear();
    RasterUnpackBindingSets.clear();
    DeferredSkinFramebuffers.clear();
    SkinSubsurfaceBindingSets.clear();
}

nvrhi::ShaderHandle LoadRasterShader(
    const std::string& source, const std::string& file,
    nvrhi::ShaderType stage, const char* entry, const char* profile)
{
    const auto path = ShaderCacheDirectory / file;
    return ShaderArtifacts.LoadShaderFile(ShaderManagerInstance,
        ShaderCacheKey{
            .Source = source,
            .Variant = file,
            .Stage = stage,
            .EntryPoint = entry,
            .TargetProfile = profile,
        }, path, file);
}

void PrepareRasterShaders()
{
    if (DeferredVertexShader && RasterUnpackPipeline)
        return;
    // Load into locals. Publication happens only after every required artifact
    // and immutable layout has been created successfully.
    std::array<nvrhi::ShaderHandle, 6> vertexShaders;
    std::array<nvrhi::ShaderHandle, 6> pixelShaders;
    const std::array<std::string, 6> variants = {
        "GBUFFER", "GBUFFER_SKINNED", "GBUFFER_GPU_DRIVEN",
        "GUIDES", "GUIDES_SKINNED", "GUIDES_GPU_DRIVEN" };
    for (size_t i = 0; i < variants.size(); ++i)
    {
        vertexShaders[i] = LoadRasterShader("Modern/SceneOpaque.hlsl",
            "SceneOpaque_" + variants[i] + ".vs", nvrhi::ShaderType::Vertex, "VS", "vs_6_0");
        const std::string base = bHeapDirectlyIndexed ? "SceneOpaqueHeapDirect" : "SceneOpaque";
        pixelShaders[i] = LoadRasterShader("Modern/" + base + ".hlsl",
            base + "_" + variants[i] + (bHeapDirectlyIndexed ? "__ps_6_6.ps" : ".ps"),
            nvrhi::ShaderType::Pixel, "PS", bHeapDirectlyIndexed ? "ps_6_6" : "ps_6_0");
    }
    auto deferredVs = LoadRasterShader("Modern/SceneDeferred.hlsl", "SceneDeferred.vs",
        nvrhi::ShaderType::Vertex, "DeferredVS", "vs_6_0");
    auto deferredPs = LoadRasterShader("Modern/SceneDeferred.hlsl", "SceneDeferred.ps",
        nvrhi::ShaderType::Pixel, "DeferredPS", "ps_6_0");
    auto deferredSkinPs = LoadRasterShader("Modern/SceneDeferredSkin.hlsl", "SceneDeferredSkin.ps",
        nvrhi::ShaderType::Pixel, "DeferredPS", "ps_6_0");
    auto skinShader = LoadRasterShader("Modern/SceneSkinSubsurface.hlsl", "SceneSkinSubsurface.cs",
        nvrhi::ShaderType::Compute, "CS", "cs_6_0");
    nvrhi::BindingLayoutDesc skinLayoutDesc;
    skinLayoutDesc.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SkinSubsurfaceConstants)));
    for (uint32_t slot = 0; slot < 7; ++slot)
        skinLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
    skinLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
    auto skinLayout = Device->createBindingLayout(skinLayoutDesc);
    auto skinPipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
        .setComputeShader(skinShader).addBindingLayout(skinLayout));
    auto unpack = LoadRasterShader("Modern/SceneGBufferUnpack.hlsl", "SceneGBufferUnpack.cs",
        nvrhi::ShaderType::Compute, "CS", "cs_6_0");

    nvrhi::BindingLayoutDesc deferredDesc;
    deferredDesc.setVisibility(nvrhi::ShaderType::Pixel).setRegisterSpaceAndDescriptorSet(3);
    for (uint32_t slot = 0; slot < 11; ++slot)
        deferredDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
    deferredDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(11))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(1));
    auto deferredLayout = Device->createBindingLayout(deferredDesc);
    nvrhi::BindingLayoutDesc unpackDesc;
    unpackDesc.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3);
    unpackDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RasterUnpackConstants)));
    for (uint32_t slot : {0u, 2u, 4u, 5u})
        unpackDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
    for (uint32_t slot : {0u, 3u, 5u, 6u})
        unpackDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(slot));
    auto unpackLayout = Device->createBindingLayout(unpackDesc);
    auto unpackPipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
        .setComputeShader(unpack).addBindingLayout(unpackLayout));
    auto taaShader = LoadRasterShader("Modern/SceneGBufferTaaGuides.hlsl", "SceneGBufferTaaGuides.cs",
        nvrhi::ShaderType::Compute, "CS", "cs_6_0");
    nvrhi::BindingLayoutDesc taaLayoutDesc;
    taaLayoutDesc.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
        .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RasterUnpackConstants)));
    for (uint32_t i : {0u, 2u, 4u, 5u}) taaLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(i));
    for (uint32_t i : {0u, 3u, 6u}) taaLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(i));
    auto taaLayout = Device->createBindingLayout(taaLayoutDesc);
    auto taaPipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
        .setComputeShader(taaShader).addBindingLayout(taaLayout));
    if (!deferredLayout || !unpackLayout || !unpackPipeline || !skinLayout || !skinPipeline)
        throw std::runtime_error("Raster surface pipeline preparation failed");
    RasterVertexShaders = std::move(vertexShaders);
    RasterPixelShaders = std::move(pixelShaders);
    DeferredVertexShader = std::move(deferredVs);
    DeferredPixelShader = std::move(deferredPs);
    DeferredSkinPixelShader = std::move(deferredSkinPs);
    SkinSubsurfaceBindingLayout = std::move(skinLayout);
    SkinSubsurfacePipeline = std::move(skinPipeline);
    RasterUnpackShader = std::move(unpack);
    DeferredBindingLayout = std::move(deferredLayout);
    RasterUnpackBindingLayout = std::move(unpackLayout);
    RasterUnpackPipeline = std::move(unpackPipeline);
    RasterTaaGuideBindingLayout = std::move(taaLayout);
    RasterTaaGuidePipeline = std::move(taaPipeline);
}

nvrhi::IGraphicsPipeline* GetRasterPipeline(
    nvrhi::IFramebuffer* framebuffer, uint32_t vertexPath, bool bDoubleSided)
{
    const uint32_t shaderIndex = vertexPath + (bRasterGuidesOnly ? 3u : 0u);
    nvrhi::GraphicsPipelineDesc desc;
    desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
        .setInputLayout(vertexPath == 1 ? SkinnedInputLayout : vertexPath == 2 ? GpuInputLayout : InputLayout)
        .setVertexShader(RasterVertexShaders[shaderIndex])
        .setPixelShader(RasterPixelShaders[shaderIndex])
        .addBindingLayout(FrameBindingLayout)
        .addBindingLayout(vertexPath == 2 ? GpuObjectBindingLayout : ObjectBindingLayout)
        .addBindingLayout(TextureManagerInstance.GetTextureBindingLayout())
        .addBindingLayout(TextureManagerInstance.GetSamplerBindingLayout());
    desc.renderState.depthStencilState.enableDepthTest().disableDepthWrite()
        .setDepthFunc(nvrhi::ComparisonFunc::Equal);
    desc.renderState.rasterState.setFrontCounterClockwise(true).enableDepthClip();
    if (bDoubleSided) desc.renderState.rasterState.setCullNone();
    else desc.renderState.rasterState.setCullBack();
    return PipelineCacheInstance.GetOrCreateGraphicsPipeline(
        GraphicsPipelineKey{0x5241535445520000ull + shaderIndex * 2 + (bDoubleSided ? 1 : 0)},
        desc, framebuffer->getFramebufferInfo(), "Raster material surface");
}

void AddRasterSurfacePass()
{
    UpdateRasterGuideRequirements();
    PrepareRasterShaders();
    Graph.AddPass<RasterSurfacePassData>(bRasterGuidesOnly ? "RasterNormalMotionGuides" : "RasterGBuffer", RenderGraphQueue::Graphics,
        [this](RenderGraphBuilder& builder, RasterSurfacePassData& data)
        {
            const std::array<nvrhi::Format, 6> formats = {
                nvrhi::Format::RGBA8_UNORM, nvrhi::Format::RGBA8_UNORM,
                nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA16_FLOAT,
                nvrhi::Format::RG32_UINT, nvrhi::Format::RGBA16_FLOAT };
            const std::array<const char*, 6> names = {
                "GBuffer Linear Base Metal", "GBuffer Oct Normal Rough AO", "GBuffer Emissive Model",
                "GBuffer UV Motion Previous Depth", "GBuffer Stable Surface ID",
                "GBuffer Static Diffuse" };
            data.bGuidesOnly = bRasterGuidesOnly;
            for (size_t i = 0; i < formats.size(); ++i)
            {
                if (data.bGuidesOnly && (i == 0 || i == 2 || i == 5)) continue;
                data.Targets[i] = builder.RenderTarget(builder.CreateTexture(RenderGraphTextureDesc{
                    .DebugName = names[i], .Format = formats[i], .bRelativeToGraph = true,
                    .CapabilityUsage = RenderGraphUsage::ShaderResource }));
            }
            RasterSurfaceTargets = data.Targets;
            data.Depth = builder.DepthRead(SceneDepthHandle);
            data.Buffers = {
                builder.ConstantBuffer(FrameBufferHandle), builder.ConstantBuffer(ObjectBufferHandle),
                builder.ShaderResource(GpuObjectsHandle), builder.ShaderResource(GpuMaterialsHandle),
                builder.ShaderResource(MaterialParametersHandle), builder.ShaderResource(BoneMatricesHandle),
                builder.ShaderResource(PreviousBoneMatricesHandle),
                builder.VertexBuffer(VisibleObjectIndicesHandle), builder.IndirectArgument(bGpuBatchingEnabled
                    ? MainCompactedArgumentsHandle : IndirectArgumentsHandle) };
            for (const GraphGeometry& geometry : GraphGeometryResources)
            {
                data.Buffers.push_back(builder.VertexBuffer(geometry.VertexBuffer));
                data.Buffers.push_back(builder.IndexBuffer(geometry.IndexBuffer));
            }
            for (const GraphMaterialTexture& texture : GraphMaterialTextures)
                data.Textures.push_back(builder.ShaderResource(texture.Handle));
        },
        [this](RenderGraphContext& context, const RasterSurfacePassData& data)
        {
            auto* commandList = context.GetCommandList();
            std::array<nvrhi::ITexture*, 7> attachments{};
            for (size_t i = 0; i < data.Targets.size(); ++i)
            {
                if (!data.Targets[i]) continue;
                attachments[i] = context.GetTexture(data.Targets[i], RenderGraphAccess::Write);
                if (i == 4) commandList->clearTextureUInt(attachments[i], nvrhi::AllSubresources, ~0u);
                else commandList->clearTextureFloat(attachments[i], nvrhi::AllSubresources, nvrhi::Color(0.f));
            }
            attachments[6] = context.GetTexture(data.Depth, RenderGraphAccess::Read);
            for (BufferHandle buffer : data.Buffers) (void)context.GetBuffer(buffer, RenderGraphAccess::Read);
            for (TextureHandle texture : data.Textures) (void)context.GetTexture(texture, RenderGraphAccess::Read);
            auto& framebuffer = RasterFramebuffers[attachments];
            if (!framebuffer)
            {
                nvrhi::FramebufferDesc desc;
                for (size_t i = 0; i < 6; ++i)
                    if (attachments[i]) desc.addColorAttachment(attachments[i]);
                desc.setDepthAttachment(attachments[6]);
                framebuffer = Device->createFramebuffer(desc);
            }
            const auto viewport = nvrhi::ViewportState().addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(LastWidth), static_cast<float>(LastHeight)));
            if (bGpuBatchingEnabled)
                RecordGroupedGpuGeometry(context, *MainIndirectCompaction, MainCompactedArgumentsHandle,
                    [&](const GpuStateGroup& group)
                    {
                        nvrhi::GraphicsState state;
                        state.setPipeline(GetGpuMaterialPipeline(framebuffer, GpuBatches[group.FirstBatch], MaterialShaderPass::GBuffer))
                            .setFramebuffer(framebuffer).setViewport(viewport)
                            .addBindingSet(FrameBindingSet).addBindingSet(GpuObjectBindingSet)
                            .addBindingSet(GetMaterialTextureBindingSet(GpuBatches[group.FirstBatch].MaterialIndex))
                            .addBindingSet(TextureManagerInstance.GetSamplerBindingSet());
                        return state;
                    });
            for (uint32_t i = 0; !bGpuBatchingEnabled && i < GpuBatches.size(); ++i)
            {
                const GpuBatch& batch = GpuBatches[i];
                const MeshEntry& mesh = MeshCache.at(batch.Key.Mesh);
                nvrhi::GraphicsState state;
                state.setPipeline(GetGpuMaterialPipeline(framebuffer, batch, MaterialShaderPass::GBuffer))
                    .setFramebuffer(framebuffer).setViewport(viewport)
                    .addBindingSet(FrameBindingSet).addBindingSet(GpuObjectBindingSet)
                    .addBindingSet(GetMaterialTextureBindingSet(batch.MaterialIndex))
                    .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                    .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(mesh.VertexBuffer).setSlot(0).setOffset(0))
                    .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(VisibleObjectIndicesBuffer).setSlot(1).setOffset(0))
                    .setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(mesh.IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(0))
                    .setIndirectParams(IndirectArgumentsBuffer);
                commandList->setGraphicsState(state);
                commandList->drawIndexedIndirect(i * sizeof(nvrhi::DrawIndexedIndirectArguments), 1);
            }
            const auto drawItems = [&](const std::vector<DrawItem>& items, bool bSkinned)
            {
                for (const DrawItem& item : items)
                {
                    const MeshEntry& mesh = MeshCache.at(item.Key);
                    commandList->writeBuffer(ObjectConstantBuffer, &item.Constants, sizeof(item.Constants));
                    nvrhi::GraphicsState state;
                    state.setPipeline(item.MaterialProgram
                        ? GetOrCreateMaterialPipeline(framebuffer, *item.MaterialProgram,
                            MaterialShaderPass::GBuffer, bSkinned, item.bDoubleSided, false)
                        : GetRasterPipeline(framebuffer, bSkinned ? 1 : 0, item.bDoubleSided))
                        .setFramebuffer(framebuffer).setViewport(viewport)
                        .addBindingSet(FrameBindingSet).addBindingSet(ObjectBindingSet)
                        .addBindingSet(GetMaterialTextureBindingSet(item.Constants.MaterialObjectMeshIndices.x))
                        .addBindingSet(TextureManagerInstance.GetSamplerBindingSet())
                        .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(mesh.VertexBuffer).setSlot(0).setOffset(MeshVertexByteOffset(mesh)))
                        .setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(mesh.IndexBuffer).setFormat(nvrhi::Format::R32_UINT).setOffset(MeshIndexByteOffset(mesh)));
                    commandList->setGraphicsState(state);
                    commandList->drawIndexed(MeshDrawArguments(mesh, item.LodIndex));
                }
            };
            drawItems(CpuDrawItems, false);
            drawItems(SkinnedDrawItems, true);
        });
}

bool ShouldUseSkinSubsurface() const
{
    return PostProcessSettings.SkinSubsurfaceEnabled &&
        Path == RenderingPath::Deferred && !IsOfflineViewportRenderingActive() &&
        CurrentPbrDebugMode == ScenePbrDebugMode::Disabled &&
        EffectiveGiSettings.DebugView == GlobalIlluminationDebugView::Composite;
}

void AddSkinSubsurfacePasses()
{
    const TextureHandle originalColor = SceneColorHandle;
    TextureHandle inputDiffuse = SkinDiffuseHandle;
    for (uint32_t operation = 0; operation < 2; ++operation)
    {
        TextureHandle output;
        Graph.AddPass<SkinSubsurfacePassData>(operation == 0
            ? "SkinDiffusionHorizontal" : "SkinDiffusionComposite", RenderGraphQueue::Compute,
            [this, operation, originalColor, inputDiffuse, &output](
                RenderGraphBuilder& builder, SkinSubsurfacePassData& data)
            {
                data.Operation = operation;
                data.Inputs = { inputDiffuse, SkinDiffuseHandle, originalColor, SceneDepthHandle,
                    RasterSurfaceTargets[1], RasterSurfaceTargets[2], RasterSurfaceTargets[4] };
                for (auto& input : data.Inputs) input = builder.ShaderResource(input);
                data.Output = builder.Write(builder.CreateTexture(RenderGraphTextureDesc{
                    .DebugName = operation == 0 ? "Skin Diffusion Horizontal" : "Skin Diffusion HDR",
                    .Format = c_SceneColorFormat, .bRelativeToGraph = true,
                    .CapabilityUsage = RenderGraphUsage::ShaderResource }));
                output = data.Output;
            },
            [this](RenderGraphContext& context, const SkinSubsurfacePassData& data)
            {
                std::array<nvrhi::ITexture*, 8> textures;
                for (size_t index = 0; index < data.Inputs.size(); ++index)
                    textures[index] = context.GetTexture(data.Inputs[index], RenderGraphAccess::Read);
                textures[7] = context.GetTexture(data.Output, RenderGraphAccess::Write);
                auto& binding = SkinSubsurfaceBindingSets[textures];
                if (!binding)
                {
                    nvrhi::BindingSetDesc desc;
                    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(SkinSubsurfaceConstants)));
                    for (uint32_t slot = 0; slot < 7; ++slot)
                        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(slot, textures[slot]));
                    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(0, textures[7]));
                    binding = Device->createBindingSet(desc, SkinSubsurfaceBindingLayout);
                    if (!binding) throw std::runtime_error("Skin diffusion binding creation failed");
                }
                const float radius = std::isfinite(PostProcessSettings.SkinSubsurfaceRadiusMeters)
                    ? std::clamp(PostProcessSettings.SkinSubsurfaceRadiusMeters, 0.0001f, 0.02f) : 0.003f;
                const SkinSubsurfaceConstants constants{
                    .InverseViewProjection = FrameData.InverseViewProjection,
                    .CameraRadius = glm::vec4(glm::vec3(FrameData.CameraPositionAndExposure), radius),
                    .PrimaryFog = glm::vec4(glm::vec3(FrameData.FogDistanceHeight), FrameData.FogColorDensity.w),
                    .SecondaryFog = glm::vec4(glm::vec3(FrameData.SecondaryFogDistanceHeight), FrameData.SecondaryFogColorDensity.w),
                    .Dimensions = glm::uvec2(LastWidth, LastHeight),
                    .Operation = data.Operation,
                    .Quality = static_cast<uint32_t>(std::clamp(PostProcessSettings.SkinSubsurfaceQuality, 1, 2)),
                };
                nvrhi::ComputeState state;
                state.setPipeline(SkinSubsurfacePipeline).addBindingSet(binding);
                auto* commandList = context.GetCommandList();
                commandList->setComputeState(state);
                commandList->setPushConstants(&constants, sizeof(constants));
                commandList->dispatch((LastWidth + 7) / 8, (LastHeight + 7) / 8);
            });
        inputDiffuse = output;
        if (operation == 1) SceneColorHandle = output;
    }
}

void AddDeferredLightingPass()
{
    SkinDiffuseHandle = {};
    Graph.AddPass<DeferredLightingPassData>("DeferredLightingHDR", RenderGraphQueue::Graphics,
        [this](RenderGraphBuilder& builder, DeferredLightingPassData& data)
        {
            AddLightDependencies(builder);
            data.Frame = builder.ConstantBuffer(FrameBufferHandle);
            data.Inputs = { DirectionalShadowHandle, IrradianceHandle, PrefilterHandle, BrdfLutHandle,
                SceneDepthHandle, RasterSurfaceTargets[0], RasterSurfaceTargets[1],
                RasterSurfaceTargets[2], RasterSurfaceTargets[3], RasterSurfaceTargets[4],
                RasterSurfaceTargets[5] };
            for (auto& input : data.Inputs) input = builder.ShaderResource(input);
            data.Color = builder.RenderTarget(builder.CreateTexture(RenderGraphTextureDesc{
                .DebugName = "Deferred HDR Scene Color", .Format = c_SceneColorFormat,
                .bRelativeToGraph = true, .CapabilityUsage = RenderGraphUsage::ShaderResource }));
            SceneColorHandle = data.Color;
            if (ShouldUseSkinSubsurface())
            {
                data.SkinDiffuse = builder.RenderTarget(builder.CreateTexture(RenderGraphTextureDesc{
                    .DebugName = "Skin Unfogged Diffuse", .Format = c_SceneColorFormat,
                    .bRelativeToGraph = true, .CapabilityUsage = RenderGraphUsage::ShaderResource }));
                SkinDiffuseHandle = data.SkinDiffuse;
            }
        },
        [this](RenderGraphContext& context, const DeferredLightingPassData& data)
        {
            (void)context.GetBuffer(data.Frame, RenderGraphAccess::Read);
            std::array<nvrhi::ITexture*, 11> textures;
            for (size_t i = 0; i < textures.size(); ++i)
                textures[i] = context.GetTexture(data.Inputs[i], RenderGraphAccess::Read);
            auto& bindingSet = DeferredBindingSets[textures];
            if (!bindingSet)
            {
                nvrhi::BindingSetDesc desc;
                for (uint32_t i = 0; i < textures.size(); ++i)
                    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(i, textures[i]));
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(11, LocalShadowPass->GetDepthArray(),
                    nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2DArray))
                .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, LocalShadowPass->GetViewRecordBuffer()))
                .addItem(nvrhi::BindingSetItem::Sampler(0, ShadowSampler))
                .addItem(nvrhi::BindingSetItem::Sampler(1, ShadowComparisonSampler));
                bindingSet = Device->createBindingSet(desc, DeferredBindingLayout);
            }
            auto* color = context.GetTexture(data.Color, RenderGraphAccess::Write);
            context.GetCommandList()->clearTextureFloat(color, nvrhi::AllSubresources,
                nvrhi::Color(0.004f, 0.008f, 0.016f, 1.0f));
            auto* framebuffer = GetOrCreateToneFramebuffer(color);
            if (data.SkinDiffuse)
            {
                auto* diffuse = context.GetTexture(data.SkinDiffuse, RenderGraphAccess::Write);
                context.GetCommandList()->clearTextureFloat(diffuse, nvrhi::AllSubresources, nvrhi::Color(0.0f));
                auto& skinFramebuffer = DeferredSkinFramebuffers[{color, diffuse}];
                if (!skinFramebuffer)
                    skinFramebuffer = Device->createFramebuffer(nvrhi::FramebufferDesc()
                        .addColorAttachment(color).addColorAttachment(diffuse));
                if (!skinFramebuffer) throw std::runtime_error("Skin diffusion framebuffer creation failed");
                framebuffer = skinFramebuffer;
            }
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setVertexShader(DeferredVertexShader).setPixelShader(data.SkinDiffuse ? DeferredSkinPixelShader : DeferredPixelShader)
                .addBindingLayout(FrameBindingLayout).addBindingLayout(DeferredBindingLayout);
            desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
            desc.renderState.rasterState.setCullNone();
            auto pipeline = PipelineCacheInstance.GetOrCreateGraphicsPipeline(
                GraphicsPipelineKey{data.SkinDiffuse ? 0x444546534b494eull : 0x4445464c49474854ull}, desc,
                framebuffer->getFramebufferInfo(), "Deferred shared PBR lighting");
            nvrhi::GraphicsState state;
            state.setPipeline(pipeline).setFramebuffer(framebuffer)
                .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                    nvrhi::Viewport(static_cast<float>(LastWidth), static_cast<float>(LastHeight))))
                .addBindingSet(GetLightingFrameBindingSet(context)).addBindingSet(bindingSet);
            context.GetCommandList()->setGraphicsState(state);
            context.GetCommandList()->draw(nvrhi::DrawArguments().setVertexCount(3));
        });
    if (SkinDiffuseHandle) AddSkinSubsurfacePasses();
}

void AddRasterSurfaceUnpackPass()
{
    Graph.AddPass<RasterUnpackPassData>("RasterSurfaceGuides", RenderGraphQueue::Compute,
        [this](RenderGraphBuilder& builder, RasterUnpackPassData& data)
        {
            data.bTaaOnly = !IsGlobalIlluminationActive();
            data.Inputs = { SceneDepthHandle, RasterSurfaceTargets[0], RasterSurfaceTargets[1],
                RasterSurfaceTargets[2], RasterSurfaceTargets[3], RasterSurfaceTargets[4] };
            // Base/emissive are directly shared with lighting consumers;
            // neither unpack shader reads them. Do not allocate or bind them
            // merely to satisfy an unused descriptor slot.
            data.Inputs[1] = {};
            data.Inputs[3] = {};
            for (auto& input : data.Inputs)
                if (input) input = builder.ShaderResource(input);
            const std::array<nvrhi::Format, 8> formats = {
                nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA16_FLOAT, nvrhi::Format::RGBA16_FLOAT,
                nvrhi::Format::RG16_FLOAT, nvrhi::Format::R32_UINT, nvrhi::Format::R16_FLOAT,
                nvrhi::Format::RG32_FLOAT, nvrhi::Format::RG32_UINT };
            for (size_t i = 0; i < formats.size(); ++i)
            {
                // Alias exact existing channels instead of round-tripping
                // RGBA8/16 and RG32_UINT through duplicate full-screen targets.
                if (i == 1 || i == 2 || i == 4 || i == 7) continue;
                if (data.bTaaOnly && i != 0 && i != 3 && i != 6) continue;
                // The compute shader overwrites every in-bounds pixel; the first
                // writer must not request prior contents of a new allocation.
                data.Outputs[i] = builder.Write(builder.CreateTexture(RenderGraphTextureDesc{
                    .DebugName = "Raster Consumer Guide " + std::to_string(i),
                    // TAA and GI share the full-precision current/previous view
                    // depth contract; half precision can also quantize the
                    // relative-depth rejection threshold at distant surfaces.
                    .Format = formats[i],
                    .bRelativeToGraph = true, .CapabilityUsage = RenderGraphUsage::ShaderResource }));
            }
            CurrentSurfaceData = {
                .Depth = SceneDepthHandle, .NormalRoughness = data.Outputs[0],
                .BaseColorMetalness = RasterSurfaceTargets[0], .Emissive = RasterSurfaceTargets[2],
                .MotionVectors = data.Outputs[3], .MaterialId = RasterSurfaceTargets[4],
                .ViewDepth = data.Outputs[6], .SurfaceId = RasterSurfaceTargets[4], .PreviousDepth = data.Outputs[5] };
            CurrentSurfaceData.bMaterialIdIsRayTracingIndex = false;
            // A scalar Texture2D<uint> reads the first component of this UINT
            // SRV. It is the validity identity, never an RT material index.
        },
        [this](RenderGraphContext& context, const RasterUnpackPassData& data)
        {
            std::array<nvrhi::ITexture*, 14> textures{};
            for (size_t i = 0; i < 6; ++i)
                if (data.Inputs[i]) textures[i] = context.GetTexture(data.Inputs[i], RenderGraphAccess::Read);
            for (size_t i = 0; i < 8; ++i)
                if (data.Outputs[i]) textures[i + 6] = context.GetTexture(data.Outputs[i], RenderGraphAccess::Write);
            auto& bindingSet = RasterUnpackBindingSets[textures];
            if (!bindingSet)
            {
                nvrhi::BindingSetDesc desc;
                desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(RasterUnpackConstants)));
                for (uint32_t i : {0u, 2u, 4u, 5u}) desc.addItem(nvrhi::BindingSetItem::Texture_SRV(i, textures[i]));
                for (uint32_t i = 0; i < 8; ++i)
                    if (textures[i + 6]) desc.addItem(nvrhi::BindingSetItem::Texture_UAV(i, textures[i + 6]));
                bindingSet = Device->createBindingSet(desc,
                    data.bTaaOnly ? RasterTaaGuideBindingLayout : RasterUnpackBindingLayout);
            }
            context.GetCommandList()->setComputeState(nvrhi::ComputeState()
                .setPipeline(data.bTaaOnly ? RasterTaaGuidePipeline : RasterUnpackPipeline).addBindingSet(bindingSet));
            const RasterUnpackConstants constants{
                CurrentRasterCamera.View * FrameData.InverseViewProjection,
                BuildSkyClipReprojection(FrameData.InverseViewProjection,
                    CurrentRasterCamera.PreviousViewProjection,
                    glm::vec3(glm::inverse(CurrentRasterCamera.View)[3])) };
            context.GetCommandList()->setPushConstants(&constants, sizeof(constants));
            context.GetCommandList()->dispatch((LastWidth + 7) / 8, (LastHeight + 7) / 8, 1);
        });
}

void UpdateRasterGuideRequirements()
{
    // Development A/B control: same scene, settings and shaders, full material
    // buffers versus demand-driven guides. Production always uses demand planning.
#if defined(HE_DEV_AGENT_RUNTIME)
    static const bool bForceFullSurface = [] {
        const char* value = std::getenv("HE_RENDER_FULL_SURFACE_DATA");
        return value && std::string_view(value) == "1";
    }();
#else
    constexpr bool bForceFullSurface = false;
#endif
    // Custom programs without a dedicated guide permutation retain their
    // actual surface shader. Never substitute built-in shading for them.
    const auto customDraw = [](const DrawItem& item) { return bool(item.MaterialProgram); };
    const bool bHasCustom = std::ranges::any_of(CpuDrawItems, customDraw) ||
        std::ranges::any_of(SkinnedDrawItems, customDraw) ||
        std::ranges::any_of(GpuBatches, [](const GpuBatch& batch) { return bool(batch.MaterialProgram); });
    const bool bDesired = !bForceFullSurface && Path == RenderingPath::Forward && !IsGlobalIlluminationActive() &&
        !ReflectionSettings.bScreenSpaceEnabled && !ReflectionSettings.bLocalProbesEnabled && !bHasCustom;
    if (bDesired != bRasterGuidesOnly)
    {
        bRasterGuidesOnly = bDesired;
        bGraphDirty = true;
    }
}
