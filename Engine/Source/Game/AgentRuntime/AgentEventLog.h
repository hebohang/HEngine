#pragma once

#include "Runtime/Core/Meta/Json.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

namespace HEngine
{
	class AgentEventLog final
	{
	public:
		explicit AgentEventLog(std::size_t capacity = 256);

		void Reset();
		void Append(std::string_view level, std::string_view message);
		void Append(
			std::string_view source,
			std::string_view logger,
			std::string_view level,
			std::string_view message);
		[[nodiscard]] Json Read(std::uint64_t afterCursor, std::size_t limit) const;

	private:
		struct Entry
		{
			std::uint64_t Cursor = 0;
			std::uint64_t ElapsedMilliseconds = 0;
			std::string Source;
			std::string Logger;
			std::string Level;
			std::string Message;
			bool bTruncated = false;
		};

		static constexpr std::size_t MaxMessageBytes = 1536;
		std::size_t mCapacity;
		std::uint64_t mNextCursor = 1;
		std::chrono::steady_clock::time_point mStartTime = std::chrono::steady_clock::now();
		std::deque<Entry> mEntries;
		mutable std::mutex mMutex;
	};
}
