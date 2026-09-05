#pragma once

#include "TypeInfo.h"

class Class;

class Field : public TypeInfo
{

public:
    Field(const Cursor& cursor, const Namespace& current_namespace, Class* parent = nullptr);

    virtual ~Field(void) {}

    bool shouldCompile(void) const;

    std::string getCodegenTypeName(void) const;
    std::string getContainerElementCodegenTypeName(void) const;

public:
    bool m_is_const;
    bool m_is_reference;
    bool m_is_pointer;
    bool m_is_raw_array;
    bool m_is_container;

    CX_CXXAccessSpecifier m_access;

    Class* m_parent;

    std::string m_name;
    std::string m_display_name;
    std::string m_type;

    std::string m_type_spelling;
    std::string m_canonical_type;
    std::string m_type_usr;
    std::string m_type_qualified_name;

    std::string m_container_element_type;
    std::string m_container_element_canonical_type;
    std::string m_container_element_usr;
    std::string m_container_element_qualified_name;

    std::string m_dependency_output_identity;
    std::string m_container_dependency_output_identity;
    std::string m_registry_type_name;
    std::string m_container_registry_type_name;

    std::string m_default;
    std::string m_serialized_name;

    bool isAccessible(void) const;
};
