#include "Common/Precompiled.h"

#include "Class.h"

BaseClass::BaseClass(const Cursor& cursor) :
    name(Utils::getTypeNameWithoutNamespace(cursor.getType())), typeSpelling(cursor.getType().GetDisplayName()),
    canonicalType(cursor.getType().GetCanonicalType().GetDisplayName()),
    qualifiedName(cursor.getType().GetDeclaration().getType().GetDisplayName()),
    usr(cursor.getType().GetDeclaration().getUSR()), access(cursor.getAccessSpecifier()), line(cursor.getSourceLine()),
    column(cursor.getSourceColumn())
{}

Class::Class(const Cursor& cursor, const Namespace& current_namespace) :
    TypeInfo(cursor, current_namespace), m_name(cursor.getSpelling()),
    m_qualified_name(Utils::getQualifiedName(cursor, current_namespace)),
    m_display_name(Utils::getNameWithoutFirstM(m_name)), m_legacy_name_unique(false), m_is_abstract(cursor.isAbstract()),
    m_is_struct(cursor.getKind() == CXCursor_StructDecl),
    m_has_explicit_constructor(false), m_has_accessible_default_constructor(false)
{
    Utils::replaceAll(m_name, " ", "");
    Utils::replaceAll(m_name, "HEngine::", "");

    if (!shouldCompileFilds())
    {
        return;
    }

    for (auto& child : cursor.getChildren())
    {
        switch (child.getKind())
        {
            case CXCursor_CXXBaseSpecifier: {
                auto base_class = new BaseClass(child);

                m_base_classes.emplace_back(base_class);
            }
            break;
            // field
            case CXCursor_FieldDecl:
                m_fields.emplace_back(new Field(child, current_namespace, this));
                break;
            case CXCursor_Constructor:
                m_has_explicit_constructor = true;
                if (child.isDefaultConstructor() && child.getAccessSpecifier() == CX_CXXPublic &&
                    (child.getSourceLine() == 0 || !child.isDeleted()))
                {
                    m_has_accessible_default_constructor = true;
                }
                break;
            default:
                break;
        }
    }
}

bool Class::shouldCompile(void) const { return shouldCompileFilds(); }

bool Class::shouldCompileFilds(void) const
{
    return m_meta_data.getFlag(NativeProperty::All) || m_meta_data.getFlag(NativeProperty::Fields) ||
           m_meta_data.getFlag(NativeProperty::WhiteListFields);
}

std::string Class::getClassName(void) { return m_name; }

bool Class::isAccessible(void) const { return m_enabled; }
