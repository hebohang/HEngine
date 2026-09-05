#include "Common/Precompiled.h"

#include "TypeInfo.h"

TypeInfo::TypeInfo(const Cursor& cursor, const Namespace& current_namespace) :
    m_meta_data(cursor), m_enabled(m_meta_data.getFlag(NativeProperty::Enable)), m_root_cursor(cursor),
    m_namespace(current_namespace), m_source_line(cursor.getSourceLine()), m_source_column(cursor.getSourceColumn()),
    m_usr(cursor.getUSR())
{}

const MetaInfo& TypeInfo::getMetaData(void) const { return m_meta_data; }

std::string TypeInfo::getSourceFile(void) const { return m_root_cursor.getSourceFile(); }

unsigned TypeInfo::getSourceLine(void) const { return m_source_line; }

unsigned TypeInfo::getSourceColumn(void) const { return m_source_column; }

const std::string& TypeInfo::getUSR(void) const { return m_usr; }

Namespace TypeInfo::getCurrentNamespace() const { return m_namespace; }

Cursor& TypeInfo::getCurosr() { return m_root_cursor; }
