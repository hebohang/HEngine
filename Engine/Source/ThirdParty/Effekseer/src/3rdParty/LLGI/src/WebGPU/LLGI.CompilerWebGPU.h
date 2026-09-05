#pragma once

#include "LLGI.BaseWebGPU.h"
#include "../LLGI.Compiler.h"

namespace LLGI
{

class CompilerWebGPU : public Compiler
{
public:
	void Compile(CompilerResult& result, const char* code, ShaderStageType shaderStage) override;

	DeviceType GetDeviceType() const override { return DeviceType::WebGPU; }
};

} // namespace LLGI
