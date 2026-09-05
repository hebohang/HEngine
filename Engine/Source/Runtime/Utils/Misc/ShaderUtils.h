#pragma once
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Platform/PlatformPaths.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <queue>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace HEngine
{
		namespace Utils
	{
#if WITH_EDITOR
		inline bool CompileShader(
			const std::filesystem::path& shaderPath,
			std::string* diagnostics = nullptr);
#endif

		inline std::string ToLowerCopy(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return value;
		}

		inline std::string TrimString(const std::string& value)
		{
			const auto beginIt = std::find_if_not(value.begin(), value.end(), [](const unsigned char ch) {
				return std::isspace(ch) != 0;
			});
			if (beginIt == value.end())
				return "";

			const auto endIt = std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char ch) {
				return std::isspace(ch) != 0;
			}).base();

			return std::string(beginIt, endIt);
		}

		inline std::filesystem::path NormalizePath(const std::filesystem::path& path)
		{
			std::error_code ec;
			const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
			if (!ec)
				return canonicalPath;

			return path.lexically_normal();
		}

		inline bool IsPathWithin(const std::filesystem::path& childPath,
			const std::filesystem::path& rootPath)
		{
			const std::filesystem::path normalizedChild = NormalizePath(childPath);
			const std::filesystem::path normalizedRoot = NormalizePath(rootPath);
			auto childIt = normalizedChild.begin();
			for (auto rootIt = normalizedRoot.begin(); rootIt != normalizedRoot.end(); ++rootIt, ++childIt)
			{
				if (childIt == normalizedChild.end() || ToLowerCopy(childIt->string()) != ToLowerCopy(rootIt->string()))
					return false;
			}
			return true;
		}

		inline bool ReadTextFile(const std::filesystem::path& filePath, std::string& outText)
		{
			std::ifstream in(filePath, std::ios::in | std::ios::binary);
			if (!in.is_open())
				return false;

			std::stringstream stream;
			stream << in.rdbuf();
			outText = stream.str();
			return true;
		}

		inline bool TryExtractIncludePath(const std::string& line, std::string& includePath)
		{
			const std::string trimmedLine = TrimString(line);
			if (!trimmedLine.starts_with("#include"))
				return false;

			const size_t firstDelimiter = trimmedLine.find_first_of("\"<");
			if (firstDelimiter == std::string::npos)
				return false;

			const char closeDelimiter = trimmedLine[firstDelimiter] == '<' ? '>' : '"';
			const size_t secondDelimiter = trimmedLine.find(closeDelimiter, firstDelimiter + 1);
			if (secondDelimiter == std::string::npos)
				return false;

			includePath = TrimString(trimmedLine.substr(firstDelimiter + 1, secondDelimiter - firstDelimiter - 1));
			return !includePath.empty();
		}

		inline std::filesystem::path ResolveIncludePath(const std::filesystem::path& sourcePath,
			const std::string& includePath,
			const std::filesystem::path& shaderRoot)
		{
			std::error_code ec;
			const std::filesystem::path includeAsPath(includePath);

			if (includeAsPath.is_absolute())
			{
				const std::filesystem::path normalizedPath = NormalizePath(includeAsPath);
				if (std::filesystem::exists(normalizedPath, ec))
					return normalizedPath;
				return {};
			}

			const std::filesystem::path localPath = NormalizePath(sourcePath.parent_path() / includeAsPath);
			if (std::filesystem::exists(localPath, ec))
				return localPath;

			const std::filesystem::path rootPath = NormalizePath(shaderRoot / includeAsPath);
			if (std::filesystem::exists(rootPath, ec))
				return rootPath;

			const std::filesystem::path enginePath = NormalizePath(
				ConfigManager::GetEngineContentFullPath("Shaders") / includeAsPath);
			if (std::filesystem::exists(enginePath, ec))
				return enginePath;

			return {};
		}

		struct ShaderDependencyGraph
		{
			std::unordered_map<std::string, std::filesystem::path> filePathByKey;
			std::unordered_map<std::string, std::unordered_set<std::string>> includeMap;
			std::unordered_map<std::string, std::unordered_set<std::string>> reverseIncludeMap;
			bool bInitialized = false;
		};

		inline ShaderDependencyGraph& GetShaderDependencyGraph(const std::filesystem::path& shaderRoot)
		{
			static std::unordered_map<std::string, ShaderDependencyGraph> shaderDependencyGraphs;
			return shaderDependencyGraphs[ToLowerCopy(NormalizePath(shaderRoot).generic_string())];
		}

		inline bool IsShaderSourceFile(const std::filesystem::path& filePath)
		{
			const std::string extension = ToLowerCopy(filePath.extension().string());
			return extension == ".hlsl" || extension == ".hlsli";
		}

		inline void RefreshShaderDependencyNodeInternal(const std::filesystem::path& sourcePath,
			const std::filesystem::path& shaderRoot,
			ShaderDependencyGraph& graph)
		{
			const std::filesystem::path normalizedSourcePath = NormalizePath(sourcePath);
			const std::string sourceKey = ToLowerCopy(normalizedSourcePath.generic_string());
			graph.filePathByKey[sourceKey] = normalizedSourcePath;

			std::error_code ec;
			if (!std::filesystem::exists(normalizedSourcePath, ec) || ec ||
				!std::filesystem::is_regular_file(normalizedSourcePath, ec) || ec ||
				!IsShaderSourceFile(normalizedSourcePath))
			{
				if (graph.includeMap.count(sourceKey))
				{
					for (const auto& oldIncludeKey : graph.includeMap[sourceKey])
					{
						auto reverseIt = graph.reverseIncludeMap.find(oldIncludeKey);
						if (reverseIt == graph.reverseIncludeMap.end())
							continue;

						reverseIt->second.erase(sourceKey);
						if (reverseIt->second.empty())
							graph.reverseIncludeMap.erase(reverseIt);
					}
					graph.includeMap.erase(sourceKey);
				}
				return;
			}

			std::string sourceText;
			if (!ReadTextFile(normalizedSourcePath, sourceText))
				return;

			std::unordered_set<std::string> newIncludeKeys;
			std::stringstream sourceStream(sourceText);
			std::string line;
			while (std::getline(sourceStream, line))
			{
				std::string includePath;
				if (!TryExtractIncludePath(line, includePath))
					continue;

				const std::filesystem::path resolvedIncludePath = ResolveIncludePath(normalizedSourcePath, includePath, shaderRoot);
				if (resolvedIncludePath.empty())
					continue;

				const std::filesystem::path normalizedIncludePath = NormalizePath(resolvedIncludePath);
				const std::string includeKey = ToLowerCopy(normalizedIncludePath.generic_string());
				graph.filePathByKey[includeKey] = normalizedIncludePath;
				newIncludeKeys.insert(includeKey);
			}

			auto& oldIncludeKeys = graph.includeMap[sourceKey];
			for (const auto& oldIncludeKey : oldIncludeKeys)
			{
				if (newIncludeKeys.count(oldIncludeKey))
					continue;

				auto reverseIt = graph.reverseIncludeMap.find(oldIncludeKey);
				if (reverseIt == graph.reverseIncludeMap.end())
					continue;

				reverseIt->second.erase(sourceKey);
				if (reverseIt->second.empty())
					graph.reverseIncludeMap.erase(reverseIt);
			}

			for (const auto& newIncludeKey : newIncludeKeys)
			{
				if (oldIncludeKeys.count(newIncludeKey))
					continue;

				graph.reverseIncludeMap[newIncludeKey].insert(sourceKey);
			}

			oldIncludeKeys = std::move(newIncludeKeys);
		}

		inline std::filesystem::path GetProjectShaderRootPath()
		{
			return NormalizePath(ConfigManager::GetInstance().GetShadersFolder());
		}

		inline std::filesystem::path GetEngineShaderRootPath()
		{
			return NormalizePath(ConfigManager::GetEngineContentFullPath("Shaders"));
		}

		inline std::filesystem::path GetShaderOutputRootPath(
			const std::filesystem::path& shaderRoot)
		{
			(void)shaderRoot;
			return NormalizePath(
				ConfigManager::GetEngineGeneratedContentFullPath("Shaders"));
		}

		inline std::filesystem::path GetShaderRootPath(const std::filesystem::path& shaderPath)
		{
			const std::filesystem::path projectRoot = GetProjectShaderRootPath();
			const std::filesystem::path engineRoot = GetEngineShaderRootPath();
			if (IsPathWithin(shaderPath, projectRoot))
				return projectRoot;
			if (IsPathWithin(shaderPath, engineRoot))
				return engineRoot;
			return projectRoot;
		}

		inline void EnsureShaderDependencyGraphInitialized(const std::filesystem::path& shaderRoot)
		{
			auto& graph = GetShaderDependencyGraph(shaderRoot);
			if (graph.bInitialized)
				return;

			std::error_code ec;
			if (!std::filesystem::exists(shaderRoot, ec))
			{
				graph.bInitialized = true;
				return;
			}

			for (const auto& entry : std::filesystem::recursive_directory_iterator(shaderRoot))
			{
				if (!entry.is_regular_file())
					continue;

				if (!IsShaderSourceFile(entry.path()))
					continue;

				RefreshShaderDependencyNodeInternal(entry.path(), shaderRoot, graph);
			}

			graph.bInitialized = true;
		}

		inline void RefreshShaderDependencyNode(const std::filesystem::path& sourcePath)
		{
			const std::filesystem::path shaderRoot = GetShaderRootPath(sourcePath);
			EnsureShaderDependencyGraphInitialized(shaderRoot);

			auto& graph = GetShaderDependencyGraph(shaderRoot);
			RefreshShaderDependencyNodeInternal(sourcePath, shaderRoot, graph);
		}

		inline std::vector<std::filesystem::path> CollectShadersDependentOnIncludeForRoot(
			const std::filesystem::path& includePath,
			const std::filesystem::path& shaderRoot)
		{
			std::vector<std::filesystem::path> dependentShaders;
			EnsureShaderDependencyGraphInitialized(shaderRoot);

			auto& graph = GetShaderDependencyGraph(shaderRoot);
			RefreshShaderDependencyNodeInternal(includePath, shaderRoot, graph);

			std::error_code ec;
			if (!std::filesystem::exists(shaderRoot, ec))
				return dependentShaders;

			const std::filesystem::path normalizedIncludePath = NormalizePath(includePath);
			const std::string includeKey = ToLowerCopy(normalizedIncludePath.generic_string());
			graph.filePathByKey[includeKey] = normalizedIncludePath;

			std::unordered_set<std::string> visitedKeys;
			std::unordered_set<std::string> shaderKeys;
			std::queue<std::string> pendingKeys;

			visitedKeys.insert(includeKey);
			pendingKeys.push(includeKey);

			while (!pendingKeys.empty())
			{
				const std::string currentKey = pendingKeys.front();
				pendingKeys.pop();

				auto reverseIt = graph.reverseIncludeMap.find(currentKey);
				if (reverseIt == graph.reverseIncludeMap.end())
					continue;

				for (const auto& parentKey : reverseIt->second)
				{
					if (!visitedKeys.insert(parentKey).second)
						continue;

					pendingKeys.push(parentKey);

					if (!graph.filePathByKey.count(parentKey))
						continue;

					const std::filesystem::path& parentPath = graph.filePathByKey[parentKey];
					if (ToLowerCopy(parentPath.extension().string()) == ".hlsl")
						shaderKeys.insert(parentKey);
				}
			}

			for (const auto& shaderKey : shaderKeys)
			{
				if (!graph.filePathByKey.count(shaderKey))
					continue;
				dependentShaders.push_back(graph.filePathByKey[shaderKey]);
			}

			std::sort(dependentShaders.begin(), dependentShaders.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
				return a.generic_string() < b.generic_string();
			});

			return dependentShaders;
		}

		inline std::vector<std::filesystem::path> CollectShadersDependentOnInclude(
			const std::filesystem::path& includePath)
		{
			std::vector<std::filesystem::path> dependentShaders =
				CollectShadersDependentOnIncludeForRoot(includePath, GetEngineShaderRootPath());
			const std::filesystem::path projectRoot = GetProjectShaderRootPath();
			if (projectRoot != GetEngineShaderRootPath())
			{
				auto projectDependents = CollectShadersDependentOnIncludeForRoot(includePath, projectRoot);
				dependentShaders.insert(
					dependentShaders.end(), projectDependents.begin(), projectDependents.end());
			}
			std::sort(dependentShaders.begin(), dependentShaders.end(), [](const auto& a, const auto& b) {
				return a.generic_string() < b.generic_string();
			});
			dependentShaders.erase(
				std::unique(dependentShaders.begin(), dependentShaders.end()), dependentShaders.end());
			return dependentShaders;
		}

		struct ShaderCompilationResult
		{
			std::filesystem::path ShaderPath;
			bool bSucceeded = false;
			std::string Diagnostics;
		};

		template<typename CompileFunction>
		inline std::vector<ShaderCompilationResult> CompileShadersWithResults(
			const std::vector<std::filesystem::path>& shaderPaths,
			CompileFunction&& compileShader)
		{
			std::vector<ShaderCompilationResult> results;
			results.reserve(shaderPaths.size());
			for (const std::filesystem::path& shaderPath : shaderPaths)
			{
				ShaderCompilationResult result;
				result.ShaderPath = shaderPath;
				try
				{
					result.bSucceeded = compileShader(
						shaderPath,
						result.Diagnostics);
				}
				catch (const std::exception& exception)
				{
					result.Diagnostics = exception.what();
				}
				catch (...)
				{
					result.Diagnostics = "Unexpected shader compiler failure.";
				}
				if (!result.bSucceeded && result.Diagnostics.empty())
					result.Diagnostics = "Shader compilation failed.";
				results.push_back(std::move(result));
			}
			return results;
		}

		template<typename CompileFunction>
		inline std::vector<ShaderCompilationResult> CompileDependentShadersForInclude(
			const std::filesystem::path& includePath,
			CompileFunction&& compileShader)
		{
			return CompileShadersWithResults(
				CollectShadersDependentOnInclude(includePath),
				std::forward<CompileFunction>(compileShader));
		}

#if WITH_EDITOR
		inline std::vector<ShaderCompilationResult> CompileDependentShadersForInclude(
			const std::filesystem::path& includePath)
		{
			return CompileDependentShadersForInclude(
				includePath,
				[](const std::filesystem::path& shaderPath, std::string& diagnostics)
				{
					return CompileShader(shaderPath, &diagnostics);
				});
		}

		inline bool RunShaderCompiler(
			const std::string& args,
			std::string* diagnostics = nullptr)
		{
			const auto shaderCompilerPath = PlatformPaths::GetExecutableDirectory() /
				"Programs" / "HShaderCompiler" / "HShaderCompiler.exe";
			if (!std::filesystem::exists(shaderCompilerPath))
			{
				if (diagnostics)
					*diagnostics = "HShaderCompiler not found: " + shaderCompilerPath.string();
				HE_CORE_ERROR("HShaderCompiler not found: {0}", shaderCompilerPath.string());
				return false;
			}

#ifdef _WIN32
			STARTUPINFOA startupInfo = {};
			startupInfo.cb = sizeof(STARTUPINFOA);
			PROCESS_INFORMATION processInfo = {};

			std::string compilerArgs = args;
			std::filesystem::path diagnosticsPath;
			if (diagnostics)
			{
				diagnostics->clear();
				const std::size_t identity = std::hash<std::string>{}(args);
				diagnosticsPath = ConfigManager::GetSavedPath(
					"Editor/ShaderDiagnostics/HShaderCompiler-" +
					std::to_string(identity) + ".log");
				std::error_code removeError;
				std::filesystem::remove(diagnosticsPath, removeError);
				compilerArgs += " --diagnosticsOutput \"" + diagnosticsPath.string() + "\"";
			}
			std::string commandLine = "\"" + shaderCompilerPath.string() + "\" " + compilerArgs;
			auto compilerDir = shaderCompilerPath.parent_path().string();
			if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, compilerDir.c_str(), &startupInfo, &processInfo))
			{
				if (diagnostics)
					*diagnostics = "Failed to launch HShaderCompiler, error=" +
						std::to_string((int)GetLastError());
				HE_CORE_ERROR("Failed to launch HShaderCompiler, error={0}, cmd={1}", (int)GetLastError(), commandLine);
				return false;
			}

			const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, INFINITE);
			DWORD exitCode = 1;
			const BOOL gotExitCode = waitResult == WAIT_OBJECT_0
				? GetExitCodeProcess(processInfo.hProcess, &exitCode)
				: FALSE;
			const DWORD processError = gotExitCode ? ERROR_SUCCESS : GetLastError();
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
			if (diagnostics && std::filesystem::is_regular_file(diagnosticsPath))
			{
				std::ifstream stream(diagnosticsPath, std::ios::binary);
				std::ostringstream contents;
				contents << stream.rdbuf();
				*diagnostics = contents.str();
			}

			if (waitResult != WAIT_OBJECT_0)
			{
				if (diagnostics)
					*diagnostics = "Failed while waiting for HShaderCompiler, result=" +
						std::to_string((int)waitResult) + ", error=" +
						std::to_string((int)processError);
				HE_CORE_ERROR("Failed while waiting for HShaderCompiler, result={0}, error={1}, cmd={2}", (int)waitResult, (int)processError, commandLine);
				return false;
			}
			if (!gotExitCode)
			{
				if (diagnostics)
					*diagnostics = "Failed to read HShaderCompiler exit code, error=" +
						std::to_string((int)processError);
				HE_CORE_ERROR("Failed to read HShaderCompiler exit code, error={0}, cmd={1}", (int)processError, commandLine);
				return false;
			}
			if (exitCode != 0)
			{
				if (diagnostics && diagnostics->empty())
					*diagnostics = "HShaderCompiler exited with code " +
						std::to_string((int)exitCode) + ".";
				HE_CORE_ERROR("HShaderCompiler exited with code {0}, cmd={1}", (int)exitCode, commandLine);
				return false;
			}

			return true;
#else
			std::string compilerArgs = args;
			std::filesystem::path diagnosticsPath;
			if (diagnostics)
			{
				diagnostics->clear();
				const std::size_t identity = std::hash<std::string>{}(args);
				diagnosticsPath = ConfigManager::GetSavedPath(
					"Editor/ShaderDiagnostics/HShaderCompiler-" +
					std::to_string(identity) + ".log");
				std::error_code removeError;
				std::filesystem::remove(diagnosticsPath, removeError);
				compilerArgs += " --diagnosticsOutput \"" + diagnosticsPath.string() + "\"";
			}
			std::string commandLine = "\"" + shaderCompilerPath.string() + "\" " + compilerArgs;
			const int exitCode = std::system(commandLine.c_str());
			if (diagnostics && std::filesystem::is_regular_file(diagnosticsPath))
			{
				std::ifstream stream(diagnosticsPath, std::ios::binary);
				std::ostringstream contents;
				contents << stream.rdbuf();
				*diagnostics = contents.str();
			}
			if (exitCode != 0)
			{
				if (diagnostics && diagnostics->empty())
					*diagnostics = "HShaderCompiler exited with code " +
						std::to_string(exitCode) + ".";
				HE_CORE_ERROR("HShaderCompiler exited with code {0}, cmd={1}", exitCode, commandLine);
				return false;
			}
			return true;
#endif
		}

		inline bool CompileShader(
			const std::filesystem::path& shaderPath,
			std::string* diagnostics)
		{
			const auto extension = ToLowerCopy(shaderPath.extension().string());
			if (extension != ".hlsl")
			{
				if (diagnostics)
					*diagnostics = "Only .hlsl shader sources can be compiled.";
				return false;
			}

			RefreshShaderDependencyNode(shaderPath);

			const auto shaderRoot = GetShaderRootPath(shaderPath);
			const auto engineShaderRoot = GetEngineShaderRootPath();
			const auto outputRoot = GetShaderOutputRootPath(shaderRoot);
			std::string args = "-s \"" + shaderPath.string() + "\" -r \"" +
				shaderRoot.string() + "\" -o \"" + outputRoot.string() + "\"";
			if (shaderRoot != engineShaderRoot)
				args += " --includePath \"" + engineShaderRoot.string() + "\"";
			return RunShaderCompiler(args, diagnostics);
		}

		inline bool CompileAllShader()
		{
			const auto engineShaderRoot = GetEngineShaderRootPath();
			const auto engineOutputRoot = GetShaderOutputRootPath(engineShaderRoot);
			std::string engineArgs = "-p \"" + engineShaderRoot.string() + "\" -r \"" +
				engineShaderRoot.string() + "\" -o \"" + engineOutputRoot.string() + "\"";
			if (!RunShaderCompiler(engineArgs))
				return false;

			const auto projectShaderRoot = GetProjectShaderRootPath();
			if (projectShaderRoot == engineShaderRoot || !std::filesystem::exists(projectShaderRoot))
				return true;

			const auto projectOutputRoot = GetShaderOutputRootPath(projectShaderRoot);
			std::string projectArgs = "-p \"" + projectShaderRoot.string() + "\" -r \"" + projectShaderRoot.string() +
				"\" -o \"" + projectOutputRoot.string() + "\" --includePath \"" +
				engineShaderRoot.string() + "\"";
			return RunShaderCompiler(projectArgs);
		}
#endif
	}
}
