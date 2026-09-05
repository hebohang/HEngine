#include "TextureRenderTestHelper.h"
#include "TextureDataGenerator.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
bool IsSameColor(const uint8_t* pixel, LLGI::Color8 color)
{
	return pixel[0] == color.R && pixel[1] == color.G && pixel[2] == color.B && pixel[3] == color.A;
}

bool IsMeaningfullyDifferent(const uint8_t* lhs, LLGI::Color8 rhs)
{
	const int diff = std::abs(static_cast<int>(lhs[0]) - static_cast<int>(rhs.R)) +
					 std::abs(static_cast<int>(lhs[1]) - static_cast<int>(rhs.G)) +
					 std::abs(static_cast<int>(lhs[2]) - static_cast<int>(rhs.B)) +
					 std::abs(static_cast<int>(lhs[3]) - static_cast<int>(rhs.A));
	return diff > 12;
}

void VerifyPixelIsClear(const TexturedRectangleRenderTargetResult& result, int32_t x, int32_t y, LLGI::Color8 clearColor)
{
	const auto* pixel = result.Data.data() + static_cast<size_t>(x + y * result.Size.X) * 4;
	if (!IsSameColor(pixel, clearColor))
	{
		std::cout << "Pixel mismatch at " << x << "," << y << " actual=" << static_cast<int>(pixel[0]) << ","
				  << static_cast<int>(pixel[1]) << "," << static_cast<int>(pixel[2]) << "," << static_cast<int>(pixel[3])
				  << " expected=" << static_cast<int>(clearColor.R) << "," << static_cast<int>(clearColor.G) << ","
				  << static_cast<int>(clearColor.B) << "," << static_cast<int>(clearColor.A) << std::endl;
	}
	VERIFY(IsSameColor(pixel, clearColor));
}

void VerifyPixelIsDrawn(const TexturedRectangleRenderTargetResult& result, int32_t x, int32_t y, LLGI::Color8 clearColor)
{
	const auto* pixel = result.Data.data() + static_cast<size_t>(x + y * result.Size.X) * 4;
	if (!IsMeaningfullyDifferent(pixel, clearColor))
	{
		std::cout << "Drawn pixel was too close to clear color at " << x << "," << y << " actual=" << static_cast<int>(pixel[0])
				  << "," << static_cast<int>(pixel[1]) << "," << static_cast<int>(pixel[2]) << "," << static_cast<int>(pixel[3])
				  << " clear=" << static_cast<int>(clearColor.R) << "," << static_cast<int>(clearColor.G) << ","
				  << static_cast<int>(clearColor.B) << "," << static_cast<int>(clearColor.A) << std::endl;
	}
	VERIFY(IsMeaningfullyDifferent(pixel, clearColor));
	VERIFY(pixel[3] == 255);
}
} // namespace

bool IsTextureFormatRenderTestSupported(LLGI::DeviceType deviceType, LLGI::TextureFormatType format)
{
	if (deviceType == LLGI::DeviceType::DirectX12 || deviceType == LLGI::DeviceType::Default)
	{
		return format != LLGI::TextureFormatType::B8G8R8A8_UNORM && format != LLGI::TextureFormatType::B8G8R8A8_UNORM_SRGB;
	}

	if (deviceType == LLGI::DeviceType::Metal)
	{
		return format != LLGI::TextureFormatType::B8G8R8A8_UNORM_SRGB;
	}

	if (deviceType == LLGI::DeviceType::Vulkan)
	{
		return true;
	}

	if (deviceType == LLGI::DeviceType::WebGPU)
	{
		if (LLGI::IsBlockCompressedFormat(format))
		{
			return false;
		}

		return format != LLGI::TextureFormatType::R16_FLOAT && format != LLGI::TextureFormatType::R32_FLOAT &&
			   format != LLGI::TextureFormatType::R32G32_FLOAT;
	}

	return true;
}

LLGI::Texture* CreateRawDataTexture(LLGI::Graphics* graphics, const TextureFormatRenderTestCase& testCase)
{
	const auto textureSize = testCase.MipLevelCount > 1 ? 16 : 64;
	const auto arrayCount = testCase.IsArray ? testCase.ArrayCount : 1;

	LLGI::TextureParameter texParam;
	texParam.Dimension = 2;
	texParam.Format = testCase.Format;
	texParam.MipLevelCount = testCase.MipLevelCount;
	texParam.SampleCount = 1;
	texParam.Size = {textureSize, textureSize, arrayCount};
	texParam.Usage = testCase.IsArray ? LLGI::TextureUsageType::Array : LLGI::TextureUsageType::NoneFlag;
	texParam.IsMipmapGenerationEnabled = false;

	auto texture = graphics->CreateTexture(texParam);
	VERIFY(texture != nullptr);
	VERIFY(texture->GetMipmapCount() == testCase.MipLevelCount);

	const auto textureData =
		TextureDataGenerator::CreateDummyTextureData(texParam.Size, testCase.Format, testCase.MipLevelCount, testCase.IsArray);
	const auto expectedSize =
		static_cast<size_t>(LLGI::GetTextureMemorySize(testCase.Format, texParam.Size, testCase.MipLevelCount, testCase.IsArray));
	VERIFY(textureData.size() == expectedSize);
	auto data = texture->Lock();
	VERIFY(data != nullptr);
	memcpy(data, textureData.data(), textureData.size());
	texture->Unlock();

	return texture;
}

void RunTextureFormatRawDataScreenRenderTest(LLGI::DeviceType deviceType, const TextureFormatRenderTestCase& testCase)
{
	if (!IsTextureFormatRenderTestSupported(deviceType, testCase.Format))
	{
		return;
	}

	TexturedRectangleRenderOptions options;
	options.WindowTitle = testCase.Name;
	options.CaptureName = std::string("SimpleRender.") + testCase.Name;
	options.MaxFrameCount = 3;
	options.CaptureFrameIndex = 0;
	options.DrawSmallRectangle = testCase.MipLevelCount > 1;

	RenderTexturedRectangleTest(deviceType, options, [&testCase](LLGI::Graphics* graphics) { return CreateRawDataTexture(graphics, testCase); });
}

void RunTextureFormatRawDataOffscreenRenderTest(LLGI::DeviceType deviceType, const TextureFormatRenderTestCase& testCase)
{
	if (!IsTextureFormatRenderTestSupported(deviceType, testCase.Format))
	{
		return;
	}

	TexturedRectangleRenderTargetOptions options;
	options.ClearColor = LLGI::Color8(8, 16, 24, 255);
	options.DrawSmallRectangle = testCase.MipLevelCount > 1;

	const auto result =
		RenderTexturedRectangleToRenderTarget(deviceType, options, [&testCase](LLGI::Graphics* graphics) { return CreateRawDataTexture(graphics, testCase); });
	VERIFY(result.Format == LLGI::TextureFormatType::R8G8B8A8_UNORM);
	VERIFY(result.Size.X == options.RenderTextureSize.X);
	VERIFY(result.Size.Y == options.RenderTextureSize.Y);
	VERIFY(result.Data.size() == static_cast<size_t>(result.Size.X * result.Size.Y * 4));

	VerifyPixelIsClear(result, 2, 2, options.ClearColor);
	VerifyPixelIsDrawn(result, result.Size.X / 2, result.Size.Y / 2, options.ClearColor);

	if (options.DrawSmallRectangle)
	{
		VerifyPixelIsDrawn(result, result.Size.X * 82 / 100, result.Size.Y * 45 / 100, options.ClearColor);
	}
}
