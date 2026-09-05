#pragma once

#include "Game/AgentRuntime/AgentEventLog.h"

#include <spdlog/sinks/base_sink.h>

#include <mutex>
#include <memory>
#include <string>

namespace HEngine
{
	class AgentSpdlogSink final : public spdlog::sinks::base_sink<std::mutex>
	{
	public:
		AgentSpdlogSink(std::shared_ptr<AgentEventLog> eventLog, std::string source);

	protected:
		void sink_it_(const spdlog::details::log_msg& message) override;
		void flush_() override;

	private:
		std::shared_ptr<AgentEventLog> mEventLog;
		std::string mSource;
	};
}
