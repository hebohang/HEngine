#include "TextureDataGenerator.h"

#include <array>
#include <cmath>
#include <cstring>

namespace
{
LLGI::Color8 GetMipColor(int32_t mipLevel)
{
	const LLGI::Color8 colors[] = {
		LLGI::Color8(255, 64, 32, 255),
		LLGI::Color8(32, 255, 64, 255),
		LLGI::Color8(64, 128, 255, 255),
		LLGI::Color8(255, 255, 64, 255),
	};
	const auto colorCount = static_cast<int32_t>(sizeof(colors) / sizeof(colors[0]));
	return colors[mipLevel % colorCount];
}

uint16_t GetHalfValue(uint8_t value)
{
	if (value == 0)
	{
		return 0x0000;
	}
	if (value < 128)
	{
		return 0x3800; // 0.5
	}
	return 0x3C00; // 1.0
}

void WriteUint16(uint8_t* dst, uint16_t value)
{
	dst[0] = static_cast<uint8_t>(value & 0xff);
	dst[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void WriteUint32(uint8_t* dst, uint32_t value)
{
	dst[0] = static_cast<uint8_t>(value & 0xff);
	dst[1] = static_cast<uint8_t>((value >> 8) & 0xff);
	dst[2] = static_cast<uint8_t>((value >> 16) & 0xff);
	dst[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

void WriteFloat(uint8_t* dst, float value) { memcpy(dst, &value, sizeof(float)); }

uint32_t PackUnsignedFloat(float value, int32_t mantissaBits)
{
	if (value <= 0.0f)
	{
		return 0;
	}

	int32_t exponent = 0;
	const auto mantissa = std::frexp(value, &exponent) * 2.0f;
	int32_t biasedExponent = exponent + 14;
	const int32_t mantissaLimit = 1 << mantissaBits;
	int32_t packedMantissa = static_cast<int32_t>(std::round((mantissa - 1.0f) * mantissaLimit));

	if (packedMantissa == mantissaLimit)
	{
		packedMantissa = 0;
		biasedExponent++;
	}

	if (biasedExponent <= 0)
	{
		return 0;
	}

	if (biasedExponent >= 31)
	{
		biasedExponent = 30;
		packedMantissa = mantissaLimit - 1;
	}

	return static_cast<uint32_t>((biasedExponent << mantissaBits) | packedMantissa);
}

uint32_t PackRG11B10UFloat(uint8_t r, uint8_t g, uint8_t b)
{
	const auto rValue = PackUnsignedFloat(r / 255.0f, 6);
	const auto gValue = PackUnsignedFloat(g / 255.0f, 6);
	const auto bValue = PackUnsignedFloat(b / 255.0f, 5);
	return rValue | (gValue << 11) | (bValue << 22);
}

uint16_t ToRGB565(const LLGI::Color8& color)
{
	return static_cast<uint16_t>(((color.R >> 3) << 11) | ((color.G >> 2) << 5) | (color.B >> 3));
}

std::array<uint8_t, 8> CreateBC1Block(const LLGI::Color8& color)
{
	std::array<uint8_t, 8> block = {};
	const auto rgb565 = ToRGB565(color);
	WriteUint16(block.data(), rgb565);
	WriteUint16(block.data() + 2, rgb565);
	return block;
}

std::array<uint8_t, 16> CreateBC2Block(const LLGI::Color8& color)
{
	std::array<uint8_t, 16> block = {};
	for (int32_t i = 0; i < 8; i++)
	{
		block[i] = 0xff;
	}

	const auto colorBlock = CreateBC1Block(color);
	memcpy(block.data() + 8, colorBlock.data(), colorBlock.size());
	return block;
}

std::array<uint8_t, 16> CreateBC3Block(const LLGI::Color8& color)
{
	std::array<uint8_t, 16> block = {};
	block[0] = 0xff;
	block[1] = 0x00;

	const auto colorBlock = CreateBC1Block(color);
	memcpy(block.data() + 8, colorBlock.data(), colorBlock.size());
	return block;
}

std::array<uint8_t, 16> CreateBC7Block(int32_t mipLevel)
{
	const std::array<std::array<uint8_t, 16>, 4> blocks = {{
		{{0x01, 0xff, 0x7f, 0x18, 0xe3, 0x8f, 0x31, 0xc6, 0xff, 0x7f, 0x18, 0xe3, 0x8f, 0x31, 0xc6, 0x73}},
		{{0x01, 0x24, 0xff, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24, 0xff, 0x92, 0x49, 0x24, 0x92, 0x49, 0xaa}},
		{{0x01, 0x6d, 0xb6, 0xdb, 0xff, 0x6d, 0xb6, 0xdb, 0x6d, 0xb6, 0xff, 0xdb, 0x6d, 0xb6, 0xdb, 0x55}},
		{{0x01, 0xff, 0xff, 0x00, 0x7e, 0x81, 0x18, 0x3c, 0xc3, 0xff, 0x7e, 0x81, 0x18, 0x3c, 0xc3, 0x99}},
	}};
	return blocks[mipLevel % static_cast<int32_t>(blocks.size())];
}

std::vector<uint8_t> CreateCompressedBlock(LLGI::TextureFormatType format, int32_t mipLevel)
{
	const auto color = GetMipColor(mipLevel);

	if (format == LLGI::TextureFormatType::BC1 || format == LLGI::TextureFormatType::BC1_SRGB)
	{
		const auto block = CreateBC1Block(color);
		return std::vector<uint8_t>(block.begin(), block.end());
	}

	if (format == LLGI::TextureFormatType::BC2 || format == LLGI::TextureFormatType::BC2_SRGB)
	{
		const auto block = CreateBC2Block(color);
		return std::vector<uint8_t>(block.begin(), block.end());
	}

	if (format == LLGI::TextureFormatType::BC3 || format == LLGI::TextureFormatType::BC3_SRGB)
	{
		const auto block = CreateBC3Block(color);
		return std::vector<uint8_t>(block.begin(), block.end());
	}

	const auto block = CreateBC7Block(mipLevel);
	return std::vector<uint8_t>(block.begin(), block.end());
}

void FillCompressedTextureData(uint8_t* dst, size_t size, LLGI::TextureFormatType format, int32_t mipLevel)
{
	const auto block = CreateCompressedBlock(format, mipLevel);
	for (size_t offset = 0; offset < size; offset += block.size())
	{
		memcpy(dst + offset, block.data(), block.size());
	}
}

void FillTextureData(uint8_t* dst, LLGI::Vec3I size, LLGI::TextureFormatType format, int32_t mipLevel)
{
	const auto color = GetMipColor(mipLevel);

	for (int32_t z = 0; z < size.Z; z++)
	{
		for (int32_t y = 0; y < size.Y; y++)
		{
			for (int32_t x = 0; x < size.X; x++)
			{
				const auto checker = static_cast<uint8_t>((x % 16 > 8 || y % 16 > 8) ? 128 : 0);
				const auto r = static_cast<uint8_t>((static_cast<int32_t>(color.R) + x + z * 19) % 256);
				const auto g = static_cast<uint8_t>((static_cast<int32_t>(color.G) + y + z * 23) % 256);
				const auto b = static_cast<uint8_t>((color.B + z * 31) ^ checker);
				const auto pixelIndex = static_cast<size_t>(x + (y + z * size.Y) * size.X);

				if (format == LLGI::TextureFormatType::R8G8B8A8_UNORM || format == LLGI::TextureFormatType::R8G8B8A8_UNORM_SRGB)
				{
					auto pixel = dst + pixelIndex * 4;
					pixel[0] = r;
					pixel[1] = g;
					pixel[2] = b;
					pixel[3] = 255;
				}
				else if (format == LLGI::TextureFormatType::B8G8R8A8_UNORM || format == LLGI::TextureFormatType::B8G8R8A8_UNORM_SRGB)
				{
					auto pixel = dst + pixelIndex * 4;
					pixel[0] = b;
					pixel[1] = g;
					pixel[2] = r;
					pixel[3] = 255;
				}
				else if (format == LLGI::TextureFormatType::R8_UNORM)
				{
					dst[pixelIndex] = r;
				}
				else if (format == LLGI::TextureFormatType::RG11B10_UFLOAT)
				{
					WriteUint32(dst + pixelIndex * 4, PackRG11B10UFloat(r, g, b));
				}
				else if (format == LLGI::TextureFormatType::R16_FLOAT)
				{
					WriteUint16(dst + pixelIndex * 2, GetHalfValue(r));
				}
				else if (format == LLGI::TextureFormatType::R32_FLOAT)
				{
					WriteFloat(dst + pixelIndex * 4, r / 255.0f);
				}
				else if (format == LLGI::TextureFormatType::R16G16_FLOAT)
				{
					auto pixel = dst + pixelIndex * 4;
					WriteUint16(pixel, GetHalfValue(r));
					WriteUint16(pixel + 2, GetHalfValue(g));
				}
				else if (format == LLGI::TextureFormatType::R32G32_FLOAT)
				{
					auto pixel = dst + pixelIndex * 8;
					WriteFloat(pixel, r / 255.0f);
					WriteFloat(pixel + 4, g / 255.0f);
				}
				else if (format == LLGI::TextureFormatType::R16G16B16A16_FLOAT)
				{
					auto pixel = dst + pixelIndex * 8;
					WriteUint16(pixel, GetHalfValue(r));
					WriteUint16(pixel + 2, GetHalfValue(g));
					WriteUint16(pixel + 4, GetHalfValue(b));
					WriteUint16(pixel + 6, GetHalfValue(255));
				}
				else if (format == LLGI::TextureFormatType::R32G32B32A32_FLOAT)
				{
					auto pixel = dst + pixelIndex * 16;
					WriteFloat(pixel, r / 255.0f);
					WriteFloat(pixel + 4, g / 255.0f);
					WriteFloat(pixel + 8, b / 255.0f);
					WriteFloat(pixel + 12, 1.0f);
				}
			}
		}
	}
}
} // namespace

std::vector<uint8_t> TextureDataGenerator::CreateDummyTextureData(
	LLGI::Vec2I size,
	LLGI::TextureFormatType format,
	int32_t mipLevelCount)
{
	return CreateDummyTextureData(LLGI::Vec3I(size.X, size.Y, 1), format, mipLevelCount, false);
}

std::vector<uint8_t> TextureDataGenerator::CreateDummyTextureData(
	LLGI::Vec3I size,
	LLGI::TextureFormatType format,
	int32_t mipLevelCount,
	bool preserveDepth)
{
	std::vector<uint8_t> ret;

	for (int32_t mipLevel = 0; mipLevel < mipLevelCount; mipLevel++)
	{
		const auto mipSize = LLGI::GetTextureMipSize(size, mipLevel, preserveDepth);
		const auto mipDataSize = static_cast<size_t>(LLGI::GetTextureMemorySize(format, mipSize));
		const auto offset = ret.size();
		ret.resize(offset + mipDataSize);
		auto dst = ret.data() + offset;

		if (LLGI::IsBlockCompressedFormat(format))
		{
			FillCompressedTextureData(dst, mipDataSize, format, mipLevel);
		}
		else
		{
			FillTextureData(dst, mipSize, format, mipLevel);
		}
	}

	return ret;
}

void TextureDataGenerator::WriteDummyTexture(LLGI::Texture* texture)
{
	auto dummyData = CreateDummyTextureData(texture->GetSizeAs2D(), texture->GetFormat(), texture->GetMipmapCount());

	auto data = texture->Lock();
	memcpy(data, dummyData.data(), dummyData.size());
	texture->Unlock();
}

void TextureDataGenerator::WriteDummyTexture(LLGI::Color8* data, LLGI::Vec2I size)
{
	for (int32_t y = 0; y < size.Y; y++)
	{
		for (int32_t x = 0; x < size.X; x++)
		{
			const auto pixelIndex = x + y * size.X;
			data[pixelIndex].R = static_cast<uint8_t>(x % 256);
			data[pixelIndex].G = static_cast<uint8_t>(y % 256);
			data[pixelIndex].B = static_cast<uint8_t>((x % 16 > 8 || y % 16 > 8) ? 128 : 0);
			data[pixelIndex].A = 255;
		}
	}
}
