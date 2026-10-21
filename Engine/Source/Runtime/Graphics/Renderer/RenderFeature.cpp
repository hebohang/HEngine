#include "Hepch.h"
#include "RenderFeature.h"
#include "CloudRenderFeature.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        struct Registration { RenderFeatureDescriptor Descriptor; RenderFeatureFactory Factory; };
        void ValidateBuiltinParameters(const Json& parameters)
        {
            if (!parameters.is_object() || !parameters.empty())
                throw std::invalid_argument("Built-in outline/postprocess parameters must be an empty object; configure their existing rendering settings instead");
        }
        class OutlineFeature final : public RenderFeature
        {
            void Build(RenderFeatureBuildContext& context) override { context.DrawSelectedObjects(); }
        };
        class PostProcessFeature final : public RenderFeature
        {
            void Build(RenderFeatureBuildContext& context) override
            {
                if (!context.bDiagnosticView)
                {
                    if (!context.PostProcess) throw std::invalid_argument("Post-processing service is unavailable");
                    context.Color() = context.PostProcess->AddPasses(context.Graph,
                        context.Color(), context.Width, context.Height);
                }
            }
        };
        std::mutex& RegistryMutex() { static std::mutex mutex; return mutex; }
        auto& Registry()
        {
            static std::map<std::string, Registration> registry{
                {"builtin.cloud", {DescribeCloudRenderFeature(), CreateCloudRenderFeature}},
                {"builtin.outline", {{"builtin.outline", 1, RenderFeatureStage::SceneOverlay,
                    uint32_t(RenderFeatureInput::Depth) | uint32_t(RenderFeatureInput::HdrColor),
                    RenderFeatureAccess::PreserveWrite, true, true},
                    [](const RenderFeatureCreateContext&, const Json& parameters) {
                        ValidateBuiltinParameters(parameters);
                        return std::make_unique<OutlineFeature>(); }}},
                {"builtin.postprocess", {{"builtin.postprocess", 1, RenderFeatureStage::HdrPostProcess,
                    uint32_t(RenderFeatureInput::HdrColor),
                    RenderFeatureAccess::ReadWrite, true, false},
                    [](const RenderFeatureCreateContext&, const Json& parameters) {
                        ValidateBuiltinParameters(parameters);
                        return std::make_unique<PostProcessFeature>(); }}}
            };
            return registry;
        }
        Json Expanded(const Json& input)
        {
            if (!input.is_array()) throw std::invalid_argument("Pipeline Features must be an array");
            return input;
        }
    }

    struct RenderFeatureHistory::State
    {
        const RenderGraph* Graph = nullptr;
        TextureHandle Texture;
        RenderGraphTextureDesc Desc;
        bool bValid = false;
        bool bRecorded = false;
        bool bStoreDeclared = false;
        uint64_t Submission = 0;
    };

    TextureHandle RenderFeatureHistory::ReadColor(RenderGraph& graph, std::string name, RenderGraphTextureDesc desc)
    {
        if (name.empty() || !desc.Width || !desc.Height || desc.Depth != 1 || desc.MipLevels != 1 ||
            desc.ArrayLayers != 1 || desc.SampleCount != 1 || desc.Format == nvrhi::Format::UNKNOWN ||
            desc.Dimension != nvrhi::TextureDimension::Texture2D || nvrhi::getFormatInfo(desc.Format).hasDepth)
            throw std::invalid_argument("Feature color history requires a named, single-sample 2D color texture");
        ++mReadRequests;
        auto& state = mStates[name];
        if (!state) state = std::make_shared<State>();
        if (state->Graph == &graph && graph.IsValid(state->Texture))
        {
            if (state->Desc.Width != desc.Width || state->Desc.Height != desc.Height || state->Desc.Format != desc.Format)
                throw std::invalid_argument("Feature history shape changed within one graph; rebuild the view graph first");
            return state->Texture;
        }
        state = std::make_shared<State>();
        state->Graph = &graph;
        desc.DebugName = "Feature history " + name;
        desc.bRelativeToGraph = false;
        state->Desc = desc;
        state->Texture = graph.CreateTexture(desc, RenderGraphResourceLifetime::Persistent);
        graph.AddPass<TextureHandle>("Initialize " + desc.DebugName,
            [state](RenderGraphBuilder& builder, TextureHandle& data) { data = builder.RenderTarget(state->Texture); },
            [state](RenderGraphContext& context, const TextureHandle& data) {
                // Validity changes only at the submission boundary. The no-op
                // path preserves last frame's contents on the same ordered queue.
                if (!state->bValid)
                    context.GetCommandList()->clearTextureFloat(context.GetTexture(data, RenderGraphAccess::Write),
                        nvrhi::AllSubresources, nvrhi::Color(0.0f));
            });
        return state->Texture;
    }

    void RenderFeatureHistory::StoreColor(RenderGraph& graph, const std::string& name, TextureHandle source)
    {
        const auto found = mStates.find(name);
        if (found == mStates.end() || found->second->Graph != &graph || !graph.IsValid(found->second->Texture))
            throw std::invalid_argument("Feature history must be read/declared before storing it");
        const auto state = found->second;
        if (state->bStoreDeclared || source == state->Texture || !graph.IsValid(source))
            throw std::invalid_argument("Feature history requires one distinct source/store per graph");
        const auto& desc = graph.GetTextureDesc(source);
        if (desc.Width != state->Desc.Width || desc.Height != state->Desc.Height || desc.Format != state->Desc.Format ||
            desc.Depth != 1 || desc.ArrayLayers != 1 || desc.MipLevels != 1 || desc.SampleCount != 1)
            throw std::invalid_argument("Feature history store source does not match its texture contract");
        state->bStoreDeclared = true;
        // Persistent resources make their final access an observable graph root,
        // so this write survives culling even without another read this frame.
        graph.AddPass<std::pair<TextureHandle, TextureHandle>>("Store " + state->Desc.DebugName,
            [source, state](RenderGraphBuilder& builder, auto& data) {
                data.first = builder.CopySource(source);
                data.second = builder.CopyDest(state->Texture);
            },
            [state](RenderGraphContext& context, const auto& data) {
                context.GetCommandList()->copyTexture(context.GetTexture(data.second, RenderGraphAccess::Write), {},
                    context.GetTexture(data.first, RenderGraphAccess::Read), {});
                state->bRecorded = true;
            });
    }

    bool RenderFeatureHistory::IsValid(const std::string& name) const noexcept
    {
        const auto found = mStates.find(name);
        return found != mStates.end() && found->second->bValid;
    }

    void RenderFeatureHistory::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        for (auto& [name, state] : mStates)
        {
            if (state->bRecorded && submission != 0)
            {
                state->bValid = true;
                state->Submission = submission;
            }
            state->bRecorded = false;
        }
    }

    void RenderFeatureHistory::AbortFrame() noexcept
    {
        for (auto& [name, state] : mStates) state->bRecorded = false;
    }

    void RenderFeatureHistory::Reset() noexcept
    {
        for (auto& [name, state] : mStates) { state->bValid = false; state->bRecorded = false; }
    }

    void ValidateRenderFeatureColorAccess(const RenderGraph& graph, uint32_t firstPass,
        TextureHandle input, TextureHandle output, RenderFeatureAccess access)
    {
        if (!graph.IsValid(input) || !graph.IsValid(output) || access > RenderFeatureAccess::FullOverwrite)
            throw std::invalid_argument("Feature color target or access declaration is invalid");
        const auto snapshot = graph.BuildDiagnosticSnapshot();
        bool bReadInput = false, bWriteInput = false, bWriteOutput = false, bFullOutput = false;
        for (const auto& declared : snapshot.Accesses)
        {
            if (declared.PassId < firstPass || declared.ResourceKind != RenderGraphDiagnosticResourceKind::Texture) continue;
            const bool read = declared.Access != RenderGraphAccess::Write;
            const bool write = declared.Access != RenderGraphAccess::Read;
            if (declared.ResourceId == input.Index)
            {
                bReadInput |= read;
                bWriteInput |= write;
                if (access == RenderFeatureAccess::PreserveWrite && write &&
                    (!read || (declared.bAttachment && declared.AttachmentOps.Load != RenderGraphAttachmentLoad::Load)))
                    throw std::invalid_argument("PreserveWrite Feature discarded existing color contents");
            }
            if (declared.ResourceId == output.Index && write)
            {
                bWriteOutput = true;
                const auto& range = declared.Subresources;
                const auto& desc = graph.GetTextureDesc(output);
                const bool fullRange = range.BaseMipLevel == 0 && range.BaseArraySlice == 0 &&
                    (range.MipLevelCount == RenderGraphSubresourceRange::AllRemaining || range.MipLevelCount == desc.MipLevels) &&
                    (range.ArraySliceCount == RenderGraphSubresourceRange::AllRemaining || range.ArraySliceCount == desc.ArrayLayers);
                bFullOutput |= fullRange && ((declared.bAttachment &&
                    (declared.AttachmentOps.Load == RenderGraphAttachmentLoad::Clear ||
                        (declared.AttachmentOps.Load == RenderGraphAttachmentLoad::Discard && declared.AttachmentOps.bFullOverwrite))) ||
                    declared.Usage == RenderGraphUsage::CopyDest);
            }
        }
        if ((access == RenderFeatureAccess::Read && (bWriteInput || input != output)) ||
            (access == RenderFeatureAccess::PreserveWrite && input != output) ||
            (input != output && !bWriteOutput) ||
            (access == RenderFeatureAccess::ReadWrite && (bWriteInput || input != output) && !bReadInput) ||
            (access == RenderFeatureAccess::FullOverwrite && (bWriteOutput || input != output) && !bFullOutput))
            throw std::invalid_argument("Feature graph color accesses do not satisfy its declared contract");
    }

    void ValidateRenderFeatureInputs(const RenderFeatureDescriptor& descriptor, RenderFeatureBuildContext& context)
    {
        const auto requiresInput = [&](RenderFeatureInput input) { return (descriptor.RequiredInputs & uint32_t(input)) != 0; };
        const auto valid = [&](TextureHandle handle) { return context.Graph.IsValid(handle); };
        if (!valid(context.Color()) ||
            (requiresInput(RenderFeatureInput::Depth) && !valid(context.Depth)) ||
            (requiresInput(RenderFeatureInput::Normal) && !valid(context.Normal)) ||
            (requiresInput(RenderFeatureInput::MotionVector) && !valid(context.MotionVector)) ||
            (requiresInput(RenderFeatureInput::OpaqueColor) && !valid(context.OpaqueColor)) ||
            (requiresInput(RenderFeatureInput::HdrColor) && !valid(context.HdrColor)) ||
            (requiresInput(RenderFeatureInput::DisplayColor) && !valid(context.DisplayColor)) ||
            (requiresInput(RenderFeatureInput::History) && !context.History))
            throw std::invalid_argument("Required Feature input is unavailable at the declared stage");
        context.ColorSampleCount = context.Graph.GetTextureDesc(context.Color()).SampleCount;
        context.DepthSampleCount = valid(context.Depth) ? context.Graph.GetTextureDesc(context.Depth).SampleCount : 0;
        context.bMsaa = context.ColorSampleCount > 1;
        if (context.Stage != RenderFeatureStage::TemporalComposite && context.Stage != RenderFeatureStage::SceneOverlay &&
            (context.ColorSampleCount != 1 || (requiresInput(RenderFeatureInput::Depth) && context.DepthSampleCount != 1)))
            throw std::invalid_argument("Post-process Feature inputs must be resolved single-sample resources");
        if ((context.bOffline && !descriptor.bOfflineSupported) ||
            (context.bMsaa && !descriptor.bMsaaSupported) ||
            (requiresInput(RenderFeatureInput::Depth) && context.DepthSampleCount != context.ColorSampleCount &&
                !(descriptor.bResolvedDepthSupported && context.DepthSampleCount == 1 && context.ColorSampleCount > 1)))
            throw std::invalid_argument("Feature does not support this view/color/depth sample contract");
    }

    Json DefaultRenderFeatureConfigurations()
    {
        return Json::array({
            {{"type", "builtin.cloud"}, {"version", 1}, {"instance", "cloud"}, {"enabled", true}, {"parameters", Json::object()}},
            {{"type", "builtin.outline"}, {"version", 1}, {"instance", "outline"}, {"enabled", true}, {"parameters", Json::object()}},
            {{"type", "builtin.postprocess"}, {"version", 1}, {"instance", "postprocess"}, {"enabled", true}, {"parameters", Json::object()}}
        });
    }

    void RegisterRenderFeature(RenderFeatureDescriptor descriptor, RenderFeatureFactory factory)
    {
        if (descriptor.Type.empty() || !descriptor.Version || !factory)
            throw std::invalid_argument("Feature registration requires type, version and factory");
        if (descriptor.Stage > RenderFeatureStage::DisplayPostProcess ||
            descriptor.ColorAccess > RenderFeatureAccess::FullOverwrite ||
            (descriptor.RequiredInputs & ~127u) != 0)
            throw std::invalid_argument("Feature registration contains an invalid stage, access or input declaration");
        std::lock_guard lock(RegistryMutex());
        const auto id = descriptor.Type;
        if (!Registry().emplace(id, Registration{std::move(descriptor), std::move(factory)}).second)
            throw std::invalid_argument("Duplicate render feature type: " + id);
    }

    void RegisterRenderFeature(RenderFeatureDescriptor descriptor, LegacyRenderFeatureFactory factory)
    {
        if (!factory) throw std::invalid_argument("Feature registration requires a factory");
        RegisterRenderFeature(std::move(descriptor),
            [factory = std::move(factory)](const RenderFeatureCreateContext&, const Json& parameters) {
                return factory(parameters);
            });
    }

    std::vector<RenderFeatureDescriptor> EnumerateRenderFeatures()
    {
        std::lock_guard lock(RegistryMutex());
        std::vector<RenderFeatureDescriptor> result;
        for (const auto& [id, registration] : Registry()) result.push_back(registration.Descriptor);
        return result;
    }

    std::vector<std::string> ValidateRenderFeatures(const Json& configurations)
    {
        std::vector<std::string> diagnostics;
        std::set<std::string> instances;
        std::lock_guard lock(RegistryMutex());
        for (const auto& entry : Expanded(configurations))
        {
            const auto type = entry.at("type").get<std::string>();
            const auto instance = entry.at("instance").get<std::string>();
            if (instance.empty() || !instances.insert(instance).second)
                throw std::invalid_argument("Feature instance must be nonempty and unique: " + instance);
            const auto found = Registry().find(type);
            if (found == Registry().end())
                diagnostics.push_back(instance + ": missing feature type " + type + "; configuration retained");
            else if (entry.at("version").get<uint32_t>() != found->second.Descriptor.Version)
                diagnostics.push_back(instance + ": unsupported configuration version; configuration retained");
        }
        return diagnostics;
    }

    RenderFeaturePlan::RenderFeaturePlan(const Json& configurations, const RenderFeatureCreateContext& createContext)
        : mDiagnostics(ValidateRenderFeatures(configurations))
    {
        // Copy factories under lock; plugin code runs without the registry lock.
        std::map<std::string, Registration> registrations;
        { std::lock_guard lock(RegistryMutex()); registrations = Registry(); }
        for (const auto& entry : Expanded(configurations))
        {
            const auto found = registrations.find(entry.at("type").get<std::string>());
            if (!entry.value("enabled", true) || found == registrations.end() ||
                entry.at("version").get<uint32_t>() != found->second.Descriptor.Version) continue;
            auto feature = found->second.Factory(createContext, entry.value("parameters", Json::object()));
            if (!feature) throw std::runtime_error("Feature factory returned no instance");
            mRequiredInputs |= found->second.Descriptor.RequiredInputs;
            mEntries.push_back({entry.at("instance").get<std::string>(), found->second.Descriptor, std::move(feature)});
        }
        std::stable_sort(mEntries.begin(), mEntries.end(), [](const Entry& a, const Entry& b) {
            return a.Descriptor.Stage < b.Descriptor.Stage;
        });
    }

    void RenderFeaturePlan::Build(RenderFeatureStage stage, RenderFeatureBuildContext& context)
    {
        context.Stage = stage;
        const auto stageOutput = context.Color();
        for (auto& entry : mEntries)
        {
            if (entry.Descriptor.Stage != stage) continue;
            auto* previousHistory = context.History;
            context.History = &entry.History;
            try
            {
                ValidateRenderFeatureInputs(entry.Descriptor, context);
                const auto colorInput = context.Color();
                const uint32_t firstPass = static_cast<uint32_t>(context.Graph.BuildDiagnosticSnapshot().Passes.size());
                const uint64_t historyReads = entry.History.ReadRequests();
                RenderGraphFeatureScope scope(context.Graph, entry.Descriptor.Type, entry.Instance);
                entry.Feature->Build(context);
                ValidateRenderFeatureColorAccess(context.Graph, firstPass, colorInput, context.Color(), entry.Descriptor.ColorAccess);
                const bool usedHistory = entry.History.ReadRequests() != historyReads;
                const bool requestedHistory = (entry.Descriptor.RequiredInputs & uint32_t(RenderFeatureInput::History)) != 0;
                if (usedHistory != requestedHistory)
                    throw std::invalid_argument("Feature history usage does not match its input declaration");
            }
            catch (...)
            {
                context.History = previousHistory;
                throw;
            }
            context.History = previousHistory;
        }
        if (stage == RenderFeatureStage::DisplayPostProcess && context.DisplayColor != stageOutput)
        {
            const auto source = context.DisplayColor;
            const auto& from = context.Graph.GetTextureDesc(source);
            const auto& to = context.Graph.GetTextureDesc(stageOutput);
            if (from.Width != to.Width || from.Height != to.Height || from.Format != to.Format ||
                from.SampleCount != 1 || to.SampleCount != 1 || from.MipLevels != 1 || to.MipLevels != 1 ||
                from.ArrayLayers != 1 || to.ArrayLayers != 1)
                throw std::invalid_argument("Display Feature output must match the host display target");
            context.Graph.AddPass<std::pair<TextureHandle, TextureHandle>>("Feature display output",
                [source, stageOutput](RenderGraphBuilder& builder, auto& data) {
                    data.first = builder.CopySource(source);
                    data.second = builder.CopyDest(stageOutput);
                }, [](RenderGraphContext& graphContext, const auto& data) {
                    graphContext.GetCommandList()->copyTexture(graphContext.GetTexture(data.second, RenderGraphAccess::Write), {},
                        graphContext.GetTexture(data.first, RenderGraphAccess::Read), {});
                });
            context.DisplayColor = stageOutput;
        }
    }

    void RenderFeaturePlan::PrepareFrame(const RenderFeatureFrameContext& context)
    { for (auto& entry : mEntries) entry.Feature->PrepareFrame(context); }
    void RenderFeaturePlan::UpdateImportedResources(RenderGraph& graph)
    { for (auto& entry : mEntries) entry.Feature->UpdateImportedResources(graph); }
    bool RenderFeaturePlan::ConsumeGraphRebuildRequest() noexcept
    {
        bool dirty = false;
        for (auto& entry : mEntries) dirty |= entry.Feature->ConsumeGraphRebuildRequest();
        return dirty;
    }
    bool RenderFeaturePlan::RequiresContinuousUpdate() const noexcept
    {
        for (const auto& entry : mEntries) if (entry.Feature->RequiresContinuousUpdate()) return true;
        return false;
    }
    double RenderFeaturePlan::GetCpuRecordMilliseconds(std::string_view type) const noexcept
    {
        double result = 0.0;
        for (const auto& entry : mEntries)
            if (type.empty() || entry.Descriptor.Type == type) result += entry.Feature->GetCpuRecordMilliseconds();
        return result;
    }
    void RenderFeaturePlan::NotifyFrameSubmitted(uint64_t submission) noexcept
    { for (auto& entry : mEntries) { entry.History.NotifyFrameSubmitted(submission); entry.Feature->NotifyFrameSubmitted(submission); } }
    void RenderFeaturePlan::AbortFrame() noexcept
    { for (auto& entry : mEntries) { entry.History.AbortFrame(); entry.Feature->AbortFrame(); } }
    void RenderFeaturePlan::ResetHistory() noexcept
    { for (auto& entry : mEntries) { entry.History.Reset(); entry.Feature->ResetHistory(); } }
    void RenderFeaturePlan::Clear(uint64_t completed)
    { for (auto& entry : mEntries) { entry.Feature->Clear(completed); entry.History.Reset(); } }
    void RenderFeaturePlan::Collect(uint64_t completed)
    { for (auto& entry : mEntries) entry.Feature->Collect(completed); }
    std::function<void()> RenderFeaturePlan::CaptureGraphState()
    {
        std::vector<std::function<void()>> exchanges;
        for (auto& entry : mEntries)
            if (auto exchange = entry.Feature->CaptureGraphState()) exchanges.push_back(std::move(exchange));
        return [exchanges = std::move(exchanges)]() { for (const auto& exchange : exchanges) exchange(); };
    }
    void RenderFeaturePlan::ClearGraphResourceCaches() noexcept
    { for (auto& entry : mEntries) entry.Feature->ClearGraphResourceCaches(); }

    std::vector<std::string> RenderFeaturePlan::ExecutionOrder() const
    {
        std::vector<std::string> result;
        for (const auto& entry : mEntries) result.push_back(entry.Instance);
        return result;
    }
}
