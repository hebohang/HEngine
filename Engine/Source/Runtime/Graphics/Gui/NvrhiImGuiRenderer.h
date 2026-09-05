#pragma once

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace HEngine
{
    struct ImGuiTextureHandle
    {
        uint32_t Index = 0;
        uint32_t Generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Index != 0 && Generation != 0;
        }

        [[nodiscard]] bool operator==(const ImGuiTextureHandle&) const = default;
    };

    struct NvrhiImGuiRendererDesc
    {
        nvrhi::IDevice* Device = nullptr;
        uint32_t FramesInFlight = 3;
        std::filesystem::path VertexShaderArtifact;
        std::filesystem::path PixelShaderArtifact;
        const char* DebugName = "HEngine ImGui NVRHI Renderer";
    };

    struct NvrhiImGuiRendererStats
    {
        uint64_t DrawCalls = 0;
        uint64_t RenderedVertices = 0;
        uint64_t RenderedIndices = 0;
        uint64_t SkippedInvalidTextures = 0;
    };

    // Pure NVRHI renderer backend for Dear ImGui. The caller owns platform input,
    // ImGui::NewFrame/Render, the command-list lifetime, and framebuffer states.
    class NvrhiImGuiRenderer final
    {
    public:
        explicit NvrhiImGuiRenderer(const NvrhiImGuiRendererDesc& desc);
        ~NvrhiImGuiRenderer();

        NvrhiImGuiRenderer(const NvrhiImGuiRenderer&) = delete;
        NvrhiImGuiRenderer& operator=(const NvrhiImGuiRenderer&) = delete;

        // Processes ImGui 1.92 dynamic atlas requests. Call after the command list
        // is opened and before RenderDrawData for the same frame.
        void NewFrame(nvrhi::ICommandList* commandList, uint32_t frameIndex);
        void RenderDrawData(
            ImDrawData* drawData,
            nvrhi::ICommandList* commandList,
            nvrhi::IFramebuffer* framebuffer,
            uint32_t frameIndex);

        [[nodiscard]] ImGuiTextureHandle RegisterTexture(
            const nvrhi::TextureHandle& texture,
            const nvrhi::SamplerHandle& sampler = nullptr,
            nvrhi::Format viewFormat = nvrhi::Format::UNKNOWN,
            nvrhi::TextureSubresourceSet subresources = nvrhi::AllSubresources);
        // retireFrameIndex must be the frame slot that last referenced the texture.
        // Its NVRHI objects and index are retained until that slot is acquired again.
        void UnregisterTexture(ImGuiTextureHandle handle, uint32_t retireFrameIndex);
        [[nodiscard]] bool IsTextureValid(ImGuiTextureHandle handle) const;

        [[nodiscard]] static ImTextureID ToImTextureID(ImGuiTextureHandle handle) noexcept;
        [[nodiscard]] static ImGuiTextureHandle FromImTextureID(ImTextureID textureId) noexcept;

        [[nodiscard]] ImGuiTextureHandle GetFontTexture() const noexcept;
        [[nodiscard]] uint32_t GetRegisteredTextureCount() const noexcept;
        [[nodiscard]] const NvrhiImGuiRendererStats& GetStats() const noexcept;

        void ResetStats() noexcept;
        void Shutdown();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
