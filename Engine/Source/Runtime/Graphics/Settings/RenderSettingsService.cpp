#include "Hepch.h"

#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Utils/FileUtils/AtomicFile.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        constexpr std::array c_MergeOrder = {
            SettingsLayer::EngineDefault,
            SettingsLayer::Project,
            SettingsLayer::User,
            SettingsLayer::Session,
            SettingsLayer::CommandLine,
        };

        [[nodiscard]] bool IsPersistentLayer(SettingsLayer layer) noexcept
        {
            return layer == SettingsLayer::Project ||
                layer == SettingsLayer::User;
        }

        [[nodiscard]] bool IsMutableLayer(SettingsLayer layer) noexcept
        {
            return layer == SettingsLayer::Project ||
                layer == SettingsLayer::User ||
                layer == SettingsLayer::Session;
        }

        [[nodiscard]] bool IsToolProperty(std::string_view id) noexcept
        {
            return id == RenderSettingIds::GiDebugView ||
                id == RenderSettingIds::GiDirectOnly ||
                id == RenderSettingIds::PbrDebugMode ||
                id == RenderSettingIds::RenderGraphValidation ||
                id == RenderSettingIds::ShadowCascadeDebug ||
                id == RenderSettingIds::CaptureSupersampleScale;
        }

        [[nodiscard]] bool IsCaptureProperty(std::string_view id) noexcept
        {
            return id == RenderSettingIds::CaptureSupersampleScale;
        }

        [[nodiscard]] bool LayerCanWrite(
            const RenderSettingDescriptor& descriptor,
            SettingsLayer layer) noexcept
        {
            return std::ranges::find(descriptor.WritableLayers, layer) !=
                descriptor.WritableLayers.end();
        }

        void AppendDiagnostics(
            std::vector<RenderSettingDiagnostic>& destination,
            const std::vector<RenderSettingDiagnostic>& source)
        {
            destination.insert(destination.end(), source.begin(), source.end());
        }

        [[nodiscard]] std::string ReadTextIfPresent(
            const std::filesystem::path& path,
            bool& bExisted)
        {
            bExisted = std::filesystem::is_regular_file(path);
            if (!bExisted)
                return {};
            std::ifstream input(path, std::ios::binary);
            if (!input)
                throw std::runtime_error(
                    "Could not read settings transaction backup: " + path.string());
            return { std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>() };
        }

        void SaveSettingsDocument(
            const std::filesystem::path& path,
            const Json& document)
        {
            const std::filesystem::path parent = path.parent_path();
            if (!parent.empty())
            {
                std::error_code error;
                std::filesystem::create_directories(parent, error);
                if (error)
                {
                    throw std::runtime_error(
                        "Could not create settings directory " +
                        parent.string() + ": " + error.message());
                }
            }
            Utils::SaveJsonToFile(document, path);
        }

        void RestoreText(
            const std::filesystem::path& path,
            bool bExisted,
            std::string_view contents)
        {
            if (bExisted)
            {
                Utils::WriteTextToFileAtomically(path, contents);
                return;
            }
            std::error_code error;
            std::filesystem::remove(path, error);
            if (error)
                throw std::runtime_error(
                    "Could not remove settings file created by failed transaction: " +
                    path.string());
        }

        [[nodiscard]] Json SelectKeys(
            const Json& complete,
            const Json& requestedKeys)
        {
            Json selected = Json::object();
            if (!requestedKeys.is_object())
                return selected;
            for (const auto& [id, value] : requestedKeys.items())
            {
                (void)value;
                if (complete.contains(id))
                    selected[id] = complete[id];
            }
            return selected;
        }

        [[nodiscard]] Json SerializeDiagnostic(
            const RenderSettingDiagnostic& diagnostic)
        {
            return Json{
                { "propertyId", diagnostic.PropertyId },
                { "input", diagnostic.InputValue },
                { "normalized", diagnostic.NormalizedValue },
                { "severity", ToString(diagnostic.Severity) },
                { "code", diagnostic.Code },
                { "message", diagnostic.Message },
            };
        }

        [[nodiscard]] Json SerializeChangePlan(
            const RenderSettingsChangePlan& plan)
        {
            Json impacts = Json::array();
            for (ApplyImpact impact : plan.Impacts)
                impacts.push_back(ToString(impact));
            return Json{
                { "changedPropertyIds", plan.ChangedPropertyIds },
                { "impacts", std::move(impacts) },
                { "highestImpact", ToString(plan.HighestImpact) },
                { "gpuIdleRequired", plan.bGpuIdleRequired },
                { "temporalHistoryResetRequired",
                    plan.bTemporalHistoryResetRequired },
                { "renderGraphRebuildRequired",
                    plan.bRenderGraphRebuildRequired },
                { "renderTargetRecreateRequired",
                    plan.bRenderTargetRecreateRequired },
                { "swapchainRecreateRequired",
                    plan.bSwapchainRecreateRequired },
                { "processRestartRequired", plan.bProcessRestartRequired },
                { "buildReconfigureRequired",
                    plan.bBuildReconfigureRequired },
            };
        }
    }

    void RenderSettingsService::Initialize(
        const RenderSettingsServiceInit& init)
    {
        std::scoped_lock lock(mMutex);
        mInit = init;
        mLayers.clear();
        mCapabilities = {};
        mRuntimeApply = {};

        mLayers[SettingsLayer::EngineDefault] = LoadLayer(
            SettingsLayer::EngineDefault, init.EngineDefaultPath, true);
        mLayers[SettingsLayer::Project] = LoadLayer(
            SettingsLayer::Project, init.ProjectOverridePath, false);
        mLayers[SettingsLayer::User] = LoadLayer(
            SettingsLayer::User, init.UserOverridePath, false);
        mLayers[SettingsLayer::Session] = {};
        mLayers[SettingsLayer::CommandLine] = {};

        // Legacy project documents may carry diagnostic/capture values. Keep
        // their runtime behavior for this session, but never write them back to
        // the project layer. A later explicit User save can retain them.
        LayerState& project = mLayers[SettingsLayer::Project];
        LayerState& session = mLayers[SettingsLayer::Session];
        session.DebugValues.update(project.DebugValues);
        session.CaptureValues.update(project.CaptureValues);
        project.DebugValues.clear();
        project.CaptureValues.clear();

        bInitialized = true;
        mSnapshot.Revision = 0;
        RefreshSnapshotLocked();
    }

    void RenderSettingsService::Clear()
    {
        std::scoped_lock lock(mMutex);
        mInit = {};
        mLayers.clear();
        mCapabilities = {};
        mSnapshot = {};
        mRuntimeApply = {};
        bInitialized = false;
    }

    bool RenderSettingsService::IsInitialized() const noexcept
    {
        std::scoped_lock lock(mMutex);
        return bInitialized;
    }

    ResolvedRenderSettings RenderSettingsService::GetSnapshot() const
    {
        std::scoped_lock lock(mMutex);
        return mSnapshot;
    }

    RenderSettingsDraft RenderSettingsService::CreateDraft(
        SettingsLayer targetLayer) const
    {
        std::scoped_lock lock(mMutex);
        return {
            .TargetLayer = targetLayer,
            .BaseRevision = mSnapshot.Revision,
            .BaseHash = mSnapshot.Hash,
            .Patch = Json::object(),
        };
    }

    Json RenderSettingsService::GetLayerValues(SettingsLayer layer) const
    {
        std::scoped_lock lock(mMutex);
        const auto iterator = mLayers.find(layer);
        if (iterator == mLayers.end())
            return Json::object();
        Json result = iterator->second.RenderValues;
        result.update(iterator->second.DebugValues);
        result.update(iterator->second.CaptureValues);
        return result;
    }

    RenderSettingsApplyResult RenderSettingsService::ApplyDraft(
        const RenderSettingsDraft& draft,
        bool bPersist)
    {
        std::scoped_lock lock(mMutex);
        RenderSettingsApplyResult failure;
        failure.Layer = draft.TargetLayer;
        failure.PreviousRevision = mSnapshot.Revision;
        failure.Revision = mSnapshot.Revision;
        failure.Hash = mSnapshot.Hash;
        if (!bInitialized)
        {
            failure.ErrorCode = "not_initialized";
            failure.Message = "RenderSettingsService is not initialized.";
            return failure;
        }
        if (!IsMutableLayer(draft.TargetLayer))
        {
            failure.ErrorCode = "layer_read_only";
            failure.Message = "The requested settings layer is not directly writable.";
            return failure;
        }
        if (!draft.Patch.is_object())
        {
            failure.ErrorCode = "invalid_patch";
            failure.Message = "A settings draft patch must be an object.";
            return failure;
        }
        if (draft.BaseRevision != mSnapshot.Revision ||
            draft.BaseHash != mSnapshot.Hash)
        {
            failure.ErrorCode = "stale_revision";
            failure.Message =
                "The settings draft is older than the current revision; refresh before applying.";
            return failure;
        }
        if (mLayers[draft.TargetLayer].bFutureSchema)
        {
            failure.ErrorCode = "future_schema";
            failure.Message =
                "This layer was written by a newer engine and cannot be overwritten.";
            return failure;
        }

        std::map<SettingsLayer, LayerState> candidate = mLayers;
        LayerState& target = candidate[draft.TargetLayer];
        for (const auto& [id, value] : draft.Patch.items())
        {
            const RenderSettingDescriptor* descriptor =
                FindRenderSettingDescriptor(id);
            if (!descriptor)
            {
                failure.ErrorCode = "unknown_property";
                failure.Message = "Unknown render setting ID: " + id;
                return failure;
            }
            if (!LayerCanWrite(*descriptor, draft.TargetLayer))
            {
                failure.ErrorCode = "layer_not_supported";
                failure.Message = "The setting cannot be written to the requested layer: " + id;
                return failure;
            }
            Json* values = &target.RenderValues;
            if (IsCaptureProperty(id))
                values = &target.CaptureValues;
            else if (IsToolProperty(id))
                values = &target.DebugValues;
            if (value.is_null())
                values->erase(id);
            else
                (*values)[id] = value;
        }
        return ApplyCandidateLocked(draft.TargetLayer, std::move(candidate),
            bPersist, draft.BaseRevision, draft.BaseHash);
    }

    RenderSettingsApplyResult RenderSettingsService::PlanDraft(
        const RenderSettingsDraft& draft) const
    {
        std::scoped_lock lock(mMutex);
        RenderSettingsApplyResult result;
        result.Layer = draft.TargetLayer;
        result.PreviousRevision = mSnapshot.Revision;
        result.Revision = mSnapshot.Revision;
        result.Hash = mSnapshot.Hash;
        if (!bInitialized)
        {
            result.ErrorCode = "not_initialized";
            result.Message = "RenderSettingsService is not initialized.";
            return result;
        }
        if (!IsMutableLayer(draft.TargetLayer) || !draft.Patch.is_object())
        {
            result.ErrorCode = "invalid_draft";
            result.Message = "The settings draft or target layer is invalid.";
            return result;
        }
        if (draft.BaseRevision != mSnapshot.Revision ||
            draft.BaseHash != mSnapshot.Hash)
        {
            result.ErrorCode = "stale_revision";
            result.Message = "The settings draft is stale.";
            return result;
        }
        if (mLayers.at(draft.TargetLayer).bFutureSchema)
        {
            result.ErrorCode = "future_schema";
            result.Message =
                "A newer settings layer cannot be planned for overwrite.";
            return result;
        }
        std::map<SettingsLayer, LayerState> candidate = mLayers;
        LayerState& target = candidate[draft.TargetLayer];
        for (const auto& [id, value] : draft.Patch.items())
        {
            const RenderSettingDescriptor* descriptor =
                FindRenderSettingDescriptor(id);
            if (!descriptor || !LayerCanWrite(*descriptor, draft.TargetLayer))
            {
                result.ErrorCode = descriptor
                    ? "layer_not_supported" : "unknown_property";
                result.Message = "The draft cannot write property: " + id;
                return result;
            }
            Json* values = &target.RenderValues;
            if (IsCaptureProperty(id))
                values = &target.CaptureValues;
            else if (IsToolProperty(id))
                values = &target.DebugValues;
            if (value.is_null())
                values->erase(id);
            else
                (*values)[id] = value;
        }
        ResolvedRenderSettings next = ResolveLocked(candidate);
        result.Diagnostics = next.Diagnostics;
        result.ChangePlan = BuildRenderSettingsChangePlan(mSnapshot, next);
        if (HasDiagnosticErrors(next.Diagnostics))
        {
            result.ErrorCode = "validation_failed";
            result.Message = "The settings draft contains invalid values.";
            return result;
        }
        result.bSuccess = true;
        result.Message =
            "Render settings draft is valid and has not been applied.";
        return result;
    }

    RenderSettingsApplyResult RenderSettingsService::ResetProperty(
        SettingsLayer layer,
        std::string_view propertyId,
        std::uint64_t expectedRevision,
        std::string_view expectedHash,
        bool bPersist)
    {
        RenderSettingsDraft draft{
            .TargetLayer = layer,
            .BaseRevision = expectedRevision,
            .BaseHash = std::string(expectedHash),
            .Patch = Json{ { std::string(propertyId), nullptr } },
        };
        return ApplyDraft(draft, bPersist);
    }

    RenderSettingsApplyResult RenderSettingsService::ResetCategory(
        SettingsLayer layer,
        std::string_view categoryPrefix,
        std::uint64_t expectedRevision,
        std::string_view expectedHash,
        bool bPersist)
    {
        Json patch = Json::object();
        {
            std::scoped_lock lock(mMutex);
            const auto iterator = mLayers.find(layer);
            if (iterator != mLayers.end())
            {
                Json values = iterator->second.RenderValues;
                values.update(iterator->second.DebugValues);
                values.update(iterator->second.CaptureValues);
                for (const auto& [id, value] : values.items())
                {
                    (void)value;
                    if (std::string_view(id).starts_with(categoryPrefix))
                        patch[id] = nullptr;
                }
            }
        }
        return ApplyDraft({ layer, expectedRevision,
            std::string(expectedHash), std::move(patch) }, bPersist);
    }

    RenderSettingsApplyResult RenderSettingsService::ResetLayer(
        SettingsLayer layer,
        std::uint64_t expectedRevision,
        std::string_view expectedHash,
        bool bPersist)
    {
        Json patch = Json::object();
        {
            std::scoped_lock lock(mMutex);
            const auto iterator = mLayers.find(layer);
            if (iterator != mLayers.end())
            {
                Json values = iterator->second.RenderValues;
                values.update(iterator->second.DebugValues);
                values.update(iterator->second.CaptureValues);
                for (const auto& [id, value] : values.items())
                {
                    (void)value;
                    patch[id] = nullptr;
                }
            }
        }
        return ApplyDraft({ layer, expectedRevision,
            std::string(expectedHash), std::move(patch) }, bPersist);
    }

    RenderSettingsApplyResult RenderSettingsService::ResetToEngineDefault(
        std::uint64_t expectedRevision,
        std::string_view expectedHash)
    {
        std::scoped_lock lock(mMutex);
        RenderSettingsApplyResult failure;
        failure.Layer = SettingsLayer::Project;
        failure.PreviousRevision = mSnapshot.Revision;
        failure.Revision = mSnapshot.Revision;
        failure.Hash = mSnapshot.Hash;
        if (expectedRevision != mSnapshot.Revision ||
            expectedHash != mSnapshot.Hash)
        {
            failure.ErrorCode = "stale_revision";
            failure.Message = "The reset request is older than the current revision.";
            return failure;
        }
        if (mLayers[SettingsLayer::Project].bFutureSchema ||
            mLayers[SettingsLayer::User].bFutureSchema)
        {
            failure.ErrorCode = "future_schema";
            failure.Message = "A newer settings document cannot be overwritten.";
            return failure;
        }

        std::map<SettingsLayer, LayerState> candidate = mLayers;
        candidate[SettingsLayer::Project] = {};
        candidate[SettingsLayer::User] = {};
        candidate[SettingsLayer::Session] = {};
        const ResolvedRenderSettings before = mSnapshot;
        ResolvedRenderSettings after = ResolveLocked(candidate);
        after.Revision = before.Revision + 1;
        after.Hash = HashResolvedRenderSettings(after);
        const RenderSettingsChangePlan plan =
            BuildRenderSettingsChangePlan(before, after);
        after.PendingImpact = plan;

        struct Backup
        {
            std::filesystem::path Path;
            std::string Contents;
            bool bExisted = false;
        };
        std::array<Backup, 2> backups{
            Backup{ mInit.ProjectOverridePath },
            Backup{ mInit.UserOverridePath },
        };
        bool bRuntimeApplyStarted = false;
        try
        {
            for (Backup& backup : backups)
                backup.Contents = ReadTextIfPresent(backup.Path, backup.bExisted);
            SaveSettingsDocument(backups[0].Path,
                SerializeRenderSettingsLayerDocument({}));
            SaveSettingsDocument(backups[1].Path,
                SerializeRenderSettingsLayerDocument({}));
            if (mRuntimeApply && !plan.ChangedPropertyIds.empty())
            {
                bRuntimeApplyStarted = true;
                mRuntimeApply(after, plan);
            }
        }
        catch (const std::exception& exception)
        {
            bool bRollbackSucceeded = true;
            try
            {
                for (const Backup& backup : backups)
                    RestoreText(backup.Path, backup.bExisted, backup.Contents);
                if (bRuntimeApplyStarted && mRuntimeApply &&
                    !plan.ChangedPropertyIds.empty())
                    mRuntimeApply(before,
                        BuildRenderSettingsChangePlan(after, before));
            }
            catch (...)
            {
                bRollbackSucceeded = false;
            }
            failure.bRolledBack = bRollbackSucceeded;
            failure.ErrorCode = "transaction_failed";
            failure.Message = exception.what();
            failure.ChangePlan = plan;
            failure.Diagnostics = after.Diagnostics;
            return failure;
        }

        mLayers = std::move(candidate);
        mSnapshot = std::move(after);
        return {
            .bSuccess = true,
            .bSaved = true,
            .Message = "All mutable overrides were removed; Engine defaults are inherited.",
            .PreviousRevision = before.Revision,
            .Revision = mSnapshot.Revision,
            .Hash = mSnapshot.Hash,
            .Layer = SettingsLayer::Project,
            .ChangePlan = plan,
            .Diagnostics = mSnapshot.Diagnostics,
        };
    }

    bool RenderSettingsService::ReloadExternalLayer(SettingsLayer layer)
    {
        std::scoped_lock lock(mMutex);
        if (!IsPersistentLayer(layer))
            return false;
        const std::filesystem::path path = GetLayerPath(layer);
        LayerState loaded = LoadLayer(layer, path, false);
        if (loaded.bFutureSchema || HasDiagnosticErrors(loaded.Diagnostics))
        {
            // Retain the last-known-good values and expose the rejected edit.
            mLayers[layer].Diagnostics = std::move(loaded.Diagnostics);
            mLayers[layer].bFutureSchema = loaded.bFutureSchema;
            RefreshSnapshotLocked();
            return false;
        }
        std::map<SettingsLayer, LayerState> candidate = mLayers;
        candidate[layer] = std::move(loaded);
        ResolvedRenderSettings next = ResolveLocked(candidate);
        if (HasDiagnosticErrors(next.Diagnostics))
        {
            mLayers[layer].Diagnostics = std::move(next.Diagnostics);
            RefreshSnapshotLocked();
            return false;
        }
        next.Revision = mSnapshot.Revision + 1;
        next.Hash = HashResolvedRenderSettings(next);
        next.PendingImpact = BuildRenderSettingsChangePlan(mSnapshot, next);
        bool bRuntimeApplyStarted = false;
        try
        {
            if (mRuntimeApply &&
                !next.PendingImpact.ChangedPropertyIds.empty())
            {
                bRuntimeApplyStarted = true;
                mRuntimeApply(next, next.PendingImpact);
            }
        }
        catch (const std::exception& exception)
        {
            if (bRuntimeApplyStarted && mRuntimeApply)
            {
                try
                {
                    mRuntimeApply(mSnapshot,
                        BuildRenderSettingsChangePlan(next, mSnapshot));
                }
                catch (...)
                {
                }
            }
            mLayers[layer].Diagnostics.push_back({
                .PropertyId = std::string(ToString(layer)),
                .Severity = RenderSettingDiagnosticSeverity::Error,
                .Code = "hot_reload_apply_failed",
                .Message = exception.what(),
            });
            RefreshSnapshotLocked();
            return false;
        }
        mLayers = std::move(candidate);
        mSnapshot = std::move(next);
        return true;
    }

    void RenderSettingsService::SetCapabilities(
        RenderSettingsCapabilities capabilities)
    {
        std::scoped_lock lock(mMutex);
        mCapabilities = std::move(capabilities);
        if (bInitialized)
        {
            const std::uint64_t revision = mSnapshot.Revision + 1;
            RefreshSnapshotLocked();
            mSnapshot.Revision = revision;
        }
    }

    void RenderSettingsService::SetRuntimeApplyCallback(
        RuntimeApplyCallback callback)
    {
        std::scoped_lock lock(mMutex);
        mRuntimeApply = std::move(callback);
    }

    void RenderSettingsService::SetCommandLineOverrides(
        const Json& stableIdPatch)
    {
        std::scoped_lock lock(mMutex);
        if (!bInitialized)
            throw std::logic_error(
                "Command-line render settings require an initialized service.");
        if (!stableIdPatch.is_object())
            throw std::invalid_argument(
                "Command-line render settings patch must be an object.");
        LayerState& layer = mLayers[SettingsLayer::CommandLine];
        for (const auto& [id, value] : stableIdPatch.items())
        {
            const RenderSettingDescriptor* descriptor =
                FindRenderSettingDescriptor(id);
            if (!descriptor)
                throw std::invalid_argument(
                    "Unknown command-line render setting ID: " + id);
            Json* values = &layer.RenderValues;
            if (IsCaptureProperty(id))
                values = &layer.CaptureValues;
            else if (IsToolProperty(id))
                values = &layer.DebugValues;
            if (value.is_null())
                values->erase(id);
            else
                (*values)[id] = value;
        }
        const std::uint64_t revision = mSnapshot.Revision + 1;
        RefreshSnapshotLocked();
        mSnapshot.Revision = revision;
    }

    ResolvedRenderSettings RenderSettingsService::ResolveWithWorldCameraOverrides(
        const Json& worldOverrides,
        const Json& cameraOverrides) const
    {
        std::scoped_lock lock(mMutex);
        std::map<SettingsLayer, LayerState> layers = mLayers;
        layers[SettingsLayer::World].RenderValues = worldOverrides;
        layers[SettingsLayer::Camera].RenderValues = cameraOverrides;

        RenderSettings requested = MakeEmergencyFallbackRenderSettings();
        RenderDebugSettings debug;
        RenderCaptureSettings capture;
        ResolvedRenderSettings result;
        for (SettingsLayer layer : { SettingsLayer::EngineDefault,
                 SettingsLayer::Project, SettingsLayer::User,
                 SettingsLayer::Session, SettingsLayer::World,
                 SettingsLayer::Camera, SettingsLayer::CommandLine })
        {
            const auto iterator = layers.find(layer);
            if (iterator == layers.end())
                continue;
            requested = ApplyRenderSettingsFlatValues(requested,
                iterator->second.RenderValues, result.Diagnostics);
            debug = ApplyRenderDebugFlatValues(debug,
                iterator->second.DebugValues, result.Diagnostics);
            capture = ApplyRenderCaptureFlatValues(capture,
                iterator->second.CaptureValues, result.Diagnostics);
            for (const auto& [id, value] : iterator->second.RenderValues.items())
            {
                (void)value;
                result.Sources[id] = layer;
            }
        }
        ResolvedRenderSettings resolved = ResolveRenderSettings(
            requested, debug, capture, mCapabilities);
        AppendDiagnostics(resolved.Diagnostics, result.Diagnostics);
        resolved.Sources = std::move(result.Sources);
        resolved.EffectiveSources = resolved.Sources;
        const Json requestedValues =
            RenderSettingsToFlatValues(resolved.Requested);
        const Json effectiveValues =
            RenderSettingsToFlatValues(resolved.Effective);
        for (const auto& [id, value] : effectiveValues.items())
        {
            if (requestedValues.contains(id) && requestedValues[id] != value)
                resolved.EffectiveSources[id] = SettingsLayer::Capability;
        }
        resolved.Revision = mSnapshot.Revision;
        resolved.Hash = HashResolvedRenderSettings(resolved);
        return resolved;
    }

    GlobalIlluminationSettings
        RenderSettingsService::BuildRequestedRuntimeGlobalIllumination() const
    {
        std::scoped_lock lock(mMutex);
        GlobalIlluminationSettings result =
            mSnapshot.Requested.GlobalIllumination;
        result.DebugView = mSnapshot.Debug.GiDebugView;
        result.bDirectOnly = mSnapshot.Debug.bGiDirectOnly;
        return result;
    }

    AntiAliasingSettings
        RenderSettingsService::BuildRequestedRuntimeAntiAliasing() const
    {
        std::scoped_lock lock(mMutex);
        AntiAliasingSettings result = mSnapshot.Requested.AntiAliasing;
        result.CaptureSupersampleScale = mSnapshot.Capture.SupersampleScale;
        return result;
    }

    RenderSettingsService::LayerState RenderSettingsService::LoadLayer(
        SettingsLayer layer,
        const std::filesystem::path& path,
        bool bRequired) const
    {
        LayerState state;
        if (path.empty() || !std::filesystem::is_regular_file(path))
        {
            if (bRequired)
            {
                state.Diagnostics.push_back({
                    .PropertyId = "Schema",
                    .InputValue = path.string(),
                    .Severity = RenderSettingDiagnosticSeverity::Warning,
                    .Code = "engine_default_missing",
                    .Message =
                        "Engine Default is missing; C++ emergency fallback is active.",
                });
            }
            return state;
        }
        try
        {
            const RenderSettingsMigrationResult migration =
                MigrateRenderSettingsDocument(Utils::LoadJsonFile(path));
            state.Diagnostics = migration.Diagnostics;
            state.bFutureSchema = migration.bFutureSchema;
            if (!migration.bSuccess)
                return state;

            std::vector<RenderSettingDiagnostic> validation;
            const RenderSettings normalizedSettings =
                ApplyRenderSettingsFlatValues(
                    MakeEmergencyFallbackRenderSettings(),
                    migration.RenderValues, validation);
            const RenderDebugSettings normalizedDebug =
                ApplyRenderDebugFlatValues({}, migration.DebugValues, validation);
            const RenderCaptureSettings normalizedCapture =
                ApplyRenderCaptureFlatValues({}, migration.CaptureValues, validation);
            AppendDiagnostics(state.Diagnostics, validation);
            if (HasDiagnosticErrors(validation))
                return state;
            state.RenderValues = SelectKeys(
                RenderSettingsToFlatValues(normalizedSettings),
                migration.RenderValues);
            state.DebugValues = SelectKeys(
                RenderDebugSettingsToFlatValues(normalizedDebug),
                migration.DebugValues);
            state.CaptureValues = SelectKeys(
                RenderCaptureSettingsToFlatValues(normalizedCapture),
                migration.CaptureValues);
        }
        catch (const std::exception& exception)
        {
            state.Diagnostics.push_back({
                .PropertyId = std::string(ToString(layer)),
                .InputValue = path.string(),
                .Severity = RenderSettingDiagnosticSeverity::Error,
                .Code = "load_failed",
                .Message = exception.what(),
            });
        }
        return state;
    }

    ResolvedRenderSettings RenderSettingsService::ResolveLocked(
        const std::map<SettingsLayer, LayerState>& layers) const
    {
        RenderSettings requested = MakeEmergencyFallbackRenderSettings();
        RenderDebugSettings debug;
        RenderCaptureSettings capture;
        std::vector<RenderSettingDiagnostic> diagnostics;
        std::map<std::string, SettingsLayer, std::less<>> sources;
        for (const RenderSettingDescriptor& descriptor :
            GetRenderSettingDescriptors())
        {
            sources[descriptor.PropertyId] = SettingsLayer::EmergencyFallback;
        }

        for (SettingsLayer layer : c_MergeOrder)
        {
            const auto iterator = layers.find(layer);
            if (iterator == layers.end())
                continue;
            const LayerState& state = iterator->second;
            AppendDiagnostics(diagnostics, state.Diagnostics);
            requested = ApplyRenderSettingsFlatValues(
                requested, state.RenderValues, diagnostics);
            debug = ApplyRenderDebugFlatValues(
                debug, state.DebugValues, diagnostics);
            capture = ApplyRenderCaptureFlatValues(
                capture, state.CaptureValues, diagnostics);
            for (const Json* values : { &state.RenderValues,
                     &state.DebugValues, &state.CaptureValues })
            {
                for (const auto& [id, value] : values->items())
                {
                    (void)value;
                    sources[id] = layer;
                }
            }
        }

        // These legacy bridge fields must never participate in persistent
        // requested state. They are injected only when calling the renderer.
        requested.GlobalIllumination.DebugView =
            GlobalIlluminationDebugView::Composite;
        requested.GlobalIllumination.bDirectOnly = false;
        requested.AntiAliasing.CaptureSupersampleScale = 1;

        ResolvedRenderSettings result = ResolveRenderSettings(
            requested, debug, capture, mCapabilities);
        AppendDiagnostics(result.Diagnostics, diagnostics);
        result.Sources = std::move(sources);
        result.EffectiveSources = result.Sources;
        const Json requestedValues = RenderSettingsToFlatValues(result.Requested);
        const Json effectiveValues = RenderSettingsToFlatValues(result.Effective);
        for (const auto& [id, value] : effectiveValues.items())
        {
            if (requestedValues.contains(id) && requestedValues[id] != value)
                result.EffectiveSources[id] = SettingsLayer::Capability;
        }
        result.Hash = HashResolvedRenderSettings(result);
        return result;
    }

    void RenderSettingsService::RefreshSnapshotLocked()
    {
        const std::uint64_t revision = mSnapshot.Revision;
        mSnapshot = ResolveLocked(mLayers);
        mSnapshot.Revision = revision;
        mSnapshot.Hash = HashResolvedRenderSettings(mSnapshot);
    }

    std::filesystem::path RenderSettingsService::GetLayerPath(
        SettingsLayer layer) const
    {
        if (layer == SettingsLayer::Project)
            return mInit.ProjectOverridePath;
        if (layer == SettingsLayer::User)
            return mInit.UserOverridePath;
        if (layer == SettingsLayer::EngineDefault)
            return mInit.EngineDefaultPath;
        return {};
    }

    Json RenderSettingsService::BuildSparseLayerDocumentLocked(
        SettingsLayer layer,
        std::map<SettingsLayer, LayerState>& layers) const
    {
        LayerState& target = layers[layer];
        RenderSettings inherited = MakeEmergencyFallbackRenderSettings();
        RenderDebugSettings inheritedDebug;
        RenderCaptureSettings inheritedCapture;
        std::vector<RenderSettingDiagnostic> ignored;
        for (SettingsLayer candidate : c_MergeOrder)
        {
            if (candidate == layer)
                break;
            const auto iterator = layers.find(candidate);
            if (iterator == layers.end())
                continue;
            inherited = ApplyRenderSettingsFlatValues(inherited,
                iterator->second.RenderValues, ignored);
            inheritedDebug = ApplyRenderDebugFlatValues(inheritedDebug,
                iterator->second.DebugValues, ignored);
            inheritedCapture = ApplyRenderCaptureFlatValues(inheritedCapture,
                iterator->second.CaptureValues, ignored);
        }
        const Json inheritedValues = RenderSettingsToFlatValues(inherited);
        const Json inheritedDebugValues =
            RenderDebugSettingsToFlatValues(inheritedDebug);
        const Json inheritedCaptureValues =
            RenderCaptureSettingsToFlatValues(inheritedCapture);
        const auto compact = [](Json& values, const Json& lower)
        {
            std::vector<std::string> eraseIds;
            for (const auto& [id, value] : values.items())
            {
                if (lower.contains(id) && lower[id] == value)
                    eraseIds.push_back(id);
            }
            for (const std::string& id : eraseIds)
                values.erase(id);
        };
        compact(target.RenderValues, inheritedValues);
        compact(target.DebugValues, inheritedDebugValues);
        compact(target.CaptureValues, inheritedCaptureValues);
        if (layer == SettingsLayer::Project)
        {
            target.DebugValues.clear();
            target.CaptureValues.clear();
        }
        return SerializeRenderSettingsLayerDocument(target.RenderValues,
            target.DebugValues, target.CaptureValues);
    }

    RenderSettingsApplyResult RenderSettingsService::ApplyCandidateLocked(
        SettingsLayer layer,
        std::map<SettingsLayer, LayerState> candidateLayers,
        bool bPersist,
        std::uint64_t expectedRevision,
        std::string_view expectedHash)
    {
        RenderSettingsApplyResult result;
        result.Layer = layer;
        result.PreviousRevision = mSnapshot.Revision;
        result.Revision = mSnapshot.Revision;
        result.Hash = mSnapshot.Hash;
        if (expectedRevision != mSnapshot.Revision ||
            expectedHash != mSnapshot.Hash)
        {
            result.ErrorCode = "stale_revision";
            result.Message = "The settings draft is stale.";
            return result;
        }

        LayerState& normalizedTarget = candidateLayers[layer];
        std::vector<RenderSettingDiagnostic> normalizationDiagnostics;
        const RenderSettings normalizedSettings = ApplyRenderSettingsFlatValues(
            MakeEmergencyFallbackRenderSettings(),
            normalizedTarget.RenderValues, normalizationDiagnostics);
        const RenderDebugSettings normalizedDebug = ApplyRenderDebugFlatValues(
            {}, normalizedTarget.DebugValues, normalizationDiagnostics);
        const RenderCaptureSettings normalizedCapture =
            ApplyRenderCaptureFlatValues({}, normalizedTarget.CaptureValues,
                normalizationDiagnostics);
        normalizedTarget.RenderValues = SelectKeys(
            RenderSettingsToFlatValues(normalizedSettings),
            normalizedTarget.RenderValues);
        normalizedTarget.DebugValues = SelectKeys(
            RenderDebugSettingsToFlatValues(normalizedDebug),
            normalizedTarget.DebugValues);
        normalizedTarget.CaptureValues = SelectKeys(
            RenderCaptureSettingsToFlatValues(normalizedCapture),
            normalizedTarget.CaptureValues);
        AppendDiagnostics(normalizedTarget.Diagnostics,
            normalizationDiagnostics);

        Json document;
        std::filesystem::path path;
        if (bPersist && IsPersistentLayer(layer))
        {
            path = GetLayerPath(layer);
            document = BuildSparseLayerDocumentLocked(layer, candidateLayers);
        }
        ResolvedRenderSettings next = ResolveLocked(candidateLayers);
        next.Revision = mSnapshot.Revision + 1;
        next.Hash = HashResolvedRenderSettings(next);
        next.PendingImpact = BuildRenderSettingsChangePlan(mSnapshot, next);
        result.ChangePlan = next.PendingImpact;
        result.Diagnostics = next.Diagnostics;
        if (HasDiagnosticErrors(next.Diagnostics))
        {
            result.ErrorCode = "validation_failed";
            result.Message = "The settings draft contains invalid values.";
            return result;
        }

        bool bFileExisted = false;
        std::string backup;
        bool bRuntimeApplyStarted = false;
        try
        {
            if (!path.empty())
            {
                backup = ReadTextIfPresent(path, bFileExisted);
                SaveSettingsDocument(path, document);
                result.bSaved = true;
            }
            if (mRuntimeApply &&
                !next.PendingImpact.ChangedPropertyIds.empty())
            {
                bRuntimeApplyStarted = true;
                mRuntimeApply(next, next.PendingImpact);
            }
        }
        catch (const std::exception& exception)
        {
            bool bRollbackSucceeded = true;
            try
            {
                if (!path.empty())
                    RestoreText(path, bFileExisted, backup);
                if (bRuntimeApplyStarted && mRuntimeApply &&
                    !next.PendingImpact.ChangedPropertyIds.empty())
                {
                    mRuntimeApply(mSnapshot,
                        BuildRenderSettingsChangePlan(next, mSnapshot));
                }
            }
            catch (...)
            {
                bRollbackSucceeded = false;
            }
            result.bSaved = false;
            result.bRolledBack = bRollbackSucceeded;
            result.ErrorCode = "transaction_failed";
            result.Message = exception.what();
            return result;
        }

        const std::uint64_t previousRevision = mSnapshot.Revision;
        mLayers = std::move(candidateLayers);
        mSnapshot = std::move(next);
        result.bSuccess = true;
        result.Message = "Render settings transaction committed.";
        result.PreviousRevision = previousRevision;
        result.Revision = mSnapshot.Revision;
        result.Hash = mSnapshot.Hash;
        result.Diagnostics = mSnapshot.Diagnostics;
        return result;
    }

    Json SerializeResolvedRenderSettings(
        const ResolvedRenderSettings& settings,
        bool bIncludeDescriptors)
    {
        Json diagnostics = Json::array();
        for (const RenderSettingDiagnostic& diagnostic : settings.Diagnostics)
            diagnostics.push_back(SerializeDiagnostic(diagnostic));
        Json sources = Json::object();
        for (const auto& [id, layer] : settings.Sources)
            sources[id] = ToString(layer);
        Json availability = Json::object();
        Json effectiveSources = Json::object();
        for (const auto& [id, layer] : settings.EffectiveSources)
            effectiveSources[id] = ToString(layer);
        for (const auto& [id, value] : settings.Availability)
        {
            availability[id] = {
                { "available", value.bAvailable },
                { "reason", value.Reason },
            };
        }
        Json result{
            { "schema", std::string(RenderSettingsSchema) },
            { "schemaVersion", RenderSettingsSchemaVersion },
            { "revision", settings.Revision },
            { "hash", settings.Hash },
            { "requested", RenderSettingsToFlatValues(settings.Requested) },
            { "effective", RenderSettingsToFlatValues(settings.Effective) },
            { "debug", RenderDebugSettingsToFlatValues(settings.Debug) },
            { "capture", RenderCaptureSettingsToFlatValues(settings.Capture) },
            { "sources", std::move(sources) },
            { "effectiveSources", std::move(effectiveSources) },
            { "availability", std::move(availability) },
            { "diagnostics", std::move(diagnostics) },
            { "impact", SerializeChangePlan(settings.PendingImpact) },
            { "capabilities", {
                { "adapterName", settings.Capabilities.Render.AdapterName },
                { "hardwareRayTracing",
                    settings.Capabilities.Render.bHardwareRayTracingRendererReady },
                { "screenSpaceGI",
                    settings.Capabilities.bScreenSpaceGiAvailable },
                { "externalViewportRenderer",
                    settings.Capabilities.bExternalViewportRendererAvailable },
                { "externalViewportRendererActive",
                    settings.Capabilities.bExternalViewportRendererActive },
                { "antiAliasingRenderPath",
                    settings.Capabilities.AntiAliasingPath ==
                            AntiAliasingRenderPath::OfflinePathTracing
                        ? "OfflinePathTracing" : "RealtimeRaster" },
            } },
        };
        if (bIncludeDescriptors)
        {
            result["descriptors"] = Json::array();
            for (const RenderSettingDescriptor& descriptor :
                GetRenderSettingDescriptors())
            {
                Json layers = Json::array();
                for (SettingsLayer layer : descriptor.WritableLayers)
                    layers.push_back(ToString(layer));
                Json presets = Json::array();
                for (GlobalIlluminationQualityPreset preset :
                    descriptor.PresetMembership)
                    presets.push_back(ToString(preset));
                result["descriptors"].push_back({
                    { "propertyId", descriptor.PropertyId },
                    { "type", static_cast<std::uint32_t>(descriptor.Type) },
                    { "default", descriptor.DefaultValue },
                    { "minimum", descriptor.Minimum
                        ? Json(*descriptor.Minimum) : Json() },
                    { "maximum", descriptor.Maximum
                        ? Json(*descriptor.Maximum) : Json() },
                    { "unit", descriptor.Unit },
                    { "category", descriptor.Category },
                    { "subcategory", descriptor.Subcategory },
                    { "labelTextId", descriptor.LabelTextId },
                    { "descriptionTextId", descriptor.DescriptionTextId },
                    { "englishFallback", descriptor.EnglishFallback },
                    { "writableLayers", std::move(layers) },
                    { "advanced", descriptor.bAdvanced },
                    { "capabilityPredicate", descriptor.CapabilityPredicate },
                    { "applyImpact", ToString(descriptor.Impact) },
                    { "restartRequired", descriptor.bRestartRequired },
                    { "presetMembership", std::move(presets) },
                    { "persistedRenderSetting",
                        descriptor.bPersistedRenderSetting },
                });
            }
        }
        return result;
    }

    Json SerializeRenderSettingsApplyResult(
        const RenderSettingsApplyResult& result)
    {
        Json diagnostics = Json::array();
        for (const RenderSettingDiagnostic& diagnostic : result.Diagnostics)
            diagnostics.push_back(SerializeDiagnostic(diagnostic));
        return Json{
            { "success", result.bSuccess },
            { "saved", result.bSaved },
            { "rolledBack", result.bRolledBack },
            { "errorCode", result.ErrorCode },
            { "message", result.Message },
            { "previousRevision", result.PreviousRevision },
            { "revision", result.Revision },
            { "hash", result.Hash },
            { "layer", ToString(result.Layer) },
            { "changePlan", SerializeChangePlan(result.ChangePlan) },
            { "diagnostics", std::move(diagnostics) },
        };
    }
}
