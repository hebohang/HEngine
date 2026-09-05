#include "Hepch.h"

#include "Runtime/UnitTest/RenderRhiContractTests.h"

#include "Runtime/Graphics/RHI/PipelineCache.h"
#include "Runtime/Graphics/RHI/MaterialDescriptorPaging.h"
#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/RHI/TextureReadback.h"
#include "Runtime/Graphics/RHI/TextureAlphaDilation.h"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace HEngine::Tests
{
    struct TextureReadbackQueueTestAccess
    {
        [[nodiscard]] static TextureReadbackHandle AddAsyncPngCompletion(
            TextureReadbackQueue& queue,
            uint64_t submission,
            std::shared_future<void> completionGate)
        {
            TextureReadbackHandle handle = queue.Allocate();
            TextureReadbackQueue::Slot& slot = queue.Require(handle);
            slot.Submission = submission;
            slot.Result = {
                .Status = TextureReadbackStatus::InFlight,
                .OutputPath = "TextureReadbackPngContract.png",
            };
            slot.Completion = std::async(
                std::launch::async,
                [completionGate = std::move(completionGate)]()
                    -> TextureReadbackQueue::AsyncCompletion
                {
                    completionGate.wait();
                    const uint8_t bgraPixel[4] = { 1, 2, 3, 4 };
                    const std::vector<uint8_t> png =
                        TextureReadbackQueue::EncodeRgba8Png(
                            1,
                            1,
                            bgraPixel,
                            sizeof(bgraPixel),
                            true);
                    return {
                        .bSucceeded = png.size() == 73,
                        .Error = png.size() == 73
                            ? std::string{}
                            : "Asynchronous PNG encoding produced an unexpected size",
                    };
                });
            return handle;
        }

        static void ResetWithoutDevice(TextureReadbackQueue& queue) noexcept
        {
            for (TextureReadbackQueue::Slot& slot : queue.mSlots)
            {
                if (!slot.bOccupied)
                    continue;
                if (slot.Completion.valid())
                {
                    slot.Completion.wait();
                    try
                    {
                        (void)slot.Completion.get();
                    }
                    catch (...)
                    {
                    }
                }
                slot.Staging = nullptr;
                slot.bOccupied = false;
                slot.bPendingCounted = false;
            }
            queue.mPendingCount = 0;
        }

        static void AddThrowingDestructorCompletion(TextureReadbackQueue& queue)
        {
            const TextureReadbackHandle handle = queue.Allocate();
            TextureReadbackQueue::Slot& slot = queue.Require(handle);
            slot.Result.Status = TextureReadbackStatus::Complete;
            slot.Completion = std::async(
                std::launch::async,
                []() -> TextureReadbackQueue::AsyncCompletion
                {
                    throw std::runtime_error("intentional destructor completion failure");
                });
        }
    };

    namespace
    {
        class CompletionGate final
        {
        public:
            CompletionGate()
                : mFuture(mPromise.get_future().share())
            {
            }

            ~CompletionGate()
            {
                Signal();
            }

            [[nodiscard]] std::shared_future<void> GetFuture() const
            {
                return mFuture;
            }

            void Signal()
            {
                if (mSignaled)
                    return;
                mPromise.set_value();
                mSignaled = true;
            }

        private:
            std::promise<void> mPromise;
            std::shared_future<void> mFuture;
            bool mSignaled = false;
        };

        class TextureReadbackQueueCleanup final
        {
        public:
            TextureReadbackQueueCleanup(
                TextureReadbackQueue& queue,
                CompletionGate& firstGate,
                CompletionGate& secondGate)
                : mQueue(queue), mFirstGate(firstGate), mSecondGate(secondGate)
            {
            }

            ~TextureReadbackQueueCleanup()
            {
                mFirstGate.Signal();
                mSecondGate.Signal();
                TextureReadbackQueueTestAccess::ResetWithoutDevice(mQueue);
            }

        private:
            TextureReadbackQueue& mQueue;
            CompletionGate& mFirstGate;
            CompletionGate& mSecondGate;
        };

        template<typename Left, typename Right>
        concept EqualityComparable = requires(const Left& left, const Right& right)
        {
            left == right;
        };

        static_assert(!std::is_same_v<BindingLayoutKey, BindlessLayoutKey>);
        static_assert(!std::is_convertible_v<BindlessLayoutKey, BindingLayoutKey>);
        static_assert(!std::is_convertible_v<BindingLayoutKey, BindlessLayoutKey>);
        static_assert(!EqualityComparable<BindingLayoutKey, BindlessLayoutKey>);
        static_assert(!std::is_same_v<GraphicsPipelineKey, ComputePipelineKey>);
        static_assert(!std::is_convertible_v<ComputePipelineKey, GraphicsPipelineKey>);
        static_assert(GetShaderBinaryFormat(RenderBackend::DX12) ==
            ShaderBinaryFormat::Dxil);
        static_assert(GetShaderBinaryFormat(RenderBackend::Vulkan) ==
            ShaderBinaryFormat::SpirV);
        static_assert(IsShaderArtifactTargetCompatible(
            RenderBackend::DX12, ShaderBinaryFormat::Dxil));
        static_assert(IsShaderArtifactTargetCompatible(
            RenderBackend::Vulkan, ShaderBinaryFormat::SpirV));
        static_assert((static_cast<uint16_t>(nvrhi::ShaderType::AllRayTracing) &
            static_cast<uint16_t>(nvrhi::ShaderType::RayGeneration)) != 0);
        static_assert((static_cast<uint16_t>(nvrhi::ShaderType::AllRayTracing) &
            static_cast<uint16_t>(nvrhi::ShaderType::Miss)) != 0);
        static_assert((static_cast<uint16_t>(nvrhi::ShaderType::AllRayTracing) &
            static_cast<uint16_t>(nvrhi::ShaderType::ClosestHit)) != 0);
        static_assert((static_cast<uint16_t>(nvrhi::ShaderType::AllRayTracing) &
            static_cast<uint16_t>(nvrhi::ShaderType::AnyHit)) != 0);
        static_assert((static_cast<uint16_t>(nvrhi::ShaderType::AllRayTracing) &
            static_cast<uint16_t>(nvrhi::ShaderType::Callable)) != 0);

        using BindingLayoutFunction = decltype(&PipelineCache::GetOrCreateBindingLayout);
        using BindlessLayoutFunction = decltype(&PipelineCache::GetOrCreateBindlessLayout);
        static_assert(!std::is_invocable_v<
            BindingLayoutFunction,
            PipelineCache&,
            BindlessLayoutKey,
            const nvrhi::BindingLayoutDesc&,
            std::string_view>);
        static_assert(!std::is_invocable_v<
            BindlessLayoutFunction,
            PipelineCache&,
            BindingLayoutKey,
            const nvrhi::BindlessLayoutDesc&,
            std::string_view>);

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

        void StoreLittleEndian16(uint8_t* destination, uint16_t value)
        {
            destination[0] = static_cast<uint8_t>(value);
            destination[1] = static_cast<uint8_t>(value >> 8u);
        }

        void StoreLittleEndian32(uint8_t* destination, uint32_t value)
        {
            destination[0] = static_cast<uint8_t>(value);
            destination[1] = static_cast<uint8_t>(value >> 8u);
            destination[2] = static_cast<uint8_t>(value >> 16u);
            destination[3] = static_cast<uint8_t>(value >> 24u);
        }

        [[nodiscard]] float ReadLittleEndianFloat(
            const std::vector<uint8_t>& bytes,
            size_t offset)
        {
            Require(offset <= bytes.size() && bytes.size() - offset >= sizeof(uint32_t),
                "Texture readback test attempted to read beyond an encoded payload.");
            const uint32_t bits = static_cast<uint32_t>(bytes[offset]) |
                (static_cast<uint32_t>(bytes[offset + 1]) << 8u) |
                (static_cast<uint32_t>(bytes[offset + 2]) << 16u) |
                (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
            return std::bit_cast<float>(bits);
        }

        [[nodiscard]] ShaderCacheKey MakeShaderKey()
        {
            return {
                .Source = "Shaders/BasePBR.hlsl",
                .Variant = "SKINNED=0;ALPHA_TEST=0",
                .Stage = nvrhi::ShaderType::Pixel,
                .EntryPoint = "PSMain",
                .TargetProfile = "ps_6_0",
                .ContentHash = "source+includes+macros+options:a1b2c3"
            };
        }
    }

    void MaterialDescriptorPagingContract()
    {
        constexpr std::array<MaterialDescriptorTextureKey, 3> reserved = {
            1,
            2,
            3,
        };
        std::vector<MaterialDescriptorPageInput> materials;
        for (std::uint32_t materialIndex = 0; materialIndex < 60;
            ++materialIndex)
        {
            const MaterialDescriptorTextureKey base =
                1000u + static_cast<MaterialDescriptorTextureKey>(materialIndex) * 3u;
            materials.push_back({
                .MaterialIndex = materialIndex,
                .TextureKeys = { base, base + 1u, base + 2u, base },
            });
        }

        const MaterialDescriptorPagingResult result =
            BuildMaterialDescriptorPages(materials, reserved, 120);
        Require(result.Pages.size() == 2 &&
                result.Assignments.size() == materials.size() &&
                result.PeakPageTextureCount <= 120,
            "Descriptor paging did not split a greater-than-120 texture scene safely.");
        for (const MaterialDescriptorPageAssignment& assignment :
            result.Assignments)
        {
            Require(assignment.PageIndex < result.Pages.size() &&
                    assignment.LocalTextureIndices.size() == 4 &&
                    assignment.LocalTextureIndices.front() ==
                        assignment.LocalTextureIndices.back(),
                "Descriptor paging lost a material-local texture mapping.");
            for (const std::uint32_t localIndex :
                assignment.LocalTextureIndices)
            {
                Require(localIndex < result.Pages[assignment.PageIndex].size(),
                    "Descriptor paging produced an out-of-page texture index.");
            }
        }

        RequireThrows<std::length_error>([&]()
        {
            MaterialDescriptorPageInput oversized{ .MaterialIndex = 99 };
			// 118 non-reserved textures plus the three defaults exceed a
			// 120-entry page by exactly one.
            for (MaterialDescriptorTextureKey key = 100; key < 218; ++key)
                oversized.TextureKeys.push_back(key);
            (void)BuildMaterialDescriptorPages(
                std::span<const MaterialDescriptorPageInput>(&oversized, 1),
                reserved,
                120);
        }, "A single material larger than a descriptor page was accepted.");
    }

	void TextureAlphaDilationContract()
	{
		const std::array<std::uint8_t, 20> source = {
			255, 32, 16, 255,
			0, 0, 0, 0,
			0, 0, 0, 0,
			16, 64, 255, 255,
			0, 0, 0, 0,
		};
		const TextureAlphaDilationResult result = DilateMaskedTextureRgb(
			std::as_bytes(std::span(source)),
			5,
			1,
			0.5f);
		Require(result.DilatedPixelCount == 3 &&
			result.Pixels.size() == source.size(),
			"Mask RGB dilation did not process every rejected texel.");
		for (std::size_t pixel = 0; pixel < 5; ++pixel)
		{
			const std::size_t offset = pixel * 4u;
			Require(std::to_integer<std::uint8_t>(result.Pixels[offset]) != 0u ||
				std::to_integer<std::uint8_t>(result.Pixels[offset + 1u]) != 0u ||
				std::to_integer<std::uint8_t>(result.Pixels[offset + 2u]) != 0u,
				"Mask RGB dilation retained black transparent padding.");
			Require(std::to_integer<std::uint8_t>(result.Pixels[offset + 3u]) ==
				source[offset + 3u],
				"Mask RGB dilation modified source alpha coverage.");
		}

		const std::array<std::uint8_t, 8> fractionalCutoffSource = {
			0, 0, 0, 84,
			40, 80, 120, 85,
		};
		const TextureAlphaDilationResult fractionalCutoffResult =
			DilateMaskedTextureRgb(
				std::as_bytes(std::span(fractionalCutoffSource)),
				2,
				1,
				0.33f);
		Require(fractionalCutoffResult.DilatedPixelCount == 1 &&
			std::to_integer<std::uint8_t>(
				fractionalCutoffResult.Pixels[0]) == 40u,
			"Mask RGB dilation cutoff did not match shader alpha testing.");

		const std::array<std::uint8_t, 8> opaqueSource = {
			1, 2, 3, 255,
			4, 5, 6, 255,
		};
		const TextureAlphaDilationResult opaqueResult = DilateMaskedTextureRgb(
			std::as_bytes(std::span(opaqueSource)),
			2,
			1,
			0.5f);
		Require(!opaqueResult.WasApplied() && opaqueResult.Pixels.empty(),
			"Opaque mask texture allocated an unnecessary dilation copy.");
	}

    void PipelineCacheKeyContract()
    {
        constexpr BindingLayoutKey ordinary{ .Value = 17 };
        constexpr BindlessLayoutKey bindless{ .Value = 17 };
        constexpr StaticBindingSetKey bindingSet{ .Value = 17 };
        constexpr GraphicsPipelineKey graphics{ .Value = 17 };
        constexpr ComputePipelineKey compute{ .Value = 17 };
        static_assert(static_cast<bool>(ordinary));
        static_assert(static_cast<bool>(bindless));
        static_assert(static_cast<bool>(bindingSet));
        static_assert(static_cast<bool>(graphics));
        static_assert(static_cast<bool>(compute));

        Require(!static_cast<bool>(BindingLayoutKey{}) &&
                !static_cast<bool>(BindlessLayoutKey{}) &&
                !static_cast<bool>(GraphicsPipelineKey{}),
            "A zero pipeline-cache key was reported as valid.");
    }

    void ShaderCacheKeyContract()
    {
        const ShaderCacheKey base = MakeShaderKey();
        const ShaderCacheKeyHash hash;
        const size_t baseHash = hash(base);

        std::unordered_set<ShaderCacheKey, ShaderCacheKeyHash> keys;
        keys.insert(base);
        keys.insert(base);

        auto requireDistinctField = [&keys, &hash, &base, baseHash](
            ShaderCacheKey changed,
            std::string_view field)
        {
            Require(changed != base,
                std::string("Shader cache equality omitted field: ") + std::string(field));
            Require(hash(changed) != baseHash,
                std::string("Shader cache hash omitted field: ") + std::string(field));
            keys.insert(std::move(changed));
        };

        ShaderCacheKey changed = base;
        changed.Source = "Shaders/BasePBR_Changed.hlsl";
        requireDistinctField(std::move(changed), "Source");
        changed = base;
        changed.Variant = "SKINNED=1;ALPHA_TEST=0";
        requireDistinctField(std::move(changed), "Variant");
        changed = base;
        changed.Stage = nvrhi::ShaderType::Vertex;
        requireDistinctField(std::move(changed), "Stage");
        changed = base;
        changed.EntryPoint = "PSShadow";
        requireDistinctField(std::move(changed), "EntryPoint");
        changed = base;
        changed.TargetProfile = "ps_6_7";
        requireDistinctField(std::move(changed), "TargetProfile");
        changed = base;
        changed.ContentHash = "source+includes+macros+options:d4e5f6";
        requireDistinctField(std::move(changed), "ContentHash");
        changed = base;
        changed.Backend = RenderBackend::Vulkan;
        requireDistinctField(std::move(changed), "Backend");
        changed = base;
        changed.BinaryFormat = ShaderBinaryFormat::SpirV;
        requireDistinctField(std::move(changed), "BinaryFormat");

        Require(keys.size() == 9,
            "Shader cache key hashing or equality collapsed distinct key fields.");

        ShaderCacheKey canonicalSource = base;
        canonicalSource.Source = "SHADERS\\BASEPBR.HLSL";
        Require(ShaderCacheKeysMatchCanonicalIdentity(base, canonicalSource),
            "Shader retirement did not canonicalize project source identity.");
        ShaderCacheKey byteIdenticalDeletedVariant = base;
        byteIdenticalDeletedVariant.Variant = "SKINNED=0;ALPHA_TEST=1";
        Require(
            byteIdenticalDeletedVariant.ContentHash == base.ContentHash &&
                !ShaderCacheKeysMatchCanonicalIdentity(
                    base, byteIdenticalDeletedVariant),
            "Shader retirement collapsed byte-identical variants to ContentHash.");

        ShaderCacheKey spirV = base;
        spirV.Backend = RenderBackend::Vulkan;
        spirV.BinaryFormat = ShaderBinaryFormat::SpirV;
        Require(!ShaderCacheKeysMatchCanonicalIdentity(base, spirV),
            "Shader retirement collapsed DXIL and SPIR-V artifacts.");

        const ShaderLibraryCacheKey libraryBase{
            .Source = "Shaders/ReferencePathTracer.hlsl",
            .Variant = "REFERENCE",
            .TargetProfile = "lib_6_3",
            .ContentHash = "source+includes+options:1234",
            .Backend = RenderBackend::DX12,
            .BinaryFormat = ShaderBinaryFormat::Dxil,
        };
        ShaderLibraryCacheKey librarySpirV = libraryBase;
        librarySpirV.Backend = RenderBackend::Vulkan;
        librarySpirV.BinaryFormat = ShaderBinaryFormat::SpirV;
        std::unordered_set<ShaderLibraryCacheKey, ShaderLibraryCacheKeyHash> libraries;
        libraries.insert(libraryBase);
        libraries.insert(librarySpirV);
        Require(libraries.size() == 2,
            "Shader library cache identity collapsed DXIL and SPIR-V artifacts.");

        const std::filesystem::path caches =
            "C:/HEngineSynthetic/Generated/Shaders/CACHES";
        const ShaderArtifactResolver dxilResolver(
            caches / "DXIL", RenderBackend::DX12);
        const ShaderArtifactResolver spirvResolver(
            caches / "SPIRV", RenderBackend::Vulkan);
        Require(dxilResolver.GetCachesRoot() == caches &&
                dxilResolver.GetArtifactDirectory() == caches / "DXIL" &&
                dxilResolver.Resolve("DXIL/SceneOpaque.vs") ==
                    caches / "DXIL/SceneOpaque.vs" &&
                spirvResolver.GetCachesRoot() == caches &&
                spirvResolver.GetArtifactDirectory() == caches / "SPIRV" &&
                spirvResolver.Resolve("DXIL/SceneOpaque.vs") ==
                    caches / "SPIRV/SceneOpaque.vs",
            "Shader artifact resolution leaked a fixed binary-format directory into callers.");
    }

    void TextureReadbackPngContract()
    {
        const uint8_t bgraPixel[4] = { 1, 2, 3, 4 };
        const std::vector<uint8_t> png = TextureReadbackQueue::EncodeRgba8Png(
            1, 1, bgraPixel, sizeof(bgraPixel), true);

        constexpr uint8_t signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        Require(png.size() == 73 &&
                std::equal(std::begin(signature), std::end(signature), png.begin()),
            "Texture readback encoder produced an invalid PNG signature or layout.");
        Require(png[16] == 0 && png[17] == 0 && png[18] == 0 && png[19] == 1 &&
                png[20] == 0 && png[21] == 0 && png[22] == 0 && png[23] == 1,
            "Texture readback encoder wrote the wrong IHDR dimensions.");

        // The test image fits in one uncompressed DEFLATE block. After zlib's
        // two-byte header and the stored-block header, the scanline contains a
        // zero PNG filter byte followed by the BGRA-to-RGBA swizzle.
        Require(png[48] == 0 && png[49] == 3 && png[50] == 2 &&
                png[51] == 1 && png[52] == 4,
            "Texture readback PNG did not preserve alpha or swizzle BGRA to RGBA.");

        RequireThrows<std::invalid_argument>([]() {
            const uint8_t pixel[4] = {};
            (void)TextureReadbackQueue::EncodeRgba8Png(1, 1, pixel, 3, false);
        }, "Texture readback PNG accepted an undersized row pitch.");

        Require(
            TextureReadbackQueue::ResolveOutputFormat("capture.PFM") ==
                TextureReadbackOutputFormat::Pfm &&
            TextureReadbackQueue::ResolveOutputFormat("capture.raw") ==
                TextureReadbackOutputFormat::Raw &&
            TextureReadbackQueue::ResolveOutputFormat(
                "capture.unknown", TextureReadbackOutputFormat::Pfm) ==
                TextureReadbackOutputFormat::Pfm,
            "Texture readback output format resolution ignored an extension or explicit override.");
        RequireThrows<std::invalid_argument>([]() {
            (void)TextureReadbackQueue::ResolveOutputFormat("capture.unknown");
        }, "Texture readback accepted an unknown output extension in Auto mode.");

        constexpr std::string_view grayPfmHeader = "Pf\n2 2\n-1.0\n";
        std::array<uint8_t, 12> halfGray{};
        halfGray.fill(0xcdu);
        StoreLittleEndian16(halfGray.data(), 0x3c00u);       // 1
        StoreLittleEndian16(halfGray.data() + 2, 0x7c00u);  // +inf
        StoreLittleEndian16(halfGray.data() + 6, 0xc000u);  // -2
        StoreLittleEndian16(halfGray.data() + 8, 0x7e01u);  // NaN
        const std::vector<uint8_t> halfGrayPfm = TextureReadbackQueue::EncodePfm(
            2, 2, nvrhi::Format::R16_FLOAT, halfGray.data(), 6,
            TextureReadbackAlphaMode::Preserve,
            TextureReadbackNonFinitePolicy::Preserve);
        Require(halfGrayPfm.size() == grayPfmHeader.size() + 4u * sizeof(float) &&
                std::equal(grayPfmHeader.begin(), grayPfmHeader.end(), halfGrayPfm.begin()),
            "R16_FLOAT PFM has an invalid header or payload size.");
        const float pfmBottomLeft = ReadLittleEndianFloat(halfGrayPfm, grayPfmHeader.size());
        const float pfmBottomRight = ReadLittleEndianFloat(
            halfGrayPfm, grayPfmHeader.size() + sizeof(float));
        const float pfmTopLeft = ReadLittleEndianFloat(
            halfGrayPfm, grayPfmHeader.size() + 2u * sizeof(float));
        const float pfmTopRight = ReadLittleEndianFloat(
            halfGrayPfm, grayPfmHeader.size() + 3u * sizeof(float));
        Require(pfmBottomLeft == -2.f && std::isnan(pfmBottomRight) &&
                pfmTopLeft == 1.f && std::isinf(pfmTopRight) && pfmTopRight > 0.f,
            "R16_FLOAT PFM did not convert half values, preserve non-finite values, or flip rows.");
        RequireThrows<std::domain_error>([&halfGray]() {
            (void)TextureReadbackQueue::EncodePfm(
                2, 2, nvrhi::Format::R16_FLOAT, halfGray.data(), 6);
        }, "Texture readback PFM did not reject non-finite values by default.");

        std::array<uint8_t, 8> halfRgba{};
        StoreLittleEndian16(halfRgba.data(), 0x3c00u);
        StoreLittleEndian16(halfRgba.data() + 2, 0x4000u);
        StoreLittleEndian16(halfRgba.data() + 4, 0x4200u);
        StoreLittleEndian16(halfRgba.data() + 6, 0x4400u);
        constexpr std::string_view colorPfmHeader = "PF\n1 1\n-1.0\n";
        const std::vector<uint8_t> halfRgbaPfm = TextureReadbackQueue::EncodePfm(
            1, 1, nvrhi::Format::RGBA16_FLOAT, halfRgba.data(), halfRgba.size(),
            TextureReadbackAlphaMode::Discard);
        Require(halfRgbaPfm.size() == colorPfmHeader.size() + 3u * sizeof(float) &&
                ReadLittleEndianFloat(halfRgbaPfm, colorPfmHeader.size()) == 1.f &&
                ReadLittleEndianFloat(
                    halfRgbaPfm, colorPfmHeader.size() + sizeof(float)) == 2.f &&
                ReadLittleEndianFloat(
                    halfRgbaPfm, colorPfmHeader.size() + 2u * sizeof(float)) == 3.f,
            "RGBA16_FLOAT PFM did not preserve RGB or omit the unrepresentable alpha channel.");
        RequireThrows<std::invalid_argument>([&halfRgba]() {
            (void)TextureReadbackQueue::EncodePfm(
                1, 1, nvrhi::Format::RGBA16_FLOAT,
                halfRgba.data(), halfRgba.size());
        }, "Texture readback silently discarded RGBA alpha in PFM output.");

        std::array<uint8_t, 4> floatGray{};
        StoreLittleEndian32(floatGray.data(), std::bit_cast<uint32_t>(42.5f));
        const std::vector<uint8_t> floatGrayPfm = TextureReadbackQueue::EncodePfm(
            1, 1, nvrhi::Format::R32_FLOAT, floatGray.data(), floatGray.size());
        Require(ReadLittleEndianFloat(floatGrayPfm, std::string_view("Pf\n1 1\n-1.0\n").size()) == 42.5f,
            "R32_FLOAT PFM changed a finite source value.");

        std::array<uint8_t, 40> floatRgbaRows{};
        floatRgbaRows.fill(0xeeu);
        const std::array<uint32_t, 8> floatRgbaBits = {
            std::bit_cast<uint32_t>(1.f), 0x7fc12345u, 0x7f800000u, 0xff800000u,
            0x80000000u, std::bit_cast<uint32_t>(2.f),
            std::bit_cast<uint32_t>(3.f), std::bit_cast<uint32_t>(4.f),
        };
        for (uint32_t component = 0; component < 4; ++component)
        {
            StoreLittleEndian32(
                floatRgbaRows.data() + component * sizeof(uint32_t),
                floatRgbaBits[component]);
            StoreLittleEndian32(
                floatRgbaRows.data() + 20u + component * sizeof(uint32_t),
                floatRgbaBits[4u + component]);
        }
        const TextureReadbackRawEncoding floatRaw = TextureReadbackQueue::EncodeRaw(
            1, 2, nvrhi::Format::RGBA32_FLOAT, floatRgbaRows.data(), 20,
            TextureReadbackNonFinitePolicy::Preserve);
        Require(floatRaw.Pixels.size() == 8u * sizeof(uint32_t) &&
                std::memcmp(floatRaw.Pixels.data(), floatRgbaRows.data(), 16) == 0 &&
                std::memcmp(floatRaw.Pixels.data() + 16, floatRgbaRows.data() + 20, 16) == 0,
            "RGBA32_FLOAT raw encoding retained row padding or changed top-down component bits.");
        Require(floatRaw.MetadataJson.find("\"channels\": 4") != std::string::npos &&
                floatRaw.MetadataJson.find("\"type\": \"float32\"") != std::string::npos &&
                floatRaw.MetadataJson.find("\"endianness\": \"little\"") != std::string::npos &&
                floatRaw.MetadataJson.find("\"colorSpace\": \"linear\"") != std::string::npos &&
                floatRaw.MetadataJson.find("\"row_order\": \"top-down\"") != std::string::npos &&
                floatRaw.MetadataJson.find("\"nan_count\": 1") != std::string::npos &&
                floatRaw.MetadataJson.find("\"positive_infinity_count\": 1") != std::string::npos &&
                floatRaw.MetadataJson.find("\"negative_infinity_count\": 1") != std::string::npos,
            "RGBA32_FLOAT raw metadata omitted layout, color-space, type, or non-finite policy evidence.");
        RequireThrows<std::domain_error>([&floatRgbaRows]() {
            (void)TextureReadbackQueue::EncodeRaw(
                1, 2, nvrhi::Format::RGBA32_FLOAT, floatRgbaRows.data(), 20);
        }, "Texture readback raw encoding did not reject non-finite values by default.");

        const TextureReadbackRawEncoding halfRaw = TextureReadbackQueue::EncodeRaw(
            2, 2, nvrhi::Format::R16_FLOAT, halfGray.data(), 6,
            TextureReadbackNonFinitePolicy::Preserve);
        const std::array<uint8_t, 8> expectedHalfRaw = {
            0x00, 0x3c, 0x00, 0x7c, 0x00, 0xc0, 0x01, 0x7e,
        };
        Require(halfRaw.Pixels.size() == expectedHalfRaw.size() &&
                std::equal(expectedHalfRaw.begin(), expectedHalfRaw.end(), halfRaw.Pixels.begin()) &&
                halfRaw.MetadataJson.find("\"type\": \"float16\"") != std::string::npos,
            "R16_FLOAT raw encoding did not preserve half bits or strip row padding.");

        std::array<uint8_t, 4> uintPixel{};
        StoreLittleEndian32(uintPixel.data(), 0xfedcba98u);
        const TextureReadbackRawEncoding uintRaw = TextureReadbackQueue::EncodeRaw(
            1, 1, nvrhi::Format::R32_UINT, uintPixel.data(), uintPixel.size());
        Require(uintRaw.Pixels == std::vector<uint8_t>(uintPixel.begin(), uintPixel.end()) &&
                uintRaw.MetadataJson.find("\"type\": \"uint32\"") != std::string::npos &&
                uintRaw.MetadataJson.find("\"colorSpace\": \"data\"") != std::string::npos,
            "R32_UINT raw encoding changed ID bits or mislabeled them as linear color.");
        RequireThrows<std::invalid_argument>([&uintPixel]() {
            (void)TextureReadbackQueue::EncodePfm(
                1, 1, nvrhi::Format::R32_UINT, uintPixel.data(), uintPixel.size());
        }, "Texture readback PFM accepted an integer texture.");
        RequireThrows<std::invalid_argument>([&halfRgba]() {
            (void)TextureReadbackQueue::EncodeRaw(
                1, 1, nvrhi::Format::RGBA16_FLOAT, halfRgba.data(), 7);
        }, "Texture readback raw encoder accepted an undersized row pitch.");

        alignas(nvrhi::IDevice) std::byte deviceSentinel{};
        TextureReadbackQueue queue(
            reinterpret_cast<nvrhi::IDevice*>(&deviceSentinel));
        CompletionGate firstGate;
        CompletionGate secondGate;
        TextureReadbackQueueCleanup cleanup(queue, firstGate, secondGate);

        const TextureReadbackHandle first =
            TextureReadbackQueueTestAccess::AddAsyncPngCompletion(
                queue,
                1,
                firstGate.GetFuture());
        const TextureReadbackHandle second =
            TextureReadbackQueueTestAccess::AddAsyncPngCompletion(
                queue,
                2,
                secondGate.GetFuture());
        Require(queue.GetPendingCount() == 2,
            "Texture readback pending count did not include asynchronous work.");

        std::future<void> collect = std::async(std::launch::async, [&queue]() {
            queue.Collect(1);
        });
        const bool bCollectReturned =
            collect.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!bCollectReturned)
            firstGate.Signal();
        collect.get();
        Require(bCollectReturned,
            "Texture readback Collect blocked on asynchronous PNG completion.");
        Require(queue.GetPendingCount() == 2 &&
                queue.GetResult(first).Status == TextureReadbackStatus::InFlight,
            "Non-blocking texture readback collection completed pending CPU work.");

        queue.Release(first);
        Require(queue.IsValid(first) && queue.GetPendingCount() == 2,
            "An in-flight texture readback was released before CPU completion.");

        std::promise<void> firstDrainStartedPromise;
        std::future<void> firstDrainStarted = firstDrainStartedPromise.get_future();
        std::future<void> firstDrain = std::async(
            std::launch::async,
            [&queue, &firstDrainStartedPromise]() {
                firstDrainStartedPromise.set_value();
                queue.Drain(1);
            });
        firstDrainStarted.wait();
        const bool bFirstDrainBlocked =
            firstDrain.wait_for(std::chrono::milliseconds(100)) ==
                std::future_status::timeout;
        firstGate.Signal();
        firstDrain.get();
        Require(bFirstDrainBlocked,
            "Texture readback Drain returned before pending CPU work completed.");
        Require(!queue.IsValid(first) && queue.IsValid(second) &&
                queue.GetPendingCount() == 1,
            "Texture readback Drain did not finalize deferred release exactly once.");

        std::future<void> cutoffDrain = std::async(std::launch::async, [&queue]() {
            queue.Drain(1);
        });
        const bool bCutoffDrainReturned =
            cutoffDrain.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!bCutoffDrainReturned)
            secondGate.Signal();
        cutoffDrain.get();
        Require(bCutoffDrainReturned && queue.IsValid(second) &&
                queue.GetPendingCount() == 1,
            "Texture readback Drain waited for a submission beyond its completion cutoff.");

        std::promise<void> secondDrainStartedPromise;
        std::future<void> secondDrainStarted = secondDrainStartedPromise.get_future();
        std::future<void> secondDrain = std::async(
            std::launch::async,
            [&queue, &secondDrainStartedPromise]() {
                secondDrainStartedPromise.set_value();
                queue.Drain(2);
            });
        secondDrainStarted.wait();
        const bool bSecondDrainBlocked =
            secondDrain.wait_for(std::chrono::milliseconds(100)) ==
                std::future_status::timeout;
        secondGate.Signal();
        secondDrain.get();
        Require(bSecondDrainBlocked && queue.IsValid(second) &&
                queue.GetPendingCount() == 0 &&
                queue.GetResult(second).Status == TextureReadbackStatus::Complete,
            "Texture readback Drain did not publish asynchronous PNG completion.");

        queue.Release(second);
        Require(!queue.IsValid(second) && queue.GetPendingCount() == 0,
            "A completed texture readback release did not retire its handle.");

        // Destructors are implicitly noexcept. A failed CPU completion must be
        // logged and contained rather than terminating the process during
        // RuntimeRenderer/exception unwinding.
        {
            alignas(nvrhi::IDevice) std::byte destructorDeviceSentinel{};
            TextureReadbackQueue destructorQueue(
                reinterpret_cast<nvrhi::IDevice*>(&destructorDeviceSentinel));
            TextureReadbackQueueTestAccess::AddThrowingDestructorCompletion(
                destructorQueue);
        }
    }
}
