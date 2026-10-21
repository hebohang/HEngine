#include "Hepch.h"
#include "Runtime/UnitTest/AsyncLoggingTests.h"
#include "Runtime/Core/Log/AsyncLogDispatch.h"
#include "Runtime/Core/Log/Log.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/dist_sink.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        class ChannelLogSink final : public spdlog::sinks::base_sink<std::mutex>
        {
        public:
            std::vector<std::string> Messages;
        protected:
            void sink_it_(const spdlog::details::log_msg& message) override
            { Messages.emplace_back(message.payload.data(), message.payload.size()); }
            void flush_() override {}
        };
        class GatedLogSink final : public spdlog::sinks::base_sink<std::mutex>
        {
        public:
            bool WaitForFirst()
            {
                std::unique_lock lock(mGateMutex);
                return mGate.wait_for(lock, std::chrono::seconds(5), [&] { return bEntered; });
            }
            void Release()
            {
                std::lock_guard lock(mGateMutex);
                bReleased = true;
                mGate.notify_all();
            }
            std::vector<std::string> Messages;
            size_t Flushes = 0;
            bool TimedOut = false;
        protected:
            void sink_it_(const spdlog::details::log_msg& message) override
            {
                {
                    std::unique_lock lock(mGateMutex);
                    if (!bEntered)
                    {
                        bEntered = true;
                        mGate.notify_all();
                        if (!mGate.wait_for(lock, std::chrono::seconds(5), [&] { return bReleased; }))
                            TimedOut = true;
                    }
                }
                Messages.emplace_back(message.payload.data(), message.payload.size());
            }
            void flush_() override { ++Flushes; }
        private:
            std::mutex mGateMutex;
            std::condition_variable mGate;
            bool bEntered = false, bReleased = false;
        };
    }

    void AsyncLogging()
    {
        auto require = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
#if !WITH_EDITOR
        require(std::dynamic_pointer_cast<spdlog::async_logger>(Log::GetCoreLogger()) != nullptr,
            "Game core logger must use async dispatch");
        require(std::dynamic_pointer_cast<spdlog::async_logger>(Log::GetClientLogger()) != nullptr,
            "Game Python logger must use async dispatch");
#endif
        AsyncLogDispatch dispatch(2);
        auto sink = CreateRef<GatedLogSink>();
        auto coreRoute = CreateRef<spdlog::sinks::dist_sink_mt>();
        auto clientRoute = CreateRef<spdlog::sinks::dist_sink_mt>();
        auto coreOnly = CreateRef<ChannelLogSink>();
        auto clientOnly = CreateRef<ChannelLogSink>();
        coreRoute->add_sink(sink); coreRoute->add_sink(coreOnly);
        clientRoute->add_sink(sink); clientRoute->add_sink(clientOnly);
        auto core = dispatch.CreateLogger("async-test-core", coreRoute);
        auto client = dispatch.CreateLogger("async-test-client", clientRoute);
        std::thread producer;
        std::promise<void> entered, finished;
        auto enteredFuture = entered.get_future();
        auto finishedFuture = finished.get_future();
        // Declared after producer: release the consumer before joining on every
        // assertion/exception path, so a failed test cannot leave a blocked queue.
        struct Cleanup
        {
            Ref<GatedLogSink> Sink;
            std::thread& Producer;
            ~Cleanup() { Sink->Release(); if (Producer.joinable()) Producer.join(); }
        } cleanup{sink, producer};
        core->info("0");
        require(sink->WaitForFirst(), "Log worker did not reach the gate");
        client->info("1");
        core->info("2");
        require(dispatch.PendingCount() == 2, "Bounded queue must retain both messages while sink is blocked");
        producer = std::thread([&] {
            entered.set_value();
            try { client->info("3"); finished.set_value(); }
            catch (...) { finished.set_exception(std::current_exception()); }
        });
        require(enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "Producer did not start");
        // A bounded completion wait tests backpressure after an explicit gate
        // handshake; no guessed sleep is used to assume worker completion.
        require(finishedFuture.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout,
            "Saturated queue must apply backpressure instead of dropping a record");
        sink->Release();
        require(finishedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "Producer remained blocked after releasing sink");
        finishedFuture.get();
        producer.join();
        dispatch.Drain();
        require(!sink->TimedOut, "Sink gate timeout invalidates the ordering evidence");
        require(sink->Messages == std::vector<std::string>({"0","1","2","3"}),
            "One worker must preserve cross-logger order without loss");
        require(coreOnly->Messages == std::vector<std::string>({"0","2"}) &&
            clientOnly->Messages == std::vector<std::string>({"1","3"}),
            "Logger-specific diagnostic sinks must not duplicate or cross-label messages");
        require(sink->Flushes == 2 && dispatch.PendingCount() == 0,
            "Drain must complete both logger flush records before returning");
        dispatch.Drain();
        require(sink->Flushes == 2, "Drain must be idempotent");
    }
}
