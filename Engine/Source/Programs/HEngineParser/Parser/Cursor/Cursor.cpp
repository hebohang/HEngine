#include "Common/Precompiled.h"
#include "Meta/MetaUtils.h"

#include "Cursor.h"

Cursor::Cursor(const CXCursor& handle) : m_handle(handle) {}

CXCursorKind Cursor::getKind(void) const { return m_handle.kind; }

std::string Cursor::getSpelling(void) const
{
    std::string spelling;

    Utils::toString(clang_getCursorSpelling(m_handle), spelling);

    return spelling;
}

std::string Cursor::getDisplayName(void) const
{
    std::string display_name;

    Utils::toString(clang_getCursorDisplayName(m_handle), display_name);

    return display_name;
}

std::string Cursor::getSourceFile(void) const
{
    std::string filename;
    getSourceLocation(&filename, nullptr, nullptr);
    return filename;
}

unsigned Cursor::getSourceLine(void) const
{
    unsigned line = 0;
    getSourceLocation(nullptr, &line, nullptr);
    return line;
}

unsigned Cursor::getSourceColumn(void) const
{
    unsigned column = 0;
    getSourceLocation(nullptr, nullptr, &column);
    return column;
}

std::string Cursor::getUSR(void) const
{
    std::string usr;
    Utils::toString(clang_getCursorUSR(m_handle), usr);
    return usr;
}

CX_CXXAccessSpecifier Cursor::getAccessSpecifier(void) const { return clang_getCXXAccessSpecifier(m_handle); }

bool Cursor::isAbstract(void) const { return clang_CXXRecord_isAbstract(m_handle) != 0; }

bool Cursor::isDefaultConstructor(void) const { return clang_CXXConstructor_isDefaultConstructor(m_handle) != 0; }

bool Cursor::isDeleted(void) const
{
    CXTranslationUnit translationUnit = clang_Cursor_getTranslationUnit(m_handle);
    if (translationUnit == nullptr)
    {
        return false;
    }

    CXToken* tokens = nullptr;
    unsigned tokenCount = 0;
    clang_tokenize(translationUnit, clang_getCursorExtent(m_handle), &tokens, &tokenCount);
    bool deleted = false;
    for (unsigned index = 0; index < tokenCount; ++index)
    {
        std::string token;
        Utils::toString(clang_getTokenSpelling(translationUnit, tokens[index]), token);
        if (token == "delete")
        {
            deleted = true;
            break;
        }
    }
    if (tokens != nullptr)
    {
        clang_disposeTokens(translationUnit, tokens, tokenCount);
    }
    return deleted;
}

void Cursor::getSourceLocation(std::string* filename, unsigned* line, unsigned* column) const
{
    const auto location = clang_getCursorLocation(m_handle);
    CXFile     file     = nullptr;
    unsigned   sourceLine = 0;
    unsigned   sourceColumn = 0;
    unsigned   offset = 0;
    clang_getSpellingLocation(location, &file, &sourceLine, &sourceColumn, &offset);

    if (filename != nullptr)
    {
        filename->clear();
        if (file != nullptr)
        {
            Utils::toString(clang_getFileName(file), *filename);
        }
    }
    if (line != nullptr)
    {
        *line = sourceLine;
    }
    if (column != nullptr)
    {
        *column = sourceColumn;
    }
}

bool Cursor::isDefinition(void) const { return clang_isCursorDefinition(m_handle); }

CursorType Cursor::getType(void) const { return clang_getCursorType(m_handle); }

Cursor::List Cursor::getChildren(void) const
{
    List children;

    auto visitor = [](CXCursor cursor, CXCursor parent, CXClientData data) {
        auto container = static_cast<List*>(data);

        container->emplace_back(cursor);

        if (cursor.kind == CXCursor_LastPreprocessing)
            return CXChildVisit_Break;

        return CXChildVisit_Continue;
    };

    clang_visitChildren(m_handle, visitor, &children);

    return children;
}

void Cursor::visitChildren(Visitor visitor, void* data) { clang_visitChildren(m_handle, visitor, data); }
