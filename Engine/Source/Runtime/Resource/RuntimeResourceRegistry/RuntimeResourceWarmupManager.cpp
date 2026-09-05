#include "Hepch.h"

#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeResourceWarmupManager.h"

#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace HEngine
{
	namespace
	{
		[[nodiscard]] std::string FoldAscii(std::string_view value)
		{
			std::string folded(value);
			for (char& character : folded)
			{
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character - 'A' + 'a');
			}
			return folded;
		}

		[[nodiscard]] std::filesystem::path ResolveProjectRoot(
			const std::filesystem::path& projectRoot)
		{
			if (projectRoot.empty())
				throw std::invalid_argument("Runtime resource warm-up project root cannot be empty.");

			std::error_code error;
			const std::filesystem::path absoluteRoot = std::filesystem::absolute(projectRoot, error);
			if (error)
				throw std::invalid_argument("Cannot make the runtime warm-up project root absolute: " + error.message());
			const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
			if (error || !std::filesystem::is_directory(canonicalRoot, error) || error)
				throw std::invalid_argument("Cannot resolve the runtime warm-up project root.");
			return canonicalRoot;
		}

		[[nodiscard]] bool IsSupportedType(RuntimeCatalogResourceType type) noexcept
		{
			switch (type)
			{
			case RuntimeCatalogResourceType::Scene:
			case RuntimeCatalogResourceType::Mesh:
			case RuntimeCatalogResourceType::Prefab:
			case RuntimeCatalogResourceType::UiDocument:
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] std::optional<RuntimeCatalogResourceType> ClassifyLooseType(
			std::string_view normalizedPath)
		{
			const std::string cacheKey = Utils::MakeProjectPathCacheKey(normalizedPath);
			if (cacheKey.ends_with(".he"))
				return RuntimeCatalogResourceType::Scene;
			if (cacheKey.ends_with(".hmesh"))
				return RuntimeCatalogResourceType::Mesh;
			if (cacheKey.ends_with(".hprefab"))
				return RuntimeCatalogResourceType::Prefab;
			if (cacheKey.ends_with(".hui"))
				return RuntimeCatalogResourceType::UiDocument;
			return std::nullopt;
		}

		[[nodiscard]] bool PathMatchesType(
			std::string_view path,
			RuntimeCatalogResourceType type)
		{
			const std::optional<RuntimeCatalogResourceType> classified = ClassifyLooseType(path);
			return classified && *classified == type;
		}

		[[nodiscard]] std::string NormalizeWarmupRootPath(
			std::string_view path,
			const std::filesystem::path& projectRoot,
			bool bRequireRegularFile,
			std::string_view context)
		{
			std::filesystem::path fullPath;
			std::string normalizedPath;
			const Utils::PathValidationResult pathResult = Utils::TryResolveProjectRelativePath(
				projectRoot,
				path,
				fullPath,
				normalizedPath);
			if (!pathResult)
				throw std::invalid_argument(std::string(context) + ": " + pathResult.Message);
			if (bRequireRegularFile)
			{
				std::error_code error;
				if (!std::filesystem::is_regular_file(fullPath, error) || error)
				{
					throw std::invalid_argument(
						std::string(context) + " must resolve to an existing regular file.");
				}
			}
			return normalizedPath;
		}

		void BuildLooseTargets(RuntimeResourceWarmupGroupPlan& group, const std::string& context)
		{
			group.Targets.reserve(group.Roots.size());
			for (const RuntimeResourceWarmupRoot& root : group.Roots)
			{
				const std::optional<RuntimeCatalogResourceType> type = ClassifyLooseType(root.Path);
				if (!type)
				{
					throw std::invalid_argument(
						context + " has unsupported explicit warm-up root: " + root.Path);
				}
				group.Targets.push_back({ root.Path, *type, true });
			}
		}

		void BuildPackagedTargets(
			RuntimeResourceWarmupGroupPlan& group,
			const std::shared_ptr<const RuntimeResourceCatalogSnapshot>& snapshot,
			const std::string& context)
		{
			const std::string tag = "Warmup:" + group.Name;
			std::unordered_set<std::string> explicitRootKeys;
			std::unordered_set<std::string> expectedTagKeys;
			explicitRootKeys.reserve(group.Roots.size());
			for (RuntimeResourceWarmupRoot& root : group.Roots)
			{
				const RuntimeResourceCatalogEntry* entry = snapshot->Find(root.Path);
				if (!entry)
					throw std::invalid_argument(context + " explicit root is absent from the packaged catalog: " + root.Path);
				if (std::find(entry->Tags.begin(), entry->Tags.end(), tag) == entry->Tags.end())
				{
					throw std::invalid_argument(
						context + " explicit root is missing packaged tag '" + tag + "': " + entry->Path);
				}
				if (!IsSupportedType(entry->Type) || !PathMatchesType(entry->Path, entry->Type))
				{
					throw std::invalid_argument(
						context + " has unsupported explicit warm-up root: " + entry->Path);
				}
				root.Path = entry->Path;
				explicitRootKeys.emplace(entry->CacheKey);
				if (root.Expand == RuntimeResourceWarmupExpand::None)
				{
					expectedTagKeys.emplace(entry->CacheKey);
				}
				else
				{
					const RuntimeCatalogExpand expand =
						root.Expand == RuntimeResourceWarmupExpand::Runtime
						? RuntimeCatalogExpand::Runtime
						: RuntimeCatalogExpand::HardRuntime;
					for (const RuntimeResourceCatalogEntry* expanded : snapshot->Expand(entry->Path, expand))
						expectedTagKeys.emplace(expanded->CacheKey);
				}
			}

			const std::vector<const RuntimeResourceCatalogEntry*> tagged = snapshot->FindByTag(tag);
			if (tagged.size() != expectedTagKeys.size() ||
				std::any_of(tagged.begin(), tagged.end(), [&expectedTagKeys](const auto* entry)
				{
					return !expectedTagKeys.contains(entry->CacheKey);
				}))
			{
				throw std::invalid_argument(
					context + " packaged tag '" + tag +
					"' does not match the configured Roots/Expand closure.");
			}
			group.Targets.reserve(tagged.size());
			for (const RuntimeResourceCatalogEntry* entry : tagged)
			{
				group.Targets.push_back({
					entry->Path,
					entry->Type,
					explicitRootKeys.contains(entry->CacheKey)
				});
			}
		}

		[[nodiscard]] float CalculateRatio(
			std::size_t total,
			std::size_t ready,
			std::size_t failed,
			std::size_t skipped,
			bool bEmptyComplete) noexcept
		{
			if (total == 0)
				return bEmptyComplete ? 1.0f : 0.0f;
			const std::size_t terminal = std::min(total, ready + failed + skipped);
			return static_cast<float>(terminal) / static_cast<float>(total);
		}
	}

	RuntimeResourceWarmupPlan BuildRuntimeWarmupPlan(
		const Json& gameObject,
		std::shared_ptr<const RuntimeResourceCatalogSnapshot> snapshot,
		const std::filesystem::path& projectRoot)
	{
		if (!gameObject.is_object())
			throw std::invalid_argument("Game must be an object when building a runtime warm-up plan.");
		if (!snapshot)
			throw std::invalid_argument("Runtime warm-up planning requires a catalog snapshot.");

		RuntimeResourceWarmupPlan plan;
		plan.ProjectRoot = ResolveProjectRoot(projectRoot);
		plan.Catalog = std::move(snapshot);
		plan.bPackaged = !plan.Catalog->IsEmpty();

		RuntimeResourceWarmupSchema schema = ParseRuntimeResourceWarmupSchema(
			gameObject,
			[&plan](std::string_view path, std::string_view context)
			{
				return NormalizeWarmupRootPath(
					path,
					plan.ProjectRoot,
					!plan.bPackaged,
					context);
			});

		plan.Groups.reserve(schema.Groups.size());
		for (std::size_t groupIndex = 0; groupIndex < schema.Groups.size(); ++groupIndex)
		{
			RuntimeResourceWarmupSchemaGroup& schemaGroup = schema.Groups[groupIndex];
			const std::string context =
				"Game.ResourceWarmup.Groups[" + std::to_string(groupIndex) + "]";

			RuntimeResourceWarmupGroupPlan group;
			group.Name = std::move(schemaGroup.Name);
			group.Trigger = std::move(schemaGroup.Trigger);
			group.Priority = schemaGroup.Priority;
			group.Expand = schemaGroup.Expand;
			group.Residency = schemaGroup.Residency;
			group.Roots = std::move(schemaGroup.Roots);

			if (plan.bPackaged)
				BuildPackagedTargets(group, plan.Catalog, context);
			else
				BuildLooseTargets(group, context);
			plan.Groups.emplace_back(std::move(group));
		}
		return plan;
	}

	struct RuntimeResourceWarmupManager::Impl
	{
		struct GroupRuntime
		{
			RuntimeResourceWarmupGroupState State = RuntimeResourceWarmupGroupState::Idle;
			RuntimeResourceGroup Resources;
			RuntimeResourceGroupProgress LastResources;
			std::vector<bool> Attempted;
			std::vector<std::optional<RuntimeResourceKey>> Keys;
			std::size_t Supported = 0;
			std::size_t AttemptedCount = 0;
			std::size_t DispatchFailures = 0;
			std::size_t UnavailableFailures = 0;
			std::exception_ptr Failure;
		};

		RuntimeResourceRegistry* Registry = nullptr;
		RuntimeResourceWarmupPlan Plan;
		RuntimeResourceWarmupDispatch Dispatch;
		std::vector<GroupRuntime> Groups;
		bool bInitialized = false;

		[[nodiscard]] std::optional<std::size_t> FindGroup(std::string_view name) const noexcept
		{
			for (std::size_t index = 0; index < Plan.Groups.size(); ++index)
			{
				if (Plan.Groups[index].Name == name)
					return index;
			}
			return std::nullopt;
		}

		void ResetGroupRuntime(std::size_t groupIndex)
		{
			GroupRuntime& runtime = Groups[groupIndex];
			runtime.Resources.Release();
			runtime.LastResources = {};
			runtime.Attempted.assign(Plan.Groups[groupIndex].Targets.size(), false);
			runtime.Keys.assign(Plan.Groups[groupIndex].Targets.size(), std::nullopt);
			runtime.AttemptedCount = 0;
			runtime.DispatchFailures = 0;
			runtime.UnavailableFailures = 0;
			runtime.Failure = nullptr;
			runtime.State = RuntimeResourceWarmupGroupState::Idle;
		}

		void StoreCurrentFailure(
			std::size_t groupIndex,
			const std::string& context) noexcept
		{
			GroupRuntime& runtime = Groups[groupIndex];
			const std::exception_ptr cause = std::current_exception();
			try
			{
				if (cause)
				{
					try
					{
						std::rethrow_exception(cause);
					}
					catch (...)
					{
						std::throw_with_nested(std::runtime_error(context));
					}
				}
				throw std::runtime_error(context);
			}
			catch (...)
			{
				runtime.Failure = std::current_exception();
			}

			runtime.State = RuntimeResourceWarmupGroupState::Failed;
			runtime.Resources.Release();
		}

		void StoreFailure(std::size_t groupIndex, const std::string& context) noexcept
		{
			GroupRuntime& runtime = Groups[groupIndex];
			try
			{
				throw std::runtime_error(context);
			}
			catch (...)
			{
				runtime.Failure = std::current_exception();
			}

			runtime.State = RuntimeResourceWarmupGroupState::Failed;
			runtime.Resources.Release();
		}

		void RefreshGroup(std::size_t groupIndex)
		{
			GroupRuntime& runtime = Groups[groupIndex];
			if (runtime.State != RuntimeResourceWarmupGroupState::Queued &&
				runtime.State != RuntimeResourceWarmupGroupState::Loading)
			{
				return;
			}

			runtime.LastResources = runtime.Resources.Progress();
			if (runtime.LastResources.Failed != 0)
			{
				std::string failedPath = "<unknown>";
				for (std::size_t index = 0; index < runtime.Keys.size(); ++index)
				{
					if (runtime.Keys[index] &&
						Registry->GetState(*runtime.Keys[index]) == RuntimeResourceState::Failed)
					{
						failedPath = Plan.Groups[groupIndex].Targets[index].Path;
						break;
					}
				}
				StoreFailure(
					groupIndex,
					"Runtime resource warm-up group '" + Plan.Groups[groupIndex].Name +
					"' failed while loading '" + failedPath + "'.");
				return;
			}
			if (runtime.LastResources.Unloaded != 0 && runtime.LastResources.Loading == 0)
			{
				runtime.UnavailableFailures = runtime.LastResources.Unloaded;
				StoreFailure(
					groupIndex,
					"Runtime resource warm-up group '" + Plan.Groups[groupIndex].Name +
					"' lost a dispatched resource.");
				return;
			}

			if (runtime.AttemptedCount == runtime.Supported)
			{
				if (runtime.LastResources.Ready == runtime.LastResources.Total)
					runtime.State = RuntimeResourceWarmupGroupState::Ready;
				else
					runtime.State = RuntimeResourceWarmupGroupState::Loading;
			}
			else
			{
				runtime.State = RuntimeResourceWarmupGroupState::Queued;
			}
		}

		[[nodiscard]] RuntimeResourceWarmupGroupProgress BuildGroupProgress(
			std::size_t groupIndex) const noexcept
		{
			const RuntimeResourceWarmupGroupPlan& plan = Plan.Groups[groupIndex];
			const GroupRuntime& runtime = Groups[groupIndex];
			RuntimeResourceWarmupGroupProgress progress;
			progress.State = runtime.State;
			progress.Total = plan.Targets.size();
			progress.Undispatched = runtime.Supported - runtime.AttemptedCount;
			progress.Dispatched = runtime.AttemptedCount;
			progress.Ready = runtime.LastResources.Ready;
			progress.Failed = runtime.LastResources.Failed +
				runtime.DispatchFailures + runtime.UnavailableFailures;
			progress.Loading = runtime.State == RuntimeResourceWarmupGroupState::Released
				? 0
				: runtime.LastResources.Loading;
			progress.Skipped = progress.Total - runtime.Supported;
			if (progress.State != RuntimeResourceWarmupGroupState::Idle)
			{
				progress.Ratio = CalculateRatio(
					progress.Total,
					progress.Ready,
					progress.Failed,
					progress.Skipped,
					progress.State == RuntimeResourceWarmupGroupState::Ready);
			}
			return progress;
		}
	};

	RuntimeResourceWarmupManager::RuntimeResourceWarmupManager()
		: mImpl(CreateScope<Impl>())
	{
	}

	RuntimeResourceWarmupManager::~RuntimeResourceWarmupManager()
	{
		Shutdown();
	}

	RuntimeResourceWarmupManager& RuntimeResourceWarmupManager::GetInstance()
	{
		static RuntimeResourceWarmupManager manager;
		return manager;
	}

	void RuntimeResourceWarmupManager::Initialize(
		RuntimeResourceRegistry& registry,
		RuntimeResourceWarmupPlan plan,
		RuntimeResourceWarmupDispatch dispatch)
	{
		if (mImpl->bInitialized)
			throw std::logic_error("Runtime resource warm-up manager is already initialized.");
		if (!registry.IsInitialized())
			throw std::logic_error("Runtime resource warm-up manager requires an initialized registry.");
		if (!plan.Catalog)
			throw std::invalid_argument("Runtime resource warm-up plan has no catalog snapshot.");
		if (!dispatch)
			throw std::invalid_argument("Runtime resource warm-up manager requires a dispatch callback.");

		plan.ProjectRoot = ResolveProjectRoot(plan.ProjectRoot);
		if (plan.bPackaged != !plan.Catalog->IsEmpty())
			throw std::invalid_argument("Runtime resource warm-up plan mode does not match its catalog snapshot.");

		std::unordered_set<std::string> names;
		names.reserve(plan.Groups.size());
		std::vector<Impl::GroupRuntime> groups;
		groups.reserve(plan.Groups.size());
		for (const RuntimeResourceWarmupGroupPlan& group : plan.Groups)
		{
			if (group.Name.empty() || group.Trigger.empty())
				throw std::invalid_argument("Runtime resource warm-up plan contains an incomplete group.");
			if (!names.emplace(FoldAscii(group.Name)).second)
				throw std::invalid_argument("Runtime resource warm-up plan contains a duplicate group: " + group.Name);

			Impl::GroupRuntime runtime;
			runtime.Attempted.assign(group.Targets.size(), false);
			runtime.Keys.assign(group.Targets.size(), std::nullopt);
			for (const RuntimeResourceWarmupTarget& target : group.Targets)
			{
				if (target.Path.empty())
					throw std::invalid_argument("Runtime resource warm-up plan contains an empty target path.");
				if (target.bExplicitRoot && !IsSupportedType(target.Type))
					throw std::invalid_argument("Runtime resource warm-up plan contains an unsupported explicit target.");
				if (IsSupportedType(target.Type))
					++runtime.Supported;
			}
			groups.emplace_back(std::move(runtime));
		}

		mImpl->Registry = &registry;
		mImpl->Plan = std::move(plan);
		mImpl->Dispatch = std::move(dispatch);
		mImpl->Groups = std::move(groups);
		mImpl->bInitialized = true;
	}

	bool RuntimeResourceWarmupManager::IsInitialized() const noexcept
	{
		return mImpl->bInitialized;
	}

	bool RuntimeResourceWarmupManager::QueueGroup(std::string_view groupName)
	{
		if (!mImpl->bInitialized || !mImpl->Registry)
			throw std::logic_error("Runtime resource warm-up manager is not initialized.");
		const std::optional<std::size_t> groupIndex = mImpl->FindGroup(groupName);
		if (!groupIndex)
			return false;

		Impl::GroupRuntime& runtime = mImpl->Groups[*groupIndex];
		if (runtime.State != RuntimeResourceWarmupGroupState::Idle &&
			runtime.State != RuntimeResourceWarmupGroupState::Released)
		{
			return false;
		}
		if (!mImpl->Registry->IsInitialized())
			throw std::logic_error("Runtime resource registry is unavailable for warm-up queueing.");

		mImpl->ResetGroupRuntime(*groupIndex);
		runtime.Resources = mImpl->Registry->CreateGroup();
		runtime.State = runtime.Supported == 0
			? RuntimeResourceWarmupGroupState::Ready
			: RuntimeResourceWarmupGroupState::Queued;
		return true;
	}

	std::size_t RuntimeResourceWarmupManager::QueueTrigger(std::string_view trigger)
	{
		if (!mImpl->bInitialized)
			throw std::logic_error("Runtime resource warm-up manager is not initialized.");

		std::size_t queued = 0;
		for (const RuntimeResourceWarmupGroupPlan& group : mImpl->Plan.Groups)
		{
			if (group.Trigger == trigger && QueueGroup(group.Name))
				++queued;
		}
		return queued;
	}

	void RuntimeResourceWarmupManager::Pump()
	{
		if (!mImpl->bInitialized || !mImpl->Registry)
			throw std::logic_error("Runtime resource warm-up manager is not initialized.");

		if (!mImpl->Registry->IsInitialized())
		{
			for (std::size_t index = 0; index < mImpl->Groups.size(); ++index)
			{
				Impl::GroupRuntime& runtime = mImpl->Groups[index];
				if (runtime.State == RuntimeResourceWarmupGroupState::Queued ||
					runtime.State == RuntimeResourceWarmupGroupState::Loading)
				{
					++runtime.DispatchFailures;
					mImpl->StoreFailure(index, "Runtime resource registry stopped during warm-up.");
				}
			}
			return;
		}

		for (std::size_t index = 0; index < mImpl->Groups.size(); ++index)
			mImpl->RefreshGroup(index);

		std::optional<std::size_t> selectedGroup;
		for (std::size_t index = 0; index < mImpl->Groups.size(); ++index)
		{
			const Impl::GroupRuntime& runtime = mImpl->Groups[index];
			if (runtime.State != RuntimeResourceWarmupGroupState::Queued ||
				runtime.AttemptedCount == runtime.Supported)
			{
				continue;
			}
			if (!selectedGroup ||
				mImpl->Plan.Groups[index].Priority > mImpl->Plan.Groups[*selectedGroup].Priority)
			{
				selectedGroup = index;
			}
		}
		if (!selectedGroup)
			return;

		const std::size_t groupIndex = *selectedGroup;
		Impl::GroupRuntime& runtime = mImpl->Groups[groupIndex];
		const RuntimeResourceWarmupGroupPlan& groupPlan = mImpl->Plan.Groups[groupIndex];
		std::optional<std::size_t> targetIndex;
		for (std::size_t index = 0; index < groupPlan.Targets.size(); ++index)
		{
			if (!runtime.Attempted[index] && IsSupportedType(groupPlan.Targets[index].Type))
			{
				targetIndex = index;
				break;
			}
		}
		if (!targetIndex)
		{
			++runtime.DispatchFailures;
			mImpl->StoreFailure(
				groupIndex,
				"Runtime resource warm-up group '" + groupPlan.Name +
				"' lost its undispatched target state.");
			return;
		}

		const RuntimeResourceWarmupTarget& target = groupPlan.Targets[*targetIndex];
		runtime.Attempted[*targetIndex] = true;
		++runtime.AttemptedCount;
		try
		{
			runtime.Keys[*targetIndex] = mImpl->Dispatch(target, runtime.Resources);
		}
		catch (...)
		{
			++runtime.DispatchFailures;
			mImpl->StoreCurrentFailure(
				groupIndex,
				"Failed to dispatch runtime warm-up target '" + target.Path +
				"' for group '" + groupPlan.Name + "'.");
			return;
		}

		mImpl->RefreshGroup(groupIndex);
	}

	std::optional<RuntimeResourceWarmupGroupProgress>
	RuntimeResourceWarmupManager::GetGroupProgress(std::string_view groupName) const
	{
		if (!mImpl->bInitialized)
			return std::nullopt;
		const std::optional<std::size_t> groupIndex = mImpl->FindGroup(groupName);
		return groupIndex
			? std::optional<RuntimeResourceWarmupGroupProgress>(mImpl->BuildGroupProgress(*groupIndex))
			: std::nullopt;
	}

	std::optional<RuntimeResourceWarmupTriggerProgress>
	RuntimeResourceWarmupManager::GetTriggerProgress(std::string_view trigger) const
	{
		if (!mImpl->bInitialized)
			return std::nullopt;

		RuntimeResourceWarmupTriggerProgress progress;
		for (std::size_t index = 0; index < mImpl->Plan.Groups.size(); ++index)
		{
			if (mImpl->Plan.Groups[index].Trigger != trigger)
				continue;

			const RuntimeResourceWarmupGroupProgress group = mImpl->BuildGroupProgress(index);
			++progress.TotalGroups;
			switch (group.State)
			{
			case RuntimeResourceWarmupGroupState::Idle: ++progress.IdleGroups; break;
			case RuntimeResourceWarmupGroupState::Queued: ++progress.QueuedGroups; break;
			case RuntimeResourceWarmupGroupState::Loading: ++progress.LoadingGroups; break;
			case RuntimeResourceWarmupGroupState::Ready: ++progress.ReadyGroups; break;
			case RuntimeResourceWarmupGroupState::Failed: ++progress.FailedGroups; break;
			case RuntimeResourceWarmupGroupState::Released: ++progress.ReleasedGroups; break;
			}
			progress.Total += group.Total;
			progress.Undispatched += group.Undispatched;
			progress.Dispatched += group.Dispatched;
			progress.Ready += group.Ready;
			progress.Failed += group.Failed;
			progress.Loading += group.Loading;
			progress.Skipped += group.Skipped;
		}
		if (progress.TotalGroups == 0)
			return std::nullopt;

		if (progress.IdleGroups != progress.TotalGroups)
		{
			const bool bEmptyComplete = progress.TotalGroups == progress.ReadyGroups;
			progress.Ratio = CalculateRatio(
				progress.Total,
				progress.Ready,
				progress.Failed,
				progress.Skipped,
				bEmptyComplete);
		}
		return progress;
	}

	void RuntimeResourceWarmupManager::RethrowFailure(std::string_view groupName) const
	{
		if (!mImpl->bInitialized)
			throw std::logic_error("Runtime resource warm-up manager is not initialized.");
		const std::optional<std::size_t> groupIndex = mImpl->FindGroup(groupName);
		if (!groupIndex)
			throw std::logic_error("Unknown runtime resource warm-up group: " + std::string(groupName));

		const Impl::GroupRuntime& runtime = mImpl->Groups[*groupIndex];
		if (runtime.State != RuntimeResourceWarmupGroupState::Failed)
		{
			throw std::logic_error(
				"Runtime resource warm-up group is not failed: " + std::string(groupName));
		}
		if (!runtime.Failure)
		{
			throw std::logic_error(
				"Runtime resource warm-up group has no stored failure: " + std::string(groupName));
		}
		std::rethrow_exception(runtime.Failure);
	}

	bool RuntimeResourceWarmupManager::ReleaseGroup(std::string_view groupName) noexcept
	{
		if (!mImpl->bInitialized)
			return false;
		const std::optional<std::size_t> groupIndex = mImpl->FindGroup(groupName);
		if (!groupIndex)
			return false;

		Impl::GroupRuntime& runtime = mImpl->Groups[*groupIndex];
		if (runtime.State == RuntimeResourceWarmupGroupState::Idle ||
			runtime.State == RuntimeResourceWarmupGroupState::Released)
		{
			return false;
		}
		if (runtime.Resources)
		{
			try
			{
				runtime.LastResources = runtime.Resources.Progress();
			}
			catch (...)
			{
			}
		}
		runtime.Resources.Release();
		runtime.State = RuntimeResourceWarmupGroupState::Released;
		return true;
	}

	std::size_t RuntimeResourceWarmupManager::ReleaseResidency(
		RuntimeResourceWarmupResidency residency) noexcept
	{
		if (!mImpl->bInitialized)
			return 0;

		std::size_t released = 0;
		for (const RuntimeResourceWarmupGroupPlan& group : mImpl->Plan.Groups)
		{
			if (group.Residency == residency && ReleaseGroup(group.Name))
				++released;
		}
		return released;
	}

	void RuntimeResourceWarmupManager::Shutdown() noexcept
	{
		if (!mImpl)
			return;
		for (Impl::GroupRuntime& group : mImpl->Groups)
			group.Resources.Release();
		mImpl->Groups.clear();
		mImpl->Plan = {};
		mImpl->Dispatch = nullptr;
		mImpl->Registry = nullptr;
		mImpl->bInitialized = false;
	}
}
