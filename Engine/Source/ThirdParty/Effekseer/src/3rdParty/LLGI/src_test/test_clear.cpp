#include "TestHelper.h"
#include "test.h"
#include <Utils/LLGI.CommandListPool.h>

void test_clear_update(LLGI::DeviceType deviceType)
{
	int count = 0;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("ClearUpdate", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreatePlatform(pp, window.get());

	auto graphics = platform->CreateGraphics();
	auto sfMemoryPool = graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128);

	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics, sfMemoryPool, 3);

	while (count < 60)
	{
		if (!platform->NewFrame())
			break;

		sfMemoryPool->NewFrame();

		LLGI::Color8 color;
		color.R = (count + 200) % 255;
		color.G = 0;
			color.B = 0;
			color.A = 255;

			auto renderPass = platform->GetCurrentScreen(color, true, false); // TODO: isDepthClear is false, because it fails with dx12.

			auto commandList = commandListPool->Get();

			commandList->Begin();
			commandList->BeginRenderPass(renderPass);
			commandList->EndRenderPass();
			commandList->End();

			graphics->Execute(commandList);

			if (TestHelper::GetIsCaptureRequired() && (count + 1) == 30)
			{
				commandListPool->WaitUntilCompleted();
				auto texture = renderPass->GetRenderTexture(0);
				auto data = graphics->CaptureRenderTarget(texture);

				// save
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat())
					.Save("Clear.Update_" + TestHelper::GetDeviceName(deviceType) + ".png");
			}

			platform->Present();
			count++;
		}

	graphics->WaitFinish();

	LLGI::SafeRelease(sfMemoryPool);
	LLGI::SafeRelease(graphics);
	LLGI::SafeRelease(platform);
}

void test_clear(LLGI::DeviceType deviceType)
{
	int count = 0;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("Clear", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreatePlatform(pp, window.get());

	auto graphics = platform->CreateGraphics();
	auto sfMemoryPool = graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128);

	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics, sfMemoryPool, 3);

	LLGI::Color8 color;
	color.R = 255;
	color.G = 0;
	color.B = 0;
	color.A = 255;

	while (count < 60)
	{
		if (!platform->NewFrame())
			break;

		sfMemoryPool->NewFrame();

			// It need to create a command buffer between NewFrame and Present.
			// Because get current screen returns other values by every frame.
			auto renderPass = platform->GetCurrentScreen(color, true, false); // TODO: isDepthClear is false, because it fails with dx12.
			auto commandList = commandListPool->Get();

			commandList->Begin();
			commandList->BeginRenderPass(renderPass);
			commandList->EndRenderPass();
			commandList->End();

			graphics->Execute(commandList);

			if (TestHelper::GetIsCaptureRequired() && (count + 1) == 30)
			{
				commandListPool->WaitUntilCompleted();
				auto texture = renderPass->GetRenderTexture(0);
				auto data = graphics->CaptureRenderTarget(texture);

				// save
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat())
					.Save("Clear.Basic_" + TestHelper::GetDeviceName(deviceType) + ".png");
			}

			platform->Present();
			count++;
		}

	graphics->WaitFinish();

	LLGI::SafeRelease(sfMemoryPool);
	LLGI::SafeRelease(graphics);
	LLGI::SafeRelease(platform);
}

TestRegister Clear_Basic("Clear.Basic", [](LLGI::DeviceType device) -> void { test_clear(device); });

TestRegister Clear_Update("Clear.Update", [](LLGI::DeviceType device) -> void { test_clear_update(device); });
