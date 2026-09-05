#include "Hepch.h"

#include "Runtime/UnitTest/MaterialSerializationTests.h"

#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/Graphics/Material/Material.h"
#include "Runtime/Graphics/Material/MaterialShaderManager.h"
#include "Runtime/Graphics/Material/MaterialShaderManifest.h"
#include "Runtime/Graphics/Material/MaterialShaderPath.h"
#include "Runtime/Graphics/Material/ShaderPropertyParser.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/ResType/BasePbrMaterialBuilder.h"
#include "Runtime/Utils/Misc/ShaderUtils.h"
#include "_generated/serializer/Material.gen.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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

        class ScopedMaterialFixture final
        {
        public:
            ScopedMaterialFixture()
            {
                static std::atomic_uint64_t sSuffix = 0;
                const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                mRoot = std::filesystem::temp_directory_path() /
                    ("HEngine_Material_" + std::to_string(timestamp) + "_" +
                        std::to_string(sSuffix.fetch_add(1, std::memory_order_relaxed)));
                mProject = mRoot / "Project";
                mEngineContent = mRoot / "EngineContent";
                mGeneratedContent = mRoot / "GeneratedContent";
                mSaved = mRoot / "Saved";

                Write("Project/Project.heproject", "{}\n");
                Write("Project/Config/BaseEngine.json", "{\"Renderer\":{\"API\":\"DX12\"}}\n");
                Write("Project/Config/Game.json", "{\"Game\":{}}\n");
                Write("Project/Config/BaseEditor.json", "{}\n");
                Write("EngineContent/Config/BaseEditor.json", "{}\n");
                std::filesystem::create_directories(mProject / "Assets/Material");
                std::filesystem::create_directories(mProject / "Assets/Textures");
                std::filesystem::create_directories(mProject / "Shaders");
                std::filesystem::create_directories(mEngineContent / "Shaders");
                std::filesystem::create_directories(mGeneratedContent / "Shaders/CACHES");

                ConfigManager::GetInstance().Clear();
                ConfigManager::GetInstance().Init(
                    mProject / "Project.heproject",
                    mEngineContent,
                    mSaved,
                    mGeneratedContent);
            }

            ~ScopedMaterialFixture()
            {
                ConfigManager::GetInstance().Clear();
                std::error_code error;
                std::filesystem::remove_all(mRoot, error);
            }

            void Write(std::string_view relativePath, std::string_view contents)
            {
                const std::filesystem::path path = mRoot / std::filesystem::path(relativePath);
                std::filesystem::create_directories(path.parent_path());
                std::ofstream stream(path, std::ios::out | std::ios::binary | std::ios::trunc);
                Require(stream.is_open(), "Could not create material test fixture.");
                stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
                stream.close();
                Require(!stream.fail(), "Could not write material test fixture.");
            }

            [[nodiscard]] std::filesystem::path GetPath(
                std::string_view relativePath) const
            {
                return mRoot / std::filesystem::path(relativePath);
            }

        private:
            std::filesystem::path mRoot;
            std::filesystem::path mProject;
            std::filesystem::path mEngineContent;
            std::filesystem::path mGeneratedContent;
            std::filesystem::path mSaved;
        };

        const MaterialRuntimeProperty& FindProperty(const Material& material, std::string_view name)
        {
            const auto iterator = std::find_if(
                material.GetExposedProperties().begin(),
                material.GetExposedProperties().end(),
                [name](const MaterialRuntimeProperty& property)
                {
                    return property.Decl.Name == name;
                });
            Require(iterator != material.GetExposedProperties().end(), "Expected material property was not found.");
            return *iterator;
        }

        MaterialShaderProperty MakeLayoutProperty(
            std::string name,
            std::string stableId,
            MaterialShaderPropertyType type,
            std::vector<double> numericDefault,
            uint32_t row)
        {
            MaterialShaderProperty property;
            property.Name = std::move(name);
            property.StableId = std::move(stableId);
            property.DisplayName = property.Name;
            property.Group = "Hot Reload";
            property.Source = "TestMaterial.hlsl";
            property.Line = row + 1;
            property.Column = 1;
            property.Type = type;
            property.RowOffset = row;
            property.ByteOffset = row * MaterialShaderParameterAlignment;
            property.ByteSize = MaterialShaderPropertyByteSize(type);
            property.Default.Numeric = std::move(numericDefault);
            return property;
        }

        MaterialShaderManifest MakeLayoutManifest(bool bSecondLayout)
        {
            MaterialShaderManifest manifest;
            manifest.SourcePath = "TestMaterial.hlsl";
            manifest.RecursiveSourceHash = std::string(64, bSecondLayout ? 'b' : 'a');
            manifest.ShadingModel = bSecondLayout
                ? MaterialShaderShadingModel::Unlit
                : MaterialShaderShadingModel::Lit;
            manifest.BlendMode = bSecondLayout
                ? MaterialShaderBlendMode::Transparent
                : MaterialShaderBlendMode::Opaque;
            manifest.CullMode = bSecondLayout
                ? MaterialShaderCullMode::None
                : MaterialShaderCullMode::Back;
            manifest.bDepthWrite = !bSecondLayout;
            manifest.bCastsShadow = !bSecondLayout;

            if (bSecondLayout)
            {
                manifest.Properties.push_back(MakeLayoutProperty(
                    "renamedTint", "surface.tint", MaterialShaderPropertyType::Color,
                    { 0.9, 0.9, 0.9, 1.0 }, 0));
                manifest.Properties.push_back(MakeLayoutProperty(
                    "newValue", "surface.new", MaterialShaderPropertyType::Float,
                    { 0.75 }, 1));
                manifest.Properties.push_back(MakeLayoutProperty(
                    "changedValue", "surface.changed", MaterialShaderPropertyType::Float3,
                    { 0.1, 0.2, 0.3 }, 2));
            }
            else
            {
                manifest.Properties.push_back(MakeLayoutProperty(
                    "oldTint", "surface.tint", MaterialShaderPropertyType::Color,
                    { 0.1, 0.1, 0.1, 1.0 }, 0));
                manifest.Properties.push_back(MakeLayoutProperty(
                    "removedValue", "surface.removed", MaterialShaderPropertyType::Float,
                    { 0.25 }, 1));
                manifest.Properties.push_back(MakeLayoutProperty(
                    "changedValue", "surface.changed", MaterialShaderPropertyType::Float,
                    { 0.5 }, 2));
            }
            manifest.ParameterBlock.ByteSize = static_cast<uint32_t>(
                manifest.Properties.size() * MaterialShaderParameterAlignment);
            manifest.BindingSignatureHash =
                BuildMaterialShaderBindingSignatureHash(manifest);

            for (const MaterialShaderVertexPath vertexPath : {
                    MaterialShaderVertexPath::Static,
                    MaterialShaderVertexPath::Skinned })
            {
                for (const MaterialShaderDescriptorMode descriptorMode : {
                        MaterialShaderDescriptorMode::Baseline,
                        MaterialShaderDescriptorMode::HeapDirect })
                {
                    for (const std::string_view stage : {
                            std::string_view("vs"), std::string_view("ps") })
                    {
                        const bool bPixel = stage == "ps";
                        const std::string permutation = std::string("FORWARD_") +
                            (vertexPath == MaterialShaderVertexPath::Static
                                ? "CPU_STATIC_"
                                : "SKINNED_") +
                            (descriptorMode == MaterialShaderDescriptorMode::Baseline
                                ? "BASELINE"
                                : "HEAP_DIRECT");
                        manifest.Artifacts.push_back({
                            .Pass = MaterialShaderPass::Forward,
                            .Stage = std::string(stage),
                            .EntryPoint = bPixel ? "HEMaterialPS" : "HEMaterialVS",
                            .TargetProfile = bPixel
                                ? (descriptorMode == MaterialShaderDescriptorMode::HeapDirect
                                    ? "ps_6_6"
                                    : "ps_6_0")
                                : "vs_6_0",
                            .Permutation = permutation,
                            .DescriptorMode = descriptorMode,
                            .VertexPath = vertexPath,
                            .RelativePath = "DXIL/Materials/TestMaterial/" +
                                permutation + "." + std::string(stage),
                            .ContentHash = std::string(64, 'c'),
                        });
                    }
                }
            }
            return manifest;
        }
    }

    void ShaderPropertyParserContract()
    {
        ScopedMaterialFixture fixture;
        fixture.Write(
            "Project/Shaders/Shared.hlsli",
            "#pragma property g_Included \"Included\" float2 default(0.25,0.75) group(\"Shared\")\n");
        fixture.Write(
            "Project/Shaders/Valid.hlsl",
            "#include \"Shared.hlsli\"\n"
            "#pragma htype vs VS\n"
            "#pragma htype ps PS\n"
            "#pragma property g_Map \"Map\" texture2d default(white) semantic(BaseColor) colorspace(SRGB)\n"
            "#pragma property g_Value \"Value\" float default(0.5) range(0.0,1.0) group(\"Surface\")\n"
            "#pragma property g_Tint \"Tint\" color default(1,0.5,0.25,1)\n"
            "#pragma property g_Enabled \"Enabled\" bool default(true)\n"
            "#pragma property g_Count \"Count\" int default(3) range(0,8)\n"
            "Texture2D<float4> g_Map : register(t7, space1);\n");
        fixture.Write(
            "Project/Shaders/Nested.hlsli",
            "#include \"Shared.hlsli\"\n");
        fixture.Write(
            "Project/Shaders/Nested.hlsl",
            "#include \"Nested.hlsli\"\n"
            "#pragma htype vs VS\n"
            "#pragma htype ps PS\n");

        const std::vector<std::filesystem::path> dependentShaders =
            Utils::CollectShadersDependentOnInclude(
                fixture.GetPath("Project/Shaders/Shared.hlsli"));
        Require(dependentShaders.size() == 2,
            "The include dependency graph did not find direct and transitive shaders.");
        Require(dependentShaders[0].filename() == "Nested.hlsl" &&
            dependentShaders[1].filename() == "Valid.hlsl",
            "The include dependency graph returned unexpected or unstable shader paths.");

        const std::vector<Utils::ShaderCompilationResult> compilationResults =
            Utils::CompileDependentShadersForInclude(
                fixture.GetPath("Project/Shaders/Shared.hlsli"),
                [](const std::filesystem::path& shaderPath, std::string& diagnostics)
                {
                    if (shaderPath.filename() != "Nested.hlsl")
                        return true;
                    diagnostics = "Synthetic compiler failure.";
                    return false;
                });
        Require(compilationResults.size() == dependentShaders.size(),
            "A failed dependent shader compilation was omitted from the outcomes.");
        Require(!compilationResults[0].bSucceeded &&
            compilationResults[0].ShaderPath == dependentShaders[0] &&
            compilationResults[0].Diagnostics == "Synthetic compiler failure.",
            "The failed dependent shader outcome lost its path or diagnostics.");
        Require(compilationResults[1].bSucceeded &&
            compilationResults[1].ShaderPath == dependentShaders[1],
            "The successful dependent shader outcome was not preserved.");

        const ShaderPropertyParseResult parsed =
            ShaderPropertyParser::ParseWithDiagnostics("Shaders/Valid.hlsl");
        Require(parsed.Succeeded(), "Valid material property declarations produced diagnostics.");
        Require(parsed.Declarations.size() == 6, "Included or local properties were not all parsed.");
        const auto texture = std::find_if(parsed.Declarations.begin(), parsed.Declarations.end(), [](const auto& value)
        {
            return value.Name == "g_Map";
        });
        Require(texture != parsed.Declarations.end(), "Texture property was not parsed.");
        Require(texture->TextureSlot == 7 && texture->RegisterSpace == 1,
            "register(tN, space1) was not parsed.");
        Require(texture->ByteOffset % 16 == 0 && texture->ByteSize == 4,
            "Texture property layout is not a deterministic float4 row.");
        Require(parsed.Declarations.back().SourceLine > 0,
            "Property source location was not recorded.");

        fixture.Write(
            "Project/Shaders/Malformed.hlsl",
            "#pragma property duplicate \"One\" float default(not-a-number)\n"
            "#pragma property duplicate \"Two\" float default(2) range(4,1)\n"
            "#pragma property broken\n");
        const ShaderPropertyParseResult malformed =
            ShaderPropertyParser::ParseWithDiagnostics("Shaders/Malformed.hlsl");
        Require(!malformed.Succeeded() && malformed.Diagnostics.size() >= 3,
            "Malformed/duplicate/range property input was not diagnosed.");

        fixture.Write(
            "Project/Shaders/Unsafe.hlsl",
            "#include \"../outside.hlsli\"\n"
            "#pragma property value \"Value\" float default(1)\n");
        const ShaderPropertyParseResult unsafe =
            ShaderPropertyParser::ParseWithDiagnostics("Shaders/Unsafe.hlsl");
        Require(!unsafe.Succeeded(), "Parent-traversal include was accepted by the compatibility parser.");
    }

    void MaterialSerializationContract()
    {
        ScopedMaterialFixture fixture;

        const std::filesystem::path generatedRoot =
            fixture.GetPath("GeneratedContent");
        const std::filesystem::path materialsRoot =
            generatedRoot / "Shaders/CACHES/Materials";
        std::filesystem::create_directories(materialsRoot);

        std::filesystem::path resolvedManifest;
        std::string normalizedShaderPath;
        std::string shaderPathError;
        Require(TryResolvePublishedMaterialShaderManifestPath(
                generatedRoot,
                "Shaders/Materials/Test.hlsl",
                resolvedManifest,
                normalizedShaderPath,
                shaderPathError) &&
            normalizedShaderPath == "Shaders/Materials/Test.hlsl" &&
            resolvedManifest == std::filesystem::weakly_canonical(
                materialsRoot / "Materials/Test.hlsl.material.json"),
            "A canonical material shader path did not resolve to its generated manifest.");

        const std::array<std::string, 5> invalidShaderPaths = {{
            "shaders/Materials/Test.hlsl",
            "Shaders/../Outside.hlsl",
            "C:/Outside.hlsl",
            "Shaders/Materials/Test.hlsli",
            std::string("Shaders/Materials/Test.hlsl") + '\0' + "Outside",
        }};
        for (const std::string& invalidShaderPath : invalidShaderPaths)
        {
            Require(!TryResolvePublishedMaterialShaderManifestPath(
                    generatedRoot,
                    invalidShaderPath,
                    resolvedManifest,
                    normalizedShaderPath,
                    shaderPathError),
                "An unsafe material shader identity resolved to a generated manifest.");
        }

        const std::filesystem::path outsideManifests =
            fixture.GetPath("OutsideGeneratedMaterials");
        std::filesystem::create_directories(outsideManifests);
        fixture.Write(
            "OutsideGeneratedMaterials/Unsafe.hlsl.material.json", "{}\n");
        const std::filesystem::path escapeLink = materialsRoot / "Escape";
        std::error_code linkError;
        std::filesystem::create_directory_symlink(
            outsideManifests, escapeLink, linkError);
        if (!linkError)
        {
            Require(!TryResolvePublishedMaterialShaderManifestPath(
                    generatedRoot,
                    "Shaders/Escape/Unsafe.hlsl",
                    resolvedManifest,
                    normalizedShaderPath,
                    shaderPathError),
                "A generated material manifest escaped through a symbolic link or junction.");
        }
        else
        {
            HE_CORE_INFO(
                "[Test] SKIP material manifest physical symlink: platform permission unavailable.");
        }

        PbrMaterialRes importedPbr;
        importedPbr.name = "ImportedMaterial";
        importedPbr.pbrMetallicRoughness.baseColorFactor = { 0.2f, 0.3f, 0.4f, 0.75f };
        importedPbr.pbrMetallicRoughness.metallicFactor = 0.65f;
        importedPbr.pbrMetallicRoughness.roughnessFactor = 0.35f;
        importedPbr.pbrMetallicRoughness.baseColorTexture.texCoord = 1;
        importedPbr.pbrMetallicRoughness.baseColorTexture.textureRes.textureSourcePath =
            "Assets/Imported/BaseColor.png";
        importedPbr.pbrMetallicRoughness.baseColorTexture.textureRes.sampler.wrapS = 10497;
        importedPbr.pbrMetallicRoughness.metallicRoughnessTexture.texCoord = 2;
        importedPbr.pbrMetallicRoughness.metallicRoughnessTexture.textureRes.textureSourcePath =
            "Assets/Imported/MetallicRoughness.png";
        importedPbr.normalTexture.texCoord = 3;
        importedPbr.normalTexture.scale = 1.4f;
        importedPbr.normalTexture.textureRes.textureSourcePath = "Assets/Imported/Normal.png";
        importedPbr.normalTexture.textureRes.sampler.minFilter = 9987;
        importedPbr.occlusionTexture.texCoord = 4;
        importedPbr.occlusionTexture.strength = 0.8f;
        importedPbr.occlusionTexture.textureRes.textureSourcePath = "Assets/Imported/AO.png";
        importedPbr.emissiveTexture.texCoord = 5;
        importedPbr.emissiveTexture.textureRes.textureSourcePath = "Assets/Imported/Emissive.png";
        importedPbr.emissiveFactor = { 0.1f, 0.2f, 0.3f };
        importedPbr.alphaMode = "MASK";
        importedPbr.alphaCutoff = 0.42f;
        importedPbr.doubleSided = true;
        importedPbr.bUseAlbedoMap = true;
        importedPbr.bUseNormalMap = true;
        importedPbr.bUseMetallicRoughnessMap = true;
        importedPbr.bUseAoMap = true;
        importedPbr.bUseEmissiveMap = true;

        const MaterialRes importedMaterial = BuildBasePbrMaterialResV2(importedPbr);
        Require(importedMaterial.SchemaVersion == 2 &&
            importedMaterial.Type == "Default" &&
            importedMaterial.PassName == "BasePBR" &&
            importedMaterial.ShaderPath == "Shaders/BasePBR.hlsl" &&
            importedMaterial.Domain == "Surface" &&
            importedMaterial.ShadingModel == "Lit" &&
            importedMaterial.BlendMode == "Mask" &&
            importedMaterial.CullMode == "None" &&
            importedMaterial.DepthWrite &&
            std::abs(importedMaterial.AlphaCutoff - 0.42f) < 0.0001f,
            "BasePBR conversion did not emit the authoritative V2 render state.");
        constexpr std::array<std::pair<std::string_view, std::string_view>, 8> expectedParameters = {{
            { "g_BaseColorFactor", "color" },
            { "g_MetallicFactor", "float" },
            { "g_RoughnessFactor", "float" },
            { "g_AoStrength", "float" },
            { "g_NormalStrength", "float" },
            { "g_EmissiveFactor", "color" },
            { "g_AlphaClip", "float" },
            { "g_AlphaCutoff", "float" },
        }};
        Require(importedMaterial.Parameters.size() == expectedParameters.size(),
            "BasePBR conversion emitted the wrong typed parameter count.");
        for (const auto& [name, type] : expectedParameters)
        {
            Require(importedMaterial.Parameters.contains(name) &&
                importedMaterial.Parameters[name].value("StableId", "") == name &&
                importedMaterial.Parameters[name].value("Type", "") == type,
                "BasePBR conversion did not match a manifest parameter name/type.");
        }
        Require(importedMaterial.Parameters["g_AlphaClip"].value("Value", 0.0f) == 1.0f,
            "BasePBR conversion did not map MASK to the typed alpha-clip value.");

        constexpr std::array<std::string_view, 7> expectedTextures = {{
            "g_AlbedoMap",
            "g_MetallicRoughnessMap",
            "g_MetallicMap",
            "g_RoughnessMap",
            "g_AoMap",
            "g_NormalMap",
            "g_EmissiveMap",
        }};
        Require(importedMaterial.Textures.size() == expectedTextures.size(),
            "BasePBR conversion emitted the wrong typed texture count.");
        for (const std::string_view name : expectedTextures)
        {
            Require(importedMaterial.Textures.contains(name) &&
                importedMaterial.Textures[name].value("StableId", "") == name &&
                importedMaterial.Textures[name].value("Type", "") == "texture2d",
                "BasePBR conversion did not match a manifest texture name/type.");
        }
        Require(
            importedMaterial.Textures["g_AlbedoMap"].value("Path", "") ==
                "Assets/Imported/BaseColor.png" &&
            importedMaterial.Textures["g_AlbedoMap"].value("ColorSpace", "") == "SRGB" &&
            importedMaterial.Textures["g_NormalMap"].value("Semantic", "") == "Normal" &&
            importedMaterial.Textures["g_EmissiveMap"].value("Fallback", "") == "black",
            "BasePBR conversion did not emit the typed texture contract.");

        Json expectedLegacyProperties;
        Serializer::Serialize(expectedLegacyProperties, importedPbr);
        Json actualLegacyProperties = importedMaterial.Properties;
        actualLegacyProperties.erase("ExposedProperties");
        Require(actualLegacyProperties == expectedLegacyProperties,
            "BasePBR dual-write changed legacy PBR fields or texture metadata.");

        PbrMaterialRes legacyRoundTrip;
        Serializer::Deserialize(importedMaterial.Properties, legacyRoundTrip);
        Require(legacyRoundTrip.name == importedPbr.name &&
            legacyRoundTrip.pbrMetallicRoughness.baseColorTexture.texCoord == 1 &&
            legacyRoundTrip.pbrMetallicRoughness.baseColorTexture.textureRes.sampler.wrapS == 10497 &&
            legacyRoundTrip.normalTexture.texCoord == 3 &&
            legacyRoundTrip.normalTexture.textureRes.sampler.minFilter == 9987 &&
            legacyRoundTrip.occlusionTexture.texCoord == 4 &&
            legacyRoundTrip.emissiveTexture.texCoord == 5 &&
            legacyRoundTrip.bUseAlbedoMap && legacyRoundTrip.bUseNormalMap &&
            legacyRoundTrip.bUseMetallicRoughnessMap && legacyRoundTrip.bUseAoMap &&
            legacyRoundTrip.bUseEmissiveMap,
            "BasePBR dual-write lost legacy texture metadata or enable flags.");
        const Json& legacyExposed = importedMaterial.Properties["ExposedProperties"];
        Require(legacyExposed["g_AlbedoMap"] ==
                importedMaterial.Textures["g_AlbedoMap"]["Path"] &&
            legacyExposed["g_NormalStrength"] ==
                importedMaterial.Parameters["g_NormalStrength"]["Value"] &&
            legacyExposed["g_AlphaCutoff"] ==
                importedMaterial.Parameters["g_AlphaCutoff"]["Value"],
            "BasePBR typed and legacy dual-write values diverged.");

        MaterialRes newResource;
        Json newResourceJson;
        Serializer::Serialize(newResourceJson, newResource);
        Require(newResourceJson.value("SchemaVersion", 0) == 2 &&
            newResourceJson["Parameters"].is_object() &&
            newResourceJson["Textures"].is_object() &&
            newResourceJson["OrphanedParameters"].is_object() &&
            newResourceJson["Properties"].is_object(),
            "A newly constructed reflected MaterialRes did not serialize valid V2 objects.");
        fixture.Write(
            "Project/Shaders/TestMaterial.hlsl",
            "#pragma htype vs VS\n"
            "#pragma htype ps PS\n"
            "#pragma property baseColorTint \"Base Color\" color default(1,1,1,1)\n"
            "#pragma property metallic \"Metallic\" float default(1) range(0,1) id(\"surface.metallic\")\n"
            "#pragma property g_Map \"Map\" texture2d default(white) semantic(BaseColor) colorspace(SRGB)\n"
            "#pragma property enabled \"Enabled\" bool default(false)\n"
            "#pragma property g_AlphaClip \"Alpha Clip\" float default(0) range(0,1)\n"
            "Texture2D<float4> g_Map : register(t0, space1);\n");

        fixture.Write(
            "Project/Assets/Material/Legacy.hmat",
            R"json({
  "Type":"Custom",
  "PassName":"TestMaterial",
  "ShaderPath":"Shaders/TestMaterial.hlsl",
  "FutureTop":{"Keep":true},
  "Properties":{
    "bUseMetallicRoughnessMap":true,
    "pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],"metallicFactor":0.1,"roughnessFactor":0.9,"baseColorTexture":{"textureRes":{"textureSourcePath":""}} ,"metallicRoughnessTexture":{"textureRes":{"textureSourcePath":""}}},
    "ExposedProperties":{"g_BaseColorFactor":[0.2,0.3,0.4,1],"g_MetallicFactor":0.65,"removed":42}
  }
})json");

        Material legacy("Assets/Material/Legacy.hmat");
        Require(legacy.GetSchemaVersion() == 1, "A material without SchemaVersion was not treated as V1.");
        Require(std::abs(legacy.metallicFactor - 0.65f) < 0.0001f,
            "Legacy alias did not update the immediate runtime PBR view.");
        Require(std::abs(legacy.roughnessFactor - 1.0f) < 0.0001f,
            "The V1-only 0.1/0.9 metallic-roughness compatibility rule was not applied.");
        Require(legacy.GetOrphanedParameters().contains("removed"),
            "Unknown V1 exposed property was not preserved as an orphan.");

        Json upgraded = legacy.BuildSerializedDocument();
        Require(upgraded.value("SchemaVersion", 0) == 2,
            "Explicit serialization did not upgrade V1 to V2.");
        Require(upgraded.contains("FutureTop") && upgraded["FutureTop"].value("Keep", false),
            "Unknown top-level fields were lost during V2 serialization.");
        Require(upgraded["Parameters"]["metallic"].value("Type", "") == "float",
            "V2 typed parameter schema was not emitted.");
        Require(upgraded["OrphanedParameters"].contains("removed"),
            "Orphaned V1 property was lost during V2 serialization.");

        upgraded["Parameters"]["legacyMetallicName"] =
            upgraded["Parameters"]["metallic"];
        upgraded["Parameters"].erase("metallic");
        // This fixture intentionally exercises V2 serialization without a
        // compiled material manifest; keep it on the legacy-compatible type.
        upgraded["Type"] = "Default";
        upgraded["BlendMode"] = "Transparent";
        upgraded["DepthWrite"] = false;
        upgraded["Properties"]["alphaMode"] = "BLEND";
        upgraded["Properties"]["ExposedProperties"]["g_AlphaClip"] = 0.0;

        fixture.Write(
            "Project/Assets/Material/V2.hmat",
            upgraded.dump(2));
        Material roundTrip("Assets/Material/V2.hmat");
        Require(roundTrip.GetSchemaVersion() == 2,
            "V2 material did not round-trip its schema version.");
        Require(std::abs(FindProperty(roundTrip, "metallic").FloatValue - 0.65f) < 0.0001f,
            "V2 typed float did not migrate by stable id after a property rename.");
        Require(roundTrip.GetOrphanedParameters().contains("removed"),
            "V2 orphan did not round-trip.");
        Require(roundTrip.mPbrMatRes.alphaMode == "BLEND",
            "V2 top-level transparent render state was overwritten by legacy g_AlphaClip.");
        roundTrip.ApplyRuntimePropertyChanges();
        Require(roundTrip.mPbrMatRes.alphaMode == "BLEND" &&
            roundTrip.BuildSerializedDocument().value("BlendMode", "") == "Transparent",
            "Applying V2 material parameters overwrote the authoritative render state.");

        const MaterialShaderManifest firstLayout = MakeLayoutManifest(false);
        fixture.Write(
            "GeneratedContent/Shaders/CACHES/Materials/TestMaterial.hlsl.material.json",
            MaterialShaderManifestToJson(firstLayout).dump(2));
        fixture.Write(
            "Project/Assets/Material/HotReload.hmat",
            R"json({
  "SchemaVersion":2,
  "Type":"Custom",
  "PassName":"TestMaterial",
  "ShaderPath":"Shaders/TestMaterial.hlsl",
  "VariantKeywords":[],
  "Domain":"Surface",
  "ShadingModel":"Lit",
  "BlendMode":"Opaque",
  "CullMode":"Back",
  "DepthWrite":true,
  "AlphaCutoff":0.5,
  "Parameters":{
    "oldTint":{"StableId":"surface.tint","Type":"color","Value":[0.2,0.3,0.4,1.0]},
    "removedValue":{"StableId":"surface.removed","Type":"float","Value":0.6},
    "changedValue":{"StableId":"surface.changed","Type":"float","Value":0.8}
  },
  "Textures":{},
  "OrphanedParameters":{},
  "Properties":{}
})json");

        Material hotReload("Assets/Material/HotReload.hmat");
        Require(hotReload.GetExpectedRecursiveSourceHash() ==
                firstLayout.RecursiveSourceHash &&
            hotReload.GetExpectedBindingSignature() ==
                firstLayout.BindingSignatureHash &&
            hotReload.GetExpectedMaterialAbiVersion() == MaterialShaderAbiVersion &&
            hotReload.GetExpectedArtifactIdentityHash() ==
                BuildMaterialShaderArtifactIdentityHash(firstLayout) &&
            hotReload.GetExpectedProgramGeneration() == 0,
            "Material did not retain the manifest identity used to build its initial layout.");
        const uint64_t initialRevision = hotReload.GetRuntimeRevision();

        const MaterialShaderManifest secondLayout = MakeLayoutManifest(true);
        fixture.Write(
            "GeneratedContent/Shaders/CACHES/Materials/TestMaterial.hlsl.material.json",
            MaterialShaderManifestToJson(secondLayout).dump(2));
        std::string refreshError;
        Require(hotReload.RefreshShaderLayoutFromManifest(&refreshError),
            "A valid shader layout refresh failed: " + refreshError);
        const MaterialRuntimeProperty& renamed = FindProperty(hotReload, "renamedTint");
        const MaterialRuntimeProperty& added = FindProperty(hotReload, "newValue");
        const MaterialRuntimeProperty& changed = FindProperty(hotReload, "changedValue");
        Require(renamed.Decl.Type == MaterialPropertyType::Color &&
                glm::all(glm::epsilonEqual(
                    renamed.ColorValue,
                    glm::vec4(0.2f, 0.3f, 0.4f, 1.0f),
                    0.0001f)),
            "Stable-id property rename did not preserve its compatible value.");
        Require(added.Decl.Type == MaterialPropertyType::Float &&
                std::abs(added.FloatValue - 0.75f) < 0.0001f,
            "A newly added property did not use its manifest default.");
        Require(changed.Decl.Type == MaterialPropertyType::Float3 &&
                glm::all(glm::epsilonEqual(
                    glm::vec3(changed.ColorValue),
                    glm::vec3(0.1f, 0.2f, 0.3f),
                    0.0001f)),
            "A type-changed property did not use the new manifest default.");
        Require(hotReload.GetOrphanedParameters().contains("removedValue") &&
                hotReload.GetOrphanedParameters()["removedValue"].value("Reason", "") ==
                    "shader-layout-changed" &&
                hotReload.GetOrphanedParameters().contains("changedValue") &&
                hotReload.GetOrphanedParameters()["changedValue"].value("Reason", "") ==
                    "incompatible-type",
            "Removed or type-changed values were not preserved in OrphanedParameters.");
        Require(hotReload.mMatRes.ShadingModel == "Unlit" &&
                hotReload.mPbrMatRes.alphaMode == "BLEND" &&
                hotReload.mPbrMatRes.doubleSided &&
                !hotReload.mMatRes.DepthWrite &&
                hotReload.GetRuntimeRevision() == initialRevision + 1 &&
                hotReload.GetExpectedRecursiveSourceHash() ==
                    secondLayout.RecursiveSourceHash &&
                hotReload.GetExpectedBindingSignature() ==
                    secondLayout.BindingSignatureHash,
            "Successful refresh did not atomically adopt render state, identity, and revision.");
        const RenderMeshSection::MaterialInstanceSnapshot refreshedSnapshot =
            hotReload.BuildRenderSnapshot();
        Require(refreshedSnapshot.ExpectedRecursiveSourceHash ==
                secondLayout.RecursiveSourceHash &&
            refreshedSnapshot.ExpectedBindingSignature ==
                secondLayout.BindingSignatureHash &&
            refreshedSnapshot.ExpectedMaterialAbiVersion == MaterialShaderAbiVersion &&
            refreshedSnapshot.ExpectedArtifactIdentityHash ==
                BuildMaterialShaderArtifactIdentityHash(secondLayout) &&
            refreshedSnapshot.ExpectedProgramGeneration == 1,
            "Render snapshot did not retain the exact manifest identity used for layout packing.");

        const uint64_t lastGoodRevision = hotReload.GetRuntimeRevision();
        const Json lastGoodOrphans = hotReload.GetOrphanedParameters();
        const std::string lastGoodHash = hotReload.GetExpectedRecursiveSourceHash();
        fixture.Write(
            "GeneratedContent/Shaders/CACHES/Materials/TestMaterial.hlsl.material.json",
            "{ invalid json");
        refreshError.clear();
        Require(!hotReload.RefreshShaderLayoutFromManifest(&refreshError) &&
                !refreshError.empty() &&
                hotReload.GetRuntimeRevision() == lastGoodRevision &&
                hotReload.GetExpectedRecursiveSourceHash() == lastGoodHash &&
                hotReload.GetOrphanedParameters() == lastGoodOrphans &&
                FindProperty(hotReload, "renamedTint").ColorValue == renamed.ColorValue &&
                hotReload.mPbrMatRes.alphaMode == "BLEND",
            "Failed layout refresh did not preserve the complete last-known-good material state.");

        const auto finalizeLayout = [](MaterialShaderManifest& manifest)
        {
            manifest.ParameterBlock.ByteSize = static_cast<uint32_t>(
                manifest.Properties.size() * MaterialShaderParameterAlignment);
            manifest.BindingSignatureHash =
                BuildMaterialShaderBindingSignatureHash(manifest);
        };
        const auto countOrphansWithStableId = [](const Json& orphans, std::string_view stableId)
        {
            size_t count = 0;
            for (const auto& [key, entry] : orphans.items())
            {
                (void)key;
                if (entry.is_object() && entry.value("StableId", std::string{}) == stableId)
                    ++count;
            }
            return count;
        };

        MaterialShaderManifest revivedLayout = secondLayout;
        revivedLayout.Properties.push_back(MakeLayoutProperty(
            "revivedValue", "surface.removed", MaterialShaderPropertyType::Float,
            { 0.1 }, static_cast<uint32_t>(revivedLayout.Properties.size())));
        finalizeLayout(revivedLayout);
        fixture.Write(
            "GeneratedContent/Shaders/CACHES/Materials/TestMaterial.hlsl.material.json",
            MaterialShaderManifestToJson(revivedLayout).dump(2));
        refreshError.clear();
        Require(hotReload.RefreshShaderLayoutFromManifest(&refreshError) &&
                std::abs(FindProperty(hotReload, "revivedValue").FloatValue - 0.6f) < 0.0001f &&
                countOrphansWithStableId(
                    hotReload.GetOrphanedParameters(), "surface.removed") == 0,
            "A deleted property was not restored and removed from orphans when its StableId returned.");

        MaterialShaderManifest firstGenerationLayout = revivedLayout;
        firstGenerationLayout.Properties.push_back(MakeLayoutProperty(
            "generationValue", "surface.generation.one", MaterialShaderPropertyType::Float,
            { 0.11 }, static_cast<uint32_t>(firstGenerationLayout.Properties.size())));
        finalizeLayout(firstGenerationLayout);
        fixture.Write(
            "GeneratedContent/Shaders/CACHES/Materials/TestMaterial.hlsl.material.json",
            MaterialShaderManifestToJson(firstGenerationLayout).dump(2));
        Require(hotReload.RefreshShaderLayoutFromManifest(&refreshError),
            "Could not publish the first same-name property generation.");
        for (MaterialRuntimeProperty& property : hotReload.GetExposedProperties())
        {
            if (property.Decl.StableId == "surface.generation.one")
                property.FloatValue = 0.31f;
        }
        hotReload.ApplyRuntimePropertyChanges();

        MaterialShaderManifest secondGenerationLayout = revivedLayout;
        secondGenerationLayout.Properties.push_back(MakeLayoutProperty(
            "generationValue", "surface.generation.two", MaterialShaderPropertyType::Float,
            { 0.22 }, static_cast<uint32_t>(secondGenerationLayout.Properties.size())));
        finalizeLayout(secondGenerationLayout);
        fixture.Write(
            "GeneratedContent/Shaders/CACHES/Materials/TestMaterial.hlsl.material.json",
            MaterialShaderManifestToJson(secondGenerationLayout).dump(2));
        Require(hotReload.RefreshShaderLayoutFromManifest(&refreshError) &&
                std::abs(FindProperty(hotReload, "generationValue").FloatValue - 0.22f) < 0.0001f,
            "A reused name with a different StableId inherited the previous generation's value.");
        for (MaterialRuntimeProperty& property : hotReload.GetExposedProperties())
        {
            if (property.Decl.StableId == "surface.generation.two")
                property.FloatValue = 0.42f;
        }
        hotReload.ApplyRuntimePropertyChanges();

        fixture.Write(
            "GeneratedContent/Shaders/CACHES/Materials/TestMaterial.hlsl.material.json",
            MaterialShaderManifestToJson(revivedLayout).dump(2));
        Require(hotReload.RefreshShaderLayoutFromManifest(&refreshError),
            "Could not remove the second same-name property generation.");
        const Json generationOrphans = hotReload.GetOrphanedParameters();
        Require(countOrphansWithStableId(generationOrphans, "surface.generation.one") == 1 &&
                countOrphansWithStableId(generationOrphans, "surface.generation.two") == 1,
            "Same-name orphan generations overwrote one another instead of remaining addressable.");

        fixture.Write(
            "Project/Assets/Material/DuplicateStableId.hmat",
            R"json({
  "SchemaVersion":2,
  "Type":"Custom",
  "PassName":"TestMaterial",
  "ShaderPath":"Shaders/TestMaterial.hlsl",
  "VariantKeywords":[],
  "Domain":"Surface",
  "ShadingModel":"Unlit",
  "BlendMode":"Transparent",
  "CullMode":"None",
  "DepthWrite":false,
  "AlphaCutoff":0.5,
  "Parameters":{
    "firstTint":{"StableId":"surface.tint","Type":"color","Value":[0.1,0.2,0.3,1.0]},
    "secondTint":{"StableId":"surface.tint","Type":"color","Value":[0.8,0.7,0.6,1.0]}
  },
  "Textures":{},
  "OrphanedParameters":{
    "secondTint":{"StableId":"legacy.tint","Type":"color","Value":[1,0,0,1],"Reason":"legacy-generation"}
  },
  "Properties":{}
})json");
        Material duplicateStableId("Assets/Material/DuplicateStableId.hmat");
        const Json duplicateOrphans = duplicateStableId.GetOrphanedParameters();
        Require(glm::all(glm::epsilonEqual(
                    FindProperty(duplicateStableId, "renamedTint").ColorValue,
                    glm::vec4(0.1f, 0.2f, 0.3f, 1.0f),
                    0.0001f)) &&
                duplicateOrphans.contains("secondTint") &&
                duplicateOrphans["secondTint"].value("StableId", "") == "legacy.tint" &&
                countOrphansWithStableId(duplicateOrphans, "surface.tint") == 1,
            "Duplicate StableId input overwrote the first value or an existing same-name orphan.");
        bool bFoundDuplicateDiagnostic = false;
        for (const auto& [key, entry] : duplicateOrphans.items())
        {
            (void)key;
            bFoundDuplicateDiagnostic = bFoundDuplicateDiagnostic ||
                (entry.is_object() &&
                    entry.value("Reason", "") == "duplicate-stable-id" &&
                    entry.value("DuplicateOf", "") == "Parameters.firstTint");
        }
        Require(bFoundDuplicateDiagnostic,
            "Duplicate StableId input was not preserved with an explicit diagnostic.");

        fixture.Write(
            "GeneratedContent/Shaders/CACHES/Materials/TestMaterial.hlsl.material.json",
            MaterialShaderManifestToJson(secondLayout).dump(2));
        fixture.Write(
            "Project/Assets/Material/CodeOnly.hmat",
            R"json({
  "SchemaVersion":2,
  "Type":"Custom",
  "PassName":"TestMaterial",
  "ShaderPath":"Shaders/TestMaterial.hlsl",
  "VariantKeywords":[],
  "Domain":"Surface",
  "ShadingModel":"Unlit",
  "BlendMode":"Transparent",
  "CullMode":"None",
  "DepthWrite":false,
  "AlphaCutoff":0.5,
  "Parameters":{},
  "Textures":{},
  "OrphanedParameters":{},
  "Properties":{}
})json");
        Material codeOnlyMaterial("Assets/Material/CodeOnly.hmat");
        MaterialShaderManifest codeOnlyManifest = secondLayout;
        codeOnlyManifest.RecursiveSourceHash = std::string(64, 'd');
        for (MaterialShaderArtifact& artifact : codeOnlyManifest.Artifacts)
            artifact.ContentHash = std::string(64, 'e');
        const MaterialShaderProgramIdentity codeOnlyIdentity{
            .SourcePath = "Shaders/TestMaterial.hlsl",
            .RecursiveContentHash = codeOnlyManifest.RecursiveSourceHash,
            .MaterialAbiVersion = codeOnlyManifest.MaterialAbiVersion,
            .BindingSignatureHash = codeOnlyManifest.BindingSignatureHash,
            .ArtifactIdentityHash =
                BuildMaterialShaderArtifactIdentityHash(codeOnlyManifest),
        };
        const uint64_t beforeCodeOnlyRevision = codeOnlyMaterial.GetRuntimeRevision();
        const MaterialShaderLayoutRefreshResult codeOnlyRefresh =
            codeOnlyMaterial.RefreshShaderLayoutFromPublishedProgram(
                codeOnlyManifest,
                codeOnlyIdentity,
                7);
        Require(codeOnlyRefresh.bSucceeded &&
                !codeOnlyRefresh.bPersistentStateChanged &&
                codeOnlyMaterial.GetRuntimeRevision() == beforeCodeOnlyRevision + 1 &&
                codeOnlyMaterial.GetExpectedProgramGeneration() == 7 &&
                codeOnlyMaterial.GetExpectedArtifactIdentityHash() ==
                    codeOnlyIdentity.ArtifactIdentityHash,
            "A code-only shader publication dirtied material state or lost exact program identity.");

        fixture.Write(
            "Project/Assets/Material/CullMismatch.hmat",
            R"json({
  "SchemaVersion":2,
  "Type":"Custom",
  "PassName":"TestMaterial",
  "ShaderPath":"Shaders/TestMaterial.hlsl",
  "VariantKeywords":[],
  "Domain":"Surface",
  "ShadingModel":"Unlit",
  "BlendMode":"Transparent",
  "CullMode":"Back",
  "DepthWrite":false,
  "AlphaCutoff":0.5,
  "Parameters":{},
  "Textures":{},
  "OrphanedParameters":{},
  "Properties":{}
})json");
        bool bRejectedCullMismatch = false;
        try
        {
            Material cullMismatch("Assets/Material/CullMismatch.hmat");
        }
        catch (const std::invalid_argument&)
        {
            bRejectedCullMismatch = true;
        }
        Require(bRejectedCullMismatch,
            "A V2 material with CullMode inconsistent with its manifest was accepted.");

        fixture.Write(
            "Project/Assets/Material/Unsafe.hmat",
            "{\"Type\":\"Custom\",\"ShaderPath\":\"../escape.hlsl\",\"Properties\":{}}\n");
        bool bRejectedUnsafePath = false;
        try
        {
            Material unsafe("Assets/Material/Unsafe.hmat");
        }
        catch (const std::invalid_argument&)
        {
            bRejectedUnsafePath = true;
        }
        Require(bRejectedUnsafePath, "Unsafe material ShaderPath was not rejected.");

        fixture.Write(
            "Project/Assets/Material/UnsafeTexture.hmat",
            R"json({
  "Type":"Default",
  "ShaderPath":"Shaders/TestMaterial.hlsl",
  "Properties":{"pbrMetallicRoughness":{"baseColorTexture":{"textureRes":{"textureSourcePath":"../outside.png"}}}}
})json");
        bool bRejectedUnsafeTexture = false;
        try
        {
            Material unsafeTexture("Assets/Material/UnsafeTexture.hmat");
        }
        catch (const std::invalid_argument&)
        {
            bRejectedUnsafeTexture = true;
        }
        Require(bRejectedUnsafeTexture, "Unsafe legacy material texture path was not rejected.");
    }
}
