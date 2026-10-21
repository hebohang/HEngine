#pragma once

#include "Game/AgentRuntime/AgentTransport.h"

#include "Runtime/Core/Base/Base.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace HEngine
{
	class FileMailboxAgentTransport final : public IAgentTransport
	{
	public:
		FileMailboxAgentTransport();
		~FileMailboxAgentTransport() override;
		[[nodiscard]] std::string_view GetName() const override { return "file-mailbox"; }
		[[nodiscard]] bool Start(const AgentTransportConfig& config, const Json& sessionDescriptor) override;
		void Stop(const Json& finalStatus) override;
		[[nodiscard]] std::vector<AgentTransportMessage> Poll(std::size_t maxMessages) override;
		[[nodiscard]] bool Respond(const AgentTransportMessage& message, const Json& response) override;
		[[nodiscard]] bool PublishStatus(const Json& status) override;

	private:
		[[nodiscard]] bool WriteAtomically(
			const std::filesystem::path& target,
			std::string_view contents,
			bool bReplaceExisting);
		[[nodiscard]] static bool IsSafeHandle(std::string_view handle);
		[[nodiscard]] static bool HasRequestSuffix(std::string_view filename);
		[[nodiscard]] static std::string GetHandle(std::string_view filename);
		void ArchiveRequest(std::string_view handle);

		struct ResponseWriter;
		Scope<ResponseWriter> mResponseWriter;
		AgentTransportConfig mConfig;
		std::filesystem::path mRequestsDirectory;
		std::filesystem::path mResponsesDirectory;
		std::filesystem::path mProcessingDirectory;
		std::filesystem::path mProcessedDirectory;
		std::filesystem::path mRejectedDirectory;
		std::unordered_map<std::string, std::size_t> mRequestReadFailures;
		std::uint64_t mTempCounter = 0;
		bool mStarted = false;
	};
}
