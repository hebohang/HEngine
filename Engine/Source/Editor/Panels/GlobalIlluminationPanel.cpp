#include "GlobalIlluminationPanel.h"

#include "Editor/EditorLayer.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Graphics/GlobalIllumination/GlobalIlluminationTechniqueRegistry.h"
#include "Runtime/Graphics/Settings/RenderSettings.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <vector>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] const char* DebugViewName(
            GlobalIlluminationDebugView view) noexcept
        {
            switch (view)
            {
            case GlobalIlluminationDebugView::Composite: return "Composite";
            case GlobalIlluminationDebugView::RawDiffuse: return "Raw Diffuse";
            case GlobalIlluminationDebugView::RawDirectDiffuse: return "Raw Direct";
            case GlobalIlluminationDebugView::RawIndirectDiffuse: return "Raw Indirect";
            case GlobalIlluminationDebugView::Albedo: return "Albedo";
            case GlobalIlluminationDebugView::DemodulatedDiffuseIllumination:
                return "Demodulated Illumination";
            case GlobalIlluminationDebugView::FilteredDiffuseIllumination:
                return "Filtered Illumination";
            case GlobalIlluminationDebugView::RemodulatedDiffuse:
                return "Remodulated Radiance";
            case GlobalIlluminationDebugView::Normal: return "Normal";
            case GlobalIlluminationDebugView::DenoisedDiffuse: return "Denoised Diffuse";
            case GlobalIlluminationDebugView::HitDistance: return "Hit Distance";
            case GlobalIlluminationDebugView::HistoryLength: return "History Length";
            case GlobalIlluminationDebugView::Confidence: return "Confidence";
            case GlobalIlluminationDebugView::Coverage: return "Coverage / Invalid";
            case GlobalIlluminationDebugView::Variance: return "Variance";
            case GlobalIlluminationDebugView::SourceLighting: return "Source Lighting (HDR)";
            case GlobalIlluminationDebugView::TemporalDiffuseIllumination: return "Temporal Illumination (HDR)";
            case GlobalIlluminationDebugView::ViewDepth: return "View Depth (log)";
            case GlobalIlluminationDebugView::ScreenHitCoverage: return "Screen Hit Coverage";
            case GlobalIlluminationDebugView::TraceTermination: return "SSGI Trace Termination (first ray)";
            case GlobalIlluminationDebugView::TraceHitLocation: return "SSGI First Hit UV / Source Luminance";
            }
            return "Unknown";
        }

        [[nodiscard]] bool IsDebugViewSupported(
            const GlobalIlluminationTechniqueDescriptor& descriptor,
            GlobalIlluminationDebugView view)
        {
            return std::ranges::find(
                descriptor.SupportedDebugViews,
                view) != descriptor.SupportedDebugViews.end();
        }
    }

    GlobalIlluminationPanel::GlobalIlluminationPanel(
        EditorLayer* parent,
        std::string panelName,
        bool* pOpen)
        : BasePanel(parent, std::move(panelName), pOpen)
    {
    }

    void GlobalIlluminationPanel::InitializePendingSettings()
    {
        Application& application = Application::GetInstance();
        mApplied = application.GetGlobalIlluminationSettings();
        mPending = mApplied;
        mAppliedAntiAliasing = application.GetAntiAliasingSettings();
        mPendingAntiAliasing = mAppliedAntiAliasing;
        RenderSettings presetSettings;
        presetSettings.GlobalIllumination = mPending;
        mQualityPreset = static_cast<int>(
            DetectGlobalIlluminationQualityPreset(presetSettings));
        bInitialized = true;
        bDirty = false;
        bAntiAliasingDirty = false;
    }

    void GlobalIlluminationPanel::ApplyQualityPreset(int preset)
    {
        mQualityPreset = std::clamp(preset, 0, 3);
        const auto selected = static_cast<GlobalIlluminationQualityPreset>(
            mQualityPreset);
        if (selected == GlobalIlluminationQualityPreset::Custom)
            return;
        RenderSettings settings;
        settings.GlobalIllumination = mPending;
        std::vector<RenderSettingDiagnostic> diagnostics;
        settings = ApplyRenderSettingsFlatValues(settings,
            GetGlobalIlluminationQualityPresetPatch(selected, mPending.Technique),
            diagnostics);
        mPending = settings.GlobalIllumination;
        bDirty = true;
    }

    void GlobalIlluminationPanel::OnImGuiRenderImpl()
    {
        if (!bInitialized)
            InitializePendingSettings();

        Application& application = Application::GetInstance();
        const RenderCapabilities capabilities = application.GetRenderCapabilities();
        const std::vector<GlobalIlluminationTechniqueDescriptor> descriptors =
            application.EnumerateGlobalIlluminationTechniques();
        const auto selectedDescriptor = std::ranges::find_if(
            descriptors,
            [this](const GlobalIlluminationTechniqueDescriptor& descriptor)
            {
                return descriptor.Id == mPending.Technique;
            });
        const char* techniquePreview = selectedDescriptor != descriptors.end()
            ? selectedDescriptor->DisplayName.c_str()
            : "Unknown";

        ImGuiWrapper::PropertyRow("Technique", [&]()
        {
            if (ImGui::BeginCombo("##GITechnique", techniquePreview))
            {
                for (const GlobalIlluminationTechniqueDescriptor& descriptor :
                    descriptors)
                {
                    const bool bAvailable =
                        descriptor.AreCapabilitiesSatisfied(capabilities);
                    ImGui::BeginDisabled(!bAvailable);
                    const bool bSelected = descriptor.Id == mPending.Technique;
                    if (ImGui::Selectable(
                            descriptor.DisplayName.c_str(),
                            bSelected) && bAvailable)
                    {
                        mPending.Technique = descriptor.Id;
                        if (descriptor.Id ==
                            GlobalIlluminationTechniqueKind::HardwareRayTracing)
                        {
                            mPending.SamplesPerPixel = 1;
                        }
                        bDirty = true;
                    }
                    if (bSelected)
                        ImGui::SetItemDefaultFocus();
                    ImGui::EndDisabled();
                    if (!bAvailable && ImGui::IsItemHovered(
                            ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip(
                            "Required renderer capabilities are unavailable.");
                    }
                }
                ImGui::EndCombo();
            }
        });

        RenderSettings detectedPresetSettings;
        detectedPresetSettings.GlobalIllumination = mPending;
        mQualityPreset = static_cast<int>(
            DetectGlobalIlluminationQualityPreset(detectedPresetSettings));
        constexpr std::array<const char*, 4> qualityNames = {
            "Custom", "Low", "Balanced", "High"
        };
        ImGuiWrapper::PropertyRow("Quality", [&]()
        {
            if (ImGui::Combo(
                    "##GIQuality",
                    &mQualityPreset,
                    qualityNames.data(),
                    static_cast<int>(qualityNames.size())))
            {
                ApplyQualityPreset(mQualityPreset);
            }
        });

        ImGuiWrapper::PropertyRow("Render Scale", [&]()
        {
            bDirty |= ImGui::SliderFloat(
                "##GIRenderScale", &mPending.RenderScale, 0.5f, 1.0f, "%.2f");
        });
        ImGuiWrapper::PropertyRow("Max Bounces", [&]()
        {
            int value = static_cast<int>(mPending.MaxBounces);
            if (ImGui::SliderInt("##GIMaxBounces", &value, 1, 8))
            {
                mPending.MaxBounces = static_cast<uint32_t>(value);
                bDirty = true;
            }
        });
        ImGuiWrapper::PropertyRow("Samples Per Pixel", [&]()
        {
            const bool bUsesScreenSpaceRayCount = mPending.Technique ==
                GlobalIlluminationTechniqueKind::ScreenSpace;
            const bool bFixedOneSample = mPending.Technique ==
                GlobalIlluminationTechniqueKind::HardwareRayTracing;
            ImGui::BeginDisabled(bFixedOneSample || bUsesScreenSpaceRayCount);
            int value = static_cast<int>(mPending.SamplesPerPixel);
            if (ImGui::SliderInt("##GISpp", &value, 1, 16))
            {
                mPending.SamplesPerPixel = static_cast<uint32_t>(value);
                bDirty = true;
            }
            ImGui::EndDisabled();
            if ((bFixedOneSample || bUsesScreenSpaceRayCount) &&
                ImGui::IsItemHovered(
                    ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(
                    bUsesScreenSpaceRayCount
                        ? "SSGI sampling is controlled by Ray Count."
                        : "This technique records exactly 1 spp per frame.");
            }
        });
        ImGuiWrapper::PropertyRow("Denoiser", [&]()
        {
            int denoiser = mPending.Denoiser ==
                    GlobalIlluminationDenoiserKind::Svgf
                ? 1
                : 0;
            const char* names[] = { "None", "SVGF" };
            if (ImGui::Combo("##GIDenoiser", &denoiser, names, 2))
            {
                mPending.Denoiser = denoiser == 1
                    ? GlobalIlluminationDenoiserKind::Svgf
                    : GlobalIlluminationDenoiserKind::None;
                bDirty = true;
            }
        });
        ImGuiWrapper::PropertyRow("A-Trous Iterations", [&]()
        {
            int value = static_cast<int>(mPending.AtrousIterations);
            if (ImGui::SliderInt("##GIAtrous", &value, 1, 5))
            {
                mPending.AtrousIterations = static_cast<uint32_t>(value);
                bDirty = true;
            }
        });
        ImGuiWrapper::PropertyRow("Bilateral Upsample", [&]()
        {
            bDirty |= ImGui::Checkbox(
                "##GIBilateral", &mPending.bBilateralUpsample);
        });

        if (selectedDescriptor != descriptors.end())
        {
            ImGuiWrapper::PropertyRow("Debug View", [&]()
            {
                if (ImGui::BeginCombo(
                        "##GIDebugView",
                        DebugViewName(mPending.DebugView)))
                {
                    for (GlobalIlluminationDebugView view :
                        selectedDescriptor->SupportedDebugViews)
                    {
                        const bool bSelected = view == mPending.DebugView;
                        if (ImGui::Selectable(DebugViewName(view), bSelected))
                        {
                            mPending.DebugView = view;
                            bDirty = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            });
            if (!IsDebugViewSupported(*selectedDescriptor, mPending.DebugView))
            {
                mPending.DebugView = GlobalIlluminationDebugView::Composite;
                bDirty = true;
            }
        }

        if (mPending.Technique == GlobalIlluminationTechniqueKind::ScreenSpace)
        {
            if (ImGui::CollapsingHeader(
                    "SSGI",
                    ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGuiWrapper::PropertyRow("Ray Count", [&]()
                {
                    int value = static_cast<int>(mPending.ScreenSpaceRayCount);
                    if (ImGui::SliderInt("##SSGIRays", &value, 1, 8))
                    {
                        mPending.ScreenSpaceRayCount = static_cast<uint32_t>(value);
                        bDirty = true;
                    }
                });
                ImGuiWrapper::PropertyRow("Max Ray Distance", [&]()
                {
                    bDirty |= ImGui::DragFloat(
                        "##SSGIDistance",
                        &mPending.ScreenSpaceMaximumRayDistance,
                        0.05f,
                        0.1f,
                        100.0f,
                        "%.2f m");
                });
                ImGuiWrapper::PropertyRow("Thickness", [&]()
                {
                    bDirty |= ImGui::DragFloat(
                        "##SSGIThickness",
                        &mPending.ScreenSpaceThickness,
                        0.005f,
                        0.001f,
                        1.0f,
                        "%.3f m");
                });
                ImGuiWrapper::PropertyRow("Step Count", [&]()
                {
                    int value = static_cast<int>(mPending.ScreenSpaceStepCount);
                    if (ImGui::SliderInt("##SSGISteps", &value, 8, 256))
                    {
                        mPending.ScreenSpaceStepCount = static_cast<uint32_t>(value);
                        bDirty = true;
                    }
                });
                ImGuiWrapper::PropertyRow("Hi-Z Start Mip", [&]()
                {
                    int value = static_cast<int>(mPending.ScreenSpaceHiZStartMip);
                    if (ImGui::SliderInt("##SSGIHiZ", &value, 0, 10))
                    {
                        mPending.ScreenSpaceHiZStartMip = static_cast<uint32_t>(value);
                        bDirty = true;
                    }
                });
                ImGuiWrapper::PropertyRow("Binary Refinement", [&]()
                {
                    int value = static_cast<int>(
                        mPending.ScreenSpaceBinaryRefinementSteps);
                    if (ImGui::SliderInt("##SSGIRefine", &value, 0, 8))
                    {
                        mPending.ScreenSpaceBinaryRefinementSteps =
                            static_cast<uint32_t>(value);
                        bDirty = true;
                    }
                });
                ImGuiWrapper::PropertyRow("Edge Fade", [&]()
                {
                    bDirty |= ImGui::SliderFloat(
                        "##SSGIEdgeFade",
                        &mPending.ScreenSpaceEdgeFade,
                        0.0f,
                        0.5f,
                        "%.3f");
                });
                ImGuiWrapper::PropertyRow("Temporal Weight", [&]()
                {
                    bDirty |= ImGui::SliderFloat(
                        "##SSGITemporal",
                        &mPending.ScreenSpaceTemporalWeight,
                        0.0f,
                        0.999f,
                        "%.3f");
                });
            }
        }

        if (ImGui::CollapsingHeader(
                "Anti-Aliasing",
                ImGuiTreeNodeFlags_DefaultOpen))
        {
            constexpr std::array<AntiAliasingMode, 5> modes = {
                AntiAliasingMode::None,
                AntiAliasingMode::Fxaa,
                AntiAliasingMode::Smaa,
                AntiAliasingMode::Taa,
                AntiAliasingMode::Msaa,
            };
            ImGuiWrapper::PropertyRow("Mode", [&]()
            {
                if (ImGui::BeginCombo(
                        "##AAMode",
                        ToString(mPendingAntiAliasing.Mode)))
                {
                    for (AntiAliasingMode mode : modes)
                    {
                        const bool bSelected =
                            mode == mPendingAntiAliasing.Mode;
                        if (ImGui::Selectable(ToString(mode), bSelected))
                        {
                            mPendingAntiAliasing.Mode = mode;
                            bAntiAliasingDirty = true;
                        }
                        if (bSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            });

            if (mPendingAntiAliasing.Mode == AntiAliasingMode::Msaa)
            {
                ImGuiWrapper::PropertyRow("MSAA Samples", [&]()
                {
                    constexpr std::array<uint32_t, 3> sampleCounts = {
                        2u, 4u, 8u
                    };
                    const std::string preview =
                        std::to_string(mPendingAntiAliasing.MsaaSamples) + "x";
                    if (ImGui::BeginCombo("##AAMsaaSamples", preview.c_str()))
                    {
                        for (uint32_t sampleCount : sampleCounts)
                        {
                            const std::string label =
                                std::to_string(sampleCount) + "x";
                            const bool bSelected = sampleCount ==
                                mPendingAntiAliasing.MsaaSamples;
                            if (ImGui::Selectable(label.c_str(), bSelected))
                            {
                                mPendingAntiAliasing.MsaaSamples = sampleCount;
                                bAntiAliasingDirty = true;
                            }
                            if (bSelected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                });
            }
            else if (mPendingAntiAliasing.Mode == AntiAliasingMode::Fxaa)
            {
                ImGuiWrapper::PropertyRow("Edge Threshold", [&]()
                {
                    bAntiAliasingDirty |= ImGui::SliderFloat(
                        "##FXAAEdgeThreshold",
                        &mPendingAntiAliasing.FxaaEdgeThreshold,
                        0.0312f,
                        0.333f,
                        "%.4f");
                });
                ImGuiWrapper::PropertyRow("Minimum Threshold", [&]()
                {
                    bAntiAliasingDirty |= ImGui::SliderFloat(
                        "##FXAAMinThreshold",
                        &mPendingAntiAliasing.FxaaEdgeThresholdMin,
                        0.0f,
                        0.125f,
                        "%.4f");
                });
                ImGuiWrapper::PropertyRow("Subpixel Quality", [&]()
                {
                    bAntiAliasingDirty |= ImGui::SliderFloat(
                        "##FXAASubpixel",
                        &mPendingAntiAliasing.FxaaSubpixelQuality,
                        0.0f,
                        1.0f,
                        "%.2f");
                });
            }
            else if (mPendingAntiAliasing.Mode == AntiAliasingMode::Smaa)
            {
                ImGuiWrapper::PropertyRow("Edge Threshold", [&]()
                {
                    bAntiAliasingDirty |= ImGui::SliderFloat(
                        "##SMAAEdgeThreshold",
                        &mPendingAntiAliasing.SmaaEdgeThreshold,
                        0.01f,
                        0.5f,
                        "%.3f");
                });
                ImGuiWrapper::PropertyRow("Search Steps", [&]()
                {
                    int value = static_cast<int>(
                        mPendingAntiAliasing.SmaaMaxSearchSteps);
                    if (ImGui::SliderInt("##SMAASearch", &value, 1, 32))
                    {
                        mPendingAntiAliasing.SmaaMaxSearchSteps =
                            static_cast<uint32_t>(value);
                        bAntiAliasingDirty = true;
                    }
                });
                ImGuiWrapper::PropertyRow("Diagonal Search", [&]()
                {
                    int value = static_cast<int>(
                        mPendingAntiAliasing.SmaaMaxDiagonalSearchSteps);
                    if (ImGui::SliderInt("##SMAADiagonal", &value, 0, 16))
                    {
                        mPendingAntiAliasing.SmaaMaxDiagonalSearchSteps =
                            static_cast<uint32_t>(value);
                        bAntiAliasingDirty = true;
                    }
                });
                ImGuiWrapper::PropertyRow("Corner Rounding", [&]()
                {
                    bAntiAliasingDirty |= ImGui::SliderFloat(
                        "##SMAACorners",
                        &mPendingAntiAliasing.SmaaCornerRounding,
                        0.0f,
                        1.0f,
                        "%.2f");
                });
            }
            else if (mPendingAntiAliasing.Mode == AntiAliasingMode::Taa)
            {
                ImGuiWrapper::PropertyRow("History Weight", [&]()
                {
                    bAntiAliasingDirty |= ImGui::SliderFloat(
                        "##TAAHistoryWeight",
                        &mPendingAntiAliasing.TaaHistoryWeight,
                        0.0f,
                        0.999f,
                        "%.3f");
                });
                ImGuiWrapper::PropertyRow("Sharpness", [&]()
                {
                    bAntiAliasingDirty |= ImGui::SliderFloat(
                        "##TAASharpness",
                        &mPendingAntiAliasing.TaaSharpness,
                        0.0f,
                        1.0f,
                        "%.2f");
                });
            }

            ImGuiWrapper::PropertyRow("Capture SSAA (Tools)", [&]()
            {
                constexpr std::array<uint32_t, 3> scales = { 1u, 2u, 4u };
                const std::string preview = std::to_string(
                    mPendingAntiAliasing.CaptureSupersampleScale) + "x";
                if (ImGui::BeginCombo("##AACaptureScale", preview.c_str()))
                {
                    for (uint32_t scale : scales)
                    {
                        const std::string label = std::to_string(scale) + "x";
                        const bool bSelected = scale ==
                            mPendingAntiAliasing.CaptureSupersampleScale;
                        if (ImGui::Selectable(label.c_str(), bSelected))
                        {
                            mPendingAntiAliasing.CaptureSupersampleScale = scale;
                            bAntiAliasingDirty = true;
                        }
                        if (bSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            });
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Developer/offline capture policy only; it does not change "
                    "the live viewport render resolution.");
            }

            if (!application.GetActiveOfflineViewportRendererId().empty())
            {
                ImGui::TextWrapped(
                    "Offline Path Tracing uses stochastic subpixel sampling. "
                    "Realtime anti-aliasing is preserved as the raster setting "
                    "but bypassed while the offline renderer is active.");
            }

            if (ImGui::Button(
                    bAntiAliasingDirty ? "Apply Anti-Aliasing" : "AA Applied"))
            {
                try
                {
                    application.ApplyAntiAliasingSettings(
                        mPendingAntiAliasing);
                    mAppliedAntiAliasing =
                        application.GetAntiAliasingSettings();
                    mPendingAntiAliasing = mAppliedAntiAliasing;
                    mLastAntiAliasingApplyError.clear();
                    bAntiAliasingDirty = false;
                }
                catch (const std::exception& exception)
                {
                    mLastAntiAliasingApplyError = exception.what();
                }
            }
            ImGuiWrapper::SameLine(0.0f);
            if (ImGui::Button("Revert AA"))
            {
                mPendingAntiAliasing = mAppliedAntiAliasing;
                mLastAntiAliasingApplyError.clear();
                bAntiAliasingDirty = false;
            }
            if (!mLastAntiAliasingApplyError.empty())
            {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                    "%s",
                    mLastAntiAliasingApplyError.c_str());
            }

            const AntiAliasingSelection selection =
                application.GetAntiAliasingSelection();
            ImGui::Text(
                "Requested / Effective: %s / %s",
                ToString(selection.Requested.Mode),
                ToString(selection.Effective.Mode));
            ImGui::Text(
                "Fallback / Bypassed: %s / %s",
                selection.bFallback ? "yes" : "no",
                selection.bBypassed ? "yes" : "no");
            ImGui::TextWrapped("Reason: %s", selection.Reason.c_str());
        }

        ImGui::Separator();
        if (ImGui::Button(bDirty ? "Apply Pending Settings" : "Applied"))
        {
            try
            {
                application.ApplyGlobalIlluminationSettings(mPending);
                mApplied = application.GetGlobalIlluminationSettings();
                mPending = mApplied;
                mLastApplyError.clear();
                bDirty = false;
            }
            catch (const std::exception& exception)
            {
                mLastApplyError = exception.what();
            }
        }
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("Revert"))
        {
            mPending = mApplied;
            bDirty = false;
            mLastApplyError.clear();
        }
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("Reset History"))
            application.ResetGlobalIlluminationHistory();
        if (!mLastApplyError.empty())
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "%s",
                mLastApplyError.c_str());
        }

        if (ImGui::CollapsingHeader(
                "Status / Statistics",
                ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped(
                "Applied at a safe frame boundary to the main viewport and all live offscreen views.");
            const GlobalIlluminationSelection selection =
                application.GetGlobalIlluminationSelection();
            ImGui::Text("Requested: %u", static_cast<uint32_t>(selection.Requested));
            ImGui::Text("Selected: %u", static_cast<uint32_t>(selection.Selected));
            ImGui::Text("Fallback: %s", selection.bFallback ? "yes" : "no");
            ImGui::TextWrapped("Reason: %s", selection.Reason.c_str());
            ImGui::Text(
                "Hardware RT AS / Pipeline: %s / %s",
                capabilities.RayTracingAccelStruct.bEnabled ? "enabled" : "disabled",
                capabilities.RayTracingPipeline.bEnabled ? "enabled" : "disabled");
            if (ImGui::TreeNode("RenderGraph / GPU stats"))
            {
                const std::string dump = application.BuildRenderGraphDebugDump();
                ImGui::TextUnformatted(dump.c_str());
                ImGui::TreePop();
            }
        }
    }
}
