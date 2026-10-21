#pragma once

#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSession.h"
#include "Plugins/OfflineReferenceRenderer/Source/ProgressiveSchedule.h"
#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSettingsJson.h"
#include "Plugins/OfflineReferenceRenderer/Source/ProgressiveDenoiser.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <chrono>
#include <thread>

namespace HEngine::Tests
{
    inline void OfflineReferenceRendererSessionContract()
    {
        const auto require = [](bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        };
        const auto requireThrows = [&require]<typename Exception>(
            auto&& callable,
            std::string_view message)
        {
            bool bCaughtExpected = false;
            try
            {
                std::forward<decltype(callable)>(callable)();
            }
            catch (const Exception&)
            {
                bCaughtExpected = true;
            }
            catch (...)
            {
            }
            require(bCaughtExpected, message);
        };

        OfflineViewportRendererSettings settings;
        const double normalBudget = Detail::ResolveProgressiveTraceBudget(4.0, 60.0, 0.0);
        require(Detail::ResolveProgressiveTraceBudget(4.0, 60.0, 6.0) == normalBudget,
            "Other GPU work below the frame allowance starved path tracing.");
        const double overloadedBudget = Detail::ResolveProgressiveTraceBudget(4.0, 60.0, 30.0);
        require(overloadedBudget >= normalBudget * 0.5 && overloadedBudget <= normalBudget,
            "An overloaded frame must retain bounded tracing progress.");
        require(Detail::ResolveProgressiveTraceBudget(16.0, 120.0, 0.0) <= 1000.0 / 120.0 * 0.5,
            "Tracing exceeded the interactive frame allowance.");
        Detail::ProgressiveSchedule busySchedule;
        busySchedule.Reset(1080);
        const auto busyBand = busySchedule.Reserve(1920, 256,
            Detail::ResolveProgressiveTraceBudget(4.0, 60.0, 6.0), 0.00002, true);
        require(busyBand.Rows >= 64,
            "A 6 ms background pass reduced a 60 FPS trace to single-row dispatches.");
        Detail::ProgressiveSchedule schedule;
        schedule.Reset(20);
        auto band = schedule.Reserve(1024, 4, 2, 0, true);
        require(band.Rows == 4 && band.Sample == 0, "Unknown timing must use conservative work.");
        schedule.Abort();
        require(schedule.Submitted() == 0 && schedule.Average() == 0, "Abort changed progress.");
        band = schedule.Reserve(1024, 4, 2, 0, true);
        schedule.Submit(10);
        require(schedule.Reserve(1024, 4, 2, 0, true).Rows == 0, "In-flight cap failed.");
        require(!schedule.Collect(9) && schedule.Average() == 0, "Completion was fabricated.");
        require(schedule.Collect(10) && schedule.Average() == 0.2 && schedule.Maximum() == 1,
            "Partial sample normalization failed.");
        band = schedule.Reserve(1024, 4, 2, 0, true);
        require(band.FirstRow == 4, "Aborted or committed rows repeated.");
        schedule.Submit(11);
        schedule.Reset(20);
        require(schedule.Reserve(1024, 4, 2, 0, true).Rows == 0, "Reset lost stale flight ownership.");
        schedule.Collect(11);
        require(schedule.Average() == 0 && schedule.Stale() == 1, "Late generation contaminated progress.");
        for (uint64_t submission = 12; submission < 32; ++submission)
        {
            band = schedule.Reserve(1024, 4, 2, 0, true);
            require(band.Sample == (submission - 12) / 5 && band.FirstRow == ((submission - 12) % 5) * 4,
                "Band sequence repeated or skipped a pixel sample.");
            schedule.Submit(submission);
            schedule.Collect(submission);
        }
        require(schedule.Completed() == 4 && schedule.Maximum() == 4 &&
            schedule.Reserve(1024, 4, 2, 0, true).Rows == 0, "Final sample boundary failed.");
        const auto migrated = DeserializeOfflineReferenceRendererSettings(Json{{"SamplesPerFrame", 46}}, {});
        require(migrated.bInteractive && migrated.SamplesPerFrame == 46 && migrated.WorkBudgetMilliseconds == 4,
            "Legacy settings were lost or bypassed the automatic budget.");
        if (ProgressiveDenoiser::Available())
        {
            ProgressiveDenoiser denoiser;
            DenoiseSnapshot snapshot;
            snapshot.Width = snapshot.Height = 32;
            snapshot.Generation = 7;
            snapshot.Samples = 4;
            snapshot.Color.resize(32 * 32 * 4, 1.f);
            snapshot.Albedo.resize(snapshot.Color.size(), 1.f);
            snapshot.Normal.resize(snapshot.Color.size(), 0.f);
            uint32_t rng = 123;
            double rawError = 0;
            for (size_t pixel = 0; pixel < 1024; ++pixel)
            {
                rng = rng * 1664525u + 1013904223u;
                const float noise = (float(rng & 65535u) / 65535.f - .5f) * .8f;
                for (size_t c = 0; c < 3; ++c) snapshot.Color[pixel * 4 + c] += noise;
                snapshot.Normal[pixel * 4 + 2] = 1.f;
                rawError += noise * noise * 3;
            }
            const auto raw = snapshot.Color;
            require(denoiser.Submit(std::move(snapshot)), "OIDN did not accept a snapshot.");
            require(!denoiser.Submit({}), "OIDN accepted an unbounded second task.");
            std::unique_ptr<DenoiseSnapshot> denoised;
            const auto started = std::chrono::steady_clock::now();
            while (!(denoised = denoiser.Poll()) && std::chrono::steady_clock::now() - started < std::chrono::seconds(30))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(denoised && denoised->Error.empty(), "OIDN execution failed or timed out.");
            require(denoised->Color == raw && denoised->Generation == 7 && denoised->Samples == 4,
                "OIDN overwrote raw samples or snapshot identity.");
            double filteredError = 0;
            for (size_t pixel = 0; pixel < 1024; ++pixel)
                for (size_t c = 0; c < 3; ++c)
                {
                    const float error = denoised->Output[pixel * 4 + c] - 1.f;
                    filteredError += error * error;
                }
            require(filteredError < rawError, "OIDN did not reduce error in a constant-radiance image.");
        }
        settings.TargetSamples = 10;
        settings.SamplesPerFrame = 4;
        require(
            Detail::ResolveReferenceOfflineDispatchCount(
                settings,
                OfflineViewportSessionState::Rendering,
                0,
                false,
                false) == 1,
            "Legacy batch size bypassed the budgeted dispatch cap.");
        require(
            Detail::ResolveReferenceOfflineDispatchCount(
                settings,
                OfflineViewportSessionState::Rendering,
                8,
                false,
                false) == 1,
            "Remaining target samples bypassed the budgeted dispatch cap.");
        require(
            Detail::ResolveReferenceOfflineDispatchCount(
                settings,
                OfflineViewportSessionState::Rendering,
                10,
                false,
                false) == 0,
            "Offline reference scheduled work after reaching TargetSamples.");
        require(
            Detail::ResolveReferenceOfflineDispatchCount(
                settings,
                OfflineViewportSessionState::Paused,
                4,
                false,
                false) == 0 &&
                Detail::ResolveReferenceOfflineDispatchCount(
                    settings,
                    OfflineViewportSessionState::Rendering,
                    4,
                    true,
                    false) == 0 &&
                Detail::ResolveReferenceOfflineDispatchCount(
                    settings,
                    OfflineViewportSessionState::Rendering,
                    4,
                    false,
                    true) == 0,
            "Offline reference pause, restart-required, or freeze scheduling was not zero.");

        using Command = Detail::ReferenceOfflineControlCommand;
        require(
            Detail::ResolveReferenceOfflineControlState(
                OfflineViewportSessionState::Rendering,
                Command::Pause,
                4,
                10,
                false) == OfflineViewportSessionState::Paused,
            "Offline reference pause transition failed.");
        require(
            Detail::ResolveReferenceOfflineControlState(
                OfflineViewportSessionState::Paused,
                Command::Resume,
                4,
                10,
                false) == OfflineViewportSessionState::Rendering,
            "Offline reference resume transition failed.");
        require(
            Detail::ResolveReferenceOfflineControlState(
                OfflineViewportSessionState::Paused,
                Command::Resume,
                4,
                10,
                true) == OfflineViewportSessionState::Paused,
            "Offline reference resumed despite a required restart.");
        require(
            Detail::ResolveReferenceOfflineControlState(
                OfflineViewportSessionState::Paused,
                Command::Resume,
                10,
                10,
                false) == OfflineViewportSessionState::Complete,
            "Offline reference resume did not preserve the complete boundary.");
        require(
            Detail::ResolveReferenceOfflineControlState(
                OfflineViewportSessionState::Cancelled,
                Command::Restart,
                0,
                10,
                false) == OfflineViewportSessionState::Rendering &&
                Detail::ResolveReferenceOfflineControlState(
                    OfflineViewportSessionState::Rendering,
                    Command::Cancel,
                    4,
                    10,
                    false) == OfflineViewportSessionState::Cancelled,
            "Offline reference restart or cancel transition failed.");
        require(
            Detail::ResolveReferenceOfflineControlState(
                OfflineViewportSessionState::Rendering,
                Command::SamplesCompleted,
                9,
                10,
                false) == OfflineViewportSessionState::Rendering &&
                Detail::ResolveReferenceOfflineControlState(
                    OfflineViewportSessionState::Rendering,
                    Command::SamplesCompleted,
                    10,
                    10,
                    false) == OfflineViewportSessionState::Complete,
            "Offline reference completed-sample boundary failed.");

        Detail::ValidateReferenceOfflineExportRequest(
            OfflineViewportAov::Beauty,
            OfflineViewportOutputFormat::Pfm);
        Detail::ValidateReferenceOfflineExportRequest(
            OfflineViewportAov::MaterialId,
            OfflineViewportOutputFormat::Raw);
        Detail::ValidateReferenceOfflineExportRequest(
            OfflineViewportAov::Beauty,
            OfflineViewportOutputFormat::ToneMappedPng);
        requireThrows.operator()<std::invalid_argument>(
            []
            {
                Detail::ValidateReferenceOfflineExportRequest(
                    OfflineViewportAov::MaterialId,
                    OfflineViewportOutputFormat::ToneMappedPng);
            },
            "Offline reference accepted tone-mapped PNG for a non-Beauty AOV.");
        requireThrows.operator()<std::invalid_argument>(
            []
            {
                Detail::ValidateReferenceOfflineExportRequest(
                    OfflineViewportAov::MaterialId,
                    OfflineViewportOutputFormat::Pfm);
            },
            "Offline reference accepted integer PFM export.");
        requireThrows.operator()<std::out_of_range>(
            []
            {
                Detail::ValidateReferenceOfflineExportRequest(
                    static_cast<OfflineViewportAov>(255),
                    OfflineViewportOutputFormat::Raw);
            },
            "Offline reference accepted an unknown AOV export.");

        ReferencePathTracerFrameParameters environmentFrame;
        environmentFrame.Camera.View = glm::mat4(1.0f);
        environmentFrame.Camera.Projection = glm::mat4(1.0f);
        environmentFrame.Camera.ViewProjection = glm::mat4(1.0f);
        GlobalIlluminationSettings environmentSettings;
        environmentSettings.Technique =
            GlobalIlluminationTechniqueKind::ExternalViewportRenderer;
        environmentSettings.Denoiser = GlobalIlluminationDenoiserKind::None;
        const uint64_t constantEnvironmentFingerprint =
            ComputeReferencePathTracerAccumulationFingerprint(
                environmentFrame,
                environmentSettings);
        environmentFrame.bSampleSceneEnvironment = true;
        const uint64_t sceneEnvironmentFingerprint =
            ComputeReferencePathTracerAccumulationFingerprint(
                environmentFrame,
                environmentSettings);
        require(
            sceneEnvironmentFingerprint != constantEnvironmentFingerprint,
            "Switching to the scene environment did not invalidate accumulation.");
        environmentFrame.EnvironmentRadiance = glm::vec3(2.0f);
        require(
            ComputeReferencePathTracerAccumulationFingerprint(
                environmentFrame,
                environmentSettings) != sceneEnvironmentFingerprint,
            "Changing scene environment intensity did not invalidate accumulation.");
    }
}
