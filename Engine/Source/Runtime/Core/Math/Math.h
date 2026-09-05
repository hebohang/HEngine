#pragma once 

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <spdlog/fmt/bundled/format.h>

#include <iostream>
#include <format>

namespace fmt
{
    template<>
    struct fmt::formatter<glm::vec2> : fmt::formatter<std::string>
    {
        auto format(glm::vec2 pt, format_context& ctx) const -> decltype(ctx.out())
        {
            return fmt::format_to(ctx.out(), "[{0}, {1}]", pt.x, pt.y);
        }
    };

    template<>
    struct fmt::formatter<glm::vec3> : fmt::formatter<std::string>
    {
        auto format(glm::vec3 pt, format_context& ctx) const -> decltype(ctx.out())
        {
            return fmt::format_to(ctx.out(), "[{0}, {1}, {2}]", pt.x, pt.y, pt.z);
        }
    };

    template<>
    struct fmt::formatter<glm::vec4> : fmt::formatter<std::string>
    {
        auto format(glm::vec4 pt, format_context& ctx) const -> decltype(ctx.out())
        {
            return fmt::format_to(ctx.out(), "[{0}, {1}, {2}, {3}]", pt.x, pt.y, pt.z, pt.w);
        }
    };

    template<>
    struct fmt::formatter<glm::mat4> : fmt::formatter<std::string>
    {
        auto format(glm::mat4 pt, format_context& ctx) const -> decltype(ctx.out())
        {
            return fmt::format_to(ctx.out(), "\n{0}, \n{1}, \n{2}, \n{3}", (glm::vec4)pt[0], (glm::vec4)pt[1], (glm::vec4)pt[2], (glm::vec4)pt[3]);
        }
    };
}

namespace HEngine
{
    namespace HMath = glm;

    using HVec2 = glm::vec2;
    using HVec3 = glm::vec3;
    using HVec4 = glm::vec4;

    using HColor3 = glm::vec3;
    using HColor4 = glm::vec4;

    static std::string format_as(const glm::vec2& pt) {
        return std::format("[{0}, {1}]", pt.x, pt.y);
    }
    static std::string format_as(const glm::vec3& pt)
    {
        return std::format("[{0}, {1}, {2}]", pt.x, pt.y, pt.z);
    }
    static std::string format_as(const glm::vec4& pt)
    {
        return std::format("[{0}, {1}, {2}, {3}]", pt.x, pt.y, pt.z, pt.w);
    }
    static std::string format_as(const glm::ivec2& pt)
    {
        return std::format("[{0}, {1}]", pt.x, pt.y);
    }
    static std::string format_as(const glm::ivec3& pt)
    {
        return std::format("[{0}, {1}, {2}]", pt.x, pt.y, pt.z);
    }
    static std::string format_as(const glm::ivec4& pt)
    {
        return std::format("[{0}, {1}, {2}, {3}]", pt.x, pt.y, pt.z, pt.w);
    }
    static std::string format_as(const glm::mat4& mat)
    {
        std::string s;
        for (int i = 0; i < 4; ++i)
        {
            for (int j = 0; j < 4; ++j)
            {
                s += std::format("{} ", mat[i][j]);
            }
            s += "\n";
        }
        return s;
    }
}