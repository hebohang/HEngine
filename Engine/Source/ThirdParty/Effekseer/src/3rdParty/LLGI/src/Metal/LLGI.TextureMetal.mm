#include "LLGI.TextureMetal.h"
#include "LLGI.Metal_Impl.h"
#include <TargetConditionals.h>

namespace LLGI
{

bool TextureMetal::Initialize(id<MTLDevice> device, const TextureParameter& parameter)
{
	MTLTextureDescriptor* textureDescriptor = nullptr;

	bool isMipmapped = parameter.MipLevelCount >= 2;

	const bool isArray = (parameter.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag;
	const bool isRenderTarget = (parameter.Usage & TextureUsageType::RenderTarget) != TextureUsageType::NoneFlag;

	type_ = TextureType::Color;
    
    MTLTextureUsage usage = MTLTextureUsageUnknown;
    
    if(BitwiseContains(parameter.Usage, TextureUsageType::Storage))
    {
        usage |= (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    }

	if (IsDepthFormat(parameter.Format))
	{
		textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ConvertFormat(parameter.Format)
																			   width:parameter.Size.X
																			  height:parameter.Size.Y
																		   mipmapped:isMipmapped];
		textureDescriptor.usage = usage | MTLTextureUsageRenderTarget;
		textureDescriptor.textureType = MTLTextureType2D;
		textureDescriptor.storageMode = MTLStorageModePrivate;
		textureDescriptor.sampleCount = parameter.SampleCount;

		if (parameter.SampleCount > 1)
		{
			textureDescriptor.textureType = MTLTextureType2DMultisample;
			textureDescriptor.storageMode = MTLStorageModePrivate;
		}

		type_ = TextureType::Depth;
	}
	else
	{
		textureDescriptor = [[[MTLTextureDescriptor alloc] init] autorelease];
		textureDescriptor.usage = usage;

		if (isRenderTarget)
		{
			textureDescriptor.usage = textureDescriptor.usage | MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			type_ = TextureType::Render;
		}

		if (parameter.Dimension == 3)
		{
			textureDescriptor.textureType = MTLTextureType3D;
		}
		else if (parameter.Dimension == 2)
		{
			if (isArray)
			{
				if (parameter.SampleCount > 1)
				{
#if !(TARGET_OS_IPHONE) && !(TARGET_OS_SIMULATOR)
					textureDescriptor.textureType = MTLTextureType2DMultisampleArray;
#else
					textureDescriptor.textureType = MTLTextureType2DArray;
#endif
				}
				else
				{
					textureDescriptor.textureType = MTLTextureType2DArray;
				}
			}
			else
			{
				if (parameter.SampleCount > 1)
				{
					textureDescriptor.textureType = MTLTextureType2DMultisample;
				}
				else
				{
					textureDescriptor.textureType = MTLTextureType2D;
				}
			}
		}
		else
		{
			throw "1d texture is not supported";
		}

		textureDescriptor.width = parameter.Size.X;
		textureDescriptor.height = parameter.Size.Y;
		textureDescriptor.depth = isArray ? 1 : parameter.Size.Z;
		textureDescriptor.arrayLength = isArray ? parameter.Size.Z : 1;
		textureDescriptor.pixelFormat = ConvertFormat(parameter.Format);
		textureDescriptor.sampleCount = parameter.SampleCount;
		textureDescriptor.mipmapLevelCount = isMipmapped ? GetMaximumMipLevels(Vec2I{parameter.Size.X, parameter.Size.Y}) : 1;

		if (parameter.SampleCount > 1 && !isArray)
		{
			textureDescriptor.textureType = MTLTextureType2DMultisample;
			textureDescriptor.storageMode = MTLStorageModePrivate;
		}
	}

	if (isMipmapped)
	{
		textureDescriptor.mipmapLevelCount = parameter.MipLevelCount;
	}

	texture_ = [device newTextureWithDescriptor:textureDescriptor];
	if (texture_ == nullptr)
	{
		Log(LogType::Error,
			std::string("TextureMetal::Initialize failed: newTextureWithDescriptor returned null. parameter=") +
				DescribeTextureParameter(parameter));
		return false;
	}

	size_ = parameter.Size;

	samplingCount_ = parameter.SampleCount;

	mipmapCount_ = parameter.MipLevelCount;

	fromExternal_ = false;

	return true;
}

void TextureMetal::Write(const uint8_t* data)
{
	if (data == nullptr)
	{
		return;
	}

	const auto format = ConvertFormat(texture_.pixelFormat);
	size_t offset = 0;
	for (int32_t mipLevel = 0; mipLevel < mipmapCount_; mipLevel++)
	{
		auto mipSize = GetTextureMipSize(size_, mipLevel);
		MTLRegion region = {
			{0, 0, 0},
			{static_cast<uint32_t>(mipSize.X), static_cast<uint32_t>(mipSize.Y), static_cast<uint32_t>(mipSize.Z)}};

		auto all_size = GetTextureMemorySize(format, mipSize);
		auto bytes_per_row = GetTextureRowPitch(format, mipSize);
		auto bytes_per_image = all_size / mipSize.Z;

		[texture_ replaceRegion:region
					mipmapLevel:mipLevel
						  slice:0
					  withBytes:data + offset
					bytesPerRow:bytes_per_row
				  bytesPerImage:bytes_per_image];

		offset += all_size;
	}
}

TextureMetal::TextureMetal() {}

TextureMetal::~TextureMetal()
{
	if (texture_ != nullptr)
	{
		[texture_ release];
		texture_ = nullptr;
	}

	SafeRelease(owner_);
}

bool TextureMetal::Initialize(GraphicsMetal* owner, const TextureParameter& parameter)
{
	auto ownerRef = static_cast<ReferenceObject*>(owner);
	SafeAssign(owner_, ownerRef);

	type_ = TextureType::Color;
	if (!Initialize(owner->GetDevice(), parameter))
	{
		return false;
	}

	format_ = ConvertFormat(texture_.pixelFormat);
    usage_ = parameter.Usage;
	parameter_ = parameter;
	data_.resize(GetTextureMemorySize(format_, parameter.Size, mipmapCount_, (parameter.Usage & TextureUsageType::Array) != TextureUsageType::NoneFlag));
	return true;
}

bool TextureMetal::Initialize(GraphicsMetal* owner, id<MTLTexture> externalTexture)
{
	auto ownerRef = static_cast<ReferenceObject*>(owner);
	SafeAssign(owner_, ownerRef);

	if (externalTexture == nullptr)
	{
		return false;
	}

	Reset(externalTexture);
	type_ = TextureType::Color;

	format_ = ConvertFormat(texture_.pixelFormat);

	return true;
}

void TextureMetal::Reset(id<MTLTexture> nativeTexture)
{
	type_ = TextureType::Screen;

	if (nativeTexture != nullptr)
	{
		[nativeTexture retain];
	}

	if (texture_ != nullptr)
	{
		[texture_ release];
	}

	texture_ = nativeTexture;

	if (texture_ != nullptr)
	{
		size_.X = static_cast<int32_t>(texture_.width);
		size_.Y = static_cast<int32_t>(texture_.height);
		size_.Z = static_cast<int32_t>(texture_.depth);
	}
	else
	{
		size_.X = 0.0f;
		size_.Y = 0.0f;
		size_.Z = 0.0f;
	}

	fromExternal_ = true;

	format_ = ConvertFormat(texture_.pixelFormat);
}

void* TextureMetal::Lock() { return data_.data(); }

void TextureMetal::Unlock()
{
	Write(data_.data());
	GenerateMipmapsOnLoad();
}

void TextureMetal::GenerateMipmapsOnLoad()
{
	// GraphicsMetal advertises load-time mipmap generation support, so loaded textures
	// must generate their mip levels before delayed command queues can sample them.
	const bool canGenerate = parameter_.IsMipmapGenerationEnabled && parameter_.MipLevelCount > 1 &&
							 !IsBlockCompressedFormat(parameter_.Format) &&
							 !BitwiseContains(parameter_.Usage, TextureUsageType::Array);
	if (!canGenerate || owner_ == nullptr)
	{
		return;
	}

	auto graphics = static_cast<GraphicsMetal*>(owner_);
	id<MTLCommandBuffer> commandBuffer = [graphics->GetCommandQueue() commandBuffer];
	id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
	[blitEncoder generateMipmapsForTexture:texture_];
	[blitEncoder endEncoding];
	[commandBuffer commit];
	[commandBuffer waitUntilCompleted];
}

bool TextureMetal::GetData(std::vector<uint8_t>& data)
{
	// TODO : Implement it
	MTLRegion region = {{0, 0, 0}, {static_cast<uint32_t>(size_.X), static_cast<uint32_t>(size_.Y), static_cast<uint32_t>(size_.Z)}};

	auto all_size = GetTextureMemorySize(ConvertFormat(texture_.pixelFormat), size_);
    auto bytes_per_row = all_size / size_.Y / size_.Z;
	auto bytes_per_image = all_size / size_.Z;

	data.resize(all_size);

	[texture_ getBytes:data.data() bytesPerRow:bytes_per_row bytesPerImage:bytes_per_image fromRegion:region mipmapLevel:0 slice:0];

	return true;
}

Vec2I TextureMetal::GetSizeAs2D() const { return Vec2I{size_.X, size_.Y}; }

}
