#include "Hepch.h"

#include "Runtime/Graphics/Renderer/SceneCloudPass.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <vector>
#include <utility>

namespace HEngine
{
    namespace
    {
        struct alignas(16) CloudConstants
        {
            glm::mat4 InverseViewProjection{ 1 }, RasterViewProjection{ 1 }, PreviousViewProjection{ 1 };
            glm::vec4 Camera{}, Layer{}, Shape{}, Wind{}, PreviousWind{}, Lit{}, Shadow{}, Sun{}, SunColor{};
            glm::uvec4 Dimensions{};
            glm::vec4 Quality{};
            glm::uvec4 State{};
        };
        static_assert(sizeof(CloudConstants) == 384);
        struct CloudPassData
        {
            std::array<TextureHandle, 6> Inputs;
            TextureHandle Output, DepthOutput;
            uint32_t Operation = 0;
        };
        struct CompositeData { TextureHandle Color, Depth, Cloud, Guide; };
        struct CpuScope
        {
            double& Total;
            std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
            ~CpuScope() { Total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count(); }
        };
        using BindingKey = std::array<nvrhi::ITexture*, 8>;
        struct Cache
        {
            std::map<BindingKey, nvrhi::BindingSetHandle> ComputeBindings, CompositeBindings;
            std::map<nvrhi::ITexture*, nvrhi::FramebufferHandle> Framebuffers;
        };
        struct History
        {
            std::array<nvrhi::TextureHandle, 2> Color, Depth;
            std::array<bool, 2> Submitted{};
        };
    }

    struct SceneCloudPass::Impl
    {
        explicit Impl(const SceneAmbientOcclusionPassDesc& desc) : Device(desc.Device)
        {
            if (!Device || !desc.SharedShaderManager) throw std::invalid_argument("Cloud pass needs shared shader manager");
            auto load = [&](const char* file, const char* source, const char* entry,
                const char* profile, nvrhi::ShaderType stage, const char* variant = "BASE")
            {
                const auto format = GetShaderBinaryFormat(desc.Backend);
                const auto path = desc.ShaderArtifactCacheRoot / std::string(GetShaderArtifactDirectoryName(format)) / file;
                std::ifstream stream(MakeShaderCacheKeyPath(path));
                std::string hash;
                for (std::string line; std::getline(stream, line);)
                    if (line.starts_with("key=")) { hash = line.substr(4); break; }
                if (hash.empty()) throw std::runtime_error("Cloud shader artifact cache key missing: " + path.string());
                return desc.SharedShaderManager->LoadShaderFile(ShaderCacheKey{
                    .Source = source, .Variant = variant, .Stage = stage, .EntryPoint = entry,
                    .TargetProfile = profile, .ContentHash = hash, .Backend = desc.Backend,
                    .BinaryFormat = format }, path, file);
            };
            Compute = load("SceneCloud.cs", "Modern/SceneCloud.hlsl", "CS", "cs_6_0", nvrhi::ShaderType::Compute);
            Vertex = load("SceneCloudComposite.vs", "Modern/SceneCloudComposite.hlsl", "VS", "vs_6_0", nvrhi::ShaderType::Vertex);
            Pixel = load("SceneCloudComposite.ps", "Modern/SceneCloudComposite.hlsl", "PS", "ps_6_0", nvrhi::ShaderType::Pixel);
            PixelMsaa = load("SceneCloudComposite_MSAA.ps", "Modern/SceneCloudComposite.hlsl", "PS", "ps_6_0", nvrhi::ShaderType::Pixel, "MSAA");
            ConstantsBuffer = Device->createBuffer(nvrhi::BufferDesc().setByteSize(sizeof(CloudConstants))
                .setIsConstantBuffer(true).setIsVolatile(true).setMaxVersions(64).setDebugName("Cloud Constants"));
            Sampler = Device->createSampler(nvrhi::SamplerDesc().setAllFilters(true)
                .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap));
            nvrhi::BindingLayoutDesc layout;
            layout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            for (uint32_t i = 0; i < 6; ++i) layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(i));
            for (uint32_t i = 0; i < 2; ++i) layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(i));
            ComputeLayout = Device->createBindingLayout(layout);
            ComputePipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
                .setComputeShader(Compute).addBindingLayout(ComputeLayout));
            nvrhi::BindingLayoutDesc graphics;
            graphics.setVisibility(nvrhi::ShaderType::AllGraphics).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
            for (uint32_t i = 0; i < 3; ++i) graphics.addItem(nvrhi::BindingLayoutItem::Texture_SRV(i));
            CompositeLayout = Device->createBindingLayout(graphics);
            if (!ComputePipeline || !CompositeLayout || !ConstantsBuffer || !Sampler)
                throw std::runtime_error("Failed to create cloud immutable GPU state");
        }

        nvrhi::TextureHandle Texture(uint32_t width, uint32_t height, nvrhi::Format format, const char* name)
        {
            auto result = Device->createTexture(nvrhi::TextureDesc().setWidth(width).setHeight(height)
                .setFormat(format).setIsUAV(true).setInitialState(nvrhi::ResourceStates::Common).setDebugName(name));
            if (!result) throw std::runtime_error("Failed to allocate cloud history");
            return result;
        }
        void Retire(bool keepNoise = false)
        {
            Retired.push_back({ LastSubmission, std::move(Resources), std::move(Caches), Noise });
            Resources = {}; Caches = {};
            if (!keepNoise) { Noise = nullptr; bNoiseSubmitted = false; }
            bValid = false; Sequence = 0;
        }
        void Allocate(uint32_t width, uint32_t height)
        {
            for (uint32_t i = 0; i < 2; ++i)
            {
                Resources.Color[i] = Texture(width, height, nvrhi::Format::RGBA16_FLOAT, "Cloud History LT");
                Resources.Depth[i] = Texture(width, height, nvrhi::Format::RG32_FLOAT, "Cloud History Depth");
            }
            if (Noise) return;
            Noise = Device->createTexture(nvrhi::TextureDesc().setWidth(64).setHeight(64).setDepth(64)
                .setDimension(nvrhi::TextureDimension::Texture3D).setFormat(nvrhi::Format::R8_UNORM)
                .setInitialState(nvrhi::ResourceStates::Common).setDebugName("Cloud Periodic Shape Noise"));
            if (!Noise) throw std::runtime_error("Failed to allocate cloud noise");
            NoiseBytes.resize(64 * 64 * 64);
            // Periodic trilinear value noise. Fixed integer hash, no runtime files.
            auto hash = [seed = Art.Seed](int x, int y, int z)
            {
                uint32_t h = uint32_t(x & 7) * 73856093u ^ uint32_t(y & 7) * 19349663u ^
                    uint32_t(z & 7) * 83492791u ^ seed;
                h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
                return float(h & 65535u) / 65535.0f;
            };
            for (int z = 0; z < 64; ++z) for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
            {
                const glm::vec3 p = glm::vec3(x, y, z) / 8.0f;
                const glm::ivec3 cell = glm::floor(p);
                glm::vec3 f = glm::fract(p); f = f * f * (3.0f - 2.0f * f);
                float value = 0;
                for (int dz = 0; dz < 2; ++dz) for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
                    value += hash(cell.x + dx, cell.y + dy, cell.z + dz) *
                        (dx ? f.x : 1 - f.x) * (dy ? f.y : 1 - f.y) * (dz ? f.z : 1 - f.z);
                NoiseBytes[(z * 64 + y) * 64 + x] = uint8_t(std::clamp(value * 255.0f, 0.0f, 255.0f));
            }
        }
        TextureHandle Import(RenderGraph& graph, const nvrhi::TextureHandle& texture, bool submitted)
        {
            const auto& d = texture->getDesc();
            return graph.ImportTexture(RenderGraphTextureDesc{ .DebugName = d.debugName,
                .Width = d.width, .Height = d.height, .Depth = d.depth, .Format = d.format, .Dimension = d.dimension },
                texture, submitted ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::ShaderResource);
        }
        nvrhi::IBindingSet* Bind(const BindingKey& key, bool graphics)
        {
            auto& map = graphics ? Caches.CompositeBindings : Caches.ComputeBindings;
            auto found = map.find(key); if (found != map.end()) return found->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, ConstantsBuffer));
            const uint32_t count = graphics ? 3 : 6;
            for (uint32_t i = 0; i < count; ++i) desc.addItem(nvrhi::BindingSetItem::Texture_SRV(i, key[i]));
            if (!graphics)
            {
                desc.addItem(nvrhi::BindingSetItem::Sampler(0, Sampler));
                desc.addItem(nvrhi::BindingSetItem::Texture_UAV(0, key[6]));
                desc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, key[7]));
            }
            auto binding = Device->createBindingSet(desc, graphics ? CompositeLayout : ComputeLayout);
            if (!binding) throw std::runtime_error("Cloud resource binding failed");
            return map.emplace(key, std::move(binding)).first->second;
        }
        void Stage(RenderGraph& graph, const char* name, uint32_t operation,
            std::array<TextureHandle, 6> inputs, TextureHandle output, TextureHandle guide)
        {
            graph.AddPass<CloudPassData>(name, RenderGraphQueue::Compute,
                [=](RenderGraphBuilder& builder, CloudPassData& data)
                {
                    for (size_t i = 0; i < inputs.size(); ++i) data.Inputs[i] = builder.ShaderResource(inputs[i]);
                    data.Output = builder.Write(output); data.DepthOutput = builder.Write(guide); data.Operation = operation;
                }, [this](RenderGraphContext& context, const CloudPassData& data)
                {
                    CpuScope measured{ CpuRecordMilliseconds };
                    BindingKey key{};
                    for (size_t i = 0; i < data.Inputs.size(); ++i) key[i] = context.GetTexture(data.Inputs[i], RenderGraphAccess::Read);
                    key[6] = context.GetTexture(data.Output, RenderGraphAccess::Write);
                    key[7] = context.GetTexture(data.DepthOutput, RenderGraphAccess::Write);
                    auto constants = Constants; constants.State.x = data.Operation;
                    auto* cmd = context.GetCommandList();
                    cmd->writeBuffer(ConstantsBuffer, &constants, sizeof(constants));
                    cmd->setComputeState(nvrhi::ComputeState().setPipeline(ComputePipeline).addBindingSet(Bind(key, false)));
                    cmd->dispatch((Constants.Dimensions.z + 7) / 8, (Constants.Dimensions.w + 7) / 8, 1);
                    bRecorded = true;
                });
        }
        nvrhi::DeviceHandle Device;
        nvrhi::ShaderHandle Compute, Vertex, Pixel, PixelMsaa;
        nvrhi::BindingLayoutHandle ComputeLayout, CompositeLayout;
        nvrhi::ComputePipelineHandle ComputePipeline;
        std::map<uint32_t, nvrhi::GraphicsPipelineHandle> Pipelines;
        nvrhi::BufferHandle ConstantsBuffer;
        nvrhi::SamplerHandle Sampler;
        nvrhi::TextureHandle Noise;
        std::vector<uint8_t> NoiseBytes;
        History Resources;
        Cache Caches;
        struct RetiredState { uint64_t Submission; History Resources; Cache Caches; nvrhi::TextureHandle Noise; };
        std::vector<RetiredState> Retired;
        CloudConstants Constants;
        CloudSettings Settings;
        RenderCloudLayer Art;
        glm::mat4 PreviousView{ 1 }, PendingView{ 1 };
        glm::vec4 PreviousWind{};
        uint64_t LastSubmission = 0, SceneGeneration = 0, ProjectionGeneration = 0;
        uint64_t LastWorldFrameNumber = 0;
        uint32_t Sequence = 0, PreviousIndex = 0;
        uint32_t ConvergenceFrameCount = 0;
        double CpuRecordMilliseconds = 0;
        bool bActive = false, bValid = false, bRecorded = false, bNoiseSubmitted = false, bRebuild = true;
        TextureHandle NoiseHandle, PreviousColor, PreviousDepth, CurrentColor, CurrentDepth;
    };

    SceneCloudPass::SceneCloudPass(const SceneAmbientOcclusionPassDesc& desc) : mImpl(CreateScope<Impl>(desc)) {}
    SceneCloudPass::~SceneCloudPass() = default;

    void SceneCloudPass::PrepareFrame(const CloudSettings& settings, const RenderWorld& world,
        const RenderCamera& raster, uint32_t width, uint32_t height, uint64_t completed)
    {
        auto& s = *mImpl;
        s.CpuRecordMilliseconds = 0;
        CpuScope measured{ s.CpuRecordMilliseconds };
        Collect(completed);
        const auto quality = ResolveCloudSettings(settings);
        auto art = world.Clouds; NormalizeCloudLayer(art);
        const bool active = quality.Quality != CloudQuality::Off && art.bEnabled;
        const uint32_t w = std::max(1u, uint32_t(std::ceil(width * quality.RenderScale)));
        const uint32_t h = std::max(1u, uint32_t(std::ceil(height * quality.RenderScale)));
        const bool resize = w != s.Constants.Dimensions.z || h != s.Constants.Dimensions.w;
        const bool reallocate = active != s.bActive || (active && (resize || art.Seed != s.Art.Seed));
        const bool keepNoise = active && s.bActive && art.Seed == s.Art.Seed;
        s.bRebuild |= reallocate;
        s.bValid &= !(reallocate || art != s.Art || quality != s.Settings || world.Camera.bCameraCut ||
            world.SceneGeneration != s.SceneGeneration || world.Camera.ProjectionGeneration != s.ProjectionGeneration);
        if (!s.bValid) s.Sequence = 0;
        // A new extraction may move the camera, lights or cloud time without
        // discarding temporal history. It still needs fresh convergence before
        // the offscreen result can be reused again.
        if (!s.bValid || world.FrameNumber != s.LastWorldFrameNumber)
            s.ConvergenceFrameCount = 0;
        s.LastWorldFrameNumber = world.FrameNumber;
        s.Settings = quality; s.Art = art; s.bActive = active;
        if (reallocate)
        {
            s.Retire(keepNoise);
            if (active) s.Allocate(w, h);
        }
        s.SceneGeneration = world.SceneGeneration; s.ProjectionGeneration = world.Camera.ProjectionGeneration;
        auto& c = s.Constants;
        c.InverseViewProjection = glm::inverse(world.Camera.NonJitteredViewProjection);
        c.RasterViewProjection = raster.ViewProjection; c.PreviousViewProjection = s.PreviousView;
        c.Camera = glm::vec4(world.Camera.Position, std::abs(world.Camera.Projection[3][3]) < 0.5f ? 1 : 0);
        c.Layer = { art.Bottom, art.Thickness, art.MaxDistance, art.Scale };
        c.Shape = { art.Coverage, art.Density, art.Roundness, art.Erosion };
        const double time = std::isfinite(world.TimeSeconds) ? world.TimeSeconds : 0.0;
        const float angle = glm::radians(art.WindAngle);
        c.Wind = { float(std::fmod(time * art.WindSpeed * std::cos(angle), art.Scale * 64000.0)), 0,
            float(std::fmod(time * art.WindSpeed * std::sin(angle), art.Scale * 64000.0)),
            float(std::fmod(time * art.EvolutionSpeed, 1024.0)) };
        c.PreviousWind = s.PreviousWind;
        c.Lit = glm::vec4(art.LitColor, art.SilverLining);
        c.Shadow = glm::vec4(art.ShadowColor, art.Ambient);
        c.Sun = glm::vec4(0.3f, 0.9f, 0.2f, 0);
        c.Sun = glm::vec4(glm::normalize(glm::vec3(c.Sun)), 0);
        c.SunColor = glm::vec4(1);
        if (!world.DirectionalLights.empty())
        {
            const auto& sun = world.DirectionalLights.front();
            if (glm::dot(sun.Direction, sun.Direction) > 1e-8f)
                c.Sun = glm::vec4(glm::normalize(-sun.Direction), 0);
            c.SunColor = glm::vec4(sun.Color * std::max(0.0f, sun.Intensity), 1);
        }
        c.Dimensions = { width, height, w, h };
        c.Quality = { quality.MaxSteps, quality.LightSteps, quality.DetailDistance, quality.HistoryWeight };
        c.State = { 0, s.Sequence, s.bValid && quality.bTemporal ? 1 : 0, quality.DebugView };
        s.PendingView = world.Camera.NonJitteredViewProjection;
    }

    TextureHandle SceneCloudPass::AddPasses(RenderGraph& graph, TextureHandle color, TextureHandle depth, TextureHandle rasterDepth)
    {
        auto& s = *mImpl; s.bRebuild = false;
        if (!s.bActive) return color;
        CpuScope measured{ s.CpuRecordMilliseconds };
        if (!s.Caches.ComputeBindings.empty() || !s.Caches.Framebuffers.empty())
        { s.Retired.push_back({ s.LastSubmission, {}, std::move(s.Caches), {} }); s.Caches = {}; }
        s.NoiseHandle = s.Import(graph, s.Noise, s.bNoiseSubmitted);
        graph.AddPass<TextureHandle>("CloudNoiseUpload", RenderGraphQueue::Graphics,
            [&s](RenderGraphBuilder& builder, TextureHandle& data) { data = builder.CopyDest(s.NoiseHandle); },
            [&s](RenderGraphContext& context, const TextureHandle& data)
            {
                CpuScope measured{ s.CpuRecordMilliseconds };
                auto* texture = context.GetTexture(data, RenderGraphAccess::Write);
                if (!s.bNoiseSubmitted) context.GetCommandList()->writeTexture(texture, 0, 0, s.NoiseBytes.data(), 64, 64 * 64);
            });
        const uint32_t previous = s.PreviousIndex, current = 1 - previous;
        s.PreviousColor = s.Import(graph, s.Resources.Color[previous], s.Resources.Submitted[previous]);
        s.PreviousDepth = s.Import(graph, s.Resources.Depth[previous], s.Resources.Submitted[previous]);
        s.CurrentColor = s.Import(graph, s.Resources.Color[current], s.Resources.Submitted[current]);
        s.CurrentDepth = s.Import(graph, s.Resources.Depth[current], s.Resources.Submitted[current]);
        const auto raw = graph.CreateTexture(RenderGraphTextureDesc{ .DebugName = "Cloud Current LT",
            .Width = s.Constants.Dimensions.z, .Height = s.Constants.Dimensions.w, .Format = nvrhi::Format::RGBA16_FLOAT });
        const auto guide = graph.CreateTexture(RenderGraphTextureDesc{ .DebugName = "Cloud Current Depth",
            .Width = s.Constants.Dimensions.z, .Height = s.Constants.Dimensions.w, .Format = nvrhi::Format::RG32_FLOAT });
        s.Stage(graph, "CloudRayMarch", 0, { depth, s.NoiseHandle, s.PreviousColor, s.PreviousDepth,
            s.PreviousColor, s.PreviousDepth }, raw, guide);
        s.Stage(graph, "CloudTemporalResolve", 1, { depth, s.NoiseHandle, raw, guide,
            s.PreviousColor, s.PreviousDepth }, s.CurrentColor, s.CurrentDepth);
        graph.AddPass<CompositeData>("CloudUpsampleComposite", RenderGraphQueue::Graphics,
            [&s, color, rasterDepth](RenderGraphBuilder& builder, CompositeData& data)
            {
                data.Color = builder.ReadWrite(color, RenderGraphUsage::RenderTarget);
                data.Depth = builder.ShaderResource(rasterDepth); data.Cloud = builder.ShaderResource(s.CurrentColor);
                data.Guide = builder.ShaderResource(s.CurrentDepth);
            }, [&s](RenderGraphContext& context, const CompositeData& data)
            {
                CpuScope measured{ s.CpuRecordMilliseconds };
                auto* target = context.GetTexture(data.Color, RenderGraphAccess::Write);
                auto& framebuffer = s.Caches.Framebuffers[target];
                if (!framebuffer) framebuffer = s.Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
                auto& pipeline = s.Pipelines[target->getDesc().sampleCount];
                if (!pipeline)
                {
                    nvrhi::GraphicsPipelineDesc desc;
                    desc.setVertexShader(s.Vertex).setPixelShader(target->getDesc().sampleCount > 1 ? s.PixelMsaa : s.Pixel)
                        .addBindingLayout(s.CompositeLayout);
                    desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
                    desc.renderState.rasterState.setCullMode(nvrhi::RasterCullMode::None);
                    desc.renderState.blendState.targets[0].setBlendEnable(true)
                        .setSrcBlend(nvrhi::BlendFactor::One).setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                        .setSrcBlendAlpha(nvrhi::BlendFactor::Zero).setDestBlendAlpha(nvrhi::BlendFactor::One);
                    pipeline = s.Device->createGraphicsPipeline(desc, framebuffer);
                    if (!pipeline) throw std::runtime_error("Cloud composite pipeline creation failed");
                }
                BindingKey key{};
                key[0] = context.GetTexture(data.Depth, RenderGraphAccess::Read);
                key[1] = context.GetTexture(data.Cloud, RenderGraphAccess::Read);
                key[2] = context.GetTexture(data.Guide, RenderGraphAccess::Read);
                auto* cmd = context.GetCommandList();
                cmd->writeBuffer(s.ConstantsBuffer, &s.Constants, sizeof(s.Constants));
                cmd->setGraphicsState(nvrhi::GraphicsState().setPipeline(pipeline).setFramebuffer(framebuffer)
                    .addBindingSet(s.Bind(key, true)).setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                        nvrhi::Viewport(float(s.Constants.Dimensions.x), float(s.Constants.Dimensions.y)))));
                cmd->draw(nvrhi::DrawArguments().setVertexCount(3));
            });
        return color;
    }
    void SceneCloudPass::UpdateImportedResources(RenderGraph& graph)
    {
        auto& s = *mImpl; if (!s.bActive || !s.PreviousColor) return;
        CpuScope measured{ s.CpuRecordMilliseconds };
        auto set = [&](TextureHandle handle, const nvrhi::TextureHandle& texture, bool submitted)
        { graph.SetTextureResource(handle, texture, submitted ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common); };
        const auto p = s.PreviousIndex, c = 1 - p;
        set(s.PreviousColor, s.Resources.Color[p], s.Resources.Submitted[p]);
        set(s.PreviousDepth, s.Resources.Depth[p], s.Resources.Submitted[p]);
        set(s.CurrentColor, s.Resources.Color[c], s.Resources.Submitted[c]);
        set(s.CurrentDepth, s.Resources.Depth[c], s.Resources.Submitted[c]);
        set(s.NoiseHandle, s.Noise, s.bNoiseSubmitted);
    }
    bool SceneCloudPass::ConsumeGraphRebuildRequest() noexcept { return std::exchange(mImpl->bRebuild, false); }
    bool SceneCloudPass::RequiresContinuousUpdate() const noexcept
    {
        // Wind is evaluated from the immutable RenderWorld's TimeSeconds. A
        // nonzero speed cannot animate the same snapshot: only its temporal
        // history needs convergence. New worlds (including realtime preview)
        // already invalidate RuntimeRenderer's offscreen reuse independently.
        const auto& s = *mImpl;
        const auto convergenceFrames = static_cast<uint32_t>(std::max(64.0,
            std::ceil(std::log(0.0001) / std::log(std::max(0.0001f, s.Settings.HistoryWeight)))));
        return s.bActive && s.Settings.bTemporal && s.ConvergenceFrameCount < convergenceFrames;
    }
    void SceneCloudPass::ResetHistory() noexcept
    { mImpl->bValid = false; mImpl->Sequence = 0; mImpl->ConvergenceFrameCount = 0; }
    double SceneCloudPass::GetCpuRecordMilliseconds() const noexcept
    { return mImpl->bActive ? mImpl->CpuRecordMilliseconds : 0.0; }
    void SceneCloudPass::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        auto& s = *mImpl; if (!s.bRecorded) return;
        s.LastSubmission = submission; s.PreviousIndex = 1 - s.PreviousIndex;
        // Both imported sets transition to ShaderResource, even on the first frame.
        s.Resources.Submitted = { true, true }; s.bNoiseSubmitted = true;
        s.PreviousView = s.PendingView; s.PreviousWind = s.Constants.Wind;
        s.bValid = true; ++s.Sequence; ++s.ConvergenceFrameCount; s.bRecorded = false;
    }
    void SceneCloudPass::AbortFrame() noexcept { mImpl->bRecorded = false; }
    void SceneCloudPass::Collect(uint64_t completed)
    { std::erase_if(mImpl->Retired, [=](const auto& entry) { return entry.Submission <= completed; }); }
    void SceneCloudPass::Clear(uint64_t completed)
    { mImpl->Retire(); mImpl->bActive = false; mImpl->bRebuild = true; Collect(completed); }
    std::function<void()> SceneCloudPass::CaptureGraphState()
    {
        return [s = mImpl.get(), caches = mImpl->Caches, noise = mImpl->NoiseHandle,
            pc = mImpl->PreviousColor, pd = mImpl->PreviousDepth, cc = mImpl->CurrentColor,
            cd = mImpl->CurrentDepth]() mutable noexcept
        {
            using std::swap; swap(s->Caches, caches); swap(s->NoiseHandle, noise);
            swap(s->PreviousColor, pc); swap(s->PreviousDepth, pd); swap(s->CurrentColor, cc); swap(s->CurrentDepth, cd);
        };
    }
    void SceneCloudPass::ClearGraphResourceCaches() noexcept { mImpl->Caches = {}; }
}
