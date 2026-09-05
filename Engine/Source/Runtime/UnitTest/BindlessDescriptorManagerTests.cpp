#include "Hepch.h"

#include "Runtime/UnitTest/BindlessDescriptorManagerTests.h"

#include "Runtime/Graphics/RHI/BindlessDescriptorManager.h"
#include "Runtime/Graphics/RHI/TextureManager.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        template<typename Exception, typename Function>
        void RequireThrows(Function&& function, std::string_view message)
        {
            bool bThrewExpected = false;
            try
            {
                function();
            }
            catch (const Exception&)
            {
                bThrewExpected = true;
            }
            Require(bThrewExpected, message);
        }
    }

    void DescriptorIndexAllocatorContract()
    {
        DescriptorIndexAllocator allocator(6, 3);
        Require(allocator.GetCapacity() == 6 && allocator.GetReservedCount() == 3,
            "Descriptor index allocator lost capacity or default reservations.");
        Require(allocator.GetAvailableCount() == 3,
            "Reserved descriptor indices were exposed for allocation.");

        const DescriptorAllocationHandle first = allocator.Allocate();
        const DescriptorAllocationHandle second = allocator.Allocate();
        const DescriptorAllocationHandle third = allocator.Allocate();
        Require(first.Index == 3 && second.Index == 4 && third.Index == 5,
            "Descriptor allocation order or reserved index boundary was unstable.");
        Require(allocator.IsValid(first) && allocator.GetLiveCount() == 3,
            "A live descriptor allocation was rejected.");
        RequireThrows<std::runtime_error>([&allocator]() { (void)allocator.Allocate(); },
            "Descriptor capacity exhaustion did not report an error.");

        allocator.Release(first, 42);
        Require(!allocator.IsValid(first) && allocator.GetLiveCount() == 2,
            "A released descriptor handle remained valid.");
        Require(allocator.GetRetiredCount() == 1 && allocator.GetAvailableCount() == 0,
            "An in-flight descriptor index was made available immediately.");
        RequireThrows<std::out_of_range>([&allocator, first]() { allocator.Release(first, 42); },
            "A stale descriptor handle was released twice.");

        allocator.Collect(41);
        Require(allocator.GetRetiredCount() == 1 && allocator.GetAvailableCount() == 0,
            "Descriptor index recycled before its submission completed.");
        RequireThrows<std::runtime_error>([&allocator]() { (void)allocator.Allocate(); },
            "Descriptor exhaustion ignored an in-flight retirement quarantine.");

        allocator.Collect(42);
        Require(allocator.GetRetiredCount() == 0 && allocator.GetAvailableCount() == 1,
            "Completed descriptor retirement was not collected.");
        const DescriptorAllocationHandle recycled = allocator.Allocate();
        Require(recycled.Index == first.Index && recycled.Generation != first.Generation,
            "Recycled descriptor index did not advance generation.");
        Require(allocator.IsValid(recycled) && !allocator.IsValid(first),
            "Descriptor generation failed to reject a stale handle after reuse.");

        allocator.Release(second, 0);
        Require(allocator.GetRetiredCount() == 0 && allocator.GetAvailableCount() == 1,
            "Immediate descriptor retirement incorrectly entered GPU quarantine.");

        RequireThrows<std::invalid_argument>([]() { DescriptorIndexAllocator invalid(0); },
            "Zero descriptor capacity was accepted.");
        RequireThrows<std::invalid_argument>([]() { DescriptorIndexAllocator invalid(2, 3); },
            "Descriptor reserved count greater than capacity was accepted.");

        ManagedTextureReferenceState textureReferences;
        const ManagedTextureHandle textureOwnerA = textureReferences.Acquire(7);
        const ManagedTextureHandle textureOwnerB = textureReferences.Acquire(7);
        Require(textureOwnerA == textureOwnerB &&
            textureReferences.GetReferenceCount() == 2,
            "Shared texture cache hits did not retain one generation with two owners.");
        Require(!textureReferences.Release(textureOwnerA) &&
            textureReferences.GetReferenceCount() == 1 &&
            textureReferences.IsValid(textureOwnerB),
            "Releasing one shared texture owner invalidated the remaining owner.");
        Require(textureReferences.Release(textureOwnerB) &&
            !textureReferences.IsValid(textureOwnerB),
            "The final shared texture release did not invalidate the generation.");
        RequireThrows<std::out_of_range>(
            [&textureReferences, textureOwnerB]()
            {
                (void)textureReferences.Release(textureOwnerB);
            },
            "A stale managed texture handle was released twice.");
        const ManagedTextureHandle textureOwnerC = textureReferences.Acquire(7);
        Require(textureOwnerC.Generation != textureOwnerB.Generation &&
            textureReferences.IsValid(textureOwnerC),
            "A recycled managed texture slot did not advance its generation.");
        Require(textureReferences.Release(textureOwnerC),
            "The recycled managed texture owner did not release cleanly.");
        RequireThrows<std::invalid_argument>(
            [&textureReferences]()
            {
                (void)textureReferences.Acquire(InvalidDescriptorIndex);
            },
            "Managed texture references accepted an invalid slot index.");

        DefaultBindlessTexture defaultTexture = DefaultBindlessTexture::Black;
        Require(TryParseDefaultBindlessTexture("white", defaultTexture) &&
            defaultTexture == DefaultBindlessTexture::White,
            "The legacy white sentinel was not mapped to the reserved default descriptor.");
        Require(TryParseDefaultBindlessTexture(
                "Assets/Textures/DefaultNormal.png", defaultTexture) &&
            defaultTexture == DefaultBindlessTexture::Normal,
            "The default normal asset alias was not mapped to the reserved descriptor.");
        Require(!TryParseDefaultBindlessTexture(
                "Assets/Textures/Pbr/silver/albedo.png", defaultTexture),
            "A real texture asset was incorrectly collapsed to a default descriptor.");
    }
}
