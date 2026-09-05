
#include <ShaderTranspilerCore.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

enum class OutputType
{
	GLSL,
	VULKAN_GLSL,
	MSL,
	HLSL,
	WGSL,
	SPV,
	Max,
};

int main(int argc, char* argv[])
{
	std::vector<std::string> args;

	for (int i = 1; i < argc; i++)
	{
		args.emplace_back(argv[i]);
	}

	LLGI::ShaderStageType shaderStage = LLGI::ShaderStageType::Max;
	OutputType outputType = OutputType::Max;
	std::string code;
	std::string inputPath;
	std::string outputPath;
	std::string compiledOutputPath;
	bool isES = false;
	bool isDX12 = false;
	bool plain = false;
	bool fixWGSLMatrixDirection = false;
	bool isVulkanSPV = false;
	int shaderModel = 0;
	std::vector<std::string> includeDir;
	std::vector<LLGI::SPIRVGeneratorMacro> macros;

	for (size_t i = 0; i < args.size();)
	{
		if (args[i] == "--vert")
		{
			shaderStage = LLGI::ShaderStageType::Vertex;
			i += 1;
		}
		else if (args[i] == "--frag")
		{
			shaderStage = LLGI::ShaderStageType::Pixel;
			i += 1;
		}
		else if (args[i] == "--comp")
		{
			shaderStage = LLGI::ShaderStageType::Compute;
			i += 1;
		}
		else if (args[i] == "-G")
		{
			outputType = OutputType::GLSL;
			i += 1;
		}
		else if (args[i] == "-M")
		{
			outputType = OutputType::MSL;
			i += 1;
		}
		else if (args[i] == "-H")
		{
			outputType = OutputType::HLSL;
			i += 1;
		}
		else if (args[i] == "-V")
		{
			outputType = OutputType::VULKAN_GLSL;
			i += 1;
		}
		else if (args[i] == "-W")
		{
			outputType = OutputType::WGSL;
			i += 1;
		}
		else if (args[i] == "-S")
		{
			outputType = OutputType::SPV;
			i += 1;
		}
		else if (args[i] == "-I")
		{
			includeDir.push_back(args[i + 1]);
			i += 2;
		}
		else if (args[i] == "-D")
		{
			macros.push_back(LLGI::SPIRVGeneratorMacro(args[i + 1].c_str(), args[i + 2].c_str()));
			i += 3;
		}
		else if (args[i] == "--sm")
		{
			shaderModel = atoi(args[i + 1].c_str());
			i += 2;
		}
		else if (args[i] == "--es")
		{
			isES = true;
			i += 1;
		}
		else if (args[i] == "--plain")
		{
			plain = true;
			i += 1;
		}
		else if (args[i] == "--dx12")
		{
			isDX12 = true;
			i += 1;
		}
		else if (args[i] == "--fix-wgsl-matrix-direction")
		{
			fixWGSLMatrixDirection = true;
			i += 1;
		}
		else if (args[i] == "--vulkan-spv")
		{
			isVulkanSPV = true;
			i += 1;
		}
		else if (args[i] == "--input")
		{
			if (i == args.size() - 1)
			{
				std::cout << "Invald input : arg is none" << std::endl;
				return 0;
			}

			std::ifstream ifs(args[i + 1]);
			if (ifs.fail())
			{
				std::cout << "Invald input : unknown file " << args[i + 1] << std::endl;
				return 0;
			}
			code = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
			inputPath = args[i + 1];
			i += 2;
		}
		else if (args[i] == "--output")
		{
			if (i == args.size() - 1)
			{
				std::cout << "Invald output : arg is none" << std::endl;
				return 0;
			}

			outputPath = args[i + 1];

			i += 2;
		}
		else if (args[i] == "--compiled-output")
		{
			if (i == args.size() - 1)
			{
				std::cout << "Invald compiled output : arg is none" << std::endl;
				return 0;
			}

			compiledOutputPath = args[i + 1];

			i += 2;
		}
		else
		{
			i++;
		}
	}

	if (outputType == OutputType::Max)
	{
		std::cout << "Unknown type" << std::endl;
		return 0;
	}

	if (shaderStage == LLGI::ShaderStageType::Max)
	{
		std::cout << "Unknown ShaderStage" << std::endl;
		return 0;
	}

	if (outputPath == "")
	{
		std::cout << "Invalid output type" << std::endl;
		return 0;
	}

	auto loadFunc = [](std::string s) -> std::vector<uint8_t>
	{
		std::ifstream file(s, std::ios_base::binary | std::ios_base::ate);
		if (file)
		{
			std::vector<uint8_t> ret;
			auto size = (int)file.tellg();
			ret.resize(size);
			file.seekg(0, file.beg);
			file.read((char*)ret.data(), size);
			return ret;
		}
		return std::vector<uint8_t>();
	};

	auto generator = std::make_shared<LLGI::SPIRVGenerator>(loadFunc);

	const bool isVulkanTarget = outputType == OutputType::VULKAN_GLSL || (outputType == OutputType::SPV && isVulkanSPV);
	auto spirv = generator->Generate(inputPath.c_str(), code.c_str(), includeDir, macros, shaderStage, isVulkanTarget, outputType == OutputType::WGSL);

	if (spirv->GetData().size() == 0)
	{
		std::cout << spirv->GetError() << std::endl;
		return 0;
	}

	std::shared_ptr<LLGI::SPIRVTranspiler> transpiler = nullptr;

	if (outputType == OutputType::GLSL)
	{
		transpiler = std::make_shared<LLGI::SPIRVToGLSLTranspiler>(false, shaderModel != 0 ? shaderModel : 430, isES, plain);
	}
	else if (outputType == OutputType::VULKAN_GLSL)
	{
		transpiler = std::make_shared<LLGI::SPIRVToGLSLTranspiler>(true);
	}
	else if (outputType == OutputType::MSL)
	{
		transpiler = std::make_shared<LLGI::SPIRVToMSLTranspiler>();
	}
	else if (outputType == OutputType::HLSL)
	{
		transpiler = std::make_shared<LLGI::SPIRVToHLSLTranspiler>(shaderModel != 0 ? shaderModel : 40, isDX12);
	}
	else if (outputType == OutputType::WGSL)
	{
		transpiler = std::make_shared<LLGI::SPIRVToWGSLTranspiler>(fixWGSLMatrixDirection);
	}

	std::cout << inputPath << " -> " << outputPath << " ShaderModel=" << shaderModel << std::endl;

	try
	{
		if (transpiler != nullptr)
		{
			if (!transpiler->Transpile(spirv, shaderStage))
			{
				std::cout << transpiler->GetErrorCode() << std::endl;
				return 1;
			}
		}
		else if (outputType == OutputType::SPV)
		{
			if (isVulkanSPV)
			{
				spirv->RemapResourceBindingsForVulkan();
			}

			std::ofstream ofs;
			ofs.open(outputPath, std::ios::trunc | std::ios::binary);
			if (!ofs)
			{
				return 1;
			}

			ofs.write(reinterpret_cast<const char*>(spirv->GetData().data()), spirv->GetData().size() * sizeof(uint32_t));
			ofs.flush();
			ofs.close();
			return 0;
		}
	}
	catch (const std::runtime_error& e)
	{
		std::cout << "Error : " << e.what() << std::endl;
		return 0;
	}

	std::ofstream outputfile(outputPath);
	if (outputfile.bad())
	{
		std::cout << "Invald output" << std::endl;
		return 0;
	}

	if (transpiler->GetCode() == "")
	{
		std::cout << "No code is generated." << std::endl;
		return 1;
	}

	outputfile << transpiler->GetCode();

	if (outputType == OutputType::WGSL && compiledOutputPath != "")
	{
		static const char header[] = {'w', 'g', 's', 'l', 'c', 'o', 'd', 'e'};
		const auto transpiledCode = transpiler->GetCode();

		std::ofstream compiledOutput(compiledOutputPath, std::ios::binary);
		if (compiledOutput.bad())
		{
			std::cout << "Invald compiled output" << std::endl;
			return 0;
		}

		compiledOutput.write(header, sizeof(header));
		compiledOutput.write(transpiledCode.data(), static_cast<std::streamsize>(transpiledCode.size()));
		const char terminator = 0;
		compiledOutput.write(&terminator, 1);
	}

	return 0;
}
