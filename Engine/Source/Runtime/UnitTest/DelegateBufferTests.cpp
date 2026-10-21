#include "Hepch.h"
#include "Runtime/UnitTest/DelegateBufferTests.h"
#include "Runtime/Core/Buffer.h"
#include "Runtime/Events/Delegate.h"
#include "Runtime/Graphics/Image.h"

#include <atomic>
#include <filesystem>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace HEngine::Tests
{
    namespace
    {
        std::atomic_int sDeleteCount{0};
        void DeleteBytes(void* pointer) noexcept
        {
            ++sDeleteCount;
            delete[] static_cast<std::byte*>(pointer);
        }
        void Require(bool condition, const char* message)
        {
            if (!condition) throw std::runtime_error(message);
        }
    }

    void DelegateBufferContract()
    {
        MultiCastDelegate<std::string> event;
        std::vector<std::string> calls;
        DelegateHandle second;
        DelegateHandle added;
        bool nested = false;
        auto first = event.Add([&](std::string value)
        {
            calls.push_back("first:" + value);
            if (value == "outer")
            {
                Require(event.Remove(second), "Removal during broadcast failed");
                added = event.Add([&](std::string text) { calls.push_back("added:" + text); });
                if (!nested)
                {
                    nested = true;
                    event.Broadcast("inner");
                }
            }
        });
        second = event.Add([&](std::string value) { calls.push_back("second:" + value); });
        event.Broadcast("outer");
        Require(calls == std::vector<std::string>({
            "first:outer", "first:inner", "added:inner" }),
            "Nested broadcast or mutation contract changed");
        Require(!event.IsHandleBound(second) && event.IsHandleBound(first) &&
            event.IsHandleBound(added), "Handle bound state is incorrect");

        DelegateHandle self;
        self = event.Add([&](std::string) { Require(event.Remove(self), "Self removal failed"); });
        event.Broadcast("again");
        Require(!event.IsHandleBound(self), "Self removal did not take effect");
        auto throwing = event.Add([](std::string) { throw std::runtime_error("expected"); });
        bool caught = false;
        try { event.Broadcast("throw"); }
        catch (const std::runtime_error&) { caught = true; }
        Require(caught, "Callback exception was swallowed");
        Require(event.Remove(throwing), "Delegate could not be mutated after exception");
        event.RemoveAll();
        Require(!event.IsHandleBound(first), "RemoveAll left a subscriber");

        constexpr int threadCount = 8;
        constexpr int handlesPerThread = 256;
        std::mutex idsMutex;
        std::set<DelegateHandle> ids;
        std::vector<std::thread> threads;
        for (int thread = 0; thread < threadCount; ++thread)
            threads.emplace_back([&]
            {
                std::vector<DelegateHandle> local;
                for (int i = 0; i < handlesPerThread; ++i)
                    local.emplace_back(0);
                std::lock_guard lock(idsMutex);
                ids.insert(local.begin(), local.end());
            });
        for (auto& thread : threads) thread.join();
        Require(ids.size() == threadCount * handlesPerThread,
            "Concurrent delegate handle IDs collided");
        MultiCastDelegate<int> concurrentEvent;
        std::atomic_int observed{0};
        threads.clear();
        for (int thread = 0; thread < threadCount; ++thread)
            threads.emplace_back([&]
            {
                for (int i = 0; i < handlesPerThread; ++i)
                    (void)concurrentEvent.Add([&](int value) { observed += value; });
            });
        for (auto& thread : threads) thread.join();
        concurrentEvent.Broadcast(1);
        Require(observed == threadCount * handlesPerThread,
            "Concurrent subscriptions were lost");

        sDeleteCount = 0;
        {
            Buffer bytes = Buffer::Adopt(new std::byte[8], 8, DeleteBytes);
            bytes.Write("abcd", 4, 4);
            Buffer replacement = Buffer::Adopt(new std::byte[4], 4, DeleteBytes);
            bytes = std::move(replacement);
            Require(sDeleteCount == 1 && bytes.Size() == 4 && replacement.Size() == 0,
                "Move assignment failed to release old allocation");
            bytes = std::move(bytes);
            Require(bytes.Size() == 4, "Self move lost allocation");
            const uint32_t typedValue = 0x12345678u;
            bytes.Write(&typedValue, sizeof(typedValue));
            Require(bytes.Get<uint32_t>() == typedValue,
                "Buffer typed access lost a valid zero-offset value");
            bool overflow = false;
            try { bytes.Write("x", std::numeric_limits<uint32_t>::max(), 2); }
            catch (const std::out_of_range&) { overflow = true; }
            Require(overflow, "Overflowing Buffer write was accepted");
            overflow = false;
            try { (void)bytes.Get<std::byte>(std::numeric_limits<uint32_t>::max()); }
            catch (const std::out_of_range&) { overflow = true; }
            Require(overflow, "Overflowing Buffer typed index was accepted");
            int borrowed = 42;
            std::span<int> view(&borrowed, 1);
            view[0] = 43;
            Require(borrowed == 43, "Borrowed span did not refer to source");
        }
        Require(sDeleteCount == 2, "Custom Buffer deleter count is wrong");

        const auto content = std::filesystem::path(__FILE__).parent_path() /
            "../../../Content/Assets/Textures";
        const Image png(content / "DefaultTexture.png");
        const Image hdr(content / "Hdr/Default.hdr");
        Require(png.GetWidth() > 0 && png.GetHeight() > 0 &&
            png.GetData().size() == size_t(png.GetWidth()) * png.GetHeight() * 4,
            "PNG decode produced an invalid byte span");
        Require(hdr.GetWidth() > 0 && hdr.GetHeight() > 0 &&
            hdr.GetData().size() == size_t(hdr.GetWidth()) * hdr.GetHeight() * 16,
            "HDR decode produced an invalid byte span");
        const size_t rowBytes = size_t(png.GetWidth()) * 4;
        std::atomic_bool flipValid{true};
        threads.clear();
        for (int thread = 0; thread < threadCount; ++thread)
            threads.emplace_back([&]
            {
                for (int repeat = 0; repeat < 12; ++repeat)
                {
                    Image flipped(content / "DefaultTexture.png", true);
                    Image normal(content / "DefaultTexture.png", false);
                    if (flipped.GetData().size() != png.GetData().size() ||
                        normal.GetData().size() != png.GetData().size() ||
                        std::memcmp(flipped.GetData().data(),
                            png.GetData().data() + (png.GetHeight() - 1) * rowBytes,
                            rowBytes) != 0 ||
                        std::memcmp(normal.GetData().data(), png.GetData().data(),
                            rowBytes) != 0)
                        flipValid = false;
                }
            });
        for (auto& thread : threads) thread.join();
        Require(flipValid, "Concurrent stb image flips shared state");
    }
}
