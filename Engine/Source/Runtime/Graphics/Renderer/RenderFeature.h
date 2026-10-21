#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/Renderer/ScenePostProcessPass.h"

#include <functional>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
    enum class RenderFeatureStage : uint8_t { TemporalComposite, SceneOverlay, HdrPostProcess, DisplayPostProcess };
    struct RenderWorld;
    struct RenderCamera;
    struct CloudSettings;
    struct RenderFeatureFrameContext
    {
        const RenderWorld* World = nullptr;
        const RenderCamera* RasterCamera = nullptr;
        const CloudSettings* Clouds = nullptr;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint64_t CompletedSubmission = 0;
        bool bOffline = false;
    };
    enum class RenderFeatureInput : uint32_t
    {
        None = 0, Depth = 1, Normal = 2, MotionVector = 4, OpaqueColor = 8,
        HdrColor = 16, DisplayColor = 32, History = 64
    };
    enum class RenderFeatureAccess : uint8_t { Read, PreserveWrite, ReadWrite, FullOverwrite };

    // Per-instance, per-view color history. Physical storage belongs to the
    // graph pool; instances retire with their graph after GPU completion.
    class RenderFeatureHistory
    {
    public:
        TextureHandle ReadColor(RenderGraph& graph, std::string name, RenderGraphTextureDesc desc);
        void StoreColor(RenderGraph& graph, const std::string& name, TextureHandle source);
        [[nodiscard]] bool IsValid(const std::string& name) const noexcept;
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void Reset() noexcept;
        [[nodiscard]] uint64_t ReadRequests() const noexcept { return mReadRequests; }
    private:
        struct State;
        std::map<std::string, std::shared_ptr<State>> mStates;
        uint64_t mReadRequests = 0;
    };

    struct RenderFeatureDescriptor
    {
        std::string Type;
        uint32_t Version = 1;
        RenderFeatureStage Stage = RenderFeatureStage::HdrPostProcess;
        uint32_t RequiredInputs = 0;
        RenderFeatureAccess ColorAccess = RenderFeatureAccess::ReadWrite;
        bool bOfflineSupported = true;
        bool bMsaaSupported = false;
        bool bResolvedDepthSupported = false;
    };

    // This capability adds the renderer's filtered selected-object draw list.
    // It exposes no renderer implementation, mesh caches, or raw scene mutation.
    struct RenderFeatureBuildContext
    {
        RenderGraph& Graph;
        TextureHandle& HdrColor;
        TextureHandle Depth;
        uint32_t Width = 0;
        uint32_t Height = 0;
        bool bOffline = false;
        bool bMsaa = false;
        bool bDiagnosticView = false;
        std::function<void()> DrawSelectedObjects;
        ScenePostProcessPass* PostProcess = nullptr;
        TextureHandle Normal;
        TextureHandle MotionVector;
        TextureHandle OpaqueColor;
        TextureHandle DisplayColor;
        bool bJittered = false;
        // During FSR SceneOverlay, HDR alpha carries transparent coverage.
        // Full-screen effects must preserve it; geometry adds its opacity.
        bool bTransparencyCoverageInAlpha = false;
        // bJittered is the legacy depth-coordinate flag. TAA-reconstructed
        // color is unjittered while surface guides can remain raster-jittered.
        bool bDepthJittered = false;
        bool bSurfaceInputsJittered = false;
        TextureHandle RasterDepth;
        RenderFeatureHistory* History = nullptr;
        RenderFeatureStage Stage = RenderFeatureStage::HdrPostProcess;
        uint32_t ColorSampleCount = 1;
        uint32_t DepthSampleCount = 1;
        // Display replacements are copied back into the original display target
        // by the plan, preserving the host's UI/present/output ownership.
        [[nodiscard]] TextureHandle& Color() noexcept
        {
            return Stage == RenderFeatureStage::DisplayPostProcess ? DisplayColor : HdrColor;
        }
    };

    // Build only records graph declarations. Persistent allocation/shader
    // preparation belongs to factory creation, outside frame recording. Graph
    // callbacks and feature instances retire together after GPU completion.
    class RenderFeature
    {
    public:
        virtual ~RenderFeature() = default;
        virtual void Build(RenderFeatureBuildContext& context) = 0;
        virtual void PrepareFrame(const RenderFeatureFrameContext&) {}
        virtual void UpdateImportedResources(RenderGraph&) {}
        virtual bool ConsumeGraphRebuildRequest() noexcept { return false; }
        virtual bool RequiresContinuousUpdate() const noexcept { return false; }
        virtual double GetCpuRecordMilliseconds() const noexcept { return 0.0; }
        virtual void NotifyFrameSubmitted(uint64_t) noexcept {}
        virtual void AbortFrame() noexcept {}
        virtual void ResetHistory() noexcept {}
        virtual void Clear(uint64_t) {}
        virtual void Collect(uint64_t) {}
        virtual std::function<void()> CaptureGraphState() { return {}; }
        virtual void ClearGraphResourceCaches() noexcept {}
    };

    struct RenderFeatureCreateContext
    {
        nvrhi::IDevice* Device = nullptr;
        ShaderManager* SharedShaderManager = nullptr;
        std::filesystem::path ShaderArtifactCacheRoot;
        std::filesystem::path AssetRootDirectory;
        std::filesystem::path EngineAssetRootDirectory;
        RenderBackend Backend = RenderBackend::DX12;
        uint32_t FramesInFlight = 3;
    };
    using RenderFeatureFactory = std::function<std::unique_ptr<RenderFeature>(
        const RenderFeatureCreateContext& context, const Json& parameters)>;
    using LegacyRenderFeatureFactory = std::function<std::unique_ptr<RenderFeature>(const Json& parameters)>;
    [[nodiscard]] Json DefaultRenderFeatureConfigurations();
    void RegisterRenderFeature(RenderFeatureDescriptor descriptor, RenderFeatureFactory factory);
    void RegisterRenderFeature(RenderFeatureDescriptor descriptor, LegacyRenderFeatureFactory factory);
    [[nodiscard]] std::vector<RenderFeatureDescriptor> EnumerateRenderFeatures();
    [[nodiscard]] std::vector<std::string> ValidateRenderFeatures(const Json& configurations);
    void ValidateRenderFeatureColorAccess(const RenderGraph& graph, uint32_t firstPass,
        TextureHandle input, TextureHandle output, RenderFeatureAccess access);
    void ValidateRenderFeatureInputs(const RenderFeatureDescriptor& descriptor,
        RenderFeatureBuildContext& context);

    class RenderFeaturePlan
    {
    public:
        explicit RenderFeaturePlan(const Json& configurations, const RenderFeatureCreateContext& createContext = {});
        void Build(RenderFeatureStage stage, RenderFeatureBuildContext& context);
        void NotifyFrameSubmitted(uint64_t submission) noexcept;
        void AbortFrame() noexcept;
        void ResetHistory() noexcept;
        void PrepareFrame(const RenderFeatureFrameContext& context);
        void UpdateImportedResources(RenderGraph& graph);
        bool ConsumeGraphRebuildRequest() noexcept;
        bool RequiresContinuousUpdate() const noexcept;
        double GetCpuRecordMilliseconds(std::string_view type = {}) const noexcept;
        void Clear(uint64_t completed);
        void Collect(uint64_t completed);
        std::function<void()> CaptureGraphState();
        void ClearGraphResourceCaches() noexcept;
        [[nodiscard]] uint32_t RequiredInputs() const noexcept { return mRequiredInputs; }
        [[nodiscard]] std::vector<std::string> ExecutionOrder() const;
        [[nodiscard]] const std::vector<std::string>& Diagnostics() const noexcept { return mDiagnostics; }
    private:
        struct Entry
        {
            std::string Instance;
            RenderFeatureDescriptor Descriptor;
            std::unique_ptr<RenderFeature> Feature;
            RenderFeatureHistory History;
        };
        std::vector<Entry> mEntries;
        std::vector<std::string> mDiagnostics;
        uint32_t mRequiredInputs = 0;
    };
}
