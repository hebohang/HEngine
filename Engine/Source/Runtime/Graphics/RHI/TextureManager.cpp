#include "Hepch.h"

#include "TextureManager.h"
#include "DdsTextureData.h"
#include "TexturePreparationQueueState.h"
#include "TextureStreamingPolicy.h"
#include "Runtime/Graphics/RHI/TextureAlphaDilation.h"

#include "Runtime/Graphics/Image.h"
#include "Runtime/Graphics/ResourceFormat.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <stb_image_resize2.h>

namespace HEngine
{
    namespace
    {
        constexpr uint32_t c_TextureChannelCount = 4;
        constexpr float c_MaxFiniteHalf = 65504.0f;

        struct UploadValidation
        {
#if defined(HE_DEV_AGENT_RUNTIME)
            TextureUploadFailure Failure = TextureUploadFailure::None;
#endif
        };

        [[nodiscard]] nvrhi::CommandListHandle AcquireUploadCommands(
            const nvrhi::DeviceHandle& device, nvrhi::CommandListHandle* reusable,
            [[maybe_unused]] UploadValidation validation)
        {
#if defined(HE_DEV_AGENT_RUNTIME)
            if (validation.Failure == TextureUploadFailure::CommandListCreation) return nullptr;
#endif
            if (!reusable) return device->createCommandList();
            if (!*reusable) *reusable = device->createCommandList();
            return *reusable;
        }

        void SubmitTextureUpload(const nvrhi::DeviceHandle& device,
            const nvrhi::CommandListHandle& commands, nvrhi::EventQueryHandle& completion,
            [[maybe_unused]] UploadValidation validation)
        {
            // Allocate tracking before submission: query allocation failure must
            // not leave an already submitted upload outside manager accounting.
            completion =
#if defined(HE_DEV_AGENT_RUNTIME)
                validation.Failure == TextureUploadFailure::CompletionQueryCreation ? nullptr :
#endif
                device->createEventQuery();
            if (!completion)
                throw std::runtime_error("NVRHI failed to create texture upload completion query");
            device->resetEventQuery(completion);
#if defined(HE_DEV_AGENT_RUNTIME)
            if (validation.Failure == TextureUploadFailure::BeforeSubmission)
                throw std::runtime_error("Texture upload validation: rejected before submission");
#endif
            if (device->executeCommandList(commands, nvrhi::CommandQueue::Graphics) == 0)
                throw std::runtime_error("NVRHI returned an invalid texture upload submission");
            device->setEventQuery(completion, nvrhi::CommandQueue::Graphics);
        }

        struct TextureMipUpload
        {
            uint32_t Width = 0;
            uint32_t Height = 0;
            std::vector<std::byte> Pixels;
        };

        [[nodiscard]] size_t DefaultTextureArrayIndex(DefaultBindlessTexture textureKind)
        {
            const size_t index = static_cast<size_t>(textureKind);
            if (index >= 3)
                throw std::out_of_range("Unknown default bindless texture kind");
            return index;
        }

        [[nodiscard]] bool ShouldGenerateMipmaps(const TextureLoadDesc& desc) noexcept
        {
			// Descriptor paging intentionally loads sampled material textures
			// without a global bindless descriptor. Mip policy is therefore
			// independent from descriptor ownership.
            return desc.bGenerateMipmaps;
        }

        [[nodiscard]] bool ShouldPreserveAlphaCoverage(
            const TextureLoadDesc& desc) noexcept
        {
            return ShouldGenerateMipmaps(desc) && desc.bPreserveAlphaCoverage;
        }

		struct ResizedTextureDimensions
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
		};

		[[nodiscard]] ResizedTextureDimensions ResolveTextureDimensions(
			uint32_t width,
			uint32_t height,
			uint32_t maximumDimension) noexcept
		{
			if (maximumDimension == 0 ||
				std::max(width, height) <= maximumDimension)
			{
				return { width, height };
			}

			if (width >= height)
			{
				return {
					maximumDimension,
					std::max(1u, static_cast<uint32_t>(std::lround(
						static_cast<double>(height) * maximumDimension / width)))
				};
			}

			return {
				std::max(1u, static_cast<uint32_t>(std::lround(
					static_cast<double>(width) * maximumDimension / height))),
				maximumDimension
			};
		}

        [[nodiscard]] uint32_t CalculateMipLevelCount(uint32_t width, uint32_t height) noexcept
        {
            uint32_t mipLevels = 1;
            while (width > 1 || height > 1)
            {
                width = std::max(width / 2u, 1u);
                height = std::max(height / 2u, 1u);
                ++mipLevels;
            }
            return mipLevels;
        }

        [[nodiscard]] float SrgbToLinear(float value)
        {
            value = std::clamp(value, 0.0f, 1.0f);
            return value <= 0.04045f
                ? value / 12.92f
                : std::pow((value + 0.055f) / 1.055f, 2.4f);
        }

        [[nodiscard]] float LinearToSrgb(float value)
        {
            value = std::clamp(value, 0.0f, 1.0f);
            return value <= 0.0031308f
                ? value * 12.92f
                : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
        }

        [[nodiscard]] uint8_t QuantizeUnorm8(float value)
        {
            return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }

        [[nodiscard]] std::vector<float> DecodeRgba8(
            std::span<const std::byte> imageData,
            uint32_t width,
            uint32_t height,
            TextureColorSpace colorSpace)
        {
            if (width == 0 || height == 0 ||
                static_cast<size_t>(width) >
                    std::numeric_limits<size_t>::max() / height / c_TextureChannelCount)
                throw std::runtime_error("Decoded RGBA8 texture dimensions are invalid");
            const size_t pixelCount = static_cast<size_t>(width) * height;
            const size_t expectedSize = pixelCount * c_TextureChannelCount;
            if (imageData.size() != expectedSize)
                throw std::runtime_error("Decoded RGBA8 texture byte count is invalid");

            const uint8_t* source = reinterpret_cast<const uint8_t*>(imageData.data());
            std::vector<float> decoded(expectedSize);
            const bool bSrgb = colorSpace == TextureColorSpace::SRgb;
            static const std::array<float, 256> sSrgbToLinear = []
            {
                std::array<float, 256> values{};
                for (size_t index = 0; index < values.size(); ++index)
                    values[index] = SrgbToLinear(static_cast<float>(index) / 255.0f);
                return values;
            }();
            for (size_t pixel = 0; pixel < pixelCount; ++pixel)
            {
                const size_t offset = pixel * c_TextureChannelCount;
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    const float value = static_cast<float>(source[offset + channel]) / 255.0f;
                    decoded[offset + channel] = bSrgb
                        ? sSrgbToLinear[source[offset + channel]]
                        : value;
                }
                // Alpha is always linear, including in SRGBA resources.
                decoded[offset + 3] = static_cast<float>(source[offset + 3]) / 255.0f;
            }
            return decoded;
        }

        [[nodiscard]] std::vector<float> DecodeRgba32Float(
            std::span<const std::byte> imageData,
            uint32_t width,
            uint32_t height)
        {
            if (width == 0 || height == 0 ||
                static_cast<size_t>(width) >
                    std::numeric_limits<size_t>::max() / height /
                        (c_TextureChannelCount * sizeof(float)))
                throw std::runtime_error("Decoded RGBA32F texture dimensions are invalid");
            const size_t valueCount = static_cast<size_t>(width) * height * c_TextureChannelCount;
            const size_t expectedSize = valueCount * sizeof(float);
            if (imageData.size() != expectedSize)
                throw std::runtime_error("Decoded RGBA32F texture byte count is invalid");

            std::vector<float> decoded(valueCount);
            std::memcpy(decoded.data(), imageData.data(), expectedSize);
            for (float& value : decoded)
            {
                value = std::isfinite(value)
                    ? std::clamp(value, -c_MaxFiniteHalf, c_MaxFiniteHalf)
                    : 0.0f;
            }
            return decoded;
        }

        [[nodiscard]] std::vector<float> DownsampleRgbaArea(
            const std::vector<float>& source,
            uint32_t sourceWidth,
            uint32_t sourceHeight,
            uint32_t destinationWidth,
            uint32_t destinationHeight)
        {
            const size_t expectedSourceValues =
                static_cast<size_t>(sourceWidth) * sourceHeight * c_TextureChannelCount;
            if (source.size() != expectedSourceValues || destinationWidth == 0 || destinationHeight == 0)
                throw std::invalid_argument("Invalid texture mip downsample dimensions");

            std::vector<float> destination(
                static_cast<size_t>(destinationWidth) * destinationHeight * c_TextureChannelCount,
                0.0f);
            const double scaleX = static_cast<double>(sourceWidth) / destinationWidth;
            const double scaleY = static_cast<double>(sourceHeight) / destinationHeight;

            for (uint32_t destinationY = 0; destinationY < destinationHeight; ++destinationY)
            {
                const double sourceY0 = destinationY * scaleY;
                const double sourceY1 = (destinationY + 1u) * scaleY;
                const uint32_t firstSourceY = static_cast<uint32_t>(std::floor(sourceY0));
                const uint32_t lastSourceY = std::min(
                    static_cast<uint32_t>(std::ceil(sourceY1)),
                    sourceHeight);

                for (uint32_t destinationX = 0; destinationX < destinationWidth; ++destinationX)
                {
                    const double sourceX0 = destinationX * scaleX;
                    const double sourceX1 = (destinationX + 1u) * scaleX;
                    const uint32_t firstSourceX = static_cast<uint32_t>(std::floor(sourceX0));
                    const uint32_t lastSourceX = std::min(
                        static_cast<uint32_t>(std::ceil(sourceX1)),
                        sourceWidth);
                    double accumulated[c_TextureChannelCount]{};
                    double totalWeight = 0.0;

                    for (uint32_t sourceY = firstSourceY; sourceY < lastSourceY; ++sourceY)
                    {
                        const double weightY = std::max(
                            0.0,
                            std::min(sourceY1, static_cast<double>(sourceY + 1u)) -
                                std::max(sourceY0, static_cast<double>(sourceY)));
                        for (uint32_t sourceX = firstSourceX; sourceX < lastSourceX; ++sourceX)
                        {
                            const double weightX = std::max(
                                0.0,
                                std::min(sourceX1, static_cast<double>(sourceX + 1u)) -
                                    std::max(sourceX0, static_cast<double>(sourceX)));
                            const double weight = weightX * weightY;
                            const size_t sourceOffset =
                                (static_cast<size_t>(sourceY) * sourceWidth + sourceX) *
                                c_TextureChannelCount;
                            for (size_t channel = 0; channel < c_TextureChannelCount; ++channel)
                                accumulated[channel] += source[sourceOffset + channel] * weight;
                            totalWeight += weight;
                        }
                    }

                    if (totalWeight <= 0.0)
                        throw std::runtime_error("Texture mip downsample produced zero coverage");
                    const size_t destinationOffset =
                        (static_cast<size_t>(destinationY) * destinationWidth + destinationX) *
                        c_TextureChannelCount;
                    for (size_t channel = 0; channel < c_TextureChannelCount; ++channel)
                    {
                        destination[destinationOffset + channel] =
                            static_cast<float>(accumulated[channel] / totalWeight);
                    }
                }
            }
            return destination;
        }

        [[nodiscard]] std::vector<std::byte> EncodeRgba8(
            const std::vector<float>& linearPixels,
            TextureColorSpace colorSpace)
        {
            if (linearPixels.size() % c_TextureChannelCount != 0)
                throw std::invalid_argument("Invalid RGBA texture mip value count");

            std::vector<std::byte> encoded(linearPixels.size());
            const bool bSrgb = colorSpace == TextureColorSpace::SRgb;
            for (size_t offset = 0; offset < linearPixels.size(); offset += c_TextureChannelCount)
            {
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    const float value = bSrgb
                        ? LinearToSrgb(linearPixels[offset + channel])
                        : linearPixels[offset + channel];
                    encoded[offset + channel] =
                        static_cast<std::byte>(QuantizeUnorm8(value));
                }
                encoded[offset + 3] =
                    static_cast<std::byte>(QuantizeUnorm8(linearPixels[offset + 3]));
            }
            return encoded;
        }

        [[nodiscard]] std::vector<std::byte> EncodeRgba32Float(
            const std::vector<float>& linearPixels)
        {
            std::vector<std::byte> encoded(linearPixels.size() * sizeof(float));
            std::memcpy(encoded.data(), linearPixels.data(), encoded.size());
            return encoded;
        }

        [[nodiscard]] float ScaleAlpha(float alpha, double scale)
        {
            return std::clamp(
                static_cast<float>(static_cast<double>(alpha) * scale),
                0.0f,
                1.0f);
        }

        [[nodiscard]] double ComputeAlphaCoverage(
            const std::vector<float>& pixels,
            float cutoff,
            double scale,
            bool bQuantizeToUnorm8)
        {
            if (pixels.empty() || pixels.size() % c_TextureChannelCount != 0)
                throw std::invalid_argument("Invalid pixels for alpha coverage calculation");

            size_t coveredPixelCount = 0;
            for (size_t offset = 3; offset < pixels.size(); offset += c_TextureChannelCount)
            {
                float alpha = ScaleAlpha(pixels[offset], scale);
                if (bQuantizeToUnorm8)
                    alpha = static_cast<float>(QuantizeUnorm8(alpha)) / 255.0f;
                if (alpha >= cutoff)
                    ++coveredPixelCount;
            }
            return static_cast<double>(coveredPixelCount) /
                static_cast<double>(pixels.size() / c_TextureChannelCount);
        }

        [[nodiscard]] double FindAlphaCoverageScale(
            const std::vector<float>& pixels,
            float cutoff,
            double targetCoverage,
            bool bQuantizeToUnorm8)
        {
            if (targetCoverage <= 0.0 || cutoff <= 0.0f)
                return 1.0;

            const double unitCoverage = ComputeAlphaCoverage(
                pixels,
                cutoff,
                1.0,
                bQuantizeToUnorm8);
            if (unitCoverage == targetCoverage)
                return 1.0;

            // Coverage is monotonic but discrete. Bracket the first scale whose
            // coverage reaches the mip-0 target from either direction, then
            // choose the nearer staircase step. Ties remain conservative.
            double lowScale = unitCoverage > targetCoverage ? 0.0 : 1.0;
            double highScale = 1.0;
            double lowCoverage = unitCoverage > targetCoverage
                ? ComputeAlphaCoverage(
                    pixels,
                    cutoff,
                    lowScale,
                    bQuantizeToUnorm8)
                : unitCoverage;
            double highCoverage = unitCoverage;
            for (uint32_t expansion = 0;
                expansion < 64u && highCoverage < targetCoverage;
                ++expansion)
            {
                highScale *= 2.0;
                highCoverage = ComputeAlphaCoverage(
                    pixels,
                    cutoff,
                    highScale,
                    bQuantizeToUnorm8);
            }

            if (highCoverage < targetCoverage)
                return highScale;

            for (uint32_t iteration = 0; iteration < 32u; ++iteration)
            {
                const double middleScale = (lowScale + highScale) * 0.5;
                const double middleCoverage = ComputeAlphaCoverage(
                    pixels,
                    cutoff,
                    middleScale,
                    bQuantizeToUnorm8);
                if (middleCoverage >= targetCoverage)
                {
                    highScale = middleScale;
                    highCoverage = middleCoverage;
                }
                else
                {
                    lowScale = middleScale;
                    lowCoverage = middleCoverage;
                }
            }

            const double lowError = std::abs(lowCoverage - targetCoverage);
            const double highError = std::abs(highCoverage - targetCoverage);
            constexpr double coverageTieTolerance = 1e-12;
            return lowError + coverageTieTolerance < highError
                ? lowScale
                : highScale;
        }

        void PreserveAlphaCoverage(
            std::vector<float>& pixels,
            float cutoff,
            double targetCoverage,
            bool bQuantizeToUnorm8)
        {
            if (targetCoverage <= 0.0)
                return;

            const double scale = FindAlphaCoverageScale(
                pixels,
                cutoff,
                targetCoverage,
                bQuantizeToUnorm8);
            for (size_t offset = 3; offset < pixels.size(); offset += c_TextureChannelCount)
                pixels[offset] = ScaleAlpha(pixels[offset], scale);
        }

        [[nodiscard]] std::vector<TextureMipUpload> GenerateMipChain(
            std::span<const std::byte> imageData,
            uint32_t width,
            uint32_t height,
            EResourceFormat imageFormat,
            TextureColorSpace colorSpace,
            bool bPreserveAlphaCoverage,
            float alphaCoverageCutoff)
        {
            const bool bFloat = imageFormat == EResourceFormat::R32G32B32A32_FLOAT;
            std::vector<float> sourcePixels = bFloat
                ? DecodeRgba32Float(imageData, width, height)
                : DecodeRgba8(imageData, width, height, colorSpace);
            const double targetAlphaCoverage = bPreserveAlphaCoverage
                ? ComputeAlphaCoverage(
                    sourcePixels,
                    alphaCoverageCutoff,
                    1.0,
                    !bFloat)
                : 0.0;
            std::vector<TextureMipUpload> mipChain;
            mipChain.reserve(CalculateMipLevelCount(width, height) - 1u);

            while (width > 1 || height > 1)
            {
                const uint32_t mipWidth = std::max(width / 2u, 1u);
                const uint32_t mipHeight = std::max(height / 2u, 1u);
                std::vector<float> mipPixels = DownsampleRgbaArea(
                    sourcePixels,
                    width,
                    height,
                    mipWidth,
                    mipHeight);
                std::vector<float> encodedPixels = mipPixels;
                if (bPreserveAlphaCoverage)
                {
                    PreserveAlphaCoverage(
                        encodedPixels,
                        alphaCoverageCutoff,
                        targetAlphaCoverage,
                        !bFloat);
                }
                mipChain.push_back(TextureMipUpload{
                    .Width = mipWidth,
                    .Height = mipHeight,
                    .Pixels = bFloat
                        ? EncodeRgba32Float(encodedPixels)
                        : EncodeRgba8(encodedPixels, colorSpace),
                });
                sourcePixels = std::move(mipPixels);
                width = mipWidth;
                height = mipHeight;
            }
            return mipChain;
        }

        // CPU-only preparation. Every byte needed by writeTexture is copied into
        // this result before Image and its stb allocation leave the worker.
        struct PreparedImageUpload
        {
            nvrhi::TextureDesc Description;
            uint32_t SourceWidth = 0, SourceHeight = 0, FirstMip = 0;
            std::vector<std::byte> BasePixels;
            std::vector<TextureMipUpload> Mips;
            std::shared_ptr<const EnvironmentSamplingProposal> Proposal;
        };

        [[nodiscard]] PreparedImageUpload PrepareImageUpload(
            const TextureLoadDesc& desc, const std::filesystem::path& resolvedPath)
        {
            Image image(resolvedPath, desc.bFlipVertically);
            const std::span<const std::byte> imageData = image.GetData();
            if (imageData.empty() || image.GetWidth() == 0 || image.GetHeight() == 0)
                throw std::runtime_error("Failed to decode texture asset: " +
                    Utils::PathToUtf8(resolvedPath));

            const float alphaCoverageCutoff = desc.bPreserveAlphaCoverage
                ? std::clamp(desc.AlphaCoverageCutoff, 0.0f, 1.0f) : 0.5f;
            std::vector<float> sanitizedFloatPixels;
            std::vector<float> resizedFloatPixels;
            std::vector<std::byte> resizedBytePixels;
            std::vector<std::byte> alphaDilatedPixels;
            std::span<const std::byte> uploadImageData = imageData;
            const ResizedTextureDimensions uploadDimensions = ResolveTextureDimensions(
                image.GetWidth(), image.GetHeight(), desc.MaximumDimension);
            const bool bResizeImage = uploadDimensions.Width != image.GetWidth() ||
                uploadDimensions.Height != image.GetHeight();
            if (image.GetFormat() == EResourceFormat::R32G32B32A32_FLOAT)
            {
                sanitizedFloatPixels = DecodeRgba32Float(
                    imageData, image.GetWidth(), image.GetHeight());
                uploadImageData = std::as_bytes(std::span<const float>(
                    sanitizedFloatPixels.data(), sanitizedFloatPixels.size()));
                if (bResizeImage)
                {
                    resizedFloatPixels.resize(static_cast<size_t>(uploadDimensions.Width) *
                        uploadDimensions.Height * c_TextureChannelCount);
                    if (!stbir_resize_float_linear(
                        sanitizedFloatPixels.data(), static_cast<int>(image.GetWidth()),
                        static_cast<int>(image.GetHeight()), 0, resizedFloatPixels.data(),
                        static_cast<int>(uploadDimensions.Width),
                        static_cast<int>(uploadDimensions.Height), 0, STBIR_RGBA))
                        throw std::runtime_error("Failed to resize texture asset: " + desc.AssetPath);
                    uploadImageData = std::as_bytes(std::span<const float>(
                        resizedFloatPixels.data(), resizedFloatPixels.size()));
                }
            }
            else
            {
                if (bResizeImage)
                {
                    resizedBytePixels.resize(static_cast<size_t>(uploadDimensions.Width) *
                        uploadDimensions.Height * c_TextureChannelCount);
                    const auto* source = reinterpret_cast<const uint8_t*>(imageData.data());
                    auto* destination = reinterpret_cast<uint8_t*>(resizedBytePixels.data());
                    const bool bResized = desc.ColorSpace == TextureColorSpace::SRgb
                        ? stbir_resize_uint8_srgb(source, static_cast<int>(image.GetWidth()),
                            static_cast<int>(image.GetHeight()), 0, destination,
                            static_cast<int>(uploadDimensions.Width),
                            static_cast<int>(uploadDimensions.Height), 0, STBIR_RGBA) != nullptr
                        : stbir_resize_uint8_linear(source, static_cast<int>(image.GetWidth()),
                            static_cast<int>(image.GetHeight()), 0, destination,
                            static_cast<int>(uploadDimensions.Width),
                            static_cast<int>(uploadDimensions.Height), 0, STBIR_RGBA) != nullptr;
                    if (!bResized)
                        throw std::runtime_error("Failed to resize texture asset: " + desc.AssetPath);
                    uploadImageData = resizedBytePixels;
                }
                if (ShouldPreserveAlphaCoverage(desc))
                {
                    TextureAlphaDilationResult dilation = DilateMaskedTextureRgb(
                        uploadImageData, uploadDimensions.Width, uploadDimensions.Height,
                        alphaCoverageCutoff);
                    if (dilation.WasApplied())
                    {
                        alphaDilatedPixels = std::move(dilation.Pixels);
                        uploadImageData = alphaDilatedPixels;
                    }
                }
            }

            PreparedImageUpload prepared;
            nvrhi::TextureDesc& textureDesc = prepared.Description;
            textureDesc.width = uploadDimensions.Width;
            textureDesc.height = uploadDimensions.Height;
            textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
            textureDesc.isShaderResource = true;
            textureDesc.initialState = desc.bAllowReadback
                ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common;
            textureDesc.keepInitialState = desc.bAllowReadback;
            textureDesc.mipLevels = ShouldGenerateMipmaps(desc)
                ? CalculateMipLevelCount(textureDesc.width, textureDesc.height) : 1u;
            textureDesc.format = image.GetFormat() == EResourceFormat::R32G32B32A32_FLOAT
                ? nvrhi::Format::RGBA32_FLOAT
                : (desc.ColorSpace == TextureColorSpace::SRgb
                    ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM);
            textureDesc.debugName = desc.DebugName.empty()
                ? "TextureManager " + desc.AssetPath : desc.DebugName;

            if (desc.bBuildEnvironmentProposal && !desc.bFlipVertically &&
                desc.ColorSpace == TextureColorSpace::Linear && !sanitizedFloatPixels.empty())
            {
                try
                {
                    prepared.Proposal = EnvironmentSamplingProposal::Build(
                        sanitizedFloatPixels, image.GetWidth(), image.GetHeight());
                }
                catch (const std::exception&)
                {
                    // The proposal is optional; the decoded HDR remains usable.
                    prepared.Proposal.reset();
                }
            }
            if (textureDesc.mipLevels > 1)
            {
                prepared.Mips = GenerateMipChain(
                    uploadImageData, textureDesc.width, textureDesc.height,
                    image.GetFormat(), desc.ColorSpace,
                    ShouldPreserveAlphaCoverage(desc), alphaCoverageCutoff);
                if (prepared.Mips.size() + 1u != textureDesc.mipLevels)
                    throw std::logic_error("Generated texture mip count does not match its resource");
            }
            prepared.SourceWidth = textureDesc.width;
            prepared.SourceHeight = textureDesc.height;
            prepared.FirstMip = std::min(desc.FirstResidentMip, textureDesc.mipLevels - 1);
            if (prepared.FirstMip > 0)
            {
                auto& base = prepared.Mips[prepared.FirstMip - 1];
                textureDesc.width = base.Width; textureDesc.height = base.Height;
                textureDesc.mipLevels -= prepared.FirstMip;
                prepared.BasePixels = std::move(base.Pixels);
                prepared.Mips.erase(prepared.Mips.begin(), prepared.Mips.begin() + prepared.FirstMip);
            }
            else prepared.BasePixels.assign(uploadImageData.begin(), uploadImageData.end());
            return prepared;
        }

        [[nodiscard]] ManagedTexture SubmitPreparedImage(
            const nvrhi::DeviceHandle& device, const TextureLoadDesc& desc,
            PreparedImageUpload&& prepared, nvrhi::EventQueryHandle& completion,
            UploadValidation validation = {}, nvrhi::CommandListHandle* reusable = nullptr)
        {
            const nvrhi::TextureDesc& textureDesc = prepared.Description;
            ManagedTexture resource;
            resource.Texture =
#if defined(HE_DEV_AGENT_RUNTIME)
                validation.Failure == TextureUploadFailure::TextureAllocation ? nullptr :
#endif
                device->createTexture(textureDesc);
            if (!resource.Texture)
                throw std::runtime_error("NVRHI failed to create texture asset: " + desc.AssetPath);
            resource.Width = textureDesc.width;
            resource.Height = textureDesc.height;
            resource.MipLevels = textureDesc.mipLevels;
            resource.SourceWidth = prepared.SourceWidth; resource.SourceHeight = prepared.SourceHeight;
            resource.FirstResidentMip = prepared.FirstMip;
            resource.Format = textureDesc.format;
            resource.AssetPath = desc.AssetPath;
            resource.EnvironmentProposal = std::move(prepared.Proposal);
            if (resource.EnvironmentProposal)
            {
                nvrhi::TextureDesc cdfDesc;
                cdfDesc.width = EnvironmentSamplingProposal::Cells + 1;
                cdfDesc.height = 1;
                cdfDesc.format = nvrhi::Format::R32_FLOAT;
                cdfDesc.isShaderResource = true;
                cdfDesc.initialState = nvrhi::ResourceStates::Common;
                cdfDesc.keepInitialState = false;
                cdfDesc.debugName = "Environment immutable proposal CDF";
                resource.EnvironmentCdf = device->createTexture(cdfDesc);
                if (!resource.EnvironmentCdf) resource.EnvironmentProposal.reset();
            }

            nvrhi::CommandListHandle commandList = AcquireUploadCommands(device, reusable, validation);
            if (!commandList)
                throw std::runtime_error("NVRHI failed to create texture upload command list");
            commandList->open();
            if (resource.EnvironmentCdf && resource.EnvironmentProposal)
            {
                const auto& cdf = resource.EnvironmentProposal->Cdf;
                commandList->beginTrackingTextureState(resource.EnvironmentCdf,
                    nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
                commandList->writeTexture(resource.EnvironmentCdf, 0, 0, cdf.data(),
                    cdf.size() * sizeof(float), cdf.size() * sizeof(float));
                commandList->setPermanentTextureState(resource.EnvironmentCdf,
                    nvrhi::ResourceStates::ShaderResource);
            }
            commandList->beginTrackingTextureState(
                resource.Texture,
                nvrhi::AllSubresources,
                nvrhi::ResourceStates::Common);
            const size_t pixelSize = textureDesc.format == nvrhi::Format::RGBA32_FLOAT ? 16u : 4u;
            const size_t rowPitch = static_cast<size_t>(textureDesc.width) * pixelSize;
            if (textureDesc.height == 0 || rowPitch >
                std::numeric_limits<size_t>::max() / textureDesc.height ||
                prepared.BasePixels.size() != rowPitch * textureDesc.height)
                throw std::runtime_error("Prepared texture base byte count is invalid");
            resource.PayloadBytes = rowPitch * textureDesc.height;
            if (resource.EnvironmentCdf)
                resource.PayloadBytes += (EnvironmentSamplingProposal::Cells + 1u) * sizeof(float);
            commandList->writeTexture(
                resource.Texture,
                0,
                0,
                prepared.BasePixels.data(),
                rowPitch,
                rowPitch * textureDesc.height);

            for (uint32_t mipLevel = 1; mipLevel < textureDesc.mipLevels; ++mipLevel)
            {
                const TextureMipUpload& mip = prepared.Mips[mipLevel - 1u];
                const size_t mipRowPitch = static_cast<size_t>(mip.Width) * pixelSize;
                if (mip.Height == 0 || mipRowPitch >
                    std::numeric_limits<size_t>::max() / mip.Height ||
                    mip.Pixels.size() != mipRowPitch * mip.Height)
                    throw std::runtime_error("Prepared texture mip byte count is invalid");
                resource.PayloadBytes += mipRowPitch * mip.Height;
                commandList->writeTexture(
                    resource.Texture,
                    0,
                    mipLevel,
                    mip.Pixels.data(),
                    mipRowPitch,
                    mipRowPitch * mip.Height);
            }
            if (!desc.bAllowReadback)
                commandList->setPermanentTextureState(
                    resource.Texture,
                    nvrhi::ResourceStates::ShaderResource);
            commandList->commitBarriers();
            commandList->close();
            SubmitTextureUpload(device, commandList, completion, validation);
            return resource;
        }

        struct PreparedDdsUpload
        {
            nvrhi::TextureDesc Description;
            DdsTextureData Dds;
            uint32_t SourceWidth = 0, SourceHeight = 0, ResidentMip = 0;
            size_t FirstMip = 0;
        };

        [[nodiscard]] PreparedDdsUpload PrepareDdsUpload(
            const TextureLoadDesc& desc, const std::filesystem::path& path)
        {
            if (desc.bFlipVertically)
                throw std::runtime_error("DDS vertical flips must be baked offline: " + desc.AssetPath);
            PreparedDdsUpload prepared;
            prepared.Dds = desc.bStreamable
                ? DdsTextureData::LoadMipTail(path, desc.MaximumDimension, desc.FirstResidentMip)
                : DdsTextureData::Load(path);
            const DdsTextureData& dds = prepared.Dds;
            if (ShouldGenerateMipmaps(desc) && !dds.HasCompleteMipChain())
                throw std::runtime_error("DDS sampled assets require a complete offline mip chain: " + desc.AssetPath);
            if (dds.Format == DdsBlockFormat::BC5 && desc.ColorSpace == TextureColorSpace::SRgb)
                throw std::runtime_error("BC5 data textures cannot use sRGB sampling: " + desc.AssetPath);
            if (dds.Format == DdsBlockFormat::RGBA16Float && desc.ColorSpace == TextureColorSpace::SRgb)
                throw std::runtime_error("HDR DDS textures require linear sampling: " + desc.AssetPath);
            if (!desc.bStreamable && desc.MaximumDimension != 0)
            {
                while (prepared.FirstMip + 1 < dds.Mips.size() &&
                    std::max(dds.Mips[prepared.FirstMip].Width,
                        dds.Mips[prepared.FirstMip].Height) > desc.MaximumDimension)
                    ++prepared.FirstMip;
                if (std::max(dds.Mips[prepared.FirstMip].Width,
                    dds.Mips[prepared.FirstMip].Height) > desc.MaximumDimension)
                    throw std::runtime_error("DDS has no mip satisfying the explicit size limit");
            }
            prepared.SourceWidth = desc.bStreamable ? dds.SourceWidth : dds.Mips[prepared.FirstMip].Width;
            prepared.SourceHeight = desc.bStreamable ? dds.SourceHeight : dds.Mips[prepared.FirstMip].Height;
            prepared.ResidentMip = desc.bStreamable ? dds.FirstResidentMip : (ShouldGenerateMipmaps(desc)
                ? std::min(desc.FirstResidentMip, static_cast<uint32_t>(dds.Mips.size() - prepared.FirstMip - 1)) : 0);
            if (!desc.bStreamable) prepared.FirstMip += prepared.ResidentMip;
            nvrhi::TextureDesc& textureDesc = prepared.Description;
            textureDesc.width = dds.Mips[prepared.FirstMip].Width;
            textureDesc.height = dds.Mips[prepared.FirstMip].Height;
            textureDesc.mipLevels = ShouldGenerateMipmaps(desc)
                ? static_cast<uint32_t>(dds.Mips.size() - prepared.FirstMip) : 1u;
            textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
            textureDesc.isShaderResource = true;
            textureDesc.initialState = desc.bAllowReadback
                ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::Common;
            textureDesc.keepInitialState = desc.bAllowReadback;
            const bool bSRgb = desc.ColorSpace == TextureColorSpace::SRgb;
            switch (dds.Format)
            {
            case DdsBlockFormat::RGBA16Float: textureDesc.format = nvrhi::Format::RGBA16_FLOAT; break;
            case DdsBlockFormat::BC1: textureDesc.format = bSRgb ? nvrhi::Format::BC1_UNORM_SRGB : nvrhi::Format::BC1_UNORM; break;
            case DdsBlockFormat::BC3: textureDesc.format = bSRgb ? nvrhi::Format::BC3_UNORM_SRGB : nvrhi::Format::BC3_UNORM; break;
            case DdsBlockFormat::BC5: textureDesc.format = nvrhi::Format::BC5_UNORM; break;
            case DdsBlockFormat::BC7: textureDesc.format = bSRgb ? nvrhi::Format::BC7_UNORM_SRGB : nvrhi::Format::BC7_UNORM; break;
            }
            textureDesc.debugName = desc.DebugName.empty()
                ? "TextureManager " + desc.AssetPath : desc.DebugName;
            return prepared;
        }

        [[nodiscard]] ManagedTexture SubmitPreparedDds(
            const nvrhi::DeviceHandle& device, const TextureLoadDesc& desc,
            PreparedDdsUpload&& prepared, nvrhi::EventQueryHandle& completion,
            UploadValidation validation = {}, nvrhi::CommandListHandle* reusable = nullptr)
        {
            const nvrhi::TextureDesc& textureDesc = prepared.Description;
            ManagedTexture resource;
            resource.Texture =
#if defined(HE_DEV_AGENT_RUNTIME)
                validation.Failure == TextureUploadFailure::TextureAllocation ? nullptr :
#endif
                device->createTexture(textureDesc);
            if (!resource.Texture)
                throw std::runtime_error("NVRHI failed to create compressed texture: " + desc.AssetPath);
            resource.Width = textureDesc.width;
            resource.Height = textureDesc.height;
            resource.MipLevels = textureDesc.mipLevels;
            resource.SourceWidth = prepared.SourceWidth; resource.SourceHeight = prepared.SourceHeight;
            resource.FirstResidentMip = prepared.ResidentMip;
            resource.Format = textureDesc.format;
            resource.AssetPath = desc.AssetPath;
            nvrhi::CommandListHandle commandList = AcquireUploadCommands(device, reusable, validation);
            if (!commandList)
                throw std::runtime_error("NVRHI failed to create DDS upload command list");
            commandList->open();
            commandList->beginTrackingTextureState(resource.Texture,
                nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
            for (uint32_t mipLevel = 0; mipLevel < textureDesc.mipLevels; ++mipLevel)
            {
                const DdsMipData& mip = prepared.Dds.Mips[prepared.FirstMip + mipLevel];
                commandList->writeTexture(resource.Texture, 0, mipLevel,
                    prepared.Dds.Bytes.data() + mip.Offset, mip.RowPitch, mip.SlicePitch);
                resource.PayloadBytes += mip.SlicePitch;
            }
            if (!desc.bAllowReadback)
                commandList->setPermanentTextureState(resource.Texture,
                    nvrhi::ResourceStates::ShaderResource);
            commandList->commitBarriers();
            commandList->close();
            SubmitTextureUpload(device, commandList, completion, validation);
            return resource;
        }

    }

    struct TexturePreparationJob
    {
        TextureLoadDesc Desc;
        std::filesystem::path Path;
        std::atomic_bool Cancelled{false};
        std::mutex Mutex;
        std::condition_variable Changed;
        std::optional<std::variant<PreparedImageUpload, PreparedDdsUpload>> Prepared;
        std::exception_ptr Failure;
        uint64_t PreparedBytes = 0;
        double CpuMilliseconds = 0.0;
        bool bDds = false;
        bool bEnqueued = false; // TextureManager owner thread only.
        bool bFinished = false; // Protected by Mutex.
    };

    class TexturePreparationQueue final
    {
    public:
        TexturePreparationQueue()
            : mWaiting(ReadPreparedByteBudget()),
              mWorker([this](std::stop_token)
            {
                for (;;)
                {
                    std::shared_ptr<TexturePreparationJob> job;
                    {
                        std::unique_lock lock(mMutex);
                        if (!mStopping && !mWaiting.Empty() && mFrameCredits != 0 &&
                            !mWaiting.CanStart(mFrameCredits))
                            mWaiting.RecordBudgetStall();
                        mChanged.wait(lock, [this]
                        { return mStopping || CanDispatch(); });
                        if (mStopping) return;
                        job = mWaiting.TakeNext();
                        --mFrameCredits;
                    }
                    const auto started = std::chrono::steady_clock::now();
                    std::optional<std::variant<PreparedImageUpload, PreparedDdsUpload>> prepared;
                    std::exception_ptr failure;
                    try
                    {
                        if (!job->Cancelled.load(std::memory_order_acquire))
                        {
                            if (job->bDds)
                                prepared.emplace(PrepareDdsUpload(job->Desc, job->Path));
                            else
                                prepared.emplace(PrepareImageUpload(job->Desc, job->Path));
                        }
                    }
                    catch (...) { failure = std::current_exception(); }
                    const double elapsed = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started).count();
                    uint64_t preparedBytes = 0;
                    if (prepared && !job->Cancelled.load(std::memory_order_acquire))
                    {
                        preparedBytes = std::visit([](const auto& value) -> uint64_t
                        {
                            using T = std::decay_t<decltype(value)>;
                            if constexpr (std::is_same_v<T, PreparedDdsUpload>)
                                return value.Dds.Bytes.size();
                            else
                            {
                                uint64_t bytes = value.BasePixels.size();
                                for (const auto& mip : value.Mips)
                                    bytes += mip.Pixels.size();
                                return bytes;
                            }
                        }, *prepared);
                    }
                    {
                        std::lock_guard lock(mMutex);
                        std::lock_guard jobLock(job->Mutex);
                        if (!job->Cancelled.load(std::memory_order_acquire))
                        {
                            job->Prepared = std::move(prepared);
                            job->Failure = failure;
                            job->PreparedBytes = preparedBytes;
                            mWaiting.AddPreparedBytes(preparedBytes);
                        }
                        job->CpuMilliseconds = elapsed;
                        job->bFinished = true;
                        mWaiting.CompleteActive();
                    }
                    job->Changed.notify_all();
                }
            }) {}

        ~TexturePreparationQueue()
        {
            {
                std::lock_guard lock(mMutex);
                mStopping = true;
            }
            mChanged.notify_all();
            if (mWorker.joinable()) mWorker.join();
        }

        bool TryEnqueue(const std::shared_ptr<TexturePreparationJob>& job)
        {
            std::lock_guard lock(mMutex);
            if (mStopping || !mWaiting.TryEnqueue(job, 32))
                return false;
            job->bEnqueued = true;
            mChanged.notify_one();
            return true;
        }

        bool TakeWaitingForOwner(const std::shared_ptr<TexturePreparationJob>& job) noexcept
        {
            std::lock_guard lock(mMutex);
            if (!mWaiting.TakeWaitingForOwner(job)) return false;
            job->bEnqueued = false;
            ++mOwnerClaims;
            return true;
        }

        void AddFrameCredits(size_t credits) noexcept
        {
            std::lock_guard lock(mMutex);
            mFrameCredits = std::min<size_t>(4, mFrameCredits + credits);
            mChanged.notify_one();
        }

        void ReleasePreparedBytes(uint64_t bytes) noexcept
        {
            std::lock_guard lock(mMutex);
            mWaiting.ReleasePreparedBytes(bytes);
            mChanged.notify_one();
        }

        void Cancel(const std::shared_ptr<TexturePreparationJob>& job) noexcept
        {
            std::lock_guard lock(mMutex);
            std::lock_guard jobLock(job->Mutex);
            job->Cancelled.store(true, std::memory_order_release);
            if (mWaiting.CancelWaiting(job)) job->bFinished = true;
            mWaiting.ReleasePreparedBytes(job->PreparedBytes);
            job->PreparedBytes = 0;
            job->Prepared.reset();
            job->Changed.notify_all();
            mChanged.notify_one();
        }

        [[nodiscard]] uint64_t GetPreparedBytes() const noexcept
        {
            std::lock_guard lock(mMutex);
            return mWaiting.PreparedBytes();
        }

        [[nodiscard]] std::pair<uint64_t, uint64_t> GetBudgetStatistics() const noexcept
        {
            std::lock_guard lock(mMutex);
            return { mWaiting.PreparedByteBudget(), mWaiting.DecodeBudgetStalls() };
        }

        [[nodiscard]] uint64_t GetOwnerClaims() const noexcept
        {
            std::lock_guard lock(mMutex);
            return mOwnerClaims;
        }

    private:
        [[nodiscard]] bool CanDispatch() const
        {
            if (!mWaiting.CanStart(mFrameCredits)) return false;
#if defined(HE_DEV_AGENT_RUNTIME)
            const char* heldPath = std::getenv("HE_TEXTURE_PREPARATION_HOLD_PATH");
            const auto next = mWaiting.PeekWaiting();
            return !heldPath || !*heldPath || !next ||
                next->Desc.AssetPath != heldPath;
#else
            return true;
#endif
        }

        static uint64_t ReadPreparedByteBudget() noexcept
        {
            constexpr uint64_t defaultMiB = 128;
            const char* value = std::getenv("HE_TEXTURE_PREPARED_BUDGET_MIB");
            if (!value || !*value) return defaultMiB * 1024 * 1024;
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(value, &end, 10);
            const uint64_t mib = end && *end == '\0' && parsed >= 1 && parsed <= 512
                ? static_cast<uint64_t>(parsed) : defaultMiB;
            return mib * 1024 * 1024;
        }

        mutable std::mutex mMutex;
        std::condition_variable mChanged;
        TexturePreparationQueueState<TexturePreparationJob> mWaiting;
        size_t mFrameCredits = 0;
        uint64_t mOwnerClaims = 0;
        bool mStopping = false;
        std::jthread mWorker;
    };

    bool TryParseDefaultBindlessTexture(
        std::string_view token,
        DefaultBindlessTexture& textureKind) noexcept
    {
        std::string normalized(token);
        for (char& character : normalized)
        {
            if (character == '/')
                character = '\\';
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        if (normalized == "white" || normalized == "assets\\textures\\white.png")
            textureKind = DefaultBindlessTexture::White;
        else if (normalized == "black" || normalized == "assets\\textures\\black.png")
            textureKind = DefaultBindlessTexture::Black;
        else if (normalized == "normal" || normalized == "assets\\textures\\defaultnormal.png")
            textureKind = DefaultBindlessTexture::Normal;
        else
            return false;
        return true;
    }

#if defined(HE_DEV_AGENT_RUNTIME)
    std::vector<TextureMipValidationData> GenerateRgba8MipsForValidation(
        std::span<const std::byte> pixels, uint32_t width, uint32_t height,
        TextureColorSpace colorSpace, bool bPreserveAlphaCoverage,
        float alphaCoverageCutoff)
    {
        std::vector<TextureMipUpload> generated = GenerateMipChain(
            pixels, width, height, EResourceFormat::R8G8B8A8_UNORM,
            colorSpace, bPreserveAlphaCoverage, alphaCoverageCutoff);
        std::vector<TextureMipValidationData> result;
        result.reserve(generated.size());
        for (TextureMipUpload& mip : generated)
            result.push_back({ mip.Width, mip.Height, std::move(mip.Pixels) });
        return result;
    }
#endif

    ManagedTextureHandle ManagedTextureReferenceState::Acquire(uint32_t index)
    {
        if (index == InvalidDescriptorIndex)
            throw std::invalid_argument("Managed texture slot index must be valid");
        if (mReferenceCount == std::numeric_limits<uint32_t>::max())
            throw std::overflow_error("Managed texture reference count overflow");
        ++mReferenceCount;
        return { index, mGeneration };
    }

    bool ManagedTextureReferenceState::Release(ManagedTextureHandle handle)
    {
        if (!IsValid(handle))
            throw std::out_of_range("Invalid or stale managed texture handle");
        --mReferenceCount;
        if (mReferenceCount != 0)
            return false;
        ++mGeneration;
        if (mGeneration == 0)
            mGeneration = 1;
        return true;
    }

    bool ManagedTextureReferenceState::IsValid(ManagedTextureHandle handle) const noexcept
    {
        return handle && mReferenceCount != 0 && handle.Generation == mGeneration;
    }

    TextureManager::TextureManager(const TextureManagerDesc& desc)
        : mDevice(desc.Device)
        , mAssetRootDirectory(desc.AssetRootDirectory)
        , mEngineAssetRootDirectory(desc.EngineAssetRootDirectory)
        , mBindless(desc.Device, desc.Bindless)
    {
        if (!mDevice)
            throw std::invalid_argument("TextureManager requires a valid NVRHI device");

        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.setAllFilters(true)
            .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap)
            .setMaxAnisotropy(8.0f);
        mMaterialSampler = mDevice->createSampler(samplerDesc);
        if (!mMaterialSampler)
            throw std::runtime_error("NVRHI failed to create the global material sampler");
        mMaterialSamplerHandle = mBindless.GetSamplers().AllocateSampler(mMaterialSampler);
        CreateDefaults();
        mPreparationQueue = std::make_unique<TexturePreparationQueue>();
    }

    TextureManager::~TextureManager()
    {
        for (Slot& slot : mSlots)
            if (slot.Preparation)
                mPreparationQueue->Cancel(slot.Preparation);
        mPreparationQueue.reset();
        for (Slot& slot : mSlots)
            if (slot.UploadCompletion)
            {
                mDevice->waitEventQuery(slot.UploadCompletion);
                mDevice->resetEventQuery(slot.UploadCompletion);
            }
        for (RetiredSlot& retired : mRetiredSlots)
            if (retired.UploadCompletion)
            {
                mDevice->waitEventQuery(retired.UploadCompletion);
                mDevice->resetEventQuery(retired.UploadCompletion);
            }
    }

    std::filesystem::path TextureManager::ResolveAssetPath(
		std::string_view path,
		bool bAllowAbsolutePath) const
    {
        const std::filesystem::path rawPath = Utils::Utf8ToPath(path);
        if (rawPath.is_absolute() || rawPath.has_root_name())
        {
            std::error_code error;
            const std::filesystem::path candidate =
                std::filesystem::weakly_canonical(rawPath, error);
            if (error)
                throw std::invalid_argument(
                    "Absolute texture path cannot be resolved: " + error.message());
			if (bAllowAbsolutePath)
				return candidate;
            const auto isWithin = [&](const std::filesystem::path& root)
            {
                error.clear();
                const std::filesystem::path canonicalRoot =
                    std::filesystem::weakly_canonical(root, error);
                if (error)
                    return false;
                const std::filesystem::path relative =
                    candidate.lexically_relative(canonicalRoot);
                return !relative.empty() && !relative.is_absolute() &&
                    !std::ranges::any_of(
                        relative,
                        [](const std::filesystem::path& component)
                        {
                            return component == "..";
                        });
            };
            if (isWithin(mAssetRootDirectory) ||
                (!mEngineAssetRootDirectory.empty() &&
                    isWithin(mEngineAssetRootDirectory)))
            {
                return candidate;
            }
            throw std::invalid_argument(
                "Absolute texture path resolves outside the configured content roots");
        }

        std::string normalized;
        const Utils::PathValidationResult validation =
            Utils::TryNormalizeProjectRelativePath(path, normalized);
        if (!validation)
            throw std::invalid_argument("Unsafe texture asset path: " + validation.Message);

        const std::filesystem::path relative = Utils::Utf8ToPath(normalized);
        const auto resolveWithinRoot = [&](const std::filesystem::path& root)
        {
            std::error_code error;
            const std::filesystem::path canonicalRoot =
                std::filesystem::weakly_canonical(root, error);
            if (error)
                throw std::invalid_argument(
                    "Texture asset root cannot be resolved: " + error.message());
            const std::filesystem::path candidate =
                std::filesystem::weakly_canonical(canonicalRoot / relative, error);
            if (error)
                throw std::invalid_argument(
                    "Texture asset path cannot be resolved: " + error.message());
            const std::filesystem::path relativeToRoot =
                candidate.lexically_relative(canonicalRoot);
            if (relativeToRoot.empty() || relativeToRoot.is_absolute() ||
                std::ranges::any_of(
                    relativeToRoot,
                    [](const std::filesystem::path& component)
                    {
                        return component == "..";
                    }))
            {
                throw std::invalid_argument(
                    "Texture asset resolves outside its content root");
            }
            return candidate;
        };

        const std::filesystem::path projectPath =
            resolveWithinRoot(mAssetRootDirectory);
        std::error_code error;
        if (std::filesystem::is_regular_file(projectPath, error) && !error)
            return projectPath;
        if (!mEngineAssetRootDirectory.empty() &&
            mEngineAssetRootDirectory != mAssetRootDirectory)
        {
            return resolveWithinRoot(mEngineAssetRootDirectory);
        }
        return projectPath;
    }

    std::string TextureManager::MakeCacheKey(
        const TextureLoadDesc& desc,
        const std::filesystem::path& resolvedPath) const
    {
        std::string key = Utils::PathToUtf8(resolvedPath) +
            (desc.ColorSpace == TextureColorSpace::SRgb ? "#srgb" : "#linear") +
            (desc.bFlipVertically ? "#flip" : "#noflip") +
            (desc.bCreateBindlessDescriptor ? "#bindless" : "#owned") +
            (desc.bBuildEnvironmentProposal ? "#envproposal1" : "#noenvproposal") +
			(ShouldGenerateMipmaps(desc) ? "#fullmips" : "#mip0") +
			"#maxdim=" + std::to_string(desc.MaximumDimension) +
            (desc.bStreamable ? "#streamable" : "#pinned-mip=" + std::to_string(desc.FirstResidentMip));
        if (desc.bAllowReadback)
            key += "#readback";
        if (!ShouldPreserveAlphaCoverage(desc))
            return key + "#straightalpha";

        float cutoff = std::clamp(desc.AlphaCoverageCutoff, 0.0f, 1.0f);
        if (cutoff == 0.0f)
            cutoff = 0.0f;
        key += "#alphacoverage=";
        key += std::to_string(std::bit_cast<uint32_t>(cutoff));
        return key;
    }

    uint32_t TextureManager::AllocateSlot()
    {
        if (!mFreeSlots.empty())
        {
            const uint32_t index = mFreeSlots.back();
            mFreeSlots.pop_back();
            mSlots[index].UploadState.Reset();
            return index;
        }
        if (mSlots.size() >= std::numeric_limits<uint32_t>::max())
            throw std::length_error("TextureManager slot index capacity exceeded");
        mSlots.emplace_back();
        return static_cast<uint32_t>(mSlots.size() - 1);
    }

    void TextureManager::CreateDefaults()
    {
        struct DefaultUpload
        {
            DefaultBindlessTexture Kind;
            nvrhi::Format Format;
            std::array<uint8_t, 8> Pixel;
            uint32_t PixelBytes;
            const char* Name;
        };
        constexpr std::array<DefaultUpload, 3> uploads = {{
            { DefaultBindlessTexture::White, nvrhi::Format::SRGBA8_UNORM,
                { 255, 255, 255, 255 }, 4, "TextureManager Default White" },
            { DefaultBindlessTexture::Black, nvrhi::Format::SRGBA8_UNORM,
                { 0, 0, 0, 255 }, 4, "TextureManager Default Black" },
            // UNORM8 cannot represent 0.5: {128,128,255} tilts a missing normal
            // map and creates grazing direct light. Only this 1x1 fallback uses
            // exact half values (.5,.5,1,1); authored maps keep their encoding.
            { DefaultBindlessTexture::Normal, nvrhi::Format::RGBA16_FLOAT,
                { 0x00, 0x38, 0x00, 0x38, 0x00, 0x3c, 0x00, 0x3c }, 8, "TextureManager Default Normal" },
        }};

        nvrhi::TextureDesc textureDesc;
        textureDesc.width = 1;
        textureDesc.height = 1;
        textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
        textureDesc.isShaderResource = true;
        textureDesc.initialState = nvrhi::ResourceStates::Common;
        textureDesc.keepInitialState = false;

        nvrhi::CommandListHandle commandList = mDevice->createCommandList();
        if (!commandList)
            throw std::runtime_error("NVRHI failed to create default texture upload command list");
        commandList->open();
        for (const DefaultUpload& upload : uploads)
        {
            textureDesc.format = upload.Format;
            textureDesc.debugName = upload.Name;
            ManagedTexture& resource = mDefaultTextures[DefaultTextureArrayIndex(upload.Kind)];
            resource.Texture = mDevice->createTexture(textureDesc);
            if (!resource.Texture)
                throw std::runtime_error(std::string("NVRHI failed to create ") + upload.Name);
            resource.Width = 1;
            resource.Height = 1;
            resource.PayloadBytes = upload.PixelBytes;
            resource.Format = upload.Format;
            resource.AssetPath = upload.Name;
            resource.bDefault = true;
            commandList->beginTrackingTextureState(
                resource.Texture,
                nvrhi::AllSubresources,
                nvrhi::ResourceStates::Common);
            commandList->writeTexture(
                resource.Texture,
                0,
                0,
                upload.Pixel.data(),
                upload.PixelBytes,
                upload.PixelBytes);
			// Managed textures are immutable after this upload. Publish their
			// permanent state so subsequent command lists and RenderGraph/UI users
			// agree on the enhanced-barrier layout without rediscovering Common.
			commandList->setPermanentTextureState(
				resource.Texture,
				nvrhi::ResourceStates::ShaderResource);
        }
        commandList->commitBarriers();
        commandList->close();
        mDevice->executeCommandList(commandList, nvrhi::CommandQueue::Graphics);
        ++mUploadStatistics.GlobalWaits;
        if (!mDevice->waitForIdle())
            throw std::runtime_error("GPU wait failed while uploading default textures");

        for (const DefaultUpload& upload : uploads)
        {
            ManagedTexture& resource = mDefaultTextures[DefaultTextureArrayIndex(upload.Kind)];
            mBindless.GetTextures().SetDefaultTexture(upload.Kind, resource.Texture);
            resource.ShaderDescriptorIndex =
                mBindless.GetTextures().GetDefaultTextureIndex(upload.Kind);
        }
    }

    ManagedTexture TextureManager::LoadDdsTexture(
        const TextureLoadDesc& desc, const std::filesystem::path& path,
        nvrhi::EventQueryHandle& completion)
    {
        return SubmitPreparedDds(mDevice, desc, PrepareDdsUpload(desc, path), completion);
    }

    ManagedTextureHandle TextureManager::LoadTexture(const TextureLoadDesc& desc)
    {
        if (std::this_thread::get_id() != mOwnerThread)
            throw std::logic_error("TextureManager::LoadTexture must run on its RHI owner thread");
        if (desc.bStreamable && (!desc.bGenerateMipmaps || desc.bRequireReady || desc.bBuildEnvironmentProposal || desc.bAllowReadback))
            throw std::invalid_argument("Streamable textures require asynchronous sampled mip chains");
        const auto requestedAt = std::chrono::steady_clock::now();
        ++mUploadStatistics.Requests;
        if (desc.bPreserveAlphaCoverage && !std::isfinite(desc.AlphaCoverageCutoff))
            throw std::invalid_argument("Alpha coverage cutoff must be finite");
        const std::filesystem::path resolvedPath = ResolveAssetPath(
			desc.AssetPath,
			desc.bAllowAbsolutePath);
        const std::string cacheKey = MakeCacheKey(desc, resolvedPath);
        if (const auto existing = mCache.find(cacheKey); existing != mCache.end())
        {
            Slot& slot = mSlots.at(existing->second.Index);
            if (slot.UploadState.Get() == TextureUploadStage::Failed &&
                !slot.bReloading)
                throw std::runtime_error(mFailedLoads.at(cacheKey));
            if (desc.bRequireReady &&
                slot.UploadState.Get() != TextureUploadStage::Failed)
                WaitForUpload(slot);
            return slot.References.Acquire(existing->second.Index);
        }
        if (const auto failure = mFailedLoads.find(cacheKey); failure != mFailedLoads.end())
            throw std::runtime_error(failure->second);

        std::string extension = resolvedPath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension == ".dds" && desc.bRequireReady)
        {
            try
            {
                nvrhi::EventQueryHandle completion;
                auto resource = LoadDdsTexture(desc, resolvedPath, completion);
                const auto handle = RegisterTexture(desc, cacheKey,
                    std::move(resource), std::move(completion), requestedAt);
                if (desc.bRequireReady) WaitForUpload(mSlots[handle.Index]);
                return handle;
            }
            catch (const std::exception& error)
            {
                const std::string message = "Failed to load DDS asset " + desc.AssetPath + ": " + error.what();
                mFailedLoads.emplace(cacheKey, message);
                throw std::runtime_error(message);
            }
        }

        if (!desc.bRequireReady)
        {
            const size_t pendingPreparations = std::count_if(
                mSlots.begin(), mSlots.end(), [](const Slot& slot)
                { return slot.UploadState.Get() == TextureUploadStage::Preparing; });
            auto job = std::make_shared<TexturePreparationJob>();
            job->Desc = desc;
            job->Path = resolvedPath;
            job->bDds = extension == ".dds";
            ManagedTexture placeholder;
            placeholder.Texture = mDefaultTextures[DefaultTextureArrayIndex(
                desc.PendingFallback)].Texture;
            placeholder.Format = job->bDds ? nvrhi::Format::UNKNOWN
                : extension == ".hdr" ? nvrhi::Format::RGBA32_FLOAT
                : (desc.ColorSpace == TextureColorSpace::SRgb
                    ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM);
            placeholder.AssetPath = desc.AssetPath;
            if (desc.bCreateBindlessDescriptor)
            {
                placeholder.BindlessHandle = mBindless.GetTextures().AllocateTexture(
                    placeholder.Texture);
                placeholder.ShaderDescriptorIndex = mBindless.GetTextures().GetShaderDescriptorIndex(
                    placeholder.BindlessHandle);
            }
            const uint32_t index = AllocateSlot();
            Slot& slot = mSlots[index];
            slot.CacheKey = cacheKey;
            slot.SourceDesc = desc;
            slot.RequestedPixels = 0; slot.LastDemandFrame = 0; slot.LastStreamingChange = mStreamingFrame;
            slot.Resource = std::move(placeholder);
            slot.Preparation = std::move(job);
            slot.UploadState.BeginPreparation();
            slot.RequestedAt = requestedAt;
            const ManagedTextureHandle handle = slot.References.Acquire(index);
            mCache.emplace(cacheKey, handle);
            (void)mPreparationQueue->TryEnqueue(slot.Preparation);
            // A burst issued between Collect calls must remain visible in the
            // peak queue metric even if a ready lease drains it immediately.
            mUploadStatistics.PeakPending = std::max(
                mUploadStatistics.PeakPending, pendingPreparations + 1);
            ++mRevision;
            return handle;
        }

        PreparedImageUpload prepared;
        try { prepared = PrepareImageUpload(desc, resolvedPath); }
        catch (const std::exception& error)
        {
            mFailedLoads.emplace(cacheKey, error.what());
            throw;
        }
        nvrhi::EventQueryHandle completion;
        ManagedTexture resource = SubmitPreparedImage(mDevice, desc, std::move(prepared), completion);
        const auto handle = RegisterTexture(desc, cacheKey, std::move(resource), std::move(completion), requestedAt);
        if (desc.bRequireReady) WaitForUpload(mSlots[handle.Index]);
        return handle;
    }

    void TextureManager::BeginStreamingFrame()
    {
        if (std::this_thread::get_id() != mOwnerThread)
            throw std::logic_error("Texture streaming requires its RHI owner thread");
        ++mStreamingFrame;
    }

    void TextureManager::RequestTextureDetail(ManagedTextureHandle handle, float pixels)
    {
        if (std::this_thread::get_id() != mOwnerThread)
            throw std::logic_error("Texture streaming requires its RHI owner thread");
        if (!IsValid(handle) || !std::isfinite(pixels) || pixels < 0) return;
        auto& slot = mSlots[handle.Index];
        if (!slot.SourceDesc.bStreamable) return;
        if (slot.LastDemandFrame != mStreamingFrame) slot.RequestedPixels = 0;
        slot.RequestedPixels = std::max(slot.RequestedPixels, pixels);
        slot.LastDemandFrame = mStreamingFrame;
    }

    void TextureManager::QueueStreamingMip(Slot& slot, uint32_t mip)
    {
        auto job = std::make_shared<TexturePreparationJob>();
        job->Desc = slot.SourceDesc;
        job->Desc.FirstResidentMip = mip;
        job->Path = ResolveAssetPath(job->Desc.AssetPath, job->Desc.bAllowAbsolutePath);
        std::string extension = job->Path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        job->bDds = extension == ".dds";
        if (slot.UploadState.Get() == TextureUploadStage::Ready) slot.UploadState.BeginReload();
        else slot.UploadState.Retry();
        slot.bReloading = true;
        slot.SourceDesc.FirstResidentMip = mip;
        slot.Preparation = std::move(job);
        slot.RequestedAt = std::chrono::steady_clock::now();
        slot.LastStreamingChange = mStreamingFrame;
        mFailedLoads.erase(slot.CacheKey);
        ++mUploadStatistics.Requests; ++mUploadStatistics.StreamingTransitions;
        (void)mPreparationQueue->TryEnqueue(slot.Preparation);
    }

    void TextureManager::EndStreamingFrame(uint64_t budgetBytes)
    {
        if (std::this_thread::get_id() != mOwnerThread)
            throw std::logic_error("Texture streaming requires its RHI owner thread");
#if defined(HE_DEV_AGENT_RUNTIME)
        // Bounded pressure injection for owned validation sessions, not a saved preference.
        static const uint64_t validationBudget = []() -> uint64_t {
            const char* value = std::getenv("HE_TEXTURE_STREAMING_BUDGET_MB");
            if (!value || !*value) return 0;
            char* end = nullptr;
            const auto mb = std::strtoull(value, &end, 10);
            return end && *end == '\0' && mb >= 1 && mb <= 8192 ? mb * 1024 * 1024 : 0;
        }();
        if (validationBudget) budgetBytes = validationBudget;
#endif
        std::vector<TextureStreamingDemand> demands;
        std::vector<size_t> slots;
        uint64_t residentBytes = 0;
        size_t inFlight = 0;
        for (size_t i = 0; i < mSlots.size(); ++i)
        {
            const auto& slot = mSlots[i];
            const auto& resource = slot.Resource;
            if (!slot.SourceDesc.bStreamable || !slot.References.GetReferenceCount() || !resource.SourceWidth) continue;
            const auto& format = nvrhi::getFormatInfo(resource.Format);
            const float pixels = mStreamingFrame - slot.LastDemandFrame < 60 ? slot.RequestedPixels : 0;
            const float required = std::max(128.0f, pixels * 2); // headroom for temporal reconstruction/UV density
            uint32_t mip = 0;
            const uint32_t size = std::max(resource.SourceWidth, resource.SourceHeight);
            while (mip < TextureLastMip(resource.SourceWidth, resource.SourceHeight) && (size >> (mip + 1)) >= required) ++mip;
            demands.push_back({resource.SourceWidth, resource.SourceHeight, format.blockSize,
                format.bytesPerBlock, mip, std::max(1.0f, pixels)});
            slots.push_back(i);
            residentBytes += resource.PayloadBytes + slot.PendingResource.PayloadBytes;
            if (slot.bReloading && slot.UploadState.Get() == TextureUploadStage::Preparing)
                residentBytes += TextureMipTailBytes(demands.back(), slot.SourceDesc.FirstResidentMip);
            if (slot.bReloading && (slot.UploadState.Get() == TextureUploadStage::Preparing ||
                slot.UploadState.Get() == TextureUploadStage::Submitted)) ++inFlight;
        }
        // Old resources awaiting a graphics fence consume headroom too.
        for (const auto& retired : mRetiredSlots) residentBytes += retired.PayloadBytes;
        const auto plan = PlanTextureResidency(demands, budgetBytes);
        mUploadStatistics.StreamingBudgetBytes = budgetBytes;
        mUploadStatistics.StreamingPlannedBytes = plan.Bytes;
        mUploadStatistics.StreamableTextures = slots.size();
        mUploadStatistics.bStreamingMinimumOverrun = plan.bMinimumExceedsBudget;
        size_t queued = 0;
        for (size_t i = 0; i < slots.size() && queued + inFlight < 2; ++i)
        {
            auto& slot = mSlots[slots[i]];
            const auto stage = slot.UploadState.Get();
            if (stage != TextureUploadStage::Ready && stage != TextureUploadStage::Failed) continue;
            uint32_t mip = plan.Mips[i];
            if (mip == slot.Resource.FirstResidentMip) continue;
            const bool demotion = mip > slot.Resource.FirstResidentMip;
            const uint64_t age = mStreamingFrame - slot.LastStreamingChange;
            if (age < (stage == TextureUploadStage::Failed ? 120u : demotion ? 60u : 8u)) continue;
            uint64_t bytes = TextureMipTailBytes(demands[i], mip);
            // A replacement needs temporary storage while the old version is sampled.
            // Demotions may use bounded scratch even when already above budget.
            // If the planned demotion itself exceeds scratch, first publish a
            // smaller tail; a later promotion can restore detail once memory retires.
            while (demotion && bytes > 64ull * 1024 * 1024 &&
                mip < TextureLastMip(demands[i].Width, demands[i].Height))
                bytes = TextureMipTailBytes(demands[i], ++mip);
            const uint64_t allowance = demotion ? std::max(budgetBytes, residentBytes) : budgetBytes;
            if (residentBytes + bytes > allowance + 64ull * 1024 * 1024)
            { ++mUploadStatistics.StreamingBudgetStalls; continue; }
            QueueStreamingMip(slot, mip); ++queued; residentBytes += bytes;
        }
        mUploadStatistics.StreamingAccountedBytes = residentBytes;
        if (mStreamingFrame % 120 == 0 && !slots.empty())
            HE_CORE_INFO("[TextureStreaming] frame={} textures={} budget={} planned={} residentAndRetiring={} transitions={} stalls={} minimumOverrun={}",
                mStreamingFrame, slots.size(), budgetBytes, plan.Bytes, residentBytes,
                mUploadStatistics.StreamingTransitions, mUploadStatistics.StreamingBudgetStalls,
                plan.bMinimumExceedsBudget);
    }

    size_t TextureManager::RequestReload(std::string_view assetPath)
    {
        if (std::this_thread::get_id() != mOwnerThread)
            throw std::logic_error("TextureManager::RequestReload must run on its RHI owner thread");
        const auto normalizePath = [](std::string_view path)
        {
            std::string key = std::filesystem::path(std::string(path))
                .lexically_normal().generic_string();
            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            return key;
        };
        const std::string requestedKey = assetPath.empty() ? std::string{} : normalizePath(assetPath);
        size_t pending = std::count_if(mSlots.begin(), mSlots.end(), [](const Slot& slot)
        {
            return slot.UploadState.Get() == TextureUploadStage::Preparing;
        });
        size_t queued = 0;
        for (Slot& slot : mSlots)
        {
            const TextureUploadStage stage = slot.UploadState.Get();
            if (slot.References.GetReferenceCount() == 0 ||
                (!requestedKey.empty() && normalizePath(slot.Resource.AssetPath) != requestedKey) ||
                (stage != TextureUploadStage::Ready && stage != TextureUploadStage::Failed))
                continue;
            if (pending >= 128) break;
            auto job = std::make_shared<TexturePreparationJob>();
            job->Desc = slot.SourceDesc;
            job->Path = ResolveAssetPath(
                job->Desc.AssetPath, job->Desc.bAllowAbsolutePath);
            std::string extension = job->Path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            job->bDds = extension == ".dds";
            slot.Preparation = std::move(job);
            if (stage == TextureUploadStage::Ready)
            {
                slot.UploadState.BeginReload();
                slot.bReloading = true;
            }
            else
                slot.UploadState.Retry();
            mFailedLoads.erase(slot.CacheKey);
            slot.RequestedAt = std::chrono::steady_clock::now();
            ++mUploadStatistics.Requests;
            (void)mPreparationQueue->TryEnqueue(slot.Preparation);
            HE_CORE_INFO("[TextureUpload] reload queued asset={}", slot.Resource.AssetPath);
            ++queued;
            ++pending;
        }
        return queued;
    }

    ManagedTextureHandle TextureManager::RegisterTexture(
        const TextureLoadDesc& desc, const std::string& cacheKey,
        ManagedTexture resource, nvrhi::EventQueryHandle completion,
        std::chrono::steady_clock::time_point requestedAt)
    {
        if (!completion)
            throw std::logic_error("Texture upload registration requires a completion query");
        ManagedTexture placeholder = resource;
        placeholder.Texture = mDefaultTextures[DefaultTextureArrayIndex(
            desc.PendingFallback)].Texture;
        placeholder.PayloadBytes = 0;
        placeholder.EnvironmentCdf = nullptr;
        placeholder.EnvironmentProposal.reset();
        if (desc.bCreateBindlessDescriptor)
        {
            placeholder.BindlessHandle = mBindless.GetTextures().AllocateTexture(placeholder.Texture);
            placeholder.ShaderDescriptorIndex =
                mBindless.GetTextures().GetShaderDescriptorIndex(placeholder.BindlessHandle);
        }

        const uint32_t index = AllocateSlot();
        Slot& slot = mSlots[index];
        slot.CacheKey = cacheKey;
        slot.SourceDesc = desc;
        slot.Resource = std::move(placeholder);
        slot.PendingResource = std::move(resource);
        slot.UploadCompletion = std::move(completion);
        slot.UploadState.Submit();
        slot.RequestedAt = requestedAt;
        mUploadStatistics.SubmittedBytes += slot.PendingResource.PayloadBytes;
        const ManagedTextureHandle handle = slot.References.Acquire(index);
        mCache.emplace(cacheKey, handle);
        ++mRevision;
        return handle;
    }

    void TextureManager::PublishCompletedSlot(Slot& slot)
    {
        if (slot.Resource.BindlessHandle)
        {
            const auto previousHandle = slot.Resource.BindlessHandle;
            if (!mBindless.GetTextures().TryPublishTexture(slot.Resource.BindlessHandle,
                slot.PendingResource.Texture, mLastSubmittedSubmission, mCompletedSubmission))
            {
                ++mUploadStatistics.DescriptorPublicationStalls;
                return;
            }
            if (slot.Resource.BindlessHandle != previousHandle)
                ++mUploadStatistics.DescriptorReplacements;
            slot.Resource.ShaderDescriptorIndex = mBindless.GetTextures().GetShaderDescriptorIndex(
                slot.Resource.BindlessHandle);
        }
        if (slot.bReloading && slot.Resource.Texture)
            mRetiredSlots.push_back({ InvalidDescriptorIndex,
                mLastSubmittedSubmission, slot.Resource.Texture,
                slot.Resource.EnvironmentCdf, nullptr,
                slot.Resource.PayloadBytes });
        slot.UploadState.Complete();
        ++mUploadStatistics.Completed;
        const double latencyMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - slot.RequestedAt).count();
        mUploadStatistics.CompletionLatencyMilliseconds += latencyMilliseconds;
        mCompletionLatencyWindow.Add(latencyMilliseconds);
        mDevice->resetEventQuery(slot.UploadCompletion);
        slot.UploadCompletion = nullptr;
        slot.Resource.Texture = std::move(slot.PendingResource.Texture);
        slot.Resource.Width = slot.PendingResource.Width;
        slot.Resource.Height = slot.PendingResource.Height;
        slot.Resource.MipLevels = slot.PendingResource.MipLevels;
        slot.Resource.SourceWidth = slot.PendingResource.SourceWidth;
        slot.Resource.SourceHeight = slot.PendingResource.SourceHeight;
        slot.Resource.FirstResidentMip = slot.PendingResource.FirstResidentMip;
        if (slot.SourceDesc.bStreamable)
            HE_CORE_INFO("[TextureStreaming] published asset={} mip={} size={}x{} bytes={}",
                slot.Resource.AssetPath, slot.Resource.FirstResidentMip, slot.Resource.Width,
                slot.Resource.Height, slot.PendingResource.PayloadBytes);
        slot.Resource.PayloadBytes = slot.PendingResource.PayloadBytes;
        slot.Resource.Format = slot.PendingResource.Format;
        slot.Resource.EnvironmentCdf = std::move(slot.PendingResource.EnvironmentCdf);
        slot.Resource.EnvironmentProposal = std::move(slot.PendingResource.EnvironmentProposal);
        slot.PendingResource = {};
        slot.bReloading = false;
        mFailedLoads.erase(slot.CacheKey);
        ++mRevision;
    }

    void TextureManager::CompletePreparation(Slot& slot)
    {
        const auto job = slot.Preparation;
        if (!job || slot.UploadState.Get() != TextureUploadStage::Preparing) return;
        std::optional<std::variant<PreparedImageUpload, PreparedDdsUpload>> prepared;
        std::exception_ptr failure;
        uint64_t preparedBytes = 0;
        double cpuMilliseconds = 0.0;
        {
            std::lock_guard lock(job->Mutex);
            if (!job->bFinished) return;
            prepared = std::move(job->Prepared);
            failure = job->Failure;
            preparedBytes = std::exchange(job->PreparedBytes, 0);
            cpuMilliseconds = job->CpuMilliseconds;
        }
        if (job->bEnqueued)
            mPreparationQueue->ReleasePreparedBytes(preparedBytes);
        mUploadStatistics.CpuPreparationMilliseconds += cpuMilliseconds;
        try
        {
            if (failure) std::rethrow_exception(failure);
            if (!prepared)
                throw std::runtime_error("Texture preparation was cancelled");
            nvrhi::EventQueryHandle completion;
            UploadValidation validation;
#if defined(HE_DEV_AGENT_RUNTIME)
            validation.Failure = std::exchange(slot.ValidationFailure, TextureUploadFailure::None);
#endif
            ManagedTexture resource = std::visit([&](auto&& value) -> ManagedTexture
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, PreparedDdsUpload>)
                    return SubmitPreparedDds(mDevice, job->Desc, std::move(value), completion, validation, std::addressof(mUploadCommands));
                else
                    return SubmitPreparedImage(mDevice, job->Desc, std::move(value), completion, validation, std::addressof(mUploadCommands));
            }, std::move(*prepared));
            slot.PendingResource = std::move(resource);
            slot.UploadCompletion = std::move(completion);
            slot.UploadState.Submit();
            mUploadStatistics.SubmittedBytes += slot.PendingResource.PayloadBytes;
            slot.Preparation.reset();
        }
        catch (const std::exception& error)
        {
            // A failure can abandon an open or closed, unsubmitted recording.
            // Discard that list instead of reopening it with unretired scratch
            // versions. Previously submitted uploads remain tracked by NVRHI.
            mUploadCommands = nullptr;
            slot.UploadState.Fail();
#if defined(HE_DEV_AGENT_RUNTIME)
            slot.ValidationFailure = TextureUploadFailure::None;
#endif
            mFailedLoads[slot.CacheKey] = error.what();
            ++mUploadStatistics.Failed;
            HE_CORE_WARN("[TextureUpload] asset={} failed: {}",
                job->Desc.AssetPath, error.what());
        }
    }

#if defined(HE_DEV_AGENT_RUNTIME)
    bool TextureManager::InjectUploadFailureForValidation(
        ManagedTextureHandle handle, TextureUploadFailure failure)
    {
        if (std::this_thread::get_id() != mOwnerThread)
            throw std::logic_error("Texture upload validation must run on its RHI owner thread");
        if (!IsValid(handle) || failure == TextureUploadFailure::None) return false;
        auto& slot = mSlots[handle.Index];
        if (slot.UploadState.Get() != TextureUploadStage::Preparing ||
            slot.ValidationFailure != TextureUploadFailure::None) return false;
        slot.ValidationFailure = failure;
        return true;
    }
#endif

    void TextureManager::QueueDeferredPreparations()
    {
        for (Slot& slot : mSlots)
        {
            if (!slot.Preparation || slot.UploadState.Get() != TextureUploadStage::Preparing ||
                slot.Preparation->bEnqueued)
                continue;
            if (!mPreparationQueue->TryEnqueue(slot.Preparation)) break;
        }
    }

    void TextureManager::WaitForUpload(Slot& slot)
    {
        if (slot.Preparation)
        {
            auto job = slot.Preparation;
            // An explicit ready request may otherwise wait behind the prepared-byte
            // budget while this owner thread is the only one able to publish it.
            if (job->bEnqueued)
                (void)mPreparationQueue->TakeWaitingForOwner(job);
            if (!job->bEnqueued)
            {
                const auto started = std::chrono::steady_clock::now();
                try
                {
                    if (job->bDds)
                        job->Prepared.emplace(PrepareDdsUpload(job->Desc, job->Path));
                    else
                        job->Prepared.emplace(PrepareImageUpload(job->Desc, job->Path));
                }
                catch (...) { job->Failure = std::current_exception(); }
                job->CpuMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                job->bFinished = true;
            }
            else
            {
                mPreparationQueue->AddFrameCredits(1);
                std::unique_lock lock(job->Mutex);
                job->Changed.wait(lock, [&] { return job->bFinished; });
            }
            CompletePreparation(slot);
            if (slot.UploadState.Get() == TextureUploadStage::Failed)
                throw std::runtime_error(mFailedLoads.at(slot.CacheKey));
        }
        if (!slot.UploadCompletion) return;
        mDevice->waitEventQuery(slot.UploadCompletion);
        PublishCompletedSlot(slot);
        if (slot.UploadCompletion)
            throw std::runtime_error("Texture ready request deferred: descriptor heap has in-flight readers");
    }

    void TextureManager::ReleaseTexture(
        ManagedTextureHandle handle,
        uint64_t retireAfterSubmission)
    {
        if (std::this_thread::get_id() != mOwnerThread)
            throw std::logic_error("TextureManager::ReleaseTexture must run on its RHI owner thread");
        if (!IsValid(handle))
            throw std::out_of_range("Invalid or stale managed texture handle");
        retireAfterSubmission = std::max(retireAfterSubmission, mLastSubmittedSubmission);
        Slot& slot = mSlots[handle.Index];
        if (!slot.References.Release(handle))
            return;
        slot.UploadState.Cancel();
        if (slot.Preparation)
        {
            mPreparationQueue->Cancel(slot.Preparation);
            slot.Preparation.reset();
        }
        mCache.erase(slot.CacheKey);
        if (slot.bReloading && slot.UploadCompletion && slot.Resource.Texture)
            mRetiredSlots.push_back({ InvalidDescriptorIndex,
                retireAfterSubmission, slot.Resource.Texture,
                slot.Resource.EnvironmentCdf, nullptr,
                slot.Resource.PayloadBytes });
        if (slot.Resource.BindlessHandle)
        {
            mBindless.GetTextures().ReleaseTexture(
                slot.Resource.BindlessHandle,
                retireAfterSubmission);
        }
        nvrhi::TextureHandle retainedTexture = slot.UploadCompletion
            ? std::move(slot.PendingResource.Texture)
            : std::move(slot.Resource.Texture);
        nvrhi::TextureHandle retainedCdf = slot.UploadCompletion
            ? std::move(slot.PendingResource.EnvironmentCdf)
            : std::move(slot.Resource.EnvironmentCdf);
        nvrhi::EventQueryHandle uploadCompletion = std::move(slot.UploadCompletion);
        const uint64_t retainedPayloadBytes = uploadCompletion
            ? slot.PendingResource.PayloadBytes : slot.Resource.PayloadBytes;
        slot.Resource = {};
        slot.PendingResource = {};
        slot.SourceDesc = {};
#if defined(HE_DEV_AGENT_RUNTIME)
        slot.ValidationFailure = TextureUploadFailure::None;
#endif
        slot.bReloading = false;
        slot.CacheKey.clear();
        if (retireAfterSubmission == 0 && !uploadCompletion)
            mFreeSlots.push_back(handle.Index);
        else
            mRetiredSlots.push_back({ handle.Index, retireAfterSubmission,
                retainedTexture, retainedCdf, uploadCompletion, retainedPayloadBytes });
        ++mRevision;
    }

    void TextureManager::Collect(
        uint64_t completedSubmission, uint64_t lastSubmittedSubmission)
    {
        if (std::this_thread::get_id() != mOwnerThread)
            throw std::logic_error("TextureManager::Collect must run on its RHI owner thread");
        mLastSubmittedSubmission = lastSubmittedSubmission;
        mCompletedSubmission = completedSubmission;
        const TextureUploadStatistics beforeCollect = GetUploadStatistics();
        mUploadStatistics.PeakPending = std::max(
            mUploadStatistics.PeakPending, beforeCollect.Pending);
        mUploadStatistics.PeakPreparedBytes = std::max(
            mUploadStatistics.PeakPreparedBytes, beforeCollect.PreparedBytes);
        mPreparationQueue->AddFrameCredits(2);
        QueueDeferredPreparations();
        size_t submittedCount = 0;
        uint64_t submittedBytes = 0;
        for (Slot& slot : mSlots)
        {
            if (!slot.Preparation || slot.UploadState.Get() != TextureUploadStage::Preparing) continue;
            uint64_t preparedBytes = 0;
            bool bFinished = false;
            bool bFailed = false;
            {
                std::lock_guard lock(slot.Preparation->Mutex);
                bFinished = slot.Preparation->bFinished;
                bFailed = bool(slot.Preparation->Failure);
                preparedBytes = slot.Preparation->PreparedBytes;
            }
            if (!bFinished) continue;
            constexpr uint64_t c_MaxUploadBytesPerCollect = 32ull * 1024 * 1024;
            if (!bFailed && (submittedCount >= 4 ||
                (submittedCount != 0 &&
                    (submittedBytes >= c_MaxUploadBytesPerCollect ||
                        preparedBytes > c_MaxUploadBytesPerCollect - submittedBytes))))
                continue;
            CompletePreparation(slot);
            if (slot.UploadCompletion)
            {
                ++submittedCount;
                submittedBytes += slot.PendingResource.PayloadBytes;
            }
        }
        QueueDeferredPreparations();
        mUploadStatistics.LastCollectUploads = submittedCount;
        mUploadStatistics.LastCollectBytes = submittedBytes;
        mBindless.Collect(completedSubmission);
        for (Slot& slot : mSlots)
        {
            if (!slot.UploadCompletion || !mDevice->pollEventQuery(slot.UploadCompletion))
                continue;
            PublishCompletedSlot(slot);
        }
        for (size_t index = 0; index < mRetiredSlots.size();)
        {
            if (mRetiredSlots[index].Submission > completedSubmission ||
                (mRetiredSlots[index].UploadCompletion &&
                    !mDevice->pollEventQuery(mRetiredSlots[index].UploadCompletion)))
            {
                ++index;
                continue;
            }
            if (mRetiredSlots[index].UploadCompletion)
                mDevice->resetEventQuery(mRetiredSlots[index].UploadCompletion);
            if (mRetiredSlots[index].Index != InvalidDescriptorIndex)
                mFreeSlots.push_back(mRetiredSlots[index].Index);
            if (index + 1 != mRetiredSlots.size())
                mRetiredSlots[index] = std::move(mRetiredSlots.back());
            mRetiredSlots.pop_back();
        }
        static const bool bLogUploadStatistics = []
        {
            const char* value = std::getenv("HE_TEXTURE_UPLOAD_TIMINGS");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        if (bLogUploadStatistics && ++mCollectCount % 15 == 0)
        {
            const auto statistics = GetUploadStatistics();
            HE_CORE_INFO("[TextureUpload] requests={} completed={} pending={} peakPending={} preparing={} deferred={} pendingBytes={} preparedBytes={} peakPreparedBytes={} preparedByteBudget={} decodeBudgetStalls={} ownerClaims={} submittedBytes={} collectUploads={} collectBytes={} failed={} cpuPrepareMs={:.3f} globalWaits={} meanLatencyMs={:.3f} latencySamples={} latencyP50Ms={:.3f} latencyP95Ms={:.3f} latencyP99Ms={:.3f} latencyMaxMs={:.3f}",
                statistics.Requests, statistics.Completed, statistics.Pending,
                statistics.PeakPending, statistics.Preparing, statistics.Deferred,
                statistics.PendingBytes, statistics.PreparedBytes,
                statistics.PeakPreparedBytes, statistics.PreparedByteBudget,
                statistics.DecodeBudgetStalls, statistics.OwnerClaims,
                statistics.SubmittedBytes,
                statistics.LastCollectUploads, statistics.LastCollectBytes,
                statistics.Failed, statistics.CpuPreparationMilliseconds,
                statistics.GlobalWaits,
                statistics.Completed == 0 ? 0.0 :
                    statistics.CompletionLatencyMilliseconds / statistics.Completed,
                statistics.RecentCompletionLatency.Samples,
                statistics.RecentCompletionLatency.P50Milliseconds,
                statistics.RecentCompletionLatency.P95Milliseconds,
                statistics.RecentCompletionLatency.P99Milliseconds,
                statistics.RecentCompletionLatency.MaxMilliseconds);
        }
    }

    bool TextureManager::IsValid(ManagedTextureHandle handle) const noexcept
    {
        return handle && handle.Index < mSlots.size() &&
            mSlots[handle.Index].References.IsValid(handle);
    }

    TextureUploadStage TextureManager::GetUploadStage(ManagedTextureHandle handle) const
    {
        if (!IsValid(handle))
            throw std::out_of_range("Invalid or stale managed texture handle");
        return mSlots[handle.Index].UploadState.Get();
    }

    const ManagedTexture& TextureManager::GetTexture(ManagedTextureHandle handle) const
    {
        if (!IsValid(handle))
            throw std::out_of_range("Invalid or stale managed texture handle");
        return mSlots[handle.Index].Resource;
    }

    const ManagedTexture& TextureManager::GetDefaultTexture(
        DefaultBindlessTexture textureKind) const
    {
        return mDefaultTextures[DefaultTextureArrayIndex(textureKind)];
    }

    const ManagedTexture* TextureManager::FindDefaultTexture(std::string_view token) const noexcept
    {
        DefaultBindlessTexture textureKind = DefaultBindlessTexture::White;
        return TryParseDefaultBindlessTexture(token, textureKind)
            ? &mDefaultTextures[DefaultTextureArrayIndex(textureKind)]
            : nullptr;
    }

    size_t TextureManager::InvalidateFailedLoads(std::string_view assetPath)
    {
        size_t count = 0;
        if (assetPath.empty())
        {
            count = mFailedLoads.size();
            mFailedLoads.clear();
        }
        else
        {
            const std::filesystem::path requested = Utils::Utf8ToPath(assetPath);
            std::vector<std::string> prefixes;
            if (requested.is_absolute() || requested.has_root_name())
            {
                prefixes.push_back(Utils::PathToUtf8(
                    ResolveAssetPath(assetPath, true)) + '#');
            }
            else
            {
                std::string normalized;
                const Utils::PathValidationResult validation =
                    Utils::TryNormalizeProjectRelativePath(assetPath, normalized);
                if (!validation)
                    throw std::invalid_argument("Unsafe texture asset path: " + validation.Message);
                const std::filesystem::path relative = Utils::Utf8ToPath(normalized);
                const auto addRoot = [&](const std::filesystem::path& root)
                {
                    if (root.empty()) return;
                    const std::filesystem::path candidate =
                        ResolveAssetPath(Utils::PathToUtf8(root / relative), false);
                    prefixes.push_back(Utils::PathToUtf8(candidate) + '#');
                };
                addRoot(mAssetRootDirectory);
                addRoot(mEngineAssetRootDirectory);
            }
            count = std::erase_if(mFailedLoads, [&prefixes](const auto& entry)
            {
                return std::ranges::any_of(prefixes, [&](const std::string& prefix)
                {
                    return entry.first.starts_with(prefix);
                });
            });
        }
        for (Slot& slot : mSlots)
        {
            if (slot.UploadState.Get() != TextureUploadStage::Failed || !slot.Preparation ||
                mFailedLoads.contains(slot.CacheKey))
                continue;
            auto retry = std::make_shared<TexturePreparationJob>();
            retry->Desc = slot.Preparation->Desc;
            retry->Path = slot.Preparation->Path;
            retry->bDds = slot.Preparation->bDds;
            mPreparationQueue->Cancel(slot.Preparation);
            slot.Preparation = std::move(retry);
            slot.UploadState.Retry();
            slot.RequestedAt = std::chrono::steady_clock::now();
            ++mUploadStatistics.Requests;
            (void)mPreparationQueue->TryEnqueue(slot.Preparation);
        }
        return count;
    }

    const nvrhi::BindingLayoutHandle& TextureManager::GetTextureBindingLayout() const noexcept
    {
        return mBindless.GetTextures().GetBindingLayout();
    }

    const nvrhi::DescriptorTableHandle& TextureManager::GetTextureDescriptorTable() const noexcept
    {
        return mBindless.GetTextures().GetDescriptorTable();
    }

    nvrhi::IBindingSet* TextureManager::GetTextureBindingSet() const noexcept
    {
        return mBindless.GetTextures().GetBindingSet();
    }

    const nvrhi::BindingLayoutHandle& TextureManager::GetSamplerBindingLayout() const noexcept
    {
        return mBindless.GetSamplers().GetBindingLayout();
    }

    const nvrhi::DescriptorTableHandle& TextureManager::GetSamplerDescriptorTable() const noexcept
    {
        return mBindless.GetSamplers().GetDescriptorTable();
    }

    nvrhi::IBindingSet* TextureManager::GetSamplerBindingSet() const noexcept
    {
        return mBindless.GetSamplers().GetBindingSet();
    }

    uint32_t TextureManager::GetMaterialSamplerIndex() const
    {
        return mBindless.GetSamplers().GetShaderDescriptorIndex(mMaterialSamplerHandle);
    }

    std::vector<nvrhi::TextureHandle> TextureManager::GetBindlessTextures() const
    {
        std::vector<nvrhi::TextureHandle> textures;
        textures.reserve(3 + mCache.size());
        for (const ManagedTexture& texture : mDefaultTextures)
            textures.push_back(texture.Texture);
        for (const Slot& slot : mSlots)
        {
            if (slot.References.GetReferenceCount() != 0 && slot.Resource.BindlessHandle)
                textures.push_back(slot.Resource.Texture);
        }
        return textures;
    }

    size_t TextureManager::GetResidentTextureCount() const noexcept
    {
        size_t count = 3;
        for (const Slot& slot : mSlots)
            count += slot.References.GetReferenceCount() != 0 ? 1u : 0u;
        return count;
    }

    uint64_t TextureManager::GetResidentTexturePayloadBytes() const noexcept
    {
        uint64_t bytes = 0;
        for (const ManagedTexture& texture : mDefaultTextures) bytes += texture.PayloadBytes;
        for (const Slot& slot : mSlots)
            if (slot.References.GetReferenceCount() != 0)
                bytes += slot.Resource.PayloadBytes + slot.PendingResource.PayloadBytes;
        for (const RetiredSlot& retired : mRetiredSlots) bytes += retired.PayloadBytes;
        return bytes;
    }

    TextureUploadStatistics TextureManager::GetUploadStatistics() const noexcept
    {
        TextureUploadStatistics stats = mUploadStatistics;
        stats.RecentCompletionLatency = mCompletionLatencyWindow.Get();
        if (mPreparationQueue)
        {
            stats.PreparedBytes = mPreparationQueue->GetPreparedBytes();
            const auto [budget, stalls] = mPreparationQueue->GetBudgetStatistics();
            stats.PreparedByteBudget = budget;
            stats.DecodeBudgetStalls = stalls;
            stats.OwnerClaims = mPreparationQueue->GetOwnerClaims();
        }
        for (const Slot& slot : mSlots)
        {
            if (slot.Preparation && slot.UploadState.Get() == TextureUploadStage::Preparing)
            {
                ++stats.Pending;
                if (slot.Preparation->bEnqueued) ++stats.Preparing;
                else ++stats.Deferred;
                std::lock_guard lock(slot.Preparation->Mutex);
                stats.PendingBytes += slot.Preparation->PreparedBytes;
            }
            if (slot.UploadCompletion)
            {
                ++stats.Pending;
                stats.PendingBytes += slot.PendingResource.PayloadBytes;
            }
        }
        for (const RetiredSlot& retired : mRetiredSlots)
            if (retired.UploadCompletion)
            {
                ++stats.Pending;
                stats.PendingBytes += retired.PayloadBytes;
            }
        return stats;
    }
}
