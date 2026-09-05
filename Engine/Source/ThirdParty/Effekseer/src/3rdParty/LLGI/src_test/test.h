
#pragma once

#include <LLGI.Buffer.h>
#include <LLGI.CommandList.h>
#include <LLGI.Compiler.h>
#include <LLGI.Graphics.h>
#include <LLGI.PipelineState.h>
#include <LLGI.Platform.h>
#include <LLGI.Shader.h>
#include <LLGI.Texture.h>

#include <iostream>

struct SimpleVertex
{
	LLGI::Vec3F Pos;
	LLGI::Vec2F UV;
	LLGI::Color8 Color;
};

struct SimpleVertexF
{
	LLGI::Vec3F Pos;
	LLGI::Vec2F UV;
	LLGI::ColorF Color;
};
