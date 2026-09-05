#pragma once

#include <glm/glm.hpp>

#include <optional>
#include <cstdint>
#include <string>
#include <vector>

namespace HEngine
{
    enum class MaterialPropertyType
    {
        Texture = 0,
        Float,
        Color,
        Float2,
        Float3,
        Bool,
        Int
    };

    enum class ShaderPropertyDiagnosticSeverity
    {
        Warning,
        Error
    };

    struct ShaderPropertyDiagnostic
    {
        ShaderPropertyDiagnosticSeverity Severity = ShaderPropertyDiagnosticSeverity::Error;
        std::string SourcePath;
        uint32_t Line = 0;
        uint32_t Column = 0;
        std::string Message;
    };

    struct MaterialPropertyDecl
    {
        std::string Name;
        std::string StableId;
        std::string DisplayName;
        std::string Category;
        MaterialPropertyType Type = MaterialPropertyType::Float;
        glm::vec4 DefaultValue = glm::vec4(0.0f);
        int DefaultInt = 0;
        bool DefaultBool = false;
        bool bHasRange = false;
        float RangeMin = 0.0f;
        float RangeMax = 1.0f;
        std::string DefaultTextureToken;
        std::string TextureSemantic;
        std::string ColorSpace;
        int TextureSlot = -1;
        int RegisterSpace = -1;
        uint32_t ArrayCount = 1;
        uint32_t RowOffset = 0;
        uint32_t ByteOffset = 0;
        uint32_t ByteSize = 0;
        uint32_t SourceLine = 0;
    };

    struct ShaderPropertyParseResult
    {
        std::vector<MaterialPropertyDecl> Declarations;
        std::vector<ShaderPropertyDiagnostic> Diagnostics;

        [[nodiscard]] bool Succeeded() const;
    };

    class ShaderPropertyParser
    {
    public:
        static ShaderPropertyParseResult ParseWithDiagnostics(const std::string& shaderPath);
        static std::vector<MaterialPropertyDecl> Parse(const std::string& shaderPath);
    private:
        static std::optional<std::string> ReadPropertiesBlock(const std::string& source);
    };
}
