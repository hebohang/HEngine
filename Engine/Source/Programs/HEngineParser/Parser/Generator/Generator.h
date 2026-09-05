#pragma once

#include "Common/SchemaModule.h"

#include <string>

class Class;

namespace Generator
{
    class GeneratorInterface
    {
    public:
        GeneratorInterface(fs::path outputRoot, std::string moduleName);
        virtual ~GeneratorInterface() = default;

        virtual bool initialize(std::string& error) = 0;
        virtual bool generate(const SchemaMoudle& schema, std::string& error) = 0;
        virtual bool finish(std::string& error) = 0;

        const std::vector<std::string>& getOutputs() const;

    protected:
        void genClassRenderData(const std::shared_ptr<Class>& classInfo, Mustache::data& classDefinition) const;
        void genClassFieldRenderData(const std::shared_ptr<Class>& classInfo, Mustache::data& fieldDefinitions) const;

        bool writeOutput(const fs::path& relativePath, const std::string& content, std::string& error);

        static std::string makeNamespaceOpen(const Namespace& nameSpace);
        static std::string makeNamespaceClose(const Namespace& nameSpace);
        static std::string makeSerializerHeaderPath(const SchemaMoudle& schema);
        static std::string makeSerializerSourcePath(const SchemaMoudle& schema);
        static std::string makeReflectionHeaderPath(const SchemaMoudle& schema);
        static std::string makeCompatibilitySerializerHeaderPath(const SchemaMoudle& schema);
        static std::string makeCompatibilitySerializerSourcePath(const SchemaMoudle& schema);
        static std::string makeCompatibilityReflectionHeaderPath(const SchemaMoudle& schema);

        fs::path m_output_root;
        std::string m_module_name;
        std::vector<std::string> m_outputs;
    };
} // namespace Generator
