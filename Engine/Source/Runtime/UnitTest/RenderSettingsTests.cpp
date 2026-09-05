#include "Hepch.h"

#include "Runtime/UnitTest/RenderSettingsTests.h"

#include "Runtime/Graphics/Settings/RenderSettings.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
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

        [[nodiscard]] bool NearlyEqual(
            float left,
            float right,
            float epsilon = 1e-6f) noexcept
        {
            return std::abs(left - right) <= epsilon;
        }

        [[nodiscard]] std::string Id(std::string_view propertyId)
        {
            return std::string(propertyId);
        }

        [[nodiscard]] const Json* FindDottedPath(
            const Json& root,
            std::string_view dottedPath)
        {
            const Json* current = &root;
            std::size_t start = 0;
            while (start < dottedPath.size())
            {
                const std::size_t end = dottedPath.find('.', start);
                const std::string key(dottedPath.substr(start,
                    end == std::string_view::npos
                        ? dottedPath.size() - start
                        : end - start));
                if (!current->is_object())
                    return nullptr;
                const auto iterator = current->find(key);
                if (iterator == current->end())
                    return nullptr;
                current = &*iterator;
                if (end == std::string_view::npos)
                    return current;
                start = end + 1;
            }
            return nullptr;
        }

        [[nodiscard]] bool HasDiagnostic(
            const std::vector<RenderSettingDiagnostic>& diagnostics,
            std::string_view propertyId,
            std::string_view code,
            RenderSettingDiagnosticSeverity severity)
        {
            return std::ranges::any_of(diagnostics,
                [&](const RenderSettingDiagnostic& diagnostic)
                {
                    return diagnostic.PropertyId == propertyId &&
                        diagnostic.Code == code &&
                        diagnostic.Severity == severity;
                });
        }

        class TemporaryDirectory final
        {
        public:
            TemporaryDirectory()
            {
                static std::atomic_uint64_t sCounter = 0;
                const auto timestamp = std::chrono::steady_clock::now()
                    .time_since_epoch().count();
                mPath = std::filesystem::temp_directory_path() /
                    ("HEngine-RenderSettingsTests-" +
                        std::to_string(timestamp) + "-" +
                        std::to_string(sCounter.fetch_add(
                            1, std::memory_order_relaxed)));
                std::filesystem::create_directories(mPath);
            }

            TemporaryDirectory(const TemporaryDirectory&) = delete;
            TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

            ~TemporaryDirectory()
            {
                std::error_code error;
                std::filesystem::remove_all(mPath, error);
            }

            [[nodiscard]] const std::filesystem::path& GetPath() const noexcept
            {
                return mPath;
            }

        private:
            std::filesystem::path mPath;
        };

        class ScopedRenderSettingsService final
        {
        public:
            explicit ScopedRenderSettingsService(
                const RenderSettingsServiceInit& init)
                : mService(RenderSettingsService::GetInstance())
            {
                mService.Clear();
                mService.Initialize(init);
            }

            ScopedRenderSettingsService(
                const ScopedRenderSettingsService&) = delete;
            ScopedRenderSettingsService& operator=(
                const ScopedRenderSettingsService&) = delete;

            ~ScopedRenderSettingsService()
            {
                mService.Clear();
            }

            [[nodiscard]] RenderSettingsService& Get() noexcept
            {
                return mService;
            }

        private:
            RenderSettingsService& mService;
        };

        [[nodiscard]] Json MakeEngineDefaultDocument()
        {
            return SerializeRenderSettingsLayerDocument(
                RenderSettingsToFlatValues(
                    MakeEmergencyFallbackRenderSettings()));
        }

        void SaveDocument(
            const std::filesystem::path& path,
            const Json& document)
        {
            if (!path.parent_path().empty())
                std::filesystem::create_directories(path.parent_path());
            Utils::SaveJsonToFile(document, path);
        }

        struct SettingsPaths
        {
            std::filesystem::path Engine;
            std::filesystem::path Project;
            std::filesystem::path User;
        };

        [[nodiscard]] SettingsPaths MakeSettingsPaths(
            const TemporaryDirectory& directory)
        {
            return {
                .Engine = directory.GetPath() / "DefaultEngine.json",
                .Project = directory.GetPath() / "BaseEngine.json",
                .User = directory.GetPath() / "UserEngine.json",
            };
        }

        [[nodiscard]] RenderSettingsServiceInit MakeServiceInit(
            const SettingsPaths& paths)
        {
            return {
                .EngineDefaultPath = paths.Engine,
                .ProjectOverridePath = paths.Project,
                .UserOverridePath = paths.User,
            };
        }

        void TestLegacyMigration()
        {
            const Json legacy = {
                { "Renderer", {
                    { "API", "DX12" },
                    { "VSync", true },
                    { "AntiAliasing", {
                        { "Mode", "TAA" },
                        { "TaaHistoryWeight", 0.875 },
                        { "CaptureSupersampleScale", 4 },
                    } },
                    { "GlobalIllumination", {
                        { "Technique", "HardwareRTGI" },
                        { "UnsupportedFallback", "ScreenSpaceGI" },
                        { "DebugView", static_cast<std::uint32_t>(
                            GlobalIlluminationDebugView::Coverage) },
                        { "DirectOnly", true },
                        { "RenderScale", 0.75 },
                        { "SamplesPerPixel", 3 },
                        { "SSGI", {
                            { "RayCount", 4 },
                            { "StepCount", 96 },
                        } },
                    } },
                } },
            };

            const RenderSettingsMigrationResult migration =
                MigrateRenderSettingsDocument(legacy);
            Require(migration.bSuccess && migration.bMigrated &&
                    migration.SourceVersion == 0,
                "Legacy BaseEngine JSON was not recognized as schema version 0.");
            Require(migration.RenderValues.at(Id(
                        RenderSettingIds::GiTechnique)) == "HardwareRTGI" &&
                    migration.RenderValues.at(Id(
                        RenderSettingIds::AntiAliasingMode)) == "TAA" &&
                    !migration.RenderValues.contains(Id(
                        RenderSettingIds::GiDebugView)) &&
                    !migration.RenderValues.contains(Id(
                        RenderSettingIds::CaptureSupersampleScale)),
                "Legacy migration mixed requested settings with tool settings.");
            Require(migration.DebugValues.at(Id(
                        RenderSettingIds::GiDebugView)) ==
                        static_cast<std::uint32_t>(
                            GlobalIlluminationDebugView::Coverage) &&
                    migration.DebugValues.at(Id(
                        RenderSettingIds::GiDirectOnly)) == true &&
                    migration.CaptureValues.at(Id(
                        RenderSettingIds::CaptureSupersampleScale)) == 4,
                "Legacy diagnostic or capture settings were not routed to their own domains.");

            std::vector<RenderSettingDiagnostic> diagnostics;
            const RenderSettings migratedSettings =
                ApplyRenderSettingsFlatValues(
                    MakeEmergencyFallbackRenderSettings(),
                    migration.RenderValues,
                    diagnostics);
            const RenderDebugSettings migratedDebug =
                ApplyRenderDebugFlatValues({}, migration.DebugValues,
                    diagnostics);
            const RenderCaptureSettings migratedCapture =
                ApplyRenderCaptureFlatValues({}, migration.CaptureValues,
                    diagnostics);
            Require(!HasDiagnosticErrors(diagnostics) &&
                    migratedSettings.bVSync &&
                    migratedSettings.AntiAliasing.Mode ==
                        AntiAliasingMode::Taa &&
                    migratedSettings.GlobalIllumination.Technique ==
                        GlobalIlluminationTechniqueKind::HardwareRayTracing &&
                    NearlyEqual(
                        migratedSettings.GlobalIllumination.RenderScale,
                        0.75f) &&
                    migratedDebug.GiDebugView ==
                        GlobalIlluminationDebugView::Coverage &&
                    migratedDebug.bGiDirectOnly &&
                    migratedCapture.SupersampleScale == 4,
                "Legacy migration did not preserve the previous effective behavior.");

            Json versionOne = SerializeRenderSettingsLayerDocument({
                { RenderSettingIds::VSync, true },
            });
            versionOne["SchemaVersion"] = 1u;
            const RenderSettingsMigrationResult versionOneMigration =
                MigrateRenderSettingsDocument(versionOne);
            Require(versionOneMigration.bSuccess &&
                    versionOneMigration.bMigrated &&
                    versionOneMigration.SourceVersion == 1 &&
                    versionOneMigration.RenderValues.at(Id(
                        RenderSettingIds::VSync)) == true,
                "Schema version 1 did not migrate through the pure migration path.");
        }

        void TestLayerMergeAndSources()
        {
            TemporaryDirectory directory;
            const SettingsPaths paths = MakeSettingsPaths(directory);
            SaveDocument(paths.Engine, MakeEngineDefaultDocument());
            SaveDocument(paths.Project, SerializeRenderSettingsLayerDocument({
                { RenderSettingIds::VSync, true },
                { RenderSettingIds::AntiAliasingMode, "FXAA" },
                { RenderSettingIds::GiSamplesPerPixel, 2 },
            }));
            SaveDocument(paths.User, SerializeRenderSettingsLayerDocument({
                { RenderSettingIds::AntiAliasingMode, "SMAA" },
                { RenderSettingIds::PostProcessExposure, 1.25 },
            }));

            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            RenderSettingsService& service = scope.Get();
            RenderSettingsDraft session = service.CreateDraft(
                SettingsLayer::Session);
            session.Patch = {
                { RenderSettingIds::GiRenderScale, 0.5 },
                { RenderSettingIds::ShadowsEnabled, false },
            };
            const RenderSettingsApplyResult sessionResult =
                service.ApplyDraft(session, false);
            Require(sessionResult.bSuccess,
                "A valid Session-layer draft failed to apply.");

            service.SetCommandLineOverrides({
                { RenderSettingIds::VSync, false },
                { RenderSettingIds::AntiAliasingMode, "TAA" },
            });
            const ResolvedRenderSettings snapshot = service.GetSnapshot();
            Require(!snapshot.Requested.bVSync &&
                    snapshot.Requested.AntiAliasing.Mode ==
                        AntiAliasingMode::Taa &&
                    snapshot.Requested.GlobalIllumination.SamplesPerPixel == 2 &&
                    NearlyEqual(
                        snapshot.Requested.GlobalIllumination.RenderScale,
                        0.5f) &&
                    !snapshot.Requested.Shadows.bEnabled &&
                    NearlyEqual(snapshot.Requested.PostProcess.Exposure,
                        1.25f),
                "Engine/Project/User/Session/CommandLine precedence was not respected.");
            Require(snapshot.Sources.at(Id(RenderSettingIds::Backend)) ==
                        SettingsLayer::EngineDefault &&
                    snapshot.Sources.at(Id(RenderSettingIds::GiSamplesPerPixel)) ==
                        SettingsLayer::Project &&
                    snapshot.Sources.at(Id(RenderSettingIds::PostProcessExposure)) ==
                        SettingsLayer::User &&
                    snapshot.Sources.at(Id(RenderSettingIds::GiRenderScale)) ==
                        SettingsLayer::Session &&
                    snapshot.Sources.at(Id(RenderSettingIds::AntiAliasingMode)) ==
                        SettingsLayer::CommandLine,
                "Resolved field sources did not identify the winning layer.");

            const ResolvedRenderSettings worldCamera =
                service.ResolveWithWorldCameraOverrides(
                    { { RenderSettingIds::PostProcessExposure, 2.0 } },
                    { { RenderSettingIds::PostProcessExposure, 3.0 } });
            Require(NearlyEqual(worldCamera.Requested.PostProcess.Exposure,
                        3.0f) &&
                    worldCamera.Sources.at(Id(
                        RenderSettingIds::PostProcessExposure)) ==
                        SettingsLayer::Camera &&
                    NearlyEqual(service.GetSnapshot().Requested.PostProcess.Exposure,
                        1.25f),
                "World/Camera overrides were not detached or Camera did not win.");
        }

        void TestSparseResetAndEngineInheritance()
        {
            TemporaryDirectory directory;
            const SettingsPaths paths = MakeSettingsPaths(directory);
            SaveDocument(paths.Engine, MakeEngineDefaultDocument());
            SaveDocument(paths.Project, SerializeRenderSettingsLayerDocument({
                { RenderSettingIds::VSync, true },
                { RenderSettingIds::GiRenderScale, 0.5 },
                { RenderSettingIds::GiMaxBounces, 2 },
                { RenderSettingIds::ShadowsEnabled, false },
            }));
            SaveDocument(paths.User, SerializeRenderSettingsLayerDocument({}));

            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            RenderSettingsService& service = scope.Get();
            ResolvedRenderSettings snapshot = service.GetSnapshot();
            RenderSettingsApplyResult result = service.ResetProperty(
                SettingsLayer::Project,
                RenderSettingIds::VSync,
                snapshot.Revision,
                snapshot.Hash,
                true);
            Require(result.bSuccess && result.bSaved &&
                    !service.GetSnapshot().Requested.bVSync &&
                    !service.GetLayerValues(SettingsLayer::Project).contains(
                        Id(RenderSettingIds::VSync)) &&
                    FindDottedPath(Utils::LoadJsonFile(paths.Project),
                        RenderSettingIds::VSync) == nullptr,
                "ResetProperty copied a default instead of removing the override.");

            snapshot = service.GetSnapshot();
            result = service.ResetCategory(
                SettingsLayer::Project,
                "Rendering.GI.",
                snapshot.Revision,
                snapshot.Hash,
                true);
            const Json afterCategory =
                service.GetLayerValues(SettingsLayer::Project);
            Require(result.bSuccess &&
                    !afterCategory.contains(Id(RenderSettingIds::GiRenderScale)) &&
                    !afterCategory.contains(Id(RenderSettingIds::GiMaxBounces)) &&
                    afterCategory.at(Id(RenderSettingIds::ShadowsEnabled)) == false,
                "ResetCategory removed unrelated settings or retained GI overrides.");

            RenderSettingsDraft inheritedValue = service.CreateDraft(
                SettingsLayer::Project);
            inheritedValue.Patch[Id(RenderSettingIds::VSync)] = false;
            result = service.ApplyDraft(inheritedValue, true);
            Require(result.bSuccess &&
                    !service.GetLayerValues(SettingsLayer::Project).contains(
                        Id(RenderSettingIds::VSync)),
                "Sparse persistence retained a value equal to its inherited value.");

            snapshot = service.GetSnapshot();
            result = service.ResetLayer(SettingsLayer::Project,
                snapshot.Revision, snapshot.Hash, true);
            Require(result.bSuccess &&
                    service.GetLayerValues(SettingsLayer::Project).empty(),
                "ResetLayer did not remove every Project override.");

            RenderSettingsDraft user = service.CreateDraft(SettingsLayer::User);
            user.Patch[Id(RenderSettingIds::VSync)] = true;
            Require(service.ApplyDraft(user, true).bSuccess,
                "Could not prepare a User override for reset-all coverage.");
            RenderSettingsDraft session = service.CreateDraft(
                SettingsLayer::Session);
            session.Patch[Id(RenderSettingIds::PostProcessExposure)] = 2.0;
            Require(service.ApplyDraft(session, false).bSuccess,
                "Could not prepare a Session override for reset-all coverage.");

            snapshot = service.GetSnapshot();
            result = service.ResetToEngineDefault(snapshot.Revision,
                snapshot.Hash);
            Require(result.bSuccess && result.bSaved &&
                    service.GetLayerValues(SettingsLayer::Project).empty() &&
                    service.GetLayerValues(SettingsLayer::User).empty() &&
                    service.GetLayerValues(SettingsLayer::Session).empty() &&
                    service.GetSnapshot().Requested ==
                        MakeEmergencyFallbackRenderSettings() &&
                    Utils::LoadJsonFile(paths.Project).size() == 2 &&
                    Utils::LoadJsonFile(paths.User).size() == 2,
                "ResetToEngineDefault did not remove all mutable overrides.");
        }

        void TestFutureSchemaProtection()
        {
            TemporaryDirectory directory;
            const SettingsPaths paths = MakeSettingsPaths(directory);
            SaveDocument(paths.Engine, MakeEngineDefaultDocument());
            Json future = SerializeRenderSettingsLayerDocument({
                { RenderSettingIds::VSync, true },
            });
            future["SchemaVersion"] = RenderSettingsSchemaVersion + 1;
            SaveDocument(paths.Project, future);

            const RenderSettingsMigrationResult migration =
                MigrateRenderSettingsDocument(future);
            Require(!migration.bSuccess && migration.bFutureSchema &&
                    HasDiagnostic(migration.Diagnostics, "SchemaVersion",
                        "future_schema",
                        RenderSettingDiagnosticSeverity::Error),
                "A future schema was not rejected with a structured diagnostic.");

            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            RenderSettingsService& service = scope.Get();
            RenderSettingsDraft draft = service.CreateDraft(
                SettingsLayer::Project);
            draft.Patch[Id(RenderSettingIds::VSync)] = false;
            const RenderSettingsApplyResult result =
                service.ApplyDraft(draft, true);
            Require(!result.bSuccess && result.ErrorCode == "future_schema" &&
                    Utils::LoadJsonFile(paths.Project) == future,
                "An older service overwrote a future-schema Project document.");
        }

        void TestInvalidDiagnostics()
        {
            std::vector<RenderSettingDiagnostic> diagnostics;
            const RenderSettings normalized = ApplyRenderSettingsFlatValues(
                MakeEmergencyFallbackRenderSettings(),
                {
                    { RenderSettingIds::GiRenderScale, 2.0 },
                    { RenderSettingIds::VSync, "yes" },
                },
                diagnostics);
            const auto normalizedDiagnostic = std::ranges::find_if(
                diagnostics,
                [](const RenderSettingDiagnostic& diagnostic)
                {
                    return diagnostic.PropertyId ==
                            RenderSettingIds::GiRenderScale &&
                        diagnostic.Code == "normalized_range";
                });
            Require(NearlyEqual(normalized.GlobalIllumination.RenderScale,
                        1.0f) &&
                    !normalized.bVSync &&
                    normalizedDiagnostic != diagnostics.end() &&
                    normalizedDiagnostic->InputValue == 2.0 &&
                    normalizedDiagnostic->NormalizedValue == 1.0 &&
                    normalizedDiagnostic->Severity ==
                        RenderSettingDiagnosticSeverity::Warning &&
                    HasDiagnostic(diagnostics, RenderSettingIds::VSync,
                        "invalid_type",
                        RenderSettingDiagnosticSeverity::Error),
                "Invalid values did not report path, input, normalization, and severity.");

            TemporaryDirectory directory;
            const SettingsPaths paths = MakeSettingsPaths(directory);
            SaveDocument(paths.Engine, MakeEngineDefaultDocument());
            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            RenderSettingsService& service = scope.Get();
            const ResolvedRenderSettings before = service.GetSnapshot();
            RenderSettingsDraft invalid = service.CreateDraft(
                SettingsLayer::Session);
            invalid.Patch[Id(RenderSettingIds::VSync)] = "yes";
            const RenderSettingsApplyResult result =
                service.ApplyDraft(invalid, false);
            const ResolvedRenderSettings after = service.GetSnapshot();
            Require(!result.bSuccess &&
                    result.ErrorCode == "validation_failed" &&
                    HasDiagnostic(result.Diagnostics, RenderSettingIds::VSync,
                        "invalid_type",
                        RenderSettingDiagnosticSeverity::Error) &&
                    after.Revision == before.Revision &&
                    after.Hash == before.Hash &&
                    after.Requested == before.Requested,
                "An invalid Service draft mutated the committed snapshot.");
        }

        void TestRevisionAndHashConcurrency()
        {
            TemporaryDirectory directory;
            const SettingsPaths paths = MakeSettingsPaths(directory);
            SaveDocument(paths.Engine, MakeEngineDefaultDocument());
            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            RenderSettingsService& service = scope.Get();

            RenderSettingsDraft first = service.CreateDraft(
                SettingsLayer::Session);
            RenderSettingsDraft stale = first;
            first.Patch[Id(RenderSettingIds::VSync)] = true;
            stale.Patch[Id(RenderSettingIds::PostProcessExposure)] = 2.0;
            const RenderSettingsApplyResult committed =
                service.ApplyDraft(first, false);
            const RenderSettingsApplyResult staleResult =
                service.ApplyDraft(stale, false);
            Require(committed.bSuccess && !staleResult.bSuccess &&
                    staleResult.ErrorCode == "stale_revision" &&
                    NearlyEqual(service.GetSnapshot().Requested.PostProcess.Exposure,
                        MakeEmergencyFallbackRenderSettings()
                            .PostProcess.Exposure),
                "A stale revision overwrote a newer settings transaction.");

            RenderSettingsDraft badHash = service.CreateDraft(
                SettingsLayer::Session);
            badHash.BaseHash = "0000000000000000";
            badHash.Patch[Id(RenderSettingIds::VSync)] = false;
            const RenderSettingsApplyResult hashResult =
                service.ApplyDraft(badHash, false);
            Require(!hashResult.bSuccess &&
                    hashResult.ErrorCode == "stale_revision" &&
                    service.GetSnapshot().Requested.bVSync,
                "A stale hash bypassed optimistic concurrency protection.");
        }

        void TestTransactionalRollback()
        {
            {
                TemporaryDirectory directory;
                const SettingsPaths paths = MakeSettingsPaths(directory);
                SaveDocument(paths.Engine, MakeEngineDefaultDocument());
                const Json originalProject =
                    SerializeRenderSettingsLayerDocument({});
                SaveDocument(paths.Project, originalProject);

                ScopedRenderSettingsService scope(MakeServiceInit(paths));
                RenderSettingsService& service = scope.Get();
                std::vector<bool> callbackVSync;
                service.SetRuntimeApplyCallback(
                    [&](const ResolvedRenderSettings& settings,
                        const RenderSettingsChangePlan&)
                    {
                        callbackVSync.push_back(settings.Requested.bVSync);
                        if (callbackVSync.size() == 1)
                            throw std::runtime_error(
                                "Synthetic runtime apply failure.");
                    });

                const ResolvedRenderSettings before = service.GetSnapshot();
                RenderSettingsDraft draft = service.CreateDraft(
                    SettingsLayer::Project);
                draft.Patch[Id(RenderSettingIds::VSync)] = true;
                const RenderSettingsApplyResult result =
                    service.ApplyDraft(draft, true);
                const ResolvedRenderSettings after = service.GetSnapshot();
                Require(!result.bSuccess && result.bRolledBack &&
                        result.ErrorCode == "transaction_failed" &&
                        callbackVSync == std::vector<bool>{ true, false } &&
                        Utils::LoadJsonFile(paths.Project) == originalProject &&
                        after.Revision == before.Revision &&
                        after.Hash == before.Hash &&
                        after.Requested == before.Requested,
                    "Runtime apply failure did not roll back disk and runtime state.");
                service.SetRuntimeApplyCallback({});
            }

            {
                TemporaryDirectory directory;
                SettingsPaths paths = MakeSettingsPaths(directory);
                SaveDocument(paths.Engine, MakeEngineDefaultDocument());
                const std::filesystem::path blockedParent =
                    directory.GetPath() / "BlockedParent";
                SaveDocument(blockedParent, Json::object());
                paths.Project = blockedParent / "BaseEngine.json";

                ScopedRenderSettingsService scope(MakeServiceInit(paths));
                RenderSettingsService& service = scope.Get();
                std::uint32_t callbackCount = 0;
                service.SetRuntimeApplyCallback(
                    [&](const ResolvedRenderSettings&,
                        const RenderSettingsChangePlan&)
                    {
                        ++callbackCount;
                    });
                const ResolvedRenderSettings before = service.GetSnapshot();
                RenderSettingsDraft draft = service.CreateDraft(
                    SettingsLayer::Project);
                draft.Patch[Id(RenderSettingIds::VSync)] = true;
                const RenderSettingsApplyResult result =
                    service.ApplyDraft(draft, true);
                const ResolvedRenderSettings after = service.GetSnapshot();
                Require(!result.bSuccess &&
                        result.ErrorCode == "transaction_failed",
                    "A synthetic save failure did not fail the transaction.");
                Require(result.bRolledBack,
                    "A synthetic save failure was not reported as rolled back.");
                Require(callbackCount == 0,
                    "Runtime apply ran after persistence had already failed.");
                Require(!std::filesystem::exists(paths.Project),
                    "A failed save left a Project settings file behind.");
                Require(after.Revision == before.Revision &&
                        after.Hash == before.Hash &&
                        after.Requested == before.Requested,
                    "A save failure changed the committed settings snapshot.");
                service.SetRuntimeApplyCallback({});
            }
        }

        void TestPersistentSaveCreatesMissingParent()
        {
            TemporaryDirectory directory;
            SettingsPaths paths = MakeSettingsPaths(directory);
            SaveDocument(paths.Engine, MakeEngineDefaultDocument());
            paths.User = directory.GetPath() / "Saved" / "Config" /
                "UserEngine.json";

            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            RenderSettingsService& service = scope.Get();
            RenderSettingsDraft draft = service.CreateDraft(SettingsLayer::User);
            draft.Patch[Id(RenderSettingIds::Backend)] = "Vulkan";
            const RenderSettingsApplyResult result =
                service.ApplyDraft(draft, true);

            Require(result.bSuccess && result.bSaved &&
                    std::filesystem::is_regular_file(paths.User) &&
                    service.GetSnapshot().Requested.Backend ==
                        RenderBackend::Vulkan,
                "A first-run User settings save did not create Saved/Config.");
        }

        void TestLastKnownGoodReload()
        {
            TemporaryDirectory directory;
            const SettingsPaths paths = MakeSettingsPaths(directory);
            SaveDocument(paths.Engine, MakeEngineDefaultDocument());
            SaveDocument(paths.Project, SerializeRenderSettingsLayerDocument({
                { RenderSettingIds::VSync, true },
            }));

            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            RenderSettingsService& service = scope.Get();
            const ResolvedRenderSettings knownGood = service.GetSnapshot();
            SaveDocument(paths.Project, SerializeRenderSettingsLayerDocument({
                { RenderSettingIds::VSync, "invalid" },
            }));
            Require(!service.ReloadExternalLayer(SettingsLayer::Project),
                "An invalid external edit was accepted.");
            const ResolvedRenderSettings retained = service.GetSnapshot();
            Require(retained.Requested.bVSync &&
                    retained.Revision == knownGood.Revision &&
                    retained.Hash == knownGood.Hash &&
                    HasDiagnostic(retained.Diagnostics,
                        RenderSettingIds::VSync,
                        "invalid_type",
                        RenderSettingDiagnosticSeverity::Error),
                "Invalid hot reload did not retain and diagnose last-known-good state.");

            SaveDocument(paths.Project, SerializeRenderSettingsLayerDocument({
                { RenderSettingIds::VSync, false },
            }));
            Require(service.ReloadExternalLayer(SettingsLayer::Project),
                "A valid external edit was rejected after an invalid one.");
            const ResolvedRenderSettings reloaded = service.GetSnapshot();
            Require(!reloaded.Requested.bVSync &&
                    reloaded.Revision == retained.Revision + 1,
                "A recovered external edit did not replace last-known-good state.");
        }

        void TestRequestedEffectiveRecovery()
        {
            TemporaryDirectory directory;
            const SettingsPaths paths = MakeSettingsPaths(directory);
            SaveDocument(paths.Engine, MakeEngineDefaultDocument());
            SaveDocument(paths.Project, SerializeRenderSettingsLayerDocument({
                { RenderSettingIds::GiTechnique, "HardwareRTGI" },
                { RenderSettingIds::GiUnsupportedFallback, "ScreenSpaceGI" },
                { RenderSettingIds::AntiAliasingMode, "SMAA" },
            }));

            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            RenderSettingsService& service = scope.Get();
            RenderSettingsCapabilities unavailable;
            unavailable.bScreenSpaceGiAvailable = true;
            unavailable.AntiAliasing.Fxaa = {
                true, "Synthetic FXAA support" };
            unavailable.AntiAliasing.Smaa = {
                false, "Synthetic SMAA shader failure" };
            service.SetCapabilities(unavailable);

            ResolvedRenderSettings snapshot = service.GetSnapshot();
            Require(snapshot.Requested.GlobalIllumination.Technique ==
                        GlobalIlluminationTechniqueKind::HardwareRayTracing &&
                    snapshot.Effective.GlobalIllumination.Technique ==
                        GlobalIlluminationTechniqueKind::ScreenSpace &&
                    snapshot.Requested.AntiAliasing.Mode ==
                        AntiAliasingMode::Smaa &&
                    snapshot.Effective.AntiAliasing.Mode ==
                        AntiAliasingMode::None &&
                    service.BuildRequestedRuntimeGlobalIllumination().Technique ==
                        GlobalIlluminationTechniqueKind::HardwareRayTracing &&
                    service.BuildRequestedRuntimeAntiAliasing().Mode ==
                        AntiAliasingMode::Smaa &&
                    HasDiagnostic(snapshot.Diagnostics,
                        RenderSettingIds::GiTechnique,
                        "capability_fallback",
                        RenderSettingDiagnosticSeverity::Warning),
                "Unsupported GI/AA did not preserve requested while resolving effective fallback.");

            RenderSettingsDraft persisted = service.CreateDraft(
                SettingsLayer::Project);
            persisted.Patch[Id(RenderSettingIds::GiMaxBounces)] = 5;
            Require(service.ApplyDraft(persisted, true).bSuccess,
                "Could not persist a requested GI setting under fallback.");
            const Json projectDocument = Utils::LoadJsonFile(paths.Project);
            const Json* persistedTechnique = FindDottedPath(
                projectDocument, RenderSettingIds::GiTechnique);
            Require(persistedTechnique && *persistedTechnique == "HardwareRTGI",
                "Capability fallback was written over the requested GI technique.");

            RenderSettingsCapabilities available = unavailable;
            available.Render.RayTracingAccelStruct.bSupported = true;
            available.Render.RayTracingAccelStruct.bEnabled = true;
            available.Render.RayTracingPipeline.bSupported = true;
            available.Render.RayTracingPipeline.bEnabled = true;
            service.SetCapabilities(available);
            snapshot = service.GetSnapshot();
            Require(snapshot.Requested.GlobalIllumination.Technique ==
                        GlobalIlluminationTechniqueKind::HardwareRayTracing &&
                    snapshot.Effective.GlobalIllumination.Technique ==
                        GlobalIlluminationTechniqueKind::HardwareRayTracing,
                "Hardware RTGI request did not recover when capabilities became available.");
        }

        void TestGlobalIlluminationPresetDetection()
        {
            for (GlobalIlluminationQualityPreset preset : {
                GlobalIlluminationQualityPreset::Low,
                GlobalIlluminationQualityPreset::Balanced,
                GlobalIlluminationQualityPreset::High,
            })
            {
                std::vector<RenderSettingDiagnostic> diagnostics;
                const RenderSettings settings = ApplyRenderSettingsFlatValues(
                    MakeEmergencyFallbackRenderSettings(),
                    GetGlobalIlluminationQualityPresetPatch(preset),
                    diagnostics);
                Require(!HasDiagnosticErrors(diagnostics) &&
                        DetectGlobalIlluminationQualityPreset(settings) == preset,
                    "A GI quality patch was not detected as its preset.");
            }

            std::vector<RenderSettingDiagnostic> diagnostics;
            RenderSettings custom = ApplyRenderSettingsFlatValues(
                MakeEmergencyFallbackRenderSettings(),
                GetGlobalIlluminationQualityPresetPatch(
                    GlobalIlluminationQualityPreset::Balanced),
                diagnostics);
            custom.GlobalIllumination.MaxBounces = 3;
            Require(DetectGlobalIlluminationQualityPreset(custom) ==
                        GlobalIlluminationQualityPreset::Custom &&
                    GetGlobalIlluminationQualityPresetPatch(
                        GlobalIlluminationQualityPreset::Custom).empty(),
                "A partial preset mismatch remained authoritative instead of becoming Custom.");
        }

        void TestToolSettingsDoNotPersistToProject()
        {
            TemporaryDirectory directory;
            const SettingsPaths paths = MakeSettingsPaths(directory);
            SaveDocument(paths.Engine, MakeEngineDefaultDocument());
            SaveDocument(paths.Project, {
                { "Renderer", {
                    { "VSync", false },
                    { "AntiAliasing", {
                        { "CaptureSupersampleScale", 4 },
                    } },
                    { "GlobalIllumination", {
                        { "DebugView", static_cast<std::uint32_t>(
                            GlobalIlluminationDebugView::Coverage) },
                        { "DirectOnly", true },
                    } },
                } },
            });

            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            RenderSettingsService& service = scope.Get();
            ResolvedRenderSettings snapshot = service.GetSnapshot();
            Require(snapshot.Debug.GiDebugView ==
                        GlobalIlluminationDebugView::Coverage &&
                    snapshot.Debug.bGiDirectOnly &&
                    snapshot.Capture.SupersampleScale == 4 &&
                    snapshot.Requested.GlobalIllumination.DebugView ==
                        GlobalIlluminationDebugView::Composite &&
                    !snapshot.Requested.GlobalIllumination.bDirectOnly &&
                    snapshot.Requested.AntiAliasing.CaptureSupersampleScale == 1 &&
                    snapshot.Sources.at(Id(RenderSettingIds::GiDebugView)) ==
                        SettingsLayer::Session &&
                    snapshot.Sources.at(Id(
                        RenderSettingIds::CaptureSupersampleScale)) ==
                        SettingsLayer::Session &&
                    service.BuildRequestedRuntimeGlobalIllumination().DebugView ==
                        GlobalIlluminationDebugView::Coverage &&
                    service.BuildRequestedRuntimeAntiAliasing()
                        .CaptureSupersampleScale == 4,
                "Legacy tool settings were not isolated in Session runtime state.");

            for (std::string_view id : {
                RenderSettingIds::GiDebugView,
                RenderSettingIds::GiDirectOnly,
                RenderSettingIds::PbrDebugMode,
                RenderSettingIds::RenderGraphValidation,
                RenderSettingIds::ShadowCascadeDebug,
                RenderSettingIds::CaptureSupersampleScale,
            })
            {
                const RenderSettingDescriptor* descriptor =
                    FindRenderSettingDescriptor(id);
                Require(descriptor && !descriptor->bPersistedRenderSetting &&
                        std::ranges::find(descriptor->WritableLayers,
                            SettingsLayer::Project) ==
                            descriptor->WritableLayers.end(),
                    "A debug/capture descriptor permits Project persistence.");
            }

            RenderSettingsDraft project = service.CreateDraft(
                SettingsLayer::Project);
            project.Patch[Id(RenderSettingIds::VSync)] = true;
            Require(service.ApplyDraft(project, true).bSuccess,
                "Could not rewrite a migrated sparse Project document.");
            const Json savedProject = Utils::LoadJsonFile(paths.Project);
            Require(FindDottedPath(savedProject,
                        RenderSettingIds::GiDebugView) == nullptr &&
                    FindDottedPath(savedProject,
                        RenderSettingIds::GiDirectOnly) == nullptr &&
                    FindDottedPath(savedProject,
                        RenderSettingIds::CaptureSupersampleScale) == nullptr &&
                    !service.GetLayerValues(SettingsLayer::Project).contains(
                        Id(RenderSettingIds::GiDebugView)) &&
                    !service.GetLayerValues(SettingsLayer::Project).contains(
                        Id(RenderSettingIds::CaptureSupersampleScale)),
                "Migrated diagnostic/capture values leaked back into Project settings.");

            RenderSettingsDraft forbidden = service.CreateDraft(
                SettingsLayer::Project);
            forbidden.Patch[Id(RenderSettingIds::GiDirectOnly)] = false;
            const RenderSettingsApplyResult forbiddenResult =
                service.ApplyDraft(forbidden, true);
            Require(!forbiddenResult.bSuccess &&
                    forbiddenResult.ErrorCode == "layer_not_supported",
                "Project accepted a diagnostic-only setting.");
        }

        [[nodiscard]] std::filesystem::path FindRepositoryRoot()
        {
            std::filesystem::path current = std::filesystem::current_path();
            for (;;)
            {
                if (std::filesystem::is_regular_file(
                        current / "Project/Project.heproject") &&
                    std::filesystem::is_regular_file(
                        current / "Engine/Content/Config/DefaultEngine.json"))
                {
                    return current;
                }
                const std::filesystem::path parent = current.parent_path();
                if (parent == current)
                    break;
                current = parent;
            }
            throw std::runtime_error(
                "Could not locate the HEngine repository root.");
        }

        void TestEngineDefaultMatchesEmergencyFallback()
        {
            const std::filesystem::path defaultPath = FindRepositoryRoot() /
                "Engine/Content/Config/DefaultEngine.json";
            const RenderSettingsMigrationResult migration =
                MigrateRenderSettingsDocument(
                    Utils::LoadJsonFile(defaultPath));
            Require(migration.bSuccess && !migration.bFutureSchema &&
                    migration.SourceVersion == RenderSettingsSchemaVersion &&
                    migration.DebugValues.empty() &&
                    migration.CaptureValues.empty(),
                "DefaultEngine.json does not use the current product schema.");

            std::vector<RenderSettingDiagnostic> diagnostics;
            const RenderSettings fromFile = ApplyRenderSettingsFlatValues(
                MakeEmergencyFallbackRenderSettings(),
                migration.RenderValues,
                diagnostics);
            const Json expected = RenderSettingsToFlatValues(
                MakeEmergencyFallbackRenderSettings());
            const Json actual = RenderSettingsToFlatValues(fromFile);
            Require(!HasDiagnosticErrors(diagnostics) &&
                    migration.RenderValues.size() == expected.size() &&
                    actual == expected,
                "DefaultEngine.json drifted from MakeEmergencyFallbackRenderSettings.");
        }

        void TestDualBackendAvailabilityAndRestartContract()
        {
            const RenderSettingDescriptor* descriptor =
                FindRenderSettingDescriptor(RenderSettingIds::Backend);
            Require(descriptor != nullptr &&
                    descriptor->Impact == ApplyImpact::ProcessRestart &&
                    descriptor->bRestartRequired &&
                    std::ranges::find(descriptor->WritableLayers,
                        SettingsLayer::Project) != descriptor->WritableLayers.end() &&
                    std::ranges::find(descriptor->WritableLayers,
                        SettingsLayer::User) != descriptor->WritableLayers.end() &&
                    std::ranges::find(descriptor->WritableLayers,
                        SettingsLayer::Session) == descriptor->WritableLayers.end(),
                "The renderer backend setting is not a persisted process-restart choice.");

            RenderSettings dx12 = MakeEmergencyFallbackRenderSettings();
            RenderSettings vulkan = dx12;
            vulkan.Backend = RenderBackend::Vulkan;
            RenderSettingsCapabilities capabilities;
            capabilities.BackendAvailability[RenderBackend::DX12] = {
                true, "Synthetic DX12 availability" };
            capabilities.BackendAvailability[RenderBackend::Vulkan] = {
                false, "Synthetic Vulkan driver failure" };

            const ResolvedRenderSettings fallback = ResolveRenderSettings(
                vulkan, {}, {}, capabilities);
            Require(fallback.Requested.Backend == RenderBackend::Vulkan &&
                    fallback.Effective.Backend == RenderBackend::DX12 &&
                    fallback.Availability.at(std::string(
                        RenderSettingIds::Backend)).Reason.find(
                            "Synthetic Vulkan driver failure") != std::string::npos &&
                    HasDiagnostic(fallback.Diagnostics,
                        RenderSettingIds::Backend,
                        "capability_fallback",
                        RenderSettingDiagnosticSeverity::Warning),
                "A Vulkan runtime failure did not preserve Requested while selecting DX12 Effective.");

            capabilities.BackendAvailability[RenderBackend::Vulkan] = {
                true, "Synthetic Vulkan availability" };
            const ResolvedRenderSettings before = ResolveRenderSettings(
                dx12, {}, {}, capabilities);
            const ResolvedRenderSettings after = ResolveRenderSettings(
                vulkan, {}, {}, capabilities);
            const RenderSettingsChangePlan plan =
                BuildRenderSettingsChangePlan(before, after);
            Require(after.Effective.Backend == RenderBackend::Vulkan &&
                    plan.bProcessRestartRequired &&
                    !plan.bBuildReconfigureRequired,
                "An available Vulkan choice did not resolve directly as a process restart.");
        }
    }

    void RenderSettingsContract()
    {
        TestLegacyMigration();
        TestLayerMergeAndSources();
        TestSparseResetAndEngineInheritance();
        TestFutureSchemaProtection();
        TestInvalidDiagnostics();
        TestRevisionAndHashConcurrency();
        TestPersistentSaveCreatesMissingParent();
        TestTransactionalRollback();
        TestLastKnownGoodReload();
        TestRequestedEffectiveRecovery();
        TestGlobalIlluminationPresetDetection();
        TestToolSettingsDoNotPersistToProject();
        TestEngineDefaultMatchesEmergencyFallback();
        TestDualBackendAvailabilityAndRestartContract();
    }
}
