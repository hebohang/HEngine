#pragma once

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace HEngine
{
    enum class RuntimeUiRenderMode : uint8_t
    {
        Direct,
        Offscreen,
    };

    enum class RuntimeUiTransitionEffect : uint32_t
    {
        None = 0,
        DiagonalStickerWipe = 1,
        StarPetalReveal = 2,
        HalftoneDotReveal = 3,
    };

    // Progress is a visibility amount: zero hides the UI layer and one reveals
    // it completely. Callers can drive the same effect backwards for exits.
    struct RuntimeUiTransitionParameters
    {
        RuntimeUiTransitionEffect Effect = RuntimeUiTransitionEffect::None;
        float Progress = 1.0f;
        float EdgeSoftnessPixels = 2.0f;
        float HalftoneCellPixels = 24.0f;
        float CenterX = 0.5f;
        float CenterY = 0.5f;
        bool bReduceMotion = false;
        bool bReduceFlash = false;
    };

    struct RuntimeUiRenderContext
    {
        nvrhi::ICommandList* CommandList = nullptr;
        nvrhi::IFramebuffer* Framebuffer = nullptr;
        nvrhi::ITexture* ColorTarget = nullptr;
        nvrhi::ITexture* StencilTarget = nullptr;
        nvrhi::ITexture* BackdropTexture = nullptr;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t FrameSlot = 0;
        uint64_t CompletedSubmission = 0;
    };

    // The target is cleared to transparent black and stencil zero before this
    // callback. UI rendering must output display-referred, premultiplied alpha
    // into the RGBA8_UNORM color attachment and may use the D24S8 attachment.
    // Supplied NVRHI pointers are valid only for the synchronous callback.
    using RuntimeUiRenderRecorder = std::function<void(const RuntimeUiRenderContext&)>;

    struct RuntimeUiRenderFrameState
    {
        RuntimeUiRenderRecorder Recorder;
        RuntimeUiTransitionParameters Transition;
        uint32_t FrameSlot = 0;
        uint64_t CompletedSubmission = 0;
    };

    struct RuntimeUiRenderGraphInputs
    {
        // Final LDR render target. A swapchain texture is valid here because it
        // is used only as a render target by UiComposite.
        TextureHandle BackBuffer;

        // Optional sampled LDR scene for backdrop effects. Do not pass a
        // swapchain texture unless it was explicitly created with SRV support;
        // prefer a tone-mapped offscreen image or an explicit scene copy.
        TextureHandle Backdrop;
        uint32_t Width = 0;
        uint32_t Height = 0;
    };

    struct RuntimeUiRenderGraphResult
    {
        RenderGraphPassHandle RuntimeUiRenderPass;
        RenderGraphPassHandle UiTransitionPass;
        RenderGraphPassHandle UiCompositePass;
        TextureHandle UiColor;
        TextureHandle UiStencil;
        TextureHandle TransitionColor;
        TextureHandle CompositedBackBuffer;
        bool bAdded = false;
    };

    struct RuntimeUiRenderGraphPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        std::filesystem::path ShaderCacheDirectory;
    };

    // Adds the high-quality runtime-UI path to an existing LDR RenderGraph:
    // RuntimeUiRender -> UiTransition -> UiComposite. Add it after tone mapping
    // and before offscreen-copy, capture and present passes.
    class RuntimeUiRenderGraphPass final
    {
    public:
        explicit RuntimeUiRenderGraphPass(const RuntimeUiRenderGraphPassDesc& desc);
        ~RuntimeUiRenderGraphPass();

        RuntimeUiRenderGraphPass(const RuntimeUiRenderGraphPass&) = delete;
        RuntimeUiRenderGraphPass& operator=(const RuntimeUiRenderGraphPass&) = delete;

        // Update immediately before graph execution. The graph callbacks read
        // this state synchronously, so changing transition progress does not
        // rebuild the graph.
        // FrameSlot must already be safe to reuse (normally after the owning
        // RenderGraphResourcePool::BeginFrame call); this is also the retirement
        // point for cache entries retained by older graph generations.
        void UpdateFrame(const RuntimeUiRenderFrameState& frame);

        [[nodiscard]] RuntimeUiRenderGraphResult AddToGraph(
            RenderGraph& graph,
            const RuntimeUiRenderGraphInputs& inputs);

        // Call only after the owning queue is idle for resize, teardown, or an
        // explicit full-cache purge. Ordinary RenderGraph::Clear/rebuild paths
        // are handled by generation-aware deferred retirement and must not use
        // this immediate release path while submissions may still be in flight.
        // The call advances the cache generation exposed to diagnostics.
        void ClearResourceCaches();

        [[nodiscard]] size_t GetFramebufferCacheSize() const noexcept;
        [[nodiscard]] size_t GetBindingSetCacheSize() const noexcept;
        [[nodiscard]] size_t GetStaleFramebufferCacheSize() const noexcept;
        [[nodiscard]] size_t GetStaleBindingSetCacheSize() const noexcept;
        [[nodiscard]] uint64_t GetCacheGeneration() const noexcept;
        [[nodiscard]] uint64_t GetGraphGeneration() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
