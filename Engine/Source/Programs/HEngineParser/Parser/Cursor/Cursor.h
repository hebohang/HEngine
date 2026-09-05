#pragma once

#include "CursorType.h"

class Cursor
{
public:
    typedef std::vector<Cursor> List;
    
    typedef CXCursorVisitor Visitor;

    Cursor(const CXCursor& handle);

    CXCursorKind getKind(void) const;

    std::string getSpelling(void) const;
    std::string getDisplayName(void) const;

    std::string getSourceFile(void) const;

    unsigned getSourceLine(void) const;
    unsigned getSourceColumn(void) const;
    std::string getUSR(void) const;

    CX_CXXAccessSpecifier getAccessSpecifier(void) const;

    bool isAbstract(void) const;
    bool isDefaultConstructor(void) const;
    bool isDeleted(void) const;

    bool isDefinition(void) const;

    CursorType getType(void) const;

    List getChildren(void) const;
    void visitChildren(Visitor visitor, void* data = nullptr);

private:
    void getSourceLocation(std::string* filename, unsigned* line, unsigned* column) const;

    CXCursor m_handle;
};
