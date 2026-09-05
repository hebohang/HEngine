
#pragma once

#include <stdint.h>

#include "../../src/LLGI.Shader.h"
#include <memory>
#include <string>
#include <vector>

namespace LLGI
{

struct ShaderReflectionUniform
{
	std::string Name;
	int32_t Offset = 0;
	int32_t Size = 0;
};

struct ShaderReflectionTexture
{
	std::string Name;
	int32_t Offset = 0;
};

class SPIRV
{
private:
	std::vector<uint32_t> data_;
	std::string error_;
	ShaderStageType shaderStage_;

public:
	SPIRV(const std::vector<uint32_t>& data, ShaderStageType shaderStage);

	SPIRV(const std::string& error);

	ShaderStageType GetStage() const;

	const std::vector<uint32_t>& GetData() const;

	void RemapResourceBindingsForVulkan();

	std::string GetError() const { return error_; }
};

class SPIRVTranspiler
{
protected:
	std::string code_;
	std::string errorCode_;

public:
	SPIRVTranspiler() = default;
	virtual ~SPIRVTranspiler() = default;

	virtual bool Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType);
	std::string GetErrorCode() const;
	std::string GetCode() const;
};

/**
	@brief it doesn't work currently
*/

class SPIRVToHLSLTranspiler : public SPIRVTranspiler
{
	int32_t shaderModel_ = 30;
	bool isDX12_ = false;

public:
	SPIRVToHLSLTranspiler(int32_t shaderModel = 40, bool isDX12 = false);
	bool Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType) override;
};

class SPIRVToMSLTranspiler : public SPIRVTranspiler
{
public:
	bool Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType) override;
};

class SPIRVToGLSLTranspiler : public SPIRVTranspiler
{
private:
	bool isVulkanMode_ = false;
	bool isES_ = false;
	int32_t shaderModel_ = 430;
	bool plain_ = false;

public:
	SPIRVToGLSLTranspiler(bool isVulkanMode, int32_t shaderModel = 430, bool isES = false, bool plain = false)
		: isVulkanMode_(isVulkanMode), isES_(isES), shaderModel_(shaderModel), plain_(plain)
	{
	}

	bool Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType) override;
};

class SPIRVToWGSLTranspiler : public SPIRVTranspiler
{
	bool fixMatrixDirection_ = false;

public:
	SPIRVToWGSLTranspiler(bool fixMatrixDirection = false);
	~SPIRVToWGSLTranspiler() override;
	bool Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType) override;
};

class SPIRVReflection : public SPIRVTranspiler
{
public:
	bool Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType) override;

	std::vector<ShaderReflectionUniform> Uniforms;

	std::vector<ShaderReflectionTexture> Textures;

	std::vector<ShaderResourceBinding> ResourceBindings;

	Vec3I NumThreads;
};

class SPIRVGeneratorMacro
{
public:
	std::string Name;
	std::string Content;

	SPIRVGeneratorMacro() {}

	SPIRVGeneratorMacro(const char* name, const char* content) : Name(name), Content(content) {}
};

class SPIRVGenerator
{
private:
	std::function<std::vector<std::uint8_t>(std::string)> onLoad_;

public:
	SPIRVGenerator(const std::function<std::vector<std::uint8_t>(std::string)>& onLoad);

	~SPIRVGenerator();

	std::shared_ptr<SPIRV> Generate(const char* path,
									const char* code,
									std::vector<std::string> includeDirs,
									std::vector<SPIRVGeneratorMacro> macros,
									ShaderStageType shaderStageType,
									bool isYInverted,
									bool addBindingOffset);
};

} // namespace LLGI
