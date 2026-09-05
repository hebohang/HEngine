#pragma once
#include "Runtime/Core/Meta/Reflection/Reflection.h"

namespace HEngine
{
#define HE_TEXTURE_FILTER_NEAREST (9728)
#define HE_TEXTURE_FILTER_LINEAR (9729)
#define HE_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST (9984)
#define HE_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST (9985)
#define HE_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR (9986)
#define HE_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR (9987)

#define HE_TEXTURE_WRAP_REPEAT (10497)
#define HE_TEXTURE_WRAP_CLAMP_TO_EDGE (33071)
#define HE_TEXTURE_WRAP_MIRRORED_REPEAT (33648)

    REFLECTION_TYPE(Sampler)
    CLASS(Sampler, Fields)
    {
        REFLECTION_BODY(Sampler);
    public:
        // - 9728 NEAREST
        // - 9729 LINEAR
        int magFilter = HE_TEXTURE_FILTER_LINEAR;
        
        // - 9728 NEAREST
        // - 9729 LINEAR
        // - 9984 NEAREST_MIPMAP_NEAREST
        // - 9985 LINEAR_MIPMAP_NEAREST
        // - 9986 NEAREST_MIPMAP_LINEAR
        // - 9987 LINEAR_MIPMAP_LINEAR
        int minFilter = HE_TEXTURE_FILTER_LINEAR;

        // - 33071 CLAMP_TO_EDGE
        // - 33648 MIRRORED_REPEAT
        // - 10497 REPEAT
        int wrapS = HE_TEXTURE_WRAP_CLAMP_TO_EDGE;

        // - 33071 CLAMP_TO_EDGE
        // - 33648 MIRRORED_REPEAT
        // - 10497 REPEAT
        int wrapT = HE_TEXTURE_WRAP_CLAMP_TO_EDGE;
    };

    // TODO: extras and extensions 
    // TODO: maybe serialize a texture file (?), then texture should be imported first
    // TODO: maybe sampler to Res (sampler.json?) then texture can share the same sampler
    REFLECTION_TYPE(TextureRes)
    CLASS(TextureRes, Fields)
    {
        REFLECTION_BODY(TextureRes);
    public:
        static std::string GetExtName() { return "htex"; }
    public:
        TextureRes() = default;
        TextureRes(const std::string& path) : textureSourcePath(path) {}
    public:
        std::string textureSourcePath;

        Sampler sampler;
    };
}