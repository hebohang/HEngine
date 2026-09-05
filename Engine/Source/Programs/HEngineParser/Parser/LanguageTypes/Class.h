#pragma once

#include "TypeInfo.h"

#include "Field.h"

struct BaseClass
{
    BaseClass(const Cursor& cursor);

    std::string name;
    std::string typeSpelling;
    std::string canonicalType;
    std::string qualifiedName;
    std::string usr;
    std::string resolvedQualifiedName;
    std::string dependencyOutputIdentity;
    CX_CXXAccessSpecifier access;
    unsigned line;
    unsigned column;
};

class Class : public TypeInfo
{
    // to access m_qualifiedName
    friend class Field;
    friend class MetaParser;

public:
    Class(const Cursor& cursor, const Namespace& current_namespace);

    virtual bool shouldCompile(void) const;

    bool shouldCompileFilds(void) const;

    template<typename T>
    using SharedPtrVector = std::vector<std::shared_ptr<T>>;

    std::string getClassName(void);

    SharedPtrVector<BaseClass> m_base_classes;

public:
    std::string m_name;

    std::string m_qualified_name;

    SharedPtrVector<Field> m_fields;

    std::string m_display_name;

    std::string m_logical_source;
    std::string m_output_identity;
    std::string m_symbol_name;

    bool m_legacy_name_unique;

    bool m_is_abstract;
    bool m_is_struct;
    bool m_has_explicit_constructor;
    bool m_has_accessible_default_constructor;

    bool isAccessible(void) const;
};
