#include "Hepch.h"

#include "Runtime/UnitTest/RenderGraphResourcePoolTests.h"

#include "Runtime/Graphics/RenderGraph/RenderGraphResourcePool.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
    namespace
    {
        constexpr RenderGraphPoolResourceKey TextureKey{
            .GraphNamespace = 7,
            .ResourceIndex = 2,
            .ResourceGeneration = 4,
            .Type = RenderGraphPoolResourceType::Texture
        };
        constexpr RenderGraphPoolResourceKey SameTextureKey = TextureKey;
        constexpr RenderGraphPoolResourceKey NewGenerationKey{
            .GraphNamespace = 7,
            .ResourceIndex = 2,
            .ResourceGeneration = 5,
            .Type = RenderGraphPoolResourceType::Texture
        };
        constexpr RenderGraphPoolResourceKey BufferKey{
            .GraphNamespace = 7,
            .ResourceIndex = 2,
            .ResourceGeneration = 4,
            .Type = RenderGraphPoolResourceType::Buffer
        };

        static_assert(TextureKey == SameTextureKey);
        static_assert(!(TextureKey == NewGenerationKey));
        static_assert(!(TextureKey == BufferKey));
        static_assert(RenderGraphFrameSlotState{ .LastSubmission = 8 }.CanBegin(8));
        static_assert(!RenderGraphFrameSlotState{ .LastSubmission = 8 }.CanBegin(7));
        static_assert(!RenderGraphFrameSlotState{
            .LastSubmission = 8,
            .bRecording = true
        }.CanBegin(100));

        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }
    }

    void RenderGraphResourcePoolPureContract()
    {
        Require(CanTrimRenderGraphTransient({42, false}, 3, 3, 41, 42),
            "Completed unused peak allocation could not be trimmed.");
        Require(!CanTrimRenderGraphTransient({42, false}, 2, 3, 41, 42) &&
                !CanTrimRenderGraphTransient({42, true}, 3, 3, 41, 100) &&
                !CanTrimRenderGraphTransient({43, false}, 3, 3, 41, 42) &&
                !CanTrimRenderGraphTransient({42, false}, 3, 3, 43, 42),
            "Cache trim admitted active working-set, recording, or in-flight allocations.");
        const RenderGraphBufferDesc bufferDesc{ .DebugName = "Capabilities", .ByteSize = 256 };
        const auto indirect = MakeRenderGraphBufferAllocationDesc(bufferDesc,
            RenderGraphUsage::IndirectArgument | RenderGraphUsage::UnorderedAccess);
        Require(indirect.isDrawIndirectArgs && indirect.canHaveUAVs && indirect.canHaveRawViews,
            "GPU-written indirect arguments lost required NVRHI allocation capabilities.");
        const auto vertex = MakeRenderGraphBufferAllocationDesc(bufferDesc,
            RenderGraphUsage::VertexBuffer | RenderGraphUsage::AccelerationStructureBuildInput);
        Require(vertex.isVertexBuffer && vertex.isAccelStructBuildInput && !vertex.isDrawIndirectArgs,
            "Raster/raytracing vertex allocation capabilities were lost or conflated.");
        Require(MakeRenderGraphBufferAllocationDesc(bufferDesc, RenderGraphUsage::IndexBuffer).isIndexBuffer &&
                MakeRenderGraphBufferAllocationDesc(bufferDesc, RenderGraphUsage::ConstantBuffer).isConstantBuffer,
            "Index or constant-buffer allocation omitted its NVRHI creation flag.");
        const auto copy = MakeRenderGraphBufferAllocationDesc(bufferDesc, RenderGraphUsage::CopyDest);
        Require(!copy.isDrawIndirectArgs && !copy.isVertexBuffer && !copy.isIndexBuffer &&
                !copy.isConstantBuffer && !copy.canHaveUAVs && !copy.isAccelStructBuildInput,
            "Copy-only buffers acquired unrelated allocation capabilities.");

        RenderGraphFrameSlotState slot;
        Require(slot.CanBegin(0), "A fresh RenderGraph resource-pool slot was unavailable.");
        Require(slot.IsUnsubmitted(), "A fresh frame slot was unavailable for candidate seeding.");

        slot.bRecording = true;
        Require(!slot.IsUnsubmitted(), "Recording storage was accepted as an empty candidate slot.");
        Require(!slot.CanBegin(100), "A recording RenderGraph frame slot was reusable.");

        slot.bRecording = false;
        slot.LastSubmission = 42;
        Require(!slot.IsUnsubmitted(), "Submitted storage was accepted as an empty candidate slot.");
        Require(!slot.CanBegin(41), "An in-flight RenderGraph frame slot was reusable.");
        Require(slot.CanBegin(42), "A completed RenderGraph frame slot remained unavailable.");
        Require(slot.CanBegin(100),
            "A frame slot was unavailable when queue-global completion was ahead of its submission.");

        RenderGraphPoolAllocationInterval interval;
        interval.Resource = TextureKey;
        interval.Lifetime = RenderGraphResourceLifetime::Transient;
        interval.PassInterval = { .FirstUse = 1, .LastUse = 3 };
        interval.PhysicalAllocationId = 11;
        Require(interval.PassInterval.FirstUse == 1 && interval.PassInterval.LastUse == 3,
            "RenderGraph pool lost the logical first/last-use interval.");
        Require(interval.Resource.ResourceGeneration == TextureKey.ResourceGeneration,
            "RenderGraph pool allocation records lost handle generation.");
        interval.bTextureReuseEligible = true;
        std::vector<RenderGraphPoolAllocationInterval> intervals{interval};
        Require(CanReuseRenderGraphTextureAllocation(intervals, 11, TextureKey, {4, 6}),
            "Disjoint transient texture lifetimes could not share an allocation.");
        Require(CanReuseRenderGraphTextureAllocation(intervals, 11, TextureKey, {0, 0}),
            "Reverse declaration order was confused with execution order.");
        Require(!CanReuseRenderGraphTextureAllocation(intervals, 11, TextureKey, {3, 4}) &&
            !CanReuseRenderGraphTextureAllocation(intervals, 11, TextureKey, {2, 2}) &&
            !CanReuseRenderGraphTextureAllocation(intervals, 11, TextureKey, {}),
            "Overlapping or invalid texture intervals were reused.");
        intervals.push_back({TextureKey, RenderGraphResourceLifetime::Transient, {7, 9}, 11, true});
        Require(!CanReuseRenderGraphTextureAllocation(intervals, 11, TextureKey, {8, 10}),
            "Allocator ignored one of a texture's existing live intervals.");
        auto otherGraph = TextureKey;
        ++otherGraph.GraphNamespace;
        Require(!CanReuseRenderGraphTextureAllocation(intervals, 11, otherGraph, {10, 11}) &&
            !CanReuseRenderGraphTextureAllocation(intervals, 11, BufferKey, {10, 11}),
            "Allocator shared incompatible graph namespaces/resource types.");
        intervals[0].bTextureReuseEligible = false;
        Require(!CanReuseRenderGraphTextureAllocation(intervals, 11, TextureKey, {10, 11}),
            "An exported or initialized texture was reused.");
        intervals[0].bTextureReuseEligible = true;
        intervals[0].Lifetime = RenderGraphResourceLifetime::Persistent;
        Require(!CanReuseRenderGraphTextureAllocation(intervals, 11, TextureKey, {10, 11}),
            "Persistent texture lifetime was treated as transient.");
    }
}
