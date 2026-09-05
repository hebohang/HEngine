#include "Common/Precompiled.h"

#include "Generator/ReflectionGenerator.h"
#include "Generator/SerializerGenerator.h"
#include "LanguageTypes/Class.h"
#include "Parser.h"
#include "TemplateManager/TemplateManager.h"
#include "Validation/AssignabilityValidator.h"

namespace
{
    constexpr size_t MaximumDiagnostics = 20;
    constexpr const char* GeneratorVersion = HE_CODEGEN_GENERATOR_VERSION;

    std::string trimCopy(std::string value)
    {
        Utils::trim(value, " \t\r\n");
        return value;
    }

    std::string lowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    std::string normalizeSlashes(std::string value)
    {
        std::replace(value.begin(), value.end(), '\\', '/');
        return value;
    }

    fs::path normalizedAbsolute(const fs::path& path)
    {
        std::error_code error;
        fs::path absolutePath = fs::absolute(path, error);
        if (error)
        {
            return path.lexically_normal();
        }
        fs::path canonicalPath = fs::weakly_canonical(absolutePath, error);
        return error ? absolutePath.lexically_normal() : canonicalPath.lexically_normal();
    }

    bool pathIsWithin(const fs::path& candidate, const fs::path& root)
    {
        std::string candidateText = normalizeSlashes(normalizedAbsolute(candidate).generic_string());
        std::string rootText = normalizeSlashes(normalizedAbsolute(root).generic_string());
#if defined(_WIN32)
        candidateText = lowerCopy(std::move(candidateText));
        rootText = lowerCopy(std::move(rootText));
#endif
        while (rootText.size() > 1 && rootText.back() == '/')
        {
            rootText.pop_back();
        }
        return candidateText == rootText ||
               (candidateText.size() > rootText.size() && candidateText.starts_with(rootText) &&
                candidateText[rootText.size()] == '/');
    }

    bool isSafeIdentityPrefix(const std::string& value)
    {
        const fs::path path(value);
        if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        {
            return false;
        }
        for (const auto& part : path)
        {
            if (part == "..")
            {
                return false;
            }
        }
        return true;
    }

    std::string jsonEscape(const std::string& value)
    {
        std::ostringstream output;
        for (const unsigned char character : value)
        {
            switch (character)
            {
                case '"': output << "\\\""; break;
                case '\\': output << "\\\\"; break;
                case '\b': output << "\\b"; break;
                case '\f': output << "\\f"; break;
                case '\n': output << "\\n"; break;
                case '\r': output << "\\r"; break;
                case '\t': output << "\\t"; break;
                default:
                    if (character < 0x20)
                    {
                        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                               << static_cast<unsigned>(character) << std::dec;
                    }
                    else
                    {
                        output << static_cast<char>(character);
                    }
                    break;
            }
        }
        return output.str();
    }

    std::string sanitizeDiagnostic(std::string message)
    {
        std::replace(message.begin(), message.end(), '|', '/');
        std::replace(message.begin(), message.end(), '\r', ' ');
        std::replace(message.begin(), message.end(), '\n', ' ');
        constexpr size_t MaximumMessageLength = 1000;
        if (message.size() > MaximumMessageLength)
        {
            message.resize(MaximumMessageLength);
            message += "...";
        }
        return message;
    }

    std::string stripTypeDecorations(std::string value)
    {
        value = trimCopy(std::move(value));
        for (const std::string prefix : {"const ", "class ", "struct ", "enum "})
        {
            while (value.starts_with(prefix))
            {
                value.erase(0, prefix.size());
                value = trimCopy(std::move(value));
            }
        }
        while (!value.empty() && (value.back() == '&' || value.back() == '*' || std::isspace(value.back()) != 0))
        {
            value.pop_back();
        }
        return trimCopy(std::move(value));
    }

    uint64_t fnv1a(const std::string& value)
    {
        uint64_t result = 1469598103934665603ULL;
        for (const unsigned char character : value)
        {
            result ^= character;
            result *= 1099511628211ULL;
        }
        return result;
    }

    std::string makeSymbol(const std::string& value)
    {
        std::string result;
        result.reserve(value.size() + 18);
        for (const unsigned char character : value)
        {
            result.push_back(std::isalnum(character) != 0 ? static_cast<char>(character) : '_');
        }
        std::ostringstream suffix;
        suffix << '_' << std::hex << std::setw(16) << std::setfill('0') << fnv1a(value);
        result += suffix.str();
        return result;
    }

    std::string clangErrorName(CXErrorCode error)
    {
        switch (error)
        {
            case CXError_Success: return "success";
            case CXError_Failure: return "failure";
            case CXError_Crashed: return "crashed";
            case CXError_InvalidArguments: return "invalid arguments";
            case CXError_ASTReadError: return "AST read error";
            default: return "unknown error";
        }
    }

    std::string withReplacementExtension(const std::string& path, const std::string& extension)
    {
        fs::path result(path);
        result.replace_extension(extension);
        return result.generic_string();
    }
}

void MetaParser::prepare(void) {}

MetaParser::MetaParser(ParserOptions options) : m_options(std::move(options)) {}

MetaParser::MetaParser(const std::string& projectInputFile,
                       const std::string& includeFilePath,
                       const std::string& includePath,
                       const std::string& includeSystemPath,
                       const std::string& moduleName,
                       bool isShowErrors)
{
    m_options.inputList = projectInputFile;
    m_options.inputHeader = includeFilePath;
    m_options.moduleName = moduleName;
    m_options.legacyMode = true;
    m_options.quiet = !isShowErrors;

    const auto includePaths = Utils::split(includePath, ";");
    for (auto path : includePaths)
    {
        path = trimCopy(std::move(path));
        if (path.empty())
        {
            continue;
        }
        m_options.includePaths.emplace_back(path);
        m_options.sourceRoots.push_back({normalizedAbsolute(path), {}});
    }
    if (!m_options.includePaths.empty())
    {
        m_options.outputRoot = m_options.includePaths.front();
    }
    if (!includeSystemPath.empty() && includeSystemPath != "*")
    {
        m_options.includePaths.emplace_back(includeSystemPath);
    }

    m_options.indexFile = m_options.outputRoot / ".he-codegen-index.json";
}

MetaParser::~MetaParser(void)
{
    if (m_translation_unit != nullptr)
    {
        clang_disposeTranslationUnit(m_translation_unit);
    }
    if (m_index != nullptr)
    {
        clang_disposeIndex(m_index);
    }
}

int MetaParser::parse(void)
{
    const auto runPhase = [](const char* name, const std::function<bool()>& phase) {
        const auto start = std::chrono::steady_clock::now();
        const bool result = phase();
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        std::cerr << "HE_CODEGEN_PHASE|" << name << '|' << duration.count() << "ms\n";
        return result;
    };
    if (!runPhase("context", [this] { return loadContext(); }) ||
        !runPhase("options", [this] { return validateOptions(); }) ||
        !runPhase("generators", [this] { return initializeGenerators(); }) ||
        !runPhase("umbrella", [this] { return parseProject(); }) ||
        !runPhase("clang-tu", [this] { return parseTranslationUnit(); }) ||
        !runPhase("clang-diagnostics", [this] { return collectClangDiagnostics(); }) ||
        !runPhase("ir-validation", [this] { return buildRegistryAndValidate(); }) ||
        !runPhase("type-traits", [this] { return validateTypeTraits(); }))
    {
        emitDiagnostics();
        return 1;
    }
    return 0;
}

int MetaParser::generateFiles(void)
{
    std::string error;
    auto phaseStart = std::chrono::steady_clock::now();
    for (const auto& [identity, schema] : m_schema_modules)
    {
        (void)identity;
        for (const auto& generator : m_generators)
        {
            if (!generator->generate(schema, error))
            {
                report("error", schema.logicalSource, 0, 0, error);
                emitDiagnostics();
                return 1;
            }
        }
    }
    std::cerr << "HE_CODEGEN_PHASE|render-files|"
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - phaseStart).count()
              << "ms\n";

    phaseStart = std::chrono::steady_clock::now();
    for (const auto& generator : m_generators)
    {
        if (!generator->finish(error))
        {
            report("error", m_options.moduleName, 0, 0, error);
            emitDiagnostics();
            return 1;
        }
    }
    std::cerr << "HE_CODEGEN_PHASE|render-aggregates|"
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - phaseStart).count()
              << "ms\n";

    std::vector<std::string> outputs;
    for (const auto& generator : m_generators)
    {
        outputs.insert(outputs.end(), generator->getOutputs().begin(), generator->getOutputs().end());
    }
    std::sort(outputs.begin(), outputs.end());
    if (std::adjacent_find(outputs.begin(), outputs.end()) != outputs.end())
    {
        report("error", m_options.moduleName, 0, 0, "generated output collision between generators");
        emitDiagnostics();
        return 1;
    }

    phaseStart = std::chrono::steady_clock::now();
    if (m_options.validateGenerated && !validateGeneratedOutputs())
    {
        emitDiagnostics();
        return 1;
    }
    std::cerr << "HE_CODEGEN_PHASE|generated-validation|"
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - phaseStart).count()
              << "ms\n";

    phaseStart = std::chrono::steady_clock::now();
    if (!writeIndex(outputs))
    {
        emitDiagnostics();
        return 1;
    }
    std::cerr << "HE_CODEGEN_PHASE|index|"
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - phaseStart).count()
              << "ms\n";
    return 0;
}

bool MetaParser::validateGeneratedOutputs(void)
{
    const fs::path validationFile = m_options.outputRoot / ".he-codegen" / "generated-validation.cpp";
    std::string error;
    if (!Utils::saveFile(
            "// Staging-only generated output validation.\n"
            "#include \"_generated/serializer/all_serializer.hpp\"\n"
            "#include \"_generated/reflection/all_reflection.h\"\n",
            validationFile,
            error))
    {
        report("error", validationFile.generic_string(), 0, 0, error);
        return false;
    }

    std::vector<std::string> argumentStorage = m_argument_storage;
    argumentStorage.insert(argumentStorage.begin() + std::min<size_t>(4, argumentStorage.size()),
                           "-I" + m_options.outputRoot.string());
    std::vector<const char*> arguments;
    arguments.reserve(argumentStorage.size());
    for (const auto& argument : argumentStorage)
    {
        arguments.emplace_back(argument.c_str());
    }

    CXTranslationUnit validationUnit = nullptr;
    const CXErrorCode parseError = clang_parseTranslationUnit2(
        m_index,
        validationFile.string().c_str(),
        arguments.data(),
        static_cast<int>(arguments.size()),
        nullptr,
        0,
        static_cast<unsigned>(CXTranslationUnit_KeepGoing),
        &validationUnit);
    if (parseError != CXError_Success || validationUnit == nullptr)
    {
        report("fatal", validationFile.generic_string(), 0, 0,
               "generated validation translation unit creation failed: " + clangErrorName(parseError));
        if (validationUnit != nullptr)
        {
            clang_disposeTranslationUnit(validationUnit);
        }
        return false;
    }

    bool valid = true;
    const unsigned diagnosticCount = clang_getNumDiagnostics(validationUnit);
    for (unsigned index = 0; index < diagnosticCount; ++index)
    {
        CXDiagnostic diagnostic = clang_getDiagnostic(validationUnit, index);
        const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        if (severity == CXDiagnostic_Error || severity == CXDiagnostic_Fatal)
        {
            valid = false;
            if (m_diagnostics.size() < MaximumDiagnostics)
            {
                const CXSourceLocation location = clang_getDiagnosticLocation(diagnostic);
                CXFile file = nullptr;
                unsigned line = 0;
                unsigned column = 0;
                unsigned offset = 0;
                clang_getSpellingLocation(location, &file, &line, &column, &offset);
                std::string source = validationFile.generic_string();
                if (file != nullptr)
                {
                    Utils::toString(clang_getFileName(file), source);
                }
                std::string message;
                Utils::toString(clang_getDiagnosticSpelling(diagnostic), message);
                report(severity == CXDiagnostic_Fatal ? "fatal" : "error", source, line, column, message);
            }
        }
        clang_disposeDiagnostic(diagnostic);
    }
    clang_disposeTranslationUnit(validationUnit);
    return valid;
}

const std::string& MetaParser::getClangVersion(void) const { return m_clang_version; }

bool MetaParser::loadContext(void)
{
    if (m_context_loaded)
    {
        return true;
    }
    m_context_loaded = true;
    if (m_options.contextFile.empty())
    {
        return true;
    }

    std::string context;
    std::string error;
    if (!Utils::loadFile(m_options.contextFile, context, error))
    {
        report("error", m_options.contextFile.generic_string(), 0, 0, error);
        return false;
    }

    std::istringstream lines(context);
    std::string line;
    unsigned lineNumber = 0;
    while (std::getline(lines, line))
    {
        ++lineNumber;
        line = trimCopy(std::move(line));
        if (line.empty() || line.starts_with('#'))
        {
            continue;
        }

        size_t separator = line.find('=');
        if (separator == std::string::npos)
        {
            separator = line.find('|');
        }
        if (separator == std::string::npos)
        {
            continue;
        }

        const std::string key = trimCopy(line.substr(0, separator));
        std::string value = trimCopy(line.substr(separator + 1));
        const std::string loweredKey = lowerCopy(key);
        if (loweredKey != "include" && loweredKey != "define" && loweredKey != "arg" &&
            loweredKey != "sourceroot")
        {
            continue;
        }
        if (value.empty())
        {
            report("error", m_options.contextFile.generic_string(), lineNumber, 1,
                   "empty value for context key " + key);
            return false;
        }

        if (loweredKey == "include")
        {
            m_options.includePaths.emplace_back(std::move(value));
        }
        else if (loweredKey == "define")
        {
            m_options.defines.emplace_back(std::move(value));
        }
        else if (loweredKey == "arg")
        {
            m_options.clangArguments.emplace_back(std::move(value));
        }
        else
        {
            std::string rootPath = value;
            std::string identityPrefix;
            const size_t prefixSeparator = value.find('|');
            if (prefixSeparator != std::string::npos)
            {
                rootPath = trimCopy(value.substr(0, prefixSeparator));
                identityPrefix = normalizeSlashes(trimCopy(value.substr(prefixSeparator + 1)));
                while (!identityPrefix.empty() && identityPrefix.front() == '/')
                {
                    identityPrefix.erase(identityPrefix.begin());
                }
                while (!identityPrefix.empty() && identityPrefix.back() == '/')
                {
                    identityPrefix.pop_back();
                }
            }
            if (rootPath.empty() || !isSafeIdentityPrefix(identityPrefix))
            {
                report("error", m_options.contextFile.generic_string(), lineNumber, 1,
                       "invalid sourceRoot entry");
                return false;
            }
            const fs::path absoluteRoot = normalizedAbsolute(rootPath);
            m_options.sourceRoots.push_back({absoluteRoot, identityPrefix});
            m_options.includePaths.emplace_back(absoluteRoot.string());
        }
    }
    return true;
}

bool MetaParser::validateOptions(void)
{
    const auto requirePath = [this](const fs::path& path, const std::string& name) {
        if (path.empty())
        {
            report("error", m_options.moduleName, 0, 0, "missing or empty " + name);
            return false;
        }
        return true;
    };

    bool valid = requirePath(m_options.inputList, "input list") &&
                 requirePath(m_options.inputHeader, "input header") &&
                 requirePath(m_options.outputRoot, "output root") &&
                 requirePath(m_options.indexFile, "index path");
    if (m_options.moduleName.empty())
    {
        report("error", "<arguments>", 0, 0, "missing or empty module name");
        valid = false;
    }

    if (!valid)
    {
        return false;
    }

    m_options.outputRoot = normalizedAbsolute(m_options.outputRoot);
    m_options.inputList = normalizedAbsolute(m_options.inputList);
    m_options.inputHeader = normalizedAbsolute(m_options.inputHeader);
    m_options.indexFile = normalizedAbsolute(m_options.indexFile);
    if (!m_options.contextFile.empty())
    {
        m_options.contextFile = normalizedAbsolute(m_options.contextFile);
    }

    if (!m_options.legacyMode &&
        (!pathIsWithin(m_options.inputHeader, m_options.outputRoot) ||
         !pathIsWithin(m_options.indexFile, m_options.outputRoot)))
    {
        report("error", "<arguments>", 0, 0, "input header and index must be inside output-root staging");
        return false;
    }

    if (!fs::is_regular_file(m_options.inputList))
    {
        report("error", m_options.inputList.generic_string(), 0, 0, "input list does not exist or is not a file");
        return false;
    }
    if (!m_options.contextFile.empty() && !fs::is_regular_file(m_options.contextFile))
    {
        report("error", m_options.contextFile.generic_string(), 0, 0, "context does not exist or is not a file");
        return false;
    }

    if (m_options.templatesDirectory.empty())
    {
        const fs::path currentPath(__FILE__);
        m_options.templatesDirectory = currentPath.parent_path().parent_path().parent_path() / "Template";
    }
    m_options.templatesDirectory = normalizedAbsolute(m_options.templatesDirectory);
    if (!fs::is_directory(m_options.templatesDirectory))
    {
        report("error", m_options.templatesDirectory.generic_string(), 0, 0,
               "templates path does not exist or is not a directory");
        return false;
    }

    std::error_code error;
    if (!fs::exists(m_options.outputRoot, error) && !fs::create_directories(m_options.outputRoot, error))
    {
        report("error", m_options.outputRoot.generic_string(), 0, 0,
               "could not create output-root staging: " + error.message());
        return false;
    }
    if (error || !fs::is_directory(m_options.outputRoot))
    {
        report("error", m_options.outputRoot.generic_string(), 0, 0, "output-root staging is not a directory");
        return false;
    }

    for (auto& sourceRoot : m_options.sourceRoots)
    {
        sourceRoot.absolutePath = normalizedAbsolute(sourceRoot.absolutePath);
    }
    std::sort(m_options.sourceRoots.begin(), m_options.sourceRoots.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.absolutePath.generic_string().size() != rhs.absolutePath.generic_string().size())
        {
            return lhs.absolutePath.generic_string().size() > rhs.absolutePath.generic_string().size();
        }
        return lhs.absolutePath.generic_string() < rhs.absolutePath.generic_string();
    });
    return true;
}

bool MetaParser::initializeGenerators(void)
{
    TemplateManager::getInstance()->configure(m_options.templatesDirectory);
    m_generators.emplace_back(
        std::make_unique<Generator::SerializerGenerator>(m_options.outputRoot, m_options.moduleName));
    m_generators.emplace_back(
        std::make_unique<Generator::ReflectionGenerator>(m_options.outputRoot, m_options.moduleName));

    std::string error;
    for (const auto& generator : m_generators)
    {
        if (!generator->initialize(error))
        {
            report("error", m_options.templatesDirectory.generic_string(), 0, 0, error);
            return false;
        }
    }
    return true;
}

bool MetaParser::parseProject(void)
{
    std::string inputList;
    std::string error;
    if (!Utils::loadFile(m_options.inputList, inputList, error))
    {
        report("error", m_options.inputList.generic_string(), 0, 0, error);
        return false;
    }

    std::vector<std::string> headers;
    for (auto header : Utils::split(inputList, ";"))
    {
        header = normalizeSlashes(trimCopy(std::move(header)));
        if (!header.empty())
        {
            headers.emplace_back(std::move(header));
        }
    }
    std::sort(headers.begin(), headers.end());
    headers.erase(std::unique(headers.begin(), headers.end()), headers.end());
    if (headers.empty())
    {
        report("error", m_options.inputList.generic_string(), 0, 0, "input list contains no headers");
        return false;
    }

    m_candidate_logical_source_counts.clear();
    m_candidate_source_files.clear();
    for (const auto& header : headers)
    {
        fs::path candidate(header);
        if (candidate.is_relative())
        {
            for (const auto& includePath : m_options.includePaths)
            {
                const fs::path includeCandidate = fs::path(includePath) / candidate;
                if (fs::exists(includeCandidate))
                {
                    candidate = includeCandidate;
                    break;
                }
            }
        }
        if (fs::exists(candidate))
        {
            const std::string candidatePath = lowerCopy(normalizeSlashes(normalizedAbsolute(candidate).generic_string()));
            m_candidate_source_files.emplace(candidatePath);
            const SourceIdentity sourceIdentity = identifySource(candidate);
            ++m_candidate_logical_source_counts[lowerCopy(sourceIdentity.logicalPath)];
        }
    }

    std::ostringstream umbrella;
    umbrella << "// Generated parser input; staging-only.\n#pragma once\n";
    for (const auto& header : headers)
    {
        umbrella << "#include \"" << header << "\"\n";
    }
    if (!Utils::saveFile(umbrella.str(), m_options.inputHeader, error))
    {
        report("error", m_options.inputHeader.generic_string(), 0, 0, error);
        return false;
    }
    return true;
}

bool MetaParser::parseTranslationUnit(void)
{
    Utils::toString(clang_getClangVersion(), m_clang_version);
#if defined(_WIN32)
    const size_t versionPosition = m_clang_version.find("version ");
    int runtimeMajor = 0;
    if (versionPosition != std::string::npos)
    {
        std::istringstream versionStream(m_clang_version.substr(versionPosition + 8));
        versionStream >> runtimeMajor;
    }
    if (runtimeMajor < 20)
    {
        report("fatal", m_options.inputHeader.generic_string(), 0, 0,
               "runtime libclang >= 20 is required; loaded " + m_clang_version);
        return false;
    }
#if defined(HE_CODEGEN_LIBCLANG_CONFIGURED_VERSION)
    if (m_clang_version.find(HE_CODEGEN_LIBCLANG_CONFIGURED_VERSION) == std::string::npos)
    {
        report("fatal", m_options.inputHeader.generic_string(), 0, 0,
               "runtime libclang does not match configured version "
               HE_CODEGEN_LIBCLANG_CONFIGURED_VERSION ": " + m_clang_version);
        return false;
    }
#endif
#endif

    m_argument_storage = {"-x", "c++", "-std=c++20", "-D__REFLECTION_PARSER__",
                          "-ferror-limit=20"};
    for (const auto& includePath : m_options.includePaths)
    {
        if (trimCopy(includePath).empty())
        {
            report("error", m_options.contextFile.generic_string(), 0, 0, "empty include path in parser context");
            return false;
        }
        m_argument_storage.emplace_back("-I" + includePath);
    }
    for (const auto& define : m_options.defines)
    {
        if (trimCopy(define).empty())
        {
            report("error", m_options.contextFile.generic_string(), 0, 0, "empty define in parser context");
            return false;
        }
        m_argument_storage.emplace_back("-D" + define);
    }
    m_argument_storage.insert(
        m_argument_storage.end(), m_options.clangArguments.begin(), m_options.clangArguments.end());

    m_arguments.clear();
    m_arguments.reserve(m_argument_storage.size());
    for (const auto& argument : m_argument_storage)
    {
        m_arguments.emplace_back(argument.c_str());
    }

    m_index = clang_createIndex(1, 0);
    if (m_index == nullptr)
    {
        report("fatal", m_options.inputHeader.generic_string(), 0, 0, "clang_createIndex failed");
        return false;
    }

    const auto translationUnitOptions = static_cast<unsigned>(CXTranslationUnit_SkipFunctionBodies |
                                                               CXTranslationUnit_KeepGoing);
    const CXErrorCode parseError = clang_parseTranslationUnit2(m_index,
                                                                m_options.inputHeader.string().c_str(),
                                                                m_arguments.data(),
                                                                static_cast<int>(m_arguments.size()),
                                                                nullptr,
                                                                0,
                                                                translationUnitOptions,
                                                                &m_translation_unit);
    if (parseError != CXError_Success || m_translation_unit == nullptr)
    {
        report("fatal", m_options.inputHeader.generic_string(), 0, 0,
               "clang_parseTranslationUnit2 failed: " + clangErrorName(parseError));
        return false;
    }
    return true;
}

bool MetaParser::collectClangDiagnostics(void)
{
    bool hasErrors = false;
    const unsigned diagnosticCount = clang_getNumDiagnostics(m_translation_unit);
    for (unsigned index = 0; index < diagnosticCount; ++index)
    {
        CXDiagnostic diagnostic = clang_getDiagnostic(m_translation_unit, index);
        const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        if (severity != CXDiagnostic_Error && severity != CXDiagnostic_Fatal)
        {
            clang_disposeDiagnostic(diagnostic);
            continue;
        }

        hasErrors = true;
        if (m_diagnostics.size() < MaximumDiagnostics)
        {
            const CXSourceLocation location = clang_getDiagnosticLocation(diagnostic);
            CXFile file = nullptr;
            unsigned line = 0;
            unsigned column = 0;
            unsigned offset = 0;
            clang_getSpellingLocation(location, &file, &line, &column, &offset);

            std::string source = m_options.inputHeader.generic_string();
            if (file != nullptr)
            {
                Utils::toString(clang_getFileName(file), source);
            }
            std::string message;
            Utils::toString(clang_getDiagnosticSpelling(diagnostic), message);
            report(severity == CXDiagnostic_Fatal ? "fatal" : "error", source, line, column, message);
        }
        clang_disposeDiagnostic(diagnostic);
    }

    if (hasErrors)
    {
        return false;
    }

    Namespace currentNamespace;
    buildClassAST(Cursor(clang_getTranslationUnitCursor(m_translation_unit)), currentNamespace);
    return m_diagnostics.empty();
}

bool MetaParser::validateTypeTraits(void)
{
    const fs::path scratchFile =
        m_options.inputHeader.parent_path() / "field-assignability.cpp";
    return ValidateFieldAssignability(
        m_index,
        m_options.inputHeader,
        scratchFile,
        m_argument_storage,
        m_classes,
        [this](const std::string& severity,
               const std::string& source,
               unsigned line,
               unsigned column,
               const std::string& message) {
            report(severity, source, line, column, message);
        });
}

void MetaParser::buildClassAST(const Cursor& cursor, Namespace& currentNamespace)
{
    for (const auto& child : cursor.getChildren())
    {
        const auto kind = child.getKind();
        if (child.isDefinition() && (kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl))
        {
            const std::string sourceFile = lowerCopy(
                normalizeSlashes(normalizedAbsolute(child.getSourceFile()).generic_string()));
            if (m_candidate_source_files.find(sourceFile) == m_candidate_source_files.end())
            {
                continue;
            }
            auto classInfo = std::make_shared<Class>(child, currentNamespace);
            reportMetadataDiagnostics(classInfo->getMetaData(),
                                      classInfo->getSourceFile(),
                                      classInfo->getSourceLine(),
                                      classInfo->getSourceColumn());
            for (const auto& field : classInfo->m_fields)
            {
                reportMetadataDiagnostics(field->getMetaData(),
                                          field->getSourceFile(),
                                          field->getSourceLine(),
                                          field->getSourceColumn());
            }
            if (classInfo->shouldCompile())
            {
                addClass(classInfo);
            }
        }
        else if (kind == CXCursor_Namespace)
        {
            const std::string namespaceName = child.getDisplayName();
            if (!namespaceName.empty())
            {
                currentNamespace.emplace_back(namespaceName);
                buildClassAST(child, currentNamespace);
                currentNamespace.pop_back();
            }
        }
    }
}

void MetaParser::reportMetadataDiagnostics(const MetaInfo& metadata,
                                           const std::string& fallbackSource,
                                           unsigned fallbackLine,
                                           unsigned fallbackColumn)
{
    for (const auto& diagnostic : metadata.getDiagnostics())
    {
        report("error",
               diagnostic.source.empty() ? fallbackSource : diagnostic.source,
               diagnostic.line == 0 ? fallbackLine : diagnostic.line,
               diagnostic.column == 0 ? fallbackColumn : diagnostic.column,
               diagnostic.message);
    }
}

void MetaParser::addClass(const std::shared_ptr<Class>& classInfo)
{
    const SourceIdentity identity = identifySource(classInfo->getSourceFile());
    classInfo->m_logical_source = identity.logicalPath;
    classInfo->m_output_identity = identity.outputIdentity;
    m_classes.emplace_back(classInfo);

    auto [schemaIter, inserted] = m_schema_modules.try_emplace(identity.outputIdentity);
    SchemaMoudle& schema = schemaIter->second;
    if (inserted)
    {
        schema.name = identity.outputIdentity;
        schema.sourceFile = normalizedAbsolute(classInfo->getSourceFile()).generic_string();
        schema.logicalSource = identity.logicalPath;
        schema.sourceInclude = identity.includePath;
        schema.outputIdentity = identity.outputIdentity;
    }
    else if (lowerCopy(schema.sourceFile) != lowerCopy(normalizedAbsolute(classInfo->getSourceFile()).generic_string()))
    {
        report(classInfo, "canonical output collision with source " + schema.sourceFile);
    }
    schema.classes.emplace_back(classInfo);
}

bool MetaParser::buildRegistryAndValidate(void)
{
    std::sort(m_classes.begin(), m_classes.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs->m_qualified_name, lhs->m_usr, lhs->m_logical_source, lhs->m_source_line) <
               std::tie(rhs->m_qualified_name, rhs->m_usr, rhs->m_logical_source, rhs->m_source_line);
    });

    for (const auto& classInfo : m_classes)
    {
        if (!classInfo->getUSR().empty())
        {
            const auto [iter, inserted] = m_types_by_usr.emplace(classInfo->getUSR(), classInfo);
            if (!inserted)
            {
                report(classInfo, "duplicate reflected clang USR; first declared at " + iter->second->m_logical_source +
                                      ":" + std::to_string(iter->second->getSourceLine()));
            }
        }

        const auto [qualifiedIter, qualifiedInserted] =
            m_types_by_qualified_name.emplace(classInfo->m_qualified_name, classInfo);
        if (!qualifiedInserted)
        {
            report(classInfo, "duplicate reflected qualified name; first declared at " +
                                  qualifiedIter->second->m_logical_source + ":" +
                                  std::to_string(qualifiedIter->second->getSourceLine()));
        }
        m_types_by_short_name[classInfo->m_display_name].emplace_back(classInfo);
    }

    for (auto& [shortName, classes] : m_types_by_short_name)
    {
        const bool unique = classes.size() == 1;
        for (auto& classInfo : classes)
        {
            classInfo->m_legacy_name_unique = unique;
            classInfo->m_symbol_name = unique ? classInfo->m_name : makeSymbol(classInfo->m_qualified_name);
        }
    }

    for (auto& [identity, schema] : m_schema_modules)
    {
        (void)identity;
        std::sort(schema.classes.begin(), schema.classes.end(), [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs->m_qualified_name, lhs->m_usr) < std::tie(rhs->m_qualified_name, rhs->m_usr);
        });
    }

    for (auto& [identity, schema] : m_schema_modules)
    {
        (void)identity;
        if (m_candidate_logical_source_counts[lowerCopy(schema.logicalSource)] > 1)
        {
            schema.sourceInclude = normalizeSlashes(normalizedAbsolute(schema.sourceFile).generic_string());
        }
    }

    bool valid = m_diagnostics.empty();
    for (const auto& classInfo : m_classes)
    {
        valid = validateClass(classInfo) && valid;
    }
    valid = validateStrongDependencyGraph() && valid;
    return valid && m_diagnostics.empty();
}

bool MetaParser::validateClass(const std::shared_ptr<Class>& classInfo)
{
    bool valid = true;
    if (classInfo->m_is_abstract)
    {
        report(classInfo, "reflected class is abstract and cannot be constructed by reflection");
        valid = false;
    }
    if (classInfo->m_has_explicit_constructor && !classInfo->m_has_accessible_default_constructor)
    {
        report(classInfo, "reflected class has no accessible default constructor");
        valid = false;
    }

    for (const auto& base : classInfo->m_base_classes)
    {
        if (base->access != CX_CXXPublic && base->access != CX_CXXInvalidAccessSpecifier)
        {
            report("error", classInfo->m_logical_source, base->line, base->column,
                   "non-public reflected base is not supported: " + base->typeSpelling);
            valid = false;
        }

        bool ambiguous = false;
        const auto dependency = resolveType(
            base->usr, base->qualifiedName, base->canonicalType, base->typeSpelling,
            classInfo->getCurrentNamespace(), ambiguous);
        if (ambiguous)
        {
            report("error", classInfo->m_logical_source, base->line, base->column,
                   "ambiguous reflected base dependency: " + base->typeSpelling);
            valid = false;
        }
        else if (dependency != nullptr)
        {
            base->resolvedQualifiedName = dependency->m_qualified_name;
            base->dependencyOutputIdentity = dependency->m_output_identity;
        }
    }

    std::map<std::string, std::string> serializedKeyOwners;
    std::set<std::string> serializedBaseStack;
    std::function<void(const std::shared_ptr<Class>&, unsigned, unsigned)> collectSerializedBaseKeys;
    collectSerializedBaseKeys = [&](const std::shared_ptr<Class>& baseClass, unsigned line, unsigned column) {
        if (!serializedBaseStack.emplace(baseClass->m_qualified_name).second)
        {
            return;
        }
        for (const auto& inheritedBase : baseClass->m_base_classes)
        {
            bool inheritedAmbiguous = false;
            const auto inheritedDependency = resolveType(
                inheritedBase->usr,
                inheritedBase->qualifiedName,
                inheritedBase->canonicalType,
                inheritedBase->typeSpelling,
                baseClass->getCurrentNamespace(),
                inheritedAmbiguous);
            if (!inheritedAmbiguous && inheritedDependency != nullptr)
            {
                collectSerializedBaseKeys(inheritedDependency, line, column);
            }
        }
        for (const auto& baseField : baseClass->m_fields)
        {
            if (!baseField->shouldCompile())
            {
                continue;
            }
            const std::string fieldOwner = baseClass->m_qualified_name + "::" + baseField->m_name;
            const auto [owner, inserted] = serializedKeyOwners.emplace(baseField->m_serialized_name, fieldOwner);
            if (!inserted)
            {
                report("error", classInfo->m_logical_source, line, column,
                       "duplicate flattened serialized key '" + baseField->m_serialized_name +
                           "' from " + owner->second + " and " + fieldOwner);
                valid = false;
            }
        }
        serializedBaseStack.erase(baseClass->m_qualified_name);
    };
    for (const auto& base : classInfo->m_base_classes)
    {
        const auto dependency = m_types_by_qualified_name.find(base->resolvedQualifiedName);
        if (dependency != m_types_by_qualified_name.end())
        {
            collectSerializedBaseKeys(dependency->second, base->line, base->column);
        }
    }

    for (const auto& field : classInfo->m_fields)
    {
        if (!field->shouldCompile())
        {
            continue;
        }
        const auto reject = [&](const std::string& message) {
            report("error", classInfo->m_logical_source, field->getSourceLine(), field->getSourceColumn(), message);
            valid = false;
        };

        const auto [serializedOwner, serializedInserted] = serializedKeyOwners.emplace(
            field->m_serialized_name, classInfo->m_qualified_name + "::" + field->m_name);
        if (!serializedInserted)
        {
            reject("duplicate flattened serialized key '" + field->m_serialized_name +
                   "' from " + serializedOwner->second + " and " + classInfo->m_qualified_name +
                   "::" + field->m_name);
        }

        if (field->m_is_const)
        {
            reject("const reflected field is not assignable: " + field->m_name);
        }
        if (field->m_is_reference)
        {
            reject("reference reflected field is not supported: " + field->m_name);
        }
        if (field->m_is_pointer)
        {
            reject("pointer reflected field is not supported: " + field->m_name);
        }
        if (field->m_is_raw_array)
        {
            reject("raw-array reflected field is not supported: " + field->m_name);
        }
        if (field->m_access != CX_CXXPublic && field->m_access != CX_CXXInvalidAccessSpecifier)
        {
            reject("non-public reflected field is not supported: " + field->m_name);
        }

        bool ambiguous = false;
        const auto dependency = resolveType(field->m_type_usr,
                                             field->m_type_qualified_name,
                                             field->m_canonical_type,
                                             field->m_type_spelling,
                                            classInfo->getCurrentNamespace(),
                                            ambiguous);
        if (ambiguous)
        {
            reject("ambiguous reflected field dependency: " + field->m_type_spelling);
        }
        else if (dependency != nullptr)
        {
            field->m_dependency_output_identity = dependency->m_output_identity;
            field->m_registry_type_name = dependency->m_qualified_name;
        }

        if (field->m_is_container)
        {
            ambiguous = false;
            const auto elementDependency = resolveType(field->m_container_element_usr,
                                                        field->m_container_element_qualified_name,
                                                        field->m_container_element_canonical_type,
                                                        field->m_container_element_type,
                                                       classInfo->getCurrentNamespace(),
                                                       ambiguous);
            if (ambiguous)
            {
                reject("ambiguous reflected container element dependency: " + field->m_container_element_type);
            }
            else if (elementDependency != nullptr)
            {
                field->m_container_dependency_output_identity = elementDependency->m_output_identity;
                field->m_container_registry_type_name = elementDependency->m_qualified_name;
            }
        }
    }
    return valid;
}

std::shared_ptr<Class> MetaParser::resolveType(const std::string& usr,
                                               const std::string& qualifiedName,
                                               const std::string& canonicalName,
                                               const std::string& spelling,
                                               const Namespace& ownerNamespace,
                                               bool& ambiguous) const
{
    ambiguous = false;
    if (!usr.empty())
    {
        const auto usrIter = m_types_by_usr.find(usr);
        if (usrIter != m_types_by_usr.end())
        {
            return usrIter->second;
        }
    }

    for (const std::string candidate : {qualifiedName, canonicalName, spelling})
    {
        const std::string normalized = stripTypeDecorations(candidate);
        if (normalized.empty())
        {
            continue;
        }
        const auto qualifiedIter = m_types_by_qualified_name.find(normalized);
        if (qualifiedIter != m_types_by_qualified_name.end())
        {
            return qualifiedIter->second;
        }

        if (normalized.find("::") == std::string::npos)
        {
            for (size_t namespaceSize = ownerNamespace.size(); namespaceSize > 0; --namespaceSize)
            {
                Namespace candidateNamespace(ownerNamespace.begin(), ownerNamespace.begin() + namespaceSize);
                const std::string contextualName = Utils::getQualifiedName(normalized, candidateNamespace);
                const auto contextualIter = m_types_by_qualified_name.find(contextualName);
                if (contextualIter != m_types_by_qualified_name.end())
                {
                    return contextualIter->second;
                }
            }

            const auto shortIter = m_types_by_short_name.find(normalized);
            if (shortIter != m_types_by_short_name.end())
            {
                if (shortIter->second.size() == 1)
                {
                    return shortIter->second.front();
                }
                ambiguous = true;
                return nullptr;
            }
        }
    }
    return nullptr;
}

bool MetaParser::validateStrongDependencyGraph(void)
{
    enum class VisitState
    {
        Unvisited,
        Visiting,
        Complete
    };
    std::map<std::string, VisitState> states;
    bool valid = true;

    std::function<void(const std::shared_ptr<Class>&)> visit = [&](const std::shared_ptr<Class>& classInfo) {
        states[classInfo->m_qualified_name] = VisitState::Visiting;
        for (const auto& base : classInfo->m_base_classes)
        {
            if (base->resolvedQualifiedName.empty())
            {
                continue;
            }
            const auto dependencyIter = m_types_by_qualified_name.find(base->resolvedQualifiedName);
            if (dependencyIter == m_types_by_qualified_name.end())
            {
                continue;
            }
            const VisitState state = states[dependencyIter->first];
            if (state == VisitState::Visiting)
            {
                report("error", classInfo->m_logical_source, base->line, base->column,
                       "strong reflected base dependency cycle involving " + dependencyIter->first);
                valid = false;
            }
            else if (state == VisitState::Unvisited)
            {
                visit(dependencyIter->second);
            }
        }
        states[classInfo->m_qualified_name] = VisitState::Complete;
    };

    for (const auto& classInfo : m_classes)
    {
        if (states[classInfo->m_qualified_name] == VisitState::Unvisited)
        {
            visit(classInfo);
        }
    }
    return valid;
}

MetaParser::SourceIdentity MetaParser::identifySource(const fs::path& sourcePath) const
{
    const fs::path absoluteSource = normalizedAbsolute(sourcePath);
    for (const auto& root : m_options.sourceRoots)
    {
        if (!pathIsWithin(absoluteSource, root.absolutePath))
        {
            continue;
        }

        std::error_code error;
        fs::path relativePath = fs::relative(absoluteSource, root.absolutePath, error);
        if (error || relativePath.empty())
        {
            continue;
        }
        const std::string logical = relativePath.lexically_normal().generic_string();
        const std::string outputIdentity = root.identityPrefix.empty()
                                               ? logical
                                               : (fs::path(root.identityPrefix) / relativePath).generic_string();
        return {logical, outputIdentity, logical};
    }

    const std::string fallback = absoluteSource.filename().generic_string();
    return {fallback, fallback, normalizeSlashes(absoluteSource.generic_string())};
}

std::string MetaParser::logicalPath(const fs::path& sourcePath) const
{
    return identifySource(sourcePath).logicalPath;
}

bool MetaParser::writeIndex(const std::vector<std::string>& outputs)
{
    std::map<std::string, size_t> basenameCounts;
    for (const auto& [identity, schema] : m_schema_modules)
    {
        (void)identity;
        ++basenameCounts[fs::path(schema.logicalSource).filename().generic_string()];
    }

    std::ostringstream index;
    const std::string generatorIdentity =
        std::string(GeneratorVersion) + "+" + (m_clang_version.empty() ? "libclang-unknown" : m_clang_version);
    index << "{\n"
          << "  \"schemaVersion\": 1,\n"
          << "  \"generatorVersion\": \"" << jsonEscape(generatorIdentity) << "\",\n"
          << "  \"libclangVersion\": \"" << jsonEscape(m_clang_version) << "\",\n"
          << "  \"module\": \"" << jsonEscape(m_options.moduleName) << "\",\n"
          << "  \"types\": [";

    bool firstType = true;
    for (const auto& classInfo : m_classes)
    {
        if (!firstType)
        {
            index << ',';
        }
        firstType = false;

        std::vector<std::string> typeOutputs = {
            "_generated/reflection/" + withReplacementExtension(classInfo->m_output_identity, "reflection.gen.h"),
            "_generated/serializer/" + withReplacementExtension(classInfo->m_output_identity, "gen.h"),
            "_generated/serializer/" + withReplacementExtension(classInfo->m_output_identity, "gen.hpp")};
        const std::string basename = fs::path(classInfo->m_logical_source).filename().generic_string();
        if (basenameCounts[basename] == 1)
        {
            for (const auto& compatibilityOutput : {
                     "_generated/reflection/" + withReplacementExtension(basename, "reflection.gen.h"),
                     "_generated/serializer/" + withReplacementExtension(basename, "gen.h"),
                     "_generated/serializer/" + withReplacementExtension(basename, "gen.hpp")})
            {
                if (std::find(typeOutputs.begin(), typeOutputs.end(), compatibilityOutput) == typeOutputs.end())
                {
                    typeOutputs.emplace_back(compatibilityOutput);
                }
            }
        }
        std::sort(typeOutputs.begin(), typeOutputs.end());

        struct IndexDependency
        {
            std::string identity;
            std::string kind;
            std::string source;
            unsigned line;
            unsigned column;
        };
        std::vector<IndexDependency> dependencies;
        const auto addDependency = [this, &dependencies](const std::string& qualifiedName,
                                                          const std::string& kind,
                                                          unsigned line,
                                                          unsigned column,
                                                          const std::string& source) {
            const auto dependency = m_types_by_qualified_name.find(qualifiedName);
            if (dependency == m_types_by_qualified_name.end())
            {
                return;
            }
            const auto& dependencyClass = dependency->second;
            dependencies.push_back({dependencyClass->getUSR().empty() ? dependencyClass->m_qualified_name
                                                                       : dependencyClass->getUSR(),
                                    kind,
                                    source,
                                    line,
                                    column});
        };
        for (const auto& base : classInfo->m_base_classes)
        {
            addDependency(base->resolvedQualifiedName,
                          "base",
                          base->line,
                          base->column,
                          classInfo->m_logical_source);
        }
        for (const auto& field : classInfo->m_fields)
        {
            if (!field->shouldCompile())
            {
                continue;
            }
            if (!field->m_dependency_output_identity.empty())
            {
                addDependency(field->m_registry_type_name,
                              "field",
                              field->getSourceLine(),
                              field->getSourceColumn(),
                              classInfo->m_logical_source);
            }
            if (!field->m_container_dependency_output_identity.empty())
            {
                addDependency(field->m_container_registry_type_name,
                              "container",
                              field->getSourceLine(),
                              field->getSourceColumn(),
                              classInfo->m_logical_source);
            }
        }
        std::sort(dependencies.begin(), dependencies.end(), [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.kind, lhs.identity, lhs.source, lhs.line, lhs.column) <
                   std::tie(rhs.kind, rhs.identity, rhs.source, rhs.line, rhs.column);
        });
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end(), [](const auto& lhs, const auto& rhs) {
                               return std::tie(lhs.kind, lhs.identity, lhs.source, lhs.line, lhs.column) ==
                                      std::tie(rhs.kind, rhs.identity, rhs.source, rhs.line, rhs.column);
                           }),
                           dependencies.end());

        index << "\n    {\n"
              << "      \"identity\": \""
              << jsonEscape(classInfo->getUSR().empty() ? classInfo->m_qualified_name : classInfo->getUSR())
              << "\",\n"
              << "      \"usr\": \"" << jsonEscape(classInfo->getUSR()) << "\",\n"
              << "      \"qualifiedName\": \"" << jsonEscape(classInfo->m_qualified_name) << "\",\n"
              << "      \"legacyName\": \"" << jsonEscape(classInfo->m_display_name) << "\",\n"
              << "      \"source\": \"" << jsonEscape(classInfo->m_logical_source) << "\",\n"
              << "      \"sourceIdentity\": \"" << jsonEscape(classInfo->m_output_identity) << "\",\n"
              << "      \"line\": " << classInfo->getSourceLine() << ",\n"
              << "      \"column\": " << classInfo->getSourceColumn() << ",\n"
              << "      \"outputs\": [";
        for (size_t outputIndex = 0; outputIndex < typeOutputs.size(); ++outputIndex)
        {
            if (outputIndex != 0)
            {
                index << ", ";
            }
            index << '"' << jsonEscape(typeOutputs[outputIndex]) << '"';
        }
        index << "],\n      \"dependencies\": [";
        for (size_t dependencyIndex = 0; dependencyIndex < dependencies.size(); ++dependencyIndex)
        {
            if (dependencyIndex != 0)
            {
                index << ", ";
            }
            const auto& dependency = dependencies[dependencyIndex];
            index << "{\"identity\": \"" << jsonEscape(dependency.identity)
                  << "\", \"kind\": \"" << jsonEscape(dependency.kind)
                  << "\", \"source\": \"" << jsonEscape(dependency.source)
                  << "\", \"line\": " << dependency.line
                  << ", \"column\": " << dependency.column << '}';
        }
        index << "]\n    }";
    }
    if (!m_classes.empty())
    {
        index << '\n';
    }
    index << "  ],\n  \"outputs\": [";
    for (size_t outputIndex = 0; outputIndex < outputs.size(); ++outputIndex)
    {
        if (outputIndex != 0)
        {
            index << ", ";
        }
        index << '"' << jsonEscape(outputs[outputIndex]) << '"';
    }
    index << "]\n}\n";

    std::string error;
    if (!Utils::saveFile(index.str(), m_options.indexFile, error))
    {
        report("error", m_options.indexFile.generic_string(), 0, 0, error);
        return false;
    }
    return true;
}

void MetaParser::report(const std::string& severity,
                        const std::string& source,
                        unsigned line,
                        unsigned column,
                        const std::string& message)
{
    if (m_diagnostics.size() >= MaximumDiagnostics)
    {
        return;
    }
    m_diagnostics.push_back(
        {severity,
         fs::path(source).is_absolute() ? logicalPath(source) : normalizeSlashes(source),
         line,
         column,
         sanitizeDiagnostic(message)});
}

void MetaParser::report(const std::shared_ptr<Class>& classInfo, const std::string& message)
{
    report("error", classInfo->m_logical_source, classInfo->getSourceLine(), classInfo->getSourceColumn(), message);
}

void MetaParser::emitDiagnostics(void) const
{
    for (const auto& diagnostic : m_diagnostics)
    {
        std::cerr << "HE_CODEGEN_DIAGNOSTIC|" << diagnostic.severity << '|' << diagnostic.logicalPath << '|'
                  << diagnostic.line << '|' << diagnostic.column << '|' << diagnostic.message << '\n';
    }
}
