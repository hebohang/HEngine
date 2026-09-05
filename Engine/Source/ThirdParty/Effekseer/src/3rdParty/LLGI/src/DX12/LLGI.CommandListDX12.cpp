#include "LLGI.CommandListDX12.h"
#include "LLGI.BufferDX12.h"
#include "LLGI.DescriptorHeapDX12.h"
#include "LLGI.GraphicsDX12.h"
#include "LLGI.PipelineStateDX12.h"
#include "LLGI.RenderPassDX12.h"
#include "LLGI.TextureDX12.h"
#include "LLGI.QueryDX12.h"
#include "LLGI.DX12MipmapShader.h"
#include <algorithm>

namespace LLGI
{
namespace
{

bool CanGenerateMipMap(const TextureDX12* texture)
{
	return texture != nullptr &&
		   texture->GetParameter().IsMipmapGenerationEnabled &&
		   texture->GetMipmapCount() > 1 &&
		   texture->GetParameter().Dimension == 2 &&
		   texture->GetParameter().SampleCount == 1 &&
		   !BitwiseContains(texture->GetParameter().Usage, TextureUsageType::Array) &&
		   !IsBlockCompressedFormat(texture->GetParameter().Format);
}

int32_t GetStorageBufferViewElementSize(int32_t stride, bool isRawBuffer)
{
	// D3D12 raw buffer views used by ByteAddressBuffer/RWByteAddressBuffer are addressed as 32-bit words.
	// They must use DXGI_FORMAT_R32_TYPELESS with the RAW flag and StructureByteStride = 0, so the caller's logical stride
	// is only used for structured buffer views.
	return isRawBuffer ? static_cast<int32_t>(sizeof(uint32_t)) : stride;
}

D3D12_SHADER_RESOURCE_VIEW_DESC CreateMipmapSRVDesc(const TextureDX12* texture, int32_t mip)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = DirectX12::GetShaderResourceViewFormat(texture->GetDXGIFormat());
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Texture2D.MostDetailedMip = mip - 1;
	desc.Texture2D.MipLevels = 1;
	desc.Texture2D.ResourceMinLODClamp = 0.0f;
	return desc;
}

D3D12_RENDER_TARGET_VIEW_DESC CreateMipmapRTVDesc(const TextureDX12* texture, int32_t mip)
{
	D3D12_RENDER_TARGET_VIEW_DESC desc = {};
	desc.Format = texture->GetDXGIFormat();
	desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	desc.Texture2D.MipSlice = mip;
	desc.Texture2D.PlaneSlice = 0;
	return desc;
}

D3D12_RESOURCE_BARRIER CreateMipmapTransitionBarrier(
	TextureDX12* texture, int32_t mip, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = texture->Get();
	barrier.Transition.Subresource = mip;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	return barrier;
}

D3D12_VIEWPORT CreateMipmapViewport(int32_t width, int32_t height)
{
	D3D12_VIEWPORT viewport = {};
	viewport.Width = static_cast<float>(width);
	viewport.Height = static_cast<float>(height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	return viewport;
}

D3D12_RECT CreateMipmapScissor(int32_t width, int32_t height)
{
	D3D12_RECT scissor = {};
	scissor.right = width;
	scissor.bottom = height;
	return scissor;
}

} // namespace

D3D12_SHADER_RESOURCE_VIEW_DESC CommandListDX12::GetSRVDescFromTexture(const TextureDX12* texture)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

	if (texture->GetParameter().Dimension == 3)
	{
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = texture->GetMipmapCount();
		srvDesc.Texture3D.MostDetailedMip = 0;
		srvDesc.Texture3D.ResourceMinLODClamp = 0.0f;
	}
	else if ((texture->GetParameter().Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag)
	{
		if (texture->GetParameter().SampleCount > 1)
		{
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
		}
		else
		{
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		}
		srvDesc.Texture2DArray.ArraySize = texture->GetParameter().Size.Z;
		srvDesc.Texture2DArray.FirstArraySlice = 0;
		srvDesc.Texture2DArray.MipLevels = texture->GetMipmapCount();
		srvDesc.Texture2DArray.MostDetailedMip = 0;
		srvDesc.Texture2DArray.PlaneSlice = 0;
		srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
	}
	else
	{
		if (texture->GetParameter().SampleCount > 1)
		{
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
		}
		else
		{
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		}
		srvDesc.Texture2D.MipLevels = texture->GetMipmapCount();
		srvDesc.Texture2D.MostDetailedMip = 0;
	}

	srvDesc.Format = DirectX12::GetShaderResourceViewFormat(texture->GetDXGIFormat());
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	return srvDesc;
}

bool CommandListDX12::CreateMipmapRootSignature()
{
	if (mipmapRootSignature_ != nullptr)
	{
		return true;
	}

	D3D12_DESCRIPTOR_RANGE range = {};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.BaseShaderRegister = 0;
	range.RegisterSpace = 0;
	range.OffsetInDescriptorsFromTableStart = 0;

	D3D12_ROOT_PARAMETER rootParameter = {};
	rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameter.DescriptorTable.NumDescriptorRanges = 1;
	rootParameter.DescriptorTable.pDescriptorRanges = &range;
	rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MipLODBias = 0.0f;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
	sampler.MinLOD = 0.0f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters = 1;
	desc.pParameters = &rootParameter;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers = &sampler;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ID3DBlob* signature = nullptr;
	ID3DBlob* error = nullptr;
	auto hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr))
	{
		SafeRelease(error);
		return false;
	}

	hr = graphics_->GetDevice()->CreateRootSignature(
		0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mipmapRootSignature_));
	SafeRelease(signature);
	SafeRelease(error);
	return SUCCEEDED(hr);
}

ID3D12PipelineState* CommandListDX12::GetMipmapPipelineState(DXGI_FORMAT format)
{
	auto found = mipmapPipelineStates_.find(format);
	if (found != mipmapPipelineStates_.end())
	{
		return found->second;
	}

	if (!CreateMipmapRootSignature())
	{
		return nullptr;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	desc.pRootSignature = mipmapRootSignature_;
	desc.VS.pShaderBytecode = DX12MipmapShader::VertexShader;
	desc.VS.BytecodeLength = DX12MipmapShader::VertexShaderSize;
	desc.PS.pShaderBytecode = DX12MipmapShader::PixelShader;
	desc.PS.BytecodeLength = DX12MipmapShader::PixelShaderSize;
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.DepthStencilState.StencilEnable = FALSE;
	desc.InputLayout.NumElements = 0;
	desc.InputLayout.pInputElementDescs = nullptr;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = format;
	desc.SampleDesc.Count = 1;

	ID3D12PipelineState* pipelineState = nullptr;
	auto hr = graphics_->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelineState));
	if (FAILED(hr))
	{
		return nullptr;
	}

	mipmapPipelineStates_[format] = pipelineState;
	return pipelineState;
}

D3D12_SAMPLER_DESC CommandListDX12::GeSamplerDescFromBindingTexture(const CommandList::BindingTexture& texture)
{
	auto wrapMode = texture.wrapMode;
	auto minMagFilter = texture.minMagFilter;

	D3D12_SAMPLER_DESC samplerDesc = {};

	if (minMagFilter == TextureMinMagFilter::Nearest)
	{
		samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	}
	else
	{
		samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	}

	if (wrapMode == TextureWrapMode::Repeat)
	{
		samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	}
	else if (wrapMode == TextureWrapMode::Clamp)
	{
		samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	}
	else if (wrapMode == TextureWrapMode::Mirror)
	{
		samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
	}

	samplerDesc.MipLODBias = 0;
	samplerDesc.MaxAnisotropy = 0;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplerDesc.MinLOD = 0.0f;
	samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
	return samplerDesc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC CommandListDX12::GetSRVDescFromStorageBuffer(const CommandList::BindingStorageBuffer& buffer, bool isRawBuffer)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = isRawBuffer ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.StructureByteStride = isRawBuffer ? 0 : buffer.binding.ElementStride;
	const auto elementSize = GetStorageBufferViewElementSize(buffer.binding.ElementStride, isRawBuffer);
	srvDesc.Buffer.NumElements = buffer.storageBuffer->GetSize() / elementSize;
	srvDesc.Buffer.Flags = isRawBuffer ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
	return srvDesc;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC CommandListDX12::GetUAVDescFromStorageBuffer(const CommandList::BindingStorageBuffer& buffer, bool isRawBuffer)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Format = isRawBuffer ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.StructureByteStride = isRawBuffer ? 0 : buffer.binding.ElementStride;
	const auto elementSize = GetStorageBufferViewElementSize(buffer.binding.ElementStride, isRawBuffer);
	uavDesc.Buffer.NumElements = buffer.storageBuffer->GetSize() / elementSize;
	uavDesc.Buffer.Flags = isRawBuffer ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
	return uavDesc;
}

bool CommandListDX12::ShouldBindStorageBufferAsSRV(
	const PipelineStateDX12* pipeline, const CommandList::BindingStorageBuffer& buffer, int32_t unit) const
{
	return pipeline != nullptr && buffer.storageBuffer != nullptr && IsReadOnly(buffer.binding.Access) && !pipeline->RequiresRawUAV(unit);
}

bool CommandListDX12::ShouldBindStorageBufferAsUAV(
	const PipelineStateDX12* pipeline, const CommandList::BindingStorageBuffer& buffer, int32_t unit) const
{
	return pipeline != nullptr && buffer.storageBuffer != nullptr && (!IsReadOnly(buffer.binding.Access) || pipeline->RequiresRawUAV(unit));
}

bool CommandListDX12::ValidateStorageBufferView(const CommandList::BindingStorageBuffer& buffer, bool isRawBuffer, int32_t unit) const
{
	if (buffer.storageBuffer == nullptr)
	{
		return false;
	}

	const auto bufferSize = buffer.storageBuffer->GetSize();
	if (isRawBuffer)
	{
		if (bufferSize % static_cast<int32_t>(sizeof(uint32_t)) == 0)
		{
			return true;
		}

		Log(LogType::Error,
			"DirectX12 ByteAddressBuffer/RWByteAddressBuffer storage buffer size must be aligned to 4 bytes. unit=" +
				std::to_string(unit) + ", size=" + std::to_string(bufferSize));
		return false;
	}

	if (buffer.binding.ElementStride > 0)
	{
		return true;
	}

	Log(LogType::Error,
		"DirectX12 structured storage buffer binding requires positive stride. unit=" + std::to_string(unit) +
			", stride=" + std::to_string(buffer.binding.ElementStride));
	return false;
}

bool CommandListDX12::CreateStorageBufferSRV(
	const CommandList::BindingStorageBuffer& buffer, bool isRawBuffer, int32_t unit, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle)
{
	if (!ValidateStorageBufferView(buffer, isRawBuffer, unit))
	{
		return false;
	}

	auto bufferDX12 = static_cast<BufferDX12*>(buffer.storageBuffer);
	bufferDX12->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	const auto srvDesc = GetSRVDescFromStorageBuffer(buffer, isRawBuffer);
	graphics_->GetDevice()->CreateShaderResourceView(bufferDX12->Get(), &srvDesc, cpuHandle);
	return true;
}

bool CommandListDX12::CreateFallbackSampledTexture()
{
	TextureParameter fallbackTextureParameter;
	auto fallbackTexture = static_cast<TextureDX12*>(graphics_->CreateTexture(fallbackTextureParameter));
	if (fallbackTexture == nullptr)
	{
		return false;
	}

	auto fallbackTextureData = static_cast<uint8_t*>(fallbackTexture->Lock());
	if (fallbackTextureData == nullptr)
	{
		SafeRelease(fallbackTexture);
		return false;
	}

	fallbackTextureData[0] = 255;
	fallbackTextureData[1] = 255;
	fallbackTextureData[2] = 255;
	fallbackTextureData[3] = 255;
	fallbackTexture->Unlock();

	fallbackSampledTexture_ = fallbackTexture;
	return true;
}

bool CommandListDX12::CreateSampledTextureSRV(TextureDX12* texture, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle)
{
	auto sourceTexture = texture != nullptr ? texture : fallbackSampledTexture_;
	if (sourceTexture == nullptr)
	{
		return false;
	}

	const auto srvDesc = GetSRVDescFromTexture(sourceTexture);
	graphics_->GetDevice()->CreateShaderResourceView(sourceTexture->Get(), &srvDesc, cpuHandle);
	return true;
}

bool CommandListDX12::CreateStorageBufferUAV(
	const CommandList::BindingStorageBuffer& buffer, bool isRawBuffer, int32_t unit, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle)
{
	if (!ValidateStorageBufferView(buffer, isRawBuffer, unit))
	{
		return false;
	}

	auto bufferDX12 = static_cast<BufferDX12*>(buffer.storageBuffer);
	bufferDX12->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	const auto uavDesc = GetUAVDescFromStorageBuffer(buffer, isRawBuffer);
	graphics_->GetDevice()->CreateUnorderedAccessView(bufferDX12->Get(), nullptr, &uavDesc, cpuHandle);
	return true;
}

void CommandListDX12::BeginInternal()
{
	rtDescriptorHeap_->Reset();

	dtDescriptorHeap_->Reset();

	cbDescriptorHeap_->Reset();

	samplerDescriptorHeap_->Reset();

	computeDescriptorHeap_->Reset();
}

CommandListDX12::CommandListDX12()
	: samplerDescriptorHeap_(nullptr)
	, cbDescriptorHeap_(nullptr)
	, computeDescriptorHeap_(nullptr)
	, rtDescriptorHeap_(nullptr)
	, dtDescriptorHeap_(nullptr)
	, commandList_(nullptr)
	, commandAllocator_(nullptr)
	, graphics_(nullptr)
	, renderPass_(nullptr)
{
}

CommandListDX12::~CommandListDX12()
{
	SafeRelease(fallbackSampledTexture_);
	SafeRelease(fence_);
	SafeRelease(mipmapRootSignature_);
	for (auto& pipelineState : mipmapPipelineStates_)
	{
		SafeRelease(pipelineState.second);
	}
	mipmapPipelineStates_.clear();

	if (fenceEvent_ != nullptr)
	{
		CloseHandle(fenceEvent_);
		fenceEvent_ = nullptr;
	}
}

bool CommandListDX12::Initialize(GraphicsDX12* graphics, int32_t drawingCount)
{
	HRESULT hr;
	ID3D12CommandAllocator* commandAllocator = nullptr;
	ID3D12GraphicsCommandList* commandList = nullptr;

	SafeAddRef(graphics);
	graphics_ = CreateSharedPtr(graphics);

	// Command Allocator
	hr = graphics_->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
	if (FAILED(hr))
	{
		auto msg = (std::string("Error : ") + std::string(__FILE__) + " : " + std::to_string(__LINE__) + std::string(" : ") +
					std::system_category().message(hr));
		::LLGI::Log(::LLGI::LogType::Error, msg.c_str());
		SafeRelease(graphics_);
		goto FAILED_EXIT;
	}
	commandAllocator_ = CreateSharedPtr(commandAllocator);

	// Command List
	hr = graphics_->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, NULL, IID_PPV_ARGS(&commandList));
	if (FAILED(hr))
	{
		auto msg = (std::string("Error : ") + std::string(__FILE__) + " : " + std::to_string(__LINE__) + std::string(" : ") +
					std::system_category().message(hr));
		::LLGI::Log(::LLGI::LogType::Error, msg.c_str());
		SafeRelease(graphics_);
		SafeRelease(commandAllocator_);
		goto FAILED_EXIT;
	}
	commandList->Close();
	commandList_ = CreateSharedPtr(commandList);

	rtDescriptorHeap_ =
		std::make_shared<DX12::DescriptorHeapAllocator>(graphics_, D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	dtDescriptorHeap_ =
		std::make_shared<DX12::DescriptorHeapAllocator>(graphics_, D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	samplerDescriptorHeap_ =
		std::make_shared<DX12::DescriptorHeapAllocator>(graphics_, D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

	cbDescriptorHeap_ =
		std::make_shared<DX12::DescriptorHeapAllocator>(graphics_, D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	computeDescriptorHeap_ =
		std::make_shared<DX12::DescriptorHeapAllocator>(graphics_, D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	if (!CreateFallbackSampledTexture())
	{
		goto FAILED_EXIT;
	}

	hr = graphics_->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
	if (FAILED(hr))
	{
		auto msg = (std::string("Error : ") + std::string(__FILE__) + " : " + std::to_string(__LINE__) + std::string(" : ") +
					std::system_category().message(hr));
		::LLGI::Log(::LLGI::LogType::Error, msg.c_str());
		goto FAILED_EXIT;
	}
	fenceEvent_ = CreateEvent(NULL, FALSE, FALSE, NULL);
	return true;

FAILED_EXIT:;
	return false;
}

void CommandListDX12::Begin()
{
	commandAllocator_->Reset();
	commandList_->Reset(commandAllocator_.get(), nullptr);

	BeginInternal();

	currentCommandList_ = commandList_.get();

	CommandList::Begin();
}

void CommandListDX12::End()
{
	assert(currentCommandList_ != nullptr);
	currentCommandList_->Close();
	currentCommandList_ = nullptr;

	CommandList::End();
}

bool CommandListDX12::BeginWithPlatform(void* platformContextPtr)
{
	auto ptr = reinterpret_cast<PlatformContextDX12*>(platformContextPtr);

	BeginInternal();

	currentCommandList_ = ptr->commandList;

	return CommandList::BeginWithPlatform(platformContextPtr);
}

void CommandListDX12::EndWithPlatform()
{
	assert(currentCommandList_ != nullptr);
	currentCommandList_ = nullptr;
	CommandList::EndWithPlatform();
}

void CommandListDX12::BeginRenderPass(RenderPass* renderPass)
{
	assert(currentCommandList_ != nullptr);

	SafeAddRef(renderPass);
	renderPass_ = CreateSharedPtr((RenderPassDX12*)renderPass);

	if (renderPass != nullptr)
	{
		// Set render target
		if (!renderPass_->ReinitializeRenderTargetViews(this, rtDescriptorHeap_, dtDescriptorHeap_))
		{
			throw "Failed to start renderPass because of descriptors.";
		}

		currentCommandList_->OMSetRenderTargets(renderPass_->GetCount(), renderPass_->GetHandleRTV(), FALSE, renderPass_->GetHandleDSV());

		// Reset scissor
		D3D12_RECT rects[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
		D3D12_VIEWPORT viewports[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
		for (int i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT && i < renderPass_->GetCount(); i++)
		{
			auto size = (renderPass_->GetRenderTarget(i)->texture_ != nullptr) ? renderPass_->GetRenderTarget(i)->texture_->GetSizeAs2D()
																			   : renderPass_->GetScreenSize();
			rects[i].top = 0;
			rects[i].left = 0;
			rects[i].right = size.X;
			rects[i].bottom = size.Y;

			viewports[i].TopLeftX = 0.0f;
			viewports[i].TopLeftY = 0.0f;
			viewports[i].Width = static_cast<float>(size.X);
			viewports[i].Height = static_cast<float>(size.Y);
			viewports[i].MinDepth = 0.0f;
			viewports[i].MaxDepth = 1.0f;
		}
		currentCommandList_->RSSetScissorRects(renderPass_->GetCount(), rects);
		currentCommandList_->RSSetViewports(renderPass_->GetCount(), viewports);

		if (renderPass_->GetIsColorCleared())
		{
			Clear(renderPass_->GetClearColor());
		}

		if (renderPass_->GetIsDepthCleared())
		{
			ClearDepth();
		}
	}

	CommandList::BeginRenderPass(renderPass);
}

void CommandListDX12::EndRenderPass()
{
	// Resolve MSAA
	if (renderPass_ != nullptr && renderPass_->GetResolvedRenderTexture() != nullptr)
	{
		auto src = static_cast<TextureDX12*>(renderPass_->GetRenderTexture(0));
		auto dst = static_cast<TextureDX12*>(renderPass_->GetResolvedRenderTexture());

		// TODO : refactor
		if (src->GetParameter().SampleCount <= 1)
		{
			Log(LogType::Error, "src SampleCount must be larger than 2.");
			return;
		}

		if (dst->GetParameter().SampleCount != 1)
		{
			Log(LogType::Error, "dst SampleCount must be 1.");
			return;
		}

		src->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		dst->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_RESOLVE_DEST);

		currentCommandList_->ResolveSubresource(dst->Get(), 0, src->Get(), 0, dst->GetDXGIFormat());
	}

	if (renderPass_ != nullptr && renderPass_->GetResolvedDepthTexture() != nullptr)
	{
		auto src = static_cast<TextureDX12*>(renderPass_->GetDepthTexture());
		auto dst = static_cast<TextureDX12*>(renderPass_->GetResolvedDepthTexture());

		// TODO : refactor
		if (src->GetParameter().SampleCount <= 1)
		{
			Log(LogType::Error, "src SampleCount must be larger than 2.");
			return;
		}

		if (dst->GetParameter().SampleCount != 1)
		{
			Log(LogType::Error, "dst SampleCount must be 1.");
			return;
		}

		src->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		dst->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_RESOLVE_DEST);

		currentCommandList_->ResolveSubresource(dst->Get(), 0, src->Get(), 0, DirectX12::GetShaderResourceViewFormat(dst->GetDXGIFormat()));
	}

	renderPass_.reset();
	CommandList::EndRenderPass();
}

void CommandListDX12::Draw(int32_t primitiveCount, int32_t instanceCount)
{
	if (currentCommandList_ == nullptr)
	{
		Log(LogType::Error, "CommandListDX12::Draw skipped: command list is null.");
		return;
	}

	BindingVertexBuffer vb_;
	BindingIndexBuffer ib_;
	PipelineState* pip_ = nullptr;

	bool isVBDirtied = false;
	bool isIBDirtied = false;
	bool isPipDirtied = false;

	if (!ValidateDrawState("CommandListDX12", primitiveCount, instanceCount, vb_, ib_, pip_, isVBDirtied, isIBDirtied, isPipDirtied))
	{
		return;
	}

	auto vb = static_cast<BufferDX12*>(vb_.vertexBuffer);
	auto ib = static_cast<BufferDX12*>(ib_.indexBuffer);
	auto pip = static_cast<PipelineStateDX12*>(pip_);

	{
		D3D12_VERTEX_BUFFER_VIEW vertexView;
		vertexView.BufferLocation = vb->Get()->GetGPUVirtualAddress() + vb_.offset;
		vertexView.StrideInBytes = vb_.stride;
		vertexView.SizeInBytes = vb_.vertexBuffer->GetSize() - vb_.offset;
		if (vb_.vertexBuffer != nullptr)
		{
			currentCommandList_->IASetVertexBuffers(0, 1, &vertexView);
		}
	}

	if (ib != nullptr)
	{
		D3D12_INDEX_BUFFER_VIEW indexView;
		indexView.BufferLocation = ib->Get()->GetGPUVirtualAddress() + ib_.offset;
		indexView.SizeInBytes = ib->GetActualSize() - ib_.offset;

		indexView.Format = ib_.stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
		currentCommandList_->IASetIndexBuffer(&indexView);
	}

	if (pip != nullptr)
	{
		currentCommandList_->SetGraphicsRootSignature(pip->GetRootSignature());
		auto p = pip->GetPipelineState();
		currentCommandList_->SetPipelineState(p);
		currentCommandList_->OMSetStencilRef(pip->StencilRef);
	}

	// count descriptor
	int32_t requiredCBDescriptorCount = NumConstantBuffer + NumTexture;
	int32_t requiredSamplerDescriptorCount = NumTexture;
	int32_t requiredStorageDescriptorCount = 0;

	for (size_t unit_ind = 0; unit_ind < NumStorageBuffer; unit_ind++)
	{
		BindingStorageBuffer storage;
		GetCurrentStorageBuffer(static_cast<int32_t>(unit_ind), storage);
		if (storage.storageBuffer != nullptr)
		{
			requiredStorageDescriptorCount = std::max(requiredStorageDescriptorCount, static_cast<int32_t>(unit_ind) + 1);
		}
	}

	requiredCBDescriptorCount += requiredStorageDescriptorCount;

	ID3D12DescriptorHeap* heapSampler = nullptr;
	ID3D12DescriptorHeap* heapConstant = nullptr;

	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 32> cpuDescriptorHandleSampler;
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 32> gpuDescriptorHandleSampler;
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 32> cpuDescriptorHandleConstant;
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 32> gpuDescriptorHandleConstant;

	if (!samplerDescriptorHeap_->Allocate(
			heapSampler, cpuDescriptorHandleSampler, gpuDescriptorHandleSampler, requiredSamplerDescriptorCount))
	{
		Log(LogType::Error, "Failed to draw because of descriptors.");
		return;
	}

	if (!cbDescriptorHeap_->Allocate(heapConstant, cpuDescriptorHandleConstant, gpuDescriptorHandleConstant, requiredCBDescriptorCount))
	{
		Log(LogType::Error, "Failed to draw because of descriptors.");
		return;
	}

	{
		// set using descriptor heaps
		ID3D12DescriptorHeap* heaps[] = {
			heapConstant,
			heapSampler,
		};
		currentCommandList_->SetDescriptorHeaps(2, heaps);

		// set descriptor tables
		currentCommandList_->SetGraphicsRootDescriptorTable(0, gpuDescriptorHandleConstant[0]);
		currentCommandList_->SetGraphicsRootDescriptorTable(1, gpuDescriptorHandleSampler[0]);
	}

	// constant buffer
	for (size_t unit_ind = 0; unit_ind < constantBuffers_.size(); unit_ind++)
	{
		auto cb = constantBuffers_[unit_ind];
		if (cb != nullptr)
		{
			auto _cb = static_cast<BufferDX12*>(cb);
			D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
			desc.BufferLocation = _cb->Get()->GetGPUVirtualAddress() + _cb->GetOffset();
			desc.SizeInBytes = _cb->GetActualSize();
			auto cpuHandle = cpuDescriptorHandleConstant[unit_ind];
			graphics_->GetDevice()->CreateConstantBufferView(&desc, cpuHandle);
		}
		else
		{
			// set dummy values
			D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
			desc.BufferLocation = D3D12_GPU_VIRTUAL_ADDRESS();
			desc.SizeInBytes = 0;
			auto cpuHandle = cpuDescriptorHandleConstant[unit_ind];
			graphics_->GetDevice()->CreateConstantBufferView(&desc, cpuHandle);
		}
	}

	{
		for (size_t unit_ind = 0; unit_ind < currentTextures_.size(); unit_ind++)
		{
			auto srvCpuHandle = cpuDescriptorHandleConstant[NumConstantBuffer + static_cast<int32_t>(unit_ind)];
			if (currentTextures_[unit_ind].texture != nullptr)
			{
				auto texture = static_cast<TextureDX12*>(currentTextures_[unit_ind].texture);

				// Make barrier to use a render target
				if (texture->GetType() == TextureType::Render)
				{
					texture->ResourceBarrier(currentCommandList_,
											 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				}

				if (!CreateSampledTextureSRV(texture, srvCpuHandle))
				{
					return;
				}
			}
			else if (unit_ind < NumStorageBuffer &&
					 ShouldBindStorageBufferAsSRV(pip, storageBuffers_[unit_ind], static_cast<int32_t>(unit_ind)))
			{
				BindingStorageBuffer storage = storageBuffers_[unit_ind];
				const bool isRawBuffer = pip->RequiresRawSRV(static_cast<int32_t>(unit_ind)) ||
										 storage.binding.StorageBufferView == StorageBufferViewType::Raw;
				if (!CreateStorageBufferSRV(storage, isRawBuffer, static_cast<int32_t>(unit_ind), srvCpuHandle))
				{
					return;
				}
			}
			else if (currentTextures_[unit_ind].isBound)
			{
				if (!CreateSampledTextureSRV(nullptr, srvCpuHandle))
				{
					return;
				}
			}

			if (currentTextures_[unit_ind].isBound)
			{
				D3D12_SAMPLER_DESC samplerDesc = GeSamplerDescFromBindingTexture(currentTextures_[unit_ind]);
				auto samplerCpuHandle = cpuDescriptorHandleSampler[unit_ind];
				graphics_->GetDevice()->CreateSampler(&samplerDesc, samplerCpuHandle);
			}
		}

		// UAV
		for (int32_t unit_ind = 0; unit_ind < NumStorageBuffer; unit_ind++)
		{
			BindingStorageBuffer storage;
			GetCurrentStorageBuffer(unit_ind, storage);

			if (!ShouldBindStorageBufferAsUAV(pip, storage, unit_ind))
				continue;

			const bool isRawBuffer = pip->RequiresRawUAV(unit_ind) || storage.binding.StorageBufferView == StorageBufferViewType::Raw;
			auto cpuHandle = cpuDescriptorHandleConstant[NumConstantBuffer + NumTexture + unit_ind];
			if (!CreateStorageBufferUAV(storage, isRawBuffer, unit_ind, cpuHandle))
			{
				return;
			}
		}
	}

	// setup a topology (triangle)

	const auto indexPerPrim = GetIndexCountPerPrimitive(pip_->Topology);
	if (indexPerPrim == 0)
	{
		Log(LogType::Error, "CommandListDX12::Draw skipped: unsupported topology. topology=" + to_string(pip_->Topology));
		return;
	}

	D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	if (pip_->Topology == TopologyType::Triangle)
	{
		topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	}
	else if (pip_->Topology == TopologyType::Line)
	{
		topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
	}
	else if (pip_->Topology == TopologyType::Point)
	{
		topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
	}

	currentCommandList_->IASetPrimitiveTopology(topology);

	// draw polygon
	currentCommandList_->DrawIndexedInstanced(primitiveCount * indexPerPrim, instanceCount, 0, 0, 0);

	CommandList::Draw(primitiveCount, instanceCount);
}

void CommandListDX12::CopyTexture(Texture* src, Texture* dst)
{
	auto srcTex = static_cast<TextureDX12*>(src);
	CopyTexture(src, dst, {0, 0, 0}, {0, 0, 0}, srcTex->GetParameter().Size, 0, 0);
}

void CommandListDX12::CopyTexture(
	Texture* src, Texture* dst, const Vec3I& srcPos, const Vec3I& dstPos, const Vec3I& size, int srcLayer, int dstLayer)
{
	if (isInRenderPass_ && !doesBeginWithPlatform_)
	{
		Log(LogType::Error, "Please call CopyTexture outside of RenderPass");
		return;
	}

	auto srcTex = static_cast<TextureDX12*>(src);
	auto dstTex = static_cast<TextureDX12*>(dst);

	D3D12_TEXTURE_COPY_LOCATION srcLoc = {}, dstLoc = {};

	srcLoc.pResource = srcTex->Get();
	srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	srcLoc.SubresourceIndex = srcLayer;

	dstLoc.pResource = dstTex->Get();
	dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dstLoc.SubresourceIndex = dstLayer;

	auto srcState = srcTex->GetState();

	srcTex->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_COPY_SOURCE);
	dstTex->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_COPY_DEST);

	D3D12_BOX srcBox;
	srcBox.left = srcPos[0];
	srcBox.right = srcPos[0] + size[0];
	srcBox.top = srcPos[1];
	srcBox.bottom = srcPos[1] + size[1];
	srcBox.front = srcPos[2];
	srcBox.back = srcPos[2] + size[2];

	currentCommandList_->CopyTextureRegion(&dstLoc, dstPos[0], dstPos[1], dstPos[2], &srcLoc, &srcBox);

	dstTex->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_GENERIC_READ);
	srcTex->ResourceBarrier(currentCommandList_, srcState);

	RegisterReferencedObject(src);
	RegisterReferencedObject(dst);
}

void CommandListDX12::GenerateMipMap(Texture* src)
{
	if (isInRenderPass_ && !doesBeginWithPlatform_)
	{
		Log(LogType::Error, "Please call GenerateMipMap outside of RenderPass");
		return;
	}

	auto srcTex = static_cast<TextureDX12*>(src);
	if (!CanGenerateMipMap(srcTex))
	{
		return;
	}

	auto pipelineState = GetMipmapPipelineState(srcTex->GetDXGIFormat());
	if (pipelineState == nullptr)
	{
		Log(LogType::Error, "Failed to create a DX12 mipmap pipeline.");
		return;
	}

	srcTex->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_GENERIC_READ);

	currentCommandList_->SetGraphicsRootSignature(mipmapRootSignature_);
	currentCommandList_->SetPipelineState(pipelineState);
	currentCommandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const auto mipmapCount = srcTex->GetMipmapCount();
	auto mipWidth = srcTex->GetSizeAs2D().X;
	auto mipHeight = srcTex->GetSizeAs2D().Y;

	for (int32_t mip = 1; mip < mipmapCount; mip++)
	{
		const auto dstWidth = std::max(1, mipWidth / 2);
		const auto dstHeight = std::max(1, mipHeight / 2);

		ID3D12DescriptorHeap* srvHeap = nullptr;
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 32> cpuSrvHandles;
		std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 32> gpuSrvHandles;
		if (!cbDescriptorHeap_->Allocate(srvHeap, cpuSrvHandles, gpuSrvHandles, 1))
		{
			Log(LogType::Error, "Failed to allocate a DX12 mipmap SRV descriptor.");
			return;
		}

		ID3D12DescriptorHeap* rtvHeap = nullptr;
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 32> cpuRtvHandles;
		std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 32> gpuRtvHandles;
		if (!rtDescriptorHeap_->Allocate(rtvHeap, cpuRtvHandles, gpuRtvHandles, 1))
		{
			Log(LogType::Error, "Failed to allocate a DX12 mipmap RTV descriptor.");
			return;
		}

		const auto srvDesc = CreateMipmapSRVDesc(srcTex, mip);
		graphics_->GetDevice()->CreateShaderResourceView(srcTex->Get(), &srvDesc, cpuSrvHandles[0]);

		const auto rtvDesc = CreateMipmapRTVDesc(srcTex, mip);
		graphics_->GetDevice()->CreateRenderTargetView(srcTex->Get(), &rtvDesc, cpuRtvHandles[0]);

		auto barrier =
			CreateMipmapTransitionBarrier(srcTex, mip, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
		currentCommandList_->ResourceBarrier(1, &barrier);

		ID3D12DescriptorHeap* heaps[] = {srvHeap};
		currentCommandList_->SetDescriptorHeaps(1, heaps);
		currentCommandList_->SetGraphicsRootDescriptorTable(0, gpuSrvHandles[0]);
		currentCommandList_->OMSetRenderTargets(1, &cpuRtvHandles[0], FALSE, nullptr);

		const auto viewport = CreateMipmapViewport(dstWidth, dstHeight);
		currentCommandList_->RSSetViewports(1, &viewport);

		const auto scissor = CreateMipmapScissor(dstWidth, dstHeight);
		currentCommandList_->RSSetScissorRects(1, &scissor);

		currentCommandList_->DrawInstanced(3, 1, 0, 0);

		barrier =
			CreateMipmapTransitionBarrier(srcTex, mip, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ);
		currentCommandList_->ResourceBarrier(1, &barrier);

		mipWidth = dstWidth;
		mipHeight = dstHeight;
	}

	RegisterReferencedObject(src);
}

void CommandListDX12::CopyBuffer(Buffer* src, Buffer* dst)
{
	auto srcBuf = static_cast<BufferDX12*>(src);
	auto dstBuf = static_cast<BufferDX12*>(dst);

	auto srcState = srcBuf->GetResourceState();
	auto dstState = dstBuf->GetResourceState();

	srcBuf->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_COPY_SOURCE);
	dstBuf->ResourceBarrier(currentCommandList_, D3D12_RESOURCE_STATE_COPY_DEST);

	currentCommandList_->CopyBufferRegion(
		dstBuf->Get(), dstBuf->GetOffset(), srcBuf->Get(), srcBuf->GetOffset(), std::min(srcBuf->GetSize(), dstBuf->GetSize()));

	srcBuf->ResourceBarrier(currentCommandList_, srcState);
	dstBuf->ResourceBarrier(currentCommandList_, dstState);

	RegisterReferencedObject(src);
	RegisterReferencedObject(dst);
}

bool CommandListDX12::ResetQuery(Query* query)
{
	return true;
}

bool CommandListDX12::BeginQuery(Query* query, uint32_t queryIndex)
{
	auto query_ = static_cast<QueryDX12*>(query);
	if (query_ == nullptr)
	{
		return false;
	}

	currentCommandList_->BeginQuery(query_->GetQueryHeap(), query_->GetQueryTypeDX12(), queryIndex);

	return true;
}

bool CommandListDX12::EndQuery(Query* query, uint32_t queryIndex)
{
	auto query_ = static_cast<QueryDX12*>(query);
	if (query_ == nullptr)
	{
		return false;
	}

	currentCommandList_->EndQuery(query_->GetQueryHeap(), query_->GetQueryTypeDX12(), queryIndex);

	return true;
}

bool CommandListDX12::RecordTimestamp(Query* query, uint32_t queryIndex)
{
	auto query_ = static_cast<QueryDX12*>(query);
	if (query_ == nullptr)
	{
		return false;
	}

	currentCommandList_->EndQuery(query_->GetQueryHeap(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);

	currentCommandList_->ResolveQueryData(query_->GetQueryHeap(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex, 1, query_->GetBuffer(), queryIndex * sizeof(uint64_t));

	return true;
}

void CommandListDX12::BeginComputePass() {}

void CommandListDX12::EndComputePass() {}

void CommandListDX12::Dispatch(int32_t groupX, int32_t groupY, int32_t groupZ, int32_t threadX, int32_t threadY, int32_t threadZ)
{
	if (currentCommandList_ == nullptr)
	{
		Log(LogType::Error, "CommandListDX12::Dispatch skipped: command list is null.");
		return;
	}
	PipelineState* pip_ = nullptr;

	bool isPipDirtied = false;

	if (!ValidateDispatchState("CommandListDX12", groupX, groupY, groupZ, threadX, threadY, threadZ, pip_, isPipDirtied))
	{
		return;
	}

	auto pip = static_cast<PipelineStateDX12*>(pip_);

	if (pip != nullptr)
	{
		currentCommandList_->SetComputeRootSignature(pip->GetComputeRootSignature());
		auto p = pip->GetComputePipelineState();
		currentCommandList_->SetPipelineState(p);
	}

	int32_t requiredCBDescriptorCount = NumConstantBuffer + NumTexture + NumStorageBuffer;

	ID3D12DescriptorHeap* heapConstant = nullptr;
	ID3D12DescriptorHeap* heapSampler = nullptr;

	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 32> cpuDescriptorHandleConstant;
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 32> gpuDescriptorHandleConstant;
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 32> cpuDescriptorHandleSampler;
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 32> gpuDescriptorHandleSampler;

	if (!cbDescriptorHeap_->Allocate(heapConstant, cpuDescriptorHandleConstant, gpuDescriptorHandleConstant, requiredCBDescriptorCount))
	{
		Log(LogType::Error, "Failed to draw because of descriptors.");
		return;
	}

	if (!samplerDescriptorHeap_->Allocate(heapSampler, cpuDescriptorHandleSampler, gpuDescriptorHandleSampler, NumTexture))
	{
		Log(LogType::Error, "Failed to draw because of descriptors.");
		return;
	}

	{
		// set using descriptor heaps
		ID3D12DescriptorHeap* heaps[] = {
			heapConstant,
			heapSampler,
		};
		currentCommandList_->SetDescriptorHeaps(2, heaps);

		// set descriptor tables
		currentCommandList_->SetComputeRootDescriptorTable(0, gpuDescriptorHandleConstant[0]);
		currentCommandList_->SetComputeRootDescriptorTable(1, gpuDescriptorHandleSampler[0]);
	}

	// constant buffer
	for (size_t unit_ind = 0; unit_ind < constantBuffers_.size(); unit_ind++)
	{
		auto cb = constantBuffers_[unit_ind];
		if (cb != nullptr)
		{
			auto _cb = static_cast<BufferDX12*>(cb);
			D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
			desc.BufferLocation = _cb->Get()->GetGPUVirtualAddress() + _cb->GetOffset();
			desc.SizeInBytes = _cb->GetActualSize();
			auto cpuHandle = cpuDescriptorHandleConstant[unit_ind];
			graphics_->GetDevice()->CreateConstantBufferView(&desc, cpuHandle);
		}
		else
		{
			// set dummy values
			D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
			desc.BufferLocation = D3D12_GPU_VIRTUAL_ADDRESS();
			desc.SizeInBytes = 0;
			auto cpuHandle = cpuDescriptorHandleConstant[unit_ind];
			graphics_->GetDevice()->CreateConstantBufferView(&desc, cpuHandle);
		}
	}

	// SRV
	for (size_t unit_ind = 0; unit_ind < currentTextures_.size(); unit_ind++)
	{
		auto srvCpuHandle = cpuDescriptorHandleConstant[NumConstantBuffer + static_cast<int32_t>(unit_ind)];
		if (currentTextures_[unit_ind].texture != nullptr &&
			!BitwiseContains(currentTextures_[unit_ind].texture->GetUsage(), TextureUsageType::Storage))
		{
			auto texture = static_cast<TextureDX12*>(currentTextures_[unit_ind].texture);

			// Make barrier to use a render target
			if (texture->GetType() == TextureType::Render)
			{
				texture->ResourceBarrier(currentCommandList_,
										 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}

			if (!CreateSampledTextureSRV(texture, srvCpuHandle))
			{
				return;
			}
		}
		else if (unit_ind < NumStorageBuffer &&
				 ShouldBindStorageBufferAsSRV(pip, storageBuffers_[unit_ind], static_cast<int32_t>(unit_ind)))
		{
			BindingStorageBuffer storage = storageBuffers_[unit_ind];

			const bool isRawBuffer = pip->RequiresRawSRV(static_cast<int32_t>(unit_ind)) ||
									 storage.binding.StorageBufferView == StorageBufferViewType::Raw;
			if (!CreateStorageBufferSRV(storage, isRawBuffer, static_cast<int32_t>(unit_ind), srvCpuHandle))
			{
				return;
			}
		}
		else if (currentTextures_[unit_ind].isBound && !CreateSampledTextureSRV(nullptr, srvCpuHandle))
		{
			return;
		}

		if (currentTextures_[unit_ind].isBound)
		{
			D3D12_SAMPLER_DESC samplerDesc = GeSamplerDescFromBindingTexture(currentTextures_[unit_ind]);
			auto samplerCpuHandle = cpuDescriptorHandleSampler[unit_ind];
			graphics_->GetDevice()->CreateSampler(&samplerDesc, samplerCpuHandle);
		}
	}

	// UAV
	for (int32_t unit_ind = 0; unit_ind < NumStorageBuffer; unit_ind++)
	{
		if (ShouldBindStorageBufferAsUAV(pip, storageBuffers_[unit_ind], unit_ind))
		{
			BindingStorageBuffer storage = storageBuffers_[unit_ind];
			const bool isRawBuffer = pip->RequiresRawUAV(unit_ind) || storage.binding.StorageBufferView == StorageBufferViewType::Raw;
			auto cpuHandle = cpuDescriptorHandleConstant[NumConstantBuffer + NumTexture + unit_ind];
			if (!CreateStorageBufferUAV(storage, isRawBuffer, unit_ind, cpuHandle))
			{
				return;
			}
		}

		if (currentTextures_[unit_ind].texture != nullptr &&
			BitwiseContains(currentTextures_[unit_ind].texture->GetUsage(), TextureUsageType::Storage))
		{
			auto texture = static_cast<TextureDX12*>(currentTextures_[unit_ind].texture);
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Format = DirectX12::GetShaderResourceViewFormat(texture->GetDXGIFormat());
			uavDesc.Texture2D.MipSlice = 0;
			uavDesc.Texture2D.PlaneSlice = 0;
			auto cpuHandle = cpuDescriptorHandleConstant[NumConstantBuffer + NumTexture + unit_ind];
			graphics_->GetDevice()->CreateUnorderedAccessView(texture->Get(), nullptr, &uavDesc, cpuHandle);
		}
	}

	currentCommandList_->Dispatch(groupX, groupY, groupZ);

	// UAV
	for (int32_t unit_ind = 0; unit_ind < NumStorageBuffer; unit_ind++)
	{
		if (ShouldBindStorageBufferAsUAV(pip, storageBuffers_[unit_ind], unit_ind))
		{
			BindingStorageBuffer storage = storageBuffers_[unit_ind];
			auto storageBuffer = static_cast<BufferDX12*>(storage.storageBuffer);
			storageBuffer->UAVBarrier(currentCommandList_);
		}
	}

	CommandList::Dispatch(groupX, groupY, groupZ, threadX, threadY, threadZ);
}

void CommandListDX12::Clear(const Color8& color)
{
	assert(currentCommandList_ != nullptr);

	auto rt = renderPass_;
	if (rt == nullptr)
		return;

	float color_[] = {color.R / 255.0f, color.G / 255.0f, color.B / 255.0f, color.A / 255.0f};

	auto handle = rt->GetHandleRTV();
	for (int i = 0; i < rt->GetCount(); i++)
	{
		currentCommandList_->ClearRenderTargetView(handle[i], color_, 0, nullptr);
	}
}

void CommandListDX12::ClearDepth()
{
	assert(currentCommandList_ != nullptr);

	auto rt = renderPass_;
	if (rt == nullptr)
		return;

	if (!rt->GetHasDepthTexture())
	{
		return;
	}

	auto depthTexture = static_cast<TextureDX12*>(rt->GetDepthTexture());
	const auto clearFlags = HasStencil(depthTexture->GetFormat()) ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH;

	auto handle = rt->GetHandleDSV();
	currentCommandList_->ClearDepthStencilView(handle[0], clearFlags, 1.0f, 0, 0, nullptr);
}

ID3D12GraphicsCommandList* CommandListDX12::GetCommandList() const { return commandList_.get(); }

ID3D12Fence* CommandListDX12::GetFence() const { return fence_; }

UINT64 CommandListDX12::GetAndIncFenceValue()
{
	auto ret = fenceValue_;
	fenceValue_ += 1;
	return ret;
}

void CommandListDX12::WaitUntilCompleted()
{

	if (fence_->GetCompletedValue() < fenceValue_ - 1)
	{
		auto hr = fence_->SetEventOnCompletion(fenceValue_ - 1, fenceEvent_);
		if (FAILED(hr))
		{
			return;
		}
		WaitForSingleObject(fenceEvent_, INFINITE);
	}
}

} // namespace LLGI
