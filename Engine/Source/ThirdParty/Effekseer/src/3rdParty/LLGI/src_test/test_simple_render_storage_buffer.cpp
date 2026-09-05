#include "TestHelper.h"
#include "test.h"

#include <Utils/LLGI.CommandListPool.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

void test_storage_buffer_compute_to_vertex_raw_layout(LLGI::DeviceType deviceType)
{
	int count = 0;
	bool verified = false;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = false;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("StorageBuffer_ComputeToVertexRawLayout", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	VERIFY(platform != nullptr);
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
	VERIFY(graphics != nullptr);

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
	VERIFY(sfMemoryPool != nullptr);
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> shaderVS = nullptr;
	std::shared_ptr<LLGI::Shader> shaderPS = nullptr;
	std::shared_ptr<LLGI::Shader> shaderCS = nullptr;
	TestHelper::CreateShader(graphics.get(), deviceType, "storage_buffer_raw_transform_render.vert", "simple_rectangle.frag", shaderVS, shaderPS);
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_raw_transform_write.comp", shaderCS);
	VERIFY(shaderVS != nullptr);
	VERIFY(shaderPS != nullptr);
	VERIFY(shaderCS != nullptr);

	std::shared_ptr<LLGI::Buffer> vertexBuffer;
	std::shared_ptr<LLGI::Buffer> indexBuffer;
	TestHelper::CreateRectangle(graphics.get(),
								LLGI::Vec3F(-0.2f, 0.2f, 0.0f),
								LLGI::Vec3F(0.2f, -0.2f, 0.0f),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(255, 255, 255, 255),
								vertexBuffer,
								indexBuffer);
	VERIFY(vertexBuffer != nullptr);
	VERIFY(indexBuffer != nullptr);

	const int elementCount = 2;
	const int transformStride = 80;
	const int transformBindingStride = transformStride;
	auto transformBuffer = LLGI::CreateSharedPtr(graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite, transformStride * elementCount));
	VERIFY(transformBuffer != nullptr);

	auto computePipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	VERIFY(computePipeline != nullptr);
	computePipeline->SetShader(LLGI::ShaderStageType::Compute, shaderCS.get());
	VERIFY(computePipeline->Compile());

	std::map<std::shared_ptr<LLGI::RenderPassPipelineState>, std::shared_ptr<LLGI::PipelineState>> pips;

	while (count < 60)
	{
		if (!platform->NewFrame())
			break;

		sfMemoryPool->NewFrame();

		auto renderPass = platform->GetCurrentScreen(LLGI::Color8(0, 0, 0, 255), true, false);
		auto renderPassPipelineState = LLGI::CreateSharedPtr(graphics->CreateRenderPassPipelineState(renderPass));

		if (pips.count(renderPassPipelineState) == 0)
		{
			auto renderPipeline = graphics->CreatePiplineState();
			renderPipeline->VertexLayouts[0] = LLGI::VertexLayoutFormat::R32G32B32_FLOAT;
			renderPipeline->VertexLayouts[1] = LLGI::VertexLayoutFormat::R32G32_FLOAT;
			renderPipeline->VertexLayouts[2] = LLGI::VertexLayoutFormat::R8G8B8A8_UNORM;
			renderPipeline->VertexLayoutNames[0] = "POSITION";
			renderPipeline->VertexLayoutNames[1] = "UV";
			renderPipeline->VertexLayoutNames[2] = "COLOR";
			renderPipeline->VertexLayoutCount = 3;
			renderPipeline->SetShader(LLGI::ShaderStageType::Vertex, shaderVS.get());
			renderPipeline->SetShader(LLGI::ShaderStageType::Pixel, shaderPS.get());
			renderPipeline->SetRenderPassPipelineState(renderPassPipelineState.get());
			VERIFY(renderPipeline->Compile());

			pips[renderPassPipelineState] = LLGI::CreateSharedPtr(renderPipeline);
		}

		auto commandList = commandListPool->Get();
		commandList->Begin();
		commandList->BeginComputePass();
		commandList->SetPipelineState(computePipeline.get());
		commandList->SetStorageBuffer(transformBuffer.get(), transformBindingStride, 0, LLGI::ShaderResourceAccess::ReadWrite);
		commandList->Dispatch(elementCount, 1, 1, 1, 1, 1);
		commandList->EndComputePass();
		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vertexBuffer.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(indexBuffer.get(), 2);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->SetStorageBuffer(transformBuffer.get(), transformBindingStride, 0, LLGI::ShaderResourceAccess::ReadOnly);
		commandList->Draw(2, elementCount);
		commandList->EndRenderPass();
		commandList->End();

		graphics->Execute(commandList);

		if ((count + 1) == 30)
		{
			commandListPool->WaitUntilCompleted();
			auto texture = renderPass->GetRenderTexture(0);
			auto data = graphics->CaptureRenderTarget(texture);
			auto textureSize = texture->GetSizeAs2D();
			Bitmap2D bitmap(data, textureSize.X, textureSize.Y, texture->GetFormat());
			auto scaleX = [textureSize](int x) { return x * textureSize.X / 1280; };
			auto scaleY = [textureSize](int y) { return y * textureSize.Y / 720; };

			auto findColor = [&bitmap, scaleY](int xMin, int xMax, bool red) {
				for (int y = scaleY(280); y < scaleY(440); y++)
				{
					for (int x = xMin; x < xMax; x++)
					{
						const auto c = bitmap.GetPixel(x, y);
						if (red)
						{
							if (c.r > 200 && c.g < 80 && c.b < 80)
							{
								return true;
							}
						}
						else
						{
							if (c.g > 200 && c.r < 80 && c.b < 80)
							{
								return true;
							}
						}
					}
				}
				return false;
			};

			if (TestHelper::GetIsCaptureRequired())
			{
				std::string path = "SimpleRender.StorageBuffer.ComputeToVertexRawLayout_" + TestHelper::GetDeviceName(deviceType) + ".png";
				bitmap.Save(path.c_str());
			}

			VERIFY(findColor(scaleX(250), scaleX(520), true));
			VERIFY(findColor(scaleX(760), scaleX(1030), false));
			verified = true;
		}

		platform->Present();
		count++;
	}

	commandListPool->WaitUntilCompleted();
	graphics->WaitFinish();
	pips.clear();
	VERIFY(verified);
}

void test_storage_buffer_compute_to_vertex_multi_slot_raw_layout(LLGI::DeviceType deviceType)
{
	int count = 0;
	bool verified = false;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = false;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("StorageBuffer_ComputeToVertexMultiSlotRawLayout", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	VERIFY(platform != nullptr);
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
	VERIFY(graphics != nullptr);

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
	VERIFY(sfMemoryPool != nullptr);
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> shaderVS = nullptr;
	std::shared_ptr<LLGI::Shader> shaderPS = nullptr;
	std::shared_ptr<LLGI::Shader> shaderCS = nullptr;
	TestHelper::CreateShader(graphics.get(), deviceType, "storage_buffer_multi_slot_raw_render.vert", "simple_rectangle.frag", shaderVS, shaderPS);
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_multi_slot_raw_write.comp", shaderCS);
	VERIFY(shaderVS != nullptr);
	VERIFY(shaderPS != nullptr);
	VERIFY(shaderCS != nullptr);

	std::shared_ptr<LLGI::Buffer> vertexBuffer;
	std::shared_ptr<LLGI::Buffer> indexBuffer;
	TestHelper::CreateRectangle(graphics.get(),
								LLGI::Vec3F(-0.2f, 0.2f, 0.0f),
								LLGI::Vec3F(0.2f, -0.2f, 0.0f),
								LLGI::Color8(255, 255, 255, 255),
								LLGI::Color8(255, 255, 255, 255),
								vertexBuffer,
								indexBuffer);
	VERIFY(vertexBuffer != nullptr);
	VERIFY(indexBuffer != nullptr);

	const int elementCount = 3;
	const int transformStride = 80;
	const int attributeStride = 16;
	const int transformBindingStride = transformStride;
	const int attributeBindingStride = attributeStride;
	auto transformBuffer = LLGI::CreateSharedPtr(graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite, transformStride * elementCount));
	auto attributeBuffer = LLGI::CreateSharedPtr(graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite, attributeStride * elementCount));
	VERIFY(transformBuffer != nullptr);
	VERIFY(attributeBuffer != nullptr);

	auto computePipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	VERIFY(computePipeline != nullptr);
	computePipeline->SetShader(LLGI::ShaderStageType::Compute, shaderCS.get());
	VERIFY(computePipeline->Compile());

	std::map<std::shared_ptr<LLGI::RenderPassPipelineState>, std::shared_ptr<LLGI::PipelineState>> pips;

	while (count < 60)
	{
		if (!platform->NewFrame())
			break;

		sfMemoryPool->NewFrame();

		auto renderPass = platform->GetCurrentScreen(LLGI::Color8(0, 0, 0, 255), true, false);
		auto renderPassPipelineState = LLGI::CreateSharedPtr(graphics->CreateRenderPassPipelineState(renderPass));

		if (pips.count(renderPassPipelineState) == 0)
		{
			auto renderPipeline = graphics->CreatePiplineState();
			renderPipeline->VertexLayouts[0] = LLGI::VertexLayoutFormat::R32G32B32_FLOAT;
			renderPipeline->VertexLayouts[1] = LLGI::VertexLayoutFormat::R32G32_FLOAT;
			renderPipeline->VertexLayouts[2] = LLGI::VertexLayoutFormat::R8G8B8A8_UNORM;
			renderPipeline->VertexLayoutNames[0] = "POSITION";
			renderPipeline->VertexLayoutNames[1] = "UV";
			renderPipeline->VertexLayoutNames[2] = "COLOR";
			renderPipeline->VertexLayoutCount = 3;
			renderPipeline->SetShader(LLGI::ShaderStageType::Vertex, shaderVS.get());
			renderPipeline->SetShader(LLGI::ShaderStageType::Pixel, shaderPS.get());
			renderPipeline->SetRenderPassPipelineState(renderPassPipelineState.get());
			VERIFY(renderPipeline->Compile());

			pips[renderPassPipelineState] = LLGI::CreateSharedPtr(renderPipeline);
		}

		auto commandList = commandListPool->Get();
		commandList->Begin();
		commandList->BeginComputePass();
		commandList->SetPipelineState(computePipeline.get());
		commandList->SetStorageBuffer(transformBuffer.get(), transformBindingStride, 0, LLGI::ShaderResourceAccess::ReadWrite);
		commandList->SetStorageBuffer(attributeBuffer.get(), attributeBindingStride, 1, LLGI::ShaderResourceAccess::ReadWrite);
		commandList->Dispatch(elementCount, 1, 1, 1, 1, 1);
		commandList->EndComputePass();
		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vertexBuffer.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(indexBuffer.get(), 2);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->SetStorageBuffer(transformBuffer.get(), transformBindingStride, 0, LLGI::ShaderResourceAccess::ReadOnly);
		commandList->SetStorageBuffer(attributeBuffer.get(), attributeBindingStride, 1, LLGI::ShaderResourceAccess::ReadOnly);
		commandList->Draw(2, elementCount);
		commandList->EndRenderPass();
		commandList->End();

		graphics->Execute(commandList);

		if ((count + 1) == 30)
		{
			commandListPool->WaitUntilCompleted();
			auto texture = renderPass->GetRenderTexture(0);
			auto data = graphics->CaptureRenderTarget(texture);
			auto textureSize = texture->GetSizeAs2D();
			Bitmap2D bitmap(data, textureSize.X, textureSize.Y, texture->GetFormat());
			auto scaleX = [textureSize](int x) { return x * textureSize.X / 1280; };
			auto scaleY = [textureSize](int y) { return y * textureSize.Y / 720; };

			auto findColor = [&bitmap](int xMin, int xMax, int yMin, int yMax, int channel) {
				for (int y = yMin; y < yMax; y++)
				{
					for (int x = xMin; x < xMax; x++)
					{
						const auto c = bitmap.GetPixel(x, y);
						const int values[3] = {c.r, c.g, c.b};
						if (values[channel] > 180 && values[(channel + 1) % 3] < 100 && values[(channel + 2) % 3] < 100)
						{
							return true;
						}
					}
				}
				return false;
			};

			if (TestHelper::GetIsCaptureRequired())
			{
				std::string path = "SimpleRender.StorageBuffer.ComputeToVertexMultiSlotRawLayout_" + TestHelper::GetDeviceName(deviceType) + ".png";
				bitmap.Save(path.c_str());
			}

			VERIFY(findColor(scaleX(210), scaleX(430), scaleY(330), scaleY(520), 0));
			VERIFY(findColor(scaleX(520), scaleX(760), scaleY(250), scaleY(470), 1));
			VERIFY(findColor(scaleX(850), scaleX(1070), scaleY(170), scaleY(390), 2));
			verified = true;
		}

		platform->Present();
		count++;
	}

	commandListPool->WaitUntilCompleted();
	graphics->WaitFinish();
	pips.clear();
	VERIFY(verified);
}

void test_storage_buffer_history_ribbon_vertex_expansion(LLGI::DeviceType deviceType)
{
	int count = 0;
	bool verified = false;

	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = false;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("StorageBuffer_HistoryRibbonVertexExpansion", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	VERIFY(platform != nullptr);
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());
	VERIFY(graphics != nullptr);

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
	VERIFY(sfMemoryPool != nullptr);
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> shaderVS = nullptr;
	std::shared_ptr<LLGI::Shader> shaderPS = nullptr;
	std::shared_ptr<LLGI::Shader> shaderCS = nullptr;
	TestHelper::CreateShader(graphics.get(), deviceType, "storage_buffer_history_ribbon_render.vert", "simple_rectangle.frag", shaderVS, shaderPS);
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_history_ribbon_write.comp", shaderCS);
	VERIFY(shaderVS != nullptr);
	VERIFY(shaderPS != nullptr);
	VERIFY(shaderCS != nullptr);

	constexpr int vertexCount = 8;
	constexpr int indexCount = 18;
	auto vertexBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::Vertex | LLGI::BufferUsageType::MapWrite, sizeof(SimpleVertex) * vertexCount));
	auto indexBuffer =
		LLGI::CreateSharedPtr(graphics->CreateBuffer(LLGI::BufferUsageType::Index | LLGI::BufferUsageType::MapWrite, sizeof(uint16_t) * indexCount));
	VERIFY(vertexBuffer != nullptr);
	VERIFY(indexBuffer != nullptr);

	{
		auto vertices = static_cast<SimpleVertex*>(vertexBuffer->Lock());
		for (int i = 0; i < vertexCount; i++)
		{
			vertices[i].Pos = LLGI::Vec3F(0.0f, 0.0f, 0.0f);
			vertices[i].UV = LLGI::Vec2F(0.0f, 0.0f);
			vertices[i].Color = LLGI::Color8(255, 255, 255, 255);
		}
		vertexBuffer->Unlock();

		const uint16_t indices[indexCount] = {
			0, 2, 3, 0, 3, 1,
			2, 4, 5, 2, 5, 3,
			4, 6, 7, 4, 7, 5,
		};
		auto indexData = static_cast<uint16_t*>(indexBuffer->Lock());
		for (int i = 0; i < indexCount; i++)
		{
			indexData[i] = indices[i];
		}
		indexBuffer->Unlock();
	}

	const int historyRecordStride = 32;
	const int ribbonRecordStride = 32;
	const int historyBindingStride = historyRecordStride;
	const int ribbonBindingStride = ribbonRecordStride;
	auto historyBuffer = LLGI::CreateSharedPtr(graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite, historyRecordStride * vertexCount));
	auto ribbonBuffer = LLGI::CreateSharedPtr(graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite, ribbonRecordStride * 2));
	VERIFY(historyBuffer != nullptr);
	VERIFY(ribbonBuffer != nullptr);

	auto computePipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	VERIFY(computePipeline != nullptr);
	computePipeline->SetShader(LLGI::ShaderStageType::Compute, shaderCS.get());
	VERIFY(computePipeline->Compile());

	std::map<std::shared_ptr<LLGI::RenderPassPipelineState>, std::shared_ptr<LLGI::PipelineState>> pips;

	while (count < 60)
	{
		if (!platform->NewFrame())
			break;

		sfMemoryPool->NewFrame();

		auto renderPass = platform->GetCurrentScreen(LLGI::Color8(0, 0, 0, 255), true, false);
		auto renderPassPipelineState = LLGI::CreateSharedPtr(graphics->CreateRenderPassPipelineState(renderPass));

		if (pips.count(renderPassPipelineState) == 0)
		{
			auto renderPipeline = graphics->CreatePiplineState();
			renderPipeline->VertexLayouts[0] = LLGI::VertexLayoutFormat::R32G32B32_FLOAT;
			renderPipeline->VertexLayouts[1] = LLGI::VertexLayoutFormat::R32G32_FLOAT;
			renderPipeline->VertexLayouts[2] = LLGI::VertexLayoutFormat::R8G8B8A8_UNORM;
			renderPipeline->VertexLayoutNames[0] = "POSITION";
			renderPipeline->VertexLayoutNames[1] = "UV";
			renderPipeline->VertexLayoutNames[2] = "COLOR";
			renderPipeline->VertexLayoutCount = 3;
			renderPipeline->SetShader(LLGI::ShaderStageType::Vertex, shaderVS.get());
			renderPipeline->SetShader(LLGI::ShaderStageType::Pixel, shaderPS.get());
			renderPipeline->SetRenderPassPipelineState(renderPassPipelineState.get());
			VERIFY(renderPipeline->Compile());

			pips[renderPassPipelineState] = LLGI::CreateSharedPtr(renderPipeline);
		}

		auto commandList = commandListPool->Get();
		commandList->Begin();
		commandList->BeginComputePass();
		commandList->SetPipelineState(computePipeline.get());
		commandList->SetStorageBuffer(historyBuffer.get(), historyBindingStride, 0, LLGI::ShaderResourceAccess::ReadWrite);
		commandList->SetStorageBuffer(ribbonBuffer.get(), ribbonBindingStride, 1, LLGI::ShaderResourceAccess::ReadWrite);
		commandList->Dispatch(vertexCount, 1, 1, 1, 1, 1);
		commandList->EndComputePass();
		commandList->BeginRenderPass(renderPass);
		commandList->SetVertexBuffer(vertexBuffer.get(), sizeof(SimpleVertex), 0);
		commandList->SetIndexBuffer(indexBuffer.get(), 2);
		commandList->SetPipelineState(pips[renderPassPipelineState].get());
		commandList->SetStorageBuffer(historyBuffer.get(), historyBindingStride, 0, LLGI::ShaderResourceAccess::ReadOnly);
		commandList->SetStorageBuffer(ribbonBuffer.get(), ribbonBindingStride, 1, LLGI::ShaderResourceAccess::ReadOnly);
		commandList->Draw(6, 2);
		commandList->EndRenderPass();
		commandList->End();

		graphics->Execute(commandList);

		if ((count + 1) == 30)
		{
			commandListPool->WaitUntilCompleted();
			auto texture = renderPass->GetRenderTexture(0);
			auto data = graphics->CaptureRenderTarget(texture);
			auto textureSize = texture->GetSizeAs2D();
			Bitmap2D bitmap(data, textureSize.X, textureSize.Y, texture->GetFormat());
			auto scaleX = [textureSize](int x) { return x * textureSize.X / 1280; };
			auto scaleY = [textureSize](int y) { return y * textureSize.Y / 720; };

			auto findColor = [&bitmap, scaleY](int xMin, int xMax, int channel) {
				for (int y = scaleY(80); y < scaleY(640); y++)
				{
					for (int x = xMin; x < xMax; x++)
					{
						const auto c = bitmap.GetPixel(x, y);
						const int values[3] = {c.r, c.g, c.b};
						if (values[channel] > 180 && values[(channel + 1) % 3] < 100 && values[(channel + 2) % 3] < 100)
						{
							return true;
						}
					}
				}
				return false;
			};

			if (TestHelper::GetIsCaptureRequired())
			{
				std::string path = "SimpleRender.StorageBuffer.HistoryRibbonVertexExpansion_" + TestHelper::GetDeviceName(deviceType) + ".png";
				bitmap.Save(path.c_str());
			}

			VERIFY(findColor(scaleX(80), scaleX(620), 0));
			VERIFY(findColor(scaleX(660), scaleX(1220), 1));
			verified = true;
		}

		platform->Present();
		count++;
	}

	commandListPool->WaitUntilCompleted();
	graphics->WaitFinish();
	pips.clear();
	VERIFY(verified);
}

TestRegister SimpleRender_StorageBufferComputeToVertexRawLayout("SimpleRender.StorageBuffer.ComputeToVertexRawLayout",
																[](LLGI::DeviceType device) -> void {
																	test_storage_buffer_compute_to_vertex_raw_layout(device);
																});

TestRegister SimpleRender_StorageBufferComputeToVertexMultiSlotRawLayout("SimpleRender.StorageBuffer.ComputeToVertexMultiSlotRawLayout",
																		 [](LLGI::DeviceType device) -> void {
																			 test_storage_buffer_compute_to_vertex_multi_slot_raw_layout(device);
																		 });

TestRegister SimpleRender_StorageBufferHistoryRibbonVertexExpansion("SimpleRender.StorageBuffer.HistoryRibbonVertexExpansion",
																	[](LLGI::DeviceType device) -> void {
																		test_storage_buffer_history_ribbon_vertex_expansion(device);
																	});
