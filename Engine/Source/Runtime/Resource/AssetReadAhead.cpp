#include "Hepch.h"

#include "Runtime/Resource/AssetReadAhead.h"

#include <fstream>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr uint64_t c_MaxStagedBytes = 96ull * 1024 * 1024;
        constexpr uint64_t c_MaxSingleFileBytes = 32ull * 1024 * 1024;
    }

    AssetReadAhead& AssetReadAhead::GetInstance()
    {
        static AssetReadAhead instance;
        return instance;
    }

    AssetReadAhead::~AssetReadAhead()
    {
        if (mWorker.joinable())
        {
            mWorker.request_stop();
            mCondition.notify_all();
            mWorker.join();
        }
    }

    void AssetReadAhead::BeginScene()
    {
        std::scoped_lock lock(mMutex);
        ++mGeneration;
        mQueue.clear();
        mEntries.clear();
        mReservedBytes = 0;
        mQueuedCount = 0;
        mHitCount = 0;
        mMissCount = 0;
        mCondition.notify_all();
    }

    void AssetReadAhead::EndScene()
    {
        uint32_t queued = 0;
        uint32_t hits = 0;
        uint32_t misses = 0;
        {
            std::scoped_lock lock(mMutex);
            queued = mQueuedCount;
            hits = mHitCount;
            misses = mMissCount;
        }
        if (queued != 0)
            HE_CORE_INFO("Asset read-ahead: queued={} hits={} misses={}",
                queued, hits, misses);
        BeginScene();
    }

    bool AssetReadAhead::Queue(const std::filesystem::path& inputPath)
    {
        const std::filesystem::path path = inputPath.lexically_normal();
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
            return false;
        const uint64_t size = std::filesystem::file_size(path, error);
        if (error || size < 128 || size > c_MaxSingleFileBytes)
            return false;

        std::scoped_lock lock(mMutex);
        if (mEntries.contains(path)) return true;
        if (size > c_MaxStagedBytes - mReservedBytes) return false;
        mReservedBytes += size;
        mEntries.emplace(path, Entry{mGeneration, size});
        mQueue.push_back(path);
        ++mQueuedCount;
        if (!mWorker.joinable())
            mWorker = std::jthread([this](std::stop_token stop) { Run(stop); });
        mCondition.notify_one();
        return true;
    }

    bool AssetReadAhead::IsReady(const std::filesystem::path& inputPath)
    {
        std::scoped_lock lock(mMutex);
        const auto found = mEntries.find(inputPath.lexically_normal());
        return found != mEntries.end() && found->second.Status == State::Ready;
    }

    std::optional<std::vector<std::byte>> AssetReadAhead::Take(
        const std::filesystem::path& inputPath)
    {
        const std::filesystem::path path = inputPath.lexically_normal();
        std::unique_lock lock(mMutex);
        auto found = mEntries.find(path);
        if (found == mEntries.end()) return std::nullopt;
        if (found->second.Status == State::Queued)
        {
            ++mMissCount;
            mReservedBytes -= found->second.ReservedBytes;
            mEntries.erase(found);
            return std::nullopt;
        }
        if (found->second.Status == State::Loading)
        {
            const uint64_t generation = found->second.Generation;
            mCondition.wait(lock, [&]
            {
                const auto current = mEntries.find(path);
                return current == mEntries.end() || current->second.Generation != generation ||
                    current->second.Status != State::Loading;
            });
            found = mEntries.find(path);
            if (found == mEntries.end() || found->second.Generation != generation)
                return std::nullopt;
        }
        if (found->second.Status != State::Ready)
        {
            ++mMissCount;
            return std::nullopt;
        }
        ++mHitCount;
        std::vector<std::byte> bytes = std::move(found->second.Bytes);
        mReservedBytes -= found->second.ReservedBytes;
        mEntries.erase(found);
        return bytes;
    }

    void AssetReadAhead::Run(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            std::filesystem::path path;
            uint64_t generation = 0;
            uint64_t expectedBytes = 0;
            {
                std::unique_lock lock(mMutex);
                mCondition.wait(lock, [&] { return stop.stop_requested() || !mQueue.empty(); });
                if (stop.stop_requested()) return;
                path = std::move(mQueue.front());
                mQueue.pop_front();
                const auto found = mEntries.find(path);
                if (found == mEntries.end() || found->second.Status != State::Queued)
                    continue;
                found->second.Status = State::Loading;
                generation = found->second.Generation;
                expectedBytes = found->second.ReservedBytes;
            }

            std::vector<std::byte> bytes;
            try
            {
                std::ifstream stream(path, std::ios::binary | std::ios::ate);
                if (stream && stream.tellg() == static_cast<std::streamoff>(expectedBytes))
                {
                    bytes.resize(static_cast<size_t>(expectedBytes));
                    stream.seekg(0);
                    if (!stream.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())))
                        bytes.clear();
                }
            }
            catch (...) { bytes.clear(); }
            {
                std::scoped_lock lock(mMutex);
                const auto found = mEntries.find(path);
                if (found != mEntries.end() && found->second.Generation == generation)
                {
                    if (bytes.size() == expectedBytes)
                    {
                        found->second.Bytes = std::move(bytes);
                        found->second.Status = State::Ready;
                    }
                    else
                    {
                        mReservedBytes -= found->second.ReservedBytes;
                        mEntries.erase(found);
                    }
                }
                mCondition.notify_all();
            }
        }
    }
}
