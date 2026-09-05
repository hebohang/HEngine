#include "Hepch.h"

#include "TextureReadback.h"
#include "Runtime/Utils/FileUtils/AtomicFile.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] uint32_t NextGeneration(uint32_t generation) noexcept
        {
            ++generation;
            return generation == 0 ? 1 : generation;
        }

        void AppendBigEndian(std::vector<uint8_t>& output, uint32_t value)
        {
            output.push_back(static_cast<uint8_t>(value >> 24u));
            output.push_back(static_cast<uint8_t>(value >> 16u));
            output.push_back(static_cast<uint8_t>(value >> 8u));
            output.push_back(static_cast<uint8_t>(value));
        }

        [[nodiscard]] uint32_t ComputeCrc32(const uint8_t* data, size_t size)
        {
            uint32_t crc = 0xffffffffu;
            for (size_t index = 0; index < size; ++index)
            {
                crc ^= data[index];
                for (uint32_t bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        }

        [[nodiscard]] uint32_t ComputeAdler32(const uint8_t* data, size_t size)
        {
            constexpr uint32_t modulus = 65521u;
            uint32_t first = 1;
            uint32_t second = 0;
            for (size_t offset = 0; offset < size;)
            {
                const size_t blockEnd = std::min(size, offset + 5552u);
                for (; offset < blockEnd; ++offset)
                {
                    first += data[offset];
                    second += first;
                }
                first %= modulus;
                second %= modulus;
            }
            return (second << 16u) | first;
        }

        void AppendChunk(
            std::vector<uint8_t>& output,
            const std::array<uint8_t, 4>& type,
            const std::vector<uint8_t>& data)
        {
            if (data.size() > std::numeric_limits<uint32_t>::max())
                throw std::overflow_error("PNG chunk exceeds the 32-bit length field");

            AppendBigEndian(output, static_cast<uint32_t>(data.size()));
            const size_t crcStart = output.size();
            output.insert(output.end(), type.begin(), type.end());
            output.insert(output.end(), data.begin(), data.end());
            AppendBigEndian(output, ComputeCrc32(output.data() + crcStart, output.size() - crcStart));
        }

        [[nodiscard]] bool IsRgba8(nvrhi::Format format) noexcept
        {
            return format == nvrhi::Format::RGBA8_UNORM ||
                format == nvrhi::Format::SRGBA8_UNORM;
        }

        [[nodiscard]] bool IsBgra8(nvrhi::Format format) noexcept
        {
            return format == nvrhi::Format::BGRA8_UNORM ||
                format == nvrhi::Format::SBGRA8_UNORM;
        }

        enum class ReadbackComponentType : uint8_t
        {
            Float16,
            Float32,
            Uint32,
        };

        struct ReadbackFormatLayout
        {
            uint32_t Channels = 0;
            uint32_t BytesPerComponent = 0;
            ReadbackComponentType ComponentType = ReadbackComponentType::Float32;
            const char* TypeName = "";
            const char* FormatName = "";
            const char* ChannelOrder = "";
        };

        [[nodiscard]] ReadbackFormatLayout GetReadbackFormatLayout(nvrhi::Format format)
        {
            switch (format)
            {
            case nvrhi::Format::R16_FLOAT:
                return { 1, 2, ReadbackComponentType::Float16,
                    "float16", "R16_FLOAT", "R" };
            case nvrhi::Format::RGBA16_FLOAT:
                return { 4, 2, ReadbackComponentType::Float16,
                    "float16", "RGBA16_FLOAT", "RGBA" };
            case nvrhi::Format::R32_FLOAT:
                return { 1, 4, ReadbackComponentType::Float32,
                    "float32", "R32_FLOAT", "R" };
            case nvrhi::Format::RGBA32_FLOAT:
                return { 4, 4, ReadbackComponentType::Float32,
                    "float32", "RGBA32_FLOAT", "RGBA" };
            case nvrhi::Format::R32_UINT:
                return { 1, 4, ReadbackComponentType::Uint32,
                    "uint32", "R32_UINT", "R" };
            default:
                throw std::invalid_argument(
                    "Texture readback format is not supported by the HDR/raw encoders");
            }
        }

        [[nodiscard]] bool IsFloatReadbackFormat(nvrhi::Format format) noexcept
        {
            return format == nvrhi::Format::R16_FLOAT ||
                format == nvrhi::Format::RGBA16_FLOAT ||
                format == nvrhi::Format::R32_FLOAT ||
                format == nvrhi::Format::RGBA32_FLOAT;
        }

        [[nodiscard]] bool IsRawReadbackFormat(nvrhi::Format format) noexcept
        {
            return IsFloatReadbackFormat(format) || format == nvrhi::Format::R32_UINT;
        }

        [[nodiscard]] size_t GetTightRowSize(
            uint32_t width,
            const ReadbackFormatLayout& layout)
        {
            const size_t bytesPerPixel =
                static_cast<size_t>(layout.Channels) * layout.BytesPerComponent;
            if (width > std::numeric_limits<size_t>::max() / bytesPerPixel)
                throw std::overflow_error("Texture readback row size exceeds addressable memory");
            return static_cast<size_t>(width) * bytesPerPixel;
        }

        [[nodiscard]] size_t GetSourceTightRowSize(
            uint32_t width,
            nvrhi::Format format)
        {
            if (IsRgba8(format) || IsBgra8(format))
            {
                if (width > std::numeric_limits<size_t>::max() / 4u)
                    throw std::overflow_error("Texture readback row size exceeds addressable memory");
                return static_cast<size_t>(width) * 4u;
            }
            return GetTightRowSize(width, GetReadbackFormatLayout(format));
        }

        void ValidateEncodingInput(
            uint32_t width,
            uint32_t height,
            const uint8_t* pixels,
            size_t rowPitch,
            const ReadbackFormatLayout& layout)
        {
            if (width == 0 || height == 0 || pixels == nullptr)
                throw std::invalid_argument("Texture readback encoding requires non-empty pixels");
            const size_t tightRowSize = GetTightRowSize(width, layout);
            if (rowPitch < tightRowSize)
                throw std::invalid_argument("Texture readback row pitch is smaller than a tight row");
            if (height > std::numeric_limits<size_t>::max() / rowPitch)
                throw std::overflow_error("Texture readback source layout exceeds addressable memory");
        }

        [[nodiscard]] uint16_t ReadLittleEndian16(const uint8_t* bytes) noexcept
        {
            return static_cast<uint16_t>(bytes[0]) |
                static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8u);
        }

        [[nodiscard]] uint32_t ReadLittleEndian32(const uint8_t* bytes) noexcept
        {
            return static_cast<uint32_t>(bytes[0]) |
                (static_cast<uint32_t>(bytes[1]) << 8u) |
                (static_cast<uint32_t>(bytes[2]) << 16u) |
                (static_cast<uint32_t>(bytes[3]) << 24u);
        }

        void AppendLittleEndian16(std::vector<uint8_t>& output, uint16_t value)
        {
            output.push_back(static_cast<uint8_t>(value));
            output.push_back(static_cast<uint8_t>(value >> 8u));
        }

        void AppendLittleEndian32(std::vector<uint8_t>& output, uint32_t value)
        {
            output.push_back(static_cast<uint8_t>(value));
            output.push_back(static_cast<uint8_t>(value >> 8u));
            output.push_back(static_cast<uint8_t>(value >> 16u));
            output.push_back(static_cast<uint8_t>(value >> 24u));
        }

        [[nodiscard]] float HalfToFloat(uint16_t value) noexcept
        {
            const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16u;
            const uint32_t halfExponent = (value >> 10u) & 0x1fu;
            uint32_t mantissa = value & 0x03ffu;
            uint32_t result = sign;
            if (halfExponent == 0)
            {
                if (mantissa != 0)
                {
                    int32_t exponent = -14;
                    while ((mantissa & 0x0400u) == 0)
                    {
                        mantissa <<= 1u;
                        --exponent;
                    }
                    mantissa &= 0x03ffu;
                    result |= static_cast<uint32_t>(exponent + 127) << 23u;
                    result |= mantissa << 13u;
                }
            }
            else if (halfExponent == 0x1fu)
            {
                result |= 0x7f800000u | (mantissa << 13u);
            }
            else
            {
                result |= (halfExponent + 112u) << 23u;
                result |= mantissa << 13u;
            }
            return std::bit_cast<float>(result);
        }

        [[nodiscard]] float ReadFloatComponent(
            const uint8_t* source,
            ReadbackComponentType componentType)
        {
            if (componentType == ReadbackComponentType::Float16)
                return HalfToFloat(ReadLittleEndian16(source));
            if (componentType == ReadbackComponentType::Float32)
                return std::bit_cast<float>(ReadLittleEndian32(source));
            throw std::invalid_argument("PFM cannot encode an unsigned integer texture");
        }

        void AppendRawComponent(
            std::vector<uint8_t>& output,
            const uint8_t* source,
            ReadbackComponentType componentType)
        {
            if (componentType == ReadbackComponentType::Float16)
                AppendLittleEndian16(output, ReadLittleEndian16(source));
            else
                AppendLittleEndian32(output, ReadLittleEndian32(source));
        }

        struct NonFiniteCounts
        {
            uint64_t NaN = 0;
            uint64_t PositiveInfinity = 0;
            uint64_t NegativeInfinity = 0;
        };

        void CountNonFinite(
            NonFiniteCounts& counts,
            const uint8_t* source,
            ReadbackComponentType componentType)
        {
            if (componentType == ReadbackComponentType::Uint32)
                return;

            bool bNegative = false;
            bool bInfinity = false;
            bool bNaN = false;
            if (componentType == ReadbackComponentType::Float16)
            {
                const uint16_t bits = ReadLittleEndian16(source);
                bNegative = (bits & 0x8000u) != 0;
                bInfinity = (bits & 0x7fffu) == 0x7c00u;
                bNaN = (bits & 0x7c00u) == 0x7c00u && (bits & 0x03ffu) != 0;
            }
            else
            {
                const uint32_t bits = ReadLittleEndian32(source);
                bNegative = (bits & 0x80000000u) != 0;
                bInfinity = (bits & 0x7fffffffu) == 0x7f800000u;
                bNaN = (bits & 0x7f800000u) == 0x7f800000u &&
                    (bits & 0x007fffffu) != 0;
            }

            if (bNaN)
                ++counts.NaN;
            else if (bInfinity && bNegative)
                ++counts.NegativeInfinity;
            else if (bInfinity)
                ++counts.PositiveInfinity;
        }

        [[nodiscard]] std::filesystem::path GetRawMetadataPath(
            const std::filesystem::path& outputPath)
        {
            std::filesystem::path metadataPath = outputPath;
            metadataPath += ".json";
            return metadataPath;
        }

        void WriteFile(
            const std::filesystem::path& outputPath,
            const std::vector<uint8_t>& bytes,
            bool bReplaceExisting)
        {
            if (outputPath.empty())
                throw std::invalid_argument("Texture readback output path is empty");

			std::error_code error;
			const std::filesystem::path parent = outputPath.parent_path();
            if (!parent.empty())
            {
                std::filesystem::create_directories(parent, error);
                if (error)
                    throw std::runtime_error("Could not create texture readback directory: " + error.message());
            }

			Utils::WriteBytesToFileAtomically(
				outputPath,
				bytes.data(),
				bytes.size(),
				bReplaceExisting
					? Utils::AtomicWriteMode::ReplaceExisting
					: Utils::AtomicWriteMode::CreateNew);
        }
    }

    TextureReadbackQueue::TextureReadbackQueue(nvrhi::IDevice* device)
        : mDevice(device)
    {
        if (mDevice == nullptr)
            throw std::invalid_argument("TextureReadbackQueue requires an NVRHI device");
    }

    TextureReadbackQueue::~TextureReadbackQueue()
    {
        try
        {
            Clear();
        }
        catch (const std::exception& exception)
        {
            HE_CORE_ERROR(
                "Texture readback queue cleanup failed during destruction: {0}",
                exception.what());
        }
        catch (...)
        {
            HE_CORE_ERROR(
                "Texture readback queue cleanup failed during destruction with an unknown error");
        }
    }

    TextureReadbackHandle TextureReadbackQueue::Record(
        nvrhi::ICommandList* commandList,
        nvrhi::ITexture* source,
        const TextureReadbackDesc& desc)
    {
		TextureReadbackHandle handle = Reserve(desc);
		try
		{
			RecordReserved(handle, commandList, source);
		}
		catch (...)
		{
			CancelPending(handle);
			throw;
		}
		return handle;
	}

	TextureReadbackHandle TextureReadbackQueue::Reserve(const TextureReadbackDesc& desc)
	{
		if (desc.OutputPath.empty())
			throw std::invalid_argument("Texture readback output path is empty");
		const TextureReadbackOutputFormat outputFormat =
			ResolveOutputFormat(desc.OutputPath, desc.OutputFormat);

		TextureReadbackHandle handle = Allocate();
		Slot& slot = mSlots[handle.Index];
		slot.RequestedSourceSlice = desc.SourceSlice;
		slot.OutputFormat = outputFormat;
		slot.AlphaMode = desc.AlphaMode;
		slot.NonFinitePolicy = desc.NonFinitePolicy;
		slot.Result = {
			.Status = TextureReadbackStatus::PendingSubmission,
			.OutputPath = desc.OutputPath,
			.MetadataPath = slot.OutputFormat == TextureReadbackOutputFormat::Raw
				? GetRawMetadataPath(desc.OutputPath)
				: std::filesystem::path{},
		};
		slot.bReplaceExisting = desc.bReplaceExisting;
		return handle;
	}

	void TextureReadbackQueue::RecordReserved(
		TextureReadbackHandle handle,
		nvrhi::ICommandList* commandList,
		nvrhi::ITexture* source)
	{
		if (commandList == nullptr || source == nullptr)
			throw std::invalid_argument("Texture readback requires a command list and source texture");

		Slot& slot = Require(handle);
		if (slot.Result.Status != TextureReadbackStatus::PendingSubmission || slot.bRecorded)
			throw std::logic_error("Texture readback reservation was already recorded or submitted");

        const nvrhi::TextureDesc& sourceDesc = source->getDesc();
        const bool bFormatSupported =
            (slot.OutputFormat == TextureReadbackOutputFormat::Png &&
                (IsRgba8(sourceDesc.format) || IsBgra8(sourceDesc.format))) ||
            (slot.OutputFormat == TextureReadbackOutputFormat::Pfm &&
                IsFloatReadbackFormat(sourceDesc.format)) ||
            (slot.OutputFormat == TextureReadbackOutputFormat::Raw &&
                IsRawReadbackFormat(sourceDesc.format));
        if (!bFormatSupported)
        {
            throw std::invalid_argument(
                "Texture readback source format is incompatible with the requested output format");
        }
        if (slot.OutputFormat == TextureReadbackOutputFormat::Pfm &&
            (sourceDesc.format == nvrhi::Format::RGBA16_FLOAT ||
                sourceDesc.format == nvrhi::Format::RGBA32_FLOAT) &&
            slot.AlphaMode != TextureReadbackAlphaMode::Discard)
        {
            throw std::invalid_argument(
                "RGBA PFM readback requires explicit alpha discard; use raw output to preserve alpha");
        }
        if (sourceDesc.sampleCount != 1)
            throw std::invalid_argument("Multisampled texture readback requires a RenderGraph resolve first");

        if (slot.RequestedSourceSlice.mipLevel >= sourceDesc.mipLevels ||
            slot.RequestedSourceSlice.arraySlice >= sourceDesc.arraySize)
        {
            throw std::out_of_range("Texture readback slice selects an invalid mip or array slice");
        }

		const nvrhi::TextureSlice resolved = slot.RequestedSourceSlice.resolve(sourceDesc);
        const uint32_t mipWidth = std::max(
            sourceDesc.width >> resolved.mipLevel, 1u);
        const uint32_t mipHeight = std::max(
            sourceDesc.height >> resolved.mipLevel, 1u);
        const uint32_t mipDepth = sourceDesc.dimension == nvrhi::TextureDimension::Texture3D
            ? std::max(sourceDesc.depth >> resolved.mipLevel, 1u)
            : 1u;
        if (resolved.x > mipWidth || resolved.width > mipWidth - resolved.x ||
            resolved.y > mipHeight || resolved.height > mipHeight - resolved.y ||
            resolved.z > mipDepth || resolved.depth > mipDepth - resolved.z)
        {
            throw std::out_of_range("Texture readback slice exceeds the selected subresource");
        }
        if (resolved.width == 0 || resolved.height == 0 || resolved.depth != 1)
            throw std::invalid_argument("Texture readback requires a non-empty 2D slice");

        // A staging texture describes copy layout only. Copying the imported
        // swapchain flags (RenderTarget + !ShaderResource) into this descriptor
        // produces an invalid D3D12 resource description and makes
        // GetCopyableFootprints return UINT64_MAX. Use one tightly scoped,
        // flag-free subresource and copy the requested source slice into it.
        nvrhi::TextureDesc stagingDesc;
        stagingDesc.width = resolved.width;
        stagingDesc.height = resolved.height;
        stagingDesc.depth = 1;
        stagingDesc.arraySize = 1;
        stagingDesc.mipLevels = 1;
        stagingDesc.sampleCount = 1;
        stagingDesc.format = sourceDesc.format;
        stagingDesc.dimension = nvrhi::TextureDimension::Texture2D;
        stagingDesc.debugName = "HEngine Texture Readback";
        nvrhi::StagingTextureHandle staging =
            mDevice->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        if (!staging)
            throw std::runtime_error("NVRHI failed to create a texture readback staging resource");

		slot.Width = resolved.width;
        slot.Height = resolved.height;
        slot.Format = sourceDesc.format;
        slot.StagingSlice = nvrhi::TextureSlice()
            .setSize(resolved.width, resolved.height, 1);
        slot.Staging = std::move(staging);
		commandList->copyTexture(slot.Staging, slot.StagingSlice, source, resolved);
		slot.bRecorded = true;
    }

    void TextureReadbackQueue::NotifySubmitted(TextureReadbackHandle handle, uint64_t submission)
    {
        if (submission == 0)
            throw std::invalid_argument("Texture readback submission must be non-zero");

		Slot& slot = Require(handle);
		if (slot.Result.Status != TextureReadbackStatus::PendingSubmission || !slot.bRecorded)
			throw std::logic_error("Texture readback was already submitted or completed");
        slot.Submission = submission;
        slot.Result.Status = TextureReadbackStatus::InFlight;
    }

    void TextureReadbackQueue::CancelPending(TextureReadbackHandle handle)
    {
        Slot& slot = Require(handle);
        if (slot.Result.Status != TextureReadbackStatus::PendingSubmission)
            throw std::logic_error("Only an unsubmitted texture readback can be cancelled");

        const uint32_t index = handle.Index;
        const uint32_t nextGeneration = NextGeneration(slot.Generation);
        if (!slot.bPendingCounted || mPendingCount == 0)
            throw std::logic_error("Texture readback pending count is inconsistent");
        --mPendingCount;
        slot = {};
        slot.Generation = nextGeneration;
        mFreeSlots.push_back(index);
    }

    void TextureReadbackQueue::Collect(uint64_t completedSubmission)
    {
		ProcessCompletions(completedSubmission, false);
    }

    void TextureReadbackQueue::Drain(uint64_t completedSubmission)
    {
		ProcessCompletions(completedSubmission, true);
    }

    void TextureReadbackQueue::ProcessCompletions(
        uint64_t completedSubmission,
        bool bWaitForCpuCompletion)
    {
		for (uint32_t index = 0; index < mSlots.size(); ++index)
		{
			Slot& slot = mSlots[index];
            if (!slot.bOccupied ||
                slot.Result.Status != TextureReadbackStatus::InFlight ||
                slot.Submission > completedSubmission)
            {
                continue;
            }

            if (!slot.Completion.valid())
            {
                BeginComplete(slot);
            }
            if (bWaitForCpuCompletion && slot.Completion.valid())
                slot.Completion.wait();
            if (!FinishComplete(slot))
                continue;

			if (slot.bPendingCounted)
			{
				if (mPendingCount == 0)
					throw std::logic_error("Texture readback pending count is inconsistent");
				--mPendingCount;
				slot.bPendingCounted = false;
			}
			if (slot.bReleaseWhenComplete)
			{
				const uint32_t nextGeneration = NextGeneration(slot.Generation);
				slot = {};
				slot.Generation = nextGeneration;
				mFreeSlots.push_back(index);
			}
		}
    }

    void TextureReadbackQueue::Release(TextureReadbackHandle handle)
    {
        Slot& slot = Require(handle);
		if (slot.Result.Status == TextureReadbackStatus::PendingSubmission)
		{
			CancelPending(handle);
			return;
		}
		if (slot.Result.Status == TextureReadbackStatus::InFlight)
		{
			slot.bReleaseWhenComplete = true;
			return;
		}

        const uint32_t index = handle.Index;
        const uint32_t nextGeneration = NextGeneration(slot.Generation);
        slot = {};
        slot.Generation = nextGeneration;
        mFreeSlots.push_back(index);
    }

    void TextureReadbackQueue::Clear()
    {
        const bool bHasGpuWork = std::ranges::any_of(mSlots, [](const Slot& slot)
        {
            return slot.bOccupied &&
                (slot.Result.Status == TextureReadbackStatus::InFlight ||
                    (slot.Result.Status == TextureReadbackStatus::PendingSubmission &&
                        slot.bRecorded));
        });
        if (bHasGpuWork && !mDevice->waitForIdle())
            HE_CORE_ERROR("Failed to wait for texture readback GPU work during queue cleanup");

        for (Slot& slot : mSlots)
        {
            if (!slot.bOccupied)
                continue;
            if (slot.Result.Status == TextureReadbackStatus::InFlight &&
                !slot.Completion.valid())
            {
                BeginComplete(slot);
            }
            if (slot.Completion.valid())
            {
                slot.Completion.wait();
                (void)slot.Completion.get();
            }
            const uint32_t nextGeneration = NextGeneration(slot.Generation);
            slot = {};
            slot.Generation = nextGeneration;
        }

        mFreeSlots.clear();
        mFreeSlots.reserve(mSlots.size());
        for (uint32_t index = 0; index < mSlots.size(); ++index)
            mFreeSlots.push_back(index);
        mPendingCount = 0;
    }

    bool TextureReadbackQueue::IsValid(TextureReadbackHandle handle) const noexcept
    {
        return handle &&
            handle.Index < mSlots.size() &&
            mSlots[handle.Index].bOccupied &&
            mSlots[handle.Index].Generation == handle.Generation;
    }

    TextureReadbackResult TextureReadbackQueue::GetResult(TextureReadbackHandle handle) const
    {
        return Require(handle).Result;
    }

    size_t TextureReadbackQueue::GetPendingCount() const noexcept
    {
        return mPendingCount;
    }

    std::vector<uint8_t> TextureReadbackQueue::EncodeRgba8Png(
        uint32_t width,
        uint32_t height,
        const uint8_t* pixels,
        size_t rowPitch,
        bool bSourceBgra)
    {
        if (width == 0 || height == 0 || pixels == nullptr)
            throw std::invalid_argument("PNG encoding requires non-empty RGBA8 pixels");
        const size_t tightRowSize = static_cast<size_t>(width) * 4u;
        if (rowPitch < tightRowSize)
            throw std::invalid_argument("PNG source row pitch is smaller than the RGBA8 row size");
        if (tightRowSize > std::numeric_limits<size_t>::max() - 1u ||
            static_cast<size_t>(height) >
                std::numeric_limits<size_t>::max() / (tightRowSize + 1u))
        {
            throw std::overflow_error("PNG dimensions exceed addressable memory");
        }

        std::vector<uint8_t> scanlines((tightRowSize + 1u) * static_cast<size_t>(height));
        for (uint32_t row = 0; row < height; ++row)
        {
            const uint8_t* sourceRow = pixels + static_cast<size_t>(row) * rowPitch;
            uint8_t* outputRow = scanlines.data() + static_cast<size_t>(row) * (tightRowSize + 1u);
            outputRow[0] = 0;
            if (!bSourceBgra)
            {
                std::memcpy(outputRow + 1, sourceRow, tightRowSize);
            }
            else
            {
                for (uint32_t column = 0; column < width; ++column)
                {
                    const uint8_t* sourcePixel = sourceRow + static_cast<size_t>(column) * 4u;
                    uint8_t* outputPixel = outputRow + 1u + static_cast<size_t>(column) * 4u;
                    outputPixel[0] = sourcePixel[2];
                    outputPixel[1] = sourcePixel[1];
                    outputPixel[2] = sourcePixel[0];
                    outputPixel[3] = sourcePixel[3];
                }
            }
        }

        std::vector<uint8_t> compressed;
        compressed.reserve(scanlines.size() + scanlines.size() / 65535u * 5u + 16u);
        compressed.push_back(0x78u);
        compressed.push_back(0x01u);
        for (size_t offset = 0; offset < scanlines.size();)
        {
            const uint16_t length = static_cast<uint16_t>(
                std::min<size_t>(65535u, scanlines.size() - offset));
            const bool bFinal = offset + length == scanlines.size();
            compressed.push_back(bFinal ? 0x01u : 0x00u);
            compressed.push_back(static_cast<uint8_t>(length));
            compressed.push_back(static_cast<uint8_t>(length >> 8u));
            const uint16_t inverseLength = static_cast<uint16_t>(~length);
            compressed.push_back(static_cast<uint8_t>(inverseLength));
            compressed.push_back(static_cast<uint8_t>(inverseLength >> 8u));
            compressed.insert(
                compressed.end(),
                scanlines.begin() + static_cast<std::ptrdiff_t>(offset),
                scanlines.begin() + static_cast<std::ptrdiff_t>(offset + length));
            offset += length;
        }
        AppendBigEndian(compressed, ComputeAdler32(scanlines.data(), scanlines.size()));

        std::vector<uint8_t> png = { 137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u };
        std::vector<uint8_t> header;
        header.reserve(13);
        AppendBigEndian(header, width);
        AppendBigEndian(header, height);
        header.insert(header.end(), { 8u, 6u, 0u, 0u, 0u });
        AppendChunk(png, { 'I', 'H', 'D', 'R' }, header);
        AppendChunk(png, { 'I', 'D', 'A', 'T' }, compressed);
        AppendChunk(png, { 'I', 'E', 'N', 'D' }, {});
        return png;
    }

    std::vector<uint8_t> TextureReadbackQueue::EncodePfm(
        uint32_t width,
        uint32_t height,
        nvrhi::Format sourceFormat,
        const uint8_t* pixels,
        size_t rowPitch,
        TextureReadbackAlphaMode alphaMode,
        TextureReadbackNonFinitePolicy nonFinitePolicy)
    {
        const ReadbackFormatLayout layout = GetReadbackFormatLayout(sourceFormat);
        if (layout.ComponentType == ReadbackComponentType::Uint32)
            throw std::invalid_argument("PFM output requires a floating-point source texture");
        if (layout.Channels == 4 && alphaMode != TextureReadbackAlphaMode::Discard)
        {
            throw std::invalid_argument(
                "RGBA PFM encoding requires explicit alpha discard");
        }
        ValidateEncodingInput(width, height, pixels, rowPitch, layout);

        const uint32_t outputChannels = layout.Channels == 1 ? 1u : 3u;
        const std::string header = std::string(outputChannels == 1 ? "Pf\n" : "PF\n") +
            std::to_string(width) + " " + std::to_string(height) + "\n-1.0\n";
        const size_t pixelCount = static_cast<size_t>(width) * height;
        if (pixelCount > (std::numeric_limits<size_t>::max() - header.size()) /
            (static_cast<size_t>(outputChannels) * sizeof(float)))
        {
            throw std::overflow_error("PFM output exceeds addressable memory");
        }

        std::vector<uint8_t> output;
        output.reserve(
            header.size() + pixelCount * outputChannels * sizeof(float));
        output.insert(output.end(), header.begin(), header.end());

        const size_t sourcePixelSize =
            static_cast<size_t>(layout.Channels) * layout.BytesPerComponent;
        for (uint32_t outputRow = 0; outputRow < height; ++outputRow)
        {
            const uint32_t sourceRowIndex = height - outputRow - 1u;
            const uint8_t* sourceRow =
                pixels + static_cast<size_t>(sourceRowIndex) * rowPitch;
            for (uint32_t column = 0; column < width; ++column)
            {
                const uint8_t* sourcePixel =
                    sourceRow + static_cast<size_t>(column) * sourcePixelSize;
                for (uint32_t channel = 0; channel < outputChannels; ++channel)
                {
                    const float value = ReadFloatComponent(
                        sourcePixel + static_cast<size_t>(channel) *
                            layout.BytesPerComponent,
                        layout.ComponentType);
                    if (!std::isfinite(value) &&
                        nonFinitePolicy == TextureReadbackNonFinitePolicy::Reject)
                    {
                        throw std::domain_error(
                            "PFM encoding rejected a non-finite source component");
                    }
                    AppendLittleEndian32(output, std::bit_cast<uint32_t>(value));
                }
            }
        }
        return output;
    }

    TextureReadbackRawEncoding TextureReadbackQueue::EncodeRaw(
        uint32_t width,
        uint32_t height,
        nvrhi::Format sourceFormat,
        const uint8_t* pixels,
        size_t rowPitch,
        TextureReadbackNonFinitePolicy nonFinitePolicy)
    {
        const ReadbackFormatLayout layout = GetReadbackFormatLayout(sourceFormat);
        ValidateEncodingInput(width, height, pixels, rowPitch, layout);
        const size_t tightRowSize = GetTightRowSize(width, layout);
        if (height > std::numeric_limits<size_t>::max() / tightRowSize)
            throw std::overflow_error("Raw texture readback exceeds addressable memory");

        TextureReadbackRawEncoding encoding;
        encoding.Pixels.reserve(tightRowSize * static_cast<size_t>(height));
        NonFiniteCounts nonFinite;
        const size_t sourcePixelSize =
            static_cast<size_t>(layout.Channels) * layout.BytesPerComponent;
        for (uint32_t row = 0; row < height; ++row)
        {
            const uint8_t* sourceRow = pixels + static_cast<size_t>(row) * rowPitch;
            for (uint32_t column = 0; column < width; ++column)
            {
                const uint8_t* sourcePixel =
                    sourceRow + static_cast<size_t>(column) * sourcePixelSize;
                for (uint32_t channel = 0; channel < layout.Channels; ++channel)
                {
                    const uint8_t* sourceComponent =
                        sourcePixel + static_cast<size_t>(channel) *
                            layout.BytesPerComponent;
                    AppendRawComponent(
                        encoding.Pixels, sourceComponent, layout.ComponentType);
                    CountNonFinite(nonFinite, sourceComponent, layout.ComponentType);
                }
            }
        }

        const uint64_t nonFiniteCount = nonFinite.NaN +
            nonFinite.PositiveInfinity + nonFinite.NegativeInfinity;
        if (nonFiniteCount != 0 &&
            nonFinitePolicy == TextureReadbackNonFinitePolicy::Reject)
        {
            throw std::domain_error(
                "Raw encoding rejected " + std::to_string(nonFiniteCount) +
                " non-finite source components");
        }

        const bool bLinearColor = layout.ComponentType != ReadbackComponentType::Uint32;
        std::ostringstream metadata;
        metadata << "{\n"
            << "  \"schema\": \"hengine.texture-readback.raw.v1\",\n"
            << "  \"width\": " << width << ",\n"
            << "  \"height\": " << height << ",\n"
            << "  \"channels\": " << layout.Channels << ",\n"
            << "  \"type\": \"" << layout.TypeName << "\",\n"
            << "  \"dtype\": \"" << layout.TypeName << "\",\n"
            << "  \"endianness\": \"little\",\n"
            << "  \"layout\": \"HWC\",\n"
            << "  \"rowOrder\": \"top-down\",\n"
            << "  \"row_order\": \"top-down\",\n"
            << "  \"channel_order\": \"" << layout.ChannelOrder << "\",\n"
            << "  \"colorSpace\": \"" << (bLinearColor ? "linear" : "data") << "\",\n"
            << "  \"color_space\": \"" << (bLinearColor ? "scene-linear" : "data") << "\",\n";
        if (bLinearColor)
            metadata << "  \"color_primaries\": \"Rec.709/sRGB\",\n";
        metadata << "  \"source_format\": \"" << layout.FormatName << "\",\n"
            << "  \"non_finite_policy\": \""
            << (nonFinitePolicy == TextureReadbackNonFinitePolicy::Preserve
                ? "preserve"
                : "reject") << "\",\n"
            << "  \"nan_count\": " << nonFinite.NaN << ",\n"
            << "  \"positive_infinity_count\": " << nonFinite.PositiveInfinity << ",\n"
            << "  \"negative_infinity_count\": " << nonFinite.NegativeInfinity << "\n"
            << "}\n";
        encoding.MetadataJson = metadata.str();
        return encoding;
    }

    TextureReadbackOutputFormat TextureReadbackQueue::ResolveOutputFormat(
        const std::filesystem::path& outputPath,
        TextureReadbackOutputFormat requestedFormat)
    {
        if (requestedFormat != TextureReadbackOutputFormat::Auto)
        {
            if (requestedFormat == TextureReadbackOutputFormat::Png ||
                requestedFormat == TextureReadbackOutputFormat::Pfm ||
                requestedFormat == TextureReadbackOutputFormat::Raw)
            {
                return requestedFormat;
            }
            throw std::invalid_argument("Unknown texture readback output format");
        }

        std::string extension = outputPath.extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (extension == ".png")
            return TextureReadbackOutputFormat::Png;
        if (extension == ".pfm")
            return TextureReadbackOutputFormat::Pfm;
        if (extension == ".raw" || extension == ".bin" ||
            extension == ".f16" || extension == ".f32")
        {
            return TextureReadbackOutputFormat::Raw;
        }
        throw std::invalid_argument(
            "Texture readback output extension must be .png, .pfm, .raw, .bin, .f16, or .f32, or an explicit format must be provided");
    }

    TextureReadbackQueue::Slot& TextureReadbackQueue::Require(TextureReadbackHandle handle)
    {
        if (!IsValid(handle))
            throw std::out_of_range("Invalid or stale texture readback handle");
        return mSlots[handle.Index];
    }

    const TextureReadbackQueue::Slot& TextureReadbackQueue::Require(TextureReadbackHandle handle) const
    {
        if (!IsValid(handle))
            throw std::out_of_range("Invalid or stale texture readback handle");
        return mSlots[handle.Index];
    }

    TextureReadbackHandle TextureReadbackQueue::Allocate()
    {
        uint32_t index = 0;
        if (mFreeSlots.empty())
        {
            index = static_cast<uint32_t>(mSlots.size());
            mSlots.emplace_back();
        }
        else
        {
            index = mFreeSlots.back();
            mFreeSlots.pop_back();
        }

        Slot& slot = mSlots[index];
        if (slot.bOccupied)
            throw std::logic_error("Texture readback queue reused an occupied slot");
        slot.bOccupied = true;
        slot.bPendingCounted = true;
        ++mPendingCount;
        return { index, slot.Generation };
    }

    void TextureReadbackQueue::BeginComplete(Slot& slot)
    {
        void* mapped = nullptr;
        try
        {
            size_t rowPitch = 0;
            mapped = mDevice->mapStagingTexture(
                slot.Staging,
                slot.StagingSlice,
                nvrhi::CpuAccessMode::Read,
                &rowPitch);
            if (mapped == nullptr)
                throw std::runtime_error("NVRHI failed to map a completed texture readback");

            const size_t tightRowSize = GetSourceTightRowSize(slot.Width, slot.Format);
            if (rowPitch < tightRowSize)
            {
                throw std::runtime_error(
                    "NVRHI returned a texture readback row pitch smaller than a tight row");
            }
            if (slot.Height > std::numeric_limits<size_t>::max() / tightRowSize)
                throw std::overflow_error("Texture readback CPU copy is too large");
            std::vector<uint8_t> pixels(
                tightRowSize * static_cast<size_t>(slot.Height));
            for (uint32_t row = 0; row < slot.Height; ++row)
            {
                std::memcpy(
                    pixels.data() + static_cast<size_t>(row) * tightRowSize,
                    static_cast<const uint8_t*>(mapped) +
                        static_cast<size_t>(row) * rowPitch,
                    tightRowSize);
            }
            mDevice->unmapStagingTexture(slot.Staging);
            mapped = nullptr;
            slot.Staging = nullptr;

            const uint32_t width = slot.Width;
            const uint32_t height = slot.Height;
            const nvrhi::Format sourceFormat = slot.Format;
            const TextureReadbackOutputFormat outputFormat = slot.OutputFormat;
            const TextureReadbackAlphaMode alphaMode = slot.AlphaMode;
            const TextureReadbackNonFinitePolicy nonFinitePolicy =
                slot.NonFinitePolicy;
            const bool bBgra = IsBgra8(slot.Format);
            const std::filesystem::path outputPath = slot.Result.OutputPath;
            const std::filesystem::path metadataPath = slot.Result.MetadataPath;
            const bool bReplaceExisting = slot.bReplaceExisting;
            slot.Completion = std::async(
                std::launch::async,
                [pixels = std::move(pixels),
                    width,
                    height,
                    rowPitch = tightRowSize,
                    sourceFormat,
                    outputFormat,
                    alphaMode,
                    nonFinitePolicy,
                    bBgra,
                    outputPath,
                    metadataPath,
                    bReplaceExisting]() mutable -> AsyncCompletion
                {
                    try
                    {
                        if (outputFormat == TextureReadbackOutputFormat::Png)
                        {
                            const std::vector<uint8_t> png = EncodeRgba8Png(
                                width,
                                height,
                                pixels.data(),
                                rowPitch,
                                bBgra);
                            WriteFile(outputPath, png, bReplaceExisting);
                        }
                        else if (outputFormat == TextureReadbackOutputFormat::Pfm)
                        {
                            const std::vector<uint8_t> pfm = EncodePfm(
                                width,
                                height,
                                sourceFormat,
                                pixels.data(),
                                rowPitch,
                                alphaMode,
                                nonFinitePolicy);
                            WriteFile(outputPath, pfm, bReplaceExisting);
                        }
                        else if (outputFormat == TextureReadbackOutputFormat::Raw)
                        {
                            TextureReadbackRawEncoding raw = EncodeRaw(
                                width,
                                height,
                                sourceFormat,
                                pixels.data(),
                                rowPitch,
                                nonFinitePolicy);
                            if (!bReplaceExisting &&
                                (std::filesystem::exists(outputPath) ||
                                    std::filesystem::exists(metadataPath)))
                            {
                                throw std::runtime_error(
                                    "Texture readback raw output or metadata already exists");
                            }
                            WriteFile(outputPath, raw.Pixels, bReplaceExisting);
                            const std::vector<uint8_t> metadata(
                                raw.MetadataJson.begin(), raw.MetadataJson.end());
                            WriteFile(metadataPath, metadata, bReplaceExisting);
                        }
                        else
                        {
                            throw std::logic_error(
                                "Texture readback resolved to an invalid output format");
                        }
                        return { .bSucceeded = true };
                    }
                    catch (const std::exception& error)
                    {
                        return {
                            .bSucceeded = false,
                            .Error = error.what(),
                        };
                    }
                });
        }
        catch (const std::exception& error)
        {
            if (mapped != nullptr)
                mDevice->unmapStagingTexture(slot.Staging);
            slot.Result.Status = TextureReadbackStatus::Failed;
            slot.Result.Error = error.what();
            slot.Staging = nullptr;
        }
    }

    bool TextureReadbackQueue::FinishComplete(Slot& slot)
    {
        if (slot.Result.Status == TextureReadbackStatus::Failed ||
            slot.Result.Status == TextureReadbackStatus::Complete)
        {
            return true;
        }
        if (!slot.Completion.valid() ||
            slot.Completion.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready)
        {
            return false;
        }
        AsyncCompletion completion = slot.Completion.get();
        slot.Result.Status = completion.bSucceeded
            ? TextureReadbackStatus::Complete
            : TextureReadbackStatus::Failed;
        slot.Result.Error = std::move(completion.Error);
        return true;
    }
}
