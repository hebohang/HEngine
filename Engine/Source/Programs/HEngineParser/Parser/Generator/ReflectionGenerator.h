#pragma once

#include "Generator/Generator.h"

namespace Generator
{
    class ReflectionGenerator : public GeneratorInterface
    {
    public:
        ReflectionGenerator() = delete;
        ReflectionGenerator(fs::path outputRoot, std::string moduleName);

        bool initialize(std::string& error) override;
        bool generate(const SchemaMoudle& schema, std::string& error) override;
        bool finish(std::string& error) override;

    private:
        std::vector<SchemaMoudle> m_schemas;
        std::vector<std::string> m_header_files;
        std::vector<std::string> m_source_symbols;
    };
} // namespace Generator
