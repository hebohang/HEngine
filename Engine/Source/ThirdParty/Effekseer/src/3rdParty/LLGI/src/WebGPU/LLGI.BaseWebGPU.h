
#pragma once

#include <webgpu/webgpu.h>
#include <webgpu/webgpu_cpp.h>
#include "../LLGI.Base.h"

namespace LLGI
{

wgpu::BlendOperation Convert(BlendEquationType type);

wgpu::BlendFactor Convert(BlendFuncType type);

wgpu::PrimitiveTopology Convert(TopologyType type);

wgpu::CompareFunction Convert(CompareFuncType type);

wgpu::CompareFunction Convert(DepthFuncType type);

wgpu::CullMode Convert(CullingMode mode);

wgpu::VertexFormat Convert(VertexLayoutFormat format);

wgpu::StencilOperation Convert(StencilOperatorType type);

wgpu::TextureFormat ConvertFormat(TextureFormatType format);

TextureFormatType ConvertFormat(wgpu::TextureFormat format);

int32_t GetSize(VertexLayoutFormat format);

} // namespace std
