#include "Hepch.h"

#include "Runtime/Resource/RuntimeProjectResourceWarmup.h"

#include "Runtime/EcsFramework/Serializer/SceneDocument.h"
#include "Runtime/Graphics/Mesh/MeshRuntimeResource.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimePrefabResource.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeUiResource.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <stdexcept>
#include <utility>

namespace HEngine
{
	namespace
	{
		inline constexpr Utils::JsonLoadLimits ProjectGameConfigLoadLimits{
			1024u * 1024u,
			16,
			4'096
		};

		[[nodiscard]] std::filesystem::path ResolveContentRoot(
			const std::filesystem::path& contentRoot,
			std::string_view description)
		{
			if (contentRoot.empty())
				throw std::invalid_argument(
					"Runtime " + std::string(description) + " warm-up root cannot be empty.");

			std::error_code error;
			const std::filesystem::path absoluteRoot = std::filesystem::absolute(contentRoot, error);
			if (error)
				throw std::invalid_argument(
					"Cannot make the runtime " + std::string(description) +
					" warm-up root absolute: " + error.message());
			const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
			if (error || !std::filesystem::is_directory(canonicalRoot, error) || error)
				throw std::invalid_argument(
					"Cannot resolve the runtime " + std::string(description) + " warm-up root.");
			return canonicalRoot;
		}

		[[nodiscard]] std::filesystem::path SelectContentRoot(
			const std::filesystem::path& projectRoot,
			const std::filesystem::path& engineContentRoot,
			std::string_view logicalPath)
		{
			std::filesystem::path projectPath;
			std::string normalizedPath;
			const Utils::PathValidationResult projectResult =
				Utils::TryResolveProjectRelativePath(
					projectRoot, logicalPath, projectPath, normalizedPath);
			if (!projectResult)
				throw std::invalid_argument(projectResult.Message);

			std::error_code error;
			if (std::filesystem::is_regular_file(projectPath, error) && !error)
				return projectRoot;

			std::filesystem::path enginePath;
			std::string engineNormalizedPath;
			const Utils::PathValidationResult engineResult =
				Utils::TryResolveProjectRelativePath(
					engineContentRoot, normalizedPath, enginePath, engineNormalizedPath);
			if (!engineResult)
				throw std::invalid_argument(engineResult.Message);
			error.clear();
			return std::filesystem::is_regular_file(enginePath, error) && !error
				? engineContentRoot
				: projectRoot;
		}

		[[nodiscard]] RuntimeResourceWarmupDispatch CreateProjectWarmupDispatch(
			RuntimeResourceRegistry& registry,
			std::filesystem::path projectRoot,
			std::filesystem::path engineContentRoot,
			std::shared_ptr<const RuntimeResourceCatalogSnapshot> catalog)
		{
			if (!catalog)
				throw std::invalid_argument("Runtime project warm-up dispatch requires a catalog snapshot.");

			return [
				&registry,
				projectRoot = std::move(projectRoot),
				engineContentRoot = std::move(engineContentRoot),
				catalog = std::move(catalog)](
					const RuntimeResourceWarmupTarget& target,
					RuntimeResourceGroup& group) -> RuntimeResourceKey
			{
				const std::filesystem::path contentRoot = SelectContentRoot(
					projectRoot, engineContentRoot, target.Path);
				switch (target.Type)
				{
				case RuntimeCatalogResourceType::Scene:
				{
					const SceneDocumentPath path = ResolveSceneDocumentPath(contentRoot, target.Path);
					(void)QueueSceneDocument(registry, path, group);
					return path.Key;
				}
				case RuntimeCatalogResourceType::Mesh:
				{
					const RuntimeResourceKey key = MeshRuntimeResource::MakeKey(contentRoot, target.Path);
					if (!key.CachePath.ends_with(".hmesh"))
					{
						throw std::invalid_argument(
							"Mesh warm-up target must use the .hmesh extension: " + target.Path);
					}
					(void)MeshRuntimeResource::QueueAsync(
						registry,
						contentRoot,
						catalog,
						target.Path,
						group);
					return key;
				}
				case RuntimeCatalogResourceType::Prefab:
				{
					const RuntimePrefabPath path = ResolveRuntimePrefabPath(contentRoot, target.Path);
					(void)QueueRuntimePrefabJson(registry, path, group);
					return path.Key;
				}
				case RuntimeCatalogResourceType::UiDocument:
				{
					const RuntimeUiPath path = ResolveRuntimeUiPath(contentRoot, target.Path);
					(void)QueueRuntimeUiDefinition(registry, path, group);
					return path.Key;
				}
				default:
					throw std::logic_error(
						"Unsupported runtime project warm-up target was dispatched.");
				}
			};
		}
	}

	RuntimeProjectResourceWarmup::~RuntimeProjectResourceWarmup()
	{
		Shutdown();
	}

	void RuntimeProjectResourceWarmup::Initialize(
		RuntimeResourceRegistry& registry,
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& engineContentRoot,
		const std::filesystem::path& gameConfigPath,
		std::shared_ptr<const RuntimeResourceCatalogSnapshot> catalog)
	{
		if (mInitialized)
			throw std::logic_error("Runtime project resource warm-up is already initialized.");
		if (!registry.IsInitialized())
			throw std::logic_error("Runtime project resource warm-up requires an initialized registry.");
		if (!catalog)
			throw std::invalid_argument("Runtime project resource warm-up requires a catalog snapshot.");

		const std::filesystem::path canonicalRoot = ResolveContentRoot(projectRoot, "project");
		const std::filesystem::path canonicalEngineContentRoot =
			ResolveContentRoot(engineContentRoot, "engine content");
		std::filesystem::path resolvedGameConfig;
		std::string normalizedGameConfig;
		const Utils::PathValidationResult gameConfigResult =
			Utils::TryResolveProjectRelativePath(
				canonicalRoot,
				gameConfigPath.generic_string(),
				resolvedGameConfig,
				normalizedGameConfig);
		if (!gameConfigResult)
			throw std::invalid_argument(gameConfigResult.Message);
		const Utils::JsonLoadResult loaded = Utils::LoadJsonFile(
			resolvedGameConfig,
			ProjectGameConfigLoadLimits);
		if (!loaded.Value.is_object())
			throw std::runtime_error(normalizedGameConfig + " root must be an object.");

		const auto game = loaded.Value.find("Game");
		if (game == loaded.Value.end() || !game->is_object())
			throw std::runtime_error(normalizedGameConfig + " is missing the Game object.");
		const auto defaultScene = game->find("DefaultScene");
		if (defaultScene == game->end() || !defaultScene->is_string())
			throw std::runtime_error(normalizedGameConfig + " is missing string Game.DefaultScene.");

		std::string scenePath = defaultScene->get<std::string>();
		if (scenePath.empty())
			throw std::runtime_error(normalizedGameConfig + " Game.DefaultScene cannot be empty.");

		RuntimeResourceWarmupPlan plan = BuildRuntimeWarmupPlan(
			*game,
			std::move(catalog),
			canonicalRoot);
		RuntimeResourceWarmupDispatch dispatch = CreateProjectWarmupDispatch(
			registry,
			plan.ProjectRoot,
			canonicalEngineContentRoot,
			plan.Catalog);

		RuntimeResourceWarmupManager& manager = RuntimeResourceWarmupManager::GetInstance();
		manager.Initialize(registry, std::move(plan), std::move(dispatch));
		mManager = &manager;
		mDefaultScene = std::move(scenePath);
		mInitialized = true;
	}

	bool RuntimeProjectResourceWarmup::IsInitialized() const noexcept
	{
		return mInitialized;
	}

	void RuntimeProjectResourceWarmup::Pump()
	{
		if (!mInitialized || !mManager)
			throw std::logic_error("Runtime project resource warm-up is not initialized.");
		mManager->Pump();
	}

	bool RuntimeProjectResourceWarmup::QueueGroup(std::string_view groupName)
	{
		if (!mInitialized || !mManager)
			throw std::logic_error("Runtime project resource warm-up is not initialized.");
		return mManager->QueueGroup(groupName);
	}

	std::optional<RuntimeResourceWarmupGroupProgress>
	RuntimeProjectResourceWarmup::GetGroupProgress(std::string_view groupName) const
	{
		if (!mInitialized || !mManager)
			return std::nullopt;
		return mManager->GetGroupProgress(groupName);
	}

	void RuntimeProjectResourceWarmup::RethrowFailure(std::string_view groupName) const
	{
		if (!mInitialized || !mManager)
			throw std::logic_error("Runtime project resource warm-up is not initialized.");
		mManager->RethrowFailure(groupName);
	}

	std::size_t RuntimeProjectResourceWarmup::ReleaseResidency(
		RuntimeResourceWarmupResidency residency) noexcept
	{
		return mInitialized && mManager
			? mManager->ReleaseResidency(residency)
			: 0;
	}

	void RuntimeProjectResourceWarmup::Shutdown() noexcept
	{
		if (mManager)
			mManager->Shutdown();
		mManager = nullptr;
		mDefaultScene.clear();
		mInitialized = false;
	}

	const std::string& RuntimeProjectResourceWarmup::GetDefaultScene() const
	{
		if (!mInitialized)
			throw std::logic_error("Runtime project resource warm-up is not initialized.");
		return mDefaultScene;
	}
}
