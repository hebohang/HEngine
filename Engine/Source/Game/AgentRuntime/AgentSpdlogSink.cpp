#include "Game/AgentRuntime/AgentSpdlogSink.h"

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>

#include <string>
#include <utility>

namespace HEngine
{
	AgentSpdlogSink::AgentSpdlogSink(std::shared_ptr<AgentEventLog> eventLog, std::string source)
		: mEventLog(std::move(eventLog)), mSource(std::move(source))
	{
	}

	void AgentSpdlogSink::sink_it_(const spdlog::details::log_msg& message)
	{
		const spdlog::string_view_t level = spdlog::level::to_string_view(message.level);
		mEventLog->Append(
			mSource,
			std::string_view(message.logger_name.data(), message.logger_name.size()),
			std::string_view(level.data(), level.size()),
			std::string_view(message.payload.data(), message.payload.size()));
	}

	void AgentSpdlogSink::flush_()
	{
	}
}
