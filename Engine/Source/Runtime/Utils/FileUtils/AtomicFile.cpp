#include "Hepch.h"

#include "Runtime/Utils/FileUtils/AtomicFile.h"
#include "Runtime/Platform/PlatformFileSystem.h"

#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace HEngine::Utils
{
    void WriteBytesToFileAtomically(
        const std::filesystem::path& outputPath,
        const void* data,
        std::size_t size,
        AtomicWriteMode writeMode)
    {
        if (size > 0 && !data)
            throw std::invalid_argument("Atomic file data cannot be null when size is non-zero.");
        if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
            throw std::runtime_error("Atomic file output is too large: " + outputPath.string());

        const std::filesystem::path temporaryPath =
            PlatformFileSystem::MakeTemporarySiblingPath(outputPath);
        try
        {
            std::ofstream fout(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!fout.is_open() || !fout)
                throw std::runtime_error("Failed to open temporary file: " + temporaryPath.string());

            if (size > 0)
                fout.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            const bool bWriteSucceeded = static_cast<bool>(fout);

            bool bFlushSucceeded = false;
            if (bWriteSucceeded)
            {
                fout.flush();
                bFlushSucceeded = static_cast<bool>(fout);
            }

            fout.clear();
            fout.close();
            const bool bCloseSucceeded = !fout.fail();

            if (!bWriteSucceeded)
                throw std::runtime_error("Failed to write temporary file: " + temporaryPath.string());
            if (!bFlushSucceeded)
                throw std::runtime_error("Failed to flush temporary file: " + temporaryPath.string());
            if (!bCloseSucceeded)
                throw std::runtime_error("Failed to close temporary file: " + temporaryPath.string());

            std::error_code publishError;
            const bool bReplaceExisting = writeMode == AtomicWriteMode::ReplaceExisting;
            if (!PlatformFileSystem::PublishFileAtomically(
                    temporaryPath,
                    outputPath,
                    bReplaceExisting,
                    publishError))
            {
                throw std::runtime_error(
                    "Failed to atomically publish file " + outputPath.string() +
                    ": " + publishError.message());
            }
        }
        catch (...)
        {
            const std::exception_ptr writeFailure = std::current_exception();
            std::error_code cleanupError;
            std::filesystem::remove(temporaryPath, cleanupError);
            if (cleanupError)
            {
                try
                {
                    std::rethrow_exception(writeFailure);
                }
                catch (...)
                {
                    std::throw_with_nested(std::runtime_error(
                        "Failed to remove temporary file " + temporaryPath.string() +
                        ": " + cleanupError.message()));
                }
            }
            std::rethrow_exception(writeFailure);
        }
    }

    void WriteTextToFileAtomically(
        const std::filesystem::path& outputPath,
        std::string_view text,
        AtomicWriteMode writeMode)
    {
        WriteBytesToFileAtomically(outputPath, text.data(), text.size(), writeMode);
    }
}
