#pragma once

#include "Runtime/Platform/PlatformProcess.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <stop_token>
#include <string>

namespace HEngine
{
    // Project-owned profiles; platform build implementations live in Tools/Build.
    class BuildPublishPanel
    {
    public:
        ~BuildPublishPanel();
        void Open();
        void Draw();
        void Shutdown();
    private:
        void Load();
        bool Save();
        void Start(const std::string& action);
        void Preview();
        void Poll();
        bool Busy() const { return mBuild.valid(); }
        std::filesystem::path Root() const;
        bool mOpen = false;
        bool mLoaded = false;
        bool mBuildAndPreview = false;
        int mSelected = 0;
        Json mConfig;
        std::string mStatus;
        std::string mLog;
        std::filesystem::path mLogPath;
        std::string mRunningProfile;
        std::future<PlatformProcess::CapturedResult> mBuild;
        std::future<PlatformProcess::CapturedResult> mPreview;
        std::stop_source mBuildStop;
        std::stop_source mPreviewStop;
        std::chrono::steady_clock::time_point mNextLogRead{};
    };
}
