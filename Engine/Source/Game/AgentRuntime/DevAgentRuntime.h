#pragma once

#include "Game/AgentRuntime/AgentCommandRegistry.h"
#include "Game/AgentRuntime/AgentCaptureController.h"
#include "Game/AgentRuntime/AgentEventLog.h"
#include "Game/AgentRuntime/AgentInputController.h"
#include "Game/AgentRuntime/AgentTransport.h"
#include "Runtime/Core/Base/Base.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace HEngine
{
	class Level;
	struct RenderGraphDiagnosticSnapshot;

	struct DevAgentRuntimeConfig
	{
		std::filesystem::path SessionDirectory;
		std::string Token;
		std::string SceneName;
		std::size_t MaxRequestBytes = AgentProtocol::MaxRequestBytes;
		std::size_t MaxResponseBytes = AgentProtocol::MaxResponseBytes;
		std::size_t MaxRequestsPerTick = AgentProtocol::MaxRequestsPerTick;
		bool bStartPaused = false;
	};

	class DevAgentRuntime final
	{
	public:
		explicit DevAgentRuntime(Scope<IAgentTransport> transport = nullptr);
		~DevAgentRuntime();

		DevAgentRuntime(const DevAgentRuntime&) = delete;
		DevAgentRuntime& operator=(const DevAgentRuntime&) = delete;

		[[nodiscard]] bool Start(const DevAgentRuntimeConfig& config, Ref<Level> level);
		void Stop();
		void Tick();
		void SetLevel(Ref<Level> level, std::string sceneName = {});
		void PrepareSimulationFrame(bool bCanAdvance = true);
		void CompleteSimulationFrame();
		void ProcessPendingCapture();
		[[nodiscard]] bool ConsumeStopRequested();

		[[nodiscard]] bool IsRunning() const { return mRunning; }
		[[nodiscard]] const std::string& GetSessionId() const { return mSessionId; }

	private:
		enum class RuntimeControlMode
		{
			Observe,
			Paused,
			Running
		};

		void RegisterCommands();
		void AttachLogSinks();
		void PublishStatus(std::string_view state);
		[[nodiscard]] Json BuildStatus(std::string_view state) const;
		[[nodiscard]] Json BuildWorldSummary() const;
		[[nodiscard]] Json BuildWorldList(const Json& params) const;
		[[nodiscard]] Json BuildWorldEntity(const Json& params) const;
		[[nodiscard]] Json BuildRenderGraphSnapshotPage(const Json& params);
		[[nodiscard]] Json BuildRuntimeControlState() const;
		[[nodiscard]] Json ReadLogs(const Json& params) const;
		[[nodiscard]] static std::string GenerateRandomHexToken(std::size_t bytes);
		[[nodiscard]] static bool IsValidToken(std::string_view token);

		Scope<IAgentTransport> mTransport;
		AgentCommandRegistry mRegistry;
		AgentCaptureController mCaptureController;
		Ref<AgentEventLog> mEventLog;
		AgentInputController mInputController;
		DevAgentRuntimeConfig mConfig;
		Ref<Level> mLevel;
		std::string mToken;
		std::string mSessionId;
		Scope<RenderGraphDiagnosticSnapshot> mRenderGraphSnapshot;
		std::string mRenderGraphSnapshotId;
		std::uint64_t mRenderGraphSnapshotFrame = 0;
		std::uint64_t mNextRenderGraphSnapshotId = 1;
		std::chrono::steady_clock::time_point mStartTime;
		std::chrono::steady_clock::time_point mLastStatusPublish;
		std::size_t mPendingSimulationSteps = 0;
		std::uint64_t mCompletedSimulationSteps = 0;
		RuntimeControlMode mRuntimeControlMode = RuntimeControlMode::Observe;
		bool mStepInProgress = false;
		bool mStopRequested = false;
		bool mRunning = false;
	};
}
