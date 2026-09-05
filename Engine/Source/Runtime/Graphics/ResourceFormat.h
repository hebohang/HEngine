#pragma once

#include <cstdint>

#include "Runtime/Utils/EnumUtils/EnumUtils.h"

namespace HEngine
{
	enum class EResourceFormat
	{
		UNKNOWN,
		R32G32B32A32_FLOAT,
		R32G32B32A32_UINT,
		R32G32B32A32_SINT,
		R32G32B32_FLOAT,
		R32G32B32_UINT,
		R32G32B32_SINT,
		R16G16B16A16_FLOAT,
		R16G16B16A16_UNORM,
		R16G16B16A16_UINT,
		R16G16B16A16_SNORM,
		R16G16B16A16_SINT,
		R32G32_FLOAT,
		R32G32_UINT,
		R32G32_SINT,
		R32G8X24_TYPELESS,
		D32_FLOAT_S8X24_UINT,
		R10G10B10A2_UNORM,
		R10G10B10A2_UINT,
		R11G11B10_FLOAT,
		R8G8B8A8_UNORM,
		R8G8B8A8_UNORM_SRGB,
		R8G8B8A8_UINT,
		R8G8B8A8_SNORM,
		R8G8B8A8_SINT,
		B8G8R8A8_UNORM,
		B8G8R8A8_UNORM_SRGB,
		R16G16_FLOAT,
		R16G16_UNORM,
		R16G16_UINT,
		R16G16_SNORM,
		R16G16_SINT,
		R32_TYPELESS,
		D32_FLOAT,
		R32_FLOAT,
		R32_UINT,
		R32_SINT,
		R24G8_TYPELESS,
		D24_UNORM_S8_UINT,
		R8G8_UNORM,
		R8G8_UINT,
		R8G8_SNORM,
		R8G8_SINT,
		R16_TYPELESS,
		R16_FLOAT,
		D16_UNORM,
		R16_UNORM,
		R16_UINT,
		R16_SNORM,
		R16_SINT,
		R8_UNORM,
		R8_UINT,
		R8_SNORM,
		R8_SINT,
		BC1_UNORM,
		BC1_UNORM_SRGB,
		BC2_UNORM,
		BC2_UNORM_SRGB,
		BC3_UNORM,
		BC3_UNORM_SRGB,
		BC4_UNORM,
		BC4_SNORM,
		BC5_UNORM,
		BC5_SNORM,
		BC6H_UF16,
		BC6H_SF16,
		BC7_UNORM,
		BC7_UNORM_SRGB
	};

	enum ESubresourceType : uint8_t
	{
		SubresourceType_SRV,
		SubresourceType_UAV,
		SubresourceType_RTV,
		SubresourceType_DSV,
		SubresourceType_Invalid
	};

	enum class EBindFlag : uint32_t
	{
		None = 0,
		ShaderResource = 1 << 0,
		RenderTarget = 1 << 1,
		DepthStencil = 1 << 2,
		UnorderedAccess = 1 << 3,
	};
	DEFINE_ENUM_BIT_OPERATORS(EBindFlag);

	enum class EResourceUsage : uint8_t
	{
		Default,
		Upload,
		Readback
	};

	// byte for EResourceFormat
	constexpr uint32_t GetFormatStride(EResourceFormat _format)
	{
		switch (_format)
		{
		case EResourceFormat::BC1_UNORM:
		case EResourceFormat::BC1_UNORM_SRGB:
		case EResourceFormat::BC4_SNORM:
		case EResourceFormat::BC4_UNORM:
			return 8u;
		case EResourceFormat::R32G32B32A32_FLOAT:
		case EResourceFormat::R32G32B32A32_UINT:
		case EResourceFormat::R32G32B32A32_SINT:
		case EResourceFormat::BC2_UNORM:
		case EResourceFormat::BC2_UNORM_SRGB:
		case EResourceFormat::BC3_UNORM:
		case EResourceFormat::BC3_UNORM_SRGB:
		case EResourceFormat::BC5_SNORM:
		case EResourceFormat::BC5_UNORM:
		case EResourceFormat::BC6H_UF16:
		case EResourceFormat::BC6H_SF16:
		case EResourceFormat::BC7_UNORM:
		case EResourceFormat::BC7_UNORM_SRGB:
			return 16u;
		case EResourceFormat::R32G32B32_FLOAT:
		case EResourceFormat::R32G32B32_UINT:
		case EResourceFormat::R32G32B32_SINT:
			return 12u;
		case EResourceFormat::R16G16B16A16_FLOAT:
		case EResourceFormat::R16G16B16A16_UNORM:
		case EResourceFormat::R16G16B16A16_UINT:
		case EResourceFormat::R16G16B16A16_SNORM:
		case EResourceFormat::R16G16B16A16_SINT:
			return 8u;
		case EResourceFormat::R32G32_FLOAT:
		case EResourceFormat::R32G32_UINT:
		case EResourceFormat::R32G32_SINT:
		case EResourceFormat::R32G8X24_TYPELESS:
		case EResourceFormat::D32_FLOAT_S8X24_UINT:
			return 8u;
		case EResourceFormat::R10G10B10A2_UNORM:
		case EResourceFormat::R10G10B10A2_UINT:
		case EResourceFormat::R11G11B10_FLOAT:
		case EResourceFormat::R8G8B8A8_UNORM:
		case EResourceFormat::R8G8B8A8_UNORM_SRGB:
		case EResourceFormat::R8G8B8A8_UINT:
		case EResourceFormat::R8G8B8A8_SNORM:
		case EResourceFormat::R8G8B8A8_SINT:
		case EResourceFormat::B8G8R8A8_UNORM:
		case EResourceFormat::B8G8R8A8_UNORM_SRGB:
		case EResourceFormat::R16G16_FLOAT:
		case EResourceFormat::R16G16_UNORM:
		case EResourceFormat::R16G16_UINT:
		case EResourceFormat::R16G16_SNORM:
		case EResourceFormat::R16G16_SINT:
		case EResourceFormat::R32_TYPELESS:
		case EResourceFormat::D32_FLOAT:
		case EResourceFormat::R32_FLOAT:
		case EResourceFormat::R32_UINT:
		case EResourceFormat::R32_SINT:
		case EResourceFormat::R24G8_TYPELESS:
		case EResourceFormat::D24_UNORM_S8_UINT:
			return 4u;
		case EResourceFormat::R8G8_UNORM:
		case EResourceFormat::R8G8_UINT:
		case EResourceFormat::R8G8_SNORM:
		case EResourceFormat::R8G8_SINT:
		case EResourceFormat::R16_TYPELESS:
		case EResourceFormat::R16_FLOAT:
		case EResourceFormat::D16_UNORM:
		case EResourceFormat::R16_UNORM:
		case EResourceFormat::R16_UINT:
		case EResourceFormat::R16_SNORM:
		case EResourceFormat::R16_SINT:
			return 2u;
		case EResourceFormat::R8_UNORM:
		case EResourceFormat::R8_UINT:
		case EResourceFormat::R8_SNORM:
		case EResourceFormat::R8_SINT:
			return 1u;
		default:
			HE_CORE_ASSERT(false, "Unknown Format");
			break;
		}

		return 16u;
	}

	// bits for EResourceFormat, use for texture
	constexpr uint32_t BitsPerPixel(EResourceFormat _format)
	{
		// refer to DirectXTex\DirectXTexUtil.cpp
		switch (_format)
		{
			//case EResourceFormat::R32G32B32A32_TYPELESS:
		case EResourceFormat::R32G32B32A32_FLOAT:
		case EResourceFormat::R32G32B32A32_UINT:
		case EResourceFormat::R32G32B32A32_SINT:
			return 128;

			//case EResourceFormat::R32G32B32_TYPELESS:
		case EResourceFormat::R32G32B32_FLOAT:
		case EResourceFormat::R32G32B32_UINT:
		case EResourceFormat::R32G32B32_SINT:
			return 96;

			//case EResourceFormat::R16G16B16A16_TYPELESS:
		case EResourceFormat::R16G16B16A16_FLOAT:
		case EResourceFormat::R16G16B16A16_UNORM:
		case EResourceFormat::R16G16B16A16_UINT:
		case EResourceFormat::R16G16B16A16_SNORM:
		case EResourceFormat::R16G16B16A16_SINT:
			//case EResourceFormat::R32G32_TYPELESS:
		case EResourceFormat::R32G32_FLOAT:
		case EResourceFormat::R32G32_UINT:
		case EResourceFormat::R32G32_SINT:
		case EResourceFormat::R32G8X24_TYPELESS:
		case EResourceFormat::D32_FLOAT_S8X24_UINT:
			//case EResourceFormat::R32_FLOAT_X8X24_TYPELESS:
			//case EResourceFormat::X32_TYPELESS_G8X24_UINT:
			//case EResourceFormat::Y416:
			//case EResourceFormat::Y210:
			//case EResourceFormat::Y216:
			return 64;

			//case EResourceFormat::R10G10B10A2_TYPELESS:
		case EResourceFormat::R10G10B10A2_UNORM:
		case EResourceFormat::R10G10B10A2_UINT:
		case EResourceFormat::R11G11B10_FLOAT:
			//case EResourceFormat::R8G8B8A8_TYPELESS:
		case EResourceFormat::R8G8B8A8_UNORM:
		case EResourceFormat::R8G8B8A8_UNORM_SRGB:
		case EResourceFormat::R8G8B8A8_UINT:
		case EResourceFormat::R8G8B8A8_SNORM:
		case EResourceFormat::R8G8B8A8_SINT:
			//case EResourceFormat::R16G16_TYPELESS:
		case EResourceFormat::R16G16_FLOAT:
		case EResourceFormat::R16G16_UNORM:
		case EResourceFormat::R16G16_UINT:
		case EResourceFormat::R16G16_SNORM:
		case EResourceFormat::R16G16_SINT:
		case EResourceFormat::R32_TYPELESS:
		case EResourceFormat::D32_FLOAT:
		case EResourceFormat::R32_FLOAT:
		case EResourceFormat::R32_UINT:
		case EResourceFormat::R32_SINT:
		case EResourceFormat::R24G8_TYPELESS:
		case EResourceFormat::D24_UNORM_S8_UINT:
			//case EResourceFormat::R24_UNORM_X8_TYPELESS:
			//case EResourceFormat::X24_TYPELESS_G8_UINT:
			//case EResourceFormat::R9G9B9E5_SHAREDEXP:
			//case EResourceFormat::R8G8_B8G8_UNORM:
			//case EResourceFormat::G8R8_G8B8_UNORM:
		case EResourceFormat::B8G8R8A8_UNORM:
			//case EResourceFormat::B8G8R8X8_UNORM:
			//case EResourceFormat::R10G10B10_XR_BIAS_A2_UNORM:
			//case EResourceFormat::B8G8R8A8_TYPELESS:
		case EResourceFormat::B8G8R8A8_UNORM_SRGB:
			//case EResourceFormat::B8G8R8X8_TYPELESS:
			//case EResourceFormat::B8G8R8X8_UNORM_SRGB:
			//case EResourceFormat::AYUV:
			//case EResourceFormat::Y410:
			//case EResourceFormat::YUY2:
			//case XBOX_EResourceFormat::R10G10B10_7E3_A2_FLOAT:
			//case XBOX_EResourceFormat::R10G10B10_6E4_A2_FLOAT:
			//case XBOX_EResourceFormat::R10G10B10_SNORM_A2_UNORM:
			return 32;

			//case EResourceFormat::P010:
			//case EResourceFormat::P016:
			//case XBOX_EResourceFormat::D16_UNORM_S8_UINT:
			//case XBOX_EResourceFormat::R16_UNORM_X8_TYPELESS:
			//case XBOX_EResourceFormat::X16_TYPELESS_G8_UINT:
			//case WIN10_EResourceFormat::V408:
			//    return 24;

			//case EResourceFormat::R8G8_TYPELESS:
		case EResourceFormat::R8G8_UNORM:
		case EResourceFormat::R8G8_UINT:
		case EResourceFormat::R8G8_SNORM:
		case EResourceFormat::R8G8_SINT:
		case EResourceFormat::R16_TYPELESS:
		case EResourceFormat::R16_FLOAT:
		case EResourceFormat::D16_UNORM:
		case EResourceFormat::R16_UNORM:
		case EResourceFormat::R16_UINT:
		case EResourceFormat::R16_SNORM:
		case EResourceFormat::R16_SINT:
			//case EResourceFormat::B5G6R5_UNORM:
			//case EResourceFormat::B5G5R5A1_UNORM:
			//case EResourceFormat::A8P8:
			//case EResourceFormat::B4G4R4A4_UNORM:
			//case WIN10_EResourceFormat::P208:
			//case WIN10_EResourceFormat::V208:
			return 16;

			//case EResourceFormat::NV12:
			//case EResourceFormat::420_OPAQUE:
			//case EResourceFormat::NV11:
			//    return 12;

			//case EResourceFormat::R8_TYPELESS:
		case EResourceFormat::R8_UNORM:
		case EResourceFormat::R8_UINT:
		case EResourceFormat::R8_SNORM:
		case EResourceFormat::R8_SINT:
			//case EResourceFormat::A8_UNORM:
			//case EResourceFormat::AI44:
			//case EResourceFormat::IA44:
			//case EResourceFormat::P8:
			//case XBOX_EResourceFormat::R4G4_UNORM:
			return 8;

			//case EResourceFormat::R1_UNORM:
			//    return 1;

			//case EResourceFormat::BC1_TYPELESS:
		case EResourceFormat::BC1_UNORM:
		case EResourceFormat::BC1_UNORM_SRGB:
			//case EResourceFormat::BC4_TYPELESS:
		case EResourceFormat::BC4_UNORM:
		case EResourceFormat::BC4_SNORM:
			return 4;

			//case EResourceFormat::BC2_TYPELESS:
		case EResourceFormat::BC2_UNORM:
		case EResourceFormat::BC2_UNORM_SRGB:
			//case EResourceFormat::BC3_TYPELESS:
		case EResourceFormat::BC3_UNORM:
		case EResourceFormat::BC3_UNORM_SRGB:
			//case EResourceFormat::BC5_TYPELESS:
		case EResourceFormat::BC5_UNORM:
		case EResourceFormat::BC5_SNORM:
			//case EResourceFormat::BC6H_TYPELESS:
		case EResourceFormat::BC6H_UF16:
		case EResourceFormat::BC6H_SF16:
			//case EResourceFormat::BC7_TYPELESS:
		case EResourceFormat::BC7_UNORM:
		case EResourceFormat::BC7_UNORM_SRGB:
			return 8;

		default:
			return 0;
		}
	}
}
