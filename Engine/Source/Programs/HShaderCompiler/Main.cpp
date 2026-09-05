#include "Runtime/Graphics/Material/MaterialShaderManifest.h"
#include "Runtime/Graphics/RHI/ShaderArtifact.h"
#include "Runtime/Platform/PlatformFileSystem.h"

#include <nlohmann/json.hpp>
#include <spirv-tools/libspirv.hpp>

#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <future>
#include <sstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <unordered_set>
#include <system_error>
#include <utility>
#include <cstring>
#include <charconv>
#include <cmath>
#include <regex>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Utils
{
    static std::vector<std::string> Split(const std::string& str, char separator = ' ')
    {
        std::vector<std::string> result;
        std::stringstream ss(str);
        std::string token;
        while (std::getline(ss, token, separator))
        {
            result.push_back(token);
        }
        return result;
    }
}

// exe path: HEngine\Engine\Binaries\Win64\Programs\HShaderCompiler
// shader root path: HEngine\Engine\Content\Shaders
std::filesystem::path RootPath = "../../../../Content/Shaders";
std::filesystem::path OutputPath;
std::vector<std::filesystem::path> AdditionalIncludePaths;
std::vector<HEngine::ShaderArtifactTarget> ArtifactTargets = {
    { HEngine::RenderBackend::DX12, HEngine::ShaderBinaryFormat::Dxil }
};

bool IsHShader(const std::filesystem::path& file)
{
    //return file.extension() == ".hshader";
    std::string extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == ".hlsl";
}

#ifdef _WIN32
static bool RunTool(
    const std::filesystem::path& toolPath,
    const std::string& args,
    std::string& diagnostics,
    bool bEchoDiagnostics = true)
{
    SECURITY_ATTRIBUTES pipeAttributes = {};
    pipeAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeAttributes.bInheritHandle = TRUE;

    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&outputRead, &outputWrite, &pipeAttributes, 0))
    {
        std::cout << "Failed to create compiler output pipe, error=" << GetLastError() << std::endl;
        return false;
    }
    if (!SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0))
    {
        std::cout << "Failed to configure compiler output pipe, error=" << GetLastError() << std::endl;
        CloseHandle(outputRead);
        CloseHandle(outputWrite);
        return false;
    }

    STARTUPINFOA startupInfo = {};
    startupInfo.cb = sizeof(STARTUPINFOA);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = outputWrite;
    startupInfo.hStdError = outputWrite;
    PROCESS_INFORMATION processInfo = {};

    std::string commandLine = "\"" + toolPath.string() + "\" " + args;
    std::vector<char> commandLineBuffer(commandLine.begin(), commandLine.end());
    commandLineBuffer.push_back('\0');
    auto workingDir = toolPath.parent_path().string();

    if (!CreateProcessA(toolPath.string().c_str(), commandLineBuffer.data(), nullptr, nullptr, TRUE, 0, nullptr, workingDir.c_str(), &startupInfo, &processInfo))
    {
        std::cout << "Failed to launch tool: " << toolPath << ", error=" << GetLastError() << std::endl;
        std::cout << "Cmd: " << commandLine << std::endl;
        CloseHandle(outputRead);
        CloseHandle(outputWrite);
        return false;
    }

    CloseHandle(outputWrite);
    outputWrite = nullptr;

    std::array<char, 4096> outputBuffer{};
    DWORD bytesRead = 0;
    while (ReadFile(outputRead, outputBuffer.data(), static_cast<DWORD>(outputBuffer.size()), &bytesRead, nullptr) && bytesRead > 0)
        diagnostics.append(outputBuffer.data(), bytesRead);
    CloseHandle(outputRead);

    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode = 0;
    const BOOL gotExitCode = waitResult == WAIT_OBJECT_0
        ? GetExitCodeProcess(processInfo.hProcess, &exitCode)
        : FALSE;

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (!gotExitCode || exitCode != 0)
    {
        if (!diagnostics.empty())
            std::cout << diagnostics;
        if (!gotExitCode)
            std::cout << "Failed to wait for tool process, result=" << waitResult << std::endl;
        else
            std::cout << "Tool exited with code " << exitCode << std::endl;
        std::cout << "Cmd: " << commandLine << std::endl;
        return false;
    }

    if (bEchoDiagnostics && !diagnostics.empty())
        std::cout << diagnostics;

    return true;
}
#endif

static std::filesystem::path ResolveInputPath(
    const std::filesystem::path& rawPath,
    const std::filesystem::path& launchWorkingDirectory)
{
    if (rawPath.empty())
        return rawPath;

    std::error_code ec;
    const std::filesystem::path absolutePath = rawPath.is_absolute()
        ? rawPath
        : (launchWorkingDirectory / rawPath);
    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(absolutePath, ec);
    if (!ec)
        return canonicalPath;

    return absolutePath.lexically_normal();
}

std::string ReadFile(const std::filesystem::path& filepath)
{
    std::string result;
    std::ifstream in(filepath, std::ios::in | std::ios::binary);
    if (in)
    {
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0)
        {
            std::string errorMes = "Could not read file size " + filepath.string();
            throw std::runtime_error(errorMes);
        }

        result.resize(static_cast<size_t>(size));
        in.seekg(0, std::ios::beg);
        if (!result.empty())
            in.read(result.data(), static_cast<std::streamsize>(result.size()));

        if (in.fail())
        {
            std::string errorMes = "Could not read file content " + filepath.string();
            throw std::runtime_error(errorMes);
        }

        in.close();
    }
    else
    {
        std::string errorMes = "Could not open file " + filepath.string();
        throw std::runtime_error(errorMes);
    }

    return result;
}

struct ShaderCompileDesc
{
    std::string type;
    std::string entry;
};

struct ShaderProfileRule
{
    std::string stage;
    std::string profile;
    std::string variant;
};

struct ShaderParseResult
{
    std::vector<ShaderCompileDesc> stages;
    std::vector<std::string> variantDefines;
    std::vector<ShaderProfileRule> profileRules;
    std::optional<HEngine::RenderBackend> backendOnly;
    std::string meta;
};

struct CompiledShaderArtifact
{
    std::string variant;
    std::string stage;
    std::string entry;
    std::string profile;
    std::string artifactStem;
    HEngine::MaterialShaderPass materialPass = HEngine::MaterialShaderPass::Forward;
    HEngine::MaterialShaderDescriptorMode descriptorMode =
        HEngine::MaterialShaderDescriptorMode::Baseline;
    HEngine::MaterialShaderVertexPath vertexPath =
        HEngine::MaterialShaderVertexPath::Static;
    bool bMaterialPermutation = false;
    std::optional<HEngine::RenderBackend> backendOnly;
};

struct MaterialPragmaParseResult
{
    bool bIsMaterialShader = false;
    HEngine::MaterialShaderManifest manifest;
    std::vector<HEngine::MaterialShaderDiagnostic> diagnostics;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic)
        {
            return diagnostic.Severity == HEngine::MaterialShaderDiagnosticSeverity::Error;
        });
    }
};

struct PublishedMaterialArtifact
{
    CompiledShaderArtifact compile;
    HEngine::ShaderArtifactTarget target;
    std::string cacheKey;
    std::filesystem::path stagedArtifactPath;
    std::filesystem::path stagedCacheKeyPath;
    std::filesystem::path finalArtifactPath;
    std::filesystem::path finalCacheKeyPath;
    std::string relativeArtifactPath;
    std::string contentHash;
    bool bNeedsPublish = false;
};

struct CommandLineOptions
{
    std::string Path = ".";
    std::string RootPath;
    std::string OutputPath;
    std::string DiagnosticsOutputPath;
    std::string Shader;
    std::string BinaryFormat = "all";
    std::vector<std::string> IncludePaths;
    bool bSelfTest = false;
};

static void PrintUsage()
{
    std::cout << "HShaderCompiler usage:\n"
              << "  -p, --path <shader-folder>\n"
              << "  -r, --rootPath <shader-root-folder>\n"
              << "  -o, --outputPath <generated-shader-folder>\n"
              << "  --diagnosticsOutput <compiler-log-file>\n"
              << "  -s, --shader <single-shader-file>\n"
              << "  -I, --includePath <additional-include-folder> (repeatable)\n"
              << "  --binary-format <dxil|spirv|all> (default: all)\n"
              << "  --self-test (validate profile/requirements parser contracts)\n"
              << "Shader declarations:\n"
              << "  #pragma htype <vs|ps|cs> <entry>\n"
              << "  #pragma htype lib [entry]\n"
              << "  #pragma hvariant <DEFINE>\n"
              << "  #pragma hprofile <stage> <profile> [BASE|<variant>]\n"
              << "  #pragma hbackend <DX12|Vulkan>\n"
              << "Surface material declarations (engine-owned entry points/permutations):\n"
              << "  #include \"Modern/MaterialSurface.hlsli\"\n"
              << "  #pragma hmaterial Surface <Lit|Unlit>\n"
              << "  #pragma hblend <Opaque|Mask|Transparent>\n"
              << "  #pragma property <g_Name> <display> <type> default(...) [attributes]\n"
              << "  void EvaluateMaterial(in HEMaterialInput, inout HEMaterialSurface)\n"
              << "Defaults are vs_6_0, ps_6_0, cs_6_0 and lib_6_3. An unconditional\n"
              << "hprofile overrides a stage; a BASE/variant rule takes precedence.\n";
}

static CommandLineOptions ParseCommandLine(int argc, char* argv[])
{
    CommandLineOptions options;

    auto readValue = [&](int& index, const std::string& optionName) -> std::string
    {
        if (index + 1 >= argc)
            throw std::runtime_error("Missing value for option: " + optionName);

        ++index;
        return std::string(argv[index]);
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            PrintUsage();
            std::exit(0);
        }
        else if (arg == "-p" || arg == "--path")
        {
            options.Path = readValue(i, arg);
        }
        else if (arg == "-r" || arg == "--rootPath")
        {
            options.RootPath = readValue(i, arg);
        }
        else if (arg == "-o" || arg == "--outputPath")
        {
            options.OutputPath = readValue(i, arg);
        }
        else if (arg == "-s" || arg == "--shader")
        {
            options.Shader = readValue(i, arg);
        }
        else if (arg == "--diagnosticsOutput")
        {
            options.DiagnosticsOutputPath = readValue(i, arg);
        }
        else if (arg == "-I" || arg == "--includePath")
        {
            options.IncludePaths.push_back(readValue(i, arg));
        }
        else if (arg == "--binary-format" || arg == "--binaryFormat")
        {
            options.BinaryFormat = readValue(i, arg);
        }
        else if (arg == "--self-test")
        {
            options.bSelfTest = true;
        }
        else if (arg.rfind("--path=", 0) == 0)
        {
            options.Path = arg.substr(strlen("--path="));
        }
        else if (arg.rfind("--rootPath=", 0) == 0)
        {
            options.RootPath = arg.substr(strlen("--rootPath="));
        }
        else if (arg.rfind("--outputPath=", 0) == 0)
        {
            options.OutputPath = arg.substr(strlen("--outputPath="));
        }
        else if (arg.rfind("--shader=", 0) == 0)
        {
            options.Shader = arg.substr(strlen("--shader="));
        }
        else if (arg.rfind("--diagnosticsOutput=", 0) == 0)
        {
            options.DiagnosticsOutputPath = arg.substr(strlen("--diagnosticsOutput="));
        }
        else if (arg.rfind("--includePath=", 0) == 0)
        {
            options.IncludePaths.push_back(arg.substr(strlen("--includePath=")));
        }
        else if (arg.rfind("--binary-format=", 0) == 0)
        {
            options.BinaryFormat = arg.substr(strlen("--binary-format="));
        }
        else if (arg.rfind("--binaryFormat=", 0) == 0)
        {
            options.BinaryFormat = arg.substr(strlen("--binaryFormat="));
        }
        else
        {
            throw std::runtime_error("Unknown command line option: " + arg);
        }
    }

    return options;
}

class ScopedCompilerOutputRedirect final
{
public:
    void Open(const std::filesystem::path& outputPath)
    {
        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error)
        {
            throw std::runtime_error(
                "Could not create compiler diagnostics directory: " + error.message());
        }
        mStream.open(outputPath, std::ios::binary | std::ios::trunc);
        if (!mStream)
            throw std::runtime_error("Could not open compiler diagnostics output file.");
        mPrevious = std::cout.rdbuf(mStream.rdbuf());
    }

    ~ScopedCompilerOutputRedirect()
    {
        if (mPrevious)
            std::cout.rdbuf(mPrevious);
    }

private:
    std::ofstream mStream;
    std::streambuf* mPrevious = nullptr;
};

static std::string TrimString(const std::string& value)
{
    auto beginIt = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    if (beginIt == value.end())
        return "";

    auto endIt = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();

    return std::string(beginIt, endIt);
}

static std::vector<HEngine::ShaderArtifactTarget> ParseShaderArtifactTargets(
    std::string selection)
{
    std::transform(selection.begin(), selection.end(), selection.begin(), [](unsigned char ch)
    {
        return static_cast<char>(std::tolower(ch));
    });
    if (selection == "dxil")
    {
        return { { HEngine::RenderBackend::DX12, HEngine::ShaderBinaryFormat::Dxil } };
    }
    if (selection == "spirv" || selection == "spir-v")
    {
        return { { HEngine::RenderBackend::Vulkan, HEngine::ShaderBinaryFormat::SpirV } };
    }
    if (selection == "all")
    {
        return {
            { HEngine::RenderBackend::DX12, HEngine::ShaderBinaryFormat::Dxil },
            { HEngine::RenderBackend::Vulkan, HEngine::ShaderBinaryFormat::SpirV },
        };
    }
    throw std::runtime_error(
        "Invalid --binary-format value '" + selection + "'. Expected dxil, spirv, or all.");
}

static const char* ShaderBackendName(HEngine::RenderBackend backend) noexcept
{
    switch (backend)
    {
    case HEngine::RenderBackend::DX12: return "DX12";
    case HEngine::RenderBackend::Vulkan: return "Vulkan";
    }
    return "Unknown";
}

static std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

static std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
    if (!ec)
        return canonicalPath;

    return path.lexically_normal();
}

static bool ReadTextFile(const std::filesystem::path& filePath, std::string& outText)
{
    std::ifstream in(filePath, std::ios::in | std::ios::binary);
    if (!in.is_open())
        return false;

    std::stringstream stream;
    stream << in.rdbuf();
    outText = stream.str();
    return true;
}

enum class IncludeParseResult
{
    NotInclude,
    Literal,
    Unsupported
};

static IncludeParseResult ParseIncludePath(const std::string& line, std::string& includePath)
{
    const std::string trimmedLine = TrimString(line);
    if (trimmedLine.empty() || trimmedLine.front() != '#')
        return IncludeParseResult::NotInclude;

    size_t directiveStart = 1;
    while (directiveStart < trimmedLine.size() &&
        std::isspace(static_cast<unsigned char>(trimmedLine[directiveStart])) != 0)
    {
        ++directiveStart;
    }
    constexpr std::string_view includeDirective = "include";
    if (trimmedLine.compare(directiveStart, includeDirective.size(), includeDirective) != 0)
        return IncludeParseResult::NotInclude;
    const size_t directiveEnd = directiveStart + includeDirective.size();
    if (directiveEnd < trimmedLine.size() &&
        std::isspace(static_cast<unsigned char>(trimmedLine[directiveEnd])) == 0 &&
        trimmedLine[directiveEnd] != '"' && trimmedLine[directiveEnd] != '<')
    {
        return IncludeParseResult::Unsupported;
    }

    size_t firstDelimiter = directiveEnd;
    while (firstDelimiter < trimmedLine.size() &&
        std::isspace(static_cast<unsigned char>(trimmedLine[firstDelimiter])) != 0)
    {
        ++firstDelimiter;
    }
    if (firstDelimiter == trimmedLine.size() ||
        (trimmedLine[firstDelimiter] != '"' && trimmedLine[firstDelimiter] != '<'))
    {
        return IncludeParseResult::Unsupported;
    }

    const char closeDelimiter = trimmedLine[firstDelimiter] == '<' ? '>' : '"';
    const size_t secondDelimiter = trimmedLine.find(closeDelimiter, firstDelimiter + 1);
    if (secondDelimiter == std::string::npos)
        return IncludeParseResult::Unsupported;

    includePath = TrimString(trimmedLine.substr(firstDelimiter + 1, secondDelimiter - firstDelimiter - 1));
    return includePath.empty()
        ? IncludeParseResult::Unsupported
        : IncludeParseResult::Literal;
}

static bool IsPathWithinIncludeDomain(const std::filesystem::path& candidatePath)
{
    const std::filesystem::path normalizedCandidate = NormalizePath(candidatePath);
    const auto isWithin = [&normalizedCandidate](const std::filesystem::path& rawRoot)
    {
        std::error_code ec;
        const std::filesystem::path relativePath = std::filesystem::relative(
            normalizedCandidate, NormalizePath(rawRoot), ec);
        if (ec || relativePath.empty() || relativePath.is_absolute() || relativePath.has_root_name())
            return false;
        for (const std::filesystem::path& component : relativePath)
        {
            if (component == "..")
                return false;
        }
        return true;
    };

    if (isWithin(RootPath))
        return true;
    return std::any_of(
        AdditionalIncludePaths.begin(),
        AdditionalIncludePaths.end(),
        isWithin);
}

static std::filesystem::path ResolveIncludePath(const std::filesystem::path& sourcePath,
    const std::string& includePath)
{
    std::error_code ec;
    if (includePath.find('\0') != std::string::npos)
        throw std::runtime_error("Shader include contains an embedded NUL byte.");
    const std::filesystem::path includeAsPath(includePath);

    if (includeAsPath.is_absolute() || includeAsPath.has_root_name())
    {
        throw std::runtime_error(
            "Absolute shader include paths are forbidden: " + includePath);
    }
    for (const std::filesystem::path& component : includeAsPath)
    {
        if (component == "..")
        {
            throw std::runtime_error(
                "Parent traversal is forbidden in shader include paths: " + includePath);
        }
    }

    const auto resolveCandidate = [&](const std::filesystem::path& basePath)
        -> std::filesystem::path
    {
        ec.clear();
        const std::filesystem::path candidatePath = NormalizePath(basePath / includeAsPath);
        if (!std::filesystem::is_regular_file(candidatePath, ec) || ec)
            return {};
        if (!IsPathWithinIncludeDomain(candidatePath))
        {
            throw std::runtime_error(
                "Shader include escapes the declared root/include domains: " + includePath +
                " resolved to " + candidatePath.generic_string());
        }
        return candidatePath;
    };

    if (const std::filesystem::path localPath = resolveCandidate(sourcePath.parent_path());
        !localPath.empty())
    {
        return localPath;
    }

    if (const std::filesystem::path rootPath = resolveCandidate(RootPath);
        !rootPath.empty())
    {
        return rootPath;
    }

    for (const std::filesystem::path& includeRoot : AdditionalIncludePaths)
    {
        if (const std::filesystem::path additionalPath = resolveCandidate(includeRoot);
            !additionalPath.empty())
        {
            return additionalPath;
        }
    }

    return {};
}

static void CollectShaderDependenciesRecursive(const std::filesystem::path& sourcePath,
    std::unordered_set<std::string>& visitedFiles,
    std::vector<std::filesystem::path>& dependencies)
{
    const std::filesystem::path normalizedSourcePath = NormalizePath(sourcePath);
    const std::string sourceKey = ToLowerCopy(normalizedSourcePath.generic_string());
    if (!visitedFiles.insert(sourceKey).second)
        return;

    dependencies.push_back(normalizedSourcePath);

    std::error_code ec;
    if (!std::filesystem::exists(normalizedSourcePath, ec))
        return;

    std::string sourceText;
    if (!ReadTextFile(normalizedSourcePath, sourceText))
        return;

    std::stringstream sourceStream(sourceText);
    std::string line;
    while (std::getline(sourceStream, line))
    {
        std::string includePath;
        const IncludeParseResult includeResult = ParseIncludePath(line, includePath);
        if (includeResult == IncludeParseResult::NotInclude)
            continue;
        if (includeResult == IncludeParseResult::Unsupported)
        {
            throw std::runtime_error(
                "Shader include directives must use a single-line literal path: " +
                normalizedSourcePath.generic_string());
        }

        const std::filesystem::path resolvedIncludePath = ResolveIncludePath(normalizedSourcePath, includePath);
        if (resolvedIncludePath.empty())
            continue;

        CollectShaderDependenciesRecursive(resolvedIncludePath, visitedFiles, dependencies);
    }
}

static std::vector<std::filesystem::path> CollectShaderDependencies(const std::filesystem::path& shaderPath)
{
    std::vector<std::filesystem::path> dependencies;
    std::unordered_set<std::string> visitedFiles;
    CollectShaderDependenciesRecursive(shaderPath, visitedFiles, dependencies);
    return dependencies;
}

class Sha256
{
public:
    void Update(std::span<const std::uint8_t> bytes)
    {
        if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - mTotalBytes)
            throw std::overflow_error("SHA-256 input is too large");
        mTotalBytes += static_cast<std::uint64_t>(bytes.size());

        std::size_t offset = 0;
        if (mBufferedBytes != 0)
        {
            const std::size_t copied = std::min(bytes.size(), mBuffer.size() - mBufferedBytes);
            std::copy_n(bytes.data(), copied, mBuffer.data() + mBufferedBytes);
            mBufferedBytes += copied;
            offset += copied;
            if (mBufferedBytes == mBuffer.size())
            {
                Transform(mBuffer.data());
                mBufferedBytes = 0;
            }
        }

        while (bytes.size() - offset >= mBuffer.size())
        {
            Transform(bytes.data() + offset);
            offset += mBuffer.size();
        }
        if (offset != bytes.size())
        {
            mBufferedBytes = bytes.size() - offset;
            std::copy_n(bytes.data() + offset, mBufferedBytes, mBuffer.data());
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> Finalize()
    {
        if (mTotalBytes > std::numeric_limits<std::uint64_t>::max() / 8)
            throw std::overflow_error("SHA-256 input is too large");
        const std::uint64_t bitLength = mTotalBytes * 8;

        mBuffer[mBufferedBytes++] = 0x80;
        if (mBufferedBytes > 56)
        {
            std::fill(mBuffer.begin() + mBufferedBytes, mBuffer.end(), 0);
            Transform(mBuffer.data());
            mBufferedBytes = 0;
        }
        std::fill(mBuffer.begin() + mBufferedBytes, mBuffer.begin() + 56, 0);
        for (std::size_t index = 0; index < 8; ++index)
        {
            mBuffer[56 + index] = static_cast<std::uint8_t>(
                (bitLength >> ((7 - index) * 8)) & 0xffu);
        }
        Transform(mBuffer.data());

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t word = 0; word < mState.size(); ++word)
        {
            for (std::size_t byte = 0; byte < 4; ++byte)
            {
                digest[word * 4 + byte] = static_cast<std::uint8_t>(
                    (mState[word] >> ((3 - byte) * 8)) & 0xffu);
            }
        }
        return digest;
    }

private:
    static constexpr std::array<std::uint32_t, 64> RoundConstants = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    static std::uint32_t RotateRight(std::uint32_t value, std::uint32_t count) noexcept
    {
        return (value >> count) | (value << (32 - count));
    }

    void Transform(const std::uint8_t* block) noexcept
    {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index)
        {
            words[index] =
                (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                static_cast<std::uint32_t>(block[index * 4 + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index)
        {
            const std::uint32_t sigma0 = RotateRight(words[index - 15], 7) ^
                RotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
            const std::uint32_t sigma1 = RotateRight(words[index - 2], 17) ^
                RotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
        }

        std::uint32_t a = mState[0];
        std::uint32_t b = mState[1];
        std::uint32_t c = mState[2];
        std::uint32_t d = mState[3];
        std::uint32_t e = mState[4];
        std::uint32_t f = mState[5];
        std::uint32_t g = mState[6];
        std::uint32_t h = mState[7];
        for (std::size_t index = 0; index < words.size(); ++index)
        {
            const std::uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + sum1 + choose + RoundConstants[index] + words[index];
            const std::uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        mState[0] += a;
        mState[1] += b;
        mState[2] += c;
        mState[3] += d;
        mState[4] += e;
        mState[5] += f;
        mState[6] += g;
        mState[7] += h;
    }

    std::array<std::uint32_t, 8> mState = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    std::array<std::uint8_t, 64> mBuffer{};
    std::uint64_t mTotalBytes = 0;
    std::size_t mBufferedBytes = 0;
};

static void HashUnsigned64(Sha256& hash, std::uint64_t value)
{
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>((value >> ((7 - index) * 8)) & 0xffu);
    hash.Update(bytes);
}

static void HashString(Sha256& hash, std::string_view value)
{
    HashUnsigned64(hash, static_cast<std::uint64_t>(value.size()));
    hash.Update(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

static std::string ToHex(std::span<const std::uint8_t> bytes)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const std::uint8_t byte : bytes)
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    return stream.str();
}

static std::string DependencyIdentity(const std::filesystem::path& dependencyPath)
{
    const std::filesystem::path normalizedDependency = NormalizePath(dependencyPath);
    const auto relativeWithin = [&normalizedDependency](const std::filesystem::path& rawRoot)
        -> std::optional<std::filesystem::path>
    {
        std::error_code ec;
        const std::filesystem::path relativePath = std::filesystem::relative(
            normalizedDependency, NormalizePath(rawRoot), ec);
        if (ec || relativePath.empty() || relativePath.is_absolute() || relativePath.has_root_name())
            return std::nullopt;
        for (const auto& component : relativePath)
        {
            if (component == "..")
                return std::nullopt;
        }
        return relativePath;
    };

    if (const auto relativePath = relativeWithin(RootPath))
        return "@root/" + relativePath->generic_string();
    for (std::size_t index = 0; index < AdditionalIncludePaths.size(); ++index)
    {
        if (const auto relativePath = relativeWithin(AdditionalIncludePaths[index]))
            return "@include" + std::to_string(index) + "/" + relativePath->generic_string();
    }
    throw std::runtime_error(
        "Shader dependency is outside the declared root/include domains: " +
        normalizedDependency.generic_string());
}

static std::string NormalizeDefine(std::string_view define)
{
    return define.find('=') == std::string_view::npos
        ? std::string(define) + "=1"
        : std::string(define);
}

static bool IsSupportedShaderStage(std::string_view stage)
{
    return stage == "vs" || stage == "ps" || stage == "cs" || stage == "lib";
}

static std::string DefaultTargetProfile(std::string_view stage)
{
    if (stage == "lib")
        return "lib_6_3";
    if (stage == "vs" || stage == "ps" || stage == "cs")
        return std::string(stage) + "_6_0";
    return {};
}

static bool TryParseTargetProfile(
    std::string_view profile,
    std::string& outStage,
    unsigned int& outMajor,
    unsigned int& outMinor)
{
    const size_t firstSeparator = profile.find('_');
    const size_t secondSeparator = firstSeparator == std::string_view::npos
        ? std::string_view::npos
        : profile.find('_', firstSeparator + 1);
    if (firstSeparator == std::string_view::npos ||
        secondSeparator == std::string_view::npos ||
        profile.find('_', secondSeparator + 1) != std::string_view::npos)
    {
        return false;
    }

    const std::string_view stage = profile.substr(0, firstSeparator);
    const std::string_view major = profile.substr(
        firstSeparator + 1, secondSeparator - firstSeparator - 1);
    const std::string_view minor = profile.substr(secondSeparator + 1);
    if (stage.empty() || major.empty() || minor.empty() ||
        !std::all_of(major.begin(), major.end(), [](unsigned char character)
        {
            return std::isdigit(character) != 0;
        }) ||
        !std::all_of(minor.begin(), minor.end(), [](unsigned char character)
        {
            return std::isdigit(character) != 0;
        }))
    {
        return false;
    }

    const auto parseNumber = [](std::string_view digits, unsigned int& value)
    {
        value = 0;
        for (const char digit : digits)
        {
            const unsigned int nextDigit = static_cast<unsigned int>(digit - '0');
            if (value > (std::numeric_limits<unsigned int>::max() - nextDigit) / 10u)
                return false;
            value = value * 10u + nextDigit;
        }
        return true;
    };

    outStage = std::string(stage);
    return parseNumber(major, outMajor) && parseNumber(minor, outMinor);
}

static std::string ResolveTargetProfile(
    const ShaderParseResult& parseResult,
    std::string_view stage,
    std::string_view variant)
{
    std::string profile = DefaultTargetProfile(stage);
    for (const ShaderProfileRule& rule : parseResult.profileRules)
    {
        if (rule.stage == stage && rule.variant.empty())
            profile = rule.profile;
    }
    for (const ShaderProfileRule& rule : parseResult.profileRules)
    {
        if (rule.stage == stage && rule.variant == variant)
            profile = rule.profile;
    }
    return profile;
}

static std::string BuildArtifactStem(
    std::string_view shaderName,
    std::string_view variant,
    std::string_view stage,
    std::string_view profile)
{
    std::string artifactStem(shaderName);
    if (variant != "BASE")
    {
        artifactStem.push_back('_');
        artifactStem += variant;
    }
    if (profile != DefaultTargetProfile(stage))
    {
        artifactStem += "__";
        artifactStem += profile;
    }
    return artifactStem;
}

static std::vector<CompiledShaderArtifact> BuildShaderArtifacts(
    const std::string& shaderName,
    const ShaderParseResult& parseResult)
{
    std::vector<std::string> variants{ "BASE" };
    variants.insert(
        variants.end(), parseResult.variantDefines.begin(), parseResult.variantDefines.end());

    std::vector<CompiledShaderArtifact> artifacts;
    artifacts.reserve(variants.size() * parseResult.stages.size());
    for (const std::string& variant : variants)
    {
        for (const ShaderCompileDesc& stage : parseResult.stages)
        {
            const std::string profile = ResolveTargetProfile(parseResult, stage.type, variant);
            artifacts.push_back({
                .variant = variant,
                .stage = stage.type,
                .entry = stage.entry,
                .profile = profile,
                .artifactStem = BuildArtifactStem(
                    shaderName, variant, stage.type, profile),
                .backendOnly = parseResult.backendOnly,
            });
        }
    }
    return artifacts;
}

static std::vector<CompiledShaderArtifact> BuildMaterialShaderArtifacts(
    const std::string& shaderName,
    const HEngine::MaterialShaderManifest& manifest)
{
    struct Permutation
    {
        HEngine::MaterialShaderPass Pass;
        HEngine::MaterialShaderVertexPath VertexPath;
        HEngine::MaterialShaderDescriptorMode DescriptorMode;
        const char* Name;
    };
    constexpr std::array ForwardPermutations = {
        Permutation{ HEngine::MaterialShaderPass::Forward, HEngine::MaterialShaderVertexPath::Static,
            HEngine::MaterialShaderDescriptorMode::Baseline, "FORWARD_CPU_STATIC_BASELINE" },
        Permutation{ HEngine::MaterialShaderPass::Forward, HEngine::MaterialShaderVertexPath::Static,
            HEngine::MaterialShaderDescriptorMode::HeapDirect, "FORWARD_CPU_STATIC_HEAP_DIRECT" },
        Permutation{ HEngine::MaterialShaderPass::Forward, HEngine::MaterialShaderVertexPath::Skinned,
            HEngine::MaterialShaderDescriptorMode::Baseline, "FORWARD_SKINNED_BASELINE" },
        Permutation{ HEngine::MaterialShaderPass::Forward, HEngine::MaterialShaderVertexPath::Skinned,
            HEngine::MaterialShaderDescriptorMode::HeapDirect, "FORWARD_SKINNED_HEAP_DIRECT" },
    };
    constexpr std::array MaskPermutations = {
        Permutation{ HEngine::MaterialShaderPass::Depth, HEngine::MaterialShaderVertexPath::Static,
            HEngine::MaterialShaderDescriptorMode::Baseline, "DEPTH_CPU_STATIC_BASELINE" },
        Permutation{ HEngine::MaterialShaderPass::Depth, HEngine::MaterialShaderVertexPath::Static,
            HEngine::MaterialShaderDescriptorMode::HeapDirect, "DEPTH_CPU_STATIC_HEAP_DIRECT" },
        Permutation{ HEngine::MaterialShaderPass::Depth, HEngine::MaterialShaderVertexPath::Skinned,
            HEngine::MaterialShaderDescriptorMode::Baseline, "DEPTH_SKINNED_BASELINE" },
        Permutation{ HEngine::MaterialShaderPass::Depth, HEngine::MaterialShaderVertexPath::Skinned,
            HEngine::MaterialShaderDescriptorMode::HeapDirect, "DEPTH_SKINNED_HEAP_DIRECT" },
        Permutation{ HEngine::MaterialShaderPass::Shadow, HEngine::MaterialShaderVertexPath::Static,
            HEngine::MaterialShaderDescriptorMode::Baseline, "SHADOW_CPU_STATIC_BASELINE" },
        Permutation{ HEngine::MaterialShaderPass::Shadow, HEngine::MaterialShaderVertexPath::Static,
            HEngine::MaterialShaderDescriptorMode::HeapDirect, "SHADOW_CPU_STATIC_HEAP_DIRECT" },
        Permutation{ HEngine::MaterialShaderPass::Shadow, HEngine::MaterialShaderVertexPath::Skinned,
            HEngine::MaterialShaderDescriptorMode::Baseline, "SHADOW_SKINNED_BASELINE" },
        Permutation{ HEngine::MaterialShaderPass::Shadow, HEngine::MaterialShaderVertexPath::Skinned,
            HEngine::MaterialShaderDescriptorMode::HeapDirect, "SHADOW_SKINNED_HEAP_DIRECT" },
        Permutation{ HEngine::MaterialShaderPass::Outline, HEngine::MaterialShaderVertexPath::Static,
            HEngine::MaterialShaderDescriptorMode::Baseline, "OUTLINE_CPU_STATIC_BASELINE" },
        Permutation{ HEngine::MaterialShaderPass::Outline, HEngine::MaterialShaderVertexPath::Static,
            HEngine::MaterialShaderDescriptorMode::HeapDirect, "OUTLINE_CPU_STATIC_HEAP_DIRECT" },
        Permutation{ HEngine::MaterialShaderPass::Outline, HEngine::MaterialShaderVertexPath::Skinned,
            HEngine::MaterialShaderDescriptorMode::Baseline, "OUTLINE_SKINNED_BASELINE" },
        Permutation{ HEngine::MaterialShaderPass::Outline, HEngine::MaterialShaderVertexPath::Skinned,
            HEngine::MaterialShaderDescriptorMode::HeapDirect, "OUTLINE_SKINNED_HEAP_DIRECT" },
    };

    std::vector<Permutation> permutations(ForwardPermutations.begin(), ForwardPermutations.end());
    if (manifest.BlendMode == HEngine::MaterialShaderBlendMode::Mask)
        permutations.insert(permutations.end(), MaskPermutations.begin(), MaskPermutations.end());

    std::vector<CompiledShaderArtifact> artifacts;
    artifacts.reserve(permutations.size() * 2);
    for (const Permutation& permutation : permutations)
    {
        for (const std::string_view stage : { std::string_view("vs"), std::string_view("ps") })
        {
            const std::string profile = stage == "vs"
                ? "vs_6_0"
                : permutation.DescriptorMode == HEngine::MaterialShaderDescriptorMode::HeapDirect
                    ? "ps_6_6"
                    : "ps_6_0";
            const std::string entry = stage == "vs" ? "HEMaterialVS" : "HEMaterialPS";
            artifacts.push_back({
                .variant = permutation.Name,
                .stage = std::string(stage),
                .entry = entry,
                .profile = profile,
                .artifactStem = shaderName + "__" + permutation.Name + "__" + profile,
                .materialPass = permutation.Pass,
                .descriptorMode = permutation.DescriptorMode,
                .vertexPath = permutation.VertexPath,
                .bMaterialPermutation = true,
            });
        }
    }
    return artifacts;
}

static std::vector<std::string> BuildShaderCompileOptions(
    const HEngine::ShaderArtifactTarget& target)
{
    std::vector<std::string> options = {
        "-nologo",
        "-Zpc",
        "-HV",
        "2021",
        "-O3",
        "-I",
        "<shader-root>"
    };
    if (target.BinaryFormat != HEngine::ShaderBinaryFormat::SpirV)
        return options;

    options.insert(options.end(), {
        "-spirv",
        "-fspv-target-env=vulkan1.3",
        "-fvk-use-dx-layout",
        "-fvk-stage-io-order=decl",
        "-D",
        "HE_SPIRV=1",
    });
    for (unsigned int descriptorSet = 0; descriptorSet <= 4; ++descriptorSet)
    {
        const std::string set = std::to_string(descriptorSet);
        options.insert(options.end(), {
            "-fvk-t-shift", "0", set,
            "-fvk-s-shift", "128", set,
            "-fvk-b-shift", "256", set,
            "-fvk-u-shift", "384", set,
        });
    }
    return options;
}

static std::string BuildDxcBackendArguments(
    const HEngine::ShaderArtifactTarget& target)
{
    if (target.BinaryFormat != HEngine::ShaderBinaryFormat::SpirV)
        return {};

    std::string arguments =
        " -spirv -fspv-target-env=vulkan1.3 -fvk-use-dx-layout"
        " -fvk-stage-io-order=decl -D HE_SPIRV=1";
    for (unsigned int descriptorSet = 0; descriptorSet <= 4; ++descriptorSet)
    {
        const std::string set = std::to_string(descriptorSet);
        arguments += " -fvk-t-shift 0 " + set +
            " -fvk-s-shift 128 " + set +
            " -fvk-b-shift 256 " + set +
            " -fvk-u-shift 384 " + set;
    }
    return arguments;
}

static std::string BuildShaderCacheKey(
    const std::vector<std::filesystem::path>& dependencies,
    const std::vector<std::string>& defines,
    const std::string& entry,
    const std::string& profile,
    const std::vector<std::string>& compileOptions,
    const HEngine::ShaderArtifactTarget& target)
{
    struct DependencyContent
    {
        std::string identity;
        std::string content;
    };

    std::vector<DependencyContent> dependencyContents;
    dependencyContents.reserve(dependencies.size());
    for (const auto& dependencyPath : dependencies)
        dependencyContents.push_back({ DependencyIdentity(dependencyPath), ReadFile(dependencyPath) });
    std::sort(dependencyContents.begin(), dependencyContents.end(), [](const auto& a, const auto& b) {
        return a.identity < b.identity;
    });

    Sha256 hash;
    HashString(hash, "HEngine.HShaderCompiler.DXC.Artifact.v3");
    HashString(hash, ShaderBackendName(target.Backend));
    HashString(hash, HEngine::ToString(target.BinaryFormat));
    HashUnsigned64(hash, static_cast<std::uint64_t>(dependencyContents.size()));
    for (const auto& dependency : dependencyContents)
    {
        HashString(hash, dependency.identity);
        HashString(hash, dependency.content);
    }
    HashUnsigned64(hash, static_cast<std::uint64_t>(defines.size()));
    for (const auto& define : defines)
        HashString(hash, NormalizeDefine(define));
    HashString(hash, entry);
    HashString(hash, profile);
    HashUnsigned64(hash, static_cast<std::uint64_t>(compileOptions.size()));
    for (const auto& option : compileOptions)
        HashString(hash, option);

    const std::array<std::uint8_t, 32> digest = hash.Finalize();
    return ToHex(digest);
}

static std::string BuildIdentityHash(std::string_view domain, std::string_view value)
{
    Sha256 hash;
    HashString(hash, domain);
    HashString(hash, value);
    return ToHex(hash.Finalize());
}

static std::string BuildRecursiveSourceHash(
    const std::vector<std::filesystem::path>& dependencies)
{
    struct DependencyContent
    {
        std::string identity;
        std::string content;
    };

    std::vector<DependencyContent> contents;
    contents.reserve(dependencies.size());
    for (const std::filesystem::path& dependency : dependencies)
        contents.push_back({ DependencyIdentity(dependency), ReadFile(dependency) });
    std::sort(contents.begin(), contents.end(), [](const auto& left, const auto& right)
    {
        return left.identity < right.identity;
    });

    Sha256 hash;
    HashString(hash, "HEngine.MaterialShader.RecursiveSource.v1");
    HashUnsigned64(hash, static_cast<std::uint64_t>(contents.size()));
    for (const DependencyContent& content : contents)
    {
        HashString(hash, content.identity);
        HashString(hash, content.content);
    }
    return ToHex(hash.Finalize());
}

static std::string BuildFileContentHash(const std::filesystem::path& path)
{
    std::string hash;
    std::string error;
    if (!HEngine::ComputeMaterialShaderFileSha256(path, hash, error))
    {
        throw std::runtime_error(
            "Failed to hash material shader artifact '" + path.generic_string() +
            "': " + error);
    }
    return hash;
}

static std::optional<std::string> NormalizeSourceRelativePath(
    const std::filesystem::path& shaderPath)
{
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(
        NormalizePath(shaderPath), NormalizePath(RootPath), ec);
    if (ec || relative.empty() || relative.is_absolute() || relative.has_root_name())
        return std::nullopt;
    for (const std::filesystem::path& component : relative)
    {
        if (component == "..")
            return std::nullopt;
    }
    const std::string normalized = relative.lexically_normal().generic_string();
    if (normalized.empty() || normalized.find('\0') != std::string::npos ||
        normalized.find('\\') != std::string::npos)
    {
        return std::nullopt;
    }
    return normalized;
}

static HEngine::MaterialShaderDiagnostic MakeMaterialDiagnostic(
    std::string code,
    std::string message,
    std::string source,
    std::uint32_t line = 0,
    std::uint32_t column = 0,
    HEngine::MaterialShaderDiagnosticSeverity severity =
        HEngine::MaterialShaderDiagnosticSeverity::Error)
{
    return {
        .Severity = severity,
        .Code = std::move(code),
        .Message = std::move(message),
        .Source = std::move(source),
        .Line = line,
        .Column = column,
    };
}

static void EmitMaterialDiagnostics(
    const std::vector<HEngine::MaterialShaderDiagnostic>& diagnostics)
{
    if (diagnostics.empty())
        return;
    nlohmann::json output;
    output["MaterialShaderDiagnostics"] = HEngine::MaterialShaderDiagnosticsToJson(diagnostics);
    std::cout << output.dump(
        -1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
}

static std::string StripLineCommentPreservingQuotes(std::string_view line)
{
    bool bDoubleQuoted = false;
    bool bSingleQuoted = false;
    bool bEscaped = false;
    for (std::size_t index = 0; index + 1 < line.size(); ++index)
    {
        const char character = line[index];
        if (bEscaped)
        {
            bEscaped = false;
            continue;
        }
        if (character == '\\' && (bDoubleQuoted || bSingleQuoted))
        {
            bEscaped = true;
            continue;
        }
        if (character == '"' && !bSingleQuoted)
            bDoubleQuoted = !bDoubleQuoted;
        else if (character == '\'' && !bDoubleQuoted)
            bSingleQuoted = !bSingleQuoted;
        else if (!bDoubleQuoted && !bSingleQuoted && character == '/' && line[index + 1] == '/')
            return std::string(line.substr(0, index));
    }
    return std::string(line);
}

class MaterialPragmaCursor final
{
public:
    explicit MaterialPragmaCursor(std::string_view text)
        : mText(text)
    {
    }

    void SkipWhitespace() noexcept
    {
        while (mOffset < mText.size() &&
            std::isspace(static_cast<unsigned char>(mText[mOffset])) != 0)
        {
            ++mOffset;
        }
    }

    [[nodiscard]] bool AtEnd() noexcept
    {
        SkipWhitespace();
        return mOffset == mText.size();
    }

    [[nodiscard]] std::uint32_t Column() const noexcept
    {
        return static_cast<std::uint32_t>(mOffset + 1);
    }

    [[nodiscard]] std::string Remaining()
    {
        SkipWhitespace();
        return std::string(mText.substr(mOffset));
    }

    bool ParseIdentifier(std::string& output)
    {
        SkipWhitespace();
        if (mOffset >= mText.size() ||
            (!std::isalpha(static_cast<unsigned char>(mText[mOffset])) && mText[mOffset] != '_'))
        {
            return false;
        }
        const std::size_t begin = mOffset++;
        while (mOffset < mText.size() &&
            (std::isalnum(static_cast<unsigned char>(mText[mOffset])) != 0 || mText[mOffset] == '_'))
        {
            ++mOffset;
        }
        output = std::string(mText.substr(begin, mOffset - begin));
        return true;
    }

    bool ParseQuotedOrIdentifier(std::string& output)
    {
        SkipWhitespace();
        if (mOffset >= mText.size())
            return false;
        if (mText[mOffset] != '"' && mText[mOffset] != '\'')
            return ParseIdentifier(output);

        const char quote = mText[mOffset++];
        output.clear();
        while (mOffset < mText.size())
        {
            const char character = mText[mOffset++];
            if (character == quote)
                return true;
            if (character == '\\' && mOffset < mText.size())
                output.push_back(mText[mOffset++]);
            else
                output.push_back(character);
        }
        return false;
    }

    bool ParseCall(std::string& name, std::string& payload)
    {
        if (!ParseIdentifier(name))
            return false;
        SkipWhitespace();
        if (mOffset >= mText.size() || mText[mOffset] != '(')
            return false;
        ++mOffset;
        const std::size_t begin = mOffset;
        bool bDoubleQuoted = false;
        bool bSingleQuoted = false;
        bool bEscaped = false;
        while (mOffset < mText.size())
        {
            const char character = mText[mOffset];
            if (bEscaped)
            {
                bEscaped = false;
                ++mOffset;
                continue;
            }
            if (character == '\\' && (bDoubleQuoted || bSingleQuoted))
            {
                bEscaped = true;
                ++mOffset;
                continue;
            }
            if (character == '"' && !bSingleQuoted)
                bDoubleQuoted = !bDoubleQuoted;
            else if (character == '\'' && !bDoubleQuoted)
                bSingleQuoted = !bSingleQuoted;
            else if (character == ')' && !bDoubleQuoted && !bSingleQuoted)
            {
                payload = TrimString(std::string(mText.substr(begin, mOffset - begin)));
                ++mOffset;
                return true;
            }
            ++mOffset;
        }
        return false;
    }

private:
    std::string_view mText;
    std::size_t mOffset = 0;
};

static std::string UnquoteMaterialValue(std::string value)
{
    value = TrimString(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\'')))
    {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

static std::vector<std::string> SplitMaterialArguments(std::string_view payload)
{
    std::vector<std::string> result;
    std::size_t begin = 0;
    bool bDoubleQuoted = false;
    bool bSingleQuoted = false;
    for (std::size_t index = 0; index <= payload.size(); ++index)
    {
        const char character = index < payload.size() ? payload[index] : ',';
        if (character == '"' && !bSingleQuoted)
            bDoubleQuoted = !bDoubleQuoted;
        else if (character == '\'' && !bDoubleQuoted)
            bSingleQuoted = !bSingleQuoted;
        else if (character == ',' && !bDoubleQuoted && !bSingleQuoted)
        {
            result.push_back(TrimString(std::string(payload.substr(begin, index - begin))));
            begin = index + 1;
        }
    }
    return result;
}

static bool TryParseFiniteDouble(std::string_view text, double& output) noexcept
{
    const std::string trimmed = TrimString(std::string(text));
    if (trimmed.empty())
        return false;
    const char* begin = trimmed.data();
    const char* end = begin + trimmed.size();
    const std::from_chars_result result = std::from_chars(begin, end, output);
    return result.ec == std::errc{} && result.ptr == end && std::isfinite(output);
}

static bool TryParseInt64(std::string_view text, std::int64_t& output) noexcept
{
    const std::string trimmed = TrimString(std::string(text));
    if (trimmed.empty())
        return false;
    const char* begin = trimmed.data();
    const char* end = begin + trimmed.size();
    const std::from_chars_result result = std::from_chars(begin, end, output);
    return result.ec == std::errc{} && result.ptr == end;
}

static std::optional<HEngine::MaterialShaderPropertyType> ParseMaterialPropertyType(
    std::string value)
{
    value = ToLowerCopy(std::move(value));
    if (value == "float") return HEngine::MaterialShaderPropertyType::Float;
    if (value == "float2" || value == "vector2") return HEngine::MaterialShaderPropertyType::Float2;
    if (value == "float3" || value == "vector3") return HEngine::MaterialShaderPropertyType::Float3;
    if (value == "color" || value == "float4") return HEngine::MaterialShaderPropertyType::Color;
    if (value == "bool") return HEngine::MaterialShaderPropertyType::Bool;
    if (value == "int") return HEngine::MaterialShaderPropertyType::Int;
    if (value == "texture" || value == "texture2d") return HEngine::MaterialShaderPropertyType::Texture2D;
    return std::nullopt;
}

static bool ParseMaterialBool(std::string value, bool& output)
{
    value = ToLowerCopy(TrimString(value));
    if (value == "true" || value == "on" || value == "1")
    {
        output = true;
        return true;
    }
    if (value == "false" || value == "off" || value == "0")
    {
        output = false;
        return true;
    }
    return false;
}

static std::optional<HEngine::MaterialShaderTextureSemantic> ParseTextureSemantic(
    std::string value)
{
    value = ToLowerCopy(UnquoteMaterialValue(std::move(value)));
    if (value == "none") return HEngine::MaterialShaderTextureSemantic::None;
    if (value == "basecolor" || value == "albedo") return HEngine::MaterialShaderTextureSemantic::BaseColor;
    if (value == "normal") return HEngine::MaterialShaderTextureSemantic::Normal;
    if (value == "metallicroughness") return HEngine::MaterialShaderTextureSemantic::MetallicRoughness;
    if (value == "occlusion" || value == "ao") return HEngine::MaterialShaderTextureSemantic::Occlusion;
    if (value == "emissive") return HEngine::MaterialShaderTextureSemantic::Emissive;
    if (value == "data") return HEngine::MaterialShaderTextureSemantic::Data;
    return std::nullopt;
}

static std::optional<HEngine::MaterialShaderColorSpace> ParseMaterialColorSpace(
    std::string value)
{
    value = ToLowerCopy(UnquoteMaterialValue(std::move(value)));
    if (value == "linear") return HEngine::MaterialShaderColorSpace::Linear;
    if (value == "srgb") return HEngine::MaterialShaderColorSpace::Srgb;
    return std::nullopt;
}

static bool SetMaterialPropertyDefault(
    HEngine::MaterialShaderProperty& property,
    std::string_view payload,
    HEngine::MaterialShaderDiagnostic& diagnostic)
{
    const std::vector<std::string> values = SplitMaterialArguments(payload);
    const std::size_t expectedNumericCount =
        property.Type == HEngine::MaterialShaderPropertyType::Float ? 1u :
        property.Type == HEngine::MaterialShaderPropertyType::Float2 ? 2u :
        property.Type == HEngine::MaterialShaderPropertyType::Float3 ? 3u :
        property.Type == HEngine::MaterialShaderPropertyType::Color ? 4u : 0u;
    if (expectedNumericCount != 0)
    {
        if (values.size() != expectedNumericCount)
        {
            diagnostic.Code = "Material.PropertyDefaultArity";
            diagnostic.Message = "Property '" + property.Name + "' requires " +
                std::to_string(expectedNumericCount) + " default components.";
            return false;
        }
        property.Default.Numeric.clear();
        for (const std::string& value : values)
        {
            double number = 0.0;
            if (!TryParseFiniteDouble(value, number))
            {
                diagnostic.Code = "Material.PropertyDefaultNumber";
                diagnostic.Message = "Property '" + property.Name + "' has a non-finite or invalid numeric default.";
                return false;
            }
            property.Default.Numeric.push_back(number);
        }
        return true;
    }

    if (values.size() != 1 || values.front().empty())
    {
        diagnostic.Code = "Material.PropertyDefaultArity";
        diagnostic.Message = "Property '" + property.Name + "' requires exactly one default value.";
        return false;
    }
    if (property.Type == HEngine::MaterialShaderPropertyType::Bool)
    {
        if (!ParseMaterialBool(values.front(), property.Default.Boolean))
        {
            diagnostic.Code = "Material.PropertyDefaultBool";
            diagnostic.Message = "Property '" + property.Name + "' bool default must be true, false, 0, or 1.";
            return false;
        }
        return true;
    }
    if (property.Type == HEngine::MaterialShaderPropertyType::Int)
    {
        if (!TryParseInt64(values.front(), property.Default.Integer))
        {
            diagnostic.Code = "Material.PropertyDefaultInt";
            diagnostic.Message = "Property '" + property.Name + "' has an invalid integer default.";
            return false;
        }
        return true;
    }
    property.Default.Texture = UnquoteMaterialValue(values.front());
    if (property.Default.Texture.empty())
    {
        diagnostic.Code = "Material.PropertyDefaultTexture";
        diagnostic.Message = "Property '" + property.Name + "' texture default cannot be empty.";
        return false;
    }
    std::string textureDefaultError;
    if (!HEngine::IsValidMaterialShaderTextureDefault(
            property.Default.Texture, &textureDefaultError))
    {
        diagnostic.Code = "Material.PropertyDefaultTexturePath";
        diagnostic.Message = "Property '" + property.Name +
            "' has an unsafe texture default: " + textureDefaultError;
        return false;
    }
    return true;
}

static bool ParseMaterialPropertyLine(
    std::string_view body,
    const std::string& source,
    std::uint32_t line,
    HEngine::MaterialShaderProperty& property,
    std::vector<HEngine::MaterialShaderDiagnostic>& diagnostics)
{
    MaterialPragmaCursor cursor(body);
    std::string type;
    if (!cursor.ParseIdentifier(property.Name) ||
        !cursor.ParseQuotedOrIdentifier(property.DisplayName) ||
        !cursor.ParseIdentifier(type))
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.PropertySyntax",
            "#pragma property requires '<name> <display-name> <type> attributes'.",
            source, line, cursor.Column()));
        return false;
    }
    if (!property.Name.starts_with("g_") || property.Name.starts_with("g_HE"))
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.PropertyName",
            "Material property names must start with 'g_' and cannot use the reserved 'g_HE' prefix.",
            source, line, 1));
        return false;
    }

    const std::optional<HEngine::MaterialShaderPropertyType> parsedType =
        ParseMaterialPropertyType(type);
    if (!parsedType)
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.PropertyType",
            "Property '" + property.Name + "' uses unsupported type '" + type + "'.",
            source, line, cursor.Column()));
        return false;
    }
    property.Type = *parsedType;
    property.StableId = property.Name;
    property.ByteSize = HEngine::MaterialShaderPropertyByteSize(property.Type);
    bool bHasDefault = false;
    bool bHasRange = false;
    bool bHasGroup = false;
    bool bHasId = false;
    bool bHasSemantic = false;
    bool bHasColorSpace = false;
    bool bHasFallback = false;

    while (!cursor.AtEnd())
    {
        const std::uint32_t attributeColumn = cursor.Column();
        std::string name;
        std::string payload;
        if (!cursor.ParseCall(name, payload))
        {
            diagnostics.push_back(MakeMaterialDiagnostic(
                "Material.PropertyAttributeSyntax",
                "Property '" + property.Name + "' has a malformed attribute.",
                source, line, attributeColumn));
            return false;
        }
        name = ToLowerCopy(std::move(name));
        if (name == "default")
        {
            if (bHasDefault)
            {
                diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.PropertyDuplicateAttribute", "Property default is declared more than once.",
                    source, line, attributeColumn));
                continue;
            }
            bHasDefault = true;
            HEngine::MaterialShaderDiagnostic diagnostic = MakeMaterialDiagnostic(
                {}, {}, source, line, attributeColumn);
            if (!SetMaterialPropertyDefault(property, payload, diagnostic))
                diagnostics.push_back(std::move(diagnostic));
        }
        else if (name == "range")
        {
            if (bHasRange)
            {
                diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.PropertyDuplicateAttribute", "Property range is declared more than once.",
                    source, line, attributeColumn));
                continue;
            }
            bHasRange = true;
            const std::vector<std::string> values = SplitMaterialArguments(payload);
            if ((property.Type != HEngine::MaterialShaderPropertyType::Float &&
                 property.Type != HEngine::MaterialShaderPropertyType::Int) ||
                values.size() != 2 ||
                !TryParseFiniteDouble(values[0], property.Range.Minimum) ||
                !TryParseFiniteDouble(values[1], property.Range.Maximum) ||
                property.Range.Minimum > property.Range.Maximum)
            {
                diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.PropertyRange",
                    "Property '" + property.Name + "' range requires two finite values with Min <= Max and is only valid for float/int.",
                    source, line, attributeColumn));
            }
            else
            {
                property.Range.bEnabled = true;
            }
        }
        else if (name == "group")
        {
            if (bHasGroup)
                diagnostics.push_back(MakeMaterialDiagnostic("Material.PropertyDuplicateAttribute", "Property group is declared more than once.", source, line, attributeColumn));
            bHasGroup = true;
            property.Group = UnquoteMaterialValue(payload);
        }
        else if (name == "id")
        {
            if (bHasId)
                diagnostics.push_back(MakeMaterialDiagnostic("Material.PropertyDuplicateAttribute", "Property id is declared more than once.", source, line, attributeColumn));
            bHasId = true;
            property.StableId = UnquoteMaterialValue(payload);
            if (property.StableId.empty())
                diagnostics.push_back(MakeMaterialDiagnostic("Material.PropertyId", "Property stable id cannot be empty.", source, line, attributeColumn));
        }
        else if (name == "semantic")
        {
            if (bHasSemantic)
                diagnostics.push_back(MakeMaterialDiagnostic("Material.PropertyDuplicateAttribute", "Property semantic is declared more than once.", source, line, attributeColumn));
            bHasSemantic = true;
            const auto semantic = ParseTextureSemantic(payload);
            if (!semantic || property.Type != HEngine::MaterialShaderPropertyType::Texture2D)
                diagnostics.push_back(MakeMaterialDiagnostic("Material.PropertySemantic", "semantic(...) is only valid for Texture2D and must use a supported semantic.", source, line, attributeColumn));
            else
                property.TextureSemantic = *semantic;
        }
        else if (name == "colorspace")
        {
            if (bHasColorSpace)
                diagnostics.push_back(MakeMaterialDiagnostic("Material.PropertyDuplicateAttribute", "Property colorspace is declared more than once.", source, line, attributeColumn));
            bHasColorSpace = true;
            const auto colorSpace = ParseMaterialColorSpace(payload);
            if (!colorSpace || property.Type != HEngine::MaterialShaderPropertyType::Texture2D)
                diagnostics.push_back(MakeMaterialDiagnostic("Material.PropertyColorSpace", "colorspace(...) is only valid for Texture2D and must be Linear or SRGB.", source, line, attributeColumn));
            else
                property.ColorSpace = *colorSpace;
        }
        else if (name == "fallback")
        {
            if (bHasFallback)
                diagnostics.push_back(MakeMaterialDiagnostic("Material.PropertyDuplicateAttribute", "Property fallback is declared more than once.", source, line, attributeColumn));
            bHasFallback = true;
            property.Fallback = UnquoteMaterialValue(payload);
            if (property.Type != HEngine::MaterialShaderPropertyType::Texture2D || property.Fallback.empty())
                diagnostics.push_back(MakeMaterialDiagnostic("Material.PropertyFallback", "fallback(...) requires a non-empty Texture2D fallback token.", source, line, attributeColumn));
        }
        else
        {
            diagnostics.push_back(MakeMaterialDiagnostic(
                "Material.PropertyAttribute",
                "Property '" + property.Name + "' uses unsupported attribute '" + name + "'.",
                source, line, attributeColumn));
        }
    }

    if (!bHasDefault)
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.PropertyMissingDefault",
            "Property '" + property.Name + "' must declare default(...).",
            source, line, 1));
    }
    if (property.Type == HEngine::MaterialShaderPropertyType::Texture2D)
    {
        if (property.Fallback.empty())
            property.Fallback = property.Default.Texture;
        if (!bHasColorSpace &&
            (property.TextureSemantic == HEngine::MaterialShaderTextureSemantic::BaseColor ||
             property.TextureSemantic == HEngine::MaterialShaderTextureSemantic::Emissive))
        {
            property.ColorSpace = HEngine::MaterialShaderColorSpace::Srgb;
        }
    }
    return true;
}

struct ParsedPragmaLine
{
    std::string Directive;
    std::string Body;
    std::uint32_t Line = 0;
    std::uint32_t Column = 0;
};

static std::optional<ParsedPragmaLine> ParsePragmaLine(
    std::string_view rawLine,
    std::uint32_t lineNumber)
{
    const std::string withoutComment = StripLineCommentPreservingQuotes(rawLine);
    const std::size_t hash = withoutComment.find_first_not_of(" \t\r");
    if (hash == std::string::npos || withoutComment[hash] != '#')
        return std::nullopt;

    MaterialPragmaCursor cursor(std::string_view(withoutComment).substr(hash + 1));
    std::string pragma;
    std::string directive;
    if (!cursor.ParseIdentifier(pragma) || ToLowerCopy(pragma) != "pragma" ||
        !cursor.ParseIdentifier(directive))
    {
        return std::nullopt;
    }
    return ParsedPragmaLine{
        .Directive = ToLowerCopy(std::move(directive)),
        .Body = cursor.Remaining(),
        .Line = lineNumber,
        .Column = static_cast<std::uint32_t>(hash + 1),
    };
}

static std::vector<std::string> SplitWhitespaceTokens(const std::string& text)
{
    std::istringstream stream(text);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token)
        tokens.push_back(std::move(token));
    return tokens;
}

static std::string NormalizedPathKey(const std::filesystem::path& path)
{
    std::string key = NormalizePath(path).generic_string();
#if defined(_WIN32)
    key = ToLowerCopy(std::move(key));
#endif
    return key;
}

static std::string SanitizeHlslForPolicyScan(std::string_view source)
{
    enum class State
    {
        Code,
        LineComment,
        BlockComment,
        StringLiteral,
        CharacterLiteral
    };

    State state = State::Code;
    bool bEscaped = false;
    std::string sanitized(source);
    for (std::size_t index = 0; index < source.size(); ++index)
    {
        const char current = source[index];
        const char next = index + 1 < source.size() ? source[index + 1] : '\0';
        const auto eraseCurrent = [&]()
        {
            if (current != '\n' && current != '\r')
                sanitized[index] = ' ';
        };

        switch (state)
        {
            case State::Code:
                if (current == '/' && next == '/')
                {
                    sanitized[index] = ' ';
                    sanitized[index + 1] = ' ';
                    ++index;
                    state = State::LineComment;
                }
                else if (current == '/' && next == '*')
                {
                    sanitized[index] = ' ';
                    sanitized[index + 1] = ' ';
                    ++index;
                    state = State::BlockComment;
                }
                else if (current == '"')
                {
                    sanitized[index] = ' ';
                    state = State::StringLiteral;
                    bEscaped = false;
                }
                else if (current == '\'')
                {
                    sanitized[index] = ' ';
                    state = State::CharacterLiteral;
                    bEscaped = false;
                }
                break;
            case State::LineComment:
                eraseCurrent();
                if (current == '\n')
                    state = State::Code;
                break;
            case State::BlockComment:
                eraseCurrent();
                if (current == '*' && next == '/')
                {
                    sanitized[index + 1] = ' ';
                    ++index;
                    state = State::Code;
                }
                break;
            case State::StringLiteral:
            case State::CharacterLiteral:
            {
                eraseCurrent();
                const char delimiter = state == State::StringLiteral ? '"' : '\'';
                if (!bEscaped && current == delimiter)
                    state = State::Code;
                bEscaped = !bEscaped && current == '\\';
                if (current != '\\')
                    bEscaped = false;
                break;
            }
        }
    }
    return sanitized;
}

static std::pair<std::uint32_t, std::uint32_t> SourceLocationFromOffset(
    std::string_view source,
    std::size_t offset)
{
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    for (std::size_t index = 0; index < std::min(offset, source.size()); ++index)
    {
        if (source[index] == '\n')
        {
            line = line == std::numeric_limits<std::uint32_t>::max() ? line : line + 1;
            column = 1;
        }
        else if (column != std::numeric_limits<std::uint32_t>::max())
        {
            ++column;
        }
    }
    return { line, column };
}

static void AppendPolicyPatternDiagnostics(
    const std::string& sanitizedSource,
    const std::regex& pattern,
    std::string_view code,
    std::string_view message,
    const std::string& sourceName,
    std::vector<HEngine::MaterialShaderDiagnostic>& diagnostics)
{
    for (std::sregex_iterator iterator(
             sanitizedSource.begin(), sanitizedSource.end(), pattern), end;
         iterator != end;
         ++iterator)
    {
        const auto [line, column] = SourceLocationFromOffset(
            sanitizedSource, static_cast<std::size_t>(iterator->position()));
        diagnostics.push_back(MakeMaterialDiagnostic(
            std::string(code), std::string(message), sourceName, line, column));
    }
}

static std::vector<std::filesystem::path> FindMaterialSurfaceContractCandidates()
{
    std::vector<std::filesystem::path> candidates;
    std::unordered_set<std::string> candidateKeys;
    const auto addCandidate = [&](const std::filesystem::path& root)
    {
        std::error_code ec;
        const std::filesystem::path candidate =
            NormalizePath(root / "Modern" / "MaterialSurface.hlsli");
        if (std::filesystem::is_regular_file(candidate, ec) && !ec &&
            candidateKeys.insert(NormalizedPathKey(candidate)).second)
        {
            candidates.push_back(candidate);
        }
    };

    addCandidate(RootPath);
    for (const std::filesystem::path& includeRoot : AdditionalIncludePaths)
        addCandidate(includeRoot);
    return candidates;
}

static void ValidateMaterialSourceContract(
    const std::filesystem::path& shaderPath,
    const std::vector<std::filesystem::path>& dependencies,
    std::vector<HEngine::MaterialShaderDiagnostic>& diagnostics)
{
    static const std::regex registerPattern(
        R"(\bregister\s*\()", std::regex_constants::icase);
    static const std::regex cbufferPattern(
        R"(\bcbuffer\s+[A-Za-z_][A-Za-z0-9_]*)", std::regex_constants::icase);
    static const std::regex samplerPattern(
        R"(\b(?:SamplerState|SamplerComparisonState|sampler(?:1D|2D|3D|CUBE|state)?)\s+[A-Za-z_][A-Za-z0-9_]*\s*(?:\[[^\]\r\n]*\]\s*)?(?=[:;=,]))",
        std::regex_constants::icase);
    static const std::regex resourcePattern(
        R"(\b(?:Texture(?:1D(?:Array)?|2D(?:Array|MS(?:Array)?)?|3D|Cube(?:Array)?)|Buffer|ByteAddressBuffer|StructuredBuffer|ConstantBuffer|RWTexture(?:1D(?:Array)?|2D(?:Array)?|3D)|RWBuffer|RWByteAddressBuffer|RWStructuredBuffer|AppendStructuredBuffer|ConsumeStructuredBuffer|RasterizerOrderedTexture(?:1D(?:Array)?|2D(?:Array)?|3D)|RasterizerOrderedBuffer|RasterizerOrderedByteAddressBuffer|RasterizerOrderedStructuredBuffer|RaytracingAccelerationStructure)\s*(?:<[^;{}\r\n]+>)?\s+[A-Za-z_][A-Za-z0-9_]*\s*(?:\[[^\]\r\n]*\]\s*)?(?=[:;=,]))",
        std::regex_constants::icase);
    static const std::regex entryPointPattern(
        R"(\bHEMaterial(?:VS|PS)\b)", std::regex_constants::icase);
    constexpr std::string_view canonicalSurfaceInclude =
        "Modern/MaterialSurface.hlsli";

    const std::string shaderSource = DependencyIdentity(shaderPath);
    const std::vector<std::filesystem::path> surfaceCandidates =
        FindMaterialSurfaceContractCandidates();
    if (surfaceCandidates.empty())
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.SurfaceContractMissing",
            "Cannot locate the engine-owned Modern/MaterialSurface.hlsli contract in the declared shader roots.",
            shaderSource));
        return;
    }
    if (surfaceCandidates.size() != 1)
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.SurfaceContractAmbiguous",
            "More than one Modern/MaterialSurface.hlsli contract is visible; project shaders cannot shadow the engine-owned Surface ABI.",
            shaderSource));
        return;
    }

    const std::filesystem::path& surfaceContractPath = surfaceCandidates.front();
    const std::string surfaceContractKey = NormalizedPathKey(surfaceContractPath);
    std::unordered_set<std::string> trustedDependencyKeys;
    for (const std::filesystem::path& trustedDependency :
         CollectShaderDependencies(surfaceContractPath))
    {
        trustedDependencyKeys.insert(NormalizedPathKey(trustedDependency));
    }

    std::size_t surfaceIncludeCount = 0;
    std::size_t trustedSurfaceIncludeCount = 0;
    for (const std::filesystem::path& dependency : dependencies)
    {
        if (trustedDependencyKeys.contains(NormalizedPathKey(dependency)))
            continue;

        const std::string sourceName = DependencyIdentity(dependency);
        const std::string sourceText = ReadFile(dependency);
        const std::string sanitizedSource = SanitizeHlslForPolicyScan(sourceText);
        AppendPolicyPatternDiagnostics(
            sanitizedSource, registerPattern, "Material.ForbiddenRegister",
            "Material definition sources cannot declare register(...); the Surface ABI owns all bindings.",
            sourceName, diagnostics);
        AppendPolicyPatternDiagnostics(
            sanitizedSource, cbufferPattern, "Material.ForbiddenCBuffer",
            "Material definition sources cannot declare cbuffer blocks; the Surface ABI owns all constant buffers.",
            sourceName, diagnostics);
        AppendPolicyPatternDiagnostics(
            sanitizedSource, samplerPattern, "Material.ForbiddenSampler",
            "Material definition sources cannot declare sampler resources; use HE_MATERIAL_DEFAULT_SAMPLER.",
            sourceName, diagnostics);
        AppendPolicyPatternDiagnostics(
            sanitizedSource, resourcePattern, "Material.ForbiddenResource",
            "Material definition sources cannot declare HLSL resources; material properties are supplied exclusively by the Surface ABI.",
            sourceName, diagnostics);
        AppendPolicyPatternDiagnostics(
            sanitizedSource, entryPointPattern, "Material.ReservedEntryPoint",
            "HEMaterialVS and HEMaterialPS are engine-owned entry points and cannot be declared or referenced by material definitions.",
            sourceName, diagnostics);

        std::istringstream stream(sourceText);
        std::string line;
        std::uint32_t lineNumber = 0;
        while (std::getline(stream, line))
        {
            ++lineNumber;
            std::string includePath;
            if (ParseIncludePath(line, includePath) != IncludeParseResult::Literal)
                continue;

            std::filesystem::path resolvedInclude;
            try
            {
                resolvedInclude = ResolveIncludePath(dependency, includePath);
            }
            catch (const std::exception& exception)
            {
                diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.IncludePolicy", exception.what(), sourceName, lineNumber));
                continue;
            }

            const bool bCanonicalSpelling = includePath == canonicalSurfaceInclude;
            const bool bResolvesToSurface = !resolvedInclude.empty() &&
                NormalizedPathKey(resolvedInclude) == surfaceContractKey;
            if (!bCanonicalSpelling && !bResolvesToSurface)
                continue;

            ++surfaceIncludeCount;
            if (!bCanonicalSpelling)
            {
                diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.SurfaceIncludeCanonical",
                    "The Surface contract must be included exactly as \"Modern/MaterialSurface.hlsli\".",
                    sourceName, lineNumber));
            }
            if (!bResolvesToSurface)
            {
                diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.SurfaceIncludeTarget",
                    "Modern/MaterialSurface.hlsli did not resolve to the unique engine-owned Surface contract.",
                    sourceName, lineNumber));
            }
            else
            {
                ++trustedSurfaceIncludeCount;
            }
        }
    }

    if (surfaceIncludeCount == 0)
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.SurfaceIncludeMissing",
            "Material definitions must include \"Modern/MaterialSurface.hlsli\" exactly once.",
            shaderSource));
    }
    else if (surfaceIncludeCount > 1)
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.SurfaceIncludeDuplicate",
            "Material definitions must include \"Modern/MaterialSurface.hlsli\" exactly once across the user dependency graph.",
            shaderSource));
    }
    if (trustedSurfaceIncludeCount != 1 && surfaceIncludeCount != 0)
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.SurfaceIncludeTrust",
            "Exactly one Surface include must resolve to the engine-owned Modern/MaterialSurface.hlsli contract.",
            shaderSource));
    }
}

static std::vector<std::string> BuildMaterialPermutationDefines(
    const HEngine::MaterialShaderManifest& manifest,
    const CompiledShaderArtifact& artifact)
{
    const auto booleanDefine = [](std::string_view name, bool bEnabled)
    {
        return std::string(name) + (bEnabled ? "=1" : "=0");
    };
    std::vector<std::string> defines = {
        "HE_MATERIAL_SURFACE=1",
        booleanDefine("HE_MATERIAL_PASS_FORWARD",
            artifact.materialPass == HEngine::MaterialShaderPass::Forward),
        booleanDefine("HE_MATERIAL_PASS_DEPTH",
            artifact.materialPass == HEngine::MaterialShaderPass::Depth),
        booleanDefine("HE_MATERIAL_PASS_SHADOW",
            artifact.materialPass == HEngine::MaterialShaderPass::Shadow),
        booleanDefine("HE_MATERIAL_PASS_OUTLINE",
            artifact.materialPass == HEngine::MaterialShaderPass::Outline),
        booleanDefine("HE_MATERIAL_SKINNED",
            artifact.vertexPath == HEngine::MaterialShaderVertexPath::Skinned),
        booleanDefine("HE_MATERIAL_DESCRIPTOR_HEAP_DIRECT",
            artifact.descriptorMode == HEngine::MaterialShaderDescriptorMode::HeapDirect),
        booleanDefine("HE_HEAP_DIRECT",
            artifact.stage == "ps" &&
                artifact.descriptorMode == HEngine::MaterialShaderDescriptorMode::HeapDirect),
        booleanDefine("HE_MATERIAL_SHADING_UNLIT",
            manifest.ShadingModel == HEngine::MaterialShaderShadingModel::Unlit),
        booleanDefine("HE_MATERIAL_BLEND_MASK",
            manifest.BlendMode == HEngine::MaterialShaderBlendMode::Mask),
        booleanDefine("HE_MATERIAL_BLEND_TRANSPARENT",
            manifest.BlendMode == HEngine::MaterialShaderBlendMode::Transparent),
        booleanDefine("HE_MATERIAL_DEPTH_WRITE", manifest.bDepthWrite),
        booleanDefine("HE_MATERIAL_CASTS_SHADOW", manifest.bCastsShadow),
    };
    defines.reserve(defines.size() + manifest.Properties.size());
    for (const HEngine::MaterialShaderProperty& property : manifest.Properties)
    {
        const std::string row = std::to_string(property.RowOffset) + "u";
        std::string expression;
        switch (property.Type)
        {
            case HEngine::MaterialShaderPropertyType::Float:
                expression = "HE_LoadMaterialFloat(" + row + ")";
                break;
            case HEngine::MaterialShaderPropertyType::Float2:
                expression = "HE_LoadMaterialFloat2(" + row + ")";
                break;
            case HEngine::MaterialShaderPropertyType::Float3:
                expression = "HE_LoadMaterialFloat3(" + row + ")";
                break;
            case HEngine::MaterialShaderPropertyType::Color:
                expression = "HE_LoadMaterialFloat4(" + row + ")";
                break;
            case HEngine::MaterialShaderPropertyType::Bool:
                expression = "(HE_LoadMaterialUint(" + row + ")!=0u)";
                break;
            case HEngine::MaterialShaderPropertyType::Int:
                expression = "asint(HE_LoadMaterialUint(" + row + "))";
                break;
            case HEngine::MaterialShaderPropertyType::Texture2D:
                expression = "HE_MATERIAL_TEXTURE(HE_LoadMaterialUint(" + row + "))";
                break;
        }
        defines.push_back(property.Name + "=" + expression);
    }
    return defines;
}

static MaterialPragmaParseResult ParseMaterialPragmas(
    const std::filesystem::path& shaderPath,
    const std::vector<std::filesystem::path>& dependencies,
    std::string recursiveSourceHash)
{
    MaterialPragmaParseResult result;
    const std::optional<std::string> sourceRelative = NormalizeSourceRelativePath(shaderPath);
    const std::string rootSource = sourceRelative.value_or(shaderPath.generic_string());

    std::vector<ParsedPragmaLine> rootPragmas;
    {
        std::istringstream stream(ReadFile(shaderPath));
        std::string line;
        std::uint32_t lineNumber = 0;
        while (std::getline(stream, line))
        {
            ++lineNumber;
            if (const auto pragma = ParsePragmaLine(line, lineNumber))
                rootPragmas.push_back(*pragma);
        }
    }

    result.bIsMaterialShader = std::any_of(
        rootPragmas.begin(), rootPragmas.end(), [](const ParsedPragmaLine& pragma)
        {
            return pragma.Directive == "hmaterial";
        });
    if (!result.bIsMaterialShader)
        return result;

    HEngine::MaterialShaderManifest& manifest = result.manifest;
    manifest.SourcePath = sourceRelative.value_or(std::string{});
    manifest.RecursiveSourceHash = std::move(recursiveSourceHash);
    if (!sourceRelative)
    {
        result.diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.SourcePath",
            "Material shader source must be inside the declared shader root.",
            rootSource));
    }
    const std::string rootSourceText = ReadFile(shaderPath);
    ValidateMaterialSourceContract(shaderPath, dependencies, result.diagnostics);
    for (const std::string_view reservedPermutationMacro : {
             std::string_view("HE_MATERIAL_PASS_FORWARD"),
             std::string_view("HE_MATERIAL_PASS_DEPTH"),
             std::string_view("HE_MATERIAL_PASS_SHADOW"),
             std::string_view("HE_MATERIAL_PASS_OUTLINE"),
             std::string_view("HE_MATERIAL_SKINNED"),
             std::string_view("HE_MATERIAL_SHADING_UNLIT"),
             std::string_view("HE_HEAP_DIRECT") })
    {
        if (rootSourceText.find(reservedPermutationMacro) != std::string::npos)
        {
            result.diagnostics.push_back(MakeMaterialDiagnostic(
                "Material.ReservedPermutationMacro",
                "Material definitions cannot branch on engine-owned permutation macro '" +
                    std::string(reservedPermutationMacro) + "'.",
                rootSource));
        }
    }

    std::size_t materialCount = 0;
    std::size_t blendCount = 0;
    std::size_t cullCount = 0;
    std::size_t depthCount = 0;
    std::size_t shadowCount = 0;
    std::size_t descriptorCount = 0;
    bool bDepthExplicit = false;
    bool bShadowExplicit = false;
    for (const ParsedPragmaLine& pragma : rootPragmas)
    {
        const std::vector<std::string> tokens = SplitWhitespaceTokens(pragma.Body);
        if (pragma.Directive == "hmaterial")
        {
            ++materialCount;
            if (tokens.size() != 2 || ToLowerCopy(tokens[0]) != "surface" ||
                (ToLowerCopy(tokens[1]) != "lit" && ToLowerCopy(tokens[1]) != "unlit"))
            {
                result.diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.HMaterial",
                    "#pragma hmaterial requires exactly 'Surface Lit' or 'Surface Unlit'.",
                    rootSource, pragma.Line, pragma.Column));
            }
            else
            {
                manifest.Domain = HEngine::MaterialShaderDomain::Surface;
                manifest.ShadingModel = ToLowerCopy(tokens[1]) == "lit"
                    ? HEngine::MaterialShaderShadingModel::Lit
                    : HEngine::MaterialShaderShadingModel::Unlit;
            }
        }
        else if (pragma.Directive == "hblend")
        {
            ++blendCount;
            const std::string value = tokens.size() == 1 ? ToLowerCopy(tokens[0]) : std::string{};
            if (value == "opaque") manifest.BlendMode = HEngine::MaterialShaderBlendMode::Opaque;
            else if (value == "mask") manifest.BlendMode = HEngine::MaterialShaderBlendMode::Mask;
            else if (value == "transparent") manifest.BlendMode = HEngine::MaterialShaderBlendMode::Transparent;
            else result.diagnostics.push_back(MakeMaterialDiagnostic(
                "Material.HBlend", "#pragma hblend requires Opaque, Mask, or Transparent.",
                rootSource, pragma.Line, pragma.Column));
        }
        else if (pragma.Directive == "hcull")
        {
            ++cullCount;
            const std::string value = tokens.size() == 1 ? ToLowerCopy(tokens[0]) : std::string{};
            if (value == "back") manifest.CullMode = HEngine::MaterialShaderCullMode::Back;
            else if (value == "none" || value == "doublesided") manifest.CullMode = HEngine::MaterialShaderCullMode::None;
            else result.diagnostics.push_back(MakeMaterialDiagnostic(
                "Material.HCull", "#pragma hcull requires Back, None, or DoubleSided.",
                rootSource, pragma.Line, pragma.Column));
        }
        else if (pragma.Directive == "hdepthwrite")
        {
            ++depthCount;
            bDepthExplicit = true;
            if (tokens.size() != 1 || !ParseMaterialBool(tokens[0], manifest.bDepthWrite))
                result.diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.HDepthWrite", "#pragma hdepthwrite requires On or Off.",
                    rootSource, pragma.Line, pragma.Column));
        }
        else if (pragma.Directive == "hshadow")
        {
            ++shadowCount;
            bShadowExplicit = true;
            if (tokens.size() != 1 || !ParseMaterialBool(tokens[0], manifest.bCastsShadow))
                result.diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.HShadow", "#pragma hshadow requires On or Off.",
                    rootSource, pragma.Line, pragma.Column));
        }
        else if (pragma.Directive == "hdescriptor")
        {
            ++descriptorCount;
            const std::string value = tokens.size() == 1 ? ToLowerCopy(tokens[0]) : std::string{};
            if (value == "baseline") manifest.DescriptorMode = HEngine::MaterialShaderDescriptorMode::Baseline;
            else if (value == "heapdirect" || value == "heap_direct") manifest.DescriptorMode = HEngine::MaterialShaderDescriptorMode::HeapDirect;
            else result.diagnostics.push_back(MakeMaterialDiagnostic(
                "Material.HDescriptor", "#pragma hdescriptor requires Baseline or HeapDirect.",
                rootSource, pragma.Line, pragma.Column));
        }
        else if (pragma.Directive == "hvariant" || pragma.Directive == "hprofile")
        {
            result.diagnostics.push_back(MakeMaterialDiagnostic(
                "Material.UnsupportedPermutationPragma",
                "Material Surface permutations and target profiles are engine-owned; remove #pragma " +
                    pragma.Directive + ".",
                rootSource, pragma.Line, pragma.Column));
        }
    }

    const auto rejectDuplicatePragma = [&](std::size_t count, std::string name)
    {
        if (count > 1)
        {
            result.diagnostics.push_back(MakeMaterialDiagnostic(
                "Material.DuplicatePragma", "#pragma " + name + " may only be declared once.", rootSource));
        }
    };
    rejectDuplicatePragma(materialCount, "hmaterial");
    rejectDuplicatePragma(blendCount, "hblend");
    rejectDuplicatePragma(cullCount, "hcull");
    rejectDuplicatePragma(depthCount, "hdepthwrite");
    rejectDuplicatePragma(shadowCount, "hshadow");
    rejectDuplicatePragma(descriptorCount, "hdescriptor");
    if (blendCount == 0)
        result.diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.MissingBlend", "Material shaders must declare #pragma hblend.", rootSource));
    if (!bDepthExplicit)
        manifest.bDepthWrite = manifest.BlendMode != HEngine::MaterialShaderBlendMode::Transparent;
    if (!bShadowExplicit)
    {
        manifest.bCastsShadow = manifest.ShadingModel == HEngine::MaterialShaderShadingModel::Lit &&
            manifest.BlendMode != HEngine::MaterialShaderBlendMode::Transparent;
    }

    std::unordered_set<std::string> propertyNames;
    std::unordered_set<std::string> stableIds;
    for (const std::filesystem::path& dependency : dependencies)
    {
        const std::string dependencySource = DependencyIdentity(dependency);
        std::istringstream stream(ReadFile(dependency));
        std::string line;
        std::uint32_t lineNumber = 0;
        while (std::getline(stream, line))
        {
            ++lineNumber;
            const auto pragma = ParsePragmaLine(line, lineNumber);
            if (!pragma || pragma->Directive != "property")
                continue;
            HEngine::MaterialShaderProperty property;
            if (!ParseMaterialPropertyLine(
                    pragma->Body, dependencySource, pragma->Line, property, result.diagnostics))
            {
                continue;
            }
            if (!propertyNames.insert(property.Name).second)
            {
                result.diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.DuplicateProperty",
                    "Material property '" + property.Name + "' is declared more than once.",
                    dependencySource, pragma->Line, pragma->Column));
            }
            if (!stableIds.insert(property.StableId).second)
            {
                result.diagnostics.push_back(MakeMaterialDiagnostic(
                    "Material.DuplicateStableId",
                    "Material property stable id '" + property.StableId + "' is declared more than once.",
                    dependencySource, pragma->Line, pragma->Column));
            }
            property.RowOffset = static_cast<std::uint32_t>(manifest.Properties.size());
            property.ByteOffset = property.RowOffset * HEngine::MaterialShaderParameterAlignment;
            property.Source = dependencySource;
            property.Line = pragma->Line;
            property.Column = pragma->Column;
            if (property.Type == HEngine::MaterialShaderPropertyType::Texture2D)
            {
                property.Binding.RegisterType = "ParameterRow";
                property.Binding.DescriptorIndex = static_cast<std::int32_t>(property.RowOffset);
            }
            manifest.Properties.push_back(std::move(property));
        }
    }
    manifest.ParameterBlock.ByteSize =
        static_cast<std::uint32_t>(manifest.Properties.size()) * HEngine::MaterialShaderParameterAlignment;
    manifest.ParameterBlock.Alignment = HEngine::MaterialShaderParameterAlignment;
    manifest.BindingSignatureHash =
        HEngine::BuildMaterialShaderBindingSignatureHash(manifest);
    return result;
}

static bool ClaimShaderArtifacts(
    const std::filesystem::path& shaderPath,
    const std::vector<CompiledShaderArtifact>& artifacts)
{
    std::string sourceIdentity = NormalizePath(RootPath).generic_string();
    sourceIdentity.push_back('\n');
    sourceIdentity += NormalizePath(shaderPath).generic_string();
#if defined(_WIN32)
    sourceIdentity = ToLowerCopy(std::move(sourceIdentity));
#endif
    const std::string sourceKey = BuildIdentityHash(
        "HEngine.HShaderCompiler.SourceIdentity.v1", sourceIdentity);
    const std::string ownerRecord =
        "format=HEngineShaderArtifactOwner-v1\nkey=" + sourceKey + "\n";

    std::vector<std::string> artifactNames{ shaderPath.stem().string() + ".meta" };
    artifactNames.reserve(artifacts.size() + 1);
    for (const CompiledShaderArtifact& artifact : artifacts)
        artifactNames.push_back(artifact.artifactStem + "." + artifact.stage);

    const std::filesystem::path ownerDirectory = OutputPath / "CACHES" / ".artifact-owners";
    std::error_code ec;
    std::filesystem::create_directories(ownerDirectory, ec);
    if (ec)
    {
        std::cout << "Failed to create shader artifact-owner directory: "
                  << ownerDirectory << ", error=" << ec.message() << std::endl;
        return false;
    }

    std::set<std::string> claimedArtifacts;
    for (const std::string& artifactName : artifactNames)
    {
        const std::string artifactKey = ToLowerCopy(artifactName);
        if (!claimedArtifacts.insert(artifactKey).second)
        {
            std::cout << "Shader declares duplicate case-insensitive cache artifact: source=\""
                      << shaderPath.generic_string() << "\" artifact=\""
                      << artifactName << "\"" << std::endl;
            return false;
        }
        const std::string artifactHash = BuildIdentityHash(
            "HEngine.HShaderCompiler.ArtifactName.v1", artifactKey);
        const std::filesystem::path ownerPath =
            ownerDirectory / (artifactHash + ".sourcekey");
        if (std::filesystem::exists(ownerPath, ec) && !ec)
        {
            if (ReadFile(ownerPath) != ownerRecord)
            {
                std::cout << "Shader cache artifact collision: source=\""
                          << shaderPath.generic_string() << "\" artifact=\""
                          << artifactName << "\"" << std::endl;
                return false;
            }
            continue;
        }
        ec.clear();
        std::ofstream owner(ownerPath, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!owner)
        {
            std::cout << "Failed to claim shader cache artifact: " << ownerPath << std::endl;
            return false;
        }
        owner << ownerRecord;
        if (!owner.good())
        {
            std::cout << "Failed to write shader artifact owner: " << ownerPath << std::endl;
            return false;
        }
    }
    return true;
}

static std::filesystem::path CacheKeyPath(const std::filesystem::path& outputFile)
{
    std::filesystem::path cacheKeyPath = outputFile;
    cacheKeyPath += ".cachekey";
    return cacheKeyPath;
}

static std::string SerializeCacheKey(const std::string& cacheKey)
{
    return "format=HEngineShaderCacheKey-v1\nalgorithm=sha256\nkey=" + cacheKey + "\n";
}

static bool IsShaderCacheCurrent(const std::filesystem::path& outputFile, const std::string& cacheKey)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(outputFile, ec) || ec)
        return false;

    std::string sidecar;
    if (!ReadTextFile(CacheKeyPath(outputFile), sidecar))
        return false;
    return sidecar == SerializeCacheKey(cacheKey);
}

static bool WriteShaderCacheKey(const std::filesystem::path& outputFile, const std::string& cacheKey)
{
    const std::filesystem::path cacheKeyPath = CacheKeyPath(outputFile);
    std::ofstream out(cacheKeyPath, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out.is_open())
    {
        std::cout << "Failed to open shader cache-key sidecar: " << cacheKeyPath << std::endl;
        return false;
    }
    out << SerializeCacheKey(cacheKey);
    if (!out.good())
    {
        std::cout << "Failed to write shader cache-key sidecar: " << cacheKeyPath << std::endl;
        return false;
    }
    return true;
}

static bool WriteShaderMeta(
    const std::string& shaderName,
    const ShaderParseResult& parseResult,
    const std::vector<CompiledShaderArtifact>& artifacts)
{
    std::filesystem::path outMetaPath = OutputPath / "CACHES" / (shaderName + ".meta");
    std::ofstream ofs(outMetaPath.string(), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!ofs.is_open())
    {
        std::cout << "Failed to open shader meta file: " << outMetaPath << std::endl;
        return false;
    }

    ofs << parseResult.meta;
    ofs << "[ShaderVariants]\r\n";
    ofs << "Defines=";
    for (size_t i = 0; i < parseResult.variantDefines.size(); ++i)
    {
        if (i > 0)
            ofs << ";";
        ofs << parseResult.variantDefines[i];
    }
    ofs << "\r\n";

    ofs << "[ShaderStageMap]\r\n";
    for (const CompiledShaderArtifact& artifact : artifacts)
    {
        ofs << artifact.variant << "." << artifact.stage << "="
            << artifact.artifactStem << "\r\n";
    }

    ofs << "[ShaderTargetProfiles]\r\n";
    for (const CompiledShaderArtifact& artifact : artifacts)
    {
        ofs << artifact.variant << "." << artifact.stage << "="
            << artifact.profile << "\r\n";
    }

    ofs << "[ShaderArtifactTargets]\r\n";
    for (const HEngine::ShaderArtifactTarget& target : ArtifactTargets)
    {
        if (parseResult.backendOnly && target.Backend != *parseResult.backendOnly)
            continue;
        ofs << ShaderBackendName(target.Backend) << "="
            << HEngine::GetShaderArtifactDirectoryName(target.BinaryFormat) << "\r\n";
    }

    if (!ofs.good())
    {
        std::cout << "Failed to write shader meta file: " << outMetaPath << std::endl;
        return false;
    }

    return true;
}

static bool TryGetPragmaLineSpan(const std::string& source, size_t pos, size_t& outBegin, size_t& outEnd)
{
    outBegin = pos;
    outEnd = source.find_first_of("\r\n", pos);
    if (outEnd == std::string::npos)
        outEnd = source.size();
    return outBegin < outEnd;
}

static size_t FindNextContentPos(const std::string& source, size_t pos)
{
    if (pos >= source.size())
        return std::string::npos;
    return source.find_first_not_of("\r\n", pos);
}

ShaderParseResult PreParse(const std::string& source, const std::string& shaderName)
{
    ShaderParseResult result;
    // TODO: 现在文件必须 CRTF 才行

    {
        // 找到 shader 的 type 与 properties，写入到 .meta 文件中
        std::string metaString = "[Type]\r\nType=";

        // shader type
        {
            const char* typeToken = "#pragma hshader";
            size_t typeTokenLength = strlen(typeToken);
            size_t pos = source.find(typeToken, 0);
            std::string shaderType;
            if (pos != std::string::npos)
            {
                size_t begin = 0;
                size_t end = 0;
                if (TryGetPragmaLineSpan(source, pos, begin, end))
                {
                    const size_t valueBegin = pos + typeTokenLength + 1;
                    if (valueBegin <= end)
                        shaderType = source.substr(valueBegin, end - valueBegin);
                }
            }
            if (shaderType.empty())
                shaderType = "PBR";
            metaString += shaderType + "\r\n[Properties]\r\n";
        }

        // shader properties
        {
            const char* typeToken = "#pragma hrange";
            size_t typeTokenLength = strlen(typeToken);
            size_t pos = source.find(typeToken, 0);
            while (pos != std::string::npos)
            {
                size_t begin = 0;
                size_t end = 0;
                if (!TryGetPragmaLineSpan(source, pos, begin, end))
                    break;

                const size_t valueBegin = pos + typeTokenLength + 1;
                std::string propertyDes;
                if (valueBegin <= end)
                    propertyDes = source.substr(valueBegin, end - valueBegin);
                auto property = Utils::Split(propertyDes);
                // 范围最小值与最大值
                if (property.size() == 3)
                {
                    metaString += property[0] + "=" + property[1] + "," + property[2] + "\r\n";
                }

                size_t nextLinePos = FindNextContentPos(source, end);
                if (nextLinePos == std::string::npos)
                    break;
                pos = source.find(typeToken, nextLinePos); //Start of next shader type declaration line
            }
        }

        result.meta = std::move(metaString);
    }

    {
        const char* variantToken = "#pragma hvariant";
        size_t variantTokenLength = strlen(variantToken);
        size_t variantPos = source.find(variantToken, 0);
        while (variantPos != std::string::npos)
        {
            size_t begin = 0;
            size_t end = 0;
            if (!TryGetPragmaLineSpan(source, variantPos, begin, end))
                break;

            const size_t valueBegin = variantPos + variantTokenLength + 1;
            std::string variantName;
            if (valueBegin <= end)
                variantName = TrimString(source.substr(valueBegin, end - valueBegin));
            if (!variantName.empty())
            {
                result.variantDefines.push_back(variantName);
            }

            size_t nextLinePos = FindNextContentPos(source, end);
            if (nextLinePos == std::string::npos)
                break;
            variantPos = source.find(variantToken, nextLinePos);
        }
    }

    {
        const char* profileToken = "#pragma hprofile";
        const size_t profileTokenLength = strlen(profileToken);
        size_t profilePos = source.find(profileToken, 0);
        while (profilePos != std::string::npos)
        {
            size_t begin = 0;
            size_t end = 0;
            if (!TryGetPragmaLineSpan(source, profilePos, begin, end))
                break;

            const size_t valueBegin = profilePos + profileTokenLength + 1;
            const std::string profileDescription = valueBegin <= end
                ? TrimString(source.substr(valueBegin, end - valueBegin))
                : std::string{};
            std::istringstream profileStream(profileDescription);
            std::vector<std::string> tokens;
            std::string token;
            while (profileStream >> token)
                tokens.push_back(std::move(token));
            if (tokens.size() != 2 && tokens.size() != 3)
            {
                throw std::runtime_error(
                    "Shader '#pragma hprofile' requires '<stage> <profile> "
                    "[BASE|<variant>]': " + shaderName + " (" + profileDescription + ")");
            }

            ShaderProfileRule rule{
                .stage = ToLowerCopy(tokens[0]),
                .profile = ToLowerCopy(tokens[1]),
            };
            if (tokens.size() == 3)
            {
                rule.variant = ToLowerCopy(tokens[2]) == "base"
                    ? "BASE"
                    : tokens[2];
            }
            result.profileRules.push_back(std::move(rule));

            const size_t nextLinePos = FindNextContentPos(source, end);
            if (nextLinePos == std::string::npos)
                break;
            profilePos = source.find(profileToken, nextLinePos);
        }
    }

    {
        const char* backendToken = "#pragma hbackend";
        const size_t backendTokenLength = strlen(backendToken);
        const size_t backendPos = source.find(backendToken, 0);
        if (backendPos != std::string::npos)
        {
            size_t begin = 0;
            size_t end = 0;
            if (!TryGetPragmaLineSpan(source, backendPos, begin, end))
                throw std::runtime_error("Malformed #pragma hbackend in shader: " + shaderName);
            const size_t valueBegin = backendPos + backendTokenLength + 1;
            const std::string backend = valueBegin <= end
                ? ToLowerCopy(TrimString(source.substr(valueBegin, end - valueBegin)))
                : std::string{};
            if (backend == "dx12")
                result.backendOnly = HEngine::RenderBackend::DX12;
            else if (backend == "vulkan")
                result.backendOnly = HEngine::RenderBackend::Vulkan;
            else
            {
                throw std::runtime_error(
                    "Shader '#pragma hbackend' requires DX12 or Vulkan: " +
                    shaderName + " (" + backend + ")");
            }
            const size_t nextBackend = source.find(backendToken, end);
            if (nextBackend != std::string::npos)
            {
                throw std::runtime_error(
                    "Shader declares #pragma hbackend more than once: " + shaderName);
            }
        }
    }

    const char* typeToken = "#pragma htype";
    size_t typeTokenLength = strlen(typeToken);
    size_t pos = source.find(typeToken, 0);
    while (pos != std::string::npos)
    {
        size_t begin = 0;
        size_t end = 0;
        if (!TryGetPragmaLineSpan(source, pos, begin, end))
            break;

        const size_t valueBegin = pos + typeTokenLength + 1;
        std::string typeDes;
        if (valueBegin <= end)
            typeDes = TrimString(source.substr(valueBegin, end - valueBegin));
        const size_t posSpace = typeDes.find_first_of(" \t");
        if (posSpace == std::string::npos)
        {
            if (ToLowerCopy(typeDes) == "lib")
            {
                result.stages.push_back({ "lib", {} });
            }
            else
            {
                std::cout << "Skip malformed #pragma htype in shader '" << shaderName << "': " << typeDes << std::endl;
            }
            size_t nextLinePos = FindNextContentPos(source, end);
            if (nextLinePos == std::string::npos)
                break;
            pos = source.find(typeToken, nextLinePos);
            continue;
        }
        result.stages.push_back({
            ToLowerCopy(TrimString(typeDes.substr(0, posSpace))),
            TrimString(typeDes.substr(posSpace + 1))
        });

        size_t nextLinePos = FindNextContentPos(source, end); //Start of shader code after shader type declaration line
        if (nextLinePos == std::string::npos)
            break;
        pos = source.find(typeToken, nextLinePos); //Start of next shader type declaration line
    }

    return result;
}

static std::vector<std::string> ParseDxilRequirementLines(const std::string& dump)
{
    constexpr std::string_view RequirementsHeader =
        "shader requires additional functionality:";
    std::vector<std::string> requirements;
    std::istringstream stream(dump);
    std::string line;
    bool bReadingRequirements = false;
    while (std::getline(stream, line))
    {
        std::string content = TrimString(line);
        if (!content.empty() && content.front() == ';')
            content = TrimString(content.substr(1));
        if (!bReadingRequirements)
        {
            if (ToLowerCopy(content).find(RequirementsHeader) != std::string::npos)
                bReadingRequirements = true;
            continue;
        }
        if (content.empty())
            break;
        requirements.push_back(std::move(content));
    }
    return requirements;
}

static bool ValidateDxilRequirements(
    const std::filesystem::path& dxcPath,
    const std::filesystem::path& outputPath,
    const std::filesystem::path& sourcePath,
    const CompiledShaderArtifact& artifact)
{
#ifdef _WIN32
    std::string dump;
    if (!RunTool(
        dxcPath,
        "-dumpbin \"" + outputPath.string() + "\"",
        dump,
        false))
    {
        std::cout << "Failed to inspect compiled DXIL requirements: source=\""
                  << sourcePath.generic_string() << "\" variant=\""
                  << artifact.variant << "\" profile=\"" << artifact.profile
                  << "\"" << std::endl;
        return false;
    }

    std::string profileStage;
    unsigned int profileMajor = 0;
    unsigned int profileMinor = 0;
    if (!TryParseTargetProfile(
        artifact.profile, profileStage, profileMajor, profileMinor))
    {
        std::cout << "Internal error: invalid target profile reached DXIL validation: "
                  << artifact.profile << std::endl;
        return false;
    }
    const std::string shaderModelRecord =
        "!{!\"" + profileStage + "\", i32 " + std::to_string(profileMajor) +
        ", i32 " + std::to_string(profileMinor) + "}";
    if (dump.find("!dx.shaderModel =") == std::string::npos ||
        dump.find(shaderModelRecord) == std::string::npos)
    {
        std::cout << "DXIL reflection target-profile mismatch: source=\""
                  << sourcePath.generic_string() << "\" variant=\""
                  << artifact.variant << "\" expected=\"" << artifact.profile
                  << "\". The compiled container did not report "
                  << shaderModelRecord << std::endl;
        return false;
    }

    const std::vector<std::string> requirements = ParseDxilRequirementLines(dump);
    if (profileMajor == 6 && profileMinor == 0)
    {
        constexpr std::array<std::string_view, 8> ForbiddenBaselineRequirements = {
            "resource descriptor heap indexing",
            "sampler descriptor heap indexing",
            "64-bit atomic",
            "atomic operations on 64-bit",
            "advanced texture ops",
            "samplecmpgradient",
            "writable msaa",
            "wave matrix",
        };
        for (const std::string& requirement : requirements)
        {
            const std::string normalizedRequirement = ToLowerCopy(requirement);
            const auto forbidden = std::find_if(
                ForbiddenBaselineRequirements.begin(),
                ForbiddenBaselineRequirements.end(),
                [&normalizedRequirement](std::string_view feature)
                {
                    return normalizedRequirement.find(feature) != std::string::npos;
                });
            if (forbidden != ForbiddenBaselineRequirements.end())
            {
                std::cout << "Baseline shader unexpectedly requires a post-SM6.0 feature: source=\""
                          << sourcePath.generic_string() << "\" variant=\""
                          << artifact.variant << "\" entry=\"" << artifact.entry
                          << "\" profile=\"" << artifact.profile << "\" requirement=\""
                          << requirement << "\". Gate the feature behind an hvariant and add "
                          << "an explicit #pragma hprofile for that variant." << std::endl;
                return false;
            }
        }
    }
    return true;
#else
    (void)dxcPath;
    (void)outputPath;
    (void)sourcePath;
    (void)artifact;
    std::cout << "DXIL requirements validation is only supported on Windows." << std::endl;
    return false;
#endif
}

static std::vector<HEngine::MaterialShaderDiagnostic> ParseDxcDiagnostics(
    const std::string& text,
    const std::string& fallbackSource)
{
    static const std::regex locationPattern(
        R"(^(.+?)\(([0-9]+),([0-9]+)\)\s*:\s*(warning|error)[^:]*:\s*(.*)$)",
        std::regex_constants::icase);
    static const std::regex colonLocationPattern(
        R"(^(.+?):([0-9]+):([0-9]+):\s*(warning|error)[^:]*:\s*(.*)$)",
        std::regex_constants::icase);
    std::vector<HEngine::MaterialShaderDiagnostic> diagnostics;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        std::smatch match;
        if (!std::regex_match(line, match, locationPattern) &&
            !std::regex_match(line, match, colonLocationPattern))
            continue;
        std::int64_t parsedLine = 0;
        std::int64_t parsedColumn = 0;
        if (!TryParseInt64(match[2].str(), parsedLine) ||
            !TryParseInt64(match[3].str(), parsedColumn) ||
            parsedLine < 0 || parsedColumn < 0)
        {
            continue;
        }
        const bool bWarning = ToLowerCopy(match[4].str()) == "warning";
        diagnostics.push_back(MakeMaterialDiagnostic(
            bWarning ? "DXC.Warning" : "DXC.Error",
            TrimString(match[5].str()),
            match[1].str().empty() ? fallbackSource : match[1].str(),
            static_cast<std::uint32_t>(std::min<std::int64_t>(
                parsedLine, std::numeric_limits<std::uint32_t>::max())),
            static_cast<std::uint32_t>(std::min<std::int64_t>(
                parsedColumn, std::numeric_limits<std::uint32_t>::max())),
            bWarning
                ? HEngine::MaterialShaderDiagnosticSeverity::Warning
                : HEngine::MaterialShaderDiagnosticSeverity::Error));
    }
    return diagnostics;
}

static bool ValidateSpirVArtifact(const std::filesystem::path& artifactPath);

static std::vector<std::string> MaterialCompileOptions(
    const HEngine::ShaderArtifactTarget& target)
{
    return BuildShaderCompileOptions(target);
}

static bool CompileMaterialArtifactToPath(
    const CompiledShaderArtifact& artifact,
    const std::filesystem::path& sourcePath,
    const std::vector<std::filesystem::path>& dependencies,
    const std::vector<std::string>& defines,
    const HEngine::ShaderArtifactTarget& target,
    const std::filesystem::path& outputPath,
    const std::string& cacheKey,
    std::vector<HEngine::MaterialShaderDiagnostic>& diagnostics)
{
    std::error_code ec;
    std::filesystem::create_directories(outputPath.parent_path(), ec);
    if (ec)
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.ArtifactDirectory",
            "Failed to create staged artifact directory: " + ec.message(),
            sourcePath.generic_string()));
        return false;
    }

    std::string defineArgs;
    for (const std::string& define : defines)
        defineArgs += " -D \"" + NormalizeDefine(define) + "\"";
    std::string includeArgs = " -I \"" + RootPath.string() + "\"";
    for (const std::filesystem::path& includeRoot : AdditionalIncludePaths)
        includeArgs += " -I \"" + includeRoot.string() + "\"";
    const std::string entryArg = artifact.entry.empty()
        ? std::string{}
        : " -E \"" + artifact.entry + "\"";
    const std::string args = "-nologo -Zpc -HV 2021 -O3" +
        BuildDxcBackendArguments(target) + " -T " + artifact.profile +
        entryArg + defineArgs + includeArgs +
        " -Fo \"" + outputPath.string() + "\" \"" + sourcePath.string() + "\"";

#ifdef _WIN32
    const std::filesystem::path dxcPath = std::filesystem::current_path() / "dxc.exe";
    std::string compilerOutput;
    if (!RunTool(dxcPath, args, compilerOutput, false))
    {
        std::vector<HEngine::MaterialShaderDiagnostic> parsed =
            ParseDxcDiagnostics(compilerOutput, sourcePath.generic_string());
        if (parsed.empty())
        {
            parsed.push_back(MakeMaterialDiagnostic(
                "DXC.Compile",
                "DXC failed for permutation '" + artifact.variant + "', stage '" + artifact.stage + "'.",
                sourcePath.generic_string()));
        }
        diagnostics.insert(
            diagnostics.end(),
            std::make_move_iterator(parsed.begin()),
            std::make_move_iterator(parsed.end()));
        return false;
    }
    std::vector<HEngine::MaterialShaderDiagnostic> parsedWarnings =
        ParseDxcDiagnostics(compilerOutput, sourcePath.generic_string());
    diagnostics.insert(
        diagnostics.end(),
        std::make_move_iterator(parsedWarnings.begin()),
        std::make_move_iterator(parsedWarnings.end()));
#else
    const std::string command = "\"dxc.exe\" " + args;
    if (system(command.c_str()) != 0)
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "DXC.Compile",
            "DXC failed for permutation '" + artifact.variant + "', stage '" + artifact.stage + "'.",
            sourcePath.generic_string()));
        return false;
    }
#endif

    if (!std::filesystem::is_regular_file(outputPath, ec) || ec)
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "DXC.Output", "DXC reported success without producing a staged shader artifact.",
            sourcePath.generic_string()));
        return false;
    }
#ifdef _WIN32
    if (target.BinaryFormat == HEngine::ShaderBinaryFormat::Dxil &&
        !ValidateDxilRequirements(dxcPath, outputPath, sourcePath, artifact))
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "DXC.Requirements", "Compiled DXIL failed target-profile or capability validation.",
            sourcePath.generic_string()));
        return false;
    }
#endif
    if (target.BinaryFormat == HEngine::ShaderBinaryFormat::SpirV &&
        !ValidateSpirVArtifact(outputPath))
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "SPIRV.Validation",
            "Compiled SPIR-V failed Vulkan 1.3 validation.",
            sourcePath.generic_string()));
        return false;
    }
    if (!WriteShaderCacheKey(outputPath, cacheKey))
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.CacheKey", "Failed to write staged shader cache-key sidecar.",
            sourcePath.generic_string()));
        return false;
    }
    return true;
}

static std::filesystem::path BuildMaterialArtifactRelativePath(
    const std::string& sourceRelativePath,
    const std::string& recursiveSourceHash,
    const std::string& cacheKey,
    const CompiledShaderArtifact& artifact,
    const HEngine::ShaderArtifactTarget& target)
{
    const std::string sourceIdentity = BuildIdentityHash(
        "HEngine.MaterialShader.SourcePath.v1", ToLowerCopy(sourceRelativePath));
    const std::string programIdentity = BuildIdentityHash(
        "HEngine.MaterialShader.ArtifactIdentity.v1",
        ToLowerCopy(sourceRelativePath) + "\n" + recursiveSourceHash + "\n" + cacheKey);
    const std::string shaderStem =
        std::filesystem::path(sourceRelativePath).stem().string();
    return std::filesystem::path(
        HEngine::GetShaderArtifactDirectoryName(target.BinaryFormat)) / "Materials" /
        sourceIdentity.substr(0, 16) / programIdentity.substr(0, 24) /
        (shaderStem + "." + artifact.stage);
}

static bool FilesEqual(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(left, ec) || ec)
        return false;
    ec.clear();
    if (!std::filesystem::is_regular_file(right, ec) || ec)
        return false;
    ec.clear();
    if (std::filesystem::file_size(left, ec) != std::filesystem::file_size(right, ec) || ec)
        return false;
    return ReadFile(left) == ReadFile(right);
}

static bool PublishImmutableFile(
    const std::filesystem::path& stagedPath,
    const std::filesystem::path& finalPath,
    std::string& errorMessage)
{
    std::error_code ec;
    std::filesystem::create_directories(finalPath.parent_path(), ec);
    if (ec)
    {
        errorMessage = "Failed to create artifact output directory: " + ec.message();
        return false;
    }
    if (std::filesystem::exists(finalPath, ec) && !ec)
    {
        if (!FilesEqual(stagedPath, finalPath))
        {
            errorMessage = "Content-addressed shader artifact already exists with different bytes: " +
                finalPath.generic_string();
            return false;
        }
        std::filesystem::remove(stagedPath, ec);
        return true;
    }

    if (!HEngine::PlatformFileSystem::PublishFileAtomically(
            stagedPath, finalPath, false, ec))
    {
        if (std::filesystem::exists(finalPath) && FilesEqual(stagedPath, finalPath))
        {
            std::error_code cleanupError;
            std::filesystem::remove(stagedPath, cleanupError);
            return true;
        }
        errorMessage = "Failed to atomically publish shader artifact: " + ec.message();
        return false;
    }
    return true;
}

static bool WriteTextFileAtomically(
    const std::filesystem::path& finalPath,
    std::string_view text,
    std::string& errorMessage)
{
    std::error_code ec;
    std::filesystem::create_directories(finalPath.parent_path(), ec);
    if (ec)
    {
        errorMessage = "Failed to create manifest output directory: " + ec.message();
        return false;
    }
    const std::filesystem::path temporaryPath =
        HEngine::PlatformFileSystem::MakeTemporarySiblingPath(finalPath);
    {
        std::ofstream output(temporaryPath, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!output)
        {
            errorMessage = "Failed to open temporary manifest: " + temporaryPath.generic_string();
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output.good())
        {
            errorMessage = "Failed to write temporary manifest: " + temporaryPath.generic_string();
            output.close();
            std::filesystem::remove(temporaryPath, ec);
            return false;
        }
    }
    if (!HEngine::PlatformFileSystem::PublishFileAtomically(
            temporaryPath, finalPath, true, ec))
    {
        errorMessage = "Failed to atomically publish material manifest: " + ec.message();
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        return false;
    }
    return true;
}

class MaterialStagingDirectory final
{
public:
    explicit MaterialStagingDirectory(const std::filesystem::path& cacheDirectory)
        : mPath(HEngine::PlatformFileSystem::MakeTemporarySiblingPath(
            cacheDirectory / ".material-staging"))
    {
        std::error_code ec;
        std::filesystem::create_directories(mPath, ec);
        if (ec)
            throw std::runtime_error("Failed to create material shader staging directory: " + ec.message());
    }

    ~MaterialStagingDirectory()
    {
        std::error_code ec;
        std::filesystem::remove_all(mPath, ec);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return mPath; }

private:
    std::filesystem::path mPath;
};

static bool HasMaterialErrors(
    const std::vector<HEngine::MaterialShaderDiagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic)
    {
        return diagnostic.Severity == HEngine::MaterialShaderDiagnosticSeverity::Error;
    });
}

static bool ValidateMaterialDependenciesUnchanged(
    const std::filesystem::path& shaderPath,
    std::string_view expectedRecursiveHash,
    std::vector<HEngine::MaterialShaderDiagnostic>& diagnostics)
{
    try
    {
        const std::vector<std::filesystem::path> currentDependencies =
            CollectShaderDependencies(shaderPath);
        const std::string currentRecursiveHash =
            BuildRecursiveSourceHash(currentDependencies);
        if (currentRecursiveHash == expectedRecursiveHash)
            return true;

        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.DependencyChangedDuringCompile",
            "A material shader source or recursive include changed during compilation; manifest publication was aborted, temporary staging will be discarded, and any immutable blobs already published remain unreachable. Retry the compile.",
            shaderPath.generic_string()));
    }
    catch (const std::exception& exception)
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.DependencyRecheck",
            "Failed to re-read the recursive material shader dependency graph before publication: " +
                std::string(exception.what()),
            shaderPath.generic_string()));
    }
    return false;
}

static bool PublishMaterialManifestWithDependencyLease(
    const std::filesystem::path& shaderPath,
    std::span<const std::filesystem::path> dependencies,
    std::string_view expectedRecursiveHash,
    const std::filesystem::path& manifestPath,
    std::string_view manifestText,
    std::vector<HEngine::MaterialShaderDiagnostic>& diagnostics)
{
    HEngine::PlatformStableFileReadLease dependencyLease;
    std::error_code leaseError;
    if (!HEngine::PlatformFileSystem::AcquireStableFileReadLease(
            dependencies, dependencyLease, leaseError))
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.DependencyChangedDuringCompile",
            "A material shader source or recursive include could not be locked for stable publication (" +
                leaseError.message() +
                "); manifest publication was aborted and the previous manifest was preserved. Retry the compile.",
            shaderPath.generic_string()));
        return false;
    }

    // This re-collects the graph only after every dependency is protected. If
    // any file changed before its lease was acquired (including a replace-save),
    // the recursive hash differs and publication is rejected. The lease remains
    // alive through the manifest's atomic replacement, closing the prior TOCTOU
    // window.
    if (!ValidateMaterialDependenciesUnchanged(
            shaderPath, expectedRecursiveHash, diagnostics))
    {
        return false;
    }

    std::string publishError;
    if (!WriteTextFileAtomically(manifestPath, manifestText, publishError))
    {
        diagnostics.push_back(MakeMaterialDiagnostic(
            "Material.ManifestPublish", publishError, shaderPath.generic_string()));
        return false;
    }
    return true;
}

static bool CompileMaterialShader(
    const std::filesystem::path& shaderPath,
    MaterialPragmaParseResult materialParse,
    const std::vector<CompiledShaderArtifact>& artifacts,
    const std::vector<std::filesystem::path>& dependencies)
{
    if (!materialParse.Succeeded())
    {
        EmitMaterialDiagnostics(materialParse.diagnostics);
        return false;
    }

    HEngine::MaterialShaderManifest& manifest = materialParse.manifest;
    MaterialStagingDirectory staging(OutputPath / "CACHES");
    std::vector<PublishedMaterialArtifact> publishedArtifacts;
    publishedArtifacts.reserve(artifacts.size() * ArtifactTargets.size());
    std::size_t publicationIndex = 0;

    for (const HEngine::ShaderArtifactTarget& artifactTarget : ArtifactTargets)
    {
        const std::vector<std::string> compileOptions =
            MaterialCompileOptions(artifactTarget);
        for (const CompiledShaderArtifact& artifact : artifacts)
        {
            if (artifactTarget.Backend == HEngine::RenderBackend::Vulkan &&
                artifact.descriptorMode == HEngine::MaterialShaderDescriptorMode::HeapDirect)
            {
                continue;
            }
            const std::vector<std::string> defines =
                BuildMaterialPermutationDefines(manifest, artifact);
            const std::string cacheKey = BuildShaderCacheKey(
                dependencies,
                defines,
                artifact.entry,
                artifact.profile,
                compileOptions,
                artifactTarget);
            const std::filesystem::path relativePath = BuildMaterialArtifactRelativePath(
                manifest.SourcePath,
                manifest.RecursiveSourceHash,
                cacheKey,
                artifact,
                artifactTarget);
            const std::filesystem::path finalPath = OutputPath / "CACHES" / relativePath;

            PublishedMaterialArtifact publication{
                .compile = artifact,
                .target = artifactTarget,
                .cacheKey = cacheKey,
                .finalArtifactPath = finalPath,
                .finalCacheKeyPath = CacheKeyPath(finalPath),
                .relativeArtifactPath = relativePath.generic_string(),
            };
            if (IsShaderCacheCurrent(finalPath, cacheKey))
            {
                publication.contentHash = BuildFileContentHash(finalPath);
            }
            else
            {
                publication.bNeedsPublish = true;
                publication.stagedArtifactPath = staging.Path() /
                    std::to_string(publicationIndex) / finalPath.filename();
                if (!CompileMaterialArtifactToPath(
                        artifact,
                        shaderPath,
                        dependencies,
                        defines,
                        artifactTarget,
                        publication.stagedArtifactPath,
                        cacheKey,
                        materialParse.diagnostics))
                {
                    EmitMaterialDiagnostics(materialParse.diagnostics);
                    return false;
                }
                publication.stagedCacheKeyPath = CacheKeyPath(publication.stagedArtifactPath);
                publication.contentHash = BuildFileContentHash(publication.stagedArtifactPath);
            }
            publishedArtifacts.push_back(std::move(publication));
            ++publicationIndex;
        }
    }

    manifest.ArtifactSets.clear();
    manifest.Artifacts.clear();
    for (const PublishedMaterialArtifact& publication : publishedArtifacts)
    {
        auto set = std::find_if(
            manifest.ArtifactSets.begin(),
            manifest.ArtifactSets.end(),
            [&publication](const HEngine::MaterialShaderArtifactSet& candidate)
            {
                return candidate.Backend == publication.target.Backend &&
                    candidate.BinaryFormat == publication.target.BinaryFormat;
            });
        if (set == manifest.ArtifactSets.end())
        {
            manifest.ArtifactSets.push_back({
                .Backend = publication.target.Backend,
                .BinaryFormat = publication.target.BinaryFormat,
            });
            set = std::prev(manifest.ArtifactSets.end());
        }
        set->Artifacts.push_back({
            .Pass = publication.compile.materialPass,
            .Stage = publication.compile.stage,
            .EntryPoint = publication.compile.entry,
            .TargetProfile = publication.compile.profile,
            .Permutation = publication.compile.variant,
            .DescriptorMode = publication.compile.descriptorMode,
            .VertexPath = publication.compile.vertexPath,
            .RelativePath = publication.relativeArtifactPath,
            .ContentHash = publication.contentHash,
        });
    }
    if (const HEngine::MaterialShaderArtifactSet* dxil =
            HEngine::FindMaterialShaderArtifactSet(
                manifest,
                HEngine::RenderBackend::DX12,
                HEngine::ShaderBinaryFormat::Dxil))
    {
        manifest.Artifacts = dxil->Artifacts;
    }
    for (const HEngine::MaterialShaderDiagnostic& diagnostic : materialParse.diagnostics)
    {
        if (diagnostic.Severity != HEngine::MaterialShaderDiagnosticSeverity::Error)
            manifest.Diagnostics.push_back(diagnostic);
    }

    const std::vector<HEngine::MaterialShaderDiagnostic> validation =
        HEngine::ValidateMaterialShaderManifest(manifest, manifest.SourcePath);
    if (HasMaterialErrors(validation))
    {
        EmitMaterialDiagnostics(validation);
        return false;
    }

    const nlohmann::json candidateManifestJson =
        HEngine::MaterialShaderManifestToJson(manifest);
    HEngine::MaterialShaderManifestLoadResult parsedManifest =
        HEngine::ParseMaterialShaderManifest(candidateManifestJson, manifest.SourcePath);
    if (!parsedManifest.Succeeded())
    {
        EmitMaterialDiagnostics(parsedManifest.Diagnostics);
        return false;
    }
    manifest = std::move(parsedManifest.Manifest);

    if (!ValidateMaterialDependenciesUnchanged(
            shaderPath, manifest.RecursiveSourceHash, materialParse.diagnostics))
    {
        EmitMaterialDiagnostics(materialParse.diagnostics);
        return false;
    }

    for (PublishedMaterialArtifact& publication : publishedArtifacts)
    {
        if (!publication.bNeedsPublish)
            continue;
        std::string publishError;
        if (!PublishImmutableFile(
                publication.stagedArtifactPath,
                publication.finalArtifactPath,
                publishError) ||
            !PublishImmutableFile(
                publication.stagedCacheKeyPath,
                publication.finalCacheKeyPath,
                publishError))
        {
            materialParse.diagnostics.push_back(MakeMaterialDiagnostic(
                "Material.ArtifactPublish", publishError, manifest.SourcePath));
            EmitMaterialDiagnostics(materialParse.diagnostics);
            return false;
        }
    }

    const std::filesystem::path manifestPath = OutputPath / "CACHES" / "Materials" /
        (std::filesystem::path(manifest.SourcePath).generic_string() + ".material.json");
    std::string manifestText = HEngine::MaterialShaderManifestToJson(manifest).dump(
        2, ' ', false, nlohmann::json::error_handler_t::replace);
    manifestText.push_back('\n');
    if (!PublishMaterialManifestWithDependencyLease(
            shaderPath,
            dependencies,
            manifest.RecursiveSourceHash,
            manifestPath,
            manifestText,
            materialParse.diagnostics))
    {
        EmitMaterialDiagnostics(materialParse.diagnostics);
        return false;
    }

    std::cout << "Published material shader manifest: "
              << manifestPath.generic_string()
              << " recursiveHash=" << manifest.RecursiveSourceHash
              << " artifactSets=" << manifest.ArtifactSets.size()
              << " artifacts=" << publishedArtifacts.size() << std::endl;
    return true;
}

static bool ValidateSpirVArtifact(const std::filesystem::path& artifactPath)
{
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(artifactPath, error);
    if (error || size < sizeof(std::uint32_t) || size % sizeof(std::uint32_t) != 0)
    {
        std::cout << "SPIR-V artifact has an invalid byte size: " << artifactPath << std::endl;
        return false;
    }

    std::vector<std::uint32_t> words(
        static_cast<std::size_t>(size / sizeof(std::uint32_t)));
    std::ifstream input(artifactPath, std::ios::binary);
    input.read(
        reinterpret_cast<char*>(words.data()),
        static_cast<std::streamsize>(size));
    if (!input || words.empty() || words.front() != 0x07230203u)
    {
        std::cout << "DXC output is not a valid SPIR-V module: " << artifactPath << std::endl;
        return false;
    }

    std::string validationMessages;
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    if (!validator.IsValid())
    {
        std::cout << "Failed to initialize SPIRV-Tools for Vulkan 1.3 validation." << std::endl;
        return false;
    }
    validator.SetMessageConsumer(
        [&validationMessages](
            spv_message_level_t level,
            const char* source,
            const spv_position_t& position,
            const char* message)
        {
            validationMessages += source ? source : "SPIR-V";
            validationMessages += ':';
            validationMessages += std::to_string(position.line);
            validationMessages += ':';
            validationMessages += std::to_string(position.column);
            validationMessages += " [" + std::to_string(static_cast<int>(level)) + "] ";
            validationMessages += message ? message : "validation error";
            validationMessages += '\n';
        });
    if (!validator.Validate(words))
    {
        std::cout << "SPIRV-Tools rejected Vulkan 1.3 artifact: "
                  << artifactPath << std::endl;
        if (!validationMessages.empty())
            std::cout << validationMessages;
        return false;
    }
    return true;
}

bool CompileShaderImpl(const CompiledShaderArtifact& artifact,
    const std::filesystem::path& filePath,
    const std::vector<std::filesystem::path>& dependencies,
    const std::vector<std::string>& defines)
{
    const std::string& stage = artifact.stage;
    const std::string& entry = artifact.entry;
    if (entry.empty() && stage != "lib")
    {
        std::cout << "Shader entry point is empty: stage=" << stage << ", file: " << filePath << std::endl;
        return false;
    }

    for (const HEngine::ShaderArtifactTarget& target : ArtifactTargets)
    {
        if (artifact.backendOnly && target.Backend != *artifact.backendOnly)
            continue;
        const std::string& profile = artifact.profile;
        const std::filesystem::path outFileName =
            OutputPath / "CACHES" /
            std::string(HEngine::GetShaderArtifactDirectoryName(target.BinaryFormat)) /
            (artifact.artifactStem + "." + stage);
        const std::vector<std::string> compileOptions =
            BuildShaderCompileOptions(target);
        const std::string cacheKey = BuildShaderCacheKey(
            dependencies, defines, entry, profile, compileOptions, target);

        if (!IsShaderCacheCurrent(outFileName, cacheKey))
        {
            std::cout << "Compile HLSL " << HEngine::ToString(target.BinaryFormat)
                      << ": source=\"" << filePath.generic_string()
                      << "\" variant=\"" << artifact.variant
                      << "\" entry=\"" << entry
                      << "\" profile=\"" << profile
                      << "\" backend=\"" << ShaderBackendName(target.Backend) << "\"" << std::endl;

            std::string defineArgs;
            for (const auto& def : defines)
            {
                defineArgs += " -D \"" + NormalizeDefine(def) + "\"";
            }
            std::string includeArgs = " -I \"" + RootPath.string() + "\"";
            for (const std::filesystem::path& includeRoot : AdditionalIncludePaths)
                includeArgs += " -I \"" + includeRoot.string() + "\"";

            const std::string entryArg = entry.empty()
                ? std::string{}
                : " -E \"" + entry + "\"";
            const std::string args = "-nologo -Zpc -HV 2021 -O3" +
                BuildDxcBackendArguments(target) + " -T " + profile +
                entryArg + defineArgs +
                includeArgs +
                " -Fo \"" + outFileName.string() + "\" \"" + filePath.string() + "\"";

#ifdef _WIN32
            const std::filesystem::path dxcPath = std::filesystem::current_path() / "dxc.exe";
            std::string diagnostics;
            if (!RunTool(dxcPath, args, diagnostics))
            {
                std::cout << "Shader compilation failed: source=\"" << filePath.generic_string()
                          << "\" variant=\"" << artifact.variant
                          << "\" entry=\"" << entry
                          << "\" profile=\"" << profile
                          << "\" backend=\"" << ShaderBackendName(target.Backend)
                          << "\" format=\"" << HEngine::ToString(target.BinaryFormat)
                          << "\"" << std::endl;
                if (profile == DefaultTargetProfile(stage))
                {
                    std::cout << "The default baseline profile cannot use post-SM6.0 features. "
                              << "Gate any advanced feature behind an hvariant and declare its "
                              << "profile with '#pragma hprofile " << stage << " <profile> <variant>'."
                              << std::endl;
                }
                return false;
            }
#else
            const std::string cmd = "\"dxc.exe\" " + args;
            if (system(cmd.c_str()) != 0)
            {
                std::cout << "Shader compilation failed: source=\"" << filePath.generic_string()
                          << "\" variant=\"" << artifact.variant
                          << "\" entry=\"" << entry
                          << "\" profile=\"" << profile
                          << "\" backend=\"" << ShaderBackendName(target.Backend)
                          << "\" format=\"" << HEngine::ToString(target.BinaryFormat)
                          << "\"" << std::endl;
                return false;
            }
#endif
            std::error_code ec;
            if (!std::filesystem::is_regular_file(outFileName, ec) || ec)
            {
                std::cout << "DXC reported success but did not create output: " << outFileName << std::endl;
                return false;
            }
#ifdef _WIN32
            if (target.BinaryFormat == HEngine::ShaderBinaryFormat::Dxil &&
                !ValidateDxilRequirements(dxcPath, outFileName, filePath, artifact))
            {
                std::filesystem::remove(outFileName, ec);
                return false;
            }
#endif
            if (target.BinaryFormat == HEngine::ShaderBinaryFormat::SpirV &&
                !ValidateSpirVArtifact(outFileName))
            {
                std::filesystem::remove(outFileName, ec);
                return false;
            }
            if (!WriteShaderCacheKey(outFileName, cacheKey))
                return false;
        }
    }

    return true;
}

bool CompileShader(const std::filesystem::path& shaderPath)
{
    const std::filesystem::path normalizedShaderPath = NormalizePath(shaderPath);
    if (!IsHShader(normalizedShaderPath))
        return true;

    const std::string source = ReadFile(normalizedShaderPath);
    const bool bMaterialCandidate = source.find("#pragma hmaterial") != std::string::npos;
    const auto validationFailure = [&](std::string code, std::string message)
    {
        if (bMaterialCandidate)
        {
            EmitMaterialDiagnostics({ MakeMaterialDiagnostic(
                std::move(code), std::move(message), normalizedShaderPath.generic_string()) });
        }
        else
        {
            std::cout << message << std::endl;
        }
        return false;
    };
    ShaderParseResult parseResult;
    try
    {
        parseResult = PreParse(source, normalizedShaderPath.stem().string());
    }
    catch (const std::exception& exception)
    {
        if (bMaterialCandidate)
        {
            EmitMaterialDiagnostics({ MakeMaterialDiagnostic(
                "Material.CompilerPragma", exception.what(), normalizedShaderPath.generic_string()) });
            return false;
        }
        throw;
    }
    const auto isIdentifier = [](std::string_view value)
    {
        if (value.empty() ||
            (!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_'))
            return false;
        return std::all_of(value.begin() + 1, value.end(), [](unsigned char character)
        {
            return std::isalnum(character) != 0 || character == '_';
        });
    };
    const std::string shaderName = normalizedShaderPath.stem().string();
    if (shaderName.empty() ||
        !std::all_of(shaderName.begin(), shaderName.end(), [](unsigned char character)
        {
            return std::isalnum(character) != 0 || character == '_' || character == '-';
        }))
    {
        return validationFailure(
            "Shader.ArtifactName",
            "Shader filename is not a safe cache-artifact name: " + normalizedShaderPath.generic_string());
    }
    std::vector<std::filesystem::path> dependencies;
    try
    {
        dependencies = CollectShaderDependencies(normalizedShaderPath);
    }
    catch (const std::exception& exception)
    {
        if (bMaterialCandidate)
        {
            EmitMaterialDiagnostics({ MakeMaterialDiagnostic(
                "Material.Include", exception.what(), normalizedShaderPath.generic_string()) });
            return false;
        }
        throw;
    }
    MaterialPragmaParseResult materialParse = ParseMaterialPragmas(
        normalizedShaderPath, dependencies, BuildRecursiveSourceHash(dependencies));
    if (materialParse.bIsMaterialShader)
    {
        const std::vector<CompiledShaderArtifact> materialArtifacts =
            BuildMaterialShaderArtifacts(shaderName, materialParse.manifest);
        return CompileMaterialShader(
            normalizedShaderPath, std::move(materialParse), materialArtifacts, dependencies);
    }

    std::set<std::string> declaredStages;
    for (const ShaderCompileDesc& stage : parseResult.stages)
    {
        if (!IsSupportedShaderStage(stage.type) ||
            (stage.type != "lib" && !isIdentifier(stage.entry)) ||
            (stage.type == "lib" && !stage.entry.empty() && !isIdentifier(stage.entry)))
        {
            return validationFailure(
                "Shader.HType",
                "Shader #pragma htype requires vs/ps/cs with an identifier entry, or lib with an optional identifier entry: " +
                    normalizedShaderPath.generic_string());
        }
        if (!declaredStages.insert(stage.type).second)
        {
            return validationFailure(
                "Shader.DuplicateStage",
                "Shader declares the same output stage more than once: source=\"" +
                    normalizedShaderPath.generic_string() + "\" stage=\"" + stage.type + "\"");
        }
    }
    if (parseResult.stages.empty())
    {
        return validationFailure(
            "Shader.MissingHType",
            "Shader does not declare any #pragma htype stages: " + normalizedShaderPath.generic_string());
    }
    std::set<std::string> declaredVariants;
    for (const std::string& variant : parseResult.variantDefines)
    {
        if (!isIdentifier(variant) || ToLowerCopy(variant) == "base")
        {
            return validationFailure(
                "Shader.HVariant",
                "Shader #pragma hvariant requires a non-BASE identifier macro: " +
                    normalizedShaderPath.generic_string());
        }
        if (!declaredVariants.insert(variant).second)
        {
            return validationFailure(
                "Shader.DuplicateVariant",
                "Shader declares the same hvariant more than once: source=\"" +
                    normalizedShaderPath.generic_string() + "\" variant=\"" + variant + "\"");
        }
    }

    std::set<std::string> profileRuleKeys;
    for (const ShaderProfileRule& rule : parseResult.profileRules)
    {
        std::string profileStage;
        unsigned int profileMajor = 0;
        unsigned int profileMinor = 0;
        if (declaredStages.find(rule.stage) == declaredStages.end() ||
            !TryParseTargetProfile(
                rule.profile, profileStage, profileMajor, profileMinor) ||
            profileStage != rule.stage || profileMajor != 6)
        {
            return validationFailure(
                "Shader.HProfile",
                "Invalid shader profile rule: source=\"" + normalizedShaderPath.generic_string() +
                    "\" stage=\"" + rule.stage + "\" profile=\"" + rule.profile +
                    "\". The stage must be declared and match a Shader Model 6 target.");
        }
        if (!rule.variant.empty() && rule.variant != "BASE" &&
            declaredVariants.find(rule.variant) == declaredVariants.end())
        {
            return validationFailure(
                "Shader.HProfileVariant",
                "Shader profile rule references an undeclared variant: source=\"" +
                    normalizedShaderPath.generic_string() + "\" stage=\"" + rule.stage +
                    "\" variant=\"" + rule.variant + "\"");
        }
        const std::string ruleKey = rule.stage + "\n" + rule.variant;
        if (!profileRuleKeys.insert(ruleKey).second)
        {
            return validationFailure(
                "Shader.DuplicateProfile",
                "Shader declares conflicting profile rules for the same stage/variant: source=\"" +
                    normalizedShaderPath.generic_string() + "\" stage=\"" + rule.stage +
                    "\" variant=\"" + (rule.variant.empty() ? "*" : rule.variant) + "\"");
        }
    }

    const std::vector<CompiledShaderArtifact> artifacts =
        BuildShaderArtifacts(shaderName, parseResult);
    if (!ClaimShaderArtifacts(normalizedShaderPath, artifacts))
        return false;
    bool succeeded = true;

    for (const CompiledShaderArtifact& artifact : artifacts)
    {
        std::vector<std::string> defines;
        if (artifact.variant != "BASE")
            defines.push_back(artifact.variant);
        if (!CompileShaderImpl(
            artifact, normalizedShaderPath, dependencies, defines))
        {
            succeeded = false;
        }
    }

    if (!succeeded)
        return false;

    return WriteShaderMeta(shaderName, parseResult, artifacts);
}

static bool HasDiagnosticCode(
    const std::vector<HEngine::MaterialShaderDiagnostic>& diagnostics,
    std::string_view code)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(), [code](const auto& diagnostic)
    {
        return diagnostic.Code == code;
    });
}

static std::size_t CountDiagnosticCode(
    const std::vector<HEngine::MaterialShaderDiagnostic>& diagnostics,
    std::string_view code)
{
    return static_cast<std::size_t>(std::count_if(
        diagnostics.begin(), diagnostics.end(), [code](const auto& diagnostic)
        {
            return diagnostic.Code == code;
        }));
}

static bool WriteSelfTestText(const std::filesystem::path& path, std::string_view text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return false;
    std::ofstream output(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output)
        return false;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

static std::optional<std::filesystem::path> FindSelfTestEngineShaderRoot()
{
    std::filesystem::path current = NormalizePath(std::filesystem::current_path());
    for (int level = 0; level < 10; ++level)
    {
        const std::filesystem::path candidate =
            current / "Engine" / "Content" / "Shaders";
        std::error_code ec;
        if (std::filesystem::is_regular_file(
                candidate / "Modern" / "MaterialSurface.hlsli", ec) && !ec)
        {
            return NormalizePath(candidate);
        }
        if (!current.has_parent_path() || current.parent_path() == current)
            break;
        current = current.parent_path();
    }
    return std::nullopt;
}

static bool ValidateMaterialPermutationSet(
    const HEngine::MaterialShaderManifest& manifest,
    bool bExpectMaskPasses)
{
    const std::vector<std::string> forward = {
        "FORWARD_CPU_STATIC_BASELINE",
        "FORWARD_CPU_STATIC_HEAP_DIRECT",
        "FORWARD_SKINNED_BASELINE",
        "FORWARD_SKINNED_HEAP_DIRECT",
    };
    const std::vector<std::string> mask = {
        "DEPTH_CPU_STATIC_BASELINE",
        "DEPTH_CPU_STATIC_HEAP_DIRECT",
        "DEPTH_SKINNED_BASELINE",
        "DEPTH_SKINNED_HEAP_DIRECT",
        "SHADOW_CPU_STATIC_BASELINE",
        "SHADOW_CPU_STATIC_HEAP_DIRECT",
        "SHADOW_SKINNED_BASELINE",
        "SHADOW_SKINNED_HEAP_DIRECT",
        "OUTLINE_CPU_STATIC_BASELINE",
        "OUTLINE_CPU_STATIC_HEAP_DIRECT",
        "OUTLINE_SKINNED_BASELINE",
        "OUTLINE_SKINNED_HEAP_DIRECT",
    };
    std::vector<std::string> expected = forward;
    if (bExpectMaskPasses)
        expected.insert(expected.end(), mask.begin(), mask.end());
    if (manifest.Artifacts.size() != expected.size() * 2)
        return false;

    for (const std::string& permutation : expected)
    {
        const HEngine::MaterialShaderPass expectedPass = permutation.starts_with("DEPTH_")
            ? HEngine::MaterialShaderPass::Depth
            : permutation.starts_with("SHADOW_")
                ? HEngine::MaterialShaderPass::Shadow
                : permutation.starts_with("OUTLINE_")
                    ? HEngine::MaterialShaderPass::Outline
                    : HEngine::MaterialShaderPass::Forward;
        const HEngine::MaterialShaderDescriptorMode expectedDescriptor =
            permutation.ends_with("HEAP_DIRECT")
                ? HEngine::MaterialShaderDescriptorMode::HeapDirect
                : HEngine::MaterialShaderDescriptorMode::Baseline;
        const HEngine::MaterialShaderVertexPath expectedVertex =
            permutation.find("SKINNED") != std::string::npos
                ? HEngine::MaterialShaderVertexPath::Skinned
                : HEngine::MaterialShaderVertexPath::Static;
        for (const std::string_view stage : { std::string_view("vs"), std::string_view("ps") })
        {
            const auto artifact = std::find_if(
                manifest.Artifacts.begin(), manifest.Artifacts.end(),
                [&](const HEngine::MaterialShaderArtifact& candidate)
                {
                    return candidate.Permutation == permutation && candidate.Stage == stage;
                });
            if (artifact == manifest.Artifacts.end() ||
                artifact->Pass != expectedPass ||
                artifact->DescriptorMode != expectedDescriptor ||
                artifact->VertexPath != expectedVertex ||
                artifact->EntryPoint != (stage == "vs" ? "HEMaterialVS" : "HEMaterialPS") ||
                artifact->TargetProfile != (stage == "vs"
                    ? "vs_6_0"
                    : expectedDescriptor == HEngine::MaterialShaderDescriptorMode::HeapDirect
                        ? "ps_6_6"
                        : "ps_6_0"))
            {
                return false;
            }
        }
    }

    const std::span<const HEngine::MaterialShaderArtifact> vulkanArtifacts =
        HEngine::GetMaterialShaderArtifacts(
            manifest,
            HEngine::RenderBackend::Vulkan,
            HEngine::ShaderBinaryFormat::SpirV);
    if (!vulkanArtifacts.empty())
    {
        const std::size_t expectedVulkanCount = bExpectMaskPasses ? 16u : 4u;
        if (vulkanArtifacts.size() != expectedVulkanCount ||
            std::ranges::any_of(vulkanArtifacts, [](const auto& artifact)
            {
                return artifact.DescriptorMode !=
                    HEngine::MaterialShaderDescriptorMode::Baseline ||
                    !artifact.RelativePath.starts_with("SPIRV/");
            }))
        {
            return false;
        }
    }
    return true;
}

static bool RunMaterialManifestSelfTests()
{
    struct GlobalRestore final
    {
        std::filesystem::path PreviousRoot = RootPath;
        std::filesystem::path PreviousOutput = OutputPath;
        std::vector<std::filesystem::path> PreviousIncludes = AdditionalIncludePaths;

        ~GlobalRestore()
        {
            RootPath = std::move(PreviousRoot);
            OutputPath = std::move(PreviousOutput);
            AdditionalIncludePaths = std::move(PreviousIncludes);
        }
    } restore;

    const std::filesystem::path temporaryRoot =
        HEngine::PlatformFileSystem::MakeTemporarySiblingPath(
            std::filesystem::temp_directory_path() / "HEngine-HShaderCompiler-SelfTest");
    struct TemporaryCleanup final
    {
        std::filesystem::path Path;
        ~TemporaryCleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(Path, ec);
        }
    } cleanup{ temporaryRoot };
    std::error_code ec;
    std::filesystem::create_directories(temporaryRoot, ec);
    if (ec)
    {
        std::cout << "HShaderCompiler self-test failed: cannot create material fixture root" << std::endl;
        return false;
    }
    RootPath = NormalizePath(temporaryRoot);
    OutputPath = temporaryRoot / "Output";
    const std::optional<std::filesystem::path> engineShaderRoot =
        FindSelfTestEngineShaderRoot();
    if (!engineShaderRoot)
    {
        std::cout << "HShaderCompiler self-test failed: cannot locate engine MaterialSurface include" << std::endl;
        return false;
    }
    AdditionalIncludePaths = { *engineShaderRoot };

    const std::filesystem::path includePath = temporaryRoot / "Include" / "MaterialShared.hlsli";
    const std::filesystem::path shaderPath = temporaryRoot / "Materials" / "Fresnel.hlsl";
    const std::string validInclude =
        "// Policy words in comments are inert: register(t0), cbuffer, SamplerState, RWTexture2D, StructuredBuffer, HEMaterialVS.\n"
        "#pragma property g_Roughness \"Roughness\" float default(0.5) range(0.04, 1.0) group(\"Surface\")\n";
    const std::string validSource =
        "#include \"Modern/MaterialSurface.hlsli\"\n"
        "#include \"Include/MaterialShared.hlsli\"\n"
        "#pragma hmaterial Surface Lit\n"
        "#pragma hblend Opaque\n"
        "#pragma property g_RimColor \"Rim Color\" color default(1, 0.2, 0.05, 1) group(\"Rim\") id(\"rim.color\")\n"
        "#pragma property g_RimPower \"Rim Power\" float default(4.0) range(0.25, 16.0) group(\"Rim\")\n"
        "void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)\n"
        "{\n"
        "    surface.PerceptualRoughness = g_Roughness;\n"
        "    const float fresnel = pow(1.0 - saturate(dot(input.NormalWS, input.ViewWS)), g_RimPower);\n"
        "    surface.Emissive += g_RimColor.rgb * fresnel;\n"
        "}\n";
    if (!WriteSelfTestText(includePath, validInclude) ||
        !WriteSelfTestText(shaderPath, validSource))
    {
        std::cout << "HShaderCompiler self-test failed: cannot write material fixtures" << std::endl;
        return false;
    }

    const std::vector<std::filesystem::path> validDependencies =
        CollectShaderDependencies(shaderPath);
    const std::string firstRecursiveHash = BuildRecursiveSourceHash(validDependencies);
    MaterialPragmaParseResult valid = ParseMaterialPragmas(
        shaderPath, validDependencies, firstRecursiveHash);
    if (!valid.bIsMaterialShader || !valid.Succeeded() ||
        valid.manifest.Properties.size() != 3 ||
        valid.manifest.ParameterBlock.ByteSize != 48 ||
        valid.manifest.BindingSignatureHash !=
            HEngine::BuildMaterialShaderBindingSignatureHash(valid.manifest))
    {
        EmitMaterialDiagnostics(valid.diagnostics);
        std::cout << "HShaderCompiler self-test failed: valid material manifest parsing/layout" << std::endl;
        return false;
    }

    const std::vector<CompiledShaderArtifact> validArtifacts =
        BuildMaterialShaderArtifacts("Fresnel", valid.manifest);
    if (validArtifacts.size() != 8 || !CompileShader(shaderPath))
    {
        std::cout << "HShaderCompiler self-test failed: material artifact compilation/publication" << std::endl;
        return false;
    }
    const std::filesystem::path publishedManifestPath =
        OutputPath / "CACHES" / "Materials" / "Materials" / "Fresnel.hlsl.material.json";
    const HEngine::MaterialShaderManifestLoadResult publishedManifest =
        HEngine::LoadMaterialShaderManifest(publishedManifestPath);
    if (!publishedManifest.Succeeded() ||
        !ValidateMaterialPermutationSet(publishedManifest.Manifest, false))
    {
        EmitMaterialDiagnostics(publishedManifest.Diagnostics);
        std::cout << "HShaderCompiler self-test failed: published material manifest validation" << std::endl;
        return false;
    }
    HEngine::MaterialShaderManifest incompleteManifest = publishedManifest.Manifest;
    incompleteManifest.Artifacts.pop_back();
    const std::vector<HEngine::MaterialShaderDiagnostic> incompleteDiagnostics =
        HEngine::ValidateMaterialShaderManifest(incompleteManifest, "self-test-incomplete");
    if (!HasDiagnosticCode(incompleteDiagnostics, "Manifest.ArtifactMatrix"))
    {
        std::cout << "HShaderCompiler self-test failed: shared typed artifact completeness validation"
                  << std::endl;
        return false;
    }
    HEngine::MaterialShaderManifest staleSignatureManifest = publishedManifest.Manifest;
    ++staleSignatureManifest.Properties.front().ByteOffset;
    const std::vector<HEngine::MaterialShaderDiagnostic> staleSignatureDiagnostics =
        HEngine::ValidateMaterialShaderManifest(staleSignatureManifest, "self-test-signature");
    if (!HasDiagnosticCode(staleSignatureDiagnostics, "Manifest.BindingSignature"))
    {
        std::cout << "HShaderCompiler self-test failed: shared binding signature validation"
                  << std::endl;
        return false;
    }

    const auto expectManifestDiagnostics = [](
        const HEngine::MaterialShaderManifest& manifest,
        std::string_view source,
        std::initializer_list<std::string_view> expectedCodes)
    {
        const std::vector<HEngine::MaterialShaderDiagnostic> diagnostics =
            HEngine::ValidateMaterialShaderManifest(manifest, std::string(source));
        for (const std::string_view code : expectedCodes)
        {
            if (!HasDiagnosticCode(diagnostics, code))
            {
                EmitMaterialDiagnostics(diagnostics);
                std::cout << "HShaderCompiler self-test failed: expected shared manifest diagnostic "
                          << code << " for " << source << std::endl;
                return false;
            }
        }
        return true;
    };
    const auto makeProperty = [](
        std::size_t index,
        HEngine::MaterialShaderPropertyType type)
    {
        HEngine::MaterialShaderProperty property;
        property.StableId = "self-test.property." + std::to_string(index);
        property.Name = "g_SelfTestProperty" + std::to_string(index);
        property.DisplayName = "Self Test Property " + std::to_string(index);
        property.Group = "SelfTest";
        property.Source = "Materials/SelfTest.hlsl";
        property.Line = static_cast<std::uint32_t>(index + 1);
        property.Column = 1;
        property.Type = type;
        property.RowOffset = static_cast<std::uint32_t>(index);
        property.ByteOffset = property.RowOffset * HEngine::MaterialShaderParameterAlignment;
        property.ByteSize = HEngine::MaterialShaderPropertyByteSize(type);
        if (type == HEngine::MaterialShaderPropertyType::Float)
        {
            property.Default.Numeric = { 0.0 };
        }
        else if (type == HEngine::MaterialShaderPropertyType::Texture2D)
        {
            property.Default.Texture = "white";
            property.TextureSemantic = HEngine::MaterialShaderTextureSemantic::Data;
            property.Fallback = "white";
            property.Binding.RegisterType = "ParameterRow";
            property.Binding.DescriptorIndex = static_cast<std::int32_t>(property.RowOffset);
        }
        return property;
    };
    const auto replaceProperties = [&](
        HEngine::MaterialShaderManifest manifest,
        std::vector<HEngine::MaterialShaderProperty> properties)
    {
        manifest.Properties = std::move(properties);
        manifest.ParameterBlock.ByteSize = static_cast<std::uint32_t>(
            manifest.Properties.size() * HEngine::MaterialShaderParameterAlignment);
        manifest.BindingSignatureHash =
            HEngine::BuildMaterialShaderBindingSignatureHash(manifest);
        return manifest;
    };

    std::vector<HEngine::MaterialShaderProperty> propertyLimitValues;
    propertyLimitValues.reserve(65);
    for (std::size_t index = 0; index < 65; ++index)
    {
        propertyLimitValues.push_back(
            makeProperty(index, HEngine::MaterialShaderPropertyType::Float));
    }
    const HEngine::MaterialShaderManifest propertyLimitManifest = replaceProperties(
        publishedManifest.Manifest, std::move(propertyLimitValues));
    if (!expectManifestDiagnostics(
            propertyLimitManifest,
            "self-test-property-limit",
            { "Manifest.PropertyLimit", "Manifest.ParameterBlockLimit" }))
    {
        return false;
    }

    std::vector<HEngine::MaterialShaderProperty> textureLimitValues;
    textureLimitValues.reserve(17);
    for (std::size_t index = 0; index < 17; ++index)
    {
        textureLimitValues.push_back(
            makeProperty(index, HEngine::MaterialShaderPropertyType::Texture2D));
    }
    const HEngine::MaterialShaderManifest textureLimitManifest = replaceProperties(
        publishedManifest.Manifest, std::move(textureLimitValues));
    if (!expectManifestDiagnostics(
            textureLimitManifest, "self-test-texture-limit", { "Manifest.TextureLimit" }))
    {
        return false;
    }

    HEngine::MaterialShaderProperty floatDefault =
        makeProperty(0, HEngine::MaterialShaderPropertyType::Float);
    floatDefault.Default.Numeric = { std::numeric_limits<double>::max() };
    const HEngine::MaterialShaderManifest floatDefaultManifest = replaceProperties(
        publishedManifest.Manifest, { std::move(floatDefault) });
    if (!expectManifestDiagnostics(
            floatDefaultManifest, "self-test-float-default", { "Manifest.PropertyDefault" }))
    {
        return false;
    }

    HEngine::MaterialShaderProperty intDefault =
        makeProperty(0, HEngine::MaterialShaderPropertyType::Int);
    intDefault.Default.Integer = std::numeric_limits<std::int64_t>::max();
    const HEngine::MaterialShaderManifest intDefaultManifest = replaceProperties(
        publishedManifest.Manifest, { std::move(intDefault) });
    if (!expectManifestDiagnostics(
            intDefaultManifest, "self-test-int-default", { "Manifest.PropertyDefault" }))
    {
        return false;
    }

    HEngine::MaterialShaderProperty rangedDefault =
        makeProperty(0, HEngine::MaterialShaderPropertyType::Float);
    rangedDefault.Default.Numeric = { 2.0 };
    rangedDefault.Range.bEnabled = true;
    rangedDefault.Range.Minimum = 0.0;
    rangedDefault.Range.Maximum = 1.0;
    const HEngine::MaterialShaderManifest rangedDefaultManifest = replaceProperties(
        publishedManifest.Manifest, { std::move(rangedDefault) });
    if (!expectManifestDiagnostics(
            rangedDefaultManifest, "self-test-default-range", { "Manifest.DefaultRange" }))
    {
        return false;
    }

    HEngine::MaterialShaderProperty roundedRangeDefault =
        makeProperty(0, HEngine::MaterialShaderPropertyType::Float);
    roundedRangeDefault.Default.Numeric = { 1.00000007 };
    roundedRangeDefault.Range.bEnabled = true;
    roundedRangeDefault.Range.Minimum = 1.00000008;
    roundedRangeDefault.Range.Maximum = 2.0;
    const HEngine::MaterialShaderManifest roundedRangeManifest = replaceProperties(
        publishedManifest.Manifest, { std::move(roundedRangeDefault) });
    const std::vector<HEngine::MaterialShaderDiagnostic> roundedRangeDiagnostics =
        HEngine::ValidateMaterialShaderManifest(
            roundedRangeManifest, "self-test-rounded-default-range");
    if (!roundedRangeDiagnostics.empty())
    {
        EmitMaterialDiagnostics(roundedRangeDiagnostics);
        std::cout << "HShaderCompiler self-test failed: float ABI-equivalent range values"
                  << std::endl;
        return false;
    }

    HEngine::MaterialShaderProperty invalidFallback =
        makeProperty(0, HEngine::MaterialShaderPropertyType::Texture2D);
    invalidFallback.Fallback = "checker";
    const HEngine::MaterialShaderManifest invalidFallbackManifest = replaceProperties(
        publishedManifest.Manifest, { std::move(invalidFallback) });
    if (!expectManifestDiagnostics(
            invalidFallbackManifest, "self-test-texture-fallback", { "Manifest.TextureFallback" }))
    {
        return false;
    }

    HEngine::MaterialShaderProperty contentTextureDefault =
        makeProperty(0, HEngine::MaterialShaderPropertyType::Texture2D);
    contentTextureDefault.Default.Texture = "Assets/Textures/Surface.png";
    const HEngine::MaterialShaderManifest contentTextureDefaultManifest = replaceProperties(
        publishedManifest.Manifest, { std::move(contentTextureDefault) });
    const std::vector<HEngine::MaterialShaderDiagnostic> contentTextureDiagnostics =
        HEngine::ValidateMaterialShaderManifest(
            contentTextureDefaultManifest, "self-test-content-texture-default");
    if (!contentTextureDiagnostics.empty())
    {
        EmitMaterialDiagnostics(contentTextureDiagnostics);
        std::cout << "HShaderCompiler self-test failed: safe content texture default"
                  << std::endl;
        return false;
    }

    const std::vector<std::string> unsafeTextureDefaults = {
        "../outside.png",
        "C:/outside.png",
        "Assets/Textures/../../outside.png",
        "Assets/Textures/not-a-texture.txt",
        "Assets/CON/texture.png",
        "Assets/Textures/invalid?.png",
        "Textures/not-content-relative.png",
        std::string("Assets/Textures/embedded.png") + '\0' + "outside",
    };
    for (const std::string& unsafeDefault : unsafeTextureDefaults)
    {
        HEngine::MaterialShaderProperty invalidDefault =
            makeProperty(0, HEngine::MaterialShaderPropertyType::Texture2D);
        invalidDefault.Default.Texture = std::string(unsafeDefault);
        const HEngine::MaterialShaderManifest invalidDefaultManifest = replaceProperties(
            publishedManifest.Manifest, { std::move(invalidDefault) });
        if (!expectManifestDiagnostics(
                invalidDefaultManifest,
                "self-test-texture-default-path",
                { "Manifest.TextureDefault" }))
        {
            return false;
        }
    }

    HEngine::MaterialShaderManifest opaqueDepthWriteManifest = publishedManifest.Manifest;
    opaqueDepthWriteManifest.bDepthWrite = false;
    if (!expectManifestDiagnostics(
            opaqueDepthWriteManifest, "self-test-opaque-depth-write", { "Manifest.RenderState" }))
    {
        return false;
    }
    HEngine::MaterialShaderManifest transparentDepthWriteManifest = publishedManifest.Manifest;
    transparentDepthWriteManifest.BlendMode = HEngine::MaterialShaderBlendMode::Transparent;
    transparentDepthWriteManifest.bDepthWrite = true;
    transparentDepthWriteManifest.bCastsShadow = false;
    if (!expectManifestDiagnostics(
            transparentDepthWriteManifest,
            "self-test-transparent-depth-write",
            { "Manifest.RenderState" }))
    {
        return false;
    }
    HEngine::MaterialShaderManifest transparentShadowManifest = publishedManifest.Manifest;
    transparentShadowManifest.BlendMode = HEngine::MaterialShaderBlendMode::Transparent;
    transparentShadowManifest.bDepthWrite = false;
    transparentShadowManifest.bCastsShadow = true;
    if (!expectManifestDiagnostics(
            transparentShadowManifest,
            "self-test-transparent-shadow",
            { "Manifest.RenderState" }))
    {
        return false;
    }

    const std::filesystem::path checkerPath =
        temporaryRoot / "Materials" / "Checker.hlsl";
    const std::string checkerSource =
        "#include \"Modern/MaterialSurface.hlsli\"\n"
        "#pragma hmaterial Surface Unlit\n"
        "#pragma hblend Mask\n"
        "#pragma property g_CheckerScale \"Checker Scale\" float default(8.0) range(1.0, 64.0)\n"
        "#pragma property g_ColorA \"Color A\" color default(1, 1, 1, 1)\n"
        "#pragma property g_ColorB \"Color B\" color default(0.05, 0.05, 0.05, 1)\n"
        "#pragma property g_Map \"Surface Map\" texture2d default(white) semantic(BaseColor) colorspace(SRGB) fallback(white)\n"
        "void EvaluateMaterial(in HEMaterialInput input, inout HEMaterialSurface surface)\n"
        "{\n"
        "    const float2 cell = floor(input.UV0 * g_CheckerScale);\n"
        "    const float checker = fmod(cell.x + cell.y, 2.0);\n"
        "    const float4 texel = g_Map.Sample(HE_MATERIAL_DEFAULT_SAMPLER, input.UV0);\n"
        "    surface.BaseColor = texel.rgb * lerp(g_ColorA.rgb, g_ColorB.rgb, checker);\n"
        "    surface.Emissive = surface.BaseColor * 0.05;\n"
        "    surface.Opacity = texel.a * lerp(1.0, 0.25, checker);\n"
        "    surface.AlphaCutoff = 0.5;\n"
        "}\n";
    if (!WriteSelfTestText(checkerPath, checkerSource))
        return false;
    const std::vector<std::filesystem::path> checkerDependencies =
        CollectShaderDependencies(checkerPath);
    MaterialPragmaParseResult checker = ParseMaterialPragmas(
        checkerPath,
        checkerDependencies,
        BuildRecursiveSourceHash(checkerDependencies));
    if (!checker.Succeeded() || checker.manifest.Properties.size() != 4 ||
        checker.manifest.Properties[3].Binding.RegisterType != "ParameterRow" ||
        checker.manifest.Properties[3].Binding.DescriptorIndex != 3)
    {
        EmitMaterialDiagnostics(checker.diagnostics);
        std::cout << "HShaderCompiler self-test failed: checker texture property layout" << std::endl;
        return false;
    }
    const std::vector<CompiledShaderArtifact> checkerArtifacts =
        BuildMaterialShaderArtifacts("Checker", checker.manifest);
    if (checkerArtifacts.size() != 32 || !CompileShader(checkerPath))
    {
        std::cout << "HShaderCompiler self-test failed: checker wrapper permutations" << std::endl;
        return false;
    }
    const HEngine::MaterialShaderManifestLoadResult checkerManifest =
        HEngine::LoadMaterialShaderManifest(
            OutputPath / "CACHES" / "Materials" / "Materials" /
                "Checker.hlsl.material.json");
    if (!checkerManifest.Succeeded() ||
        !ValidateMaterialPermutationSet(checkerManifest.Manifest, true))
    {
        EmitMaterialDiagnostics(checkerManifest.Diagnostics);
        std::cout << "HShaderCompiler self-test failed: checker pass/tier/vertex permutations" << std::endl;
        return false;
    }

    const nlohmann::json manifestJson =
        HEngine::MaterialShaderManifestToJson(checkerManifest.Manifest);
    const HEngine::MaterialShaderManifestLoadResult roundTrip =
        HEngine::ParseMaterialShaderManifest(manifestJson, "self-test");
    if (!roundTrip.Succeeded() || roundTrip.Manifest.Properties.size() != 4 ||
        !ValidateMaterialPermutationSet(roundTrip.Manifest, true))
    {
        EmitMaterialDiagnostics(roundTrip.Diagnostics);
        std::cout << "HShaderCompiler self-test failed: manifest JSON loader/validator round trip" << std::endl;
        return false;
    }

    nlohmann::json versionOneJson = manifestJson;
    versionOneJson["FormatVersion"] = 1;
    for (nlohmann::json& propertyJson : versionOneJson["Properties"])
    {
        propertyJson.erase("Source");
        propertyJson.erase("Line");
        propertyJson.erase("Column");
    }
    const HEngine::MaterialShaderManifestLoadResult versionOne =
        HEngine::ParseMaterialShaderManifest(versionOneJson, "self-test-version-one");
    if (versionOne.Succeeded() || versionOne.Diagnostics.size() != 1 ||
        CountDiagnosticCode(versionOne.Diagnostics, "Manifest.FormatVersion") != 1 ||
        CountDiagnosticCode(versionOne.Diagnostics, "Manifest.MissingField") != 0)
    {
        EmitMaterialDiagnostics(versionOne.Diagnostics);
        std::cout << "HShaderCompiler self-test failed: v1 manifest was not rejected early"
                  << std::endl;
        return false;
    }

    nlohmann::json embeddedErrorJson = manifestJson;
    embeddedErrorJson["Diagnostics"] = nlohmann::json::array({ {
        { "Severity", "Error" },
        { "Code", "SelfTest.EmbeddedError" },
        { "Message", "Embedded compiler errors must fail every manifest consumer." },
        { "Source", "Materials/Checker.hlsl" },
        { "Line", 1 },
        { "Column", 1 },
    } });
    const HEngine::MaterialShaderManifestLoadResult embeddedError =
        HEngine::ParseMaterialShaderManifest(embeddedErrorJson, "self-test-embedded-error");
    if (embeddedError.Succeeded() ||
        !HasDiagnosticCode(embeddedError.Diagnostics, "SelfTest.EmbeddedError"))
    {
        std::cout << "HShaderCompiler self-test failed: embedded error diagnostic semantics"
                  << std::endl;
        return false;
    }

    nlohmann::json malformedDiagnosticsJson = manifestJson;
    malformedDiagnosticsJson["Diagnostics"] = nlohmann::json::array({
        "not-an-object",
        nullptr,
        7,
    });
    const HEngine::MaterialShaderManifestLoadResult malformedDiagnostics =
        HEngine::ParseMaterialShaderManifest(
            malformedDiagnosticsJson, "self-test-malformed-diagnostics");
    if (malformedDiagnostics.Succeeded() ||
        CountDiagnosticCode(
            malformedDiagnostics.Diagnostics, "Manifest.DiagnosticType") != 3)
    {
        std::cout << "HShaderCompiler self-test failed: malformed embedded diagnostics accepted"
                  << std::endl;
        return false;
    }

    nlohmann::json invalidSeverityJson = manifestJson;
    invalidSeverityJson["Diagnostics"] = nlohmann::json::array({ {
        { "Severity", "Fatal" },
        { "Code", "SelfTest.PhantomInvalidSeverity" },
        { "Message", "This invalid diagnostic must not be committed." },
        { "Source", "Materials/Checker.hlsl" },
        { "Line", 4 },
        { "Column", 5 },
    } });
    const HEngine::MaterialShaderManifestLoadResult invalidSeverity =
        HEngine::ParseMaterialShaderManifest(
            invalidSeverityJson, "self-test-invalid-diagnostic-severity");
    if (invalidSeverity.Succeeded() || invalidSeverity.Diagnostics.size() != 1 ||
        CountDiagnosticCode(invalidSeverity.Diagnostics, "Manifest.InvalidEnum") != 1 ||
        HasDiagnosticCode(
            invalidSeverity.Diagnostics, "SelfTest.PhantomInvalidSeverity") ||
        !invalidSeverity.Manifest.Diagnostics.empty())
    {
        EmitMaterialDiagnostics(invalidSeverity.Diagnostics);
        std::cout << "HShaderCompiler self-test failed: invalid diagnostic severity leaked a phantom error"
                  << std::endl;
        return false;
    }

    nlohmann::json missingDiagnosticFieldJson = manifestJson;
    missingDiagnosticFieldJson["Diagnostics"] = nlohmann::json::array({ {
        { "Severity", "Warning" },
        { "Code", "SelfTest.PhantomMissingField" },
        { "Source", "Materials/Checker.hlsl" },
        { "Line", 6 },
        { "Column", 7 },
    } });
    const HEngine::MaterialShaderManifestLoadResult missingDiagnosticField =
        HEngine::ParseMaterialShaderManifest(
            missingDiagnosticFieldJson, "self-test-missing-diagnostic-field");
    if (missingDiagnosticField.Succeeded() ||
        missingDiagnosticField.Diagnostics.size() != 1 ||
        CountDiagnosticCode(missingDiagnosticField.Diagnostics, "Manifest.MissingField") != 1 ||
        HasDiagnosticCode(
            missingDiagnosticField.Diagnostics, "SelfTest.PhantomMissingField") ||
        !missingDiagnosticField.Manifest.Diagnostics.empty())
    {
        EmitMaterialDiagnostics(missingDiagnosticField.Diagnostics);
        std::cout << "HShaderCompiler self-test failed: incomplete diagnostic leaked a phantom entry"
                  << std::endl;
        return false;
    }

    nlohmann::json warningBeforeValidationErrorJson = manifestJson;
    warningBeforeValidationErrorJson["DepthWrite"] = false;
    warningBeforeValidationErrorJson["Diagnostics"] = nlohmann::json::array({ {
        { "Severity", "Warning" },
        { "Code", "SelfTest.EmbeddedWarning" },
        { "Message", "An embedded warning must not hide a validation error." },
        { "Source", "Materials/Checker.hlsl" },
        { "Line", 2 },
        { "Column", 3 },
    } });
    const HEngine::MaterialShaderManifestLoadResult warningBeforeValidationError =
        HEngine::ParseMaterialShaderManifest(
            warningBeforeValidationErrorJson, "self-test-error-priority");
    const HEngine::MaterialShaderDiagnostic* firstError =
        HEngine::FindFirstMaterialShaderError(warningBeforeValidationError.Diagnostics);
    const std::string formattedDiagnostics = HEngine::FormatMaterialShaderDiagnostics(
        warningBeforeValidationError.Diagnostics);
    if (warningBeforeValidationError.Succeeded() || firstError == nullptr ||
        firstError->Code != "Manifest.RenderState" ||
        formattedDiagnostics.find("Warning [SelfTest.EmbeddedWarning]") == std::string::npos ||
        formattedDiagnostics.find("Error [Manifest.RenderState]") == std::string::npos)
    {
        std::cout << "HShaderCompiler self-test failed: embedded warning hid validation error"
                  << std::endl;
        return false;
    }

    if (!WriteSelfTestText(includePath,
            "#pragma property g_Roughness \"Roughness\" float default(0.75) range(0.04, 1.0)\n"))
    {
        return false;
    }
    const std::string secondRecursiveHash = BuildRecursiveSourceHash(
        CollectShaderDependencies(shaderPath));
    if (firstRecursiveHash == secondRecursiveHash)
    {
        std::cout << "HShaderCompiler self-test failed: include content did not change recursive hash" << std::endl;
        return false;
    }
    std::vector<HEngine::MaterialShaderDiagnostic> dependencyChangeDiagnostics;
    if (ValidateMaterialDependenciesUnchanged(
            shaderPath, firstRecursiveHash, dependencyChangeDiagnostics) ||
        !HasDiagnosticCode(
            dependencyChangeDiagnostics, "Material.DependencyChangedDuringCompile"))
    {
        std::cout << "HShaderCompiler self-test failed: pre-publication dependency mutation gate"
                  << std::endl;
        return false;
    }

    const std::vector<std::filesystem::path> leaseDependencies =
        CollectShaderDependencies(shaderPath);
    const std::string leaseRecursiveHash =
        BuildRecursiveSourceHash(leaseDependencies);
    const std::string leasedIncludeText = ReadFile(includePath);
    HEngine::PlatformStableFileReadLease dependencyLease;
    std::error_code leaseError;
    if (!HEngine::PlatformFileSystem::AcquireStableFileReadLease(
            leaseDependencies, dependencyLease, leaseError) ||
        !dependencyLease.IsValid())
    {
        std::cout << "HShaderCompiler self-test failed: cannot acquire dependency publication lease: "
                  << leaseError.message() << std::endl;
        return false;
    }

    std::future<bool> inPlaceMutation = std::async(
        std::launch::async,
        [&includePath]()
        {
            return WriteSelfTestText(
                includePath,
                "#pragma property g_Roughness \"Roughness\" float default(0.1)\n");
        });
    if (inPlaceMutation.get() || ReadFile(includePath) != leasedIncludeText)
    {
        std::cout << "HShaderCompiler self-test failed: publication lease allowed concurrent in-place mutation"
                  << std::endl;
        return false;
    }

    const std::filesystem::path replacementPath =
        HEngine::PlatformFileSystem::MakeTemporarySiblingPath(includePath);
    if (!WriteSelfTestText(
            replacementPath,
            "#pragma property g_Roughness \"Roughness\" float default(0.2)\n"))
    {
        std::cout << "HShaderCompiler self-test failed: cannot stage dependency replace fixture"
                  << std::endl;
        return false;
    }
    std::error_code replaceError;
    std::future<bool> replaceMutation = std::async(
        std::launch::async,
        [&replacementPath, &includePath, &replaceError]()
        {
            return HEngine::PlatformFileSystem::PublishFileAtomically(
                replacementPath, includePath, true, replaceError);
        });
    if (replaceMutation.get() || !replaceError ||
        ReadFile(includePath) != leasedIncludeText)
    {
        std::cout << "HShaderCompiler self-test failed: publication lease allowed concurrent replace-save"
                  << std::endl;
        return false;
    }
    dependencyLease = HEngine::PlatformStableFileReadLease{};
    std::filesystem::remove(replacementPath, ec);

    const std::filesystem::path leaseManifestPath =
        OutputPath / "CACHES" / "Materials" / "lease-preservation.material.json";
    constexpr std::string_view previousManifestText = "previous-manifest\n";
    constexpr std::string_view candidateManifestText = "candidate-manifest\n";
    if (!WriteSelfTestText(leaseManifestPath, previousManifestText))
    {
        std::cout << "HShaderCompiler self-test failed: cannot create dependency lease manifest fixture"
                  << std::endl;
        return false;
    }

    const std::filesystem::path completedReplacementPath =
        HEngine::PlatformFileSystem::MakeTemporarySiblingPath(includePath);
    std::error_code completedReplaceError;
    if (!WriteSelfTestText(
            completedReplacementPath,
            "#pragma property g_Roughness \"Roughness\" float default(0.3)\n") ||
        !HEngine::PlatformFileSystem::PublishFileAtomically(
            completedReplacementPath, includePath, true, completedReplaceError))
    {
        std::cout << "HShaderCompiler self-test failed: cannot complete dependency replace fixture: "
                  << completedReplaceError.message() << std::endl;
        return false;
    }
    std::vector<HEngine::MaterialShaderDiagnostic> completedReplaceDiagnostics;
    const bool bPublishedAfterCompletedReplace =
        PublishMaterialManifestWithDependencyLease(
            shaderPath,
            leaseDependencies,
            leaseRecursiveHash,
            leaseManifestPath,
            candidateManifestText,
            completedReplaceDiagnostics);
    if (bPublishedAfterCompletedReplace ||
        !HasDiagnosticCode(
            completedReplaceDiagnostics, "Material.DependencyChangedDuringCompile") ||
        ReadFile(leaseManifestPath) != previousManifestText)
    {
        std::cout << "HShaderCompiler self-test failed: completed replace-save was not detected before publication"
                  << std::endl;
        return false;
    }
    if (!WriteSelfTestText(includePath, leasedIncludeText) ||
        BuildRecursiveSourceHash(CollectShaderDependencies(shaderPath)) != leaseRecursiveHash)
    {
        std::cout << "HShaderCompiler self-test failed: cannot restore replaced dependency fixture"
                  << std::endl;
        return false;
    }

    std::fstream concurrentWriter(
        includePath, std::ios::in | std::ios::out | std::ios::binary);
    if (!concurrentWriter)
    {
        std::cout << "HShaderCompiler self-test failed: cannot establish concurrent dependency writer"
                  << std::endl;
        return false;
    }
    std::vector<HEngine::MaterialShaderDiagnostic> leasePublishDiagnostics;
    const bool bPublishedWithConcurrentWriter =
        PublishMaterialManifestWithDependencyLease(
            shaderPath,
            leaseDependencies,
            leaseRecursiveHash,
            leaseManifestPath,
            candidateManifestText,
            leasePublishDiagnostics);
    concurrentWriter.close();
    if (bPublishedWithConcurrentWriter ||
        !HasDiagnosticCode(
            leasePublishDiagnostics, "Material.DependencyChangedDuringCompile") ||
        ReadFile(leaseManifestPath) != previousManifestText)
    {
        std::cout << "HShaderCompiler self-test failed: dependency lease failure replaced the previous manifest"
                  << std::endl;
        return false;
    }

    const auto parseSource = [&](const std::string& fixtureSource)
    {
        WriteSelfTestText(shaderPath, fixtureSource);
        const auto dependencies = CollectShaderDependencies(shaderPath);
        return ParseMaterialPragmas(shaderPath, dependencies, BuildRecursiveSourceHash(dependencies));
    };
    const std::string materialPrefix =
        "#include \"Modern/MaterialSurface.hlsli\"\n"
        "#pragma hmaterial Surface Lit\n"
        "#pragma hblend Opaque\n"
        "#pragma htype ps PS\n";

    const MaterialPragmaParseResult malformedMaterial = parseSource(
        "#include \"Modern/MaterialSurface.hlsli\"\n"
        "#pragma hmaterial Volume Lit\n"
        "#pragma hblend Opaque\n"
        "#pragma htype ps PS\n"
        "float4 PS() : SV_Target { return 1; }\n");
    if (malformedMaterial.Succeeded() || !HasDiagnosticCode(
            malformedMaterial.diagnostics, "Material.HMaterial"))
    {
        std::cout << "HShaderCompiler self-test failed: malformed hmaterial diagnostics" << std::endl;
        return false;
    }

    const MaterialPragmaParseResult malformed = parseSource(
        materialPrefix +
        "#pragma property g_Value \"Value\" float default(not-a-number)\n"
        "float4 PS() : SV_Target { return 1; }\n");
    if (malformed.Succeeded() || !HasDiagnosticCode(
            malformed.diagnostics, "Material.PropertyDefaultNumber"))
    {
        std::cout << "HShaderCompiler self-test failed: malformed default diagnostics" << std::endl;
        return false;
    }

    const MaterialPragmaParseResult duplicate = parseSource(
        materialPrefix +
        "#pragma property g_Value \"Value\" float default(0)\n"
        "#pragma property g_Value \"Value Again\" float default(1)\n"
        "float4 PS() : SV_Target { return 1; }\n");
    if (duplicate.Succeeded() || !HasDiagnosticCode(
            duplicate.diagnostics, "Material.DuplicateProperty"))
    {
        std::cout << "HShaderCompiler self-test failed: duplicate property diagnostics" << std::endl;
        return false;
    }

    const MaterialPragmaParseResult invalidRange = parseSource(
        materialPrefix +
        "#pragma property g_Value \"Value\" float default(0) range(2, 1)\n"
        "float4 PS() : SV_Target { return 1; }\n");
    if (invalidRange.Succeeded() || !HasDiagnosticCode(
            invalidRange.diagnostics, "Material.PropertyRange"))
    {
        std::cout << "HShaderCompiler self-test failed: invalid range diagnostics" << std::endl;
        return false;
    }

    const MaterialPragmaParseResult invalidTextureDefault = parseSource(
        materialPrefix +
        "#pragma property g_Map \"Map\" texture2d default(\"../outside.png\")\n"
        "float4 PS() : SV_Target { return 1; }\n");
    if (invalidTextureDefault.Succeeded() || !HasDiagnosticCode(
            invalidTextureDefault.diagnostics, "Material.PropertyDefaultTexturePath"))
    {
        std::cout << "HShaderCompiler self-test failed: unsafe texture default diagnostics"
                  << std::endl;
        return false;
    }

    const MaterialPragmaParseResult invalidExplicitBinding = parseSource(
        materialPrefix +
        "#pragma property g_Map \"Map\" texture2d default(white)\n"
        "Texture2D g_Map : register(t3, space2);\n"
        "float4 PS() : SV_Target { return 1; }\n");
    if (invalidExplicitBinding.Succeeded() || !HasDiagnosticCode(
            invalidExplicitBinding.diagnostics, "Material.ForbiddenRegister"))
    {
        std::cout << "HShaderCompiler self-test failed: explicit texture register diagnostics" << std::endl;
        return false;
    }

    const MaterialPragmaParseResult missingSurfaceInclude = parseSource(
        "#pragma hmaterial Surface Lit\n"
        "#pragma hblend Opaque\n");
    if (missingSurfaceInclude.Succeeded() || !HasDiagnosticCode(
            missingSurfaceInclude.diagnostics, "Material.SurfaceIncludeMissing"))
    {
        std::cout << "HShaderCompiler self-test failed: missing Surface include diagnostics"
                  << std::endl;
        return false;
    }

    const MaterialPragmaParseResult duplicateSurfaceInclude = parseSource(
        "#include \"Modern/MaterialSurface.hlsli\"\n"
        "#include \"Modern/MaterialSurface.hlsli\"\n"
        "#pragma hmaterial Surface Lit\n"
        "#pragma hblend Opaque\n");
    if (duplicateSurfaceInclude.Succeeded() || !HasDiagnosticCode(
            duplicateSurfaceInclude.diagnostics, "Material.SurfaceIncludeDuplicate"))
    {
        std::cout << "HShaderCompiler self-test failed: duplicate Surface include diagnostics"
                  << std::endl;
        return false;
    }

    const MaterialPragmaParseResult nonCanonicalSurfaceInclude = parseSource(
        "#include \"modern/MaterialSurface.hlsli\"\n"
        "#pragma hmaterial Surface Lit\n"
        "#pragma hblend Opaque\n");
    if (nonCanonicalSurfaceInclude.Succeeded() || !HasDiagnosticCode(
            nonCanonicalSurfaceInclude.diagnostics, "Material.SurfaceIncludeCanonical"))
    {
        std::cout << "HShaderCompiler self-test failed: canonical Surface include diagnostics"
                  << std::endl;
        return false;
    }

    bool bParentTraversalRejected = false;
    try
    {
        (void)ResolveIncludePath(shaderPath, "../Include/MaterialShared.hlsli");
    }
    catch (const std::exception&)
    {
        bParentTraversalRejected = true;
    }
    if (!bParentTraversalRejected)
    {
        std::cout << "HShaderCompiler self-test failed: parent include traversal rejection"
                  << std::endl;
        return false;
    }

    const std::filesystem::path forbiddenIncludePath =
        temporaryRoot / "Include" / "ForbiddenResources.hlsli";
    if (!WriteSelfTestText(
            forbiddenIncludePath,
            "// register(t0), cbuffer, SamplerState, RWTexture2D, StructuredBuffer, HEMaterialPS\n"
            "cbuffer UserConstants { float4 g_UserValue; };\n"
            "SamplerState g_UserSampler;\n"
            "RWTexture2D<float4> g_UserOutput;\n"
            "StructuredBuffer<float4> g_UserInput;\n"
            "Buffer<float4> g_UserBuffer;\n"
            "ByteAddressBuffer g_UserBytes;\n"
            "ConstantBuffer<float4> g_UserConstants;\n"
            "TextureCubeArray<float4> g_UserCubeArray;\n"
            "Texture2D<float4> g_UserTexture : register(t7, space8);\n"
            "float4 HEMaterialPS() : SV_Target0 { return 0.0f; }\n"))
    {
        std::cout << "HShaderCompiler self-test failed: cannot write forbidden dependency fixture"
                  << std::endl;
        return false;
    }
    const MaterialPragmaParseResult forbiddenDependency = parseSource(
        materialPrefix +
        "#include \"Include/ForbiddenResources.hlsli\"\n");
    if (forbiddenDependency.Succeeded() ||
        !HasDiagnosticCode(forbiddenDependency.diagnostics, "Material.ForbiddenRegister") ||
        !HasDiagnosticCode(forbiddenDependency.diagnostics, "Material.ForbiddenCBuffer") ||
        !HasDiagnosticCode(forbiddenDependency.diagnostics, "Material.ForbiddenSampler") ||
        CountDiagnosticCode(forbiddenDependency.diagnostics, "Material.ForbiddenResource") != 7 ||
        !HasDiagnosticCode(forbiddenDependency.diagnostics, "Material.ReservedEntryPoint"))
    {
        EmitMaterialDiagnostics(forbiddenDependency.diagnostics);
        std::cout << "HShaderCompiler self-test failed: recursive dependency Surface ABI policy scan"
                  << std::endl;
        return false;
    }

    const CompiledShaderArtifact identityArtifact{
        .variant = "BASE",
        .stage = "ps",
        .entry = "PS",
        .profile = "ps_6_0",
        .artifactStem = "Same",
    };
    const std::filesystem::path firstIdentity = BuildMaterialArtifactRelativePath(
        "First/Same.hlsl",
        firstRecursiveHash,
        firstRecursiveHash,
        identityArtifact,
        { HEngine::RenderBackend::DX12, HEngine::ShaderBinaryFormat::Dxil });
    const std::filesystem::path secondIdentity = BuildMaterialArtifactRelativePath(
        "Second/Same.hlsl",
        firstRecursiveHash,
        firstRecursiveHash,
        identityArtifact,
        { HEngine::RenderBackend::DX12, HEngine::ShaderBinaryFormat::Dxil });
    if (firstIdentity == secondIdentity)
    {
        std::cout << "HShaderCompiler self-test failed: same-stem material artifact identity collision" << std::endl;
        return false;
    }
	if (firstIdentity.generic_string().size() > 96 ||
		secondIdentity.generic_string().size() > 96)
	{
		std::cout << "HShaderCompiler self-test failed: material artifact identity exceeds the packaged path budget" << std::endl;
		return false;
	}

    return true;
}

static bool RunShaderArtifactSelfTests()
{
#ifndef _WIN32
    std::cout << "HShaderCompiler self-test failed: shader artifact contracts require DXC on Windows"
              << std::endl;
    return false;
#else
    const std::optional<std::filesystem::path> shaderRoot =
        FindSelfTestEngineShaderRoot();
    if (!shaderRoot)
    {
        std::cout << "HShaderCompiler self-test failed: cannot locate shader artifact contract source"
                  << std::endl;
        return false;
    }
    const std::filesystem::path sourcePath =
        *shaderRoot / "Validation" / "ShaderArtifactContract.hlsl";
    std::error_code error;
    if (!std::filesystem::is_regular_file(sourcePath, error) || error)
    {
        std::cout << "HShaderCompiler self-test failed: missing shader artifact contract source: "
                  << sourcePath << std::endl;
        return false;
    }

    const std::filesystem::path temporaryRoot =
        HEngine::PlatformFileSystem::MakeTemporarySiblingPath(
            std::filesystem::temp_directory_path() /
            "HEngine-HShaderCompiler-ArtifactContract");
    struct TemporaryCleanup final
    {
        std::filesystem::path Path;
        ~TemporaryCleanup()
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(Path, cleanupError);
        }
    } cleanup{ temporaryRoot };
    std::filesystem::create_directories(temporaryRoot, error);
    if (error)
    {
        std::cout << "HShaderCompiler self-test failed: cannot create artifact contract directory"
                  << std::endl;
        return false;
    }

    const HEngine::ShaderArtifactTarget dxilTarget{
        HEngine::RenderBackend::DX12,
        HEngine::ShaderBinaryFormat::Dxil,
    };
    const HEngine::ShaderArtifactTarget spirVTarget{
        HEngine::RenderBackend::Vulkan,
        HEngine::ShaderBinaryFormat::SpirV,
    };
    const std::vector<std::string> dxilOptions =
        BuildShaderCompileOptions(dxilTarget);
    const std::vector<std::string> spirVOptions =
        BuildShaderCompileOptions(spirVTarget);
    const auto hasOption = [](const std::vector<std::string>& options, std::string_view option)
    {
        return std::find(options.begin(), options.end(), option) != options.end();
    };
    const bool bHasWaveSizeAssumption = std::ranges::any_of(
        dxilOptions, [](const std::string& option)
        {
            return ToLowerCopy(option).find("wave-size") != std::string::npos ||
                ToLowerCopy(option).find("wave_size") != std::string::npos;
        }) || std::ranges::any_of(
        spirVOptions, [](const std::string& option)
        {
            return ToLowerCopy(option).find("wave-size") != std::string::npos ||
                ToLowerCopy(option).find("wave_size") != std::string::npos;
        });
    if (!hasOption(dxilOptions, "-Zpc") || !hasOption(spirVOptions, "-Zpc") ||
        !hasOption(spirVOptions, "-spirv") ||
        !hasOption(spirVOptions, "-fvk-use-dx-layout") ||
        bHasWaveSizeAssumption)
    {
        std::cout << "HShaderCompiler self-test failed: backend compile option ABI" << std::endl;
        return false;
    }

    const std::string dxilCacheKey = BuildShaderCacheKey(
        {}, {}, {}, "lib_6_3", dxilOptions, dxilTarget);
    const std::string spirVCacheKey = BuildShaderCacheKey(
        {}, {}, {}, "lib_6_3", spirVOptions, spirVTarget);
    if (dxilCacheKey == spirVCacheKey)
    {
        std::cout << "HShaderCompiler self-test failed: backend/binary format cache identity"
                  << std::endl;
        return false;
    }

    const std::filesystem::path dxcPath =
        std::filesystem::current_path() / "dxc.exe";
    const auto compile = [&](const HEngine::ShaderArtifactTarget& target,
                             std::string_view profile,
                             std::string_view entry,
                             const std::filesystem::path& outputPath,
                             const std::filesystem::path& assemblyPath)
    {
        const std::string entryArguments = entry.empty()
            ? std::string{}
            : " -E \"" + std::string(entry) + "\"";
        const std::string assemblyArguments = assemblyPath.empty()
            ? std::string{}
            : " -Fc \"" + assemblyPath.string() + "\"";
        const std::string arguments = "-nologo -Zpc -HV 2021 -O3" +
            BuildDxcBackendArguments(target) + " -T " + std::string(profile) +
            entryArguments + " -I \"" + shaderRoot->string() + "\"" +
            " -Fo \"" + outputPath.string() + "\"" +
            assemblyArguments + " \"" + sourcePath.string() + "\"";
        std::string diagnostics;
        return RunTool(dxcPath, arguments, diagnostics, false);
    };

    const std::filesystem::path dxilPixel = temporaryRoot / "contract.ps.dxil";
    const std::filesystem::path dxilLibrary = temporaryRoot / "contract.lib.dxil";
    const std::filesystem::path spirVPixel = temporaryRoot / "contract.ps.spv";
    const std::filesystem::path spirVLibrary = temporaryRoot / "contract.lib.spv";
    const std::filesystem::path spirVPixelAssembly = temporaryRoot / "contract.ps.spvasm";
    const std::filesystem::path spirVLibraryAssembly = temporaryRoot / "contract.lib.spvasm";
    if (!compile(dxilTarget, "ps_6_0", "PSContract", dxilPixel, {}) ||
        !compile(dxilTarget, "lib_6_3", {}, dxilLibrary, {}) ||
        !compile(spirVTarget, "ps_6_0", "PSContract", spirVPixel, spirVPixelAssembly) ||
        !compile(spirVTarget, "lib_6_3", {}, spirVLibrary, spirVLibraryAssembly))
    {
        std::cout << "HShaderCompiler self-test failed: DXIL/SPIR-V contract compilation"
                  << std::endl;
        return false;
    }

    const CompiledShaderArtifact pixelArtifact{
        .variant = "BASE",
        .stage = "ps",
        .entry = "PSContract",
        .profile = "ps_6_0",
        .artifactStem = "ShaderArtifactContract",
    };
    const CompiledShaderArtifact libraryArtifact{
        .variant = "BASE",
        .stage = "lib",
        .entry = {},
        .profile = "lib_6_3",
        .artifactStem = "ShaderArtifactContract",
    };
    if (!ValidateDxilRequirements(dxcPath, dxilPixel, sourcePath, pixelArtifact) ||
        !ValidateDxilRequirements(dxcPath, dxilLibrary, sourcePath, libraryArtifact) ||
        !ValidateSpirVArtifact(spirVPixel) ||
        !ValidateSpirVArtifact(spirVLibrary))
    {
        std::cout << "HShaderCompiler self-test failed: generated artifact validation"
                  << std::endl;
        return false;
    }

    const std::string pixelAssembly = ReadFile(spirVPixelAssembly);
    const std::string libraryAssembly = ReadFile(spirVLibraryAssembly);
    const std::array<std::string_view, 13> pixelContracts = {
        "OpEntryPoint Fragment %PSContract \"PSContract\"",
        "OpDecorate %gl_FrontFacing BuiltIn FrontFacing",
        "OpName %g_Frame \"g_Frame\"",
        "OpDecorate %type_PushConstant_ContractFrameConstants Block",
        "OpMemberDecorate %type_PushConstant_ContractFrameConstants 0 Offset 0",
        "%g_Frame = OpVariable %_ptr_PushConstant_type_PushConstant_ContractFrameConstants PushConstant",
        "OpDecorate %g_MaterialTextures DescriptorSet 1",
        "OpDecorate %g_MaterialTextures Binding 0",
        "OpDecorate %g_MaterialSamplers DescriptorSet 4",
        "OpDecorate %g_MaterialSamplers Binding 128",
        "MatrixStride 16",
        "RowMajor",
        "OpVectorTimesMatrix",
    };
    const std::array<std::string_view, 18> libraryContracts = {
        "OpCapability RayTracingKHR",
        "\"RayGenerationContract\"",
        "\"MissContract\"",
        "\"ClosestHitContract\"",
        "\"AnyHitContract\"",
        "\"CallableContract\"",
        "BuiltIn InstanceCustomIndexNV",
        "HitAttributeNV",
        "\"barycentrics\"",
        "OpDecorate %g_Scene DescriptorSet 2",
        "OpDecorate %g_Scene Binding 0",
        "OpDecorate %g_InstanceTransforms DescriptorSet 2",
        "OpDecorate %g_InstanceTransforms Binding 1",
        "OpDecorate %g_Output DescriptorSet 3",
        "OpDecorate %g_Output Binding 384",
        "OpTraceRayKHR",
        "OpIgnoreIntersectionKHR",
        "MatrixStride 16",
    };
    const auto containsAll = [](std::string_view text, const auto& contracts)
    {
        return std::ranges::all_of(contracts, [text](std::string_view contract)
        {
            return text.find(contract) != std::string_view::npos;
        });
    };
    if (!containsAll(pixelAssembly, pixelContracts) ||
        !containsAll(libraryAssembly, libraryContracts))
    {
        std::cout << "HShaderCompiler self-test failed: SPIR-V matrix/binding/semantic contract"
                  << std::endl;
        return false;
    }
    if (pixelAssembly.find("OpDecorate %g_Frame DescriptorSet") !=
            std::string::npos ||
        pixelAssembly.find("OpDecorate %g_Frame Binding") !=
            std::string::npos)
    {
        std::cout << "HShaderCompiler self-test failed: Vulkan push constants were emitted as a descriptor"
                  << std::endl;
        return false;
    }
    return true;
#endif
}

static bool RunSelfTests()
{
    const ShaderParseResult parseResult = PreParse(
        "#pragma htype ps PS\n"
        "#pragma hvariant HEAP_DIRECT\n"
        "#pragma hprofile ps ps_6_1\n"
        "#pragma hprofile ps ps_6_0 BASE\n"
        "#pragma hprofile ps ps_6_6 HEAP_DIRECT\n",
        "ProfileSelfTest");
    const std::vector<CompiledShaderArtifact> artifacts =
        BuildShaderArtifacts("ProfileSelfTest", parseResult);
    if (artifacts.size() != 2 ||
        artifacts[0].variant != "BASE" ||
        artifacts[0].profile != "ps_6_0" ||
        artifacts[0].artifactStem != "ProfileSelfTest" ||
        artifacts[1].variant != "HEAP_DIRECT" ||
        artifacts[1].profile != "ps_6_6" ||
        artifacts[1].artifactStem !=
            "ProfileSelfTest_HEAP_DIRECT__ps_6_6")
    {
        std::cout << "HShaderCompiler self-test failed: profile precedence or artifact naming"
                  << std::endl;
        return false;
    }

    const ShaderParseResult dx12Only = PreParse(
        "#pragma htype ps PS\n"
        "#pragma hbackend DX12\n",
        "BackendGateSelfTest");
    const std::vector<CompiledShaderArtifact> dx12OnlyArtifacts =
        BuildShaderArtifacts("BackendGateSelfTest", dx12Only);
    if (dx12OnlyArtifacts.size() != 1 ||
        !dx12OnlyArtifacts.front().backendOnly ||
        *dx12OnlyArtifacts.front().backendOnly != HEngine::RenderBackend::DX12)
    {
        std::cout << "HShaderCompiler self-test failed: explicit backend target gate"
                  << std::endl;
        return false;
    }

    const std::vector<std::string> requirements = ParseDxilRequirementLines(
        "; Note: shader requires additional functionality:\r\n"
        ";       Resource descriptor heap indexing\r\n"
        ";       Sampler descriptor heap indexing\r\n"
        ";\r\n"
        "; Pipeline Runtime Information:\r\n");
    if (requirements != std::vector<std::string>{
            "Resource descriptor heap indexing",
            "Sampler descriptor heap indexing" })
    {
        std::cout << "HShaderCompiler self-test failed: DXIL requirements parsing"
                  << std::endl;
        return false;
    }

    if (!RunMaterialManifestSelfTests())
        return false;

    if (!RunShaderArtifactSelfTests())
        return false;

    std::cout << "HShaderCompiler self-test PASS" << std::endl;
    return true;
}

void InitCachePath(const std::filesystem::path& path)
{
    auto cachePath = path / "CACHES";
    auto cachePathDXIL = path / "CACHES" / "DXIL";
    auto cachePathSpirV = path / "CACHES" / "SPIRV";

    auto CreatePath = [](const std::filesystem::path& _path) {
        std::error_code ec;
        if (!std::filesystem::exists(_path, ec))
        {
            std::filesystem::create_directories(_path, ec);
            if (ec)
            {
                std::cout << "Failed to create directory: " << _path << ", error=" << ec.message() << std::endl;
            }
        }
    };
    CreatePath(cachePath);
    CreatePath(cachePathDXIL);
    CreatePath(cachePathSpirV);
}

bool ProcessPath(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_directory(path, ec))
    {
        std::cout << "Invalid shader folder path: " << path << std::endl;
        return false;
    }

    bool succeeded = true;
    for (auto& p : std::filesystem::directory_iterator(path, ec))
    {
        if (ec)
        {
            std::cout << "Failed to enumerate shader folder: " << path << ", error=" << ec.message() << std::endl;
            return false;
        }

        const auto& filePath = p.path();
        if (!CompileShader(filePath))
            succeeded = false;

        if (std::filesystem::is_directory(filePath) &&
            ToLowerCopy(filePath.filename().string()) != "caches")
        {
            if (!ProcessPath(filePath))
                succeeded = false;
        }
    }

    return succeeded;
}

int main(int argc, char* argv[])
{
    ScopedCompilerOutputRedirect diagnosticsRedirect;
    try
    {
    std::filesystem::path launchWorkingDirectory = std::filesystem::current_path();
#ifdef _WIN32
    {
        char exePath[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len > 0)
        {
            std::filesystem::current_path(std::filesystem::path(exePath).parent_path());
        }
    }
#endif

    const CommandLineOptions commandLine = ParseCommandLine(argc, argv);
    if (!commandLine.DiagnosticsOutputPath.empty())
    {
        diagnosticsRedirect.Open(ResolveInputPath(
            commandLine.DiagnosticsOutputPath,
            launchWorkingDirectory));
    }
    ArtifactTargets = ParseShaderArtifactTargets(commandLine.BinaryFormat);
    if (commandLine.bSelfTest)
        return RunSelfTests() ? 0 : 1;

    std::filesystem::path path = commandLine.Path;
    path = ResolveInputPath(path, launchWorkingDirectory);

    std::filesystem::path shaderRootPath = commandLine.RootPath;
    if (!shaderRootPath.empty())
    {
        RootPath = ResolveInputPath(shaderRootPath, launchWorkingDirectory);
    }
    else
    {
        RootPath = NormalizePath(RootPath);
    }

    if (!commandLine.OutputPath.empty())
    {
        OutputPath = ResolveInputPath(commandLine.OutputPath, launchWorkingDirectory);
    }
    else
    {
        // Never write generated artifacts back into the engine's source-owned
        // Content tree when the compiler is launched manually.
        OutputPath = ResolveInputPath(
            "../../../../../out/generated/Engine/Shaders",
            std::filesystem::current_path());
    }

    AdditionalIncludePaths.clear();
    for (const std::string& includePath : commandLine.IncludePaths)
        AdditionalIncludePaths.push_back(NormalizePath(ResolveInputPath(includePath, launchWorkingDirectory)));

    InitCachePath(OutputPath);

    // 如果指定单个shader，则仅编译这个shader
    std::filesystem::path shaderPath = commandLine.Shader;
    if (!shaderPath.empty())
    {
        shaderPath = ResolveInputPath(shaderPath, launchWorkingDirectory);
        return CompileShader(shaderPath) ? 0 : 1;
    }

    if (!ProcessPath(path))
        return 1;

    return 0;
    }
    catch (const std::exception& e)
    {
        std::cout << "HShaderCompiler fatal error: " << e.what() << std::endl;
        return 1;
    }
}
