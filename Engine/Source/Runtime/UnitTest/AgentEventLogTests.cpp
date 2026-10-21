#include "Hepch.h"
#include "Runtime/UnitTest/AgentEventLogTests.h"
#include "Game/AgentRuntime/AgentEventLog.h"
#include <stdexcept>
#include <string>

namespace HEngine::Tests
{
    void AgentEventLogRingContract()
    {
        const auto require = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        AgentEventLog log(3);
        for (int i = 1; i <= 8; ++i) log.Append("info", std::to_string(i));
        auto result = log.Read(0, 100);
        require(result.at("capacity") == 3 && result.at("maxMessageBytes") == 1536,
            "event log capacity contract");
        require(result.at("scope") == "session-engine-client-agent-runtime" &&
            result.at("sources") == Json::array({"engine", "client", "agent-runtime"}), "scope metadata");
        require(result.at("oldestCursor") == 6 && result.at("nextCursor") == 8 &&
            result.at("truncated") == false && result.at("entries").size() == 3, "wrapped cursor range");
        for (int i = 0; i < 3; ++i)
        {
            const auto& entry = result.at("entries").at(i);
            require(entry.at("cursor") == i + 6 && entry.at("message") == std::to_string(i + 6), "wrap ordering");
            require(entry.at("source") == "agent-runtime" && entry.at("logger") == "AgentRuntime" &&
                entry.at("level") == "info" && entry.at("elapsedMs").is_number_unsigned(), "default append metadata");
        }
        require(log.Read(4, 100).at("truncated") == true, "dropped cursor signals truncation");
        require(log.Read(5, 100).at("truncated") == false, "adjacent cursor has no missing event");
        for (uint64_t cursor : {uint64_t(8), uint64_t(99)})
        {
            auto empty = log.Read(cursor, 100);
            require(empty.at("entries").empty() && empty.at("nextCursor") == cursor, "empty preserves caller cursor");
        }
        require(log.Read(0, 0).at("entries").size() == 1, "zero limit clamps to one");
        log.Reset();
        result = log.Read(0, 100);
        require(result.at("entries").empty() && result.at("oldestCursor") == 1 &&
            result.at("nextCursor") == 0 && result.at("truncated") == false, "reset clears ring and cursors");
        require(log.Read(99, 100).at("nextCursor") == 99, "reset empty high cursor preserved");
        log.Append("warn", "fresh");
        result = log.Read(0, 100);
        require(result.at("entries").size() == 1 && result.at("entries").at(0).at("cursor") == 1 &&
            result.at("entries").at(0).at("message") == "fresh", "reset begins a new epoch");

        AgentEventLog one(0);
        one.Append("info", "first"); one.Append("info", "second");
        result = one.Read(0, 100);
        require(result.at("capacity") == 1 && result.at("entries").size() == 1 &&
            result.at("entries").at(0).at("message") == "second", "zero capacity clamps to one");
        const std::string source(1800, 's'), logger(1900, 'l'), level(2000, 'v');
        for (std::size_t length : {1535u, 1536u, 1537u})
        {
            const std::string message(length, 'm');
            one.Append(source, logger, level, message);
            const auto entry = one.Read(0, 1).at("entries").at(0);
            require(entry.at("source") == source && entry.at("logger") == logger && entry.at("level") == level,
                "metadata remains unbounded and exact");
            require(entry.at("message") == message.substr(0, 1536) &&
                entry.contains("truncated") == (length > 1536), "message byte limit and truncation key");
            if (length > 1536) require(entry.at("truncated") == true, "truncation flag value");
        }
        const std::string embedded("a\0b", 3);
        one.Append("x", "y", "z", embedded);
        auto entry = one.Read(0, 1).at("entries").at(0);
        require(entry.at("message").get<std::string>() == embedded && !entry.contains("truncated") &&
            entry.at("source") == "x", "overwrite clears stale truncation and preserves embedded NUL");
        const std::string utf8Boundary = std::string(1534, 'a') + "\xC3\xA9!";
        one.Append("info", utf8Boundary);
        entry = one.Read(0, 1).at("entries").at(0);
        require(entry.at("message").get<std::string>() == utf8Boundary.substr(0, 1536) &&
            entry.at("truncated") == true, "UTF8 message length uses bytes at a complete codepoint boundary");

        AgentEventLog defaults;
        for (int i = 1; i <= 300; ++i) defaults.Append("info", std::to_string(i));
        result = defaults.Read(0, 101);
        require(result.at("capacity") == 256 && result.at("oldestCursor") == 45 &&
            result.at("entries").size() == 100 && result.at("nextCursor") == 144, "default capacity and upper clamp");
        result = defaults.Read(144, 100);
        require(result.at("entries").size() == 100 && result.at("nextCursor") == 244, "second page");
        result = defaults.Read(244, 100);
        require(result.at("entries").size() == 56 && result.at("entries").at(0).at("cursor") == 245 &&
            result.at("nextCursor") == 300, "final page without duplicates");
    }
}
