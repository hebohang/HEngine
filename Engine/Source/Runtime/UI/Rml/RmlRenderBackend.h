#pragma once

#include "Runtime/Core/Base/Base.h"

#include <cstdint>
#include <filesystem>

namespace Rml
{
    class RenderInterface;
}

namespace nvrhi
{
    class ICommandList;
    class IDevice;
    class IFramebuffer;
    class ITexture;
}

namespace HEngine
{
    struct RmlRenderCapabilities
    {
        bool Scissor = false;
        bool Transform = false;
        bool ClipMask = false;
        bool ClipMaskRequiresStencilAttachment = false;
        bool Layers = false;
        bool Filters = false;
        bool MaskImage = false;
        // CustomShaders only advertises the engine-owned decorator whitelist;
        // it never permits markup to provide or compile shader source.
        bool CustomShaders = false;
    };

    class IRmlRenderBackend
    {
    public:
        virtual ~IRmlRenderBackend() = default;

        [[nodiscard]] virtual Rml::RenderInterface* GetRenderInterface() = 0;
        [[nodiscard]] virtual RmlRenderCapabilities GetCapabilities() const noexcept = 0;
        virtual void SetViewportSize(
            float renderWidth,
            float renderHeight,
            float layoutWidth = 0.0f,
            float layoutHeight = 0.0f) = 0;
    };

    struct NvrhiRmlRenderBackendDesc
    {
        nvrhi::IDevice* Device = nullptr;
        uint32_t FramesInFlight = 3;
        std::filesystem::path VertexShaderArtifact;
        std::filesystem::path PixelShaderArtifact;
        const char* DebugName = "HEngine RmlUi NVRHI Renderer";
    };

    struct NvrhiRmlRenderBackendStats
    {
        uint64_t DrawCalls = 0;
        uint64_t RenderedTriangles = 0;
        uint64_t GeometryUploads = 0;
        uint64_t TextureUploads = 0;
        uint64_t GeometrySlotsReused = 0;
        uint64_t GeometryBuffersReused = 0;
		// Filled only when HE_RML_RECORD_TIMINGS=1; cumulative owner-thread CPU time.
		uint64_t GeometryCompileNanoseconds = 0;
		uint64_t GeometryBufferCreateNanoseconds = 0;
		uint64_t GeometryBufferWriteNanoseconds = 0;
		uint64_t GeometryDrawNanoseconds = 0;
		uint64_t GeometryBuffersCreated = 0;
		uint64_t GeometryBufferBytesCreated = 0;
		uint64_t GeometryBuffersCreatedLe512 = 0;
		uint64_t GeometryBuffersCreatedLe4096 = 0;
		uint64_t GeometryArenaAllocations = 0;
        uint64_t TextureSlotsReused = 0;
        uint64_t InvalidGeometryHandles = 0;
        uint64_t InvalidTextureHandles = 0;
        uint32_t LiveGeometries = 0;
        uint32_t LiveTextures = 0;
        uint32_t DeferredReleases = 0;
        uint32_t CachedPipelines = 0;
        uint32_t UnsupportedFeatureRequests = 0;
        uint32_t LayerTargetsAllocated = 0;
        uint32_t PeakLayerDepth = 0;
        uint64_t LayerComposites = 0;
        uint64_t FilterPasses = 0;
        uint64_t CustomShaderDraws = 0;
    };

    // RmlUi calls the RenderInterface synchronously between BeginFrame and EndFrame.
    // The frame slot must already be GPU-safe when BeginFrame is called. Clip-mask rendering
    // additionally requires a writable stencil attachment on the supplied framebuffer.
    class INvrhiRmlRenderBackend : public IRmlRenderBackend
    {
    public:
        virtual void BeginFrame(
            nvrhi::ICommandList* commandList,
            nvrhi::IFramebuffer* framebuffer,
            uint32_t frameSlot,
            uint64_t completedSubmission,
            nvrhi::ITexture* backdropTexture) = 0;
        virtual void EndFrame() = 0;
        // Upload staging is retained until the owner confirms that the command
        // list was submitted. AbortFrame makes first-use uploads recordable
        // again instead of exposing uninitialized GPU resources next frame.
        virtual void NotifyFrameSubmitted(uint32_t frameSlot, uint64_t submission) = 0;
        virtual void AbortFrame(uint32_t frameSlot) noexcept = 0;

        [[nodiscard]] virtual const NvrhiRmlRenderBackendStats& GetStats() const noexcept = 0;
        virtual void ResetStats() noexcept = 0;
        virtual void Shutdown() = 0;
    };

    [[nodiscard]] Scope<INvrhiRmlRenderBackend> CreateNvrhiRmlRenderBackend(
        const NvrhiRmlRenderBackendDesc& desc);
}
