#include "Common/Precompiled.h"

#include "Cursor.h"
#include "CursorType.h"

CursorType::CursorType(const CXType& handle) : m_handle(handle) {}

std::string CursorType::GetDisplayName(void) const
{
    std::string display_name;

    Utils::toString(clang_getTypeSpelling(m_handle), display_name);

    return display_name;
}

int CursorType::GetArgumentCount(void) const { return clang_getNumArgTypes(m_handle); }

CursorType CursorType::GetArgument(unsigned index) const { return clang_getArgType(m_handle, index); }

CursorType CursorType::GetCanonicalType(void) const { return clang_getCanonicalType(m_handle); }

int CursorType::GetTemplateArgumentCount(void) const { return clang_Type_getNumTemplateArguments(m_handle); }

CursorType CursorType::GetTemplateArgument(unsigned index) const
{
    return clang_Type_getTemplateArgumentAsType(m_handle, index);
}

Cursor CursorType::GetDeclaration(void) const { return clang_getTypeDeclaration(m_handle); }

CXTypeKind CursorType::GetKind(void) const { return m_handle.kind; }

bool CursorType::IsConst(void) const { return clang_isConstQualifiedType(m_handle) ? true : false; }

bool CursorType::IsReference(void) const
{
    return m_handle.kind == CXType_LValueReference || m_handle.kind == CXType_RValueReference;
}

bool CursorType::IsArray(void) const
{
    return m_handle.kind == CXType_ConstantArray || m_handle.kind == CXType_IncompleteArray ||
           m_handle.kind == CXType_VariableArray || m_handle.kind == CXType_DependentSizedArray;
}
