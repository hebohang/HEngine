#pragma once

#include "RenderTestHelper.h"

struct TextureFormatRenderTestCase
{
	TextureFormatRenderTestCase(
		const char* name,
		LLGI::TextureFormatType format,
		int32_t mipLevelCount,
		bool isArray = false,
		int32_t arrayCount = 1)
		: Name(name)
		, Format(format)
		, MipLevelCount(mipLevelCount)
		, IsArray(isArray)
		, ArrayCount(arrayCount)
	{
	}

	const char* Name = "";
	LLGI::TextureFormatType Format = LLGI::TextureFormatType::Unknown;
	int32_t MipLevelCount = 1;
	bool IsArray = false;
	int32_t ArrayCount = 1;
};

bool IsTextureFormatRenderTestSupported(LLGI::DeviceType deviceType, LLGI::TextureFormatType format);

LLGI::Texture* CreateRawDataTexture(LLGI::Graphics* graphics, const TextureFormatRenderTestCase& testCase);

void RunTextureFormatRawDataScreenRenderTest(LLGI::DeviceType deviceType, const TextureFormatRenderTestCase& testCase);

void RunTextureFormatRawDataOffscreenRenderTest(LLGI::DeviceType deviceType, const TextureFormatRenderTestCase& testCase);
