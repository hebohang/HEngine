#include "LLGI.TextureDX12.h"
#include "LLGI.CommandListDX12.h"

namespace LLGI
{
namespace
{

size_t GetSubresourceSourceOffset(TextureFormatType format, const Vec3I& textureSize, int32_t mipLevel, int32_t arrayLayer, bool isArray)
{
	size_t offset = 0;
	for (int32_t i = 0; i < mipLevel; i++)
	{
		offset += static_cast<size_t>(GetTextureMemorySize(format, GetTextureMipSize(textureSize, i, isArray)));
	}

	if (isArray)
	{
		auto mipSize = GetTextureMipSize(textureSize, mipLevel, true);
		mipSize.Z = 1;
		offset += static_cast<size_t>(GetTextureMemorySize(format, mipSize)) * arrayLayer;
	}

	return offset;
}

} // namespace

TextureDX12::TextureDX12(GraphicsDX12* graphics, bool hasStrongRef) : graphics_(graphics), hasStrongRef_(hasStrongRef)
{
	if (hasStrongRef_)
	{
		SafeAddRef(graphics_);
	}

	assert(graphics_ != nullptr);
	device_ = graphics_->GetDevice();
	commandQueue_ = graphics_->GetCommandQueue();

	SafeAddRef(device_);
	SafeAddRef(commandQueue_);
	memset(&footprint_, 0, sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT));
}

TextureDX12::TextureDX12(ID3D12Resource* textureResource, ID3D12Device* device, ID3D12CommandQueue* commandQueue)
	: texture_(textureResource), commandQueue_(commandQueue), device_(device)
{
	type_ = TextureType::Screen;

	SafeAddRef(texture_);
	SafeAddRef(device_);
	SafeAddRef(commandQueue_);
	memset(&footprint_, 0, sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT));

	auto desc = texture_->GetDesc();
	dxgiFormat_ = desc.Format;

	format_ = ConvertFormat(desc.Format);
	texture_size_ = Vec3I(static_cast<int32_t>(desc.Width), static_cast<int32_t>(desc.Height), 1);
	cpu_memory_size_ = GetTextureMemorySize(format_, {texture_size_.X, texture_size_.Y, 1});
	mipmapCount_ = desc.MipLevels;

	UINT64 size = 0;
	device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint_, nullptr, nullptr, &size);
}

TextureDX12::~TextureDX12()
{
	SafeRelease(texture_);
	SafeRelease(buffer_for_upload_);
	SafeRelease(buffer_for_readback_);

	if (hasStrongRef_)
	{
		SafeRelease(graphics_);
	}
	SafeRelease(device_);
	SafeRelease(commandQueue_);
}

bool TextureDX12::Initialize(const TextureParameter& parameter)
{
	if (parameter.Dimension < 2)
	{
		return false;
	}

	format_ = parameter.Format;
	usage_ = parameter.Usage;
	dxgiFormat_ = ConvertFormat(parameter.Format);
	texture_size_ = parameter.Size;
	samplingCount_ = parameter.SampleCount;
	parameter_ = parameter;
	mipmapCount_ = parameter.MipLevelCount;
	const auto preserveDepth = (parameter.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag;
	cpu_memory_size_ = GetTextureMemorySize(format_, parameter.Size, mipmapCount_, preserveDepth);

	type_ = TextureType::Color;

	D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	if (parameter.Dimension == 2)
	{
		dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	}
	else if (parameter.Dimension == 3)
	{
		dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	}

	D3D12_RESOURCE_FLAGS resourceFlag = D3D12_RESOURCE_FLAG_NONE;

	if (IsDepthFormat(parameter.Format))
	{
		resourceFlag = resourceFlag | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		type_ = TextureType::Depth;
	}

	if ((parameter.Usage & TextureUsageType::RenderTarget) != TextureUsageType::NoneFlag)
	{
		resourceFlag = resourceFlag | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		type_ = TextureType::Render;
	}

	if (BitwiseContains(parameter.Usage, TextureUsageType::Storage))
	{
		resourceFlag |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}

	if (parameter.IsMipmapGenerationEnabled && parameter.MipLevelCount > 1 && type_ == TextureType::Color && parameter.Dimension == 2 &&
		parameter.SampleCount == 1 && !IsBlockCompressedFormat(parameter.Format))
	{
		resourceFlag |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	}

	if (type_ == TextureType::Render)
	{
		texture_ = CreateResourceBuffer(device_,
										D3D12_HEAP_TYPE_DEFAULT,
										dxgiFormat_,
										dimension,
										D3D12_RESOURCE_STATE_GENERIC_READ,
										resourceFlag,
										parameter.Size,
										parameter.SampleCount,
										parameter.MipLevelCount);
		state_ = D3D12_RESOURCE_STATE_GENERIC_READ;
	}
	else if (type_ == TextureType::Depth)
	{
		texture_ = CreateResourceBuffer(device_,
										D3D12_HEAP_TYPE_DEFAULT,
										dxgiFormat_,
										dimension,
										D3D12_RESOURCE_STATE_DEPTH_READ,
										resourceFlag,
										parameter.Size,
										parameter.SampleCount,
										parameter.MipLevelCount);
		state_ = D3D12_RESOURCE_STATE_DEPTH_READ;
	}
	else if (type_ == TextureType::Color)
	{

		texture_ = CreateResourceBuffer(device_,
										D3D12_HEAP_TYPE_DEFAULT,
										dxgiFormat_,
										dimension,
										D3D12_RESOURCE_STATE_COPY_DEST,
										resourceFlag,
										parameter.Size,
										parameter.SampleCount,
										parameter.MipLevelCount);

		state_ = D3D12_RESOURCE_STATE_COPY_DEST;
	}

	if (texture_ == nullptr)
		return false;

	// TODO: when it's NOT editable, do NOT call CreateBuffer.
	CreateUploadReadbackBuffer();

	return true;
}

bool TextureDX12::Initialize(ID3D12Resource* textureResource)
{
	type_ = TextureType::Color;
	texture_ = textureResource;

	SafeAddRef(texture_);

	auto desc = texture_->GetDesc();
	dxgiFormat_ = desc.Format;

	format_ = ConvertFormat(desc.Format);
	texture_size_ = Vec3I(static_cast<int32_t>(desc.Width), static_cast<int32_t>(desc.Height), static_cast<int32_t>(desc.DepthOrArraySize));
	cpu_memory_size_ = GetTextureMemorySize(format_, texture_size_);
	mipmapCount_ = desc.MipLevels;

	UINT64 size = 0;
	device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint_, nullptr, nullptr, &size);

	return true;
}

void TextureDX12::CreateUploadReadbackBuffer()
{
	auto textureDesc = texture_->GetDesc();
	const UINT subresourceCount =
		textureDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
			? textureDesc.MipLevels
			: static_cast<UINT>(textureDesc.MipLevels * textureDesc.DepthOrArraySize);
	footprints_.resize((std::max)(subresourceCount, 1u));
	device_->GetCopyableFootprints(&textureDesc, 0, static_cast<UINT>(footprints_.size()), 0, footprints_.data(), nullptr, nullptr, &uploadBufferSize_);
	footprint_ = footprints_.at(0);

	buffer_for_upload_ = CreateResourceBuffer(device_,
											  D3D12_HEAP_TYPE_UPLOAD,
											  DXGI_FORMAT_UNKNOWN,
											  D3D12_RESOURCE_DIMENSION_BUFFER,
											  D3D12_RESOURCE_STATE_GENERIC_READ,
											  D3D12_RESOURCE_FLAG_NONE,
											  {static_cast<int32_t>(uploadBufferSize_), 1, 1},
											  1);
	assert(buffer_for_upload_ != nullptr);

	buffer_for_readback_ = CreateResourceBuffer(device_,
												D3D12_HEAP_TYPE_READBACK,
												DXGI_FORMAT_UNKNOWN,
												D3D12_RESOURCE_DIMENSION_BUFFER,
												D3D12_RESOURCE_STATE_COPY_DEST,
												D3D12_RESOURCE_FLAG_NONE,
												{static_cast<int32_t>(uploadBufferSize_), 1, 1},
												1);
	assert(buffer_for_readback_ != nullptr);

	locked_buffer_.resize(cpu_memory_size_);
}

void* TextureDX12::Lock()
{
	return locked_buffer_.empty() ? nullptr : locked_buffer_.data();
}

void TextureDX12::Unlock()
{
	uint8_t* ptr = nullptr;
	buffer_for_upload_->Map(0, nullptr, (void**)&ptr);

	if (ptr != nullptr)
	{
		const bool isArray = (parameter_.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag;
		const int32_t mipLevelCount = parameter_.MipLevelCount > 0 ? parameter_.MipLevelCount : 1;
		for (UINT subresource = 0; subresource < static_cast<UINT>(footprints_.size()); subresource++)
		{
			const int32_t mipLevel = static_cast<int32_t>(subresource % mipLevelCount);
			const int32_t arrayLayer = isArray ? static_cast<int32_t>(subresource / mipLevelCount) : 0;
			auto mipSize = GetTextureMipSize(texture_size_, mipLevel, isArray);
			if (isArray)
			{
				mipSize.Z = 1;
			}

			const int32_t rowCount = GetTextureRowCount(format_, mipSize);
			const int32_t rowPitch = GetTextureRowPitch(format_, mipSize);
			const auto& footprint = footprints_.at(subresource);
			const size_t subresourceSize = static_cast<size_t>(rowPitch) * rowCount;
			const size_t srcOffset = GetSubresourceSourceOffset(format_, texture_size_, mipLevel, arrayLayer, isArray);

			if (srcOffset + subresourceSize > locked_buffer_.size())
			{
				break;
			}

			auto dst = ptr + footprint.Offset;
			const auto src = locked_buffer_.data() + srcOffset;
			for (int32_t row = 0; row < rowCount; row++)
			{
				memcpy(dst + static_cast<size_t>(row) * footprint.Footprint.RowPitch, src + static_cast<size_t>(row) * rowPitch, rowPitch);
			}
		}
	}
	buffer_for_upload_->Unmap(0, nullptr);

	ID3D12CommandAllocator* commandAllocator = nullptr;
	ID3D12GraphicsCommandList* commandList = nullptr;
	ID3D12Fence* fence = nullptr;
	ID3D12CommandList* commandLists[] = {nullptr};
	HRESULT hr = S_OK;

	D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};

	HANDLE event = CreateEvent(0, 0, 0, 0);

	if (event == nullptr)
	{
		auto msg = (std::string("Error : ") + std::string(__FILE__) + " : " + std::to_string(__LINE__) + std::string(" : "));
		::LLGI::Log(::LLGI::LogType::Error, msg.c_str());
		goto FAILED_EXIT;
	}

	hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
	if (FAILED(hr))
	{
		SHOW_DX12_ERROR(hr, device_);
		goto FAILED_EXIT;
	}

	hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, NULL, IID_PPV_ARGS(&commandList));
	if (FAILED(hr))
	{
		SHOW_DX12_ERROR(hr, device_);
		goto FAILED_EXIT;
	}

	hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	if (FAILED(hr))
	{
		SHOW_DX12_ERROR(hr, device_);
		goto FAILED_EXIT;
	}

	ResourceBarrier(commandList, D3D12_RESOURCE_STATE_COPY_DEST);

	for (UINT subresource = 0; subresource < static_cast<UINT>(footprints_.size()); subresource++)
	{
		src.pResource = buffer_for_upload_;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = footprints_.at(subresource);

		dst.pResource = texture_;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = subresource;

		commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}

	ResourceBarrier(commandList, D3D12_RESOURCE_STATE_GENERIC_READ);

	commandList->Close();
	commandLists[0] = commandList;
	commandQueue_->ExecuteCommandLists(1, commandLists);

	// TODO optimize it
	hr = commandQueue_->Signal(fence, 1);
	fence->SetEventOnCompletion(1, event);
	WaitForSingleObject(event, INFINITE);

FAILED_EXIT:
	SafeRelease(commandList);
	SafeRelease(commandAllocator);
	SafeRelease(fence);

	if (event != nullptr)
	{
		CloseHandle(event);
	}

	GenerateMipmapsOnLoad();
}

void TextureDX12::GenerateMipmapsOnLoad()
{
	// GetIsMipmapGenerationSupportedOnTextureLoad of GraphicsDX12 returns true,
	// so mipmaps must be generated when texture data is transferred.
	const bool canGenerate = parameter_.IsMipmapGenerationEnabled && parameter_.MipLevelCount > 1 && parameter_.Dimension == 2 &&
							 parameter_.SampleCount == 1 && !BitwiseContains(parameter_.Usage, TextureUsageType::Array) &&
							 !IsBlockCompressedFormat(parameter_.Format);
	if (!canGenerate || graphics_ == nullptr)
	{
		return;
	}

	auto memoryPool = CreateSharedPtr(graphics_->CreateSingleFrameMemoryPool(1024, 1));
	if (memoryPool == nullptr)
	{
		Log(LogType::Error, "Failed to create a memory pool for mipmap generation.");
		return;
	}

	auto generationCommandList = CreateSharedPtr(graphics_->CreateCommandList(memoryPool.get()));
	if (generationCommandList == nullptr)
	{
		Log(LogType::Error, "Failed to create a command list for mipmap generation.");
		return;
	}

	memoryPool->NewFrame();
	generationCommandList->Begin();
	generationCommandList->GenerateMipMap(this);
	generationCommandList->End();
	graphics_->Execute(generationCommandList.get());
	generationCommandList->WaitUntilCompleted();
}

bool TextureDX12::GetData(std::vector<uint8_t>& data)
{

	ID3D12CommandAllocator* commandAllocator = nullptr;
	ID3D12GraphicsCommandList* commandList = nullptr;
	ID3D12Fence* fence = nullptr;
	ID3D12CommandList* commandLists[] = {nullptr};
	HRESULT hr = S_OK;
	uint8_t* ptr = nullptr;

	D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};

	HANDLE event = CreateEvent(0, 0, 0, 0);

	if (event == nullptr)
	{
		auto msg = (std::string("Error : ") + std::string(__FILE__) + " : " + std::to_string(__LINE__) + std::string(" : "));
		::LLGI::Log(::LLGI::LogType::Error, msg.c_str());
		goto FAILED_EXIT;
	}

	hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
	if (FAILED(hr))
	{
		SHOW_DX12_ERROR(hr, device_);
		goto FAILED_EXIT;
	}

	hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, NULL, IID_PPV_ARGS(&commandList));
	if (FAILED(hr))
	{
		SHOW_DX12_ERROR(hr, device_);
		goto FAILED_EXIT;
	}

	hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	if (FAILED(hr))
	{
		SHOW_DX12_ERROR(hr, device_);
		goto FAILED_EXIT;
	}

	src.pResource = texture_;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;

	dst.pResource = buffer_for_readback_;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = footprint_;

	ResourceBarrier(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);

	commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	ResourceBarrier(commandList, D3D12_RESOURCE_STATE_GENERIC_READ);

	commandList->Close();
	commandLists[0] = commandList;
	commandQueue_->ExecuteCommandLists(1, commandLists);

	// TODO optimize it
	hr = commandQueue_->Signal(fence, 1);
	fence->SetEventOnCompletion(1, event);
	WaitForSingleObject(event, INFINITE);

	buffer_for_readback_->Map(0, nullptr, (void**)&ptr);
	if (ptr != nullptr)
	{
		UINT64 size = 0;
		auto textureDesc = texture_->GetDesc();
		device_->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint_, nullptr, nullptr, &size);

		data.resize(static_cast<size_t>(size));
		memcpy(data.data(), ptr, static_cast<size_t>(size));
		buffer_for_readback_->Unmap(0, nullptr);
	}

	SafeRelease(commandList);
	SafeRelease(commandAllocator);
	SafeRelease(fence);

	if (event != nullptr)
	{
		CloseHandle(event);
	}

	return true;

FAILED_EXIT:
	SafeRelease(commandList);
	SafeRelease(commandAllocator);
	SafeRelease(fence);

	if (event != nullptr)
	{
		CloseHandle(event);
	}

	return false;
}

Vec2I TextureDX12::GetSizeAs2D() const { return Vec2I{texture_size_.X, texture_size_.Y}; }

void TextureDX12::ResourceBarrier(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES state)
{
	if (state_ == state)
		return;

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = texture_;
	barrier.Transition.StateBefore = state_;
	barrier.Transition.StateAfter = state;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	commandList->ResourceBarrier(1, &barrier);
	state_ = state;
}

} // namespace LLGI
