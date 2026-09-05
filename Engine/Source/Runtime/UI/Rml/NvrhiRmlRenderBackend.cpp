#include "Hepch.h"

#include "Runtime/UI/Rml/RmlRenderBackend.h"
#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Vertex.h>
#include <nvrhi/nvrhi.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace HEngine
{
    namespace
    {
        struct RmlProjectionConstants
        {
            float Transform[16] = {};
            float Translation[2] = {};
            float Padding[2] = {};
        };

        enum class RmlPostprocessMode : uint32_t
        {
            Copy = 0,
            Opacity,
            Blur,
            DropShadow,
            ColorMatrix,
            MaskImage,
        };

        struct alignas(16) RmlPostprocessConstants
        {
            uint32_t Mode = static_cast<uint32_t>(RmlPostprocessMode::Copy);
            float Opacity = 1.f;
            float Sigma = 0.f;
            float Padding0 = 0.f;
            float InverseTextureSize[2] = { 1.f, 1.f };
            float UvMin[2] = { 0.f, 0.f };
            float UvMax[2] = { 1.f, 1.f };
            float OffsetUv[2] = { 0.f, 0.f };
            float ShadowColor[4] = {};
            float ColorMatrix[16] = {};
        };

        static_assert(sizeof(RmlProjectionConstants) == 80);
        static_assert(sizeof(RmlPostprocessConstants) == 128);

        struct alignas(16) RmlDecoratorConstants
        {
            float Transform[16] = {};
            float Translation[2] = {};
            float Padding[2] = {};
            uint32_t Mode = 0;
            float Time = 0.f;
            float Dimensions[2] = {};
        };

        static_assert(sizeof(RmlDecoratorConstants) == 96);

        [[nodiscard]] uint64_t PackRmlHandle(uint32_t index, uint32_t generation) noexcept
        {
            return (static_cast<uint64_t>(generation) << 32u) | static_cast<uint64_t>(index);
        }

        void UnpackRmlHandle(uintptr_t handle, uint32_t& index, uint32_t& generation) noexcept
        {
            const uint64_t value = static_cast<uint64_t>(handle);
            index = static_cast<uint32_t>(value & 0xffffffffull);
            generation = static_cast<uint32_t>(value >> 32u);
        }

        [[nodiscard]] uint32_t NextGeneration(uint32_t generation) noexcept
        {
            ++generation;
            return generation == 0 ? 1u : generation;
        }

        void PremultiplyAlpha(std::vector<Rml::byte>& pixels)
        {
            for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
            {
                const uint32_t alpha = pixels[offset + 3];
                pixels[offset + 0] = static_cast<Rml::byte>((pixels[offset + 0] * alpha) / 255u);
                pixels[offset + 1] = static_cast<Rml::byte>((pixels[offset + 1] * alpha) / 255u);
                pixels[offset + 2] = static_cast<Rml::byte>((pixels[offset + 2] * alpha) / 255u);
            }
        }

        [[nodiscard]] std::vector<Rml::byte> LoadTextureFileBytes(const Rml::String& source)
        {
            if (Rml::FileInterface* fileInterface = Rml::GetFileInterface())
            {
                Rml::String data;
                if (fileInterface->LoadFile(source, data))
                {
                    return std::vector<Rml::byte>(
                        reinterpret_cast<const Rml::byte*>(data.data()),
                        reinterpret_cast<const Rml::byte*>(data.data()) + data.size());
                }
            }

            std::ifstream stream(source.c_str(), std::ios::binary | std::ios::ate);
            if (!stream)
                return {};
            const std::streamsize size = stream.tellg();
            if (size <= 0)
                return {};
            std::vector<Rml::byte> bytes(static_cast<size_t>(size));
            stream.seekg(0, std::ios::beg);
            if (!stream.read(reinterpret_cast<char*>(bytes.data()), size))
                return {};
            return bytes;
        }

        class NvrhiRmlRenderBackend final : public INvrhiRmlRenderBackend, public Rml::RenderInterface
        {
        private:
            enum class PipelineMode : uint8_t
            {
                Normal,
                NormalClipped,
                ClipMaskReplace,
                ClipMaskIncrement,
            };

            enum class FilterType : uint8_t
            {
                Invalid,
                Opacity,
                Blur,
                DropShadow,
                ColorMatrix,
                MaskImage,
            };

            enum class DecoratorType : uint8_t
            {
                Invalid,
                Sheen,
                Halftone,
            };

            static constexpr uint8_t MaxClipMaskReference = 254;
            static constexpr uint32_t MaxRenderLayers = 8;

            enum class RetiredResourceType
            {
                Geometry,
                Texture,
            };

            struct GeometrySlot
            {
                nvrhi::BufferHandle VertexBuffer;
                nvrhi::BufferHandle IndexBuffer;
                std::vector<Rml::Vertex> Vertices;
                std::vector<uint32_t> Indices;
                uint32_t IndexCount = 0;
                uint32_t Generation = 1;
                bool bOccupied = false;
                bool bUploaded = false;
            };

            struct TextureSlot
            {
                nvrhi::TextureHandle Texture;
                nvrhi::BindingSetHandle BindingSet;
                std::vector<Rml::byte> Pixels;
                uint32_t Width = 0;
                uint32_t Height = 0;
                uint32_t Generation = 1;
                bool bOccupied = false;
                bool bUploaded = false;
            };

            struct RetiredResource
            {
                RetiredResourceType Type = RetiredResourceType::Geometry;
                uint32_t Index = 0;
                nvrhi::BufferHandle VertexBuffer;
                nvrhi::BufferHandle IndexBuffer;
                nvrhi::TextureHandle Texture;
                nvrhi::BindingSetHandle BindingSet;
                std::vector<uint8_t> PendingFrameSlots;
            };

            struct PipelineEntry
            {
                nvrhi::FramebufferInfo FramebufferInfo;
                PipelineMode Mode = PipelineMode::Normal;
                nvrhi::GraphicsPipelineHandle Pipeline;
            };

            struct PostprocessPipelineEntry
            {
                nvrhi::FramebufferInfo FramebufferInfo;
                bool bBlend = false;
                bool bClipped = false;
                nvrhi::GraphicsPipelineHandle Pipeline;
            };

            struct DecoratorPipelineEntry
            {
                nvrhi::FramebufferInfo FramebufferInfo;
                PipelineMode Mode = PipelineMode::Normal;
                nvrhi::GraphicsPipelineHandle Pipeline;
            };

            struct StencilInitializePipelineEntry
            {
                nvrhi::FramebufferInfo FramebufferInfo;
                nvrhi::GraphicsPipelineHandle Pipeline;
            };

            struct RenderTarget
            {
                nvrhi::TextureHandle Color;
                nvrhi::TextureHandle DepthStencil;
                nvrhi::FramebufferHandle Framebuffer;
                uint32_t Width = 0;
                uint32_t Height = 0;
                bool bClipMaskEnabled = false;
                uint8_t ClipMaskReference = 0;
            };

            struct ColorTarget
            {
                nvrhi::TextureHandle Color;
                nvrhi::FramebufferHandle Framebuffer;
                uint32_t Width = 0;
                uint32_t Height = 0;
            };

            struct PostprocessBindingEntry
            {
                nvrhi::ITexture* Source = nullptr;
                nvrhi::ITexture* Secondary = nullptr;
                nvrhi::BindingSetHandle BindingSet;
            };

            struct DecoratorBindingEntry
            {
                nvrhi::ITexture* Texture = nullptr;
                nvrhi::BindingSetHandle BindingSet;
            };

            struct FrameLayerPool
            {
                std::array<RenderTarget, MaxRenderLayers> Layers;
                std::array<ColorTarget, 2> Postprocess;
                std::vector<PostprocessBindingEntry> PostprocessBindings;
                std::vector<DecoratorBindingEntry> DecoratorBindings;
                uint32_t LayerDepth = 0;
            };

            struct CompiledFilter
            {
                FilterType Type = FilterType::Invalid;
                float Value = 1.f;
                float Sigma = 0.f;
                Rml::Vector2f Offset = {};
                Rml::ColourbPremultiplied Color = {};
                Rml::Matrix4f ColorMatrix = Rml::Matrix4f::Identity();
                nvrhi::TextureHandle MaskTexture;
            };

            struct CompiledDecorator
            {
                DecoratorType Type = DecoratorType::Invalid;
                Rml::Vector2f Dimensions = {};
            };

            struct RetiredAuxiliaryResource
            {
                nvrhi::TextureHandle Texture;
                std::vector<uint8_t> PendingFrameSlots;
            };

            struct PendingUpload
            {
                uint32_t Index = 0;
                uint32_t Generation = 0;
            };

            struct PixelRectangle
            {
                int Left = 0;
                int Top = 0;
                int Right = 0;
                int Bottom = 0;
            };

        public:
            explicit NvrhiRmlRenderBackend(const NvrhiRmlRenderBackendDesc& desc)
                : mDevice(desc.Device)
                , mShaderManager(desc.Device)
                , mShaderArtifacts(desc.VertexShaderArtifact.parent_path(), desc.Device)
                , mFramesInFlight(desc.FramesInFlight)
                , mDebugName(desc.DebugName ? desc.DebugName : "HEngine RmlUi NVRHI Renderer")
            {
                static_assert(sizeof(uintptr_t) >= sizeof(uint64_t),
                    "RmlUi NVRHI handles require the supported 64-bit build");

                if (!mDevice)
                    throw std::invalid_argument("NVRHI RmlUi renderer requires a valid device");
                if (mFramesInFlight == 0)
                    throw std::invalid_argument("NVRHI RmlUi renderer requires at least one frame in flight");
                if (desc.VertexShaderArtifact.empty() || desc.PixelShaderArtifact.empty())
                    throw std::invalid_argument("NVRHI RmlUi renderer requires compiled shaders");

                const auto loadShader = [this](
                    const std::filesystem::path& path,
                    std::string source,
                    nvrhi::ShaderType stage,
                    const char* debugName)
                {
                    const bool bVertex = stage == nvrhi::ShaderType::Vertex;
                    return mShaderArtifacts.LoadShaderFile(
                        mShaderManager,
                        ShaderCacheKey{
                            .Source = std::move(source),
                            .Variant = "BASE",
                            .Stage = stage,
                            .EntryPoint = bVertex ? "VS" : "PS",
                            .TargetProfile = bVertex ? "vs_6_0" : "ps_6_0",
                        },
                        path,
                        debugName);
                };

                mVertexShader = loadShader(
                    desc.VertexShaderArtifact,
                    "Validation/NvrhiRml.hlsl",
                    nvrhi::ShaderType::Vertex,
                    "RmlUi VS");
                mPixelShader = loadShader(
                    desc.PixelShaderArtifact,
                    "Validation/NvrhiRml.hlsl",
                    nvrhi::ShaderType::Pixel,
                    "RmlUi PS");
                const std::filesystem::path shaderCache = desc.VertexShaderArtifact.parent_path();
                mPostprocessVertexShader = loadShader(
                    shaderCache / "NvrhiRmlPostprocess.vs",
                    "Validation/NvrhiRmlPostprocess.hlsl",
                    nvrhi::ShaderType::Vertex,
                    "RmlUi Postprocess VS");
                mPostprocessPixelShader = loadShader(
                    shaderCache / "NvrhiRmlPostprocess.ps",
                    "Validation/NvrhiRmlPostprocess.hlsl",
                    nvrhi::ShaderType::Pixel,
                    "RmlUi Postprocess PS");
                mDecoratorPixelShader = loadShader(
                    shaderCache / "NvrhiRmlDecorator.ps",
                    "Validation/NvrhiRmlDecorator.hlsl",
                    nvrhi::ShaderType::Pixel,
                    "RmlUi Decorator PS");
                CreateImmutableState();

                mGeometrySlots.resize(1);
                mTextureSlots.resize(1);
                mLastCompletedSubmissions.resize(mFramesInFlight, 0);
                mLastSubmittedSubmissions.resize(mFramesInFlight, 0);
                mFrameLayerPools.resize(mFramesInFlight);
            }

            ~NvrhiRmlRenderBackend() override
            {
                if (mCommandList)
                {
                    try
                    {
                        EndFrame();
                    }
                    catch (...)
                    {
                    }
                }
                try
                {
                    Shutdown();
                }
                catch (...)
                {
                }
            }

            [[nodiscard]] Rml::RenderInterface* GetRenderInterface() override
            {
                return this;
            }

            [[nodiscard]] RmlRenderCapabilities GetCapabilities() const noexcept override
            {
                return {
                    .Scissor = true,
                    .Transform = true,
                    .ClipMask = true,
                    .ClipMaskRequiresStencilAttachment = true,
                    .Layers = true,
                    .Filters = true,
                    .MaskImage = true,
                    .CustomShaders = true,
                };
            }

            void SetViewportSize(
                float renderWidth,
                float renderHeight,
                float layoutWidth,
                float layoutHeight) override
            {
                mViewportWidth = std::max(renderWidth, 1.f);
                mViewportHeight = std::max(renderHeight, 1.f);
                mLayoutWidth = layoutWidth > 0.f ? layoutWidth : mViewportWidth;
                mLayoutHeight = layoutHeight > 0.f ? layoutHeight : mViewportHeight;
            }

            void BeginFrame(
                nvrhi::ICommandList* commandList,
                nvrhi::IFramebuffer* framebuffer,
                uint32_t frameSlot,
                uint64_t completedSubmission,
                nvrhi::ITexture* backdropTexture) override
            {
                if (mShutdown)
                    throw std::logic_error("NVRHI RmlUi BeginFrame called after shutdown");
                if (mCommandList)
                    throw std::logic_error("NVRHI RmlUi BeginFrame called before EndFrame");
                if (mPendingUploadFrame)
                {
                    throw std::logic_error(
                        "NVRHI RmlUi BeginFrame requires NotifyFrameSubmitted or AbortFrame for the previous frame");
                }
                if (!commandList || !framebuffer)
                    throw std::invalid_argument("NVRHI RmlUi BeginFrame received a null command list or framebuffer");
                if (frameSlot >= mFramesInFlight)
                    throw std::out_of_range("NVRHI RmlUi frame slot exceeds FramesInFlight");
                if (completedSubmission < mLastCompletedSubmissions[frameSlot])
                    throw std::logic_error("NVRHI RmlUi completed submission regressed for a frame slot");
                if (completedSubmission < mLastSubmittedSubmissions[frameSlot])
                {
                    throw std::logic_error(
                        "NVRHI RmlUi frame slot was reused before its submitted GPU work completed");
                }

                mLastCompletedSubmissions[frameSlot] = completedSubmission;
                CollectRetiredResources(frameSlot);
                CollectRetiredAuxiliaryResources(frameSlot);
                mCommandList = commandList;
                mBaseFramebuffer = framebuffer;
                mFramebuffer = framebuffer;
                mBackdropTexture = backdropTexture;
                mActiveFrameSlot = frameSlot;
                FrameLayerPool& layerPool = GetActiveLayerPool();
                layerPool.LayerDepth = 0;
                layerPool.PostprocessBindings.clear();
                layerPool.DecoratorBindings.clear();
                mCurrentLayerHandle = {};
                mBaseClipMaskEnabled = false;
                mBaseClipMaskReference = 0;

                const nvrhi::FramebufferInfoEx& info = framebuffer->getFramebufferInfo();
                mViewportWidth = static_cast<float>(std::max(info.width, 1u));
                mViewportHeight = static_cast<float>(std::max(info.height, 1u));
                mScissorEnabled = false;
                mScissor = Rml::Rectanglei::FromSize(Rml::Vector2i(
                    static_cast<int>(std::round(mLayoutWidth)),
                    static_cast<int>(std::round(mLayoutHeight))));
                mTransform = Rml::Matrix4f::Identity();
                mPipelineMode = PipelineMode::Normal;
                mPipelineStencilReference = 0;
                mClipMaskReference = 0;
                mClipMaskEnabled = false;

                mCommandList->beginMarker(mDebugName.c_str());
                if (!mWhiteTextureUploaded)
                {
                    constexpr uint32_t whitePixel = 0xffffffffu;
                    mCommandList->writeTexture(mWhiteTexture, 0, 0, &whitePixel, sizeof(whitePixel));
                    mWhiteTextureUploaded = true;
                    mWhiteTextureUploadedThisFrame = true;
                    MarkPendingUploadFrame();
                    ++mStats.TextureUploads;
                }
            }

            void EndFrame() override
            {
                if (!mCommandList)
                    throw std::logic_error("NVRHI RmlUi EndFrame called without BeginFrame");
                mCommandList->endMarker();
                GetActiveLayerPool().LayerDepth = 0;
                mCommandList = nullptr;
                mBaseFramebuffer = nullptr;
                mFramebuffer = nullptr;
                mBackdropTexture = nullptr;
                mCurrentLayerHandle = {};
                mPipelineMode = PipelineMode::Normal;
                mPipelineStencilReference = 0;
                mClipMaskReference = 0;
                mClipMaskEnabled = false;
            }

            void NotifyFrameSubmitted(uint32_t frameSlot, uint64_t submission) override
            {
                if (frameSlot >= mFramesInFlight)
                    throw std::out_of_range("NVRHI RmlUi submitted frame slot exceeds FramesInFlight");
                if (submission < mLastSubmittedSubmissions[frameSlot])
                    throw std::logic_error("NVRHI RmlUi submitted fence value regressed for a frame slot");
                mLastSubmittedSubmissions[frameSlot] = submission;
                if (!mPendingUploadFrame)
                    return;
                if (frameSlot != mPendingUploadFrameSlot)
                    throw std::logic_error("NVRHI RmlUi upload submission frame slot mismatch");

                for (const PendingUpload& upload : mPendingGeometryUploads)
                {
                    GeometrySlot* geometry = ResolveGeometry(static_cast<Rml::CompiledGeometryHandle>(
                        PackRmlHandle(upload.Index, upload.Generation)));
                    if (!geometry || !geometry->bUploaded)
                        continue;
                    geometry->Vertices.clear();
                    geometry->Vertices.shrink_to_fit();
                    geometry->Indices.clear();
                    geometry->Indices.shrink_to_fit();
                }
                for (const PendingUpload& upload : mPendingTextureUploads)
                {
                    TextureSlot* texture = ResolveTexture(static_cast<Rml::TextureHandle>(
                        PackRmlHandle(upload.Index, upload.Generation)));
                    if (!texture || !texture->bUploaded)
                        continue;
                    texture->Pixels.clear();
                    texture->Pixels.shrink_to_fit();
                }
                mWhiteTextureUploadedThisFrame = false;
                ClearPendingUploads();
            }

            void AbortFrame(uint32_t frameSlot) noexcept override
            {
                if (!mPendingUploadFrame)
                    return;
                if (frameSlot != mPendingUploadFrameSlot)
                {
                    HE_CORE_ERROR(
                        "NVRHI RmlUi upload abort frame slot mismatch: expected {0}, received {1}; "
                        "staging is conservatively retained for re-upload.",
                        mPendingUploadFrameSlot,
                        frameSlot);
                }

                for (const PendingUpload& upload : mPendingGeometryUploads)
                {
                    GeometrySlot* geometry = ResolveGeometry(static_cast<Rml::CompiledGeometryHandle>(
                        PackRmlHandle(upload.Index, upload.Generation)));
                    if (geometry)
                        geometry->bUploaded = false;
                }
                for (const PendingUpload& upload : mPendingTextureUploads)
                {
                    TextureSlot* texture = ResolveTexture(static_cast<Rml::TextureHandle>(
                        PackRmlHandle(upload.Index, upload.Generation)));
                    if (texture)
                        texture->bUploaded = false;
                }
                for (const PendingUpload& generated : mPendingGpuGeneratedTextures)
                {
                    TextureSlot* texture = ResolveTexture(static_cast<Rml::TextureHandle>(
                        PackRmlHandle(generated.Index, generated.Generation)));
                    if (!texture)
                        continue;
                    const uint64_t byteCount = static_cast<uint64_t>(texture->Width)
                        * static_cast<uint64_t>(texture->Height) * 4ull;
                    if (byteCount <= std::numeric_limits<size_t>::max())
                    {
                        // A callback texture cannot be reconstructed once its
                        // layer command list is aborted. Upload transparent
                        // premultiplied pixels on its next use, avoiding stale
                        // or uninitialized GPU memory while keeping the handle valid.
                        texture->Pixels.assign(static_cast<size_t>(byteCount), Rml::byte{});
                        texture->bUploaded = false;
                    }
                }
                if (mWhiteTextureUploadedThisFrame)
                    mWhiteTextureUploaded = false;
                mWhiteTextureUploadedThisFrame = false;
                ClearPendingUploads();
            }

            [[nodiscard]] const NvrhiRmlRenderBackendStats& GetStats() const noexcept override
            {
                return mStats;
            }

            void ResetStats() noexcept override
            {
                const uint32_t liveGeometries = mStats.LiveGeometries;
                const uint32_t liveTextures = mStats.LiveTextures;
                const uint32_t deferredReleases = mStats.DeferredReleases;
                const uint32_t cachedPipelines = mStats.CachedPipelines;
                mStats = {};
                mStats.LiveGeometries = liveGeometries;
                mStats.LiveTextures = liveTextures;
                mStats.DeferredReleases = deferredReleases;
                mStats.CachedPipelines = cachedPipelines;
            }

            void Shutdown() override
            {
                if (mShutdown)
                    return;
                if (mCommandList)
                    throw std::logic_error("NVRHI RmlUi renderer cannot shut down inside a frame");

                mRetiredResources.clear();
                mRetiredAuxiliaryResources.clear();
                mFreeGeometrySlots.clear();
                mFreeTextureSlots.clear();
                mGeometrySlots.clear();
                mTextureSlots.clear();
                mPipelines.clear();
                mPostprocessPipelines.clear();
                mDecoratorPipelines.clear();
                mStencilInitializePipelines.clear();
                mFrameLayerPools.clear();
                mDecoratorBindingLayout = nullptr;
                mPostprocessBindingLayout = nullptr;
                mWhiteBindingSet = nullptr;
                mWhiteTexture = nullptr;
                mDefaultSampler = nullptr;
                mInputLayout = nullptr;
                mBindingLayout = nullptr;
                mPixelShader = nullptr;
                mVertexShader = nullptr;
                mDecoratorPixelShader = nullptr;
                mPostprocessPixelShader = nullptr;
                mPostprocessVertexShader = nullptr;
                mDevice = nullptr;
                mStats.LiveGeometries = 0;
                mStats.LiveTextures = 0;
                mStats.DeferredReleases = 0;
                mStats.CachedPipelines = 0;
                mShutdown = true;
            }

            Rml::CompiledGeometryHandle CompileGeometry(
                Rml::Span<const Rml::Vertex> vertices,
                Rml::Span<const int> indices) override
            {
                if (mShutdown || vertices.empty() || indices.empty())
                    return 0;
                if (vertices.size() > std::numeric_limits<uint32_t>::max()
                    || indices.size() > std::numeric_limits<uint32_t>::max())
                {
                    return 0;
                }

                std::vector<uint32_t> convertedIndices;
                convertedIndices.reserve(indices.size());
                for (const int index : indices)
                {
                    if (index < 0 || static_cast<size_t>(index) >= vertices.size())
                        return 0;
                    convertedIndices.push_back(static_cast<uint32_t>(index));
                }

                nvrhi::BufferDesc vertexDesc;
                vertexDesc.setByteSize(static_cast<uint64_t>(vertices.size()) * sizeof(Rml::Vertex))
                    .setIsVertexBuffer(true)
                    .enableAutomaticStateTracking(nvrhi::ResourceStates::VertexBuffer)
                    .setDebugName("RmlUi Geometry Vertices");
                nvrhi::BufferHandle vertexBuffer = mDevice->createBuffer(vertexDesc);

                nvrhi::BufferDesc indexDesc;
                indexDesc.setByteSize(static_cast<uint64_t>(convertedIndices.size()) * sizeof(uint32_t))
                    .setIsIndexBuffer(true)
                    .enableAutomaticStateTracking(nvrhi::ResourceStates::IndexBuffer)
                    .setDebugName("RmlUi Geometry Indices");
                nvrhi::BufferHandle indexBuffer = mDevice->createBuffer(indexDesc);
                if (!vertexBuffer || !indexBuffer)
                    return 0;

                const uint32_t slotIndex = AllocateGeometrySlot();
                GeometrySlot& slot = mGeometrySlots[slotIndex];
                slot.VertexBuffer = std::move(vertexBuffer);
                slot.IndexBuffer = std::move(indexBuffer);
                slot.Vertices.assign(vertices.begin(), vertices.end());
                slot.Indices = std::move(convertedIndices);
                slot.IndexCount = static_cast<uint32_t>(slot.Indices.size());
                slot.bUploaded = false;
                ++mStats.LiveGeometries;
                return static_cast<Rml::CompiledGeometryHandle>(PackRmlHandle(slotIndex, slot.Generation));
            }

            void RenderGeometry(
                Rml::CompiledGeometryHandle geometryHandle,
                Rml::Vector2f translation,
                Rml::TextureHandle textureHandle) override
            {
                RequireActiveFrame("RenderGeometry");
                GeometrySlot* geometry = ResolveGeometry(geometryHandle);
                if (!geometry)
                {
                    ++mStats.InvalidGeometryHandles;
                    return;
                }

                nvrhi::IBindingSet* bindingSet = mWhiteBindingSet;
                TextureSlot* texture = nullptr;
                if (textureHandle != 0)
                {
                    texture = ResolveTexture(textureHandle);
                    if (!texture)
                    {
                        ++mStats.InvalidTextureHandles;
                    }
                    else
                    {
                        UploadTexture(*texture);
                        bindingSet = texture->BindingSet;
                    }
                }

                UploadGeometry(*geometry);
                nvrhi::IGraphicsPipeline* pipeline = GetOrCreatePipeline(mPipelineMode);

                const int viewportWidth = std::max(static_cast<int>(mViewportWidth), 1);
                const int viewportHeight = std::max(static_cast<int>(mViewportHeight), 1);
                const float scissorScaleX = mViewportWidth / std::max(mLayoutWidth, 1.f);
                const float scissorScaleY = mViewportHeight / std::max(mLayoutHeight, 1.f);
                int left = 0;
                int top = 0;
                int right = viewportWidth;
                int bottom = viewportHeight;
                if (mScissorEnabled)
                {
                    left = std::clamp(
                        static_cast<int>(std::floor(mScissor.Left() * scissorScaleX)),
                        0,
                        viewportWidth);
                    top = std::clamp(
                        static_cast<int>(std::floor(mScissor.Top() * scissorScaleY)),
                        0,
                        viewportHeight);
                    right = std::clamp(
                        static_cast<int>(std::ceil(mScissor.Right() * scissorScaleX)),
                        left,
                        viewportWidth);
                    bottom = std::clamp(
                        static_cast<int>(std::ceil(mScissor.Bottom() * scissorScaleY)),
                        top,
                        viewportHeight);
                }
                if (right <= left || bottom <= top)
                    return;

                const Rml::Matrix4f projection = Rml::Matrix4f::ProjectOrtho(
                    0.f, mLayoutWidth, mLayoutHeight, 0.f, -10000.f, 10000.f);
                const Rml::Matrix4f transform = projection * mTransform;
                RmlProjectionConstants constants;
                std::memcpy(constants.Transform, transform.data(), sizeof(constants.Transform));
                constants.Translation[0] = translation.x;
                constants.Translation[1] = translation.y;

                nvrhi::ViewportState viewportState;
                viewportState.addViewport(nvrhi::Viewport(mViewportWidth, mViewportHeight))
                    .addScissorRect(nvrhi::Rect(left, right, top, bottom));
                nvrhi::GraphicsState state;
                state.setPipeline(pipeline)
                    .setFramebuffer(mFramebuffer)
                    .setViewport(viewportState)
                    .addBindingSet(bindingSet)
                    .addVertexBuffer(nvrhi::VertexBufferBinding()
                        .setBuffer(geometry->VertexBuffer)
                        .setSlot(0)
                        .setOffset(0))
                    .setIndexBuffer(nvrhi::IndexBufferBinding()
                        .setBuffer(geometry->IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT)
                        .setOffset(0));
                if (mPipelineMode != PipelineMode::Normal)
                    state.setDynamicStencilRefValue(mPipelineStencilReference);
                mCommandList->setGraphicsState(state);
                mCommandList->setPushConstants(&constants, sizeof(constants));
                mCommandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(
                    geometry->IndexCount));
                ++mStats.DrawCalls;
                mStats.RenderedTriangles += geometry->IndexCount / 3u;
            }

            void ReleaseGeometry(Rml::CompiledGeometryHandle geometryHandle) override
            {
                uint32_t index = 0;
                uint32_t generation = 0;
                UnpackRmlHandle(geometryHandle, index, generation);
                GeometrySlot* slot = ResolveGeometry(geometryHandle);
                if (!slot)
                {
                    ++mStats.InvalidGeometryHandles;
                    return;
                }

                RetiredResource retired;
                retired.Type = RetiredResourceType::Geometry;
                retired.Index = index;
                retired.VertexBuffer = std::move(slot->VertexBuffer);
                retired.IndexBuffer = std::move(slot->IndexBuffer);
                retired.PendingFrameSlots.assign(mFramesInFlight, 1);
                slot->Vertices.clear();
                slot->Indices.clear();
                slot->IndexCount = 0;
                slot->bUploaded = false;
                slot->bOccupied = false;
                mRetiredResources.push_back(std::move(retired));
                --mStats.LiveGeometries;
                UpdateDeferredReleaseStats();
            }

            Rml::TextureHandle LoadTexture(
                Rml::Vector2i& textureDimensions,
                const Rml::String& source) override
            {
                const std::vector<Rml::byte> encoded = LoadTextureFileBytes(source);
                if (encoded.empty() || encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                    return 0;

                int width = 0;
                int height = 0;
                int channels = 0;
                stbi_set_flip_vertically_on_load_thread(0);
                stbi_uc* decoded = stbi_load_from_memory(
                    encoded.data(),
                    static_cast<int>(encoded.size()),
                    &width,
                    &height,
                    &channels,
                    STBI_rgb_alpha);
                if (!decoded || width <= 0 || height <= 0)
                {
                    stbi_image_free(decoded);
                    return 0;
                }

                const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
                std::vector<Rml::byte> pixels(decoded, decoded + byteCount);
                stbi_image_free(decoded);
                PremultiplyAlpha(pixels);
                const Rml::TextureHandle handle = GenerateTexture(
                    Rml::Span<const Rml::byte>(pixels.data(), pixels.size()),
                    Rml::Vector2i(width, height));
                if (handle != 0)
                    textureDimensions = Rml::Vector2i(width, height);
                return handle;
            }

            Rml::TextureHandle GenerateTexture(
                Rml::Span<const Rml::byte> source,
                Rml::Vector2i sourceDimensions) override
            {
                if (mShutdown || sourceDimensions.x <= 0 || sourceDimensions.y <= 0)
                    return 0;
                const uint64_t byteCount = static_cast<uint64_t>(sourceDimensions.x)
                    * static_cast<uint64_t>(sourceDimensions.y) * 4ull;
                if (byteCount != source.size() || byteCount > std::numeric_limits<size_t>::max())
                    return 0;

                nvrhi::TextureDesc textureDesc;
                textureDesc.setWidth(static_cast<uint32_t>(sourceDimensions.x))
                    .setHeight(static_cast<uint32_t>(sourceDimensions.y))
                    .setFormat(nvrhi::Format::RGBA8_UNORM)
                    .setDimension(nvrhi::TextureDimension::Texture2D)
                    .setDebugName("RmlUi Texture")
                    .enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
                nvrhi::TextureHandle texture = mDevice->createTexture(textureDesc);
                if (!texture)
                    return 0;
                nvrhi::BindingSetHandle bindingSet = CreateTextureBindingSet(texture);
                if (!bindingSet)
                    return 0;

                const uint32_t slotIndex = AllocateTextureSlot();
                TextureSlot& slot = mTextureSlots[slotIndex];
                slot.Texture = std::move(texture);
                slot.BindingSet = std::move(bindingSet);
                slot.Pixels.assign(source.begin(), source.end());
                slot.Width = static_cast<uint32_t>(sourceDimensions.x);
                slot.Height = static_cast<uint32_t>(sourceDimensions.y);
                slot.bUploaded = false;
                ++mStats.LiveTextures;
                return static_cast<Rml::TextureHandle>(PackRmlHandle(slotIndex, slot.Generation));
            }

            void ReleaseTexture(Rml::TextureHandle textureHandle) override
            {
                uint32_t index = 0;
                uint32_t generation = 0;
                UnpackRmlHandle(textureHandle, index, generation);
                TextureSlot* slot = ResolveTexture(textureHandle);
                if (!slot)
                {
                    ++mStats.InvalidTextureHandles;
                    return;
                }

                RetiredResource retired;
                retired.Type = RetiredResourceType::Texture;
                retired.Index = index;
                retired.Texture = std::move(slot->Texture);
                retired.BindingSet = std::move(slot->BindingSet);
                retired.PendingFrameSlots.assign(mFramesInFlight, 1);
                slot->Pixels.clear();
                slot->Width = 0;
                slot->Height = 0;
                slot->bUploaded = false;
                slot->bOccupied = false;
                mRetiredResources.push_back(std::move(retired));
                --mStats.LiveTextures;
                UpdateDeferredReleaseStats();
            }

            void EnableScissorRegion(bool enable) override
            {
                mScissorEnabled = enable;
            }

            void SetScissorRegion(Rml::Rectanglei region) override
            {
                mScissor = region;
            }

            void SetTransform(const Rml::Matrix4f* transform) override
            {
                mTransform = transform ? *transform : Rml::Matrix4f::Identity();
            }

            void EnableClipMask(bool enable) override
            {
                RequireActiveFrame("EnableClipMask");
                if (!enable)
                {
                    mClipMaskEnabled = false;
                    mClipMaskReference = 0;
                    mPipelineStencilReference = 0;
                    mPipelineMode = PipelineMode::Normal;
                    return;
                }

                mClipMaskEnabled = HasWritableStencilAttachment();
                mClipMaskReference = 0;
                mPipelineStencilReference = 0;
                mPipelineMode = PipelineMode::Normal;
                if (!mClipMaskEnabled)
                    ReportClipMaskTargetError("EnableClipMask");
            }

            void RenderToClipMask(
                Rml::ClipMaskOperation operation,
                Rml::CompiledGeometryHandle geometry,
                Rml::Vector2f translation) override
            {
                RequireActiveFrame("RenderToClipMask");
                if (!mClipMaskEnabled)
                {
                    if (HasWritableStencilAttachment())
                        ReportClipMaskSequenceError("RenderToClipMask called while the clip mask is disabled");
                    else
                        ReportClipMaskTargetError("RenderToClipMask");
                    return;
                }

                PipelineMode maskMode = PipelineMode::ClipMaskReplace;
                uint8_t writeReference = 1;
                uint8_t resultReference = 1;
                switch (operation)
                {
                case Rml::ClipMaskOperation::Set:
                    InitializeClipMaskStencil(0);
                    break;
                case Rml::ClipMaskOperation::SetInverse:
                    InitializeClipMaskStencil(1);
                    writeReference = 0;
                    break;
                case Rml::ClipMaskOperation::Intersect:
                    if (mClipMaskReference == 0)
                    {
                        ReportClipMaskSequenceError(
                            "RenderToClipMask(Intersect) requires an existing Set or SetInverse mask");
                        return;
                    }
                    if (mClipMaskReference >= MaxClipMaskReference)
                    {
                        ReportClipMaskSequenceError(
                            "RmlUi clip-mask intersection depth exceeds the supported limit of 254");
                        return;
                    }
                    maskMode = PipelineMode::ClipMaskIncrement;
                    writeReference = mClipMaskReference;
                    resultReference = static_cast<uint8_t>(mClipMaskReference + 1u);
                    break;
                }

                mPipelineMode = maskMode;
                mPipelineStencilReference = writeReference;
                try
                {
                    RenderGeometry(geometry, translation, {});
                }
                catch (...)
                {
                    mPipelineMode = PipelineMode::Normal;
                    mPipelineStencilReference = 0;
                    mClipMaskReference = 0;
                    mClipMaskEnabled = false;
                    throw;
                }

                mClipMaskReference = resultReference;
                mPipelineStencilReference = resultReference;
                mPipelineMode = PipelineMode::NormalClipped;
            }

            Rml::LayerHandle PushLayer() override
            {
                RequireActiveFrame("PushLayer");
                FrameLayerPool& pool = GetActiveLayerPool();
                if (pool.LayerDepth >= MaxRenderLayers)
                {
                    ReportRejectedFeature("render layer depth exceeds the fixed limit of 8");
                    throw std::runtime_error("NVRHI RmlUi render layer depth exceeds 8");
                }

                SaveCurrentClipState();
                const bool bInheritClipMask = mClipMaskEnabled && mClipMaskReference > 0
                    && HasWritableStencilAttachment();
                nvrhi::ITexture* inheritedDepthStencil = bInheritClipMask
                    ? mFramebuffer->getDesc().depthAttachment.texture
                    : nullptr;
                const uint8_t inheritedClipReference = mClipMaskReference;
                RenderTarget& layer = EnsureLayerTarget(pool, pool.LayerDepth);
                mCommandList->clearTextureFloat(
                    layer.Color,
                    nvrhi::AllSubresources,
                    nvrhi::Color(0.f, 0.f, 0.f, 0.f));
                if (inheritedDepthStencil
                    && inheritedDepthStencil->getDesc().format == nvrhi::Format::D24S8
                    && inheritedDepthStencil->getDesc().width == layer.Width
                    && inheritedDepthStencil->getDesc().height == layer.Height)
                {
                    mCommandList->copyTexture(
                        layer.DepthStencil,
                        nvrhi::TextureSlice(),
                        inheritedDepthStencil,
                        nvrhi::TextureSlice());
                    layer.bClipMaskEnabled = true;
                    layer.ClipMaskReference = inheritedClipReference;
                }
                else
                {
                    mCommandList->clearDepthStencilTexture(
                        layer.DepthStencil,
                        nvrhi::AllSubresources,
                        true,
                        1.f,
                        true,
                        0);
                    layer.bClipMaskEnabled = false;
                    layer.ClipMaskReference = 0;
                }
                ++pool.LayerDepth;
                mStats.PeakLayerDepth = std::max(mStats.PeakLayerDepth, pool.LayerDepth);
                const Rml::LayerHandle handle = static_cast<Rml::LayerHandle>(pool.LayerDepth);
                SwitchActiveTarget(handle);
                return handle;
            }

            void CompositeLayers(
                Rml::LayerHandle source,
                Rml::LayerHandle destination,
                Rml::BlendMode blendMode,
                Rml::Span<const Rml::CompiledFilterHandle> filters) override
            {
                RequireActiveFrame("CompositeLayers");
                ValidateLayerHandle(source, "source");
                ValidateLayerHandle(destination, "destination");
                CompositeLayerContents(source, destination, blendMode, filters);
                ++mStats.LayerComposites;
            }

            void PopLayer() override
            {
                RequireActiveFrame("PopLayer");
                FrameLayerPool& pool = GetActiveLayerPool();
                if (pool.LayerDepth == 0)
                    throw std::logic_error("NVRHI RmlUi PopLayer called with an empty layer stack");
                SaveCurrentClipState();
                --pool.LayerDepth;
                SwitchActiveTarget(static_cast<Rml::LayerHandle>(pool.LayerDepth));
            }

            Rml::TextureHandle SaveLayerAsTexture() override
            {
                RequireActiveFrame("SaveLayerAsTexture");
                const PixelRectangle bounds = GetPixelScissor();
                if (bounds.Right <= bounds.Left || bounds.Bottom <= bounds.Top)
                    return {};
                nvrhi::ITexture* source = GetLayerColor(mCurrentLayerHandle);
                if (!source || source->getDesc().format != nvrhi::Format::RGBA8_UNORM)
                {
                    ReportRejectedFeature("SaveLayerAsTexture requires an RGBA8 render layer");
                    return {};
                }

                const uint32_t width = static_cast<uint32_t>(bounds.Right - bounds.Left);
                const uint32_t height = static_cast<uint32_t>(bounds.Bottom - bounds.Top);
                nvrhi::TextureHandle texture = CreateColorTexture(
                    width,
                    height,
                    false,
                    "RmlUi Saved Layer Texture");
                if (!texture)
                    return {};
                nvrhi::BindingSetHandle bindingSet = CreateTextureBindingSet(texture);
                if (!bindingSet)
                    return {};

                nvrhi::TextureSlice destinationSlice;
                destinationSlice.setSize(width, height, 1);
                nvrhi::TextureSlice sourceSlice;
                sourceSlice.setOrigin(
                    static_cast<uint32_t>(bounds.Left),
                    static_cast<uint32_t>(bounds.Top),
                    0).setSize(width, height, 1);
                mCommandList->copyTexture(texture, destinationSlice, source, sourceSlice);

                const uint32_t slotIndex = AllocateTextureSlot();
                TextureSlot& slot = mTextureSlots[slotIndex];
                slot.Texture = std::move(texture);
                slot.BindingSet = std::move(bindingSet);
                slot.Pixels.clear();
                slot.Width = width;
                slot.Height = height;
                slot.bUploaded = true;
                ++mStats.LiveTextures;
                mPendingGpuGeneratedTextures.push_back(PendingUpload{ slotIndex, slot.Generation });
                MarkPendingUploadFrame();
                return static_cast<Rml::TextureHandle>(PackRmlHandle(slotIndex, slot.Generation));
            }

            Rml::CompiledFilterHandle SaveLayerAsMaskImage() override
            {
                RequireActiveFrame("SaveLayerAsMaskImage");
                nvrhi::ITexture* source = GetLayerColor(mCurrentLayerHandle);
                if (!source || source->getDesc().format != nvrhi::Format::RGBA8_UNORM)
                {
                    ReportRejectedFeature("SaveLayerAsMaskImage requires an RGBA8 render layer");
                    return {};
                }

                auto filter = std::make_unique<CompiledFilter>();
                filter->Type = FilterType::MaskImage;
                filter->MaskTexture = CreateColorTexture(
                    static_cast<uint32_t>(mViewportWidth),
                    static_cast<uint32_t>(mViewportHeight),
                    false,
                    "RmlUi Mask Image");
                if (!filter->MaskTexture)
                    return {};
                mCommandList->copyTexture(
                    filter->MaskTexture,
                    nvrhi::TextureSlice(),
                    source,
                    nvrhi::TextureSlice());
                return reinterpret_cast<Rml::CompiledFilterHandle>(filter.release());
            }

            Rml::CompiledFilterHandle CompileFilter(
                const Rml::String& name,
                const Rml::Dictionary& parameters) override
            {
                auto filter = std::make_unique<CompiledFilter>();
                if (name == "opacity")
                {
                    filter->Type = FilterType::Opacity;
                    filter->Value = std::clamp(FiniteOr(Rml::Get(parameters, "value", 1.f), 1.f), 0.f, 1.f);
                }
                else if (name == "blur")
                {
                    filter->Type = FilterType::Blur;
                    filter->Sigma = std::clamp(FiniteOr(Rml::Get(parameters, "sigma", 1.f), 1.f), 0.f, 32.f);
                }
                else if (name == "drop-shadow")
                {
                    filter->Type = FilterType::DropShadow;
                    filter->Sigma = std::clamp(FiniteOr(Rml::Get(parameters, "sigma", 0.f), 0.f), 0.f, 32.f);
                    filter->Offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.f));
                    filter->Offset.x = std::clamp(FiniteOr(filter->Offset.x, 0.f), -4096.f, 4096.f);
                    filter->Offset.y = std::clamp(FiniteOr(filter->Offset.y, 0.f), -4096.f, 4096.f);
                    filter->Color = Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied();
                }
                else if (name == "brightness")
                {
                    filter->Type = FilterType::ColorMatrix;
                    const float value = std::clamp(FiniteOr(Rml::Get(parameters, "value", 1.f), 1.f), 0.f, 8.f);
                    filter->ColorMatrix = Rml::Matrix4f::Diag(value, value, value, 1.f);
                }
                else if (name == "contrast")
                {
                    filter->Type = FilterType::ColorMatrix;
                    const float value = std::clamp(FiniteOr(Rml::Get(parameters, "value", 1.f), 1.f), 0.f, 8.f);
                    const float grayness = 0.5f - 0.5f * value;
                    filter->ColorMatrix = Rml::Matrix4f::Diag(value, value, value, 1.f);
                    filter->ColorMatrix.SetColumn(3, Rml::Vector4f(grayness, grayness, grayness, 1.f));
                }
                else if (name == "invert")
                {
                    filter->Type = FilterType::ColorMatrix;
                    const float value = std::clamp(FiniteOr(Rml::Get(parameters, "value", 1.f), 1.f), 0.f, 1.f);
                    const float inverted = 1.f - 2.f * value;
                    filter->ColorMatrix = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.f);
                    filter->ColorMatrix.SetColumn(3, Rml::Vector4f(value, value, value, 1.f));
                }
                else if (name == "grayscale")
                {
                    filter->Type = FilterType::ColorMatrix;
                    const float value = std::clamp(FiniteOr(Rml::Get(parameters, "value", 1.f), 1.f), 0.f, 1.f);
                    const float reverse = 1.f - value;
                    const Rml::Vector3f gray = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
                    filter->ColorMatrix = Rml::Matrix4f::FromRows(
                        { gray.x + reverse, gray.y, gray.z, 0.f },
                        { gray.x, gray.y + reverse, gray.z, 0.f },
                        { gray.x, gray.y, gray.z + reverse, 0.f },
                        { 0.f, 0.f, 0.f, 1.f });
                }
                else if (name == "sepia")
                {
                    filter->Type = FilterType::ColorMatrix;
                    const float value = std::clamp(FiniteOr(Rml::Get(parameters, "value", 1.f), 1.f), 0.f, 1.f);
                    const float reverse = 1.f - value;
                    const Rml::Vector3f red = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
                    const Rml::Vector3f green = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
                    const Rml::Vector3f blue = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
                    filter->ColorMatrix = Rml::Matrix4f::FromRows(
                        { red.x + reverse, red.y, red.z, 0.f },
                        { green.x, green.y + reverse, green.z, 0.f },
                        { blue.x, blue.y, blue.z + reverse, 0.f },
                        { 0.f, 0.f, 0.f, 1.f });
                }
                else if (name == "hue-rotate")
                {
                    filter->Type = FilterType::ColorMatrix;
                    const float value = FiniteOr(Rml::Get(parameters, "value", 0.f), 0.f);
                    const float sine = std::sin(value);
                    const float cosine = std::cos(value);
                    filter->ColorMatrix = Rml::Matrix4f::FromRows(
                        { 0.213f + 0.787f * cosine - 0.213f * sine, 0.715f - 0.715f * cosine - 0.715f * sine, 0.072f - 0.072f * cosine + 0.928f * sine, 0.f },
                        { 0.213f - 0.213f * cosine + 0.143f * sine, 0.715f + 0.285f * cosine + 0.140f * sine, 0.072f - 0.072f * cosine - 0.283f * sine, 0.f },
                        { 0.213f - 0.213f * cosine - 0.787f * sine, 0.715f - 0.715f * cosine + 0.715f * sine, 0.072f + 0.928f * cosine + 0.072f * sine, 0.f },
                        { 0.f, 0.f, 0.f, 1.f });
                }
                else if (name == "saturate")
                {
                    filter->Type = FilterType::ColorMatrix;
                    const float value = std::clamp(FiniteOr(Rml::Get(parameters, "value", 1.f), 1.f), 0.f, 8.f);
                    filter->ColorMatrix = Rml::Matrix4f::FromRows(
                        { 0.213f + 0.787f * value, 0.715f - 0.715f * value, 0.072f - 0.072f * value, 0.f },
                        { 0.213f - 0.213f * value, 0.715f + 0.285f * value, 0.072f - 0.072f * value, 0.f },
                        { 0.213f - 0.213f * value, 0.715f - 0.715f * value, 0.072f + 0.928f * value, 0.f },
                        { 0.f, 0.f, 0.f, 1.f });
                }

                if (filter->Type == FilterType::Invalid)
                {
                    ReportRejectedFeature(std::string("unknown filter '") + name.c_str() + "'");
                    return {};
                }
                return reinterpret_cast<Rml::CompiledFilterHandle>(filter.release());
            }

            void ReleaseFilter(Rml::CompiledFilterHandle filterHandle) override
            {
                if (!filterHandle)
                    return;
                std::unique_ptr<CompiledFilter> filter(reinterpret_cast<CompiledFilter*>(filterHandle));
                if (filter->MaskTexture)
                {
                    RetiredAuxiliaryResource retired;
                    retired.Texture = std::move(filter->MaskTexture);
                    retired.PendingFrameSlots.assign(mFramesInFlight, 1);
                    mRetiredAuxiliaryResources.push_back(std::move(retired));
                    UpdateDeferredReleaseStats();
                }
            }

            Rml::CompiledShaderHandle CompileShader(
                const Rml::String& name,
                const Rml::Dictionary& parameters) override
            {
                if (name != "shader")
                {
                    ReportRejectedFeature(std::string("non-whitelisted shader type '") + name.c_str() + "'");
                    return {};
                }

                auto shader = std::make_unique<CompiledDecorator>();
                const Rml::String value = Rml::Get(parameters, "value", Rml::String());
                if (value == "he-sheen")
                    shader->Type = DecoratorType::Sheen;
                else if (value == "he-halftone")
                    shader->Type = DecoratorType::Halftone;
                else
                {
                    ReportRejectedFeature(std::string("custom shader value '") + value.c_str() + "' is not whitelisted");
                    return {};
                }
                shader->Dimensions = Rml::Get(parameters, "dimensions", Rml::Vector2f(1.f));
                shader->Dimensions.x = std::clamp(FiniteOr(shader->Dimensions.x, 1.f), 1.f, 16384.f);
                shader->Dimensions.y = std::clamp(FiniteOr(shader->Dimensions.y, 1.f), 1.f, 16384.f);
                return reinterpret_cast<Rml::CompiledShaderHandle>(shader.release());
            }

            void RenderShader(
                Rml::CompiledShaderHandle shaderHandle,
                Rml::CompiledGeometryHandle geometryHandle,
                Rml::Vector2f translation,
                Rml::TextureHandle textureHandle) override
            {
                RequireActiveFrame("RenderShader");
                if (!shaderHandle)
                    return;
                RenderDecorator(
                    *reinterpret_cast<const CompiledDecorator*>(shaderHandle),
                    geometryHandle,
                    translation,
                    textureHandle);
            }

            void ReleaseShader(Rml::CompiledShaderHandle shaderHandle) override
            {
                delete reinterpret_cast<CompiledDecorator*>(shaderHandle);
            }

        private:
            [[nodiscard]] static float FiniteOr(float value, float fallback) noexcept
            {
                return std::isfinite(value) ? value : fallback;
            }

            [[nodiscard]] FrameLayerPool& GetActiveLayerPool()
            {
                return mFrameLayerPools.at(mActiveFrameSlot);
            }

            [[nodiscard]] const FrameLayerPool& GetActiveLayerPool() const
            {
                return mFrameLayerPools.at(mActiveFrameSlot);
            }

            void ValidateLayerHandle(Rml::LayerHandle handle, std::string_view role) const
            {
                if (handle == 0)
                    return;
                const uint64_t value = static_cast<uint64_t>(handle);
                if (value > GetActiveLayerPool().LayerDepth || value > MaxRenderLayers)
                {
                    throw std::out_of_range(
                        std::string("NVRHI RmlUi ") + std::string(role) + " layer handle is not active");
                }
            }

            [[nodiscard]] nvrhi::ITexture* GetBaseColor() const
            {
                if (!mBaseFramebuffer || mBaseFramebuffer->getDesc().colorAttachments.empty())
                    return nullptr;
                const nvrhi::FramebufferAttachment& attachment =
                    mBaseFramebuffer->getDesc().colorAttachments[0];
                return attachment.valid() ? attachment.texture : nullptr;
            }

            [[nodiscard]] nvrhi::ITexture* GetLayerColor(
                Rml::LayerHandle handle,
                bool bUseBackdropForBase = false) const
            {
                if (handle == 0)
                {
                    if (bUseBackdropForBase && mBackdropTexture)
                        return mBackdropTexture;
                    return GetBaseColor();
                }
                ValidateLayerHandle(handle, "source");
                return GetActiveLayerPool().Layers[static_cast<size_t>(handle - 1)].Color;
            }

            [[nodiscard]] nvrhi::IFramebuffer* GetLayerFramebuffer(Rml::LayerHandle handle) const
            {
                if (handle == 0)
                    return mBaseFramebuffer;
                ValidateLayerHandle(handle, "destination");
                return GetActiveLayerPool().Layers[static_cast<size_t>(handle - 1)].Framebuffer;
            }

            [[nodiscard]] nvrhi::TextureHandle CreateColorTexture(
                uint32_t width,
                uint32_t height,
                bool bRenderTarget,
                const char* debugName) const
            {
                nvrhi::TextureDesc desc;
                desc.setWidth(std::max(width, 1u))
                    .setHeight(std::max(height, 1u))
                    .setFormat(nvrhi::Format::RGBA8_UNORM)
                    .setDimension(nvrhi::TextureDimension::Texture2D)
                    .setIsRenderTarget(bRenderTarget)
                    .setDebugName(debugName)
                    .enableAutomaticStateTracking(
                        bRenderTarget
                            ? nvrhi::ResourceStates::RenderTarget
                            : nvrhi::ResourceStates::ShaderResource);
                if (bRenderTarget)
                    desc.setClearValue(nvrhi::Color(0.f, 0.f, 0.f, 0.f));
                return mDevice->createTexture(desc);
            }

            [[nodiscard]] RenderTarget& EnsureLayerTarget(FrameLayerPool& pool, uint32_t index)
            {
                RenderTarget& target = pool.Layers.at(index);
                const uint32_t width = std::max(static_cast<uint32_t>(std::round(mViewportWidth)), 1u);
                const uint32_t height = std::max(static_cast<uint32_t>(std::round(mViewportHeight)), 1u);
                if (target.Framebuffer && target.Width == width && target.Height == height)
                    return target;

                pool.PostprocessBindings.clear();
                pool.DecoratorBindings.clear();
                target = {};
                target.Color = CreateColorTexture(width, height, true, "RmlUi Render Layer Color");

                nvrhi::TextureDesc depthDesc;
                depthDesc.setWidth(width)
                    .setHeight(height)
                    .setFormat(nvrhi::Format::D24S8)
                    .setDimension(nvrhi::TextureDimension::Texture2D)
                    .setIsRenderTarget(true)
                    .setClearValue(nvrhi::Color(1.f, 0.f, 0.f, 0.f))
                    .setDebugName("RmlUi Render Layer DepthStencil")
                    .enableAutomaticStateTracking(nvrhi::ResourceStates::DepthWrite);
                target.DepthStencil = mDevice->createTexture(depthDesc);

                if (target.Color && target.DepthStencil)
                {
                    nvrhi::FramebufferDesc framebufferDesc;
                    framebufferDesc.addColorAttachment(target.Color)
                        .setDepthAttachment(target.DepthStencil);
                    target.Framebuffer = mDevice->createFramebuffer(framebufferDesc);
                }
                if (!target.Framebuffer)
                    throw std::runtime_error("NVRHI failed to allocate an RmlUi RGBA8+D24S8 render layer");
                target.Width = width;
                target.Height = height;
                ++mStats.LayerTargetsAllocated;
                return target;
            }

            [[nodiscard]] ColorTarget& EnsurePostprocessTarget(FrameLayerPool& pool, uint32_t index)
            {
                ColorTarget& target = pool.Postprocess.at(index);
                const uint32_t width = std::max(static_cast<uint32_t>(std::round(mViewportWidth)), 1u);
                const uint32_t height = std::max(static_cast<uint32_t>(std::round(mViewportHeight)), 1u);
                if (target.Framebuffer && target.Width == width && target.Height == height)
                    return target;

                pool.PostprocessBindings.clear();
                target = {};
                target.Color = CreateColorTexture(width, height, true, "RmlUi Postprocess Color");
                if (target.Color)
                {
                    nvrhi::FramebufferDesc framebufferDesc;
                    framebufferDesc.addColorAttachment(target.Color);
                    target.Framebuffer = mDevice->createFramebuffer(framebufferDesc);
                }
                if (!target.Framebuffer)
                    throw std::runtime_error("NVRHI failed to allocate an RmlUi postprocess target");
                target.Width = width;
                target.Height = height;
                return target;
            }

            void SaveCurrentClipState()
            {
                if (!mCommandList)
                    return;
                if (mCurrentLayerHandle == 0)
                {
                    mBaseClipMaskEnabled = mClipMaskEnabled;
                    mBaseClipMaskReference = mClipMaskReference;
                    return;
                }
                RenderTarget& target = GetActiveLayerPool().Layers[
                    static_cast<size_t>(mCurrentLayerHandle - 1)];
                target.bClipMaskEnabled = mClipMaskEnabled;
                target.ClipMaskReference = mClipMaskReference;
            }

            void SwitchActiveTarget(Rml::LayerHandle handle)
            {
                ValidateLayerHandle(handle, "active");
                SaveCurrentClipState();
                mCurrentLayerHandle = handle;
                mFramebuffer = GetLayerFramebuffer(handle);
                if (handle == 0)
                {
                    mClipMaskEnabled = mBaseClipMaskEnabled;
                    mClipMaskReference = mBaseClipMaskReference;
                }
                else
                {
                    const RenderTarget& target = GetActiveLayerPool().Layers[
                        static_cast<size_t>(handle - 1)];
                    mClipMaskEnabled = target.bClipMaskEnabled;
                    mClipMaskReference = target.ClipMaskReference;
                }
                mPipelineMode = mClipMaskEnabled && mClipMaskReference > 0
                    ? PipelineMode::NormalClipped
                    : PipelineMode::Normal;
                mPipelineStencilReference = mClipMaskReference;
            }

            [[nodiscard]] PixelRectangle GetPixelScissor() const
            {
                const int viewportWidth = std::max(static_cast<int>(std::round(mViewportWidth)), 1);
                const int viewportHeight = std::max(static_cast<int>(std::round(mViewportHeight)), 1);
                PixelRectangle result{ 0, 0, viewportWidth, viewportHeight };
                if (!mScissorEnabled)
                    return result;

                const float scaleX = mViewportWidth / std::max(mLayoutWidth, 1.f);
                const float scaleY = mViewportHeight / std::max(mLayoutHeight, 1.f);
                result.Left = std::clamp(
                    static_cast<int>(std::floor(mScissor.Left() * scaleX)), 0, viewportWidth);
                result.Top = std::clamp(
                    static_cast<int>(std::floor(mScissor.Top() * scaleY)), 0, viewportHeight);
                result.Right = std::clamp(
                    static_cast<int>(std::ceil(mScissor.Right() * scaleX)), result.Left, viewportWidth);
                result.Bottom = std::clamp(
                    static_cast<int>(std::ceil(mScissor.Bottom() * scaleY)), result.Top, viewportHeight);
                return result;
            }

            [[nodiscard]] nvrhi::IBindingSet* GetOrCreatePostprocessBindingSet(
                nvrhi::ITexture* source,
                nvrhi::ITexture* secondary)
            {
                if (!source)
                    return nullptr;
                if (!secondary)
                    secondary = mWhiteTexture;
                FrameLayerPool& pool = GetActiveLayerPool();
                for (const PostprocessBindingEntry& entry : pool.PostprocessBindings)
                {
                    if (entry.Source == source && entry.Secondary == secondary)
                        return entry.BindingSet;
                }

                nvrhi::BindingSetDesc desc;
                desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(RmlPostprocessConstants)))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(1, secondary))
                    .addItem(nvrhi::BindingSetItem::Sampler(0, mDefaultSampler));
                nvrhi::BindingSetHandle bindingSet = mDevice->createBindingSet(desc, mPostprocessBindingLayout);
                if (!bindingSet)
                    throw std::runtime_error("NVRHI failed to create an RmlUi postprocess binding set");
                pool.PostprocessBindings.push_back(PostprocessBindingEntry{
                    source,
                    secondary,
                    std::move(bindingSet),
                });
                return pool.PostprocessBindings.back().BindingSet;
            }

            [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreatePostprocessPipeline(
                nvrhi::IFramebuffer* framebuffer,
                bool bBlend,
                bool bClipped)
            {
                const nvrhi::FramebufferInfo info = framebuffer->getFramebufferInfo();
                for (const PostprocessPipelineEntry& entry : mPostprocessPipelines)
                {
                    if (entry.FramebufferInfo == info && entry.bBlend == bBlend && entry.bClipped == bClipped)
                        return entry.Pipeline;
                }

                nvrhi::GraphicsPipelineDesc desc;
                desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                    .setVertexShader(mPostprocessVertexShader)
                    .setPixelShader(mPostprocessPixelShader)
                    .addBindingLayout(mPostprocessBindingLayout);
                desc.renderState.rasterState.setCullNone().enableScissor().enableDepthClip();
                nvrhi::DepthStencilState& depthStencil = desc.renderState.depthStencilState;
                depthStencil.disableDepthTest().disableDepthWrite();
                if (bBlend)
                {
                    desc.renderState.blendState.targets[0]
                        .enableBlend()
                        .setSrcBlend(nvrhi::BlendFactor::One)
                        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                        .setBlendOp(nvrhi::BlendOp::Add)
                        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                        .setBlendOpAlpha(nvrhi::BlendOp::Add);
                }
                if (bClipped)
                {
                    nvrhi::DepthStencilState::StencilOpDesc stencil;
                    stencil.setStencilFunc(nvrhi::ComparisonFunc::Equal);
                    depthStencil.enableStencil()
                        .setStencilReadMask(0xff)
                        .setStencilWriteMask(0)
                        .setDynamicStencilRef(true)
                        .setFrontFaceStencil(stencil)
                        .setBackFaceStencil(stencil);
                }

                nvrhi::GraphicsPipelineHandle pipeline = mDevice->createGraphicsPipeline(desc, info);
                if (!pipeline)
                    throw std::runtime_error("NVRHI failed to create an RmlUi postprocess pipeline");
                mPostprocessPipelines.push_back(PostprocessPipelineEntry{
                    info,
                    bBlend,
                    bClipped,
                    std::move(pipeline),
                });
                mStats.CachedPipelines = static_cast<uint32_t>(
                    mPipelines.size() + mPostprocessPipelines.size() + mDecoratorPipelines.size()
                    + mStencilInitializePipelines.size());
                return mPostprocessPipelines.back().Pipeline;
            }

            void RecordPostprocess(
                nvrhi::ITexture* source,
                nvrhi::ITexture* secondary,
                nvrhi::IFramebuffer* destination,
                const RmlPostprocessConstants& constants,
                bool bBlend,
                bool bClipped,
                uint8_t stencilReference)
            {
                const PixelRectangle scissor = GetPixelScissor();
                if (!source || !destination || scissor.Right <= scissor.Left || scissor.Bottom <= scissor.Top)
                    return;
                nvrhi::IBindingSet* bindingSet = GetOrCreatePostprocessBindingSet(source, secondary);
                nvrhi::GraphicsState state;
                state.setPipeline(GetOrCreatePostprocessPipeline(destination, bBlend, bClipped))
                    .setFramebuffer(destination)
                    .setViewport(nvrhi::ViewportState()
                        .addViewport(nvrhi::Viewport(mViewportWidth, mViewportHeight))
                        .addScissorRect(nvrhi::Rect(
                            scissor.Left,
                            scissor.Right,
                            scissor.Top,
                            scissor.Bottom)))
                    .addBindingSet(bindingSet);
                if (bClipped)
                    state.setDynamicStencilRefValue(stencilReference);
                mCommandList->setGraphicsState(state);
                mCommandList->setPushConstants(&constants, sizeof(constants));
                mCommandList->draw(nvrhi::DrawArguments().setVertexCount(3));
                ++mStats.DrawCalls;
                mStats.RenderedTriangles += 1;
            }

            [[nodiscard]] RmlPostprocessConstants MakePostprocessConstants(
                const CompiledFilter* filter) const
            {
                RmlPostprocessConstants constants;
                constants.InverseTextureSize[0] = 1.f / std::max(mViewportWidth, 1.f);
                constants.InverseTextureSize[1] = 1.f / std::max(mViewportHeight, 1.f);
                const PixelRectangle scissor = GetPixelScissor();
                constants.UvMin[0] = (static_cast<float>(scissor.Left) + 0.5f) / std::max(mViewportWidth, 1.f);
                constants.UvMin[1] = (static_cast<float>(scissor.Top) + 0.5f) / std::max(mViewportHeight, 1.f);
                constants.UvMax[0] = (static_cast<float>(std::max(scissor.Right, scissor.Left + 1)) - 0.5f)
                    / std::max(mViewportWidth, 1.f);
                constants.UvMax[1] = (static_cast<float>(std::max(scissor.Bottom, scissor.Top + 1)) - 0.5f)
                    / std::max(mViewportHeight, 1.f);
                const Rml::Matrix4f identity = Rml::Matrix4f::Identity();
                std::memcpy(constants.ColorMatrix, identity.data(), sizeof(constants.ColorMatrix));
                if (!filter)
                    return constants;

                switch (filter->Type)
                {
                case FilterType::Opacity:
                    constants.Mode = static_cast<uint32_t>(RmlPostprocessMode::Opacity);
                    constants.Opacity = filter->Value;
                    break;
                case FilterType::Blur:
                    constants.Mode = static_cast<uint32_t>(RmlPostprocessMode::Blur);
                    constants.Sigma = filter->Sigma * 0.5f * (
                        mViewportWidth / std::max(mLayoutWidth, 1.f)
                        + mViewportHeight / std::max(mLayoutHeight, 1.f));
                    break;
                case FilterType::DropShadow:
                    constants.Mode = static_cast<uint32_t>(RmlPostprocessMode::DropShadow);
                    constants.Sigma = filter->Sigma * 0.5f * (
                        mViewportWidth / std::max(mLayoutWidth, 1.f)
                        + mViewportHeight / std::max(mLayoutHeight, 1.f));
                    constants.OffsetUv[0] = filter->Offset.x / std::max(mLayoutWidth, 1.f);
                    constants.OffsetUv[1] = filter->Offset.y / std::max(mLayoutHeight, 1.f);
                    constants.ShadowColor[0] = static_cast<float>(filter->Color.red) / 255.f;
                    constants.ShadowColor[1] = static_cast<float>(filter->Color.green) / 255.f;
                    constants.ShadowColor[2] = static_cast<float>(filter->Color.blue) / 255.f;
                    constants.ShadowColor[3] = static_cast<float>(filter->Color.alpha) / 255.f;
                    break;
                case FilterType::ColorMatrix:
                    constants.Mode = static_cast<uint32_t>(RmlPostprocessMode::ColorMatrix);
                    std::memcpy(constants.ColorMatrix, filter->ColorMatrix.data(), sizeof(constants.ColorMatrix));
                    break;
                case FilterType::MaskImage:
                    constants.Mode = static_cast<uint32_t>(RmlPostprocessMode::MaskImage);
                    break;
                case FilterType::Invalid:
                    break;
                }
                return constants;
            }

            void CompositeLayerContents(
                Rml::LayerHandle sourceHandle,
                Rml::LayerHandle destinationHandle,
                Rml::BlendMode blendMode,
                Rml::Span<const Rml::CompiledFilterHandle> filterHandles)
            {
                nvrhi::ITexture* source = GetLayerColor(sourceHandle, sourceHandle == 0);
                if (!source)
                {
                    ReportRejectedFeature("base-layer filtering requires an explicit backdrop texture");
                    return;
                }

                FrameLayerPool& pool = GetActiveLayerPool();
                nvrhi::ITexture* filteredSource = source;
                if (!filterHandles.empty())
                {
                    ColorTarget& primary = EnsurePostprocessTarget(pool, 0);
                    ColorTarget& secondary = EnsurePostprocessTarget(pool, 1);
                    mCommandList->clearTextureFloat(
                        primary.Color,
                        nvrhi::AllSubresources,
                        nvrhi::Color(0.f, 0.f, 0.f, 0.f));
                    RecordPostprocess(
                        source,
                        nullptr,
                        primary.Framebuffer,
                        MakePostprocessConstants(nullptr),
                        false,
                        false,
                        0);
                    ColorTarget* read = &primary;
                    ColorTarget* write = &secondary;

                    for (Rml::CompiledFilterHandle handle : filterHandles)
                    {
                        if (!handle)
                            continue;
                        const CompiledFilter& filter = *reinterpret_cast<const CompiledFilter*>(handle);
                        if (filter.Type == FilterType::Invalid)
                            continue;
                        mCommandList->clearTextureFloat(
                            write->Color,
                            nvrhi::AllSubresources,
                            nvrhi::Color(0.f, 0.f, 0.f, 0.f));
                        RecordPostprocess(
                            read->Color,
                            filter.Type == FilterType::MaskImage ? filter.MaskTexture.Get() : nullptr,
                            write->Framebuffer,
                            MakePostprocessConstants(&filter),
                            false,
                            false,
                            0);
                        std::swap(read, write);
                        ++mStats.FilterPasses;
                    }
                    filteredSource = read->Color;
                }

                const Rml::LayerHandle originalTarget = mCurrentLayerHandle;
                SwitchActiveTarget(destinationHandle);
                const bool bClipped = mClipMaskEnabled
                    && mClipMaskReference > 0
                    && HasWritableStencilAttachment();
                RecordPostprocess(
                    filteredSource,
                    nullptr,
                    mFramebuffer,
                    MakePostprocessConstants(nullptr),
                    blendMode == Rml::BlendMode::Blend,
                    bClipped,
                    mClipMaskReference);
                SwitchActiveTarget(originalTarget);
            }

            [[nodiscard]] nvrhi::IBindingSet* GetOrCreateDecoratorBindingSet(nvrhi::ITexture* texture)
            {
                if (!texture)
                    texture = mWhiteTexture;
                FrameLayerPool& pool = GetActiveLayerPool();
                for (const DecoratorBindingEntry& entry : pool.DecoratorBindings)
                {
                    if (entry.Texture == texture)
                        return entry.BindingSet;
                }
                nvrhi::BindingSetDesc desc;
                desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(RmlDecoratorConstants)))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture))
                    .addItem(nvrhi::BindingSetItem::Sampler(0, mDefaultSampler));
                nvrhi::BindingSetHandle bindingSet = mDevice->createBindingSet(desc, mDecoratorBindingLayout);
                if (!bindingSet)
                    throw std::runtime_error("NVRHI failed to create an RmlUi decorator binding set");
                pool.DecoratorBindings.push_back(DecoratorBindingEntry{ texture, std::move(bindingSet) });
                return pool.DecoratorBindings.back().BindingSet;
            }

            [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateDecoratorPipeline(PipelineMode mode)
            {
                const nvrhi::FramebufferInfo info = mFramebuffer->getFramebufferInfo();
                for (const DecoratorPipelineEntry& entry : mDecoratorPipelines)
                {
                    if (entry.FramebufferInfo == info && entry.Mode == mode)
                        return entry.Pipeline;
                }
                nvrhi::GraphicsPipelineDesc desc;
                desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                    .setInputLayout(mInputLayout)
                    .setVertexShader(mVertexShader)
                    .setPixelShader(mDecoratorPixelShader)
                    .addBindingLayout(mDecoratorBindingLayout);
                nvrhi::DepthStencilState& depthStencil = desc.renderState.depthStencilState;
                depthStencil.disableDepthTest().disableDepthWrite();
                desc.renderState.rasterState.setCullNone().enableScissor().enableDepthClip();
                if (mode == PipelineMode::Normal || mode == PipelineMode::NormalClipped)
                {
                    desc.renderState.blendState.targets[0]
                        .enableBlend()
                        .setSrcBlend(nvrhi::BlendFactor::One)
                        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                        .setBlendOp(nvrhi::BlendOp::Add)
                        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                        .setBlendOpAlpha(nvrhi::BlendOp::Add);
                }
                else
                {
                    desc.renderState.blendState.targets[0]
                        .disableBlend()
                        .setColorWriteMask(static_cast<nvrhi::ColorMask>(0));
                }
                if (mode != PipelineMode::Normal)
                {
                    nvrhi::DepthStencilState::StencilOpDesc stencil;
                    stencil.setStencilFunc(
                        mode == PipelineMode::NormalClipped || mode == PipelineMode::ClipMaskIncrement
                            ? nvrhi::ComparisonFunc::Equal
                            : nvrhi::ComparisonFunc::Always);
                    if (mode == PipelineMode::ClipMaskReplace)
                        stencil.setPassOp(nvrhi::StencilOp::Replace);
                    else if (mode == PipelineMode::ClipMaskIncrement)
                        stencil.setPassOp(nvrhi::StencilOp::IncrementAndClamp);
                    depthStencil.enableStencil()
                        .setStencilReadMask(0xff)
                        .setStencilWriteMask(mode == PipelineMode::NormalClipped ? 0 : 0xff)
                        .setDynamicStencilRef(true)
                        .setFrontFaceStencil(stencil)
                        .setBackFaceStencil(stencil);
                }
                nvrhi::GraphicsPipelineHandle pipeline = mDevice->createGraphicsPipeline(desc, info);
                if (!pipeline)
                    throw std::runtime_error("NVRHI failed to create an RmlUi decorator pipeline");
                mDecoratorPipelines.push_back(DecoratorPipelineEntry{ info, mode, std::move(pipeline) });
                mStats.CachedPipelines = static_cast<uint32_t>(
                    mPipelines.size() + mPostprocessPipelines.size() + mDecoratorPipelines.size()
                    + mStencilInitializePipelines.size());
                return mDecoratorPipelines.back().Pipeline;
            }

            void RenderDecorator(
                const CompiledDecorator& decorator,
                Rml::CompiledGeometryHandle geometryHandle,
                Rml::Vector2f translation,
                Rml::TextureHandle textureHandle)
            {
                GeometrySlot* geometry = ResolveGeometry(geometryHandle);
                if (!geometry)
                {
                    ++mStats.InvalidGeometryHandles;
                    return;
                }
                TextureSlot* texture = textureHandle ? ResolveTexture(textureHandle) : nullptr;
                if (textureHandle && !texture)
                    ++mStats.InvalidTextureHandles;
                if (texture)
                    UploadTexture(*texture);
                UploadGeometry(*geometry);

                const PixelRectangle scissor = GetPixelScissor();
                if (scissor.Right <= scissor.Left || scissor.Bottom <= scissor.Top)
                    return;
                const Rml::Matrix4f projection = Rml::Matrix4f::ProjectOrtho(
                    0.f, mLayoutWidth, mLayoutHeight, 0.f, -10000.f, 10000.f);
                const Rml::Matrix4f transform = projection * mTransform;
                RmlDecoratorConstants constants;
                std::memcpy(constants.Transform, transform.data(), sizeof(constants.Transform));
                constants.Translation[0] = translation.x;
                constants.Translation[1] = translation.y;
                constants.Mode = decorator.Type == DecoratorType::Sheen ? 1u : 2u;
                if (Rml::SystemInterface* system = Rml::GetSystemInterface())
                    constants.Time = static_cast<float>(system->GetElapsedTime());
                constants.Dimensions[0] = decorator.Dimensions.x;
                constants.Dimensions[1] = decorator.Dimensions.y;

                nvrhi::GraphicsState state;
                state.setPipeline(GetOrCreateDecoratorPipeline(mPipelineMode))
                    .setFramebuffer(mFramebuffer)
                    .setViewport(nvrhi::ViewportState()
                        .addViewport(nvrhi::Viewport(mViewportWidth, mViewportHeight))
                        .addScissorRect(nvrhi::Rect(
                            scissor.Left,
                            scissor.Right,
                            scissor.Top,
                            scissor.Bottom)))
                    .addBindingSet(GetOrCreateDecoratorBindingSet(texture ? texture->Texture.Get() : nullptr))
                    .addVertexBuffer(nvrhi::VertexBufferBinding()
                        .setBuffer(geometry->VertexBuffer)
                        .setSlot(0)
                        .setOffset(0))
                    .setIndexBuffer(nvrhi::IndexBufferBinding()
                        .setBuffer(geometry->IndexBuffer)
                        .setFormat(nvrhi::Format::R32_UINT)
                        .setOffset(0));
                if (mPipelineMode != PipelineMode::Normal)
                    state.setDynamicStencilRefValue(mPipelineStencilReference);
                mCommandList->setGraphicsState(state);
                mCommandList->setPushConstants(&constants, sizeof(constants));
                mCommandList->drawIndexed(nvrhi::DrawArguments().setVertexCount(geometry->IndexCount));
                ++mStats.DrawCalls;
                mStats.RenderedTriangles += geometry->IndexCount / 3u;
                ++mStats.CustomShaderDraws;
            }

            void CollectRetiredAuxiliaryResources(uint32_t frameSlot)
            {
                auto it = mRetiredAuxiliaryResources.begin();
                while (it != mRetiredAuxiliaryResources.end())
                {
                    it->PendingFrameSlots[frameSlot] = 0;
                    if (std::any_of(
                        it->PendingFrameSlots.begin(),
                        it->PendingFrameSlots.end(),
                        [](uint8_t pending) { return pending != 0; }))
                    {
                        ++it;
                    }
                    else
                    {
                        it = mRetiredAuxiliaryResources.erase(it);
                    }
                }
                UpdateDeferredReleaseStats();
            }

            void UpdateDeferredReleaseStats() noexcept
            {
                mStats.DeferredReleases = static_cast<uint32_t>(
                    mRetiredResources.size() + mRetiredAuxiliaryResources.size());
            }

            void MarkPendingUploadFrame()
            {
                if (!mPendingUploadFrame)
                {
                    mPendingUploadFrame = true;
                    mPendingUploadFrameSlot = mActiveFrameSlot;
                }
                else if (mPendingUploadFrameSlot != mActiveFrameSlot)
                {
                    throw std::logic_error("NVRHI RmlUi uploads span multiple unreported frame slots");
                }
            }

            void ClearPendingUploads() noexcept
            {
                mPendingGeometryUploads.clear();
                mPendingTextureUploads.clear();
                mPendingGpuGeneratedTextures.clear();
                mPendingUploadFrame = false;
                mPendingUploadFrameSlot = 0;
            }

            void ReportRejectedFeature(std::string feature)
            {
                if (mReportedUnsupportedFeatures.emplace(feature).second)
                    HE_CORE_ERROR("NVRHI RmlUi request rejected: {0}.", feature);
                ++mStats.UnsupportedFeatureRequests;
            }

            [[nodiscard]] nvrhi::Format GetDepthAttachmentFormat() const noexcept
            {
                if (!mFramebuffer)
                    return nvrhi::Format::UNKNOWN;

                const nvrhi::FramebufferAttachment& attachment = mFramebuffer->getDesc().depthAttachment;
                if (!attachment.valid())
                    return nvrhi::Format::UNKNOWN;
                if (attachment.format != nvrhi::Format::UNKNOWN)
                    return attachment.format;
                return attachment.texture->getDesc().format;
            }

            [[nodiscard]] bool HasWritableStencilAttachment() const noexcept
            {
                if (!mFramebuffer)
                    return false;
                const nvrhi::FramebufferAttachment& attachment = mFramebuffer->getDesc().depthAttachment;
                if (!attachment.valid() || attachment.isReadOnly)
                    return false;
                return nvrhi::getFormatInfo(GetDepthAttachmentFormat()).hasStencil;
            }

            void ReportClipMaskTargetError(std::string_view operation)
            {
                const nvrhi::FramebufferAttachment& attachment = mFramebuffer->getDesc().depthAttachment;
                const nvrhi::Format format = GetDepthAttachmentFormat();
                const char* formatName = nvrhi::getFormatInfo(format).name;
                const std::string key = "clip-mask target requires writable stencil";
                if (mReportedUnsupportedFeatures.emplace(key).second)
                {
                    HE_CORE_ERROR(
                        "NVRHI RmlUi {0} requires a writable stencil attachment, but the current framebuffer "
                        "has attachment={1}, format={2}, readOnly={3}. Provide D24S8 or another stencil format.",
                        operation,
                        attachment.valid(),
                        formatName ? formatName : "UNKNOWN",
                        attachment.valid() && attachment.isReadOnly);
                }
                ++mStats.UnsupportedFeatureRequests;
            }

            void ReportClipMaskSequenceError(std::string_view message)
            {
                if (mReportedUnsupportedFeatures.emplace(message).second)
                    HE_CORE_ERROR("NVRHI RmlUi clip-mask request rejected: {0}.", message);
                ++mStats.UnsupportedFeatureRequests;
            }

            void ClearClipMaskStencil(uint8_t value)
            {
                const nvrhi::FramebufferAttachment& attachment = mFramebuffer->getDesc().depthAttachment;
                mCommandList->clearDepthStencilTexture(
                    attachment.texture,
                    attachment.subresources,
                    false,
                    1.f,
                    true,
                    value);
            }

            [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreateStencilInitializePipeline()
            {
                const nvrhi::FramebufferInfo info = mFramebuffer->getFramebufferInfo();
                for (const StencilInitializePipelineEntry& entry : mStencilInitializePipelines)
                {
                    if (entry.FramebufferInfo == info)
                        return entry.Pipeline;
                }

                nvrhi::GraphicsPipelineDesc desc;
                desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                    .setVertexShader(mPostprocessVertexShader);
                desc.renderState.rasterState.setCullNone().enableScissor().enableDepthClip();
                for (size_t targetIndex = 0; targetIndex < info.colorFormats.size(); ++targetIndex)
                {
                    desc.renderState.blendState.targets[targetIndex]
                        .disableBlend()
                        .setColorWriteMask(static_cast<nvrhi::ColorMask>(0));
                }

                nvrhi::DepthStencilState::StencilOpDesc stencil;
                stencil.setStencilFunc(nvrhi::ComparisonFunc::Always)
                    .setPassOp(nvrhi::StencilOp::Replace);
                desc.renderState.depthStencilState
                    .disableDepthTest()
                    .disableDepthWrite()
                    .enableStencil()
                    .setStencilReadMask(0xff)
                    .setStencilWriteMask(0xff)
                    .setDynamicStencilRef(true)
                    .setFrontFaceStencil(stencil)
                    .setBackFaceStencil(stencil);

                nvrhi::GraphicsPipelineHandle pipeline = mDevice->createGraphicsPipeline(desc, info);
                if (!pipeline)
                {
                    throw std::runtime_error(
                        "NVRHI failed to create the RmlUi fullscreen stencil initialization pipeline");
                }
                mStencilInitializePipelines.push_back(StencilInitializePipelineEntry{
                    .FramebufferInfo = info,
                    .Pipeline = pipeline,
                });
                mStats.CachedPipelines = static_cast<uint32_t>(
                    mPipelines.size() + mPostprocessPipelines.size() + mDecoratorPipelines.size()
                    + mStencilInitializePipelines.size());
                return pipeline;
            }

            void FillClipMaskStencil(uint8_t value)
            {
                const nvrhi::FramebufferInfoEx& info = mFramebuffer->getFramebufferInfo();
                const float width = static_cast<float>(std::max(info.width, 1u));
                const float height = static_cast<float>(std::max(info.height, 1u));
                nvrhi::GraphicsState state;
                state.setPipeline(GetOrCreateStencilInitializePipeline())
                    .setFramebuffer(mFramebuffer)
                    .setViewport(nvrhi::ViewportState().addViewportAndScissorRect(
                        nvrhi::Viewport(width, height)))
                    .setDynamicStencilRefValue(value);
                mCommandList->setGraphicsState(state);
                mCommandList->draw(nvrhi::DrawArguments().setVertexCount(3));
                ++mStats.DrawCalls;
                ++mStats.RenderedTriangles;
            }

            void InitializeClipMaskStencil(uint8_t value)
            {
                const nvrhi::FramebufferAttachment& attachment = mFramebuffer->getDesc().depthAttachment;
                const nvrhi::TextureDesc& textureDesc = attachment.texture->getDesc();
                if (!textureDesc.useClearValue)
                {
                    // Without an optimized clear value, writing the entire stencil
                    // plane avoids D3D12 clear-value mismatch warnings altogether.
                    FillClipMaskStencil(value);
                    return;
                }

                // Match NVRHI's DX12 resource creation conversion (UINT8 cast)
                // exactly, including for a caller-provided non-integral value.
                const uint8_t optimizedValue = static_cast<uint8_t>(std::clamp(
                    FiniteOr(textureDesc.clearValue.g, 0.f),
                    0.f,
                    255.f));
                ClearClipMaskStencil(optimizedValue);
                if (optimizedValue != value)
                    FillClipMaskStencil(value);
            }

            void CreateImmutableState()
            {
                nvrhi::BindingLayoutDesc layoutDesc;
                layoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                    .setRegisterSpaceAndDescriptorSet(3)
                    .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RmlProjectionConstants)))
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::Sampler(0));
                mBindingLayout = mDevice->createBindingLayout(layoutDesc);

                nvrhi::BindingLayoutDesc postprocessLayoutDesc;
                postprocessLayoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                    .setRegisterSpaceAndDescriptorSet(3)
                    .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RmlPostprocessConstants)))
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                    .addItem(nvrhi::BindingLayoutItem::Sampler(0));
                mPostprocessBindingLayout = mDevice->createBindingLayout(postprocessLayoutDesc);

                nvrhi::BindingLayoutDesc decoratorLayoutDesc;
                decoratorLayoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                    .setRegisterSpaceAndDescriptorSet(3)
                    .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RmlDecoratorConstants)))
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::Sampler(0));
                mDecoratorBindingLayout = mDevice->createBindingLayout(decoratorLayoutDesc);

                nvrhi::SamplerDesc samplerDesc;
                samplerDesc.setAllFilters(true)
                    .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
                mDefaultSampler = mDevice->createSampler(samplerDesc);

                const std::array<nvrhi::VertexAttributeDesc, 3> attributes = {
                    nvrhi::VertexAttributeDesc()
                        .setName("POSITION")
                        .setFormat(nvrhi::Format::RG32_FLOAT)
                        .setBufferIndex(0)
                        .setOffset(static_cast<uint32_t>(offsetof(Rml::Vertex, position)))
                        .setElementStride(sizeof(Rml::Vertex)),
                    nvrhi::VertexAttributeDesc()
                        .setName("COLOR")
                        .setFormat(nvrhi::Format::RGBA8_UNORM)
                        .setBufferIndex(0)
                        .setOffset(static_cast<uint32_t>(offsetof(Rml::Vertex, colour)))
                        .setElementStride(sizeof(Rml::Vertex)),
                    nvrhi::VertexAttributeDesc()
                        .setName("TEXCOORD")
                        .setFormat(nvrhi::Format::RG32_FLOAT)
                        .setBufferIndex(0)
                        .setOffset(static_cast<uint32_t>(offsetof(Rml::Vertex, tex_coord)))
                        .setElementStride(sizeof(Rml::Vertex)),
                };
                mInputLayout = mDevice->createInputLayout(
                    attributes.data(), static_cast<uint32_t>(attributes.size()), mVertexShader);

                nvrhi::TextureDesc whiteDesc;
                whiteDesc.setWidth(1)
                    .setHeight(1)
                    .setFormat(nvrhi::Format::RGBA8_UNORM)
                    .setDimension(nvrhi::TextureDimension::Texture2D)
                    .setDebugName("RmlUi White Texture")
                    .enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
                mWhiteTexture = mDevice->createTexture(whiteDesc);
                if (mWhiteTexture)
                    mWhiteBindingSet = CreateTextureBindingSet(mWhiteTexture);

                if (!mBindingLayout || !mPostprocessBindingLayout || !mDecoratorBindingLayout
                    || !mDefaultSampler || !mInputLayout
                    || !mWhiteTexture || !mWhiteBindingSet)
                {
                    throw std::runtime_error("NVRHI failed to create immutable RmlUi renderer state");
                }
            }

            [[nodiscard]] nvrhi::BindingSetHandle CreateTextureBindingSet(
                const nvrhi::TextureHandle& texture) const
            {
                nvrhi::BindingSetDesc bindingDesc;
                bindingDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(RmlProjectionConstants)))
                    .addItem(nvrhi::BindingSetItem::Texture_SRV(
                        0, texture, nvrhi::Format::RGBA8_UNORM, nvrhi::AllSubresources))
                    .addItem(nvrhi::BindingSetItem::Sampler(0, mDefaultSampler));
                return mDevice->createBindingSet(bindingDesc, mBindingLayout);
            }

            [[nodiscard]] nvrhi::IGraphicsPipeline* GetOrCreatePipeline(PipelineMode mode)
            {
                const nvrhi::FramebufferInfo info = mFramebuffer->getFramebufferInfo();
                for (const PipelineEntry& entry : mPipelines)
                {
                    if (entry.FramebufferInfo == info && entry.Mode == mode)
                        return entry.Pipeline;
                }

                nvrhi::GraphicsPipelineDesc desc;
                desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
                    .setInputLayout(mInputLayout)
                    .setVertexShader(mVertexShader)
                    .setPixelShader(mPixelShader)
                    .addBindingLayout(mBindingLayout);
                nvrhi::DepthStencilState& depthStencil = desc.renderState.depthStencilState;
                depthStencil.disableDepthTest().disableDepthWrite();
                desc.renderState.rasterState.setCullNone().enableScissor().enableDepthClip();
                if (mode == PipelineMode::Normal || mode == PipelineMode::NormalClipped)
                {
                    desc.renderState.blendState.targets[0]
                        .enableBlend()
                        .setSrcBlend(nvrhi::BlendFactor::One)
                        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                        .setBlendOp(nvrhi::BlendOp::Add)
                        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
                        .setBlendOpAlpha(nvrhi::BlendOp::Add);
                }
                else
                {
                    for (size_t targetIndex = 0; targetIndex < info.colorFormats.size(); ++targetIndex)
                    {
                        desc.renderState.blendState.targets[targetIndex]
                            .disableBlend()
                            .setColorWriteMask(static_cast<nvrhi::ColorMask>(0));
                    }
                }

                if (mode != PipelineMode::Normal)
                {
                    nvrhi::DepthStencilState::StencilOpDesc stencil;
                    stencil.setStencilFunc(
                        mode == PipelineMode::NormalClipped || mode == PipelineMode::ClipMaskIncrement
                            ? nvrhi::ComparisonFunc::Equal
                            : nvrhi::ComparisonFunc::Always);
                    if (mode == PipelineMode::ClipMaskReplace)
                        stencil.setPassOp(nvrhi::StencilOp::Replace);
                    else if (mode == PipelineMode::ClipMaskIncrement)
                        stencil.setPassOp(nvrhi::StencilOp::IncrementAndClamp);

                    depthStencil.enableStencil()
                        .setStencilReadMask(0xff)
                        .setStencilWriteMask(mode == PipelineMode::NormalClipped ? 0 : 0xff)
                        .setDynamicStencilRef(true)
                        .setFrontFaceStencil(stencil)
                        .setBackFaceStencil(stencil);
                }

                nvrhi::GraphicsPipelineHandle pipeline = mDevice->createGraphicsPipeline(desc, info);
                if (!pipeline)
                    throw std::runtime_error("NVRHI failed to create the RmlUi graphics pipeline");
                mPipelines.push_back(PipelineEntry{
                    .FramebufferInfo = info,
                    .Mode = mode,
                    .Pipeline = pipeline,
                });
                mStats.CachedPipelines = static_cast<uint32_t>(
                    mPipelines.size() + mPostprocessPipelines.size() + mDecoratorPipelines.size()
                    + mStencilInitializePipelines.size());
                return pipeline;
            }

            uint32_t AllocateGeometrySlot()
            {
                uint32_t index = 0;
                if (!mFreeGeometrySlots.empty())
                {
                    index = mFreeGeometrySlots.back();
                    mFreeGeometrySlots.pop_back();
                    ++mStats.GeometrySlotsReused;
                }
                else
                {
                    if (mGeometrySlots.size() >= std::numeric_limits<uint32_t>::max())
                        throw std::overflow_error("RmlUi geometry registry exhausted");
                    index = static_cast<uint32_t>(mGeometrySlots.size());
                    mGeometrySlots.emplace_back();
                }
                mGeometrySlots[index].bOccupied = true;
                return index;
            }

            uint32_t AllocateTextureSlot()
            {
                uint32_t index = 0;
                if (!mFreeTextureSlots.empty())
                {
                    index = mFreeTextureSlots.back();
                    mFreeTextureSlots.pop_back();
                    ++mStats.TextureSlotsReused;
                }
                else
                {
                    if (mTextureSlots.size() >= std::numeric_limits<uint32_t>::max())
                        throw std::overflow_error("RmlUi texture registry exhausted");
                    index = static_cast<uint32_t>(mTextureSlots.size());
                    mTextureSlots.emplace_back();
                }
                mTextureSlots[index].bOccupied = true;
                return index;
            }

            [[nodiscard]] GeometrySlot* ResolveGeometry(Rml::CompiledGeometryHandle handle)
            {
                uint32_t index = 0;
                uint32_t generation = 0;
                UnpackRmlHandle(handle, index, generation);
                if (index == 0 || generation == 0 || index >= mGeometrySlots.size())
                    return nullptr;
                GeometrySlot& slot = mGeometrySlots[index];
                return slot.bOccupied && slot.Generation == generation ? &slot : nullptr;
            }

            [[nodiscard]] TextureSlot* ResolveTexture(Rml::TextureHandle handle)
            {
                uint32_t index = 0;
                uint32_t generation = 0;
                UnpackRmlHandle(handle, index, generation);
                if (index == 0 || generation == 0 || index >= mTextureSlots.size())
                    return nullptr;
                TextureSlot& slot = mTextureSlots[index];
                return slot.bOccupied && slot.Generation == generation ? &slot : nullptr;
            }

            void UploadGeometry(GeometrySlot& geometry)
            {
                if (geometry.bUploaded)
                    return;
                mCommandList->writeBuffer(
                    geometry.VertexBuffer,
                    geometry.Vertices.data(),
                    geometry.Vertices.size() * sizeof(Rml::Vertex));
                mCommandList->writeBuffer(
                    geometry.IndexBuffer,
                    geometry.Indices.data(),
                    geometry.Indices.size() * sizeof(uint32_t));
                geometry.bUploaded = true;
                const uint32_t index = static_cast<uint32_t>(&geometry - mGeometrySlots.data());
                mPendingGeometryUploads.push_back(PendingUpload{ index, geometry.Generation });
                MarkPendingUploadFrame();
                ++mStats.GeometryUploads;
            }

            void UploadTexture(TextureSlot& texture)
            {
                if (texture.bUploaded)
                    return;
                mCommandList->writeTexture(
                    texture.Texture,
                    0,
                    0,
                    texture.Pixels.data(),
                    static_cast<size_t>(texture.Width) * 4u);
                texture.bUploaded = true;
                const uint32_t index = static_cast<uint32_t>(&texture - mTextureSlots.data());
                mPendingTextureUploads.push_back(PendingUpload{ index, texture.Generation });
                MarkPendingUploadFrame();
                ++mStats.TextureUploads;
            }

            void CollectRetiredResources(uint32_t frameSlot)
            {
                auto it = mRetiredResources.begin();
                while (it != mRetiredResources.end())
                {
                    it->PendingFrameSlots[frameSlot] = 0;
                    if (std::any_of(
                        it->PendingFrameSlots.begin(),
                        it->PendingFrameSlots.end(),
                        [](uint8_t pending) { return pending != 0; }))
                    {
                        ++it;
                        continue;
                    }

                    if (it->Type == RetiredResourceType::Geometry)
                    {
                        GeometrySlot& slot = mGeometrySlots[it->Index];
                        slot.Generation = NextGeneration(slot.Generation);
                        mFreeGeometrySlots.push_back(it->Index);
                    }
                    else
                    {
                        TextureSlot& slot = mTextureSlots[it->Index];
                        slot.Generation = NextGeneration(slot.Generation);
                        mFreeTextureSlots.push_back(it->Index);
                    }
                    it = mRetiredResources.erase(it);
                }
                UpdateDeferredReleaseStats();
            }

            void RequireActiveFrame(const char* operation) const
            {
                if (!mCommandList || !mFramebuffer)
                    throw std::logic_error(std::string("NVRHI RmlUi ") + operation + " called outside BeginFrame/EndFrame");
            }

        private:
            nvrhi::DeviceHandle mDevice;
            ShaderManager mShaderManager;
            ShaderArtifactResolver mShaderArtifacts;
            uint32_t mFramesInFlight = 0;
            std::string mDebugName;
            nvrhi::ShaderHandle mVertexShader;
            nvrhi::ShaderHandle mPixelShader;
            nvrhi::ShaderHandle mPostprocessVertexShader;
            nvrhi::ShaderHandle mPostprocessPixelShader;
            nvrhi::ShaderHandle mDecoratorPixelShader;
            nvrhi::InputLayoutHandle mInputLayout;
            nvrhi::BindingLayoutHandle mBindingLayout;
            nvrhi::BindingLayoutHandle mPostprocessBindingLayout;
            nvrhi::BindingLayoutHandle mDecoratorBindingLayout;
            nvrhi::SamplerHandle mDefaultSampler;
            nvrhi::TextureHandle mWhiteTexture;
            nvrhi::BindingSetHandle mWhiteBindingSet;
            std::vector<GeometrySlot> mGeometrySlots;
            std::vector<TextureSlot> mTextureSlots;
            std::vector<uint32_t> mFreeGeometrySlots;
            std::vector<uint32_t> mFreeTextureSlots;
            std::vector<RetiredResource> mRetiredResources;
            std::vector<RetiredAuxiliaryResource> mRetiredAuxiliaryResources;
            std::vector<PipelineEntry> mPipelines;
            std::vector<PostprocessPipelineEntry> mPostprocessPipelines;
            std::vector<DecoratorPipelineEntry> mDecoratorPipelines;
            std::vector<StencilInitializePipelineEntry> mStencilInitializePipelines;
            std::vector<FrameLayerPool> mFrameLayerPools;
            std::vector<uint64_t> mLastCompletedSubmissions;
            std::vector<uint64_t> mLastSubmittedSubmissions;
            std::vector<PendingUpload> mPendingGeometryUploads;
            std::vector<PendingUpload> mPendingTextureUploads;
            std::vector<PendingUpload> mPendingGpuGeneratedTextures;
            nvrhi::ICommandList* mCommandList = nullptr;
            nvrhi::IFramebuffer* mBaseFramebuffer = nullptr;
            nvrhi::IFramebuffer* mFramebuffer = nullptr;
            nvrhi::ITexture* mBackdropTexture = nullptr;
            Rml::Matrix4f mTransform = Rml::Matrix4f::Identity();
            Rml::Rectanglei mScissor = {};
            NvrhiRmlRenderBackendStats mStats;
            uint32_t mActiveFrameSlot = 0;
            uint32_t mPendingUploadFrameSlot = 0;
            float mViewportWidth = 1.f;
            float mViewportHeight = 1.f;
            float mLayoutWidth = 1.f;
            float mLayoutHeight = 1.f;
            bool mScissorEnabled = false;
            bool mWhiteTextureUploaded = false;
            bool mWhiteTextureUploadedThisFrame = false;
            bool mShutdown = false;
            bool mPendingUploadFrame = false;
            bool mClipMaskEnabled = false;
            bool mBaseClipMaskEnabled = false;
            PipelineMode mPipelineMode = PipelineMode::Normal;
            uint8_t mPipelineStencilReference = 0;
            uint8_t mClipMaskReference = 0;
            uint8_t mBaseClipMaskReference = 0;
            Rml::LayerHandle mCurrentLayerHandle = {};
            std::unordered_set<std::string> mReportedUnsupportedFeatures;
        };
    }

    Scope<INvrhiRmlRenderBackend> CreateNvrhiRmlRenderBackend(
        const NvrhiRmlRenderBackendDesc& desc)
    {
        return CreateScope<NvrhiRmlRenderBackend>(desc);
    }
}
