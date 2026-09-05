#pragma once

#include "../LLGI.Graphics.h"
#include "../LLGI.PipelineState.h"

#import <MetalKit/MetalKit.h>

namespace LLGI
{

//! which buffer is used as vertex buffer
const int VertexBufferIndex = 4;

MTLPixelFormat ConvertFormat(TextureFormatType format);

TextureFormatType ConvertFormat(MTLPixelFormat format);

} // namespace LLGI
