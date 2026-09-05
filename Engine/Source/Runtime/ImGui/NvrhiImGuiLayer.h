#pragma once

#include "Runtime/Graphics/Gui/NvrhiImGuiRenderer.h"
#include "Runtime/ImGui/ImGuiLayer.h"

#include <memory>

namespace HEngine
{
    class Window;

    struct NvrhiImGuiLayerDesc
    {
        // Non-owning. The Window must outlive the layer and its GLFW callbacks.
        Window* MainWindow = nullptr;
        NvrhiImGuiRendererDesc Renderer;
        bool bInstallGlfwCallbacks = true;
#if defined(WITH_EDITOR)
        bool bEnableDocking = true;
#else
        bool bEnableDocking = false;
#endif
    };

    // Owns the Dear ImGui context and GLFW platform backend, while delegating
    // all GPU work to the pure-NVRHI renderer. Secondary platform windows are
    // intentionally disabled until they have NVRHI-owned swap chains.
    class NvrhiImGuiLayer final : public ImGuiLayer
    {
    public:
        NvrhiImGuiLayer();
        ~NvrhiImGuiLayer() override;

        NvrhiImGuiLayer(const NvrhiImGuiLayer&) = delete;
        NvrhiImGuiLayer& operator=(const NvrhiImGuiLayer&) = delete;

        void Initialize(const NvrhiImGuiLayerDesc& desc);
        void Shutdown();

        void OnAttach() override;
        void OnDetach() override;
        void Begin() override;
        void End() override;

        void BeginCpuFrame();
        // frameSlot must be a GPU-safe slot acquired from RenderDevice::BeginFrame.
        void RecordGpuFrame(
            nvrhi::ICommandList* commandList,
            nvrhi::IFramebuffer* framebuffer,
            uint32_t frameSlot);
        void CancelCpuFrame() noexcept;

        [[nodiscard]] ImGuiTextureHandle RegisterTexture(
            const nvrhi::TextureHandle& texture,
            const nvrhi::SamplerHandle& sampler = nullptr,
            nvrhi::Format viewFormat = nvrhi::Format::UNKNOWN,
            nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources);
        void UnregisterTexture(ImGuiTextureHandle handle, uint32_t retireFrameSlot);
        [[nodiscard]] bool IsTextureValid(ImGuiTextureHandle handle) const;

        [[nodiscard]] static ImTextureID ToImTextureID(ImGuiTextureHandle handle) noexcept;
        [[nodiscard]] static ImGuiTextureHandle FromImTextureID(ImTextureID textureId) noexcept;

        [[nodiscard]] ImGuiTextureHandle GetFontTexture() const noexcept;
        [[nodiscard]] uint32_t GetRegisteredTextureCount() const noexcept;
        [[nodiscard]] const NvrhiImGuiRendererStats& GetRendererStats() const;
        void ResetRendererStats() noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept;
        [[nodiscard]] bool IsCpuFrameActive() const noexcept;
        [[nodiscard]] bool AreSecondaryViewportsEnabled() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
