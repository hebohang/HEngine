
#include <memory>

#if defined(ENABLE_VULKAN_COMPILER)

#if defined(ENABLE_GLSLANG_WITHOUT_INSTALL)
#include <SPIRV/GlslangToSpv.h>
#else
#include <glslang/SPIRV/GlslangToSpv.h>
#endif

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#endif

#include "LLGI.CompilerVulkan.h"

namespace LLGI
{

#if defined(ENABLE_VULKAN_COMPILER)

#endif

CompilerVulkan::CompilerVulkan()
{
#if defined(ENABLE_VULKAN_COMPILER)
	glslang::InitializeProcess();
#endif
}

CompilerVulkan::~CompilerVulkan()
{
#if defined(ENABLE_VULKAN_COMPILER)
	glslang::FinalizeProcess();
#endif
}

void CompilerVulkan::Initialize() {}

void CompilerVulkan::Compile(CompilerResult& result, const char* code, ShaderStageType shaderStage)
{
#if defined(ENABLE_VULKAN_COMPILER)
	EShLanguage stage;
	switch (shaderStage)
	{
	case ShaderStageType::Vertex:
		stage = EShLanguage::EShLangVertex;
		break;
	case ShaderStageType::Pixel:
		stage = EShLanguage::EShLangFragment;
		break;
	case ShaderStageType::Compute:
		stage = EShLanguage::EShLangCompute;
		break;
	default:
		result.Message = "Invalid shader stage.";
		return;
	}

	auto shader = std::make_shared<glslang::TShader>(stage);

	int ClientInputSemanticsVersion = 100; // #define VULKAN 100
	glslang::EShTargetClientVersion VulkanClientVersion = glslang::EShTargetVulkan_1_0;
	glslang::EShTargetLanguageVersion TargetVersion = glslang::EShTargetSpv_1_0;

	const char* shaderCode[1] = {code};
	const int shaderLenght[1] = {static_cast<int>(strlen(code))};
	const char* shaderName[1] = {"shadercode"};
	shader->setStringsWithLengthsAndNames(shaderCode, shaderLenght, shaderName, 1);
	shader->setEntryPoint("main");
	shader->setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, ClientInputSemanticsVersion);
	shader->setEnvClient(glslang::EShClientVulkan, VulkanClientVersion);
	shader->setEnvTarget(glslang::EShTargetSpv, TargetVersion);

	EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);

	// compile
	if (!shader->parse(GetDefaultResources(), 100, false, messages))
	{
		result.Message += "GLSL Parsing Failed:";
		result.Message += shader->getInfoLog();
		result.Message += shader->getInfoDebugLog();
		return;
	}

	// link
	auto program = std::make_shared<glslang::TProgram>();
	program->addShader(shader.get());
	if (!program->link(messages))
	{
		result.Message += shader->getInfoLog();
		result.Message += shader->getInfoDebugLog();
		return;
	}
	else if (shader->getInfoLog())
	{
		result.Message += shader->getInfoLog();
		result.Message += shader->getInfoDebugLog();
	}

	std::vector<unsigned int> spirvCode;
	spv::SpvBuildLogger logger;
	glslang::SpvOptions spvOptions;
	glslang::GlslangToSpv(*program->getIntermediate(stage), spirvCode, &logger, &spvOptions);

	result.Binary.resize(1);
	result.Binary[0].resize(spirvCode.size() * sizeof(unsigned int));
	memcpy(result.Binary[0].data(), spirvCode.data(), result.Binary[0].size());
#endif
}

} // namespace LLGI
