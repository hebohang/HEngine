#include "Hepch.h"

#include "Runtime/Graphics/Renderer/SceneReflectionPass.h"

#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    namespace
    {
        struct alignas(16) ProbeConstants
        {
            glm::vec4 BoundsMinBlend{ 0.0f };
            glm::vec4 BoundsMaxIntensity{ 0.0f };
            glm::vec4 CapturePositionMaximumMip{ 0.0f };
        };
        struct alignas(16) ReflectionConstants
        {
            glm::mat4 InverseViewProjection{ 1.0f };
            glm::mat4 ViewProjection{ 1.0f };
            glm::vec4 CameraPositionIblIntensity{ 0.0f };
            glm::vec4 ViewForwardGlobalMaximumMip{ 0.0f };
            glm::uvec4 Dimensions{ 1u };
            glm::vec4 TraceParameters{ 50.0f, 0.2f, 0.6f, 0.08f };
            glm::uvec4 TraceOptions{ 64u, 5u, 1u, 0u };
            glm::vec4 NormalBiasOperation{ 0.03f, 0.0f, 0.0f, 0.0f };
            std::array<ProbeConstants, MaximumSceneReflectionProbes> Probes;
            glm::uvec4 DiffuseProbeMask{ 0u };
        };
        static_assert(sizeof(ProbeConstants) == 48);
        static_assert(sizeof(ReflectionConstants) == 624);
        constexpr uint32_t ReflectionInputCount = 26;
        struct PassData
        {
            std::array<TextureHandle, ReflectionInputCount> Inputs;
            TextureHandle Output;
            uint32_t Operation = 0;
        };
        struct BindingKey
        {
            std::array<nvrhi::ITexture*, ReflectionInputCount + 1> Textures{};
            bool operator==(const BindingKey&) const = default;
        };
        struct BindingHash
        {
            size_t operator()(const BindingKey& key) const noexcept
            {
                size_t hash = 0;
                for (const auto* texture : key.Textures)
                    hash ^= reinterpret_cast<size_t>(texture) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };
        float FiniteClamp(float value, float minimum, float maximum, float fallback)
        { return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback; }
    }

    struct SceneReflectionPass::Impl
    {
        using BindingMap = std::unordered_map<BindingKey, nvrhi::BindingSetHandle, BindingHash>;
        struct RetiredBindings { uint64_t Submission = 0; BindingMap Bindings; };
        explicit Impl(const SceneReflectionPassDesc& desc) : Device(desc.Device)
        {
            if (!Device || desc.ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("Scene reflections require a device and shader artifact root");
            if (!desc.SharedShaderManager) OwnedShaderManager = CreateScope<ShaderManager>(Device);
            auto& shaders = desc.SharedShaderManager ? *desc.SharedShaderManager : *OwnedShaderManager;
            ShaderArtifactResolver resolver(desc.ShaderArtifactCacheRoot, desc.Backend);
            Shader = resolver.LoadShaderFile(shaders, ShaderCacheKey{
                .Source = "Modern/SceneReflections.hlsl", .Variant = "BASE",
                .Stage = nvrhi::ShaderType::Compute, .EntryPoint = "CS", .TargetProfile = "cs_6_0",
            }, "SceneReflections.cs", "Scene reflections");
            ConstantsBuffer = Device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(sizeof(ReflectionConstants)).setIsConstantBuffer(true)
                .setIsVolatile(true).setMaxVersions(64).setDebugName("Scene Reflection Constants"));
            Sampler = Device->createSampler(nvrhi::SamplerDesc()
                .setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
            nvrhi::BindingLayoutDesc layout;
            layout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
                .addItem(nvrhi::BindingLayoutItem::Sampler(0));
            for (uint32_t i = 0; i < ReflectionInputCount; ++i)
                layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(i));
            layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
            Layout = Device->createBindingLayout(layout);
            Pipeline = Device->createComputePipeline(nvrhi::ComputePipelineDesc()
                .setComputeShader(Shader).addBindingLayout(Layout));
            if (!Shader || !ConstantsBuffer || !Sampler || !Layout || !Pipeline)
                throw std::runtime_error("NVRHI failed to create scene reflection state");
        }

        nvrhi::IBindingSet* GetBindings(const BindingKey& key)
        {
            const auto found = Bindings.find(key);
            if (found != Bindings.end()) return found->second;
            for (uint32_t i = 0; i < ReflectionInputCount; ++i)
                if (key.Textures[i]->getDesc().sampleCount != 1)
                    throw std::invalid_argument("Scene reflections require resolved single-sample inputs; MSAA is not implicitly discarded");
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, ConstantsBuffer))
                .addItem(nvrhi::BindingSetItem::Sampler(0, Sampler));
            for (uint32_t i = 0; i < ReflectionInputCount; ++i)
                desc.addItem(nvrhi::BindingSetItem::Texture_SRV(i, key.Textures[i]));
            desc.addItem(nvrhi::BindingSetItem::Texture_UAV(0, key.Textures[ReflectionInputCount]));
            auto binding = Device->createBindingSet(desc, Layout);
            if (!binding) throw std::runtime_error("NVRHI failed to bind scene reflection resources");
            return Bindings.emplace(key, std::move(binding)).first->second;
        }
        void Retire()
        {
            if (Bindings.empty()) return;
            Retired.push_back({ LastSubmission, std::move(Bindings) });
            Bindings.clear();
        }
        void AddStage(RenderGraph& graph, const char* name, uint32_t operation,
            const std::array<TextureHandle, ReflectionInputCount>& inputs, TextureHandle output)
        {
            graph.AddPass<PassData>(name, RenderGraphQueue::Compute,
                [=](RenderGraphBuilder& builder, PassData& data)
                {
                    for (uint32_t i = 0; i < ReflectionInputCount; ++i)
                        data.Inputs[i] = builder.ShaderResource(inputs[i]);
                    data.Output = builder.Write(output);
                    data.Operation = operation;
                },
                [this](RenderGraphContext& context, const PassData& data)
                {
                    ReflectionConstants constants = FrameConstants;
                    constants.NormalBiasOperation.y = static_cast<float>(data.Operation);
                    BindingKey key;
                    for (uint32_t i = 0; i < ReflectionInputCount; ++i)
                        key.Textures[i] = context.GetTexture(data.Inputs[i], RenderGraphAccess::Read);
                    key.Textures[ReflectionInputCount] = context.GetTexture(data.Output, RenderGraphAccess::Write);
                    auto* commands = context.GetCommandList();
                    commands->writeBuffer(ConstantsBuffer, &constants, sizeof(constants));
                    nvrhi::ComputeState state;
                    state.setPipeline(Pipeline).addBindingSet(GetBindings(key));
                    commands->setComputeState(state);
                    const uint32_t width = data.Operation == 0 ? constants.Dimensions.z : constants.Dimensions.x;
                    const uint32_t height = data.Operation == 0 ? constants.Dimensions.w : constants.Dimensions.y;
                    commands->dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
                    bFrameRecorded = true;
                });
        }
        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        nvrhi::ShaderHandle Shader;
        nvrhi::BufferHandle ConstantsBuffer;
        nvrhi::SamplerHandle Sampler;
        nvrhi::BindingLayoutHandle Layout;
        nvrhi::ComputePipelineHandle Pipeline;
        BindingMap Bindings;
        std::vector<RetiredBindings> Retired;
        std::array<nvrhi::TextureHandle, MaximumSceneReflectionProbes> ProbeTextures;
        std::array<nvrhi::TextureHandle, MaximumSceneReflectionProbes> ProbeIrradianceTextures;
        SceneReflectionSettings Settings;
        ReflectionConstants FrameConstants;
        uint64_t LastSubmission = 0;
        bool bGraphRebuild = true;
        bool bFrameRecorded = false;
    };

    SceneReflectionPass::SceneReflectionPass(const SceneReflectionPassDesc& desc) : mImpl(CreateScope<Impl>(desc)) {}
    SceneReflectionPass::~SceneReflectionPass() = default;

    void SceneReflectionPass::PrepareFrame(const SceneReflectionSettings& requested, const RenderCamera& camera,
        uint32_t width, uint32_t height, float iblIntensity, float globalMaximumMip,
        std::span<const SceneReflectionProbe> probes, uint64_t completedSubmission, float diffuseIblWeight)
    {
        if (width == 0 || height == 0 || std::abs(glm::determinant(camera.ViewProjection)) < 1e-12f)
            throw std::invalid_argument("Scene reflections require valid native dimensions and raster camera");
        if (probes.size() > MaximumSceneReflectionProbes)
            throw std::invalid_argument("Scene reflection probe selection exceeds the published budget of eight");
        Collect(completedSubmission);
        mImpl->bGraphRebuild |= requested.bScreenSpaceEnabled != mImpl->Settings.bScreenSpaceEnabled ||
            requested.bLocalProbesEnabled != mImpl->Settings.bLocalProbesEnabled ||
            width != mImpl->FrameConstants.Dimensions.x || height != mImpl->FrameConstants.Dimensions.y;
        mImpl->Settings = requested;
        auto& constants = mImpl->FrameConstants;
        constants.InverseViewProjection = glm::inverse(camera.ViewProjection);
        constants.ViewProjection = camera.ViewProjection;
        constants.CameraPositionIblIntensity = glm::vec4(camera.Position, FiniteClamp(iblIntensity, 0.0f, 100.0f, 0.0f));
        const glm::mat4 inverseView = glm::inverse(camera.View);
        constants.ViewForwardGlobalMaximumMip = glm::vec4(glm::normalize(-glm::vec3(inverseView[2])),
            FiniteClamp(globalMaximumMip, 0.0f, 20.0f, 0.0f));
        constants.Dimensions = { width, height, (width + 1u) / 2u, (height + 1u) / 2u };
        constants.TraceParameters = { FiniteClamp(requested.MaximumDistance, 0.1f, 500.0f, 50.0f),
            FiniteClamp(requested.Thickness, 0.001f, 10.0f, 0.2f),
            FiniteClamp(requested.RoughnessCutoff, 0.05f, 1.0f, 0.6f),
            FiniteClamp(requested.EdgeFade, 0.001f, 0.5f, 0.08f) };
        constants.TraceOptions = { std::clamp(requested.StepCount, 8u, 256u),
            std::min(requested.BinaryRefinementSteps, 12u), requested.bScreenSpaceEnabled ? 1u : 0u, 0u };
        constants.NormalBiasOperation.x = FiniteClamp(requested.NormalBias, 0.0f, 1.0f, 0.03f);
        constants.NormalBiasOperation.z = requested.bGuideCacheEnabled ? 1.0f : 0.0f;
        constants.NormalBiasOperation.w = FiniteClamp(diffuseIblWeight, 0.0f, 1.0f, 0.0f);
        uint32_t count = 0;
        constants.DiffuseProbeMask = glm::uvec4(0u);
        std::array<nvrhi::TextureHandle, MaximumSceneReflectionProbes> textures;
        std::array<nvrhi::TextureHandle, MaximumSceneReflectionProbes> irradianceTextures;
        if (requested.bLocalProbesEnabled)
        {
            for (const auto& probe : probes)
            {
                if (!probe.PrefilteredEnvironment || !glm::all(glm::lessThan(probe.BoundsMin, probe.BoundsMax)))
                    continue;
                const auto& desc = probe.PrefilteredEnvironment->getDesc();
                if (desc.dimension != nvrhi::TextureDimension::TextureCube)
                    throw std::invalid_argument("Local reflection probe prefilter must be a cubemap");
                constants.Probes[count] = { glm::vec4(probe.BoundsMin, FiniteClamp(probe.BlendDistance, 0.001f, 1000.0f, 1.0f)),
                    glm::vec4(probe.BoundsMax, FiniteClamp(probe.Intensity, 0.0f, 100.0f, 1.0f)),
                    glm::vec4(probe.CapturePosition, static_cast<float>(std::max(desc.mipLevels, 1u) - 1u)) };
                if (probe.Irradiance && probe.Irradiance->getDesc().dimension != nvrhi::TextureDimension::TextureCube)
                    throw std::invalid_argument("Local probe irradiance must be a cubemap");
                irradianceTextures[count] = probe.Irradiance;
                if (probe.Irradiance) constants.DiffuseProbeMask.x |= 1u << count;
                textures[count++] = probe.PrefilteredEnvironment;
            }
        }
        mImpl->bGraphRebuild |= textures != mImpl->ProbeTextures;
        mImpl->ProbeTextures = std::move(textures);
        mImpl->bGraphRebuild |= irradianceTextures != mImpl->ProbeIrradianceTextures;
        mImpl->ProbeIrradianceTextures = std::move(irradianceTextures);
        constants.TraceOptions.w = count;
    }

    TextureHandle SceneReflectionPass::AddPasses(RenderGraph& graph, TextureHandle opaqueHdr, TextureHandle depth,
        TextureHandle baseColorMetallic, TextureHandle normalRoughnessAo, TextureHandle emissiveModel,
        TextureHandle screenSpaceAo, TextureHandle globalPrefilteredEnvironment, TextureHandle brdfLut,
        TextureHandle globalIrradiance)
    {
        mImpl->Retire();
        mImpl->bGraphRebuild = false;
        if (!mImpl->Settings.bScreenSpaceEnabled && mImpl->FrameConstants.TraceOptions.w == 0u) return opaqueHdr;
        if (!opaqueHdr || !depth || !baseColorMetallic || !normalRoughnessAo || !emissiveModel ||
            !screenSpaceAo || !globalPrefilteredEnvironment || !brdfLut || !globalIrradiance)
            throw std::invalid_argument("Scene reflections require complete single-sample raster surface and IBL inputs");
        std::array<TextureHandle, ReflectionInputCount> inputs{ opaqueHdr, depth, baseColorMetallic,
            normalRoughnessAo, emissiveModel, screenSpaceAo, globalPrefilteredEnvironment, brdfLut };
        std::unordered_map<nvrhi::ITexture*, TextureHandle> importedProbes;
        for (uint32_t i = 0; i < MaximumSceneReflectionProbes; ++i)
        {
            const auto& texture = mImpl->ProbeTextures[i];
            if (!texture) { inputs[8u + i] = globalPrefilteredEnvironment; continue; }
            if (const auto found = importedProbes.find(texture); found != importedProbes.end())
            { inputs[8u + i] = found->second; continue; }
            const auto& desc = texture->getDesc();
            inputs[8u + i] = graph.ImportTexture(RenderGraphTextureDesc{
                .DebugName = "Local Reflection Probe " + std::to_string(i),
                .Width = desc.width, .Height = desc.height, .MipLevels = desc.mipLevels,
                .ArrayLayers = desc.arraySize, .Format = desc.format, .Dimension = desc.dimension },
                texture, nvrhi::ResourceStates::ShaderResource, nvrhi::ResourceStates::ShaderResource);
            importedProbes.emplace(texture, inputs[8u + i]);
        }
        inputs[17] = globalIrradiance;
        for (uint32_t i = 0; i < MaximumSceneReflectionProbes; ++i)
        {
            const auto& texture = mImpl->ProbeIrradianceTextures[i];
            if (!texture) { inputs[18u + i] = globalIrradiance; continue; }
            if (const auto found = importedProbes.find(texture); found != importedProbes.end())
            { inputs[18u + i] = found->second; continue; }
            const auto& desc = texture->getDesc();
            inputs[18u + i] = graph.ImportTexture(RenderGraphTextureDesc{
                .DebugName = "Local Probe Irradiance " + std::to_string(i),
                .Width = desc.width, .Height = desc.height, .MipLevels = desc.mipLevels,
                .ArrayLayers = desc.arraySize, .Format = desc.format, .Dimension = desc.dimension },
                texture, nvrhi::ResourceStates::ShaderResource, nvrhi::ResourceStates::ShaderResource);
            importedProbes.emplace(texture, inputs[18u + i]);
        }
        const auto dimensions = mImpl->FrameConstants.Dimensions;
        inputs[16] = opaqueHdr;
        if (mImpl->Settings.bScreenSpaceEnabled)
        {
            const auto half = graph.CreateTexture(RenderGraphTextureDesc{ .DebugName = "SSR Half Radiance Confidence",
                .Width = dimensions.z, .Height = dimensions.w, .Format = nvrhi::Format::RGBA16_FLOAT });
            mImpl->AddStage(graph, "SSRHalfTrace", 0u, inputs, half);
            inputs[16] = half;
        }
        const auto output = graph.CreateTexture(RenderGraphTextureDesc{ .DebugName = "Reflection Native HDR",
            .Width = dimensions.x, .Height = dimensions.y, .Format = nvrhi::Format::RGBA16_FLOAT });
        mImpl->AddStage(graph, "ReflectionReplaceComposite", 1u, inputs, output);
        return output;
    }
    bool SceneReflectionPass::ConsumeGraphRebuildRequest() noexcept
    { const bool result = mImpl->bGraphRebuild; mImpl->bGraphRebuild = false; return result; }
    void SceneReflectionPass::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        if (mImpl->bFrameRecorded) mImpl->LastSubmission = submission;
        mImpl->bFrameRecorded = false;
    }
    void SceneReflectionPass::AbortFrame() noexcept { mImpl->bFrameRecorded = false; }
    void SceneReflectionPass::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mImpl->Retired, [completedSubmission](const Impl::RetiredBindings& state)
            { return state.Submission <= completedSubmission; });
    }
    void SceneReflectionPass::Clear(uint64_t completedSubmission)
    {
        mImpl->Retire();
        mImpl->ProbeTextures = {};
        mImpl->ProbeIrradianceTextures = {};
        mImpl->bGraphRebuild = true;
        Collect(completedSubmission);
    }
    std::function<void()> SceneReflectionPass::CaptureGraphState()
    {
        return [impl = mImpl.get(), bindings = mImpl->Bindings, retired = mImpl->Retired]() mutable noexcept
        { impl->Bindings.swap(bindings); impl->Retired.swap(retired); };
    }
    void SceneReflectionPass::ClearGraphResourceCaches() noexcept
    {
        mImpl->Bindings.clear();
        for (auto& retired : mImpl->Retired) retired.Bindings.clear();
    }
}
