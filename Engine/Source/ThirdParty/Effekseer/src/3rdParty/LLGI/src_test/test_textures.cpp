
#include "TestHelper.h"
#include "test.h"

#include <Utils/LLGI.CommandListPool.h>
#include <fstream>
#include <iostream>
#include <map>

void test_textures(LLGI::DeviceType deviceType)
{
	auto compiler = LLGI::CreateCompiler(deviceType);

	int count = 0;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("TextureRectangle", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreatePlatform(pp, window.get());

	auto graphics = platform->CreateGraphics();
	auto sfMemoryPool = graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128);

	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics, sfMemoryPool, 3);

	// Create textures
	LLGI::TextureInitializationParameter texParamSrc1;
	texParamSrc1.Size = {1, 1};
	auto texSrc1 = LLGI::CreateSharedPtr(graphics->CreateTexture(texParamSrc1));

	LLGI::TextureInitializationParameter texParamSrc2;
	texParamSrc2.Size = {1, 1};
	auto texSrc2 = LLGI::CreateSharedPtr(graphics->CreateTexture(texParamSrc2));

	LLGI::TextureInitializationParameter texParamSrc3;
	texParamSrc3.Size = {1, 1};
	auto texSrc3 = LLGI::CreateSharedPtr(graphics->CreateTexture(texParamSrc3));

	LLGI::TextureParameter texParamDst1;
	texParamDst1.Size = {1, 1, 1};
	auto texDst1 = LLGI::CreateSharedPtr(graphics->CreateTexture(texParamDst1));

	LLGI::TextureParameter texParamDst2;
	texParamDst2.Size = {1, 1, 3};
	texParamDst2.Usage = LLGI::TextureUsageType::Array;
	auto texDst2 = LLGI::CreateSharedPtr(graphics->CreateTexture(texParamDst2));

	LLGI::TextureParameter texParamDst3;
	texParamDst3.Size = {1, 1, 3};
	texParamDst3.Dimension = 3;
	auto texDst3 = LLGI::CreateSharedPtr(graphics->CreateTexture(texParamDst3));

	assert(texDst2 != nullptr);
	assert(texDst3 != nullptr);

	if (auto data = texSrc1->Lock())
	{
		auto p = static_cast<uint8_t*>(data);
		p[0] = 255;
		p[1] = 0;
		p[2] = 0;
		p[3] = 255;
		texSrc1->Unlock();
	}

	if (auto data = texSrc2->Lock())
	{
		auto p = static_cast<uint8_t*>(data);
		p[0] = 0;
		p[1] = 255;
		p[2] = 0;
		p[3] = 255;
		texSrc2->Unlock();
	}

	if (auto data = texSrc3->Lock())
	{
		auto p = static_cast<uint8_t*>(data);
		p[0] = 0;
		p[1] = 0;
		p[2] = 255;
		p[3] = 255;
		texSrc3->Unlock();
	}

	LLGI::Shader* shader_vs = nullptr;
	LLGI::Shader* shader_ps = nullptr;

	std::vector<LLGI::DataStructure> data_vs;
	std::vector<LLGI::DataStructure> data_ps;

	if (compiler == nullptr)
	{
		auto binary_vs = TestHelper::LoadData("simple_texture_rectangle.vert.spv");
		auto binary_ps = TestHelper::LoadData("textures.frag.spv");

		LLGI::DataStructure d_vs;
		LLGI::DataStructure d_ps;

		d_vs.Data = binary_vs.data();
		d_vs.Size = static_cast<int32_t>(binary_vs.size());
		d_ps.Data = binary_ps.data();
		d_ps.Size = static_cast<int32_t>(binary_ps.size());

		data_vs.push_back(d_vs);
		data_ps.push_back(d_ps);

		shader_vs = graphics->CreateShader(data_vs.data(), static_cast<int32_t>(data_vs.size()));
		shader_ps = graphics->CreateShader(data_ps.data(), static_cast<int32_t>(data_ps.size()));
	}
	else
	{
		LLGI::CompilerResult result_vs;
		LLGI::CompilerResult result_ps;

		if (platform->GetDeviceType() == LLGI::DeviceType::Metal || platform->GetDeviceType() == LLGI::DeviceType::DirectX12 ||
			platform->GetDeviceType() == LLGI::DeviceType::Vulkan || platform->GetDeviceType() == LLGI::DeviceType::WebGPU)
		{
			auto code_vs = TestHelper::LoadData("simple_texture_rectangle.vert");
			auto code_ps = TestHelper::LoadData("textures.frag");
			code_vs.push_back(0);
			code_ps.push_back(0);

			compiler->Compile(result_vs, (const char*)code_vs.data(), LLGI::ShaderStageType::Vertex);
			compiler->Compile(result_ps, (const char*)code_ps.data(), LLGI::ShaderStageType::Pixel);
		}

		for (auto& b : result_vs.Binary)
		{
			LLGI::DataStructure d;
			d.Data = b.data();
			d.Size = static_cast<int32_t>(b.size());
			data_vs.push_back(d);
		}

		for (auto& b : result_ps.Binary)
		{
			LLGI::DataStructure d;
			d.Data = b.data();
			d.Size = static_cast<int32_t>(b.size());
			data_ps.push_back(d);
		}

		shader_vs = graphics->CreateShader(data_vs.data(), static_cast<int32_t>(data_vs.size()));
		shader_ps = graphics->CreateShader(data_ps.data(), static_cast<int32_t>(data_ps.size()));
	}

	assert(shader_vs != nullptr);
	assert(shader_ps != nullptr);

	LLGI::Vec3F vert_ul = LLGI::Vec3F(-0.5, 0.5, 0.5);
	LLGI::Vec3F vert_lr = LLGI::Vec3F(0.5, -0.5, 0.5);

	std::shared_ptr<LLGI::Buffer> vb;
	std::shared_ptr<LLGI::Buffer> ib;
	TestHelper::CreateRectangle(graphics, vert_ul, vert_lr, LLGI::Color8(255, 255, 255, 255), LLGI::Color8(255, 255, 255, 255), vb, ib);

	std::map<std::shared_ptr<LLGI::RenderPassPipelineState>, std::shared_ptr<LLGI::PipelineState>> pips;

	while (count < 60)
	{
		if (!platform->NewFrame())
			break;

		sfMemoryPool->NewFrame();

		LLGI::Color8 color;
		color.R = count % 255;
		color.G = 0;
		color.B = 0;
		color.A = 255;

		auto renderPass = platform->GetCurrentScreen(color, true, false); // TODO: isDepthClear is false, because it fails with dx12.
		auto renderPassPipelineState = LLGI::CreateSharedPtr(graphics->CreateRenderPassPipelineState(renderPass));

		if (pips.count(renderPassPipelineState) == 0)
		{
			auto pip = graphics->CreatePiplineState();
			pip->VertexLayouts[0] = LLGI::VertexLayoutFormat::R32G32B32_FLOAT;
			pip->VertexLayouts[1] = LLGI::VertexLayoutFormat::R32G32_FLOAT;
			pip->VertexLayouts[2] = LLGI::VertexLayoutFormat::R8G8B8A8_UNORM;
			pip->VertexLayoutNames[0] = "POSITION";
			pip->VertexLayoutNames[1] = "UV";
			pip->VertexLayoutNames[2] = "COLOR";
			pip->VertexLayoutCount = 3;

			pip->SetShader(LLGI::ShaderStageType::Vertex, shader_vs);
			pip->SetShader(LLGI::ShaderStageType::Pixel, shader_ps);
			pip->SetRenderPassPipelineState(renderPassPipelineState.get());
			pip->Compile();

			pips[renderPassPipelineState] = LLGI::CreateSharedPtr(pip);
		}

		auto commandList = commandListPool->Get();
		commandList->Begin();

		commandList->CopyTexture(texSrc1.get(), texDst1.get(), {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 0, 0);
		commandList->CopyTexture(texSrc2.get(), texDst2.get(), {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 0, 1);
		commandList->CopyTexture(texSrc3.get(), texDst3.get(), {0, 0, 0}, {0, 0, 1}, {1, 1, 1}, 0, 0);

		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vb.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(ib.get(), 2);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->SetTexture(texDst1.get(), LLGI::TextureWrapMode::Repeat, LLGI::TextureMinMagFilter::Nearest, 0);
		commandList->SetTexture(texDst2.get(), LLGI::TextureWrapMode::Repeat, LLGI::TextureMinMagFilter::Nearest, 1);
		commandList->SetTexture(texDst3.get(), LLGI::TextureWrapMode::Repeat, LLGI::TextureMinMagFilter::Nearest, 2);
		commandList->Draw(2);

		commandList->EndRenderPass();
			commandList->End();

			graphics->Execute(commandList);

			if (TestHelper::GetIsCaptureRequired() && (count + 1) == 30)
			{
				commandListPool->WaitUntilCompleted();

				auto textureMipmap = renderPass->GetRenderTexture(0);
				auto data = graphics->CaptureRenderTarget(textureMipmap);
				std::string path = "SimpleRender.Textures_" + TestHelper::GetDeviceName(deviceType) + ".png";
				Bitmap2D(data, textureMipmap->GetSizeAs2D().X, textureMipmap->GetSizeAs2D().Y, textureMipmap->GetFormat()).Save(path.c_str());
			}

			platform->Present();
			count++;
		}

	pips.clear();

	graphics->WaitFinish();

	LLGI::SafeRelease(sfMemoryPool);
	LLGI::SafeRelease(shader_vs);
	LLGI::SafeRelease(shader_ps);
	LLGI::SafeRelease(graphics);
	LLGI::SafeRelease(platform);

	LLGI::SafeRelease(compiler);
}
TestRegister SimpleRender_Textures("SimpleRender.Textures", [](LLGI::DeviceType device) -> void { test_textures(device); });
