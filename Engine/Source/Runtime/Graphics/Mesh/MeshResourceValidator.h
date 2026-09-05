#pragma once

#include "Runtime/Resource/ResType/Mesh.h"

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>

namespace HEngine
{
    enum class MeshValidationError : std::uint8_t
    {
        None = 0,
        UnsupportedVersion,
        MissingPrimitive,
        ResourceLimitExceeded,
        InvalidBuffer,
        InvalidBufferPath,
        InvalidReferencedPath,
        BufferFileError,
        BufferSizeMismatch,
        InvalidBufferView,
        UnsupportedBufferStride,
        InvalidAccessor,
        UnsupportedAccessor,
        InvalidPrimitive,
        IndexOutOfRange,
        InvalidVertexData,
        AllocationFailed,
        Cancelled
    };

    struct MeshValidationResult
    {
        MeshValidationError Code = MeshValidationError::None;
        std::string Message;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Code == MeshValidationError::None;
        }
    };

    class MeshResourceValidator final
    {
    public:
        [[nodiscard]] static MeshValidationResult ValidateSchema(const MeshRes& meshResource);
        [[nodiscard]] static MeshValidationResult ValidateLoaded(const MeshRes& meshResource);
        [[nodiscard]] static MeshValidationResult LoadAndValidateBuffers(
            MeshRes& meshResource,
            const std::filesystem::path& projectRoot,
            std::stop_token stopToken = {});
    };
}
