#include "Hepch.h"

#include "Runtime/UnitTest/RenderRhiContractTests.h"

#include "Runtime/Graphics/RHI/PipelineCache.h"
#include "Runtime/Graphics/RHI/MaterialDescriptorPaging.h"
#include "Runtime/Graphics/RHI/ShaderArtifactResolver.h"
#include "Runtime/Graphics/RHI/ShaderManager.h"
#include "Runtime/Graphics/RHI/TextureReadback.h"
#include "Runtime/Graphics/Renderer/MainViewAabb.h"
#include "Runtime/Graphics/GlobalIllumination/SsgiReactiveBudget.h"
#include "Runtime/Graphics/GlobalIllumination/SsgiReactiveShadowScope.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"
#include "Runtime/Graphics/RHI/TextureAlphaDilation.h"
#include "Runtime/Graphics/RHI/DdsTextureData.h"
#include "Runtime/Graphics/RHI/TextureStreamingPolicy.h"
#include "Runtime/Resource/AssetReadAhead.h"
#include "Runtime/Graphics/RHI/EnvironmentSamplingProposal.h"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

// Compile the actual scalar shader source, not a separately maintained copy.
namespace HEngine::Tests::ShaderMath
{
    using std::exp;
    using std::max;
    using std::min;
    using std::sqrt;
#include "../../../Content/Shaders/Shared/SurfaceLightingMath.hlsli"
}

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

    void RenderingShaderMathContract()
    {
        using namespace ShaderMath;
        // Independent double-precision midpoint quadrature tests the exact
        // density profile, including rays crossing the base and start plane.
        const auto integrateFog = [](double begin, double end, double length,
            double start, double base, double falloff, double density) {
            constexpr int steps = 16384;
            start = std::clamp(start, 0.0, length);
            const double step = (length - start) / steps;
            double sum = 0.0;
            for (int index = 0; index < steps; ++index)
            {
                const double distance = start + (index + 0.5) * step;
                const double height = begin + (end - begin) * distance / length;
                sum += std::exp(-falloff * std::max(height - base, 0.0));
            }
            return std::min(sum * step * density, 80.0);
        };
        for (const float begin : { -30.0f, 0.0f, 1.0f, 100.0f })
            for (const float end : { -20.0f, 0.0f, 1.00001f, 70.0f })
                for (const float start : { 0.0f, 40.0f, 200.0f })
                    for (const float falloff : { 0.0f, 0.0001f, 0.05f, 2.0f })
                    {
                        const float actual = HE_HeightFogOpticalDepth(
                            begin, end, 200.0f, start, 1.0f, falloff, 0.024f);
                        const double reference = integrateFog(
                            begin, end, 200.0, start, 1.0, falloff, 0.024);
                        Require(std::isfinite(actual) && actual >= 0.0f &&
                            std::abs(actual - reference) <= 2e-5 * std::max(reference, 1.0),
                            "Height fog integral diverged from density quadrature.");
                    }
        Require(HE_HeightFogOpticalDepth(0, 10, 0, 0, 0, 1, 1) == 0 &&
            HE_HeightFogOpticalDepth(0, 10, 10, 20, 0, 1, 1) == 0 &&
            HE_HeightFogOpticalDepth(0, 10, 10, 0, 0, 1, 0) == 0,
            "Empty or disabled fog segment must remain transparent.");

        // Compare with the original Smith lambda definition, independently
        // of the rearranged shader form; also verify reciprocity and finiteness.
        for (const float roughness : { 0.04f, 0.08f, 0.25f, 0.5f, 1.0f })
            for (const float nDotV : { 0.001f, 0.05f, 0.25f, 0.7f, 1.0f })
                for (const float nDotL : { 0.001f, 0.05f, 0.25f, 0.7f, 1.0f })
                {
                    const double alpha = std::max(double(roughness) * roughness, 0.0016);
                    const auto lambda = [alpha](double cosine) {
                        return 0.5 * (std::sqrt(1.0 + alpha * alpha *
                            (1.0 - cosine * cosine) / (cosine * cosine)) - 1.0);
                    };
                    const double reference = 1.0 / (4.0 * nDotV * nDotL *
                        (1.0 + lambda(nDotV) + lambda(nDotL)));
                    const float actual = HE_VisibilitySmithGGXCorrelated(nDotV, nDotL, roughness);
                    Require(std::isfinite(actual) && actual > 0.0f &&
                        std::abs(actual - reference) <= reference * 2e-5 &&
                        actual == HE_VisibilitySmithGGXCorrelated(nDotL, nDotV, roughness),
                        "GGX visibility must match Smith lambda and be reciprocal.");
                }
    }

    void EnvironmentSamplingProposalContract()
    {
        using Proposal = EnvironmentSamplingProposal;
        constexpr uint32_t width = Proposal::Width;
        constexpr uint32_t height = Proposal::Height;
        constexpr size_t values = size_t(width) * height * 4u;
        const auto validCdf = [](const std::shared_ptr<const Proposal>& proposal) {
            Require(bool(proposal), "Positive environment must publish a proposal");
            Require(proposal->Cdf.front() == 0.f && proposal->Cdf.back() == 1.f,
                "Published CDF endpoints must be exact zero and one");
            for (size_t index = 0; index < proposal->Cdf.size(); ++index)
            {
                Require(std::isfinite(proposal->Cdf[index]) &&
                    proposal->Cdf[index] >= 0.f && proposal->Cdf[index] <= 1.f,
                    "Every published CDF entry must be finite and normalized");
                if (index != 0)
                    Require(proposal->Cdf[index] >= proposal->Cdf[index - 1],
                        "Published CDF must be monotonic, including collapsed intervals");
            }
        };

        const auto empty = Proposal::Build(std::span<const float>{}, width, height);
        Require(!empty, "Empty span must not publish a usable environment proposal");
        const std::array<float, 4> onePixel{ 1.f, 1.f, 1.f, 1.f };
        const auto zeroWidth = Proposal::Build(onePixel, 0u, 1u);
        const auto zeroHeight = Proposal::Build(onePixel, 1u, 0u);
        Require(!zeroWidth && !zeroHeight, "Zero dimensions must return null");
        const auto shortSpan = Proposal::Build(onePixel, 2u, 1u);
        const auto longSpan = Proposal::Build(onePixel, 1u, 2u);
        Require(!shortSpan && !longSpan, "Mismatched RGBA span must return null");
        const std::array<float, 8> oversizedPixels{};
        const auto oversized = Proposal::Build(oversizedPixels, 1u, 1u);
        Require(!oversized, "Oversized RGBA span must return null");
        std::vector<float> pixels(values, 0.f);
        const auto black = Proposal::Build(pixels, width, height);
        Require(!black, "Black environment must use the explicit missing-proposal fallback");

        // One source pixel at exactly proposal resolution lies wholly inside one cell.
        // Its entire nonzero probability is known without reproducing Build's integration.
        constexpr size_t brightCell = size_t(32u) * width + 17u;
        pixels[brightCell * 4u + 2u] = 3.f;
        const auto isolated = Proposal::Build(pixels, width, height);
        validCdf(isolated);
        Require(isolated->Cdf[brightCell] == 0.f && isolated->Cdf[brightCell + 1u] == 1.f,
            "One isolated positive cell must own all probability mass");
        Require(isolated->Cdf[1u] == 0.f && isolated->Cdf[Proposal::Cells - 1u] == 1.f,
            "Unlit cells before and after the isolated emitter must have zero mass");

        pixels[brightCell * 4u] = -7.f;
        pixels[brightCell * 4u + 1u] = std::numeric_limits<float>::quiet_NaN();
        pixels[brightCell * 4u + 3u] = std::numeric_limits<float>::infinity();
        pixels[0] = -std::numeric_limits<float>::infinity();
        pixels[4] = std::numeric_limits<float>::quiet_NaN();
        const auto contaminated = Proposal::Build(pixels, width, height);
        validCdf(contaminated);
        Require(contaminated->Cdf == isolated->Cdf,
            "Negative/nonfinite RGB and nonfinite alpha must not poison finite blue energy");
        std::fill(pixels.begin(), pixels.end(), 0.f);
        const auto replacedWithBlack = Proposal::Build(pixels, width, height);
        Require(!replacedWithBlack && isolated->Cdf[brightCell + 1u] == 1.f,
            "Publishing another input must not mutate the previously owned CDF");

        // Constant radiance is uniform per steradian, not per latitude texel.
        // Analytic spherical-cap CDF at 45/90/135 degrees is (1-cos(theta))/2.
        // These closed-form checkpoints deliberately do not copy Build's pixel loop.
        std::fill(pixels.begin(), pixels.end(), 1.f);
        const auto uniform = Proposal::Build(pixels, width, height);
        validCdf(uniform);
        const double cap45 = (1.0 - std::sqrt(0.5)) * 0.5;
        constexpr double cdfTolerance = 2.0 * std::numeric_limits<float>::epsilon();
        Require(std::abs(double(uniform->Cdf[size_t(height / 4u) * width]) - cap45) < cdfTolerance &&
            std::abs(double(uniform->Cdf[size_t(height / 2u) * width]) - 0.5) < cdfTolerance &&
            std::abs(double(uniform->Cdf[size_t(3u * height / 4u) * width]) - (1.0 - cap45)) < cdfTolerance,
            "Constant-radiance CDF must match analytic spherical cap area");
        // A cell's mass has subtraction error from two quantized CDF endpoints.
        // Absolute tolerance, not relative error near tiny polar cells, is appropriate.
        constexpr double pi = 3.14159265358979323846;
        const double polarCell = (1.0 - std::cos(pi / height)) / (2.0 * width);
        const double equatorialCell = std::sin(pi / height) / (2.0 * width);
        const double actualPolar = double(uniform->Cdf[1u]) - double(uniform->Cdf[0u]);
        const size_t equator = size_t(height / 2u) * width;
        const double actualEquator = double(uniform->Cdf[equator + 1u]) - double(uniform->Cdf[equator]);
        Require(std::abs(actualPolar - polarCell) < cdfTolerance &&
            std::abs(actualEquator - equatorialCell) < cdfTolerance && actualPolar < actualEquator,
            "Constant radiance must weight polar cells by their smaller solid angle");

        pixels[0] = pixels[1] = pixels[2] = 1e30f;
        const auto highDynamicRange = Proposal::Build(pixels, width, height);
        validCdf(highDynamicRange);
        Require(highDynamicRange->Cdf[1u] == 1.f,
            "A dominant first cell must round to full CDF mass in this float stress case");
        bool bCollapsedPositiveInterval = false;
        for (size_t index = 1; index < Proposal::Cells; ++index)
            bCollapsedPositiveInterval |= highDynamicRange->Cdf[index] == highDynamicRange->Cdf[index + 1u];
        Require(bCollapsedPositiveInterval,
            "Float CDF can collapse tiny positive probabilities without invalidating endpoints");
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

    void TextureStreamingContract()
    {
        Require(TextureLastMip(1, 1) == 0 && TextureLastMip(4096, 1024) == 12,
            "Texture mip range must retain non-square tails");
        TextureStreamingDemand rgba{ 8, 4, 1, 4, 0, 1 };
        Require(TextureMipTailBytes(rgba, 0) == 172 && TextureMipTailBytes(rgba, 2) == 12,
            "RGBA mip accounting must include every tail level");
        TextureStreamingDemand bc{ 7, 5, 4, 16, 0, 1 };
        Require(TextureMipTailBytes(bc, 0) == 96 && TextureMipTailBytes(bc, 1) == 32,
            "Compressed mip accounting must round each level to complete blocks");
        std::array<TextureStreamingDemand, 2> demands = {
            TextureStreamingDemand{1024, 1024, 1, 4, 0, 1000},
            TextureStreamingDemand{1024, 1024, 1, 4, 0, 1} };
        const auto full = PlanTextureResidency(demands, 16ull * 1024 * 1024);
        Require(full.Mips == std::vector<uint32_t>({0, 0}) && !full.bMinimumExceedsBudget,
            "Sufficient residency budget must retain requested detail");
        const auto pressured = PlanTextureResidency(demands, 6ull * 1024 * 1024);
        Require(pressured.Bytes <= 6ull * 1024 * 1024 && pressured.Mips[0] < pressured.Mips[1],
            "Budget pressure must reduce low-priority detail first");
        const auto tiny = PlanTextureResidency(demands, 0);
        Require(tiny.bMinimumExceedsBudget && tiny.Bytes == 8 && tiny.Mips[0] == 10 && tiny.Mips[1] == 10,
            "Impossible budgets must retain a valid minimum and report the overrun");
        demands[0].DesiredMip = 100;
        demands[1].DesiredMip = 3;
        const auto clamped = PlanTextureResidency(demands, 1024ull * 1024);
        Require(clamped.Mips[0] == 10 && clamped.Mips[1] == 3,
            "Residency requests must clamp mip indices and never promote beyond demand");
        const auto empty = PlanTextureResidency({}, 0);
        Require(empty.Mips.empty() && empty.Bytes == 0 && !empty.bMinimumExceedsBudget,
            "Empty views require no residency");
    }

    void DdsTextureDataContract()
    {
        const auto write = [](std::vector<std::byte>& bytes, size_t offset, uint32_t value)
        {
            for (uint32_t index = 0; index < 4; ++index)
                bytes.at(offset + index) = std::byte((value >> (index * 8u)) & 255u);
        };
        const auto make = [&](uint32_t format, bool bLegacy = false)
        {
            // 7x5 -> 3x2 -> 1x1 exercises non-multiple-of-four blocks and tails.
            const size_t blockSize = format == 71 || format == 72 ? 8u : 16u;
            std::vector<std::byte> bytes((bLegacy ? 128 : 148) + 6 * blockSize);
            write(bytes, 0, 0x20534444u);
            write(bytes, 4, 124); write(bytes, 8, 0x21007u);
            write(bytes, 12, 5); write(bytes, 16, 7); write(bytes, 28, 3);
            write(bytes, 76, 32); write(bytes, 80, 4); write(bytes, 108, 0x401008u);
            write(bytes, 84, bLegacy ? (format == 71 ? 0x31545844u :
                (format == 77 ? 0x35545844u : 0x32495441u)) : 0x30315844u);
            if (!bLegacy)
            {
                write(bytes, 128, format); write(bytes, 132, 3);
                write(bytes, 140, 1); write(bytes, 144, 1);
            }
            return bytes;
        };
        for (uint32_t format : {71u, 72u, 77u, 78u, 83u, 98u, 99u})
        {
            const DdsTextureData dds = DdsTextureData::Parse(make(format));
            const size_t blockSize = format < 77 ? 8u : 16u;
            Require(dds.HasCompleteMipChain() && dds.Mips.size() == 3,
                "DDS full mip chain was not retained.");
            Require(dds.Mips[0].RowPitch == 2 * blockSize && dds.Mips[0].SlicePitch == 4 * blockSize &&
                dds.Mips[1].RowPitch == blockSize && dds.Mips[2].SlicePitch == blockSize,
                "DDS block rows or sub-block tail mips have incorrect upload pitches.");
            Require(dds.bSRgb == (format == 72 || format == 78 || format == 99),
                "DDS encoded color space was lost.");
            Require(dds.Mips.back().Offset + blockSize == dds.Bytes.size(),
                "DDS final payload offset exceeds the owning storage.");
        }
        auto hdrBytes = make(10);
        hdrBytes.resize(148 + (7 * 5 + 3 * 2 + 1) * 8);
        const auto hdr = DdsTextureData::Parse(hdrBytes);
        Require(hdr.Format == DdsBlockFormat::RGBA16Float && !hdr.bSRgb &&
            hdr.HasCompleteMipChain() && hdr.Mips[0].RowPitch == 56 &&
            hdr.Mips[0].SlicePitch == 280 && hdr.Mips[1].RowPitch == 24 &&
            hdr.Mips.back().SlicePitch == 8,
            "Linear HDR DDS mip pitches were treated as compressed blocks.");
        hdrBytes.pop_back();
        RequireThrows<std::runtime_error>([&] { (void)DdsTextureData::Parse(hdrBytes); },
            "Truncated HDR DDS tail was accepted.");
        for (uint32_t format : {71u, 77u, 83u})
            Require(DdsTextureData::Parse(make(format, true)).HasCompleteMipChain(),
                "Legacy DXT1/DXT5/ATI2 header was rejected.");
        auto singleMip = make(98);
        write(singleMip, 28, 1);
        Require(!DdsTextureData::Parse(singleMip).HasCompleteMipChain(),
            "A single full-size mip was reported as a complete chain.");
        for (const auto [offset, value] : std::array<std::pair<size_t, uint32_t>, 9>{{
            {0, 0}, {16, 16385}, {12, 0}, {28, 99}, {112, 0x200},
            {132, 4}, {140, 2}, {128, 84}, {144, 2}}})
        {
            auto malformed = make(98);
            write(malformed, offset, value);
            RequireThrows<std::runtime_error>([&] { (void)DdsTextureData::Parse(malformed); },
                "Malformed or unsupported DDS was accepted.");
        }
        for (size_t size : {size_t(0), size_t(127), size_t(140), make(98).size() - 1})
        {
            auto truncated = make(98);
            truncated.resize(size);
            RequireThrows<std::runtime_error>([&] { (void)DdsTextureData::Parse(truncated); },
                "Truncated DDS header or final block was accepted.");
        }
        const auto temporaryRoot = std::filesystem::temp_directory_path() /
            ("HEngine_DdsReadAhead_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(temporaryRoot);
        struct Cleanup final
        {
            std::filesystem::path Path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(Path, error); }
        } cleanup{temporaryRoot};
        const auto tailPath = temporaryRoot / "tail.dds";
        const auto saveTail = [&](const std::vector<std::byte>& bytes)
        {
            std::ofstream stream(tailPath, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            Require(static_cast<bool>(stream), "DDS tail fixture could not be written.");
        };
        for (uint32_t format : {10u, 71u, 72u, 77u, 78u, 83u, 98u, 99u})
        {
            auto bytes = make(format);
            if (format == 10) bytes.resize(148 + (7 * 5 + 3 * 2 + 1) * 8);
            for (size_t i = 148; i < bytes.size(); ++i) bytes[i] = std::byte(i % 251);
            saveTail(bytes);
            const auto full = DdsTextureData::Parse(bytes);
            for (uint32_t requested : {0u, 1u, 2u, 99u})
            {
                const uint32_t selected = std::min(requested, 2u);
                const auto tail = DdsTextureData::LoadMipTail(tailPath, 0, requested);
                const auto start = full.Mips[selected].Offset;
                Require(tail.SourceWidth == 7 && tail.SourceHeight == 5 &&
                    tail.FirstResidentMip == selected && tail.Mips.size() == 3 - selected &&
                    tail.Mips.front().Offset == 0 && tail.HasCompleteMipChain(),
                    "DDS tail lost source dimensions, rebasing or clamped residency.");
                Require(tail.Format == full.Format && tail.bSRgb == full.bSRgb &&
                    tail.Bytes == std::vector<std::byte>(bytes.begin() + start, bytes.end()) &&
                    tail.FileBytesRead == 148 + tail.Bytes.size(),
                    "DDS tail did not read only the header and exact selected payload.");
                for (size_t i = 0; i < tail.Mips.size(); ++i)
                    Require(tail.Mips[i].Offset == full.Mips[i + selected].Offset - start &&
                        tail.Mips[i].RowPitch == full.Mips[i + selected].RowPitch &&
                        tail.Mips[i].SlicePitch == full.Mips[i + selected].SlicePitch,
                        "DDS tail changed subresource offsets or pitches.");
            }
            const auto limited = DdsTextureData::LoadMipTail(tailPath, 3, 1);
            Require(limited.SourceWidth == 3 && limited.SourceHeight == 2 &&
                limited.FirstResidentMip == 1 && limited.Mips.front().Width == 1 &&
                limited.Bytes.size() == full.Mips.back().SlicePitch,
                "DDS explicit dimension limit must precede the resident mip offset.");
            bytes.pop_back(); saveTail(bytes);
            RequireThrows<std::runtime_error>([&] { (void)DdsTextureData::LoadMipTail(tailPath, 0, 99); },
                "DDS partial IO accepted a truncated chain.");
        }
        for (uint32_t format : {71u, 77u, 83u})
        {
            const auto bytes = make(format, true); saveTail(bytes);
            const auto tail = DdsTextureData::LoadMipTail(tailPath, 0, 1);
            Require(tail.FileBytesRead == 128 + tail.Bytes.size() && tail.Mips.size() == 2,
                "Legacy DDS tail read more than its header and selected payload.");
        }
        saveTail(singleMip);
        RequireThrows<std::runtime_error>([&] { (void)DdsTextureData::LoadMipTail(tailPath, 0, 99); },
            "Streaming accepted a missing offline mip chain.");
        auto truncatedHeader = make(98); truncatedHeader.resize(140); saveTail(truncatedHeader);
        RequireThrows<std::runtime_error>([&] { (void)DdsTextureData::LoadMipTail(tailPath, 0, 1); },
            "DDS partial IO accepted a truncated DX10 header.");
        RequireThrows<std::runtime_error>([&] { (void)DdsTextureData::LoadMipTail(temporaryRoot / "missing.dds", 0, 1); },
            "DDS partial IO accepted a missing source.");
        const auto path = temporaryRoot / "shot.dds";
        const auto source = make(98);
        {
            std::ofstream stream(path, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(source.data()),
                static_cast<std::streamsize>(source.size()));
            Require(static_cast<bool>(stream), "DDS read-ahead fixture could not be written.");
        }
        AssetReadAhead& readAhead = AssetReadAhead::GetInstance();
        readAhead.BeginScene();
        Require(readAhead.Queue(path), "Valid DDS was rejected by bounded read-ahead.");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!readAhead.IsReady(path) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Require(readAhead.IsReady(path), "DDS read-ahead did not complete.");
        const DdsTextureData staged = DdsTextureData::Load(path);
        Require(staged.Bytes == source && staged.Mips.size() == 3,
            "Staged DDS did not retain its exact payload or mip metadata.");
        Require(!readAhead.IsReady(path), "Consumed DDS remained staged.");
        Require(readAhead.Queue(path), "DDS tail could not be queued for read-ahead.");
        const auto tailDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!readAhead.IsReady(path) && std::chrono::steady_clock::now() < tailDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Require(readAhead.IsReady(path), "DDS tail read-ahead did not complete.");
        const auto stagedTail = DdsTextureData::LoadMipTail(path, 0, 1);
        Require(stagedTail.FileBytesRead == 0 && stagedTail.Bytes.size() == 32 &&
            stagedTail.Mips.size() == 2 && stagedTail.Mips.front().Offset == 0 && !readAhead.IsReady(path),
            "DDS tail did not consume staged data without reopening the file.");
        Require(readAhead.Queue(path), "DDS could not be queued for scene cancellation.");
        const auto secondDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!readAhead.IsReady(path) && std::chrono::steady_clock::now() < secondDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Require(readAhead.IsReady(path), "Second DDS read-ahead did not complete.");
        readAhead.EndScene();
        Require(!readAhead.IsReady(path), "Scene stop retained a staged DDS.");
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

    void SsgiReactiveBudgetContract()
    {
        const GlobalIlluminationSettings defaults;
        Require(!defaults.bSvgfSharedUncertainty && !defaults.bSsgiLocalReactiveBudget &&
            defaults.ScreenSpaceDiffuseHitDistanceWeight == 1.0f,
            "First-round candidates changed baseline defaults.");
        RenderWorld noShadowWorld;
        RenderDirectionalLight light;
        light.Intensity = 1500.f;
        light.bCastShadows = false;
        noShadowWorld.DirectionalLights.push_back(light);
        Require(!SsgiHasPotentialShadowTransport(noShadowWorld, true, true),
            "Fixture directional light without projection must permit local scope.");
        Require(SsgiHasPotentialShadowTransport(noShadowWorld, false, false),
            "Unknown provider accepted a standard raster shadow-disable proof.");
        auto shadowWorld = noShadowWorld;
        shadowWorld.DirectionalLights[0].bCastShadows = true;
        Require(SsgiHasPotentialShadowTransport(shadowWorld, true, true),
            "Enabled directional shadows lost global transport scope.");
        Require(!SsgiHasPotentialShadowTransport(shadowWorld, false, true),
            "Standard raster global shadow disable did not suppress projection.");
        shadowWorld.DirectionalLights[0].Intensity = 0.f;
        Require(!SsgiHasPotentialShadowTransport(shadowWorld, true, true), "Zero light casts transport shadows.");
        shadowWorld.DirectionalLights[0].Intensity = 1500.f;
        shadowWorld.DirectionalLights[0].ShadowStrength = 0.f;
        Require(!SsgiHasPotentialShadowTransport(shadowWorld, true, true), "Zero shadow strength casts transport shadows.");
        shadowWorld.DirectionalLights[0].ShadowStrength = std::numeric_limits<float>::quiet_NaN();
        Require(SsgiHasPotentialShadowTransport(shadowWorld, true, true), "Malformed shadow input did not fail closed.");
        auto localLightWorld = noShadowWorld;
        localLightWorld.PointLights.emplace_back();
        Require(SsgiHasPotentialShadowTransport(localLightWorld, true, true), "Point-light shadow atlas lost conservative scope.");
        localLightWorld.PointLights.clear();
        localLightWorld.SpotLights.emplace_back();
        Require(SsgiHasPotentialShadowTransport(localLightWorld, true, true), "Spot-light shadow atlas lost conservative scope.");
        const RenderObject defaultCaster;
        Require(defaultCaster.bCastShadow, "Fixture contract expects the real default caster flag.");
        const glm::mat4 projection(1.f);
        std::vector<SsgiReactiveObject> objects{{ 1, 1, {-.5f, -.1f, .5f}, {-.4f, .1f, .6f}, true, false }};
        objects[0].bGlobalOnChange = defaultCaster.bCastShadow &&
            SsgiHasPotentialShadowTransport(noShadowWorld, true, true);
        SsgiReactiveBudget budget;
        const auto prepare = [&](uint64_t fingerprint = 1, const glm::mat4& camera = glm::mat4(1.f), bool reset = false) {
            return budget.Prepare(objects, 1, fingerprint, camera, 640, 480, .01f, 1, .001f, reset);
        };
        const auto full = [](const SsgiReactiveRegion& region) { return region.Pixels == glm::uvec4(0u, 0u, 640u, 480u); };
        for (uint32_t frame = 0; frame < 16; ++frame)
        {
            const auto region = prepare();
            Require(full(region) && region.RemainingFrames == 16 - frame,
                "Initial reset must cover exactly 16 successful frames.");
            budget.Commit();
        }
        Require(prepare().Pixels == glm::uvec4(0u), "Static trusted scene retained a global reactive window.");
        budget.Abort();
        objects[0].Revision = 2;
        objects[0].Minimum.x = .4f; objects[0].Maximum.x = .5f;
        const auto moved = prepare(2);
        Require(!full(moved) && moved.RemainingFrames == 16 && moved.Pixels.x < 160 && moved.Pixels.z > 480,
            "Local transport mask omitted old/new object positions or expanded to full screen unexpectedly.");
        Require(moved.Pixels.x % 8 == 0 && moved.Pixels.y % 8 == 0 && moved.Pixels.z % 8 == 0 && moved.Pixels.w % 8 == 0,
            "Reactive rectangle is not aligned to the 8x8 trace tile grid.");
        budget.Abort();
        Require(prepare(2).Pixels == moved.Pixels && prepare(2).RemainingFrames == 16,
            "Prepare/abort aged or committed a reactive transport window.");
        budget.Commit();
        glm::mat4 translated(1.f); translated[3].x = .2f;
        const auto cameraMoved = prepare(2, translated);
        Require(cameraMoved.Pixels.x > moved.Pixels.x && cameraMoved.Pixels.z > moved.Pixels.z,
            "Reactive world bounds were not reprojected after camera motion.");
        budget.Abort();
        for (uint32_t frame = 0; frame < 15; ++frame)
        {
            Require(prepare(2).RemainingFrames == 15 - frame, "Reactive response expired before 16 submissions.");
            budget.Commit();
        }
        Require(prepare(2).Pixels == glm::uvec4(0u), "Reactive response did not expire after 16 submissions.");
        budget.Abort();
        objects[0].bGlobalOnChange = true; objects[0].Revision = 3;
        Require(full(prepare(3)), "Changed shadow/emissive influence was incorrectly localized by geometry radius.");
        budget.Abort();
        objects[0].bTrustedBounds = false;
        const auto unknown = prepare(3);
        Require(full(unknown) && (unknown.Reasons & 4u), "Unknown animated/displaced geometry lost full-screen fallback.");
        budget.Commit();
        for (uint32_t frame = 0; frame < 15; ++frame)
        {
            Require(prepare(3).RemainingFrames == 15 - frame, "Static unknown bounds renewed without a change.");
            budget.Commit();
        }
        Require(prepare(3).Pixels == glm::uvec4(0u), "Stable unknown object never returned to configured rays.");
        budget.Abort();
        objects[0].bContinuousDeformation = true;
        Require(full(prepare(3)), "Explicit skinned deformation lost fallback."); budget.Commit();
        Require(prepare(3).RemainingFrames == 16, "Explicit dynamic deformation stopped renewing.");
        budget.Abort();

        SsgiReactiveBudget nearBudget;
        std::vector<SsgiReactiveObject> nearObjects{{1, 1, {-.1f,-.1f,.02f}, {.1f,.1f,.1f}, true, false}};
        for (int frame = 0; frame < 16; ++frame)
        {
            (void)nearBudget.Prepare(nearObjects, 1, 1, projection, 640, 480, .001f, 1, 0.f, false);
            nearBudget.Commit();
        }
        nearObjects[0].Revision = 2; nearObjects[0].Minimum.z = -.1f;
        const auto nearMask = nearBudget.Prepare(nearObjects, 1, 2, projection, 640, 480, .001f, 1, 0.f, false);
        Require(full(nearMask) && (nearMask.Reasons & 16u), "Near-plane crossing used a nonconservative projected mask.");
        nearBudget.Abort();
        nearObjects.clear();
        const auto removed = nearBudget.Prepare(nearObjects, 1, 2, projection, 640, 480, .001f, 1, 0.f, false);
        Require(!full(removed) && removed.Pixels != glm::uvec4(0u) && removed.RemainingFrames == 16,
            "Removed geometry did not retain its old transport influence region.");
        SsgiReactiveBudget overflowBudget;
        std::vector<SsgiReactiveObject> overflow(257, objects[0]);
        for (int index = 0; index < 257; ++index) { overflow[index].EntityId = index; overflow[index].bTrustedBounds = true; }
        const auto overflowMask = overflowBudget.Prepare(overflow, 1, 1, projection, 640, 480, .001f, 1, 0.f, false);
        Require(full(overflowMask) && (overflowMask.Reasons & 8u), "Change capacity overflow discarded unprocessed geometry.");
        Require(full(prepare(3, projection, true)), "Camera cut did not force full-screen reactive recovery.");
    }

    void MainViewAabbContract()
    {
        const glm::mat4 clip(1.f);
        const auto visible = [&](glm::vec3 center, glm::vec3 extent) {
            return IsMainViewAabbVisible(clip, center, extent, 3840, 2160);
        };
        Require(visible({0.f,0.f,.5f}, {.1f,.1f,.1f}), "Interior AABB rejected.");
        Require(!visible({2.f,0.f,.5f}, {.01f,100.f,.1f}),
            "Elongated offscreen AABB retained sphere false-positive.");
        Require(visible({2.f,0.f,.5f}, {1.01f,.1f,.1f}), "Intersecting AABB rejected.");
        Require(visible({0.f,0.f,0.f}, {.1f,.1f,.1f}), "Near-plane crossing rejected.");
        Require(visible({0.f,0.f,1.f}, {.1f,.1f,.1f}), "Far-plane crossing rejected.");
        Require(!visible({0.f,0.f,-1.f}, {.1f,.1f,.1f}), "Wholly behind near plane retained.");
        Require(!visible({0.f,0.f,2.f}, {.1f,.1f,.1f}), "Wholly beyond far plane retained.");
        Require(visible({1.f + 1.f / 3840.f,0.f,.5f}, {0.f,0.f,0.f}), "Native-pixel guard lost.");
        Require(visible({0.f,0.f,.5f}, {-1.f,1.f,1.f}), "Unknown negative extent was culled.");
        glm::mat4 invalid(1.f); invalid[0][0] = std::numeric_limits<float>::infinity();
        Require(IsMainViewAabbVisible(invalid, {100.f,0.f,.5f}, {1.f,1.f,1.f}, 3840,2160),
            "Nonfinite projection failed open.");
        Require(IsMainViewAabbVisible(clip, {100.f,0.f,.5f}, {1.f,1.f,1.f}, 0,0),
            "Uninitialized viewport failed open.");
        // Shear/reflection world AABB support must contain all transformed corners.
        glm::mat3 linear(1.f); linear[0][0] = -2.f; linear[1][0] = 3.f;
        const glm::vec3 localExtent(.2f,.3f,.1f);
        const glm::vec3 worldExtent = glm::abs(linear[0]) * localExtent.x +
            glm::abs(linear[1]) * localExtent.y + glm::abs(linear[2]) * localExtent.z;
        for (unsigned mask = 0; mask < 8; ++mask)
        {
            const glm::vec3 corner((mask&1) ? localExtent.x : -localExtent.x,
                (mask&2) ? localExtent.y : -localExtent.y, (mask&4) ? localExtent.z : -localExtent.z);
            Require(glm::all(glm::lessThanEqual(glm::abs(linear*corner), worldExtent + glm::vec3(1e-6f))),
                "World AABB failed shear/reflection containment.");
        }
        // A visible vertex anywhere in a candidate box is a witness that the
        // candidate must survive; include corner/edge and perspective cases.
        glm::mat4 perspective(0.f); perspective[0][0]=1.f; perspective[1][1]=1.f;
        perspective[2][2]=-1.1f; perspective[2][3]=-1.f; perspective[3][2]=-.11f;
        for (int i=0;i<1000;++i)
        {
            const glm::vec3 center(float((i*17)%101-50)/10.f, float((i*31)%83-41)/10.f,
                -float((i*7)%100+1)/10.f);
            const glm::vec3 extent(.1f+float(i%7)/10.f,.1f+float(i%11)/10.f,.05f);
            bool witness=false;
            for (unsigned mask=0;mask<8;++mask)
            {
                const glm::vec3 corner=center+extent*glm::vec3((mask&1)?1.f:-1.f,
                    (mask&2)?1.f:-1.f,(mask&4)?1.f:-1.f);
                const glm::vec4 q=perspective*glm::vec4(corner,1.f);
                witness=witness || (q.w>0.f && std::abs(q.x)<=q.w && std::abs(q.y)<=q.w && q.z>=0.f && q.z<=q.w);
            }
            Require(!witness || IsMainViewAabbVisible(perspective,center,extent,3840,2160),
                "AABB rejected visible perspective corner witness.");
        }
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
                floatRaw.MetadataJson.find("\"colorSpace\": \"unspecified\"") != std::string::npos &&
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

        const std::array<float, 6> viewDepthRows{ 1.0f, -2.0f, 99.0f, 3.0f, -4.0f, 99.0f };
        const auto* viewDepthBytes = reinterpret_cast<const uint8_t*>(viewDepthRows.data());
        const TextureReadbackRawEncoding depthRaw = TextureReadbackQueue::EncodeRaw(
            1, 2, nvrhi::Format::RG32_FLOAT, viewDepthBytes, 3 * sizeof(float));
        Require(depthRaw.Pixels.size() == 4 * sizeof(float) &&
                std::equal(depthRaw.Pixels.begin(), depthRaw.Pixels.begin() + 8, viewDepthBytes) &&
                std::equal(depthRaw.Pixels.begin() + 8, depthRaw.Pixels.end(), viewDepthBytes + 12) &&
                depthRaw.MetadataJson.find("\"channels\": 2") != std::string::npos &&
                depthRaw.MetadataJson.find("\"colorSpace\": \"unspecified\"") != std::string::npos,
            "Native current/previous view-depth capture changed signed values or retained row padding.");
        RequireThrows<std::invalid_argument>([viewDepthBytes]() {
            (void)TextureReadbackQueue::EncodePfm(
                1, 2, nvrhi::Format::RG32_FLOAT, viewDepthBytes, 3 * sizeof(float));
        }, "Two-channel PFM must not read a nonexistent third component.");

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

        // Native GBuffer exports preserve bit patterns and strip only row
        // padding. Odd width catches accidental component/pixel stride mixing.
        std::array<uint8_t, 32> nativeDepthRows{};
        nativeDepthRows.fill(0xe7u);
        const std::array<uint32_t, 6> nativeDepthBits{
            0u, std::bit_cast<uint32_t>(1.f), 0x7fc12345u,
            0x80000000u, std::bit_cast<uint32_t>(0.125f), 0x7f800000u,
        };
        for (size_t index = 0; index < nativeDepthBits.size(); ++index)
            StoreLittleEndian32(nativeDepthRows.data() + (index / 3) * 16 + (index % 3) * 4,
                nativeDepthBits[index]);
        const auto nativeDepth = TextureReadbackQueue::EncodeRaw(3, 2, nvrhi::Format::D32,
            nativeDepthRows.data(), 16, TextureReadbackNonFinitePolicy::Preserve);
        Require(nativeDepth.Pixels.size() == 24 &&
            std::memcmp(nativeDepth.Pixels.data(), nativeDepthRows.data(), 12) == 0 &&
            std::memcmp(nativeDepth.Pixels.data() + 12, nativeDepthRows.data() + 16, 12) == 0,
            "Native D32 raw changed float bits, row order, or retained padding.");
        Require(nativeDepth.MetadataJson.find("\"source_format\": \"D32\"") != std::string::npos &&
            nativeDepth.MetadataJson.find("\"width\": 3") != std::string::npos &&
            nativeDepth.MetadataJson.find("\"height\": 2") != std::string::npos &&
            nativeDepth.MetadataJson.find("\"nan_count\": 1") != std::string::npos &&
            nativeDepth.MetadataJson.find("\"positive_infinity_count\": 1") != std::string::npos,
            "Native D32 raw metadata lost the source format, size, or nonfinite counts.");
        RequireThrows<std::domain_error>([&nativeDepthRows]() {
            (void)TextureReadbackQueue::EncodeRaw(3, 2, nvrhi::Format::D32, nativeDepthRows.data(), 16);
        }, "Native D32 raw did not honor default nonfinite rejection.");

        const std::array<uint8_t, 32> packedNormalRows{
            0, 1, 127, 255, 255, 128, 64, 32, 17, 31, 63, 129, 0xee, 0xee, 0xee, 0xee,
            5, 250, 0, 255, 1, 2, 3, 4, 127, 128, 254, 255, 0xee, 0xee, 0xee, 0xee,
        };
        const auto packedNormal = TextureReadbackQueue::EncodeRaw(3, 2, nvrhi::Format::RGBA8_UNORM,
            packedNormalRows.data(), 16);
        Require(packedNormal.Pixels.size() == 24 &&
            std::memcmp(packedNormal.Pixels.data(), packedNormalRows.data(), 12) == 0 &&
            std::memcmp(packedNormal.Pixels.data() + 12, packedNormalRows.data() + 16, 12) == 0,
            "Packed normal raw normalized, swizzled, expanded, or lost uint8 components.");
        Require(packedNormal.MetadataJson.find("\"source_format\": \"RGBA8_UNORM\"") != std::string::npos &&
            packedNormal.MetadataJson.find("\"channels\": 4") != std::string::npos &&
            packedNormal.MetadataJson.find("\"type\": \"uint8\"") != std::string::npos &&
            packedNormal.MetadataJson.find("\"colorSpace\": \"data\"") != std::string::npos &&
            packedNormal.MetadataJson.find("\"nan_count\": 0") != std::string::npos,
            "Packed normal raw metadata mislabeled native unsigned bytes.");
        RequireThrows<std::invalid_argument>([&packedNormalRows]() {
            (void)TextureReadbackQueue::EncodeRaw(3, 2, nvrhi::Format::RGBA8_UNORM,
                packedNormalRows.data(), 11);
        }, "Packed normal raw accepted a row pitch shorter than width times four.");
        RequireThrows<std::invalid_argument>([&packedNormalRows]() {
            (void)TextureReadbackQueue::EncodePfm(3, 2, nvrhi::Format::RGBA8_UNORM,
                packedNormalRows.data(), 16, TextureReadbackAlphaMode::Discard);
        }, "Packed normal integer bytes were silently converted to PFM.");

        const std::array<uint8_t, 8> reactiveRows{0, 127, 255, 0xee, 1, 128, 254, 0xee};
        const auto reactive = TextureReadbackQueue::EncodeRaw(3, 2, nvrhi::Format::R8_UNORM,
            reactiveRows.data(), 4);
        Require(reactive.Pixels == std::vector<uint8_t>({0, 127, 255, 1, 128, 254}) &&
            reactive.MetadataJson.find("\"source_format\": \"R8_UNORM\"") != std::string::npos &&
            reactive.MetadataJson.find("\"channels\": 1") != std::string::npos &&
            reactive.MetadataJson.find("\"type\": \"uint8\"") != std::string::npos,
            "Reactive mask capture changed UNORM bits, channel count, or retained row padding.");
        RequireThrows<std::invalid_argument>([&reactiveRows]() {
            (void)TextureReadbackQueue::EncodeRaw(3, 2, nvrhi::Format::R8_UNORM,
                reactiveRows.data(), 2);
        }, "Reactive mask raw accepted a row pitch shorter than its width.");

        std::array<uint8_t, 64> surfaceIdRows{};
        surfaceIdRows.fill(0xeeu);
        const std::array<uint32_t, 12> surfaceIdWords{
            0xffffffffu, 0xfedcba98u, 0u, 1u, 0x80000000u, 0x7fffffffu,
            0x01020304u, 0x04030201u, 0x7fc12345u, 0x7f800000u, 0x12345678u, 0x9abcdef0u,
        };
        for (size_t index = 0; index < surfaceIdWords.size(); ++index)
            StoreLittleEndian32(surfaceIdRows.data() + (index / 6) * 32 + (index % 6) * 4,
                surfaceIdWords[index]);
        const auto surfaceIds = TextureReadbackQueue::EncodeRaw(3, 2, nvrhi::Format::RG32_UINT,
            surfaceIdRows.data(), 32);
        Require(surfaceIds.Pixels.size() == 48 &&
            std::memcmp(surfaceIds.Pixels.data(), surfaceIdRows.data(), 24) == 0 &&
            std::memcmp(surfaceIds.Pixels.data() + 24, surfaceIdRows.data() + 32, 24) == 0,
            "Surface-ID raw lost a channel, truncated high bits, or retained row padding.");
        Require(surfaceIds.MetadataJson.find("\"source_format\": \"RG32_UINT\"") != std::string::npos &&
            surfaceIds.MetadataJson.find("\"channels\": 2") != std::string::npos &&
            surfaceIds.MetadataJson.find("\"type\": \"uint32\"") != std::string::npos &&
            surfaceIds.MetadataJson.find("\"nan_count\": 0") != std::string::npos &&
            surfaceIds.MetadataJson.find("\"positive_infinity_count\": 0") != std::string::npos,
            "Surface-ID raw interpreted integer identities as floating point.");
        RequireThrows<std::invalid_argument>([&surfaceIdRows]() {
            (void)TextureReadbackQueue::EncodeRaw(3, 2, nvrhi::Format::RG32_UINT, surfaceIdRows.data(), 23);
        }, "Surface-ID raw accepted a row pitch shorter than two uint32 channels per pixel.");

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
