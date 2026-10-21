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
            validGraph.SetTextureResource(backbuffer, {}, nvrhi::ResourceStates::ShaderResource);
            Require(validGraph.NeedsCompile(),
                "An imported state update did not invalidate observable final-state analysis.");
            Require(validGraph.Compile(), "Updated imported state failed compilation.");
            validGraph.SetTextureResource(backbuffer, {}, nvrhi::ResourceStates::ShaderResource);
            Require(!validGraph.NeedsCompile(), "An unchanged imported state invalidated the graph cache.");
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

        void FeatureOwnershipContract()
        {
            RenderGraph graph;
            auto add = [&graph](const char* name)
            {
                graph.AddPass<TexturePassData>(name,
                    [](RenderGraphBuilder& builder, TexturePassData&) { builder.SetSideEffect(); },
                    [](RenderGraphContext&, const TexturePassData&) {});
            };
            {
                RenderGraphFeatureScope outer(graph, "test.outer", "outer-1");
                add("Outer");
                try
                {
                    RenderGraphFeatureScope inner(graph, "test.inner", "inner-2");
                    add("Inner");
                    throw std::runtime_error("expected failure");
                }
                catch (const std::runtime_error&) {}
                add("RestoredOuter");
            }
            add("Core");
            Require(graph.Compile(), "Feature ownership graph failed compilation.");
            const auto snapshot = graph.BuildDiagnosticSnapshot();
            Require(snapshot.Passes[0].FeatureTypeId == "test.outer" &&
                    snapshot.Passes[1].FeatureInstanceId == "inner-2" &&
                    snapshot.Passes[2].FeatureInstanceId == "outer-1" &&
                    snapshot.Passes[3].FeatureTypeId.empty(),
                "Nested or failed feature build scopes leaked pass ownership.");
            Require(!snapshot.Passes[0].bHasRecordedCommands,
                "A CPU-only graph claimed an actual recorded queue before recording.");
        }

        void BackendDepthReadContract()
        {
            for (const auto other : { RenderGraphUsage::ShaderResource, RenderGraphUsage::CopySource,
                    RenderGraphUsage::ResolveSource, RenderGraphUsage::Present })
            {
                bool rejected = false;
                try { (void)RenderGraph::MapTextureUsageToResourceStates(RenderGraphUsage::DepthRead | other,
                    nvrhi::Format::D32, nvrhi::GraphicsAPI::D3D12); }
                catch (const RenderGraphValidationError&) { rejected = true; }
                Require(rejected, "D3D12 accepted depth-read combined with an incompatible layout.");
            }
            for (const bool overlap : { false, true })
            {
                RenderGraph graph;
                auto desc = MakeTextureDesc("Depth array");
                desc.Format = nvrhi::Format::D32;
                desc.ArrayLayers = 2;
                const auto texture = graph.CreateTexture(desc, RenderGraphResourceLifetime::Transient, true);
                graph.AddPass<TexturePassData>("Depth mixed ranges",
                    [texture, overlap](RenderGraphBuilder& builder, TexturePassData& data) {
                        data.Texture = builder.DepthRead(texture, { 0, 1, 0, 1 });
                        (void)builder.ShaderResource(texture, { 0, 1, overlap ? 0u : 1u, overlap ? 2u : 1u });
                        builder.SetSideEffect();
                    }, [](RenderGraphContext&, const TexturePassData&) {});
                Require(graph.Compile(), "Portable depth graph failed logical compilation.");
                graph.ValidateBackendResourceUsages(nvrhi::GraphicsAPI::VULKAN);
                bool rejected = false;
                try { graph.ValidateBackendResourceUsages(nvrhi::GraphicsAPI::D3D12); }
                catch (const RenderGraphValidationError&) { rejected = true; }
                Require(rejected == overlap, "D3D12 preparation did not distinguish overlapping and disjoint depth ranges.");
            }
        }

        void OverwrittenContentsCulling()
        {
            Require(RenderGraph::MapTextureUsageToResourceStates(RenderGraphUsage::ShaderResource,
                    nvrhi::Format::D32, nvrhi::GraphicsAPI::D3D12) == nvrhi::ResourceStates::ShaderResource,
                "D3D12 depth sampling acquired an incompatible enhanced depth-attachment layout.");
            for (const auto format : { nvrhi::Format::D32, nvrhi::Format::D24S8 })
                Require(RenderGraph::MapTextureUsageToResourceStates(RenderGraphUsage::ShaderResource, format, nvrhi::GraphicsAPI::VULKAN) ==
                    (nvrhi::ResourceStates::ShaderResource | nvrhi::ResourceStates::DepthRead),
                    "Depth SRV barriers do not match NVRHI's depth-read descriptor layout.");
            Require(RenderGraph::MapTextureUsageToResourceStates(RenderGraphUsage::ShaderResource,
                    nvrhi::Format::RGBA16_FLOAT, nvrhi::GraphicsAPI::VULKAN) == nvrhi::ResourceStates::ShaderResource,
                "Color SRV incorrectly acquired a depth attachment state.");
            Require(RenderGraph::MapTextureUsageToResourceStates(RenderGraphUsage::DepthWrite,
                    nvrhi::Format::D32, nvrhi::GraphicsAPI::VULKAN) == nvrhi::ResourceStates::DepthWrite,
                "Depth writes incorrectly acquired shader-read state.");
            for (const auto load : { RenderGraphAttachmentLoad::Load, RenderGraphAttachmentLoad::Clear,
                    RenderGraphAttachmentLoad::Discard })
            {
                RenderGraph graph;
                const auto texture = graph.CreateTexture(MakeTextureDesc("Overwrite"));
                const auto old = graph.AddPass<TexturePassData>("Old",
                    [texture](RenderGraphBuilder& builder, TexturePassData& data) {
                        data.Texture = builder.RenderTarget(texture);
                    }, [](RenderGraphContext&, const TexturePassData&) {});
                graph.AddPass<TexturePassData>("Replacement",
                    [texture, load](RenderGraphBuilder& builder, TexturePassData& data) {
                        RenderGraphAttachmentOps ops;
                        ops.Load = load;
                        ops.bFullOverwrite = load == RenderGraphAttachmentLoad::Discard;
                        data.Texture = builder.ColorAttachment(texture, ops);
                        builder.Export(texture);
                    }, [](RenderGraphContext&, const TexturePassData&) {});
                Require(graph.Compile(), "Overwrite graph failed compilation.");
                Require(graph.IsPassCulled(old) == (load != RenderGraphAttachmentLoad::Load),
                    "Attachment overwrite/load did not distinguish old content liveness.");
            }
            RenderGraph conservative;
            const auto legacyTexture = conservative.CreateTexture(MakeTextureDesc("Legacy writes"));
            const auto legacyOld = conservative.AddPass<TexturePassData>("Legacy old",
                [legacyTexture](RenderGraphBuilder& builder, TexturePassData& data) {
                    data.Texture = builder.RenderTarget(legacyTexture);
                }, [](RenderGraphContext&, const TexturePassData&) {});
            conservative.AddPass<TexturePassData>("Legacy new",
                [legacyTexture](RenderGraphBuilder& builder, TexturePassData& data) {
                    data.Texture = builder.RenderTarget(legacyTexture);
                    builder.Export(legacyTexture);
                }, [](RenderGraphContext&, const TexturePassData&) {});
            Require(conservative.Compile() && !conservative.IsPassCulled(legacyOld),
                "Ordinary Write incorrectly implied a complete overwrite.");
            // A clear of one mip/slice cannot kill a producer of exported peers.
            RenderGraph partial;
            auto desc = MakeTextureDesc("Partial overwrite");
            desc.MipLevels = 2;
            desc.ArrayLayers = 2;
            const auto texture = partial.CreateTexture(desc);
            const auto old = partial.AddPass<TexturePassData>("Whole texture",
                [texture](RenderGraphBuilder& builder, TexturePassData& data) {
                    data.Texture = builder.RenderTarget(texture);
                }, [](RenderGraphContext&, const TexturePassData&) {});
            partial.AddPass<TexturePassData>("One subresource",
                [texture](RenderGraphBuilder& builder, TexturePassData& data) {
                    RenderGraphAttachmentOps ops;
                    ops.Load = RenderGraphAttachmentLoad::Clear;
                    data.Texture = builder.ColorAttachment(texture, ops, { 1, 1, 1, 1 });
                    builder.Export(texture);
                }, [](RenderGraphContext&, const TexturePassData&) {});
            Require(partial.Compile() && !partial.IsPassCulled(old),
                "Partial mip/slice clear killed the producer of untouched exported contents.");

            for (const bool keepReader : { false, true })
            {
                RenderGraph ordered;
                const auto target = ordered.CreateTexture(MakeTextureDesc("Hazards"));
                ordered.AddPass<TexturePassData>("Old live writer",
                    [target](RenderGraphBuilder& builder, TexturePassData& data) {
                        data.Texture = builder.RenderTarget(target);
                        builder.SetSideEffect();
                    }, [](RenderGraphContext&, const TexturePassData&) {});
                const auto reader = ordered.AddPass<TexturePassData>("Reader",
                    [target, keepReader](RenderGraphBuilder& builder, TexturePassData& data) {
                        data.Texture = builder.ShaderResource(target);
                        builder.SetSideEffect(keepReader);
                    }, [](RenderGraphContext&, const TexturePassData&) {});
                ordered.AddPass<TexturePassData>("Clear writer",
                    [target](RenderGraphBuilder& builder, TexturePassData& data) {
                        RenderGraphAttachmentOps ops;
                        ops.Load = RenderGraphAttachmentLoad::Clear;
                        data.Texture = builder.ColorAttachment(target, ops);
                        builder.Export(target);
                    }, [](RenderGraphContext&, const TexturePassData&) {});
                Require(ordered.Compile() && ordered.IsPassCulled(reader) == !keepReader,
                    "WAR ordering made an unused reader live or killed an observable reader.");
                const auto expected = keepReader
                    ? std::vector<std::string>{ "Old live writer", "Reader", "Clear writer" }
                    : std::vector<std::string>{ "Old live writer", "Clear writer" };
                Require(ordered.GetCompiledPassNames() == expected,
                    "Live WAW/WAR hazards lost their execution order.");
            }
        }

        void AttachmentContentsContract()
        {
            for (const auto load : {RenderGraphAttachmentLoad::Load,
                    RenderGraphAttachmentLoad::Clear, RenderGraphAttachmentLoad::Discard})
            {
                for (const auto store : {RenderGraphAttachmentStore::Store, RenderGraphAttachmentStore::Discard})
                {
                    RenderGraph graph;
                    const auto texture = graph.CreateTexture(MakeTextureDesc("Attachment"));
                    graph.AddPass<TexturePassData>("AttachmentWriter",
                        [texture, load, store](RenderGraphBuilder& builder, TexturePassData& data)
                        {
                            RenderGraphAttachmentOps ops;
                            ops.Load = load;
                            ops.Store = store;
                            ops.bFullOverwrite = load == RenderGraphAttachmentLoad::Discard;
                            data.Texture = builder.ColorAttachment(texture, ops);
                        }, [](RenderGraphContext&, const TexturePassData&) {});
                    graph.AddPass<TexturePassData>("AttachmentReader",
                        [texture](RenderGraphBuilder& builder, TexturePassData& data)
                        {
                            data.Texture = builder.ShaderResource(texture);
                            builder.SetSideEffect();
                        }, [](RenderGraphContext&, const TexturePassData&) {});
                    const bool expected = load != RenderGraphAttachmentLoad::Load &&
                        store == RenderGraphAttachmentStore::Store;
                    Require(graph.Compile() == expected,
                        "Attachment load/store initialization contract was not enforced.");
                }
            }
            RenderGraph graph;
            const auto texture = graph.CreateTexture(MakeTextureDesc("PartialDiscard"));
            bool bRejected = false;
            try
            {
                graph.AddPass<TexturePassData>("PartialDiscard",
                    [texture](RenderGraphBuilder& builder, TexturePassData& data)
                    {
                        RenderGraphAttachmentOps ops;
                        ops.Load = RenderGraphAttachmentLoad::Discard;
                        data.Texture = builder.ColorAttachment(texture, ops);
                    }, [](RenderGraphContext&, const TexturePassData&) {});
            }
            catch (const RenderGraphValidationError&)
            {
                bRejected = true;
            }
            Require(bRejected, "Discard/store accepted a partial write without initialized contents.");

            RenderGraph depthGraph;
            auto depthDesc = MakeTextureDesc("DepthAttachment");
            depthDesc.Format = nvrhi::Format::D32;
            const auto depth = depthGraph.CreateTexture(depthDesc);
            depthGraph.AddPass<TexturePassData>("ClearDepth",
                [depth](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    RenderGraphAttachmentOps ops;
                    ops.Load = RenderGraphAttachmentLoad::Clear;
                    ops.ClearDepth = 0.0f;
                    data.Texture = builder.DepthAttachment(depth, ops);
                }, [](RenderGraphContext&, const TexturePassData&) {});
            depthGraph.AddPass<TexturePassData>("PreserveDepth",
                [depth](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.DepthAttachment(depth, {});
                    builder.Export(depth);
                }, [](RenderGraphContext&, const TexturePassData&) {});
            Require(depthGraph.Compile(), "Cleared depth attachment could not preserve existing contents.");

            RenderGraph discardGraph;
            const auto discarded = discardGraph.CreateTexture(MakeTextureDesc("DiscardedExport"));
            discardGraph.AddPass<TexturePassData>("DiscardExport",
                [discarded](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    RenderGraphAttachmentOps ops;
                    ops.Load = RenderGraphAttachmentLoad::Clear;
                    ops.Store = RenderGraphAttachmentStore::Discard;
                    data.Texture = builder.ColorAttachment(discarded, ops);
                    builder.Export(discarded);
                }, [](RenderGraphContext&, const TexturePassData&) {});
            Require(!discardGraph.Compile() && ContainsError(discardGraph, "exports discarded"),
                "An exported attachment retained discarded contents.");
        }

        void SubresourceInitializationContract()
        {
            {
                RenderGraph graph;
                auto desc = MakeTextureDesc("OverlappingRanges");
                desc.MipLevels = 2;
                const auto texture = graph.CreateTexture(desc);
                bool bRejected = false;
                try
                {
                    graph.AddPass<TexturePassData>("OverlappingReadWrite",
                        [texture](RenderGraphBuilder& builder, TexturePassData& data)
                        {
                            builder.ShaderResource(texture);
                            data.Texture = builder.CopyDest(texture, {1, 1, 0, 1});
                        }, [](RenderGraphContext&, const TexturePassData&) {});
                }
                catch (const RenderGraphValidationError&)
                {
                    bRejected = true;
                }
                Require(bRejected, "Overlapping unequal ranges bypassed incompatible-state validation.");
                bRejected = false;
                try
                {
                    graph.AddPass<TexturePassData>("OutOfBounds",
                        [texture](RenderGraphBuilder& builder, TexturePassData& data)
                        {
                            data.Texture = builder.CopyDest(texture, {1, 2, 0, 1});
                        }, [](RenderGraphContext&, const TexturePassData&) {});
                }
                catch (const RenderGraphValidationError&)
                {
                    bRejected = true;
                }
                Require(bRejected, "Out-of-bounds mip access was accepted.");
            }
            for (bool bReadOtherSlice : {false, true})
            {
                RenderGraph graph;
                auto desc = MakeTextureDesc("PartialTexture");
                desc.MipLevels = 2;
                desc.ArrayLayers = 2;
                const auto texture = graph.CreateTexture(desc);
                graph.AddPass<TexturePassData>("WriteOneSubresource",
                    [texture](RenderGraphBuilder& builder, TexturePassData& data)
                    {
                        data.Texture = builder.CopyDest(texture, {0, 1, 0, 1});
                    }, [](RenderGraphContext&, const TexturePassData&) {});
                graph.AddPass<TexturePassData>("ReadUnwrittenSubresource",
                    [texture, bReadOtherSlice](RenderGraphBuilder& builder, TexturePassData& data)
                    {
                        data.Texture = builder.ShaderResource(texture,
                            {bReadOtherSlice ? 0u : 1u, 1, bReadOtherSlice ? 1u : 0u, 1});
                        builder.SetSideEffect();
                    }, [](RenderGraphContext&, const TexturePassData&) {});
                Require(!graph.Compile() && ContainsError(graph, "read before initialization"),
                    "Writing a sibling mip or slice initialized unread texture contents.");
            }

            RenderGraph graph;
            auto desc = MakeTextureDesc("MipChain");
            desc.MipLevels = 2;
            const auto texture = graph.CreateTexture(desc);
            graph.AddPass<TexturePassData>("InitializeMip0",
                [texture](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.CopyDest(texture, {0, 1, 0, 1});
                }, [](RenderGraphContext&, const TexturePassData&) {});
            graph.AddPass<TexturePassData>("GenerateMip1",
                [texture](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    builder.ShaderResource(texture, {0, 1, 0, 1});
                    data.Texture = builder.Write(texture, RenderGraphUsage::UnorderedAccess, {1, 1, 0, 1});
                }, [](RenderGraphContext&, const TexturePassData&) {});
            graph.AddPass<TexturePassData>("ConsumeAllMips",
                [texture](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.ShaderResource(texture);
                    builder.SetSideEffect();
                }, [](RenderGraphContext&, const TexturePassData&) {});
            Require(graph.Compile(), "Disjoint mip reads/writes failed valid mip-chain compilation.");

            RenderGraph exportGraph;
            const auto exported = exportGraph.CreateTexture(desc);
            for (uint32_t mip = 0; mip < 2; ++mip)
                exportGraph.AddPass<TexturePassData>("IndependentMip" + std::to_string(mip),
                    [exported, mip](RenderGraphBuilder& builder, TexturePassData& data)
                    {
                        data.Texture = builder.CopyDest(exported, {mip, 1, 0, 1});
                        builder.Export(exported);
                    }, [](RenderGraphContext&, const TexturePassData&) {});
            Require(exportGraph.Compile(), "Independent exported mip writers failed compilation.");
            const auto snapshot = exportGraph.BuildDiagnosticSnapshot();
            Require(snapshot.Passes.size() == 2 && !snapshot.Passes[0].bCulled && !snapshot.Passes[1].bCulled,
                "Export culling lost an independently written texture subresource.");

            RenderGraph partialExport;
            const auto incomplete = partialExport.CreateTexture(desc);
            partialExport.AddPass<TexturePassData>("IncompleteExport",
                [incomplete](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.CopyDest(incomplete, {0, 1, 0, 1});
                    builder.Export(incomplete);
                }, [](RenderGraphContext&, const TexturePassData&) {});
            Require(!partialExport.Compile() && ContainsError(partialExport, "exports uninitialized subresource"),
                "An export exposed a never-initialized mip.");
        }

        void SparseResourceDependencyContract()
        {
            // A material-rich scene has many unrelated textures/buffers and
            // multiple declarations for disjoint subresources in the same pass.
            // Read-before-writer reordering must retain both writers without
            // inventing dependencies between independent resources.
            RenderGraph graph;
            constexpr uint32_t resourceCount = 192;
            for (uint32_t index = 0; index < resourceCount; ++index)
            {
                auto desc = MakeTextureDesc("SparseTexture" + std::to_string(index));
                desc.MipLevels = 3;
                desc.ArrayLayers = 2;
                const auto texture = graph.CreateTexture(desc);
                const auto buffer = graph.CreateBuffer(MakeBufferDesc("SparseBuffer" + std::to_string(index)));
                graph.AddPass<LifetimePassData>("Read" + std::to_string(index),
                    [texture, buffer](RenderGraphBuilder& builder, LifetimePassData& data)
                    {
                        data.Texture = builder.ShaderResource(texture, { 0, 1, 0, 1 });
                        data.Buffer = builder.ShaderResource(buffer);
                        builder.ShaderResource(texture, { 2, 1, 1, 1 });
                        builder.SetSideEffect();
                    }, [](RenderGraphContext&, const LifetimePassData&) {});
                graph.AddPass<LifetimePassData>("Write" + std::to_string(index),
                    [texture, buffer](RenderGraphBuilder& builder, LifetimePassData& data)
                    {
                        data.Texture = builder.CopyDest(texture, { 0, 1, 0, 1 });
                        data.Buffer = builder.Write(buffer, RenderGraphUsage::CopyDest);
                        builder.CopyDest(texture, { 2, 1, 1, 1 });
                    }, [](RenderGraphContext&, const LifetimePassData&) {});
            }
            for (uint32_t compile = 0; compile < 2; ++compile)
            {
                Require(graph.Compile(), "Sparse multi-resource graph failed compilation.");
                const auto snapshot = graph.BuildDiagnosticSnapshot();
                Require(snapshot.ExecutionOrder.size() == resourceCount * 2,
                    "Sparse dependency compilation culled a required subresource writer.");
                Require(snapshot.Dependencies.size() == resourceCount,
                    "Sparse dependency compilation introduced cross-resource hazards.");
                for (uint32_t index = 0; index < resourceCount; ++index)
                {
                    const auto& dependency = snapshot.Dependencies[index];
                    Require(dependency.PrerequisitePassId == index * 2 + 1 &&
                            dependency.DependentPassId == index * 2,
                        "Sparse dependency compilation lost read-before-writer ordering.");
                }
            }
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
        SubresourceInitializationContract();
        AttachmentContentsContract();
        OverwrittenContentsCulling();
        BackendDepthReadContract();
        FeatureOwnershipContract();
        DiagnosticSnapshotContract();
        SparseResourceDependencyContract();
        ResizeInvalidatesCompilation();
    }
}
