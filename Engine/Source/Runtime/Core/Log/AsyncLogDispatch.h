#pragma once

#include "Runtime/Core/Base/Base.h"

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>

#include <stdexcept>
#include <cstdio>
#include <string>
#include <vector>

namespace HEngine
{
    // One worker preserves ordering across all attached loggers. Queue saturation
    // applies backpressure: messages are never silently overwritten or discarded.
    // CreateLogger/Drain are owner-thread operations; quiesce producers before Drain.
    class AsyncLogDispatch
    {
    public:
        explicit AsyncLogDispatch(size_t capacity = 8192)
        {
            if (capacity == 0) throw std::invalid_argument("Log queue capacity must be positive");
            mPool = CreateRef<spdlog::details::thread_pool>(capacity, 1);
        }
        AsyncLogDispatch(const AsyncLogDispatch&) = delete;
        AsyncLogDispatch& operator=(const AsyncLogDispatch&) = delete;
        ~AsyncLogDispatch()
        {
            try { Drain(); }
            catch (...)
            {
                std::fputs("HEngine: log dispatcher flush failed during destruction\n", stderr);
                std::fflush(stderr);
                mPool.reset();
            }
        }
        Ref<spdlog::logger> CreateLogger(const std::string& name, const spdlog::sink_ptr& sink)
        {
            if (!mPool) throw std::logic_error("Log dispatcher already drained");
            auto logger = CreateRef<spdlog::async_logger>(name, sink, mPool, spdlog::async_overflow_policy::block);
            mLoggers.push_back(logger);
            return logger;
        }
        size_t PendingCount() const { return mPool ? mPool->queue_size() : 0; }
        void Drain()
        {
            if (!mPool) return;
            for (const auto& logger : mLoggers) logger->flush();
            // flush() only enqueues. Pool destruction appends terminate after the
            // flush records and joins the sole worker, proving actual completion.
            mPool.reset();
            mLoggers.clear();
        }
    private:
        Ref<spdlog::details::thread_pool> mPool;
        std::vector<Ref<spdlog::logger>> mLoggers;
    };
}
