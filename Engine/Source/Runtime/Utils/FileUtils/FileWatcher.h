#pragma once
#include "Runtime/Events/Delegate.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <ratio>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HEngine
{
    class FileWatcher
    {
        DECLARE_MULTICAST_DELEGATE(FileModifiedEvent, const std::filesystem::path&);
    public:
        FileWatcher() = default;
        ~FileWatcher()
        {
            StopWorker();

            std::scoped_lock lock(mMutex);
            mFileModifiedEvent.RemoveAll();
            mWatchingFiles.clear();
            mWatchingDirectories.clear();
            mSpeciCallBacks.clear();
            mPendingModifiedFiles.clear();
            mPendingModifiedLookup.clear();
        }

        // path can be Directory
        void AddFile(const std::filesystem::path& path, bool recursive = true)
        {
            RegisterWatchPath(path, recursive);
            StartWorkerIfNeeded();
        }

        // path can only be a file
        void AddCallBack(const std::filesystem::path& path, std::function<void(const std::filesystem::path&)> func)
        {
            AddFile(path, false);

            const std::filesystem::path normalizedPath = NormalizeWatchPath(path);
            std::scoped_lock lock(mMutex);
            mSpeciCallBacks[normalizedPath].push_back(std::move(func));
        }

        void Update()
        {
            std::vector<std::filesystem::path> modifiedFiles;
            {
                std::scoped_lock lock(mMutex);
                modifiedFiles.swap(mPendingModifiedFiles);
                mPendingModifiedLookup.clear();
            }

            for (const auto& file : modifiedFiles)
            {
                mFileModifiedEvent.Broadcast(file);

                std::vector<std::function<void(const std::filesystem::path&)>> specificCallbacks;
                {
                    std::scoped_lock lock(mMutex);
                    if (auto it = mSpeciCallBacks.find(file); it != mSpeciCallBacks.end())
                        specificCallbacks = it->second;
                }

                for (const auto& func : specificCallbacks)
                    func(file);
            }
        }

        FileModifiedEvent& GetFileModifiedEvent() { return mFileModifiedEvent; }
    private:
        using WatchFileMap = std::unordered_map<std::filesystem::path, std::filesystem::file_time_type>;
        using WatchDirectoryList = std::vector<std::pair<std::filesystem::path, bool>>;

        struct ScanSnapshot
        {
            WatchFileMap watchingFiles;
            WatchDirectoryList watchingDirectories;
        };

        struct ScanResult
        {
            WatchFileMap discoveredFiles;
            WatchFileMap modifiedFiles;
            std::vector<std::filesystem::path> removedFiles;
        };

        static std::filesystem::path NormalizeWatchPath(const std::filesystem::path& path)
        {
            std::error_code ec;
            const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
            if (!ec)
                return canonicalPath;

            return path.lexically_normal();
        }

        void StartWorkerIfNeeded()
        {
            std::scoped_lock lock(mMutex);
            if (mWorkerThread.joinable())
                return;

            mStopRequested = false;
            mScanRequested = true;
            mWorkerThread = std::thread([this]() { WorkerLoop(); });
        }

        void StopWorker()
        {
            {
                std::scoped_lock lock(mMutex);
                mStopRequested = true;
                mScanRequested = true;
            }

            mWakeCondition.notify_all();
            if (mWorkerThread.joinable())
                mWorkerThread.join();
        }

        void RegisterWatchPath(const std::filesystem::path& path, bool recursive)
        {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
                return;

            const std::filesystem::path normalizedPath = NormalizeWatchPath(path);
            {
                std::scoped_lock lock(mMutex);
                if (std::filesystem::is_regular_file(path, ec))
                {
                    const auto writeTime = std::filesystem::last_write_time(path, ec);
                    if (!ec)
                        mWatchingFiles[normalizedPath] = writeTime;
                }
                else
                {
                    if (!ContainsDirectoryLocked(normalizedPath, recursive))
                        mWatchingDirectories.emplace_back(normalizedPath, recursive);

                    AddDirectoryFilesLocked(normalizedPath, recursive);
                }

                mScanRequested = true;
            }

            mWakeCondition.notify_all();
        }

        bool ContainsDirectoryLocked(const std::filesystem::path& directory, bool recursive) const
        {
            for (const auto& [watchedDirectory, watchedRecursive] : mWatchingDirectories)
            {
                if (watchedDirectory == directory && watchedRecursive == recursive)
                    return true;
            }

            return false;
        }

        void AddDirectoryFilesLocked(const std::filesystem::path& directory, bool recursive)
        {
            std::error_code ec;
            try
            {
                if (recursive)
                {
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
                    {
                        if (!entry.is_regular_file())
                            continue;

                        const std::filesystem::path normalizedPath = NormalizeWatchPath(entry.path());
                        const auto writeTime = std::filesystem::last_write_time(entry.path(), ec);
                        if (!ec)
                            mWatchingFiles[normalizedPath] = writeTime;
                    }
                }
                else
                {
                    for (const auto& entry : std::filesystem::directory_iterator(directory))
                    {
                        if (!entry.is_regular_file())
                            continue;

                        const std::filesystem::path normalizedPath = NormalizeWatchPath(entry.path());
                        const auto writeTime = std::filesystem::last_write_time(entry.path(), ec);
                        if (!ec)
                            mWatchingFiles[normalizedPath] = writeTime;
                    }
                }
            }
            catch (const std::exception&)
            {
            }
        }

        void WorkerLoop()
        {
            HE_PROFILE_THREAD("Shader Watcher Worker");

            while (true)
            {
                ScanSnapshot snapshot;
                {
                    std::unique_lock lock(mMutex);
                    if (mStopRequested)
                        break;

                    snapshot.watchingFiles = mWatchingFiles;
                    snapshot.watchingDirectories = mWatchingDirectories;
                    mScanRequested = false;
                }

                ApplyScanResult(ScanSnapshotState(snapshot));

                std::unique_lock lock(mMutex);
                mWakeCondition.wait_for(lock, mPollInterval, [this]() {
                    return mStopRequested || mScanRequested;
                });

                if (mStopRequested)
                    break;
            }
        }

        ScanResult ScanSnapshotState(const ScanSnapshot& snapshot) const
        {
            ScanResult result;
            std::error_code ec;

            for (const auto& [directory, recursive] : snapshot.watchingDirectories)
            {
                if (!std::filesystem::exists(directory, ec))
                    continue;

                try
                {
                    if (recursive)
                    {
                        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
                        {
                            if (!entry.is_regular_file())
                                continue;

                            const std::filesystem::path normalizedPath = NormalizeWatchPath(entry.path());
                            if (snapshot.watchingFiles.count(normalizedPath))
                                continue;

                            const auto writeTime = std::filesystem::last_write_time(entry.path(), ec);
                            if (!ec)
                                result.discoveredFiles[normalizedPath] = writeTime;
                        }
                    }
                    else
                    {
                        for (const auto& entry : std::filesystem::directory_iterator(directory))
                        {
                            if (!entry.is_regular_file())
                                continue;

                            const std::filesystem::path normalizedPath = NormalizeWatchPath(entry.path());
                            if (snapshot.watchingFiles.count(normalizedPath))
                                continue;

                            const auto writeTime = std::filesystem::last_write_time(entry.path(), ec);
                            if (!ec)
                                result.discoveredFiles[normalizedPath] = writeTime;
                        }
                    }
                }
                catch (const std::exception&)
                {
                }
            }

            for (const auto& [file, fileTime] : snapshot.watchingFiles)
            {
                if (!std::filesystem::exists(file, ec))
                {
                    result.removedFiles.push_back(file);
                    continue;
                }

                const auto fileLastWriteTime = std::filesystem::last_write_time(file, ec);
                if (ec)
                    continue;

                if (fileTime != fileLastWriteTime)
                    result.modifiedFiles[file] = fileLastWriteTime;
            }

            return result;
        }

        void ApplyScanResult(const ScanResult& result)
        {
            std::scoped_lock lock(mMutex);

            for (const auto& [file, writeTime] : result.discoveredFiles)
            {
                if (!mWatchingFiles.count(file))
                    mWatchingFiles[file] = writeTime;
            }

            for (const auto& [file, writeTime] : result.modifiedFiles)
            {
                auto it = mWatchingFiles.find(file);
                if (it == mWatchingFiles.end())
                    continue;

                if (it->second == writeTime)
                    continue;

                it->second = writeTime;
                QueueModifiedFileLocked(file);
            }

            for (const auto& removedFile : result.removedFiles)
            {
                mWatchingFiles.erase(removedFile);
                mSpeciCallBacks.erase(removedFile);
                mPendingModifiedLookup.erase(removedFile);
            }
        }

        void QueueModifiedFileLocked(const std::filesystem::path& file)
        {
            if (!mPendingModifiedLookup.insert(file).second)
                return;

            mPendingModifiedFiles.push_back(file);
        }

    private:
        mutable std::mutex mMutex;
        std::condition_variable mWakeCondition;
        std::thread mWorkerThread;
        bool mStopRequested = false;
        bool mScanRequested = false;
        const std::chrono::milliseconds mPollInterval = std::chrono::milliseconds(250);
        std::unordered_map<std::filesystem::path, std::filesystem::file_time_type> mWatchingFiles;
        std::vector<std::pair<std::filesystem::path, bool>> mWatchingDirectories;
        std::unordered_map<std::filesystem::path, std::vector<std::function<void(const std::filesystem::path&)>>> mSpeciCallBacks;
        std::vector<std::filesystem::path> mPendingModifiedFiles;
        std::unordered_set<std::filesystem::path> mPendingModifiedLookup;
        FileModifiedEvent mFileModifiedEvent;
    };

    inline FileWatcher shaderWatcher;
}
