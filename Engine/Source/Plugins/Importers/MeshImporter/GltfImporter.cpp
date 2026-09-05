#include "GltfImporter.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Resource/ResType/Mesh.h"
#include "Runtime/Resource/ResType/Material.h"
#include "Runtime/Resource/ResType/BasePbrMaterialBuilder.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Mesh.gen.h"
#include "all_serializer.h"

#include <set>

namespace HEngine
{
	static void ParseAccessor(const Json& gltfJson, int gltfAccessorIndex, int& meshResAccessorIndex, MeshRes& meshRes,
		std::unordered_map<int, int>& bufferMap, std::unordered_map<int, int>& bufferViewMap, std::unordered_map<int, int>& accessorMap,
		const std::filesystem::path& assetPath, const std::filesystem::path& outDir)
	{
		// accessor
		if (accessorMap.contains(gltfAccessorIndex))
		{
			meshResAccessorIndex = accessorMap[gltfAccessorIndex];
			return;
		}
		Accessor accessor;
		Json accessorJson = gltfJson["accessors"][gltfAccessorIndex];
		Serializer::Deserialize(accessorJson, accessor);

		auto saveAccessor = [&meshRes, &meshResAccessorIndex, &accessorMap, &gltfAccessorIndex, &accessor](int meshResBufferViewIndex) {
			accessor.bufferViewIndex = meshResBufferViewIndex;
			meshRes.accessorVec.emplace_back(accessor);
			meshResAccessorIndex = meshRes.accessorVec.size() - 1;
			accessorMap[gltfAccessorIndex] = meshResAccessorIndex;
		};

		auto saveBufferView = [&](int gltfBufferViewIndex, int meshResBufferIndex, MeshBufferView& bufferView) {
			bufferView.bufferIndex = meshResBufferIndex;
			meshRes.bufferViewVec.emplace_back(bufferView);
			saveAccessor(meshRes.bufferViewVec.size() - 1);
			bufferViewMap[gltfBufferViewIndex] = accessor.bufferViewIndex;
		};

		auto saveBuffer = [&](int gltfBufferViewIndex, int gltfBufferIndex, std::string uri, MeshBufferView& bufferView, MeshBuffer& buffer) {
			buffer.uri = uri;
			meshRes.bufferVec.emplace_back(buffer);
			saveBufferView(gltfBufferViewIndex, meshRes.bufferVec.size() - 1, bufferView);
			bufferMap[gltfBufferIndex] = bufferView.bufferIndex;
		};

		// bufferView
		if (accessorJson.contains("bufferView"))
		{
			int gltfBufferViewIndex = accessorJson["bufferView"];
			if (bufferViewMap.contains(gltfBufferViewIndex))
			{
				saveAccessor(bufferViewMap[gltfBufferViewIndex]);
				return;
			}
			Json bufferViewJson = gltfJson["bufferViews"][gltfBufferViewIndex];
			MeshBufferView bufferView;
			Serializer::Deserialize(bufferViewJson, bufferView);

			// buffer
			int gltfBufferIndex = bufferViewJson["buffer"];
			if (bufferMap.contains(gltfBufferIndex))
			{
				saveBufferView(gltfBufferViewIndex, bufferMap[gltfBufferIndex], bufferView);
				return;
			}
			Json bufferJson = gltfJson["buffers"][gltfBufferIndex];
			MeshBuffer buffer;
			Serializer::Deserialize(bufferJson, buffer);

			// buffer->uri is now required
			std::string uri = bufferJson["uri"];
			if (uri.substr(0, 5) == "data:")
			{
				// embedded
				saveBuffer(gltfBufferIndex, gltfBufferIndex, uri, bufferView, buffer);
				return;
			}

			auto uriSrcPath = assetPath.parent_path() / uri;
			if (!std::filesystem::exists(uriSrcPath))
			{
				HE_CORE_ERROR("gltf buffer uri (maybe .bin file) not exist.");
				return;
			}
			std::filesystem::path newPath = outDir / uri;
			// if two gltf file have same .bin file name, then will only get the first one
			if (!std::filesystem::exists(newPath))
			{
				std::filesystem::copy_file(uriSrcPath, newPath);
			}
			saveBuffer(gltfBufferIndex, gltfBufferIndex, ConfigManager::GetRelativePath(newPath), bufferView, buffer);
		}
	}

	void GltfImporter::Import(const std::filesystem::path& assetPath, const std::filesystem::path& outDir)
	{
		Json gltfJson = Utils::LoadJsonFile(assetPath);
		std::string fileNameWithoutExt = assetPath.stem().string();
		std::unordered_map<int, std::string> MatMap;  // index to material res path 
		std::unordered_map<int, std::string> TexSrcMap;  // index to texture source path
		std::unordered_map<int, TextureRes> TexResMap;  // index to texture res (now texture res info is in material)
		std::unordered_map<int, Sampler> SamplerMap;

		if (gltfJson.contains("images"))
		{
			int texSrcIndex = 0;
			for (auto& imageJson : gltfJson["images"])
			{
				if (imageJson.contains("uri"))
				{
					auto texSrcPath = assetPath.parent_path() / imageJson["uri"];
					if (std::filesystem::exists(texSrcPath))
					{
						auto texFileName = texSrcPath.filename();
						auto newPath = AssetManager::GetAvailableFileName(texFileName.string().c_str(), outDir);
						std::filesystem::copy_file(texSrcPath, newPath);
						TexSrcMap[texSrcIndex] = ConfigManager::GetRelativePath(newPath);
					}
				}
				texSrcIndex++;
			}
		}

		if (gltfJson.contains("samplers"))
		{
			int samplerIndex = 0;
			for (auto& samplerJson : gltfJson["samplers"])
			{
				Sampler sampler;
				Serializer::Deserialize(samplerJson, sampler);
				SamplerMap[samplerIndex++] = sampler;
			}
		}

		if (gltfJson.contains("textures"))
		{
			int texIndex = 0;
			for (auto& textureJson : gltfJson["textures"])
			{
				TextureRes textureRes;
				Serializer::Deserialize(textureJson, textureRes);
				if (textureJson.contains("source"))
				{
					textureRes.textureSourcePath = TexSrcMap[textureJson["source"]];
				}
				if (textureJson.contains("sampler"))
				{
					textureRes.sampler = SamplerMap[textureJson["sampler"]];
				}
				TexResMap[texIndex++] = textureRes;
			}
		}

		if (gltfJson.contains("materials"))
		{
			int matIndex = 0;
			for (auto& materialJson : gltfJson["materials"])
			{
				PbrMaterialRes pbrMaterialRes;
				Serializer::Deserialize(materialJson, pbrMaterialRes);
				if (materialJson.contains("pbrMetallicRoughness"))
				{
					auto pbrMetallicRoughnessJson = materialJson["pbrMetallicRoughness"];
					if (pbrMetallicRoughnessJson.contains("baseColorTexture"))
					{
						pbrMaterialRes.pbrMetallicRoughness.baseColorTexture.textureRes = TexResMap[pbrMetallicRoughnessJson["baseColorTexture"]["index"]];
						if (!pbrMaterialRes.pbrMetallicRoughness.baseColorTexture.textureRes.textureSourcePath.empty())
						{
							pbrMaterialRes.bUseAlbedoMap = true;
						}
					}
					if (pbrMetallicRoughnessJson.contains("metallicRoughnessTexture"))
					{
						pbrMaterialRes.pbrMetallicRoughness.metallicRoughnessTexture.textureRes = TexResMap[pbrMetallicRoughnessJson["metallicRoughnessTexture"]["index"]];
						if (!pbrMaterialRes.pbrMetallicRoughness.metallicRoughnessTexture.textureRes.textureSourcePath.empty())
						{
							pbrMaterialRes.bUseMetallicRoughnessMap = true;
						}
					}
				}
				if (materialJson.contains("normalTexture"))
				{
					pbrMaterialRes.normalTexture.textureRes = TexResMap[materialJson["normalTexture"]["index"]];
					if (!pbrMaterialRes.normalTexture.textureRes.textureSourcePath.empty())
					{
						pbrMaterialRes.bUseNormalMap = true;
					}
				}
				if (materialJson.contains("occlusionTexture"))
				{
					pbrMaterialRes.occlusionTexture.textureRes = TexResMap[materialJson["occlusionTexture"]["index"]];
					if (!pbrMaterialRes.occlusionTexture.textureRes.textureSourcePath.empty())
					{
						pbrMaterialRes.bUseAoMap = true;
					}
				}
				if (materialJson.contains("emissiveTexture"))
				{
					pbrMaterialRes.emissiveTexture.textureRes = TexResMap[materialJson["emissiveTexture"]["index"]];
					if (!pbrMaterialRes.emissiveTexture.textureRes.textureSourcePath.empty())
					{
						pbrMaterialRes.bUseEmissiveMap = true;
					}
				}

				const MaterialRes materialRes = BuildBasePbrMaterialResV2(pbrMaterialRes);

				std::filesystem::path matResPath = AssetManager::NewAsset(materialRes, pbrMaterialRes.name.empty() ? fileNameWithoutExt.c_str() : pbrMaterialRes.name.c_str(), outDir);
				MatMap[matIndex++] = ConfigManager::GetRelativePath(matResPath);
			}
		}

		if (gltfJson.contains("meshes"))
		{
			for (auto& meshJson : gltfJson["meshes"])
			{
				// every mesh has its own buffer, bufferview and accessor vec
				std::unordered_map<int, int> bufferMap;  // gltf buffer index to MeshRes.bufferVec index
				std::unordered_map<int, int> bufferViewMap;  // gltf buffer index to MeshRes.bufferViewVec index
				std::unordered_map<int, int> accessorMap;  // gltf buffer index to MeshRes.accessorVec index

				MeshRes meshRes;
				Serializer::Deserialize(meshJson, meshRes);
				meshRes.version = 1;
				meshRes.skeletonPath.clear();
				for (int i = 0; i < meshRes.primitives.size(); i++)
				{
					// material res path
					meshRes.primitives[i].materialResPath = meshJson["primitives"][i].contains("material") ? MatMap[meshJson["primitives"][i]["material"]] : "";

					// indices
					if (meshJson["primitives"][i].contains("indices"))
					{
						int gltfAccessorIndex = meshJson["primitives"][i]["indices"];
						ParseAccessor(gltfJson, gltfAccessorIndex, meshRes.primitives[i].indices, meshRes, bufferMap, bufferViewMap, accessorMap, assetPath, outDir);
					}

					// attribute
					for (auto& [attributeName, meshResAccessorIndex] : meshRes.primitives[i].attributes)
					{
						// accessor
						int gltfAccessorIndex = meshJson["primitives"][i]["attributes"][attributeName];
						ParseAccessor(gltfJson, gltfAccessorIndex, meshResAccessorIndex, meshRes, bufferMap, bufferViewMap, accessorMap, assetPath, outDir);
					}
				}
				auto meshOutPath = AssetManager::NewAsset(meshRes, meshRes.name.empty() ? fileNameWithoutExt.c_str() : meshRes.name.c_str(), outDir);
				Json savedMeshJson = Utils::LoadJsonFile(meshOutPath);
				savedMeshJson["version"] = meshRes.version;
				savedMeshJson["skeletonPath"] = "";
				Utils::SaveJsonToFile(savedMeshJson, meshOutPath);
			}
		}
	}
}
