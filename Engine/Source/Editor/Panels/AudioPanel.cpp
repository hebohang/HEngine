#include "AudioPanel.h"

#include "Editor/EditorLayer.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Audio/ProjectAudioPath.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/StringUtils/StringUtils.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace HEngine
{
    AudioPanel::AudioPanel(EditorLayer* parent, std::string audioPath)
        : BasePanel(parent, audioPath, &mOpen), mAudioPath(std::move(audioPath))
    {
    }

    AudioPanel::~AudioPanel()
    {
        StopPlayback();
    }

    void AudioPanel::StopPlayback()
    {
        if (mHandle)
            AudioEngine::GetInstance().Stop(mHandle);
        mHandle = {};
        bPlaying = false;
    }

    void AudioPanel::OnImGuiRender()
    {
        if (!(*pOpen))
        {
            StopPlayback();
            return;
        }

        if (!ImGui::Begin(Utils::WStringToAnsi(std::filesystem::path(mName).wstring()).c_str(), pOpen))
        {
            ImGui::End();
            return;
        }

        OnImGuiRenderImpl();
        ImGui::End();
    }

    void AudioPanel::OnImGuiRenderImpl()
    {
        auto& audio = AudioEngine::GetInstance();

        ImGui::Columns(2);

        static bool bInitializeColumns = true;
        static float sLastUIScale = -1.0f;
        if (bInitializeColumns || ImGuiWrapper::HasScaleChanged(sLastUIScale))
        {
            ImGuiWrapper::SetColumnWidth(0, 280.0f);
            bInitializeColumns = false;
        }

        if (ImGui::BeginChild("AUDIO_VIEW"))
        {
            auto replay = [this, &audio]()
            {
                if (mHandle)
                    audio.Stop(mHandle);

                AudioPlayParams params;
                params.Bus = AudioBus::UI;
                ProjectAudioPath audioPath;
                const Utils::PathValidationResult pathResult = TryResolveProjectAudioPath(
                    ConfigManager::GetInstance().GetRootFolder(),
                    mAudioPath,
                    audioPath);
                if (!pathResult)
                {
                    HE_CORE_WARN("Rejected editor audio path '{0}': {1}",
                        mAudioPath, pathResult.Message);
                    mHandle = {};
                }
                else
                {
                    mHandle = audio.Play(audioPath.FullPath, params);
                }
                bPlaying = static_cast<bool>(mHandle);
                if (!mHandle)
                {
                    mLengthSeconds = 0.0f;
                    progressVal = 0.0f;
                    return;
                }

                mLengthSeconds = static_cast<float>(audio.GetLength(mHandle));
                if (progressVal > 0.0f)
                    audio.Seek(mHandle, progressVal);
            };

            if (!bPlaying)
            {
                const std::string playLabel = std::string(EditorLayer::GetText("Play")) + "###AudioPlayback";
                if (ImGui::Button(playLabel.c_str()))
                    replay();
            }
            else
            {
                const std::string stopLabel = std::string(EditorLayer::GetText("Stop")) + "###AudioPlayback";
                if (ImGui::Button(stopLabel.c_str()))
                {
                    StopPlayback();
                }
            }

            if (mHandle)
            {
                progressVal = static_cast<float>(audio.GetPlaybackPosition(mHandle));
                if (!audio.IsPlaying(mHandle) && !audio.IsPaused(mHandle))
                {
                    progressVal = 0.0f;
                    mHandle = {};
                    bPlaying = false;
                }
            }

            const float sliderMaximum = std::max(mLengthSeconds, 0.01f);
            ImGui::SliderFloat("##AudioProgress", &progressVal, 0.0f, sliderMaximum, "%.2f s");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                if (mHandle)
                    audio.Seek(mHandle, progressVal);
                else
                    replay();
            }
        }
        ImGui::EndChild();

        ImGui::NextColumn();

        if (ImGui::BeginChild("AUDIO_INFO"))
        {
            const std::string filename = std::filesystem::path(mAudioPath).filename().string();
            ImGui::TextWrapped("%s", filename.c_str());
            ImGui::Separator();
            ImGui::Text("%s: %.2f s", EditorLayer::GetText("Length"), mLengthSeconds);
            ImGui::Text("%s: %.2f s", EditorLayer::GetText("Position"), progressVal);
            ImGui::Text("%s: %s", EditorLayer::GetText("Output"),
                EditorLayer::GetText(audio.IsDeviceAvailable() ? "Available" : "Silent fallback"));

            const float fraction = mLengthSeconds > 0.0f
                ? std::clamp(progressVal / mLengthSeconds, 0.0f, 1.0f)
                : 0.0f;
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));
        }
        ImGui::EndChild();

        ImGui::Columns(1);
    }
}
