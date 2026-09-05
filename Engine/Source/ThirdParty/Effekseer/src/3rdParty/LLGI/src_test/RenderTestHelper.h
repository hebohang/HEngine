#pragma once

#include "TestHelper.h"
#include "test.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TexturedRectangleRenderOptions
{
	const char* WindowTitle = "TextureRectangle";
	std::string CaptureName = "TextureRectangle";
	int32_t MaxFrameCount = 60;
	int32_t CaptureFrameIndex = 29;
	bool AllowNullTexture = false;
	bool DrawSmallRectangle = false;
	LLGI::TextureWrapMode WrapMode = LLGI::TextureWrapMode::Repeat;
	LLGI::TextureMinMagFilter MinMagFilter = LLGI::TextureMinMagFilter::Nearest;
	std::function<void(Bitmap2D&, LLGI::Texture*)> OnCaptured;
};

using TextureFactory = std::function<LLGI::Texture*(LLGI::Graphics*)>;

struct TexturedRectangleRenderTargetOptions
{
	LLGI::Vec2I RenderTextureSize = LLGI::Vec2I(128, 128);
	LLGI::TextureFormatType RenderTextureFormat = LLGI::TextureFormatType::R8G8B8A8_UNORM;
	LLGI::Color8 ClearColor = LLGI::Color8(8, 16, 24, 255);
	bool AllowNullTexture = false;
	bool DrawSmallRectangle = false;
	LLGI::TextureWrapMode WrapMode = LLGI::TextureWrapMode::Repeat;
	LLGI::TextureMinMagFilter MinMagFilter = LLGI::TextureMinMagFilter::Nearest;
};

struct TexturedRectangleRenderTargetResult
{
	std::vector<uint8_t> Data;
	LLGI::Vec2I Size;
	LLGI::TextureFormatType Format = LLGI::TextureFormatType::Unknown;
};

void RenderTexturedRectangleTest(
	LLGI::DeviceType deviceType,
	const TexturedRectangleRenderOptions& options,
	const TextureFactory& textureFactory);

TexturedRectangleRenderTargetResult RenderTexturedRectangleToRenderTarget(
	LLGI::DeviceType deviceType,
	const TexturedRectangleRenderTargetOptions& options,
	const TextureFactory& textureFactory);
