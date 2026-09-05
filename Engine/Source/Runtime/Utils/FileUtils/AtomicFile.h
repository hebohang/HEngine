#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace HEngine::Utils
{
    enum class AtomicWriteMode
    {
        ReplaceExisting,
        // Fails without changing the target if another file already exists there.
        CreateNew
    };

    // Throws on invalid input, open, write, flush, close, or atomic publish failure.
    // The previous destination is preserved when an exception is reported.
    void WriteBytesToFileAtomically(
        const std::filesystem::path& outputPath,
        const void* data,
        std::size_t size,
        AtomicWriteMode writeMode = AtomicWriteMode::ReplaceExisting);

    void WriteTextToFileAtomically(
        const std::filesystem::path& outputPath,
        std::string_view text,
        AtomicWriteMode writeMode = AtomicWriteMode::ReplaceExisting);
}
