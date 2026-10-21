#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    // Bounded CPU file staging for assets that an authored sequence will soon
    // reveal. GPU resources remain owned by their normal managers.
    class AssetReadAhead final
    {
    public:
        static AssetReadAhead& GetInstance();

        void BeginScene();
        void EndScene();
        [[nodiscard]] bool Queue(const std::filesystem::path& path);
        [[nodiscard]] bool IsReady(const std::filesystem::path& path);
        [[nodiscard]] std::optional<std::vector<std::byte>> Take(
            const std::filesystem::path& path);

    private:
        AssetReadAhead() = default;
        ~AssetReadAhead();
        AssetReadAhead(const AssetReadAhead&) = delete;
        AssetReadAhead& operator=(const AssetReadAhead&) = delete;

        enum class State : uint8_t { Queued, Loading, Ready };
        struct Entry
        {
            uint64_t Generation = 0;
            uint64_t ReservedBytes = 0;
            State Status = State::Queued;
            std::vector<std::byte> Bytes;
        };

        void Run(std::stop_token stop);

        std::mutex mMutex;
        std::condition_variable mCondition;
        std::jthread mWorker;
        std::deque<std::filesystem::path> mQueue;
        std::unordered_map<std::filesystem::path, Entry> mEntries;
        uint64_t mGeneration = 0;
        uint64_t mReservedBytes = 0;
        uint32_t mQueuedCount = 0;
        uint32_t mHitCount = 0;
        uint32_t mMissCount = 0;
    };
}
