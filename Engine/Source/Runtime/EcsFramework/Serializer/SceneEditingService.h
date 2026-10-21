#pragma once

#include "Runtime/Core/Meta/Json.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace HEngine
{
    // Main-thread authoring session. The host ECS is authoritative when a writer
    // is bound; mDocument is its serialized observation, never a simulation world.
    class SceneEditingService final
    {
    public:
        using Writer = std::function<void(const Json& entityValues)>;
        using ChangeObserver = std::function<void(uint64_t revision)>;
        void SetChangeObserver(ChangeObserver observer) { mChangeObserver = std::move(observer); }
        void Open(const std::filesystem::path& projectRoot, const std::string& path);
        // A freshly deserialized host snapshot includes reflected defaults and
        // load-time normalization. It is the clean baseline, while the original
        // disk document remains the independent external-write conflict token.
        void Bind(const std::filesystem::path& projectRoot, const std::string& path,
            Json document, Writer writer = {}, bool bLoadedFromDisk = false);
        Json Execute(const std::string& method, const Json& request);
        // Called at the host's authoring boundary, never with a runtime snapshot.
        // Known UI edits join history; unannounced edits invalidate history.
        void Observe(Json document, bool knownEdit);
        // Explicit Play-only transaction. Copies registered value fields onto
        // existing author entities; never copies components, objects or handles.
        Json ApplyPlayScriptFields(const Json& runtimeDocument);
        void SetPlaying(bool playing) { mPlaying = playing; }
        const std::string& GetSceneId() const { return mSceneId; }
        uint64_t GetRevision() const { return mRevision; }
        bool IsDirty() const;
        bool CanUndo() const { return mCursor > 0 && !mBlocked; }
        bool CanRedo() const { return mCursor < mHistory.size() && !mBlocked; }
        const Json& GetDocument() const { return mDocument; }
        static Json ComponentDefaults(const std::string& component);

    private:
        struct Change { std::string Id; Json Before, After; uint64_t Revision = 0; size_t Bytes = 0; };
        Json Receipt() const;
        Json Prepare(const Json& request, Json& after, Json& temporaryIds) const;
        Json Commit(Json before, Json after, bool write);
        void Write(const Json& values, const Json& rollback);
        void Publish(Change change, bool write);
        void CheckThread() const;
        void NotifyChanged() noexcept;
        std::filesystem::path mRoot, mPath, mEngineContentRoot;
        std::filesystem::file_time_type mDiskWriteTime{};
        std::string mSceneId, mProjectPath;
        Json mDocument = Json::object(), mDiskDocument, mSavedDocument;
        Writer mWriter;
        ChangeObserver mChangeObserver;
        uint64_t mRevision = 0, mSavedRevision = 0, mDeltaFloor = 0;
        std::vector<Change> mHistory;
        std::deque<Change> mEvents;
        size_t mCursor = 0, mHistoryBytes = 0, mEventBytes = 0;
        std::map<std::string, std::pair<std::string, Json>> mRequests;
        std::map<std::string, Json> mPrefabPlans;
        std::thread::id mThread;
        bool mPlaying = false, mBlocked = false, mDiskExists = false, mNotificationFailed = false, mNotifying = false;
        mutable bool bDirtyCacheValid = false, bCachedDirty = false;
    };
}
