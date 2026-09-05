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
        RenderGraphFrameSlotState slot;
        Require(slot.CanBegin(0), "A fresh RenderGraph resource-pool slot was unavailable.");

        slot.bRecording = true;
        Require(!slot.CanBegin(100), "A recording RenderGraph frame slot was reusable.");

        slot.bRecording = false;
        slot.LastSubmission = 42;
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
    }
}
