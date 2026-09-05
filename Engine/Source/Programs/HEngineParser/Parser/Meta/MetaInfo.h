#pragma once

#include "Cursor/Cursor.h"

class MetaInfo
{
public:
    struct Diagnostic
    {
        std::string source;
        unsigned line = 0;
        unsigned column = 0;
        std::string message;
    };

    MetaInfo(const Cursor& cursor);

    std::string getProperty(const std::string& key) const;

    bool getFlag(const std::string& key) const;

    const std::vector<Diagnostic>& getDiagnostics() const noexcept;

private:
    typedef std::pair<std::string, std::string> Property;

    std::unordered_map<std::string, std::string> m_properties;
    std::vector<Diagnostic> m_diagnostics;

private:
    std::vector<Property> extractProperties(const Cursor& cursor, std::string& error) const;
    void addDiagnostic(const Cursor& cursor, std::string message);
};
