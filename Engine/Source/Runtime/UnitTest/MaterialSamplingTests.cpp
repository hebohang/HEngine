#include "Hepch.h"

#include "Runtime/UnitTest/MaterialSamplingTests.h"
#include "Runtime/Resource/ResType/BasePbrMaterialBuilder.h"
#include "Runtime/Resource/ResType/TextureSampling.h"
#include "_generated/serializer/TextureSampling.gen.h"

#include <cmath>
#include <stdexcept>

namespace HEngine::Tests
{
    void MaterialSamplingContract()
    {
        const auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
        TextureSampling sampling;
        auto packed = PackTextureSampling(sampling);
        require(packed.Options == glm::uvec4(0,0,0,261), "Legacy sampling default no longer selects the hardware repeat/trilinear path.");
        for (int wrapS : {10497,33071,33648})
            for (int wrapT : {10497,33071,33648})
                for (int minFilter : {9728,9729,9984,9985,9986,9987})
                    for (int magFilter : {9728,9729})
                    {
                        sampling.wrapS = wrapS; sampling.wrapT = wrapT;
                        sampling.minFilter = minFilter; sampling.magFilter = magFilter;
                        packed = PackTextureSampling(sampling);
                        require(packed.Options.y < 3 && packed.Options.z < 3 && (packed.Options.w & 255) < 6,
                            "A standard glTF sampler combination cannot be represented.");
                    }
        sampling = {};
        sampling.texCoord = 1;
        sampling.scale = {2,3};
        sampling.rotation = glm::radians(90.0f);
        sampling.offset = {.25f,-.5f};
        packed = PackTextureSampling(sampling);
        const glm::vec3 coordinate(.5f,.25f,1);
        require(std::abs(glm::dot(glm::vec3(packed.UvRow0), coordinate) + .5f) < 1e-5f &&
            std::abs(glm::dot(glm::vec3(packed.UvRow1), coordinate) - .5f) < 1e-5f,
            "Texture transform must scale, rotate and then translate.");
        Json json;
        Serializer::Serialize(json, sampling);
        TextureSampling roundTrip;
        Serializer::Deserialize(json, roundTrip);
        require(roundTrip.texCoord == 1 && roundTrip.offset == sampling.offset, "Generated serialization lost UV1/transform.");
        TextureSampling legacy;
        Serializer::Deserialize(Json::object(), legacy);
        require(PackTextureSampling(legacy).Options == glm::uvec4(0,0,0,261), "Missing fields do not retain backward-compatible sampling defaults.");
        bool rejected = false;
        try { sampling.texCoord = 2; (void)PackTextureSampling(sampling); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Unsupported UV channel must be rejected.");

        PbrMaterialRes pbr;
        pbr.bUseAlbedoMap = true;
        pbr.pbrMetallicRoughness.baseColorTexture.textureRes.textureSourcePath = "Assets/Textures/Fixture.png";
        pbr.pbrMetallicRoughness.baseColorTexture.texCoord = 1;
        pbr.pbrMetallicRoughness.baseColorTexture.sampling = roundTrip;
        const auto material = BuildBasePbrMaterialResV2(pbr);
        require(material.Textures.at("g_AlbedoMap").at("Sampling").at("texCoord") == 1,
            "PBR builder discarded per-texture UV selection.");
    }
}
