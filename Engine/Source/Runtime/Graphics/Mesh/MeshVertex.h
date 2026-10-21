#pragma once

#include <glm/glm.hpp>

namespace HEngine
{
    // CPU mesh asset layout shared by import, GPU upload and scene submission.
    // Pipeline/input-layout declarations live in the NVRHI renderer instead of
    // being embedded in asset data.
    struct StaticVertex
    {
        glm::vec3 Pos{ 0.0f };
        glm::vec3 PreviousPosition{ 0.0f };
        glm::vec3 Normal{ 0.0f };
        glm::vec2 TexCoord{ 0.0f };
        glm::vec2 TexCoord1{ 0.0f };
        glm::vec4 Color{ 1.0f };
        glm::vec3 Tangent{ 0.0f };
        glm::vec3 Bitangent{ 0.0f };

        StaticVertex() = default;
        StaticVertex(const StaticVertex&) = default;
        StaticVertex& operator=(const StaticVertex&) = default;
        StaticVertex(StaticVertex&&) = default;
        StaticVertex& operator=(StaticVertex&&) = default;

        StaticVertex(
            const glm::vec3 position,
            const glm::vec3 normal,
            const glm::vec2 texCoord,
            const glm::vec3 tangent,
            const glm::vec3 bitangent)
            : Pos(position)
            , PreviousPosition(position)
            , Normal(normal)
            , TexCoord(texCoord)
            , Tangent(tangent)
            , Bitangent(bitangent)
        {
        }
    };
}
