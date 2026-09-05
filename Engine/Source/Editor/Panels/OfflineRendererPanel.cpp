#include "OfflineRendererPanel.h"

#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace HEngine
{
    namespace
    {
        [[nodiscard]] const char* AovName(OfflineViewportAov aov) noexcept
        {
            switch (aov)
            {
            case OfflineViewportAov::Beauty: return "Beauty";
            case OfflineViewportAov::DirectDiffuse: return "Direct Diffuse";
            case OfflineViewportAov::IndirectDiffuse: return "Indirect Diffuse";
            case OfflineViewportAov::Albedo: return "Albedo";
            case OfflineViewportAov::WorldNormal: return "World Normal";
            case OfflineViewportAov::ViewDepth: return "View Depth";
            case OfflineViewportAov::MaterialId: return "Material ID";
            case OfflineViewportAov::InstanceId: return "Instance ID";
            case OfflineViewportAov::SampleCount: return "Sample Count";
            }
            return "Unknown";
        }

        [[nodiscard]] const char* SessionStateName(
            OfflineViewportSessionState state) noexcept
        {
            switch (state)
            {
            case OfflineViewportSessionState::Idle: return "Idle";
            case OfflineViewportSessionState::Rendering: return "Rendering";
            case OfflineViewportSessionState::Paused: return "Paused";
            case OfflineViewportSessionState::Complete: return "Complete";
            case OfflineViewportSessionState::Cancelled: return "Cancelled";
            case OfflineViewportSessionState::Failed: return "Failed";
            }
            return "Unknown";
        }

        [[nodiscard]] bool RequiresRawOutput(OfflineViewportAov aov) noexcept
        {
            return aov == OfflineViewportAov::MaterialId ||
                aov == OfflineViewportAov::InstanceId ||
                aov == OfflineViewportAov::SampleCount;
        }

        [[nodiscard]] bool IsSafeFileName(std::string_view value) noexcept
        {
            return !value.empty() && std::ranges::all_of(
                value,
                [](unsigned char character)
                {
                    return std::isalnum(character) != 0 || character == '-' ||
                        character == '_' || character == '.';
                });
        }
    }

    OfflineRendererPanel::OfflineRendererPanel(
        EditorLayer* parent,
        std::string panelName,
        bool* pOpen)
        : BasePanel(parent, std::move(panelName), pOpen)
    {
    }

    void OfflineRendererPanel::InitializeFromSession(
        IOfflineViewportRendererSession& session)
    {
        mProviderId = std::string(session.GetProviderId());
        mApplied = session.GetOfflineSettings();
        mPending = mApplied;
        if (mPending.OutputDirectory.empty())
        {
            mPending.OutputDirectory =
                ConfigManager::GetSavedPath("OfflineRenders");
        }
        std::snprintf(
            mOutputDirectory.data(),
            mOutputDirectory.size(),
            "%s",
            mPending.OutputDirectory.string().c_str());
        std::snprintf(
            mOutputFileName.data(),
            mOutputFileName.size(),
            "%s",
            mPending.OutputFileName.c_str());
        mStatus.clear();
        mExport = {};
        bInitialized = true;
        bDirty = false;
    }

    void OfflineRendererPanel::PollExport(
        IOfflineViewportRendererSession& session)
    {
        if (!mExport || !session.IsExportValid(mExport))
            return;
        const TextureReadbackResult result = session.GetExportResult(mExport);
        if (result.Status == TextureReadbackStatus::Complete)
        {
            mStatus = "Export complete: " + result.OutputPath.string();
            session.ReleaseExport(mExport);
            mExport = {};
        }
        else if (result.Status == TextureReadbackStatus::Failed)
        {
            mStatus = "Export failed: " + result.Error;
            session.ReleaseExport(mExport);
            mExport = {};
        }
    }

    void OfflineRendererPanel::OnImGuiRenderImpl()
    {
        Application& application = Application::GetInstance();
        IOfflineViewportRendererSession* session =
            application.GetOfflineViewportRendererSession();
        if (!session)
        {
            bInitialized = false;
            mProviderId.clear();
            mExport = {};
            const std::vector<OfflineViewportRendererProviderDescriptor>
                providers = OfflineViewportRendererProviderRegistry::Get().Enumerate();
            if (providers.empty())
            {
                ImGui::TextWrapped(
                    "No offline viewport renderer is registered. The developer plugin is disabled or unavailable in this host.");
                return;
            }
            ImGui::TextWrapped(
                "Select an available offline viewport mode to create a render session.");
            const RenderCapabilities capabilities =
                application.GetRenderCapabilities();
            for (const OfflineViewportRendererProviderDescriptor& provider :
                providers)
            {
                const OfflineViewportRendererSupport support =
                    OfflineViewportRendererProviderRegistry::Get().QuerySupport(
                        provider.Id,
                        capabilities);
                ImGui::BulletText(
                    "%s: %s",
                    provider.DisplayName.c_str(),
                    support.Reason.c_str());
            }
            return;
        }
        if (!bInitialized || mProviderId != session->GetProviderId())
            InitializeFromSession(*session);
        PollExport(*session);

        ImGuiWrapper::PropertyRow("Target Samples", [&]()
        {
            int value = static_cast<int>(mPending.TargetSamples);
            if (ImGui::InputInt("##OfflineTargetSamples", &value))
            {
                mPending.TargetSamples = static_cast<uint32_t>(
                    std::clamp(value, 1, 1048576));
                bDirty = true;
            }
        });
        ImGuiWrapper::PropertyRow("Samples Per Frame", [&]()
        {
            int value = static_cast<int>(mPending.SamplesPerFrame);
            if (ImGui::SliderInt("##OfflineSamplesPerFrame", &value, 1, 64))
            {
                mPending.SamplesPerFrame = static_cast<uint32_t>(value);
                bDirty = true;
            }
        });
        ImGuiWrapper::PropertyRow("Max Bounces", [&]()
        {
            int value = static_cast<int>(mPending.MaxBounces);
            if (ImGui::SliderInt("##OfflineMaxBounces", &value, 0, 16))
            {
                mPending.MaxBounces = static_cast<uint32_t>(value);
                bDirty = true;
            }
        });
        ImGuiWrapper::PropertyRow("Max Ray Distance", [&]()
        {
            bDirty |= ImGui::DragFloat(
                "##OfflineMaxRayDistance",
                &mPending.MaximumRayDistance,
                1.0f,
                0.001f,
                100000.0f,
                "%.2f m");
        });
        ImGuiWrapper::PropertyRow("Render Scale", [&]()
        {
            bDirty |= ImGui::SliderFloat(
                "##OfflineRenderScale",
                &mPending.RenderScale,
                0.25f,
                2.0f,
                "%.2f");
        });
        ImGuiWrapper::PropertyRow("Fixed Resolution", [&]()
        {
            int width = static_cast<int>(mPending.FixedWidth);
            int height = static_cast<int>(mPending.FixedHeight);
            ImGui::SetNextItemWidth(ImGuiWrapper::S(90.0f));
            if (ImGui::InputInt("##OfflineWidth", &width))
            {
                mPending.FixedWidth = static_cast<uint32_t>(
                    std::clamp(width, 0, 16384));
                bDirty = true;
            }
            ImGuiWrapper::SameLine(0.0f);
            ImGui::TextUnformatted("x");
            ImGuiWrapper::SameLine(0.0f);
            ImGui::SetNextItemWidth(ImGuiWrapper::S(90.0f));
            if (ImGui::InputInt("##OfflineHeight", &height))
            {
                mPending.FixedHeight = static_cast<uint32_t>(
                    std::clamp(height, 0, 16384));
                bDirty = true;
            }
        });
        ImGuiWrapper::PropertyRow("Lock Aspect Ratio", [&]()
        {
            bDirty |= ImGui::Checkbox(
                "##OfflineLockAspect",
                &mPending.bLockAspectRatio);
        });
        ImGuiWrapper::PropertyRow("Seed", [&]()
        {
            int mode = mPending.SeedMode == OfflineViewportSeedMode::Fixed
                ? 0
                : 1;
            const char* modes[] = { "Fixed", "Random" };
            ImGui::SetNextItemWidth(ImGuiWrapper::S(90.0f));
            if (ImGui::Combo("##OfflineSeedMode", &mode, modes, 2))
            {
                mPending.SeedMode = mode == 0
                    ? OfflineViewportSeedMode::Fixed
                    : OfflineViewportSeedMode::Random;
                bDirty = true;
            }
            ImGuiWrapper::SameLine(0.0f);
            ImGui::BeginDisabled(mode != 0);
            int seed = static_cast<int>(mPending.Seed);
            ImGui::SetNextItemWidth(ImGuiWrapper::S(120.0f));
            if (ImGui::InputInt("##OfflineSeed", &seed))
            {
                mPending.Seed = static_cast<uint32_t>(seed);
                bDirty = true;
            }
            ImGui::EndDisabled();
        });
        ImGuiWrapper::PropertyRow("Scene Environment", [&]()
        {
            bDirty |= ImGui::Checkbox(
                "##OfflineSceneEnvironment",
                &mPending.bUseSceneEnvironment);
        });
        ImGuiWrapper::PropertyRow("Environment Radiance", [&]()
        {
            ImGui::BeginDisabled(mPending.bUseSceneEnvironment);
            bDirty |= ImGui::DragFloat3(
                "##OfflineEnvironmentRadiance",
                &mPending.EnvironmentRadiance.x,
                0.01f,
                0.0f,
                1000.0f,
                "%.3f");
            ImGui::EndDisabled();
        });
        ImGuiWrapper::PropertyRow("Direct Only", [&]()
        {
            bDirty |= ImGui::Checkbox(
                "##OfflineDirectOnly",
                &mPending.bDirectOnly);
        });
        ImGuiWrapper::PropertyRow("Auto Reset", [&]()
        {
            bDirty |= ImGui::Checkbox(
                "##OfflineAutoReset",
                &mPending.bAutoReset);
        });
        ImGuiWrapper::PropertyRow("Preview AOV", [&]()
        {
            if (ImGui::BeginCombo(
                    "##OfflineAov",
                    AovName(mPending.PreviewAov)))
            {
                for (OfflineViewportAov aov : session->GetAvailableAovs())
                {
                    const bool bSelected = aov == mPending.PreviewAov;
                    if (ImGui::Selectable(AovName(aov), bSelected))
                    {
                        mPending.PreviewAov = aov;
                        if (RequiresRawOutput(aov))
                        {
                            mPending.OutputFormat =
                                OfflineViewportOutputFormat::Raw;
                        }
                        else if (aov != OfflineViewportAov::Beauty &&
                            mPending.OutputFormat ==
                                OfflineViewportOutputFormat::ToneMappedPng)
                        {
                            mPending.OutputFormat =
                                OfflineViewportOutputFormat::Pfm;
                        }
                        bDirty = true;
                    }
                }
                ImGui::EndCombo();
            }
        });

        ImGui::Separator();
        if (ImGui::Button(bDirty ? "Apply Pending Settings" : "Applied"))
        {
            try
            {
                mPending.OutputDirectory = std::filesystem::path(
                    mOutputDirectory.data());
                mPending.OutputFileName = mOutputFileName.data();
                session->ApplyOfflineSettings(mPending);
                mApplied = session->GetOfflineSettings();
                mPending = mApplied;
                bDirty = false;
                mStatus.clear();
            }
            catch (const std::exception& exception)
            {
                mStatus = exception.what();
            }
        }
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("Revert"))
        {
            mPending = mApplied;
            std::snprintf(
                mOutputDirectory.data(),
                mOutputDirectory.size(),
                "%s",
                mPending.OutputDirectory.string().c_str());
            std::snprintf(
                mOutputFileName.data(),
                mOutputFileName.size(),
                "%s",
                mPending.OutputFileName.c_str());
            bDirty = false;
        }

        if (ImGui::Button("Pause"))
            session->Pause();
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("Resume"))
            session->Resume();
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("Reset / Render"))
            session->Restart();
        ImGuiWrapper::SameLine(0.0f);
        if (ImGui::Button("Cancel"))
            session->Cancel();

        ImGui::Separator();
        ImGuiWrapper::PropertyRow("Output Directory", [&]()
        {
            if (ImGui::InputText(
                    "##OfflineOutputDirectory",
                    mOutputDirectory.data(),
                    mOutputDirectory.size()))
            {
                bDirty = true;
            }
        });
        ImGuiWrapper::PropertyRow("File Name", [&]()
        {
            if (ImGui::InputText(
                    "##OfflineFileName",
                    mOutputFileName.data(),
                    mOutputFileName.size()))
            {
                bDirty = true;
            }
        });
        ImGuiWrapper::PropertyRow("Output Format", [&]()
        {
            int format = 0;
            if (mPending.OutputFormat ==
                OfflineViewportOutputFormat::ToneMappedPng)
            {
                format = 1;
            }
            else if (mPending.OutputFormat ==
                OfflineViewportOutputFormat::Raw)
            {
                format = 2;
            }
            const char* formats[] = {
                "Linear PFM",
                "Tone-mapped PNG Preview",
                "Raw",
            };
            ImGui::BeginDisabled(RequiresRawOutput(mPending.PreviewAov));
            if (ImGui::Combo("##OfflineOutputFormat", &format, formats, 3))
            {
                mPending.OutputFormat = format == 0
                    ? OfflineViewportOutputFormat::Pfm
                    : format == 1
                        ? OfflineViewportOutputFormat::ToneMappedPng
                        : OfflineViewportOutputFormat::Raw;
                if (mPending.OutputFormat ==
                        OfflineViewportOutputFormat::ToneMappedPng &&
                    mPending.PreviewAov != OfflineViewportAov::Beauty)
                {
                    mPending.OutputFormat = OfflineViewportOutputFormat::Pfm;
                    mStatus = "Tone-mapped PNG is available only for Beauty.";
                }
                bDirty = true;
            }
            ImGui::EndDisabled();
        });
        ImGui::BeginDisabled(static_cast<bool>(mExport));
        if (ImGui::Button("Export Selected AOV"))
        {
            try
            {
                if (!IsSafeFileName(mOutputFileName.data()))
                    throw std::invalid_argument("Output file name is unsafe");
                const std::filesystem::path outputDirectory(
                    mOutputDirectory.data());
                std::filesystem::create_directories(outputDirectory);
                const char* extension = mPending.OutputFormat ==
                        OfflineViewportOutputFormat::Raw
                    ? ".raw"
                    : mPending.OutputFormat ==
                            OfflineViewportOutputFormat::ToneMappedPng
                        ? ".png"
                        : ".pfm";
                mExport = session->RequestExport(
                    mPending.PreviewAov,
                    outputDirectory /
                        (std::string(mOutputFileName.data()) + "-" +
                            AovName(mPending.PreviewAov) + extension),
                    mPending.OutputFormat);
                mStatus = "Export queued.";
            }
            catch (const std::exception& exception)
            {
                mStatus = exception.what();
            }
        }
        ImGui::EndDisabled();

        const OfflineViewportRendererProgress progress =
            session->GetOfflineProgress();
        ImGui::Separator();
        const std::string_view providerId = session->GetProviderId();
        ImGui::Text(
            "Provider: %.*s",
            static_cast<int>(providerId.size()),
            providerId.data());
        const OfflineViewportRendererSupport support =
            OfflineViewportRendererProviderRegistry::Get().QuerySupport(
                providerId,
                application.GetRenderCapabilities());
        ImGui::TextWrapped("Support: %s", support.Reason.c_str());
        ImGui::Text("State: %s", SessionStateName(progress.State));
        ImGui::Text(
            "Samples: %llu / %llu",
            static_cast<unsigned long long>(progress.AccumulatedSamples),
            static_cast<unsigned long long>(progress.TargetSamples));
        const float fraction = progress.TargetSamples > 0
            ? static_cast<float>(progress.AccumulatedSamples) /
                static_cast<float>(progress.TargetSamples)
            : 0.0f;
        ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f));
        ImGui::Text("Resolution: %u x %u", progress.Width, progress.Height);
        ImGui::Text("Elapsed: %.2f s", progress.ElapsedSeconds);
        ImGui::Text(
            "Last GPU pass body: %.3f ms",
            progress.LastGpuMilliseconds);
        ImGui::Text(
            "Last reset mask: 0x%08x",
            static_cast<uint32_t>(progress.LastResetReason));
        ImGui::Text(
            "Scene / Camera invalidated: %s / %s",
            progress.bSceneInvalidated ? "yes" : "no",
            progress.bCameraInvalidated ? "yes" : "no");
        if (!progress.Error.empty())
            ImGui::TextWrapped("Session error: %s", progress.Error.c_str());
        if (!mStatus.empty())
            ImGui::TextWrapped("%s", mStatus.c_str());
    }
}
