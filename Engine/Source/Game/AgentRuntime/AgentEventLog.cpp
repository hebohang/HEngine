#include "Game/AgentRuntime/AgentEventLog.h"

#include <algorithm>

namespace HEngine
{
	AgentEventLog::AgentEventLog(std::size_t capacity)
		: mCapacity(std::max<std::size_t>(capacity, 1))
	{
	}

	void AgentEventLog::Reset()
	{
		const std::scoped_lock lock(mMutex);
		mEntries.clear();
		mNextCursor = 1;
		mStartTime = std::chrono::steady_clock::now();
	}

	void AgentEventLog::Append(std::string_view level, std::string_view message)
	{
		Append("agent-runtime", "AgentRuntime", level, message);
	}

	void AgentEventLog::Append(
		std::string_view source,
		std::string_view logger,
		std::string_view level,
		std::string_view message)
	{
		const std::scoped_lock lock(mMutex);
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - mStartTime);
		const bool bTruncated = message.size() > MaxMessageBytes;
		mEntries.push_back(Entry{
			mNextCursor++,
			static_cast<std::uint64_t>(elapsed.count()),
			std::string(source),
			std::string(logger),
			std::string(level),
			std::string(message.substr(0, MaxMessageBytes)),
			bTruncated
		});
		while (mEntries.size() > mCapacity)
			mEntries.pop_front();
	}

	Json AgentEventLog::Read(std::uint64_t afterCursor, std::size_t limit) const
	{
		const std::scoped_lock lock(mMutex);
		limit = std::clamp<std::size_t>(limit, 1, 100);
		const std::uint64_t oldestCursor = mEntries.empty() ? mNextCursor : mEntries.front().Cursor;
		Json entries = Json::array();
		std::uint64_t nextCursor = afterCursor;

		for (const Entry& entry : mEntries)
		{
			if (entry.Cursor <= afterCursor)
				continue;
			if (entries.size() >= limit)
				break;

			entries.push_back({
				{ "cursor", entry.Cursor },
				{ "elapsedMs", entry.ElapsedMilliseconds },
				{ "source", entry.Source },
				{ "logger", entry.Logger },
				{ "level", entry.Level },
				{ "message", entry.Message }
			});
			if (entry.bTruncated)
				entries.back()["truncated"] = true;
			nextCursor = entry.Cursor;
		}

		return Json{
			{ "scope", "session-engine-client-agent-runtime" },
			{ "sources", Json::array({ "engine", "client", "agent-runtime" }) },
			{ "capacity", mCapacity },
			{ "maxMessageBytes", MaxMessageBytes },
			{ "oldestCursor", oldestCursor },
			{ "nextCursor", nextCursor },
			{ "truncated", afterCursor > 0 && afterCursor < oldestCursor &&
				oldestCursor - afterCursor > 1 },
			{ "entries", std::move(entries) }
		};
	}
}
