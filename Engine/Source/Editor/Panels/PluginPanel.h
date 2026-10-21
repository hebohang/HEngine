#pragma once

#include "BasePanel.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Platform/PlatformProcess.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace HEngine
{
    class PluginPanel final : public BasePanel
    {
        REGISTER_PANEL(PluginPanel)
    public:
        PluginPanel(EditorLayer* parent, std::string name, bool* open);
        ~PluginPanel() override;
        void OnImGuiRender() override;
    protected:
        void OnImGuiRenderImpl() override;
    private:
        void Reload();
        void Apply();
        std::vector<Json> mManifests;
        Json mPending;
        Json mOriginal;
        std::string mOriginalText;
        std::string mMessage;
        int mProfile = 0;
        bool bInitialized = false;
        std::atomic<bool> bRunning = false;
        std::atomic<bool> bFinished = false;
        PlatformProcess::CapturedResult mResult;
        std::mutex mMutex;
        std::jthread mWorker;
    };
}
