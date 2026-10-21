#include "Game/AgentRuntime/DevAgentRuntime.h"

#include "Game/AgentRuntime/FileMailboxAgentTransport.h"
#include "Game/AgentRuntime/AgentSpdlogSink.h"
#include "Runtime/Animation/CharacterPose.h"
#include "Runtime/Core/Log/LogSession.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Mesh/MeshComponent.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingService.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingLevel.h"
#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/EcsFramework/System/Render/RenderSystem3D.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Graphics/RenderGraph/RenderGraph.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Graphics/Renderer/RuntimeRenderer.h"
#include "Runtime/Graphics/RHI/RenderDevice.h"
#include "Runtime/Graphics/Renderer/SceneRenderer.h"
#include "Runtime/UI/Rml/RmlUiManager.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/Mesh/SubMesh.h"
#include "Runtime/Graphics/VFX/EffekseerRuntime.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "_generated/serializer/all_serializer.h"

#if HE_ENABLE_GM
#include "Runtime/DevTools/GM/GMService.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
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
	struct AgentRenderValidationState
	{
		RuntimeOffscreenViewHandle View;
		TextureReadbackHandle Capture;
		Ref<const RenderWorld> World;
		uint64_t SubmittedSnapshots = 0;
		uint64_t NextCapture = 1;
	};
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
				const std::string componentName = Components::GetTypeName();
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
		mAnimationObservationWorld.reset();
		if (!mRunning)
		{
			mInputController.Stop();
			return;
		}
		mEventLog->Append("info", "Dev Agent Runtime stopped.");
		mInputController.Stop();
		mCaptureController.Stop();
		DestroyValidationView();
		mTransport->Stop(BuildStatus("stopped"));
		mRunning = false;
		mMemorySamplePending = false;
		mMemoryCompletedSequence = 0;
		mMemoryBeforeMailbox = {};
		mMemoryAfterMailbox = {};
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

		const bool bCaptureMemory = mMemorySamplePending;
		PlatformProcessMemorySnapshot memoryBeforeMailbox;
		if (bCaptureMemory)
			memoryBeforeMailbox = PlatformProcess::ReadCurrentMemory();

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

			if (!mTransport->Respond(message, response))
				mEventLog->Append("error", "Failed to publish mailbox response.");
		}

		const auto now = std::chrono::steady_clock::now();
		if (now - mLastStatusPublish >= std::chrono::seconds(1))
		{
			PublishStatus("ready");
			mLastStatusPublish = now;
		}
		if (bCaptureMemory)
		{
			const auto memoryAfterMailbox = PlatformProcess::ReadCurrentMemory();
			mMemoryBeforeMailbox = memoryBeforeMailbox;
			mMemoryAfterMailbox = memoryAfterMailbox;
			++mMemoryCompletedSequence;
			mMemorySamplePending = false;
		}
	}

	void DevAgentRuntime::SetLevel(Ref<Level> level, std::string sceneName)
	{
		mAnimationObservationWorld.reset();
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
            mInputController.PublishPendingSimulationInput();
			mLevel->SetRuntimePaused(false);
			return;
		}
		if (mRuntimeControlMode != RuntimeControlMode::Paused)
			return;

        if (mCaptureController.IsSequencePending() &&
            !Application::GetInstance().CanAdvanceCaptureSequence())
        {
            mLevel->SetRuntimePaused(true);
            return;
        }
		if (mPendingSimulationSteps > 0)
		{
            mInputController.PublishPendingSimulationInput();
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
        Application::GetInstance().SetCaptureSimulationStep(mCompletedSimulationSteps);
		if (mLevel)
			mLevel->SetRuntimePaused(true);
		mStepInProgress = false;
	}

	void DevAgentRuntime::SubmitValidationView()
	{
		if (!mRenderValidation || !mRenderValidation->View || !mLevel) return;
		const auto* system = mLevel->TryGetSystem<RenderSystem3D>();
		const auto world = system ? system->AcquireLatestRenderWorld() : Ref<const RenderWorld>{};
		if (!world) return;
		mRenderValidation->World = world;
		Application::GetInstance().SubmitOffscreenView(mRenderValidation->View, world);
		++mRenderValidation->SubmittedSnapshots;
	}

	void DevAgentRuntime::DestroyValidationView()
	{
		if (!mRenderValidation) return;
		auto& application = Application::GetInstance();
		if (mRenderValidation->Capture && application.IsOffscreenCaptureValid(mRenderValidation->Capture))
			application.ReleaseOffscreenCapture(mRenderValidation->Capture);
		application.DestroyOffscreenView(mRenderValidation->View);
		mRenderValidation.reset();
	}

	Json DevAgentRuntime::BuildValidationViewStatus() const
	{
		if (!mRenderValidation || !mRenderValidation->View) return Json{{"available",false}};
		const auto& state = *mRenderValidation;
		auto& application = Application::GetInstance();
		const auto output = application.GetOffscreenOutput(state.View);
		const auto& stats = application.GetOffscreenSceneStats(state.View);
        const auto vfx = EffekseerRuntime::GetStats();
		Json passes = Json::array();
		for (size_t i = 0; i < std::min(size_t(128), stats.CompiledPasses.size()); ++i)
			passes.push_back(stats.CompiledPasses[i]);
		Json response{{"available",true},{"width",output.Width},{"height",output.Height},
			{"viewIndex",state.View.Index},{"viewGeneration",state.View.Generation},
			{"outputGeneration",output.Generation},{"submittedSnapshots",state.SubmittedSnapshots},
			{"sceneGeneration",state.World ? state.World->SceneGeneration : 0},
			{"worldFrame",state.World ? state.World->FrameNumber : 0},
			{"hasRecordedGraph",!stats.CompiledPasses.empty()},{"passes",std::move(passes)},
			{"materialGenerationMax",stats.MaterialProgramGenerationMax},
			{"customMaterialDraws",stats.CustomMaterialCpuFallbackDrawCount},
            {"customCpuDraws",stats.CustomMaterialCpuFallbackDrawCount},
            {"customGpuDraws",stats.CustomMaterialGpuDrawCount},
            {"customGpuObjects",stats.CustomMaterialGpuObjectCount},
            {"cpuGraphRecordMs",stats.CpuGraphRecordMilliseconds},
            {"reducedLodSections",stats.ReducedLodSectionCount},
            {"baseRasterTriangles",stats.BaseRasterTriangleCount},
            {"selectedRasterTriangles",stats.SelectedRasterTriangleCount},
            {"customGpuObjectCountKind","submittedCandidates"},
            {"renderObjectCount",state.World ? state.World->Objects.size() : 0},
            {"rayTracingInstanceCount",stats.RayTracingInstanceCount},
            {"vfx",{{"activeEffects",vfx.ActiveEffects.Current},{"drawCalls",vfx.DrawCalls.Current},
                {"activeGpuParticles",vfx.ActiveGpuParticles.Current},
                {"activeGpuParticlesStatus",static_cast<uint32_t>(vfx.ActiveGpuParticles.Status)},
                {"activeEffectsStatus",static_cast<uint32_t>(vfx.ActiveEffects.Status)},
                {"drawCallsStatus",static_cast<uint32_t>(vfx.DrawCalls.Status)},
                {"gpuParticlesEnabled",vfx.bGpuParticlesEnabled},
                {"scope","runtimeGlobalLastRecordedView; submissions do not prove visible pixels"}}},
            {"cameraPosition",state.World ? Json::array({state.World->Camera.Position.x,
                state.World->Camera.Position.y,state.World->Camera.Position.z}) : Json::array()},
			{"cameraPolicy","Current live 3D snapshot and its projection; use the main view aspect ratio for a matching composition."}};
		if (state.Capture && application.IsOffscreenCaptureValid(state.Capture))
		{
			const auto capture = application.GetOffscreenCaptureResult(state.Capture);
			const char* status = capture.Status == TextureReadbackStatus::Complete ? "complete" :
				capture.Status == TextureReadbackStatus::Failed ? "failed" : "pending";
			response["capture"] = {{"status",status},{"path",Utils::PathToUtf8(capture.OutputPath)},
				{"error",Utf8Prefix(capture.Error,512)}};
		}
		return response;
	}

	Json DevAgentRuntime::BuildAnimationStatus()
	{
		const auto world = Application::GetInstance().GetLastSubmittedRenderWorld();
		if (!world) return Json{{"available",false},{"source","lastSuccessfullySubmittedMainRenderWorld"}};
		constexpr size_t objectLimit = 16;
		constexpr size_t matrixLimit = 4096;
		constexpr size_t vertexLimit = 16384;
		constexpr double epsilon = 1e-6;
		const auto hashPalette = [matrixLimit](const std::vector<glm::mat4>& palette) {
			uint64_t hash = 14695981039346656037ull;
			const auto addWord = [&hash](uint64_t word, uint32_t bytes) {
				for (uint32_t i = 0; i < bytes; ++i) { hash ^= (word >> (i * 8u)) & 0xffu; hash *= 1099511628211ull; }
			};
			addWord(palette.size(), 8);
			for (size_t i = 0; i < std::min(palette.size(), matrixLimit); ++i)
				for (int column = 0; column < 4; ++column)
					for (int row = 0; row < 4; ++row)
						addWord(std::bit_cast<uint32_t>(palette[i][column][row]), 4);
			std::ostringstream stream;
			stream << std::hex << std::setw(16) << std::setfill('0') << hash;
			return stream.str();
		};
        const auto hashVectors = [](const std::vector<glm::vec3>& vectors) {
            uint64_t hash = 14695981039346656037ull;
            const auto addWord = [&hash](uint64_t word, uint32_t bytes) {
                for (uint32_t byte = 0; byte < bytes; ++byte) { hash ^= (word >> (byte * 8u)) & 0xffu; hash *= 1099511628211ull; }
            };
            addWord(vectors.size(), 8);
            for (const auto& vector : vectors)
                for (int axis = 0; axis < 3; ++axis) addWord(std::bit_cast<uint32_t>(vector[axis]), 4);
            std::ostringstream stream;
            stream << std::hex << std::setw(16) << std::setfill('0') << hash;
            return stream.str();
        };
		struct PaletteDifference { size_t Changed = 0; size_t NonFinite = 0; double Maximum = 0.0; };
		const auto compare = [matrixLimit, epsilon](const std::vector<glm::mat4>& current, const std::vector<glm::mat4>& previous) {
			PaletteDifference result;
			const size_t currentCount = std::min(current.size(), matrixLimit);
			const size_t previousCount = std::min(previous.size(), matrixLimit);
			const size_t count = std::min(currentCount, previousCount);
			result.Changed = std::max(currentCount, previousCount) - count;
			for (size_t i = 0; i < count; ++i)
			{
				bool changed = false;
				for (int column = 0; column < 4; ++column)
					for (int row = 0; row < 4; ++row)
					{
						const double a = current[i][column][row], b = previous[i][column][row];
						if (!std::isfinite(a) || !std::isfinite(b)) { ++result.NonFinite; continue; }
						const double delta = std::abs(a - b);
						changed = changed || delta > epsilon;
						result.Maximum = std::max(result.Maximum, delta);
					}
				result.Changed += changed ? 1u : 0u;
			}
			return result;
		};
		const bool sameScene = mAnimationObservationWorld &&
			mAnimationObservationWorld->SceneGeneration == world->SceneGeneration;
        const Ref<Level> activeLevel = mAuthorPreview ? mAuthorPreview : mLevel;
        const auto* renderSystem = activeLevel ? activeLevel->TryGetSystem<RenderSystem3D>() : nullptr;
        const bool liveSceneMatches = renderSystem && renderSystem->GetSceneGeneration() == world->SceneGeneration;
		Json objects = Json::array();
		size_t skinnedObjects = 0;
		for (const auto& object : world->Objects)
		{
			const bool skinned = !object.BonePalette.empty() ||
				std::any_of(object.Sections.begin(), object.Sections.end(), [](const auto& section) {
					return section.SubMeshResource && section.SubMeshResource->IsSkinned(); });
			if (!skinned) continue;
			++skinnedObjects;
			if (objects.size() == objectLimit) continue;
			const auto delta = compare(object.BonePalette, object.PreviousBonePalette);
			Json item{{"entityId",object.EntityId},{"objectGpuIndex",object.ObjectGpuIndex},
				{"currentPaletteCount",object.BonePalette.size()},{"previousPaletteCount",object.PreviousBonePalette.size()},
				{"currentPaletteHash",hashPalette(object.BonePalette)},{"previousPaletteHash",hashPalette(object.PreviousBonePalette)},
				{"hashCoversFullPalettes",object.BonePalette.size() <= matrixLimit && object.PreviousBonePalette.size() <= matrixLimit},
				{"changedMatrixCount",delta.Changed},{"maxElementDelta",delta.Maximum},{"nonFiniteComparisons",delta.NonFinite},
				{"observationComparisonAvailable",false}};
            item["uuid"] = "";
            const auto handle = static_cast<entt::entity>(static_cast<uint32_t>(object.EntityId));
            if (liveSceneMatches && activeLevel->mRegistry.valid(handle) && activeLevel->mRegistry.all_of<IDComponent>(handle))
                item["uuid"] = activeLevel->mRegistry.get<IDComponent>(handle).ID.str();
            item["clothSolvers"] = Json::array();
            item["footContacts"] = Json::array();
            if (liveSceneMatches && activeLevel->mRegistry.valid(handle) && activeLevel->mRegistry.all_of<AnimatorComponent>(handle))
            {
                const auto& animator = activeLevel->mRegistry.get<AnimatorComponent>(handle);
                Json parameters = Json::object();
                size_t nonFiniteParameters = 0;
                for (const auto& [name, value] : animator.mParameters)
                {
                    if (parameters.size() == 64) break;
                    if (!value.is_boolean() && !value.is_number()) continue;
                    if (value.is_number() && !std::isfinite(value.get<double>())) ++nonFiniteParameters;
                    parameters[name.substr(0, 128)] = value;
                }
                item["controller"] = {{"source","currentAnimatorComponent_notLastSubmittedPalette"},
                    {"loaded",animator.bLoaded},{"currentNodeId",animator.mCurrentNodeId},
                    {"targetNodeId",animator.mTargetNodeId},{"stateName",animator.mCurrentStateName.substr(0, 256)},
                    {"inTransition",animator.mInTransition},{"transitionTime",animator.mTransitionTime},
                    {"transitionDuration",animator.mTransitionDuration},{"progress",animator.mProgress},
                    {"parameterCount",animator.mParameters.size()},{"nonFiniteParameters",nonFiniteParameters},
                    {"parameters",std::move(parameters)}};
                const auto feet = animator.mCharacterPose.GetFootContactDiagnostics(16);
                item["footContactsSource"] = "currentCpuCharacterPoseSolver_notLastSubmittedPalette";
                item["footContactsTimingNote"] = "Current solver and lastSubmitted render palette can differ by one frame; invalid contacts retain prior coordinates.";
                item["footContactsConfiguredCount"] = animator.Feet.size();
                for (size_t index = 0; index < feet.size(); ++index)
                {
                    const auto& foot = feet[index];
                    const auto vectorJson = [](const glm::vec3& value) { return Json::array({value.x, value.y, value.z}); };
                    item["footContacts"].push_back({{"index",index},
                        {"bone",index < animator.Feet.size() ? animator.Feet[index].Foot : ""},
                        {"animated",vectorJson(foot.Animated)},{"anchor",vectorJson(foot.Anchor)},
                        {"target",vectorJson(foot.Target)},{"weight",foot.Weight},
                        {"locked",foot.Locked},{"valid",foot.Valid}});
                }
                for (const auto& [section, solver] : animator.mClothSimulation)
                {
                    const auto& positions = solver.Positions();
                    const size_t count = std::min({positions.size(), solver.Rest.size(), solver.Radii.size(), vertexLimit});
                    double maximumDistance = 0, maximumExcess = 0, maximumPinDrift = 0;
                    size_t nonFinite = 0;
                    for (size_t index = 0; index < count; ++index)
                    {
                        const double distance = glm::distance(positions[index], solver.Rest[index]);
                        const double radius = solver.Radii[index];
                        if (!std::isfinite(distance) || !std::isfinite(radius)) { ++nonFinite; continue; }
                        maximumDistance = std::max(maximumDistance, distance);
                        maximumExcess = std::max(maximumExcess, distance - radius);
                        if (radius <= 0) maximumPinDrift = std::max(maximumPinDrift, distance);
                    }
                    item["clothSolvers"].push_back({{"section",section},{"particles",positions.size()},
                        {"positionsHash",hashVectors(solver.Positions())},{"velocitiesHash",hashVectors(solver.Velocities())},
                        {"hashCoversFullState",true},{"source","currentAnimatorSolver"},
                        {"worldSamples",count},{"worldStateComplete",count == positions.size() && count == solver.Rest.size() && count == solver.Radii.size()},
                        {"maxWorldRestDistance",maximumDistance},{"maxWorldRadiusExcess",maximumExcess},
                        {"maxPinnedWorldDrift",maximumPinDrift},{"worldNonFinite",nonFinite}});
                }
                item["limbContacts"] = Json::array();
                if (activeLevel->mRegistry.all_of<MeshComponent>(handle))
                {
                    const auto& mesh = activeLevel->mRegistry.get<MeshComponent>(handle).mMesh;
                    for (const auto& limb : animator.LimbTargets)
                    {
                        if (item["limbContacts"].size() == objectLimit) break;
                        glm::mat4 bone;
                        glm::vec3 target = limb.WorldTarget;
                        bool targetResolved = true;
                        float effectiveWeight = limb.Weight;
                        if (!limb.WeightParameter.empty())
                            effectiveWeight *= std::clamp(animator.GetFloat(limb.WeightParameter, 0.0f), 0.0f, 1.0f);
                        // Match the solver's target precedence. A support-hand
                        // target can be in the other hand's bone frame rather
                        // than an entity frame or the configured world point.
                        if (!limb.TargetBone.empty())
                        {
                            glm::mat4 targetBone;
                            targetResolved = mesh && CharacterPose::BoneMatrix(
                                mesh->mAnimation, object.BonePalette, limb.TargetBone, targetBone);
                            if (targetResolved)
                                target = glm::vec3(object.WorldTransform * targetBone * glm::vec4(limb.TargetOffset, 1));
                        }
                        else if (!limb.TargetUUID.empty())
                        {
                            const UUID targetId(limb.TargetUUID);
                            const Entity targetEntity = targetId.isValid() ? activeLevel->TryGetEntityByUUID(targetId) : Entity{};
                            targetResolved = static_cast<bool>(targetEntity);
                            if (targetResolved) target = glm::vec3(activeLevel->GetWorldTransform(targetEntity) * glm::vec4(limb.TargetOffset, 1));
                        }
                        const bool boneResolved = mesh && CharacterPose::BoneMatrix(mesh->mAnimation, object.BonePalette, limb.End, bone);
                        Json contact{{"bone",limb.End},{"targetUUID",limb.TargetUUID},{"weight",limb.Weight},
                            {"targetBone",limb.TargetBone},{"weightParameter",limb.WeightParameter},{"effectiveWeight",effectiveWeight},
                            {"boneResolved",boneResolved},{"targetResolved",targetResolved},
                            {"source","lastSubmittedPaletteAndWorld_currentConstraintTarget"}};
                        if (boneResolved && targetResolved)
                        {
                            const glm::vec3 end = glm::vec3(object.WorldTransform * bone[3]);
                            const glm::vec3 effector = glm::vec3(object.WorldTransform * bone * glm::vec4(limb.EffectorLocalOffset, 1));
                            const double distance = glm::distance(effector, target);
                            contact["finite"] = std::isfinite(distance);
                            if (std::isfinite(distance))
                            {
                                contact["endWorld"] = {end.x,end.y,end.z};
                                contact["effectorLocalOffset"] = {limb.EffectorLocalOffset.x,limb.EffectorLocalOffset.y,limb.EffectorLocalOffset.z};
                                contact["effectorWorld"] = {effector.x,effector.y,effector.z};
                                contact["boneOriginDistance"] = glm::distance(end, target);
                                contact["targetWorld"] = {target.x,target.y,target.z};
                                contact["distance"] = distance;
                            }
                        }
                        item["limbContacts"].push_back(std::move(contact));
                    }
                }
            }
			if (sameScene)
			{
				const auto found = std::find_if(mAnimationObservationWorld->Objects.begin(),
					mAnimationObservationWorld->Objects.end(), [&object](const auto& previous) { return previous.EntityId == object.EntityId; });
				if (found != mAnimationObservationWorld->Objects.end())
				{
					const auto observation = compare(object.BonePalette, found->BonePalette);
					item["observationComparisonAvailable"] = true;
					item["observationPaletteCount"] = found->BonePalette.size();
					item["observationComparisonComplete"] = object.BonePalette.size() <= matrixLimit && found->BonePalette.size() <= matrixLimit;
					item["observationChangedMatrixCount"] = observation.Changed;
					item["observationMaxElementDelta"] = observation.Maximum;
					item["observationNonFiniteComparisons"] = observation.NonFinite;
				}
			}
			size_t totalVertices = 0, scanned = 0, invalidWeights = 0, invalidMapping = 0, unweighted = 0;
            double maximumWeightSumError = 0.0;
            Json deformations = Json::array();
            for (const auto& section : object.Sections)
            {
                if (!section.SubMeshResource) continue;
                const auto& vertices = section.SubMeshResource->mSkinnedVertices;
                const auto& base = section.SubMeshResource->GetBaseSkinnedVertices();
                if (section.SubMeshResource->GetDeformationRevision() > 0 && !vertices.empty())
                {
                    const size_t stride = std::max<size_t>(1, (vertices.size() + 1023) / 1024);
                    size_t samples = 0, nonFinite = 0;
                    float maxBaseDelta = 0, maxHistoryDelta = 0;
                    for (size_t i = 0; i < vertices.size(); i += stride)
                    {
                        ++samples;
                        const float baseDelta = glm::distance(vertices[i].Pos, base[i].Pos);
                        const float historyDelta = glm::distance(vertices[i].Pos, vertices[i].PreviousPosition);
                        if (!std::isfinite(baseDelta) || !std::isfinite(historyDelta)) { ++nonFinite; continue; }
                        maxBaseDelta = std::max(maxBaseDelta, baseDelta);
                        maxHistoryDelta = std::max(maxHistoryDelta, historyDelta);
                    }
                    deformations.push_back({{"materialIndex",section.SubMeshResource->mMaterialIndex},
                        {"revision",section.SubMeshResource->GetDeformationRevision()},
                        {"morphTargetCount",section.SubMeshResource->GetMorphTargets().size()},
                        {"vertices",vertices.size()},{"samples",samples},{"nonFinite",nonFinite},
                        {"maxBasePositionDelta",maxBaseDelta},{"maxPreviousPositionDelta",maxHistoryDelta}});
                }
                totalVertices += vertices.size();
				for (size_t i = 0; i < vertices.size() && scanned < vertexLimit; ++i, ++scanned)
				{
					double weightSum = 0.0;
					for (size_t influence = 0; influence < MAX_BONE_INFLUENCE; ++influence)
					{
						const float weight = vertices[i].mWeights[influence];
						const int bone = vertices[i].mBoneIDs[influence];
						if (!std::isfinite(weight) || weight < 0.0f) { ++invalidWeights; continue; }
						if (weight == 0.0f) continue;
						if (bone < 0 || static_cast<size_t>(bone) >= object.BonePalette.size()) { ++invalidMapping; continue; }
						weightSum += weight;
					}
					unweighted += weightSum <= epsilon ? 1u : 0u;
					maximumWeightSumError = std::max(maximumWeightSumError, std::abs(weightSum - 1.0));
				}
			}
            item["deformation"] = std::move(deformations);
            item["weights"] = {{"sectionVertexCount",totalVertices},{"inspectedVertices",scanned},
				{"complete",scanned == totalVertices},{"invalidWeights",invalidWeights},
				{"positiveInfluencesOutsidePalette",invalidMapping},{"verticesWithoutValidWeight",unweighted},
				{"maxValidWeightSumError",maximumWeightSumError}};
			objects.push_back(std::move(item));
		}
		Json result{{"available",true},{"source","lastSuccessfullySubmittedMainRenderWorld"},
			{"sceneGeneration",world->SceneGeneration},{"renderWorldFrame",world->FrameNumber},
			{"skinnedObjectCount",skinnedObjects},{"reportedObjectCount",objects.size()},
			{"objectsTruncated",skinnedObjects > objectLimit},{"objectLimit",objectLimit},
			{"paletteMatrixLimit",matrixLimit},{"weightVertexLimitPerObject",vertexLimit},
			{"comparisonEpsilon",epsilon},{"hashAlgorithm","FNV-1a-64: palette count and IEEE754 bits, little endian, matrix column order"},
			{"previousObservationFrame",sameScene ? mAnimationObservationWorld->FrameNumber : 0},
			{"objects",std::move(objects)},
			{"evidenceLimit","Submitted CPU bone data and bounded weight/index checks only; neither changed hashes nor matrices prove visual skinning correctness."}};
		mAnimationObservationWorld = world;
		return result;
	}

	void DevAgentRuntime::ProcessPendingCapture()
	{
		if (!mRunning)
			return;
		SubmitValidationView();
        const bool bSequence = mCaptureController.IsSequencePending();
		const AgentCaptureProcessResult result = mCaptureController.ProcessPending(
			Application::GetInstance(),
			Application::GetInstance().GetFrameNumber());
		if (result == AgentCaptureProcessResult::None)
			return;
        if (bSequence && result == AgentCaptureProcessResult::Failed)
        {
            mPendingSimulationSteps = 0;
            mRuntimeControlMode = RuntimeControlMode::Paused;
            if (mLevel) mLevel->SetRuntimePaused(true);
        }
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
        (void)mRegistry.Register({"runtime.memory",
            "Diagnostic only: sample=true arms one next-Tick before/after-mailbox memory pair; poll completedSequence. Does not change acceptance budgets.",
            "runtime:read", {{"sample", "optional boolean; default false"}}}, [this](const Json& params)
            {
                RequireOnlyParams(params, "runtime.memory", {"sample"});
                if (params.contains("sample") && !params.at("sample").is_boolean())
                    throw std::invalid_argument("runtime.memory sample must be boolean.");
                const bool bRequested = params.value("sample", false);
                const bool bAccepted = !bRequested || !mMemorySamplePending;
                if (bRequested && bAccepted)
                    mMemorySamplePending = true;
                const auto encode = [](const PlatformProcessMemorySnapshot& value)
                {
                    return Json{{"processAvailable",value.bProcessAvailable},
                        {"defaultHeapAvailable",value.bDefaultHeapAvailable},
                        {"processError",value.ProcessError},{"defaultHeapError",value.DefaultHeapError},
                        {"privateBytes",value.PrivateBytes},{"workingSetBytes",value.WorkingSetBytes},
                        {"defaultHeapAllocatedBytes",value.DefaultHeapAllocatedBytes},
                        {"defaultHeapCommittedBytes",value.DefaultHeapCommittedBytes},
                        {"defaultHeapReservedBytes",value.DefaultHeapReservedBytes}};
                };
                return Json{{"accepted",bAccepted},{"pending",mMemorySamplePending},
                    {"completedSequence",mMemoryCompletedSequence},
                    {"beforeMailbox",mMemoryCompletedSequence ? encode(mMemoryBeforeMailbox) : Json(nullptr)},
                    {"afterMailbox",mMemoryCompletedSequence ? encode(mMemoryAfterMailbox) : Json(nullptr)},
                    {"scope","One owner-thread Tick before Poll and after PublishStatus; process and default heap sampled sequentially, other threads may allocate. Default heap excludes other heaps/direct/driver allocations; diagnostic only."}};
            });
        (void)mRegistry.Register({"vfx.describe", "Describe current VFX capabilities and parameter contracts.",
            "vfx:read", Json::object()}, [](const Json& params)
            { RequireNoParams(params, "vfx.describe"); return EffekseerRuntime::Describe(); });
        (void)mRegistry.Register({"vfx.quality",
            "Query with {} or set tier at the pre-update closed-host boundary. Capacity changes require stopped effects; accepted=false preserves actual policy.",
            "vfx:write", {{"tier","optional: Low|Medium|High"}}}, [](const Json& params)
            {
                if (!params.is_object()) throw std::invalid_argument("vfx.quality requires an object.");
                RequireOnlyParams(params, "vfx.quality", {"tier"});
                auto desired = EffekseerRuntime::GetQualityTier();
                if (params.contains("tier"))
                {
                    if (!params.at("tier").is_string())
                        throw std::invalid_argument("vfx.quality tier must be Low, Medium or High.");
                    const auto tier = params.at("tier").get<std::string>();
                    if (tier == "Low") desired = VFXQualityTier::Low;
                    else if (tier == "Medium") desired = VFXQualityTier::Medium;
                    else if (tier == "High") desired = VFXQualityTier::High;
                    else throw std::invalid_argument("vfx.quality tier must be Low, Medium or High.");
                    EffekseerRuntime::SetQualityTier(desired);
                }
                const auto actual = EffekseerRuntime::GetQualityTier();
                const char* tier = actual == VFXQualityTier::Low ? "Low" :
                    actual == VFXQualityTier::Medium ? "Medium" : "High";
                const auto gpu = EffekseerRuntime::Describe().at("gpu");
                return Json{{"accepted",actual == desired},{"tier",tier},
                    {"requested",gpu.at("requested")},{"enabled",gpu.at("enabled")}};
            });
        (void)mRegistry.Register({"vfx.reload", "Transactionally replace a cached effect; playing handles retain the old version.",
            "vfx:write", {{"path","project-relative efkpkg"}}}, [](const Json& params)
            { return Json{{"accepted",EffekseerRuntime::ReloadEffect(params.at("path").get<std::string>())},
                {"playingInstances","retain_previous_version"}}; });
        (void)mRegistry.Register({"vfx.stats", "Read VFX metrics with explicit measurement status.",
            "vfx:read", Json::object()}, [](const Json& params)
            {
                RequireNoParams(params, "vfx.stats");
                return EffekseerRuntime::DescribeStats();
            });
        (void)mRegistry.Register({"vfx.play", "Play an effect with a reproducible seed before its first update.",
            "vfx:write", {{"path", "project-relative efkpkg"}, {"seed", "int32"}}},
            [](const Json& params)
            {
                VFXPlayParams play;
                const auto position = params.value("position", std::vector<float>{0,0,0});
                if (position.size() != 3)
                    throw std::invalid_argument("position requires three finite world coordinates");
                for (float value : position)
                    if (!std::isfinite(value)) throw std::invalid_argument("position must be finite");
                play.Position = {position[0], position[1], position[2]};
                play.Scale = params.value("scale", 1.0f);
                play.Speed = params.value("speed", 1.0f);
                if (!std::isfinite(play.Scale) || play.Scale <= 0 || play.Scale > 1000 ||
                    !std::isfinite(play.Speed) || play.Speed < 0 || play.Speed > 16)
                    throw std::invalid_argument("scale in (0,1000], speed in [0,16] required");
                play.bOverrideRandomSeed = params.contains("seed");
                if (play.bOverrideRandomSeed && (!params.at("seed").is_number_integer() ||
                    params.at("seed") < Json(INT32_MIN) || params.at("seed") > Json(INT32_MAX)))
                    throw std::invalid_argument("seed must be an integer in int32 range");
                play.RandomSeed = params.value("seed", int32_t{0});
                const auto handle = EffekseerRuntime::PlayEffect(params.at("path").get<std::string>(), play);
                if (handle < 0)
                    throw std::invalid_argument(EffekseerRuntime::IsGpuParticlesEnabled()
                        ? "VFX play rejected; check the package format, resource availability and current VFX budgets."
                        : "VFX play rejected; GPU particles are disabled in this session. Use a compatible CPU package and check resource availability and VFX budgets.");
                return Json{{"handle", handle}, {"playing", EffekseerRuntime::IsEffectPlaying(handle)}};
            });
        (void)mRegistry.Register({"vfx.control", "Query, pause, resume, stop or adjust a live effect.",
            "vfx:write", {{"handle", "int32"}, {"action", "status|pause|resume|stop|speed|color|dynamicInput|parameter|transform"}}},
            [](const Json& params)
            {
                if (!params.at("handle").is_number_integer() || params.at("handle") < Json(INT32_MIN) ||
                    params.at("handle") > Json(INT32_MAX))
                    throw std::invalid_argument("handle must be an integer in int32 range");
                const int32_t handle = params.at("handle").get<int32_t>();
                const auto action = params.at("action").get<std::string>();
                bool accepted = false;
                if (action == "status") accepted = true;
                else if (action == "pause" || action == "resume")
                    accepted = EffekseerRuntime::SetEffectPaused(handle, action == "pause");
                else if (action == "stop") { EffekseerRuntime::StopEffect(handle); accepted = true; }
                else if (action == "speed")
                    accepted = EffekseerRuntime::SetEffectSpeed(handle, params.at("value").get<float>());
                else if (action == "parameter")
                    accepted = EffekseerRuntime::SetEffectParameter(handle, params.at("name").get<std::string>(), params.at("value").get<float>());
                else if (action == "dynamicInput")
                    accepted = EffekseerRuntime::SetEffectDynamicInput(handle, params.at("index").get<uint32_t>(), params.at("value").get<float>());
                else if (action == "transform")
                {
                    const auto position = params.at("position").get<std::vector<float>>();
                    const auto forward = params.value("forward", std::vector<float>{0,0,-1});
                    if (position.size() != 3 || forward.size() != 3)
                        throw std::invalid_argument("transform requires position and forward triples");
                    for (const float value : position)
                        if (!std::isfinite(value)) throw std::invalid_argument("position must be finite");
                    for (const float value : forward)
                        if (!std::isfinite(value)) throw std::invalid_argument("forward must be finite");
                    accepted = EffekseerRuntime::SetEffectTransform(handle,
                        {position[0],position[1],position[2]}, {forward[0],forward[1],forward[2]});
                }
                else if (action == "color")
                {
                    const auto color = params.at("value").get<std::vector<float>>();
                    if (color.size() != 4) throw std::invalid_argument("color requires RGBA values");
                    accepted = EffekseerRuntime::SetEffectColor(handle, {color[0],color[1],color[2],color[3]});
                }
                else throw std::invalid_argument("unknown VFX control action");
                auto status = EffekseerRuntime::GetEffectStatus(handle);
                status["accepted"] = accepted;
                return status;
            });
		(void)mRegistry.Register({"render.view.create", "Create one session-owned independent offscreen view of the live 3D world.",
			"render:validation", {{"width","integer 64..3840"},{"height","integer 64..2160"}}},
			[this](const Json& params) {
				RequireOnlyParams(params, "render.view.create", {"width","height"});
				if (!params.contains("width") || !params.contains("height") ||
					!params.at("width").is_number_integer() || !params.at("height").is_number_integer())
					throw std::invalid_argument("render.view.create requires integer width and height.");
				const int width = params.at("width").get<int>(), height = params.at("height").get<int>();
				if (width < 64 || width > 3840 || height < 64 || height > 2160)
					throw std::invalid_argument("Validation view extent exceeds its native-4K budget.");
				if (mRenderValidation && mRenderValidation->View)
					throw std::logic_error("The session already owns a validation view; destroy it first.");
				auto state = CreateScope<AgentRenderValidationState>();
				state->View = Application::GetInstance().CreateOffscreenView(width, height, "Agent rendering validation");
				mRenderValidation = std::move(state);
				SubmitValidationView();
				return BuildValidationViewStatus();
			});
		(void)mRegistry.Register({"render.view.status", "Read independent view graph execution and capture completion.","render:validation",Json::object()},
			[this](const Json& params) { RequireNoParams(params,"render.view.status"); return BuildValidationViewStatus(); });
		(void)mRegistry.Register({"render.view.capture", "Queue a native-size offscreen PNG into this session's captures directory.","render:validation",Json::object()},
			[this](const Json& params) {
				RequireNoParams(params,"render.view.capture");
				if (!mRenderValidation || !mRenderValidation->View) throw std::logic_error("No session-owned validation view.");
				auto& application = Application::GetInstance();
				if (mRenderValidation->Capture && application.IsOffscreenCaptureValid(mRenderValidation->Capture))
				{
					const auto status = application.GetOffscreenCaptureResult(mRenderValidation->Capture).Status;
					if (status == TextureReadbackStatus::PendingSubmission || status == TextureReadbackStatus::InFlight)
						throw std::logic_error("A validation view capture is already pending.");
					application.ReleaseOffscreenCapture(mRenderValidation->Capture);
				}
				const auto root = mConfig.SessionDirectory / "captures";
				if (std::filesystem::is_symlink(std::filesystem::symlink_status(root)) || !std::filesystem::is_directory(root))
					throw std::runtime_error("Session capture directory is unavailable.");
				const auto path = root / ("offscreen-" + std::to_string(mRenderValidation->NextCapture++) + ".png");
				if (std::filesystem::exists(path)) throw std::runtime_error("Validation capture output already exists.");
				mRenderValidation->Capture = application.RequestOffscreenCapture(mRenderValidation->View, path);
				SubmitValidationView();
				return BuildValidationViewStatus();
			});
		(void)mRegistry.Register({"render.view.destroy", "Release the view owned by this session and its capture handles.","render:validation",Json::object()},
			[this](const Json& params) { RequireNoParams(params,"render.view.destroy"); DestroyValidationView(); return Json{{"ok",true},{"available",false}}; });
		(void)mRegistry.Register({"render.animation.status", "Inspect bounded bone palettes and skin weights from the last successfully submitted main RenderWorld.","read-only:render-animation",Json::object()},
			[this](const Json& params) { RequireNoParams(params,"render.animation.status"); return BuildAnimationStatus(); });
		(void)mRegistry.Register({"render.object.lookup", "Resolve a captured surface-id object index to the last submitted render object and its scene entity.",
			"read-only:render-debug",{{"objectGpuIndex","uint32 from the red channel of a surface-id raw capture"}}},
			[this](const Json& params) {
				if (params.size() != 1 || !params.contains("objectGpuIndex") ||
					!params.at("objectGpuIndex").is_number_unsigned() ||
					params.at("objectGpuIndex").get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
					throw std::invalid_argument("render.object.lookup requires a uint32 objectGpuIndex.");
				const auto world = Application::GetInstance().GetLastSubmittedRenderWorld();
				if (!world) return Json{{"available",false},{"found",false}};
				const auto index = params.at("objectGpuIndex").get<std::uint32_t>();
				Json result{{"available",true},{"found",false},
					{"objectGpuIndex",index},{"renderWorldFrame",world->FrameNumber},
					{"sceneGeneration",world->SceneGeneration}};
				const Ref<Level> activeLevel = mAuthorPreview ? mAuthorPreview : mLevel;
				const auto* renderSystem = activeLevel ? activeLevel->TryGetSystem<RenderSystem3D>() : nullptr;
				const bool liveSceneMatches = renderSystem &&
					renderSystem->GetSceneGeneration() == world->SceneGeneration;
				for (const auto& object : world->Objects)
				{
					if (object.ObjectGpuIndex != index) continue;
					result["found"] = true;
					result["entityId"] = object.EntityId;
					result["meshAssetPath"] = object.MeshAssetPath;
					result["uuid"] = nullptr;
					result["tag"] = nullptr;
					if (liveSceneMatches && object.EntityId >= 0)
					{
						const auto handle = static_cast<entt::entity>(static_cast<std::uint32_t>(object.EntityId));
						if (activeLevel->mRegistry.valid(handle))
						{
							if (activeLevel->mRegistry.all_of<IDComponent>(handle))
								result["uuid"] = activeLevel->mRegistry.get<IDComponent>(handle).ID.str();
							if (activeLevel->mRegistry.all_of<TagComponent>(handle))
								result["tag"] = Utf8Prefix(activeLevel->mRegistry.get<TagComponent>(handle).Tag, 256);
						}
					}
					Json materials = Json::array();
					for (const auto& section : object.Sections)
						materials.push_back(section.GetMaterialInstance().AssetPath);
					result["materials"] = std::move(materials);
					return result;
				}
				return result;
			});
		(void)mRegistry.Register({"render.material.refresh", "Publish an already compiled Surface material artifact through the existing frame-boundary refresh.",
			"render:validation",{{"path","Shaders/<relative>.hlsl"}}},
			[](const Json& params) {
				if (params.size() != 1 || !params.contains("path") || !params.at("path").is_string())
					throw std::invalid_argument("render.material.refresh requires a path.");
				const std::string path = params.at("path").get<std::string>();
				std::string normalized;
				if (path.size() > 400 || !Utils::TryNormalizeProjectRelativePath(path, normalized) ||
					!normalized.starts_with("Shaders/") || !normalized.ends_with(".hlsl"))
					throw std::invalid_argument("Material refresh requires a safe Shaders/<relative>.hlsl path.");
				const auto projectSource = ConfigManager::GetProjectFullPath(normalized);
				const bool projectShader = std::filesystem::is_regular_file(projectSource);
				const auto result = Application::GetInstance().RefreshMaterialShaderAtFrameBoundary(normalized);
				Json response{{"succeeded",result.Succeeded()},{"published",result.bPublished},{"path",normalized},
					{"shaderRoot",Utils::PathToUtf8(projectShader ? ConfigManager::GetProjectFullPath("Shaders") : ConfigManager::GetEngineContentFullPath("Shaders"))},
					{"sourceFile",Utils::PathToUtf8(projectShader ? projectSource : ConfigManager::GetEngineContentFullPath(normalized))},
					{"includeRoot",Utils::PathToUtf8(ConfigManager::GetEngineContentFullPath("Shaders"))},
					{"artifactOutput",Utils::PathToUtf8(ConfigManager::GetEngineGeneratedContentFullPath("Shaders"))},
					{"diagnostics",Json::array()}};
				for (size_t i = 0; i < std::min(size_t(8), result.Diagnostics.size()); ++i)
					response["diagnostics"].push_back({{"code",result.Diagnostics[i].Code},
						{"message",Utf8Prefix(result.Diagnostics[i].Message,512)}});
				if (result.Program)
				{
					response["generation"] = result.Program->GetGeneration();
					response["sourceHash"] = result.Program->GetIdentity().RecursiveContentHash;
					response["artifactHash"] = result.Program->GetIdentity().ArtifactIdentityHash;
				}
				return response;
			});
        (void)mRegistry.Register({"ui.element.rect", "Read the visible active-document element's rectangle for bounded UI input.",
            "read-only:ui", {{"id","element id"}}}, [](const Json& params) {
                RequireOnlyParams(params, "ui.element.rect", {"id"});
                if (!params.contains("id") || !params["id"].is_string())
                    throw std::invalid_argument("ui.element.rect requires an element id");
                std::array<float,4> rect{};
                const bool visible = RmlUiManager::GetInstance().GetActiveElementRect(params["id"].get<std::string>(), rect);
                return Json{{"visible",visible},{"rect",rect}};
            });
        (void)mRegistry.Register({"input.pointer", "Queue a session-owned pointer and right button, optionally advancing once per simulation step; input.clear releases both.",
            "control:synthetic-pointer", {{"x","number within viewport"},{"y","number within viewport"},{"rightHeld","bool"},
                {"steps","optional integer 1..600; requires stepX and stepY"},
                {"stepX","optional horizontal pixels per simulation step"},{"stepY","optional vertical pixels per simulation step"}}},
            [this](const Json& params) { auto& app = Application::GetInstance();
                return mInputController.Pointer(params, app.GetRenderWidth(), app.GetRenderHeight()); });
		(void)mRegistry.Register({"window.resize", "Set an exact windowed client extent; GPU targets follow at the frame boundary.",
			"runtime:window", {{"width", "integer 64..8192"}, {"height", "integer 64..8192"}}},
			[](const Json& params) {
				if (params.size() != 2 || !params.contains("width") || !params.contains("height") ||
					!params.at("width").is_number_integer() || !params.at("height").is_number_integer())
					throw std::invalid_argument("window.resize requires integer width and height only.");
				const int64_t width = params.at("width").get<int64_t>();
				const int64_t height = params.at("height").get<int64_t>();
				if (width < 64 || width > 8192 || height < 64 || height > 8192)
					throw std::invalid_argument("window.resize dimensions must be in 64..8192.");
				Window& window = Application::GetWindow();
				window.SetClientSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
				return Json{{"ok",true},{"clientWidth",window.GetWidth()},{"clientHeight",window.GetHeight()},
					{"completion","clientResized"},{"observation","renderWidth/renderHeight update after the current frame boundary"}};
			});
		(void)mRegistry.Register({"camera.cut", "Invalidate temporal history; optionally schedule a capture relative to this same request.",
			"runtime:camera", Json{{"capture", "optional capture.frame request; requires paused runtime, delayFrames relative to this cut"}}},
			[this](const Json& params) {
				for (const auto& [name, value] : params.items())
				{
					(void)value;
					if (name != "capture") throw std::invalid_argument("Unknown camera.cut parameter.");
				}
				const Ref<Level> activeLevel = mAuthorPreview ? mAuthorPreview : mLevel;
				auto* renderSystem = activeLevel ? activeLevel->TryGetSystem<RenderSystem3D>() : nullptr;
				if (!renderSystem) throw std::invalid_argument("camera.cut requires an active 3D render system.");
				Json capture = Json::object();
				const auto requestFrame = Application::GetInstance().GetFrameNumber();
				if (params.contains("capture"))
				{
					if (mAuthorPreview || !mLevel || mPendingSimulationSteps || mStepInProgress ||
						mRuntimeControlMode != RuntimeControlMode::Paused)
						throw std::invalid_argument("A cut-relative capture requires an idle paused runtime.");
					// Validate/reserve the capture first. RequestCameraCut cannot throw,
					// so a rejected output never changes temporal history.
					capture = mCaptureController.Request(params.at("capture"), requestFrame);
				}
				renderSystem->RequestCameraCut();
				return Json{{"ok",true},{"completion","queuedForNextExtraction"},
					{"sceneGeneration",renderSystem->GetSceneGeneration()},{"target",mAuthorPreview ? "preview" : "runtime"},
					{"requestSimulationClockFrame", requestFrame}, {"capture", std::move(capture)}};
			});
        (void)mRegistry.Register({"render.validation.upscalingFailure", "Inject one FSR failure at the next frame boundary; never persists settings.",
            "runtime:render-validation", {{"stage", "jitter or dispatch"},
                {"capture", "optional capture.frame request relative to the failed frame"}}},
            [this](const Json& params) {
                RequireOnlyParams(params, "render.validation.upscalingFailure", {"stage", "capture"});
                if (mAuthorPreview || !mLevel || mPendingSimulationSteps || mStepInProgress ||
                    mRuntimeControlMode != RuntimeControlMode::Paused)
                    throw std::invalid_argument("FSR failure diagnostics require an idle paused runtime.");
                const std::string stage = params.value("stage", std::string{});
                if (stage != "jitter" && stage != "dispatch")
                    throw std::invalid_argument("FSR failure stage must be jitter or dispatch.");
                auto& app = Application::GetInstance();
                if (!app.IsFrameCaptureSourceAvailable(FrameCaptureSource::UpscaleMotion))
                    throw std::invalid_argument("FSR must be active for failure diagnostics.");
                Json capture = Json::object();
                if (params.contains("capture"))
                    capture = mCaptureController.Request(params.at("capture"), app.GetFrameNumber());
                if (!app.InjectUpscalingFailureForValidation(stage == "dispatch"))
                    throw std::logic_error("FSR failure could not be armed.");
                return Json{{"armed", true}, {"stage", stage}, {"capture", std::move(capture)}};
            });
		(void)mRegistry.Register({"render.validation.transport", "Change an existing runtime entity for bounded light-transport diagnostics; never saves the scene.",
            "runtime:render-validation", {{"uuid", "existing entity UUID"},
                {"position", "optional finite vec3"}, {"rotation", "optional Euler radians vec3"},
                {"directionalLux", "optional finite 0..1000000"},
                {"capture", "optional capture.frame request, with delayFrames relative to this mutation"}}},
            [this](const Json& params) {
                if (!mLevel || mRuntimeControlMode != RuntimeControlMode::Paused)
                    throw std::invalid_argument("Transport diagnostics require a paused runtime level.");
                for (const auto& [key, value] : params.items())
                    if (key != "uuid" && key != "position" && key != "rotation" && key != "directionalLux" && key != "capture")
                        throw std::invalid_argument("Unknown transport diagnostic parameter.");
                Entity entity = mLevel->TryGetEntityByUUID(UUID(params.at("uuid").get<std::string>()));
                if (!entity) throw std::invalid_argument("Transport diagnostic entity was not found.");
                const auto readVector = [&params](const char* name) {
                    const auto& array = params.at(name);
                    if (!array.is_array() || array.size() != 3)
                        throw std::invalid_argument("Transport vector requires three numbers.");
                    glm::vec3 result;
                    for (int i = 0; i < 3; ++i) {
                        result[i] = array[i].get<float>();
                        if (!std::isfinite(result[i]) || std::abs(result[i]) > 10000.0f)
                            throw std::invalid_argument("Transport vector is outside the finite diagnostic range.");
                    }
                    return result;
                };
                // Validate the entire request before changing any component.
                const glm::vec3 position = params.contains("position") ? readVector("position") : glm::vec3(0);
                const glm::vec3 rotation = params.contains("rotation") ? readVector("rotation") : glm::vec3(0);
                const float lux = params.value("directionalLux", 0.0f);
                if (!std::isfinite(lux) || lux < 0.0f || lux > 1000000.0f ||
                    (params.contains("directionalLux") && !entity.HasComponent<DirectionalLightComponent>()) ||
                    ((params.contains("position") || params.contains("rotation")) && !entity.HasComponent<TransformComponent>()))
                    throw std::invalid_argument("Transport diagnostic component or illuminance is invalid.");
                Json capture = Json::object();
                if (params.contains("capture"))
                    capture = mCaptureController.Request(params.at("capture"), Application::GetInstance().GetFrameNumber());
                if (params.contains("position")) entity.GetComponent<TransformComponent>().Translation = position;
                if (params.contains("rotation")) entity.GetComponent<TransformComponent>().Rotation = rotation;
                if (params.contains("directionalLux")) entity.GetComponent<DirectionalLightComponent>().Intensity = lux;
                return Json{{"applied", true}, {"persisted", false}, {"request", params},
                    {"mutationRenderFrame", Application::GetInstance().GetFrameNumber()}, {"capture", capture}};
            });
		(void)mRegistry.Register({"render.texture.reload", "Reprepare a live texture and publish it after GPU upload completion while retaining its handle.",
			"render:write", {{"path", "project-relative live texture asset path"}}},
			[](const Json& params) {
				RequireOnlyParams(params, "render.texture.reload", {"path"});
				if (!params.contains("path") || !params.at("path").is_string())
					throw std::invalid_argument("render.texture.reload requires a string path.");
				const size_t queued = Application::GetInstance().RequestTextureReload(
					params.at("path").get_ref<const std::string&>());
				return Json{{"ok",true},{"queued",queued},{"completion","frame_boundary"}};
			});
		(void)mRegistry.Register({"render.texture.readyProbe", "Validate an async request followed immediately by a shared blocking-ready lease.",
			"render:write", {{"path", "project-relative texture asset path"},
				{"maximumDimension", "optional uint32; distinct values force distinct cache keys"},
				{"colorSpace", "optional linear or srgb"},
				{"generateMipmaps", "optional boolean"},
				{"preserveAlphaCoverage", "optional boolean"},
				{"alphaCoverageCutoff", "optional float"},
				{"readbackMips", "optional boolean; bounded RGBA8 GPU mip readback"}}},
			[](const Json& params) {
				RequireOnlyParams(params, "render.texture.readyProbe", {"path", "maximumDimension",
					"colorSpace", "generateMipmaps", "preserveAlphaCoverage", "alphaCoverageCutoff",
					"readbackMips"});
				if (!params.contains("path") || !params.at("path").is_string())
					throw std::invalid_argument("render.texture.readyProbe requires a string path.");
				uint32_t maximumDimension = 0;
				if (params.contains("maximumDimension"))
				{
					if (!params.at("maximumDimension").is_number_unsigned())
						throw std::invalid_argument("maximumDimension must be an unsigned integer.");
					maximumDimension = params.at("maximumDimension").get<uint32_t>();
				}
				if (params.contains("colorSpace") && !params.at("colorSpace").is_string())
					throw std::invalid_argument("colorSpace must be linear or srgb.");
				const std::string colorSpace = params.value("colorSpace", std::string("linear"));
				if (colorSpace != "linear" && colorSpace != "srgb")
					throw std::invalid_argument("colorSpace must be linear or srgb.");
				if (params.contains("generateMipmaps") && !params.at("generateMipmaps").is_boolean())
					throw std::invalid_argument("generateMipmaps must be a boolean.");
				if (params.contains("preserveAlphaCoverage") && !params.at("preserveAlphaCoverage").is_boolean())
					throw std::invalid_argument("preserveAlphaCoverage must be a boolean.");
				if (params.contains("alphaCoverageCutoff") && !params.at("alphaCoverageCutoff").is_number())
					throw std::invalid_argument("alphaCoverageCutoff must be a number.");
				if (params.contains("readbackMips") && !params.at("readbackMips").is_boolean())
					throw std::invalid_argument("readbackMips must be a boolean.");
				const TextureReadyProbeResult result = Application::GetInstance().ProbeTextureReady(
					params.at("path").get_ref<const std::string&>(), maximumDimension,
					colorSpace == "srgb", params.value("generateMipmaps", false),
					params.value("preserveAlphaCoverage", false),
					params.value("alphaCoverageCutoff", 0.5f), params.value("readbackMips", false));
				Json mips = Json::array();
				for (const auto& mip : result.MipReadback)
					mips.push_back(Json{{"width", mip.Width}, {"height", mip.Height},
						{"hash", mip.Hash}, {"coveredPixels", mip.CoveredPixels}});
				return Json{{"sharedHandle", result.SharedHandle}, {"ready", true},
					{"width", result.Width}, {"height", result.Height},
					{"mipLevels", result.MipLevels}, {"format", result.FormatName},
					{"mipReadback", std::move(mips)}};
			});
		(void)mRegistry.Register({"render.texture.burstProbe", "Request distinct texture variants together, then confirm shared ready leases and release every handle.",
			"render:write", {{"path", "project-relative texture asset path"},
				{"count", "number of distinct variants, 1..96"}}},
			[](const Json& params) {
				RequireOnlyParams(params, "render.texture.burstProbe", {"path", "count"});
				if (!params.contains("path") || !params.at("path").is_string() ||
					!params.contains("count") || !params.at("count").is_number_unsigned())
					throw std::invalid_argument("render.texture.burstProbe requires path and unsigned count.");
				const auto result = Application::GetInstance().ProbeTextureBurst(
					params.at("path").get_ref<const std::string&>(),
					params.at("count").get<uint32_t>());
				return Json{{"requested", result.Requested}, {"ready", result.Ready},
					{"shared", result.Shared}};
			});
		(void)mRegistry.Register({"render.texture.streamingProbe", "Observe a burst of async textures across render frames without blocking-ready leases.",
			"render:write", {{"action", "begin, status, or end"},
				{"path", "project-relative texture asset path for begin"},
				{"count", "number of distinct variants for begin, 1..96"},
				{"baseDimension", "optional first maximum dimension, 32..2048; default 64"}}},
			[](const Json& params) {
				RequireOnlyParams(params, "render.texture.streamingProbe", {"action", "path", "count", "baseDimension"});
				if (!params.contains("action") || !params.at("action").is_string())
					throw std::invalid_argument("render.texture.streamingProbe requires an action.");
				const std::string action = params.at("action").get<std::string>();
				TextureStreamingProbeResult result;
				if (action == "begin")
				{
					if (!params.contains("path") || !params.at("path").is_string() ||
						!params.contains("count") || !params.at("count").is_number_unsigned())
						throw std::invalid_argument("Streaming probe begin requires path and unsigned count.");
					if (params.contains("baseDimension") && !params.at("baseDimension").is_number_unsigned())
						throw std::invalid_argument("Streaming probe baseDimension must be unsigned.");
					result = Application::GetInstance().BeginTextureStreamingProbe(
						params.at("path").get_ref<const std::string&>(),
						params.at("count").get<uint32_t>(),
						params.value("baseDimension", 64u));
				}
				else if (action == "status" || action == "end")
				{
					if (params.contains("path") || params.contains("count") ||
						params.contains("baseDimension"))
						throw std::invalid_argument("Streaming probe status/end accepts action only.");
					result = Application::GetInstance().InspectTextureStreamingProbe(action == "end");
				}
				else throw std::invalid_argument("Streaming probe action must be begin, status, or end.");
				return Json{{"requested", result.Requested}, {"ready", result.Ready},
					{"pending", result.Pending}};
			});
		(void)mRegistry.Register({"runtime.scene.load", "Queue a transactional runtime scene load; optional forced reload replaces the current scene generation.",
			"runtime:scene", {{"path", "project-relative Assets/*.he"}, {"forceReload", "optional boolean, default false"}}},
			[this](const Json& params) {
				for (const auto& [name, value] : params.items())
				{
					(void)value;
					if (name != "path" && name != "forceReload")
						throw std::invalid_argument("runtime.scene.load contains an unknown parameter.");
				}
				if (!params.contains("path") || !params.at("path").is_string() ||
					(params.contains("forceReload") && !params.at("forceReload").is_boolean()))
					throw std::invalid_argument("runtime.scene.load requires a string path and optional boolean forceReload.");
				const SceneDocumentPath path = ResolveSceneDocumentPath(ConfigManager::GetInstance().GetRootFolder(),
					params.at("path").get<std::string>());
				if (!path.Key.CachePath.starts_with("assets/") || !std::filesystem::is_regular_file(path.FullPath))
					throw std::invalid_argument("runtime.scene.load requires an existing scene below Project/Assets.");
				const bool bForceReload = params.value("forceReload", false);
				const auto* renderSystem = mLevel ? mLevel->TryGetSystem<RenderSystem3D>() : nullptr;
				const uint64_t previousGeneration = renderSystem ? renderSystem->GetSceneGeneration() : 0u;
				if (!Application::GetInstance().RequestRuntimeScene(path.Key.Path, bForceReload))
					return Json{{"ok",false},{"error",{{"code","scene_request_pending"},{"message","A runtime scene request is already queued."}}}};
				mAuthorPreview.reset();
				return Json{{"ok",true},{"path",path.Key.Path},{"forceReload",bForceReload},
					{"previousSceneGeneration",previousGeneration},{"completion","queued"},
					{"observation","world.summary scene and sceneGeneration identify committed loads; logs report transaction failures"}};
			});
		(void)mRegistry.Register({"scene.open", "Open an explicit authoring document, isolated from simulation.", "authoring:scene", {{"path", "project-relative .he"}}},
			[this](const Json& params) {
				if (params.size() != 1 || !params.contains("path")) throw std::invalid_argument("scene.open requires path only.");
				if (mSceneEditing && mSceneEditing->IsDirty()) throw std::invalid_argument("Save the current authoring document before opening another scene.");
				auto service = CreateScope<SceneEditingService>();
				service->Open(ConfigManager::GetInstance().GetRootFolder(), params.at("path"));
				mAuthorPreview.reset(); mSceneEditing = std::move(service);
				return mSceneEditing->Execute("describe", Json::object());
			});
		for (const std::string action : {"describe", "query", "plan", "apply", "diff", "validate", "save", "undo", "redo", "export_prefab"})
			(void)mRegistry.Register({"scene." + action, action == "export_prefab" ? "Export the current author-document subtree to a new Assets .hprefab; no runtime ECS synchronization or overwrite." : "Shared authoring scene service; discover schemas on demand.", "authoring:scene",
                    action == "export_prefab" ? Json{{"sceneId","bound author session UUID"},{"baseRevision","current revision"},{"requestId","idempotent request ID"},{"entity","subtree root UUID"},{"path","new Assets/*.hprefab, existing parent"}} : Json{{"sceneId", "bound authoring session UUID"}}},
				[this, action](const Json& params) {
					if (!mSceneEditing) return Json{{"ok",false},{"error",{{"code","authoring_not_open"},{"message","Use scene.open with an explicit project-relative .he path."}}}};
					Json result = mSceneEditing->Execute(action, params);
					if (result.value("ok", false) && (action == "apply" || action == "undo" || action == "redo"))
					{
						mAuthorPreview.reset();
						result["preview"] = "stale; invoke scene.preview then capture.frame";
					}
					return result;
				});
		(void)mRegistry.Register({"scene.preview", "Materialize the author document for render-only preview; capture.frame waits for a subsequent frame.", "authoring:preview", Json::object()},
			[this](const Json& params) {
				if (!mSceneEditing || params.at("sceneId") != mSceneEditing->GetSceneId() || params.at("revision") != mSceneEditing->GetRevision())
					return Json{{"ok",false},{"error",{{"code","revision_conflict"},{"message","Preview requires the current sceneId and revision."}}}};
				// Preview never starts physics, scripts or animation. Only the renderer
				// consumes this disposable Level in GameLayer::OnUpdate.
				auto preview = CreateRef<Level>();
				Json visual = Json::object();
				for (auto& [id, entity] : mSceneEditing->GetDocument().items())
					for (const char* component : {"TagComponent","TransformComponent","MeshComponent","CameraComponent",
						"PointLightComponent","DirectionalLightComponent","EnvironmentLightComponent","SpotLightComponent","ReflectionProbeComponent"})
						if (entity.contains(component)) visual[id][component] = entity.at(component);
				ApplySceneEditingValues(preview, visual);
				preview->OnViewportResize(Application::GetInstance().GetRenderWidth(), Application::GetInstance().GetRenderHeight());
				mAuthorPreview = std::move(preview);
				return Json{{"ok",true},{"sceneId",mSceneEditing->GetSceneId()},{"revision",mSceneEditing->GetRevision()},
					{"completion","resourcesMaterialized"},{"observation","capture.frame waits for the next rendered frame"},
					{"limitations",Json::array({"physics, scripts and animation do not advance in author preview; use runtime.scene.load for runtime coverage"})}};
			});
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
						{ "patch", GetGlobalIlluminationQualityPresetPatch(
							preset, snapshot.Effective.GlobalIllumination.Technique) },
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
                const auto before = Application::GetInstance().GetConfigurationPreparationStats();
				auto result = SerializeRenderSettingsApplyResult(
					RenderSettingsService::GetInstance().ApplyDraft(draft, bPersist));
                const auto preparation = Application::GetInstance().GetConfigurationPreparationStats();
                if (preparation.AttemptId != before.AttemptId)
                {
                    result["runtimePreparation"] = Json{
                        {"attemptId", preparation.AttemptId}, {"committed", preparation.bCommitted},
                        {"preparedViews", preparation.PreparedViews},
                        {"preparationMilliseconds", preparation.PreparationMilliseconds},
                        {"transactionMilliseconds", preparation.TransactionMilliseconds},
                        {"observedActivePoolNominalBytes", preparation.ObservedActivePoolNominalBytes},
                        {"preparedCandidatePoolNominalBytes", preparation.PreparedCandidatePoolNominalBytes},
                        {"memoryInterpretation", "Nominal observed active pools and successfully prepared candidate pools only; shared completed transient allocations are counted in both pools. Excludes imported histories/assets, retired pools, failed partial allocations and native allocation alignment. This is not a measured VRAM peak. Preparation includes completion wait for transactional storage reuse."}
                    };
                }
                return result;
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

        (void)mRegistry.Register({"capture.sequence", "Capture 8..16 consecutive rendered frames with one simulation step per image, then pause.",
            "control:framebuffer-capture", Json{{"prefix","safe filename prefix"},{"frames","integer 8..16"},
                {"format","optional png/pfm/raw; default png"},
                {"source","optional automatic/depth/motion/upscale-motion/upscale-color/upscale-opaque/normal/surface-id/reactive/composition; explicit sources require raw"},
                {"cameraCut","optional bool; atomic reset after all validation"},
                {"settleFrames","optional integer 0..600 successful paused submissions; requires cameraCut=true"}}},
            [this](const Json& params) {
                if (Application::GetInstance().GetRunOptions().FixedDeltaSeconds <= 0.0f)
                    throw std::logic_error("capture.sequence requires a fixed-delta Agent session");
                if (!mLevel || mPendingSimulationSteps || mStepInProgress || mRuntimeControlMode != RuntimeControlMode::Paused)
                    throw std::logic_error("capture.sequence requires paused simulation with no queued steps");
                const bool bCutRequested = params.contains("cameraCut") && params["cameraCut"].is_boolean() && params["cameraCut"].get<bool>();
                auto* renderSystem = mLevel->TryGetSystem<RenderSystem3D>();
                if (bCutRequested && (mAuthorPreview || !renderSystem))
                    throw std::invalid_argument("Atomic sequence cut requires the active runtime 3D renderer");
                auto result = mCaptureController.RequestSequence(params, Application::GetInstance(),
                    Application::GetInstance().GetFrameNumber());
                // RequestSequence validates/reserves every output and allocates before this noexcept mutation.
                if (bCutRequested) renderSystem->RequestCameraCut();
                mPendingSimulationSteps = params.at("frames").get<size_t>();
                mLevel->SetRuntimePaused(true);
                return result;
            });
        (void)mRegistry.Register({"capture.cancel", "Cancel a pending sequence and retire its in-flight readbacks safely.",
            "control:framebuffer-capture", Json::object()}, [this](const Json& params) {
                RequireNoParams(params, "capture.cancel");
                if (mCaptureController.IsSequencePending())
                {
                    Application::GetInstance().CancelFrameCaptureSequence();
                    mPendingSimulationSteps = 0;
                    mRuntimeControlMode = RuntimeControlMode::Paused;
                    if (mLevel) mLevel->SetRuntimePaused(true);
                }
                return mCaptureController.BuildStatus();
            });
		if (!mRegistry.Register(
			AgentCommandDescription{
				"capture.frame",
                "Capture the next rendered frame: PNG display or PFM/raw scene-linear data in the session directory.",
				"control:framebuffer-capture",
                Json{ { "filename", "safe ASCII .png/.pfm/.raw filename required, max 64 bytes" },
                    { "delayFrames", "optional 0..600 rendered frames" },
                    { "source", "optional automatic/depth/motion/upscale-motion/upscale-color/upscale-opaque/normal/surface-id/reactive/composition; explicit sources require .raw" } }
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
				Json{ { "held", "canonical key-name string[] required, max 32" }, {"onStep", "bool optional; publish at next simulation frame"} }
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
                if (mCaptureController.IsSequencePending())
                    throw std::logic_error("Cannot resume simulation during capture.sequence");
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
                if (mCaptureController.IsSequencePending())
                    throw std::logic_error("Cannot queue simulation steps during capture.sequence");
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
			AddLogSink(coreLogger, CreateRef<AgentSpdlogSink>(mEventLog, "engine"));
		if (clientLogger && clientLogger != coreLogger)
			AddLogSink(clientLogger, CreateRef<AgentSpdlogSink>(mEventLog, "client"));
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
                    { "featureType", Utf8Prefix(pass.FeatureTypeId, MaxRenderGraphTextBytes) },
                    { "featureInstance", Utf8Prefix(pass.FeatureInstanceId, MaxRenderGraphTextBytes) },
                    { "recordedQueue", pass.bHasRecordedCommands ? Json(RenderGraphQueueId(pass.RecordedQueue)) : Json(nullptr) },
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
					{ "physicalTextureGroup", resource.PhysicalTextureGroup },
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
		const auto* renderSystem = mLevel ? mLevel->TryGetSystem<RenderSystem3D>() : nullptr;
		return Json{
			{ "protocolVersion", AgentProtocol::Version },
			{ "sessionId", mSessionId },
			{ "state", std::string(state) },
			{ "scene", mConfig.SceneName },
			{ "sceneGeneration", renderSystem ? renderSystem->GetSceneGeneration() : 0u },
			{ "renderWidth", application.GetRenderWidth() },
			{ "renderHeight", application.GetRenderHeight() },
			{ "cursorCaptured", Application::GetWindow().IsMouseCursorCaptured() },
			{ "runtimeSceneRequestPending", application.HasRuntimeSceneRequest() },
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

		const auto* renderSystem = mLevel->TryGetSystem<RenderSystem3D>();
		return Json{
			{ "available", true },
			{ "scene", mConfig.SceneName },
			{ "sceneGeneration", renderSystem ? renderSystem->GetSceneGeneration() : 0u },
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
		const auto worldMatrix = mLevel->GetWorldTransform(entity);
		result["worldMatrix"] = Json::array();
		for (int column = 0; column < 4; ++column)
			for (int row = 0; row < 4; ++row)
				result["worldMatrix"].push_back(worldMatrix[column][row]);
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
