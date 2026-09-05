#include "Game/AgentRuntime/DevAgentRuntime.h"

#include "Game/AgentRuntime/FileMailboxAgentTransport.h"
#include "Game/AgentRuntime/AgentSpdlogSink.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "_generated/serializer/all_serializer.h"

#if HE_ENABLE_GM
#include "Runtime/DevTools/GM/GMService.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace HEngine
{
	namespace
	{
		// Sinks intentionally live until logger teardown. A process therefore owns at most
		// one Agent session; start a fresh process instead of reusing an old runtime.
		std::atomic_bool sAgentSessionClaimedForProcess = false;
		constexpr std::size_t MaxWorldPageSize = 100;
		constexpr std::size_t MaxQueuedSimulationSteps = 120;
		constexpr std::size_t MaxProjectedComponents = 16;
		constexpr std::size_t MaxSerializedComponentBytes = 32 * 1024;
		constexpr std::size_t MaxProjectedComponentsBytes = 128 * 1024;
		constexpr std::size_t MaxRenderGraphPageSize = 100;
		constexpr std::size_t MaxRenderGraphTextBytes = 1024;
		constexpr std::size_t MaxSettingsPatchEntries = 128;

		void RequireNoParams(const Json& params, std::string_view method)
		{
			if (!params.empty())
				throw std::invalid_argument(std::string(method) + " does not accept parameters.");
		}

		struct WorldPage
		{
			std::uint64_t Offset = 0;
			std::size_t Limit = 50;
		};

		[[nodiscard]] WorldPage ParseWorldPage(const Json& params)
		{
			for (const auto& [name, value] : params.items())
			{
				(void)value;
				if (name != "offset" && name != "limit")
					throw std::invalid_argument("world.list contains an unknown parameter.");
			}

			WorldPage page;
			if (params.contains("offset"))
			{
				if (!params["offset"].is_number_unsigned())
					throw std::invalid_argument("world.list offset must be an unsigned integer.");
				page.Offset = params["offset"].get<std::uint64_t>();
			}
			if (params.contains("limit"))
			{
				if (!params["limit"].is_number_unsigned())
					throw std::invalid_argument("world.list limit must be an unsigned integer.");
				const std::uint64_t requestedLimit = params["limit"].get<std::uint64_t>();
				if (requestedLimit == 0 || requestedLimit > MaxWorldPageSize)
					throw std::invalid_argument("world.list limit must be in the range 1..100.");
				page.Limit = static_cast<std::size_t>(requestedLimit);
			}
			return page;
		}

		[[nodiscard]] std::string Utf8Prefix(std::string_view text, std::size_t maxBytes)
		{
			std::size_t cursor = 0;
			std::size_t validEnd = 0;
			while (cursor < text.size() && cursor < maxBytes)
			{
				const unsigned char first = static_cast<unsigned char>(text[cursor]);
				std::size_t width = 1;
				if ((first & 0xE0u) == 0xC0u)
					width = 2;
				else if ((first & 0xF0u) == 0xE0u)
					width = 3;
				else if ((first & 0xF8u) == 0xF0u)
					width = 4;
				if (cursor + width > text.size() || cursor + width > maxBytes)
					break;
				cursor += width;
				validEnd = cursor;
			}
			return std::string(text.substr(0, validEnd));
		}

		void RequireOnlyParams(
			const Json& params,
			std::string_view method,
			std::initializer_list<std::string_view> allowed)
		{
			for (const auto& [name, value] : params.items())
			{
				(void)value;
				if (std::ranges::find(allowed, std::string_view(name)) == allowed.end())
				{
					throw std::invalid_argument(
						std::string(method) + " contains an unknown parameter: " + name);
				}
			}
		}

		[[nodiscard]] SettingsLayer ParseMutableSettingsLayer(
			const Json& params,
			std::string_view method)
		{
			if (!params.contains("layer"))
				return SettingsLayer::Session;
			if (!params["layer"].is_string())
			{
				throw std::invalid_argument(
					std::string(method) + " layer must be a string.");
			}

			const std::string& layer = params["layer"].get_ref<const std::string&>();
			if (layer == "Session" || layer == "session")
				return SettingsLayer::Session;
			if (layer == "Project" || layer == "project")
				return SettingsLayer::Project;
			if (layer == "User" || layer == "user")
				return SettingsLayer::User;
			throw std::invalid_argument(
				std::string(method) + " layer must be Session, Project, or User.");
		}

		void ReadSettingsBase(
			const Json& params,
			std::string_view method,
			std::uint64_t& revision,
			std::string& hash)
		{
			if (!params.contains("baseRevision") ||
				!params["baseRevision"].is_number_unsigned())
			{
				throw std::invalid_argument(
					std::string(method) + " baseRevision must be an unsigned integer.");
			}
			if (!params.contains("baseHash") || !params["baseHash"].is_string())
			{
				throw std::invalid_argument(
					std::string(method) + " baseHash must be a string.");
			}
			revision = params["baseRevision"].get<std::uint64_t>();
			hash = params["baseHash"].get<std::string>();
			if (hash.empty() || hash.size() > 128)
			{
				throw std::invalid_argument(
					std::string(method) + " baseHash must contain 1..128 bytes.");
			}
		}

		[[nodiscard]] Json ReadSettingsPatch(
			const Json& params,
			std::string_view method)
		{
			if (!params.contains("patch") || !params["patch"].is_object())
			{
				throw std::invalid_argument(
					std::string(method) + " patch must be an object keyed by stable property ID.");
			}
			if (params["patch"].size() > MaxSettingsPatchEntries)
			{
				throw std::invalid_argument(
					std::string(method) + " patch exceeds 128 properties.");
			}
			for (const auto& [propertyId, value] : params["patch"].items())
			{
				(void)value;
				if (propertyId.empty() || propertyId.size() > 128 ||
					!std::ranges::all_of(propertyId, [](unsigned char character)
					{
						return std::isalnum(character) != 0 || character == '.' ||
							character == '_' || character == '-';
					}))
				{
					throw std::invalid_argument(
						std::string(method) + " patch contains an invalid property ID.");
				}
			}
			return params["patch"];
		}

		[[nodiscard]] bool ReadExplicitPersistence(
			const Json& params,
			SettingsLayer layer,
			std::string_view method)
		{
			const bool bHasPersist = params.contains("persist");
			if (bHasPersist && !params["persist"].is_boolean())
			{
				throw std::invalid_argument(
					std::string(method) + " persist must be a boolean.");
			}
			const bool bPersist = bHasPersist && params["persist"].get<bool>();
			if (layer == SettingsLayer::Session)
			{
				if (bPersist)
				{
					throw std::invalid_argument(
						std::string(method) + " cannot persist the Session layer.");
				}
				return false;
			}
			if (!bHasPersist || !bPersist)
			{
				throw std::invalid_argument(
					std::string(method) +
					" requires persist=true for Project or User writes.");
			}
			return true;
		}

		[[nodiscard]] RenderSettingsDraft BuildSettingsDraft(
			const Json& params,
			std::string_view method,
			bool bRequirePatch)
		{
			RenderSettingsDraft draft;
			draft.TargetLayer = ParseMutableSettingsLayer(params, method);
			ReadSettingsBase(
				params, method, draft.BaseRevision, draft.BaseHash);
			if (bRequirePatch)
				draft.Patch = ReadSettingsPatch(params, method);
			return draft;
		}

		[[nodiscard]] const char* RenderGraphQueueId(
			RenderGraphQueue queue) noexcept
		{
			switch (queue)
			{
			case RenderGraphQueue::Graphics: return "graphics";
			case RenderGraphQueue::Compute: return "compute";
			case RenderGraphQueue::Copy: return "copy";
			}
			return "unknown";
		}

		[[nodiscard]] const char* RenderGraphAccessId(
			RenderGraphAccess access) noexcept
		{
			switch (access)
			{
			case RenderGraphAccess::Read: return "read";
			case RenderGraphAccess::Write: return "write";
			case RenderGraphAccess::ReadWrite: return "readWrite";
			}
			return "unknown";
		}

		[[nodiscard]] const char* RenderGraphLifetimeId(
			RenderGraphResourceLifetime lifetime) noexcept
		{
			switch (lifetime)
			{
			case RenderGraphResourceLifetime::Imported: return "imported";
			case RenderGraphResourceLifetime::Persistent: return "persistent";
			case RenderGraphResourceLifetime::Transient: return "transient";
			}
			return "unknown";
		}

		[[nodiscard]] const char* RenderGraphResourceKindId(
			RenderGraphDiagnosticResourceKind kind) noexcept
		{
			switch (kind)
			{
			case RenderGraphDiagnosticResourceKind::Texture: return "texture";
			case RenderGraphDiagnosticResourceKind::Buffer: return "buffer";
			case RenderGraphDiagnosticResourceKind::AccelerationStructure:
				return "accelerationStructure";
			}
			return "unknown";
		}

		[[nodiscard]] Json SerializeRenderGraphUsage(RenderGraphUsage usage)
		{
			constexpr std::array flags = {
				std::pair{ RenderGraphUsage::RenderTarget, "renderTarget" },
				std::pair{ RenderGraphUsage::DepthRead, "depthRead" },
				std::pair{ RenderGraphUsage::DepthWrite, "depthWrite" },
				std::pair{ RenderGraphUsage::ShaderResource, "shaderResource" },
				std::pair{ RenderGraphUsage::UnorderedAccess, "unorderedAccess" },
				std::pair{ RenderGraphUsage::CopySource, "copySource" },
				std::pair{ RenderGraphUsage::CopyDest, "copyDest" },
				std::pair{ RenderGraphUsage::Present, "present" },
				std::pair{ RenderGraphUsage::VertexBuffer, "vertexBuffer" },
				std::pair{ RenderGraphUsage::IndexBuffer, "indexBuffer" },
				std::pair{ RenderGraphUsage::ConstantBuffer, "constantBuffer" },
				std::pair{ RenderGraphUsage::IndirectArgument, "indirectArgument" },
				std::pair{ RenderGraphUsage::AccelerationStructureBuildInput,
					"accelerationStructureBuildInput" },
				std::pair{ RenderGraphUsage::AccelerationStructureBuildWrite,
					"accelerationStructureBuildWrite" },
				std::pair{ RenderGraphUsage::AccelerationStructureRead,
					"accelerationStructureRead" },
				std::pair{ RenderGraphUsage::ResolveSource, "resolveSource" },
				std::pair{ RenderGraphUsage::ResolveDest, "resolveDest" },
			};
			Json result = Json::array();
			for (const auto& [flag, name] : flags)
			{
				if (HasRenderGraphUsage(usage, flag))
					result.push_back(name);
			}
			return result;
		}

		[[nodiscard]] Json SerializeRenderGraphLifetimeInterval(
			const RenderGraphResourceLifetimeInfo& interval)
		{
			if (!interval.IsUsed())
				return nullptr;
			return Json{
				{ "firstPassId", interval.FirstUse },
				{ "lastPassId", interval.LastUse },
			};
		}

		[[nodiscard]] Json SerializeRenderGraphSubresources(
			const RenderGraphDiagnosticAccessSnapshot& access)
		{
			if (!access.bHasSubresources)
				return nullptr;
			const auto count = [](std::uint32_t value) -> Json
			{
				return value == RenderGraphSubresourceRange::AllRemaining
					? Json(nullptr)
					: Json(value);
			};
			return Json{
				{ "baseMipLevel", access.Subresources.BaseMipLevel },
				{ "mipLevelCount", count(access.Subresources.MipLevelCount) },
				{ "baseArraySlice", access.Subresources.BaseArraySlice },
				{ "arraySliceCount", count(access.Subresources.ArraySliceCount) },
			};
		}

		[[nodiscard]] Json BuildRenderGraphSummary(
			const RenderGraphDiagnosticSnapshot& snapshot)
		{
			std::size_t culledPasses = 0;
			std::size_t textureCount = 0;
			std::size_t bufferCount = 0;
			std::size_t accelerationStructureCount = 0;
			for (const RenderGraphDiagnosticPassSnapshot& pass : snapshot.Passes)
				culledPasses += pass.bCulled ? 1u : 0u;
			for (const RenderGraphDiagnosticResourceSnapshot& resource :
				snapshot.Resources)
			{
				switch (resource.Kind)
				{
				case RenderGraphDiagnosticResourceKind::Texture: ++textureCount; break;
				case RenderGraphDiagnosticResourceKind::Buffer: ++bufferCount; break;
				case RenderGraphDiagnosticResourceKind::AccelerationStructure:
					++accelerationStructureCount;
					break;
				}
			}
			return Json{
				{ "compiled", snapshot.bCompiled },
				{ "dirty", snapshot.bDirty },
				{ "width", snapshot.Width },
				{ "height", snapshot.Height },
				{ "passCount", snapshot.Passes.size() },
				{ "executedPassCount", snapshot.ExecutionOrder.size() },
				{ "culledPassCount", culledPasses },
				{ "resourceCount", snapshot.Resources.size() },
				{ "textureCount", textureCount },
				{ "bufferCount", bufferCount },
				{ "accelerationStructureCount", accelerationStructureCount },
				{ "accessCount", snapshot.Accesses.size() },
				{ "dependencyCount", snapshot.Dependencies.size() },
				{ "compileErrorCount", snapshot.CompileErrors.size() },
			};
		}

		[[nodiscard]] std::string DisplaySettingsValue(const Json& value)
		{
			if (value.is_string())
				return value.get<std::string>();
			return value.dump(-1, ' ', false, Json::error_handler_t::replace);
		}

		[[nodiscard]] const char* RenderSettingValueTypeId(
			RenderSettingValueType type) noexcept
		{
			switch (type)
			{
			case RenderSettingValueType::Boolean: return "boolean";
			case RenderSettingValueType::Integer: return "integer";
			case RenderSettingValueType::Float: return "float";
			case RenderSettingValueType::Enumeration: return "enum";
			case RenderSettingValueType::String: return "string";
			}
			return "unknown";
		}

		[[nodiscard]] Json BuildChineseRenderSettingsReport(
			const ResolvedRenderSettings& snapshot)
		{
			const Json serialized = SerializeResolvedRenderSettings(snapshot, false);
			const Json& requested = serialized.at("requested");
			const Json& effective = serialized.at("effective");
			const Json& sources = serialized.at("sources");
			const Json& availability = serialized.at("availability");
			Json fallbacks = Json::array();
			for (const RenderSettingDescriptor& descriptor :
				GetRenderSettingDescriptors())
			{
				if (!descriptor.bPersistedRenderSetting ||
					!requested.contains(descriptor.PropertyId) ||
					!effective.contains(descriptor.PropertyId) ||
					requested.at(descriptor.PropertyId) ==
						effective.at(descriptor.PropertyId))
				{
					continue;
				}
				std::string reason;
				if (availability.contains(descriptor.PropertyId))
				{
					reason = availability.at(descriptor.PropertyId).value(
						"reason", "");
				}
				fallbacks.push_back({
					{ "propertyId", descriptor.PropertyId },
					{ "requested", requested.at(descriptor.PropertyId) },
					{ "effective", effective.at(descriptor.PropertyId) },
					{ "source", sources.value(descriptor.PropertyId, "Unknown") },
					{ "reason", Utf8Prefix(reason, MaxRenderGraphTextBytes) },
					{ "applyImpact", ToString(descriptor.Impact) },
				});
			}

			std::size_t errors = 0;
			std::size_t warnings = 0;
			for (const Json& diagnostic : serialized.at("diagnostics"))
			{
				const std::string severity = diagnostic.value("severity", "Info");
				errors += severity == "Error" ? 1u : 0u;
				warnings += severity == "Warning" ? 1u : 0u;
			}

			const std::string status = errors > 0
				? "error"
				: (!fallbacks.empty() || warnings > 0 ? "warning" : "healthy");
			std::ostringstream markdown;
			markdown << "# 渲染设置诊断报告\n\n"
				<< "- 修订：" << snapshot.Revision << "\n"
				<< "- 哈希：" << snapshot.Hash << "\n"
				<< "- 显卡："
				<< (snapshot.Capabilities.Render.AdapterName.empty()
					? "未报告" : snapshot.Capabilities.Render.AdapterName)
				<< "\n"
				<< "- requested/effective 不一致：" << fallbacks.size() << " 项\n"
				<< "- 诊断：" << errors << " 个错误，" << warnings << " 个警告\n";
			if (!fallbacks.empty())
			{
				markdown << "\n## 回退与规范化\n";
				for (const Json& fallback : fallbacks)
				{
					markdown << "- " << fallback.at("propertyId").get<std::string>()
						<< "：请求 " << DisplaySettingsValue(fallback.at("requested"))
						<< "，生效 " << DisplaySettingsValue(fallback.at("effective"));
					const std::string reason = fallback.value("reason", "");
					if (!reason.empty())
						markdown << "（" << reason << "）";
					markdown << "\n";
				}
			}

			return Json{
				{ "language", "zh-CN" },
				{ "status", status },
				{ "revision", snapshot.Revision },
				{ "hash", snapshot.Hash },
				{ "fallbackCount", fallbacks.size() },
				{ "warningCount", warnings },
				{ "errorCount", errors },
				{ "fallbacks", std::move(fallbacks) },
				{ "markdown", markdown.str() },
			};
		}

		[[nodiscard]] bool IsSafeMethodForLog(std::string_view method)
		{
			return !method.empty() && method.size() <= AgentProtocol::MaxMethodLength &&
				std::all_of(method.begin(), method.end(), [](unsigned char character)
				{
					return std::isalnum(character) != 0 || character == '_' ||
						character == '-' || character == '.';
				});
		}

		template<Component... Components>
		[[nodiscard]] Json CountComponents(ComponentGroup<Components...>, const Ref<Level>& level)
		{
			Json counts = Json::object();
			([&counts, &level]()
			{
				const std::size_t count = level->mRegistry.view<Components>().size();
				if (count > 0)
					counts[Components::GetTypeName()] = count;
			}(), ...);
			return counts;
		}

		template<Component... Components>
		[[nodiscard]] std::vector<std::string> GetRegisteredComponentNames(ComponentGroup<Components...>)
		{
			std::vector<std::string> names{ Components::GetTypeName()... };
			std::sort(names.begin(), names.end());
			return names;
		}

		template<Component... Components>
		[[nodiscard]] std::vector<std::string> GetPresentComponentNames(
			ComponentGroup<Components...>,
			Entity entity)
		{
			std::vector<std::string> names;
			([&names, &entity]()
			{
				if (entity.HasComponent<Components>())
					names.emplace_back(Components::GetTypeName());
			}(), ...);
			std::sort(names.begin(), names.end());
			return names;
		}

		template<Component... Components>
		[[nodiscard]] Json SerializeProjectedComponents(
			ComponentGroup<Components...>,
			Entity entity,
			const std::vector<std::string>& requested,
			Json& missing)
		{
			Json components = Json::object();
			std::size_t projectedBytes = 0;
			([&components, &entity, &requested, &missing, &projectedBytes]()
			{
				const std::string_view componentName = Components::GetTypeName();
				if (std::find(requested.begin(), requested.end(), componentName) == requested.end())
					return;
				if (!entity.HasComponent<Components>())
				{
					missing.push_back(std::string(componentName));
					return;
				}
				Json value;
				Serializer::Serialize(value, entity.GetComponent<Components>());
				const std::size_t serializedBytes = value.dump(
					-1, ' ', false, Json::error_handler_t::replace).size();
				if (serializedBytes > MaxSerializedComponentBytes)
				{
					throw AgentCommandError(
						"component_too_large",
						"Projected component exceeds the 32 KiB limit.");
				}
				if (serializedBytes > MaxProjectedComponentsBytes - projectedBytes)
				{
					throw AgentCommandError(
						"projection_too_large",
						"Projected components exceed the 128 KiB limit.");
				}
				projectedBytes += serializedBytes;
				components[std::string(componentName)] = std::move(value);
			}(), ...);
			return components;
		}
	}

	DevAgentRuntime::DevAgentRuntime(Scope<IAgentTransport> transport)
		: mTransport(std::move(transport)), mEventLog(CreateRef<AgentEventLog>())
	{
		if (!mTransport)
			mTransport = CreateScope<FileMailboxAgentTransport>();
		RegisterCommands();
	}

	DevAgentRuntime::~DevAgentRuntime()
	{
		try
		{
			Stop();
		}
		catch (const std::exception& error)
		{
			Log::WriteFallbackError(
				"Failed to stop Dev Agent Runtime during destruction.",
				error.what());
		}
		catch (...)
		{
			Log::WriteFallbackError(
				"Failed to stop Dev Agent Runtime during destruction.",
				"unknown exception");
		}
	}

	bool DevAgentRuntime::Start(const DevAgentRuntimeConfig& config, Ref<Level> level)
	{
		if (mRunning || config.SessionDirectory.empty() || !config.SessionDirectory.is_absolute())
			return false;
		if (!config.Token.empty() && !IsValidToken(config.Token))
			return false;
		if (config.MaxRequestBytes == 0 || config.MaxRequestBytes > AgentProtocol::MaxRequestBytes ||
			config.MaxResponseBytes == 0 || config.MaxResponseBytes > AgentProtocol::MaxResponseBytes ||
			config.MaxRequestsPerTick == 0 || config.MaxRequestsPerTick > AgentProtocol::MaxRequestsPerTick)
		{
			return false;
		}

		bool bExpectedSessionState = false;
		if (!sAgentSessionClaimedForProcess.compare_exchange_strong(bExpectedSessionState, true))
			return false;

		mConfig = config;
		mEventLog->Reset();
		mConfig.SessionDirectory = config.SessionDirectory.lexically_normal();
		mLevel = std::move(level);
		mToken = config.Token.empty() ? GenerateRandomHexToken(32) : config.Token;
		mSessionId = GenerateRandomHexToken(16);
		mStartTime = std::chrono::steady_clock::now();
		mLastStatusPublish = mStartTime;
		mPendingSimulationSteps = 0;
		mCompletedSimulationSteps = 0;
		mRuntimeControlMode = config.bStartPaused
			? RuntimeControlMode::Paused
			: RuntimeControlMode::Observe;
		mStepInProgress = false;
		mStopRequested = false;
		mRenderGraphSnapshot.reset();
		mRenderGraphSnapshotId.clear();
		mRenderGraphSnapshotFrame = 0;
		mNextRenderGraphSnapshotId = 1;
		if (mLevel && config.bStartPaused)
			mLevel->SetRuntimePaused(true);
		const AgentTransportConfig transportConfig{
			mConfig.SessionDirectory,
			mConfig.MaxRequestBytes,
			mConfig.MaxResponseBytes
		};
		const Json descriptor{
			{ "protocolVersion", AgentProtocol::Version },
			{ "sessionId", mSessionId },
			{ "state", "starting" },
			{ "transport", std::string(mTransport->GetName()) },
			{ "token", mToken },
			{ "requestPattern", "requests/<handle>.request.json" },
			{ "responsePattern", "responses/<handle>.response.json" },
			{ "startPaused", config.bStartPaused },
			{ "limits", {
				{ "maxRequestBytes", mConfig.MaxRequestBytes },
				{ "maxResponseBytes", mConfig.MaxResponseBytes },
				{ "maxRequestsPerTick", mConfig.MaxRequestsPerTick }
			} }
		};
		if (!mTransport->Start(transportConfig, descriptor))
		{
			sAgentSessionClaimedForProcess.store(false);
			mLevel.reset();
			mToken.clear();
			mSessionId.clear();
			return false;
		}
		if (!mCaptureController.Start(mConfig.SessionDirectory, mSessionId))
		{
			mTransport->Stop(Json{
				{ "protocolVersion", AgentProtocol::Version },
				{ "sessionId", mSessionId },
				{ "state", "failed" },
				{ "error", "capture_root_unavailable" }
			});
			sAgentSessionClaimedForProcess.store(false);
			mLevel.reset();
			mToken.clear();
			mSessionId.clear();
			return false;
		}

		mRunning = true;
		mInputController.Start();
		AttachLogSinks();
		mEventLog->Append("info", "Dev Agent Runtime started.");
		PublishStatus("ready");
		HE_CORE_INFO("[AgentRuntime] Ready session={0}", mSessionId);
		return true;
	}

	void DevAgentRuntime::Stop()
	{
		if (!mRunning)
		{
			mInputController.Stop();
			return;
		}
		mEventLog->Append("info", "Dev Agent Runtime stopped.");
		mInputController.Stop();
		mCaptureController.Stop();
		mTransport->Stop(BuildStatus("stopped"));
		mRunning = false;
		mLevel.reset();
		mToken.clear();
		mPendingSimulationSteps = 0;
		mCompletedSimulationSteps = 0;
		mRuntimeControlMode = RuntimeControlMode::Observe;
		mStepInProgress = false;
		mStopRequested = false;
		mRenderGraphSnapshot.reset();
		mRenderGraphSnapshotId.clear();
		mRenderGraphSnapshotFrame = 0;
		HE_CORE_INFO("[AgentRuntime] Stopped session={0}", mSessionId);
	}

	void DevAgentRuntime::Tick()
	{
		if (!mRunning)
			return;

		for (AgentTransportMessage& message : mTransport->Poll(mConfig.MaxRequestsPerTick))
		{
			Json response;
			if (message.HasTransportError())
			{
				response = AgentCommandRegistry::MakeError(message.Handle, message.ErrorCode, message.ErrorMessage);
				mEventLog->Append("warn", "Rejected malformed mailbox request.");
			}
			else
			{
				std::string method = "invalid-request";
				if (message.Payload.is_object() && message.Payload.contains("method") &&
					message.Payload["method"].is_string() &&
					IsSafeMethodForLog(message.Payload["method"].get_ref<const std::string&>()))
				{
					method = message.Payload["method"].get<std::string>();
				}
				response = mRegistry.Dispatch(message.Payload, mToken);
				mEventLog->Append(response.value("ok", false) ? "info" : "warn", "Handled " + method + ".");
			}

			if (response.dump(-1, ' ', false, Json::error_handler_t::replace).size() > mConfig.MaxResponseBytes)
			{
				response = AgentCommandRegistry::MakeError(
					nullptr,
					"response_too_large",
					"Response exceeds the session byte limit.");
			}
			if (!mTransport->Respond(message, response))
				mEventLog->Append("error", "Failed to publish mailbox response.");
		}

		const auto now = std::chrono::steady_clock::now();
		if (now - mLastStatusPublish >= std::chrono::seconds(1))
		{
			PublishStatus("ready");
			mLastStatusPublish = now;
		}
	}

	void DevAgentRuntime::SetLevel(Ref<Level> level, std::string sceneName)
	{
		mLevel = std::move(level);
		if (mLevel)
		{
			if (mRuntimeControlMode == RuntimeControlMode::Paused)
				mLevel->SetRuntimePaused(true);
			else if (mRuntimeControlMode == RuntimeControlMode::Running)
				mLevel->SetRuntimePaused(false);
		}
		if (!sceneName.empty())
			mConfig.SceneName = std::move(sceneName);
		if (mRunning)
			PublishStatus("ready");
	}

	void DevAgentRuntime::PrepareSimulationFrame(bool bCanAdvance)
	{
		if (!mRunning || !mLevel || mStepInProgress)
			return;
		if (!bCanAdvance)
		{
			if (mRuntimeControlMode == RuntimeControlMode::Paused)
				mLevel->SetRuntimePaused(true);
			return;
		}

		if (mRuntimeControlMode == RuntimeControlMode::Running)
		{
			mLevel->SetRuntimePaused(false);
			return;
		}
		if (mRuntimeControlMode != RuntimeControlMode::Paused)
			return;

		if (mPendingSimulationSteps > 0)
		{
			mLevel->SetRuntimePaused(false);
			mStepInProgress = true;
		}
		else
		{
			mLevel->SetRuntimePaused(true);
		}
	}

	void DevAgentRuntime::CompleteSimulationFrame()
	{
		if (!mStepInProgress)
			return;

		if (mPendingSimulationSteps > 0)
			--mPendingSimulationSteps;
		++mCompletedSimulationSteps;
		if (mLevel)
			mLevel->SetRuntimePaused(true);
		mStepInProgress = false;
	}

	void DevAgentRuntime::ProcessPendingCapture()
	{
		if (!mRunning)
			return;
		const AgentCaptureProcessResult result = mCaptureController.ProcessPending(
			Application::GetInstance(),
			Application::GetInstance().GetFrameNumber());
		if (result == AgentCaptureProcessResult::None)
			return;
		mEventLog->Append(
			result == AgentCaptureProcessResult::Completed ? "info" : "error",
			result == AgentCaptureProcessResult::Completed
				? "Live framebuffer capture completed."
				: "Live framebuffer capture failed.");
		PublishStatus("ready");
	}

	bool DevAgentRuntime::ConsumeStopRequested()
	{
		const bool bRequested = mStopRequested;
		mStopRequested = false;
		return bRequested;
	}

	void DevAgentRuntime::RegisterCommands()
	{
		if (!mRegistry.Register(
			AgentCommandDescription{
				"api.discover",
				"List implemented commands.",
				"read-only",
				Json::object()
			},
			[this](const Json& params)
			{
				RequireNoParams(params, "api.discover");
				return mRegistry.Discover();
			}))
		{
			throw std::logic_error("Failed to register api.discover.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"render.settings.describe",
				"Describe stable render-setting IDs, validation metadata, layers, and GI presets.",
				"read-only:render-settings",
				Json::object()
			},
			[](const Json& params)
			{
				RequireNoParams(params, "render.settings.describe");
				const ResolvedRenderSettings snapshot =
					RenderSettingsService::GetInstance().GetSnapshot();
				Json serialized = SerializeResolvedRenderSettings(snapshot, true);
				Json& serializedDescriptors = serialized["descriptors"];
				const std::vector<RenderSettingDescriptor>& descriptors =
					GetRenderSettingDescriptors();
				for (std::size_t index = 0;
					index < descriptors.size() && index < serializedDescriptors.size();
					++index)
				{
					serializedDescriptors[index]["type"] =
						RenderSettingValueTypeId(descriptors[index].Type);
					serializedDescriptors[index]["enumValues"] =
						descriptors[index].EnumValues;
				}
				Json presets = Json::array();
				for (GlobalIlluminationQualityPreset preset : {
						GlobalIlluminationQualityPreset::Low,
						GlobalIlluminationQualityPreset::Balanced,
						GlobalIlluminationQualityPreset::High })
				{
					presets.push_back({
						{ "id", ToString(preset) },
						{ "patch", GetGlobalIlluminationQualityPresetPatch(preset) },
					});
				}
				return Json{
					{ "schema", serialized.at("schema") },
					{ "schemaVersion", serialized.at("schemaVersion") },
					{ "revision", snapshot.Revision },
					{ "hash", snapshot.Hash },
					{ "defaultWriteLayer", "Session" },
					{ "mutableLayers", Json::array({ "Session", "User", "Project" }) },
					{ "persistentWritesRequireExplicitPersist", true },
					{ "descriptors", std::move(serialized["descriptors"]) },
					{ "giQualityPresets", std::move(presets) },
				};
			}))
		{
			throw std::logic_error("Failed to register render.settings.describe.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"render.settings.get",
				"Read the immutable requested/effective render-settings snapshot.",
				"read-only:render-settings",
				Json::object()
			},
			[](const Json& params)
			{
				RequireNoParams(params, "render.settings.get");
				return SerializeResolvedRenderSettings(
					RenderSettingsService::GetInstance().GetSnapshot(), false);
			}))
		{
			throw std::logic_error("Failed to register render.settings.get.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"render.settings.plan",
				"Validate and plan a render-settings patch without committing it.",
				"read-only:render-settings-plan",
				Json{
					{ "layer", "Session|User|Project optional, default Session" },
					{ "baseRevision", "uint64 required" },
					{ "baseHash", "string required" },
					{ "patch", "object required, stable property IDs to values/null" },
				}
			},
			[](const Json& params)
			{
				RequireOnlyParams(
					params,
					"render.settings.plan",
					{ "layer", "baseRevision", "baseHash", "patch" });
				const RenderSettingsDraft draft = BuildSettingsDraft(
					params, "render.settings.plan", true);
				return SerializeRenderSettingsApplyResult(
					RenderSettingsService::GetInstance().PlanDraft(draft));
			}))
		{
			throw std::logic_error("Failed to register render.settings.plan.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"render.settings.apply",
				"Transactionally apply a render-settings patch; Session is the safe default.",
				"control:render-settings",
				Json{
					{ "layer", "Session|User|Project optional, default Session" },
					{ "baseRevision", "uint64 required" },
					{ "baseHash", "string required" },
					{ "patch", "object required, stable property IDs to values/null" },
					{ "persist", "boolean; explicit true required for User/Project" },
				}
			},
			[](const Json& params)
			{
				RequireOnlyParams(
					params,
					"render.settings.apply",
					{ "layer", "baseRevision", "baseHash", "patch", "persist" });
				const RenderSettingsDraft draft = BuildSettingsDraft(
					params, "render.settings.apply", true);
				const bool bPersist = ReadExplicitPersistence(
					params, draft.TargetLayer, "render.settings.apply");
				return SerializeRenderSettingsApplyResult(
					RenderSettingsService::GetInstance().ApplyDraft(draft, bPersist));
			}))
		{
			throw std::logic_error("Failed to register render.settings.apply.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"render.settings.reset",
				"Delete property/category/layer overrides or restore all mutable layers to Engine defaults.",
				"control:render-settings",
				Json{
					{ "mode", "property|category|layer|engineDefault required" },
					{ "layer", "Session|User|Project optional, default Session" },
					{ "propertyId", "stable property ID required for property mode" },
					{ "categoryPrefix", "stable property-ID prefix required for category mode" },
					{ "baseRevision", "uint64 required" },
					{ "baseHash", "string required" },
					{ "persist", "boolean; explicit true required for persistent writes" },
				}
			},
			[](const Json& params)
			{
				RequireOnlyParams(
					params,
					"render.settings.reset",
					{ "mode", "layer", "propertyId", "categoryPrefix",
						"baseRevision", "baseHash", "persist" });
				if (!params.contains("mode") || !params["mode"].is_string())
					throw std::invalid_argument("render.settings.reset mode must be a string.");

				std::uint64_t revision = 0;
				std::string hash;
				ReadSettingsBase(
					params, "render.settings.reset", revision, hash);
				const std::string& mode = params["mode"].get_ref<const std::string&>();
				RenderSettingsApplyResult result;
				RenderSettingsService& service = RenderSettingsService::GetInstance();
				if (mode == "engineDefault")
				{
					if (params.contains("layer") || params.contains("propertyId") ||
						params.contains("categoryPrefix") ||
						!params.contains("persist") ||
						!params["persist"].is_boolean() ||
						!params["persist"].get<bool>())
					{
						throw std::invalid_argument(
							"render.settings.reset engineDefault requires persist=true and no layer/property/category.");
					}
					result = service.ResetToEngineDefault(revision, hash);
				}
				else
				{
					const SettingsLayer layer = ParseMutableSettingsLayer(
						params, "render.settings.reset");
					const bool bPersist = ReadExplicitPersistence(
						params, layer, "render.settings.reset");
					if (mode == "property")
					{
						if (!params.contains("propertyId") ||
							!params["propertyId"].is_string() ||
							params["propertyId"].get_ref<const std::string&>().empty() ||
							params.contains("categoryPrefix"))
						{
							throw std::invalid_argument(
								"render.settings.reset property mode requires propertyId only.");
						}
						result = service.ResetProperty(
							layer,
							params["propertyId"].get_ref<const std::string&>(),
							revision,
							hash,
							bPersist);
					}
					else if (mode == "category")
					{
						if (!params.contains("categoryPrefix") ||
							!params["categoryPrefix"].is_string() ||
							params["categoryPrefix"].get_ref<const std::string&>().empty() ||
							params.contains("propertyId"))
						{
							throw std::invalid_argument(
								"render.settings.reset category mode requires categoryPrefix only.");
						}
						result = service.ResetCategory(
							layer,
							params["categoryPrefix"].get_ref<const std::string&>(),
							revision,
							hash,
							bPersist);
					}
					else if (mode == "layer")
					{
						if (params.contains("propertyId") ||
							params.contains("categoryPrefix"))
						{
							throw std::invalid_argument(
								"render.settings.reset layer mode does not accept property/category.");
						}
						result = service.ResetLayer(
							layer, revision, hash, bPersist);
					}
					else
					{
						throw std::invalid_argument(
							"render.settings.reset mode must be property, category, layer, or engineDefault.");
					}
				}
				return SerializeRenderSettingsApplyResult(result);
			}))
		{
			throw std::logic_error("Failed to register render.settings.reset.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"render.settings.report",
				"Build a bounded Chinese report of sources, diagnostics, and requested/effective fallbacks.",
				"read-only:render-settings-report",
				Json::object()
			},
			[](const Json& params)
			{
				RequireNoParams(params, "render.settings.report");
				return BuildChineseRenderSettingsReport(
					RenderSettingsService::GetInstance().GetSnapshot());
			}))
		{
			throw std::logic_error("Failed to register render.settings.report.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"render.graph.snapshot",
				"Read a paged immutable RenderGraph diagnostic snapshot without parsing DebugDump text.",
				"read-only:render-graph",
				Json{
					{ "snapshotId", "string optional; omit to capture a new immutable snapshot" },
					{ "section", "summary|passes|resources|accesses|dependencies|compileErrors|topologicalOrder|executionOrder optional" },
					{ "offset", "uint64 optional" },
					{ "limit", "uint 1..100 optional" },
				}
			},
			[this](const Json& params)
			{
				return BuildRenderGraphSnapshotPage(params);
			}))
		{
			throw std::logic_error("Failed to register render.graph.snapshot.");
		}

#if HE_ENABLE_GM
		if (!mRegistry.Register(
			AgentCommandDescription{
				"gm.discover",
				"Discover reusable Engine and Project GM commands with typed schemas.",
				"read-only:gm",
				Json::object()
			},
			[](const Json& params)
			{
				RequireNoParams(params, "gm.discover");
				return GMService::GetInstance().Discover();
			}))
		{
			throw std::logic_error("Failed to register gm.discover.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"gm.execute",
				"Execute one discovered GM command and return its uniform receipt.",
				"control:gm",
				Json{
					{ "command", "string required, discovered GM command name" },
					{ "arguments", "object optional, default {}" },
					{ "confirm", "boolean optional, required true for elevated risk" }
				}
			},
			[](const Json& params)
			{
				for (const auto& [name, value] : params.items())
				{
					(void)value;
					if (name != "command" && name != "arguments" && name != "confirm")
						throw std::invalid_argument("gm.execute contains an unknown parameter.");
				}
				if (!params.contains("command") || !params["command"].is_string())
					throw std::invalid_argument("gm.execute command must be a string.");
				Json arguments = Json::object();
				if (params.contains("arguments"))
				{
					if (!params["arguments"].is_object())
						throw std::invalid_argument("gm.execute arguments must be an object.");
					arguments = params["arguments"];
				}
				bool bConfirmed = false;
				if (params.contains("confirm"))
				{
					if (!params["confirm"].is_boolean())
						throw std::invalid_argument("gm.execute confirm must be a boolean.");
					bConfirmed = params["confirm"].get<bool>();
				}
				return GMService::GetInstance().Execute(
					params["command"].get_ref<const std::string&>(),
					arguments,
					bConfirmed,
					"agent").ToJson();
			}))
		{
			throw std::logic_error("Failed to register gm.execute.");
		}
#endif

		if (!mRegistry.Register(
			AgentCommandDescription{
				"capture.frame",
				"Capture the next rendered main framebuffer into the session captures directory.",
				"control:framebuffer-capture",
				Json{ { "filename", "safe ASCII .png filename required, max 64 bytes" } }
			},
			[this](const Json& params)
			{
				return mCaptureController.Request(
					params,
					Application::GetInstance().GetFrameNumber());
			}))
		{
			throw std::logic_error("Failed to register capture.frame.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"input.click",
				"Dispatch a bounded virtual pointer move/down/up click without OS injection.",
				"control:virtual-pointer-event",
				Json{
					{ "x", "number required, main framebuffer coordinate" },
					{ "y", "number required, main framebuffer coordinate" },
					{ "button", "left|right optional, default left" }
				}
			},
			[this](const Json& params)
			{
				const std::uint32_t width = Application::GetInstance().GetRenderWidth();
				const std::uint32_t height = Application::GetInstance().GetRenderHeight();
				if (width == 0 || height == 0)
					throw AgentCommandError("framebuffer_unavailable", "The main framebuffer has no drawable area.");
				return mInputController.Click(params, width, height);
			}))
		{
			throw std::logic_error("Failed to register input.click.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"input.keys",
				"List canonical synthetic keyboard key names and current state.",
				"control:synthetic-keyboard",
				Json::object()
			},
			[this](const Json& params)
			{
				RequireNoParams(params, "input.keys");
				return mInputController.ListKeys();
			}))
		{
			throw std::logic_error("Failed to register input.keys.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"input.set",
				"Atomically replace held synthetic keyboard keys.",
				"control:synthetic-keyboard",
				Json{ { "held", "canonical key-name string[] required, max 32" } }
			},
			[this](const Json& params) { return mInputController.SetHeldKeys(params); }))
		{
			throw std::logic_error("Failed to register input.set.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"input.clear",
				"Release every synthetic keyboard key.",
				"control:synthetic-keyboard",
				Json::object()
			},
			[this](const Json& params) { return mInputController.Clear(params); }))
		{
			throw std::logic_error("Failed to register input.clear.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"logs.read",
				"Read bounded engine, client, and Agent Runtime logs by cursor.",
				"read-only:session-logs",
				Json{
					{ "after", "uint64 optional" },
					{ "limit", "uint 1..100 optional" }
				}
			},
			[this](const Json& params) { return ReadLogs(params); }))
		{
			throw std::logic_error("Failed to register logs.read.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"session.status",
				"Read session and runtime status.",
				"read-only",
				Json::object()
			},
			[this](const Json& params)
			{
				RequireNoParams(params, "session.status");
				return BuildStatus("ready");
			}))
		{
			throw std::logic_error("Failed to register session.status.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"world.summary",
				"Count scene entities and component types.",
				"read-only",
				Json::object()
			},
			[this](const Json& params)
			{
				RequireNoParams(params, "world.summary");
				return BuildWorldSummary();
			}))
		{
			throw std::logic_error("Failed to register world.summary.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"world.list",
				"List entities with bounded offset pagination.",
				"read-only",
				Json{
					{ "offset", "uint64 optional" },
					{ "limit", "uint 1..100 optional" }
				}
			},
			[this](const Json& params) { return BuildWorldList(params); }))
		{
			throw std::logic_error("Failed to register world.list.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"world.get",
				"Read one entity with optional component projection.",
				"read-only",
				Json{
					{ "uuid", "string required" },
					{ "components", "string[] max 16 optional" }
				}
			},
			[this](const Json& params) { return BuildWorldEntity(params); }))
		{
			throw std::logic_error("Failed to register world.get.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"runtime.pause",
				"Pause game simulation.",
				"control:runtime",
				Json::object()
			},
			[this](const Json& params)
			{
				RequireNoParams(params, "runtime.pause");
				if (mPendingSimulationSteps > 0 || mStepInProgress)
				{
					throw AgentCommandError(
						"step_in_progress",
						"runtime.pause cannot cancel a pending step ticket.");
				}
				mRuntimeControlMode = RuntimeControlMode::Paused;
				if (mLevel)
					mLevel->SetRuntimePaused(true);
				mEventLog->Append("info", "Runtime paused by Agent Runtime.");
				return BuildRuntimeControlState();
			}))
		{
			throw std::logic_error("Failed to register runtime.pause.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"runtime.resume",
				"Resume game simulation.",
				"control:runtime",
				Json::object()
			},
			[this](const Json& params)
			{
				RequireNoParams(params, "runtime.resume");
				if (mPendingSimulationSteps > 0 || mStepInProgress)
				{
					throw AgentCommandError(
						"step_in_progress",
						"runtime.resume cannot cancel a pending step ticket.");
				}
				mRuntimeControlMode = RuntimeControlMode::Running;
				if (mLevel)
					mLevel->SetRuntimePaused(false);
				mEventLog->Append("info", "Runtime resumed by Agent Runtime.");
				return BuildRuntimeControlState();
			}))
		{
			throw std::logic_error("Failed to register runtime.resume.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"runtime.step",
				"Queue bounded simulation frames while paused.",
				"control:runtime",
				Json{ { "frames", "uint 1..120 optional" } }
			},
			[this](const Json& params)
			{
				for (const auto& [name, value] : params.items())
				{
					(void)value;
					if (name != "frames")
						throw std::invalid_argument("runtime.step contains an unknown parameter.");
				}
				std::uint64_t frames = 1;
				if (params.contains("frames"))
				{
					if (!params["frames"].is_number_unsigned())
						throw std::invalid_argument("runtime.step frames must be an unsigned integer.");
					frames = params["frames"].get<std::uint64_t>();
				}
				if (frames == 0 || frames > MaxQueuedSimulationSteps ||
					frames > MaxQueuedSimulationSteps - mPendingSimulationSteps ||
					mCompletedSimulationSteps > std::numeric_limits<std::uint64_t>::max() -
						mPendingSimulationSteps - frames)
				{
					throw std::invalid_argument("runtime.step pending frames must remain in the range 1..120.");
				}
				mRuntimeControlMode = RuntimeControlMode::Paused;
				mPendingSimulationSteps += static_cast<std::size_t>(frames);
				if (mLevel)
					mLevel->SetRuntimePaused(true);
				mEventLog->Append("info", "Simulation frames queued by Agent Runtime.");
				Json result = BuildRuntimeControlState();
				const std::uint64_t target = mCompletedSimulationSteps + mPendingSimulationSteps;
				result["ticket"] = mSessionId + "-step-" + std::to_string(target);
				result["targetCompletedSteps"] = target;
				return result;
			}))
		{
			throw std::logic_error("Failed to register runtime.step.");
		}

		if (!mRegistry.Register(
			AgentCommandDescription{
				"session.stop",
				"Request an orderly owned-session shutdown.",
				"control:session",
				Json::object()
			},
			[this](const Json& params)
			{
				RequireNoParams(params, "session.stop");
				mStopRequested = true;
				mEventLog->Append("info", "Orderly session stop requested.");
				return Json{ { "stopRequested", true } };
			}))
		{
			throw std::logic_error("Failed to register session.stop.");
		}
	}

	void DevAgentRuntime::AttachLogSinks()
	{
		const std::shared_ptr<spdlog::logger>& coreLogger = Log::GetCoreLogger();
		const std::shared_ptr<spdlog::logger>& clientLogger = Log::GetClientLogger();
		if (coreLogger)
			coreLogger->sinks().push_back(std::make_shared<AgentSpdlogSink>(mEventLog, "engine"));
		if (clientLogger && clientLogger != coreLogger)
			clientLogger->sinks().push_back(std::make_shared<AgentSpdlogSink>(mEventLog, "client"));
	}

	void DevAgentRuntime::PublishStatus(std::string_view state)
	{
		if (mRunning && !mTransport->PublishStatus(BuildStatus(state)))
			mEventLog->Append("error", "Failed to publish session status.");
	}

	Json DevAgentRuntime::BuildRenderGraphSnapshotPage(const Json& params)
	{
		RequireOnlyParams(
			params,
			"render.graph.snapshot",
			{ "snapshotId", "section", "offset", "limit" });

		std::string section = "summary";
		if (params.contains("section"))
		{
			if (!params["section"].is_string())
				throw std::invalid_argument("render.graph.snapshot section must be a string.");
			section = params["section"].get<std::string>();
		}
		constexpr std::array<std::string_view, 8> sections = {
			"summary",
			"passes",
			"resources",
			"accesses",
			"dependencies",
			"compileErrors",
			"topologicalOrder",
			"executionOrder",
		};
		if (std::ranges::find(sections, section) == sections.end())
		{
			throw std::invalid_argument(
				"render.graph.snapshot section is not a supported stable section ID.");
		}

		std::uint64_t offset = 0;
		std::size_t limit = 50;
		if (params.contains("offset"))
		{
			if (!params["offset"].is_number_unsigned())
				throw std::invalid_argument("render.graph.snapshot offset must be an unsigned integer.");
			offset = params["offset"].get<std::uint64_t>();
		}
		if (params.contains("limit"))
		{
			if (!params["limit"].is_number_unsigned())
				throw std::invalid_argument("render.graph.snapshot limit must be an unsigned integer.");
			const std::uint64_t requested = params["limit"].get<std::uint64_t>();
			if (requested == 0 || requested > MaxRenderGraphPageSize)
				throw std::invalid_argument("render.graph.snapshot limit must be in the range 1..100.");
			limit = static_cast<std::size_t>(requested);
		}

		if (params.contains("snapshotId"))
		{
			if (!params["snapshotId"].is_string())
				throw std::invalid_argument("render.graph.snapshot snapshotId must be a string.");
			const std::string& requestedId =
				params["snapshotId"].get_ref<const std::string&>();
			if (requestedId.empty() || requestedId.size() > 128)
				throw std::invalid_argument("render.graph.snapshot snapshotId has an invalid length.");
			if (!mRenderGraphSnapshot || requestedId != mRenderGraphSnapshotId)
			{
				throw AgentCommandError(
					"snapshot_not_found",
					"The requested RenderGraph snapshot is no longer retained; omit snapshotId to capture a new one.");
			}
		}
		else
		{
			Application& application = Application::GetInstance();
			mRenderGraphSnapshot = CreateScope<RenderGraphDiagnosticSnapshot>(
				application.GetRenderGraphDiagnosticSnapshot());
			mRenderGraphSnapshotFrame = application.GetFrameNumber();
			mRenderGraphSnapshotId = mSessionId + "-render-graph-" +
				std::to_string(mNextRenderGraphSnapshotId++);
			if (mNextRenderGraphSnapshotId == 0)
				mNextRenderGraphSnapshotId = 1;
		}

		const RenderGraphDiagnosticSnapshot& snapshot = *mRenderGraphSnapshot;
		Json result{
			{ "snapshotId", mRenderGraphSnapshotId },
			{ "renderFrame", mRenderGraphSnapshotFrame },
			{ "section", section },
			{ "summary", BuildRenderGraphSummary(snapshot) },
		};
		if (section == "summary")
		{
			result["offset"] = 0;
			result["limit"] = 0;
			result["total"] = 1;
			result["nextOffset"] = nullptr;
			result["items"] = Json::array();
			return result;
		}

		const auto buildPage = [&]<typename Serializer>(
			std::size_t total,
			Serializer&& serializeAt)
		{
			Json items = Json::array();
			const std::uint64_t total64 = static_cast<std::uint64_t>(total);
			const std::uint64_t begin = std::min(offset, total64);
			const std::uint64_t end = std::min(
				total64,
				begin + std::min<std::uint64_t>(
					limit, std::numeric_limits<std::uint64_t>::max() - begin));
			for (std::uint64_t index = begin; index < end; ++index)
				items.push_back(serializeAt(static_cast<std::size_t>(index)));
			result["offset"] = begin;
			result["limit"] = limit;
			result["total"] = total64;
			result["nextOffset"] = end < total64 ? Json(end) : Json(nullptr);
			result["items"] = std::move(items);
		};

		if (section == "passes")
		{
			buildPage(snapshot.Passes.size(), [&](std::size_t index)
			{
				const RenderGraphDiagnosticPassSnapshot& pass = snapshot.Passes[index];
				return Json{
					{ "passId", pass.PassId },
					{ "topologicalIndex", pass.TopologicalIndex == InvalidRenderGraphIndex
						? Json(nullptr) : Json(pass.TopologicalIndex) },
					{ "executionIndex", pass.ExecutionIndex == InvalidRenderGraphIndex
						? Json(nullptr) : Json(pass.ExecutionIndex) },
					{ "name", Utf8Prefix(pass.Name, MaxRenderGraphTextBytes) },
					{ "queue", RenderGraphQueueId(pass.Queue) },
					{ "sideEffect", pass.bSideEffect },
					{ "setupComplete", pass.bSetupComplete },
					{ "culled", pass.bCulled },
				};
			});
		}
		else if (section == "resources")
		{
			buildPage(snapshot.Resources.size(), [&](std::size_t index)
			{
				const RenderGraphDiagnosticResourceSnapshot& resource =
					snapshot.Resources[index];
				return Json{
					{ "resourceId", resource.ResourceId },
					{ "kind", RenderGraphResourceKindId(resource.Kind) },
					{ "name", Utf8Prefix(resource.Name, MaxRenderGraphTextBytes) },
					{ "lifetime", RenderGraphLifetimeId(resource.Lifetime) },
					{ "passInterval", SerializeRenderGraphLifetimeInterval(
						resource.PassInterval) },
					{ "requiredUsage", SerializeRenderGraphUsage(resource.RequiredUsage) },
					{ "requiredUsageMask", static_cast<std::uint32_t>(resource.RequiredUsage) },
					{ "initiallyInitialized", resource.bInitiallyInitialized },
					{ "exported", resource.bExported },
					{ "width", resource.Width },
					{ "height", resource.Height },
					{ "depth", resource.Depth },
					{ "mipLevels", resource.MipLevels },
					{ "arrayLayers", resource.ArrayLayers },
					{ "sampleCount", resource.SampleCount },
					{ "format", Utf8Prefix(resource.FormatName, 128) },
					{ "relativeToGraph", resource.bRelativeToGraph },
					{ "widthScale", resource.WidthScale },
					{ "heightScale", resource.HeightScale },
					{ "byteSize", resource.ByteSize },
					{ "structStride", resource.StructStride },
				};
			});
		}
		else if (section == "accesses")
		{
			buildPage(snapshot.Accesses.size(), [&](std::size_t index)
			{
				const RenderGraphDiagnosticAccessSnapshot& access = snapshot.Accesses[index];
				return Json{
					{ "passId", access.PassId },
					{ "resourceKind", RenderGraphResourceKindId(access.ResourceKind) },
					{ "resourceId", access.ResourceId },
					{ "access", RenderGraphAccessId(access.Access) },
					{ "usage", SerializeRenderGraphUsage(access.Usage) },
					{ "usageMask", static_cast<std::uint32_t>(access.Usage) },
					{ "subresources", SerializeRenderGraphSubresources(access) },
				};
			});
		}
		else if (section == "dependencies")
		{
			buildPage(snapshot.Dependencies.size(), [&](std::size_t index)
			{
				const RenderGraphDiagnosticDependencySnapshot& dependency =
					snapshot.Dependencies[index];
				return Json{
					{ "prerequisitePassId", dependency.PrerequisitePassId },
					{ "dependentPassId", dependency.DependentPassId },
					{ "explicit", dependency.bExplicit },
				};
			});
		}
		else if (section == "compileErrors")
		{
			buildPage(snapshot.CompileErrors.size(), [&](std::size_t index)
			{
				return Json{
					{ "message", Utf8Prefix(
						snapshot.CompileErrors[index].Message,
						MaxRenderGraphTextBytes) },
				};
			});
		}
		else if (section == "topologicalOrder")
		{
			buildPage(snapshot.TopologicalOrder.size(), [&](std::size_t index)
			{
				return Json(snapshot.TopologicalOrder[index]);
			});
		}
		else
		{
			buildPage(snapshot.ExecutionOrder.size(), [&](std::size_t index)
			{
				return Json(snapshot.ExecutionOrder[index]);
			});
		}
		return result;
	}

	Json DevAgentRuntime::BuildStatus(std::string_view state) const
	{
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - mStartTime);
		const std::size_t entityCount = mLevel ? mLevel->mRegistry.alive() : 0;
		const Application& application = Application::GetInstance();
		return Json{
			{ "protocolVersion", AgentProtocol::Version },
			{ "sessionId", mSessionId },
			{ "state", std::string(state) },
			{ "scene", mConfig.SceneName },
			{ "uptimeMs", elapsed.count() },
			{ "renderFrame", application.GetFrameNumber() },
			{ "completedRenderFrames", application.GetCompletedFrameCount() },
			{ "simulationTimeSeconds", application.GetSimulationTimeSeconds() },
			{ "stopRequested", mStopRequested },
			{ "input", mInputController.BuildStatus() },
			{ "capture", mCaptureController.BuildStatus() },
			{ "world", {
				{ "available", mLevel != nullptr },
				{ "entityCount", entityCount },
				{ "runtimeControl", BuildRuntimeControlState() }
			} }
		};
	}

	Json DevAgentRuntime::BuildWorldSummary() const
	{
		if (!mLevel)
			return Json{ { "available", false } };

		return Json{
			{ "available", true },
			{ "scene", mConfig.SceneName },
			{ "entityCount", mLevel->mRegistry.alive() },
			{ "runtimeControl", BuildRuntimeControlState() },
			{ "componentCounts", CountComponents(ComponentsSerialized{}, mLevel) }
		};
	}

	Json DevAgentRuntime::BuildWorldList(const Json& params) const
	{
		const WorldPage page = ParseWorldPage(params);
		if (!mLevel)
			return Json{ { "available", false }, { "entities", Json::array() } };

		const auto view = mLevel->mRegistry.view<IDComponent, TagComponent>();
		const std::size_t total = view.size_hint();
		Json entities = Json::array();
		std::uint64_t skipped = 0;
		for (const EntityHandle handle : view)
		{
			if (skipped < page.Offset)
			{
				++skipped;
				continue;
			}
			if (entities.size() >= page.Limit)
				break;

			const std::string& name = view.get<TagComponent>(handle).Tag;
			const std::string boundedName = Utf8Prefix(name, 256);
			Json entity{
				{ "uuid", view.get<IDComponent>(handle).ID.str() },
				{ "name", boundedName }
			};
			if (boundedName.size() != name.size())
				entity["nameTruncated"] = true;
			entities.push_back(std::move(entity));
		}

		Json nextOffset = nullptr;
		const std::uint64_t returnedThrough = page.Offset + entities.size();
		if (returnedThrough < total)
			nextOffset = returnedThrough;
		return Json{
			{ "available", true },
			{ "total", total },
			{ "offset", page.Offset },
			{ "limit", page.Limit },
			{ "nextOffset", std::move(nextOffset) },
			{ "entities", std::move(entities) }
		};
	}

	Json DevAgentRuntime::BuildWorldEntity(const Json& params) const
	{
		for (const auto& [name, value] : params.items())
		{
			(void)value;
			if (name != "uuid" && name != "components")
				throw std::invalid_argument("world.get contains an unknown parameter.");
		}
		if (!params.contains("uuid") || !params["uuid"].is_string())
			throw std::invalid_argument("world.get uuid is required.");
		const std::string uuidText = params["uuid"].get<std::string>();
		if (uuidText.empty() || uuidText.size() > 64)
			throw std::invalid_argument("world.get uuid has an invalid format.");

		const bool bProjectionRequested = params.contains("components");
		std::vector<std::string> requestedComponents;
		if (bProjectionRequested)
		{
			if (!params["components"].is_array() ||
				params["components"].size() > MaxProjectedComponents)
			{
				throw std::invalid_argument("world.get components must be an array with at most 16 entries.");
			}
			for (const Json& value : params["components"])
			{
				if (!value.is_string())
					throw std::invalid_argument("world.get component names must be strings.");
				const std::string name = value.get<std::string>();
				if (name.empty() || name.size() > AgentProtocol::MaxMethodLength ||
					!std::all_of(name.begin(), name.end(), [](unsigned char character)
					{
						return std::isalnum(character) != 0 || character == '_';
					}))
				{
					throw std::invalid_argument("world.get component name has an invalid format.");
				}
				if (std::find(requestedComponents.begin(), requestedComponents.end(), name) !=
					requestedComponents.end())
				{
					throw std::invalid_argument("world.get components must not contain duplicates.");
				}
				requestedComponents.push_back(name);
			}

			const std::vector<std::string> registered =
				GetRegisteredComponentNames(ComponentsSerialized{});
			for (const std::string& requested : requestedComponents)
			{
				if (!std::binary_search(registered.begin(), registered.end(), requested))
					throw std::invalid_argument("world.get requested an unknown component type.");
			}
		}
		if (!mLevel)
			return Json{ { "available", false }, { "found", false } };

		UUID uuid(uuidText);
		if (!uuid.isValid())
			throw std::invalid_argument("world.get uuid has an invalid format.");
		Entity entity = mLevel->TryGetEntityByUUID(uuid);
		if (!entity)
			return Json{ { "available", true }, { "found", false }, { "uuid", uuidText } };

		const std::string entityName = entity.GetName();
		const std::string boundedName = Utf8Prefix(entityName, 256);
		Json result{
			{ "available", true },
			{ "found", true },
			{ "uuid", entity.UUID().str() },
			{ "name", boundedName },
			{ "componentNames", GetPresentComponentNames(ComponentsSerialized{}, entity) }
		};
		if (boundedName.size() != entityName.size())
			result["nameTruncated"] = true;
		if (bProjectionRequested)
		{
			Json missing = Json::array();
			result["components"] = SerializeProjectedComponents(
				ComponentsSerialized{}, entity, requestedComponents, missing);
			if (!missing.empty())
				result["missingComponents"] = std::move(missing);
		}
		return result;
	}

	Json DevAgentRuntime::BuildRuntimeControlState() const
	{
		const char* controlMode = "observe";
		if (mRuntimeControlMode == RuntimeControlMode::Paused)
			controlMode = "paused";
		else if (mRuntimeControlMode == RuntimeControlMode::Running)
			controlMode = "running";
		return Json{
			{ "paused", mLevel ? mLevel->IsRuntimePaused() : false },
			{ "controlMode", controlMode },
			{ "pendingSteps", mPendingSimulationSteps },
			{ "completedSteps", mCompletedSimulationSteps },
			{ "stepInProgress", mStepInProgress }
		};
	}

	Json DevAgentRuntime::ReadLogs(const Json& params) const
	{
		for (const auto& [name, value] : params.items())
		{
			(void)value;
			if (name != "after" && name != "limit")
				throw std::invalid_argument("logs.read contains an unknown parameter.");
		}

		std::uint64_t after = 0;
		std::size_t limit = 50;
		if (params.contains("after"))
		{
			if (!params["after"].is_number_unsigned())
				throw std::invalid_argument("logs.read after must be an unsigned integer.");
			after = params["after"].get<std::uint64_t>();
		}
		if (params.contains("limit"))
		{
			if (!params["limit"].is_number_unsigned())
				throw std::invalid_argument("logs.read limit must be an unsigned integer.");
			const std::uint64_t requestedLimit = params["limit"].get<std::uint64_t>();
			if (requestedLimit == 0 || requestedLimit > 100)
				throw std::invalid_argument("logs.read limit must be in the range 1..100.");
			limit = static_cast<std::size_t>(requestedLimit);
		}
		return mEventLog->Read(after, limit);
	}

	std::string DevAgentRuntime::GenerateRandomHexToken(std::size_t bytes)
	{
		std::random_device random;
		std::ostringstream stream;
		stream << std::hex << std::setfill('0');
		for (std::size_t offset = 0; offset < bytes; offset += sizeof(std::uint32_t))
			stream << std::setw(8) << random();
		std::string result = stream.str();
		result.resize(bytes * 2);
		return result;
	}

	bool DevAgentRuntime::IsValidToken(std::string_view token)
	{
		if (token.size() < 32 || token.size() > 256)
			return false;
		return std::all_of(token.begin(), token.end(), [](unsigned char character)
		{
			return std::isalnum(character) != 0 || character == '_' || character == '-';
		});
	}
}
