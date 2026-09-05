#include "Hepch.h"

#include "Runtime/Graphics/Mesh/MeshResourceValidator.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr std::size_t MaxMeshBufferCount = 1'024;
        constexpr std::size_t MaxMeshBufferViewCount = 262'144;
        constexpr std::size_t MaxMeshAccessorCount = 262'144;
        constexpr std::size_t MaxMeshPrimitiveCount = 4'096;
        constexpr std::size_t MaxMeshBufferBytes = std::size_t{ 1 } << 28;
        constexpr std::size_t MaxMeshTotalBufferBytes = std::size_t{ 1 } << 29;
        constexpr std::size_t MaxPrimitiveVertexCount = 10'000'000;
        constexpr std::size_t MaxPrimitiveIndexCount = 30'000'000;
        constexpr std::size_t MaxDecodedCpuBytes = std::size_t{ 1 } << 30;
        constexpr std::size_t EstimatedStaticVertexBytes = 128;
        constexpr std::size_t EstimatedSkinnedVertexBytes = 192;

        MeshValidationResult Failure(MeshValidationError code, std::string message)
        {
            return { code, std::move(message) };
        }

        MeshValidationResult CancellationFailure()
        {
            return Failure(MeshValidationError::Cancelled, "mesh resource loading was cancelled");
        }

        bool ShouldCheckCancellation(std::size_t index) noexcept
        {
            return (index & 0x0fff) == 0;
        }

        bool EqualsAsciiCaseInsensitive(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (std::size_t index = 0; index < lhs.size(); ++index)
            {
                char left = lhs[index];
                char right = rhs[index];
                if (left >= 'A' && left <= 'Z')
                    left = static_cast<char>(left - 'A' + 'a');
                if (right >= 'A' && right <= 'Z')
                    right = static_cast<char>(right - 'A' + 'a');
                if (left != right)
                    return false;
            }
            return true;
        }

        bool HasExpectedExtension(std::string_view path, std::string_view extension)
        {
            return path.size() >= extension.size() &&
                EqualsAsciiCaseInsensitive(path.substr(path.size() - extension.size()), extension);
        }

        MeshValidationResult NormalizeReferencedPath(
            const std::filesystem::path& projectRoot,
            std::string_view expectedExtension,
            std::string context,
            std::string& inOutPath)
        {
            if (inOutPath.empty())
                return {};

            std::filesystem::path fullPath;
            std::string normalizedPath;
            const auto pathResult = Utils::TryResolveProjectRelativePath(
                projectRoot,
                inOutPath,
                fullPath,
                normalizedPath);
            if (!pathResult)
            {
                return Failure(
                    MeshValidationError::InvalidReferencedPath,
                    std::move(context) + " is unsafe: " + pathResult.Message);
            }
            if (!HasExpectedExtension(normalizedPath, expectedExtension))
            {
                return Failure(
                    MeshValidationError::InvalidReferencedPath,
                    std::move(context) + " must use the " + std::string(expectedExtension) + " extension");
            }

            inOutPath = std::move(normalizedPath);
            return {};
        }

        bool CheckedAdd(std::size_t lhs, std::size_t rhs, std::size_t& outValue)
        {
            if (rhs > std::numeric_limits<std::size_t>::max() - lhs)
                return false;
            outValue = lhs + rhs;
            return true;
        }

        bool CheckedMultiply(std::size_t lhs, std::size_t rhs, std::size_t& outValue)
        {
            if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs)
                return false;
            outValue = lhs * rhs;
            return true;
        }

        std::size_t ComponentSize(int componentType)
        {
            switch (componentType)
            {
            case HENGINE_COMPONENT_TYPE_BYTE:
            case HENGINE_COMPONENT_TYPE_UNSIGNED_BYTE:
                return 1;
            case HENGINE_COMPONENT_TYPE_SHORT:
            case HENGINE_COMPONENT_TYPE_UNSIGNED_SHORT:
                return 2;
            case HENGINE_COMPONENT_TYPE_INT:
            case HENGINE_COMPONENT_TYPE_UNSIGNED_INT:
            case HENGINE_COMPONENT_TYPE_FLOAT:
                return 4;
            case HENGINE_COMPONENT_TYPE_DOUBLE:
                return 8;
            default:
                return 0;
            }
        }

        std::size_t ComponentCount(std::string_view type)
        {
            if (type == "SCALAR")
                return 1;
            if (type == "VEC2")
                return 2;
            if (type == "VEC3")
                return 3;
            if (type == "VEC4" || type == "MAT2")
                return 4;
            if (type == "MAT3")
                return 9;
            if (type == "MAT4")
                return 16;
            return 0;
        }

        bool IsValidIndexComponent(int componentType)
        {
            return componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_BYTE ||
                componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_SHORT ||
                componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_INT;
        }

        MeshValidationResult ValidateAccessorRange(
            const MeshRes& meshResource,
            std::size_t accessorIndex,
            bool bRequireLoadedData,
            std::stop_token stopToken)
        {
            const Accessor& accessor = meshResource.accessorVec[accessorIndex];
            if (accessor.bufferViewIndex < 0 ||
                static_cast<std::size_t>(accessor.bufferViewIndex) >= meshResource.bufferViewVec.size())
            {
                return Failure(
                    MeshValidationError::InvalidAccessor,
                    "accessor[" + std::to_string(accessorIndex) + "] has an invalid buffer view index");
            }

            const std::size_t componentSize = ComponentSize(accessor.componentType);
            const std::size_t componentCount = ComponentCount(accessor.type);
            if (componentSize == 0 || componentCount == 0 || accessor.count == 0)
            {
                return Failure(
                    MeshValidationError::InvalidAccessor,
                    "accessor[" + std::to_string(accessorIndex) + "] has an invalid component, type, or count");
            }

            std::size_t elementSize = 0;
            std::size_t accessorBytes = 0;
            std::size_t accessorEnd = 0;
            if (!CheckedMultiply(componentSize, componentCount, elementSize) ||
                !CheckedMultiply(elementSize, accessor.count, accessorBytes) ||
                !CheckedAdd(accessor.byteOffset, accessorBytes, accessorEnd))
            {
                return Failure(
                    MeshValidationError::InvalidAccessor,
                    "accessor[" + std::to_string(accessorIndex) + "] byte range overflows");
            }

            const MeshBufferView& view = meshResource.bufferViewVec[accessor.bufferViewIndex];
            std::size_t absoluteAccessorOffset = 0;
            if (!CheckedAdd(view.byteOffset, accessor.byteOffset, absoluteAccessorOffset) ||
                absoluteAccessorOffset % componentSize != 0)
            {
                return Failure(
                    MeshValidationError::InvalidAccessor,
                    "accessor[" + std::to_string(accessorIndex) + "] byte offset is misaligned");
            }

            if (accessorEnd > view.byteLength)
            {
                return Failure(
                    MeshValidationError::InvalidAccessor,
                    "accessor[" + std::to_string(accessorIndex) + "] exceeds its buffer view");
            }

            if (bRequireLoadedData)
            {
                const MeshBuffer& buffer = meshResource.bufferVec[view.bufferIndex];
                std::size_t dataOffset = 0;
                std::size_t dataEnd = 0;
                if (!CheckedAdd(view.byteOffset, accessor.byteOffset, dataOffset) ||
                    !CheckedAdd(dataOffset, accessorBytes, dataEnd) ||
                    dataEnd > buffer.data.size())
                {
                    return Failure(
                        MeshValidationError::InvalidAccessor,
                        "accessor[" + std::to_string(accessorIndex) + "] exceeds loaded buffer data");
                }

                if (accessor.componentType == HENGINE_COMPONENT_TYPE_FLOAT)
                {
                    const std::size_t valueCount = accessorBytes / sizeof(float);
                    for (std::size_t valueIndex = 0; valueIndex < valueCount; ++valueIndex)
                    {
						if (ShouldCheckCancellation(valueIndex) && stopToken.stop_requested())
							return CancellationFailure();

                        float value = 0.0f;
                        std::memcpy(
                            &value,
                            buffer.data.data() + dataOffset + valueIndex * sizeof(float),
                            sizeof(value));
                        if (!std::isfinite(value))
                        {
                            return Failure(
                                MeshValidationError::InvalidVertexData,
                                "accessor[" + std::to_string(accessorIndex) + "] contains a non-finite float");
                        }
                    }
                }
            }

            return {};
        }

        MeshValidationResult ValidatePrimitiveAttribute(
            const MeshRes& meshResource,
            std::size_t primitiveIndex,
            std::string_view attributeName,
            int accessorIndex,
            std::size_t vertexCount)
        {
            if (accessorIndex < 0 ||
                static_cast<std::size_t>(accessorIndex) >= meshResource.accessorVec.size())
            {
                return Failure(
                    MeshValidationError::InvalidPrimitive,
                    "primitive[" + std::to_string(primitiveIndex) + "] attribute " +
                    std::string(attributeName) + " has an invalid accessor index");
            }

            const Accessor& accessor = meshResource.accessorVec[accessorIndex];
            if (accessor.componentType != HENGINE_COMPONENT_TYPE_FLOAT || accessor.count != vertexCount)
            {
                return Failure(
                    MeshValidationError::UnsupportedAccessor,
                    "primitive[" + std::to_string(primitiveIndex) + "] attribute " +
                    std::string(attributeName) + " is not a matching float vertex stream");
            }

            const bool bKnownAttribute =
                attributeName == "POSITION" || attributeName == "NORMAL" ||
                attributeName == "BITANGENT" || attributeName == "BINORMAL" ||
                attributeName == "TEXCOORD_0" || attributeName == "TANGENT" ||
                attributeName == "JOINTS_0" || attributeName == "WEIGHTS_0";
            const bool bExpectedType =
                ((attributeName == "POSITION" || attributeName == "NORMAL" ||
                    attributeName == "BITANGENT" || attributeName == "BINORMAL") && accessor.type == "VEC3") ||
                (attributeName == "TEXCOORD_0" && accessor.type == "VEC2") ||
                (attributeName == "TANGENT" && (accessor.type == "VEC3" || accessor.type == "VEC4")) ||
                ((attributeName == "JOINTS_0" || attributeName == "WEIGHTS_0") && accessor.type == "VEC4");
            const std::size_t componentCount = ComponentCount(accessor.type);
            if ((bKnownAttribute && !bExpectedType) ||
                (!bKnownAttribute && (componentCount < 2 || componentCount > 4)))
            {
                return Failure(
                    MeshValidationError::UnsupportedAccessor,
                    "primitive[" + std::to_string(primitiveIndex) + "] attribute " +
                    std::string(attributeName) + " has an unsupported type");
            }

            return {};
        }

        std::uint32_t ReadIndex(const std::uint8_t* data, int componentType)
        {
            if (componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_BYTE)
                return *data;
            if (componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_SHORT)
            {
                std::uint16_t value = 0;
                std::memcpy(&value, data, sizeof(value));
                return value;
            }

            std::uint32_t value = 0;
            std::memcpy(&value, data, sizeof(value));
            return value;
        }

        MeshValidationResult ValidateSkinStreamValues(
            const MeshRes& meshResource,
            std::size_t primitiveIndex,
            std::string_view attributeName,
            int accessorIndex,
            std::stop_token stopToken)
        {
            const Accessor& accessor = meshResource.accessorVec[accessorIndex];
            const MeshBufferView& view = meshResource.bufferViewVec[accessor.bufferViewIndex];
            const MeshBuffer& buffer = meshResource.bufferVec[view.bufferIndex];

            std::size_t dataOffset = 0;
            std::size_t valueCount = 0;
            if (!CheckedAdd(view.byteOffset, accessor.byteOffset, dataOffset) ||
                !CheckedMultiply(accessor.count, std::size_t{ 4 }, valueCount))
            {
                return Failure(
                    MeshValidationError::InvalidVertexData,
                    "primitive[" + std::to_string(primitiveIndex) + "] " +
                    std::string(attributeName) + " range overflows");
            }

            const bool bJointStream = attributeName == "JOINTS_0";
            const double maxJointIndex = static_cast<double>(std::numeric_limits<int>::max());
            for (std::size_t valueIndex = 0; valueIndex < valueCount; ++valueIndex)
            {
				if (ShouldCheckCancellation(valueIndex) && stopToken.stop_requested())
					return CancellationFailure();

                float value = 0.0f;
                std::memcpy(
                    &value,
                    buffer.data.data() + dataOffset + valueIndex * sizeof(float),
                    sizeof(value));
                const double numericValue = static_cast<double>(value);
                const bool bInvalidJoint = bJointStream &&
                    (numericValue < 0.0 || numericValue > maxJointIndex ||
                        std::trunc(numericValue) != numericValue);
                const bool bInvalidWeight = !bJointStream && numericValue < 0.0;
                if (!std::isfinite(value) || bInvalidJoint || bInvalidWeight)
                {
                    return Failure(
                        MeshValidationError::InvalidVertexData,
                        "primitive[" + std::to_string(primitiveIndex) + "] " +
                        std::string(attributeName) + " contains an invalid value");
                }
            }
            return {};
        }

        MeshValidationResult ValidateMesh(
			const MeshRes& meshResource,
			bool bRequireLoadedData,
			std::stop_token stopToken = {})
        {
			if (stopToken.stop_requested())
				return CancellationFailure();

            if (meshResource.version != 1)
            {
                return Failure(
                    MeshValidationError::UnsupportedVersion,
                    "unsupported mesh resource version " + std::to_string(meshResource.version));
            }
            if (meshResource.primitives.empty())
                return Failure(MeshValidationError::MissingPrimitive, "mesh resource has no primitives");
            if (meshResource.bufferVec.empty())
                return Failure(MeshValidationError::InvalidBuffer, "mesh resource has no buffers");
            if (meshResource.bufferVec.size() > MaxMeshBufferCount ||
                meshResource.bufferViewVec.size() > MaxMeshBufferViewCount ||
                meshResource.accessorVec.size() > MaxMeshAccessorCount ||
                meshResource.primitives.size() > MaxMeshPrimitiveCount)
            {
                return Failure(
                    MeshValidationError::ResourceLimitExceeded,
                    "mesh resource exceeds a schema collection limit");
            }

            std::size_t totalBufferBytes = 0;
            for (std::size_t bufferIndex = 0; bufferIndex < meshResource.bufferVec.size(); ++bufferIndex)
            {
				if (stopToken.stop_requested())
					return CancellationFailure();

                const MeshBuffer& buffer = meshResource.bufferVec[bufferIndex];
                if (buffer.byteLength <= 0 ||
                    static_cast<std::size_t>(buffer.byteLength) > MaxMeshBufferBytes ||
                    buffer.uri.empty())
                {
                    return Failure(
                        MeshValidationError::InvalidBuffer,
                        "buffer[" + std::to_string(bufferIndex) + "] has an invalid length or URI");
                }
                if (!bRequireLoadedData && !buffer.data.empty())
                {
                    return Failure(
                        MeshValidationError::InvalidBuffer,
                        "buffer[" + std::to_string(bufferIndex) + "] contains serialized runtime data");
                }

                std::size_t newTotalBufferBytes = 0;
                if (!CheckedAdd(
                        totalBufferBytes,
                        static_cast<std::size_t>(buffer.byteLength),
                        newTotalBufferBytes) ||
                    newTotalBufferBytes > MaxMeshTotalBufferBytes)
                {
                    return Failure(
                        MeshValidationError::ResourceLimitExceeded,
                        "mesh resource exceeds the total binary byte limit");
                }
                totalBufferBytes = newTotalBufferBytes;

                if (bRequireLoadedData && buffer.data.size() != static_cast<std::size_t>(buffer.byteLength))
                {
                    return Failure(
                        MeshValidationError::BufferSizeMismatch,
                        "buffer[" + std::to_string(bufferIndex) + "] loaded size does not match byteLength");
                }
            }

            for (std::size_t viewIndex = 0; viewIndex < meshResource.bufferViewVec.size(); ++viewIndex)
            {
				if (ShouldCheckCancellation(viewIndex) && stopToken.stop_requested())
					return CancellationFailure();

                const MeshBufferView& view = meshResource.bufferViewVec[viewIndex];
                if (view.bufferIndex < 0 ||
                    static_cast<std::size_t>(view.bufferIndex) >= meshResource.bufferVec.size() ||
                    view.byteLength == 0)
                {
                    return Failure(
                        MeshValidationError::InvalidBufferView,
                        "bufferView[" + std::to_string(viewIndex) + "] is invalid");
                }
                if (view.byteStride != 0)
                {
                    return Failure(
                        MeshValidationError::UnsupportedBufferStride,
                        "bufferView[" + std::to_string(viewIndex) + "] uses unsupported interleaved data");
                }

                std::size_t viewEnd = 0;
                const std::size_t bufferLength =
                    static_cast<std::size_t>(meshResource.bufferVec[view.bufferIndex].byteLength);
                if (!CheckedAdd(view.byteOffset, view.byteLength, viewEnd) || viewEnd > bufferLength)
                {
                    return Failure(
                        MeshValidationError::InvalidBufferView,
                        "bufferView[" + std::to_string(viewIndex) + "] exceeds its buffer");
                }
            }

            for (std::size_t accessorIndex = 0; accessorIndex < meshResource.accessorVec.size(); ++accessorIndex)
            {
                const MeshValidationResult result =
					ValidateAccessorRange(meshResource, accessorIndex, bRequireLoadedData, stopToken);
                if (!result)
                    return result;
            }

            std::size_t decodedCpuBytes = 0;
            for (std::size_t primitiveIndex = 0; primitiveIndex < meshResource.primitives.size(); ++primitiveIndex)
            {
				if (stopToken.stop_requested())
					return CancellationFailure();

                const Primitive& primitive = meshResource.primitives[primitiveIndex];
                if (primitive.mode != 4 || primitive.indices < 0 ||
                    static_cast<std::size_t>(primitive.indices) >= meshResource.accessorVec.size())
                {
                    return Failure(
                        MeshValidationError::InvalidPrimitive,
                        "primitive[" + std::to_string(primitiveIndex) + "] has invalid topology or indices");
                }

                const auto positionIt = primitive.attributes.find("POSITION");
                const auto normalIt = primitive.attributes.find("NORMAL");
                if (positionIt == primitive.attributes.end() || normalIt == primitive.attributes.end() ||
                    positionIt->second < 0 ||
                    static_cast<std::size_t>(positionIt->second) >= meshResource.accessorVec.size())
                {
                    return Failure(
                        MeshValidationError::InvalidPrimitive,
                        "primitive[" + std::to_string(primitiveIndex) + "] is missing POSITION or NORMAL");
                }

                const Accessor& positionAccessor = meshResource.accessorVec[positionIt->second];
                if (positionAccessor.componentType != HENGINE_COMPONENT_TYPE_FLOAT ||
                    positionAccessor.type != "VEC3" || positionAccessor.count == 0)
                {
                    return Failure(
                        MeshValidationError::UnsupportedAccessor,
                        "primitive[" + std::to_string(primitiveIndex) + "] POSITION is unsupported");
                }
                if (positionAccessor.count > MaxPrimitiveVertexCount)
                {
                    return Failure(
                        MeshValidationError::ResourceLimitExceeded,
                        "primitive[" + std::to_string(primitiveIndex) + "] exceeds the vertex limit");
                }

                const bool bHasJoints = primitive.attributes.contains("JOINTS_0");
                const bool bHasWeights = primitive.attributes.contains("WEIGHTS_0");
                if (bHasJoints != bHasWeights)
                {
                    return Failure(
                        MeshValidationError::InvalidPrimitive,
                        "primitive[" + std::to_string(primitiveIndex) + "] has incomplete skin streams");
                }

                for (const auto& [attributeName, accessorIndex] : primitive.attributes)
                {
                    const MeshValidationResult result = ValidatePrimitiveAttribute(
                        meshResource,
                        primitiveIndex,
                        attributeName,
                        accessorIndex,
                        positionAccessor.count);
                    if (!result)
                        return result;
                }

                const Accessor& indexAccessor = meshResource.accessorVec[primitive.indices];
                if (indexAccessor.type != "SCALAR" || !IsValidIndexComponent(indexAccessor.componentType) ||
                    indexAccessor.count == 0 || indexAccessor.count % 3 != 0)
                {
                    return Failure(
                        MeshValidationError::UnsupportedAccessor,
                        "primitive[" + std::to_string(primitiveIndex) + "] index accessor is unsupported");
                }
                if (indexAccessor.count > MaxPrimitiveIndexCount)
                {
                    return Failure(
                        MeshValidationError::ResourceLimitExceeded,
                        "primitive[" + std::to_string(primitiveIndex) + "] exceeds the index limit");
                }

                const std::size_t estimatedVertexBytes = bHasJoints
                    ? EstimatedSkinnedVertexBytes
                    : EstimatedStaticVertexBytes;
                std::size_t decodedVertexBytes = 0;
                std::size_t decodedIndexBytes = 0;
                std::size_t primitiveDecodedBytes = 0;
                std::size_t newDecodedCpuBytes = 0;
                if (!CheckedMultiply(positionAccessor.count, estimatedVertexBytes, decodedVertexBytes) ||
                    !CheckedMultiply(indexAccessor.count, sizeof(std::uint32_t), decodedIndexBytes) ||
                    !CheckedAdd(decodedVertexBytes, decodedIndexBytes, primitiveDecodedBytes) ||
                    !CheckedAdd(decodedCpuBytes, primitiveDecodedBytes, newDecodedCpuBytes) ||
                    newDecodedCpuBytes > MaxDecodedCpuBytes)
                {
                    return Failure(
                        MeshValidationError::ResourceLimitExceeded,
                        "mesh resource exceeds the decoded CPU byte limit");
                }
                decodedCpuBytes = newDecodedCpuBytes;

                if (bRequireLoadedData)
                {
                    const MeshBufferView& view = meshResource.bufferViewVec[indexAccessor.bufferViewIndex];
                    const MeshBuffer& buffer = meshResource.bufferVec[view.bufferIndex];
                    const std::size_t componentSize = ComponentSize(indexAccessor.componentType);
                    const std::size_t dataOffset = view.byteOffset + indexAccessor.byteOffset;
                    for (std::size_t index = 0; index < indexAccessor.count; ++index)
                    {
						if (ShouldCheckCancellation(index) && stopToken.stop_requested())
							return CancellationFailure();

                        const std::uint32_t value =
                            ReadIndex(buffer.data.data() + dataOffset + index * componentSize, indexAccessor.componentType);
                        if (value >= positionAccessor.count)
                        {
                            return Failure(
                                MeshValidationError::IndexOutOfRange,
                                "primitive[" + std::to_string(primitiveIndex) + "] index " +
                                std::to_string(index) + " exceeds its vertex stream");
                        }
                    }

                    if (bHasJoints)
                    {
                        const MeshValidationResult jointResult = ValidateSkinStreamValues(
                            meshResource,
                            primitiveIndex,
                            "JOINTS_0",
							primitive.attributes.at("JOINTS_0"),
							stopToken);
                        if (!jointResult)
                            return jointResult;

                        const MeshValidationResult weightResult = ValidateSkinStreamValues(
                            meshResource,
                            primitiveIndex,
                            "WEIGHTS_0",
							primitive.attributes.at("WEIGHTS_0"),
							stopToken);
                        if (!weightResult)
                            return weightResult;
                    }
                }
            }

            return {};
        }

        void ReleaseBufferData(MeshRes& meshResource)
        {
            for (MeshBuffer& buffer : meshResource.bufferVec)
                std::vector<std::uint8_t>().swap(buffer.data);
        }
    }

    MeshValidationResult MeshResourceValidator::ValidateSchema(const MeshRes& meshResource)
    {
        return ValidateMesh(meshResource, false);
    }

    MeshValidationResult MeshResourceValidator::ValidateLoaded(const MeshRes& meshResource)
    {
        return ValidateMesh(meshResource, true);
    }

    MeshValidationResult MeshResourceValidator::LoadAndValidateBuffers(
        MeshRes& meshResource,
        const std::filesystem::path& projectRoot,
		std::stop_token stopToken)
    {
		if (stopToken.stop_requested())
			return CancellationFailure();

		MeshValidationResult result = ValidateMesh(meshResource, false, stopToken);
        if (!result)
            return result;

        MeshRes pendingResource;
        try
        {
            pendingResource = meshResource;
        }
        catch (const std::bad_alloc&)
        {
            return Failure(MeshValidationError::AllocationFailed, "mesh resource staging allocation failed");
        }
        catch (const std::length_error&)
        {
            return Failure(MeshValidationError::AllocationFailed, "mesh resource staging allocation is too large");
        }

        result = NormalizeReferencedPath(
            projectRoot,
            ".hskel",
            "skeletonPath",
            pendingResource.skeletonPath);
        if (!result)
            return result;

        for (std::size_t primitiveIndex = 0; primitiveIndex < pendingResource.primitives.size(); ++primitiveIndex)
        {
			if (stopToken.stop_requested())
			{
				ReleaseBufferData(pendingResource);
				return CancellationFailure();
			}

            result = NormalizeReferencedPath(
                projectRoot,
                ".hmat",
                "primitive[" + std::to_string(primitiveIndex) + "].materialResPath",
                pendingResource.primitives[primitiveIndex].materialResPath);
            if (!result)
                return result;
        }

        for (std::size_t bufferIndex = 0; bufferIndex < pendingResource.bufferVec.size(); ++bufferIndex)
        {
			if (stopToken.stop_requested())
			{
				ReleaseBufferData(pendingResource);
				return CancellationFailure();
			}

            MeshBuffer& buffer = pendingResource.bufferVec[bufferIndex];
            std::filesystem::path fullPath;
            std::string normalizedPath;
            const auto pathResult = Utils::TryResolveProjectRelativePath(
                projectRoot,
                buffer.uri,
                fullPath,
                normalizedPath);
            if (!pathResult)
            {
                ReleaseBufferData(pendingResource);
                return Failure(
                    MeshValidationError::InvalidBufferPath,
                    "buffer[" + std::to_string(bufferIndex) + "] has an unsafe URI: " + pathResult.Message);
            }

            std::error_code fileError;
            if (!std::filesystem::is_regular_file(fullPath, fileError) || fileError)
            {
                ReleaseBufferData(pendingResource);
                return Failure(
                    MeshValidationError::BufferFileError,
                    "buffer[" + std::to_string(bufferIndex) + "] file is missing or unreadable");
            }

            const std::uintmax_t fileSize = std::filesystem::file_size(fullPath, fileError);
            if (fileError)
            {
                ReleaseBufferData(pendingResource);
                return Failure(
                    MeshValidationError::BufferFileError,
                    "buffer[" + std::to_string(bufferIndex) + "] size could not be read: " + fileError.message());
            }
            if (fileSize != static_cast<std::uintmax_t>(buffer.byteLength))
            {
                ReleaseBufferData(pendingResource);
                return Failure(
                    MeshValidationError::BufferSizeMismatch,
                    "buffer[" + std::to_string(bufferIndex) + "] file size does not match byteLength");
            }

            try
            {
                buffer.data.resize(static_cast<std::size_t>(buffer.byteLength));
            }
            catch (const std::bad_alloc&)
            {
                ReleaseBufferData(pendingResource);
                return Failure(
                    MeshValidationError::AllocationFailed,
                    "buffer[" + std::to_string(bufferIndex) + "] could not be allocated");
            }
            catch (const std::length_error&)
            {
                ReleaseBufferData(pendingResource);
                return Failure(
                    MeshValidationError::AllocationFailed,
                    "buffer[" + std::to_string(bufferIndex) + "] allocation is too large");
            }

            std::ifstream file(fullPath, std::ios::binary);
            if (!file.is_open())
            {
                ReleaseBufferData(pendingResource);
                return Failure(
                    MeshValidationError::BufferFileError,
                    "buffer[" + std::to_string(bufferIndex) + "] could not be opened");
            }

			constexpr std::size_t ReadChunkBytes = std::size_t{ 1 } << 20;
			std::size_t readOffset = 0;
			while (readOffset < buffer.data.size())
			{
				if (stopToken.stop_requested())
				{
					ReleaseBufferData(pendingResource);
					return CancellationFailure();
				}

				const std::size_t chunkBytes =
					std::min(ReadChunkBytes, buffer.data.size() - readOffset);
				file.read(
					reinterpret_cast<char*>(buffer.data.data() + readOffset),
					static_cast<std::streamsize>(chunkBytes));
				if (file.gcount() != static_cast<std::streamsize>(chunkBytes) || file.bad())
				{
					ReleaseBufferData(pendingResource);
					return Failure(
						MeshValidationError::BufferFileError,
						"buffer[" + std::to_string(bufferIndex) + "] could not be read completely");
				}
				readOffset += chunkBytes;
			}

            buffer.uri = std::move(normalizedPath);
        }

		result = ValidateMesh(pendingResource, true, stopToken);
        if (!result)
        {
            ReleaseBufferData(pendingResource);
            return result;
        }

        meshResource = std::move(pendingResource);
        return {};
    }
}
