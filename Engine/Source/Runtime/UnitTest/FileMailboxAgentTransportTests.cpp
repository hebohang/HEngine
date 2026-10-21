#include "Hepch.h"
#include "Runtime/UnitTest/FileMailboxAgentTransportTests.h"
#include "Game/AgentRuntime/FileMailboxAgentTransport.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, const char* message)
        {
            if (!condition) throw std::runtime_error(message);
        }

        std::string ReadBytes(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            Require(stream.is_open(), "mailbox response must exist");
            return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
        }

        void WriteBytes(const std::filesystem::path& path, const std::string& contents)
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            stream.close();
            Require(!stream.fail(), "mailbox fixture write failed");
        }

        struct MailboxFixture
        {
            std::filesystem::path Directory;
            FileMailboxAgentTransport Transport;

        private:
            std::filesystem::path mTemporaryRoot;
            std::filesystem::path mCreatedLeaf;
            bool bCreatedExclusively = false;

        public:

            explicit MailboxFixture(std::size_t limit)
            {
                try
                {
                    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
                    mTemporaryRoot = std::filesystem::canonical(std::filesystem::temp_directory_path());
                    Require(mTemporaryRoot.is_absolute(), "temporary root must be absolute");
                    for (unsigned attempt = 0; attempt < 100; ++attempt)
                    {
                        mCreatedLeaf = "he-mailbox-contract-" + std::to_string(stamp) + "-" + std::to_string(attempt);
                        Directory = mTemporaryRoot / mCreatedLeaf;
                        if (std::filesystem::create_directory(Directory))
                        {
                            bCreatedExclusively = true;
                            break;
                        }
                    }
                    Require(bCreatedExclusively, "failed to create exclusive mailbox fixture");
                    AgentTransportConfig config;
                    config.SessionDirectory = Directory;
                    config.MaxResponseBytes = limit;
                    Require(Transport.Start(config, Json::object()), "real mailbox transport failed to start");
                }
                catch (...)
                {
                    Cleanup();
                    throw;
                }
            }

            ~MailboxFixture() { Cleanup(); }

            void Cleanup() noexcept
            {
                try
                {
                    try { Transport.Stop(Json::object()); } catch (...) {}
                    if (!bCreatedExclusively || !Directory.is_absolute() || mCreatedLeaf.empty() ||
                        Directory.filename() != mCreatedLeaf || Directory.parent_path() != mTemporaryRoot)
                        return;
                    std::error_code error;
                    const auto status = std::filesystem::symlink_status(Directory, error);
                    if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
                        return;
                    const auto resolved = std::filesystem::canonical(Directory, error);
                    if (error || resolved != Directory || resolved.parent_path() != mTemporaryRoot ||
                        resolved.filename() != mCreatedLeaf)
                        return;
                    std::filesystem::remove_all(resolved, error);
                    if (!error) bCreatedExclusively = false;
                }
                catch (...) {} // Preserve suspicious/replaced paths rather than risking deletion.
            }

            AgentTransportMessage Request(const std::string& handle)
            {
                const Json request = { {"protocol", 1}, {"id", handle},
                    {"method", "session.status"}, {"params", Json::object()} };
                WriteBytes(Directory / "requests" / (handle + ".request.json"), request.dump());
                auto messages = Transport.Poll(1);
                Require(messages.size() == 1 && messages.front().Handle == handle &&
                    !messages.front().HasTransportError(), "real Poll must claim and decode request");
                Require(std::filesystem::exists(Directory / "processing" / (handle + ".processing.json")),
                    "request must remain processing before successful reply");
                return std::move(messages.front());
            }

            std::filesystem::path ResponsePath(const std::string& handle) const
            {
                return Directory / "responses" / (handle + ".response.json");
            }

            void RequireArchived(const std::string& handle) const
            {
                Require(std::filesystem::exists(Directory / "processed" / (handle + ".processed.json")) &&
                    !std::filesystem::exists(Directory / "processing" / (handle + ".processing.json")),
                    "only successful response archives request");
            }
        };
    }

    void FileMailboxAgentTransportContract()
    {
        constexpr std::size_t limit = 256;
        MailboxFixture fixture(limit);
        const std::string oversizedError =
            R"({"error":{"code":"response_too_large","message":"Response exceeds the session byte limit."},"id":null,"ok":false,"protocol":1})";
        for (const std::size_t size : { limit - 1, limit, limit + 1 })
        {
            const auto handle = "boundary-" + std::to_string(size);
            auto request = fixture.Request(handle);
            const Json response = std::string(size - 2, 'a');
            Require(fixture.Transport.Respond(request, response), "boundary response publication failed");
            const auto contents = ReadBytes(fixture.ResponsePath(handle));
            if (size <= limit)
                Require(contents == "\"" + std::string(size - 2, 'a') + "\"" && contents.size() == size,
                    "response must preserve bytes at or below exact limit");
            else
                Require(Json::parse(contents) == Json::parse(oversizedError), "over limit must use original error envelope");
            fixture.RequireArchived(handle);
        }

        auto encoded = fixture.Request("encoded");
        const std::string value = std::string("quote\" slash\\ newline\n nul") + std::string(1, '\0') +
            " utf8:" + "\xC3\xA9" + " invalid:" + std::string(1, static_cast<char>(0xFF));
        const Json response = { {"text", value} };
        Require(fixture.Transport.Respond(encoded, response), "UTF8 replacement response failed");
        const auto contents = ReadBytes(fixture.ResponsePath("encoded"));
        const std::string expected = "{\"text\":\"quote\\\" slash\\\\ newline\\n nul\\u0000 utf8:" +
            std::string("\xC3\xA9") + " invalid:" + "\xEF\xBF\xBD" + "\"}";
        Require(contents == expected, "wire bytes must preserve escaping and replace invalid UTF8");
        fixture.RequireArchived("encoded");

        // Persistent serializer must reset after large replies and overflow, per transport.
        MailboxFixture independent(64);
        for (unsigned cycle = 0; cycle < 4; ++cycle)
        {
            for (const std::size_t length : { 1u, 254u, 4096u, 2u })
            {
                const auto handle = "reuse-" + std::to_string(cycle) + "-" + std::to_string(length);
                auto request = fixture.Request(handle);
                Require(fixture.Transport.Respond(request, Json(std::string(length, 'r'))), "reuse response failed");
                const auto bytes = ReadBytes(fixture.ResponsePath(handle));
                Require(length + 2 <= limit ? bytes == "\"" + std::string(length, 'r') + "\"" :
                    Json::parse(bytes) == Json::parse(oversizedError), "serializer leaked prior response bytes/state");
                fixture.RequireArchived(handle);
                if (length > limit)
                {
                    const auto afterHandle = handle + "-utf8";
                    auto afterOverflow = fixture.Request(afterHandle);
                    Require(fixture.Transport.Respond(afterOverflow, response), "UTF8 reply after overflow failed");
                    Require(ReadBytes(fixture.ResponsePath(afterHandle)) == expected,
                        "overflow must not retain UTF8 decoder or escape-buffer state");
                    fixture.RequireArchived(afterHandle);
                }
                auto other = independent.Request(handle);
                Require(independent.Transport.Respond(other, Json("independent")), "second instance response failed");
                Require(ReadBytes(independent.ResponsePath(handle)) == "\"independent\"" &&
                    ReadBytes(fixture.ResponsePath(handle)) == bytes, "transport instances share response storage");
                independent.RequireArchived(handle);
            }
        }

        auto collision = fixture.Request("collision");
        WriteBytes(fixture.ResponsePath("collision"), "existing-response");
        Require(!fixture.Transport.Respond(collision, Json::object()), "existing response must not be overwritten");
        Require(ReadBytes(fixture.ResponsePath("collision")) == "existing-response", "collision changed existing bytes");
        Require(std::filesystem::exists(fixture.Directory / "processing/collision.processing.json") &&
            !std::filesystem::exists(fixture.Directory / "processed/collision.processed.json"),
            "failed publication must not archive request");
        for (const auto& entry : std::filesystem::directory_iterator(fixture.Directory / "responses"))
            Require(entry.path().filename().string().find(".tmp.") == std::string::npos,
                "response collision must clean temporary file");

        AgentTransportMessage invalid;
        invalid.Handle = "../escape";
        Require(!fixture.Transport.Respond(invalid, Json::object()), "path traversal handle must be rejected");
        invalid.Handle.clear();
        Require(!fixture.Transport.Respond(invalid, Json::object()), "empty handle must be rejected");
        invalid.Handle = std::string(AgentProtocol::MaxRequestIdLength + 1, 'a');
        Require(!fixture.Transport.Respond(invalid, Json::object()), "oversized handle must be rejected");
        auto stopped = fixture.Request("stopped");
        fixture.Transport.Stop(Json::object());
        Require(!fixture.Transport.Respond(stopped, Json::object()) &&
            !std::filesystem::exists(fixture.ResponsePath("stopped")), "stopped transport cannot publish");

        // Restart this exact transport into an exclusively-created child directory.
        // The fixture's validated root cleanup owns the child as well.
        const auto previousStatus = ReadBytes(fixture.Directory / "status.json");
        const auto restartedDirectory = fixture.Directory / "restarted-session";
        Require(std::filesystem::create_directory(restartedDirectory), "restart directory must be exclusive");
        AgentTransportConfig restartedConfig;
        restartedConfig.SessionDirectory = restartedDirectory;
        restartedConfig.MaxResponseBytes = limit;
        Require(fixture.Transport.Start(restartedConfig, Json::object()), "same transport must restart in new directory");
        const Json restartedRequest = {{"protocol", 1}, {"id", "restarted"},
            {"method", "session.status"}, {"params", Json::object()}};
        WriteBytes(restartedDirectory / "requests/restarted.request.json", restartedRequest.dump());
        auto restartedMessages = fixture.Transport.Poll(1);
        Require(restartedMessages.size() == 1 && restartedMessages.front().Handle == "restarted" &&
            !restartedMessages.front().HasTransportError(), "restarted transport must claim new request");
        Require(fixture.Transport.Respond(restartedMessages.front(), response), "restarted UTF8 response failed");
        Require(ReadBytes(restartedDirectory / "responses/restarted.response.json") == expected,
            "restarted serializer must preserve byte contract");
        Require(std::filesystem::exists(restartedDirectory / "processed/restarted.processed.json") &&
            !std::filesystem::exists(restartedDirectory / "processing/restarted.processing.json"),
            "restarted request must archive in new session");
        fixture.Transport.Stop(Json::object());
        Require(!fixture.Transport.Respond(restartedMessages.front(), response), "second Stop disables responses");
        Require(ReadBytes(fixture.Directory / "status.json") == previousStatus &&
            !std::filesystem::exists(fixture.ResponsePath("restarted")), "restart must not publish into old session");

        MailboxFixture tiny(2); // {} descriptor fits; established error envelope deliberately exceeds this limit.
        auto tinyRequest = tiny.Request("tiny");
        Require(tiny.Transport.Respond(tinyRequest, Json("x")), "tiny limit still publishes established error envelope");
        const auto tinyContents = ReadBytes(tiny.ResponsePath("tiny"));
        Require(tinyContents.size() > 2 && Json::parse(tinyContents) == Json::parse(oversizedError),
            "preserve existing small-budget error-envelope behavior");
        tiny.RequireArchived("tiny");
    }
}
