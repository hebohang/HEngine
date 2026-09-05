#include "Hepch.h"

#include "Runtime/UnitTest/MaterialPipelineKeyTests.h"

#include "Runtime/Graphics/Material/MaterialPipelineKey.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace HEngine::Tests
{
    namespace
    {
        struct MaterialInstanceInputs
        {
            float ScalarParameter = 0.0f;
            std::string TexturePath;
        };

        using MaterialPipelineKeyBuilder = GraphicsPipelineKey (*)(
            const MaterialPipelineDesc&) noexcept;

        static_assert(std::is_same_v<
            decltype(&BuildMaterialPipelineKey),
            MaterialPipelineKeyBuilder>);
        static_assert(!std::is_invocable_v<
            MaterialPipelineKeyBuilder,
            const MaterialPipelineDesc&,
            const MaterialInstanceInputs&>);

        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        [[nodiscard]] MaterialPipelineDesc MakePipelineDesc()
        {
            return {
                .Program = {
                    .RecursiveContentHash = "recursive-source-hash:a1b2c3",
                    .ArtifactIdentityHash = "artifact-set-hash:102030",
                    .Generation = 7,
                    .MaterialAbiVersion = 2,
                    .BindingSignature = "binding-signature:d4e5f6",
                },
                .Pass = MaterialPipelinePass::ForwardOpaque,
                .VertexPath = MaterialPipelineVertexPath::CpuStatic,
                .DescriptorTier = MaterialPipelineDescriptorTier::Baseline,
                .InputLayout = { .Value = 0x535441544943ull },
                .BlendMode = MaterialPipelineBlendMode::Opaque,
                .DepthWrite = MaterialPipelineDepthWrite::Enabled,
                .CullMode = MaterialPipelineCullMode::Back,
                .Sidedness = MaterialPipelineSidedness::SingleSided,
                .AlphaMode = MaterialPipelineAlphaMode::Opaque,
                .Framebuffer = {
                    .ColorFormat = nvrhi::Format::RGBA16_FLOAT,
                    .DepthFormat = nvrhi::Format::D32,
                    .SampleCount = 1,
                    .SampleQuality = 0,
                },
            };
        }
    }

    void MaterialPipelineKeyContract()
    {
        const MaterialPipelineDesc base = MakePipelineDesc();
        const GraphicsPipelineKey baseKey = BuildMaterialPipelineKey(base);
        Require(static_cast<bool>(baseKey), "Material pipeline key must be non-zero.");
        Require(BuildMaterialPipelineKey(base) == baseKey,
            "Identical material pipeline descriptions produced unstable keys.");

        auto requireDistinctField = [&base, baseKey](
            MaterialPipelineDesc changed,
            std::string_view field)
        {
            Require(changed != base,
                std::string("Material pipeline test did not change field: ") + std::string(field));
            Require(BuildMaterialPipelineKey(changed) != baseKey,
                std::string("Material pipeline key omitted field: ") + std::string(field));
        };

        MaterialPipelineDesc changed = base;
        changed.Program.RecursiveContentHash = "recursive-source-hash:changed";
        requireDistinctField(std::move(changed), "Program.RecursiveContentHash");
        changed = base;
        changed.Program.ArtifactIdentityHash = "artifact-set-hash:changed";
        requireDistinctField(std::move(changed), "Program.ArtifactIdentityHash");
        changed = base;
        changed.Program.Generation = 8;
        requireDistinctField(std::move(changed), "Program.Generation");
        changed = base;
        changed.Program.MaterialAbiVersion = 3;
        requireDistinctField(std::move(changed), "Program.MaterialAbiVersion");
        changed = base;
        changed.Program.BindingSignature = "binding-signature:changed";
        requireDistinctField(std::move(changed), "Program.BindingSignature");
        changed = base;
        changed.Pass = MaterialPipelinePass::ForwardTransparent;
        requireDistinctField(std::move(changed), "Pass");
        changed = base;
        changed.VertexPath = MaterialPipelineVertexPath::GpuDriven;
        requireDistinctField(std::move(changed), "VertexPath");
        changed = base;
        changed.DescriptorTier = MaterialPipelineDescriptorTier::HeapDirect;
        requireDistinctField(std::move(changed), "DescriptorTier");
        changed = base;
        changed.InputLayout.Value = 0x534b494e4e4544ull;
        requireDistinctField(std::move(changed), "InputLayout");
        changed = base;
        changed.BlendMode = MaterialPipelineBlendMode::AlphaBlend;
        requireDistinctField(std::move(changed), "BlendMode");
        changed = base;
        changed.DepthWrite = MaterialPipelineDepthWrite::Disabled;
        requireDistinctField(std::move(changed), "DepthWrite");
        changed = base;
        changed.CullMode = MaterialPipelineCullMode::None;
        requireDistinctField(std::move(changed), "CullMode");
        changed = base;
        changed.Sidedness = MaterialPipelineSidedness::DoubleSided;
        requireDistinctField(std::move(changed), "Sidedness");
        changed = base;
        changed.AlphaMode = MaterialPipelineAlphaMode::Mask;
        requireDistinctField(std::move(changed), "AlphaMode");
        changed = base;
        changed.Framebuffer.ColorFormat = nvrhi::Format::RGBA8_UNORM;
        requireDistinctField(std::move(changed), "Framebuffer.ColorFormat");
        changed = base;
        changed.Framebuffer.DepthFormat = nvrhi::Format::D24S8;
        requireDistinctField(std::move(changed), "Framebuffer.DepthFormat");
        changed = base;
        changed.Framebuffer.SampleCount = 4;
        requireDistinctField(std::move(changed), "Framebuffer.SampleCount");
        changed = base;
        changed.Framebuffer.SampleQuality = 1;
        requireDistinctField(std::move(changed), "Framebuffer.SampleQuality");

        MaterialInstanceInputs instanceInputs{
            .ScalarParameter = 0.25f,
            .TexturePath = "Assets/Textures/First.png",
        };
        const GraphicsPipelineKey beforeInstanceChange = BuildMaterialPipelineKey(base);
        instanceInputs.ScalarParameter = 0.75f;
        instanceInputs.TexturePath = "Assets/Textures/Second.png";
        const GraphicsPipelineKey afterInstanceChange = BuildMaterialPipelineKey(base);
        Require(beforeInstanceChange == afterInstanceChange,
            "Per-instance parameter values or texture paths affected the material pipeline key.");
    }
}
