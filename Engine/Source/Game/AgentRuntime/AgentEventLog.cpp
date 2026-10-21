#include "Game/AgentRuntime/AgentEventLog.h"

#include <algorithm>

namespace HEngine
{
	AgentEventLog::AgentEventLog(std::size_t capacity)
		: mCapacity(std::max<std::size_t>(capacity, 1)), mEntries(mCapacity)
	{
		for (Entry& entry : mEntries)
			entry.Message.reserve(MaxMessageBytes);
	}

	void AgentEventLog::Reset()
	{
		const std::scoped_lock lock(mMutex);
		// Reset logical contents while retaining reusable slot storage.
		for (Entry& entry : mEntries)
		{
			entry.Cursor = 0;
			entry.ElapsedMilliseconds = 0;
			entry.Source.clear();
			entry.Logger.clear();
			entry.Level.clear();
			entry.Message.clear();
			entry.bTruncated = false;
		}
		mHead = 0;
		mCount = 0;
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
		Entry& entry = mEntries[(mHead + mCount) % mCapacity];
		// These metadata fields intentionally remain unbounded, as before.
		// Reserve before mutation so allocation failure leaves readable entries
		// intact. Only the existing 1536-byte message bound is preallocated.
		entry.Source.reserve(source.size());
		entry.Logger.reserve(logger.size());
		entry.Level.reserve(level.size());
		entry.Source.assign(source);
		entry.Logger.assign(logger);
		entry.Level.assign(level);
		entry.Message.assign(message.substr(0, MaxMessageBytes));
		entry.Cursor = mNextCursor++;
		entry.ElapsedMilliseconds = static_cast<std::uint64_t>(elapsed.count());
		entry.bTruncated = bTruncated;
		if (mCount < mCapacity)
			++mCount;
		else
			mHead = (mHead + 1) % mCapacity;
	}

	Json AgentEventLog::Read(std::uint64_t afterCursor, std::size_t limit) const
	{
		const std::scoped_lock lock(mMutex);
		limit = std::clamp<std::size_t>(limit, 1, 100);
		const std::uint64_t oldestCursor = mCount == 0 ? mNextCursor : mEntries[mHead].Cursor;
		Json entries = Json::array();
		std::uint64_t nextCursor = afterCursor;

		for (std::size_t index = 0; index < mCount; ++index)
		{
			const Entry& entry = mEntries[(mHead + index) % mCapacity];
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
