#include "Hepch.h"

#include "Runtime/UnitTest/RuntimeUiResourceTests.h"

#include "_generated/serializer/UIRes.gen.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeUiResource.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HEngine::Tests
{
	namespace
	{
		void Require(bool condition, std::string_view message)
		{
			if (!condition)
				throw std::runtime_error(std::string(message));
		}

		class ScopedTempDirectory final
		{
		public:
			ScopedTempDirectory()
			{
				const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
				mPath = std::filesystem::temp_directory_path() /
					("HEngine_RuntimeUiResource_" + std::to_string(suffix));
				std::filesystem::create_directories(mPath);
			}

			~ScopedTempDirectory()
			{
				std::error_code error;
				std::filesystem::remove_all(mPath, error);
			}

			[[nodiscard]] const std::filesystem::path& Get() const noexcept
			{
				return mPath;
			}

		private:
			std::filesystem::path mPath;
		};

		template<typename Callback>
		void RequireInvalidArgument(Callback&& callback, std::string_view message)
		{
			bool bThrew = false;
			try
			{
				callback();
			}
			catch (const std::invalid_argument&)
			{
				bThrew = true;
			}
			Require(bThrew, message);
		}

		template<typename Callback>
		void RequireLoadFailure(Callback&& callback, std::string_view message)
		{
			bool bThrew = false;
			try
			{
				callback();
			}
			catch (const std::exception&)
			{
				bThrew = true;
			}
			Require(bThrew, message);
		}

		[[nodiscard]] UIRes MakeUiDefinition(std::string name, std::string text)
		{
			UIRes definition;
			definition.Name = std::move(name);

			UINodeRes canvas;
			canvas.Id = 1;
			canvas.Type = UIWidgetType::Canvas;
			canvas.Name = "Canvas";
			canvas.ElementId = "canvas_root";
			canvas.Text = std::move(text);
			canvas.Layout.AnchorMax = { 1.0f, 1.0f };
			canvas.Layout.OffsetMax = { 1280.0f, 720.0f };
			definition.Nodes.push_back(std::move(canvas));
			return definition;
		}

		[[nodiscard]] std::vector<char> ReadFileBytes(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if (!stream)
				throw std::runtime_error("Cannot read UI test artifact: " + path.string());
			const std::streamoff end = stream.tellg();
			if (end < 0)
				throw std::runtime_error("Cannot measure UI test artifact: " + path.string());
			std::vector<char> bytes(static_cast<size_t>(end));
			stream.seekg(0, std::ios::beg);
			if (!bytes.empty())
				stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			if (!stream)
				throw std::runtime_error("Cannot read complete UI test artifact: " + path.string());
			return bytes;
		}

		class ScopedReadOnlyFile final
		{
		public:
			explicit ScopedReadOnlyFile(const std::filesystem::path& path)
				: mPath(path)
			{
				std::error_code error;
				mOriginalPermissions = std::filesystem::status(mPath, error).permissions();
				if (error)
					throw std::runtime_error("Cannot inspect UI test artifact permissions: " + error.message());

				constexpr std::filesystem::perms writePermissions =
					std::filesystem::perms::owner_write |
					std::filesystem::perms::group_write |
					std::filesystem::perms::others_write;
				std::filesystem::permissions(
					mPath,
					writePermissions,
					std::filesystem::perm_options::remove,
					error);
				if (error)
					throw std::runtime_error("Cannot make UI test artifact read-only: " + error.message());
			}

			~ScopedReadOnlyFile()
			{
				if (bRestored)
					return;

				std::error_code error;
				std::filesystem::permissions(
					mPath,
					mOriginalPermissions,
					std::filesystem::perm_options::replace,
					error);
			}

			void Restore()
			{
				std::error_code error;
				std::filesystem::permissions(
					mPath,
					mOriginalPermissions,
					std::filesystem::perm_options::replace,
					error);
				if (error)
					throw std::runtime_error("Cannot restore UI test artifact permissions: " + error.message());
				bRestored = true;
			}

		private:
			std::filesystem::path mPath;
			std::filesystem::perms mOriginalPermissions = std::filesystem::perms::unknown;
			bool bRestored = false;
		};

		[[nodiscard]] bool HasUiTransactionTemporaryFiles(
			const std::filesystem::path& directory,
			std::string_view prefix)
		{
			for (const std::filesystem::directory_entry& entry :
				std::filesystem::directory_iterator(directory))
			{
				const std::string filename = entry.path().filename().string();
				if (filename.starts_with(prefix) && filename.find(".tmp.") != std::string::npos)
					return true;
			}
			return false;
		}
	}

	void RuntimeUiResourceContract()
	{
		ScopedTempDirectory tempDirectory;
		const std::filesystem::path projectRoot = tempDirectory.Get() / "Project";
		const std::filesystem::path uiDirectory = projectRoot / "Assets" / "UI";
		std::filesystem::create_directories(uiDirectory);

		const std::filesystem::path huiPath = uiDirectory / "Hud.hui";
		UIRes firstDefinition = MakeUiDefinition("First", "First generation");
		firstDefinition.GeneratedRmlPath = "Assets/UI/Hud.rml";
		firstDefinition.GeneratedRcssPath = "Assets/UI/Hud.rcss";
		std::string error;
		Require(SaveUIRes(firstDefinition, huiPath, &error),
			"Failed to create the initial UI resource fixture.");

		const RuntimeUiPath firstPath = ResolveRuntimeUiPath(
			projectRoot,
			"Assets\\UI//./Hud.hui");
		Require(firstPath.Key.Path == "Assets/UI/Hud.hui",
			"UI path was not normalized to the canonical project form.");
		Require(firstPath.Key.CachePath == "assets/ui/hud.hui",
			"UI cache key was not case folded.");
		Require(firstPath.RmlProjectPath == "Assets/UI/Hud.rml" &&
			firstPath.RcssProjectPath == "Assets/UI/Hud.rcss",
			"UI sibling project paths were not derived from the .hui path.");

		const RuntimeUiPath aliasPath = ResolveRuntimeUiPath(
			projectRoot,
			"assets/ui/hud.hui");
		Require(aliasPath.Key == firstPath.Key,
			"Equivalent UI path aliases did not produce one registry key.");

		const RuntimeUiPath writtenPath = ResolveRuntimeUiWrittenPath(projectRoot, huiPath);
		Require(writtenPath.Key == firstPath.Key,
			"Absolute project-local UI save path did not resolve to the load key.");
		const RuntimeUiPath newWrittenPath = ResolveRuntimeUiWrittenPath(
			projectRoot,
			uiDirectory / "NewHud.hui");
		Require(newWrittenPath.Key.Path == "Assets/UI/NewHud.hui",
			"A project-local non-existing UI save target was rejected.");

		RequireInvalidArgument([&]
		{
			(void)ResolveRuntimeUiPath(projectRoot, "Assets/UI/../Hud.hui");
		}, "UI parent traversal was accepted.");
		RequireInvalidArgument([&]
		{
			(void)ResolveRuntimeUiPath(projectRoot, huiPath.generic_string());
		}, "Absolute UI load path was accepted.");
		RequireInvalidArgument([&]
		{
			(void)ResolveRuntimeUiPath(projectRoot, "Assets/UI/Hud.rml");
		}, "A non-.hui resource was accepted as a UI definition.");
		RequireInvalidArgument([&]
		{
			(void)ResolveRuntimeUiWrittenPath(
				projectRoot,
				tempDirectory.Get() / "Outside.hui");
		}, "A UI save target outside the project root was accepted.");

		const std::filesystem::path mismatchHuiPath = uiDirectory / "Mismatch.hui";
		UIRes mismatchDefinition = MakeUiDefinition("Mismatch", "Mismatch");
		mismatchDefinition.GeneratedRmlPath = "Assets/UI/Mismatch.rml";
		mismatchDefinition.GeneratedRcssPath = "Assets/UI/Mismatch.rcss";
		Require(SaveUIRes(mismatchDefinition, mismatchHuiPath, &error),
			"Failed to create the generated-path mismatch fixture.");
		mismatchDefinition.GeneratedRmlPath = "Assets/UI/Other.rml";
		Json mismatchJson;
		Serializer::Serialize(mismatchJson, mismatchDefinition);
		Utils::SaveJsonToFile(mismatchJson, mismatchHuiPath);
		const RuntimeUiPath mismatchPath = ResolveRuntimeUiPath(
			projectRoot,
			"Assets/UI/Mismatch.hui");
		RuntimeResourceRegistry mismatchRegistry;
		mismatchRegistry.Init(1);
		RequireLoadFailure([&]
		{
			(void)AcquireRuntimeUiDefinition(mismatchRegistry, mismatchPath);
		}, "A UI definition with a mismatched generated RML path was accepted.");
		mismatchRegistry.Shutdown();

		RuntimeResourceRegistry registry;
		registry.Init(2);
		{
			RuntimeResourceGroup group = registry.CreateGroup();
			RuntimeResourceHandle<RuntimeUiDefinition> first =
				AcquireRuntimeUiDefinition(registry, firstPath);
			const uint64_t firstGeneration = first.GetGeneration();
			Require(first->GetDefinition().Name == "First",
				"Initial UI definition was not loaded.");

			RuntimeResourceHandle<RuntimeUiDefinition> alias =
				AcquireRuntimeUiDefinition(registry, aliasPath);
			Require(alias.GetGeneration() == firstGeneration,
				"UI alias created a second registry generation.");
			Require(registry.GetStats().PhysicalLoads == 1,
				"UI aliases performed more than one physical load.");
			Require(group.Pin(first), "Failed to pin the initial UI generation.");

			first.Reset();
			alias.Reset();
			Require(registry.CollectUnused() == 0,
				"A group-pinned UI generation was evicted.");

			UIRes updatedDefinition = MakeUiDefinition("Second", "Second generation");
			Require(SaveRuntimeUiDefinition(
				registry,
				writtenPath,
				updatedDefinition,
				&error),
				"Failed to save the replacement UI generation.");
			const RuntimeUiPath updatedPath = ResolveRuntimeUiPath(
				projectRoot,
				"Assets/UI/Hud.hui");
			RuntimeResourceHandle<RuntimeUiDefinition> updated =
				AcquireRuntimeUiDefinition(registry, updatedPath);
			Require(updated.GetGeneration() != firstGeneration,
				"UI save invalidation did not create a replacement generation.");
			Require(updated->GetDefinition().Name == "Second",
				"Replacement UI generation did not read the updated definition.");
			Require(updated->GetDefinition().GeneratedRmlPath == updatedPath.RmlProjectPath &&
				updated->GetDefinition().GeneratedRcssPath == updatedPath.RcssProjectPath,
				"UI save did not persist the generated sibling contract.");
			Require(group.Pin(updated), "Failed to pin the replacement UI generation.");

			const std::vector<char> huiBytes = ReadFileBytes(updatedPath.HuiFullPath);
			const std::vector<char> rmlBytes = ReadFileBytes(updatedPath.RmlFullPath);
			const std::vector<char> rcssBytes = ReadFileBytes(updatedPath.RcssFullPath);
			const uint64_t updatedGeneration = updated.GetGeneration();

			UIRes invalidDefinition;
			invalidDefinition.Name = "Invalid";
			Require(!SaveRuntimeUiDefinition(
				registry,
				updatedPath,
				invalidDefinition,
				&error),
				"A UI definition without nodes unexpectedly saved.");
			Require(ReadFileBytes(updatedPath.HuiFullPath) == huiBytes &&
				ReadFileBytes(updatedPath.RmlFullPath) == rmlBytes &&
				ReadFileBytes(updatedPath.RcssFullPath) == rcssBytes,
				"A failed UI save changed one or more published artifacts.");

			RuntimeResourceHandle<RuntimeUiDefinition> cached =
				AcquireRuntimeUiDefinition(registry, updatedPath);
			Require(cached.GetGeneration() == updatedGeneration,
				"A failed UI save invalidated the cached generation.");
			Require(registry.GetStats().PhysicalLoads == 2,
				"A failed UI save caused an unexpected physical reload.");

			UIRes publishFailureDefinition = MakeUiDefinition(
				"PublishFailure",
				"This generation must not become visible");
			bool bPublishSucceeded = false;
			{
				ScopedReadOnlyFile readOnlyHui(updatedPath.HuiFullPath);
				bPublishSucceeded = SaveRuntimeUiDefinition(
					registry,
					updatedPath,
					publishFailureDefinition,
					&error);
				readOnlyHui.Restore();
			}
			Require(!bPublishSucceeded,
				"Replacing a read-only final .hui artifact unexpectedly succeeded.");
			Require(error.find(updatedPath.HuiFullPath.string()) != std::string::npos,
				"A transactional UI publish failure did not report the original failing artifact.");
			Require(ReadFileBytes(updatedPath.HuiFullPath) == huiBytes &&
				ReadFileBytes(updatedPath.RmlFullPath) == rmlBytes &&
				ReadFileBytes(updatedPath.RcssFullPath) == rcssBytes,
				"A later UI publish failure did not restore every previous artifact.");

			RuntimeResourceHandle<RuntimeUiDefinition> rollbackCached =
				AcquireRuntimeUiDefinition(registry, updatedPath);
			Require(rollbackCached.GetGeneration() == updatedGeneration &&
				rollbackCached->GetDefinition().Name == "Second",
				"A failed transactional UI save exposed a mixed cache generation.");
			Require(registry.GetStats().PhysicalLoads == 2,
				"A failed transactional UI save invalidated or reloaded the cached generation.");
			Require(!HasUiTransactionTemporaryFiles(uiDirectory, "Hud."),
				"A failed transactional UI save left staging or backup files behind.");

			const std::filesystem::path asyncHuiPath = uiDirectory / "Async.hui";
			UIRes asyncDefinition = MakeUiDefinition("Async", "Async generation");
			asyncDefinition.GeneratedRmlPath = "Assets/UI/Async.rml";
			asyncDefinition.GeneratedRcssPath = "Assets/UI/Async.rcss";
			Require(SaveUIRes(asyncDefinition, asyncHuiPath, &error),
				"Failed to create the async UI resource fixture.");
			const RuntimeUiPath asyncPath = ResolveRuntimeUiPath(
				projectRoot,
				"Assets/UI/Async.hui");
			Require(QueueRuntimeUiDefinition(registry, asyncPath, group),
				"UI async request did not queue its physical load.");
			RuntimeResourceHandle<RuntimeUiDefinition> async =
				AcquireRuntimeUiDefinition(registry, asyncPath);
			Require(async->GetDefinition().Name == "Async",
				"UI async load did not finalize its immutable definition.");
			Require(registry.GetStats().PhysicalLoads == 3,
				"UI sync/invalidate/async physical load count was not deterministic.");

			updated.Reset();
			cached.Reset();
			rollbackCached.Reset();
			async.Reset();
			group.Release();
			Require(registry.CollectUnused() == 3,
				"Released UI generations were not reclaimed on the init thread.");
			Require(registry.GetStats().Resident == 0,
				"UI resources remained resident after all leases and pins were released.");
		}
		registry.Shutdown();
	}
}
