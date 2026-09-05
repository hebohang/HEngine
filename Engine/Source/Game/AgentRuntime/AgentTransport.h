#pragma once

#include "Game/AgentRuntime/AgentProtocol.h"

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace HEngine
{
	struct AgentTransportConfig
	{
		std::filesystem::path SessionDirectory;
		std::size_t MaxRequestBytes = AgentProtocol::MaxRequestBytes;
		std::size_t MaxResponseBytes = AgentProtocol::MaxResponseBytes;
	};

	class IAgentTransport
	{
	public:
		virtual ~IAgentTransport() = default;

		[[nodiscard]] virtual std::string_view GetName() const = 0;
		[[nodiscard]] virtual bool Start(const AgentTransportConfig& config, const Json& sessionDescriptor) = 0;
		virtual void Stop(const Json& finalStatus) = 0;
		[[nodiscard]] virtual std::vector<AgentTransportMessage> Poll(std::size_t maxMessages) = 0;
		[[nodiscard]] virtual bool Respond(const AgentTransportMessage& message, const Json& response) = 0;
		[[nodiscard]] virtual bool PublishStatus(const Json& status) = 0;
	};
}
