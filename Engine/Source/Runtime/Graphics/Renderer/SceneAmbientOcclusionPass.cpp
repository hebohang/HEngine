#include "Hepch.h"

#include "Runtime/Graphics/Renderer/SceneAmbientOcclusionPass.h"

#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/Scene/RenderWorld.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    namespace
    {
        struct alignas(16) GtaoConstants
        {
            glm::mat4 InverseViewProjection{ 1.0f };
            glm::vec4 CameraPositionRadius{ 0.0f, 0.0f, 0.0f, 1.0f };
            glm::uvec4 Dimensions{ 1u };
            glm::vec4 ViewForwardIntensity{ 0.0f, 0.0f, -1.0f, 1.0f };
            glm::vec4 Parameters{ 1.0f, 0.01f, 0.0f, 1.0f };
        };
        static_assert(sizeof(GtaoConstants) == 128);
        struct PassData { TextureHandle Depth, Normal, Input, Output; uint32_t Operation = 0; };
        struct BindingKey
        {
            std::array<nvrhi::ITexture*, 4> Textures{};
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
    }

    struct SceneAmbientOcclusionPass::Impl
    {
        using BindingMap = std::unordered_map<BindingKey, nvrhi::BindingSetHandle, BindingHash>;
        struct RetiredBindings { uint64_t Submission = 0; BindingMap Bindings; };
        explicit Impl(const SceneAmbientOcclusionPassDesc& desc) : Device(desc.Device)
        {
            if (!Device || desc.ShaderArtifactCacheRoot.empty())
                throw std::invalid_argument("GTAO requires a device and shader artifact cache root");
            if (!desc.SharedShaderManager)
                OwnedShaderManager = CreateScope<ShaderManager>(Device);
            ShaderManager& shaderManager = desc.SharedShaderManager ? *desc.SharedShaderManager : *OwnedShaderManager;
            const auto binaryFormat = GetShaderBinaryFormat(desc.Backend);
            const auto artifactPath = desc.ShaderArtifactCacheRoot /
                std::string(GetShaderArtifactDirectoryName(binaryFormat)) / "SceneGtao.cs";
            std::ifstream keyStream(MakeShaderCacheKeyPath(artifactPath));
            std::string hash;
            for (std::string line; std::getline(keyStream, line);)
                if (line.starts_with("key=")) { hash = line.substr(4); break; }
            if (hash.empty())
                throw std::runtime_error("GTAO shader cache-key sidecar is missing or invalid");
            Shader = shaderManager.LoadShaderFile(ShaderCacheKey{
                .Source = "Modern/SceneGtao.hlsl", .Variant = "BASE",
                .Stage = nvrhi::ShaderType::Compute, .EntryPoint = "CS",
                .TargetProfile = "cs_6_0", .ContentHash = hash,
                .Backend = desc.Backend, .BinaryFormat = binaryFormat }, artifactPath, "Scene GTAO CS");
            nvrhi::BindingLayoutDesc layout;
            layout.setVisibility(nvrhi::ShaderType::Compute).setRegisterSpaceAndDescriptorSet(3)
                .addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(GtaoConstants)))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
                .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
                .addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
            Layout = Device->createBindingLayout(layout);
            nvrhi::ComputePipelineDesc pipeline;
            pipeline.setComputeShader(Shader).addBindingLayout(Layout);
            Pipeline = Device->createComputePipeline(pipeline);
            if (!Shader || !Layout || !Pipeline)
                throw std::runtime_error("NVRHI failed to create GTAO immutable state");
        }

        nvrhi::IBindingSet* GetBindings(const BindingKey& key)
        {
            const auto found = Bindings.find(key);
            if (found != Bindings.end()) return found->second;
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(GtaoConstants)))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(0, key.Textures[0]))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(1, key.Textures[1]))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(2, key.Textures[2]))
                .addItem(nvrhi::BindingSetItem::Texture_UAV(0, key.Textures[3]));
            auto binding = Device->createBindingSet(desc, Layout);
            if (!binding) throw std::runtime_error("NVRHI failed to bind GTAO textures");
            return Bindings.emplace(key, std::move(binding)).first->second;
        }

        void Retire()
        {
            if (Bindings.empty()) return;
            Retired.push_back({ LastSubmission, std::move(Bindings) });
            Bindings.clear();
        }

        void AddStage(RenderGraph& graph, const char* name, uint32_t operation,
            TextureHandle depth, TextureHandle normal, TextureHandle input, TextureHandle output)
        {
            graph.AddPass<PassData>(name, RenderGraphQueue::Compute,
                [=](RenderGraphBuilder& builder, PassData& data)
                {
                    data.Depth = builder.ShaderResource(depth);
                    data.Normal = builder.ShaderResource(normal);
                    data.Input = builder.ShaderResource(input);
                    data.Output = builder.Write(output);
                    data.Operation = operation;
                },
                [this](RenderGraphContext& context, const PassData& data)
                {
                    GtaoConstants constants = FrameConstants;
                    constants.Parameters.z = static_cast<float>(
                        (static_cast<uint32_t>(constants.Parameters.z) & ~3u) | data.Operation);
                    BindingKey key{{ context.GetTexture(data.Depth, RenderGraphAccess::Read),
                        context.GetTexture(data.Normal, RenderGraphAccess::Read),
                        context.GetTexture(data.Input, RenderGraphAccess::Read),
                        context.GetTexture(data.Output, RenderGraphAccess::Write) }};
                    nvrhi::ComputeState state;
                    state.setPipeline(Pipeline).addBindingSet(GetBindings(key));
                    auto* commands = context.GetCommandList();
                    commands->setComputeState(state);
                    commands->setPushConstants(&constants, sizeof(constants));
                    const uint32_t width = data.Operation == 2 ? constants.Dimensions.x : constants.Dimensions.z;
                    const uint32_t height = data.Operation == 2 ? constants.Dimensions.y : constants.Dimensions.w;
                    commands->dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
                    bFrameRecorded = true;
                });
        }

        nvrhi::DeviceHandle Device;
        Scope<ShaderManager> OwnedShaderManager;
        nvrhi::ShaderHandle Shader;
        nvrhi::BindingLayoutHandle Layout;
        nvrhi::ComputePipelineHandle Pipeline;
        BindingMap Bindings;
        std::vector<RetiredBindings> Retired;
        SceneAmbientOcclusionSettings Settings;
        GtaoConstants FrameConstants;
        uint64_t LastSubmission = 0;
        bool bGraphRebuild = true;
        bool bFrameRecorded = false;
    };

    SceneAmbientOcclusionPass::SceneAmbientOcclusionPass(const SceneAmbientOcclusionPassDesc& desc)
        : mImpl(CreateScope<Impl>(desc)) {}
    SceneAmbientOcclusionPass::~SceneAmbientOcclusionPass() = default;

    void SceneAmbientOcclusionPass::PrepareFrame(const SceneAmbientOcclusionSettings& requested,
        const RenderCamera& camera, uint32_t width, uint32_t height, uint64_t completedSubmission)
    {
        if (width == 0 || height == 0 || std::abs(glm::determinant(camera.ViewProjection)) < 1e-12f)
            throw std::invalid_argument("GTAO requires valid native dimensions and an invertible raster camera");
        Collect(completedSubmission);
        mImpl->bGraphRebuild |= requested.bEnabled != mImpl->Settings.bEnabled ||
            requested.Technique != mImpl->Settings.Technique ||
            width != mImpl->FrameConstants.Dimensions.x || height != mImpl->FrameConstants.Dimensions.y;
        mImpl->Settings = requested;
        mImpl->Settings.Radius = std::isfinite(requested.Radius) ? std::clamp(requested.Radius, 0.001f, 100.0f) : 1.0f;
        mImpl->Settings.Intensity = std::isfinite(requested.Intensity) ? std::clamp(requested.Intensity, 0.0f, 4.0f) : 1.0f;
        mImpl->Settings.NormalBias = std::isfinite(requested.NormalBias)
            ? std::clamp(requested.NormalBias, 0.0f, mImpl->Settings.Radius * 0.25f) : 0.01f;
        mImpl->Settings.SliceCount = std::clamp(requested.SliceCount, 2u, 8u);
        mImpl->Settings.StepsPerSide = std::clamp(requested.StepsPerSide, 2u, 16u);
        mImpl->Settings.SampleCount = std::clamp(requested.SampleCount, 8u, 64u);
        mImpl->FrameConstants.InverseViewProjection = glm::inverse(camera.ViewProjection);
        mImpl->FrameConstants.CameraPositionRadius = glm::vec4(camera.Position, mImpl->Settings.Radius);
        mImpl->FrameConstants.Dimensions = { width, height, (width + 1u) / 2u, (height + 1u) / 2u };
        const glm::mat4 inverseView = glm::inverse(camera.View);
        const glm::vec3 forward = glm::normalize(-glm::vec3(inverseView[2]));
        mImpl->FrameConstants.ViewForwardIntensity = glm::vec4(forward, mImpl->Settings.Intensity);
        mImpl->FrameConstants.Parameters = { std::abs(camera.Projection[1][1]) * static_cast<float>(height) * 0.5f,
            mImpl->Settings.NormalBias,
            static_cast<float>((mImpl->Settings.SliceCount << 2u) | (mImpl->Settings.StepsPerSide << 6u) |
                (mImpl->Settings.Technique == SceneAmbientOcclusionTechnique::SSAO ? 1u << 11u : 0u) |
                (mImpl->Settings.SampleCount << 12u) | (mImpl->Settings.bFastUpsample ? 1u << 19u : 0u)),
            std::abs(camera.Projection[3][3]) < 0.5f ? 1.0f : 0.0f };
    }

    TextureHandle SceneAmbientOcclusionPass::AddPasses(RenderGraph& graph, TextureHandle depth,
        TextureHandle packedNormalRoughnessAo)
    {
        mImpl->Retire();
        mImpl->bGraphRebuild = false;
        if (!mImpl->Settings.bEnabled) return {};
        if (!depth || !packedNormalRoughnessAo)
            throw std::invalid_argument("GTAO requires raster depth and packed normal inputs");
        const auto dimensions = mImpl->FrameConstants.Dimensions;
        const auto raw = graph.CreateTexture(RenderGraphTextureDesc{ .DebugName = "GTAO Half Visibility",
            .Width = dimensions.z, .Height = dimensions.w, .Format = nvrhi::Format::R16_FLOAT });
        const auto filtered = graph.CreateTexture(RenderGraphTextureDesc{ .DebugName = "GTAO Half Bilateral",
            .Width = dimensions.z, .Height = dimensions.w, .Format = nvrhi::Format::R16_FLOAT });
        const auto output = graph.CreateTexture(RenderGraphTextureDesc{ .DebugName = "GTAO Native Visibility",
            .Width = dimensions.x, .Height = dimensions.y, .Format = nvrhi::Format::R8_UNORM });
        mImpl->AddStage(graph, mImpl->Settings.Technique == SceneAmbientOcclusionTechnique::SSAO
            ? "SSAONormalObscurance" : "GTAOHorizonIntegration", 0, depth, packedNormalRoughnessAo, depth, raw);
        const bool bSsao = mImpl->Settings.Technique == SceneAmbientOcclusionTechnique::SSAO;
        mImpl->AddStage(graph, bSsao ? "SSAOBilateralDenoise" : "GTAOBilateralDenoise",
            1, depth, packedNormalRoughnessAo, raw, filtered);
        mImpl->AddStage(graph, bSsao ? "SSAOBilateralUpsample" : "GTAOBilateralUpsample",
            2, depth, packedNormalRoughnessAo, filtered, output);
        return output;
    }
    bool SceneAmbientOcclusionPass::ConsumeGraphRebuildRequest() noexcept
    {
        const bool result = mImpl->bGraphRebuild;
        mImpl->bGraphRebuild = false;
        return result;
    }
    void SceneAmbientOcclusionPass::NotifyFrameSubmitted(uint64_t submission) noexcept
    {
        if (mImpl->bFrameRecorded) mImpl->LastSubmission = submission;
        mImpl->bFrameRecorded = false;
    }
    void SceneAmbientOcclusionPass::AbortFrame() noexcept { mImpl->bFrameRecorded = false; }
    void SceneAmbientOcclusionPass::Collect(uint64_t completedSubmission)
    {
        std::erase_if(mImpl->Retired, [completedSubmission](const Impl::RetiredBindings& state)
            { return state.Submission <= completedSubmission; });
    }
    void SceneAmbientOcclusionPass::Clear(uint64_t completedSubmission)
    {
        mImpl->Retire();
        mImpl->bGraphRebuild = true;
        Collect(completedSubmission);
    }
    std::function<void()> SceneAmbientOcclusionPass::CaptureGraphState()
    {
        return [impl = mImpl.get(), bindings = mImpl->Bindings, retired = mImpl->Retired]() mutable noexcept
        { impl->Bindings.swap(bindings); impl->Retired.swap(retired); };
    }
    void SceneAmbientOcclusionPass::ClearGraphResourceCaches() noexcept
    {
        mImpl->Bindings.clear();
        for (auto& retired : mImpl->Retired) retired.Bindings.clear();
    }
}
