#pragma once 

#include "BasePanel.h"
#include "Runtime/Audio/AudioTypes.h"

#include <string>

namespace HEngine
{
    class AudioPanel : public BasePanel
    {
    public:
        AudioPanel(EditorLayer* parent, std::string audioPath);
        ~AudioPanel();

        virtual void OnImGuiRender() override;
        void OnImGuiRenderImpl() override;

        bool mOpen = true;
    private:
        void StopPlayback();

        bool bPlaying = false;
        float progressVal = 0.0f;
        float mLengthSeconds = 0.0f;
        std::string mAudioPath;
        AudioHandle mHandle{};
    };
}
