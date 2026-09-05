#include "Hepch.h"

#include "TextureManager.h"
#include "Runtime/Graphics/RHI/TextureAlphaDilation.h"

#include "Runtime/Graphics/Image.h"
#include "Runtime/Graphics/ResourceFormat.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <stb_image_resize2.h>

namespace HEngine
{
    namespace
    {
        constexpr uint32_t c_TextureChannelCount = 4;
        constexpr float c_MaxFiniteHalf = 65504.0f;

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
    }

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
			(ShouldGenerateMipmaps(desc) ? "#fullmips" : "#mip0") +
			"#maxdim=" + std::to_string(desc.MaximumDimension);
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
            std::array<uint8_t, 4> Pixel;
            const char* Name;
        };
        constexpr std::array<DefaultUpload, 3> uploads = {{
            { DefaultBindlessTexture::White, nvrhi::Format::SRGBA8_UNORM,
                { 255, 255, 255, 255 }, "TextureManager Default White" },
            { DefaultBindlessTexture::Black, nvrhi::Format::SRGBA8_UNORM,
                { 0, 0, 0, 255 }, "TextureManager Default Black" },
            { DefaultBindlessTexture::Normal, nvrhi::Format::RGBA8_UNORM,
                { 128, 128, 255, 255 }, "TextureManager Default Normal" },
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
                upload.Pixel.size(),
                upload.Pixel.size());
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

    ManagedTextureHandle TextureManager::LoadTexture(const TextureLoadDesc& desc)
    {
        if (desc.bPreserveAlphaCoverage && !std::isfinite(desc.AlphaCoverageCutoff))
            throw std::invalid_argument("Alpha coverage cutoff must be finite");
        const float alphaCoverageCutoff = desc.bPreserveAlphaCoverage
            ? std::clamp(desc.AlphaCoverageCutoff, 0.0f, 1.0f)
            : 0.5f;
        const std::filesystem::path resolvedPath = ResolveAssetPath(
			desc.AssetPath,
			desc.bAllowAbsolutePath);
        const std::string cacheKey = MakeCacheKey(desc, resolvedPath);
        if (const auto existing = mCache.find(cacheKey); existing != mCache.end())
        {
            Slot& slot = mSlots.at(existing->second.Index);
            return slot.References.Acquire(existing->second.Index);
        }
        if (const auto failure = mFailedLoads.find(cacheKey); failure != mFailedLoads.end())
            throw std::runtime_error(failure->second);

        Image image(resolvedPath, desc.bFlipVertically);
        const std::span<const std::byte> imageData = image.GetData();
        if (imageData.empty() || image.GetWidth() == 0 || image.GetHeight() == 0)
        {
            const std::string error = "Failed to decode texture asset: " +
                Utils::PathToUtf8(resolvedPath);
            mFailedLoads.emplace(cacheKey, error);
            throw std::runtime_error(error);
        }

        std::vector<float> sanitizedFloatPixels;
		std::vector<float> resizedFloatPixels;
		std::vector<std::byte> resizedBytePixels;
		std::vector<std::byte> alphaDilatedPixels;
        std::span<const std::byte> uploadImageData = imageData;
		const ResizedTextureDimensions uploadDimensions = ResolveTextureDimensions(
			image.GetWidth(),
			image.GetHeight(),
			desc.MaximumDimension);
		const bool bResizeImage = uploadDimensions.Width != image.GetWidth() ||
			uploadDimensions.Height != image.GetHeight();
        if (image.GetFormat() == EResourceFormat::R32G32B32A32_FLOAT)
        {
            sanitizedFloatPixels = DecodeRgba32Float(
                imageData,
                image.GetWidth(),
                image.GetHeight());
            uploadImageData = std::as_bytes(std::span<const float>(
                sanitizedFloatPixels.data(),
                sanitizedFloatPixels.size()));
			if (bResizeImage)
			{
				resizedFloatPixels.resize(
					static_cast<size_t>(uploadDimensions.Width) *
					uploadDimensions.Height * c_TextureChannelCount);
				if (!stbir_resize_float_linear(
					sanitizedFloatPixels.data(),
					static_cast<int>(image.GetWidth()),
					static_cast<int>(image.GetHeight()),
					0,
					resizedFloatPixels.data(),
					static_cast<int>(uploadDimensions.Width),
					static_cast<int>(uploadDimensions.Height),
					0,
					STBIR_RGBA))
				{
					throw std::runtime_error("Failed to resize texture asset: " + desc.AssetPath);
				}
				uploadImageData = std::as_bytes(std::span<const float>(
					resizedFloatPixels.data(),
					resizedFloatPixels.size()));
			}
        }
		else
		{
			if (bResizeImage)
			{
				resizedBytePixels.resize(
					static_cast<size_t>(uploadDimensions.Width) *
					uploadDimensions.Height * c_TextureChannelCount);
				const auto* sourcePixels = reinterpret_cast<const uint8_t*>(imageData.data());
				auto* destinationPixels = reinterpret_cast<uint8_t*>(resizedBytePixels.data());
				const bool bResizeSucceeded = desc.ColorSpace == TextureColorSpace::SRgb
					? stbir_resize_uint8_srgb(
						sourcePixels,
						static_cast<int>(image.GetWidth()),
						static_cast<int>(image.GetHeight()),
						0,
						destinationPixels,
						static_cast<int>(uploadDimensions.Width),
						static_cast<int>(uploadDimensions.Height),
						0,
						STBIR_RGBA) != nullptr
					: stbir_resize_uint8_linear(
						sourcePixels,
						static_cast<int>(image.GetWidth()),
						static_cast<int>(image.GetHeight()),
						0,
						destinationPixels,
						static_cast<int>(uploadDimensions.Width),
						static_cast<int>(uploadDimensions.Height),
						0,
						STBIR_RGBA) != nullptr;
				if (!bResizeSucceeded)
					throw std::runtime_error("Failed to resize texture asset: " + desc.AssetPath);
				uploadImageData = resizedBytePixels;
			}

			if (ShouldPreserveAlphaCoverage(desc))
			{
				TextureAlphaDilationResult dilation = DilateMaskedTextureRgb(
					uploadImageData,
					uploadDimensions.Width,
					uploadDimensions.Height,
					alphaCoverageCutoff);
				if (dilation.WasApplied())
				{
					alphaDilatedPixels = std::move(dilation.Pixels);
					uploadImageData = alphaDilatedPixels;
				}
			}
		}

        nvrhi::TextureDesc textureDesc;
		textureDesc.width = uploadDimensions.Width;
		textureDesc.height = uploadDimensions.Height;
        textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
        textureDesc.isShaderResource = true;
        textureDesc.initialState = nvrhi::ResourceStates::Common;
        textureDesc.keepInitialState = false;
        textureDesc.mipLevels = ShouldGenerateMipmaps(desc)
            ? CalculateMipLevelCount(textureDesc.width, textureDesc.height)
            : 1u;
        if (image.GetFormat() == EResourceFormat::R32G32B32A32_FLOAT)
        {
            textureDesc.format = nvrhi::Format::RGBA32_FLOAT;
        }
        else
        {
            textureDesc.format = desc.ColorSpace == TextureColorSpace::SRgb
                ? nvrhi::Format::SRGBA8_UNORM
                : nvrhi::Format::RGBA8_UNORM;
        }
        textureDesc.debugName = desc.DebugName.empty()
            ? "TextureManager " + desc.AssetPath
            : desc.DebugName;

        ManagedTexture resource;
        resource.Texture = mDevice->createTexture(textureDesc);
        if (!resource.Texture)
            throw std::runtime_error("NVRHI failed to create texture asset: " + desc.AssetPath);
        resource.Width = textureDesc.width;
        resource.Height = textureDesc.height;
        resource.Format = textureDesc.format;
        resource.AssetPath = desc.AssetPath;

        std::vector<TextureMipUpload> mipChain;
        if (textureDesc.mipLevels > 1)
        {
            mipChain = GenerateMipChain(
                uploadImageData,
                textureDesc.width,
                textureDesc.height,
                image.GetFormat(),
                desc.ColorSpace,
                ShouldPreserveAlphaCoverage(desc),
                alphaCoverageCutoff);
            if (mipChain.size() + 1u != textureDesc.mipLevels)
                throw std::logic_error("Generated texture mip count does not match its resource");
        }

        nvrhi::CommandListHandle commandList = mDevice->createCommandList();
        if (!commandList)
            throw std::runtime_error("NVRHI failed to create texture upload command list");
        commandList->open();
        commandList->beginTrackingTextureState(
            resource.Texture,
            nvrhi::AllSubresources,
            nvrhi::ResourceStates::Common);
        const size_t pixelSize = textureDesc.format == nvrhi::Format::RGBA32_FLOAT ? 16u : 4u;
        const size_t rowPitch = static_cast<size_t>(textureDesc.width) * pixelSize;
        commandList->writeTexture(
            resource.Texture,
            0,
            0,
            uploadImageData.data(),
            rowPitch,
            rowPitch * textureDesc.height);

        for (uint32_t mipLevel = 1; mipLevel < textureDesc.mipLevels; ++mipLevel)
        {
            const TextureMipUpload& mip = mipChain[mipLevel - 1u];
            const size_t mipRowPitch = static_cast<size_t>(mip.Width) * pixelSize;
            commandList->writeTexture(
                resource.Texture,
                0,
                mipLevel,
                mip.Pixels.data(),
                mipRowPitch,
                mipRowPitch * mip.Height);
        }
		commandList->setPermanentTextureState(
			resource.Texture,
			nvrhi::ResourceStates::ShaderResource);
        commandList->commitBarriers();
        commandList->close();
        mDevice->executeCommandList(commandList, nvrhi::CommandQueue::Graphics);
        if (!mDevice->waitForIdle())
            throw std::runtime_error("GPU wait failed while uploading texture asset: " + desc.AssetPath);

        if (desc.bCreateBindlessDescriptor)
        {
            resource.BindlessHandle = mBindless.GetTextures().AllocateTexture(resource.Texture);
            resource.ShaderDescriptorIndex =
                mBindless.GetTextures().GetShaderDescriptorIndex(resource.BindlessHandle);
        }

        const uint32_t index = AllocateSlot();
        Slot& slot = mSlots[index];
        slot.CacheKey = cacheKey;
        slot.Resource = std::move(resource);
        const ManagedTextureHandle handle = slot.References.Acquire(index);
        mCache.emplace(cacheKey, handle);
        ++mRevision;
        return handle;
    }

    void TextureManager::ReleaseTexture(
        ManagedTextureHandle handle,
        uint64_t retireAfterSubmission)
    {
        if (!IsValid(handle))
            throw std::out_of_range("Invalid or stale managed texture handle");
        Slot& slot = mSlots[handle.Index];
        if (!slot.References.Release(handle))
            return;
        mCache.erase(slot.CacheKey);
        if (slot.Resource.BindlessHandle)
        {
            mBindless.GetTextures().ReleaseTexture(
                slot.Resource.BindlessHandle,
                retireAfterSubmission);
        }
        nvrhi::TextureHandle retainedTexture = std::move(slot.Resource.Texture);
        slot.Resource = {};
        slot.CacheKey.clear();
        if (retireAfterSubmission == 0)
            mFreeSlots.push_back(handle.Index);
        else
            mRetiredSlots.push_back({ handle.Index, retireAfterSubmission, retainedTexture });
        ++mRevision;
    }

    void TextureManager::Collect(uint64_t completedSubmission)
    {
        mBindless.Collect(completedSubmission);
        for (size_t index = 0; index < mRetiredSlots.size();)
        {
            if (mRetiredSlots[index].Submission > completedSubmission)
            {
                ++index;
                continue;
            }
            mFreeSlots.push_back(mRetiredSlots[index].Index);
            if (index + 1 != mRetiredSlots.size())
                mRetiredSlots[index] = std::move(mRetiredSlots.back());
            mRetiredSlots.pop_back();
        }
    }

    bool TextureManager::IsValid(ManagedTextureHandle handle) const noexcept
    {
        return handle && handle.Index < mSlots.size() &&
            mSlots[handle.Index].References.IsValid(handle);
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
        if (assetPath.empty())
        {
            const size_t count = mFailedLoads.size();
            mFailedLoads.clear();
            return count;
        }
        std::filesystem::path resolved = ResolveAssetPath(assetPath);
        const std::string prefix = Utils::PathToUtf8(resolved) + '#';
        return std::erase_if(mFailedLoads, [&prefix](const auto& entry)
        {
            return entry.first.starts_with(prefix);
        });
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
}
