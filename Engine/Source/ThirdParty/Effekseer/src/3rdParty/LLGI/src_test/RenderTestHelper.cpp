#include "RenderTestHelper.h"

#include <Utils/LLGI.CommandListPool.h>
#include <map>

namespace
{
std::shared_ptr<LLGI::PipelineState> CreateTexturedRectanglePipeline(
	LLGI::Graphics* graphics,
	LLGI::RenderPass* renderPass,
	std::shared_ptr<LLGI::Shader>& shaderVS,
	std::shared_ptr<LLGI::Shader>& shaderPS)
{
	auto renderPassPipelineState = LLGI::CreateSharedPtr(graphics->CreateRenderPassPipelineState(renderPass));
	auto pip = graphics->CreatePiplineState();
	pip->VertexLayouts[0] = LLGI::VertexLayoutFormat::R32G32B32_FLOAT;
	pip->VertexLayouts[1] = LLGI::VertexLayoutFormat::R32G32_FLOAT;
	pip->VertexLayouts[2] = LLGI::VertexLayoutFormat::R8G8B8A8_UNORM;
	pip->VertexLayoutNames[0] = "POSITION";
	pip->VertexLayoutNames[1] = "UV";
	pip->VertexLayoutNames[2] = "COLOR";
	pip->VertexLayoutCount = 3;
	pip->SetShader(LLGI::ShaderStageType::Vertex, shaderVS.get());
	pip->SetShader(LLGI::ShaderStageType::Pixel, shaderPS.get());
	pip->SetRenderPassPipelineState(renderPassPipelineState.get());
	VERIFY(pip->Compile());
	return LLGI::CreateSharedPtr(pip);
}
} // namespace

void RenderTexturedRectangleTest(
	LLGI::DeviceType deviceType,
	const TexturedRectangleRenderOptions& options,
	const TextureFactory& textureFactory)
{
	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow(options.WindowTitle, LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreatePlatform(pp, window.get());
	VERIFY(platform != nullptr);

	auto graphics = platform->CreateGraphics();
	VERIFY(graphics != nullptr);

	auto sfMemoryPool = graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128);
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics, sfMemoryPool, 3);
	auto texture = textureFactory(graphics);
	VERIFY(options.AllowNullTexture || texture != nullptr);

	std::shared_ptr<LLGI::Buffer> vb;
	std::shared_ptr<LLGI::Buffer> ib;
	TestHelper::CreateRectangle(graphics,
								LLGI::Vec3F(-0.5f, 0.5f, 0.5f),
								LLGI::Vec3F(0.5f, -0.5f, 0.5f),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(255, 255, 255, 255),
								vb,
								ib);

	std::shared_ptr<LLGI::Buffer> vbSmall;
	std::shared_ptr<LLGI::Buffer> ibSmall;
	if (options.DrawSmallRectangle)
	{
		TestHelper::CreateRectangle(graphics,
									LLGI::Vec3F(0.62f, 0.12f, 0.5f),
									LLGI::Vec3F(0.66f, 0.08f, 0.5f),
									LLGI::Color8(255, 255, 255, 255),
									LLGI::Color8(255, 255, 255, 255),
									vbSmall,
									ibSmall);
	}

	std::map<std::shared_ptr<LLGI::RenderPassPipelineState>, std::shared_ptr<LLGI::PipelineState>> pips;
	std::shared_ptr<LLGI::Shader> shaderVS = nullptr;
	std::shared_ptr<LLGI::Shader> shaderPS = nullptr;
	TestHelper::CreateShader(graphics, deviceType, "simple_texture_rectangle.vert", "simple_texture_rectangle.frag", shaderVS, shaderPS);
	VERIFY(shaderVS != nullptr);
	VERIFY(shaderPS != nullptr);

	for (int32_t count = 0; count < options.MaxFrameCount; count++)
	{
		if (!platform->NewFrame())
		{
			break;
		}

		sfMemoryPool->NewFrame();

		LLGI::Color8 color;
		color.R = count % 255;
		color.G = 0;
		color.B = 0;
		color.A = 255;

		auto renderPass = platform->GetCurrentScreen(color, true, false);
		auto renderPassPipelineState = LLGI::CreateSharedPtr(graphics->CreateRenderPassPipelineState(renderPass));

		if (pips.count(renderPassPipelineState) == 0)
		{
			pips[renderPassPipelineState] = CreateTexturedRectanglePipeline(graphics, renderPass, shaderVS, shaderPS);
		}

		auto commandList = commandListPool->Get();
		commandList->Begin();
		commandList->BeginRenderPass(renderPass);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->SetTexture(texture, options.WrapMode, options.MinMagFilter, 0);

		commandList->SetVertexBuffer(vb.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(ib.get(), 2);
		commandList->Draw(2);

		if (options.DrawSmallRectangle)
		{
			commandList->SetVertexBuffer(vbSmall.get(), sizeof(SimpleVertex), 0);
			commandList->SetIndexBuffer(ibSmall.get(), 2);
			commandList->Draw(2);
		}

		commandList->EndRenderPass();
		commandList->End();

		graphics->Execute(commandList);

		if (TestHelper::GetIsCaptureRequired() && count == options.CaptureFrameIndex)
		{
			commandListPool->WaitUntilCompleted();
			auto renderTexture = renderPass->GetRenderTexture(0);
			auto captured = graphics->CaptureRenderTarget(renderTexture);
			Bitmap2D bitmap(captured, renderTexture->GetSizeAs2D().X, renderTexture->GetSizeAs2D().Y, renderTexture->GetFormat());

			if (options.OnCaptured)
			{
				options.OnCaptured(bitmap, renderTexture);
			}
			else
			{
				const auto path = options.CaptureName + "_" + TestHelper::GetDeviceName(deviceType) + ".png";
				bitmap.Save(path);
			}
			break;
		}

		platform->Present();
	}

	commandListPool->WaitUntilCompleted();
	graphics->WaitFinish();

	pips.clear();
	shaderVS.reset();
	shaderPS.reset();
	vb.reset();
	ib.reset();
	vbSmall.reset();
	ibSmall.reset();
	commandListPool.reset();

	LLGI::SafeRelease(sfMemoryPool);
	LLGI::SafeRelease(texture);
	LLGI::SafeRelease(graphics);
	LLGI::SafeRelease(platform);
}

TexturedRectangleRenderTargetResult RenderTexturedRectangleToRenderTarget(
	LLGI::DeviceType deviceType,
	const TexturedRectangleRenderTargetOptions& options,
	const TextureFactory& textureFactory)
{
	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = false;

	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, nullptr));
	VERIFY(platform != nullptr);

	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
	VERIFY(graphics != nullptr);

	auto texture = LLGI::CreateSharedPtr(textureFactory(graphics.get()));
	VERIFY(options.AllowNullTexture || texture != nullptr);

	LLGI::RenderTextureInitializationParameter renderTextureParam;
	renderTextureParam.Size = options.RenderTextureSize;
	renderTextureParam.Format = options.RenderTextureFormat;
	auto renderTexture = LLGI::CreateSharedPtr(graphics->CreateRenderTexture(renderTextureParam));
	VERIFY(renderTexture != nullptr);

	auto renderPass = LLGI::CreateSharedPtr(graphics->CreateRenderPass(renderTexture.get(), nullptr, nullptr, nullptr));
	VERIFY(renderPass != nullptr);
	renderPass->SetClearColor(options.ClearColor);
	renderPass->SetIsColorCleared(true);

	std::shared_ptr<LLGI::Shader> shaderVS = nullptr;
	std::shared_ptr<LLGI::Shader> shaderPS = nullptr;
	TestHelper::CreateShader(graphics.get(), deviceType, "simple_texture_rectangle.vert", "simple_texture_rectangle.frag", shaderVS, shaderPS);
	VERIFY(shaderVS != nullptr);
	VERIFY(shaderPS != nullptr);

	std::shared_ptr<LLGI::Buffer> vb;
	std::shared_ptr<LLGI::Buffer> ib;
	TestHelper::CreateRectangle(graphics.get(),
								LLGI::Vec3F(-0.5f, 0.5f, 0.5f),
								LLGI::Vec3F(0.5f, -0.5f, 0.5f),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(255, 255, 255, 255),
								vb,
								ib);

	std::shared_ptr<LLGI::Buffer> vbSmall;
	std::shared_ptr<LLGI::Buffer> ibSmall;
	if (options.DrawSmallRectangle)
	{
		TestHelper::CreateRectangle(graphics.get(),
									LLGI::Vec3F(0.62f, 0.12f, 0.5f),
									LLGI::Vec3F(0.66f, 0.08f, 0.5f),
									LLGI::Color8(255, 255, 255, 255),
									LLGI::Color8(255, 255, 255, 255),
									vbSmall,
									ibSmall);
	}

	auto pipelineState = CreateTexturedRectanglePipeline(graphics.get(), renderPass.get(), shaderVS, shaderPS);
	VERIFY(pipelineState != nullptr);

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 16));
	VERIFY(sfMemoryPool != nullptr);
	sfMemoryPool->NewFrame();

	auto commandList = LLGI::CreateSharedPtr(graphics->CreateCommandList(sfMemoryPool.get()));
	VERIFY(commandList != nullptr);
	commandList->Begin();
	commandList->BeginRenderPass(renderPass.get());
	commandList->SetPipelineState(pipelineState.get());
	commandList->SetTexture(texture.get(), options.WrapMode, options.MinMagFilter, 0);

	commandList->SetVertexBuffer(vb.get(), sizeof(SimpleVertex), 0);
	commandList->SetIndexBuffer(ib.get(), 2);
	commandList->Draw(2);

	if (options.DrawSmallRectangle)
	{
		commandList->SetVertexBuffer(vbSmall.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(ibSmall.get(), 2);
		commandList->Draw(2);
	}

	commandList->EndRenderPass();
	commandList->End();

	graphics->Execute(commandList.get());
	graphics->WaitFinish();

	TexturedRectangleRenderTargetResult result;
	result.Data = graphics->CaptureRenderTarget(renderTexture.get());
	result.Size = renderTexture->GetSizeAs2D();
	result.Format = renderTexture->GetFormat();
	return result;
}
