#pragma once

#include "Runtime/Core/Meta/Json.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace HEngine
{
    // Explicit bounded Editor validation only; never a shipping/runtime service.
    class EditorSceneMailbox
    {
    public:
        void Initialize(bool boundedRun);
        void Poll(const std::function<void(const std::filesystem::path&)>& openScene,
            const std::function<Json(const Json&)>& preview,
            const std::function<Json(const Json&)>& material);
        [[nodiscard]] bool IsVfxDiagnosticActive() const { return mVfxActive; }
        [[nodiscard]] bool HasVfxStep() const { return mVfxActive && mVfxRemaining > 0; }
        void SetVfxSceneContext(const void* scene, bool pending);
        void BeginVfxFrame(const void* scene);
        void CompleteVfxFrame(const void* scene);
        bool ShutdownVfx() noexcept;
    private:
        void FinishVfxRetirement();
        Json VfxRequest(const Json& params);
        Json VfxState() const;
        const void* mVfxCurrentScene = nullptr;
        const void* mVfxOwnedScene = nullptr;
        bool mVfxScenePending = false;
        bool mVfxRetiring = false;
        bool mVfxRetired = false;
        bool mVfxActive = false;
        bool mVfxCleanupFailed = false;
        bool mVfxSavedRealtime = false;
        uint32_t mVfxRemaining = 0;
        uint32_t mVfxTotalFrames = 0;
        uint64_t mVfxTicket = 0;
        uint64_t mVfxCompletedTicket = 0;
        uint64_t mVfxUpdateBefore = 0;
        std::set<int32_t> mVfxHandles;
        std::filesystem::path CheckedScenePath(const std::string& path) const;
        std::filesystem::path mDirectory;
        std::filesystem::path mSceneRoot;
        std::string mToken;
        std::map<std::string, std::string> mProcessed;
        std::filesystem::path mClaimedRequest;
        std::filesystem::path mClaimCandidate;
        unsigned mClaimRetries = 0;
        unsigned mOpenRetries = 0;
        uint64_t mCaptureTicket = 0;
        uint64_t mCaptureDueFrame = 0;
        std::filesystem::path mCapturePath;
        Json mCaptureReceipt;
    };
}
