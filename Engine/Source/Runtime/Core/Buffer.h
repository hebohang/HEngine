#pragma once

#include "Base/Base.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>

namespace HEngine
{
    // Owns bytes through the deleter supplied at adoption. Borrowed memory is
    // represented by std::span, never by Buffer.
    class Buffer
    {
    public:
        using Deleter = void(*)(void*) noexcept;

        Buffer() = default;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&& other) noexcept { Swap(other); }
        Buffer& operator=(Buffer&& other) noexcept
        {
            if (this != &other)
            {
                Release();
                Swap(other);
            }
            return *this;
        }
        ~Buffer() { Release(); }

        static Buffer Adopt(void* data, uint32_t size, Deleter deleter)
        {
            if ((data == nullptr) != (size == 0) || (data && !deleter))
                throw std::invalid_argument("Buffer adoption requires bytes and a deleter");
            Buffer result;
            result.mData = static_cast<std::byte*>(data);
            result.mSize = size;
            result.mDeleter = deleter;
            return result;
        }

        static Buffer Allocate(uint32_t size)
        {
            if (size == 0) return {};
            auto* data = new std::byte[size];
            return Adopt(data, size, [](void* pointer) noexcept
                { delete[] static_cast<std::byte*>(pointer); });
        }

        void Release() noexcept
        {
            if (mData) mDeleter(mData);
            mData = nullptr;
            mSize = 0;
            mDeleter = nullptr;
        }

        [[nodiscard]] std::span<std::byte> View() noexcept { return {mData, mSize}; }
        [[nodiscard]] std::span<const std::byte> View() const noexcept { return {mData, mSize}; }
        operator std::span<const std::byte>() const noexcept { return View(); }
        [[nodiscard]] uint32_t Size() const noexcept { return mSize; }
        [[nodiscard]] std::byte* Data() noexcept { return mData; }
        [[nodiscard]] const std::byte* Data() const noexcept { return mData; }
        [[nodiscard]] void* GetRawBuffer() noexcept { return mData; }

        std::byte& operator[](uint32_t index)
        {
            if (index >= mSize) throw std::out_of_range("Buffer index out of range");
            return mData[index];
        }
        std::byte operator[](uint32_t index) const
        {
            if (index >= mSize) throw std::out_of_range("Buffer index out of range");
            return mData[index];
        }

        template<typename T>
        T& Get(uint32_t index = 0)
        {
            if (sizeof(T) > mSize ||
                size_t(index) > (size_t(mSize) - sizeof(T)) / sizeof(T))
                throw std::out_of_range("Buffer::Get out of range");
            const size_t offset = size_t(index) * sizeof(T);
            return *reinterpret_cast<T*>(mData + offset);
        }

        void Write(const void* data, uint32_t size, uint32_t offset = 0)
        {
            if (offset > mSize || size > mSize - offset || (size && !data))
                throw std::out_of_range("Buffer::Write out of range");
            if (size) std::memcpy(mData + offset, data, size);
        }

        template<typename T>
        T* As() noexcept { return reinterpret_cast<T*>(mData); }

    private:
        void Swap(Buffer& other) noexcept
        {
            std::swap(mData, other.mData);
            std::swap(mSize, other.mSize);
            std::swap(mDeleter, other.mDeleter);
        }

        std::byte* mData = nullptr;
        uint32_t mSize = 0;
        Deleter mDeleter = nullptr;
    };
}
