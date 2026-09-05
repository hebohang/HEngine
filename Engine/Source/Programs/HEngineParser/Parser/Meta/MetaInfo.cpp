#include "Common/Precompiled.h"

#include "Parser/Parser.h"

#include "MetaInfo.h"

namespace
{
    struct Delimiter
    {
        char character = '\0';
        size_t offset = 0;
    };

    [[nodiscard]] bool isOpeningDelimiter(char character) noexcept
    {
        return character == '(' || character == '[' || character == '{' || character == '<';
    }

    [[nodiscard]] bool isClosingDelimiter(char character) noexcept
    {
        return character == ')' || character == ']' || character == '}' || character == '>';
    }

    [[nodiscard]] char matchingOpeningDelimiter(char character) noexcept
    {
        switch (character)
        {
            case ')': return '(';
            case ']': return '[';
            case '}': return '{';
            case '>': return '<';
            default: return '\0';
        }
    }

    [[nodiscard]] std::string delimiterName(char character)
    {
        switch (character)
        {
            case '(': return "parenthesis '('";
            case '[': return "bracket '['";
            case '{': return "brace '{'";
            case '<': return "angle bracket '<'";
            case ')': return "parenthesis ')'";
            case ']': return "bracket ']'";
            case '}': return "brace '}'";
            case '>': return "angle bracket '>'";
            default: return "delimiter";
        }
    }

    bool splitMetadata(const std::string& input,
                       char separator,
                       std::vector<std::string>& result,
                       std::string& error)
    {
        result.clear();
        error.clear();
        std::string current;
        std::vector<Delimiter> delimiters;
        char quote = '\0';
        size_t quoteOffset = 0;
        bool escaped = false;

        for (size_t offset = 0; offset < input.size(); ++offset)
        {
            const char character = input[offset];
            if (quote != '\0')
            {
                current.push_back(character);
                if (escaped)
                {
                    escaped = false;
                    continue;
                }
                if (character == '\\')
                {
                    escaped = true;
                    continue;
                }
                if (character == quote)
                {
                    quote = '\0';
                }
                continue;
            }
            if (character == '\'' || character == '"')
            {
                quote = character;
                quoteOffset = offset;
                current.push_back(character);
                continue;
            }

            if (isOpeningDelimiter(character))
            {
                delimiters.push_back({character, offset});
                current.push_back(character);
                continue;
            }
            if (isClosingDelimiter(character))
            {
                if (delimiters.empty())
                {
                    error = "metadata contains an unexpected closing " + delimiterName(character);
                    return false;
                }
                const char expected = matchingOpeningDelimiter(character);
                if (delimiters.back().character != expected)
                {
                    error = "metadata closes " + delimiterName(character) +
                        " while " + delimiterName(delimiters.back().character) + " is still open";
                    return false;
                }
                delimiters.pop_back();
                current.push_back(character);
                continue;
            }

            if (character == separator && delimiters.empty())
            {
                result.emplace_back(std::move(current));
                current.clear();
            }
            else
            {
                current.push_back(character);
            }
        }

        if (quote != '\0')
        {
            error = std::string("metadata contains an unclosed ") +
                (quote == '"' ? "double quote" : "single quote") +
                " opened at offset " + std::to_string(quoteOffset);
            return false;
        }
        if (!delimiters.empty())
        {
            error = "metadata contains an unclosed " + delimiterName(delimiters.back().character) +
                " opened at offset " + std::to_string(delimiters.back().offset);
            return false;
        }

        result.emplace_back(std::move(current));
        return true;
    }

    [[nodiscard]] size_t findPropertySeparator(const std::string& input)
    {
        std::vector<char> delimiters;
        char quote = '\0';
        bool escaped = false;
        for (size_t offset = 0; offset < input.size(); ++offset)
        {
            const char character = input[offset];
            if (quote != '\0')
            {
                if (escaped)
                {
                    escaped = false;
                    continue;
                }
                if (character == '\\')
                {
                    escaped = true;
                    continue;
                }
                if (character == quote)
                    quote = '\0';
                continue;
            }
            if (character == '\'' || character == '"')
            {
                quote = character;
                continue;
            }
            if (isOpeningDelimiter(character))
            {
                delimiters.push_back(character);
                continue;
            }
            if (isClosingDelimiter(character))
            {
                if (!delimiters.empty())
                    delimiters.pop_back();
                continue;
            }
            if (character == ':' && delimiters.empty())
                return offset;
        }
        return std::string::npos;
    }
}

MetaInfo::MetaInfo(const Cursor& cursor)
{
    for (auto& child : cursor.getChildren())
    {

        if (child.getKind() != CXCursor_AnnotateAttr)
            continue;

        std::string error;
        const auto properties = extractProperties(child, error);
        if (!error.empty())
        {
            addDiagnostic(child, std::move(error));
            continue;
        }
        for (const auto& property : properties)
        {
            const bool inserted = m_properties.emplace(property.first, property.second).second;
            if (!inserted)
            {
                addDiagnostic(child, "duplicate metadata key '" + property.first + "'");
            }
        }
    }
}

std::string MetaInfo::getProperty(const std::string& key) const
{
    auto search = m_properties.find(key);

    // use an empty string by default
    return search == m_properties.end() ? "" : search->second;
}

bool MetaInfo::getFlag(const std::string& key) const { return m_properties.find(key) != m_properties.end(); }

const std::vector<MetaInfo::Diagnostic>& MetaInfo::getDiagnostics() const noexcept
{
    return m_diagnostics;
}

std::vector<MetaInfo::Property> MetaInfo::extractProperties(const Cursor& cursor, std::string& error) const
{
    std::vector<Property> ret_list;

    auto propertyList = cursor.getDisplayName();

    std::vector<std::string> properties;
    if (!splitMetadata(propertyList, ',', properties, error))
        return ret_list;

    static const std::string white_space_string = " \t\r\n";

    for (auto& property_item : properties)
    {
        const size_t separator = findPropertySeparator(property_item);
        std::string key = separator == std::string::npos
            ? property_item
            : property_item.substr(0, separator);
        key = Utils::trim(key, white_space_string);
        if (key.empty())
        {
            continue;
        }
        std::string value = separator == std::string::npos
            ? std::string()
            : property_item.substr(separator + 1);
        ret_list.emplace_back(std::move(key), Utils::trim(value, white_space_string));
    }
    return ret_list;
}

void MetaInfo::addDiagnostic(const Cursor& cursor, std::string message)
{
    m_diagnostics.push_back({
        .source = cursor.getSourceFile(),
        .line = cursor.getSourceLine(),
        .column = cursor.getSourceColumn(),
        .message = std::move(message),
    });
}
