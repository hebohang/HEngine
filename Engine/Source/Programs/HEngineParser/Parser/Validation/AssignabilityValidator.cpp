#include "Common/Precompiled.h"

#include "LanguageTypes/Class.h"
#include "Meta/MetaUtils.h"
#include "Validation/AssignabilityValidator.h"

namespace
{
    std::string escapeCppString(std::string_view value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char character : value)
        {
            switch (character)
            {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += character; break;
            }
        }
        return escaped;
    }

    std::string normalizeSlashes(std::string value)
    {
        std::replace(value.begin(), value.end(), '\\', '/');
        return value;
    }
}

bool ValidateFieldAssignability(
    CXIndex index,
    const fs::path& inputHeader,
    const fs::path& scratchFile,
    const std::vector<std::string>& clangArguments,
    const std::vector<std::shared_ptr<Class>>& classes,
    const AssignabilityDiagnosticReporter& report)
{
    std::ostringstream source;
    source << "// Generated staging-only assignability validation; do not edit.\n"
           << "#include \""
           << escapeCppString(normalizeSlashes(inputHeader.generic_string()))
           << "\"\n#include <type_traits>\n#include <utility>\n";

    size_t checkCount = 0;
    for (const auto& classInfo : classes)
    {
        source << "#line " << classInfo->getSourceLine() << " \""
               << escapeCppString(normalizeSlashes(classInfo->getSourceFile())) << "\"\n"
               << "static_assert(std::is_default_constructible_v<"
               << classInfo->m_qualified_name << ">, "
               << "\"HE_CODEGEN_NON_DEFAULT_CONSTRUCTIBLE: "
               << escapeCppString(classInfo->m_qualified_name) << "\");\n";
        ++checkCount;
        for (const auto& field : classInfo->m_fields)
        {
            if (!field->shouldCompile())
            {
                continue;
            }
            source << "#line " << field->getSourceLine() << " \""
                   << escapeCppString(normalizeSlashes(field->getSourceFile())) << "\"\n"
                   << "static_assert(std::is_assignable_v<"
                   << "decltype((std::declval<" << classInfo->m_qualified_name
                   << "&>()." << field->m_name << ")), "
                   << "std::remove_reference_t<decltype((std::declval<"
                   << classInfo->m_qualified_name << "&>()." << field->m_name
                   << "))>&>, "
                   << "\"HE_CODEGEN_NON_ASSIGNABLE: "
                   << escapeCppString(classInfo->m_qualified_name + "::" + field->m_name)
                   << "\");\n";
            ++checkCount;
            if (field->m_is_container)
            {
                source << "#line " << field->getSourceLine() << " \""
                       << escapeCppString(normalizeSlashes(field->getSourceFile())) << "\"\n"
                       << "static_assert(std::is_assignable_v<"
                       << "decltype((std::declval<" << classInfo->m_qualified_name
                       << "&>()." << field->m_name << "[0])), "
                       << "typename std::remove_reference_t<decltype((std::declval<"
                       << classInfo->m_qualified_name << "&>()." << field->m_name
                       << "))>::value_type&>, "
                       << "\"HE_CODEGEN_NON_ASSIGNABLE: "
                       << escapeCppString(classInfo->m_qualified_name + "::" + field->m_name + "[]")
                       << "\");\n";
                ++checkCount;
            }
        }
    }

    if (checkCount == 0)
    {
        return true;
    }

    std::string error;
    if (!Utils::saveFile(source.str(), scratchFile, error))
    {
        report("error", scratchFile.generic_string(), 0, 0, error);
        return false;
    }

    std::vector<const char*> argumentPointers;
    argumentPointers.reserve(clangArguments.size());
    for (const auto& argument : clangArguments)
    {
        argumentPointers.emplace_back(argument.c_str());
    }

    CXTranslationUnit translationUnit = nullptr;
    const auto translationUnitOptions = static_cast<unsigned>(
        CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing);
    const CXErrorCode parseError = clang_parseTranslationUnit2(
        index,
        scratchFile.string().c_str(),
        argumentPointers.data(),
        static_cast<int>(argumentPointers.size()),
        nullptr,
        0,
        translationUnitOptions,
        &translationUnit);
    if (parseError != CXError_Success || translationUnit == nullptr)
    {
        report("fatal", scratchFile.generic_string(), 0, 0,
               "clang_parseTranslationUnit2 failed during reflected field assignability validation");
        return false;
    }

    bool valid = true;
    const unsigned diagnosticCount = clang_getNumDiagnostics(translationUnit);
    for (unsigned index = 0; index < diagnosticCount; ++index)
    {
        CXDiagnostic diagnostic = clang_getDiagnostic(translationUnit, index);
        const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        if (severity != CXDiagnostic_Error && severity != CXDiagnostic_Fatal)
        {
            clang_disposeDiagnostic(diagnostic);
            continue;
        }

        valid = false;
        unsigned line = 0;
        unsigned column = 0;
        std::string diagnosticSource = scratchFile.generic_string();
        CXString presumedFilename;
        clang_getPresumedLocation(
            clang_getDiagnosticLocation(diagnostic), &presumedFilename, &line, &column);
        std::string presumedSource;
        Utils::toString(presumedFilename, presumedSource);
        if (!presumedSource.empty())
        {
            diagnosticSource = presumedSource;
        }
        std::string message;
        Utils::toString(clang_getDiagnosticSpelling(diagnostic), message);
        const size_t marker = message.find("HE_CODEGEN_NON_ASSIGNABLE:");
        if (marker != std::string::npos)
        {
            message = "reflected field is not assignable by generated setter: " +
                message.substr(marker + std::string("HE_CODEGEN_NON_ASSIGNABLE:").size());
        }
        const size_t constructorMarker = message.find("HE_CODEGEN_NON_DEFAULT_CONSTRUCTIBLE:");
        if (constructorMarker != std::string::npos)
        {
            message = "reflected class is not default constructible: " +
                message.substr(constructorMarker +
                               std::string("HE_CODEGEN_NON_DEFAULT_CONSTRUCTIBLE:").size());
        }
        report(severity == CXDiagnostic_Fatal ? "fatal" : "error",
               diagnosticSource, line, column, message);
        clang_disposeDiagnostic(diagnostic);
    }

    clang_disposeTranslationUnit(translationUnit);
    return valid;
}
