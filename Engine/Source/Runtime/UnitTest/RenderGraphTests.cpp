#include "Hepch.h"

#include "Runtime/UnitTest/RenderGraphTests.h"

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        struct TexturePassData
        {
            TextureHandle Texture;
        };

        struct BufferPassData
        {
            BufferHandle Buffer;
        };

        struct TexturePairPassData
        {
            TextureHandle Input;
            TextureHandle Output;
        };

        struct LifetimePassData
        {
            TextureHandle Texture;
            TextureHandle PersistentTexture;
            BufferHandle Buffer;
        };

        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        [[nodiscard]] RenderGraphTextureDesc MakeTextureDesc(std::string name)
        {
            RenderGraphTextureDesc desc;
            desc.DebugName = std::move(name);
            return desc;
        }

        [[nodiscard]] RenderGraphBufferDesc MakeBufferDesc(std::string name)
        {
            RenderGraphBufferDesc desc;
            desc.DebugName = std::move(name);
            desc.ByteSize = 256;
            desc.StructStride = 16;
            return desc;
        }

        [[nodiscard]] bool ContainsError(const RenderGraph& graph, std::string_view text)
        {
            return std::any_of(
                graph.GetCompileErrors().begin(),
                graph.GetCompileErrors().end(),
                [text](const std::string& error)
                {
                    return error.find(text) != std::string::npos;
                });
        }

        void StableTopologicalSort()
        {
            RenderGraph graph;
            TextureHandle produced;
            TextureHandle independent;
            TextureHandle finalOutput;

            const RenderGraphPassHandle producer = graph.AddPass<TexturePassData>(
                "Producer",
                [&produced](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.CreateTexture(MakeTextureDesc("Produced"));
                    produced = builder.RenderTarget(data.Texture);
                },
                [](RenderGraphContext&, const TexturePassData&) {});

            const RenderGraphPassHandle independentPass = graph.AddPass<TexturePassData>(
                "Independent",
                [&independent](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.CreateTexture(MakeTextureDesc("IndependentOutput"));
                    independent = builder.RenderTarget(data.Texture);
                    builder.Export(independent);
                },
                [](RenderGraphContext&, const TexturePassData&) {});

            const RenderGraphPassHandle consumer = graph.AddPass<TexturePairPassData>(
                "Consumer",
                [&produced, &finalOutput](RenderGraphBuilder& builder, TexturePairPassData& data)
                {
                    data.Input = builder.ShaderResource(produced);
                    data.Output = builder.CreateTexture(MakeTextureDesc("FinalOutput"));
                    finalOutput = builder.RenderTarget(data.Output);
                    builder.Export(finalOutput);
                },
                [](RenderGraphContext&, const TexturePairPassData&) {});

            Require(graph.Compile(), "A valid RenderGraph failed compilation.");
            Require(
                graph.GetCompiledPassNames() ==
                    std::vector<std::string>{ "Producer", "Independent", "Consumer" },
                "RenderGraph topological sorting was not stable.");
            Require(!graph.IsPassCulled(producer) &&
                    !graph.IsPassCulled(independentPass) &&
                    !graph.IsPassCulled(consumer),
                "A reachable RenderGraph pass was culled.");
        }

        void DependencyCycleDetection()
        {
            RenderGraph graph;
            const TextureHandle first = graph.CreateTexture(MakeTextureDesc("CycleA"));
            const TextureHandle second = graph.CreateTexture(MakeTextureDesc("CycleB"));

            graph.AddPass<TexturePairPassData>(
                "CycleA",
                [first, second](RenderGraphBuilder& builder, TexturePairPassData& data)
                {
                    data.Input = builder.ShaderResource(second);
                    data.Output = builder.RenderTarget(first);
                },
                [](RenderGraphContext&, const TexturePairPassData&) {});
            graph.AddPass<TexturePairPassData>(
                "CycleB",
                [first, second](RenderGraphBuilder& builder, TexturePairPassData& data)
                {
                    data.Input = builder.ShaderResource(first);
                    data.Output = builder.RenderTarget(second);
                },
                [](RenderGraphContext&, const TexturePairPassData&) {});
            graph.Export(first);

            Require(!graph.Compile(), "A cyclic RenderGraph unexpectedly compiled.");
            Require(ContainsError(graph, "cycle"),
                "A cyclic RenderGraph did not report a cycle diagnostic.");
        }

        void PassCulling()
        {
            RenderGraph graph;
            const RenderGraphPassHandle orphan = graph.AddPass<TexturePassData>(
                "Orphan",
                [](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.CreateTexture(MakeTextureDesc("OrphanOutput"));
                    builder.RenderTarget(data.Texture);
                },
                [](RenderGraphContext&, const TexturePassData&) {});
            const RenderGraphPassHandle live = graph.AddPass<TexturePassData>(
                "Live",
                [](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.CreateTexture(MakeTextureDesc("LiveOutput"));
                    builder.RenderTarget(data.Texture);
                    builder.Export(data.Texture);
                },
                [](RenderGraphContext&, const TexturePassData&) {});

            Require(graph.Compile(), "The pass-culling graph failed compilation.");
            Require(graph.IsPassCulled(orphan), "An unobservable pass was not culled.");
            Require(!graph.IsPassCulled(live), "An exported-output pass was culled.");
            Require(graph.GetCompiledPassNames() == std::vector<std::string>{ "Live" },
                "A culled pass remained in the execution order.");
        }

        void HandleGenerationValidation()
        {
            static_assert(!std::is_same_v<TextureHandle, BufferHandle>);

            RenderGraph graph;
            const TextureHandle staleTexture = graph.CreateTexture(MakeTextureDesc("OldTexture"));
            const BufferHandle staleBuffer = graph.CreateBuffer(MakeBufferDesc("OldBuffer"));
            graph.Clear();
            const TextureHandle liveTexture = graph.CreateTexture(MakeTextureDesc("NewTexture"));
            const BufferHandle liveBuffer = graph.CreateBuffer(MakeBufferDesc("NewBuffer"));

            Require(staleTexture.Index == liveTexture.Index &&
                    staleTexture.Generation != liveTexture.Generation,
                "A recycled texture slot did not receive a new generation.");
            Require(staleBuffer.Index == liveBuffer.Index &&
                    staleBuffer.Generation != liveBuffer.Generation,
                "A recycled buffer slot did not receive a new generation.");
            Require(!graph.IsValid(staleTexture) && !graph.IsValid(staleBuffer),
                "A stale RenderGraph handle remained valid.");
            Require(graph.IsValid(liveTexture) && graph.IsValid(liveBuffer),
                "A current RenderGraph handle was rejected.");

            bool bRejectedStaleTexture = false;
            try
            {
                graph.AddPass<TexturePassData>(
                    "StaleTexture",
                    [staleTexture](RenderGraphBuilder& builder, TexturePassData& data)
                    {
                        data.Texture = builder.ShaderResource(staleTexture);
                    },
                    [](RenderGraphContext&, const TexturePassData&) {});
            }
            catch (const RenderGraphValidationError&)
            {
                bRejectedStaleTexture = true;
            }
            Require(bRejectedStaleTexture,
                "A pass setup accepted a stale texture handle.");

            bool bRejectedStaleBuffer = false;
            try
            {
                graph.AddPass<BufferPassData>(
                    "StaleBuffer",
                    [staleBuffer](RenderGraphBuilder& builder, BufferPassData& data)
                    {
                        data.Buffer = builder.ShaderResource(staleBuffer);
                    },
                    [](RenderGraphContext&, const BufferPassData&) {});
            }
            catch (const RenderGraphValidationError&)
            {
                bRejectedStaleBuffer = true;
            }
            Require(bRejectedStaleBuffer,
                "A pass setup accepted a stale buffer handle.");
        }

        void ResourceLifetimeAnalysis()
        {
            RenderGraph graph;
            TextureHandle sharedTexture;
            TextureHandle persistentTexture;
            BufferHandle intermediateBuffer;

            graph.AddPass<LifetimePassData>(
                "LifetimeProducer",
                [&sharedTexture, &persistentTexture](RenderGraphBuilder& builder, LifetimePassData& data)
                {
                    data.Texture = builder.CreateTexture(MakeTextureDesc("LifetimeTexture"));
                    sharedTexture = builder.RenderTarget(data.Texture);
                    data.PersistentTexture = builder.CreateTexture(
                        MakeTextureDesc("PersistentTexture"),
                        RenderGraphResourceLifetime::Persistent);
                    persistentTexture = builder.RenderTarget(data.PersistentTexture);
                },
                [](RenderGraphContext&, const LifetimePassData&) {});
            graph.AddPass<LifetimePassData>(
                "LifetimeMiddle",
                [&sharedTexture, &intermediateBuffer](RenderGraphBuilder& builder, LifetimePassData& data)
                {
                    data.Texture = builder.ShaderResource(sharedTexture);
                    data.Buffer = builder.CreateBuffer(MakeBufferDesc("LifetimeBuffer"));
                    intermediateBuffer = builder.Write(
                        data.Buffer,
                        RenderGraphUsage::UnorderedAccess);
                },
                [](RenderGraphContext&, const LifetimePassData&) {});
            graph.AddPass<LifetimePassData>(
                "LifetimeConsumer",
                [&sharedTexture, &intermediateBuffer](RenderGraphBuilder& builder, LifetimePassData& data)
                {
                    data.Texture = builder.ShaderResource(sharedTexture);
                    data.Buffer = builder.ShaderResource(intermediateBuffer);
                    builder.SetSideEffect();
                },
                [](RenderGraphContext&, const LifetimePassData&) {});

            Require(graph.Compile(), "The resource-lifetime graph failed compilation.");
            Require(graph.GetLifetime(sharedTexture) == RenderGraphResourceLifetime::Transient,
                "A transient resource has the wrong lifetime class.");
            Require(graph.GetLifetime(persistentTexture) == RenderGraphResourceLifetime::Persistent,
                "A persistent resource has the wrong lifetime class.");
            Require(graph.GetLifetime(intermediateBuffer) == RenderGraphResourceLifetime::Transient,
                "A transient buffer has the wrong lifetime class.");

            const RenderGraphResourceLifetimeInfo textureLifetime =
                graph.GetLifetimeInfo(sharedTexture);
            const RenderGraphResourceLifetimeInfo bufferLifetime =
                graph.GetLifetimeInfo(intermediateBuffer);
            const RenderGraphResourceLifetimeInfo persistentLifetime =
                graph.GetLifetimeInfo(persistentTexture);
            Require(textureLifetime.FirstUse == 0 && textureLifetime.LastUse == 2,
                "Texture first/last-use analysis returned the wrong interval.");
            Require(bufferLifetime.FirstUse == 1 && bufferLifetime.LastUse == 2,
                "Buffer first/last-use analysis returned the wrong interval.");
            Require(persistentLifetime.FirstUse == 0 && persistentLifetime.LastUse == 0,
                "Persistent-resource first/last-use analysis returned the wrong interval.");

            const std::string debugDump = graph.BuildDebugDump();
            Require(debugDump.find("LifetimeProducer") != std::string::npos &&
                    debugDump.find("LifetimeTexture") != std::string::npos &&
                    debugDump.find("first=0 last=2") != std::string::npos,
                "RenderGraph debug dump omitted execution or lifetime information.");
        }

        void ImportedResourceStateValidation()
        {
            RenderGraph invalidGraph;
            const TextureHandle invalidTexture = invalidGraph.ImportTexture(
                MakeTextureDesc("InvalidImportedTexture"),
                nvrhi::TextureHandle{},
                nvrhi::ResourceStates::Unknown,
                nvrhi::ResourceStates::Present);
            const BufferHandle invalidBuffer = invalidGraph.ImportBuffer(
                MakeBufferDesc("InvalidImportedBuffer"),
                nvrhi::BufferHandle{},
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Unknown);
            const AccelerationStructureHandle accelerationStructure =
                invalidGraph.ImportAccelerationStructure(
                    { .DebugName = "ImportedAccelerationStructure" },
                    nvrhi::rt::AccelStructHandle{},
                    nvrhi::ResourceStates::AccelStructRead,
                    nvrhi::ResourceStates::AccelStructRead);
            Require(invalidGraph.IsValid(invalidTexture) &&
                    invalidGraph.IsValid(invalidBuffer) &&
                    invalidGraph.IsValid(accelerationStructure),
                "Imported resource handles were invalid before graph compilation.");
            Require(!invalidGraph.Compile(),
                "Imported resources with unknown states unexpectedly compiled.");
            Require(ContainsError(invalidGraph, "explicit initial and final states"),
                "Invalid imported states did not produce a useful diagnostic.");

            RenderGraph validGraph;
            const TextureHandle backbuffer = validGraph.ImportTexture(
                MakeTextureDesc("Backbuffer"),
                nvrhi::TextureHandle{},
                nvrhi::ResourceStates::Common,
                nvrhi::ResourceStates::Present);
            validGraph.AddPass<TexturePassData>(
                "Present",
                [backbuffer](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.Present(backbuffer);
                },
                [](RenderGraphContext&, const TexturePassData&) {});

            Require(validGraph.Compile(),
                "An imported resource with explicit states failed compilation.");
            Require(validGraph.GetLifetime(backbuffer) == RenderGraphResourceLifetime::Imported,
                "An imported resource has the wrong lifetime class.");
            const RenderGraphResourceLifetimeInfo lifetime = validGraph.GetLifetimeInfo(backbuffer);
            Require(lifetime.FirstUse == 0 && lifetime.LastUse == 0,
                "Imported-resource lifetime analysis returned the wrong interval.");
        }

        void UsageStateMapping()
        {
            const auto bits = [](nvrhi::ResourceStates states)
            {
                return static_cast<uint32_t>(states);
            };

            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::None) ==
                    nvrhi::ResourceStates::Unknown,
                "No RenderGraph usage did not map to Unknown state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::RenderTarget) ==
                    nvrhi::ResourceStates::RenderTarget,
                "RenderTarget usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::DepthRead) ==
                    nvrhi::ResourceStates::DepthRead,
                "DepthRead usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::DepthWrite) ==
                    nvrhi::ResourceStates::DepthWrite,
                "DepthWrite usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::ShaderResource) ==
                    nvrhi::ResourceStates::ShaderResource,
                "ShaderResource usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::UnorderedAccess) ==
                    nvrhi::ResourceStates::UnorderedAccess,
                "UnorderedAccess usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::CopySource) ==
                    nvrhi::ResourceStates::CopySource,
                "CopySource usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::CopyDest) ==
                    nvrhi::ResourceStates::CopyDest,
                "CopyDest usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::Present) ==
                    nvrhi::ResourceStates::Present,
                "Present usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::VertexBuffer) ==
                    nvrhi::ResourceStates::VertexBuffer,
                "VertexBuffer usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::IndexBuffer) ==
                    nvrhi::ResourceStates::IndexBuffer,
                "IndexBuffer usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::ConstantBuffer) ==
                    nvrhi::ResourceStates::ConstantBuffer,
                "ConstantBuffer usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::IndirectArgument) ==
                    nvrhi::ResourceStates::IndirectArgument,
                "IndirectArgument usage mapped to the wrong NVRHI state.");

            const uint32_t expectedCombined =
                bits(nvrhi::ResourceStates::DepthRead) |
                bits(nvrhi::ResourceStates::ShaderResource) |
                bits(nvrhi::ResourceStates::CopySource);
            const uint32_t actualCombined = bits(RenderGraph::MapUsageToResourceStates(
                RenderGraphUsage::DepthRead |
                RenderGraphUsage::ShaderResource |
                RenderGraphUsage::CopySource));
            Require(actualCombined == expectedCombined,
                "Combined RenderGraph usages lost an NVRHI resource state.");
        }

        void BufferUsageValidation()
        {
            RenderGraph graph;
            const TextureHandle texture = graph.CreateTexture(MakeTextureDesc("BufferOnlyMisuse"));
            bool bRejectedTextureAsVertexBuffer = false;
            try
            {
                graph.AddPass<TexturePassData>(
                    "InvalidVertexBufferTexture",
                    [texture](RenderGraphBuilder& builder, TexturePassData& data)
                    {
                        data.Texture = builder.Read(texture, RenderGraphUsage::VertexBuffer);
                    },
                    [](RenderGraphContext&, const TexturePassData&) {});
            }
            catch (const RenderGraphValidationError&)
            {
                bRejectedTextureAsVertexBuffer = true;
            }
            Require(bRejectedTextureAsVertexBuffer,
                "RenderGraph accepted a texture declared with VertexBuffer usage.");

            bool bRejectedTextureAsIndirectArguments = false;
            try
            {
                graph.AddPass<TexturePassData>(
                    "InvalidIndirectArgumentTexture",
                    [texture](RenderGraphBuilder& builder, TexturePassData& data)
                    {
                        data.Texture = builder.Read(
                            texture,
                            RenderGraphUsage::IndirectArgument);
                    },
                    [](RenderGraphContext&, const TexturePassData&) {});
            }
            catch (const RenderGraphValidationError&)
            {
                bRejectedTextureAsIndirectArguments = true;
            }
            Require(bRejectedTextureAsIndirectArguments,
                "RenderGraph accepted a texture declared with IndirectArgument usage.");

            const BufferHandle buffer = graph.CreateBuffer(MakeBufferDesc("ReadOnlyBuffer"),
                RenderGraphResourceLifetime::Transient, true);
            bool bRejectedWriteAsIndexBuffer = false;
            try
            {
                graph.AddPass<BufferPassData>(
                    "InvalidIndexBufferWrite",
                    [buffer](RenderGraphBuilder& builder, BufferPassData& data)
                    {
                        data.Buffer = builder.Write(buffer, RenderGraphUsage::IndexBuffer);
                    },
                    [](RenderGraphContext&, const BufferPassData&) {});
            }
            catch (const RenderGraphValidationError&)
            {
                bRejectedWriteAsIndexBuffer = true;
            }
            Require(bRejectedWriteAsIndexBuffer,
                "RenderGraph accepted write access with read-only IndexBuffer usage.");
        }

        void ReadWriteRenderTargetOrdering()
        {
            RenderGraph graph;
            TextureHandle sceneColor;

            const RenderGraphPassHandle opaque = graph.AddPass<TexturePassData>(
                "Opaque",
                [&sceneColor](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.CreateTexture(MakeTextureDesc("HdrSceneColor"));
                    sceneColor = builder.RenderTarget(data.Texture);
                },
                [](RenderGraphContext&, const TexturePassData&) {});
            const RenderGraphPassHandle transparent = graph.AddPass<TexturePassData>(
                "TransparentBlend",
                [&sceneColor](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.ReadWrite(
                        sceneColor,
                        RenderGraphUsage::RenderTarget);
                },
                [](RenderGraphContext&, const TexturePassData&) {});
            const RenderGraphPassHandle toneMapping = graph.AddPass<TexturePassData>(
                "ToneMapping",
                [&sceneColor](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.ShaderResource(sceneColor);
                    builder.SetSideEffect();
                },
                [](RenderGraphContext&, const TexturePassData&) {});

            Require(graph.Compile(),
                "A read-write render-target blend graph failed compilation.");
            Require(graph.GetCompiledPassNames() ==
                    std::vector<std::string>{ "Opaque", "TransparentBlend", "ToneMapping" },
                "ReadWrite RenderTarget did not preserve producer/blend/consumer ordering.");
            Require(!graph.IsPassCulled(opaque) &&
                    !graph.IsPassCulled(transparent) &&
                    !graph.IsPassCulled(toneMapping),
                "A live ReadWrite RenderTarget pass was culled.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::RenderTarget) ==
                    nvrhi::ResourceStates::RenderTarget,
                "ReadWrite RenderTarget usage no longer maps to RenderTarget state.");
        }

        void DepthShaderResourceThenDepthReadOrdering()
        {
            RenderGraph graph;
            const TextureHandle sceneDepth = graph.CreateTexture(
                MakeTextureDesc("ShaderReadableDepth"),
                RenderGraphResourceLifetime::Transient,
                true);
            TextureHandle depthSnapshot;

            graph.AddPass<TexturePassData>(
                "DepthSnapshot",
                [sceneDepth, &depthSnapshot](
                    RenderGraphBuilder& builder,
                    TexturePassData& data)
                {
                    data.Texture = builder.ShaderResource(sceneDepth);
                    depthSnapshot = builder.CreateTexture(MakeTextureDesc("DepthSnapshot"));
                    depthSnapshot = builder.RenderTarget(depthSnapshot);
                    builder.SetSideEffect();
                },
                [](RenderGraphContext&, const TexturePassData&) {});
            graph.AddPass<TexturePassData>(
                "VFXDepthRead",
                [sceneDepth, depthSnapshot](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.DepthRead(sceneDepth);
                    (void)builder.ShaderResource(depthSnapshot);
                    builder.SetSideEffect();
                },
                [](RenderGraphContext&, const TexturePassData&) {});

            Require(graph.Compile(),
                "Depth ShaderResource followed by read-only DSV use failed compilation.");
            Require(graph.GetCompiledPassNames() ==
                    std::vector<std::string>{ "DepthSnapshot", "VFXDepthRead" },
                "Depth ShaderResource-to-DepthRead ordering was not preserved.");
            Require(RenderGraph::MapUsageToResourceStates(RenderGraphUsage::ShaderResource) ==
                    nvrhi::ResourceStates::ShaderResource &&
                    RenderGraph::MapUsageToResourceStates(RenderGraphUsage::DepthRead) ==
                    nvrhi::ResourceStates::DepthRead,
                "Depth snapshot and read-only DSV usages no longer map to separate states.");
        }

        void ReadBeforeInitializationValidation()
        {
            RenderGraph graph;
            const TextureHandle uninitialized =
                graph.CreateTexture(MakeTextureDesc("Uninitialized"));
            graph.AddPass<TexturePassData>(
                "InvalidRead",
                [uninitialized](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.ShaderResource(uninitialized);
                    builder.SetSideEffect();
                },
                [](RenderGraphContext&, const TexturePassData&) {});

            Require(!graph.Compile(),
                "A read-before-initialization graph unexpectedly compiled.");
            Require(ContainsError(graph, "read before initialization"),
                "A read-before-initialization error had no useful diagnostic.");
        }

        void DiagnosticSnapshotContract()
        {
            static_assert(std::is_copy_constructible_v<RenderGraphDiagnosticSnapshot>);
            static_assert(std::is_same_v<
                decltype(RenderGraphDiagnosticPassSnapshot{}.PassId),
                uint32_t>);
            static_assert(std::is_same_v<
                decltype(RenderGraphDiagnosticResourceSnapshot{}.ResourceId),
                uint32_t>);
            static_assert(std::is_same_v<
                decltype(RenderGraphDiagnosticResourceSnapshot{}.FormatName),
                std::string>);
            static_assert(!std::is_pointer_v<
                decltype(RenderGraphDiagnosticResourceSnapshot{}.ResourceId)>);
            static_assert(!std::is_same_v<
                decltype(RenderGraphDiagnosticResourceSnapshot{}.ResourceId),
                TextureHandle>);
            static_assert(!std::is_same_v<
                decltype(RenderGraphDiagnosticResourceSnapshot{}.ResourceId),
                nvrhi::TextureHandle>);

            RenderGraph graph;
            graph.Resize(640, 360);
            const BufferHandle frameData = graph.CreateBuffer(
                MakeBufferDesc("DiagnosticFrameData"),
                RenderGraphResourceLifetime::Persistent,
                true);
            TextureHandle sceneColor;

            const RenderGraphPassHandle producer = graph.AddPass<LifetimePassData>(
                "DiagnosticProducer",
                [frameData, &sceneColor](
                    RenderGraphBuilder& builder,
                    LifetimePassData& data)
                {
                    RenderGraphTextureDesc desc = MakeTextureDesc("DiagnosticSceneColor");
                    desc.Format = nvrhi::Format::RGBA16_FLOAT;
                    desc.bRelativeToGraph = true;
                    desc.WidthScale = 0.5f;
                    desc.HeightScale = 0.5f;
                    data.Buffer = builder.ShaderResource(frameData);
                    data.Texture = builder.CreateTexture(desc);
                    sceneColor = builder.RenderTarget(data.Texture);
                },
                [](RenderGraphContext&, const LifetimePassData&) {});
            graph.AddPass<TexturePassData>(
                "DiagnosticConsumer",
                [producer, sceneColor](
                    RenderGraphBuilder& builder,
                    TexturePassData& data)
                {
                    builder.DependsOn(producer);
                    data.Texture = builder.ShaderResource(sceneColor);
                    builder.SetSideEffect();
                },
                [](RenderGraphContext&, const TexturePassData&) {});

            Require(graph.Compile(), "The diagnostic snapshot graph failed compilation.");
            const RenderGraphDiagnosticSnapshot snapshot = graph.BuildDiagnosticSnapshot();
            Require(snapshot.bCompiled && !snapshot.bDirty &&
                    snapshot.Width == 640 && snapshot.Height == 360,
                "The diagnostic snapshot omitted graph state or dimensions.");
            Require(snapshot.Passes.size() == 2 &&
                    snapshot.ExecutionOrder == std::vector<uint32_t>{ 0, 1 } &&
                    snapshot.Passes[0].Name == "DiagnosticProducer" &&
                    snapshot.Passes[0].ExecutionIndex == 0 &&
                    snapshot.Passes[1].Name == "DiagnosticConsumer" &&
                    snapshot.Passes[1].ExecutionIndex == 1 &&
                    !snapshot.Passes[0].bCulled && !snapshot.Passes[1].bCulled,
                "The diagnostic snapshot omitted pass execution state.");

            const auto texture = std::find_if(
                snapshot.Resources.begin(),
                snapshot.Resources.end(),
                [](const RenderGraphDiagnosticResourceSnapshot& resource)
                {
                    return resource.Kind == RenderGraphDiagnosticResourceKind::Texture &&
                        resource.Name == "DiagnosticSceneColor";
                });
            Require(texture != snapshot.Resources.end() &&
                    texture->Width == 320 && texture->Height == 180 &&
                    texture->FormatName == "RGBA16_FLOAT" &&
                    texture->PassInterval.FirstUse == 0 &&
                    texture->PassInterval.LastUse == 1 &&
                    HasRenderGraphUsage(
                        texture->RequiredUsage,
                        RenderGraphUsage::RenderTarget) &&
                    HasRenderGraphUsage(
                        texture->RequiredUsage,
                        RenderGraphUsage::ShaderResource),
                "The diagnostic snapshot omitted texture descriptor or lifetime data.");

            const auto buffer = std::find_if(
                snapshot.Resources.begin(),
                snapshot.Resources.end(),
                [](const RenderGraphDiagnosticResourceSnapshot& resource)
                {
                    return resource.Kind == RenderGraphDiagnosticResourceKind::Buffer &&
                        resource.Name == "DiagnosticFrameData";
                });
            Require(buffer != snapshot.Resources.end() &&
                    buffer->Lifetime == RenderGraphResourceLifetime::Persistent &&
                    buffer->ByteSize == 256 && buffer->StructStride == 16,
                "The diagnostic snapshot omitted buffer descriptor data.");
            Require(snapshot.Accesses.size() == 3,
                "The diagnostic snapshot omitted declared resource accesses.");
            const auto bufferAccess = std::find_if(
                snapshot.Accesses.begin(),
                snapshot.Accesses.end(),
                [](const RenderGraphDiagnosticAccessSnapshot& access)
                {
                    return access.PassId == 0 &&
                        access.ResourceKind == RenderGraphDiagnosticResourceKind::Buffer;
                });
            const auto consumerAccess = std::find_if(
                snapshot.Accesses.begin(),
                snapshot.Accesses.end(),
                [](const RenderGraphDiagnosticAccessSnapshot& access)
                {
                    return access.PassId == 1 &&
                        access.ResourceKind == RenderGraphDiagnosticResourceKind::Texture;
                });
            Require(bufferAccess != snapshot.Accesses.end() &&
                    bufferAccess->ResourceId == 0 &&
                    bufferAccess->Access == RenderGraphAccess::Read &&
                    bufferAccess->Usage == RenderGraphUsage::ShaderResource &&
                    !bufferAccess->bHasSubresources &&
                    consumerAccess != snapshot.Accesses.end() &&
                    consumerAccess->ResourceId == 0 &&
                    consumerAccess->Access == RenderGraphAccess::Read &&
                    consumerAccess->Usage == RenderGraphUsage::ShaderResource &&
                    consumerAccess->bHasSubresources,
                "The diagnostic snapshot lost access kind, ID, mode, or usage data.");

            const auto dependency = std::find_if(
                snapshot.Dependencies.begin(),
                snapshot.Dependencies.end(),
                [](const RenderGraphDiagnosticDependencySnapshot& edge)
                {
                    return edge.PrerequisitePassId == 0 && edge.DependentPassId == 1;
                });
            Require(dependency != snapshot.Dependencies.end() && dependency->bExplicit,
                "The diagnostic snapshot omitted or misclassified a pass dependency.");
            Require(snapshot.CompileErrors.empty(),
                "A valid graph diagnostic snapshot reported compile errors.");

            graph.Clear();
            Require(snapshot.Passes.size() == 2 &&
                    snapshot.Resources.size() == 2 &&
                    snapshot.Passes[0].Name == "DiagnosticProducer" &&
                    texture->Name == "DiagnosticSceneColor",
                "A retained diagnostic snapshot changed with its source graph.");

            RenderGraph invalidGraph;
            const TextureHandle uninitialized = invalidGraph.CreateTexture(
                MakeTextureDesc("DiagnosticUninitialized"));
            invalidGraph.AddPass<TexturePassData>(
                "DiagnosticInvalidRead",
                [uninitialized](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.ShaderResource(uninitialized);
                    builder.SetSideEffect();
                },
                [](RenderGraphContext&, const TexturePassData&) {});
            Require(!invalidGraph.Compile(),
                "The invalid diagnostic graph unexpectedly compiled.");
            const RenderGraphDiagnosticSnapshot invalidSnapshot =
                invalidGraph.BuildDiagnosticSnapshot();
            Require(!invalidSnapshot.bCompiled && invalidSnapshot.bDirty &&
                    !invalidSnapshot.CompileErrors.empty() &&
                    invalidSnapshot.CompileErrors.front().Message.find(
                        "read before initialization") != std::string::npos,
                "The diagnostic snapshot omitted a structured compile error.");
        }

        void ResizeInvalidatesCompilation()
        {
            RenderGraph graph;
            graph.Resize(100, 80);
            TextureHandle relativeTexture;
            graph.AddPass<TexturePassData>(
                "RelativeOutput",
                [&relativeTexture](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    RenderGraphTextureDesc desc = MakeTextureDesc("RelativeTexture");
                    desc.bRelativeToGraph = true;
                    desc.WidthScale = 0.5f;
                    desc.HeightScale = 0.25f;
                    data.Texture = builder.CreateTexture(desc);
                    relativeTexture = builder.RenderTarget(data.Texture);
                    builder.Export(relativeTexture);
                },
                [](RenderGraphContext&, const TexturePassData&) {});

            Require(graph.Compile(), "The relative-resource graph failed compilation.");
            Require(graph.GetTextureDesc(relativeTexture).Width == 50 &&
                    graph.GetTextureDesc(relativeTexture).Height == 20,
                "A relative texture used the wrong initial dimensions.");

            graph.Resize(320, 180);
            Require(graph.NeedsCompile(), "RenderGraph resize did not invalidate compilation.");
            Require(graph.GetTextureDesc(relativeTexture).Width == 160 &&
                    graph.GetTextureDesc(relativeTexture).Height == 45,
                "A relative texture did not resize with the graph.");
            Require(graph.Compile(), "The resized RenderGraph failed recompilation.");
        }
    }

    void RenderGraphContract()
    {
        StableTopologicalSort();
        DependencyCycleDetection();
        PassCulling();
        HandleGenerationValidation();
        ResourceLifetimeAnalysis();
        ImportedResourceStateValidation();
        UsageStateMapping();
        BufferUsageValidation();
        ReadWriteRenderTargetOrdering();
        DepthShaderResourceThenDepthReadOrdering();
        ReadBeforeInitializationValidation();
        DiagnosticSnapshotContract();
        ResizeInvalidatesCompilation();
    }
}
