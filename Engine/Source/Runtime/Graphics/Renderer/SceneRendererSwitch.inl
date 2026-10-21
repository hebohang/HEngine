// Included inside SceneRenderer::Impl after the raster/shadow declarations.
// A candidate owns only graph resources. Meshes, textures, material programs,
// GPU Scene, light grids and their immutable tables remain shared.
struct SceneGraphEffectBundle
{
    Scope<RayTracingScene> RayScene;
    Scope<SceneSurfaceDataPass> Surface;
    Scope<SvgfDenoiser> Denoiser;
    Scope<DepthPyramidPass> Depth;
    Scope<GlobalIlluminationCompositePass> Composite;
    // Destroy the technique before its borrowed scene and denoiser.
    GlobalIlluminationTechniqueLease Technique;
    Scope<SceneAmbientOcclusionPass> Ao;
    Scope<SceneReflectionPass> Reflection;
    Scope<ScenePostProcessPass> Post;
    Scope<SceneUpscalingPass> Upscaling;
    Scope<RenderFeaturePlan> Features;
};

void ExchangeGraphEffects(SceneGraphEffectBundle& effects) noexcept
{
    using std::swap;
    swap(HardwareRayTracingScene, effects.RayScene);
    swap(SurfaceDataPassInstance, effects.Surface);
    swap(SvgfDenoiserInstance, effects.Denoiser);
    swap(DepthPyramidPassInstance, effects.Depth);
    swap(CompositePassInstance, effects.Composite);
    swap(ActiveGiTechnique, effects.Technique);
    swap(AmbientOcclusionPass, effects.Ao);
    swap(ReflectionPass, effects.Reflection);
    swap(PostProcessPass, effects.Post);
    swap(UpscalingPass, effects.Upscaling);
    swap(Features, effects.Features);
}

struct PreparedSceneGraphSwitch
{
    RenderGraph CandidateGraph;
    Scope<RenderGraphResourcePool> CandidatePool;
    SceneGraphEffectBundle Effects;
    std::vector<std::function<void()>> StateExchanges;
    uint64_t RetirementSubmission = 0;
    uint64_t NominalPoolBytes = 0;
    bool bInitialConfiguration = false;
};

Scope<PreparedSceneGraphSwitch> PreparedGraphSwitch;
std::vector<Scope<PreparedSceneGraphSwitch>> RetiredGraphSwitches;
TextureHandle RasterLitHdrHandle;
SceneRendererFrameDesc PathSwitchFrame;
bool bHasPathSwitchFrame = false;
double LastPathPreparationMilliseconds = 0.0;
uint64_t LastPathCandidatePoolBytes = 0;
#if defined(HE_DEV_AGENT_RUNTIME)
bool bCandidateFailureInjected = false;
#endif

template<typename References, typename Values, size_t... Indices>
static void ExchangeGraphState(References references, Values& values,
    std::index_sequence<Indices...>) noexcept
{
    using std::swap;
    (swap(std::get<Indices>(references), std::get<Indices>(values)), ...);
}

std::function<void()> CaptureRendererGraphState()
{
    // These are graph-local handles, cache refs and graph-dependent selection
    // values. CPU scene rows and stable material/geometry ownership are omitted.
    const auto references = [this]() {
        return std::tie(Path, RequestedAaSettings, RequestedUpscaling, bFsrActiveLastFrame, RequestedGiSettings, EffectiveGiSettings,
            GiSelection, ActiveGiRequiredInputs, ActiveGiCapabilityRequirements,
            CurrentIlluminationTechniqueSettings, CurrentRtRevisions,
            GlobalIlluminationRenderSequence, bGlobalIlluminationFrameRecorded,
            CloudQualitySettings, AmbientOcclusionSettings, ReflectionSettings, PreparedReflectionProbes, PostProcessSettings,
            TextureStreamingBudgetBytes, bGpuOcclusionEnabled, bGpuBatchingEnabled, bGpuTightFrustumEnabled, bGpuShadowBatchingEnabled, bLodEnabled, bShadowsEnabled, bContinuousPcfEnabled, LodMaxPixelError,
            FrameData, CurrentRasterCamera, PendingRasterView, PendingRasterViewProjection,
            PendingRasterJitterPixels, PendingRasterWorldFrame,
            PendingTaaNonJitteredViewProjection, PendingRasterJitterGeneration,
            bRasterCameraFrameRecorded, PendingMaterialShaderPublicationRevision,
            AaSelection, bHasPreviousRasterCamera, bRasterCameraHistoryReset,
            BackBufferHandle, SceneColorHandle, RasterLitHdrHandle, OpaqueDirectHdrHandle, ToneMappedSceneHandle,
            SceneDepthHandle, RasterSceneDepthHandle, bPostTaaDepthRequired, bRasterGuidesOnly,
            CurrentVfxView, DirectionalShadowHandle,
            IrradianceHandle, PrefilterHandle, EnvironmentHandle, BrdfLutHandle,
            OffscreenColorHandle, OffscreenDepthHandle, RuntimeUiBackdropHandle,
            FrameBufferHandle, ObjectBufferHandle, GpuObjectsHandle, GpuMaterialsHandle,
            MaterialParametersHandle, GpuBoundsHandle, GpuDrawDataHandle,
            VisibleObjectIndicesHandle, IndirectArgumentsHandle, GpuReadbackHandle,
            Primitive2DVerticesHandle, BoneMatricesHandle, PreviousBoneMatricesHandle,
            ReactiveBudget,
            RasterSurfaceTargets, CurrentRtGraphResources, CurrentSurfaceData,
            CurrentDepthPyramid, CurrentGiOutputs, CurrentAmbientOcclusion,
            LocalShadowGraphResources, LightGraphResources, ShadowGpuGraphViews,
            GraphGeometryResources, GraphMaterialTextures, GraphArenaPages,
            DepthCompactedArgumentsHandle, MainCompactedArgumentsHandle,
            LastWidth, LastHeight, LastOutputWidth, LastOutputHeight, LastBackBufferFormat, LastBackBufferInitialState,
            CurrentCaptureSource,
            LastOutputMode, LastSceneOverlayMode, bLastReuseOffscreenScene, bGraphDirty, Stats,
            OpaqueFramebuffers, ToneFramebuffers, ToneBindingSets, ShadowFramebuffers,
            EnvironmentBindingSets, DirectionalShadowTextures, SkyBindingSets,
            LightingFrameBindingSets, RasterFramebuffers, DeferredBindingSets, RasterUnpackBindingSets,
            SkinDiffuseHandle, DeferredSkinFramebuffers, SkinSubsurfaceBindingSets);
    };
    auto values = std::apply([](const auto&... items) { return std::make_tuple(items...); }, references());
    std::vector<std::pair<MeshEntry*, uint32_t>> geometryIndices;
    geometryIndices.reserve(MeshOrder.size());
    for (const auto* key : MeshOrder)
    {
        auto& mesh = MeshCache.at(key);
        geometryIndices.emplace_back(&mesh, mesh.GraphIndex);
    }
    return [references, values = std::move(values), geometryIndices = std::move(geometryIndices)]() mutable noexcept {
        ExchangeGraphState(references(), values,
            std::make_index_sequence<std::tuple_size_v<decltype(values)>>{});
        for (auto& [mesh, index] : geometryIndices) std::swap(mesh->GraphIndex, index);
    };
}

std::vector<std::function<void()>> CaptureAllGraphState()
{
    std::vector<std::function<void()>> result;
    result.reserve(14);
    result.push_back(CaptureRendererGraphState());
    result.push_back(SceneAntiAliasing.CaptureGraphState());
    if (SvgfDenoiserInstance) result.push_back(SvgfDenoiserInstance->CaptureGraphState());
    if (IsGlobalIlluminationActive())
    {
        auto capsule = GetActiveIlluminationTechnique()->CaptureGraphState();
        if (!capsule)
            throw std::logic_error("Active GI extension has no graph transaction state support");
        result.push_back(std::move(capsule));
    }
    if (SurfaceDataPassInstance) result.push_back(SurfaceDataPassInstance->CaptureGraphState());
    if (DepthPyramidPassInstance) result.push_back(DepthPyramidPassInstance->CaptureGraphState());
    if (CompositePassInstance) result.push_back(CompositePassInstance->CaptureGraphState());
    result.push_back(Features->CaptureGraphState());
    result.push_back(AmbientOcclusionPass->CaptureGraphState());
    result.push_back(ReflectionPass->CaptureGraphState());
    result.push_back(PostProcessPass->CaptureGraphState());
    result.push_back(VfxRenderGraphPass.CaptureGraphState());
    result.push_back(RuntimeUiRenderPass.CaptureGraphState());
    result.push_back(GpuOcclusionPass->CaptureGraphState());
    result.push_back(DepthIndirectCompaction->CaptureGraphState());
    result.push_back(MainIndirectCompaction->CaptureGraphState());
    result.push_back(ShadowGpuSubmission->CaptureGraphState());
    return result;
}

void ClearAllGraphResourceCaches() noexcept
{
    OpaqueFramebuffers.clear(); ToneFramebuffers.clear(); ToneBindingSets.clear();
    ShadowFramebuffers.clear(); EnvironmentBindingSets.clear();
    DirectionalShadowTextures.clear(); SkyBindingSets.clear(); ClearRasterCaches();
    SceneAntiAliasing.ClearGraphResourceCaches();
    if (SvgfDenoiserInstance) SvgfDenoiserInstance->ClearGraphResourceCaches();
    if (IsGlobalIlluminationActive()) GetActiveIlluminationTechnique()->ClearGraphResourceCaches();
    if (SurfaceDataPassInstance) SurfaceDataPassInstance->ClearGraphResourceCaches();
    if (DepthPyramidPassInstance) DepthPyramidPassInstance->ClearCaches();
    if (CompositePassInstance) CompositePassInstance->ClearGraphResourceCaches();
    Features->ClearGraphResourceCaches();
    AmbientOcclusionPass->ClearGraphResourceCaches();
    ReflectionPass->ClearGraphResourceCaches();
    PostProcessPass->ClearGraphResourceCaches();
    VfxRenderGraphPass.ClearGraphResourceCaches();
    RuntimeUiRenderPass.ClearGraphResourceCaches();
    GpuOcclusionPass->ClearGraphResourceCaches();
    DepthIndirectCompaction->ClearGraphResourceCaches();
    MainIndirectCompaction->ClearGraphResourceCaches();
    ShadowGpuSubmission->ClearGraphResourceCaches();
}

static void ActivateGraphState(std::vector<std::function<void()>>& capsules) noexcept
{
    // Capsule construction can allocate; activation only swaps existing state.
    for (auto& capsule : capsules) if (capsule) capsule();
}

void RememberPathSwitchFrame(const SceneRendererFrameDesc& frame)
{
    PathSwitchFrame = frame;
    // Do not prolong swap-chain references or retain caller-owned callbacks.
    // The active graph already owns its current backbuffer until graph clear.
    PathSwitchFrame.BackBuffer = nullptr;
    PathSwitchFrame.CommandList = nullptr;
    PathSwitchFrame.SceneOverlayRecorder = {};
    PathSwitchFrame.WindowOverlayRecorder = {};
    PathSwitchFrame.CaptureRecorder = {};
    PathSwitchFrame.LinearHdrCaptureRecorder = {};
    PathSwitchFrame.WorldSnapshot.reset();
    PathSwitchFrame.bReuseOffscreenScene = false;
    bHasPathSwitchFrame = true;
}

void CollectPathGraphTransactions(uint64_t completedSubmission)
{
    std::erase_if(RetiredGraphSwitches, [completedSubmission](const auto& retired) {
        return retired->RetirementSubmission <= completedSubmission;
    });
}

void InvalidatePathSwitchFrame(uint64_t completedSubmission)
{
    // Call before resizing/releasing swap-chain resources, after the owner has
    // obtained its actual completed submission. Never substitutes WaitIdle.
    PreparedGraphSwitch.reset();
    CollectPathGraphTransactions(completedSubmission);
    PathSwitchFrame = {};
    bHasPathSwitchFrame = false;
}

void PrepareCandidateReflectionProbes(const RenderWorld& world)
{
    PreparedReflectionProbes.clear();
    if (!ReflectionSettings.bLocalProbesEnabled) return;
    std::vector<const RenderReflectionProbe*> probes;
    for (const auto& probe : world.ReflectionProbes)
        if (!probe.HdrTexturePath.empty()) probes.push_back(&probe);
    const auto distance = [&](const RenderReflectionProbe* probe) {
        const glm::vec3 delta = world.Camera.Position -
            glm::clamp(world.Camera.Position, probe->BoundsMin, probe->BoundsMax);
        return glm::dot(delta, delta);
    };
    std::stable_sort(probes.begin(), probes.end(), [&](auto* left, auto* right) {
        const float a = distance(left), b = distance(right);
        return a == b ? left->EntityId < right->EntityId : a < b;
    });
    for (size_t i = 0; i < std::min(probes.size(), size_t(MaximumSceneReflectionProbes)); ++i)
    {
        const auto& probe = *probes[i];
        // A probe can first become visible to the renderer when this candidate
        // enables probes. Reuse the normal resource-preparation cache instead
        // of requiring a successful draw with probes already enabled. Cold
        // preparation completes its upload before returning; it does not select
        // the active environment or mutate the previous graph. Ready entries
        // take the cache-only path on subsequent configuration switches.
        const auto preparation = PrepareEnvironment(probe.HdrTexturePath);
        if (preparation.State != SceneEnvironmentState::Ready)
            throw std::runtime_error("Candidate reflection probe environment is not ready: " +
                probe.HdrTexturePath + (preparation.Error.empty() ? std::string{} : ": " + preparation.Error));
        const auto found = EnvironmentCache.find(preparation.Path);
        if (found == EnvironmentCache.end() || !found->second.Prefilter)
            throw std::runtime_error("Candidate reflection probe has no prepared cubemap: " + probe.HdrTexturePath);
        PreparedReflectionProbes.push_back(SceneReflectionProbe{
            .BoundsMin = probe.BoundsMin, .BlendDistance = probe.BlendDistance,
            .BoundsMax = probe.BoundsMax, .Intensity = probe.Intensity,
            .CapturePosition = probe.CapturePosition,
            .PrefilteredEnvironment = found->second.Prefilter,
            .Irradiance = probe.bAffectDiffuse ? found->second.Irradiance : nullptr,
        });
    }
}

void PrepareConfigurationEffects(const SceneRendererDesc& target,
    const RenderWorld& world, SceneRendererFrameDesc& frame, bool bPrepareFrame)
{
    Path = target.Path;
    RequestedAaSettings = target.AntiAliasing;
    RequestedUpscaling = target.Upscaling;
    TextureStreamingBudgetBytes = target.TextureStreamingBudgetBytes;
    CloudQualitySettings = target.Clouds;
    AmbientOcclusionSettings = target.AmbientOcclusion;
    ReflectionSettings = target.Reflections;
    PostProcessSettings = target.PostProcess;
    UpscalingPass = CreateScope<SceneUpscalingPass>(SceneAntiAliasingPassDesc{
        .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
        .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
    const bool canUpscale = !world.bEditorView && frame.OutputMode == SceneRendererOutputMode::SwapChain;
    const auto fallback = ResolveRenderExtent(LastOutputWidth, LastOutputHeight,
        canUpscale ? target.PostProcess.RenderHeight : 0);
    const auto extent = UpscalingPass->Prepare(target.Upscaling, {LastOutputWidth, LastOutputHeight},
        fallback, canUpscale && target.GlobalIllumination.DebugView == GlobalIlluminationDebugView::Composite,
        frame.CompletedSubmission);
    frame.Width = extent.Width; frame.Height = extent.Height;
    LastWidth = frame.Width; LastHeight = frame.Height;
    Features = CreateScope<RenderFeaturePlan>(target.PipelineFeatures, RenderFeatureCreateContext{
        .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
        .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(),
        .AssetRootDirectory = AssetRootDirectory, .EngineAssetRootDirectory = EngineAssetRootDirectory,
        .Backend = Backend, .FramesInFlight = FramesInFlight });
    bGpuOcclusionEnabled = target.bGpuOcclusionEnabled;
    bGpuBatchingEnabled = target.bGpuBatchingEnabled && SupportsCountedIndirectDraws(Capabilities);
    bGpuTightFrustumEnabled = target.bGpuTightFrustumEnabled;
    bGpuShadowBatchingEnabled = target.bGpuShadowBatchingEnabled && SupportsCountedIndirectDraws(Capabilities);
    bLodEnabled = target.bLodEnabled;
    bShadowsEnabled = target.bShadowsEnabled;
    bContinuousPcfEnabled = target.bContinuousPcfEnabled;
    LodMaxPixelError = target.LodMaxPixelError;
    ConfigureGlobalIllumination(target.GlobalIllumination);
    AmbientOcclusionPass = CreateScope<SceneAmbientOcclusionPass>(SceneAmbientOcclusionPassDesc{
        .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
        .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
    ReflectionPass = CreateScope<SceneReflectionPass>(SceneReflectionPassDesc{
        .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
        .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
    PostProcessPass = CreateScope<ScenePostProcessPass>(ScenePostProcessPassDesc{
        .Device = Device, .SharedShaderManager = &MaterialShaderHandleManagerInstance,
        .ShaderArtifactCacheRoot = ShaderCacheDirectory.parent_path(), .Backend = Backend });
    RefreshAntiAliasingSelection();
    bHasPreviousRasterCamera = false;
    bRasterCameraHistoryReset = true;
    bGraphDirty = true;
    if (!bPrepareFrame) return;
    PrepareCandidateReflectionProbes(world);
    PrepareAntiAliasingFrame(world, frame);
    FrameData.ViewProjection = CurrentRasterCamera.ViewProjection;
    FrameData.InverseViewProjection = glm::inverse(CurrentRasterCamera.ViewProjection);
    FrameData.IblSettings.w = IsGlobalIlluminationActive() ? 1.0f : 0.0f;
    PostProcessPass->PrepareFrame(ResolveViewPostProcess(world), 0.0f, Exposure,
        frame.CompletedSubmission, true);
    (void)PostProcessPass->ConsumeGraphRebuildRequest();
    Features->PrepareFrame(RenderFeatureFrameContext{
        .World = &world, .RasterCamera = &CurrentRasterCamera, .Clouds = &CloudQualitySettings,
        .Width = frame.Width, .Height = frame.Height, .CompletedSubmission = frame.CompletedSubmission,
        .bOffline = IsOfflineViewportRenderingActive() });
    (void)Features->ConsumeGraphRebuildRequest();
    AmbientOcclusionPass->PrepareFrame(AmbientOcclusionSettings, CurrentRasterCamera,
        frame.Width, frame.Height, frame.CompletedSubmission);
    (void)AmbientOcclusionPass->ConsumeGraphRebuildRequest();
    ReflectionPass->PrepareFrame(ReflectionSettings, CurrentRasterCamera,
        frame.Width, frame.Height, FrameData.IblSettings.x, FrameData.IblSettings.y,
        PreparedReflectionProbes, frame.CompletedSubmission);
    (void)ReflectionPass->ConsumeGraphRebuildRequest();
    PrepareSceneSurfaceFrame(world, frame);
    PrepareGlobalIlluminationFrame(world, frame);
    bGlobalIlluminationFrameRecorded = false;
    bRasterCameraFrameRecorded = false;
}

void PrewarmConfigurationPipelines()
{
    auto color = Graph.GetPhysicalTexture(RasterLitHdrHandle);
    auto depth = Graph.GetPhysicalTexture(RasterSceneDepthHandle);
    auto* forwardFramebuffer = GetOrCreateOpaqueFramebuffer(color, depth);
    auto* depthFramebuffer = GetOrCreateShadowFramebuffer(Graph.GetPhysicalTexture(RasterSceneDepthHandle));
    for (bool bDoubleSided : {false, true})
    {
        (void)GetOrCreateGpuOpaquePipeline(forwardFramebuffer, bDoubleSided);
        (void)GetOrCreateSkinnedPipeline(forwardFramebuffer, false, bDoubleSided);
        (void)GetOrCreateSkinnedPipeline(forwardFramebuffer, true, bDoubleSided);
        (void)GetOrCreateTransparentPipeline(forwardFramebuffer, bDoubleSided);
        for (uint32_t vertexPath = 0; vertexPath < 3; ++vertexPath)
            (void)GetOrCreateDepthPrepassPipeline(depthFramebuffer,
                vertexPath == 2, vertexPath == 1, bDoubleSided);
    }
    (void)GetOrCreateOpaquePipeline(forwardFramebuffer);
    (void)GetOrCreateSkyPipeline(forwardFramebuffer);
    nvrhi::FramebufferHandle rasterFramebuffer;
    if (RasterSurfaceTargets[0])
    {
        PrepareRasterShaders();
        nvrhi::FramebufferDesc desc;
        for (const auto target : RasterSurfaceTargets)
            desc.addColorAttachment(Graph.GetPhysicalTexture(target));
        desc.setDepthAttachment(Graph.GetPhysicalTexture(SceneDepthHandle));
        rasterFramebuffer = Device->createFramebuffer(desc);
        if (!rasterFramebuffer) throw std::runtime_error("Candidate GBuffer framebuffer creation failed");
        for (uint32_t vertexPath = 0; vertexPath < 3; ++vertexPath)
            for (bool bDoubleSided : {false, true})
                (void)GetRasterPipeline(rasterFramebuffer, vertexPath, bDoubleSided);
    }
    if (Path == RenderingPath::Deferred)
    {
        nvrhi::GraphicsPipelineDesc lighting;
        lighting.setPrimType(nvrhi::PrimitiveType::TriangleList)
            .setVertexShader(DeferredVertexShader).setPixelShader(DeferredPixelShader)
            .addBindingLayout(FrameBindingLayout).addBindingLayout(DeferredBindingLayout);
        lighting.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
        lighting.renderState.rasterState.setCullNone();
        (void)PipelineCacheInstance.GetOrCreateGraphicsPipeline(
            GraphicsPipelineKey{0x4445464c49474854ull}, lighting,
            GetOrCreateToneFramebuffer(color)->getFramebufferInfo(), "Deferred shared PBR lighting");
    }
    for (const auto* items : {&CpuDrawItems, &SkinnedDrawItems})
        for (const auto& item : *items)
            if (item.MaterialProgram)
            {
                const bool bSkinned = items == &SkinnedDrawItems;
                // Match DrawDepthPrepass: custom opacity is evaluated by the
                // Mask permutation; opaque Surface materials use the common
                // geometry depth pipeline already prepared above.
                if (item.MaterialProgram->GetManifest().BlendMode == MaterialShaderBlendMode::Mask)
                    (void)GetOrCreateMaterialPipeline(depthFramebuffer, *item.MaterialProgram,
                        MaterialShaderPass::Depth, bSkinned, item.bDoubleSided, false);
                // Forward with temporal/GI guides also draws the GBuffer.
                // Every pass present in the candidate graph must be prepared.
                if (rasterFramebuffer)
                    (void)GetOrCreateMaterialPipeline(rasterFramebuffer, *item.MaterialProgram,
                        MaterialShaderPass::GBuffer, bSkinned, item.bDoubleSided, false);
                if (Path == RenderingPath::Forward)
                    (void)GetOrCreateMaterialPipeline(forwardFramebuffer, *item.MaterialProgram,
                        MaterialShaderPass::Forward, bSkinned, item.bDoubleSided, false);
            }
    for (const auto& item : TransparentDrawItems)
        if (item.MaterialProgram)
            (void)GetOrCreateMaterialPipeline(forwardFramebuffer, *item.MaterialProgram,
                MaterialShaderPass::Forward, item.bSkinned, item.bDoubleSided, true);
    (void)GetOrCreateTonePipeline(GetOrCreateToneFramebuffer(
        Graph.GetPhysicalTexture(ToneMappedSceneHandle)));
}

SceneRendererConfigurationPreparationResult PrepareConfigurationGraphTransaction(
    const SceneRendererDesc& target, const RenderWorld& world,
    const SceneRendererFrameDesc* preparationFrame)
{
    PreparedGraphSwitch.reset();
    ValidateAntiAliasingSettings(target.AntiAliasing);
    if (target.TextureStreamingBudgetBytes < 1024ull * 1024 || target.TextureStreamingBudgetBytes > 8192ull * 1024 * 1024)
        throw std::invalid_argument("Texture streaming budget must be in [1, 8192] MiB");
    if (!std::isfinite(target.LodMaxPixelError) || target.LodMaxPixelError < 0.1f || target.LodMaxPixelError > 8.0f)
        throw std::invalid_argument("LOD maximum pixel error must be in [0.1, 8]");
    for (uint32_t slot = 0; slot < FramesInFlight; ++slot)
        if (ResourcePool.IsFrameActive(slot))
            throw std::logic_error("Rendering path preparation requires a frame boundary");
    auto candidate = CreateScope<PreparedSceneGraphSwitch>();
    candidate->CandidatePool = CreateScope<RenderGraphResourcePool>(Device, FramesInFlight, target.bTransientTextureReuse);
    candidate->CandidatePool->SetTransientCacheBudget(target.TransientCacheBudgetBytes);
    // Reserve the fence-retirement record before committing any state.
    RetiredGraphSwitches.reserve(RetiredGraphSwitches.size() + 1);
    candidate->bInitialConfiguration = !bHasPathSwitchFrame || !Graph.IsCompiled() || LastSubmittedSubmission == 0;
    const bool bColdPreparation = candidate->bInitialConfiguration && preparationFrame;
    if (bColdPreparation && (!preparationFrame->Width || !preparationFrame->Height ||
        (preparationFrame->OutputMode != SceneRendererOutputMode::OffscreenOnly && !preparationFrame->BackBuffer)))
        throw std::invalid_argument("Pipeline preparation requires a valid host output context");
    const bool bPrepareGraph = !candidate->bInitialConfiguration || bColdPreparation;
    if (IsOfflineViewportRenderingActive())
        throw std::logic_error("Raster path switching requires the raster viewport renderer");

    const auto start = std::chrono::steady_clock::now();
    if (!candidate->bInitialConfiguration)
    {
        // Native 4K SSGI switching exceeded the 6 GiB laptop budget when
        // retaining old storage beside independently allocated candidate banks.
        // This bounded configuration boundary wait enables transactional handle
        // reuse; steady frames retain their existing asynchronous fence path.
        const uint64_t lastSubmission = std::max(LastSubmittedSubmission, ResourcePool.GetLastSubmission());
        if (lastSubmission > CurrentCompletedSubmission)
        {
            if (!Device->waitForIdle())
                throw std::runtime_error("Failed to complete GPU work before candidate storage reuse");
            CurrentCompletedSubmission = lastSubmission;
        }
        CollectPathGraphTransactions(CurrentCompletedSubmission);
        ResourcePool.Collect(CurrentCompletedSubmission);
        candidate->CandidatePool->SeedCompletedTransients(ResourcePool, CurrentCompletedSubmission);
    }
    auto previousState = CaptureAllGraphState();
    SceneGraphEffectBundle previousEffects;
    SceneRendererFrameDesc frame = bColdPreparation ? *preparationFrame : PathSwitchFrame;
    if (!candidate->bInitialConfiguration && frame.OutputMode != SceneRendererOutputMode::OffscreenOnly)
        frame.BackBuffer = Graph.GetPhysicalTexture(BackBufferHandle);
    frame.CompletedSubmission = CurrentCompletedSubmission;
    Graph.Swap(candidate->CandidateGraph);
    try
    {
        ClearAllGraphResourceCaches();
        ExchangeGraphEffects(previousEffects);
        if (bPrepareGraph)
        {
            // RememberPathSwitchFrame stores the previous INTERNAL extent. The
            // candidate must derive its new extent from the physical output and
            // the target cap, including cold preparation before RecordFrame runs.
            if (frame.OutputMode == SceneRendererOutputMode::OffscreenOnly)
            {
                LastOutputWidth = OffscreenOutput.Width;
                LastOutputHeight = OffscreenOutput.Height;
            }
            else
            {
                if (!frame.BackBuffer)
                    throw std::invalid_argument("Pipeline preparation requires a physical output texture");
                LastOutputWidth = frame.BackBuffer->getDesc().width;
                LastOutputHeight = frame.BackBuffer->getDesc().height;
            }
            const bool bCanScale = !world.bEditorView &&
                frame.OutputMode == SceneRendererOutputMode::SwapChain &&
                !IsOfflineViewportRenderingActive();
            const auto extent = ResolveRenderExtent(LastOutputWidth, LastOutputHeight,
                bCanScale ? target.PostProcess.RenderHeight : 0);
            frame.Width = extent.Width;
            frame.Height = extent.Height;
            LastWidth = frame.Width;
            LastHeight = frame.Height;
        }
        PrepareConfigurationEffects(target, world, frame, bPrepareGraph);
        if (bColdPreparation)
        {
            // New registry instances have no previous CPU scene tables. Build
            // their real geometry/material inputs before graph/PSO validation.
            CurrentOutputMode = frame.OutputMode;
            CurrentSceneOverlayMode = frame.SceneOverlayMode;
            LastWidth = frame.Width;
            LastHeight = frame.Height;
            static const RenderWorld noShadowCasters;
            (void)LocalShadowPass->PrepareFrame(bShadowsEnabled ? world : noShadowCasters, frame.CompletedSubmission);
            LightGrid->SetLocalShadowIndices(LocalShadowPass->GetPointShadowViews(), LocalShadowPass->GetSpotShadowViews());
            PrepareLightGrid(world, frame);
            PrepareFrameData(world);
            PrepareGpuBatchFrame();
            PrepareGpuShadowFrame();
            GpuOcclusionPass->PrepareFrame(CurrentRasterCamera,
                static_cast<uint32_t>(GpuDrawDataCpu.size()), static_cast<uint32_t>(GpuBatches.size()), frame.CompletedSubmission);
            PrepareSceneSurfaceFrame(world, frame);
            PrepareGlobalIlluminationFrame(world, frame);
        }
        if (bPrepareGraph)
        {
            LastWidth = frame.Width; LastHeight = frame.Height;
            BuildGraph(frame);
            // Every bank is proven before commit. Matching completed transient
            // storage is shared with the inactive rollback pool, so the next
            // two frames cannot discover a deferred allocation failure.
            for (uint32_t slot = 0; slot < FramesInFlight; ++slot)
            {
                candidate->CandidatePool->BeginFrame(slot, CurrentCompletedSubmission);
                try
                {
                    Graph.PrepareResources(*candidate->CandidatePool, slot);
                    PrewarmConfigurationPipelines();
                    candidate->CandidatePool->EndPreparation(slot);
                }
                catch (...)
                {
                    candidate->CandidatePool->AbortFrame(slot);
                    throw;
                }
            }
        }
        const auto poolStats = candidate->CandidatePool->GetStats();
        candidate->NominalPoolBytes = poolStats.PersistentTextureBytes + poolStats.PersistentBufferBytes +
            poolStats.TransientTextureBytes + poolStats.TransientBufferBytes;
#if defined(HE_DEV_AGENT_RUNTIME)
        // Fault injection is consumed only after a real candidate was fully
        // allocated, and is intentionally excluded from rollback capsules.
        if (bPrepareGraph && !bCandidateFailureInjected)
        {
            const char* fault = std::getenv("HE_DEV_RENDER_FAIL_CANDIDATE_ONCE");
            const char* previewOnly = std::getenv("HE_DEV_RENDER_FAIL_CANDIDATE_PREVIEW_ONLY");
            if (fault && std::string_view(fault) == "1" &&
                (!(previewOnly && std::string_view(previewOnly) == "1") ||
                    frame.OutputMode == SceneRendererOutputMode::OffscreenOnly))
            {
                bCandidateFailureInjected = true;
                throw std::runtime_error("Injected candidate failure after candidate resource allocation");
            }
        }
#endif
        candidate->StateExchanges = CaptureAllGraphState();
        // Return the original graph and every consumer's bindings before the
        // renderer-wide transaction proceeds to preparation of another view.
        Graph.Swap(candidate->CandidateGraph);
        ExchangeGraphEffects(previousEffects);
        candidate->Effects = std::move(previousEffects);
        ActivateGraphState(previousState);
    }
    catch (...)
    {
        Graph.Swap(candidate->CandidateGraph);
        ExchangeGraphEffects(previousEffects);
        ActivateGraphState(previousState);
        throw;
    }
    LastPathPreparationMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    LastPathCandidatePoolBytes = candidate->NominalPoolBytes;
    const SceneRendererConfigurationPreparationResult result{
        .bGraphPrepared = bPrepareGraph,
        .CandidatePoolNominalBytes = candidate->NominalPoolBytes,
        .PreparationMilliseconds = LastPathPreparationMilliseconds,
    };
    PreparedGraphSwitch = std::move(candidate);
    return result;
}

void CommitConfigurationGraphTransaction() noexcept
{
    if (!PreparedGraphSwitch) return;

    // All operations before history reset are allocation-free exchanges. The
    // prepared state capsule now holds the OLD cache refs, graph handles and AA
    // history allocations, and is retained beside the old graph and old pool.
    PreparedGraphSwitch->RetirementSubmission = std::max(LastSubmittedSubmission,
        ResourcePool.GetLastSubmission());
    Graph.Swap(PreparedGraphSwitch->CandidateGraph);
    ResourcePool.Swap(*PreparedGraphSwitch->CandidatePool);
    ExchangeGraphEffects(PreparedGraphSwitch->Effects);
    ActivateGraphState(PreparedGraphSwitch->StateExchanges);
    RetiredGraphSwitches.push_back(std::move(PreparedGraphSwitch));
    PostProcessPass->ResetHistory();
    SceneAntiAliasing.ResetHistory();
    UpscalingPass->ResetHistory();
    bHasPreviousRasterCamera = false;
    // Candidate effects have fresh temporal allocations. GI reset happened
    // during preparation; no extension call or logging can throw after commit.
}
