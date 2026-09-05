#include "Hepch.h"

#include "Runtime/UnitTest/MaterialThumbnailCacheKeyTests.h"

#include "Runtime/Graphics/Material/MaterialThumbnailCacheKey.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
	namespace
	{
		void Require(bool condition, std::string_view message)
		{
			if (!condition)
				throw std::runtime_error(std::string(message));
		}

		[[nodiscard]] MaterialThumbnailCacheKeyDesc MakeDesc()
		{
			return {
				.MaterialDocumentContent = R"({"SchemaVersion":2,"Parameters":{"roughness":0.5}})",
				.ShaderIdentity = "recursive-shader-hash:a1b2c3",
				.ShaderArtifactIdentity =
					"Forward|Static|Baseline|Pixel|artifact-content-hash:0102",
				.TextureDependencies = {
					{ "Assets/Textures/Base.png", "base-content-hash" },
					{ "Assets/Textures/Normal.png", "normal-content-hash" },
				},
				.PreviewModelIdentity = "MaterialBall.hmesh:6819:300|MaterialBall.bin:3599776:301",
				.EnvironmentIdentity = "Default.hdr:4194304:400",
				.RigVersion = "material-thumbnail-rig-v1",
				.MaterialAbiVersion = 1,
				.RendererVersion = 1,
			};
		}
	}

	void MaterialThumbnailCacheKeyContract()
	{
		const MaterialThumbnailCacheKeyDesc base = MakeDesc();
		const std::string baseKey = BuildMaterialThumbnailCacheKey(base);
		Require(baseKey.size() == 64 && std::all_of(baseKey.begin(), baseKey.end(), [](unsigned char value)
		{
			return std::isdigit(value) != 0 || (value >= 'a' && value <= 'f');
		}), "Material thumbnail cache key is not a complete lowercase SHA-256 digest.");
		Require(BuildMaterialThumbnailCacheKey(base) == baseKey,
			"Identical material thumbnail inputs produced unstable keys.");

		auto requireChanged = [&base, &baseKey](MaterialThumbnailCacheKeyDesc changed, std::string_view field)
		{
			Require(BuildMaterialThumbnailCacheKey(changed) != baseKey,
				std::string("Material thumbnail cache key omitted field: ") + std::string(field));
		};

		MaterialThumbnailCacheKeyDesc changed = base;
		changed.MaterialDocumentContent += " ";
		requireChanged(std::move(changed), "MaterialDocumentContent");
		changed = base;
		changed.ShaderIdentity = "recursive-shader-hash:changed";
		requireChanged(std::move(changed), "ShaderIdentity");
		changed = base;
		changed.ShaderArtifactIdentity += ":changed";
		requireChanged(std::move(changed), "ShaderArtifactIdentity");
		changed = base;
		changed.TextureDependencies[0].ContentHash = "changed-content-with-same-metadata";
		requireChanged(std::move(changed), "Texture.ContentHash");
		changed = base;
		changed.PreviewModelIdentity += ":changed";
		requireChanged(std::move(changed), "PreviewModelIdentity");
		changed = base;
		changed.EnvironmentIdentity += ":changed";
		requireChanged(std::move(changed), "EnvironmentIdentity");
		changed = base;
		changed.RigVersion = "material-thumbnail-rig-v2";
		requireChanged(std::move(changed), "RigVersion");
		changed = base;
		changed.MaterialAbiVersion++;
		requireChanged(std::move(changed), "MaterialAbiVersion");
		changed = base;
		changed.RendererVersion++;
		requireChanged(std::move(changed), "RendererVersion");

		changed = base;
		std::reverse(changed.TextureDependencies.begin(), changed.TextureDependencies.end());
		Require(BuildMaterialThumbnailCacheKey(changed) == baseKey,
			"Texture dependency enumeration order changed the material thumbnail key.");

		const std::vector<std::string> evictions = SelectMaterialThumbnailCacheEvictions({
			{ "active", 70, 1, true },
			{ "oldest", 40, 2, false },
			{ "older", 30, 3, false },
			{ "recent", 20, 4, false },
		}, 2, 100);
		Require(evictions == std::vector<std::string>({ "oldest", "older" }),
			"Thumbnail cache eviction did not enforce count/bytes in LRU order.");

		const std::vector<std::string> protectedOnly =
			SelectMaterialThumbnailCacheEvictions({
				{ "active", 80, 1, true },
				{ "queued", 80, 2, true },
			}, 0, 0);
		Require(protectedOnly.empty(),
			"Thumbnail cache eviction selected an active or queued entry.");
	}
}
