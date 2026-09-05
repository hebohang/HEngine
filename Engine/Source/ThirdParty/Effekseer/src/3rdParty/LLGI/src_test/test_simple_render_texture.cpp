#include "TextureDataGenerator.h"
#include "TextureRenderTestHelper.h"
#include "test.h"

#include <string>

enum class SimpleTextureRectangleTestMode
{
	RGBA8,
	RGBA32F,
	R8,
	Null,
};

namespace
{
LLGI::TextureFormatType GetSimpleTextureFormat(SimpleTextureRectangleTestMode mode)
{
	if (mode == SimpleTextureRectangleTestMode::RGBA32F)
	{
		return LLGI::TextureFormatType::R32G32B32A32_FLOAT;
	}
	if (mode == SimpleTextureRectangleTestMode::R8)
	{
		return LLGI::TextureFormatType::R8_UNORM;
	}
	return LLGI::TextureFormatType::R8G8B8A8_UNORM;
}

std::string GetSimpleTextureCaptureName(SimpleTextureRectangleTestMode mode)
{
	if (mode == SimpleTextureRectangleTestMode::RGBA32F)
	{
		return "SimpleRender.TextureRGB32F";
	}
	if (mode == SimpleTextureRectangleTestMode::R8)
	{
		return "SimpleRender.TextureR8";
	}
	if (mode == SimpleTextureRectangleTestMode::Null)
	{
		return "SimpleRender.TextureNull";
	}
	return "SimpleRender.TextureRGB8";
}

void test_simple_texture_rectangle(LLGI::DeviceType deviceType, SimpleTextureRectangleTestMode mode)
{
	TexturedRectangleRenderOptions options;
	options.WindowTitle = "TextureRectangle";
	const auto captureName = GetSimpleTextureCaptureName(mode);
	options.CaptureName = captureName;
	options.AllowNullTexture = mode == SimpleTextureRectangleTestMode::Null;

	if (mode == SimpleTextureRectangleTestMode::Null)
	{
		options.OnCaptured = [deviceType, captureName](Bitmap2D& bitmap, LLGI::Texture* texture) {
			const auto center = bitmap.GetPixel(texture->GetSizeAs2D().X / 2, texture->GetSizeAs2D().Y / 2);
			VERIFY(center.g > 128);
			bitmap.Save(captureName + "_" + TestHelper::GetDeviceName(deviceType) + ".png");
		};

		RenderTexturedRectangleTest(deviceType, options, [](LLGI::Graphics*) { return static_cast<LLGI::Texture*>(nullptr); });
		return;
	}

	RenderTexturedRectangleTest(deviceType, options, [mode](LLGI::Graphics* graphics) {
		LLGI::TextureInitializationParameter texParam;
		texParam.Format = GetSimpleTextureFormat(mode);
		texParam.Size = LLGI::Vec2I(256, 256);

		auto texture = graphics->CreateTexture(texParam);
		VERIFY(texture != nullptr);
		VERIFY(texture->GetType() == LLGI::TextureType::Color);

		TextureDataGenerator::WriteDummyTexture(texture);
		return texture;
	});
}
} // namespace

TestRegister SimpleRender_Tex_RGBA8("SimpleRender.Texture_RGBA8", [](LLGI::DeviceType device) -> void {
	test_simple_texture_rectangle(device, SimpleTextureRectangleTestMode::RGBA8);
});

TestRegister SimpleRender_Tex_RGBA32F("SimpleRender.Texture_RGBA32F", [](LLGI::DeviceType device) -> void {
	test_simple_texture_rectangle(device, SimpleTextureRectangleTestMode::RGBA32F);
});
TestRegister SimpleRender_Tex_R8("SimpleRender.Texture_R8", [](LLGI::DeviceType device) -> void {
	test_simple_texture_rectangle(device, SimpleTextureRectangleTestMode::R8);
});

TestRegister SimpleRender_Tex_B8G8R8A8("SimpleRender.Texture_B8G8R8A8", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_B8G8R8A8", LLGI::TextureFormatType::B8G8R8A8_UNORM, 1});
});

TestRegister SimpleRender_Tex_RG11B10_UFLOAT("SimpleRender.Texture_RG11B10_UFLOAT", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_RG11B10_UFLOAT", LLGI::TextureFormatType::RG11B10_UFLOAT, 1});
});

TestRegister SimpleRender_Tex_R8G8B8A8_SRGB("SimpleRender.Texture_R8G8B8A8_SRGB", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_R8G8B8A8_SRGB", LLGI::TextureFormatType::R8G8B8A8_UNORM_SRGB, 1});
});

TestRegister SimpleRender_Tex_B8G8R8A8_SRGB("SimpleRender.Texture_B8G8R8A8_SRGB", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_B8G8R8A8_SRGB", LLGI::TextureFormatType::B8G8R8A8_UNORM_SRGB, 1});
});

TestRegister SimpleRender_Tex_R16F("SimpleRender.Texture_R16F", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_R16F", LLGI::TextureFormatType::R16_FLOAT, 1});
});

TestRegister SimpleRender_Tex_R32F("SimpleRender.Texture_R32F", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_R32F", LLGI::TextureFormatType::R32_FLOAT, 1});
});

TestRegister SimpleRender_Tex_RG16F("SimpleRender.Texture_RG16F", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_RG16F", LLGI::TextureFormatType::R16G16_FLOAT, 1});
});

TestRegister SimpleRender_Tex_RG32F("SimpleRender.Texture_RG32F", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_RG32F", LLGI::TextureFormatType::R32G32_FLOAT, 1});
});

TestRegister SimpleRender_Tex_RGBA16F("SimpleRender.Texture_RGBA16F", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_RGBA16F", LLGI::TextureFormatType::R16G16B16A16_FLOAT, 1});
});

TestRegister SimpleRender_Tex_BC1("SimpleRender.Texture_BC1", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC1", LLGI::TextureFormatType::BC1, 1});
});

TestRegister SimpleRender_Tex_BC2("SimpleRender.Texture_BC2", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC2", LLGI::TextureFormatType::BC2, 1});
});

TestRegister SimpleRender_Tex_BC3("SimpleRender.Texture_BC3", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC3", LLGI::TextureFormatType::BC3, 1});
});

TestRegister SimpleRender_Tex_BC7("SimpleRender.Texture_BC7", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC7", LLGI::TextureFormatType::BC7, 1});
});

TestRegister SimpleRender_Tex_BC1_SRGB("SimpleRender.Texture_BC1_SRGB", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC1_SRGB", LLGI::TextureFormatType::BC1_SRGB, 1});
});

TestRegister SimpleRender_Tex_BC2_SRGB("SimpleRender.Texture_BC2_SRGB", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC2_SRGB", LLGI::TextureFormatType::BC2_SRGB, 1});
});

TestRegister SimpleRender_Tex_BC3_SRGB("SimpleRender.Texture_BC3_SRGB", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC3_SRGB", LLGI::TextureFormatType::BC3_SRGB, 1});
});

TestRegister SimpleRender_Tex_BC7_SRGB("SimpleRender.Texture_BC7_SRGB", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC7_SRGB", LLGI::TextureFormatType::BC7_SRGB, 1});
});

TestRegister SimpleRender_Tex_BC1_MipMap_RawData("SimpleRender.Texture_BC1_MipMap_RawData", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC1_MipMap_RawData", LLGI::TextureFormatType::BC1, 4});
});

TestRegister SimpleRender_Tex_BC2_MipMap_RawData("SimpleRender.Texture_BC2_MipMap_RawData", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC2_MipMap_RawData", LLGI::TextureFormatType::BC2, 4});
});

TestRegister SimpleRender_Tex_BC3_MipMap_RawData("SimpleRender.Texture_BC3_MipMap_RawData", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC3_MipMap_RawData", LLGI::TextureFormatType::BC3, 4});
});

TestRegister SimpleRender_Tex_BC7_MipMap_RawData("SimpleRender.Texture_BC7_MipMap_RawData", [](LLGI::DeviceType device) -> void {
	RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC7_MipMap_RawData", LLGI::TextureFormatType::BC7, 4});
});

TestRegister SimpleRender_Tex_BC1_SRGB_MipMap_RawData(
	"SimpleRender.Texture_BC1_SRGB_MipMap_RawData",
	[](LLGI::DeviceType device) -> void {
		RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC1_SRGB_MipMap_RawData", LLGI::TextureFormatType::BC1_SRGB, 4});
	});

TestRegister SimpleRender_Tex_BC2_SRGB_MipMap_RawData(
	"SimpleRender.Texture_BC2_SRGB_MipMap_RawData",
	[](LLGI::DeviceType device) -> void {
		RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC2_SRGB_MipMap_RawData", LLGI::TextureFormatType::BC2_SRGB, 4});
	});

TestRegister SimpleRender_Tex_BC3_SRGB_MipMap_RawData(
	"SimpleRender.Texture_BC3_SRGB_MipMap_RawData",
	[](LLGI::DeviceType device) -> void {
		RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC3_SRGB_MipMap_RawData", LLGI::TextureFormatType::BC3_SRGB, 4});
	});

TestRegister SimpleRender_Tex_BC7_SRGB_MipMap_RawData(
	"SimpleRender.Texture_BC7_SRGB_MipMap_RawData",
	[](LLGI::DeviceType device) -> void {
		RunTextureFormatRawDataScreenRenderTest(device, {"Texture_BC7_SRGB_MipMap_RawData", LLGI::TextureFormatType::BC7_SRGB, 4});
	});

TestRegister SimpleRender_Tex_CompressedArrayMipMap_RawData(
	"SimpleRender.Texture_CompressedArrayMipMap_RawData",
	[](LLGI::DeviceType device) -> void {
		const TextureFormatRenderTestCase testCases[] = {
			{"Texture_BC1_ArrayMipMap_RawData", LLGI::TextureFormatType::BC1, 4, true, 3},
			{"Texture_BC2_ArrayMipMap_RawData", LLGI::TextureFormatType::BC2, 4, true, 3},
			{"Texture_BC3_ArrayMipMap_RawData", LLGI::TextureFormatType::BC3, 4, true, 3},
			{"Texture_BC7_ArrayMipMap_RawData", LLGI::TextureFormatType::BC7, 4, true, 3},
			{"Texture_BC1_SRGB_ArrayMipMap_RawData", LLGI::TextureFormatType::BC1_SRGB, 4, true, 3},
			{"Texture_BC2_SRGB_ArrayMipMap_RawData", LLGI::TextureFormatType::BC2_SRGB, 4, true, 3},
			{"Texture_BC3_SRGB_ArrayMipMap_RawData", LLGI::TextureFormatType::BC3_SRGB, 4, true, 3},
			{"Texture_BC7_SRGB_ArrayMipMap_RawData", LLGI::TextureFormatType::BC7_SRGB, 4, true, 3},
		};

		if (!IsTextureFormatRenderTestSupported(device, LLGI::TextureFormatType::BC1))
		{
			return;
		}

		LLGI::PlatformParameter pp;
		pp.Device = device;
		pp.WaitVSync = false;

		auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("TextureCompressedArrayMipMap", LLGI::Vec2I(64, 64)));
		VERIFY(window != nullptr);

		auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
		VERIFY(platform != nullptr);

		auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
		VERIFY(graphics != nullptr);

		for (const auto& testCase : testCases)
		{
			auto texture = LLGI::CreateSharedPtr(CreateRawDataTexture(graphics.get(), testCase));
			VERIFY(texture != nullptr);
			VERIFY(texture->GetUsage() == LLGI::TextureUsageType::Array);
			VERIFY(texture->GetFormat() == testCase.Format);
		}

		graphics->WaitFinish();
	});

TestRegister SimpleRender_Tex_Null("SimpleRender.Texture_Null", [](LLGI::DeviceType device) -> void {
	test_simple_texture_rectangle(device, SimpleTextureRectangleTestMode::Null);
});
