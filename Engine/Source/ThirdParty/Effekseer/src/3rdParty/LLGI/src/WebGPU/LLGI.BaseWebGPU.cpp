#include "LLGI.BaseWebGPU.h"

namespace LLGI
{

wgpu::BlendOperation Convert(BlendEquationType type)
{
	if (type == BlendEquationType::Add)
		return wgpu::BlendOperation::Add;
	if (type == BlendEquationType::Max)
		return wgpu::BlendOperation::Max;
	if (type == BlendEquationType::Min)
		return wgpu::BlendOperation::Min;
	if (type == BlendEquationType::ReverseSub)
		return wgpu::BlendOperation::ReverseSubtract;
	if (type == BlendEquationType::Sub)
		return wgpu::BlendOperation::Subtract;

	throw "Not implemented";
}

wgpu::BlendFactor Convert(BlendFuncType type)
{
	if (type == BlendFuncType::Zero)
		return wgpu::BlendFactor::Zero;
	if (type == BlendFuncType::One)
		return wgpu::BlendFactor::One;
	if (type == BlendFuncType::SrcColor)
		return wgpu::BlendFactor::Src;
	if (type == BlendFuncType::OneMinusSrcColor)
		return wgpu::BlendFactor::OneMinusSrc;
	if (type == BlendFuncType::SrcAlpha)
		return wgpu::BlendFactor::SrcAlpha;
	if (type == BlendFuncType::OneMinusSrcAlpha)
		return wgpu::BlendFactor::OneMinusSrcAlpha;
	if (type == BlendFuncType::DstAlpha)
		return wgpu::BlendFactor::DstAlpha;
	if (type == BlendFuncType::OneMinusDstAlpha)
		return wgpu::BlendFactor::OneMinusDstAlpha;
	if (type == BlendFuncType::DstColor)
		return wgpu::BlendFactor::Dst;
	if (type == BlendFuncType::OneMinusDstColor)
		return wgpu::BlendFactor::OneMinusDst;

	throw "Not implemented";
}

wgpu::PrimitiveTopology Convert(TopologyType type)
{
	if (type == TopologyType::Point)
		return wgpu::PrimitiveTopology::PointList;

	if (type == TopologyType::Line)
		return wgpu::PrimitiveTopology::LineList;

	if (type == TopologyType::Triangle)
		return wgpu::PrimitiveTopology::TriangleList;

	throw "Not implemented";
}

wgpu::CompareFunction Convert(CompareFuncType type)
{
	if (type == CompareFuncType::Always)
		return wgpu::CompareFunction::Always;

	if (type == CompareFuncType::Equal)
		return wgpu::CompareFunction::Equal;

	if (type == CompareFuncType::Greater)
		return wgpu::CompareFunction::Greater;

	if (type == CompareFuncType::GreaterEqual)
		return wgpu::CompareFunction::GreaterEqual;

	if (type == CompareFuncType::Less)
		return wgpu::CompareFunction::Less;

	if (type == CompareFuncType::LessEqual)
		return wgpu::CompareFunction::LessEqual;

	if (type == CompareFuncType::Never)
		return wgpu::CompareFunction::Never;

	if (type == CompareFuncType::NotEqual)
		return wgpu::CompareFunction::NotEqual;

	throw "Not implemented";
}

wgpu::CompareFunction Convert(DepthFuncType type)
{
	if (type == DepthFuncType::Always)
		return wgpu::CompareFunction::Always;

	if (type == DepthFuncType::Equal)
		return wgpu::CompareFunction::Equal;

	if (type == DepthFuncType::Greater)
		return wgpu::CompareFunction::Greater;

	if (type == DepthFuncType::GreaterEqual)
		return wgpu::CompareFunction::GreaterEqual;

	if (type == DepthFuncType::Less)
		return wgpu::CompareFunction::Less;

	if (type == DepthFuncType::LessEqual)
		return wgpu::CompareFunction::LessEqual;

	if (type == DepthFuncType::Never)
		return wgpu::CompareFunction::Never;

	if (type == DepthFuncType::NotEqual)
		return wgpu::CompareFunction::NotEqual;

	throw "Not implemented";
}


wgpu::CullMode Convert(CullingMode mode)
{
	if (mode == CullingMode::Clockwise)
		return wgpu::CullMode::Back;

	if (mode == CullingMode::CounterClockwise)
		return wgpu::CullMode::Front;

	if (mode == CullingMode::DoubleSide)
		return wgpu::CullMode::None;

	throw "Not implemented";
}

wgpu::VertexFormat Convert(VertexLayoutFormat format)
{
	if (format == VertexLayoutFormat::R32_FLOAT)
		return wgpu::VertexFormat::Float32;

	if (format == VertexLayoutFormat::R32G32_FLOAT)
		return wgpu::VertexFormat::Float32x2;

	if (format == VertexLayoutFormat::R32G32B32_FLOAT)
		return wgpu::VertexFormat::Float32x3;

	if (format == VertexLayoutFormat::R32G32B32A32_FLOAT)
		return wgpu::VertexFormat::Float32x4;

	if (format == VertexLayoutFormat::R8G8B8A8_UNORM)
		return wgpu::VertexFormat::Unorm8x4;

	if (format == VertexLayoutFormat::R8G8B8A8_UINT)
		return wgpu::VertexFormat::Uint8x4;

	throw "Not implemented";
}

wgpu::StencilOperation Convert(StencilOperatorType type)
{
	if (type == StencilOperatorType::Keep)
		return wgpu::StencilOperation::Keep;

	if (type == StencilOperatorType::Zero)
		return wgpu::StencilOperation::Zero;

	if (type == StencilOperatorType::Replace)
		return wgpu::StencilOperation::Replace;

	if (type == StencilOperatorType::Invert)
		return wgpu::StencilOperation::Invert;

	if (type == StencilOperatorType::IncClamp)
		return wgpu::StencilOperation::IncrementClamp;

	if (type == StencilOperatorType::DecClamp)
		return wgpu::StencilOperation::DecrementClamp;

	if (type == StencilOperatorType::IncRepeat)
		return wgpu::StencilOperation::IncrementWrap;

	if (type == StencilOperatorType::DecRepeat)
		return wgpu::StencilOperation::DecrementWrap;

	throw "Not implemented";
}

wgpu::TextureFormat ConvertFormat(TextureFormatType format)
{
	if (format == TextureFormatType::R8G8B8A8_UNORM)
		return wgpu::TextureFormat::RGBA8Unorm;

	if (format == TextureFormatType::B8G8R8A8_UNORM)
		return wgpu::TextureFormat::BGRA8Unorm;

	if (format == TextureFormatType::RG11B10_UFLOAT)
		return wgpu::TextureFormat::RG11B10Ufloat;

	if (format == TextureFormatType::R16G16B16A16_FLOAT)
		return wgpu::TextureFormat::RGBA16Float;

	if (format == TextureFormatType::R32G32B32A32_FLOAT)
		return wgpu::TextureFormat::RGBA32Float;

	if (format == TextureFormatType::R8G8B8A8_UNORM_SRGB)
		return wgpu::TextureFormat::RGBA8UnormSrgb;

	if (format == TextureFormatType::B8G8R8A8_UNORM_SRGB)
		return wgpu::TextureFormat::BGRA8UnormSrgb;

	if (format == TextureFormatType::R16G16_FLOAT)
		return wgpu::TextureFormat::RG16Float;

	if (format == TextureFormatType::R8_UNORM)
		return wgpu::TextureFormat::R8Unorm;

	if (format == TextureFormatType::BC1)
		return wgpu::TextureFormat::BC1RGBAUnorm;

	if (format == TextureFormatType::BC2)
		return wgpu::TextureFormat::BC2RGBAUnorm;

	if (format == TextureFormatType::BC3)
		return wgpu::TextureFormat::BC3RGBAUnorm;

	if (format == TextureFormatType::BC7)
		return wgpu::TextureFormat::BC7RGBAUnorm;

	if (format == TextureFormatType::BC1_SRGB)
		return wgpu::TextureFormat::BC1RGBAUnormSrgb;

	if (format == TextureFormatType::BC2_SRGB)
		return wgpu::TextureFormat::BC2RGBAUnormSrgb;

	if (format == TextureFormatType::BC3_SRGB)
		return wgpu::TextureFormat::BC3RGBAUnormSrgb;

	if (format == TextureFormatType::BC7_SRGB)
		return wgpu::TextureFormat::BC7RGBAUnormSrgb;

	if (format == TextureFormatType::D32)
		return wgpu::TextureFormat::Depth32Float;

	if (format == TextureFormatType::D24S8)
		return wgpu::TextureFormat::Depth24PlusStencil8;

	if (format == TextureFormatType::D32S8)
		return wgpu::TextureFormat::Depth32FloatStencil8;

	if (format == TextureFormatType::Unknown)
		return wgpu::TextureFormat::Undefined;

	throw "Not implemented";
}

TextureFormatType ConvertFormat(wgpu::TextureFormat format)
{
	if (format == wgpu::TextureFormat::RGBA8Unorm)
		return TextureFormatType::R8G8B8A8_UNORM;

	if (format == wgpu::TextureFormat::BGRA8Unorm)
		return TextureFormatType::B8G8R8A8_UNORM;

	if (format == wgpu::TextureFormat::RG11B10Ufloat)
		return TextureFormatType::RG11B10_UFLOAT;

	if (format == wgpu::TextureFormat::RGBA16Float)
		return TextureFormatType::R16G16B16A16_FLOAT;

	if (format == wgpu::TextureFormat::RGBA32Float)
		return TextureFormatType::R32G32B32A32_FLOAT;

	if (format == wgpu::TextureFormat::RGBA8UnormSrgb)
		return TextureFormatType::R8G8B8A8_UNORM_SRGB;

	if (format == wgpu::TextureFormat::BGRA8UnormSrgb)
		return TextureFormatType::B8G8R8A8_UNORM_SRGB;

	if (format == wgpu::TextureFormat::RG16Float)
		return TextureFormatType::R16G16_FLOAT;

	if (format == wgpu::TextureFormat::R8Unorm)
		return TextureFormatType::R8_UNORM;

	if (format == wgpu::TextureFormat::BC1RGBAUnorm)
		return TextureFormatType::BC1;

	if (format == wgpu::TextureFormat::BC2RGBAUnorm)
		return TextureFormatType::BC2;

	if (format == wgpu::TextureFormat::BC3RGBAUnorm)
		return TextureFormatType::BC3;

	if (format == wgpu::TextureFormat::BC7RGBAUnorm)
		return TextureFormatType::BC7;

	if (format == wgpu::TextureFormat::BC1RGBAUnormSrgb)
		return TextureFormatType::BC1_SRGB;

	if (format == wgpu::TextureFormat::BC2RGBAUnormSrgb)
		return TextureFormatType::BC2_SRGB;

	if (format == wgpu::TextureFormat::BC3RGBAUnormSrgb)
		return TextureFormatType::BC3_SRGB;

	if (format == wgpu::TextureFormat::BC7RGBAUnormSrgb)
		return TextureFormatType::BC7_SRGB;

	if (format == wgpu::TextureFormat::Depth32Float)
		return TextureFormatType::D32;

	if (format == wgpu::TextureFormat::Depth24PlusStencil8)
		return TextureFormatType::D24S8;

	if (format == wgpu::TextureFormat::Depth32FloatStencil8)
		return TextureFormatType::D32S8;

	if (format == wgpu::TextureFormat::Undefined)
		return TextureFormatType::Unknown;

	throw "Not implemented";
}

int32_t GetSize(VertexLayoutFormat format)
{
	if (format == VertexLayoutFormat::R32G32B32_FLOAT)
	{
		return sizeof(float) * 3;
	}
	else if (format == VertexLayoutFormat::R32G32B32A32_FLOAT)
	{
		return sizeof(float) * 4;
	}
	else if (format == VertexLayoutFormat::R32_FLOAT)
	{
		return sizeof(float) * 1;
	}
	else if (format == VertexLayoutFormat::R32G32_FLOAT)
	{
		return sizeof(float) * 2;
	}
	else if (format == VertexLayoutFormat::R8G8B8A8_UINT)
	{
		return sizeof(float);
	}
	else if (format == VertexLayoutFormat::R8G8B8A8_UNORM)
	{
		return sizeof(float);
	}
	else
	{
		Log(LogType::Error, "Unimplemented VertexLoayoutFormat");
		return 0;
	}
}

} // namespace LLGI
