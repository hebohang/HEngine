#pragma once

class Cursor;

class CursorType
{
public:
    CursorType(const CXType& handle);

    std::string GetDisplayName(void) const;

    int GetArgumentCount(void) const;

    CursorType GetArgument(unsigned index) const;

    CursorType GetCanonicalType(void) const;

    int GetTemplateArgumentCount(void) const;

    CursorType GetTemplateArgument(unsigned index) const;

    Cursor GetDeclaration(void) const;

    CXTypeKind GetKind(void) const;

    bool IsConst(void) const;

    bool IsReference(void) const;

    bool IsArray(void) const;

private:
    CXType m_handle;
};
