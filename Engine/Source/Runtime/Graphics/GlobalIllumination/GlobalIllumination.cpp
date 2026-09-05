#include "Hepch.h"

#include "Runtime/Graphics/GlobalIllumination/GlobalIllumination.h"

#include <algorithm>
#include <cmath>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] bool IsHardwareRayTracingReady(
            const RenderCapabilities& capabilities) noexcept
        {
            return capabilities.RayTracingAccelStruct.bEnabled &&
                capabilities.RayTracingPipeline.bEnabled;
        }

        [[nodiscard]] bool Changed(float left, float right) noexcept
        {
            return !std::isfinite(left) || !std::isfinite(right) ||
                std::abs(left - right) > 1e-6f;
        }
    }

    GlobalIlluminationSelection ResolveGlobalIlluminationSelection(
        const GlobalIlluminationSettings& settings,
        const RenderCapabilities& capabilities,
        bool bScreenSpaceTechniqueAvailable)
    {
        GlobalIlluminationSelection selection;
        selection.Requested = settings.Technique;
        selection.Selected = settings.Technique;

        if (settings.Technique == GlobalIlluminationTechniqueKind::Off)
        {
            selection.Reason = "Global illumination is disabled by settings.";
            return selection;
        }

        if (settings.Technique == GlobalIlluminationTechniqueKind::ScreenSpace)
        {
            selection.bSupported = bScreenSpaceTechniqueAvailable;
            if (bScreenSpaceTechniqueAvailable)
            {
                selection.Reason = "The screen-space GI technique is available.";
            }
            else
            {
                selection.Selected = GlobalIlluminationTechniqueKind::Off;
                selection.bFallback = true;
                selection.Reason =
                    "The screen-space GI technique is not implemented; GI is disabled.";
            }
            return selection;
        }

        if (settings.Technique ==
            GlobalIlluminationTechniqueKind::ExternalViewportRenderer)
        {
            selection.bSupported = false;
            selection.bFallback = true;
            selection.Selected =
                settings.UnsupportedFallback ==
                        GlobalIlluminationFallbackKind::ScreenSpace &&
                    bScreenSpaceTechniqueAvailable
                ? GlobalIlluminationTechniqueKind::ScreenSpace
                : GlobalIlluminationTechniqueKind::Off;
            selection.Reason =
                "External viewport rendering is available only through an enabled offline renderer plugin.";
            return selection;
        }

        if (IsHardwareRayTracingReady(capabilities))
        {
            selection.bUsesHardwareRayTracing = true;
            selection.Reason =
                "Hardware RT acceleration structures and pipelines are available for real-time RTGI.";
            return selection;
        }

        selection.bSupported = false;
        selection.bFallback = true;
        selection.Selected = GlobalIlluminationTechniqueKind::Off;
        if (settings.UnsupportedFallback ==
                GlobalIlluminationFallbackKind::ScreenSpace &&
            bScreenSpaceTechniqueAvailable)
        {
            selection.Selected = GlobalIlluminationTechniqueKind::ScreenSpace;
            selection.Reason =
                "Hardware RT acceleration structures or pipelines are unavailable; selected the screen-space fallback.";
        }
        else
        {
            selection.Reason =
                "Hardware RT acceleration structures or pipelines are unavailable; GI is disabled and raster rendering remains active.";
        }
        return selection;
    }

    GlobalIlluminationCompositionSource
        SelectGlobalIlluminationCompositionSource(
            GlobalIlluminationCompositionMode mode,
            float primaryCoverage,
            float primaryConfidence,
            float secondaryCoverage,
            float secondaryConfidence) noexcept
    {
        const bool bPrimaryValid = std::isfinite(primaryCoverage) &&
            primaryCoverage > 0.0f;
        const bool bSecondaryValid = std::isfinite(secondaryCoverage) &&
            secondaryCoverage > 0.0f;
        if (mode == GlobalIlluminationCompositionMode::Replace)
        {
            return bPrimaryValid
                ? GlobalIlluminationCompositionSource::Primary
                : GlobalIlluminationCompositionSource::None;
        }
        if (mode == GlobalIlluminationCompositionMode::FillInvalidByCoverage)
        {
            if (bPrimaryValid)
                return GlobalIlluminationCompositionSource::Primary;
            return bSecondaryValid
                ? GlobalIlluminationCompositionSource::Secondary
                : GlobalIlluminationCompositionSource::None;
        }
        if (!bPrimaryValid)
        {
            return bSecondaryValid
                ? GlobalIlluminationCompositionSource::Secondary
                : GlobalIlluminationCompositionSource::None;
        }
        if (!bSecondaryValid)
            return GlobalIlluminationCompositionSource::Primary;
        const float safePrimaryConfidence = std::isfinite(primaryConfidence)
            ? primaryConfidence
            : 0.0f;
        const float safeSecondaryConfidence = std::isfinite(secondaryConfidence)
            ? secondaryConfidence
            : 0.0f;
        return safeSecondaryConfidence > safePrimaryConfidence
            ? GlobalIlluminationCompositionSource::Secondary
            : GlobalIlluminationCompositionSource::Primary;
    }

    GlobalIlluminationHistoryResetReason
        CompareGlobalIlluminationHistorySignatures(
            const std::optional<GlobalIlluminationHistorySignature>& previous,
            const GlobalIlluminationHistorySignature& current)
    {
        if (!previous)
            return GlobalIlluminationHistoryResetReason::FirstFrame;

        GlobalIlluminationHistoryResetReason reasons =
            GlobalIlluminationHistoryResetReason::None;
        if (previous->Width != current.Width || previous->Height != current.Height)
            reasons |= GlobalIlluminationHistoryResetReason::Resize;
        if (Changed(previous->RenderScale, current.RenderScale))
            reasons |= GlobalIlluminationHistoryResetReason::RenderScale;
        if (current.bCameraCut)
            reasons |= GlobalIlluminationHistoryResetReason::CameraCut;
        if (previous->ProjectionFingerprint != current.ProjectionFingerprint)
            reasons |= GlobalIlluminationHistoryResetReason::Projection;
        if (previous->JitterSequenceGeneration != current.JitterSequenceGeneration)
            reasons |= GlobalIlluminationHistoryResetReason::JitterSequence;
        if (previous->SceneGeneration != current.SceneGeneration)
            reasons |= GlobalIlluminationHistoryResetReason::SceneReload;
        if (previous->GeometryTopologyGeneration !=
            current.GeometryTopologyGeneration)
        {
            reasons |= GlobalIlluminationHistoryResetReason::GeometryTopology;
        }
        if (previous->MaterialGeneration != current.MaterialGeneration)
            reasons |= GlobalIlluminationHistoryResetReason::Material;
        if (previous->LightGeneration != current.LightGeneration)
            reasons |= GlobalIlluminationHistoryResetReason::Light;
        if (previous->TechniqueStateFingerprint !=
            current.TechniqueStateFingerprint)
        {
            reasons |=
                GlobalIlluminationHistoryResetReason::ExternalRendererState;
        }
        if (previous->Technique != current.Technique)
            reasons |= GlobalIlluminationHistoryResetReason::Technique;
        if (previous->MaxBounces != current.MaxBounces)
            reasons |= GlobalIlluminationHistoryResetReason::BounceCount;
        if (previous->bHalfResolution != current.bHalfResolution)
            reasons |= GlobalIlluminationHistoryResetReason::ResolutionMode;
        if (previous->Denoiser != current.Denoiser)
            reasons |= GlobalIlluminationHistoryResetReason::Denoiser;
        if (previous->SamplesPerPixel != current.SamplesPerPixel)
            reasons |= GlobalIlluminationHistoryResetReason::Sampling;
        return reasons;
    }

    GlobalIlluminationHistoryResetReason GlobalIlluminationHistoryTracker::Update(
        const GlobalIlluminationHistorySignature& signature)
    {
        GlobalIlluminationHistoryResetReason reasons =
            CompareGlobalIlluminationHistorySignatures(mPrevious, signature);
        reasons |= mPendingReasons;
        mPendingReasons = GlobalIlluminationHistoryResetReason::None;
        mPrevious = signature;
        return reasons;
    }

    void GlobalIlluminationHistoryTracker::Reset(
        GlobalIlluminationHistoryResetReason reason) noexcept
    {
        mPendingReasons |= reason;
    }

    GlobalIlluminationHistoryResetReason
        GlobalIlluminationHistoryTracker::GetPendingReasons() const noexcept
    {
        return mPendingReasons;
    }

    bool GlobalIlluminationHistoryTracker::HasValidHistory() const noexcept
    {
        return mPrevious.has_value() &&
            mPendingReasons == GlobalIlluminationHistoryResetReason::None;
    }
}
