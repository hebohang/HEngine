#include "Hepch.h"
#include "Log.h"
#include "AsyncLogDispatch.h"
#include "LogSession.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/dist_sink.h>

#include <cstdio>
#include <filesystem>

namespace HEngine
{
	std::shared_ptr<spdlog::logger> Log::sCoreLogger;
	std::shared_ptr<spdlog::logger> Log::sClientLogger;
	static std::shared_ptr<spdlog::sinks::sink> sFileSink;

#if !WITH_EDITOR
	static Ref<spdlog::sinks::dist_sink_mt> sRuntimeCoreSinks, sRuntimeClientSinks;
	static Scope<AsyncLogDispatch> sRuntimeDispatch;
#endif
	
	void Log::Init()
	{
		spdlog::set_pattern("%^[%T] %n: %v%$");  // 设置样式，有适当的颜色、时间码、logger名以及实际的消息

#if !WITH_EDITOR
		if (sCoreLogger == nullptr || sClientLogger == nullptr)
		{
			sRuntimeCoreSinks = CreateRef<spdlog::sinks::dist_sink_mt>();
			sRuntimeClientSinks = CreateRef<spdlog::sinks::dist_sink_mt>();
			auto console = CreateRef<spdlog::sinks::stdout_color_sink_mt>();
			console->set_pattern("%^[%T] %n: %v%$");
			sRuntimeCoreSinks->add_sink(console);
			sRuntimeClientSinks->add_sink(console);
			sRuntimeDispatch = CreateScope<AsyncLogDispatch>();
			sCoreLogger = sRuntimeDispatch->CreateLogger("HEngine", sRuntimeCoreSinks);
			sClientLogger = sRuntimeDispatch->CreateLogger("Python", sRuntimeClientSinks);
			for (const auto& logger : {sCoreLogger, sClientLogger})
			{
				logger->set_level(spdlog::level::trace);
				logger->set_error_handler([](const std::string& error) {
					Log::WriteFallbackError("Asynchronous log sink failed", error);
				});
			}
		}
#else

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
#endif
	}

	void AddLogSink(const Ref<spdlog::logger>& logger, const Ref<spdlog::sinks::sink>& sink)
	{
		if (!logger || !sink) throw std::invalid_argument("Log sink registration requires a logger and sink");
#if !WITH_EDITOR
		if (logger == Log::GetCoreLogger()) sRuntimeCoreSinks->add_sink(sink);
		else if (logger == Log::GetClientLogger()) sRuntimeClientSinks->add_sink(sink);
		else throw std::invalid_argument("Log sink registration requires an engine-owned logger");
#else
		// Editor sink registration stays on the UI owner thread.
		logger->sinks().push_back(sink);
#endif
	}

	void ShutdownLogSession() noexcept
	{
#if !WITH_EDITOR
		if (!sRuntimeDispatch) return;
		try
		{
			// Retain a synchronous sink for rare late static-destructor diagnostics.
			// Normal frame producers have stopped before this lifecycle boundary.
			auto core = CreateRef<spdlog::logger>("HEngine", sRuntimeCoreSinks);
			auto client = CreateRef<spdlog::logger>("Python", sRuntimeClientSinks);
			core->set_level(spdlog::level::trace);
			client->set_level(spdlog::level::trace);
			sRuntimeDispatch->Drain();
			Log::GetCoreLogger() = std::move(core);
			Log::GetClientLogger() = std::move(client);
			sRuntimeDispatch.reset();
		}
		catch (const std::exception& error) { Log::WriteFallbackError("Log drain failed", error.what()); }
		catch (...) { Log::WriteFallbackError("Log drain failed", "unknown exception"); }
#endif
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
#if !WITH_EDITOR
			// Unlike editing logger.sinks(), dist_sink serializes this startup-time
			// registration against any console message already running on the worker.
			sRuntimeCoreSinks->add_sink(sFileSink);
			sRuntimeClientSinks->add_sink(sFileSink);
#else
			sCoreLogger->sinks().push_back(sFileSink);
			sClientLogger->sinks().push_back(sFileSink);
#endif
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
