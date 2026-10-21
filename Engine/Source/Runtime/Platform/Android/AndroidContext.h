#pragma once

#include <android/native_window.h>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <deque>
#include <mutex>

namespace HEngine
{
    struct AndroidInputSnapshot
    {
        float X = 0, Y = 0, MoveX = 0, MoveY = 0;
        bool Primary = false, Secondary = false, Jump = false, Pause = false;
    };
    struct AndroidContext
    {
        std::mutex Mutex;
        std::condition_variable Changed;
        ANativeWindow* PendingWindow = nullptr;
        std::uint64_t WindowRevision = 0, AppliedRevision = 0;
        bool Resumed = false, Stop = false, Finished = false, Started = false, Prepared = false;
        bool TouchControls = false;
        AndroidInputSnapshot Input;
        AndroidInputSnapshot PendingInput;
        std::deque<AndroidInputSnapshot> InputEvents;
        std::string Error;
        std::filesystem::path ContentRoot, SavedRoot;
    };
    AndroidContext& GetAndroidContext();
}
