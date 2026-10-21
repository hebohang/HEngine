#pragma once

#include "Runtime/Core/Buffer.h"
#include "Runtime/Graphics/ResourceFormat.h"

#include <cstddef>
#include <filesystem>
#include <span>

namespace HEngine
{
    class Image
    {
    public:
        Image(const std::filesystem::path& path, bool flip = false);
        ~Image();
    public:
        [[nodiscard]] std::span<const std::byte> GetData() const
        {
            return buffer.View();
        }
        [[nodiscard]] const Buffer& GetBuffer() const { return buffer; }
        [[nodiscard]] uint32_t GetWidth() const { return width; }
        [[nodiscard]] uint32_t GetHeight() const { return height; }
        [[nodiscard]] EResourceFormat GetFormat() const { return format; }
    private:
        Buffer buffer;
        uint32_t width = 0;
        uint32_t height = 0;
        EResourceFormat format = EResourceFormat::UNKNOWN;
    };
}
