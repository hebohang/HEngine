#include "Hepch.h"

#include "Runtime/Graphics/Renderer/CloudRenderFeature.h"
#include "Runtime/Graphics/Renderer/SceneCloudPass.h"

#include <stdexcept>

namespace HEngine
{
    namespace
    {
        // The feature owns the cloud renderer and all of its GPU/history resources.
        // The host supplies only immutable frame data and shared creation services.
        class CloudRenderFeature final : public RenderFeature
        {
        public:
            explicit CloudRenderFeature(const RenderFeatureCreateContext& context)
                : mPass(CreateScope<SceneCloudPass>(SceneAmbientOcclusionPassDesc{
                    .Device = context.Device,
                    .SharedShaderManager = context.SharedShaderManager,
                    .ShaderArtifactCacheRoot = context.ShaderArtifactCacheRoot,
                    .Backend = context.Backend })) {}

            void PrepareFrame(const RenderFeatureFrameContext& context) override
            {
                if (!context.World || !context.RasterCamera || !context.Clouds)
                    throw std::invalid_argument("Cloud feature requires world, raster camera and resolved cloud settings.");
                auto settings = *context.Clouds;
                if (context.bOffline) settings.Quality = CloudQuality::Off;
                mPass->PrepareFrame(settings, *context.World, *context.RasterCamera,
                    context.Width, context.Height, context.CompletedSubmission);
            }
            void Build(RenderFeatureBuildContext& context) override
            {
                if (!context.bOffline)
                    context.HdrColor = mPass->AddPasses(context.Graph, context.HdrColor, context.Depth, context.RasterDepth);
            }
            void UpdateImportedResources(RenderGraph& graph) override { mPass->UpdateImportedResources(graph); }
            bool ConsumeGraphRebuildRequest() noexcept override { return mPass->ConsumeGraphRebuildRequest(); }
            bool RequiresContinuousUpdate() const noexcept override { return mPass->RequiresContinuousUpdate(); }
            double GetCpuRecordMilliseconds() const noexcept override { return mPass->GetCpuRecordMilliseconds(); }
            void ResetHistory() noexcept override { mPass->ResetHistory(); }
            void NotifyFrameSubmitted(uint64_t submission) noexcept override { mPass->NotifyFrameSubmitted(submission); }
            void AbortFrame() noexcept override { mPass->AbortFrame(); }
            void Clear(uint64_t completed) override { mPass->Clear(completed); }
            void Collect(uint64_t completed) override { mPass->Collect(completed); }
            std::function<void()> CaptureGraphState() override { return mPass->CaptureGraphState(); }
            void ClearGraphResourceCaches() noexcept override { mPass->ClearGraphResourceCaches(); }
        private:
            Scope<SceneCloudPass> mPass;
        };
    }

    RenderFeatureDescriptor DescribeCloudRenderFeature()
    {
        return {
            .Type = "builtin.cloud",
            .Version = 1,
            .Stage = RenderFeatureStage::TemporalComposite,
            .RequiredInputs = uint32_t(RenderFeatureInput::Depth) | uint32_t(RenderFeatureInput::HdrColor),
            .ColorAccess = RenderFeatureAccess::PreserveWrite,
            .bOfflineSupported = true,
            .bMsaaSupported = true,
            .bResolvedDepthSupported = true,
        };
    }

    std::unique_ptr<RenderFeature> CreateCloudRenderFeature(
        const RenderFeatureCreateContext& context, const Json& parameters)
    {
        if (!parameters.is_object() || !parameters.empty())
            throw std::invalid_argument("Cloud parameters use the existing Rendering.Clouds settings; inline parameters must be empty.");
        if (!context.Device || !context.SharedShaderManager)
            throw std::invalid_argument("Cloud feature requires device and shared shader services.");
        return std::make_unique<CloudRenderFeature>(context);
    }
}
