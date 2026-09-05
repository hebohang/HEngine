#pragma once

#include "Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSession.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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
        settings.TargetSamples = 10;
        settings.SamplesPerFrame = 4;
        require(
            Detail::ResolveReferenceOfflineDispatchCount(
                settings,
                OfflineViewportSessionState::Rendering,
                0,
                false,
                false) == 4,
            "Offline reference did not schedule SamplesPerFrame at frame start.");
        require(
            Detail::ResolveReferenceOfflineDispatchCount(
                settings,
                OfflineViewportSessionState::Rendering,
                8,
                false,
                false) == 2,
            "Offline reference did not clamp dispatches to remaining TargetSamples.");
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
                Command::SamplesSubmitted,
                9,
                10,
                false) == OfflineViewportSessionState::Rendering &&
                Detail::ResolveReferenceOfflineControlState(
                    OfflineViewportSessionState::Rendering,
                    Command::SamplesSubmitted,
                    10,
                    10,
                    false) == OfflineViewportSessionState::Complete,
            "Offline reference submitted-sample completion boundary failed.");

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
