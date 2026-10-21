#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace HEngine
{
    class ShaderManager;
    struct SceneIndirectCompactionPassDesc
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        RenderBackend Backend = RenderBackend::DX12;
        std::string DebugName = "Indirect";
    };
    struct SceneIndirectGroupMapping
    {
        uint32_t GroupIndex = 0;
        uint32_t GroupBase = 0; // first compact argument slot reserved for this group
        bool operator==(const SceneIndirectGroupMapping&) const = default;
    };
    struct SceneIndirectCompactionStats
    {
        uint64_t ResidentBytes = 0;
        uint64_t RetiredBytes = 0;
        uint64_t UploadedBytes = 0; // current PrepareFrame/record cycle
        uint32_t CandidateCount = 0;
        uint32_t GroupCount = 0;
    };
    // Input retains the existing 20-byte indexed arguments. Each enabled
    // command is copied unchanged into its group's disjoint reserved range.
    // Output arguments and 32-bit draw counts share one raw/indirect buffer.
    class SceneIndirectCompactionPass final
    {
    public:
        explicit SceneIndirectCompactionPass(const SceneIndirectCompactionPassDesc& desc);
        ~SceneIndirectCompactionPass();
        [[nodiscard]] bool PrepareFrame(std::span<const SceneIndirectGroupMapping> mapping,
            uint32_t groupCount, uint64_t completedSubmission);
        [[nodiscard]] BufferHandle AddPasses(RenderGraph& graph, BufferHandle inputArguments);
        [[nodiscard]] nvrhi::IBuffer* GetBuffer() const noexcept;
        [[nodiscard]] uint32_t GetArgumentOffset(uint32_t groupBase) const;
        [[nodiscard]] uint32_t GetCountOffset(uint32_t groupIndex) const;
        [[nodiscard]] const SceneIndirectCompactionStats& GetStats() const noexcept;
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void Collect(uint64_t completedSubmission);
        void Clear(uint64_t completedSubmission);
        [[nodiscard]] std::function<void()> CaptureGraphState();
        void ClearGraphResourceCaches() noexcept;
    private:
        struct Impl;
        Scope<Impl> mImpl;
    };
}
