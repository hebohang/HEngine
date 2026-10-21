#include "Hepch.h"

#include "Runtime/Graphics/GlobalIllumination/SceneSurfaceDataPass.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    namespace
    {
        struct alignas(16) SurfaceFrameConstants
        {
            glm::mat4 CurrentViewProjection{ 1.0f };
            glm::mat4 PreviousViewProjection{ 1.0f };
        };

        static_assert(sizeof(SurfaceFrameConstants) == 128);

        struct alignas(16) SurfaceDrawConstants
        {
            uint32_t StableInstanceId = 0;
            glm::uvec3 Padding{ 0u };
        };

        static_assert(sizeof(SurfaceDrawConstants) == 16);

        struct SurfaceDataPassData
        {
            TextureHandle Depth;
            SceneSurfaceData Surface;
            BufferHandle Vertices;
            BufferHandle Indices;
            BufferHandle Materials;
            BufferHandle Instances;
            bool bSceneReady = false;
        };

        [[nodiscard]] std::string ReadShaderContentHash(
            const std::filesystem::path& path)
        {
            std::ifstream stream(path);
            if (!stream)
            {
                throw std::runtime_error(
                    "Failed to open surface-data shader cache-key sidecar: " +
                    Utils::PathToUtf8(path));
            }
            std::string line;
            while (std::getline(stream, line))
            {
                constexpr std::string_view Prefix = "key=";
                if (line.starts_with(Prefix) && line.size() > Prefix.size())
                    return line.substr(Prefix.size());
            }
            throw std::runtime_error(
                "Surface-data shader cache-key sidecar has no key: " +
                Utils::PathToUtf8(path));
        }

        struct FramebufferKey
        {
            std::array<nvrhi::ITexture*, 8> Colors{};
            nvrhi::ITexture* Depth = nullptr;

            [[nodiscard]] bool operator==(const FramebufferKey&) const = default;
        };

        struct FramebufferKeyHash
        {
            [[nodiscard]] size_t operator()(const FramebufferKey& key) const noexcept
            {
                size_t hash = reinterpret_cast<size_t>(key.Depth);
                for (nvrhi::ITexture* color : key.Colors)
                {
                    hash ^= reinterpret_cast<size_t>(color) +
                        0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                }
                return hash;
            }
        };
    }

    struct SceneSurfaceDataPass::Impl
    {
        struct DrawBinding
        {
            SurfaceDrawConstants Constants;
            nvrhi::BufferHandle ConstantBuffer;
            nvrhi::BindingSetHandle BindingSet;
            bool bUploaded = false;
        };

        explicit Impl(const SceneSurfaceDataPassDesc& desc)
            : Device(desc.Device)
            , OwnedShaderManager(desc.SharedShaderManager
                ? nullptr
                : CreateScope<ShaderManager>(desc.Device))
            , ShaderManagerInstance(desc.SharedShaderManager
                ? *desc.SharedShaderManager
                : *OwnedShaderManager)
            , Scene(desc.Scene)
            , ShaderArtifactCacheRoot(desc.ShaderArtifactCacheRoot)
            , Backend(desc.Backend)
            , BinaryFormat(GetShaderBinaryFormat(desc.Backend))
        {
            if (!Device)
                throw std::invalid_argument("SceneSurfaceDataPass requires a valid NVRHI device");
            if (!Scene)
                throw std::invalid_argument("SceneSurfaceDataPass requires a RayTracingScene");
            if (ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("SceneSurfaceDataPass requires a shader artifact cache root");
            CreateImmutableState();
        }

        void CreateImmutableState()
        {
            const std::filesystem::path artifactDirectory =
                ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(BinaryFormat));
            const std::filesystem::path vertexPath = artifactDirectory /
                "SceneSurfaceData.vs";
            const std::filesystem::path pixelPath = artifactDirectory /
                "SceneSurfaceData.ps";
            VertexShader = ShaderManagerInstance.LoadShaderFile(
                ShaderCacheKey{
                    .Source = "GlobalIllumination/SceneSurfaceData.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                    .ContentHash = ReadShaderContentHash(
                        MakeShaderCacheKeyPath(vertexPath)),
                    .Backend = Backend,
                    .BinaryFormat = BinaryFormat,
                },
                vertexPath,
                "GI Surface Data VS");
            PixelShader = ShaderManagerInstance.LoadShaderFile(
                ShaderCacheKey{
                    .Source = "GlobalIllumination/SceneSurfaceData.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                    .ContentHash = ReadShaderContentHash(
                        MakeShaderCacheKeyPath(pixelPath)),
                    .Backend = Backend,
                    .BinaryFormat = BinaryFormat,
                },
                pixelPath,
                "GI Surface Data PS");

            nvrhi::BindingLayoutDesc frameLayoutDesc;
            frameLayoutDesc
                .setVisibility(nvrhi::ShaderType::Vertex)
                .setRegisterSpaceAndDescriptorSet(0)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(
                    0,
                    sizeof(SurfaceFrameConstants)));
            FrameBindingLayout = Device->createBindingLayout(frameLayoutDesc);
            nvrhi::BindingSetDesc frameSetDesc;
            frameSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(
                0,
                sizeof(SurfaceFrameConstants)));
            FrameBindingSet = Device->createBindingSet(
                frameSetDesc,
                FrameBindingLayout);

            nvrhi::BindingLayoutDesc sceneLayoutDesc;
            sceneLayoutDesc
                .setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(2)
                .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4))
                .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
            SceneBindingLayout = Device->createBindingLayout(sceneLayoutDesc);

            const std::array<nvrhi::VertexAttributeDesc, 2> attributes = {
                nvrhi::VertexAttributeDesc()
                    .setName("POSITION")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(
                        offsetof(RayTracingVertex, Position)))
                    .setElementStride(sizeof(RayTracingVertex)),
                nvrhi::VertexAttributeDesc()
                    .setName("NORMAL")
                    .setFormat(nvrhi::Format::RGB32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(
                        offsetof(RayTracingVertex, Normal)))
                    .setElementStride(sizeof(RayTracingVertex)),
            };
            InputLayout = Device->createInputLayout(
                attributes.data(),
                static_cast<uint32_t>(attributes.size()),
                VertexShader);
            if (!VertexShader || !PixelShader || !FrameBindingLayout ||
                !FrameBindingSet || !SceneBindingLayout || !InputLayout)
            {
                throw std::runtime_error("NVRHI failed to create GI surface-data immutable state");
            }
        }

        void UpdateSceneBindingSet()
        {
            const RayTracingSceneGpuResources& resources = Scene->GetGpuResources();
            const std::span<const RayTracingInstanceMapping> mappings =
                Scene->GetInstanceMappings();
            if (SceneGeneration == resources.Generation &&
                DrawBindings.size() == mappings.size())
            {
                return;
            }

            DrawBindings.clear();
            DrawBindings.reserve(mappings.size());
            for (const RayTracingInstanceMapping& mapping : mappings)
            {
                DrawBinding binding{
                    .Constants = {
                        .StableInstanceId = mapping.StableInstanceId,
                    },
                };
                binding.ConstantBuffer = Device->createBuffer(
                    nvrhi::BufferDesc()
                        .setByteSize(sizeof(SurfaceDrawConstants))
                        .setIsConstantBuffer(true)
                        .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                        .setKeepInitialState(true)
                        .setDebugName("GI Surface Stable Instance ID"));
                if (!binding.ConstantBuffer)
                {
                    throw std::runtime_error(
                        "NVRHI failed to create a GI surface draw constant buffer");
                }

                nvrhi::BindingSetDesc desc;
                desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                        0,
                        binding.ConstantBuffer))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                        4,
                        resources.Materials))
                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                        5,
                        resources.Instances));
                binding.BindingSet = Device->createBindingSet(
                    desc,
                    SceneBindingLayout);
                if (!binding.BindingSet)
                {
                    throw std::runtime_error(
                        "NVRHI failed to bind GI surface-data draw resources");
                }
                DrawBindings.push_back(std::move(binding));
            }
            SceneGeneration = resources.Generation;
        }

        [[nodiscard]] nvrhi::IFramebuffer* GetFramebuffer(
            const FramebufferKey& key)
        {
            const auto existing = Framebuffers.find(key);
            if (existing != Framebuffers.end())
                return existing->second;
            nvrhi::FramebufferDesc desc;
            for (nvrhi::ITexture* color : key.Colors)
                desc.addColorAttachment(color);
            desc.setDepthAttachment(key.Depth);
            nvrhi::FramebufferHandle framebuffer = Device->createFramebuffer(desc);
            if (!framebuffer)
                throw std::runtime_error("NVRHI failed to create the GI surface-data framebuffer");
            return Framebuffers.emplace(key, std::move(framebuffer)).first->second;
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetPipeline(
            nvrhi::IFramebuffer* framebuffer)
        {
            if (Pipeline)
                return Pipeline;
            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(InputLayout)
                .setVertexShader(VertexShader)
                .setPixelShader(PixelShader)
                .addBindingLayout(FrameBindingLayout)
                .addBindingLayout(SceneBindingLayout);
            desc.renderState.depthStencilState
                .enableDepthTest()
                .disableDepthWrite()
                .setDepthFunc(nvrhi::ComparisonFunc::Equal);
            desc.renderState.rasterState
                .setCullBack()
                .setFrontCounterClockwise(true)
                .enableDepthClip();
            Pipeline = Device->createGraphicsPipeline(
                desc,
                framebuffer->getFramebufferInfo());
            if (!Pipeline)
                throw std::runtime_error("NVRHI failed to create the GI surface-data pipeline");
            return Pipeline;
        }

        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        ShaderManager& ShaderManagerInstance;
        RayTracingScene* Scene = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
        ShaderBinaryFormat BinaryFormat = ShaderBinaryFormat::Dxil;
        nvrhi::ShaderHandle VertexShader;
        nvrhi::ShaderHandle PixelShader;
        nvrhi::BindingLayoutHandle FrameBindingLayout;
        nvrhi::BindingLayoutHandle SceneBindingLayout;
        nvrhi::BindingSetHandle FrameBindingSet;
        std::vector<DrawBinding> DrawBindings;
        nvrhi::InputLayoutHandle InputLayout;
        nvrhi::GraphicsPipelineHandle Pipeline;
        std::unordered_map<
            FramebufferKey,
            nvrhi::FramebufferHandle,
            FramebufferKeyHash> Framebuffers;
        SurfaceFrameConstants FrameConstants;
        SceneSurfaceDataPassStats Stats;
        uint64_t SceneGeneration = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        bool bPrepared = false;
        bool bSceneReady = false;
    };

    SceneSurfaceDataPass::SceneSurfaceDataPass(
        const SceneSurfaceDataPassDesc& desc)
        : mImpl(CreateScope<Impl>(desc))
    {
    }

    SceneSurfaceDataPass::~SceneSurfaceDataPass() = default;

    void SceneSurfaceDataPass::PrepareFrame(
        const RenderCamera& camera,
        uint32_t width,
        uint32_t height)
    {
        if (width == 0 || height == 0)
            throw std::invalid_argument("SceneSurfaceDataPass dimensions must be non-zero");
        mImpl->bSceneReady = mImpl->Scene->IsReady();
        if (mImpl->bSceneReady)
        {
            mImpl->UpdateSceneBindingSet();
        }
        else if (!mImpl->Scene->GetInstanceMappings().empty())
        {
            throw std::logic_error(
                "SceneSurfaceDataPass scene resources are incomplete");
        }
        mImpl->FrameConstants.CurrentViewProjection = camera.ViewProjection;
        mImpl->FrameConstants.PreviousViewProjection = camera.PreviousViewProjection;
        mImpl->Width = width;
        mImpl->Height = height;
        mImpl->Stats = {
            .DrawCount = static_cast<uint32_t>(
                mImpl->Scene->GetInstanceMappings().size()),
            .ExcludedInstanceCount =
                mImpl->Scene->GetStats().ExcludedInstanceCount,
        };
        mImpl->bPrepared = true;
    }

    SceneSurfaceData SceneSurfaceDataPass::AddPasses(
        RenderGraph& renderGraph,
        TextureHandle sceneDepth,
        const RayTracingSceneGraphResources& sceneResources)
    {
        if (!mImpl->bPrepared)
            throw std::logic_error("SceneSurfaceDataPass AddPasses requires PrepareFrame");
        if (!sceneDepth || (mImpl->bSceneReady &&
            (!sceneResources.Vertices || !sceneResources.Indices ||
                !sceneResources.Materials || !sceneResources.Instances)))
        {
            throw std::invalid_argument("SceneSurfaceDataPass graph inputs are incomplete");
        }

        SceneSurfaceData surface;
        surface.Depth = sceneDepth;
        renderGraph.AddPass<SurfaceDataPassData>(
            "SceneSurfaceData",
            RenderGraphQueue::Graphics,
            [sceneDepth,
                sceneResources,
                bSceneReady = mImpl->bSceneReady,
                &surface](
                RenderGraphBuilder& builder,
                SurfaceDataPassData& data)
            {
                data.Depth = builder.DepthRead(sceneDepth);
                data.Surface.Depth = data.Depth;
                data.Surface.NormalRoughness = builder.RenderTarget(
                    builder.CreateTexture(RenderGraphTextureDesc{
                        .DebugName = "GI Surface Normal Roughness",
                        .Format = nvrhi::Format::RGBA16_FLOAT,
                        .bRelativeToGraph = true,
                        .CapabilityUsage = RenderGraphUsage::ShaderResource,
                    }));
                data.Surface.BaseColorMetalness = builder.RenderTarget(
                    builder.CreateTexture(RenderGraphTextureDesc{
                        .DebugName = "GI Surface Base Color Metalness",
                        .Format = nvrhi::Format::RGBA16_FLOAT,
                        .bRelativeToGraph = true,
                        .CapabilityUsage = RenderGraphUsage::ShaderResource,
                    }));
                data.Surface.Emissive = builder.RenderTarget(
                    builder.CreateTexture(RenderGraphTextureDesc{
                        .DebugName = "GI Surface Emissive",
                        .Format = nvrhi::Format::RGBA16_FLOAT,
                        .bRelativeToGraph = true,
                        .CapabilityUsage = RenderGraphUsage::ShaderResource,
                    }));
                data.Surface.MotionVectors = builder.RenderTarget(
                    builder.CreateTexture(RenderGraphTextureDesc{
                        .DebugName = "GI Surface Motion Vectors",
                        .Format = nvrhi::Format::RG16_FLOAT,
                        .bRelativeToGraph = true,
                        .CapabilityUsage = RenderGraphUsage::ShaderResource,
                    }));
                data.Surface.MaterialId = builder.RenderTarget(
                    builder.CreateTexture(RenderGraphTextureDesc{
                        .DebugName = "GI Surface Material ID",
                        .Format = nvrhi::Format::R32_UINT,
                        .bRelativeToGraph = true,
                        .CapabilityUsage = RenderGraphUsage::ShaderResource |
                            RenderGraphUsage::UnorderedAccess,
                    }));
                data.Surface.PreviousDepth = builder.RenderTarget(
                    builder.CreateTexture(RenderGraphTextureDesc{
                        .DebugName = "GI Surface Previous Device Depth",
                        .Format = nvrhi::Format::R32_FLOAT,
                        .bRelativeToGraph = true,
                        .CapabilityUsage = RenderGraphUsage::ShaderResource,
                    }));
                data.Surface.ViewDepth = builder.RenderTarget(
                    builder.CreateTexture(RenderGraphTextureDesc{
                        .DebugName = "GI Surface Current Previous View Depth",
                        .Format = nvrhi::Format::RG32_FLOAT,
                        .bRelativeToGraph = true,
                        .CapabilityUsage = RenderGraphUsage::ShaderResource,
                    }));
                data.Surface.SurfaceId = builder.RenderTarget(
                    builder.CreateTexture(RenderGraphTextureDesc{
                        .DebugName = "GI Surface Stable Instance Primitive ID",
                        .Format = nvrhi::Format::RG32_UINT,
                        .bRelativeToGraph = true,
                        .CapabilityUsage = RenderGraphUsage::ShaderResource,
                    }));
                data.bSceneReady = bSceneReady;
                if (bSceneReady)
                {
                    data.Vertices = builder.VertexBuffer(sceneResources.Vertices);
                    data.Indices = builder.IndexBuffer(sceneResources.Indices);
                    data.Materials = builder.ShaderResource(
                        sceneResources.Materials);
                    data.Instances = builder.ShaderResource(
                        sceneResources.Instances);
                }
                surface = data.Surface;
            },
            [impl = mImpl.get()](
                RenderGraphContext& context,
                const SurfaceDataPassData& data)
            {
                nvrhi::ITexture* depth = context.GetTexture(
                    data.Depth,
                    RenderGraphAccess::Read);
                nvrhi::ITexture* normalRoughness = context.GetTexture(
                    data.Surface.NormalRoughness,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* baseColorMetalness = context.GetTexture(
                    data.Surface.BaseColorMetalness,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* emissive = context.GetTexture(
                    data.Surface.Emissive,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* motion = context.GetTexture(
                    data.Surface.MotionVectors,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* materialId = context.GetTexture(
                    data.Surface.MaterialId,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* previousDepth = context.GetTexture(
                    data.Surface.PreviousDepth,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* viewDepth = context.GetTexture(
                    data.Surface.ViewDepth,
                    RenderGraphAccess::Write);
                nvrhi::ITexture* surfaceId = context.GetTexture(
                    data.Surface.SurfaceId,
                    RenderGraphAccess::Write);
                nvrhi::ICommandList* commandList = context.GetCommandList();
                commandList->clearTextureFloat(
                    normalRoughness,
                    nvrhi::AllSubresources,
                    nvrhi::Color(0.0f));
                commandList->clearTextureFloat(
                    baseColorMetalness,
                    nvrhi::AllSubresources,
                    nvrhi::Color(0.0f));
                commandList->clearTextureFloat(
                    emissive,
                    nvrhi::AllSubresources,
                    nvrhi::Color(0.0f));
                commandList->clearTextureFloat(
                    motion,
                    nvrhi::AllSubresources,
                    nvrhi::Color(0.0f));
                commandList->clearTextureUInt(
                    materialId,
                    nvrhi::AllSubresources,
                    InvalidRayTracingSceneIndex);
                commandList->clearTextureFloat(
                    previousDepth,
                    nvrhi::AllSubresources,
                    nvrhi::Color(1.0f));
                commandList->clearTextureFloat(
                    viewDepth,
                    nvrhi::AllSubresources,
                    nvrhi::Color(0.0f));
                commandList->clearTextureUInt(
                    surfaceId,
                    nvrhi::AllSubresources,
                    InvalidRayTracingSceneIndex);

                if (!data.bSceneReady)
                    return;

                nvrhi::IBuffer* vertices = context.GetBuffer(
                    data.Vertices,
                    RenderGraphAccess::Read);
                nvrhi::IBuffer* indices = context.GetBuffer(
                    data.Indices,
                    RenderGraphAccess::Read);
                (void)context.GetBuffer(data.Materials, RenderGraphAccess::Read);
                (void)context.GetBuffer(data.Instances, RenderGraphAccess::Read);

                const FramebufferKey framebufferKey{
                    .Colors = {
                        normalRoughness,
                        baseColorMetalness,
                        emissive,
                        motion,
                        materialId,
                        previousDepth,
                        viewDepth,
                        surfaceId,
                    },
                    .Depth = depth,
                };
                nvrhi::IFramebuffer* framebuffer =
                    impl->GetFramebuffer(framebufferKey);
                nvrhi::IGraphicsPipeline* pipeline =
                    impl->GetPipeline(framebuffer);
                const nvrhi::ViewportState viewport = nvrhi::ViewportState()
                    .addViewportAndScissorRect(nvrhi::Viewport(
                        static_cast<float>(impl->Width),
                        static_cast<float>(impl->Height)));
                const std::span<const RayTracingGeometryRecord> geometries =
                    impl->Scene->GetGeometryRecords();
                const std::span<const RayTracingInstanceMapping> mappings =
                    impl->Scene->GetInstanceMappings();
                if (impl->DrawBindings.size() != mappings.size())
                {
                    throw std::logic_error(
                        "GI surface-data draw bindings are stale");
                }
                for (size_t mappingIndex = 0;
                    mappingIndex < mappings.size();
                    ++mappingIndex)
                {
                    const RayTracingInstanceMapping& mapping =
                        mappings[mappingIndex];
                    Impl::DrawBinding& drawBinding =
                        impl->DrawBindings[mappingIndex];
                    if (mapping.GeometryIndex >= geometries.size())
                        throw std::logic_error("GI surface-data geometry mapping is stale");
                    if (drawBinding.Constants.StableInstanceId !=
                        mapping.StableInstanceId)
                    {
                        throw std::logic_error(
                            "GI surface-data stable instance binding is stale");
                    }
                    if (!drawBinding.bUploaded)
                    {
                        commandList->writeBuffer(
                            drawBinding.ConstantBuffer,
                            &drawBinding.Constants,
                            sizeof(drawBinding.Constants));
                        drawBinding.bUploaded = true;
                    }
                    const RayTracingGeometryRecord& geometry =
                        geometries[mapping.GeometryIndex];
                    nvrhi::GraphicsState state;
                    state.setPipeline(pipeline)
                        .setFramebuffer(framebuffer)
                        .setViewport(viewport)
                        .addBindingSet(impl->FrameBindingSet)
                        .addBindingSet(drawBinding.BindingSet)
                        .addVertexBuffer(nvrhi::VertexBufferBinding()
                            .setBuffer(vertices)
                            .setSlot(0)
                            .setOffset(
                                static_cast<uint64_t>(geometry.VertexIndexData.x) *
                                sizeof(RayTracingVertex)))
                        .setIndexBuffer(nvrhi::IndexBufferBinding()
                            .setBuffer(indices)
                            .setFormat(nvrhi::Format::R32_UINT)
                            .setOffset(
                                static_cast<uint64_t>(geometry.VertexIndexData.y) *
                                sizeof(uint32_t)));
                    commandList->setGraphicsState(state);
                    commandList->setPushConstants(
                        &impl->FrameConstants,
                        sizeof(impl->FrameConstants));
                    commandList->drawIndexed(nvrhi::DrawArguments()
                        .setVertexCount(geometry.GeometryData.x)
                        .setInstanceCount(1));
                }
            });
        return surface;
    }

    const SceneSurfaceDataPassStats& SceneSurfaceDataPass::GetStats() const noexcept
    {
        return mImpl->Stats;
    }

    void SceneSurfaceDataPass::ClearCaches()
    {
        mImpl->Framebuffers.clear();
        mImpl->Pipeline = nullptr;
        mImpl->DrawBindings.clear();
        mImpl->SceneGeneration = 0;
    }

    std::function<void()> SceneSurfaceDataPass::CaptureGraphState()
    {
        return [impl = mImpl.get(), frames = mImpl->Framebuffers]() mutable noexcept
        { impl->Framebuffers.swap(frames); };
    }

    void SceneSurfaceDataPass::ClearGraphResourceCaches() noexcept { mImpl->Framebuffers.clear(); }
}
