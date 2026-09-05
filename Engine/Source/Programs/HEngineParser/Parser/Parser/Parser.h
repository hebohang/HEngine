#pragma once

#include "Common/Namespace.h"
#include "Common/Precompiled.h"
#include "Common/SchemaModule.h"
#include "Cursor/Cursor.h"
#include "Generator/Generator.h"

class Class;
class MetaInfo;

struct ParserSourceRoot
{
    fs::path absolutePath;
    std::string identityPrefix;
};

struct ParserOptions
{
    fs::path inputList;
    fs::path inputHeader;
    fs::path outputRoot;
    fs::path contextFile;
    fs::path templatesDirectory;
    fs::path indexFile;
    std::string moduleName;
    bool quiet = false;
    bool validateGenerated = false;
    bool legacyMode = false;

    std::vector<std::string> includePaths;
    std::vector<std::string> defines;
    std::vector<std::string> clangArguments;
    std::vector<ParserSourceRoot> sourceRoots;
};

class MetaParser
{
public:
    static void prepare(void);

    explicit MetaParser(ParserOptions options);
    MetaParser(const std::string& projectInputFile,
               const std::string& includeFilePath,
               const std::string& includePath,
               const std::string& includeSystemPath,
               const std::string& moduleName,
               bool isShowErrors);
    ~MetaParser(void);

    int parse(void);
    int generateFiles(void);

    const std::string& getClangVersion(void) const;

private:
    struct SourceIdentity
    {
        std::string logicalPath;
        std::string outputIdentity;
        std::string includePath;
    };

    struct Diagnostic
    {
        std::string severity;
        std::string logicalPath;
        unsigned line = 0;
        unsigned column = 0;
        std::string message;
    };

    bool loadContext(void);
    bool validateOptions(void);
    bool parseProject(void);
    bool parseTranslationUnit(void);
    bool collectClangDiagnostics(void);
    bool validateTypeTraits(void);
    bool buildRegistryAndValidate(void);
    bool validateClass(const std::shared_ptr<Class>& classInfo);
    bool validateStrongDependencyGraph(void);
    bool initializeGenerators(void);
    bool validateGeneratedOutputs(void);
    bool writeIndex(const std::vector<std::string>& outputs);

    void buildClassAST(const Cursor& cursor, Namespace& currentNamespace);
    void addClass(const std::shared_ptr<Class>& classInfo);
    void reportMetadataDiagnostics(const MetaInfo& metadata,
                                   const std::string& fallbackSource,
                                   unsigned fallbackLine,
                                   unsigned fallbackColumn);

    std::shared_ptr<Class> resolveType(const std::string& usr,
                                       const std::string& qualifiedName,
                                       const std::string& canonicalName,
                                       const std::string& spelling,
                                       const Namespace& ownerNamespace,
                                       bool& ambiguous) const;
    SourceIdentity identifySource(const fs::path& sourcePath) const;
    std::string logicalPath(const fs::path& sourcePath) const;

    void report(const std::string& severity,
                const std::string& source,
                unsigned line,
                unsigned column,
                const std::string& message);
    void report(const std::shared_ptr<Class>& classInfo, const std::string& message);
    void emitDiagnostics(void) const;

    ParserOptions m_options;

    CXIndex m_index = nullptr;
    CXTranslationUnit m_translation_unit = nullptr;

    std::vector<std::string> m_argument_storage;
    std::vector<const char*> m_arguments;

    std::map<std::string, SchemaMoudle> m_schema_modules;
    std::vector<std::shared_ptr<Class>> m_classes;
    std::map<std::string, std::shared_ptr<Class>> m_types_by_usr;
    std::map<std::string, std::shared_ptr<Class>> m_types_by_qualified_name;
    std::map<std::string, std::vector<std::shared_ptr<Class>>> m_types_by_short_name;
    std::map<std::string, size_t> m_candidate_logical_source_counts;
    std::set<std::string> m_candidate_source_files;

    std::vector<std::unique_ptr<Generator::GeneratorInterface>> m_generators;
    std::vector<Diagnostic> m_diagnostics;
    std::string m_clang_version;
    bool m_context_loaded = false;
};
