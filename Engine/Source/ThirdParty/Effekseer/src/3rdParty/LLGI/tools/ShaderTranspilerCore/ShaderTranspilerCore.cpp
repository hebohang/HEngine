#include "ShaderTranspilerCore.h"

#if defined(ENABLE_GLSLANG_WITHOUT_INSTALL)
#include <SPIRV/GlslangToSpv.h>
#else
#include <glslang/SPIRV/GlslangToSpv.h>
#endif

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include <functional>
#include <set>
#include <unordered_map>

#if (ENABLE_SPIRVCROSS_WITHOUT_INSTALL)
#include <spirv_cross.hpp>
#include <spirv_glsl.hpp>
#include <spirv_hlsl.hpp>
#include <spirv_msl.hpp>
#include <spirv_reflect.hpp>
#else
#include <spirv_cross/spirv_cross.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include <spirv_cross/spirv_hlsl.hpp>
#include <spirv_cross/spirv_msl.hpp>
#include <spirv_cross/spirv_reflect.hpp>
#endif

#if (ENABLE_WEBGPU)
#include <tint/tint.h>
#endif

#include <iostream>
#include <regex>
#include <sstream>

namespace LLGI
{

namespace
{
std::string Replace(std::string target, std::string from_, std::string to_)
{
	std::string::size_type Pos(target.find(from_));

	while (Pos != std::string::npos)
	{
		target.replace(Pos, from_.length(), to_);
		Pos = target.find(from_, Pos + to_.length());
	}

	return target;
}

#if (ENABLE_WEBGPU)
struct WGSLStructInfo
{
	std::unordered_map<std::string, std::string> ArrayMembers;
	std::set<std::string> SquareMatrixMembers;
};

std::string NormalizeWGSLMatrixDirection(std::string code)
{
	static const std::regex modelMatrixRegex(R"(\((localPos|localPosition|worldPos|localNormal|localBinormal|localTangent) \* (mModel)\))");
	static const std::regex cameraMatrixRegex(R"(\((\w+) \* (v\._\w+_mCameraProj)\))");
	static const std::regex cameraMatrixExpressionRegex(R"(\(\(([^()]+ \+ [^()]+)\) \* (v\._\w+_mCameraProj)\))");
	static const std::regex cameraProjMatrixRegex(
		R"(\(\((vec4<f32>\([^()]+\)) \* ([A-Za-z_]\w*\.[A-Za-z_]\w*\.CameraMat)\) \* ([A-Za-z_]\w*\.[A-Za-z_]\w*\.ProjMat)\))");

	code = std::regex_replace(code, modelMatrixRegex, "($2 * $1)");
	code = std::regex_replace(code, cameraMatrixRegex, "($2 * $1)");
	code = std::regex_replace(code, cameraMatrixExpressionRegex, "($2 * ($1))");
	code = std::regex_replace(code, cameraProjMatrixRegex, "($3 * ($2 * $1))");
	code = Replace(code, "worldNormal = normalize(worldNormal);", "worldNormal = vec4<f32>(normalize(worldNormal.xyz), 0.0f);");
	code = Replace(code, "worldBinormal = normalize(worldBinormal);", "worldBinormal = vec4<f32>(normalize(worldBinormal.xyz), 0.0f);");
	code = Replace(code, "worldTangent = normalize(worldTangent);", "worldTangent = vec4<f32>(normalize(worldTangent.xyz), 0.0f);");
	return code;
}

std::unordered_map<std::string, WGSLStructInfo> CollectWGSLStructInfo(const std::string& code)
{
	std::unordered_map<std::string, WGSLStructInfo> structInfo;
	const std::regex structBeginRegex(R"(^\s*struct\s+([A-Za-z_]\w*)\s*\{\s*$)");
	const std::regex structEndRegex(R"(^\s*\}\s*$)");
	const std::regex arrayMemberRegex(R"(^\s*([A-Za-z_]\w*)\s*:\s*array<([A-Za-z_]\w*)>\s*,\s*$)");
	const std::regex matrixMemberRegex(R"(^\s*([A-Za-z_]\w*)\s*:\s*mat([234])x([234])<f32>\s*,\s*$)");

	std::stringstream input(code);
	std::string line;
	std::string currentStruct;
	while (std::getline(input, line))
	{
		std::smatch match;
		if (currentStruct.empty())
		{
			if (std::regex_match(line, match, structBeginRegex))
			{
				currentStruct = match[1].str();
				structInfo[currentStruct] = WGSLStructInfo{};
			}
			continue;
		}

		if (std::regex_match(line, structEndRegex))
		{
			currentStruct.clear();
			continue;
		}

		if (std::regex_match(line, match, arrayMemberRegex))
		{
			structInfo[currentStruct].ArrayMembers[match[1].str()] = match[2].str();
		}
		else if (std::regex_match(line, match, matrixMemberRegex) && match[2].str() == match[3].str())
		{
			structInfo[currentStruct].SquareMatrixMembers.insert(match[1].str());
		}
	}

	return structInfo;
}

std::string NormalizeWGSLSquareMatrixStorageStores(std::string code)
{
	const auto structInfo = CollectWGSLStructInfo(code);
	const std::regex storageVarRegex(R"(^\s*@group\([^)]+\)\s+@binding\([^)]+\)\s+var<storage[^>]*>\s+([A-Za-z_]\w*)\s*:\s*([A-Za-z_]\w*)\s*;\s*$)");
	const std::regex storageElementAliasRegex(R"(^(\s*)let\s+([A-Za-z_]\w*)\s*=\s*&\(([A-Za-z_]\w*)\.([A-Za-z_]\w*)\[.*\]\)\s*;\s*$)");
	const std::regex storageStoreRegex(R"(^(\s*)\(\*\(([A-Za-z_]\w*)\)\)\.([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\.([A-Za-z_]\w*)\s*;\s*$)");

	std::unordered_map<std::string, std::string> storageVarElementStructs;
	std::unordered_map<std::string, std::string> aliasElementStructs;
	std::stringstream input(code);
	std::stringstream output;
	std::string line;
	while (std::getline(input, line))
	{
		std::smatch match;
		if (std::regex_match(line, match, storageVarRegex))
		{
			const auto storageName = match[1].str();
			const auto wrapperStruct = match[2].str();
			auto wrapperIt = structInfo.find(wrapperStruct);
			if (wrapperIt != structInfo.end() && wrapperIt->second.ArrayMembers.size() == 1)
			{
				storageVarElementStructs[storageName] = wrapperIt->second.ArrayMembers.begin()->second;
			}
		}
		else if (std::regex_match(line, match, storageElementAliasRegex))
		{
			const auto aliasName = match[2].str();
			const auto storageName = match[3].str();
			auto storageIt = storageVarElementStructs.find(storageName);
			if (storageIt != storageVarElementStructs.end())
			{
				aliasElementStructs[aliasName] = storageIt->second;
			}
		}
		else if (std::regex_match(line, match, storageStoreRegex))
		{
			const auto indent = match[1].str();
			const auto aliasName = match[2].str();
			const auto lhsField = match[3].str();
			const auto rhsName = match[4].str();
			const auto rhsField = match[5].str();
			auto aliasIt = aliasElementStructs.find(aliasName);
			if (aliasIt != aliasElementStructs.end())
			{
				auto structIt = structInfo.find(aliasIt->second);
				if (structIt != structInfo.end() && lhsField == rhsField &&
					structIt->second.SquareMatrixMembers.count(lhsField) > 0)
				{
					output << indent << "(*(" << aliasName << "))." << lhsField << " = transpose(" << rhsName << "." << rhsField << ");\n";
					continue;
				}
			}
		}

		output << line << "\n";
	}

	return output.str();
}

std::string NormalizeWGSLForLLGI(std::string code, ShaderStageType shaderStageType, bool fixMatrixDirection)
{
	for (uint32_t i = 0; i < TextureSlotMax; i++)
	{
		code = Replace(code, "@group(0u) @binding(" + std::to_string(300 + i) + "u)", "@group(0) @binding(" + std::to_string(i) + ")");
		code = Replace(code, "@group(0) @binding(" + std::to_string(300 + i) + ")", "@group(0) @binding(" + std::to_string(i) + ")");

		const auto storageGroup = shaderStageType == ShaderStageType::Compute ? 2 : 1;
		code = Replace(code,
					   "@group(0u) @binding(" + std::to_string(400 + i) + "u)",
					   "@group(" + std::to_string(storageGroup) + ") @binding(" + std::to_string(i) + ")");
		code = Replace(code,
					   "@group(0) @binding(" + std::to_string(400 + i) + ")",
					   "@group(" + std::to_string(storageGroup) + ") @binding(" + std::to_string(i) + ")");

		code = Replace(code, "@group(0u) @binding(" + std::to_string(100 + i) + "u)", "@group(1) @binding(" + std::to_string(i) + ")");
		code = Replace(code, "@group(0) @binding(" + std::to_string(100 + i) + ")", "@group(1) @binding(" + std::to_string(i) + ")");

		code = Replace(code, "@group(0u) @binding(" + std::to_string(200 + i) + "u)", "@group(1) @binding(" + std::to_string(i) + ")");
		code = Replace(code, "@group(0) @binding(" + std::to_string(200 + i) + ")", "@group(1) @binding(" + std::to_string(i) + ")");
	}

	std::stringstream input(code);
	std::stringstream output;
	std::string line;
	while (std::getline(input, line))
	{
		if (line.find(": sampler") != std::string::npos)
		{
			for (uint32_t i = 0; i < TextureSlotMax; i++)
			{
				line = Replace(line, "@group(0u) @binding(" + std::to_string(i) + "u)", "@group(2) @binding(" + std::to_string(i) + ")");
				line = Replace(line, "@group(0) @binding(" + std::to_string(i) + ")", "@group(2) @binding(" + std::to_string(i) + ")");
			}
		}
		output << line << "\n";
	}
	code = output.str();

	if (fixMatrixDirection)
	{
		code = NormalizeWGSLMatrixDirection(code);
	}

	code = NormalizeWGSLSquareMatrixStorageStores(code);

	return code;
}
#endif

// https://stackoverflow.com/questions/8518743/get-directory-from-file-path-c/14631366
std::string dirnameOf(const std::string& fname)
{
	size_t pos = fname.find_last_of("\\/");
	return (std::string::npos == pos) ? "" : fname.substr(0, pos);
}
} // namespace

// Based on https://github.com/KhronosGroup/glslang/blob/master/StandAlone/DirStackFileIncluder.h

// Default include class for normal include convention of search backward
// through the stack of active include paths (for nested includes).
// Can be overridden to customize.
class DirStackFileIncluder : public glslang::TShader::Includer
{
public:
	DirStackFileIncluder(const std::function<std::vector<std::uint8_t>(std::string)>& onLoad)
		: externalLocalDirectoryCount(0), onLoad_(onLoad)
	{
	}

	virtual IncludeResult* includeLocal(const char* headerName, const char* includerName, size_t inclusionDepth) override
	{
		return readLocalPath(headerName, includerName, static_cast<int>(inclusionDepth));
	}

	virtual IncludeResult* includeSystem(const char* headerName, const char* /*includerName*/, size_t /*inclusionDepth*/) override
	{
		return readSystemPath(headerName);
	}

	// Externally set directories. E.g., from a command-line -I<dir>.
	//  - Most-recently pushed are checked first.
	//  - All these are checked after the parse-time stack of local directories
	//    is checked.
	//  - This only applies to the "local" form of #include.
	//  - Makes its own copy of the path.
	virtual void pushExternalLocalDirectory(const std::string& dir)
	{
		directoryStack.push_back(dir);
		externalLocalDirectoryCount = static_cast<int>(directoryStack.size());
	}

	virtual void releaseInclude(IncludeResult* result) override
	{
		if (result != nullptr)
		{
			delete[] static_cast<tUserDataElement*>(result->userData);
			delete result;
		}
	}

	virtual ~DirStackFileIncluder() override {}

protected:
	typedef char tUserDataElement;
	std::vector<std::string> directoryStack;
	int externalLocalDirectoryCount;
	std::function<std::vector<std::uint8_t>(std::string)> onLoad_;

	// Search for a valid "local" path based on combining the stack of include
	// directories and the nominal name of the header.
	virtual IncludeResult* readLocalPath(const char* headerName, const char* includerName, int depth)
	{
		// Discard popped include directories, and
		// initialize when at parse-time first level.
		directoryStack.resize(depth + externalLocalDirectoryCount);
		if (depth == 1)
			directoryStack.back() = getDirectory(includerName);

		// Find a directory that works, using a reverse search of the include stack.
		for (auto it = directoryStack.rbegin(); it != directoryStack.rend(); ++it)
		{
			std::string path = *it + '/' + headerName;
			std::replace(path.begin(), path.end(), '\\', '/');

			auto file = onLoad_(path);

			if (file.size() > 0)
			{
				directoryStack.push_back(getDirectory(path));

				char* content = new tUserDataElement[file.size()];
				memcpy(content, file.data(), file.size());
				return new IncludeResult(path, content, file.size(), content);
			}
		}

		return nullptr;
	}

	// Search for a valid <system> path.
	// Not implemented yet; returning nullptr signals failure to find.
	virtual IncludeResult* readSystemPath(const char* /*headerName*/) const { return nullptr; }

	// If no path markers, return current working directory.
	// Otherwise, strip file name and return path leading up to it.
	virtual std::string getDirectory(const std::string path) const
	{
		size_t last = path.find_last_of("/\\");
		return last == std::string::npos ? "." : path.substr(0, last);
	}
};

namespace
{
EShLanguage GetGlslangShaderStage(ShaderStageType type)
{
	if (type == ShaderStageType::Vertex)
		return EShLanguage::EShLangVertex;
	if (type == ShaderStageType::Pixel)
		return EShLanguage::EShLangFragment;
	if (type == ShaderStageType::Compute)
		return EShLanguage::EShLangCompute;
	throw std::string("Unimplemented ShaderStage");
}

uint32_t MakeSpvInstruction(spv::Op op, uint32_t wordCount)
{
	return (wordCount << 16) | static_cast<uint32_t>(op);
}

bool IsAnnotationInstruction(spv::Op op)
{
	switch (op)
	{
	case spv::OpDecorate:
	case spv::OpMemberDecorate:
	case spv::OpGroupDecorate:
	case spv::OpGroupMemberDecorate:
	case spv::OpDecorateId:
	case spv::OpDecorateStringGOOGLE:
	case spv::OpMemberDecorateStringGOOGLE:
		return true;
	default:
		return false;
	}
}

bool SetDecorationValue(std::vector<uint32_t>& data, uint32_t targetId, spv::Decoration decoration, uint32_t value)
{
	for (size_t i = 5; i < data.size();)
	{
		const auto word = data[i];
		const auto wordCount = word >> 16;
		const auto op = static_cast<spv::Op>(word & 0xffff);

		if (wordCount == 0 || i + wordCount > data.size())
		{
			return false;
		}

		if (op == spv::OpDecorate && wordCount >= 4 && data[i + 1] == targetId &&
			data[i + 2] == static_cast<uint32_t>(decoration))
		{
			data[i + 3] = value;
			return true;
		}

		i += wordCount;
	}

	return false;
}

void SetOrInsertDecorationValue(std::vector<uint32_t>& data, uint32_t targetId, spv::Decoration decoration, uint32_t value)
{
	if (SetDecorationValue(data, targetId, decoration, value))
	{
		return;
	}

	size_t insertPos = 5;
	for (size_t i = 5; i < data.size();)
	{
		const auto word = data[i];
		const auto wordCount = word >> 16;
		const auto op = static_cast<spv::Op>(word & 0xffff);

		if (wordCount == 0 || i + wordCount > data.size())
		{
			break;
		}

		if (IsAnnotationInstruction(op))
		{
			insertPos = i + wordCount;
		}

		i += wordCount;
	}

	const std::vector<uint32_t> instruction = {MakeSpvInstruction(spv::OpDecorate, 4), targetId, static_cast<uint32_t>(decoration), value};
	data.insert(data.begin() + insertPos, instruction.begin(), instruction.end());
}

void SetDescriptorSet(std::vector<uint32_t>& data, const spirv_cross::Resource& resource, uint32_t descriptorSet)
{
	SetOrInsertDecorationValue(data, resource.id, spv::DecorationDescriptorSet, descriptorSet);
}
} // namespace

SPIRV::SPIRV(const std::vector<uint32_t>& data, ShaderStageType shaderStage) : data_(data), shaderStage_(shaderStage) {}

SPIRV::SPIRV(const std::string& error) : error_(error), shaderStage_{} {}

ShaderStageType SPIRV::GetStage() const { return shaderStage_; }

const std::vector<uint32_t>& SPIRV::GetData() const { return data_; }

void SPIRV::RemapResourceBindingsForVulkan()
{
	spirv_cross::Compiler compiler(data_);
	const auto resources = compiler.get_shader_resources();

	for (const auto& resource : resources.uniform_buffers)
	{
		SetDescriptorSet(data_, resource, 0);
	}

	for (const auto& resource : resources.sampled_images)
	{
		SetDescriptorSet(data_, resource, 1);
	}

	for (const auto& resource : resources.separate_images)
	{
		SetDescriptorSet(data_, resource, 1);
	}

	for (const auto& resource : resources.separate_samplers)
	{
		SetDescriptorSet(data_, resource, 1);
	}

	for (const auto& resource : resources.storage_buffers)
	{
		SetDescriptorSet(data_, resource, 2);
	}

	for (const auto& resource : resources.storage_images)
	{
		SetDescriptorSet(data_, resource, 3);
	}
}

bool SPIRVTranspiler::Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType) { return false; }

std::string SPIRVTranspiler::GetErrorCode() const { return errorCode_; }

std::string SPIRVTranspiler::GetCode() const { return code_; }

SPIRVToHLSLTranspiler::SPIRVToHLSLTranspiler(int32_t shaderModel, bool isDX12) : shaderModel_(shaderModel), isDX12_(isDX12) {}

bool SPIRVToHLSLTranspiler::Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType)
{
	spirv_cross::CompilerHLSL compiler(spirv->GetData());

	std::vector<std::pair<std::string, int>> remapping;

	if (shaderModel_ <= 30 && !isDX12_)
	{
		compiler.build_combined_image_samplers();

		spirv_cross::ShaderResources resources = compiler.get_shader_resources();

		for (auto& remap : compiler.get_combined_image_samplers())
		{
			auto name = spirv_cross::join("Sampler_", compiler.get_name(remap.sampler_id));
			compiler.set_name(remap.combined_id, name);

			auto location = compiler.get_decoration(remap.sampler_id, spv::DecorationBinding);

			remapping.push_back(std::make_pair(name, location));
		}
	}

	spirv_cross::CompilerGLSL::Options options;
	options.separate_shader_objects = true;
	compiler.set_common_options(options);

	spirv_cross::CompilerHLSL::Options targetOptions;
	targetOptions.shader_model = shaderModel_;
	targetOptions.force_storage_buffer_as_uav = isDX12_ && shaderStageType == LLGI::ShaderStageType::Compute;
	compiler.set_hlsl_options(targetOptions);

	code_ = compiler.compile();

	if (shaderModel_ <= 30)
	{
		for (auto nr : remapping)
		{
			auto src = "uniform sampler2D " + nr.first;
			auto dst = src + " : register(s" + std::to_string(nr.second) + ")";
			code_ = Replace(code_, src, dst);
		}

		code_ = Replace(code_, "packoffset", "register");
	}

	return true;
}

bool SPIRVToMSLTranspiler::Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType)
{
	spirv_cross::CompilerMSL compiler(spirv->GetData());

	int storage_buffer_offset = 10;

	{
		spirv_cross::ShaderResources resources = compiler.get_shader_resources();

		for (const auto& buffer : resources.storage_buffers)
		{
			auto name = buffer.name;
			auto location = compiler.get_decoration(buffer.id, spv::DecorationBinding);
			compiler.set_decoration(buffer.id, spv::DecorationBinding, location + storage_buffer_offset);
		}
	}
	spirv_cross::CompilerGLSL::Options options;
	compiler.set_common_options(options);

	spirv_cross::CompilerMSL::Options targetOptions;
	targetOptions.enable_decoration_binding = true;
	compiler.set_msl_options(targetOptions);

	code_ = compiler.compile();

	return true;
}

bool SPIRVToGLSLTranspiler::Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType)
{
	spirv_cross::CompilerGLSL compiler(spirv->GetData());

	if (shaderStageType == LLGI::ShaderStageType::Compute)
	{
		// if (!isVulkanMode_)
		{
			compiler.build_dummy_sampler_for_combined_images();

			// to combine a sampler and a texture
			compiler.build_combined_image_samplers();
		}
	}
	else
	{
		// to combine a sampler and a texture
		compiler.build_combined_image_samplers();
	}

	spirv_cross::ShaderResources resources = compiler.get_shader_resources();

	std::map<spirv_cross::VariableID, uint32_t> sampler_id_to_index;

	if (shaderModel_ <= 420 || isVulkanMode_)
	{
		for (auto& remap : compiler.get_combined_image_samplers())
		{
			compiler.set_name(remap.combined_id, spirv_cross::join("Sampler_", compiler.get_name(remap.sampler_id)));
			auto location = compiler.get_decoration(remap.sampler_id, spv::DecorationBinding);

			if (!isVulkanMode_)
			{
				compiler.set_decoration(remap.combined_id, spv::DecorationLocation, location);
			}
		}
	}

	for (auto& remap : compiler.get_combined_image_samplers())
	{
		auto location = compiler.get_decoration(remap.sampler_id, spv::DecorationBinding);
		sampler_id_to_index[remap.combined_id] = location;
	}

	for (auto& resource : resources.sampled_images)
	{
		auto i = sampler_id_to_index[resource.id];
		compiler.set_decoration(resource.id, spv::DecorationBinding, i);

		if (isVulkanMode_)
		{
			compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 1);
		}
	}

	if (shaderStageType == LLGI::ShaderStageType::Compute && isVulkanMode_)
	{
		for (auto& resource : resources.storage_images)
		{
			compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 3);
		}

		for (auto& resource : resources.separate_images)
		{
			compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 1);
		}
	}

	int cb_ind = 0;

	for (auto& resource : resources.uniform_buffers)
	{
		if (!isVulkanMode_)
		{
			if (spirv->GetStage() == ShaderStageType::Vertex)
			{
				compiler.set_name(resource.id, "CBVS" + std::to_string(cb_ind));
			}
			else if (spirv->GetStage() == ShaderStageType::Pixel)
			{
				compiler.set_name(resource.id, "CBPS" + std::to_string(cb_ind));
			}
			else if (spirv->GetStage() == ShaderStageType::Compute)
			{
				compiler.set_name(resource.id, "CBCS" + std::to_string(cb_ind));
			}
		}

		if (isVulkanMode_)
		{
			compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 0);
		}
		cb_ind++;
	}

	if (isVulkanMode_)
	{
		for (auto& resource : resources.storage_buffers)
		{
			compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 2);
		}
	}

	spirv_cross::CompilerGLSL::Options options;
	options.version = shaderModel_;
	if (shaderModel_ >= 420)
	{
		options.enable_420pack_extension = true;

		if (!isVulkanMode_)
		{
			options.separate_shader_objects = true;
		}
	}

	options.emit_uniform_buffer_as_plain_uniforms = plain_;
	options.vulkan_semantics = isVulkanMode_;
	options.es = isES_;

	compiler.set_common_options(options);

	code_ = compiler.compile();

	if (!isVulkanMode_ && shaderModel_ <= 330)
	{
		if (spirv->GetStage() == ShaderStageType::Vertex)
		{
			code_ = Replace(code_, "_entryPointOutput_", "_VSPS_");
		}
		else if (spirv->GetStage() == ShaderStageType::Pixel)
		{
			code_ = Replace(code_, "Input_", "_VSPS_");
		}
	}

	return true;
}

SPIRVToWGSLTranspiler::SPIRVToWGSLTranspiler(bool fixMatrixDirection) : fixMatrixDirection_(fixMatrixDirection)
{
#if (ENABLE_WEBGPU)
	tint::Initialize();
#endif
}

SPIRVToWGSLTranspiler::~SPIRVToWGSLTranspiler()
{
#if (ENABLE_WEBGPU)
	tint::Shutdown();
#endif
}

bool SPIRVToWGSLTranspiler::Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType)
{
#if (ENABLE_WEBGPU)
	tint::wgsl::writer::Options gen_options;
	gen_options.allow_non_uniform_derivatives = true;
	gen_options.allowed_features.features.insert(tint::wgsl::LanguageFeature::kReadonlyAndReadwriteStorageTextures);
	auto result = tint::SpirvToWgsl(spirv->GetData(), gen_options);
	if (result != tint::Success)
	{
		errorCode_ = result.Failure().reason;
		return false;
	}

	code_ = NormalizeWGSLForLLGI(result.Get(), shaderStageType, fixMatrixDirection_);
	return true;
#else
	errorCode_ = "WGSL output requires ShaderTranspilerCore to be built with BUILD_WEBGPU=ON.";
	return false;
#endif
}

class ReflectionCompiler : public spirv_cross::Compiler
{
public:
	ReflectionCompiler(const std::vector<uint32_t>& data) : Compiler(data) {}
	virtual ~ReflectionCompiler() = default;

	size_t get_member_count(uint32_t id) const
	{
		const spirv_cross::Meta& m = ir.meta.at(id);
		return m.members.size();
	}

	spirv_cross::SPIRType get_member_type(const spirv_cross::SPIRType& struct_type, uint32_t index) const
	{
		return get<spirv_cross::SPIRType>(struct_type.member_types[index]);
	}
};

bool SPIRVReflection::Transpile(const std::shared_ptr<SPIRV>& spirv, LLGI::ShaderStageType shaderStageType)
{
	Textures.clear();
	Uniforms.clear();
	ResourceBindings.clear();

	ReflectionCompiler compiler(spirv->GetData());
	spirv_cross::ShaderResources resources = compiler.get_shader_resources();

	// Texture
	for (const auto& sampler : resources.separate_images)
	{
		ShaderReflectionTexture t;
		t.Name = sampler.name;
		t.Offset = compiler.get_decoration(sampler.id, spv::DecorationBinding);
		Textures.push_back(t);

		ShaderResourceBinding binding;
		binding.ResourceType = ShaderResourceType::Texture;
		binding.Access = ShaderResourceAccess::ReadOnly;
		binding.Stage = shaderStageType;
		binding.Slot = t.Offset;
		ResourceBindings.push_back(binding);
	}

	// Uniform
	for (const auto& resource : resources.uniform_buffers)
	{
		ShaderResourceBinding binding;
		binding.ResourceType = ShaderResourceType::UniformBuffer;
		binding.Access = ShaderResourceAccess::ReadOnly;
		binding.Stage = shaderStageType;
		binding.Slot = compiler.get_decoration(resource.id, spv::DecorationBinding);
		ResourceBindings.push_back(binding);

		auto count = compiler.get_member_count(resource.base_type_id);
		auto spirvType = compiler.get_type(resource.type_id);

		for (size_t i = 0; i < count; i++)
		{
			ShaderReflectionUniform u;
			auto memberType = compiler.get_member_type(spirvType, static_cast<uint32_t>(i));
			u.Name = compiler.get_member_name(resource.base_type_id, static_cast<uint32_t>(i));
			u.Size = static_cast<int32_t>(compiler.get_declared_struct_member_size(spirvType, static_cast<uint32_t>(i)));
			u.Offset = compiler.get_member_decoration(resource.base_type_id, static_cast<uint32_t>(i), spv::DecorationOffset);
			Uniforms.push_back(u);
		}
	}

	for (const auto& resource : resources.storage_buffers)
	{
		ShaderResourceBinding binding;
		binding.ResourceType = ShaderResourceType::StorageBuffer;
		binding.Access = compiler.get_buffer_block_flags(resource.id).get(spv::DecorationNonWritable)
							 ? ShaderResourceAccess::ReadOnly
							 : ShaderResourceAccess::ReadWrite;
		binding.StorageBufferView = StorageBufferViewType::Structured;
		binding.Stage = shaderStageType;
		binding.Slot = compiler.get_decoration(resource.id, spv::DecorationBinding);
		ResourceBindings.push_back(binding);
	}

	// NumThreads
	if (spirv->GetStage() == ShaderStageType::Compute)
	{
		auto entries = compiler.get_entry_points_and_stages();
		for (auto& e : entries)
		{
			const auto& spv_entry = compiler.get_entry_point(e.name, e.execution_model);

			spirv_cross::SpecializationConstant spec_x, spec_y, spec_z;
			compiler.get_work_group_size_specialization_constants(spec_x, spec_y, spec_z);

			if (e.name == "main")
			{
				NumThreads.X = spec_x.id != spirv_cross::ID(0) ? spec_x.constant_id : spv_entry.workgroup_size.x;
				NumThreads.Y = spec_y.id != spirv_cross::ID(0) ? spec_y.constant_id : spv_entry.workgroup_size.y;
				NumThreads.Z = spec_z.id != spirv_cross::ID(0) ? spec_z.constant_id : spv_entry.workgroup_size.z;
			}
		}
	}

	return true;
}

SPIRVGenerator::SPIRVGenerator(const std::function<std::vector<std::uint8_t>(std::string)>& onLoad) : onLoad_(onLoad)
{
	glslang::InitializeProcess();
}

SPIRVGenerator::~SPIRVGenerator() { glslang::FinalizeProcess(); }

std::shared_ptr<SPIRV> SPIRVGenerator::Generate(const char* path,
												const char* code,
												std::vector<std::string> includeDirs,
												std::vector<SPIRVGeneratorMacro> macros,
												ShaderStageType shaderStageType,
												bool isYInverted,
												bool addBindingOffset)
{
	std::string codeStr(code);
	glslang::TProgram program;
	auto shaderStage = GetGlslangShaderStage(shaderStageType);

	glslang::TShader shader = glslang::TShader(shaderStage);
	shader.setEnvInput(glslang::EShSourceHlsl, shaderStage, glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
	shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

	if (isYInverted)
	{
		shader.setInvertY(true);
	}

	const char* shaderStrings[1];
	shaderStrings[0] = codeStr.c_str();
	shader.setEntryPoint("main");

	std::string macro;

	for (auto& m : macros)
	{
		macro += "#define " + m.Name + " " + m.Content + "\n";
	}

	shader.setPreamble(macro.c_str());

	if (addBindingOffset)
	{
		shader.setShiftBinding(glslang::TResourceType::EResSampler, 0);
		shader.setShiftBinding(glslang::TResourceType::EResTexture, 100);
		shader.setShiftBinding(glslang::TResourceType::EResImage, 200);
		shader.setShiftBinding(glslang::TResourceType::EResUbo, 300);
		shader.setShiftBinding(glslang::TResourceType::EResSsbo, 400);
		shader.setShiftBinding(glslang::TResourceType::EResUav, 500);
	}

	shader.setStrings(shaderStrings, 1);
	const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShOptFull | EShMsgHlslOffsets);

	DirStackFileIncluder includer(onLoad_);
	includer.pushExternalLocalDirectory(dirnameOf(path));

	for (auto& d : includeDirs)
	{
		includer.pushExternalLocalDirectory(d);
	}

	int defaultVersion = 110;
	if (!shader.parse(GetDefaultResources(), defaultVersion, false, messages, includer))
	{
		return std::make_shared<SPIRV>(shader.getInfoLog());
	}

	program.addShader(&shader);

	if (!program.link(messages))
	{
		return std::make_shared<SPIRV>(program.getInfoLog());
	}

	if (addBindingOffset)
	{
		if (!program.mapIO())
		{
			return std::make_shared<SPIRV>(program.getInfoLog());
		}
	}

	std::vector<unsigned int> spirv;
	glslang::SpvOptions spvOptions;
	spvOptions.optimizeSize = true;
	spvOptions.disableOptimizer = false;

	glslang::GlslangToSpv(*program.getIntermediate(shaderStage), spirv, &spvOptions);

	return std::make_shared<SPIRV>(spirv, shaderStageType);
}

} // namespace LLGI
