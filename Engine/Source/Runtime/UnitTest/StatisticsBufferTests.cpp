#include "Hepch.h"
#include "Runtime/UnitTest/StatisticsBufferTests.h"
#include "Runtime/Profile/BoundedStatisticsBuffer.h"

#include <stdexcept>

namespace HEngine::Tests
{
    void StatisticsBuffer()
    {
        struct Frame
        {
            uint64_t Id;
            double Wall;
            Scope<std::vector<int>> Payload;
        };
        BoundedStatisticsBuffer<Frame> buffer;
        buffer.Reserve(2);
        buffer.Append({31, 110.3746, CreateScope<std::vector<int>>(std::initializer_list<int>{3, 7})});
        auto* first = &buffer.Back();
        buffer.Append({32, 12.5, CreateScope<std::vector<int>>(std::initializer_list<int>{8})});
        // Late previous-frame counters may update retained rows without relocation.
        first->Wall = 110.3747;
        Frame rejected{33, 19.0, CreateScope<std::vector<int>>(std::initializer_list<int>{9})};
        bool overflow = false;
        try { buffer.Append(std::move(rejected)); }
        catch (const std::runtime_error&) { overflow = true; }
        if (!overflow || !rejected.Payload || buffer.Size() != 2 || buffer.Limit() != 2)
            throw std::runtime_error("Statistics overflow must preserve both retained and rejected records");
        const auto rows = buffer.Rows();
        if (&rows[0] != first || rows[0].Id != 31 || rows[1].Id != 32 ||
            rows[0].Wall != 110.3747 || *rows[0].Payload != std::vector<int>({3, 7}) ||
            *rows[1].Payload != std::vector<int>({8}))
            throw std::runtime_error("Statistics capture changed ordering, identity or move-owned payload");
        bool reconfigure = false;
        try { buffer.Reserve(3); }
        catch (const std::logic_error&) { reconfigure = true; }
        if (!reconfigure || &buffer.Rows()[0] != first)
            throw std::runtime_error("Statistics capture must not invalidate previous-frame pointers");
    }
}
