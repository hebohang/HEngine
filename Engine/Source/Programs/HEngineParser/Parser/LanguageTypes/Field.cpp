#include "Common/Precompiled.h"

#include "Class.h"
#include "Field.h"

Field::Field(const Cursor& cursor, const Namespace& current_namespace, Class* parent) :
    TypeInfo(cursor, current_namespace), m_is_const(cursor.getType().IsConst()),
    m_is_reference(cursor.getType().IsReference()), m_is_pointer(cursor.getType().GetKind() == CXType_Pointer),
    m_is_raw_array(cursor.getType().IsArray()), m_is_container(false),
    m_access(cursor.getAccessSpecifier()), m_parent(parent),
    m_name(cursor.getSpelling()), m_display_name(Utils::getNameWithoutFirstM(m_name)),
    m_type(Utils::getTypeNameWithoutNamespace(cursor.getType())), m_type_spelling(cursor.getType().GetDisplayName()),
    m_canonical_type(cursor.getType().GetCanonicalType().GetDisplayName()),
    m_type_usr(cursor.getType().GetDeclaration().getUSR()),
    m_type_qualified_name(cursor.getType().GetDeclaration().getType().GetDisplayName())
{
    const auto type = cursor.getType();
    const int templateArgumentCount = type.GetTemplateArgumentCount();
    const bool isVector = m_type_spelling.starts_with("std::vector<") ||
                          m_canonical_type.starts_with("std::vector<");
    if (isVector && templateArgumentCount > 0)
    {
        const auto elementType = type.GetTemplateArgument(0);
        m_is_container = elementType.GetKind() != CXType_Invalid;
        if (m_is_container)
        {
            m_container_element_type = elementType.GetDisplayName();
            m_container_element_canonical_type = elementType.GetCanonicalType().GetDisplayName();
            const auto declaration = elementType.GetDeclaration();
            m_container_element_usr = declaration.getUSR();
            m_container_element_qualified_name = declaration.getType().GetDisplayName();
        }
    }

    Utils::replaceAll(m_type, "HEngine::", "");

    auto ret_string = Utils::getStringWithoutQuot(m_meta_data.getProperty("default"));
    m_default       = ret_string;

    m_serialized_name = Utils::getStringWithoutQuot(m_meta_data.getProperty("serializedName"));
    if (m_serialized_name.empty())
    {
        m_serialized_name = Utils::getStringWithoutQuot(m_meta_data.getProperty("SerializeName"));
    }
    if (m_serialized_name.empty())
    {
        m_serialized_name = m_name;
    }
}

bool Field::shouldCompile(void) const { return isAccessible(); }

std::string Field::getContainerElementCodegenTypeName(void) const
{
    if (!m_container_registry_type_name.empty())
    {
        return m_container_registry_type_name;
    }
    const std::string qualifiedSuffix = "::" + m_container_element_type;
    if (!m_container_element_qualified_name.empty() &&
        (m_container_element_qualified_name == m_container_element_type ||
         m_container_element_qualified_name.ends_with(qualifiedSuffix)))
    {
        return m_container_element_qualified_name;
    }
    return m_container_element_type;
}

std::string Field::getCodegenTypeName(void) const
{
    if (m_is_container)
    {
        std::string containerType = m_type_spelling;
        const std::string elementType = getContainerElementCodegenTypeName();
        if (!m_container_element_type.empty() && elementType != m_container_element_type)
        {
            size_t position = 0;
            while ((position = containerType.find(m_container_element_type, position)) != std::string::npos)
            {
                containerType.replace(position, m_container_element_type.size(), elementType);
                position += elementType.size();
            }
        }
        return containerType;
    }
    const std::string qualifiedSuffix = "::" + m_type_spelling;
    if (!m_type_qualified_name.empty() &&
        (m_type_qualified_name == m_type_spelling || m_type_qualified_name.ends_with(qualifiedSuffix)))
    {
        return m_type_qualified_name;
    }
    return m_type_spelling;
}

bool Field::isAccessible(void) const
{
    return ((m_parent->m_meta_data.getFlag(NativeProperty::Fields) ||
             m_parent->m_meta_data.getFlag(NativeProperty::All)) &&
            !m_meta_data.getFlag(NativeProperty::Disable)) ||
           (m_parent->m_meta_data.getFlag(NativeProperty::WhiteListFields) &&
            m_meta_data.getFlag(NativeProperty::Enable));
}
