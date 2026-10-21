#include "Hepch.h"

#include "Runtime/UnitTest/RenderSettingsTests.h"

#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/EcsFramework/Component/Light/EnvironmentLightComponent.h"
#include "Runtime/Graphics/Settings/RenderSettings.h"
#include "Runtime/Graphics/Settings/PipelineProfile.h"
#include "Runtime/Graphics/Renderer/RenderFeature.h"
#include "Runtime/Graphics/Renderer/RenderResolution.h"
#include "Runtime/Graphics/Renderer/RejectedViewConfigurationInputs.h"
#include "Runtime/Graphics/Renderer/RenderPipelineRegistry.h"
#include "_generated/serializer/EnvironmentLightComponent.gen.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <limits>
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
                { RenderSettingIds::PostProcessExposure, -0.32192809488736235 },
                { RenderSettingIds::LocalExposureShadowStrength, 0.3 },
                { RenderSettingIds::LocalExposureHighlightStrength, 0.2 },
                { RenderSettingIds::LocalExposureMaxEV, 1.5 },
                { RenderSettingIds::SkinSubsurfaceEnabled, true },
                { RenderSettingIds::SkinSubsurfaceRadiusMeters, 0.004 },
                { RenderSettingIds::SkinSubsurfaceQuality, 2 },
                { RenderSettingIds::RenderHeight, 1080 },
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
            Require(snapshot.Requested.PostProcess.RenderHeight == 1080 &&
                    snapshot.Effective.PostProcess.RenderHeight == 1080,
                "Internal resolution cap did not survive user settings persistence.");
            Require(NearlyEqual(snapshot.Requested.PostProcess.LocalExposureShadowStrength, 0.3f) &&
                    NearlyEqual(snapshot.Effective.PostProcess.LocalExposureHighlightStrength, 0.2f) &&
                    NearlyEqual(snapshot.Effective.PostProcess.LocalExposureMaxEV, 1.5f),
                "Local exposure controls did not survive user-layer serialization and runtime resolution.");
            Require(snapshot.Effective.PostProcess.SkinSubsurfaceEnabled &&
                    NearlyEqual(snapshot.Effective.PostProcess.SkinSubsurfaceRadiusMeters, 0.004f) &&
                    snapshot.Effective.PostProcess.SkinSubsurfaceQuality == 2,
                "Skin diffusion controls did not survive user-layer serialization and runtime resolution.");
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
                    { { RenderSettingIds::PostProcessExposure, -1.0 } },
                    { { RenderSettingIds::PostProcessExposure, -1.584962500721156 } });
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
                draft.Patch[Id(RenderSettingIds::CloudQuality)] = "High";
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

        void TestCountedIndirectCapabilityFallback()
        {
            RenderSettings requested = MakeEmergencyFallbackRenderSettings();
            requested.bGpuBatchingEnabled = true;
            requested.bGpuShadowBatchingEnabled = true;
            requested.bGpuOcclusionEnabled = true;
            requested.bGpuTightFrustumEnabled = true;
            RenderSettingsCapabilities capabilities;
            capabilities.Render.bDrawIndirectFirstInstance = true;
            capabilities.Render.bMultiDrawIndirect = true;
            // MoltenVK may support GPU indirect draws without indirect-count.
            capabilities.Render.bDrawIndirectCount = false;
            const auto fallback = ResolveRenderSettings(requested, {}, {}, capabilities);
            Require(fallback.Requested.bGpuBatchingEnabled && fallback.Requested.bGpuShadowBatchingEnabled &&
                    !fallback.Effective.bGpuBatchingEnabled && !fallback.Effective.bGpuShadowBatchingEnabled &&
                    fallback.Effective.bGpuOcclusionEnabled && fallback.Effective.bGpuTightFrustumEnabled,
                "Indirect-count fallback changed the request or disabled GPU visibility instead of only compaction.");
            for (const auto id : { RenderSettingIds::GpuBatchingEnabled, RenderSettingIds::GpuShadowBatchingEnabled })
                Require(!fallback.Availability.at(Id(id)).bAvailable &&
                        HasDiagnostic(fallback.Diagnostics, id, "capability_fallback", RenderSettingDiagnosticSeverity::Warning),
                    "Unavailable counted draw groups were not exposed and diagnosed in settings.");
            capabilities.Render.bDrawIndirectCount = true;
            const auto recovered = ResolveRenderSettings(requested, {}, {}, capabilities);
            Require(recovered.Effective.bGpuBatchingEnabled && recovered.Effective.bGpuShadowBatchingEnabled &&
                    BuildRenderSettingsChangePlan(fallback, recovered).bRenderGraphRebuildRequired,
                "Counted indirect support did not restore requested groups through a graph rebuild.");
            capabilities.Render.bMultiDrawIndirect = false;
            const auto singleDraw = ResolveRenderSettings(requested, {}, {}, capabilities);
            Require(!singleDraw.Effective.bGpuBatchingEnabled && !singleDraw.Effective.bGpuShadowBatchingEnabled,
                "Counted draw groups ignored missing multi-draw support.");
        }

        void TestGlobalIlluminationPresetDetection()
        {
            RenderSettings previous = MakeEmergencyFallbackRenderSettings();
            previous.GlobalIllumination.RenderScale = 1.0f;
            previous.GlobalIllumination.MaxBounces = 4;
            previous.GlobalIllumination.AtrousIterations = 5;
            previous.GlobalIllumination.bSvgfOptimizedKernels = false;
            previous.GlobalIllumination.bBilateralUpsample = false;
            previous.GlobalIllumination.ScreenSpaceRayCount = 8;
            previous.GlobalIllumination.ScreenSpaceStepCount = 128;
            previous.GlobalIllumination.ScreenSpaceHiZStartMip = 0;
            previous.GlobalIllumination.ScreenSpaceBinaryRefinementSteps = 16;
            previous.GlobalIllumination.ScreenSpaceMaximumRayDistance = 0.1f;
            previous.GlobalIllumination.ScreenSpaceThickness = 1.0f;
            for (GlobalIlluminationQualityPreset preset : {
                GlobalIlluminationQualityPreset::Low,
                GlobalIlluminationQualityPreset::Balanced,
                GlobalIlluminationQualityPreset::High,
            })
            {
                std::vector<RenderSettingDiagnostic> diagnostics;
                const RenderSettings settings = ApplyRenderSettingsFlatValues(
                    previous,
                    GetGlobalIlluminationQualityPresetPatch(
                        preset, previous.GlobalIllumination.Technique),
                    diagnostics);
                Require(!HasDiagnosticErrors(diagnostics) &&
                        DetectGlobalIlluminationQualityPreset(settings) == preset,
                    "A GI quality patch was not detected as its preset.");
                const auto& gi = settings.GlobalIllumination;
                Require(gi.bSvgfOptimizedKernels && gi.bBilateralUpsample &&
                        gi.ScreenSpaceHiZStartMip > 0 &&
                        gi.ScreenSpaceBinaryRefinementSteps <= 6 &&
                        gi.ScreenSpaceMaximumRayDistance >= 5.0f &&
                        gi.ScreenSpaceThickness <= 0.1f,
                    "A GI preset retained incompatible or expensive custom settings.");
                if (preset != GlobalIlluminationQualityPreset::High)
                {
                    Require(gi.RenderScale <= 0.5f && gi.MaxBounces == 1,
                        "An affordable GI preset retained full-resolution or multi-bounce tracing.");
                }
                if (preset == GlobalIlluminationQualityPreset::Low)
                {
                    const uint32_t expectedRays = gi.Technique ==
                        GlobalIlluminationTechniqueKind::ScreenSpace ? 2u : 1u;
                    Require(gi.ScreenSpaceRayCount == expectedRays &&
                            gi.ScreenSpaceStepCount <= 64 &&
                            gi.AtrousIterations <= 2,
                        "The lowest GI preset exceeded its tracing or denoising budget.");
                }
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

        void TestGlobalIlluminationPresetTechniqueIsolation()
        {
            const auto high = GlobalIlluminationQualityPreset::High;
            const auto low = GlobalIlluminationQualityPreset::Low;
            Require(GetGlobalIlluminationQualityPresetPatch(low).at(
                        Id(RenderSettingIds::GiSsgiRayCount)) == 1,
                "The unspecified GI Low preset lost its legacy one-ray budget.");
            const Json legacy = GetGlobalIlluminationQualityPresetPatch(high);
            Require(legacy.at(Id(RenderSettingIds::GiRenderScale)) == 1.0 &&
                    legacy.at(Id(RenderSettingIds::GiMaxBounces)) == 4 &&
                    legacy.at(Id(RenderSettingIds::GiAtrousIterations)) == 4 &&
                    legacy.at(Id(RenderSettingIds::GiSsgiRayCount)) == 8 &&
                    legacy.at(Id(RenderSettingIds::GiSsgiMaximumRayDistance)) == 20.0,
                "The unspecified GI High preset lost its legacy budget.");
            for (GlobalIlluminationTechniqueKind technique : {
                GlobalIlluminationTechniqueKind::ScreenSpace,
                GlobalIlluminationTechniqueKind::HardwareRayTracing,
                GlobalIlluminationTechniqueKind::ExternalViewportRenderer,
            })
            {
                RenderSettings previous = MakeEmergencyFallbackRenderSettings();
                previous.GlobalIllumination.Technique = technique;
                for (GlobalIlluminationQualityPreset preset : {
                    GlobalIlluminationQualityPreset::Low,
                    GlobalIlluminationQualityPreset::Balanced,
                    high,
                })
                {
                    const Json patch = GetGlobalIlluminationQualityPresetPatch(
                        preset, technique);
                    std::vector<RenderSettingDiagnostic> diagnostics;
                    RenderSettings settings = ApplyRenderSettingsFlatValues(
                        previous, patch, diagnostics);
                    Require(!HasDiagnosticErrors(diagnostics) &&
                            settings.GlobalIllumination.Technique == technique &&
                            DetectGlobalIlluminationQualityPreset(settings) == preset,
                        "A technique-specific GI preset changed technique or failed detection.");
                    if (preset == GlobalIlluminationQualityPreset::Balanced ||
                        technique != GlobalIlluminationTechniqueKind::ScreenSpace)
                    {
                        Require(patch == GetGlobalIlluminationQualityPresetPatch(preset),
                            "An SSGI preset change altered another technique or quality budget.");
                        continue;
                    }
                    if (preset == low)
                    {
                        Json expected = GetGlobalIlluminationQualityPresetPatch(low);
                        expected[Id(RenderSettingIds::GiSsgiRayCount)] = 2;
                        Require(patch == expected,
                            "SSGI Low changed more than its minimum two-ray sampling budget.");
                        settings.GlobalIllumination.ScreenSpaceRayCount = 1;
                        Require(DetectGlobalIlluminationQualityPreset(settings) ==
                                GlobalIlluminationQualityPreset::Custom,
                            "The optional one-ray SSGI configuration was still labelled Low.");
                        continue;
                    }
                    const auto& gi = settings.GlobalIllumination;
                    Require(gi.RenderScale == 0.5f && gi.MaxBounces == 1 &&
                            gi.ScreenSpaceRayCount == 4 && gi.ScreenSpaceStepCount == 128 &&
                            gi.AtrousIterations == 3 &&
                            gi.ScreenSpaceMaximumRayDistance == 10.0f &&
                            gi.ScreenSpaceThickness == 0.1f &&
                            gi.ScreenSpaceTemporalWeight == 0.97f,
                        "SSGI High does not match the bounded single-bounce tracing budget.");
                    settings.GlobalIllumination.Technique =
                        GlobalIlluminationTechniqueKind::HardwareRayTracing;
                    Require(DetectGlobalIlluminationQualityPreset(settings) ==
                            GlobalIlluminationQualityPreset::Custom,
                        "SSGI High was incorrectly detected as Hardware RTGI High.");
                }
            }
        }

        void TestGlobalIlluminationDebugDisablesAntiAliasing()
        {
            RenderSettingsCapabilities capabilities;
            capabilities.bScreenSpaceGiAvailable = true;
            capabilities.AntiAliasing.Fxaa = { true, "Synthetic FXAA support" };
            capabilities.AntiAliasing.Smaa = { true, "Synthetic SMAA support" };
            capabilities.AntiAliasing.Taa = { true, "Synthetic TAA support" };
            capabilities.AntiAliasing.Msaa4x = { true, "Synthetic MSAA support" };
            for (AntiAliasingMode mode : { AntiAliasingMode::Fxaa,
                AntiAliasingMode::Smaa, AntiAliasingMode::Taa, AntiAliasingMode::Msaa })
            {
                RenderSettings requested = MakeEmergencyFallbackRenderSettings();
                requested.GlobalIllumination.Technique =
                    GlobalIlluminationTechniqueKind::ScreenSpace;
                requested.Path = RenderingPath::Forward;
                requested.PostProcess.SkinSubsurfaceEnabled = true;
                requested.bGpuOcclusionEnabled = false;
                requested.Reflections.bScreenSpaceEnabled = false;
                requested.Reflections.bLocalProbesEnabled = false;
                requested.AntiAliasing.Mode = mode;
                requested.AntiAliasing.MsaaSamples = 4;
                RenderDebugSettings debug;
                debug.GiDebugView = GlobalIlluminationDebugView::RawIndirectDiffuse;
                const auto diagnostic = ResolveRenderSettings(
                    requested, debug, {}, capabilities);
                Require(diagnostic.Requested.AntiAliasing.Mode == mode &&
                        diagnostic.Effective.AntiAliasing.Mode == AntiAliasingMode::None,
                    "GI diagnostic AA resolution changed the request or retained filtering.");
                debug.GiDebugView = GlobalIlluminationDebugView::Composite;
                const auto composite = ResolveRenderSettings(
                    requested, debug, {}, capabilities);
                Require(composite.Requested.PostProcess.SkinSubsurfaceEnabled &&
                        !composite.Effective.PostProcess.SkinSubsurfaceEnabled &&
                        !composite.Availability.at(Id(RenderSettingIds::SkinSubsurfaceEnabled)).bAvailable,
                    "Forward skin diffusion was silently reported as active or lost the user request.");
                Require(composite.Effective.AntiAliasing.Mode == mode,
                    "Composite did not restore the requested available anti-aliasing mode.");
            }
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
                        current / "Engine/Templates/Blank/Project.heproject") &&
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
            Require(DetectGlobalIlluminationQualityPreset(fromFile) ==
                    GlobalIlluminationQualityPreset::Low,
                "Enabling GI with shipped defaults did not select the affordable Low budget.");
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
            const RenderBackend compiledFallback = IsRenderBackendCompiled(RenderBackend::DX12)
                ? RenderBackend::DX12 : RenderBackend::Vulkan;
            Require(fallback.Requested.Backend == RenderBackend::Vulkan &&
                    fallback.Effective.Backend == compiledFallback &&
                    fallback.Availability.at(std::string(
                        RenderSettingIds::Backend)).Reason.find(
                            "Synthetic Vulkan driver failure") != std::string::npos &&
                    HasDiagnostic(fallback.Diagnostics,
                        RenderSettingIds::Backend,
                        "capability_fallback",
                        RenderSettingDiagnosticSeverity::Warning),
                "A Vulkan runtime failure did not preserve Requested while selecting a compiled Effective backend.");

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

    void TestViewSettingsPersistenceContract()
    {
        TemporaryDirectory directory;
        const SettingsPaths paths = MakeSettingsPaths(directory);
        SaveDocument(paths.Engine, MakeEngineDefaultDocument());
        {
            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            auto& service = scope.Get();
            auto draft = service.CreateDraft(SettingsLayer::Project);
            draft.Patch = { { RenderSettingIds::ViewOverrides, { { "main", { { RenderSettingIds::AoEnabled, false } } } } } };
            Require(service.ApplyDraft(draft, true).bSuccess, "Could not persist project view overrides.");
            draft = service.CreateDraft(SettingsLayer::User);
            draft.Patch = { { RenderSettingIds::ViewOverrides, { { "main", { { RenderSettingIds::Path, "Forward" } } } } } };
            Require(service.ApplyDraft(draft, true).bSuccess, "Could not persist user view overrides.");
        }
        {
            ScopedRenderSettingsService scope(MakeServiceInit(paths));
            auto& service = scope.Get();
            auto snapshot = service.GetSnapshot();
            Require(snapshot.Requested.ViewOverrides["main"].size() == 2, "Reload lost per-view layer inheritance.");
            auto draft = service.CreateDraft(SettingsLayer::User);
            draft.Patch = { { RenderSettingIds::ViewOverrides, Json::object() } };
            Require(service.ApplyDraft(draft, true).bSuccess, "Could not restore per-view inheritance.");
            snapshot = service.GetSnapshot();
            Require(snapshot.Requested.ViewOverrides["main"].size() == 1 &&
                snapshot.Requested.ViewOverrides["main"].contains(RenderSettingIds::AoEnabled),
                "Restoring user view inheritance erased the project override.");
        }
    }

    void TestFeatureGraphContracts()
    {
        RenderGraph graph;
        RenderGraphTextureDesc desc{ .DebugName = "Feature input", .Width = 8, .Height = 8,
            .Format = nvrhi::Format::RGBA16_FLOAT };
        auto color = graph.CreateTexture(desc, RenderGraphResourceLifetime::Transient, true);
        graph.AddPass<TextureHandle>("Preserve color",
            [color](RenderGraphBuilder& builder, TextureHandle& data) { data = builder.ColorAttachment(color, {}); },
            [](RenderGraphContext&, const TextureHandle&) {});
        bool rejected = false;
        try { ValidateRenderFeatureColorAccess(graph, 0, color, color, RenderFeatureAccess::Read); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "Read-only Feature was allowed to modify its color input.");
        ValidateRenderFeatureColorAccess(graph, 0, color, color, RenderFeatureAccess::PreserveWrite);
        rejected = false;
        try { ValidateRenderFeatureColorAccess(graph, 0, color, color, RenderFeatureAccess::FullOverwrite); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "Full-overwrite Feature accepted a preserving attachment declaration.");

        RenderGraph samples;
        auto msaaDesc = desc;
        msaaDesc.SampleCount = 4;
        auto msaaColor = samples.CreateTexture(msaaDesc, RenderGraphResourceLifetime::Transient, true);
        auto depthDesc = desc;
        depthDesc.Format = nvrhi::Format::D32;
        const auto depth = samples.CreateTexture(depthDesc, RenderGraphResourceLifetime::Transient, true);
        RenderFeatureBuildContext sampleContext{ samples, msaaColor, depth };
        RenderFeatureDescriptor sampleFeature{ .Type = "test.samples", .RequiredInputs =
            uint32_t(RenderFeatureInput::Depth) | uint32_t(RenderFeatureInput::HdrColor), .bMsaaSupported = true,
            .bResolvedDepthSupported = true };
        rejected = false;
        try { ValidateRenderFeatureInputs(sampleFeature, sampleContext); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "HDR Feature accepted unresolved color/depth inputs.");
        sampleContext.Stage = RenderFeatureStage::TemporalComposite;
        ValidateRenderFeatureInputs(sampleFeature, sampleContext);
        Require(sampleContext.ColorSampleCount == 4 && sampleContext.DepthSampleCount == 1,
            "Feature input metadata hid mixed sample counts.");
        sampleFeature.bResolvedDepthSupported = false;
        rejected = false;
        try { ValidateRenderFeatureInputs(sampleFeature, sampleContext); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "Feature accepted mixed samples without declaring support.");

        struct HistoryCopyFeature final : RenderFeature
        {
            void Build(RenderFeatureBuildContext& context) override
            {
                const auto original = context.Color();
                const auto textureDesc = context.Graph.GetTextureDesc(original);
                const auto previous = context.History->ReadColor(context.Graph, "color", textureDesc);
                const auto output = context.Graph.CreateTexture(textureDesc);
                context.Graph.AddPass<std::pair<TextureHandle, TextureHandle>>("Previous frame color",
                    [previous, output](RenderGraphBuilder& builder, auto& data) {
                        data.first = builder.CopySource(previous);
                        data.second = builder.CopyDest(output);
                    }, [](RenderGraphContext& context, const auto& data) {
                        context.GetCommandList()->copyTexture(context.GetTexture(data.second, RenderGraphAccess::Write), {},
                            context.GetTexture(data.first, RenderGraphAccess::Read), {});
                    });
                context.History->StoreColor(context.Graph, "color", original);
                context.Color() = output;
            }
        };
        struct NoHistoryFeature final : RenderFeature { void Build(RenderFeatureBuildContext&) override {} };
        RegisterRenderFeature({ .Type = "test.contract.history", .Stage = RenderFeatureStage::DisplayPostProcess,
            .RequiredInputs = uint32_t(RenderFeatureInput::DisplayColor) | uint32_t(RenderFeatureInput::History),
            .ColorAccess = RenderFeatureAccess::FullOverwrite },
            [](const Json&) { return std::make_unique<HistoryCopyFeature>(); });
        RegisterRenderFeature({ .Type = "test.contract.emptyhistory", .RequiredInputs = uint32_t(RenderFeatureInput::History) },
            [](const Json&) { return std::make_unique<NoHistoryFeature>(); });
        const auto configuration = [](const char* type) { return Json::array({
            {{"type", type}, {"instance", "test"}, {"version", 1}, {"enabled", true}, {"parameters", Json::object()}} }); };
        RenderGraph display;
        auto hdr = display.CreateTexture(desc, RenderGraphResourceLifetime::Transient, true);
        auto backBuffer = display.CreateTexture(desc, RenderGraphResourceLifetime::Transient, true);
        RenderFeatureBuildContext displayContext{ display, hdr };
        displayContext.DisplayColor = backBuffer;
        RenderFeaturePlan historyPlan(configuration("test.contract.history"));
        historyPlan.Build(RenderFeatureStage::DisplayPostProcess, displayContext);
        Require(displayContext.DisplayColor == backBuffer, "Display Feature replacement lost the host output target.");
        Require(display.Compile(), "Feature history initialization/read/store graph failed compilation.");
        const auto snapshot = display.BuildDiagnosticSnapshot();
        Require(std::ranges::any_of(snapshot.Resources, [](const auto& resource) {
            return resource.Lifetime == RenderGraphResourceLifetime::Persistent;
        }), "History Feature did not create graph-owned persistent storage.");
        Require(std::ranges::any_of(snapshot.Passes, [](const auto& pass) {
            return pass.Name == "Feature display output";
        }), "Display Feature replacement was not copied to the host output.");
        RenderFeaturePlan emptyHistory(configuration("test.contract.emptyhistory"));
        rejected = false;
        try { emptyHistory.Build(RenderFeatureStage::HdrPostProcess, displayContext); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "A declared History input was accepted without acquiring history storage.");
        RenderFeatureHistory isolated;
        const auto history = isolated.ReadColor(graph, "independent", desc);
        isolated.StoreColor(graph, "independent", color);
        Require(graph.Compile(), "Independent history write-back graph failed compilation.");
        Require(std::ranges::any_of(graph.BuildDiagnosticSnapshot().Passes, [](const auto& pass) {
            return pass.Name == "Store Feature history independent" && !pass.bCulled;
        }), "History write-back with no later same-frame reader was culled.");
        isolated.NotifyFrameSubmitted(1);
        Require(!isolated.IsValid("independent"), "Unrecorded history became valid merely from a submission notification.");
        isolated.AbortFrame();
        isolated.Reset();
        Require(graph.IsValid(history) && !isolated.IsValid("independent"), "History reset destroyed storage or invented valid data.");
    }

    void TestFeatureFactoryLifetimeContract()
    {
        struct CountingFeature final : RenderFeature
        {
            explicit CountingFeature(std::shared_ptr<int> live) : Live(std::move(live)) { ++*Live; }
            ~CountingFeature() override { --*Live; }
            void Build(RenderFeatureBuildContext&) override {}
            std::shared_ptr<int> Live;
        };
        const auto live = std::make_shared<int>(0);
        RegisterRenderFeature({ .Type = "test.profile.lifetime", .RequiredInputs = uint32_t(RenderFeatureInput::Normal) },
            [live](const Json&) { return std::make_unique<CountingFeature>(live); });
        RegisterRenderFeature({ .Type = "test.profile.failure" },
            [](const Json&) -> std::unique_ptr<RenderFeature> { throw std::runtime_error("injected factory failure"); });
        const Json first = { { "type", "test.profile.lifetime" }, { "version", 1 },
            { "instance", "first" }, { "enabled", true }, { "parameters", Json::object() } };
        {
            RenderFeaturePlan plan(Json::array({ first }));
            Require(*live == 1 && (plan.RequiredInputs() & uint32_t(RenderFeatureInput::Normal)) != 0,
                "Feature creation or declared input aggregation failed.");
        }
        Require(*live == 0, "Feature instance outlived its plan.");
        auto second = first;
        second["type"] = "test.profile.failure";
        second["instance"] = "second";
        bool failed = false;
        try { RenderFeaturePlan plan(Json::array({ first, second })); }
        catch (const std::exception&) { failed = true; }
        Require(failed && *live == 0, "A failed Feature factory leaked earlier prepared instances.");
        auto disabled = second;
        disabled["enabled"] = false;
        RenderFeaturePlan disabledPlan(Json::array({ disabled }));
        Require(disabledPlan.RequiredInputs() == 0, "Disabled Feature unexpectedly created resources.");
        RenderFeaturePlan emptyPlan(Json::array());
        Require(emptyPlan.RequiredInputs() == 0, "Explicit empty Feature list re-enabled defaults.");
        const auto observedFrames = std::make_shared<uint32_t>(0);
        RegisterRenderFeature({ .Type = "test.profile.context" },
            [live, observedFrames](const RenderFeatureCreateContext& context, const Json& parameters) {
                *observedFrames = context.FramesInFlight;
                Require(parameters.value("sentinel", 0) == 17, "Context factory lost Feature parameters.");
                return std::make_unique<CountingFeature>(live);
            });
        auto contextual = first;
        contextual["type"] = "test.profile.context";
        contextual["parameters"] = { { "sentinel", 17 } };
        {
            RenderFeatureCreateContext createContext;
            createContext.FramesInFlight = 7;
            RenderFeaturePlan contextPlan(Json::array({ contextual }), createContext);
            Require(*observedFrames == 7 && *live == 1, "Feature factory did not receive its view creation context.");
        }
        Require(*live == 0, "Context-created Feature outlived its plan.");
        for (const auto* type : { "builtin.outline", "builtin.postprocess" })
        {
            auto invalidBuiltin = contextual;
            invalidBuiltin["type"] = type;
            failed = false;
            try { RenderFeaturePlan invalidPlan(Json::array({ invalidBuiltin })); }
            catch (const std::exception&) { failed = true; }
            Require(failed, "Built-in Feature silently accepted unknown parameters.");
        }
        failed = false;
        try { RegisterRenderFeature({ .Type = "test.profile.lifetime" }, [live](const Json&) { return std::make_unique<CountingFeature>(live); }); }
        catch (const std::exception&) { failed = true; }
        Require(failed, "Duplicate Feature type registration replaced the original factory.");
    }

    void TestPipelineRegistryResolutionContract()
    {
        const auto invocations = std::make_shared<int>(0);
        RegisterRenderPipeline("test.settings.pipeline", [invocations](const SceneRendererDesc&) -> Scope<RenderPipeline> {
            ++*invocations;
            return {};
        });
        auto requested = MakeEmergencyFallbackRenderSettings();
        requested.PipelineType = "test.settings.pipeline";
        const auto resolved = ResolveRenderSettings(requested, {}, {}, {});
        const auto types = EnumerateRenderPipelineTypes();
        Require(resolved.Effective.PipelineType == requested.PipelineType &&
            IsRenderPipelineRegistered(requested.PipelineType) &&
            std::ranges::find(types, requested.PipelineType) != types.end() && *invocations == 0,
            "Pipeline availability queries invoked a factory or rejected a registered implementation.");
        requested.PipelineType = "missing.settings.pipeline";
        const auto unavailable = ResolveRenderSettings(requested, {}, {}, {});
        Require(unavailable.Requested.PipelineType == requested.PipelineType && unavailable.Effective.PipelineType == "realtime",
            "Missing pipeline failed to preserve Requested and fall back to realtime.");
    }

    void TestPublishedSnapshotCallbackContract()
    {
        TemporaryDirectory directory;
        const auto paths = MakeSettingsPaths(directory);
        SaveDocument(paths.Engine, MakeEngineDefaultDocument());
        ScopedRenderSettingsService scope(MakeServiceInit(paths));
        auto& service = scope.Get();
        auto publishedBefore = service.GetSnapshot();
        bool readCompleted = false;
        bool mutationRejected = false;
        bool failCandidate = false;
        service.SetRuntimeApplyCallback([&](const ResolvedRenderSettings& candidate, const RenderSettingsChangePlan&)
        {
            const auto observed = service.GetSnapshot();
            const auto nestedDraft = service.CreateDraft();
            Require(service.IsInitialized() && observed.Revision == publishedBefore.Revision &&
                observed.Hash == publishedBefore.Hash && nestedDraft.BaseHash == publishedBefore.Hash,
                "Runtime preparation observed an unpublished candidate snapshot.");
            readCompleted = true;
            try { (void)service.ApplyDraft(nestedDraft, false); }
            catch (const std::logic_error&) { mutationRejected = true; }
            if (failCandidate && candidate.Hash != publishedBefore.Hash)
                throw std::runtime_error("injected callback failure after snapshot read");
        });
        auto draft = service.CreateDraft();
        draft.Patch = { { RenderSettingIds::AoEnabled, false } };
        Require(service.ApplyDraft(draft, false).bSuccess && readCompleted && mutationRejected,
            "Runtime callback could not read snapshots or nested mutation was not rejected.");
        Require(service.GetSnapshot().Revision > publishedBefore.Revision && !service.GetSnapshot().Requested.AmbientOcclusion.bEnabled,
            "Successful settings transaction did not publish its snapshot.");
        publishedBefore = service.GetSnapshot();
        failCandidate = true;
        draft = service.CreateDraft();
        draft.Patch = { { RenderSettingIds::AoEnabled, true } };
        const auto failed = service.ApplyDraft(draft, false);
        Require(!failed.bSuccess && failed.bRolledBack && service.GetSnapshot().Hash == publishedBefore.Hash,
            "Failed settings preparation published a candidate or failed to preserve the previous snapshot.");
    }

    void TestPipelineProfileAliasContract()
    {
        TemporaryDirectory directory;
        auto target = CapturePipelineProfile("Renamed", MakeEmergencyFallbackRenderSettings());
        target.Values[RenderSettingIds::AoEnabled] = false;
        SaveDocument(directory.GetPath() / "Renamed.hepipeline", SerializePipelineProfile(target));
        auto alias = SerializePipelineProfile(CapturePipelineProfile("Original", MakeEmergencyFallbackRenderSettings()));
        alias["AliasTarget"] = "Renamed";
        const auto oldPath = directory.GetPath() / "Original.hepipeline";
        SaveDocument(oldPath, alias);
        const auto loaded = LoadPipelineProfileAsset(oldPath);
        Require(loaded.Name == "Renamed" && !loaded.Values[RenderSettingIds::AoEnabled].get<bool>(),
            "A renamed Profile did not preserve loading through its original reference.");
        auto expectFailure = [&]()
        {
            bool rejected = false;
            try { (void)LoadPipelineProfileAsset(oldPath); } catch (const std::exception&) { rejected = true; }
            Require(rejected, "Invalid or cyclic Profile alias was accepted.");
        };
        alias["AliasTarget"] = "Missing";
        SaveDocument(oldPath, alias);
        expectFailure();
        alias["AliasTarget"] = "Original";
        SaveDocument(oldPath, alias);
        expectFailure();
        alias["AliasTarget"] = "../Renamed";
        SaveDocument(oldPath, alias);
        expectFailure();
        alias["AliasTarget"] = "Renamed";
        SaveDocument(oldPath, alias);
        auto loop = SerializePipelineProfile(target);
        loop["AliasTarget"] = "Original";
        SaveDocument(directory.GetPath() / "Renamed.hepipeline", loop);
        expectFailure();
    }

    void TestTransientCacheBudgetContract()
    {
        const auto defaults = MakeEmergencyFallbackRenderSettings();
        Require(defaults.TransientCacheBudgetMiB == 0, "Default transient cache policy unexpectedly changed.");
        const auto* descriptor = FindRenderSettingDescriptor(RenderSettingIds::TransientCacheBudgetMiB);
        Require(descriptor && descriptor->Unit == "MiB" &&
            std::ranges::find(descriptor->WritableLayers, SettingsLayer::Camera) == descriptor->WritableLayers.end() &&
            std::ranges::find(descriptor->WritableLayers, SettingsLayer::World) == descriptor->WritableLayers.end(),
            "Device cache budget permits per-view overrides.");
        for (const auto input : { -1, 0, 512, 16384, 20000 })
        {
            std::vector<RenderSettingDiagnostic> diagnostics;
            const auto result = ApplyRenderSettingsFlatValues(defaults,
                { { RenderSettingIds::TransientCacheBudgetMiB, input } }, diagnostics);
            Require(result.TransientCacheBudgetMiB == static_cast<std::uint32_t>(std::clamp(input, 0, 16384)),
                "Transient cache budget range normalization failed.");
            Require((input >= 0 && input <= 16384) || !diagnostics.empty(),
                "Out-of-range cache budget lacked a normalization diagnostic.");
        }
    }

    void TestPipelineProfileContract()
    {
        auto profile = CapturePipelineProfile("Portable", MakeEmergencyFallbackRenderSettings());
        const Json features = Json::array({ { { "type", "plugin.missing" }, { "version", 1 },
            { "instance", "unique" }, { "enabled", true }, { "parameters", { { "future", 4.25 } } } } });
        profile.Values[RenderSettingIds::PipelineFeatures] = features;
        profile.Values["Rendering.FuturePlugin.Value"] = Json::array({ 1, 2, 3 });
        const auto restored = ParsePipelineProfile(SerializePipelineProfile(profile));
        Require(restored.Values == profile.Values, "Profile round trip destroyed unknown plugin data.");
        auto legacy = SerializePipelineProfile(profile);
        legacy["SchemaVersion"] = 0;
        legacy["Values"] = legacy["Settings"];
        legacy.erase("Settings");
        Require(ParsePipelineProfile(legacy).Values == profile.Values, "Profile version zero migration lost settings.");
        std::vector<RenderSettingDiagnostic> diagnostics;
        const auto requested = ApplyRenderSettingsFlatValues(MakeEmergencyFallbackRenderSettings(), BuildPipelineProfilePatch(restored), diagnostics);
        Require(!HasDiagnosticErrors(diagnostics), "Unknown Feature payload was rejected instead of retained.");
        const auto resolved = ResolveRenderSettings(requested, {}, {}, {});
        Require(resolved.Requested.PipelineFeatures == features && !resolved.Effective.PipelineFeatures[0]["enabled"].get<bool>(),
            "Unavailable Feature did not preserve Requested and disable Effective.");
        auto duplicate = features;
        duplicate.push_back(features[0]);
        diagnostics.clear();
        (void)ApplyRenderSettingsFlatValues(requested, { { RenderSettingIds::PipelineFeatures, duplicate } }, diagnostics);
        Require(HasDiagnosticErrors(diagnostics), "Duplicate Feature identity was accepted.");
        profile.Values[RenderSettingIds::Backend] = "Vulkan";
        bool rejected = false;
        try { (void)BuildPipelineProfilePatch(profile); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "A per-view Profile accepted a device-level setting.");
        auto base = ResolveRenderSettings(MakeEmergencyFallbackRenderSettings(), {}, {}, {});
        base.Sources[Id(RenderSettingIds::PostProcessExposure)] = SettingsLayer::CommandLine;
        const auto view = ResolveRenderViewSettings(base,
            { { RenderSettingIds::Backend, "Vulkan" }, { RenderSettingIds::PostProcessExposure, 5.0 } },
            { { RenderSettingIds::AoEnabled, false } });
        Require(view.Requested.Backend == base.Requested.Backend &&
            view.Requested.PostProcess.Exposure == base.Requested.PostProcess.Exposure &&
            !view.Requested.AmbientOcclusion.bEnabled, "View override permissions or command-line precedence failed.");
        diagnostics.clear();
        auto layered = ApplyRenderSettingsFlatValues(base.Requested,
            { { RenderSettingIds::ViewOverrides, { { "main", { { RenderSettingIds::AoEnabled, false } } } } } }, diagnostics);
        layered = ApplyRenderSettingsFlatValues(layered,
            { { RenderSettingIds::ViewOverrides, { { "preview", { { RenderSettingIds::Path, "Forward" } } },
                { "main", { { RenderSettingIds::PostProcessExposure, 2.0 } } } } } }, diagnostics);
        Require(!HasDiagnosticErrors(diagnostics) && layered.ViewOverrides["main"].size() == 2 &&
            layered.ViewOverrides.contains("preview"), "Layering view settings erased another view or inherited field.");
        diagnostics.clear();
        (void)ApplyRenderSettingsFlatValues(base.Requested,
            { { RenderSettingIds::ViewOverrides, { { "main", { { RenderSettingIds::Backend, "Vulkan" } } } } } }, diagnostics);
        Require(HasDiagnosticErrors(diagnostics), "Named view accepted a device override.");
        auto future = SerializePipelineProfile(restored);
        future["SchemaVersion"] = 99;
        rejected = false;
        try { (void)ParsePipelineProfile(future); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "A future Profile schema was silently downgraded.");
    }

    void TestFramesInFlightContract()
    {
        const RenderSettings defaults = MakeEmergencyFallbackRenderSettings();
        std::vector<RenderSettingDiagnostic> diagnostics;
        const RenderSettings two = ApplyRenderSettingsFlatValues(defaults,
            { { RenderSettingIds::FramesInFlight, 2 } }, diagnostics);
        Require(!HasDiagnosticErrors(diagnostics) && defaults.FramesInFlight == 3 &&
                two.FramesInFlight == 2 && !(two == defaults),
            "Frame queue configuration did not preserve the default or apply two slots.");
        const auto migration = MigrateRenderSettingsDocument(
            SerializeRenderSettingsLayerDocument(RenderSettingsToFlatValues(two)));
        Require(migration.bSuccess &&
                ApplyRenderSettingsFlatValues(defaults, migration.RenderValues, diagnostics) == two,
            "Frame queue configuration failed serialization round trip.");
        const auto before = ResolveRenderSettings(defaults, {}, {}, {});
        const auto after = ResolveRenderSettings(two, {}, {}, {});
        Require(BuildRenderSettingsChangePlan(before, after).bProcessRestartRequired,
            "Changing frame queue depth must require renderer restart.");
        for (const int invalid : { 0, 1, 4, -1 })
        {
            diagnostics.clear();
            const auto rejected = ApplyRenderSettingsFlatValues(defaults,
                { { RenderSettingIds::FramesInFlight, invalid } }, diagnostics);
            Require(rejected.FramesInFlight >= 2 && rejected.FramesInFlight <= 3 &&
                    !diagnostics.empty(),
                "Invalid frame queue depth was accepted without a diagnostic.");
        }
    }

    void TestRejectedViewConfigurationInputContract()
    {
        RejectedViewConfigurationInputs cache;
        cache.BaseHash = "base";
        const Json world = {{RenderSettingIds::Path, "Forward"}};
        const Json camera = {{RenderSettingIds::PostProcessExposure, "bad"}};
        cache.Views.push_back({0, 2, 3, true, "preview", world, camera});
        Require(cache.MatchesBase("base") && !cache.MatchesBase("changed") &&
            cache.MatchesViewCount(1) && !cache.MatchesViewCount(0) && !cache.MatchesViewCount(2),
            "Rejected input cache ignored changed base or live-view additions/removals");
        const auto matches = [&](uint32_t slot, uint32_t generation, uint64_t scene,
            bool present, std::string_view kind, const Json& w, const Json& c)
        { return cache.MatchesView(0, slot, generation, scene, present, kind, w, c); };
        // No frame/pointer/time fields exist in the production matcher: fresh
        // ECS snapshots with the same configuration can skip rejected work.
        for (int frame = 0; frame < 30; ++frame)
            Require(matches(0, 2, 3, true, "preview", Json(world), Json(camera)),
                "Repeated rejected view input did not match by configuration value");
        Require(!matches(1, 2, 3, true, "preview", world, camera) &&
            !matches(0, 4, 3, true, "preview", world, camera) &&
            !matches(0, 2, 4, true, "preview", world, camera) &&
            !matches(0, 2, 3, false, "preview", world, camera) &&
            !matches(0, 2, 3, true, "main", world, camera) &&
            !matches(0, 2, 3, true, "preview", Json::object(), camera) &&
            !matches(0, 2, 3, true, "preview", world, Json::object()) &&
            !cache.MatchesView(1, 0, 2, 3, true, "preview", world, camera),
            "Rejected input cache ignored changed view identity or override data");

        const auto base = ResolveRenderSettings(MakeEmergencyFallbackRenderSettings(), {}, {}, {});
        for (const auto& patch : {
            Json{{RenderSettingIds::Backend, "Vulkan"}},
            Json{{RenderSettingIds::Path, "invalid-path"}},
            Json{{RenderSettingIds::PostProcessExposure, "bad"}} })
        {
            const auto fromWorld = ResolveRenderViewSettings(base, patch, Json::object());
            const auto fromCamera = ResolveRenderViewSettings(base, Json::object(), patch);
            Require(HasDiagnosticErrors(fromWorld.Diagnostics) && HasDiagnosticErrors(fromCamera.Diagnostics),
                "Persistent semantic override rejection lost its diagnostic");
        }
        for (auto feature : DefaultRenderFeatureConfigurations())
        {
            feature["parameters"] = {{"invalid", true}};
            const Json features = Json::array({feature});
            const auto resolved = ResolveRenderViewSettings(base, Json::object(), {{RenderSettingIds::PipelineFeatures, features}});
            Require(!HasDiagnosticErrors(resolved.Diagnostics),
                "Built-in parameter regression no longer reaches the factory boundary");
            bool rejected = false;
            try { RenderFeaturePlan plan(features); }
            catch (const std::invalid_argument& error)
            { rejected = std::string(error.what()).find("parameters") != std::string::npos; }
            Require(rejected, "Built-in invalid parameters did not use recoverable invalid_argument contract");
        }
    }

    void RenderSettingsContract()
    {
        const auto fullHd = ResolveRenderExtent(3840, 2160, 1080);
        const auto native = ResolveRenderExtent(3840, 2160, 0);
        const auto smallWindow = ResolveRenderExtent(1280, 720, 1080);
        const auto ultraWide = ResolveRenderExtent(3440, 1440, 1080);
        const auto portrait = ResolveRenderExtent(1080, 1920, 1080);
        Require(fullHd.Width == 1920 && fullHd.Height == 1080 &&
                native.Width == 3840 && native.Height == 2160 &&
                smallWindow.Width == 1280 && smallWindow.Height == 720 &&
                ultraWide.Width == 2580 && ultraWide.Height == 1080 &&
                portrait.Width == 608 && portrait.Height == 1080,
            "Internal resolution cap failed native, 4K, small-window, or aspect-ratio contracts.");
        TestRejectedViewConfigurationInputContract();
        {
            const auto quality4k = ResolveUpscalingExtent(3840, 2160, UpscalingQuality::Quality);
            const auto performance4k = ResolveUpscalingExtent(3840, 2160, UpscalingQuality::Performance);
            const auto nativeAa = ResolveUpscalingExtent(2560, 1440, UpscalingQuality::NativeAA);
            const auto minimized = ResolveUpscalingExtent(0, 1440, UpscalingQuality::Balanced);
            Require(quality4k.Width == 2560 && quality4k.Height == 1440 &&
                performance4k.Width == 1920 && performance4k.Height == 1080 &&
                nativeAa.Width == 2560 && nativeAa.Height == 1440 && minimized.Height == 0,
                "FSR quality ratios must resolve from output size and tolerate minimized windows");
            const auto defaults = MakeEmergencyFallbackRenderSettings();
            Require(defaults.Upscaling.Mode == UpscalingMode::Off, "Existing projects must retain their AA path");
            std::vector<RenderSettingDiagnostic> diagnostics;
            const auto requested = ApplyRenderSettingsFlatValues(defaults, {
                {RenderSettingIds::UpscalingMode, "FSR3.1"},
                {RenderSettingIds::UpscalingQuality, "Performance"},
                {RenderSettingIds::UpscalingSharpness, 0.35}}, diagnostics);
            Require(!HasDiagnosticErrors(diagnostics) && requested.Upscaling.Mode == UpscalingMode::Fsr31 &&
                requested.Upscaling.Quality == UpscalingQuality::Performance &&
                NearlyEqual(requested.Upscaling.Sharpness, 0.35f), "FSR settings were not applied");
            const auto restored = ApplyRenderSettingsFlatValues(defaults,
                RenderSettingsToFlatValues(requested), diagnostics);
            Require(restored.Upscaling == requested.Upscaling, "FSR settings did not round-trip");
            const auto budget = ApplyRenderSettingsFlatValues(defaults,
                {{RenderSettingIds::TextureStreamingBudget, 192}}, diagnostics);
            Require(budget.TextureStreamingBudgetMiB == 192 && budget != defaults,
                "Texture residency budget must participate in settings equality");
            const auto budgetRoundTrip = ApplyRenderSettingsFlatValues(defaults,
                MigrateRenderSettingsDocument(SerializeRenderSettingsLayerDocument(
                    RenderSettingsToFlatValues(budget))).RenderValues, diagnostics);
            Require(budgetRoundTrip.TextureStreamingBudgetMiB == 192,
                "Texture residency budget did not survive serialization");
            const auto budgetPlan = BuildRenderSettingsChangePlan(ResolveRenderSettings(defaults, {}, {}, {}),
                ResolveRenderSettings(budget, {}, {}, {}));
            Require(budgetPlan.HighestImpact == ApplyImpact::Live && !budgetPlan.bRenderTargetRecreateRequired &&
                !budgetPlan.bTemporalHistoryResetRequired && !budgetPlan.bGpuIdleRequired,
                "Texture residency budget must apply without resetting temporal rendering");
            const auto clampedBudget = ApplyRenderSettingsFlatValues(defaults,
                {{RenderSettingIds::TextureStreamingBudget, 0}}, diagnostics);
            Require(clampedBudget.TextureStreamingBudgetMiB == 1,
                "Zero residency budget must normalize to the supported minimum");
            diagnostics.clear();
            const auto invalidBudget = ApplyRenderSettingsFlatValues(defaults,
                {{RenderSettingIds::TextureStreamingBudget, "512"}}, diagnostics);
            Require(HasDiagnosticErrors(diagnostics) && invalidBudget.TextureStreamingBudgetMiB == 512,
                "Texture residency budget must reject non-integer input without changing the default");
            diagnostics.clear();
            const auto before = ResolveRenderSettings(defaults, {}, {}, {});
            auto after = ResolveRenderSettings(requested, {}, {}, {});
            Require(BuildRenderSettingsChangePlan(before, after).bRenderTargetRecreateRequired,
                "FSR mode/quality change must invalidate render targets");
            auto sharpened = requested;
            sharpened.Upscaling.Sharpness = 0.7f;
            Require(BuildRenderSettingsChangePlan(after, ResolveRenderSettings(sharpened, {}, {}, {})).HighestImpact
                == ApplyImpact::Live, "FSR sharpness must not recreate temporal history");
        }
        {
            const auto defaults = MakeEmergencyFallbackRenderSettings();
            Require(defaults.Clouds.Quality == CloudQuality::Off, "Old settings must default clouds to Off");
            CloudSettings custom;
            custom.Quality = CloudQuality::Balanced;
            auto balanced = ResolveCloudSettings(custom);
            Require(balanced.RenderScale == 0.5f && balanced.MaxSteps == 40 && balanced.LightSteps == 3,
                "Balanced cloud budget mismatch");
            custom.Quality = CloudQuality::Low;
            Require(ResolveCloudSettings(custom).RenderScale == 0.25f, "Low cloud scale is per dimension");
            custom.RenderScale = std::numeric_limits<float>::quiet_NaN();
            bool rejected = false;
            try { (void)ResolveCloudSettings(custom); } catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Nonfinite cloud quality must be rejected");
            EnvironmentLightComponent sceneCloud;
            sceneCloud.bCloudsEnabled = true;
            sceneCloud.CloudSeed = 91827;
            sceneCloud.CloudCoverage = 0.6f;
            Json encoded;
            Serializer::Serialize(encoded, sceneCloud);
            EnvironmentLightComponent restored;
            Serializer::Deserialize(encoded, restored);
            Require(restored.bCloudsEnabled && restored.CloudSeed == 91827 && restored.CloudCoverage == 0.6f,
                "Cloud scene art failed reflected serialization roundtrip");
            EnvironmentLightComponent legacy;
            Serializer::Deserialize(Json::object(), legacy);
            Require(!legacy.bCloudsEnabled && legacy.CloudThickness > 0, "Legacy environment cloud defaults invalid");
            RenderCloudLayer layer;
            Require(!layer.bEnabled, "Old scene cloud default must be disabled");
            layer.Thickness = -1; layer.Coverage = 2;
            NormalizeCloudLayer(layer);
            Require(layer.Thickness == 1 && layer.Coverage == 1, "Cloud art bounds must normalize consistently");
            layer = {};
            Require(IntersectCloudLayer(0, 0, layer) == glm::vec2(0), "Horizontal ray below cloud must miss");
            Require(IntersectCloudLayer(100, 0, layer).y == layer.MaxDistance, "Horizontal ray inside layer must be finite");
            Require(IntersectCloudLayer(0, 1, layer) == glm::vec2(90,185), "Upward cloud interval mismatch");
            Require(IntersectCloudLayer(200, -1, layer) == glm::vec2(15,110), "Downward cloud interval mismatch");
            const double fine = std::pow(std::exp(-0.035 * 100.0 / 80), 80);
            const double coarse = std::pow(std::exp(-0.035 * 100.0 / 24), 24);
            Require(std::abs(fine - coarse) < 1e-12, "Cloud transmission must not depend on step budget");
            std::vector<RenderSettingDiagnostic> diagnostics;
            const auto configured = ApplyRenderSettingsFlatValues(defaults, {
                { RenderSettingIds::CloudQuality, "High" },
                { RenderSettingIds::CloudHistoryWeight, 0.7 },
                { RenderSettingIds::CloudTemporal, false } }, diagnostics);
            Require(!HasDiagnosticErrors(diagnostics) && configured.Clouds.Quality == CloudQuality::High,
                "Cloud settings failed to apply");
            const auto doc = MigrateRenderSettingsDocument(
                SerializeRenderSettingsLayerDocument(RenderSettingsToFlatValues(configured)));
            Require(doc.bSuccess && ApplyRenderSettingsFlatValues(defaults, doc.RenderValues, diagnostics) == configured,
                "Cloud settings failed JSON roundtrip");
            auto requested = defaults;
            requested.Clouds.Quality = CloudQuality::Balanced;
            const auto before = ResolveRenderSettings(requested, {}, {}, {});
            requested.Clouds.Quality = CloudQuality::High;
            const auto high = ResolveRenderSettings(requested, {}, {}, {});
            auto plan = BuildRenderSettingsChangePlan(before, high);
            Require(plan.bTemporalHistoryResetRequired && !plan.bRenderGraphRebuildRequired && !plan.bGpuIdleRequired,
                "Same-size cloud quality changes must not rebuild the graph or idle the GPU");
            requested.Clouds.Quality = CloudQuality::Low;
            plan = BuildRenderSettingsChangePlan(high, ResolveRenderSettings(requested, {}, {}, {}));
            Require(plan.bRenderTargetRecreateRequired && !plan.bGpuIdleRequired,
                "Cloud dimension changes must retire targets without GPU idle");
            requested.Clouds.Quality = CloudQuality::High;
            requested.Clouds.MaxSteps = 17;
            plan = BuildRenderSettingsChangePlan(high, ResolveRenderSettings(requested, {}, {}, {}));
            Require(plan.HighestImpact == ApplyImpact::Live,
                "Editing a dormant Custom budget must not invalidate preset cloud history");
        }

        {
            const auto defaults = MakeEmergencyFallbackRenderSettings();
            std::vector<RenderSettingDiagnostic> diagnostics;
            const auto ssao = ApplyRenderSettingsFlatValues(defaults, {
                { RenderSettingIds::AoTechnique, "SSAO" },
                { RenderSettingIds::AoSampleCount, 32 },
                { RenderSettingIds::AoNormalBias, 0.02 },
                { RenderSettingIds::AoFastUpsample, true } }, diagnostics);
            Require(!HasDiagnosticErrors(diagnostics) &&
                ssao.AmbientOcclusion.Technique == SceneAmbientOcclusionTechnique::SSAO &&
                ssao.AmbientOcclusion.SampleCount == 32 && ssao.AmbientOcclusion.bFastUpsample &&
                NearlyEqual(ssao.AmbientOcclusion.NormalBias, 0.02f),
                "AO technique and quality controls did not reach runtime settings.");
            const auto migration = MigrateRenderSettingsDocument(
                SerializeRenderSettingsLayerDocument(RenderSettingsToFlatValues(ssao)));
            Require(migration.bSuccess &&
                ApplyRenderSettingsFlatValues(defaults, migration.RenderValues, diagnostics) == ssao,
                "AO settings did not survive a project settings round trip.");
            const auto back = ApplyRenderSettingsFlatValues(ssao,
                { { RenderSettingIds::AoTechnique, "GTAO" } }, diagnostics);
            Require(back.AmbientOcclusion.Technique == SceneAmbientOcclusionTechnique::GTAO,
                "SSAO could not switch back to GTAO.");
            diagnostics.clear();
            const auto rejected = ApplyRenderSettingsFlatValues(ssao,
                { { RenderSettingIds::AoTechnique, "RayTraced" } }, diagnostics);
            Require(!diagnostics.empty() &&
                rejected.AmbientOcclusion.Technique == ssao.AmbientOcclusion.Technique,
                "Unknown AO algorithm silently changed the effective technique.");
            for (const int invalid : { -1, 0, 65 })
            {
                diagnostics.clear();
                const auto clamped = ApplyRenderSettingsFlatValues(ssao,
                    { { RenderSettingIds::AoSampleCount, invalid } }, diagnostics);
                Require(!diagnostics.empty() && clamped.AmbientOcclusion.SampleCount >= 8 &&
                    clamped.AmbientOcclusion.SampleCount <= 64,
                    "Unsafe SSAO loop bound was accepted without a diagnostic.");
            }
        }
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
        TestCountedIndirectCapabilityFallback();
        TestGlobalIlluminationPresetDetection();
        TestGlobalIlluminationPresetTechniqueIsolation();
        TestGlobalIlluminationDebugDisablesAntiAliasing();
        TestToolSettingsDoNotPersistToProject();
        TestEngineDefaultMatchesEmergencyFallback();
        TestDualBackendAvailabilityAndRestartContract();
        TestFramesInFlightContract();
        TestPipelineProfileContract();
        TestTransientCacheBudgetContract();
        TestPipelineProfileAliasContract();
        TestPublishedSnapshotCallbackContract();
        TestPipelineRegistryResolutionContract();
        TestFeatureFactoryLifetimeContract();
        TestFeatureGraphContracts();
        TestViewSettingsPersistenceContract();
    }
}
