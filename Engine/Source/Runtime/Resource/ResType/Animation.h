#pragma once
#include "Runtime/Core/Meta/Reflection/Reflection.h"
#include <vector>
#include <string>

namespace HEngine
{
    // Key-frame types (plain structs, serialized manually)

    REFLECTION_TYPE(AnimKeyPositionRes)
    STRUCT(AnimKeyPositionRes, Fields)
    {
        REFLECTION_BODY(AnimKeyPositionRes);
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        float timeStamp = 0.0f;
    };

    REFLECTION_TYPE(AnimKeyRotationRes)
    STRUCT(AnimKeyRotationRes, Fields)
    {
        REFLECTION_BODY(AnimKeyRotationRes);
        // quaternion: w, x, y, z
        float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
        float timeStamp = 0.0f;
    };

    REFLECTION_TYPE(AnimKeyScaleRes)
    STRUCT(AnimKeyScaleRes, Fields)
    {
        REFLECTION_BODY(AnimKeyScaleRes);
        float sx = 1.0f, sy = 1.0f, sz = 1.0f;
        float timeStamp = 0.0f;
    };

    REFLECTION_TYPE(AnimBoneChannelRes)
    STRUCT(AnimBoneChannelRes, Fields)
    {
        REFLECTION_BODY(AnimBoneChannelRes);
        std::string name;
        std::vector<AnimKeyPositionRes> positionKeys;
        std::vector<AnimKeyRotationRes> rotationKeys;
        std::vector<AnimKeyScaleRes>    scaleKeys;
    };

    REFLECTION_TYPE(AnimNodeRes)
    STRUCT(AnimNodeRes, Fields)
    {
        REFLECTION_BODY(AnimNodeRes);
        std::string          name;
        std::vector<float>   localTransform; // 16 floats, column-major GLM mat4
        PROPERTY(Disable)
        std::vector<AnimNodeRes> children;   // recursive, serialised manually
    };

    REFLECTION_TYPE(AnimBoneInfoRes)
    STRUCT(AnimBoneInfoRes, Fields)
    {
        REFLECTION_BODY(AnimBoneInfoRes);
        std::string        name;
        int                id = 0;
        std::vector<float> offsetMatrix; // 16 floats, column-major GLM mat4
    };

    REFLECTION_TYPE(AnimEventRes)
    STRUCT(AnimEventRes, Fields)
    {
        REFLECTION_BODY(AnimEventRes);
        std::string id;
        float timeStamp = 0.0f; // Clip ticks, in [0, duration).
        std::string name;
        std::string payload; // Application-defined JSON or asset identifier.
    };

    REFLECTION_TYPE(AnimClipRes)
    STRUCT(AnimClipRes, Fields)
    {
        REFLECTION_BODY(AnimClipRes);
        std::string                      name;
        float                            duration        = 0.0f;
        float                            ticksPerSecond  = 24.0f;
        std::vector<AnimBoneChannelRes>  channels;
        std::vector<AnimEventRes> events;
    };

    REFLECTION_TYPE(AnimRes)
    STRUCT(AnimRes, Fields)
    {
        REFLECTION_BODY(AnimRes);
        static std::string GetExtName() { return "hanim"; }

        int version = 1;
        std::string                  name;
        std::string                  meshResPath;
        std::string                  targetSkeletonPath;
        std::vector<AnimBoneInfoRes> boneInfos;
        AnimNodeRes                  rootNode;
        std::vector<AnimClipRes>     clips;
    };
}
