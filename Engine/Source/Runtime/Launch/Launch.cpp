#include "Hepch.h"
#include "DevelopmentLaunchPaths.h"
#include "PackagedProject.h"
#include "ProjectIdentity.h"

#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Log/LogSession.h"
#include "Runtime/Graphics/RHI/RenderBackend.h"
#include "Runtime/Platform/PlatformPaths.h"
#include "Runtime/Platform/PlatformProcess.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeResourceCatalog/RuntimeResourceCatalog.h"
#include "Runtime/Scripting/ScriptEngine.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <HEngine/ProjectBuildConfig.h>

#ifdef WITH_EDITOR
#include "Runtime/Utils/Misc/ShaderUtils.h"
#endif

#include <cstdlib>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine
{
	// To be defined in CLIENT
	extern void MyAppInitialize(Application& app);
}

namespace
{
	void AppendExceptionChain(
		std::string& output,
		const std::exception& exception,
		std::size_t depth)
	{
		if (depth != 0)
			output += "\nCaused by: ";
		output += exception.what();

		constexpr std::size_t c_MaxExceptionDepth = 16;
		if (depth >= c_MaxExceptionDepth)
		{
			output += "\nCaused by: exception chain truncated.";
			return;
		}

		const auto* nested = dynamic_cast<const std::nested_exception*>(&exception);
		if (nested == nullptr || !nested->nested_ptr())
			return;

		try
		{
			std::rethrow_exception(nested->nested_ptr());
		}
		catch (const std::exception& nestedException)
		{
			AppendExceptionChain(output, nestedException, depth + 1);
		}
		catch (...)
		{
			output += "\nCaused by: non-standard exception.";
		}
	}

	[[nodiscard]] std::string DescribeExceptionChain(const std::exception& exception)
	{
		std::string result;
		AppendExceptionChain(result, exception, 0);
		return result;
	}

	[[nodiscard]] bool HasExactLaunchOption(
		int argc,
		char** argv,
		std::string_view expected) noexcept
	{
		for (int index = 1; index < argc; ++index)
		{
			if (std::string_view(argv[index]) == expected)
				return true;
		}
		return false;
	}

	[[nodiscard]] bool IsOneShotOrGraphicsOverrideArgument(
		std::string_view argument)
	{
		return argument == "--warp" ||
			argument == "--strict-graphics-api" ||
			argument == "--strict-renderer" ||
			argument == "--package-check" ||
			argument == "--agent-start-paused" ||
			argument.starts_with("--graphics-api=") ||
			argument.starts_with("--renderer=") ||
			argument.starts_with("--render-path=") ||
			argument.starts_with("--render-settings=") ||
			argument.starts_with("--exit-after-frames=") ||
			argument.starts_with("--capture-frame=") ||
			argument.starts_with("--capture-path=") ||
			argument.starts_with("--render-stats-path=") ||
			argument.starts_with("--package-check-id=") ||
			argument.starts_with("--agent-session=") ||
			argument.starts_with("--agent-token=");
	}

	[[nodiscard]] std::vector<std::string> BuildEditorRelaunchArguments(
		int argc,
		char** argv)
	{
		std::vector<std::string> result;
		result.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0u);
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view argument = argv[index];
			if (!IsOneShotOrGraphicsOverrideArgument(argument))
				result.emplace_back(argument);
		}
		return result;
	}

	[[nodiscard]] std::string_view GetOptionValue(std::string_view argument, std::string_view prefix)
	{
		if (!argument.starts_with(prefix) || argument.size() == prefix.size())
			throw std::invalid_argument("Missing value for launch option: " + std::string(prefix));
		return argument.substr(prefix.size());
	}

	[[nodiscard]] std::uint64_t ParsePositiveFrameNumber(
		std::string_view text,
		std::string_view optionName)
	{
		std::uint64_t value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (error != std::errc{} || end != text.data() + text.size() || value == 0)
			throw std::invalid_argument(std::string(optionName) + " requires a positive integer.");
		return value;
	}

	[[nodiscard]] float ParseFixedDelta(std::string_view text)
	{
		std::size_t parsedCharacters = 0;
		const float value = std::stof(std::string(text), &parsedCharacters);
		if (parsedCharacters != text.size() || !std::isfinite(value) || value <= 0.0f || value > 1.0f)
			throw std::invalid_argument("--fixed-delta requires seconds in the range (0, 1].");
		return value;
	}

	struct ParsedWindowSize
	{
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
	};

	[[nodiscard]] ParsedWindowSize ParseWindowSize(std::string_view text)
	{
		const std::size_t separator = text.find_first_of("xX");
		if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size())
			throw std::invalid_argument("--resolution requires WIDTHxHEIGHT.");

		ParsedWindowSize size;
		const std::string_view widthText = text.substr(0, separator);
		const std::string_view heightText = text.substr(separator + 1);
		const auto [widthEnd, widthError] = std::from_chars(
			widthText.data(), widthText.data() + widthText.size(), size.Width);
		const auto [heightEnd, heightError] = std::from_chars(
			heightText.data(), heightText.data() + heightText.size(), size.Height);
		if (widthError != std::errc{} || widthEnd != widthText.data() + widthText.size() ||
			heightError != std::errc{} || heightEnd != heightText.data() + heightText.size() ||
			size.Width == 0 || size.Height == 0 || size.Width > 8192 || size.Height > 8192)
		{
			throw std::invalid_argument("--resolution requires integer dimensions in the range [1, 8192].");
		}
		return size;
	}

#if defined(HE_PACKAGED_GAME)
	[[nodiscard]] std::string ParsePackageCheckId(std::string_view text)
	{
		if (text.size() != 32)
		{
			throw std::invalid_argument(
				"--package-check-id requires exactly 32 hexadecimal characters.");
		}

		std::string result;
		result.reserve(text.size());
		for (const char character : text)
		{
			if (character >= '0' && character <= '9')
			{
				result.push_back(character);
			}
			else if (character >= 'a' && character <= 'f')
			{
				result.push_back(character);
			}
			else if (character >= 'A' && character <= 'F')
			{
				result.push_back(static_cast<char>(character - 'A' + 'a'));
			}
			else
			{
				throw std::invalid_argument(
					"--package-check-id requires exactly 32 hexadecimal characters.");
			}
		}
		return result;
	}
#endif

#if !defined(HE_PACKAGED_GAME)
	struct DevelopmentProjectSelection
	{
		std::filesystem::path Root;
		std::filesystem::path GameConfig;
	};

	[[nodiscard]] std::string ReadRequiredProjectString(
		const HEngine::Json& descriptor,
		std::string_view fieldName)
	{
		const auto field = descriptor.find(std::string(fieldName));
		if (field == descriptor.end() || !field->is_string())
		{
			throw std::runtime_error(
				"Project.heproject requires a string field named " + std::string(fieldName) + ".");
		}

		const std::string value = field->get<std::string>();
		if (value.empty())
		{
			throw std::runtime_error(
				"Project.heproject field " + std::string(fieldName) + " cannot be empty.");
		}
		return value;
	}

#if defined(WITH_EDITOR) || defined(HE_DEV_AGENT_RUNTIME)
	[[nodiscard]] std::string ParseStartupScene(std::string_view text)
	{
		std::string normalized;
		const HEngine::Utils::PathValidationResult validation =
			HEngine::Utils::TryNormalizeProjectRelativePath(text, normalized);
		if (!validation || !normalized.starts_with("Assets/") ||
			HEngine::Utils::Utf8ToPath(normalized).extension() != ".he")
		{
			throw std::invalid_argument(
				"--startup-scene requires a safe Assets-relative .he path.");
		}
		return normalized;
	}
#endif

	void RequireMatchingBuildValue(
		const HEngine::Json& descriptor,
		std::string_view fieldName,
		std::string_view buildValue)
	{
		const std::string selectedValue = ReadRequiredProjectString(descriptor, fieldName);
		if (selectedValue == buildValue)
			return;

		throw std::runtime_error(
			"Selected Project.heproject " + std::string(fieldName) + " ('" + selectedValue +
			"') does not match this build ('" + std::string(buildValue) +
			"'). Rebuild with hectl -Project for the selected project.");
	}

	[[nodiscard]] DevelopmentProjectSelection ResolveDevelopmentProject(
		const std::filesystem::path& projectSelection, const std::string& gameConfigOverride)
	{
		const std::filesystem::path absoluteSelection = projectSelection.is_absolute()
			? projectSelection
			: std::filesystem::absolute(projectSelection);

		std::filesystem::path projectRoot;
		std::filesystem::path descriptorPath;
		if (std::filesystem::is_regular_file(absoluteSelection))
		{
			if (absoluteSelection.filename() != "Project.heproject")
			{
				throw std::runtime_error(
					"--project must name a project directory or Project.heproject: " +
					absoluteSelection.string());
			}
			descriptorPath = absoluteSelection;
			projectRoot = absoluteSelection.parent_path();
		}
		else if (std::filesystem::is_directory(absoluteSelection))
		{
			projectRoot = absoluteSelection;
			descriptorPath = projectRoot / "Project.heproject";
		}
		else
		{
			throw std::runtime_error("Selected project path does not exist: " + absoluteSelection.string());
		}

		if (!std::filesystem::is_regular_file(descriptorPath))
			throw std::runtime_error("Project descriptor was not found: " + descriptorPath.string());

		std::error_code canonicalError;
		const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(projectRoot, canonicalError);
		if (canonicalError)
			throw std::runtime_error("Could not resolve selected project root: " + projectRoot.string());

		descriptorPath = canonicalRoot / "Project.heproject";
		const HEngine::Json descriptor = HEngine::Utils::LoadJsonFile(descriptorPath);
		const auto schemaVersion = descriptor.find("SchemaVersion");
		if (schemaVersion == descriptor.end() || !schemaVersion->is_number_integer() ||
			schemaVersion->get<std::int64_t>() != 1)
		{
			throw std::runtime_error("Project.heproject SchemaVersion must be 1: " + descriptorPath.string());
		}

		RequireMatchingBuildValue(descriptor, "EngineVersion", HE_PROJECT_ENGINE_VERSION);
#if !HE_PORTABLE_EDITOR && !HE_PORTABLE_PLAYER
		RequireMatchingBuildValue(descriptor, "ProjectId", HE_PROJECT_ID);
		RequireMatchingBuildValue(descriptor, "DisplayName", HE_PROJECT_DISPLAY_NAME);
		RequireMatchingBuildValue(descriptor, "ExecutableName", HE_PROJECT_EXECUTABLE_NAME);
#endif
#if HE_PORTABLE_PLAYER
        (void)HEngine::ReadProjectIdentity(descriptor, HE_PROJECT_ENGINE_VERSION);
#endif

		std::string normalizedGameConfig;
		if (!HEngine::Utils::TryNormalizeProjectRelativePath(
			gameConfigOverride.empty() ? ReadRequiredProjectString(descriptor, "GameConfig") : gameConfigOverride,
			normalizedGameConfig) ||
			std::filesystem::path(normalizedGameConfig).extension() != ".json")
			throw std::runtime_error("Project GameConfig must be a safe project-relative JSON path.");
		const std::filesystem::path gameConfig(normalizedGameConfig);
		const auto resolvedGameConfig = std::filesystem::weakly_canonical(canonicalRoot / gameConfig);
		const auto relativeGameConfig = resolvedGameConfig.lexically_relative(canonicalRoot);
		if (relativeGameConfig.empty() || *relativeGameConfig.begin() == ".." ||
			!std::filesystem::is_regular_file(resolvedGameConfig))
			throw std::runtime_error("Project GameConfig is missing or escapes the project root.");
#if !HE_PORTABLE_EDITOR && !HE_PORTABLE_PLAYER
		const std::filesystem::path buildGameConfig =
			std::filesystem::path(HE_PROJECT_GAME_CONFIG).lexically_normal();
		if (gameConfig.generic_string() != buildGameConfig.generic_string())
		{
			throw std::runtime_error(
				"Selected Project.heproject GameConfig ('" + gameConfig.generic_string() +
				"') does not match this build ('" + buildGameConfig.generic_string() +
				"'). Rebuild with hectl -Project for the selected project.");
		}
#endif

		return { canonicalRoot, gameConfig };
	}
#endif

	[[nodiscard]] HEngine::ApplicationRunOptions ParseRunOptions(int argc, char** argv)
	{
		HEngine::ApplicationRunOptions options;
		bool bPackageCheckRequested = false;
		bool bGraphicsApiSpecified = false;
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view argument = argv[index];
			if (argument == "--package-check")
			{
				if (bPackageCheckRequested)
					throw std::invalid_argument("--package-check may only be specified once.");
				bPackageCheckRequested = true;
				options.ExitAfterFrames = 2;
				options.FixedDeltaSeconds = 1.0f / 60.0f;
				options.WindowWidth = 640;
				options.WindowHeight = 360;
				options.bNoCursorCapture = true;
				options.bNoVSync = true;
				options.bRenderStats = true;
			}
#if defined(HE_PACKAGED_GAME)
			else if (argument.starts_with("--package-check-id="))
			{
				if (!options.PackageCheckId.empty())
					throw std::invalid_argument("--package-check-id may only be specified once.");
				options.PackageCheckId = ParsePackageCheckId(
					GetOptionValue(argument, "--package-check-id="));
			}
#endif
			else if (argument == "--warp")
			{
				options.bUseWarp = true;
			}
			else if (argument.starts_with("--graphics-api=") ||
				argument.starts_with("--renderer="))
			{
				if (bGraphicsApiSpecified)
					throw std::invalid_argument(
						"The graphics API may only be specified once.");
				const std::string_view prefix = argument.starts_with("--graphics-api=")
					? "--graphics-api=" : "--renderer=";
				options.GraphicsApiOverride = HEngine::ParseRenderBackend(
					GetOptionValue(argument, prefix));
				bGraphicsApiSpecified = true;
			}
			else if (argument == "--strict-graphics-api" ||
				argument == "--strict-renderer")
			{
				options.bStrictGraphicsApi = true;
			}
			else if (argument == "--no-cursor-capture")
			{
				options.bNoCursorCapture = true;
			}
			else if (argument == "--no-vsync")
			{
				options.bNoVSync = true;
			}
			else if (argument == "--render-stats")
			{
				options.bRenderStats = true;
			}
			else if (argument.starts_with("--render-path="))
			{
				const std::string_view path = GetOptionValue(argument, "--render-path=");
				if (path == "forward" || path == "Forward")
					options.RenderingPathOverride = "Forward";
				else if (path == "deferred" || path == "Deferred")
					options.RenderingPathOverride = "Deferred";
				else
					throw std::invalid_argument("--render-path requires forward or deferred.");
			}
			else if (argument.starts_with("--render-settings="))
			{
				options.RenderSettingsPath = std::filesystem::path(std::string(
					GetOptionValue(argument, "--render-settings=")));
				if (!options.RenderSettingsPath.is_absolute())
					throw std::invalid_argument("--render-settings requires an absolute JSON path.");
			}
			else if (argument.starts_with("--render-stats-path="))
			{
				options.RenderStatsPath = std::filesystem::path(std::string(
					GetOptionValue(argument, "--render-stats-path=")));
				if (!options.RenderStatsPath.is_absolute())
					throw std::invalid_argument("--render-stats-path requires an absolute CSV path.");
				options.bRenderStats = true;
			}
			else if (argument.starts_with("--performance-warmup-frames="))
			{
				const std::string_view value = GetOptionValue(argument, "--performance-warmup-frames=");
				options.PerformanceWarmupFrames = value == "0" ? 0 :
					ParsePositiveFrameNumber(value, "--performance-warmup-frames");
				if (options.PerformanceWarmupFrames > 10000000)
					throw std::invalid_argument("--performance-warmup-frames exceeds 10000000.");
			}
			else if (argument.starts_with("--exit-after-frames="))
			{
				options.ExitAfterFrames = ParsePositiveFrameNumber(
					GetOptionValue(argument, "--exit-after-frames="),
					"--exit-after-frames");
			}
			else if (argument.starts_with("--fixed-delta="))
			{
				options.FixedDeltaSeconds = ParseFixedDelta(
					GetOptionValue(argument, "--fixed-delta="));
			}
            else if (argument == "--windowed")
            {
                options.FullscreenOverride = false;
            }
            else if (argument == "--borderless-fullscreen")
            {
                options.FullscreenOverride = true;
            }
			else if (argument.starts_with("--resolution="))
			{
				const ParsedWindowSize size = ParseWindowSize(
					GetOptionValue(argument, "--resolution="));
				options.WindowWidth = size.Width;
				options.WindowHeight = size.Height;
			}
#if !defined(HE_PACKAGED_GAME)
			else if (argument == "--render-validation")
			{
				options.bRenderValidation = true;
			}
			else if (argument.starts_with("--capture-frame="))
			{
				options.CaptureFrame = ParsePositiveFrameNumber(
					GetOptionValue(argument, "--capture-frame="),
					"--capture-frame");
			}
			else if (argument.starts_with("--capture-path="))
			{
				options.CapturePath = std::filesystem::path(std::string(
					GetOptionValue(argument, "--capture-path=")));
			}
			else if (argument.starts_with("--saved-root="))
			{
				options.SavedRoot = std::filesystem::path(std::string(
					GetOptionValue(argument, "--saved-root=")));
				if (!options.SavedRoot.is_absolute())
					throw std::invalid_argument("--saved-root requires an absolute path.");
			}
            else if (argument.starts_with("--viewport-mode="))
            {
                options.StartupViewportMode = std::string(GetOptionValue(argument, "--viewport-mode="));
            }
            else if (argument.starts_with("--startup-scene="))
			{
#if defined(WITH_EDITOR) || defined(HE_DEV_AGENT_RUNTIME)
				options.StartupScene = ParseStartupScene(
					GetOptionValue(argument, "--startup-scene="));
#else
				throw std::invalid_argument(
					"--startup-scene is only supported by HEngineEditor or Dev Agent HGame.");
#endif
			}
			else if (argument.starts_with("--project="))
			{
				options.ProjectRoot = std::filesystem::path(std::string(
					GetOptionValue(argument, "--project=")));
			}
#if HE_PORTABLE_EDITOR || HE_PORTABLE_PLAYER
			else if (argument.starts_with("--game-config="))
			{
				if (!options.ProjectGameConfig.empty())
					throw std::invalid_argument("--game-config may only be specified once.");
				options.ProjectGameConfig = std::string(GetOptionValue(argument, "--game-config="));
			}
#endif
#if defined(HE_DEV_AGENT_RUNTIME)
			else if (argument.starts_with("--input-replay="))
			{
				if (!options.InputReplayPath.empty())
					throw std::invalid_argument("--input-replay may only be specified once.");
				options.InputReplayPath = std::filesystem::path(std::string(
					GetOptionValue(argument, "--input-replay=")));
				if (!options.InputReplayPath.is_absolute())
					throw std::invalid_argument("--input-replay requires an absolute JSON path.");
			}
			else if (argument.starts_with("--agent-session="))
			{
				options.AgentSessionDirectory = std::filesystem::path(std::string(
					GetOptionValue(argument, "--agent-session=")));
				if (!options.AgentSessionDirectory.is_absolute())
					throw std::invalid_argument("--agent-session requires an absolute path.");
			}
			else if (argument.starts_with("--agent-token="))
			{
				options.AgentToken = std::string(GetOptionValue(argument, "--agent-token="));
			}
			else if (argument == "--agent-start-paused")
			{
				options.bAgentStartPaused = true;
			}
#endif
#else
			else if (argument.starts_with("--project=") ||
				argument.starts_with("--saved-root="))
			{
				throw std::invalid_argument(
					"Packaged games do not accept external project or Saved roots.");
			}
#endif
			else
			{
				throw std::invalid_argument("Unknown launch option: " + std::string(argument));
			}
		}

#if defined(HE_PACKAGED_GAME)
		if (!options.PackageCheckId.empty() && !bPackageCheckRequested)
			throw std::invalid_argument("--package-check-id may only be used with --package-check.");
		if (bPackageCheckRequested && options.PackageCheckId.empty())
		{
			throw std::invalid_argument(
				"--package-check requires --package-check-id=<32 hexadecimal characters>.");
		}
#endif

#if !defined(HE_PACKAGED_GAME)
		if ((options.CaptureFrame == 0) != options.CapturePath.empty())
			throw std::invalid_argument("--capture-frame and --capture-path must be provided together.");
		if (options.CaptureFrame > 0 && options.ExitAfterFrames > 0 &&
			options.CaptureFrame > options.ExitAfterFrames)
		{
			throw std::invalid_argument("--capture-frame cannot be greater than --exit-after-frames.");
		}
#if defined(HE_DEV_AGENT_RUNTIME)
		if (!options.InputReplayPath.empty() && options.HasAgentSession())
			throw std::invalid_argument("--input-replay cannot be combined with --agent-session.");
		if (!options.InputReplayPath.empty() && options.ExitAfterFrames == 0)
			throw std::invalid_argument("--input-replay requires --exit-after-frames.");
		if (!options.AgentToken.empty() && !options.HasAgentSession())
			throw std::invalid_argument("--agent-token requires --agent-session.");
		if (options.bAgentStartPaused && !options.HasAgentSession())
			throw std::invalid_argument("--agent-start-paused requires --agent-session.");
#endif
#endif
		if (options.bUseWarp && options.GraphicsApiOverride ==
				HEngine::RenderBackend::Vulkan)
		{
			throw std::invalid_argument(
				"--warp is a DX12-only adapter option and cannot be combined with Vulkan.");
		}
		return options;
	}

	struct GraphicsProbeRequest
	{
		HEngine::RenderBackend Backend = HEngine::RenderBackend::DX12;
		HEngine::RenderAdapterSelectionOptions AdapterSelection;
		std::filesystem::path OutputPath = "GraphicsCaps.json";
	};

	[[nodiscard]] std::optional<GraphicsProbeRequest> ParseGraphicsProbeRequest(
		int argc,
		char** argv)
	{
		bool bRequested = false;
		bool bLegacyDx12Probe = false;
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view argument = argv[index];
			if (argument == "--dx12-probe" || argument == "--graphics-probe")
			{
				bRequested = true;
				bLegacyDx12Probe = argument == "--dx12-probe";
				break;
			}
		}
		if (!bRequested)
			return std::nullopt;

		GraphicsProbeRequest request;
		bool bOutputSpecified = false;
		bool bGraphicsApiSpecified = false;
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view argument = argv[index];
			if (argument == "--dx12-probe" || argument == "--graphics-probe" ||
				argument == "--strict-graphics-api" || argument == "--strict-renderer")
				continue;

			std::string_view outputValue;
			if (argument == "--output")
			{
				if (index + 1 >= argc)
					throw std::invalid_argument("--output requires a path for --dx12-probe.");
				outputValue = argv[++index];
			}
			else if (argument.starts_with("--output="))
			{
				outputValue = GetOptionValue(argument, "--output=");
			}
			else if (argument.starts_with("--graphics-api=") ||
				argument.starts_with("--renderer="))
			{
				if (bGraphicsApiSpecified)
					throw std::invalid_argument(
						"The graphics API may only be specified once for --graphics-probe.");
				const std::string_view prefix = argument.starts_with("--graphics-api=")
					? "--graphics-api=" : "--renderer=";
				request.Backend = HEngine::ParseRenderBackend(
					GetOptionValue(argument, prefix));
				bGraphicsApiSpecified = true;
				continue;
			}
			else if (argument == "--warp")
			{
				request.AdapterSelection.bUseWarp = true;
				continue;
			}
			else
			{
				throw std::invalid_argument(
					"Unknown graphics probe option: " + std::string(argument));
			}

			if (bOutputSpecified)
				throw std::invalid_argument("--output may only be specified once for --dx12-probe.");
			if (outputValue.empty())
				throw std::invalid_argument("--output requires a non-empty path for --dx12-probe.");
			request.OutputPath = std::filesystem::path(std::string(outputValue));
			bOutputSpecified = true;
		}
		if (bLegacyDx12Probe && bGraphicsApiSpecified &&
			request.Backend != HEngine::RenderBackend::DX12)
		{
			throw std::invalid_argument(
				"--dx12-probe cannot be combined with a non-DX12 graphics API.");
		}
		if (request.AdapterSelection.bUseWarp &&
			request.Backend != HEngine::RenderBackend::DX12)
		{
			throw std::invalid_argument(
				"--warp is a DX12-only adapter option and cannot be combined with Vulkan.");
		}
		return request;
	}
}

int main(int argc, char** argv)
{
	HEngine::LogSession logSession;
	bool bScriptInitialized = false;
	const bool bPackageCheckRequested =
		HasExactLaunchOption(argc, argv, "--package-check");
	std::filesystem::path diagnosticLogPath;
	try
	{
		HEngine::Log::Init();
#if defined(HE_PACKAGED_GAME) && !HE_PORTABLE_PLAYER
		if (!bPackageCheckRequested)
		{
			try
			{
				diagnosticLogPath =
					HEngine::PlatformPaths::GetLocalAppDataDirectory() /
					"HEngine" / "Projects" / HE_PROJECT_ID / "Saved" / "Logs" / "HEngine.log";
				if (!HEngine::Log::InstallFileSink(diagnosticLogPath))
					diagnosticLogPath.clear();
			}
			catch (...)
			{
				diagnosticLogPath.clear();
			}
		}
#endif
		if (const std::optional<GraphicsProbeRequest> probeRequest =
			ParseGraphicsProbeRequest(argc, argv))
		{
			try
			{
				HEngine::RunRenderCapabilityProbe(
					probeRequest->Backend,
					probeRequest->OutputPath,
					probeRequest->AdapterSelection);
				return EXIT_SUCCESS;
			}
			catch (const std::exception& exception)
			{
				const std::string details = DescribeExceptionChain(exception);
				const std::string label = std::string(HEngine::ToString(
					probeRequest->Backend)) + " capability probe failed";
				HE_CORE_ERROR("{0}: {1}", label, details);
				HEngine::Log::WriteFallbackError(label, details);
				std::fprintf(stderr, "%s: %s\n", label.c_str(), details.c_str());
				return EXIT_FAILURE;
			}
			catch (...)
			{
				const std::string label = std::string(HEngine::ToString(
					probeRequest->Backend)) +
					" capability probe failed with an unexpected error";
				HE_CORE_ERROR("{0}", label);
				HEngine::Log::WriteFallbackError(label);
				std::fprintf(stderr, "%s\n", label.c_str());
				return EXIT_FAILURE;
			}
		}
		const HEngine::ApplicationRunOptions runOptions = ParseRunOptions(argc, argv);
		HEngine::Application::GetInstance().ConfigureRunOptions(runOptions);

		// Initialize all paths before systems start loading content.
#if defined(HE_PACKAGED_GAME)
		const HEngine::PackagedProjectPaths paths =
			HEngine::PackagedProject::Prepare(runOptions.PackageCheckId);
		if (bPackageCheckRequested || HE_PORTABLE_PLAYER)
		{
			diagnosticLogPath = paths.savedRoot / "Logs" / "HEngine.log";
			if (!HEngine::Log::InstallFileSink(diagnosticLogPath))
				diagnosticLogPath.clear();
		}
		HEngine::ConfigManager::GetInstance().Init(
			paths.contentRoot,
			paths.contentRoot,
			paths.savedRoot,
			paths.contentRoot,
			paths.gameConfig);
		HEngine::RuntimeResourceCatalog::GetInstance().Install(paths.resourceCatalog);
#else
		const HEngine::DevelopmentLaunchPaths launchPaths =
			HEngine::ResolveDevelopmentLaunchPaths(
				runOptions.ProjectRoot,
				HEngine::PlatformPaths::GetApplicationContentDirectory(),
				HE_DEFAULT_PROJECT_DIR,
				HE_ENGINE_CONTENT_DIR,
				HE_ENGINE_GENERATED_CONTENT_DIR);
		const DevelopmentProjectSelection project = ResolveDevelopmentProject(
			launchPaths.ProjectSelection, runOptions.ProjectGameConfig);
		HEngine::ConfigManager::GetInstance().Init(
			project.Root,
			launchPaths.EngineContentRoot,
			runOptions.SavedRoot,
			launchPaths.EngineGeneratedContentRoot,
			project.GameConfig);
#endif

		// https://github.com/pybind/pybind11/issues/3112
		// We should only have one interpreter in our program.
		HEngine::ScriptEngine::GetInstance().Init();
		bScriptInitialized = true;

		HEngine::MyAppInitialize(HEngine::Application::GetInstance());
		HEngine::Application::GetInstance().Run();
		const bool bRestartRequested =
			HEngine::Application::GetInstance().IsRestartRequested();
		HEngine::Application::GetInstance().Clean();

		HEngine::ScriptEngine::GetInstance().Shutdown();
		bScriptInitialized = false;
		if (bRestartRequested)
		{
			const std::vector<std::string> restartArguments =
				BuildEditorRelaunchArguments(argc, argv);
			const HEngine::PlatformProcessLaunchResult restart =
				HEngine::PlatformProcess::RelaunchCurrentProcess(
					restartArguments);
			if (!restart.bSuccess)
			{
				const std::string details =
					"Editor restart failed: " + restart.Error;
				HEngine::Log::WriteFallbackError(details);
				std::fprintf(stderr, "%s\n", details.c_str());
				return EXIT_FAILURE;
			}
		}
		return EXIT_SUCCESS;
	}
	catch (const std::exception& exception)
	{
		const std::string details = DescribeExceptionChain(exception);
		try
		{
			HEngine::Application::GetInstance().Clean();
		}
		catch (...)
		{
		}
		if (bScriptInitialized)
		{
			try
			{
				HEngine::ScriptEngine::GetInstance().Shutdown();
			}
			catch (...)
			{
			}
		}

#if defined(HE_PACKAGED_GAME)
		HEngine::Log::WriteFallbackError(
			HE_PROJECT_DISPLAY_NAME " could not start",
			details);
		if (HEngine::Log::GetCoreLogger())
		{
			HEngine::Log::GetCoreLogger()->error(
				"{} could not start: {}", HE_PROJECT_DISPLAY_NAME, details);
			HEngine::Log::GetCoreLogger()->flush();
		}
		std::string message =
			HE_PROJECT_DISPLAY_NAME " could not start.\n\n" + details +
			"\n\nPlease reinstall the game if the problem continues.";
		if (!diagnosticLogPath.empty())
			message += "\n\nDiagnostic log:\n" +
				HEngine::Utils::PathToUtf8(diagnosticLogPath);
		if (!bPackageCheckRequested)
			HEngine::PlatformPaths::ShowFatalDialog(HE_PROJECT_DISPLAY_NAME, message);
#else
		HEngine::Log::WriteFallbackError(
			HE_PROJECT_DISPLAY_NAME " could not start",
			details);
#endif
		return EXIT_FAILURE;
	}
	catch (...)
	{
		try
		{
			HEngine::Application::GetInstance().Clean();
		}
		catch (...)
		{
		}
		if (bScriptInitialized)
		{
			try
			{
				HEngine::ScriptEngine::GetInstance().Shutdown();
			}
			catch (...)
			{
			}
		}

#if defined(HE_PACKAGED_GAME)
		HEngine::Log::WriteFallbackError(
			HE_PROJECT_DISPLAY_NAME " could not start because of an unexpected error");
		if (HEngine::Log::GetCoreLogger())
		{
			HEngine::Log::GetCoreLogger()->error(
				"{} could not start because of an unexpected error", HE_PROJECT_DISPLAY_NAME);
			HEngine::Log::GetCoreLogger()->flush();
		}
		std::string message =
			HE_PROJECT_DISPLAY_NAME " could not start because of an unexpected error.\n\n"
			"Please reinstall the game if the problem continues.";
		if (!diagnosticLogPath.empty())
			message += "\n\nDiagnostic log:\n" +
				HEngine::Utils::PathToUtf8(diagnosticLogPath);
		if (!bPackageCheckRequested)
		{
			HEngine::PlatformPaths::ShowFatalDialog(
				HE_PROJECT_DISPLAY_NAME,
				message);
		}
#else
		HEngine::Log::WriteFallbackError(
			HE_PROJECT_DISPLAY_NAME " could not start because of an unexpected error");
#endif
		return EXIT_FAILURE;
	}
}
