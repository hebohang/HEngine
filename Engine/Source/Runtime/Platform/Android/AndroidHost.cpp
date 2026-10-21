#include "AndroidContext.h"
#include "AndroidManagedHost.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Log/LogSession.h"

#include <android/native_window_jni.h>
#include <android/log.h>
#include <spdlog/sinks/android_sink.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <stdexcept>

int main(int argc, char** argv);

namespace HEngine
{
    namespace { std::atomic<AndroidManagedEntry> sManagedEntry = nullptr; }
    AndroidContext& GetAndroidContext() { static AndroidContext context; return context; }
    AndroidManagedEntry GetAndroidManagedEntry()
    {
        const auto entry = sManagedEntry.load();
        if (!entry) throw std::runtime_error("Android managed host has not registered the C# script entry");
        return entry;
    }
}

#define HE_ANDROID_EXPORT extern "C" __attribute__((visibility("default")))

HE_ANDROID_EXPORT int he_android_prepare()
{
    auto& state = HEngine::GetAndroidContext();
    std::lock_guard lock(state.Mutex);
    if (state.Prepared && !state.Finished) return 0;
    state.Prepared = true;
    state.Started = state.Finished = state.Stop = state.Resumed = state.TouchControls = false;
    state.Input = state.PendingInput = {};
    state.InputEvents.clear();
    state.Error.clear();
    state.WindowRevision = state.AppliedRevision = 0;
    return 1;
}

HE_ANDROID_EXPORT int he_android_error(char* output, int capacity)
{
    if (!output || capacity < 1) return 0;
    auto& state = HEngine::GetAndroidContext();
    std::lock_guard lock(state.Mutex);
    const auto size = std::min(state.Error.size(), static_cast<size_t>(capacity - 1));
    std::memcpy(output, state.Error.data(), size);
    output[size] = 0;
    return static_cast<int>(size);
}

HE_ANDROID_EXPORT int he_android_run(const char* contentRoot, const char* savedRoot,
    HEngine::AndroidManagedEntry entry)
{
    auto& state = HEngine::GetAndroidContext();
    {
        std::lock_guard lock(state.Mutex);
        if (!state.Prepared || state.Started || state.Stop || state.Finished || !contentRoot || !savedRoot || !entry) return -1;
        state.ContentRoot = contentRoot;
        state.SavedRoot = savedRoot;
        if (!state.ContentRoot.is_absolute() || !state.SavedRoot.is_absolute()) return -1;
        state.Started = true;
        HEngine::sManagedEntry.store(entry);
    }
    int result = -1;
    try
    {
        // Android has no terminal: keep engine diagnostics in the device log.
        static const bool loggingReady = [] {
            HEngine::Log::Init();
            auto sink = std::make_shared<spdlog::sinks::android_sink_mt>("HEngine");
            HEngine::AddLogSink(HEngine::Log::GetCoreLogger(), sink);
            HEngine::AddLogSink(HEngine::Log::GetClientLogger(), sink);
            return true;
        }();
        (void)loggingReady;
        char application[] = "hegame";
        char api[] = "--graphics-api=vulkan";
        char strict[] = "--strict-graphics-api";
        char* arguments[]{application, api, strict};
        result = main(3, arguments);
    }
    catch (const std::exception& error)
    {
        { std::lock_guard lock(state.Mutex); state.Error = error.what(); }
        __android_log_print(ANDROID_LOG_ERROR, "HEngine", "%s", error.what());
    }
    catch (...) { __android_log_write(ANDROID_LOG_ERROR, "HEngine", "Native host failed"); }
    {
        std::lock_guard lock(state.Mutex);
        state.Finished = true;
        if (state.PendingWindow) ANativeWindow_release(state.PendingWindow);
        state.PendingWindow = nullptr;
        state.Changed.notify_all();
    }
    return result;
}

HE_ANDROID_EXPORT void he_android_surface(JNIEnv* environment, jobject surface)
{
    auto* window = surface ? ANativeWindow_fromSurface(environment, surface) : nullptr;
    auto& state = HEngine::GetAndroidContext();
    std::unique_lock lock(state.Mutex);
    if (state.Finished)
    {
        if (window) ANativeWindow_release(window);
        return;
    }
    if (state.PendingWindow) ANativeWindow_release(state.PendingWindow);
    state.PendingWindow = window;
    const auto revision = ++state.WindowRevision;
    state.Input = state.PendingInput = {};
    state.InputEvents.clear();
    state.Changed.notify_all();
    // SurfaceView must not release its BufferQueue while Vulkan still owns it.
    if (!surface && state.Started)
        state.Changed.wait(lock, [&] { return state.AppliedRevision >= revision || state.Finished; });
}

HE_ANDROID_EXPORT void he_android_resume(int resumed)
{
    auto& state = HEngine::GetAndroidContext();
    std::lock_guard lock(state.Mutex);
    state.Resumed = resumed != 0;
    if (!state.Resumed)
    {
        state.Input = state.PendingInput = {};
        state.InputEvents.clear();
    }
    state.Changed.notify_all();
}

HE_ANDROID_EXPORT void he_android_stop()
{
    auto& state = HEngine::GetAndroidContext();
    std::lock_guard lock(state.Mutex);
    state.Stop = true;
    if (!state.Started)
    {
        state.Finished = true;
        if (state.PendingWindow) ANativeWindow_release(state.PendingWindow);
        state.PendingWindow = nullptr;
    }
    state.Input = state.PendingInput = {};
    state.InputEvents.clear();
    state.Changed.notify_all();
}

HE_ANDROID_EXPORT int he_android_touch_controls()
{
    auto& state = HEngine::GetAndroidContext();
    std::lock_guard lock(state.Mutex);
    return state.TouchControls;
}

HE_ANDROID_EXPORT void he_android_input(float x, float y, float moveX, float moveY, int buttons)
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(moveX) || !std::isfinite(moveY)) return;
    auto& state = HEngine::GetAndroidContext();
    std::lock_guard lock(state.Mutex);
    if (!state.Resumed || state.Stop) return;
    state.PendingInput = {x, y, std::clamp(moveX, -1.f, 1.f), std::clamp(moveY, -1.f, 1.f),
        (buttons & 1) != 0, (buttons & 2) != 0, (buttons & 4) != 0, (buttons & 8) != 0};
    // Keep button edges even for taps shorter than a render frame. On overflow
    // cancel the gesture so a pathological event stream cannot leave keys held.
    if (state.InputEvents.size() >= 256)
    {
        state.InputEvents.clear();
        state.PendingInput = {};
    }
    state.InputEvents.push_back(state.PendingInput);
}
