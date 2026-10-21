#pragma once

#include "ScriptCore.h"

#include <string_view>

namespace HEngine
{
    // Versioned scene payload, bounded to 1 MiB / 1024 fields. Parsing rejects
    // unsupported tags instead of interpreting arbitrary language object graphs.
    std::string EncodeScriptFields(const ScriptFields& fields);
    ScriptFields DecodeScriptFields(std::string_view json);
    std::string RemapScriptEntityReferences(std::string_view json, const std::map<std::string, std::string>& ids, bool rejectExternal = false);
    ScriptAggregate ParseScriptAggregate(std::string_view json);
    ScriptValue PreserveUnknownScriptMembers(const ScriptValue& previous, ScriptValue current);
    std::vector<std::string> ScriptReferenceIds(const ScriptValue& value, bool resources);
}
