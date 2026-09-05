#include "Hepch.h"

#include "ShaderPropertyParser.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <unordered_map>

namespace HEngine
{
    namespace
    {
        static std::string TrimCopy(const std::string& s)
        {
            const auto begin = s.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return "";
            const auto end = s.find_last_not_of(" \t\r\n");
            return s.substr(begin, end - begin + 1);
        }

        static std::string ToLowerCopy(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        static std::string UnquoteCopy(const std::string& s)
        {
            const std::string trimmed = TrimCopy(s);
            if (trimmed.size() >= 2)
            {
                const char first = trimmed.front();
                const char last = trimmed.back();
                if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
                    return trimmed.substr(1, trimmed.size() - 2);
            }
            return trimmed;
        }

        static std::string RemoveUtf8Bom(std::string s)
        {
            if (s.size() >= 3 &&
                (unsigned char)s[0] == 0xEF &&
                (unsigned char)s[1] == 0xBB &&
                (unsigned char)s[2] == 0xBF)
            {
                s.erase(0, 3);
            }
            return s;
        }

        static std::string StripLineCommentPreserveQuotes(const std::string& line)
        {
            bool inDoubleQuote = false;
            bool inSingleQuote = false;
            for (size_t i = 0; i + 1 < line.size(); ++i)
            {
                const char c = line[i];
                const char n = line[i + 1];

                if (c == '"' && !inSingleQuote)
                    inDoubleQuote = !inDoubleQuote;
                else if (c == '\'' && !inDoubleQuote)
                    inSingleQuote = !inSingleQuote;

                if (!inDoubleQuote && !inSingleQuote && c == '/' && n == '/')
                    return line.substr(0, i);
            }
            return line;
        }

        static bool IsIdentStart(char c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        }

        static bool IsIdentChar(char c)
        {
            return IsIdentStart(c) || (c >= '0' && c <= '9');
        }

        static size_t SkipWhitespace(const std::string& s, size_t i)
        {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
                ++i;
            return i;
        }

        static bool ParseQuoted(const std::string& s, size_t& i, std::string& out)
        {
            if (i >= s.size() || (s[i] != '"' && s[i] != '\''))
                return false;

            const char quote = s[i++];
            std::string value;
            while (i < s.size())
            {
                const char c = s[i++];
                if (c == '\\' && i < s.size())
                {
                    value.push_back(s[i++]);
                    continue;
                }
                if (c == quote)
                {
                    out = value;
                    return true;
                }
                value.push_back(c);
            }
            return false;
        }

        static bool ParseIdentifier(const std::string& s, size_t& i, std::string& out)
        {
            i = SkipWhitespace(s, i);
            if (i >= s.size() || !IsIdentStart(s[i]))
                return false;

            const size_t begin = i;
            ++i;
            while (i < s.size() && IsIdentChar(s[i]))
                ++i;

            out = s.substr(begin, i - begin);
            return true;
        }

        static bool ParsePropertyPragmaLine(
            const std::string& rawLine,
            std::string& outName,
            std::string& outDisplayName,
            std::string& outType,
            std::string& outTail)
        {
            std::string line = TrimCopy(RemoveUtf8Bom(StripLineCommentPreserveQuotes(rawLine)));
            if (line.empty())
                return false;

            static const std::regex headerPattern(
                R"(^#\s*pragma\s+property\s+(.+)$)",
                std::regex_constants::icase);

            std::smatch headerMatch;
            if (!std::regex_match(line, headerMatch, headerPattern))
                return false;

            std::string body = headerMatch[1].str();
            size_t i = 0;

            if (!ParseIdentifier(body, i, outName))
                return false;

            i = SkipWhitespace(body, i);
            if (i >= body.size())
                return false;

            if (body[i] == '"' || body[i] == '\'')
            {
                if (!ParseQuoted(body, i, outDisplayName))
                    return false;
            }
            else
            {
                // 兼容未加引号的写法：displayName 按下一个 token 解析
                if (!ParseIdentifier(body, i, outDisplayName))
                    return false;
            }

            if (!ParseIdentifier(body, i, outType))
                return false;

            outTail = TrimCopy(body.substr(i));
            return true;
        }

        static std::vector<std::string> SplitByComma(const std::string& text)
        {
            std::vector<std::string> out;
            size_t begin = 0;
            while (begin < text.size())
            {
                size_t comma = text.find(',', begin);
                if (comma == std::string::npos)
                    comma = text.size();
                out.push_back(TrimCopy(text.substr(begin, comma - begin)));
                begin = comma + 1;
            }
            return out;
        }

        static bool TryParseFiniteFloat(const std::string& text, float& outValue)
        {
            const std::string trimmed = TrimCopy(text);
            if (trimmed.empty() || trimmed.find('\0') != std::string::npos)
                return false;

            errno = 0;
            char* end = nullptr;
            const float value = std::strtof(trimmed.c_str(), &end);
            if (errno == ERANGE || end != trimmed.c_str() + trimmed.size() || !std::isfinite(value))
                return false;
            outValue = value;
            return true;
        }

        static bool TryParseInt(const std::string& text, int& outValue)
        {
            const std::string trimmed = TrimCopy(text);
            if (trimmed.empty() || trimmed.find('\0') != std::string::npos)
                return false;

            int value = 0;
            const auto result = std::from_chars(
                trimmed.data(), trimmed.data() + trimmed.size(), value);
            if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size())
                return false;
            outValue = value;
            return true;
        }

        static bool TryParseBool(const std::string& text, bool& outValue)
        {
            std::string normalized = ToLowerCopy(TrimCopy(text));
            if (normalized == "true" || normalized == "1")
            {
                outValue = true;
                return true;
            }
            if (normalized == "false" || normalized == "0")
            {
                outValue = false;
                return true;
            }
            return false;
        }

        static bool ContainsParentTraversal(const std::filesystem::path& path)
        {
            return std::any_of(path.begin(), path.end(), [](const std::filesystem::path& component)
            {
                return component == "..";
            });
        }

        static std::filesystem::path NormalizeExistingPath(const std::filesystem::path& path)
        {
            std::error_code error;
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
            return error ? path.lexically_normal() : canonical;
        }

        static bool IsPathWithin(
            const std::filesystem::path& candidate,
            const std::filesystem::path& root)
        {
            std::error_code error;
            const std::filesystem::path relative = std::filesystem::relative(
                NormalizeExistingPath(candidate), NormalizeExistingPath(root), error);
            if (error || relative.empty() || relative.is_absolute() || relative.has_root_name())
                return false;
            return !ContainsParentTraversal(relative);
        }

        static void AddDiagnostic(
            ShaderPropertyParseResult& result,
            ShaderPropertyDiagnosticSeverity severity,
            const std::filesystem::path& sourcePath,
            uint32_t line,
            std::string message)
        {
            result.Diagnostics.push_back({
                .Severity = severity,
                .SourcePath = Utils::PathToUtf8(sourcePath),
                .Line = line,
                .Column = 1,
                .Message = std::move(message),
            });
        }

        static std::string ReadTextFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::in | std::ios::binary);
            if (!file)
                return "";

            std::string content;
            file.seekg(0, std::ios::end);
            content.resize(static_cast<size_t>(file.tellg()));
            file.seekg(0, std::ios::beg);
            file.read(content.data(), static_cast<std::streamsize>(content.size()));
            return content;
        }

        static bool TryResolveIncludePath(
            const std::filesystem::path& currentDir,
            const std::string& includePath,
            std::filesystem::path& outPath)
        {
            outPath.clear();
            if (includePath.empty() || includePath.find('\0') != std::string::npos)
                return false;

            const std::filesystem::path includeRel = Utils::Utf8ToPath(includePath);
            if (includeRel.is_absolute() || includeRel.has_root_name() || ContainsParentTraversal(includeRel))
                return false;

            const std::filesystem::path projectShaderRoot =
                NormalizeExistingPath(ConfigManager::GetInstance().GetShadersFolder());
            const std::filesystem::path engineShaderRoot = NormalizeExistingPath(
                ConfigManager::GetEngineContentFullPath("Shaders"));
            const auto tryCandidate = [&](const std::filesystem::path& candidate)
            {
                std::error_code error;
                if (!std::filesystem::is_regular_file(candidate, error) || error)
                    return false;
                const std::filesystem::path normalized = NormalizeExistingPath(candidate);
                if (!IsPathWithin(normalized, projectShaderRoot) &&
                    !IsPathWithin(normalized, engineShaderRoot))
                {
                    return false;
                }
                outPath = normalized;
                return true;
            };

            if (tryCandidate(currentDir / includeRel))
                return true;
            if (tryCandidate(projectShaderRoot / includeRel))
                return true;
            if (tryCandidate(engineShaderRoot / includeRel))
                return true;

            return false;
        }

        static std::string ExpandShaderSource(
            const std::filesystem::path& filePath,
            std::unordered_set<std::string>& visited,
            ShaderPropertyParseResult& result)
        {
            std::error_code fileError;
            if (!std::filesystem::is_regular_file(filePath, fileError) || fileError)
                return "";

            const std::filesystem::path normalizedFilePath = NormalizeExistingPath(filePath);
            const std::string visitedKey = Utils::MakeProjectPathCacheKey(
                Utils::PathToUtf8(normalizedFilePath));

            if (!visited.insert(visitedKey).second)
                return "";

            const std::string text = ReadTextFile(normalizedFilePath);
            if (text.empty())
                return "";

            static const std::regex includePattern(
                R"re(^\s*#\s*include\s*(["<])([^">]+)[">])re");

            std::stringstream ss(text);
            std::string line;
            std::string expanded;
            const std::filesystem::path currentDir = normalizedFilePath.parent_path();
            uint32_t lineNumber = 0;
            while (std::getline(ss, line))
            {
                ++lineNumber;
                std::smatch includeMatch;
                if (std::regex_search(line, includeMatch, includePattern))
                {
                    std::filesystem::path includeFullPath;
                    if (TryResolveIncludePath(currentDir, includeMatch[2].str(), includeFullPath))
                    {
                        expanded += ExpandShaderSource(includeFullPath, visited, result);
                        expanded += "\n";
                        continue;
                    }
                    AddDiagnostic(
                        result,
                        ShaderPropertyDiagnosticSeverity::Error,
                        normalizedFilePath,
                        lineNumber,
                        "Shader include is missing or outside the allowed Project/Engine shader roots: " +
                            includeMatch[2].str());
                }

                expanded += line;
                expanded += "\n";
            }

            return expanded;
        }
    }

    std::optional<std::string> ShaderPropertyParser::ReadPropertiesBlock(const std::string& source)
    {
        const std::string token = "namespace Properties";
        const size_t nsPos = source.find(token);
        if (nsPos == std::string::npos)
            return std::nullopt;

        const size_t openBrace = source.find('{', nsPos + token.size());
        if (openBrace == std::string::npos)
            return std::nullopt;

        int depth = 1;
        size_t i = openBrace + 1;
        for (; i < source.size(); ++i)
        {
            if (source[i] == '{')
                depth++;
            else if (source[i] == '}')
            {
                depth--;
                if (depth == 0)
                    break;
            }
        }

        if (depth != 0 || i <= openBrace)
            return std::nullopt;

        return source.substr(openBrace + 1, i - openBrace - 1);
    }

    bool ShaderPropertyParseResult::Succeeded() const
    {
        return std::none_of(Diagnostics.begin(), Diagnostics.end(), [](const ShaderPropertyDiagnostic& diagnostic)
        {
            return diagnostic.Severity == ShaderPropertyDiagnosticSeverity::Error;
        });
    }

    ShaderPropertyParseResult ShaderPropertyParser::ParseWithDiagnostics(const std::string& shaderPath)
    {
        ShaderPropertyParseResult result;
        if (shaderPath.empty())
        {
            AddDiagnostic(
                result,
                ShaderPropertyDiagnosticSeverity::Error,
                {},
                0,
                "Shader path is empty.");
            return result;
        }

        std::filesystem::path fullPath;
        try
        {
            fullPath = ConfigManager::GetFullPath(shaderPath);
        }
        catch (const std::exception& exception)
        {
            AddDiagnostic(
                result,
                ShaderPropertyDiagnosticSeverity::Error,
                Utils::Utf8ToPath(shaderPath),
                0,
                exception.what());
            return result;
        }

        std::error_code error;
        if (!std::filesystem::is_regular_file(fullPath, error) || error)
        {
            AddDiagnostic(
                result,
                ShaderPropertyDiagnosticSeverity::Error,
                fullPath,
                0,
                "Shader source does not exist or is not a regular file.");
            return result;
        }

        std::unordered_set<std::string> visited;
        const std::string source = ExpandShaderSource(fullPath, visited, result);
        if (source.empty())
        {
            AddDiagnostic(
                result,
                ShaderPropertyDiagnosticSeverity::Error,
                fullPath,
                0,
                "Shader source is empty or could not be read.");
            return result;
        }

        const std::regex defaultPattern(
            R"(default\(([^\)]*)\))",
            std::regex_constants::icase);
        const std::regex rangePattern(
            R"(range\(([^\),]+)\s*,\s*([^\)]+)\))",
            std::regex_constants::icase);
        const std::regex groupPattern(
            R"(group\(([^\)]*)\))",
            std::regex_constants::icase);
        const std::regex semanticPattern(
            R"(semantic\(([^\)]*)\))",
            std::regex_constants::icase);
        const std::regex colorSpacePattern(
            R"(colorspace\(([^\)]*)\))",
            std::regex_constants::icase);
        const std::regex fallbackPattern(
            R"(fallback\(([^\)]*)\))",
            std::regex_constants::icase);
        const std::regex stableIdPattern(
            R"(id\(([^\)]*)\))",
            std::regex_constants::icase);
        const std::regex propertyPrefixPattern(
            R"(^\s*#\s*pragma\s+property\b)",
            std::regex_constants::icase);
        const std::regex textureRegisterPattern(
            R"(Texture2D(?:\s*<[^>]+>)?\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*register\(\s*t([0-9]+)\s*(?:,\s*space([0-9]+)\s*)?\))",
            std::regex_constants::icase);

        struct TextureRegister
        {
            int Slot = -1;
            int Space = 0;
        };
        std::unordered_map<std::string, TextureRegister> textureRegisterMap;
        for (std::sregex_iterator iterator(source.begin(), source.end(), textureRegisterPattern), end;
            iterator != end;
            ++iterator)
        {
            int slot = -1;
            int space = 0;
            if (!TryParseInt((*iterator)[2].str(), slot) ||
                ((*iterator)[3].matched && !TryParseInt((*iterator)[3].str(), space)) ||
                slot < 0 || space < 0)
            {
                AddDiagnostic(
                    result,
                    ShaderPropertyDiagnosticSeverity::Error,
                    fullPath,
                    0,
                    "Texture register index is malformed or out of range.");
                continue;
            }

            const std::string name = (*iterator)[1].str();
            textureRegisterMap[name] = { .Slot = slot, .Space = space };
        }

        std::unordered_set<std::string> declaredNames;
        std::unordered_set<std::string> declaredStableIds;
        std::unordered_map<std::string, std::string> declaredTextureBindings;
        std::stringstream stream(source);
        std::string line;
        uint32_t lineNumber = 0;
        while (std::getline(stream, line))
        {
            ++lineNumber;
            const bool bPropertyLine = std::regex_search(line, propertyPrefixPattern);
            std::string propertyName;
            std::string displayName;
            std::string typeText;
            std::string tail;
            if (!ParsePropertyPragmaLine(line, propertyName, displayName, typeText, tail))
            {
                if (bPropertyLine)
                {
                    AddDiagnostic(
                        result,
                        ShaderPropertyDiagnosticSeverity::Error,
                        fullPath,
                        lineNumber,
                        "Malformed #pragma property; expected '<name> \"display\" <type> [options]'.");
                }
                continue;
            }

            if (!declaredNames.insert(propertyName).second)
            {
                AddDiagnostic(
                    result,
                    ShaderPropertyDiagnosticSeverity::Error,
                    fullPath,
                    lineNumber,
                    "Duplicate material property: " + propertyName);
                continue;
            }

            MaterialPropertyDecl declaration;
            declaration.Name = propertyName;
            declaration.StableId = propertyName;
            declaration.DisplayName = displayName;
            declaration.SourceLine = lineNumber;
            declaration.RowOffset = static_cast<uint32_t>(result.Declarations.size());
            declaration.ByteOffset = declaration.RowOffset * 16u;

            const std::string normalizedType = ToLowerCopy(typeText);
            if (normalizedType == "texture" || normalizedType == "texture2d")
            {
                declaration.Type = MaterialPropertyType::Texture;
                declaration.ByteSize = 4;
            }
            else if (normalizedType == "float")
            {
                declaration.Type = MaterialPropertyType::Float;
                declaration.ByteSize = 4;
            }
            else if (normalizedType == "float2" || normalizedType == "vector2")
            {
                declaration.Type = MaterialPropertyType::Float2;
                declaration.ByteSize = 8;
            }
            else if (normalizedType == "float3" || normalizedType == "vector3")
            {
                declaration.Type = MaterialPropertyType::Float3;
                declaration.ByteSize = 12;
            }
            else if (normalizedType == "color" || normalizedType == "float4")
            {
                declaration.Type = MaterialPropertyType::Color;
                declaration.ByteSize = 16;
                declaration.DefaultValue.w = 1.0f;
            }
            else if (normalizedType == "bool")
            {
                declaration.Type = MaterialPropertyType::Bool;
                declaration.ByteSize = 4;
            }
            else if (normalizedType == "int")
            {
                declaration.Type = MaterialPropertyType::Int;
                declaration.ByteSize = 4;
            }
            else
            {
                AddDiagnostic(
                    result,
                    ShaderPropertyDiagnosticSeverity::Error,
                    fullPath,
                    lineNumber,
                    "Unsupported material property type '" + typeText + "'.");
                continue;
            }

            bool bPropertyValid = true;
            std::smatch defaultMatch;
            if (std::regex_search(tail, defaultMatch, defaultPattern))
            {
                const std::string defaultPayload = TrimCopy(defaultMatch[1].str());
                const std::vector<std::string> values = SplitByComma(defaultPayload);
                if (declaration.Type == MaterialPropertyType::Texture)
                {
                    declaration.DefaultTextureToken = UnquoteCopy(defaultPayload);
                    bPropertyValid = !declaration.DefaultTextureToken.empty();
                }
                else if (declaration.Type == MaterialPropertyType::Bool)
                {
                    bPropertyValid = values.size() == 1 &&
                        TryParseBool(values.front(), declaration.DefaultBool);
                    declaration.DefaultValue.x = declaration.DefaultBool ? 1.0f : 0.0f;
                }
                else if (declaration.Type == MaterialPropertyType::Int)
                {
                    bPropertyValid = values.size() == 1 &&
                        TryParseInt(values.front(), declaration.DefaultInt);
                    declaration.DefaultValue.x = static_cast<float>(declaration.DefaultInt);
                }
                else
                {
                    size_t requiredValueCount = 1;
                    if (declaration.Type == MaterialPropertyType::Float2)
                        requiredValueCount = 2;
                    else if (declaration.Type == MaterialPropertyType::Float3)
                        requiredValueCount = 3;
                    else if (declaration.Type == MaterialPropertyType::Color)
                        requiredValueCount = values.size() == 3 ? 3 : 4;
                    bPropertyValid = values.size() == requiredValueCount;
                    for (size_t index = 0; bPropertyValid && index < values.size(); ++index)
                        bPropertyValid = TryParseFiniteFloat(values[index], declaration.DefaultValue[index]);
                    if (declaration.Type == MaterialPropertyType::Color && values.size() == 3)
                        declaration.DefaultValue.w = 1.0f;
                }

                if (!bPropertyValid)
                {
                    AddDiagnostic(
                        result,
                        ShaderPropertyDiagnosticSeverity::Error,
                        fullPath,
                        lineNumber,
                        "Invalid default(...) for material property '" + propertyName + "'.");
                }
            }

            std::smatch rangeMatch;
            if (std::regex_search(tail, rangeMatch, rangePattern))
            {
                declaration.bHasRange = true;
                if ((declaration.Type != MaterialPropertyType::Float &&
                        declaration.Type != MaterialPropertyType::Int) ||
                    !TryParseFiniteFloat(rangeMatch[1].str(), declaration.RangeMin) ||
                    !TryParseFiniteFloat(rangeMatch[2].str(), declaration.RangeMax) ||
                    declaration.RangeMin > declaration.RangeMax)
                {
                    bPropertyValid = false;
                    AddDiagnostic(
                        result,
                        ShaderPropertyDiagnosticSeverity::Error,
                        fullPath,
                        lineNumber,
                        "Invalid range(min,max) for material property '" + propertyName + "'.");
                }
                else if (std::regex_search(tail, defaultMatch, defaultPattern) &&
                    (declaration.DefaultValue.x < declaration.RangeMin ||
                        declaration.DefaultValue.x > declaration.RangeMax))
                {
                    bPropertyValid = false;
                    AddDiagnostic(
                        result,
                        ShaderPropertyDiagnosticSeverity::Error,
                        fullPath,
                        lineNumber,
                        "Default value is outside range(...) for material property '" + propertyName + "'.");
                }
            }

            std::smatch match;
            if (std::regex_search(tail, match, groupPattern))
                declaration.Category = UnquoteCopy(match[1].str());
            if (std::regex_search(tail, match, semanticPattern))
                declaration.TextureSemantic = UnquoteCopy(match[1].str());
            if (std::regex_search(tail, match, colorSpacePattern))
                declaration.ColorSpace = UnquoteCopy(match[1].str());
            if (std::regex_search(tail, match, fallbackPattern))
                declaration.DefaultTextureToken = UnquoteCopy(match[1].str());
            if (std::regex_search(tail, match, stableIdPattern))
                declaration.StableId = UnquoteCopy(match[1].str());
            if (declaration.StableId.empty() ||
                !declaredStableIds.insert(declaration.StableId).second)
            {
                bPropertyValid = false;
                AddDiagnostic(
                    result,
                    ShaderPropertyDiagnosticSeverity::Error,
                    fullPath,
                    lineNumber,
                    "Material property stable id must be non-empty and unique: " +
                        declaration.StableId);
            }

            if (declaration.Type == MaterialPropertyType::Texture)
            {
                if (const auto binding = textureRegisterMap.find(declaration.Name);
                    binding != textureRegisterMap.end())
                {
                    declaration.TextureSlot = binding->second.Slot;
                    declaration.RegisterSpace = binding->second.Space;
                    const std::string bindingKey = std::to_string(declaration.RegisterSpace) +
                        ":" + std::to_string(declaration.TextureSlot);
                    if (const auto owner = declaredTextureBindings.find(bindingKey);
                        owner != declaredTextureBindings.end() && owner->second != declaration.Name)
                    {
                        bPropertyValid = false;
                        AddDiagnostic(
                            result,
                            ShaderPropertyDiagnosticSeverity::Error,
                            fullPath,
                            lineNumber,
                            "Texture binding conflicts with property '" + owner->second + "'.");
                    }
                    else
                    {
                        declaredTextureBindings[bindingKey] = declaration.Name;
                    }
                }
            }

            if (bPropertyValid)
                result.Declarations.push_back(std::move(declaration));
        }

        return result;
    }

    std::vector<MaterialPropertyDecl> ShaderPropertyParser::Parse(const std::string& shaderPath)
    {
        ShaderPropertyParseResult result = ParseWithDiagnostics(shaderPath);
        return result.Succeeded()
            ? std::move(result.Declarations)
            : std::vector<MaterialPropertyDecl>{};
    }
}
