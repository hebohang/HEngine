#include "TestHelper.h"
#include "TextureDataGenerator.h"
#include "TextureRenderTestHelper.h"
#include "test.h"

#include <Utils/LLGI.CommandListPool.h>
#include <cmath>
#include <iostream>

namespace
{
struct BrowserComputeInput
{
	float value1;
	float value2;
};

struct BrowserComputeOutput
{
	float value;
};

struct BrowserConstant
{
	float values[4];
};

void configureSimpleVertexLayout(LLGI::PipelineState* pipelineState)
{
	pipelineState->VertexLayouts[0] = LLGI::VertexLayoutFormat::R32G32B32_FLOAT;
	pipelineState->VertexLayouts[1] = LLGI::VertexLayoutFormat::R32G32_FLOAT;
	pipelineState->VertexLayouts[2] = LLGI::VertexLayoutFormat::R8G8B8A8_UNORM;
	pipelineState->VertexLayoutNames[0] = "POSITION";
	pipelineState->VertexLayoutNames[1] = "UV";
	pipelineState->VertexLayoutNames[2] = "COLOR";
	pipelineState->VertexLayoutCount = 3;
}

void test_webgpu_browser_offscreen_render(LLGI::DeviceType deviceType)
{
	VERIFY(deviceType == LLGI::DeviceType::WebGPU);

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = false;

	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, nullptr));
	VERIFY(platform != nullptr);

	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
	VERIFY(graphics != nullptr);

	LLGI::RenderTextureInitializationParameter renderTextureParam;
	renderTextureParam.Size = LLGI::Vec2I(64, 64);
	renderTextureParam.Format = LLGI::TextureFormatType::R8G8B8A8_UNORM;
	auto renderTexture = LLGI::CreateSharedPtr(graphics->CreateRenderTexture(renderTextureParam));
	VERIFY(renderTexture != nullptr);

	auto renderPass = LLGI::CreateSharedPtr(graphics->CreateRenderPass(renderTexture.get(), nullptr, nullptr, nullptr));
	VERIFY(renderPass != nullptr);
	renderPass->SetClearColor(LLGI::Color8(32, 64, 96, 255));
	renderPass->SetIsColorCleared(true);

	std::shared_ptr<LLGI::Shader> shaderVS = nullptr;
	std::shared_ptr<LLGI::Shader> shaderPS = nullptr;
	TestHelper::CreateShader(graphics.get(), deviceType, "simple_rectangle.vert", "simple_rectangle.frag", shaderVS, shaderPS);
	VERIFY(shaderVS != nullptr);
	VERIFY(shaderPS != nullptr);

	std::shared_ptr<LLGI::Buffer> vertexBuffer;
	std::shared_ptr<LLGI::Buffer> indexBuffer;
	TestHelper::CreateRectangle(graphics.get(),
								LLGI::Vec3F(-0.5f, 0.5f, 0.5f),
								LLGI::Vec3F(0.5f, -0.5f, 0.5f),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(0, 255, 0, 255),
								vertexBuffer,
								indexBuffer);
	VERIFY(vertexBuffer != nullptr);
	VERIFY(indexBuffer != nullptr);

	auto renderPassPipelineState = LLGI::CreateSharedPtr(graphics->CreateRenderPassPipelineState(renderPass.get()));
	VERIFY(renderPassPipelineState != nullptr);

	auto pipelineState = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	VERIFY(pipelineState != nullptr);
	configureSimpleVertexLayout(pipelineState.get());
	pipelineState->SetShader(LLGI::ShaderStageType::Vertex, shaderVS.get());
	pipelineState->SetShader(LLGI::ShaderStageType::Pixel, shaderPS.get());
	pipelineState->SetRenderPassPipelineState(renderPassPipelineState.get());
	VERIFY(pipelineState->Compile());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 16));
	VERIFY(sfMemoryPool != nullptr);
	sfMemoryPool->NewFrame();

	auto commandList = LLGI::CreateSharedPtr(graphics->CreateCommandList(sfMemoryPool.get()));
	VERIFY(commandList != nullptr);
	commandList->Begin();
	commandList->BeginRenderPass(renderPass.get());
	commandList->SetVertexBuffer(vertexBuffer.get(), sizeof(SimpleVertex), 0);
	commandList->SetIndexBuffer(indexBuffer.get(), 2);
	commandList->SetPipelineState(pipelineState.get());
	commandList->Draw(2);
	commandList->EndRenderPass();
	commandList->End();

	graphics->Execute(commandList.get());
	graphics->WaitFinish();
}

void test_webgpu_browser_texture_and_constant_render(LLGI::DeviceType deviceType)
{
	VERIFY(deviceType == LLGI::DeviceType::WebGPU);

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = false;

	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, nullptr));
	VERIFY(platform != nullptr);

	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
	VERIFY(graphics != nullptr);

	LLGI::TextureInitializationParameter textureParam;
	textureParam.Size = LLGI::Vec2I(256, 256);
	textureParam.Format = LLGI::TextureFormatType::R8G8B8A8_UNORM;
	auto texture = LLGI::CreateSharedPtr(graphics->CreateTexture(textureParam));
	VERIFY(texture != nullptr);
	TextureDataGenerator::WriteDummyTexture(texture.get());

	LLGI::RenderTextureInitializationParameter renderTextureParam;
	renderTextureParam.Size = LLGI::Vec2I(128, 128);
	renderTextureParam.Format = LLGI::TextureFormatType::R8G8B8A8_UNORM;
	auto renderTexture = LLGI::CreateSharedPtr(graphics->CreateRenderTexture(renderTextureParam));
	VERIFY(renderTexture != nullptr);

	auto renderPass = LLGI::CreateSharedPtr(graphics->CreateRenderPass(renderTexture.get(), nullptr, nullptr, nullptr));
	VERIFY(renderPass != nullptr);
	renderPass->SetClearColor(LLGI::Color8(8, 16, 24, 255));
	renderPass->SetIsColorCleared(true);

	std::shared_ptr<LLGI::Shader> textureVS = nullptr;
	std::shared_ptr<LLGI::Shader> texturePS = nullptr;
	TestHelper::CreateShader(graphics.get(), deviceType, "simple_texture_rectangle.vert", "simple_texture_rectangle.frag", textureVS, texturePS);
	VERIFY(textureVS != nullptr);
	VERIFY(texturePS != nullptr);

	std::shared_ptr<LLGI::Shader> constantVS = nullptr;
	std::shared_ptr<LLGI::Shader> constantPS = nullptr;
	TestHelper::CreateShader(graphics.get(), deviceType, "simple_constant_rectangle.vert", "simple_constant_rectangle.frag", constantVS, constantPS);
	VERIFY(constantVS != nullptr);
	VERIFY(constantPS != nullptr);

	std::shared_ptr<LLGI::Buffer> vertexBuffer;
	std::shared_ptr<LLGI::Buffer> indexBuffer;
	TestHelper::CreateRectangle(graphics.get(),
								LLGI::Vec3F(-0.8f, 0.8f, 0.5f),
								LLGI::Vec3F(0.8f, -0.8f, 0.5f),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(0, 255, 0, 255),
								vertexBuffer,
								indexBuffer);
	VERIFY(vertexBuffer != nullptr);
	VERIFY(indexBuffer != nullptr);

	auto renderPassPipelineState = LLGI::CreateSharedPtr(graphics->CreateRenderPassPipelineState(renderPass.get()));
	VERIFY(renderPassPipelineState != nullptr);

	auto texturePipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	VERIFY(texturePipeline != nullptr);
	configureSimpleVertexLayout(texturePipeline.get());
	texturePipeline->SetShader(LLGI::ShaderStageType::Vertex, textureVS.get());
	texturePipeline->SetShader(LLGI::ShaderStageType::Pixel, texturePS.get());
	texturePipeline->SetRenderPassPipelineState(renderPassPipelineState.get());
	VERIFY(texturePipeline->Compile());

	auto constantPipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	VERIFY(constantPipeline != nullptr);
	configureSimpleVertexLayout(constantPipeline.get());
	constantPipeline->SetShader(LLGI::ShaderStageType::Vertex, constantVS.get());
	constantPipeline->SetShader(LLGI::ShaderStageType::Pixel, constantPS.get());
	constantPipeline->SetRenderPassPipelineState(renderPassPipelineState.get());
	VERIFY(constantPipeline->Compile());

	auto vertexConstantBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::Constant | LLGI::BufferUsageType::MapWrite, sizeof(BrowserConstant)));
	auto pixelConstantBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::Constant | LLGI::BufferUsageType::MapWrite, sizeof(BrowserConstant)));
	VERIFY(vertexConstantBuffer != nullptr);
	VERIFY(pixelConstantBuffer != nullptr);

	{
		auto data = static_cast<BrowserConstant*>(vertexConstantBuffer->Lock());
		data->values[0] = 0.15f;
		data->values[1] = -0.10f;
		data->values[2] = 0.0f;
		data->values[3] = 0.0f;
		vertexConstantBuffer->Unlock();
	}
	{
		auto data = static_cast<BrowserConstant*>(pixelConstantBuffer->Lock());
		data->values[0] = 0.05f;
		data->values[1] = 0.10f;
		data->values[2] = 0.15f;
		data->values[3] = 0.0f;
		pixelConstantBuffer->Unlock();
	}

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 16));
	VERIFY(sfMemoryPool != nullptr);
	sfMemoryPool->NewFrame();

	auto commandList = LLGI::CreateSharedPtr(graphics->CreateCommandList(sfMemoryPool.get()));
	VERIFY(commandList != nullptr);
	commandList->Begin();
	commandList->BeginRenderPass(renderPass.get());
	commandList->SetVertexBuffer(vertexBuffer.get(), sizeof(SimpleVertex), 0);
	commandList->SetIndexBuffer(indexBuffer.get(), 2);
	commandList->SetPipelineState(texturePipeline.get());
	commandList->SetTexture(texture.get(), LLGI::TextureWrapMode::Clamp, LLGI::TextureMinMagFilter::Linear, 0);
	commandList->Draw(2);
	commandList->ResetTextures();
	commandList->SetPipelineState(constantPipeline.get());
	commandList->SetConstantBuffer(vertexConstantBuffer.get(), 0);
	commandList->SetConstantBuffer(pixelConstantBuffer.get(), 1);
	commandList->Draw(2);
	commandList->EndRenderPass();
	commandList->End();

	graphics->Execute(commandList.get());
	graphics->WaitFinish();

	const auto data = graphics->CaptureRenderTarget(renderTexture.get());
	VERIFY(data.size() == 128 * 128 * 4);

	const auto verifyPixel = [&data](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
		const auto* pixel = data.data() + (x + y * 128) * 4;
		const bool matched = std::abs(static_cast<int>(pixel[0]) - static_cast<int>(r)) <= 1 &&
							 std::abs(static_cast<int>(pixel[1]) - static_cast<int>(g)) <= 1 &&
							 std::abs(static_cast<int>(pixel[2]) - static_cast<int>(b)) <= 1 &&
							 std::abs(static_cast<int>(pixel[3]) - static_cast<int>(a)) <= 1;
		if (!matched)
		{
			std::cout << "Pixel mismatch at " << x << "," << y << " actual=" << static_cast<int>(pixel[0]) << ","
					  << static_cast<int>(pixel[1]) << "," << static_cast<int>(pixel[2]) << "," << static_cast<int>(pixel[3])
					  << " expected=" << static_cast<int>(r) << "," << static_cast<int>(g) << "," << static_cast<int>(b) << ","
					  << static_cast<int>(a) << std::endl;
		}
		VERIFY(matched);
	};

	verifyPixel(64, 64, 155, 255, 188, 255);
	verifyPixel(2, 2, 8, 16, 24, 255);
}

void test_webgpu_browser_texture_format_raw_data(LLGI::DeviceType deviceType, const TextureFormatRenderTestCase& testCase)
{
	VERIFY(deviceType == LLGI::DeviceType::WebGPU);
	RunTextureFormatRawDataOffscreenRenderTest(deviceType, testCase);
}

void test_webgpu_browser_compute_compile(LLGI::DeviceType deviceType)
{
	VERIFY(deviceType == LLGI::DeviceType::WebGPU);

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = false;

	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, nullptr));
	VERIFY(platform != nullptr);

	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
	VERIFY(graphics != nullptr);

	std::shared_ptr<LLGI::Shader> shaderCS = nullptr;
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "basic.comp", shaderCS);
	VERIFY(shaderCS != nullptr);

	auto pipelineState = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	VERIFY(pipelineState != nullptr);
	pipelineState->SetShader(LLGI::ShaderStageType::Compute, shaderCS.get());
	VERIFY(pipelineState->Compile());
}

void test_webgpu_browser_compute_dispatch(LLGI::DeviceType deviceType)
{
	VERIFY(deviceType == LLGI::DeviceType::WebGPU);

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = false;

	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, nullptr));
	VERIFY(platform != nullptr);

	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
	VERIFY(graphics != nullptr);

	std::shared_ptr<LLGI::Shader> shaderCS = nullptr;
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "basic.comp", shaderCS);
	VERIFY(shaderCS != nullptr);

	auto pipelineState = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	VERIFY(pipelineState != nullptr);
	pipelineState->SetShader(LLGI::ShaderStageType::Compute, shaderCS.get());
	VERIFY(pipelineState->Compile());

	const int dataSize = 32;
	auto uploadBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapWrite | LLGI::BufferUsageType::CopySrc, sizeof(BrowserComputeInput) * dataSize));
	auto inputBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopyDst, sizeof(BrowserComputeInput) * dataSize));
	auto outputBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, sizeof(BrowserComputeOutput) * dataSize));
	auto readbackTarget = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapRead | LLGI::BufferUsageType::CopyDst, sizeof(BrowserComputeOutput) * dataSize));
	auto constantBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::Constant | LLGI::BufferUsageType::MapWrite, sizeof(float)));
	VERIFY(uploadBuffer != nullptr);
	VERIFY(inputBuffer != nullptr);
	VERIFY(outputBuffer != nullptr);
	VERIFY(readbackTarget != nullptr);
	VERIFY(constantBuffer != nullptr);

	{
		auto data = static_cast<BrowserComputeInput*>(uploadBuffer->Lock());
		for (int i = 0; i < dataSize; i++)
		{
			data[i].value1 = static_cast<float>(i + 1);
			data[i].value2 = static_cast<float>(i + 3);
		}
		uploadBuffer->Unlock();
	}
	{
		auto data = static_cast<float*>(constantBuffer->Lock());
		*data = 7.0f;
		constantBuffer->Unlock();
	}

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 16));
	VERIFY(sfMemoryPool != nullptr);
	sfMemoryPool->NewFrame();

	auto commandList = LLGI::CreateSharedPtr(graphics->CreateCommandList(sfMemoryPool.get()));
	VERIFY(commandList != nullptr);
	commandList->Begin();
	commandList->CopyBuffer(uploadBuffer.get(), inputBuffer.get());
	commandList->BeginComputePass();
	commandList->SetPipelineState(pipelineState.get());
	commandList->SetStorageBuffer(inputBuffer.get(), sizeof(BrowserComputeInput), 0, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->SetStorageBuffer(outputBuffer.get(), sizeof(BrowserComputeOutput), 1, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->SetConstantBuffer(constantBuffer.get(), 0);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->CopyBuffer(outputBuffer.get(), readbackTarget.get());
	commandList->End();

	graphics->Execute(commandList.get());
	graphics->WaitFinish();

	auto result = static_cast<const BrowserComputeOutput*>(readbackTarget->Lock());
	VERIFY(result != nullptr);
	for (int i = 0; i < dataSize; i++)
	{
		const float expected = static_cast<float>(i + 1) * static_cast<float>(i + 3) + 7.0f;
		VERIFY(std::fabs(result[i].value - expected) < 0.001f);
	}
	readbackTarget->Unlock();
}

void test_webgpu_browser_render_readback(LLGI::DeviceType deviceType)
{
	VERIFY(deviceType == LLGI::DeviceType::WebGPU);

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = false;

	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, nullptr));
	VERIFY(platform != nullptr);

	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
	VERIFY(graphics != nullptr);

	LLGI::RenderTextureInitializationParameter renderTextureParam;
	renderTextureParam.Size = LLGI::Vec2I(16, 16);
	renderTextureParam.Format = LLGI::TextureFormatType::R8G8B8A8_UNORM;
	auto renderTexture = LLGI::CreateSharedPtr(graphics->CreateRenderTexture(renderTextureParam));
	VERIFY(renderTexture != nullptr);

	auto renderPass = LLGI::CreateSharedPtr(graphics->CreateRenderPass(renderTexture.get(), nullptr, nullptr, nullptr));
	VERIFY(renderPass != nullptr);
	renderPass->SetClearColor(LLGI::Color8(11, 22, 33, 255));
	renderPass->SetIsColorCleared(true);

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 16));
	VERIFY(sfMemoryPool != nullptr);
	sfMemoryPool->NewFrame();

	auto commandList = LLGI::CreateSharedPtr(graphics->CreateCommandList(sfMemoryPool.get()));
	VERIFY(commandList != nullptr);
	commandList->Begin();
	commandList->BeginRenderPass(renderPass.get());
	commandList->EndRenderPass();
	commandList->End();

	graphics->Execute(commandList.get());
	graphics->WaitFinish();

	const auto data = graphics->CaptureRenderTarget(renderTexture.get());
	VERIFY(data.size() == 16 * 16 * 4);
	for (int i = 0; i < 16 * 16; i++)
	{
		const auto* pixel = data.data() + i * 4;
		VERIFY(pixel[0] == 11);
		VERIFY(pixel[1] == 22);
		VERIFY(pixel[2] == 33);
		VERIFY(pixel[3] == 255);
	}
}

void test_webgpu_browser_screen_presentation(LLGI::DeviceType deviceType)
{
	VERIFY(deviceType == LLGI::DeviceType::WebGPU);

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = false;

	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("WebGPU Browser", LLGI::Vec2I(640, 360)));
	VERIFY(window != nullptr);

	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	VERIFY(platform != nullptr);

	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
	VERIFY(graphics != nullptr);

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 16));
	VERIFY(sfMemoryPool != nullptr);

	std::shared_ptr<LLGI::Shader> shaderVS = nullptr;
	std::shared_ptr<LLGI::Shader> shaderPS = nullptr;
	TestHelper::CreateShader(graphics.get(), deviceType, "simple_rectangle.vert", "simple_rectangle.frag", shaderVS, shaderPS);
	VERIFY(shaderVS != nullptr);
	VERIFY(shaderPS != nullptr);

	std::shared_ptr<LLGI::Buffer> vertexBuffer;
	std::shared_ptr<LLGI::Buffer> indexBuffer;
	TestHelper::CreateRectangle(graphics.get(),
								LLGI::Vec3F(-0.6f, 0.6f, 0.5f),
								LLGI::Vec3F(0.6f, -0.6f, 0.5f),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(0, 255, 96, 255),
								vertexBuffer,
								indexBuffer);
	VERIFY(vertexBuffer != nullptr);
	VERIFY(indexBuffer != nullptr);

	std::shared_ptr<LLGI::RenderPassPipelineState> renderPassPipelineState;
	std::shared_ptr<LLGI::PipelineState> pipelineState;

	for (int frame = 0; frame < 3; frame++)
	{
		VERIFY(platform->NewFrame());
		sfMemoryPool->NewFrame();

		const LLGI::Color8 color(28, 96, 180, 255);
		auto renderPass = platform->GetCurrentScreen(color, true, false);
		VERIFY(renderPass != nullptr);
		if (pipelineState == nullptr)
		{
			renderPassPipelineState = LLGI::CreateSharedPtr(graphics->CreateRenderPassPipelineState(renderPass));
			VERIFY(renderPassPipelineState != nullptr);

			pipelineState = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
			VERIFY(pipelineState != nullptr);
			configureSimpleVertexLayout(pipelineState.get());
			pipelineState->SetShader(LLGI::ShaderStageType::Vertex, shaderVS.get());
			pipelineState->SetShader(LLGI::ShaderStageType::Pixel, shaderPS.get());
			pipelineState->SetRenderPassPipelineState(renderPassPipelineState.get());
			VERIFY(pipelineState->Compile());
		}

		auto commandList = LLGI::CreateSharedPtr(graphics->CreateCommandList(sfMemoryPool.get()));
		VERIFY(commandList != nullptr);
		commandList->Begin();
		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vertexBuffer.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(indexBuffer.get(), 2);
		commandList->SetPipelineState(pipelineState.get());
		commandList->Draw(2);
		commandList->EndRenderPass();
		commandList->End();

		graphics->Execute(commandList.get());
		platform->Present();
		graphics->WaitFinish();
	}
}
} // namespace

TestRegister WebGPUBrowser_OffscreenRender(
	"WebGPUBrowser.OffscreenRender",
	[](LLGI::DeviceType device) -> void { test_webgpu_browser_offscreen_render(device); });

TestRegister WebGPUBrowser_ComputeCompile(
	"WebGPUBrowser.ComputeCompile",
	[](LLGI::DeviceType device) -> void { test_webgpu_browser_compute_compile(device); });

TestRegister WebGPUBrowser_TextureAndConstantRender(
	"WebGPUBrowser.TextureAndConstantRender",
	[](LLGI::DeviceType device) -> void { test_webgpu_browser_texture_and_constant_render(device); });

TestRegister WebGPUBrowser_TextureRawData_RGBA8(
	"WebGPUBrowser.TextureRawData_RGBA8",
	[](LLGI::DeviceType device) -> void {
		test_webgpu_browser_texture_format_raw_data(device, {"TextureRawData_RGBA8", LLGI::TextureFormatType::R8G8B8A8_UNORM, 1});
	});

TestRegister WebGPUBrowser_TextureRawData_R8(
	"WebGPUBrowser.TextureRawData_R8",
	[](LLGI::DeviceType device) -> void {
		test_webgpu_browser_texture_format_raw_data(device, {"TextureRawData_R8", LLGI::TextureFormatType::R8_UNORM, 1});
	});

TestRegister WebGPUBrowser_TextureRawData_RG11B10_UFLOAT(
	"WebGPUBrowser.TextureRawData_RG11B10_UFLOAT",
	[](LLGI::DeviceType device) -> void {
		test_webgpu_browser_texture_format_raw_data(device, {"TextureRawData_RG11B10_UFLOAT", LLGI::TextureFormatType::RG11B10_UFLOAT, 1});
	});

TestRegister WebGPUBrowser_TextureRawData_RGBA8_MipMap(
	"WebGPUBrowser.TextureRawData_RGBA8_MipMap",
	[](LLGI::DeviceType device) -> void {
		test_webgpu_browser_texture_format_raw_data(device, {"TextureRawData_RGBA8_MipMap", LLGI::TextureFormatType::R8G8B8A8_UNORM, 4});
	});

TestRegister WebGPUBrowser_ComputeDispatch(
	"WebGPUBrowser.ComputeDispatch",
	[](LLGI::DeviceType device) -> void { test_webgpu_browser_compute_dispatch(device); });

TestRegister WebGPUBrowser_RenderReadback(
	"WebGPUBrowser.RenderReadback",
	[](LLGI::DeviceType device) -> void { test_webgpu_browser_render_readback(device); });

TestRegister WebGPUBrowser_ScreenPresentation(
	"WebGPUBrowser.ScreenPresentation",
	[](LLGI::DeviceType device) -> void { test_webgpu_browser_screen_presentation(device); });
