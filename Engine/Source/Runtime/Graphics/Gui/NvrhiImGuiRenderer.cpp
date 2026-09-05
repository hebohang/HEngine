#include "Hepch.h"

#include "NvrhiImGuiRenderer.h"
#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace HEngine
{
    namespace
    {
        constexpr uint32_t c_DefaultVertexCapacity = 5'000;
        constexpr uint32_t c_DefaultIndexCapacity = 10'000;
        constexpr uint32_t c_TextureIndexMask = 0xffffffffu;

        struct ProjectionConstants
        {
            float Scale[2];
            float Translate[2];
        };

        [[nodiscard]] uint64_t PackTextureHandle(ImGuiTextureHandle handle) noexcept
        {
            return (static_cast<uint64_t>(handle.Generation) << 32u)
                | static_cast<uint64_t>(handle.Index & c_TextureIndexMask);
        }

        [[nodiscard]] ImGuiTextureHandle UnpackTextureHandle(uint64_t value) noexcept
        {
            return ImGuiTextureHandle{
                .Index = static_cast<uint32_t>(value & c_TextureIndexMask),
                .Generation = static_cast<uint32_t>(value >> 32u),
            };
        }

    }

    struct NvrhiImGuiRenderer::Impl
    {
        struct TextureSlot
        {
            nvrhi::TextureHandle Texture;
            nvrhi::SamplerHandle Sampler;
            nvrhi::BindingSetHandle BindingSet;
            nvrhi::Format ViewFormat = nvrhi::Format::UNKNOWN;
            nvrhi::TextureSubresourceSet Subresources = nvrhi::AllSubresources;
            uint32_t Generation = 1;
            bool bOccupied = false;
            bool bAtlasOwned = false;
        };

        struct DeferredTextureRelease
        {
            uint32_t Index = 0;
            uint32_t RetireFrameSlot = 0;
        };

        struct FrameBuffers
        {
            nvrhi::BufferHandle VertexBuffer;
            nvrhi::BufferHandle IndexBuffer;
            uint32_t VertexCapacity = 0;
            uint32_t IndexCapacity = 0;
        };

        struct PipelineEntry
        {
            nvrhi::FramebufferInfo FramebufferInfo;
            nvrhi::GraphicsPipelineHandle Pipeline;
        };

        explicit Impl(const NvrhiImGuiRendererDesc& desc)
            : Device(desc.Device)
            , ShaderManagerInstance(desc.Device)
            , ShaderArtifacts(desc.VertexShaderArtifact.parent_path(), desc.Device)
            , FramesInFlight(desc.FramesInFlight)
            , DebugName(desc.DebugName ? desc.DebugName : "HEngine ImGui NVRHI Renderer")
        {
            if (!Device)
                throw std::invalid_argument("NvrhiImGuiRenderer requires a valid NVRHI device");
            if (FramesInFlight == 0)
                throw std::invalid_argument("NvrhiImGuiRenderer requires at least one frame in flight");
            if (desc.VertexShaderArtifact.empty() || desc.PixelShaderArtifact.empty())
                throw std::invalid_argument("NvrhiImGuiRenderer requires compiled vertex and pixel shaders");

            if (ImGui::GetCurrentContext() == nullptr)
                throw std::runtime_error("NvrhiImGuiRenderer requires an active ImGui context");
            ImGuiIO& io = ImGui::GetIO();
            if (io.BackendRendererUserData != nullptr)
                throw std::runtime_error("An ImGui renderer backend is already initialized");

            VertexShader = ShaderArtifacts.LoadShaderFile(
                ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Validation/NvrhiImGui.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Vertex,
                    .EntryPoint = "VS",
                    .TargetProfile = "vs_6_0",
                },
                desc.VertexShaderArtifact,
                "ImGui VS");
            PixelShader = ShaderArtifacts.LoadShaderFile(
                ShaderManagerInstance,
                ShaderCacheKey{
                    .Source = "Validation/NvrhiImGui.hlsl",
                    .Variant = "BASE",
                    .Stage = nvrhi::ShaderType::Pixel,
                    .EntryPoint = "PS",
                    .TargetProfile = "ps_6_0",
                },
                desc.PixelShaderArtifact,
                "ImGui PS");
            CreateImmutableState();

            FrameResources.resize(FramesInFlight);
            TextureSlots.resize(1); // Index zero is permanently invalid.

            io.BackendRendererUserData = this;
            io.BackendRendererName = "HEngine_NVRHI";
            io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
            platformIO.Renderer_TextureMaxWidth = 16'384;
            platformIO.Renderer_TextureMaxHeight = 16'384;
        }

        ~Impl()
        {
            Shutdown();
        }

        void CreateImmutableState()
        {
            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                .setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ProjectionConstants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            BindingLayout = Device->createBindingLayout(layoutDesc);

            nvrhi::SamplerDesc samplerDesc;
            samplerDesc.setAllFilters(true)
                .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
            DefaultSampler = Device->createSampler(samplerDesc);

            const std::array<nvrhi::VertexAttributeDesc, 3> attributes = {
                nvrhi::VertexAttributeDesc()
                    .setName("POSITION")
                    .setFormat(nvrhi::Format::RG32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(ImDrawVert, pos)))
                    .setElementStride(sizeof(ImDrawVert)),
                nvrhi::VertexAttributeDesc()
                    .setName("TEXCOORD")
                    .setFormat(nvrhi::Format::RG32_FLOAT)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(ImDrawVert, uv)))
                    .setElementStride(sizeof(ImDrawVert)),
                nvrhi::VertexAttributeDesc()
                    .setName("COLOR")
                    .setFormat(nvrhi::Format::RGBA8_UNORM)
                    .setBufferIndex(0)
                    .setOffset(static_cast<uint32_t>(offsetof(ImDrawVert, col)))
                    .setElementStride(sizeof(ImDrawVert)),
            };
            InputLayout = Device->createInputLayout(
                attributes.data(),
                static_cast<uint32_t>(attributes.size()),
                VertexShader);

            if (!BindingLayout || !DefaultSampler || !InputLayout)
                throw std::runtime_error("NVRHI failed to create immutable ImGui renderer state");
        }

        [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreatePipeline(nvrhi::IFramebuffer* framebuffer)
        {
            const nvrhi::FramebufferInfo info = framebuffer->getFramebufferInfo();
            for (const PipelineEntry& entry : Pipelines)
            {
                if (entry.FramebufferInfo == info)
                    return entry.Pipeline;
            }

            nvrhi::GraphicsPipelineDesc desc;
            desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                .setInputLayout(InputLayout)
                .setVertexShader(VertexShader)
                .setPixelShader(PixelShader)
                .addBindingLayout(BindingLayout);
            desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
            desc.renderState.rasterState.setCullNone().enableScissor().enableDepthClip();
            desc.renderState.blendState.targets[0]
                .enableBlend()
                .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                .setBlendOp(nvrhi::BlendOp::Add)
                .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                .setBlendOpAlpha(nvrhi::BlendOp::Add);

            nvrhi::GraphicsPipelineHandle pipeline = Device->createGraphicsPipeline(desc, info);
            if (!pipeline)
                throw std::runtime_error("NVRHI failed to create ImGui graphics pipeline");
            Pipelines.push_back(PipelineEntry{ .FramebufferInfo = info, .Pipeline = pipeline });
            return pipeline;
        }

        void EnsureFrameBuffers(uint32_t frameIndex, uint32_t vertexCount, uint32_t indexCount)
        {
            FrameBuffers& frame = FrameResources[frameIndex % FramesInFlight];
            if (!frame.VertexBuffer || frame.VertexCapacity < vertexCount)
            {
                frame.VertexCapacity = std::max(vertexCount + c_DefaultVertexCapacity, frame.VertexCapacity * 2u);
                nvrhi::BufferDesc desc;
                desc.setByteSize(static_cast<uint64_t>(frame.VertexCapacity) * sizeof(ImDrawVert))
                    .setIsVertexBuffer(true)
                    .enableAutomaticStateTracking(nvrhi::ResourceStates::VertexBuffer)
                    .setDebugName("ImGui Frame Vertex Buffer");
                frame.VertexBuffer = Device->createBuffer(desc);
            }
            if (!frame.IndexBuffer || frame.IndexCapacity < indexCount)
            {
                frame.IndexCapacity = std::max(indexCount + c_DefaultIndexCapacity, frame.IndexCapacity * 2u);
                nvrhi::BufferDesc desc;
                desc.setByteSize(static_cast<uint64_t>(frame.IndexCapacity) * sizeof(ImDrawIdx))
                    .setIsIndexBuffer(true)
                    .enableAutomaticStateTracking(nvrhi::ResourceStates::IndexBuffer)
                    .setDebugName("ImGui Frame Index Buffer");
                frame.IndexBuffer = Device->createBuffer(desc);
            }
            if (!frame.VertexBuffer || !frame.IndexBuffer)
                throw std::runtime_error("NVRHI failed to create ImGui frame geometry buffers");
        }

        [[nodiscard]] ImGuiTextureHandle RegisterTexture(
            const nvrhi::TextureHandle& texture,
            const nvrhi::SamplerHandle& sampler,
            nvrhi::Format viewFormat,
            nvrhi::TextureSubresourceSet subresources,
            bool bAtlasOwned)
        {
            if (bShutdown)
                throw std::runtime_error("Cannot register an ImGui texture after renderer shutdown");
            if (!texture)
                throw std::invalid_argument("Cannot register a null ImGui texture");

            uint32_t index = 0;
            if (!FreeTextureSlots.empty())
            {
                index = FreeTextureSlots.back();
                FreeTextureSlots.pop_back();
            }
            else
            {
                if (TextureSlots.size() >= std::numeric_limits<uint32_t>::max())
                    throw std::overflow_error("ImGui texture registry exhausted");
                index = static_cast<uint32_t>(TextureSlots.size());
                TextureSlots.emplace_back();
            }

            TextureSlot& slot = TextureSlots[index];
            if (slot.Generation == 0)
                slot.Generation = 1;
            slot.Texture = texture;
            slot.Sampler = sampler ? sampler : DefaultSampler;
            slot.ViewFormat = viewFormat;
            slot.Subresources = subresources;
            slot.bOccupied = true;
            slot.bAtlasOwned = bAtlasOwned;

            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ProjectionConstants)))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, slot.Texture, viewFormat, subresources))
                .addItem(nvrhi::BindingSetItem::Sampler(0, slot.Sampler));
            slot.BindingSet = Device->createBindingSet(bindingDesc, BindingLayout);
            if (!slot.BindingSet)
            {
                slot = TextureSlot{};
                FreeTextureSlots.push_back(index);
                throw std::runtime_error("NVRHI failed to create ImGui texture binding set");
            }

            return ImGuiTextureHandle{ .Index = index, .Generation = slot.Generation };
        }

        [[nodiscard]] TextureSlot* ResolveTexture(ImGuiTextureHandle handle)
        {
            if (!handle || handle.Index >= TextureSlots.size())
                return nullptr;
            TextureSlot& slot = TextureSlots[handle.Index];
            if (!slot.bOccupied || slot.Generation != handle.Generation)
                return nullptr;
            return &slot;
        }

        [[nodiscard]] const TextureSlot* ResolveTexture(ImGuiTextureHandle handle) const
        {
            return const_cast<Impl*>(this)->ResolveTexture(handle);
        }

        void ScheduleTextureRelease(ImGuiTextureHandle handle, uint32_t retireFrameIndex)
        {
            TextureSlot* slot = ResolveTexture(handle);
            if (!slot)
                return;
            slot->bOccupied = false;
            slot->bAtlasOwned = false;
            DeferredReleases.push_back(DeferredTextureRelease{
                .Index = handle.Index,
                .RetireFrameSlot = retireFrameIndex % FramesInFlight,
            });
        }

        void CollectDeferredReleases(uint32_t frameIndex)
        {
            auto it = DeferredReleases.begin();
            while (it != DeferredReleases.end())
            {
                // BeginFrame has waited for this exact frame slot before NewFrame
                // is called, so all earlier graphics-queue references are complete.
                if (it->RetireFrameSlot != frameIndex % FramesInFlight)
                {
                    ++it;
                    continue;
                }

                TextureSlot& slot = TextureSlots[it->Index];
                slot.Texture = nullptr;
                slot.Sampler = nullptr;
                slot.BindingSet = nullptr;
                slot.ViewFormat = nvrhi::Format::UNKNOWN;
                slot.Subresources = nvrhi::AllSubresources;
                ++slot.Generation;
                if (slot.Generation == 0)
                    slot.Generation = 1;
                FreeTextureSlots.push_back(it->Index);
                it = DeferredReleases.erase(it);
            }
        }

        void UpdateTexture(ImTextureData* textureData, nvrhi::ICommandList* commandList, uint32_t frameIndex)
        {
            if (textureData == nullptr)
                return;

            if (textureData->Status == ImTextureStatus_WantCreate)
            {
                if (textureData->Format != ImTextureFormat_RGBA32)
                    throw std::runtime_error("NVRHI ImGui renderer only accepts RGBA32 atlas textures");
                nvrhi::TextureDesc desc;
                desc.setWidth(static_cast<uint32_t>(textureData->Width))
                    .setHeight(static_cast<uint32_t>(textureData->Height))
                    .setFormat(nvrhi::Format::RGBA8_UNORM)
                    .setDimension(nvrhi::TextureDimension::Texture2D)
                    .setDebugName("ImGui Dynamic Atlas")
                    .enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
                nvrhi::TextureHandle texture = Device->createTexture(desc);
                if (!texture)
                    throw std::runtime_error("NVRHI failed to create the ImGui dynamic atlas");
                commandList->writeTexture(
                    texture,
                    0,
                    0,
                    textureData->GetPixels(),
                    static_cast<size_t>(textureData->GetPitch()));
                const ImGuiTextureHandle handle = RegisterTexture(
                    texture,
                    DefaultSampler,
                    nvrhi::Format::RGBA8_UNORM,
                    nvrhi::AllSubresources,
                    true);
                textureData->SetTexID(NvrhiImGuiRenderer::ToImTextureID(handle));
                textureData->SetStatus(ImTextureStatus_OK);
            }
            else if (textureData->Status == ImTextureStatus_WantUpdates)
            {
                const ImGuiTextureHandle handle = NvrhiImGuiRenderer::FromImTextureID(textureData->GetTexID());
                TextureSlot* slot = ResolveTexture(handle);
                if (!slot || !slot->bAtlasOwned)
                    throw std::runtime_error("ImGui requested an update for an invalid atlas texture handle");

                // NVRHI writeTexture uploads complete mips. The atlas CPU pixels are authoritative,
                // so a full upload safely covers all update rectangles without native interop.
                commandList->writeTexture(
                    slot->Texture,
                    0,
                    0,
                    textureData->GetPixels(),
                    static_cast<size_t>(textureData->GetPitch()));
                textureData->SetStatus(ImTextureStatus_OK);
            }
            else if (textureData->Status == ImTextureStatus_WantDestroy
                && textureData->UnusedFrames >= static_cast<int>(FramesInFlight))
            {
                const ImGuiTextureHandle handle = NvrhiImGuiRenderer::FromImTextureID(textureData->GetTexID());
                ScheduleTextureRelease(handle, frameIndex);
                textureData->SetTexID(ImTextureID_Invalid);
                textureData->BackendUserData = nullptr;
                textureData->SetStatus(ImTextureStatus_Destroyed);
            }
        }

        void ProcessTextureUpdates(ImVector<ImTextureData*>* textures, nvrhi::ICommandList* commandList, uint32_t frameIndex)
        {
            if (textures == nullptr)
                return;
            for (ImTextureData* texture : *textures)
            {
                if (texture->Status != ImTextureStatus_OK)
                    UpdateTexture(texture, commandList, frameIndex);
            }
        }

        void NewFrame(nvrhi::ICommandList* commandList, uint32_t frameIndex)
        {
            if (bShutdown)
                throw std::runtime_error("NvrhiImGuiRenderer::NewFrame called after shutdown");
            if (commandList == nullptr)
                throw std::invalid_argument("NvrhiImGuiRenderer::NewFrame requires an open command list");
            CollectDeferredReleases(frameIndex);
            ProcessTextureUpdates(&ImGui::GetPlatformIO().Textures, commandList, frameIndex);
        }

        void RenderDrawData(
            ImDrawData* drawData,
            nvrhi::ICommandList* commandList,
            nvrhi::IFramebuffer* framebuffer,
            uint32_t frameIndex)
        {
            if (bShutdown)
                throw std::runtime_error("NvrhiImGuiRenderer::RenderDrawData called after shutdown");
            if (!drawData || !commandList || !framebuffer)
                throw std::invalid_argument("NvrhiImGuiRenderer::RenderDrawData received a null argument");
            if (drawData->DisplaySize.x <= 0.f || drawData->DisplaySize.y <= 0.f
                || drawData->TotalVtxCount <= 0 || drawData->TotalIdxCount <= 0)
                return;

            ProcessTextureUpdates(drawData->Textures, commandList, frameIndex);
            EnsureFrameBuffers(
                frameIndex,
                static_cast<uint32_t>(drawData->TotalVtxCount),
                static_cast<uint32_t>(drawData->TotalIdxCount));

            std::vector<ImDrawVert> vertices;
            std::vector<ImDrawIdx> indices;
            vertices.reserve(drawData->TotalVtxCount);
            indices.reserve(drawData->TotalIdxCount);
            for (const ImDrawList* drawList : drawData->CmdLists)
            {
                vertices.insert(
                    vertices.end(),
                    drawList->VtxBuffer.Data,
                    drawList->VtxBuffer.Data + drawList->VtxBuffer.Size);
                indices.insert(
                    indices.end(),
                    drawList->IdxBuffer.Data,
                    drawList->IdxBuffer.Data + drawList->IdxBuffer.Size);
            }

            FrameBuffers& frame = FrameResources[frameIndex % FramesInFlight];
            commandList->writeBuffer(frame.VertexBuffer, vertices.data(), vertices.size() * sizeof(ImDrawVert));
            commandList->writeBuffer(frame.IndexBuffer, indices.data(), indices.size() * sizeof(ImDrawIdx));

            const float framebufferWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
            const float framebufferHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
            const ProjectionConstants projection{
                .Scale = { 2.f / drawData->DisplaySize.x, -2.f / drawData->DisplaySize.y },
                .Translate = {
                    -1.f - drawData->DisplayPos.x * (2.f / drawData->DisplaySize.x),
                    1.f + drawData->DisplayPos.y * (2.f / drawData->DisplaySize.y),
                },
            };

            nvrhi::IGraphicsPipeline* pipeline = GetOrCreatePipeline(framebuffer);
            const ImVec2 clipOffset = drawData->DisplayPos;
            const ImVec2 clipScale = drawData->FramebufferScale;
            int globalIndexOffset = 0;
            int globalVertexOffset = 0;

            commandList->beginMarker(DebugName.c_str());
            for (const ImDrawList* drawList : drawData->CmdLists)
            {
                for (const ImDrawCmd& drawCommand : drawList->CmdBuffer)
                {
                    if (drawCommand.UserCallback != nullptr)
                    {
                        if (drawCommand.UserCallback != ImDrawCallback_ResetRenderState)
                            drawCommand.UserCallback(drawList, &drawCommand);
                        continue;
                    }

                    const float clipMinX = (drawCommand.ClipRect.x - clipOffset.x) * clipScale.x;
                    const float clipMinY = (drawCommand.ClipRect.y - clipOffset.y) * clipScale.y;
                    const float clipMaxX = (drawCommand.ClipRect.z - clipOffset.x) * clipScale.x;
                    const float clipMaxY = (drawCommand.ClipRect.w - clipOffset.y) * clipScale.y;
                    const int minX = static_cast<int>(std::max(clipMinX, 0.f));
                    const int minY = static_cast<int>(std::max(clipMinY, 0.f));
                    const int maxX = static_cast<int>(std::min(clipMaxX, framebufferWidth));
                    const int maxY = static_cast<int>(std::min(clipMaxY, framebufferHeight));
                    if (maxX <= minX || maxY <= minY)
                        continue;

                    TextureSlot* texture = ResolveTexture(
                        NvrhiImGuiRenderer::FromImTextureID(drawCommand.GetTexID()));
                    if (!texture)
                    {
                        ++Stats.SkippedInvalidTextures;
                        continue;
                    }

                    nvrhi::ViewportState viewportState;
                    viewportState.addViewport(nvrhi::Viewport(framebufferWidth, framebufferHeight))
                        .addScissorRect(nvrhi::Rect(minX, maxX, minY, maxY));
                    nvrhi::GraphicsState state;
                    state.setPipeline(pipeline)
                        .setFramebuffer(framebuffer)
                        .setViewport(viewportState)
                        .addBindingSet(texture->BindingSet)
                        .addVertexBuffer(nvrhi::VertexBufferBinding()
                            .setBuffer(frame.VertexBuffer)
                            .setSlot(0)
                            .setOffset(0))
                        .setIndexBuffer(nvrhi::IndexBufferBinding()
                            .setBuffer(frame.IndexBuffer)
                            .setFormat(sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT)
                            .setOffset(0));
                    commandList->setGraphicsState(state);
                    commandList->setPushConstants(&projection, sizeof(projection));
                    commandList->drawIndexed(nvrhi::DrawArguments()
                        .setVertexCount(drawCommand.ElemCount)
                        .setStartIndexLocation(drawCommand.IdxOffset + globalIndexOffset)
                        .setStartVertexLocation(drawCommand.VtxOffset + globalVertexOffset));
                    ++Stats.DrawCalls;
                    Stats.RenderedIndices += drawCommand.ElemCount;
                }
                globalIndexOffset += drawList->IdxBuffer.Size;
                globalVertexOffset += drawList->VtxBuffer.Size;
            }
            commandList->endMarker();
            Stats.RenderedVertices += static_cast<uint64_t>(drawData->TotalVtxCount);
        }

        void Shutdown()
        {
            if (bShutdown)
                return;

            if (ImGui::GetCurrentContext() != nullptr)
            {
                ImGuiIO& io = ImGui::GetIO();
                if (io.BackendRendererUserData == this)
                {
                    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
                    {
                        const ImGuiTextureHandle handle = NvrhiImGuiRenderer::FromImTextureID(texture->GetTexID());
                        const TextureSlot* slot = ResolveTexture(handle);
                        if (slot && slot->bAtlasOwned)
                        {
                            texture->SetTexID(ImTextureID_Invalid);
                            texture->BackendUserData = nullptr;
                            texture->SetStatus(ImTextureStatus_Destroyed);
                        }
                    }
                    io.BackendRendererName = nullptr;
                    io.BackendRendererUserData = nullptr;
                    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
                }
            }

            DeferredReleases.clear();
            FreeTextureSlots.clear();
            TextureSlots.clear();
            FrameResources.clear();
            Pipelines.clear();
            InputLayout = nullptr;
            BindingLayout = nullptr;
            DefaultSampler = nullptr;
            PixelShader = nullptr;
            VertexShader = nullptr;
            Device = nullptr;
            bShutdown = true;
        }

        nvrhi::DeviceHandle Device;
        ShaderManager ShaderManagerInstance;
        ShaderArtifactResolver ShaderArtifacts;
        uint32_t FramesInFlight = 0;
        std::string DebugName;
        nvrhi::ShaderHandle VertexShader;
        nvrhi::ShaderHandle PixelShader;
        nvrhi::InputLayoutHandle InputLayout;
        nvrhi::BindingLayoutHandle BindingLayout;
        nvrhi::SamplerHandle DefaultSampler;
        std::vector<FrameBuffers> FrameResources;
        std::vector<PipelineEntry> Pipelines;
        std::vector<TextureSlot> TextureSlots;
        std::vector<uint32_t> FreeTextureSlots;
        std::vector<DeferredTextureRelease> DeferredReleases;
        NvrhiImGuiRendererStats Stats;
        bool bShutdown = false;
    };

    NvrhiImGuiRenderer::NvrhiImGuiRenderer(const NvrhiImGuiRendererDesc& desc)
        : mImpl(std::make_unique<Impl>(desc))
    {
    }

    NvrhiImGuiRenderer::~NvrhiImGuiRenderer() = default;

    void NvrhiImGuiRenderer::NewFrame(nvrhi::ICommandList* commandList, uint32_t frameIndex)
    {
        mImpl->NewFrame(commandList, frameIndex);
    }

    void NvrhiImGuiRenderer::RenderDrawData(
        ImDrawData* drawData,
        nvrhi::ICommandList* commandList,
        nvrhi::IFramebuffer* framebuffer,
        uint32_t frameIndex)
    {
        mImpl->RenderDrawData(drawData, commandList, framebuffer, frameIndex);
    }

    ImGuiTextureHandle NvrhiImGuiRenderer::RegisterTexture(
        const nvrhi::TextureHandle& texture,
        const nvrhi::SamplerHandle& sampler,
        nvrhi::Format viewFormat,
        nvrhi::TextureSubresourceSet subresources)
    {
        return mImpl->RegisterTexture(texture, sampler, viewFormat, subresources, false);
    }

    void NvrhiImGuiRenderer::UnregisterTexture(ImGuiTextureHandle handle, uint32_t retireFrameIndex)
    {
        mImpl->ScheduleTextureRelease(handle, retireFrameIndex);
    }

    bool NvrhiImGuiRenderer::IsTextureValid(ImGuiTextureHandle handle) const
    {
        return mImpl->ResolveTexture(handle) != nullptr;
    }

    ImTextureID NvrhiImGuiRenderer::ToImTextureID(ImGuiTextureHandle handle) noexcept
    {
        return static_cast<ImTextureID>(PackTextureHandle(handle));
    }

    ImGuiTextureHandle NvrhiImGuiRenderer::FromImTextureID(ImTextureID textureId) noexcept
    {
        return UnpackTextureHandle(static_cast<uint64_t>(textureId));
    }

    ImGuiTextureHandle NvrhiImGuiRenderer::GetFontTexture() const noexcept
    {
        if (ImGui::GetCurrentContext() == nullptr)
            return {};
        return FromImTextureID(ImGui::GetIO().Fonts->TexRef.GetTexID());
    }

    uint32_t NvrhiImGuiRenderer::GetRegisteredTextureCount() const noexcept
    {
        uint32_t count = 0;
        for (const Impl::TextureSlot& slot : mImpl->TextureSlots)
            count += slot.bOccupied ? 1u : 0u;
        return count;
    }

    const NvrhiImGuiRendererStats& NvrhiImGuiRenderer::GetStats() const noexcept
    {
        return mImpl->Stats;
    }

    void NvrhiImGuiRenderer::ResetStats() noexcept
    {
        mImpl->Stats = {};
    }

    void NvrhiImGuiRenderer::Shutdown()
    {
        mImpl->Shutdown();
    }
}
