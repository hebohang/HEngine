#include "Hepch.h"

#include "MeshLoader.h"
#include "Mesh.h"
#include "MeshResourceValidator.h"
#include "MeshBindPose.h"
#include "Runtime/Core/TaskScheduler.h"
#include "SubMesh.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialResourceCache.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ResType/Mesh.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "_generated/serializer/Mesh.gen.h"
#include "_generated/serializer/Material.gen.h"

#if HE_RUNTIME_ASSET_IMPORT
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#endif

#include <cmath>
#include <atomic>
#include <cstring>
#include <cstdint>
#if HE_RUNTIME_ASSET_IMPORT
#include <regex>
#endif
#include <stdexcept>
#include <system_error>

namespace HEngine
{
	namespace Utils
	{
		static void SetVertexBoneDataToDefault(SkinnedVertex& vertex)
		{
			for (int i = 0; i < MAX_BONE_INFLUENCE; i++)
			{
				vertex.mBoneIDs[i] = -1;
				vertex.mWeights[i] = 0.0f;
			}
		}

		template <typename Function>
		static bool ForEachCancellable(
			std::size_t count,
			std::stop_token stopToken,
			Function&& function)
		{
			for (std::size_t index = 0; index < count; ++index)
			{
				if ((index & 0x0fff) == 0 && stopToken.stop_requested())
					return false;
				function(index);
			}
			return !stopToken.stop_requested();
		}

		template <typename VertexType>
		static bool CalTangentBitangent(
			std::vector<VertexType>& vertices,
			const std::vector<uint32_t>& indices,
			std::stop_token stopToken = {})
		{
			// 如果模型本身不含tangent与bitangent，则手工计算
			if (!ForEachCancellable(vertices.size(), stopToken, [&vertices](std::size_t index)
			{
				vertices[index].Tangent = glm::vec3(0.0f);
				vertices[index].Bitangent = glm::vec3(0.0f);
			}))
			{
				return false;
			}

			for (size_t i = 0; i + 2 < indices.size(); i += 3)
			{
				if ((i & 0x0fff) == 0 && stopToken.stop_requested())
					return false;

				// infer to http://www.opengl-tutorial.org/intermediate-tutorials/tutorial-13-normal-mapping/#computing-the-tangents-and-bitangents

				const uint32_t i0 = indices[i + 0];
				const uint32_t i1 = indices[i + 1];
				const uint32_t i2 = indices[i + 2];
				if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
					continue;

				// Shortcuts for vertices
				const glm::vec3& v0 = vertices[i0].Pos;
				const glm::vec3& v1 = vertices[i1].Pos;
				const glm::vec3& v2 = vertices[i2].Pos;

				// Shortcuts for UVs
				const glm::vec2& uv0 = vertices[i0].TexCoord;
				const glm::vec2& uv1 = vertices[i1].TexCoord;
				const glm::vec2& uv2 = vertices[i2].TexCoord;

				// Edges of the triangle : position delta
				glm::vec3 deltaPos1 = v1 - v0;
				glm::vec3 deltaPos2 = v2 - v0;

				// UV delta
				glm::vec2 deltaUV1 = uv1 - uv0;
				glm::vec2 deltaUV2 = uv2 - uv0;

				const float determinant = deltaUV1.x * deltaUV2.y - deltaUV1.y * deltaUV2.x;
				if (std::abs(determinant) <= 1e-8f)
					continue;

				float r = 1.0f / determinant;
				glm::vec3 tangent = (deltaPos1 * deltaUV2.y - deltaPos2 * deltaUV1.y) * r;
				glm::vec3 bitangent = (deltaPos2 * deltaUV1.x - deltaPos1 * deltaUV2.x) * r;

				vertices[i0].Tangent += tangent;
				vertices[i1].Tangent += tangent;
				vertices[i2].Tangent += tangent;

				vertices[i0].Bitangent += bitangent;
				vertices[i1].Bitangent += bitangent;
				vertices[i2].Bitangent += bitangent;
			}
			return !stopToken.stop_requested();
		}

		template <typename VertexType>
		static bool NormalizeTangentFrame(
			std::vector<VertexType>& vertices,
			std::stop_token stopToken = {})
		{
			for (std::size_t index = 0; index < vertices.size(); ++index)
			{
				if ((index & 0x0fff) == 0 && stopToken.stop_requested())
					return false;

				VertexType& vertex = vertices[index];
				const float normalLenSq = glm::dot(vertex.Normal, vertex.Normal);
				const glm::vec3 normal = normalLenSq > 1e-8f
					? vertex.Normal * glm::inversesqrt(normalLenSq)
					: glm::vec3(0.0f, 0.0f, 1.0f);

				glm::vec3 tangent = vertex.Tangent - normal * glm::dot(normal, vertex.Tangent);
				if (glm::dot(tangent, tangent) <= 1e-8f)
				{
					tangent = std::abs(normal.z) < 0.999f
						? glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), normal)
						: glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal);
				}
				tangent = glm::normalize(tangent);

				const glm::vec3 generatedBitangent = glm::cross(normal, tangent);
				const float handedness = glm::dot(generatedBitangent, vertex.Bitangent) < 0.0f ? -1.0f : 1.0f;

				vertex.Normal = normal;
				vertex.Tangent = tangent;
				vertex.Bitangent = generatedBitangent * handedness;
			}
			return !stopToken.stop_requested();
		}
	}

	void MeshLoader::LoadModel(Mesh* mesh, const std::string& path)
	{
		HE_PROFILE_SECTION("MeshLoader::LoadModel");
		auto fullPath = ConfigManager::GetFullPath(path);
		if (Utils::PathToUtf8(fullPath.extension()) == ".hmesh")
			LoadModelHMESH(mesh, fullPath);
#if !HE_RUNTIME_ASSET_IMPORT
		else
			throw std::runtime_error("Runtime raw model import is disabled; import this asset to .hmesh with HAssetImporter or the Editor: " + path);
#else
		else
		{
#if WITH_EDITOR
			mesh->InvalidateEditorPickData();
#endif
			mesh->mMaterial.clear();
			mesh->mSubMeshes.clear();
			LoadModelASSIMP(mesh, path);
		}
#endif
	}

	MeshDecodeResult MeshLoader::DecodeModelHMESH(
		const std::filesystem::path& fullPath,
		const std::filesystem::path& projectRoot,
		std::stop_token stopToken)
	{
		HE_PROFILE_SECTION("MeshLoader::LoadModelHMESH");
		auto Failure = [](MeshDecodeError code, std::string message)
		{
			return MeshDecodeResult{ code, std::move(message), {} };
		};

		if (stopToken.stop_requested())
			return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");

		constexpr std::uintmax_t MaxMeshMetadataBytes = std::uintmax_t{ 32 } * 1024 * 1024;
		std::error_code metadataError;
		if (!std::filesystem::is_regular_file(fullPath, metadataError) || metadataError)
		{
			return Failure(MeshDecodeError::MetadataFileError,
				"Mesh metadata is missing or is not a regular file: " + Utils::PathToUtf8(fullPath) +
				(metadataError ? ": " + metadataError.message() : std::string()));
		}

		const std::uintmax_t metadataBytes = std::filesystem::file_size(fullPath, metadataError);
		if (metadataError)
		{
			return Failure(MeshDecodeError::MetadataFileError,
				"Cannot read mesh metadata size " + Utils::PathToUtf8(fullPath) + ": " + metadataError.message());
		}
		if (metadataBytes > MaxMeshMetadataBytes)
		{
			return Failure(
				MeshDecodeError::MetadataTooLarge,
				"Mesh metadata exceeds the 32 MiB limit: " + Utils::PathToUtf8(fullPath));
		}

		MeshRes meshRes;
		try
		{
			const Json meshJson = Utils::LoadJsonFile(fullPath);
			Serializer::Deserialize(meshJson, meshRes);
		}
		catch (const std::bad_alloc&)
		{
			return Failure(MeshDecodeError::AllocationFailed, "Mesh metadata allocation failed.");
		}
		catch (const std::length_error&)
		{
			return Failure(MeshDecodeError::AllocationFailed, "Mesh metadata allocation is too large.");
		}
		catch (const std::exception& exception)
		{
			return Failure(
				MeshDecodeError::InvalidResource,
				"Cannot deserialize mesh metadata " + Utils::PathToUtf8(fullPath) + ": " + exception.what());
		}

		const MeshValidationResult validation = MeshResourceValidator::LoadAndValidateBuffers(
			meshRes,
			projectRoot,
			stopToken);
		if (!validation)
		{
			const MeshDecodeError decodeError = validation.Code == MeshValidationError::Cancelled
				? MeshDecodeError::Cancelled
				: validation.Code == MeshValidationError::AllocationFailed
					? MeshDecodeError::AllocationFailed
					: MeshDecodeError::InvalidResource;
			return Failure(
				decodeError,
				"Invalid mesh resource " + Utils::PathToUtf8(fullPath) + ": " + validation.Message);
		}

		try
		{
			DecodedModel decodedModel;
			decodedModel.SkeletonPath = meshRes.skeletonPath;

			std::unordered_map<std::string, uint32_t> materialPathToIndex;

		for (const Primitive& primitive : meshRes.primitives)
		{
			if (stopToken.stop_requested())
				return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");

			DecodedSubMesh decodedSubMesh;
			std::vector<uint32_t>& indices = decodedSubMesh.Indices;

			const auto decodeIndices = [&](int indexAccessor, std::vector<uint32_t>& indices)
			{
				const Accessor& indiceAccessor = meshRes.accessorVec[indexAccessor];
				const MeshBufferView& bufferView = meshRes.bufferViewVec[indiceAccessor.bufferViewIndex];
				const MeshBuffer& buffer = meshRes.bufferVec[bufferView.bufferIndex];
				std::size_t byteOffset = bufferView.byteOffset + indiceAccessor.byteOffset;

				auto CopyIndicesData = [&buffer, &byteOffset, &indiceAccessor, &indices, stopToken](auto& dataVec)
				{
					dataVec.resize(indiceAccessor.count);
					std::size_t totalBytes = indiceAccessor.count * Utils::GetComponentSizeInBytes(indiceAccessor.componentType) * Utils::GetNumComponentsInType(indiceAccessor.type);
					std::memcpy(dataVec.data(), buffer.data.data() + byteOffset, totalBytes);
					indices.resize(dataVec.size());
                    if (dataVec.size() >= 65536)
                    {
                        std::atomic_bool cancelled{false};
                        (void)GetRuntimeTaskScheduler().ParallelFor(
                            "Mesh index conversion", dataVec.size(), 16384,
                            [&](size_t i)
                            {
                                if ((i & 0x0fff) == 0 && stopToken.stop_requested())
                                    cancelled.store(true, std::memory_order_relaxed);
                                if (!cancelled.load(std::memory_order_relaxed))
                                    indices[i] = static_cast<uint32_t>(dataVec[i]);
                            }, TaskPriority::Background);
                        return !cancelled.load(std::memory_order_relaxed);
                    }
					for (size_t i = 0; i < dataVec.size(); ++i)
					{
						if ((i & 0x0fff) == 0 && stopToken.stop_requested())
							return false;
						indices[i] = static_cast<uint32_t>(dataVec[i]);
					}
					return true;
				};

				bool bCopied = false;
				if (indiceAccessor.componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_BYTE)
				{
					std::vector<uint8_t> dataVec;
					bCopied = CopyIndicesData(dataVec);
				}
				else if (indiceAccessor.componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_SHORT)
				{
					std::vector<uint16_t> dataVec;
					bCopied = CopyIndicesData(dataVec);
				}
				else if (indiceAccessor.componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_INT)
				{
					std::vector<uint32_t> dataVec;
					bCopied = CopyIndicesData(dataVec);
				}
				return bCopied;
			};
			if (!decodeIndices(primitive.indices, indices))
				return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
			for (const MeshLod& lod : primitive.lods)
			{
				SubMeshLod decodedLod;
				decodedLod.ObjectSpaceError = lod.objectSpaceError;
				if (!decodeIndices(lod.indices, decodedLod.Indices))
					return Failure(MeshDecodeError::Cancelled, "Mesh LOD decode was cancelled.");
				decodedSubMesh.Lods.push_back(std::move(decodedLod));
			}

			// vertices
			const bool bIsSkinnedPrimitive =
				primitive.attributes.count("JOINTS_0") && primitive.attributes.count("WEIGHTS_0");

			if (bIsSkinnedPrimitive)
			{
				std::vector<SkinnedVertex> vertices;
				bool initSize = false;

				bool bWithTexCoord = false;
				bool bWithTangent = false;
				bool bWithBitangent = false;

				for (const auto& [attrName, attrAccessorIndex] : primitive.attributes)
				{
					if (stopToken.stop_requested())
						return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");

					const Accessor& attrAccessor = meshRes.accessorVec[attrAccessorIndex];
					const MeshBufferView& bufferView = meshRes.bufferViewVec[attrAccessor.bufferViewIndex];
					const MeshBuffer& buffer = meshRes.bufferVec[bufferView.bufferIndex];
					std::size_t byteOffset = bufferView.byteOffset + attrAccessor.byteOffset;

					if (!initSize)
					{
						vertices.resize(attrAccessor.count);
						if (!Utils::ForEachCancellable(vertices.size(), stopToken, [&vertices](std::size_t index)
						{
							Utils::SetVertexBoneDataToDefault(vertices[index]);
						}))
							return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
						initSize = true;
					}

					auto CopyAttrData = [&buffer, &byteOffset, &attrAccessor](auto& dataVec)
					{
						dataVec.resize(attrAccessor.count);
						std::size_t totalBytes = attrAccessor.count * Utils::GetComponentSizeInBytes(attrAccessor.componentType) * Utils::GetNumComponentsInType(attrAccessor.type);
						std::memcpy(dataVec.data(), buffer.data.data() + byteOffset, totalBytes);
					};

					const int numComp = Utils::GetNumComponentsInType(attrAccessor.type);
                    if (attrName == "CLOTH_WEIGHT")
                    {
                        const size_t stride = bufferView.byteStride != 0 ? bufferView.byteStride : sizeof(float);
                        for (size_t index = 0; index < vertices.size(); ++index)
                            std::memcpy(&vertices[index].ClothWeight,
                                buffer.data.data() + byteOffset + index * stride, sizeof(float));
                        continue;
                    }
                    if (attrName == "TEXCOORD_1" || attrName == "COLOR_0")
                    {
                        for (size_t index = 0; index < vertices.size(); ++index)
                        {
                            const auto* source = buffer.data.data() + byteOffset + index * numComp * sizeof(float);
                            if (attrName == "TEXCOORD_1") std::memcpy(&vertices[index].TexCoord1, source, 2 * sizeof(float));
                            else { vertices[index].Color = glm::vec4(1.0f); std::memcpy(&vertices[index].Color, source, numComp * sizeof(float)); }
                        }
                        continue;
                    }


					if (numComp == 2)
					{
						HE_CORE_ASSERT(attrAccessor.componentType == HENGINE_COMPONENT_TYPE_FLOAT, "We do not support non-float VEC2 now.");
						std::vector<glm::vec2> dataVec;
						CopyAttrData(dataVec);

						if (attrName == "TEXCOORD_0")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t index)
							{
								vertices[index].TexCoord = dataVec[index];
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
							bWithTexCoord = true;
						}
					}

					if (numComp == 3)
					{
						HE_CORE_ASSERT(attrAccessor.componentType == HENGINE_COMPONENT_TYPE_FLOAT, "We do not support non-float VEC3 now.");
						std::vector<glm::vec3> dataVec;
						CopyAttrData(dataVec);

						if (attrName == "POSITION")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t index)
							{
								vertices[index].Pos = dataVec[index];
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
						}
						else if (attrName == "NORMAL")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t index)
							{
								vertices[index].Normal = dataVec[index];
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
						}
						else if (attrName == "TANGENT")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t index)
							{
								vertices[index].Tangent = dataVec[index];
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
							bWithTangent = true;
						}
						else if (attrName == "BITANGENT" || attrName == "BINORMAL")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t index)
							{
								vertices[index].Bitangent = dataVec[index];
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
							bWithBitangent = true;
						}
					}

					if (numComp == 4)
					{
						HE_CORE_ASSERT(attrAccessor.componentType == HENGINE_COMPONENT_TYPE_FLOAT, "We do not support non-float VEC4 now.");
						std::vector<glm::vec4> dataVec;
						CopyAttrData(dataVec);

						if (attrName == "TANGENT")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t i)
							{
								vertices[i].Tangent = glm::vec3(dataVec[i]);
								vertices[i].Bitangent = glm::cross(vertices[i].Normal, vertices[i].Tangent) * dataVec[i].w;
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
							bWithTangent = true;
							bWithBitangent = true;
						}
						else if (attrName.starts_with("JOINTS_"))
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t i)
							{
								vertices[i].mBoneIDs[(attrName.back()-'0')*4+0] = static_cast<int>(dataVec[i].x);
								vertices[i].mBoneIDs[(attrName.back()-'0')*4+1] = static_cast<int>(dataVec[i].y);
								vertices[i].mBoneIDs[(attrName.back()-'0')*4+2] = static_cast<int>(dataVec[i].z);
								vertices[i].mBoneIDs[(attrName.back()-'0')*4+3] = static_cast<int>(dataVec[i].w);
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
						}
						else if (attrName.starts_with("WEIGHTS_"))
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t i)
							{
								vertices[i].mWeights[(attrName.back()-'0')*4+0] = dataVec[i].x;
								vertices[i].mWeights[(attrName.back()-'0')*4+1] = dataVec[i].y;
								vertices[i].mWeights[(attrName.back()-'0')*4+2] = dataVec[i].z;
								vertices[i].mWeights[(attrName.back()-'0')*4+3] = dataVec[i].w;
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
						}
					}
				}

				if (!bWithTexCoord)
				{
					if (!Utils::ForEachCancellable(vertices.size(), stopToken, [&vertices](std::size_t index)
					{
						vertices[index].TexCoord = glm::vec2(0.0f, 0.0f);
					}))
						return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
				}

				if (!bWithTangent || !bWithBitangent)
				{
					if (!Utils::CalTangentBitangent(vertices, indices, stopToken))
						return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
				}
				if (!Utils::NormalizeTangentFrame(vertices, stopToken))
					return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");

				for (auto& vertex : vertices)
                {
                    double total = 0;
                    for (float weight : vertex.mWeights) total += weight;
                    if (total > 0)
                        for (float& weight : vertex.mWeights) weight = static_cast<float>(weight / total);
                }
                decodedSubMesh.SkinnedVertices = std::move(vertices);
			}
			else
			{
				std::vector<StaticVertex> vertices;
				bool initSize = false;

				bool bWithTexCoord = false;
				bool bWithTangent = false;
				bool bWithBitangent = false;

				for (const auto& [attrName, attrAccessorIndex] : primitive.attributes)
				{
					if (stopToken.stop_requested())
						return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");

					const Accessor& attrAccessor = meshRes.accessorVec[attrAccessorIndex];
					const MeshBufferView& bufferView = meshRes.bufferViewVec[attrAccessor.bufferViewIndex];
					const MeshBuffer& buffer = meshRes.bufferVec[bufferView.bufferIndex];
					std::size_t byteOffset = bufferView.byteOffset + attrAccessor.byteOffset;

					if (!initSize)
					{
						vertices.resize(attrAccessor.count);
						initSize = true;
					}

					auto CopyAttrData = [&buffer, &byteOffset, &attrAccessor](auto& dataVec)
					{
						dataVec.resize(attrAccessor.count);
						std::size_t totalBytes = attrAccessor.count * Utils::GetComponentSizeInBytes(attrAccessor.componentType) * Utils::GetNumComponentsInType(attrAccessor.type);
						std::memcpy(dataVec.data(), buffer.data.data() + byteOffset, totalBytes);
					};

					const int numComp = Utils::GetNumComponentsInType(attrAccessor.type);
                    if (attrName == "TEXCOORD_1" || attrName == "COLOR_0")
                    {
                        for (size_t index = 0; index < vertices.size(); ++index)
                        {
                            const auto* source = buffer.data.data() + byteOffset + index * numComp * sizeof(float);
                            if (attrName == "TEXCOORD_1") std::memcpy(&vertices[index].TexCoord1, source, 2 * sizeof(float));
                            else { vertices[index].Color = glm::vec4(1.0f); std::memcpy(&vertices[index].Color, source, numComp * sizeof(float)); }
                        }
                        continue;
                    }


					if (numComp == 2)
					{
						HE_CORE_ASSERT(attrAccessor.componentType == HENGINE_COMPONENT_TYPE_FLOAT, "We do not support non-float VEC2 now.");
						std::vector<glm::vec2> dataVec;
						CopyAttrData(dataVec);

						if (attrName == "TEXCOORD_0")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t index)
							{
								vertices[index].TexCoord = dataVec[index];
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
							bWithTexCoord = true;
						}
					}

					if (numComp == 3)
					{
						HE_CORE_ASSERT(attrAccessor.componentType == HENGINE_COMPONENT_TYPE_FLOAT, "We do not support non-float VEC3 now.");
						std::vector<glm::vec3> dataVec;
						CopyAttrData(dataVec);

						if (attrName == "POSITION")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t index)
							{
								vertices[index].Pos = dataVec[index];
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
						}
						else if (attrName == "NORMAL")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t index)
							{
								vertices[index].Normal = dataVec[index];
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
						}
						else if (attrName == "TANGENT")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t index)
							{
								vertices[index].Tangent = dataVec[index];
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
							bWithTangent = true;
						}
						else if (attrName == "BITANGENT" || attrName == "BINORMAL")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t index)
							{
								vertices[index].Bitangent = dataVec[index];
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
							bWithBitangent = true;
						}
					}

					if (numComp == 4)
					{
						HE_CORE_ASSERT(attrAccessor.componentType == HENGINE_COMPONENT_TYPE_FLOAT, "We do not support non-float VEC4 now.");
						std::vector<glm::vec4> dataVec;
						CopyAttrData(dataVec);

						if (attrName == "TANGENT")
						{
							if (!Utils::ForEachCancellable(dataVec.size(), stopToken, [&](std::size_t i)
							{
								vertices[i].Tangent = glm::vec3(dataVec[i]);
								vertices[i].Bitangent = glm::cross(vertices[i].Normal, vertices[i].Tangent) * dataVec[i].w;
							}))
								return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
							bWithTangent = true;
							bWithBitangent = true;
						}
					}
				}

				if (!bWithTexCoord)
				{
					if (!Utils::ForEachCancellable(vertices.size(), stopToken, [&vertices](std::size_t index)
					{
						vertices[index].TexCoord = glm::vec2(0.0f, 0.0f);
					}))
						return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
				}

				if (!bWithTangent || !bWithBitangent)
				{
					if (!Utils::CalTangentBitangent(vertices, indices, stopToken))
						return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");
				}
				if (!Utils::NormalizeTangentFrame(vertices, stopToken))
					return Failure(MeshDecodeError::Cancelled, "Mesh decode was cancelled.");

				decodedSubMesh.StaticVertices = std::move(vertices);
			}

                // Decode validated dense FLOAT/VEC3 target streams with accessor stride.
                const size_t vertexCount = decodedSubMesh.IsSkinned() ? decodedSubMesh.SkinnedVertices.size() : decodedSubMesh.StaticVertices.size();
                if (primitive.targets.size() > 256)
                    return Failure(MeshDecodeError::InvalidResource, "Morph target count exceeds 256.");
                for (size_t targetIndex = 0; targetIndex < primitive.targets.size(); ++targetIndex)
                {
                    MorphTarget target;
                    target.Name = targetIndex < primitive.targetNames.size() ? primitive.targetNames[targetIndex] : "Target_" + std::to_string(targetIndex);
                    target.DefaultWeight = targetIndex < primitive.targetWeights.size() ? primitive.targetWeights[targetIndex] :
                        (targetIndex < meshRes.weights.size() ? meshRes.weights[targetIndex] : 0.0f);
                    if (target.Name.empty() || !std::isfinite(target.DefaultWeight))
                        return Failure(MeshDecodeError::InvalidResource, "Invalid morph target name or weight.");
                    for (const auto& [semantic, accessorIndex] : primitive.targets[targetIndex])
                    {
                        if (semantic != "POSITION" && semantic != "NORMAL" && semantic != "TANGENT")
                            return Failure(MeshDecodeError::InvalidResource, "Unsupported morph target semantic.");
                        if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= meshRes.accessorVec.size())
                            return Failure(MeshDecodeError::InvalidResource, "Invalid morph target accessor.");
                        const Accessor& accessor = meshRes.accessorVec[accessorIndex];
                        if (accessor.count != vertexCount || accessor.type != "VEC3" || accessor.componentType != HENGINE_COMPONENT_TYPE_FLOAT)
                            return Failure(MeshDecodeError::InvalidResource, "Morph target requires matching FLOAT/VEC3 vertices.");
                        const MeshBufferView& view = meshRes.bufferViewVec[accessor.bufferViewIndex];
                        const MeshBuffer& buffer = meshRes.bufferVec[view.bufferIndex];
                        auto& output = semantic == "POSITION" ? target.Positions : (semantic == "NORMAL" ? target.Normals : target.Tangents);
                        if (vertexCount > (256u * 1024u * 1024u) / (sizeof(glm::vec3) * 3u * std::max(size_t{1}, primitive.targets.size())))
                            return Failure(MeshDecodeError::InvalidResource, "Morph target section exceeds 256 MiB decoded limit.");
                        output.resize(vertexCount);
                        const size_t stride = view.byteStride ? view.byteStride : sizeof(float) * 3;
                        for (size_t vertex = 0; vertex < vertexCount; ++vertex)
                        {
                            if ((vertex & 4095) == 0 && stopToken.stop_requested())
                                return Failure(MeshDecodeError::Cancelled, "Morph decode was cancelled.");
                            std::memcpy(&output[vertex], buffer.data.data() + view.byteOffset + accessor.byteOffset + vertex * stride, sizeof(float) * 3);
                            const auto& delta = output[vertex];
                            if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z))
                                return Failure(MeshDecodeError::InvalidResource, "Morph target contains non-finite deltas.");
                        }
                    }
                    decodedSubMesh.MorphTargets.push_back(std::move(target));
                }

				// material
				if (!primitive.materialResPath.empty())
				{
					const auto materialIter = materialPathToIndex.find(primitive.materialResPath);
					if (materialIter != materialPathToIndex.end())
					{
						decodedSubMesh.MaterialIndex = materialIter->second;
					}
					else
					{
						decodedSubMesh.MaterialIndex = static_cast<std::uint32_t>(decodedModel.MaterialPaths.size());
						materialPathToIndex.emplace(primitive.materialResPath, decodedSubMesh.MaterialIndex);
						decodedModel.MaterialPaths.push_back(primitive.materialResPath);
					}
				}
				decodedModel.SubMeshes.push_back(std::move(decodedSubMesh));
			}

			return { MeshDecodeError::None, {}, std::move(decodedModel) };
		}
		catch (const std::bad_alloc&)
		{
			return Failure(MeshDecodeError::AllocationFailed, "Mesh decode allocation failed.");
		}
		catch (const std::length_error&)
		{
			return Failure(MeshDecodeError::AllocationFailed, "Mesh decode allocation is too large.");
		}
	}

	void MeshLoader::FinalizeModelHMESH(Mesh* mesh, DecodedModel&& decodedModel)
	{
		if (mesh == nullptr)
			throw std::invalid_argument("Cannot finalize a mesh into a null destination.");
		if (decodedModel.SubMeshes.empty())
			throw std::runtime_error("Cannot finalize a decoded mesh without submeshes.");

		const std::size_t materialCount = decodedModel.MaterialPaths.empty()
			? std::size_t{ 1 }
			: decodedModel.MaterialPaths.size();
		for (const DecodedSubMesh& decodedSubMesh : decodedModel.SubMeshes)
		{
			const bool bHasStaticVertices = !decodedSubMesh.StaticVertices.empty();
			const bool bHasSkinnedVertices = !decodedSubMesh.SkinnedVertices.empty();
			if (bHasStaticVertices == bHasSkinnedVertices || decodedSubMesh.Indices.empty())
				throw std::runtime_error("Cannot finalize an incomplete decoded submesh.");
			if (decodedSubMesh.MaterialIndex >= materialCount)
				throw std::runtime_error("Decoded submesh material index is out of range.");
		}

		std::vector<Ref<Material>> materials;
		materials.reserve(materialCount);
		if (decodedModel.MaterialPaths.empty())
		{
			materials.push_back(CreateRef<Material>());
		}
		else
		{
			for (const std::string& materialPath : decodedModel.MaterialPaths)
				materials.push_back(
					MaterialResourceCache::GetInstance().Get(materialPath));
		}

        size_t requiredBones = 0;
        for (const auto& section : decodedModel.SubMeshes)
            for (const auto& vertex : section.SkinnedVertices)
                for (int slot = 0; slot < MAX_BONE_INFLUENCE; ++slot)
                    if (vertex.mWeights[slot] > 0)
                    {
                        if (vertex.mBoneIDs[slot] < 0) throw std::invalid_argument("Weighted vertex has negative joint index");
                        requiredBones = std::max(requiredBones, static_cast<size_t>(vertex.mBoneIDs[slot]) + 1);
                    }
        std::vector<glm::mat4> bindPalette;
        std::map<std::string, BoneInfo> bindBoneInfo;
        if (requiredBones && !decodedModel.SkeletonPath.empty() && decodedModel.SkeletonPath != "None")
        {
            SkeletonRes skeleton;
            std::string error;
            if (!LoadSkeletonRes(ConfigManager::GetFullPath(decodedModel.SkeletonPath), skeleton, &error))
                throw std::runtime_error("Mesh bind skeleton load failed: " + error);
            bindPalette = BuildMeshBindPalette(skeleton);
            if (requiredBones > bindPalette.size()) throw std::invalid_argument("Mesh joint index exceeds skeleton bind palette");
            for (size_t index = 0; index < skeleton.bones.size(); ++index)
            {
                const auto& bone = skeleton.bones[index];
                if (bone.name.empty() || bindBoneInfo.contains(bone.name))
                    throw std::invalid_argument("Mesh bind skeleton has empty or duplicate bone name");
                BoneInfo info;
                info.id = bone.index;
                info.offset = glm::make_mat4(skeleton.inverseBindMatrices[index].data());
                bindBoneInfo.emplace(bone.name, info);
            }
        }
        else if (requiredBones)
        {
            // Legacy skeleton-less meshes store bind-space vertices; retain identity skinning,
            // with capacity derived from their actual weighted indices rather than 100.
            bindPalette.assign(requiredBones, glm::mat4(1));
        }
		std::vector<Ref<SubMesh>> subMeshes;
		subMeshes.reserve(decodedModel.SubMeshes.size());
		for (DecodedSubMesh& decodedSubMesh : decodedModel.SubMeshes)
		{
			if (decodedSubMesh.IsSkinned())
			{
				subMeshes.push_back(CreateRef<SubMesh>(
					std::move(decodedSubMesh.SkinnedVertices),
					std::move(decodedSubMesh.Indices),
					decodedSubMesh.MaterialIndex));
			}
			else
			{
				subMeshes.push_back(CreateRef<SubMesh>(
					std::move(decodedSubMesh.StaticVertices),
					std::move(decodedSubMesh.Indices),
					decodedSubMesh.MaterialIndex));
			}
			subMeshes.back()->SetLods(std::move(decodedSubMesh.Lods));
            subMeshes.back()->SetMorphTargets(std::move(decodedSubMesh.MorphTargets));
            if (subMeshes.back()->HasEffectiveMorphTargets())
            {
                subMeshes.back()->BeginDeformation();
                subMeshes.back()->ApplyMorphWeights({});
                subMeshes.back()->EndDeformation(true);
            }
		}

		mesh->mSkeletonPath = std::move(decodedModel.SkeletonPath);
        mesh->mBindBoneMatrices = std::move(bindPalette);
        mesh->mBoneCounter = static_cast<int>(mesh->mBindBoneMatrices.size());
        mesh->mBoneInfoMap = std::move(bindBoneInfo);
		mesh->mMaterial = std::move(materials);
		mesh->mSubMeshes = std::move(subMeshes);
#if WITH_EDITOR
		mesh->InvalidateEditorPickData();
#endif
	}

	void MeshLoader::LoadModelHMESH(Mesh* mesh, const std::filesystem::path& fullPath)
	{
		MeshDecodeResult decoded = DecodeModelHMESH(
			fullPath,
			ConfigManager::GetContentRootForPath(fullPath));
		if (!decoded)
			throw std::runtime_error(decoded.Message);
		FinalizeModelHMESH(mesh, std::move(decoded.Model));
	}

#if HE_RUNTIME_ASSET_IMPORT
	void MeshLoader::LoadModelASSIMP(Mesh* mesh, const std::string& path)
	{
		Assimp::Importer importer;
		std::string standardPath = std::regex_replace(path, std::regex("\\\\"), "/");
		std::string standardFullPath = std::regex_replace(
			Utils::PathToUtf8(ConfigManager::GetFullPath(path)),
			std::regex("\\\\"),
			"/");
		const aiScene* scene = importer.ReadFile(standardFullPath, aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace);

		if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
		{
			HE_CORE_ERROR("ERROR::ASSIMP::{0}", importer.GetErrorString());
			return;
		}

		mesh->mDirectory = standardPath.substr(0, standardPath.find_last_of('/'));

		// fix bug: 模型材质可能不一致，不清楚是assimp的问题还是模型本身问题
		mesh->mMaterial.resize(scene->mNumMaterials);
		for (uint32_t i = 0; i < scene->mNumMaterials; i++)
			mesh->mMaterial[i] = CreateRef<Material>();

		uint32_t subMeshIndex = 0;
		if (scene->HasAnimations())
		{
			mesh->bAnimated = true;
			ProcessNodeASSIMP(mesh, scene->mRootNode, scene, subMeshIndex);
			mesh->mAnimation = Animation(standardFullPath, mesh);
			mesh->mAnimator = Animator(&mesh->mAnimation);
		}
		else
			ProcessNodeASSIMP(mesh, scene->mRootNode, scene, subMeshIndex);

		mesh->mSkeletonPath.clear();
	}

	void MeshLoader::ProcessNodeASSIMP(Mesh* mesh, aiNode* node, const aiScene* scene, uint32_t& subMeshIndex)
	{
		for (uint32_t i = 0; i < node->mNumMeshes; ++i)
		{
			aiMesh* aiMesh = scene->mMeshes[node->mMeshes[i]];

			if (mesh->bAnimated)
				mesh->mSubMeshes.push_back(ProcessMeshASSIMP<SkinnedVertex>(mesh, aiMesh, scene, subMeshIndex));
			else
				mesh->mSubMeshes.push_back(ProcessMeshASSIMP<StaticVertex>(mesh, aiMesh, scene, subMeshIndex));

			subMeshIndex++;
		}

		for (uint32_t i = 0; i < node->mNumChildren; ++i)
		{
			ProcessNodeASSIMP(mesh, node->mChildren[i], scene, subMeshIndex);
		}
	}

	template <typename Vertex>
	Ref<SubMesh> MeshLoader::ProcessMeshASSIMP(Mesh* mesh, aiMesh* assimpMesh, const aiScene* scene, uint32_t& subMeshIndex)
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;

		for (uint32_t i = 0; i < assimpMesh->mNumVertices; ++i)
		{
			Vertex vertex;

			if (mesh->bAnimated)
			{
				Utils::SetVertexBoneDataToDefault((*reinterpret_cast<SkinnedVertex*>(&vertex)));
			}

			//pos
			glm::vec3 vector;
			vector.x = assimpMesh->mVertices[i].x;
			vector.y = assimpMesh->mVertices[i].y;
			vector.z = assimpMesh->mVertices[i].z;
			vertex.Pos = vector;

			//normal
			if (assimpMesh->HasNormals())
			{
				vector.x = assimpMesh->mNormals[i].x;
				vector.y = assimpMesh->mNormals[i].y;
				vector.z = assimpMesh->mNormals[i].z;
				vertex.Normal = vector;
			}

			//tex coord
			if (assimpMesh->mTextureCoords[0])
			{
				glm::vec2 vec;
				vec.x = assimpMesh->mTextureCoords[0][i].x;
				vec.y = assimpMesh->mTextureCoords[0][i].y;
				vertex.TexCoord = vec;
			}
			else
				vertex.TexCoord = glm::vec2(0.0f, 0.0f);

            if (assimpMesh->HasTextureCoords(1)) vertex.TexCoord1 = {assimpMesh->mTextureCoords[1][i].x, assimpMesh->mTextureCoords[1][i].y};
            if (assimpMesh->HasVertexColors(0)) { const auto& color = assimpMesh->mColors[0][i]; vertex.Color = {color.r,color.g,color.b,color.a}; }
			if (assimpMesh->HasTangentsAndBitangents())
			{
				// tangent
				vector.x = assimpMesh->mTangents[i].x;
				vector.y = assimpMesh->mTangents[i].y;
				vector.z = assimpMesh->mTangents[i].z;
				vertex.Tangent = vector;
				// bitangent
				vector.x = assimpMesh->mBitangents[i].x;
				vector.y = assimpMesh->mBitangents[i].y;
				vector.z = assimpMesh->mBitangents[i].z;
				vertex.Bitangent = vector;
			}

			vertices.push_back(vertex);
		}

		for (uint32_t i = 0; i < assimpMesh->mNumFaces; ++i)
		{
			aiFace face = assimpMesh->mFaces[i];
			for (uint32_t j = 0; j < face.mNumIndices; ++j)
			{
				indices.push_back(face.mIndices[j]);
			}
		}

		// 如果模型本身不含tangent与bitangent，则手工计算
		if (!assimpMesh->HasTangentsAndBitangents())
		{
			Utils::CalTangentBitangent(vertices, indices);
		}
		Utils::NormalizeTangentFrame(vertices);

		// Bones
		if (mesh->bAnimated)
		{
			for (size_t boneIndex = 0; boneIndex < assimpMesh->mNumBones; ++boneIndex)
			{
				int boneID = -1;
				std::string boneName = assimpMesh->mBones[boneIndex]->mName.C_Str();
				if (mesh->mBoneInfoMap.find(boneName) == mesh->mBoneInfoMap.end())
				{
					BoneInfo newBoneInfo;
					newBoneInfo.id = mesh->mBoneCounter;
					newBoneInfo.offset = Utils::ConvertMatrixToGLMFormat(assimpMesh->mBones[boneIndex]->mOffsetMatrix);
					mesh->mBoneInfoMap[boneName] = newBoneInfo;
					boneID = mesh->mBoneCounter;
					mesh->mBoneCounter++;
				}
				else
				{
					boneID = mesh->mBoneInfoMap[boneName].id;
				}
				HE_CORE_ASSERT(boneID != -1);
				auto weights = assimpMesh->mBones[boneIndex]->mWeights;
				int numWeights = assimpMesh->mBones[boneIndex]->mNumWeights;

				for (int weightIndex = 0; weightIndex < numWeights; ++weightIndex)
				{
					int vertexId = weights[weightIndex].mVertexId;
					float weight = weights[weightIndex].mWeight;
                    if (vertexId < 0 || static_cast<size_t>(vertexId) >= vertices.size() || !std::isfinite(weight) || weight < 0)
                        throw std::invalid_argument("Invalid direct-import skin influence");
                    if (weight == 0) continue;
                    auto& vertex = *reinterpret_cast<SkinnedVertex*>(&vertices[vertexId]);
                    int slot = -1;
                    for (int i = 0; i < MAX_BONE_INFLUENCE; ++i)
                    {
                        if (vertex.mBoneIDs[i] == boneID) { slot = i; break; }
                        if (vertex.mBoneIDs[i] < 0 && slot < 0) slot = i;
                    }
                    if (slot < 0) throw std::invalid_argument("Direct-import vertex " + std::to_string(vertexId) + " exceeds 16 influences; no silent truncation");
                    vertex.mBoneIDs[slot] = boneID;
                    vertex.mWeights[slot] += weight;
				}
			}
		}

        if (mesh->bAnimated)
            for (auto& source : vertices)
            {
                auto& vertex = *reinterpret_cast<SkinnedVertex*>(&source);
                double total = 0;
                for (float weight : vertex.mWeights) total += weight;
                if (!std::isfinite(total)) throw std::invalid_argument("Invalid direct-import total skin weight");
                if (total > 0) for (float& weight : vertex.mWeights) weight = static_cast<float>(weight / total);
            }

		// process materials
		aiMaterial* material = scene->mMaterials[assimpMesh->mMaterialIndex];
		// we assume a convention for sampler names in the shaders. Each diffuse texture should be named
		// as 'texture_diffuseN' where N is a sequential number ranging from 1 to MAX_SAMPLER_NUMBER. 
		// Same applies to other texture as the following list summarizes:
		// diffuse: texture_diffuseN
		// specular: texture_specularN
		// normal: texture_normalN

		const auto& loadTexture = [&](aiTextureType type) {
			loadMaterialTexturesASSIMP(mesh, material, type, assimpMesh->mMaterialIndex);
		};

		for (uint32_t type = aiTextureType_NONE; type < aiTextureType_AMBIENT_OCCLUSION; type++)
		{
			loadTexture(static_cast<aiTextureType>(type));
		}

		return CreateRef<SubMesh>(vertices, indices, assimpMesh->mMaterialIndex);
	}

	void MeshLoader::loadMaterialTexturesASSIMP(Mesh* mesh, aiMaterial* mat, unsigned int type, uint32_t& materialIndex)
	{
		aiTextureType assimpTextureType = (aiTextureType)type;

		for (unsigned int i = 0; i < mat->GetTextureCount(assimpTextureType); i++)
		{
			aiString str;
			mat->GetTexture(assimpTextureType, i, &str);

			std::string filename = std::string(str.C_Str());
			filename = mesh->mDirectory + '/' + filename;
			std::replace(filename.begin(), filename.end(), '/', '\\');
			if (!std::filesystem::exists(ConfigManager::GetFullPath(filename)))
			{
				HE_CORE_WARN("Mesh material texture does not exist: {0}", filename);
				continue;
			}

			Material& material = *mesh->mMaterial[materialIndex];
			switch (assimpTextureType)
			{
			case aiTextureType_DIFFUSE:
			case aiTextureType_BASE_COLOR:
				material.SetTexturePath(MaterialTextureSemantic::BaseColor, filename);
				break;
			case aiTextureType_HEIGHT:
				break;
			case aiTextureType_AMBIENT:
			case aiTextureType_AMBIENT_OCCLUSION:
				material.SetTexturePath(MaterialTextureSemantic::Occlusion, filename);
				break;
			case aiTextureType_NORMALS:
			case aiTextureType_NORMAL_CAMERA:
				material.SetTexturePath(MaterialTextureSemantic::Normal, filename);
				break;
			case aiTextureType_SPECULAR:
			case aiTextureType_METALNESS:
				material.SetTexturePath(MaterialTextureSemantic::MetallicRoughness, filename);
				break;
			case aiTextureType_DIFFUSE_ROUGHNESS:
				material.SetTexturePath(MaterialTextureSemantic::MetallicRoughness, filename);
				break;
			case aiTextureType_EMISSIVE:
				material.SetTexturePath(MaterialTextureSemantic::Emissive, filename);
				break;
			default:
				break;
			}
		}
	}
#endif
}
