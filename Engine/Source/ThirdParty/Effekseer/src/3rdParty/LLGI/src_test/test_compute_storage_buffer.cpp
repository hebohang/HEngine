#include "TestHelper.h"
#include "test.h"

#include <LLGI.Buffer.h>
#include <Utils/LLGI.CommandListPool.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

struct StructuredInputData
{
	float position[3];
	uint32_t reserved;
	uint32_t normal;
	uint32_t tangent;
	uint32_t uv;
	uint32_t color;
};

struct StructuredOutputData
{
	float position[3];
	uint32_t normal;
	uint32_t tangent;
	uint32_t color;
	uint32_t padding0;
	uint32_t padding1;
};

struct SlotTransitionData
{
	float position[3];
	uint32_t direction;
};

struct SlotTransitionOutputData
{
	float position[3];
	uint32_t direction;
	uint32_t checksum;
	uint32_t padding0;
	uint32_t padding1;
	uint32_t padding2;
};

struct MultiSlotInputA
{
	float values[3];
	uint32_t tag;
};

struct MultiSlotInputB
{
	uint32_t x;
	uint32_t y;
	uint32_t z;
	uint32_t w;
};

struct MultiSlotOutputA
{
	float value;
	uint32_t tag;
	uint32_t mix;
	uint32_t padding;
};

struct MultiSlotOutputB
{
	float values[3];
	uint32_t checksum;
};

struct StructuredMatrixRecordInput
{
	uint32_t flags;
	float age;
	uint32_t packed[2];
	float transform[4][3];
	float direction[3];
	uint32_t color;
};

struct StructuredMatrixRecordOutput
{
	float translation[3];
	uint32_t flags;
	float direction[3];
	uint32_t color;
	float axis0[3];
	uint32_t checksum;
};

struct MatrixIndexingInput
{
	float clip[4][4];
	float transform[4][3];
	float local[4];
	uint32_t row;
	uint32_t flags;
	uint32_t padding[2];
};

struct MatrixIndexingOutput
{
	float clip[4];
	float indexed[3];
	uint32_t checksum;
};

static bool NearlyEqual(float a, float b, float epsilon = 0.001f)
{
	return std::fabs(a - b) <= epsilon;
}

static uint32_t FloatAsUint(float value)
{
	uint32_t ret = 0;
	std::memcpy(&ret, &value, sizeof(ret));
	return ret;
}

static void TransformPoint(float dst[3], const float transform[4][3], const float local[4])
{
	for (int axis = 0; axis < 3; axis++)
	{
		dst[axis] = local[0] * transform[0][axis] + local[1] * transform[1][axis] + local[2] * transform[2][axis] +
					local[3] * transform[3][axis];
	}
}

static void TransformClip(float dst[4], const float clip[4][4], const float point[4])
{
	for (int row = 0; row < 4; row++)
	{
		dst[row] = clip[row][0] * point[0] + clip[row][1] * point[1] + clip[row][2] * point[2] + clip[row][3] * point[3];
	}
}

void test_compute_shader_storage_buffer_structured_slot1(LLGI::DeviceType deviceType)
{
	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("ComputeShaderStorageBufferStructuredSlot1", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> shaderCS = nullptr;
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_structured_slot1.comp", shaderCS);

	auto pipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	pipeline->SetShader(LLGI::ShaderStageType::Compute, shaderCS.get());
	if (!pipeline->Compile())
	{
		abort();
	}

	const int dataSize = 64;
	std::vector<StructuredInputData> inputData(dataSize);
	for (int i = 0; i < dataSize; i++)
	{
		inputData[i].position[0] = i * 1.25f + 0.5f;
		inputData[i].position[1] = i * -0.75f + 2.0f;
		inputData[i].position[2] = i * 0.5f - 3.0f;
		inputData[i].reserved = 0x11110000u + i;
		inputData[i].normal = 0x00200300u + i * 17u;
		inputData[i].tangent = 0x00100400u + i * 19u;
		inputData[i].uv = 0x01020000u + i * 23u;
		inputData[i].color = 0x80402010u + i * 29u;
	}

	auto uploadBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapWrite | LLGI::BufferUsageType::CopySrc, sizeof(StructuredInputData) * dataSize));
	auto storageBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageRead | LLGI::BufferUsageType::CopyDst, sizeof(StructuredInputData) * dataSize));
	auto outputStorageBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, sizeof(StructuredOutputData) * dataSize));
	auto readbackBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapRead | LLGI::BufferUsageType::CopyDst, sizeof(StructuredOutputData) * dataSize));

	{
		auto data = static_cast<StructuredInputData*>(uploadBuffer->Lock());
		for (int i = 0; i < dataSize; i++)
		{
			data[i] = inputData[i];
		}
		uploadBuffer->Unlock();
	}

	if (!platform->NewFrame())
	{
		return;
	}

	sfMemoryPool->NewFrame();
	const int32_t bufferStride = sizeof(StructuredInputData);
	const int32_t outputStride = sizeof(StructuredOutputData);

	auto commandList = commandListPool->Get();
	commandList->Begin();
	commandList->CopyBuffer(uploadBuffer.get(), storageBuffer.get());
	commandList->BeginComputePass();
	commandList->SetPipelineState(pipeline.get());
	commandList->SetStorageBuffer(outputStorageBuffer.get(), outputStride, 0, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->SetStorageBuffer(storageBuffer.get(), bufferStride, 1, LLGI::ShaderResourceAccess::ReadOnly);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->CopyBuffer(outputStorageBuffer.get(), readbackBuffer.get());
	commandList->End();

	graphics->Execute(commandList);
	graphics->WaitFinish();

	auto result = static_cast<const StructuredOutputData*>(readbackBuffer->Lock());
	if (result == nullptr)
	{
		abort();
	}

	for (int i = 0; i < dataSize; i++)
	{
		const auto& expected = inputData[i];
		const auto& actual = result[i];
		if (actual.position[0] != expected.position[0] + 1.0f ||
			actual.position[1] != expected.position[1] + 2.0f ||
			actual.position[2] != expected.position[2] + 3.0f ||
			actual.normal != expected.normal ||
			actual.tangent != expected.tangent ||
			actual.color != (expected.color ^ expected.uv ^ expected.reserved))
		{
			std::cout << "Failed : StructuredSlot1 Mismatch " << i << std::endl;
			abort();
		}
	}

	readbackBuffer->Unlock();
	platform->Present();
}

void test_compute_shader_storage_buffer_slot1_pass_transition(LLGI::DeviceType deviceType)
{
	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("ComputeShaderStorageBufferSlot1Transition", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> writeShader = nullptr;
	std::shared_ptr<LLGI::Shader> readShader = nullptr;
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_write_slot1.comp", writeShader);
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_read_slot1.comp", readShader);

	auto writePipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	writePipeline->SetShader(LLGI::ShaderStageType::Compute, writeShader.get());
	if (!writePipeline->Compile())
	{
		abort();
	}

	auto readPipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	readPipeline->SetShader(LLGI::ShaderStageType::Compute, readShader.get());
	if (!readPipeline->Compile())
	{
		abort();
	}

	const int dataSize = 64;
	auto storageBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, sizeof(SlotTransitionData) * dataSize));
	auto outputStorageBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, sizeof(SlotTransitionOutputData) * dataSize));
	auto readbackBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapRead | LLGI::BufferUsageType::CopyDst, sizeof(SlotTransitionOutputData) * dataSize));

	if (!platform->NewFrame())
	{
		return;
	}

	sfMemoryPool->NewFrame();
	const int32_t bufferStride = sizeof(SlotTransitionData);
	const int32_t outputStride = sizeof(SlotTransitionOutputData);

	auto commandList = commandListPool->Get();
	commandList->Begin();
	commandList->BeginComputePass();
	commandList->SetPipelineState(writePipeline.get());
	commandList->SetStorageBuffer(storageBuffer.get(), bufferStride, 1, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->BeginComputePass();
	commandList->SetPipelineState(readPipeline.get());
	commandList->SetStorageBuffer(outputStorageBuffer.get(), outputStride, 0, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->SetStorageBuffer(storageBuffer.get(), bufferStride, 1, LLGI::ShaderResourceAccess::ReadOnly);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->CopyBuffer(outputStorageBuffer.get(), readbackBuffer.get());
	commandList->End();

	graphics->Execute(commandList);
	graphics->WaitFinish();

	auto result = static_cast<const SlotTransitionOutputData*>(readbackBuffer->Lock());
	if (result == nullptr)
	{
		abort();
	}

	for (int i = 0; i < dataSize; i++)
	{
		const float x = i * 2.0f + 0.25f;
		const float y = i * -3.0f + 0.5f;
		const float z = i * 4.0f - 0.75f;
		const uint32_t direction = 0x12340000u + i * 31u;
		const uint32_t checksum = direction ^ static_cast<uint32_t>(i * 0x01010101u);
		const auto& actual = result[i];
		if (actual.position[0] != x + 10.0f ||
			actual.position[1] != y + 20.0f ||
			actual.position[2] != z + 30.0f ||
			actual.direction != direction ||
			actual.checksum != checksum)
		{
			std::cout << "Failed : Slot1PassTransition Mismatch " << i << std::endl;
			abort();
		}
	}

	readbackBuffer->Unlock();
	platform->Present();
}

void test_compute_shader_storage_buffer_multi_slot_read_write(LLGI::DeviceType deviceType)
{
	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("ComputeShaderStorageBufferMultiSlot", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> shaderCS = nullptr;
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_multi_slot_read_write.comp", shaderCS);

	auto pipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	pipeline->SetShader(LLGI::ShaderStageType::Compute, shaderCS.get());
	if (!pipeline->Compile())
	{
		abort();
	}

	const int dataSize = 64;
	std::vector<MultiSlotInputA> inputA(dataSize);
	std::vector<MultiSlotInputB> inputB(dataSize);
	for (int i = 0; i < dataSize; i++)
	{
		inputA[i].values[0] = i + 0.25f;
		inputA[i].values[1] = i * -2.0f + 0.5f;
		inputA[i].values[2] = i * 3.0f - 0.75f;
		inputA[i].tag = 0x33000000u + i * 7u;
		inputB[i].x = 0x01000000u + i * 11u;
		inputB[i].y = 0x00200000u + i * 13u;
		inputB[i].z = 0x00030000u + i * 17u;
		inputB[i].w = 0x00004000u + i * 19u;
	}

	auto uploadA = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapWrite | LLGI::BufferUsageType::CopySrc, sizeof(MultiSlotInputA) * dataSize));
	auto uploadB = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapWrite | LLGI::BufferUsageType::CopySrc, sizeof(MultiSlotInputB) * dataSize));
	auto inputBufferA = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageRead | LLGI::BufferUsageType::CopyDst, sizeof(MultiSlotInputA) * dataSize));
	auto inputBufferB = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageRead | LLGI::BufferUsageType::CopyDst, sizeof(MultiSlotInputB) * dataSize));
	auto outputBufferA = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, sizeof(MultiSlotOutputA) * dataSize));
	auto outputBufferB = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, sizeof(MultiSlotOutputB) * dataSize));
	auto readbackA = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapRead | LLGI::BufferUsageType::CopyDst, sizeof(MultiSlotOutputA) * dataSize));
	auto readbackB = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapRead | LLGI::BufferUsageType::CopyDst, sizeof(MultiSlotOutputB) * dataSize));

	{
		auto dataA = static_cast<MultiSlotInputA*>(uploadA->Lock());
		auto dataB = static_cast<MultiSlotInputB*>(uploadB->Lock());
		for (int i = 0; i < dataSize; i++)
		{
			dataA[i] = inputA[i];
			dataB[i] = inputB[i];
		}
		uploadA->Unlock();
		uploadB->Unlock();
	}

	if (!platform->NewFrame())
	{
		return;
	}

	sfMemoryPool->NewFrame();
	const int32_t inputStrideA = sizeof(MultiSlotInputA);
	const int32_t inputStrideB = sizeof(MultiSlotInputB);
	const int32_t outputStrideA = sizeof(MultiSlotOutputA);
	const int32_t outputStrideB = sizeof(MultiSlotOutputB);

	auto commandList = commandListPool->Get();
	commandList->Begin();
	commandList->CopyBuffer(uploadA.get(), inputBufferA.get());
	commandList->CopyBuffer(uploadB.get(), inputBufferB.get());
	commandList->BeginComputePass();
	commandList->SetPipelineState(pipeline.get());
	commandList->SetStorageBuffer(outputBufferA.get(), outputStrideA, 0, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->SetStorageBuffer(inputBufferA.get(), inputStrideA, 1, LLGI::ShaderResourceAccess::ReadOnly);
	commandList->SetStorageBuffer(inputBufferB.get(), inputStrideB, 2, LLGI::ShaderResourceAccess::ReadOnly);
	commandList->SetStorageBuffer(outputBufferB.get(), outputStrideB, 3, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->CopyBuffer(outputBufferA.get(), readbackA.get());
	commandList->CopyBuffer(outputBufferB.get(), readbackB.get());
	commandList->End();

	graphics->Execute(commandList);
	graphics->WaitFinish();

	auto resultA = static_cast<const MultiSlotOutputA*>(readbackA->Lock());
	auto resultB = static_cast<const MultiSlotOutputB*>(readbackB->Lock());
	if (resultA == nullptr || resultB == nullptr)
	{
		abort();
	}

	for (int i = 0; i < dataSize; i++)
	{
		const auto mix = inputA[i].tag ^ inputB[i].x ^ inputB[i].y ^ inputB[i].z ^ inputB[i].w;
		const auto expectedValue = inputA[i].values[0] + inputA[i].values[1] + inputA[i].values[2] + static_cast<float>(inputB[i].x & 0xffu);
		if (resultA[i].value != expectedValue ||
			resultA[i].tag != inputA[i].tag ||
			resultA[i].mix != mix ||
			resultB[i].values[0] != inputA[i].values[0] * 2.0f ||
			resultB[i].values[1] != inputA[i].values[1] * 3.0f ||
			resultB[i].values[2] != inputA[i].values[2] * 4.0f ||
			resultB[i].checksum != mix + static_cast<uint32_t>(i))
		{
			std::cout << "Failed : MultiSlotReadWrite Mismatch " << i << std::endl;
			abort();
		}
	}

	readbackA->Unlock();
	readbackB->Unlock();
	platform->Present();
}

void test_compute_shader_storage_buffer_structured_matrix_record(LLGI::DeviceType deviceType)
{
	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("ComputeShaderStorageBufferStructuredMatrixRecord", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> writeShader = nullptr;
	std::shared_ptr<LLGI::Shader> readShader = nullptr;
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_structured_matrix_record_write.comp", writeShader);
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_structured_matrix_record.comp", readShader);

	auto writePipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	writePipeline->SetShader(LLGI::ShaderStageType::Compute, writeShader.get());
	if (!writePipeline->Compile())
	{
		abort();
	}

	auto readPipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	readPipeline->SetShader(LLGI::ShaderStageType::Compute, readShader.get());
	if (!readPipeline->Compile())
	{
		abort();
	}

	const int dataSize = 32;
	std::vector<StructuredMatrixRecordInput> inputData(dataSize);
	for (int i = 0; i < dataSize; i++)
	{
		auto& input = inputData[i];
		input.flags = 0x12000000u + i * 17u;
		input.age = 0.5f + i * 0.125f;
		input.packed[0] = 0x00100000u + i * 19u;
		input.packed[1] = 0x00020000u + i * 23u;
		for (int row = 0; row < 4; row++)
		{
			for (int column = 0; column < 3; column++)
			{
				input.transform[row][column] = static_cast<float>((i + 1) * (row + 2) + column) * 0.125f;
			}
		}
		input.transform[3][0] = -0.75f + i * 0.03125f;
		input.transform[3][1] = 0.25f - i * 0.015625f;
		input.transform[3][2] = 1.5f + i * 0.0625f;
		input.direction[0] = 0.25f + i * 0.015625f;
		input.direction[1] = -0.5f + i * 0.03125f;
		input.direction[2] = 0.75f - i * 0.0078125f;
		input.color = 0x80402010u + i * 29u;
	}

	auto recordBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite, sizeof(StructuredMatrixRecordInput) * dataSize));
	auto outputStorageBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, sizeof(StructuredMatrixRecordOutput) * dataSize));
	auto readbackBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapRead | LLGI::BufferUsageType::CopyDst, sizeof(StructuredMatrixRecordOutput) * dataSize));

	if (!platform->NewFrame())
	{
		return;
	}

	sfMemoryPool->NewFrame();

	auto commandList = commandListPool->Get();
	commandList->Begin();
	commandList->BeginComputePass();
	commandList->SetPipelineState(writePipeline.get());
	commandList->SetStorageBuffer(recordBuffer.get(), sizeof(StructuredMatrixRecordInput), 1, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->BeginComputePass();
	commandList->SetPipelineState(readPipeline.get());
	commandList->SetStorageBuffer(outputStorageBuffer.get(), sizeof(StructuredMatrixRecordOutput), 0, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->SetStorageBuffer(recordBuffer.get(), sizeof(StructuredMatrixRecordInput), 1, LLGI::ShaderResourceAccess::ReadOnly);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->CopyBuffer(outputStorageBuffer.get(), readbackBuffer.get());
	commandList->End();

	graphics->Execute(commandList);
	graphics->WaitFinish();

	auto result = static_cast<const StructuredMatrixRecordOutput*>(readbackBuffer->Lock());
	if (result == nullptr)
	{
		abort();
	}

	for (int i = 0; i < dataSize; i++)
	{
		const auto& input = inputData[i];
		const auto& actual = result[i];
		const uint32_t expectedFlags = input.flags ^ FloatAsUint(input.age);
		const uint32_t expectedChecksum = input.flags ^ input.packed[0] ^ input.packed[1] ^ input.color;
		if (actual.flags != expectedFlags || actual.color != input.color || actual.checksum != expectedChecksum)
		{
			std::cout << "Failed : StructuredMatrixRecord Scalar Mismatch " << i << std::endl;
			abort();
		}

		for (int axis = 0; axis < 3; axis++)
		{
			const float expectedDirection = input.direction[axis] + input.transform[2][axis] * 0.25f;
			const float expectedAxis0 = input.transform[0][axis] * input.age + input.direction[axis];
			if (!NearlyEqual(actual.translation[axis], input.transform[3][axis]) ||
				!NearlyEqual(actual.direction[axis], expectedDirection) ||
				!NearlyEqual(actual.axis0[axis], expectedAxis0))
			{
				std::cout << "Failed : StructuredMatrixRecord Float Mismatch " << i << std::endl;
				abort();
			}
		}
	}

	readbackBuffer->Unlock();
	platform->Present();
}

void test_compute_shader_storage_buffer_matrix_indexing(LLGI::DeviceType deviceType)
{
	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("ComputeShaderStorageBufferMatrixIndexing", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> writeShader = nullptr;
	std::shared_ptr<LLGI::Shader> readShader = nullptr;
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_matrix_indexing_write.comp", writeShader);
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_matrix_indexing.comp", readShader);

	auto writePipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	writePipeline->SetShader(LLGI::ShaderStageType::Compute, writeShader.get());
	if (!writePipeline->Compile())
	{
		abort();
	}

	auto readPipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	readPipeline->SetShader(LLGI::ShaderStageType::Compute, readShader.get());
	if (!readPipeline->Compile())
	{
		abort();
	}

	const int dataSize = 24;
	std::vector<MatrixIndexingInput> inputData(dataSize);
	for (int i = 0; i < dataSize; i++)
	{
		auto& input = inputData[i];
		for (int row = 0; row < 4; row++)
		{
			for (int column = 0; column < 4; column++)
			{
				input.clip[row][column] = (row == column ? 1.0f : 0.0f) + static_cast<float>((i + 1) * (row + 1) - column) * 0.03125f;
			}
		}
		for (int row = 0; row < 4; row++)
		{
			for (int column = 0; column < 3; column++)
			{
				input.transform[row][column] = static_cast<float>((row + 1) * (column + 2)) * 0.125f + i * 0.0078125f;
			}
		}
		input.transform[3][0] = -0.5f + i * 0.046875f;
		input.transform[3][1] = 0.375f - i * 0.0234375f;
		input.transform[3][2] = 0.25f + i * 0.015625f;
		input.local[0] = 0.25f + i * 0.015625f;
		input.local[1] = -0.5f + i * 0.0078125f;
		input.local[2] = 0.75f - i * 0.00390625f;
		input.local[3] = 1.0f;
		input.row = static_cast<uint32_t>(i % 4);
		input.flags = 0x45000000u + i * 31u;
		input.padding[0] = 0;
		input.padding[1] = 0;
	}

	auto recordBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite, sizeof(MatrixIndexingInput) * dataSize));
	auto outputStorageBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, sizeof(MatrixIndexingOutput) * dataSize));
	auto readbackBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapRead | LLGI::BufferUsageType::CopyDst, sizeof(MatrixIndexingOutput) * dataSize));

	if (!platform->NewFrame())
	{
		return;
	}

	sfMemoryPool->NewFrame();

	auto commandList = commandListPool->Get();
	commandList->Begin();
	commandList->BeginComputePass();
	commandList->SetPipelineState(writePipeline.get());
	commandList->SetStorageBuffer(recordBuffer.get(), sizeof(MatrixIndexingInput), 1, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->BeginComputePass();
	commandList->SetPipelineState(readPipeline.get());
	commandList->SetStorageBuffer(outputStorageBuffer.get(), sizeof(MatrixIndexingOutput), 0, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->SetStorageBuffer(recordBuffer.get(), sizeof(MatrixIndexingInput), 1, LLGI::ShaderResourceAccess::ReadOnly);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->CopyBuffer(outputStorageBuffer.get(), readbackBuffer.get());
	commandList->End();

	graphics->Execute(commandList);
	graphics->WaitFinish();

	auto result = static_cast<const MatrixIndexingOutput*>(readbackBuffer->Lock());
	if (result == nullptr)
	{
		abort();
	}

	for (int i = 0; i < dataSize; i++)
	{
		const auto& input = inputData[i];
		const auto& actual = result[i];
		float world[4] = {};
		TransformPoint(world, input.transform, input.local);
		world[3] = 1.0f;

		float expectedClip[4] = {};
		TransformClip(expectedClip, input.clip, world);

		const uint32_t row = input.row & 3u;
		const uint32_t nextRow = (row + 1u) & 3u;
		const uint32_t expectedChecksum = input.flags ^ input.row ^ FloatAsUint(input.local[3]);
		if (actual.checksum != expectedChecksum)
		{
			std::cout << "Failed : MatrixIndexing Checksum Mismatch " << i << std::endl;
			abort();
		}
		for (int axis = 0; axis < 4; axis++)
		{
			if (!NearlyEqual(actual.clip[axis], expectedClip[axis]))
			{
				std::cout << "Failed : MatrixIndexing Clip Mismatch " << i << " axis=" << axis << " actual=" << actual.clip[axis]
						  << " expected=" << expectedClip[axis] << std::endl;
				abort();
			}
		}
		for (int axis = 0; axis < 3; axis++)
		{
			const float expectedIndexed = input.transform[row][axis] + input.transform[nextRow][axis] * 0.25f;
			if (!NearlyEqual(actual.indexed[axis], expectedIndexed))
			{
				std::cout << "Failed : MatrixIndexing Row Mismatch " << i << std::endl;
				abort();
			}
		}
	}

	readbackBuffer->Unlock();
	platform->Present();
}

void test_compute_shader_storage_buffer_rebind_same_slot(LLGI::DeviceType deviceType)
{
	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("ComputeShaderStorageBufferRebindSameSlot", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> shader = nullptr;
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_write_slot1.comp", shader);

	auto pipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	pipeline->SetShader(LLGI::ShaderStageType::Compute, shader.get());
	if (!pipeline->Compile())
	{
		abort();
	}

	const int dataSize = 16;
	const int32_t bufferStride = sizeof(SlotTransitionData);
	auto storageBufferA = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, bufferStride * dataSize));
	auto storageBufferB = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, bufferStride * dataSize));
	auto readbackA = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapRead | LLGI::BufferUsageType::CopyDst, bufferStride * dataSize));
	auto readbackB = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapRead | LLGI::BufferUsageType::CopyDst, bufferStride * dataSize));

	if (!platform->NewFrame())
	{
		return;
	}

	sfMemoryPool->NewFrame();

	LLGI::ShaderResourceBinding slot1Binding;
	slot1Binding.ResourceType = LLGI::ShaderResourceType::StorageBuffer;
	slot1Binding.Access = LLGI::ShaderResourceAccess::ReadWrite;
	slot1Binding.StorageBufferView = LLGI::StorageBufferViewType::Raw;
	slot1Binding.Slot = 1;
	slot1Binding.ElementStride = bufferStride;

	auto commandList = commandListPool->Get();
	commandList->Begin();
	commandList->BeginComputePass();
	commandList->SetPipelineState(pipeline.get());
	commandList->SetStorageBuffer(storageBufferA.get(), slot1Binding);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->SetStorageBuffer(storageBufferB.get(), slot1Binding);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->CopyBuffer(storageBufferA.get(), readbackA.get());
	commandList->CopyBuffer(storageBufferB.get(), readbackB.get());
	commandList->End();

	graphics->Execute(commandList);
	graphics->WaitFinish();

	auto verify = [](const SlotTransitionData* result, const char* label, int count) {
		if (result == nullptr)
		{
			abort();
		}

		for (int i = 0; i < count; i++)
		{
			const float x = i * 2.0f + 0.25f;
			const float y = i * -3.0f + 0.5f;
			const float z = i * 4.0f - 0.75f;
			const uint32_t direction = 0x12340000u + i * 31u;
			if (result[i].position[0] != x ||
				result[i].position[1] != y ||
				result[i].position[2] != z ||
				result[i].direction != direction)
			{
				std::cout << "Failed : RebindSameSlot Mismatch " << label << " index=" << i << std::endl;
				abort();
			}
		}
	};

	auto resultA = static_cast<const SlotTransitionData*>(readbackA->Lock());
	auto resultB = static_cast<const SlotTransitionData*>(readbackB->Lock());
	verify(resultA, "A", dataSize);
	verify(resultB, "B", dataSize);
	readbackA->Unlock();
	readbackB->Unlock();
	platform->Present();
}

void test_compute_shader_storage_buffer_copy_to_smaller_buffer(LLGI::DeviceType deviceType)
{
	LLGI::PlatformParameter pp;
	pp.Device = deviceType;
	pp.WaitVSync = true;
	auto window = std::unique_ptr<LLGI::Window>(LLGI::CreateWindow("ComputeShaderStorageBufferCopyToSmallerBuffer", LLGI::Vec2I(1280, 720)));
	auto platform = LLGI::CreateSharedPtr(LLGI::CreatePlatform(pp, window.get()));
	auto graphics = LLGI::CreateSharedPtr(platform->CreateGraphics());

	auto sfMemoryPool = LLGI::CreateSharedPtr(graphics->CreateSingleFrameMemoryPool(1024 * 1024, 128));
	auto commandListPool = std::make_shared<LLGI::CommandListPool>(graphics.get(), sfMemoryPool.get(), 3);

	std::shared_ptr<LLGI::Shader> shader = nullptr;
	TestHelper::CreateComputeShader(graphics.get(), deviceType, "storage_buffer_write_slot1.comp", shader);

	auto pipeline = LLGI::CreateSharedPtr(graphics->CreatePiplineState());
	pipeline->SetShader(LLGI::ShaderStageType::Compute, shader.get());
	if (!pipeline->Compile())
	{
		abort();
	}

	const int dataSize = 16;
	const int readbackSize = 5;
	const int32_t bufferStride = sizeof(SlotTransitionData);
	auto storageBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::StorageWrite | LLGI::BufferUsageType::CopySrc, bufferStride * dataSize));
	auto readbackBuffer = LLGI::CreateSharedPtr(
		graphics->CreateBuffer(LLGI::BufferUsageType::MapRead | LLGI::BufferUsageType::CopyDst, bufferStride * readbackSize));

	if (!platform->NewFrame())
	{
		return;
	}

	sfMemoryPool->NewFrame();

	auto commandList = commandListPool->Get();
	commandList->Begin();
	commandList->BeginComputePass();
	commandList->SetPipelineState(pipeline.get());
	commandList->SetStorageBuffer(storageBuffer.get(), bufferStride, 1, LLGI::ShaderResourceAccess::ReadWrite);
	commandList->Dispatch(dataSize, 1, 1, 1, 1, 1);
	commandList->EndComputePass();
	commandList->CopyBuffer(storageBuffer.get(), readbackBuffer.get());
	commandList->End();

	graphics->Execute(commandList);
	graphics->WaitFinish();

	auto result = static_cast<const SlotTransitionData*>(readbackBuffer->Lock());
	if (result == nullptr)
	{
		abort();
	}

	for (int i = 0; i < readbackSize; i++)
	{
		const float x = i * 2.0f + 0.25f;
		const float y = i * -3.0f + 0.5f;
		const float z = i * 4.0f - 0.75f;
		const uint32_t direction = 0x12340000u + i * 31u;
		if (result[i].position[0] != x ||
			result[i].position[1] != y ||
			result[i].position[2] != z ||
			result[i].direction != direction)
		{
			std::cout << "Failed : CopyToSmallerBuffer Mismatch " << i << std::endl;
			abort();
		}
	}

	readbackBuffer->Unlock();
	platform->Present();
}

TestRegister ComputeShader_StorageBufferStructuredSlot1("ComputeShader.StorageBuffer.StructuredSlot1",
														[](LLGI::DeviceType device) -> void {
															test_compute_shader_storage_buffer_structured_slot1(device);
														});

TestRegister ComputeShader_StorageBufferSlot1PassTransition("ComputeShader.StorageBuffer.Slot1PassTransition",
															[](LLGI::DeviceType device) -> void {
																test_compute_shader_storage_buffer_slot1_pass_transition(device);
															});

TestRegister ComputeShader_StorageBufferMultiSlotReadWrite("ComputeShader.StorageBuffer.MultiSlotReadWrite",
														   [](LLGI::DeviceType device) -> void {
															   test_compute_shader_storage_buffer_multi_slot_read_write(device);
														   });

TestRegister ComputeShader_StorageBufferStructuredMatrixRecord("ComputeShader.StorageBuffer.StructuredMatrixRecord",
															   [](LLGI::DeviceType device) -> void {
																   test_compute_shader_storage_buffer_structured_matrix_record(device);
															   });

TestRegister ComputeShader_StorageBufferMatrixIndexing("ComputeShader.StorageBuffer.MatrixIndexing",
													   [](LLGI::DeviceType device) -> void {
														   test_compute_shader_storage_buffer_matrix_indexing(device);
													   });

TestRegister ComputeShader_StorageBufferRebindSameSlot("ComputeShader.StorageBuffer.RebindSameSlot",
													   [](LLGI::DeviceType device) -> void {
														   test_compute_shader_storage_buffer_rebind_same_slot(device);
													   });

TestRegister ComputeShader_StorageBufferCopyToSmallerBuffer("ComputeShader.StorageBuffer.CopyToSmallerBuffer",
															[](LLGI::DeviceType device) -> void {
																test_compute_shader_storage_buffer_copy_to_smaller_buffer(device);
															});
