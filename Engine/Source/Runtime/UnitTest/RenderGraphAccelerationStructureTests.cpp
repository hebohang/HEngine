#include "Hepch.h"

#include "Runtime/UnitTest/RenderGraphAccelerationStructureTests.h"

#include "Runtime/Graphics/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        struct AccelerationStructurePassData
        {
            AccelerationStructureHandle AccelerationStructure;
            BufferHandle BuildInput;
        };

        struct TexturePassData
        {
            TextureHandle Texture;
        };

        static_assert(std::is_same_v<
            decltype(std::declval<const RenderGraphContext&>().GetAccelerationStructure(
                AccelerationStructureHandle{})),
            nvrhi::rt::IAccelStruct*>);

        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        [[nodiscard]] RenderGraphAccelerationStructureDesc MakeAccelerationStructureDesc(
            std::string name)
        {
            RenderGraphAccelerationStructureDesc desc;
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

        [[nodiscard]] RenderGraphTextureDesc MakeTextureDesc(std::string name)
        {
            RenderGraphTextureDesc desc;
            desc.DebugName = std::move(name);
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

        void StaleHandleValidation()
        {
            RenderGraph graph;
            const AccelerationStructureHandle stale = graph.ImportAccelerationStructure(
                MakeAccelerationStructureDesc("OldTLAS"),
                {},
                nvrhi::ResourceStates::AccelStructRead,
                nvrhi::ResourceStates::AccelStructRead);
            Require(graph.IsValid(stale),
                "A newly imported acceleration structure handle was invalid.");
            Require(graph.GetLifetime(stale) == RenderGraphResourceLifetime::Imported,
                "An imported acceleration structure has the wrong lifetime.");

            graph.Clear();
            Require(!graph.IsValid(stale),
                "RenderGraph Clear did not invalidate an acceleration structure handle.");

            const AccelerationStructureHandle replacement = graph.ImportAccelerationStructure(
                MakeAccelerationStructureDesc("NewTLAS"),
                {},
                nvrhi::ResourceStates::AccelStructRead,
                nvrhi::ResourceStates::AccelStructRead);
            Require(replacement.Index == stale.Index &&
                    replacement.Generation != stale.Generation,
                "Acceleration structure slot reuse did not advance the handle generation.");

            bool bRejectedStaleHandle = false;
            try
            {
                graph.AddPass<AccelerationStructurePassData>(
                    "StaleTLASRead",
                    [stale](RenderGraphBuilder& builder, AccelerationStructurePassData& data)
                    {
                        data.AccelerationStructure =
                            builder.AccelerationStructureRead(stale);
                    },
                    [](RenderGraphContext&, const AccelerationStructurePassData&) {});
            }
            catch (const RenderGraphValidationError&)
            {
                bRejectedStaleHandle = true;
            }
            Require(bRejectedStaleHandle,
                "RenderGraph accepted a stale acceleration structure handle.");
        }

        void BuildWriteThenReadDependency()
        {
            RenderGraph graph;
            const AccelerationStructureHandle sceneTlas = graph.ImportAccelerationStructure(
                MakeAccelerationStructureDesc("SceneTLAS"),
                {},
                nvrhi::ResourceStates::AccelStructRead,
                nvrhi::ResourceStates::AccelStructRead,
                false);
            const BufferHandle instanceBuffer = graph.CreateBuffer(
                MakeBufferDesc("TLASInstances"),
                RenderGraphResourceLifetime::Transient,
                true);

            const RenderGraphPassHandle build =
                graph.AddPass<AccelerationStructurePassData>(
                    "TLASBuild",
                    [sceneTlas, instanceBuffer](
                        RenderGraphBuilder& builder,
                        AccelerationStructurePassData& data)
                    {
                        data.BuildInput =
                            builder.AccelerationStructureBuildInput(instanceBuffer);
                        data.AccelerationStructure =
                            builder.AccelerationStructureBuildWrite(sceneTlas);
                    },
                    [](RenderGraphContext&, const AccelerationStructurePassData&) {});
            const RenderGraphPassHandle trace =
                graph.AddPass<AccelerationStructurePassData>(
                    "TraceHardwareRays",
                    [sceneTlas](
                        RenderGraphBuilder& builder,
                        AccelerationStructurePassData& data)
                    {
                        data.AccelerationStructure =
                            builder.AccelerationStructureRead(sceneTlas);
                        builder.SetSideEffect();
                    },
                    [](RenderGraphContext&, const AccelerationStructurePassData&) {});

            Require(graph.Compile(),
                "A TLAS build-write followed by RT-read failed compilation.");
            Require(graph.GetCompiledPassNames() ==
                    std::vector<std::string>{ "TLASBuild", "TraceHardwareRays" },
                "The TLAS build-write to RT-read dependency was not preserved.");
            Require(!graph.IsPassCulled(build) && !graph.IsPassCulled(trace),
                "A live TLAS build or ray tracing pass was culled.");

            const RenderGraphResourceLifetimeInfo lifetime =
                graph.GetLifetimeInfo(sceneTlas);
            Require(lifetime.FirstUse == 0 && lifetime.LastUse == 1,
                "Acceleration structure lifetime analysis returned the wrong interval.");

            const std::string debugDump = graph.BuildDebugDump();
            Require(debugDump.find("AccelerationStructures:") != std::string::npos &&
                    debugDump.find("SceneTLAS") != std::string::npos &&
                    debugDump.find("first=0 last=1") != std::string::npos,
                "RenderGraph debug dump omitted acceleration structure lifetime data.");
        }

        void ReadBeforeBuildValidation()
        {
            RenderGraph graph;
            const AccelerationStructureHandle unbuilt = graph.ImportAccelerationStructure(
                MakeAccelerationStructureDesc("UnbuiltTLAS"),
                {},
                nvrhi::ResourceStates::AccelStructRead,
                nvrhi::ResourceStates::AccelStructRead,
                false);
            graph.AddPass<AccelerationStructurePassData>(
                "InvalidTrace",
                [unbuilt](RenderGraphBuilder& builder, AccelerationStructurePassData& data)
                {
                    data.AccelerationStructure =
                        builder.AccelerationStructureRead(unbuilt);
                    builder.SetSideEffect();
                },
                [](RenderGraphContext&, const AccelerationStructurePassData&) {});

            Require(!graph.Compile(),
                "An acceleration structure read-before-build unexpectedly compiled.");
            Require(ContainsError(graph, "read before initialization"),
                "Acceleration structure read-before-build had no useful diagnostic.");
        }

        void UsageAndStateValidation()
        {
            Require(RenderGraph::MapUsageToResourceStates(
                    RenderGraphUsage::AccelerationStructureBuildInput) ==
                    nvrhi::ResourceStates::AccelStructBuildInput,
                "AS build-input usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(
                    RenderGraphUsage::AccelerationStructureBuildWrite) ==
                    nvrhi::ResourceStates::AccelStructWrite,
                "AS build-write usage mapped to the wrong NVRHI state.");
            Require(RenderGraph::MapUsageToResourceStates(
                    RenderGraphUsage::AccelerationStructureRead) ==
                    nvrhi::ResourceStates::AccelStructRead,
                "AS read usage mapped to the wrong NVRHI state.");

            RenderGraph graph;
            const TextureHandle texture = graph.CreateTexture(
                MakeTextureDesc("TextureMisuse"),
                RenderGraphResourceLifetime::Transient,
                true);
            const BufferHandle buffer = graph.CreateBuffer(
                MakeBufferDesc("BufferMisuse"),
                RenderGraphResourceLifetime::Transient,
                true);
            const AccelerationStructureHandle accelerationStructure =
                graph.ImportAccelerationStructure(
                    MakeAccelerationStructureDesc("AccelerationStructureMisuse"),
                    {},
                    nvrhi::ResourceStates::AccelStructRead,
                    nvrhi::ResourceStates::AccelStructRead);

            bool bRejectedTextureUsage = false;
            try
            {
                graph.AddPass<TexturePassData>(
                    "InvalidTextureBuildInput",
                    [texture](RenderGraphBuilder& builder, TexturePassData& data)
                    {
                        data.Texture = builder.Read(
                            texture,
                            RenderGraphUsage::AccelerationStructureBuildInput);
                    },
                    [](RenderGraphContext&, const TexturePassData&) {});
            }
            catch (const RenderGraphValidationError&)
            {
                bRejectedTextureUsage = true;
            }
            Require(bRejectedTextureUsage,
                "RenderGraph accepted AS build-input usage on a texture.");

            bool bRejectedBufferUsage = false;
            try
            {
                graph.AddPass<AccelerationStructurePassData>(
                    "InvalidBufferASRead",
                    [buffer](RenderGraphBuilder& builder, AccelerationStructurePassData& data)
                    {
                        data.BuildInput = builder.Read(
                            buffer,
                            RenderGraphUsage::AccelerationStructureRead);
                    },
                    [](RenderGraphContext&, const AccelerationStructurePassData&) {});
            }
            catch (const RenderGraphValidationError&)
            {
                bRejectedBufferUsage = true;
            }
            Require(bRejectedBufferUsage,
                "RenderGraph accepted acceleration structure read usage on a buffer.");

            bool bRejectedAccelerationStructureUsage = false;
            try
            {
                graph.AddPass<AccelerationStructurePassData>(
                    "InvalidASShaderResource",
                    [accelerationStructure](
                        RenderGraphBuilder& builder,
                        AccelerationStructurePassData& data)
                    {
                        data.AccelerationStructure = builder.Read(
                            accelerationStructure,
                            RenderGraphUsage::ShaderResource);
                    },
                    [](RenderGraphContext&, const AccelerationStructurePassData&) {});
            }
            catch (const RenderGraphValidationError&)
            {
                bRejectedAccelerationStructureUsage = true;
            }
            Require(bRejectedAccelerationStructureUsage,
                "RenderGraph accepted ShaderResource usage on an acceleration structure.");
        }

        void CullingAndImportedStateValidation()
        {
            RenderGraph culledGraph;
            const AccelerationStructureHandle readable =
                culledGraph.ImportAccelerationStructure(
                    MakeAccelerationStructureDesc("UnusedTLAS"),
                    {},
                    nvrhi::ResourceStates::AccelStructRead,
                    nvrhi::ResourceStates::AccelStructRead);
            const RenderGraphPassHandle unusedRead =
                culledGraph.AddPass<AccelerationStructurePassData>(
                    "UnusedTLASRead",
                    [readable](
                        RenderGraphBuilder& builder,
                        AccelerationStructurePassData& data)
                    {
                        data.AccelerationStructure =
                            builder.AccelerationStructureRead(readable);
                    },
                    [](RenderGraphContext&, const AccelerationStructurePassData&) {});
            Require(culledGraph.Compile(),
                "A valid unused AS-read graph failed compilation.");
            Require(culledGraph.IsPassCulled(unusedRead),
                "An unobservable acceleration structure read was not culled.");

            RenderGraph exportedGraph;
            const AccelerationStructureHandle exported =
                exportedGraph.ImportAccelerationStructure(
                    MakeAccelerationStructureDesc("ExportedBLAS"),
                    {},
                    nvrhi::ResourceStates::AccelStructRead,
                    nvrhi::ResourceStates::AccelStructRead,
                    false);
            const RenderGraphPassHandle exportedBuild =
                exportedGraph.AddPass<AccelerationStructurePassData>(
                    "ExportedBLASBuild",
                    [exported](
                        RenderGraphBuilder& builder,
                        AccelerationStructurePassData& data)
                    {
                        data.AccelerationStructure =
                            builder.AccelerationStructureBuildWrite(exported);
                        builder.Export(exported);
                    },
                    [](RenderGraphContext&, const AccelerationStructurePassData&) {});
            Require(exportedGraph.Compile(),
                "An exported AS build graph failed compilation.");
            Require(!exportedGraph.IsPassCulled(exportedBuild),
                "A build writing an exported acceleration structure was culled.");

            RenderGraph invalidStateGraph;
            (void)invalidStateGraph.ImportAccelerationStructure(
                MakeAccelerationStructureDesc("UnknownStateTLAS"),
                {},
                nvrhi::ResourceStates::Unknown,
                nvrhi::ResourceStates::AccelStructRead);
            Require(!invalidStateGraph.Compile(),
                "An imported AS with an unknown boundary state unexpectedly compiled.");
            Require(ContainsError(invalidStateGraph, "explicit initial and final states"),
                "Invalid imported AS states had no useful diagnostic.");
        }

        void RasterGraphUnaffected()
        {
            RenderGraph graph;
            TextureHandle sceneColor;
            graph.AddPass<TexturePassData>(
                "OpaqueRaster",
                [&sceneColor](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.CreateTexture(
                        MakeTextureDesc("RasterSceneColor"));
                    sceneColor = builder.RenderTarget(data.Texture);
                },
                [](RenderGraphContext&, const TexturePassData&) {});
            graph.AddPass<TexturePassData>(
                "ToneMapRaster",
                [sceneColor](RenderGraphBuilder& builder, TexturePassData& data)
                {
                    data.Texture = builder.ShaderResource(sceneColor);
                    builder.SetSideEffect();
                },
                [](RenderGraphContext&, const TexturePassData&) {});

            Require(graph.Compile(),
                "A texture-only raster graph failed after adding AS resources.");
            Require(graph.GetCompiledPassNames() ==
                    std::vector<std::string>{ "OpaqueRaster", "ToneMapRaster" },
                "Adding AS resources changed texture-only raster pass ordering.");
        }
    }

    void RenderGraphAccelerationStructureContract()
    {
        StaleHandleValidation();
        BuildWriteThenReadDependency();
        ReadBeforeBuildValidation();
        UsageAndStateValidation();
        CullingAndImportedStateValidation();
        RasterGraphUnaffected();
    }
}
