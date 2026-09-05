#pragma once

#include "Runtime/Core/Base/Base.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

#include <filesystem>
#include <string_view>

namespace HEngine
{
	class HENGINE_API Log
	{
	public:
		static void Init();
		[[nodiscard]] static bool InstallFileSink(
			const std::filesystem::path& path) noexcept;
		static void WriteFallbackError(std::string_view message, std::string_view details = {}) noexcept;

		[[nodiscard]] inline static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return sCoreLogger; }
		[[nodiscard]] inline static std::shared_ptr<spdlog::logger>& GetClientLogger() { return sClientLogger; }
	private:
		static std::shared_ptr<spdlog::logger> sCoreLogger;
		static std::shared_ptr<spdlog::logger> sClientLogger;
	};
}

// Core log macros
#define HE_CORE_TRACE(...)		::HEngine::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define HE_CORE_INFO(...)		::HEngine::Log::GetCoreLogger()->info(__VA_ARGS__)
#define HE_CORE_WARN(...)		::HEngine::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define HE_CORE_ERROR(...)		::HEngine::Log::GetCoreLogger()->error(__VA_ARGS__)
// #define HE_CORE_FATAL(...)		::HEngine::Log::GetCoreLogger()->fatal(__VA_ARGS__)

// Client log macros
#define HE_TRACE(...)			::HEngine::Log::GetClientLogger()->trace(__VA_ARGS__)
#define HE_INFO(...)			::HEngine::Log::GetClientLogger()->info(__VA_ARGS__)
#define HE_WARN(...)			::HEngine::Log::GetClientLogger()->warn(__VA_ARGS__)
#define HE_ERROR(...)			::HEngine::Log::GetClientLogger()->error(__VA_ARGS__)
// #define HE_FATAL(...)			::HEngine::Log::GetClientLogger()->fatal(__VA_ARGS__)
