#include "Hepch.h"
#include "Log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdio>
#include <filesystem>

namespace HEngine
{
	std::shared_ptr<spdlog::logger> Log::sCoreLogger;
	std::shared_ptr<spdlog::logger> Log::sClientLogger;
	static std::shared_ptr<spdlog::sinks::sink> sFileSink;
	
	void Log::Init()
	{
		spdlog::set_pattern("%^[%T] %n: %v%$");  // 设置样式，有适当的颜色、时间码、logger名以及实际的消息

		if (sCoreLogger == nullptr)
		{
			// may define in Editor
			sCoreLogger = spdlog::stdout_color_mt("HEngine");
			sCoreLogger->set_level(spdlog::level::trace);
		}

		if (sClientLogger == nullptr)
		{
			sClientLogger = spdlog::stdout_color_mt("Python");
			sClientLogger->set_level(spdlog::level::trace);
		}
	}

	bool Log::InstallFileSink(const std::filesystem::path& path) noexcept
	{
		try
		{
			if (path.empty())
				return false;
			if (sCoreLogger == nullptr || sClientLogger == nullptr)
				Init();
			if (sFileSink != nullptr)
				return true;

			std::error_code error;
			std::filesystem::create_directories(path.parent_path(), error);
			if (error)
				return false;

			sFileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
				path.native(), true);
			sFileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %n [%l] %v");
			sCoreLogger->sinks().push_back(sFileSink);
			sClientLogger->sinks().push_back(sFileSink);
			sCoreLogger->flush_on(spdlog::level::err);
			sClientLogger->flush_on(spdlog::level::err);
			return true;
		}
		catch (...)
		{
			sFileSink.reset();
			return false;
		}
	}

	void Log::WriteFallbackError(std::string_view message, std::string_view details) noexcept
	{
		std::fwrite(message.data(), sizeof(char), message.size(), stderr);
		if (!details.empty())
		{
			std::fwrite(": ", sizeof(char), 2, stderr);
			std::fwrite(details.data(), sizeof(char), details.size(), stderr);
		}
		std::fputc('\n', stderr);
		std::fflush(stderr);
	}
}
