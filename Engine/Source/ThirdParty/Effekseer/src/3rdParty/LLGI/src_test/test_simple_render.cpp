
#include "TestHelper.h"
#include "test.h"

#include <Utils/LLGI.CommandListPool.h>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

enum class SingleRectangleTestMode
{
	Triangle,
	Line,
	Point,
};

void test_simple_rectangle(LLGI::DeviceType deviceType, SingleRectangleTestMode mode)
{
	if (mode == SingleRectangleTestMode::Point)
	{
		if (deviceType == LLGI::DeviceType::Default)
			return;
		if (deviceType == LLGI::DeviceType::Vulkan)
			return;
	}

	int count = 0;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("SimpleRectangle", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreatePlatform(pp, window.get());

	LLGI::SafeAddRef(platform);

	auto graphics = platform->CreateGraphics();
	graphics->SetDisposed([platform]() -> void { platform->Release(); });

	auto sfMemoryPool = graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128);

	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics, sfMemoryPool, 3);

	std::shared_ptr<LLGI::Shader> shader_vs = nullptr;
	std::shared_ptr<LLGI::Shader> shader_ps = nullptr;

	TestHelper::CreateShader(graphics, deviceType, "simple_rectangle.vert", "simple_rectangle.frag", shader_vs, shader_ps);

	std::shared_ptr<LLGI::Buffer> vb;
	std::shared_ptr<LLGI::Buffer> ib;
	TestHelper::CreateRectangle(graphics,
								LLGI::Vec3F(-0.5, 0.5, 0.5),
								LLGI::Vec3F(0.5, -0.5, 0.5),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(0, 255, 0, 255),
								vb,
								ib);

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

			if (mode == SingleRectangleTestMode::Triangle)
			{
				pip->Topology = LLGI::TopologyType::Triangle;
			}
			else if (mode == SingleRectangleTestMode::Line)
			{
				pip->Topology = LLGI::TopologyType::Line;
			}
			else if (mode == SingleRectangleTestMode::Point)
			{
				pip->Topology = LLGI::TopologyType::Point;
			}

			pip->SetShader(LLGI::ShaderStageType::Vertex, shader_vs.get());
			pip->SetShader(LLGI::ShaderStageType::Pixel, shader_ps.get());
			pip->SetRenderPassPipelineState(renderPassPipelineState.get());
			if (!pip->Compile())
			{
				abort();
			}

			pips[renderPassPipelineState] = LLGI::CreateSharedPtr(pip);
		}

		auto commandList = commandListPool->Get();
		commandList->Begin();
		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vb.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(ib.get(), 2);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->Draw(2);
		commandList->EndRenderPass();
		commandList->End();

			graphics->Execute(commandList);

			if (TestHelper::GetIsCaptureRequired() && (count + 1) == 30)
			{
				commandListPool->WaitUntilCompleted();
				auto texture = renderPass->GetRenderTexture(0);
				auto data = graphics->CaptureRenderTarget(texture);

			if (mode == SingleRectangleTestMode::Triangle)
			{
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat())
					.Save("SimpleRender.BasicTriangle_" + TestHelper::GetDeviceName(deviceType) + ".png");
			}
			else if (mode == SingleRectangleTestMode::Line)
			{
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat())
					.Save("SimpleRender.BasicLine_" + TestHelper::GetDeviceName(deviceType) + ".png");
			}
			else if (mode == SingleRectangleTestMode::Point)
			{
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat())
					.Save("SimpleRender.BasicPoint_" + TestHelper::GetDeviceName(deviceType) + ".png");
				}
				break;
			}

			platform->Present();
			count++;
		}

	commandListPool->WaitUntilCompleted();

	pips.clear();

	graphics->WaitFinish();
	LLGI::SafeRelease(sfMemoryPool);
	LLGI::SafeRelease(graphics);
	LLGI::SafeRelease(platform);
}

void test_index_offset(LLGI::DeviceType deviceType)
{
	int count = 0;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("IndexOffset", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreatePlatform(pp, window.get());
	LLGI::SafeAddRef(platform);

	auto graphics = platform->CreateGraphics();
	graphics->SetDisposed([platform]() -> void { platform->Release(); });

	auto sfMemoryPool = graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128);

	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics, sfMemoryPool, 3);

	std::shared_ptr<LLGI::Shader> shader_vs = nullptr;
	std::shared_ptr<LLGI::Shader> shader_ps = nullptr;

	TestHelper::CreateShader(graphics, deviceType, "simple_rectangle.vert", "simple_rectangle.frag", shader_vs, shader_ps);

	std::shared_ptr<LLGI::Buffer> vb;
	std::shared_ptr<LLGI::Buffer> ib;
	TestHelper::CreateRectangle(graphics,
								LLGI::Vec3F(-0.5, 0.5, 0.5),
								LLGI::Vec3F(0.5, -0.5, 0.5),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(0, 255, 0, 255),
								vb,
								ib);

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

			pip->SetShader(LLGI::ShaderStageType::Vertex, shader_vs.get());
			pip->SetShader(LLGI::ShaderStageType::Pixel, shader_ps.get());
			pip->SetRenderPassPipelineState(renderPassPipelineState.get());
			pip->Compile();

			pips[renderPassPipelineState] = LLGI::CreateSharedPtr(pip);
		}

		auto commandList = commandListPool->Get();
		commandList->Begin();
		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vb.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(ib.get(), 2, 3 * 2);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->Draw(1);
		commandList->EndRenderPass();
		commandList->End();

			graphics->Execute(commandList);

			if (TestHelper::GetIsCaptureRequired() && (count + 1) == 30)
			{
				commandListPool->WaitUntilCompleted();
				auto texture = renderPass->GetRenderTexture(0);
				auto data = graphics->CaptureRenderTarget(texture);
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat())
					.Save("SimpleRender.IndexOffset_" + TestHelper::GetDeviceName(deviceType) + ".png");
				break;
			}

			platform->Present();
			count++;
		}

	commandListPool->WaitUntilCompleted();

	pips.clear();

	graphics->WaitFinish();

	LLGI::SafeRelease(sfMemoryPool);
	LLGI::SafeRelease(graphics);
	LLGI::SafeRelease(platform);
}

void test_simple_constant_rectangle(LLGI::ConstantBufferType type, LLGI::DeviceType deviceType)
{
	auto code_gl_vs = R"(
#version 440 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

layout(binding = 0) uniform Block 
{
	vec4 u_offset;
};

out gl_PerVertex
{
	vec4 gl_Position;
};

out vec2 v_uv;
out vec4 v_color;

void main()
{
	gl_Position.x  = a_position.x;
	gl_Position.y  = a_position.y;
	gl_Position.z  = a_position.z;
	gl_Position.w  = 1.0f;
	gl_Position = gl_Position + u_offset;
	v_uv = a_uv;
	v_color = a_color;
}

)";

	auto code_gl_ps = R"(
#version 440 core
#extension GL_NV_gpu_shader5:require

layout(binding = 0) uniform Block 
{
	vec4 u_offset;
};

in vec2 v_uv;
in vec4 v_color;

layout(location = 0) out vec4 color;

void main()
{
    color = v_color + u_offset;
}

)";

	auto compiler = LLGI::CreateCompiler(deviceType);

	int count = 0;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("ConstantRectangle", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreatePlatform(pp, window.get());

	auto graphics = platform->CreateGraphics();
	auto sfMemoryPool = graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128);

	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics, sfMemoryPool, 3);

	LLGI::Buffer* cb_vs = nullptr;
	LLGI::Buffer* cb_ps = nullptr;

	LLGI::Shader* shader_vs = nullptr;
	LLGI::Shader* shader_ps = nullptr;

	std::vector<LLGI::DataStructure> data_vs;
	std::vector<LLGI::DataStructure> data_ps;

	if (compiler == nullptr)
	{
		auto binary_vs = TestHelper::LoadData("simple_constant_rectangle.vert.spv");
		auto binary_ps = TestHelper::LoadData("simple_constant_rectangle.frag.spv");

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
			auto code_vs = TestHelper::LoadData("simple_constant_rectangle.vert");
			auto code_ps = TestHelper::LoadData("simple_constant_rectangle.frag");
			code_vs.push_back(0);
			code_ps.push_back(0);

			compiler->Compile(result_vs, (const char*)code_vs.data(), LLGI::ShaderStageType::Vertex);
			compiler->Compile(result_ps, (const char*)code_ps.data(), LLGI::ShaderStageType::Pixel);
		}
		else
		{
			compiler->Compile(result_vs, code_gl_vs, LLGI::ShaderStageType::Vertex);
			compiler->Compile(result_ps, code_gl_ps, LLGI::ShaderStageType::Pixel);
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

	std::shared_ptr<LLGI::Buffer> vb;
	std::shared_ptr<LLGI::Buffer> ib;
	TestHelper::CreateRectangle(graphics,
								LLGI::Vec3F(-0.5, 0.5, 0.5),
								LLGI::Vec3F(0.5, -0.5, 0.5),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(0, 255, 0, 255),
								vb,
								ib);

	if (type == LLGI::ConstantBufferType::LongTime)
	{
		cb_vs = graphics->CreateBuffer(LLGI::BufferUsageType::Constant | LLGI::BufferUsageType::MapWrite, sizeof(float) * 4);
		cb_ps = graphics->CreateBuffer(LLGI::BufferUsageType::Constant | LLGI::BufferUsageType::MapWrite, sizeof(float) * 4);

		auto cb_vs_buf = (float*)cb_vs->Lock();
		cb_vs_buf[0] = 0.2f;
		cb_vs_buf[1] = 0.0f;
		cb_vs_buf[2] = 0.0f;
		cb_vs_buf[3] = 0.0f;
		cb_vs->Unlock();

		auto cb_ps_buf = (float*)cb_ps->Lock();
		cb_ps_buf[0] = 0.0f;
		cb_ps_buf[1] = -1.0f;
		cb_ps_buf[2] = -1.0f;
		cb_ps_buf[3] = 0.0f;
		cb_ps->Unlock();
	}

	std::map<std::shared_ptr<LLGI::RenderPassPipelineState>, std::shared_ptr<LLGI::PipelineState>> pips;

	while (count < 60)
	{
		if (!platform->NewFrame())
		{
			break;
		}

		sfMemoryPool->NewFrame();

		if (type == LLGI::ConstantBufferType::ShortTime)
		{
			cb_vs = sfMemoryPool->CreateConstantBuffer(sizeof(float) * 4);
			cb_ps = sfMemoryPool->CreateConstantBuffer(sizeof(float) * 4);

			auto cb_vs_buf = (float*)cb_vs->Lock();
			cb_vs_buf[0] = (count % 100) / 100.0f;
			cb_vs_buf[1] = 0.0f;
			cb_vs_buf[2] = 0.0f;
			cb_vs_buf[3] = 0.0f;
			cb_vs->Unlock();

			auto cb_ps_buf = (float*)cb_ps->Lock();
			cb_ps_buf[0] = 0.0f;
			cb_ps_buf[1] = -1.0f;
			cb_ps_buf[2] = -1.0f;
			cb_ps_buf[3] = 0.0f;
			cb_ps->Unlock();
		}

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

		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vb.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(ib.get(), 2);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->SetConstantBuffer(cb_vs, 0);
		commandList->SetConstantBuffer(cb_ps, 1);
		commandList->Draw(2);

		commandList->EndRenderPass();
		commandList->End();

			graphics->Execute(commandList);

			if (type == LLGI::ConstantBufferType::ShortTime)
			{
				LLGI::SafeRelease(cb_vs);
				LLGI::SafeRelease(cb_ps);
			}

			if (TestHelper::GetIsCaptureRequired() && (count + 1) == 30)
			{
				commandListPool->WaitUntilCompleted();
				auto texture = renderPass->GetRenderTexture(0);
				auto data = graphics->CaptureRenderTarget(texture);

			if (type == LLGI::ConstantBufferType::LongTime)
			{
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat())
					.Save("SimpleRender.ConstantLT_" + TestHelper::GetDeviceName(deviceType) + ".png");
			}
			else
			{
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat())
					.Save("SimpleRender.ConstantST_" + TestHelper::GetDeviceName(deviceType) + ".png");
			}

				break;
			}

			platform->Present();
			count++;
		}

	commandListPool->WaitUntilCompleted();
	graphics->WaitFinish();

	pips.clear();

	LLGI::SafeRelease(sfMemoryPool);
	LLGI::SafeRelease(cb_vs);
	LLGI::SafeRelease(cb_ps);
	LLGI::SafeRelease(shader_vs);
	LLGI::SafeRelease(shader_ps);
	LLGI::SafeRelease(graphics);
	LLGI::SafeRelease(platform);

	LLGI::SafeRelease(compiler);
}

void test_instancing(LLGI::DeviceType deviceType)
{
	int count = 0;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("Instancing", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));

	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> shader_vs = nullptr;
	std::shared_ptr<LLGI::Shader> shader_ps = nullptr;

	TestHelper::CreateShader(graphics.get(), deviceType, "instancing.vert", "simple_rectangle.frag", shader_vs, shader_ps);

	std::shared_ptr<LLGI::Buffer> vb;
	std::shared_ptr<LLGI::Buffer> ib;
	TestHelper::CreateRectangle(graphics.get(),
								LLGI::Vec3F(-0.2f, 0.2f, 0.5f),
								LLGI::Vec3F(0.2f, -0.2f, 0.5f),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(0, 255, 0, 255),
								vb,
								ib);

	auto cb = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::Constant | LLGI::BufferUsageType::MapWrite, sizeof(float) * 4 * 10));

	if (auto buf = static_cast<float*>(cb->Lock()))
	{
		for (int32_t i = 0; i < 10; i++)
		{
			buf[i * 4 + 0] = i * 0.1f;
			buf[i * 4 + 1] = i * 0.1f;
			buf[i * 4 + 2] = 0.0f;
			buf[i * 4 + 3] = 0.0f;
		}
		cb->Unlock();
	}

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

			pip->SetShader(LLGI::ShaderStageType::Vertex, shader_vs.get());
			pip->SetShader(LLGI::ShaderStageType::Pixel, shader_ps.get());
			pip->SetRenderPassPipelineState(renderPassPipelineState.get());
			pip->Compile();

			pips[renderPassPipelineState] = LLGI::CreateSharedPtr(pip);
		}

		auto commandList = commandListPool->Get();
		commandList->Begin();

		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vb.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(ib.get(), 2);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->SetConstantBuffer(cb.get(), 0);
		commandList->Draw(2, 5);
		commandList->EndRenderPass();
		commandList->End();

			graphics->Execute(commandList);

			if (TestHelper::GetIsCaptureRequired() && (count + 1) == 30)
			{
				commandListPool->WaitUntilCompleted();
				auto texture = renderPass->GetRenderTexture(0);
				auto data = graphics->CaptureRenderTarget(texture);

			// save
				std::string path = "SimpleRender.Instancing_" + TestHelper::GetDeviceName(deviceType) + ".png";
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat()).Save(path.c_str());
			}

			platform->Present();
			count++;
		}

	commandListPool->WaitUntilCompleted();
	graphics->WaitFinish();
	pips.clear();
}

void test_vertex_structured(LLGI::DeviceType deviceType)
{
	int count = 0;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("Vertex_Structured", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));

	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> shader_vs = nullptr;
	std::shared_ptr<LLGI::Shader> shader_ps = nullptr;

	TestHelper::CreateShader(graphics.get(), deviceType, "vertex_structured.vert", "simple_rectangle.frag", shader_vs, shader_ps);

	std::shared_ptr<LLGI::Buffer> vb;
	std::shared_ptr<LLGI::Buffer> ib;
	TestHelper::CreateRectangle(graphics.get(),
								LLGI::Vec3F(-0.2f, 0.2f, 0.5f),
								LLGI::Vec3F(0.2f, -0.2f, 0.5f),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(0, 255, 0, 255),
								vb,
								ib);

	struct InputData
	{
		float value1;
		float value2;
	};

	int dataSize = 6;

	std::vector<InputData> inputData;
	inputData.resize(dataSize);
	for (int i = 0; i < dataSize; i++)
	{
		inputData[i].value1 = static_cast<float>(i) * 0.2f;
		inputData[i].value2 = static_cast<float>(i) * 0.3f;
	}

	std::shared_ptr<LLGI::Buffer> inputBuffer;
	inputBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapWrite | LLGI::BufferUsageType::CopySrc, sizeof(InputData) * dataSize));

	{
		auto data = (InputData*)inputBuffer->Lock();
		for (int i = 0; i < dataSize; i++)
		{
			data[i] = inputData[i];
		}
		inputBuffer->Unlock();
	}

	std::shared_ptr<LLGI::Buffer> inputStorageBuffer;
	inputStorageBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopyDst, sizeof(InputData) * dataSize));

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

			pip->SetShader(LLGI::ShaderStageType::Vertex, shader_vs.get());
			pip->SetShader(LLGI::ShaderStageType::Pixel, shader_ps.get());
			pip->SetRenderPassPipelineState(renderPassPipelineState.get());
			pip->Compile();

			pips[renderPassPipelineState] = LLGI::CreateSharedPtr(pip);
		}

		auto commandList = commandListPool->Get();
		commandList->Begin();
		commandList->CopyBuffer(inputBuffer.get(), inputStorageBuffer.get());
		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vb.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(ib.get(), 2);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->SetStorageBuffer(inputStorageBuffer.get(), sizeof(InputData), 0, LLGI::ShaderResourceAccess::ReadOnly);
		commandList->Draw(2, 5);
		commandList->EndRenderPass();
		commandList->End();

			graphics->Execute(commandList);

			if (TestHelper::GetIsCaptureRequired() && (count + 1) == 30)
			{
				commandListPool->WaitUntilCompleted();
				auto texture = renderPass->GetRenderTexture(0);
				auto data = graphics->CaptureRenderTarget(texture);

			// save
				std::string path = "SimpleRender.VertexStructured_" + TestHelper::GetDeviceName(deviceType) + ".png";
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat()).Save(path.c_str());
			}

			platform->Present();
			count++;
		}

	pips.clear();
}

void test_vtf(LLGI::DeviceType deviceType)
{
	int count = 0;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("VTF", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));

	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> shader_vs = nullptr;
	std::shared_ptr<LLGI::Shader> shader_ps = nullptr;

	TestHelper::CreateShader(graphics.get(), deviceType, "vtf.vert", "simple_rectangle.frag", shader_vs, shader_ps);

	LLGI::TextureInitializationParameter texParam;
	texParam.Size = LLGI::Vec2I(16, 16);
	texParam.Format = LLGI::TextureFormatType::R8G8B8A8_UNORM;
	auto textureDrawn = LLGI::CreateSharedPtr(graphics->CreateTexture(texParam));

	auto texBuf = static_cast<LLGI::Color8*>(textureDrawn->Lock());
	for (size_t i = 0; i < 16 * 16; i++)
	{
		texBuf[i].R = 127;
		texBuf[i].G = 127;
		texBuf[i].B = 127;
		texBuf[i].A = 127;
	}
	textureDrawn->Unlock();

	std::shared_ptr<LLGI::Buffer> vb;
	std::shared_ptr<LLGI::Buffer> ib;
	TestHelper::CreateRectangle(graphics.get(),
								LLGI::Vec3F(-0.2f, 0.2f, 0.5f),
								LLGI::Vec3F(0.2f, -0.2f, 0.5f),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(255, 255, 255, 255),
								vb,
								ib);

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

		auto renderPass = platform->GetCurrentScreen(color, true, false);
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

			pip->SetShader(LLGI::ShaderStageType::Vertex, shader_vs.get());
			pip->SetShader(LLGI::ShaderStageType::Pixel, shader_ps.get());
			pip->SetRenderPassPipelineState(renderPassPipelineState.get());
			pip->Compile();

			pips[renderPassPipelineState] = LLGI::CreateSharedPtr(pip);
		}

		auto commandList = commandListPool->Get();
		commandList->Begin();

		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vb.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(ib.get(), 2);
		commandList->SetTexture(textureDrawn.get(), LLGI::TextureWrapMode::Clamp, LLGI::TextureMinMagFilter::Linear, 0);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->Draw(2, 5);
		commandList->EndRenderPass();
		commandList->End();

			graphics->Execute(commandList);

			if (TestHelper::GetIsCaptureRequired() && (count + 1) == 30)
			{
				commandListPool->WaitUntilCompleted();
				auto texture = renderPass->GetRenderTexture(0);
				auto data = graphics->CaptureRenderTarget(texture);

			// save
				std::string path = "SimpleRender.VTF_" + TestHelper::GetDeviceName(deviceType) + ".png";
				Bitmap2D(data, texture->GetSizeAs2D().X, texture->GetSizeAs2D().Y, texture->GetFormat()).Save(path.c_str());
			}

			platform->Present();
			count++;
		}

	commandListPool->WaitUntilCompleted();
	graphics->WaitFinish();
	pips.clear();
}

TestRegister SimpleRender_BasicTriangle("SimpleRender.BasicTriangle", [](LLGI::DeviceType device) -> void {
	test_simple_rectangle(device, SingleRectangleTestMode::Triangle);
});

TestRegister SimpleRender_BasicLine("SimpleRender.BasicLine",
									[](LLGI::DeviceType device) -> void { test_simple_rectangle(device, SingleRectangleTestMode::Line); });

TestRegister SimpleRender_BasicPoint("SimpleRender.BasicPoint", [](LLGI::DeviceType device) -> void {
	test_simple_rectangle(device, SingleRectangleTestMode::Point);
});

TestRegister SimpleRender_IndexOffset("SimpleRender.IndexOffset", [](LLGI::DeviceType device) -> void { test_index_offset(device); });

TestRegister SimpleRender_ConstantLT("SimpleRender.ConstantLT", [](LLGI::DeviceType device) -> void {
	test_simple_constant_rectangle(LLGI::ConstantBufferType::LongTime, device);
});

TestRegister SimpleRender_ConstantST("SimpleRender.ConstantST", [](LLGI::DeviceType device) -> void {
	test_simple_constant_rectangle(LLGI::ConstantBufferType::ShortTime, device);
});

TestRegister SimpleRender_Instansing("SimpleRender.Instansing", [](LLGI::DeviceType device) -> void { test_instancing(device); });

TestRegister SimpleRender_VertexStructured("SimpleRender.VertexStructured",
										   [](LLGI::DeviceType device) -> void { test_vertex_structured(device); });

TestRegister SimpleRender_VTF("SimpleRender.VTF", [](LLGI::DeviceType device) -> void { test_vtf(device); });
