#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace HEngine
{
    // Single-producer capture storage. Overflow is fatal evidence loss, never eviction.
    template<class T>
    class BoundedStatisticsBuffer
    {
    public:
        void Reserve(size_t limit)
        {
            if (mLimit != 0 || limit == 0)
                throw std::logic_error("Statistics buffer must be reserved exactly once with a positive limit");
            mRows.reserve(limit);
            mLimit = limit;
        }

        void Append(T&& row)
        {
            if (mRows.size() >= mLimit)
                throw std::runtime_error("Statistics buffer exhausted; no records were silently dropped");
            mRows.push_back(std::move(row));
        }

        [[nodiscard]] std::span<const T> Rows() const { return mRows; }
        [[nodiscard]] T& Back()
        {
            if (mRows.empty()) throw std::logic_error("Statistics buffer is empty");
            return mRows.back();
        }
        [[nodiscard]] size_t Size() const { return mRows.size(); }
        [[nodiscard]] size_t Limit() const { return mLimit; }

    private:
        std::vector<T> mRows;
        size_t mLimit = 0;
    };
}
