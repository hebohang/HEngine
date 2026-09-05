#pragma once
#include "Common/Namespace.h"

#include "Cursor/Cursor.h"

#include "Meta/MetaInfo.h"
#include "Parser/Parser.h"

class TypeInfo
{
public:
    TypeInfo(const Cursor& cursor, const Namespace& current_namespace);
    virtual ~TypeInfo(void) {}

    const MetaInfo& getMetaData(void) const;

    std::string getSourceFile(void) const;

    unsigned getSourceLine(void) const;

    unsigned getSourceColumn(void) const;

    const std::string& getUSR(void) const;

    Namespace getCurrentNamespace() const;

    Cursor& getCurosr();

protected:
    MetaInfo m_meta_data;

    bool m_enabled;

    std::string m_alias_cn;

    Namespace m_namespace;

    unsigned m_source_line;
    unsigned m_source_column;

    std::string m_usr;

private:
    // cursor that represents the root of this language type
    Cursor m_root_cursor;
};
