#include "Common/Precompiled.h"

#include "Generator/Generator.h"
#include "LanguageTypes/Class.h"

namespace
{
    std::string replaceExtension(const std::string& path, const std::string& extension)
    {
        fs::path result(path);
        result.replace_extension(extension);
        return result.generic_string();
    }

    bool isSafeRelativePath(const fs::path& path)
    {
        if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
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

    std::string escapeCppStringLiteral(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const unsigned char character : value)
        {
            switch (character)
            {
                case '\\': escaped += "\\\\"; break;
                case '"': escaped += "\\\""; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:
                    if (character < 0x20)
                    {
                        std::ostringstream code;
                        code << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                             << static_cast<unsigned>(character);
                        escaped += code.str();
                    }
                    else
                    {
                        escaped.push_back(static_cast<char>(character));
                    }
                    break;
            }
        }
        return escaped;
    }
}

namespace Generator
{
    GeneratorInterface::GeneratorInterface(fs::path outputRoot, std::string moduleName) :
        m_output_root(std::move(outputRoot)), m_module_name(std::move(moduleName))
    {}

    const std::vector<std::string>& GeneratorInterface::getOutputs() const { return m_outputs; }

    void GeneratorInterface::genClassRenderData(const std::shared_ptr<Class>& classInfo,
                                                Mustache::data& classDefinition) const
    {
        classDefinition.set("class_name", classInfo->m_name);
        classDefinition.set("class_forward_keyword", classInfo->m_is_struct ? "struct" : "class");
        classDefinition.set("class_qualified_name", classInfo->m_qualified_name);
        classDefinition.set("class_symbol_name", classInfo->m_symbol_name);
        classDefinition.set("class_registry_name", classInfo->m_qualified_name);
        classDefinition.set("class_legacy_name", classInfo->m_display_name);
        classDefinition.set("class_namespace_open", makeNamespaceOpen(classInfo->getCurrentNamespace()));
        classDefinition.set("class_namespace_close", makeNamespaceClose(classInfo->getCurrentNamespace()));
        classDefinition.set("class_source", classInfo->m_logical_source);
        classDefinition.set("class_line", std::to_string(classInfo->getSourceLine()));
        classDefinition.set("class_column", std::to_string(classInfo->getSourceColumn()));
        classDefinition.set("class_base_class_size", std::to_string(classInfo->m_base_classes.size()));
        classDefinition.set("class_need_register", true);
        classDefinition.set("class_has_unique_legacy_alias", classInfo->m_legacy_name_unique);

        if (!classInfo->m_base_classes.empty())
        {
            Mustache::data baseDefinitions(Mustache::data::type::list);
            classDefinition.set("class_has_base", true);
            for (size_t index = 0; index < classInfo->m_base_classes.size(); ++index)
            {
                const auto& base = classInfo->m_base_classes[index];
                Mustache::data baseDefinition;
                baseDefinition.set("class_base_class_name", base->typeSpelling);
                baseDefinition.set("class_base_class_qualified_name", base->resolvedQualifiedName.empty()
                                                                           ? base->typeSpelling
                                                                           : base->resolvedQualifiedName);
                baseDefinition.set("class_base_class_index", std::to_string(index));
                baseDefinitions.push_back(baseDefinition);
            }
            classDefinition.set("class_base_class_defines", baseDefinitions);
        }

        if (classInfo->m_name.size() > 9 && classInfo->m_name.ends_with("Component"))
        {
            classDefinition.set("pyComponentName", classInfo->m_name.substr(0, classInfo->m_name.size() - 9));
        }

        Mustache::data fieldDefinitions(Mustache::data::type::list);
        genClassFieldRenderData(classInfo, fieldDefinitions);
        classDefinition.set("class_field_defines", fieldDefinitions);
    }

    void GeneratorInterface::genClassFieldRenderData(const std::shared_ptr<Class>& classInfo,
                                                     Mustache::data& fieldDefinitions) const
    {
        for (const auto& field : classInfo->m_fields)
        {
            if (!field->shouldCompile())
            {
                continue;
            }

            Mustache::data fieldDefinition;
            const std::string fieldType = field->getCodegenTypeName();
            fieldDefinition.set("class_field_name", field->m_name);
            fieldDefinition.set("class_field_type", fieldType);
            fieldDefinition.set("class_field_legacy_type", field->m_type);
            fieldDefinition.set("class_field_registry_type",
                                field->m_is_container ? fieldType :
                                    (field->m_registry_type_name.empty() ? field->m_type : field->m_registry_type_name));
            fieldDefinition.set("class_field_display_name", field->m_display_name);
            fieldDefinition.set("class_field_serialized_name", field->m_serialized_name);
            fieldDefinition.set("class_field_serialized_name_cpp", escapeCppStringLiteral(field->m_serialized_name));
            fieldDefinition.set("class_field_source", classInfo->m_logical_source);
            fieldDefinition.set("class_field_line", std::to_string(field->getSourceLine()));
            fieldDefinition.set("class_field_column", std::to_string(field->getSourceColumn()));
            fieldDefinition.set("class_field_is_vector", field->m_is_container);
            fieldDefinitions.push_back(fieldDefinition);
        }
    }

    bool GeneratorInterface::writeOutput(const fs::path& relativePath,
                                         const std::string& content,
                                         std::string& error)
    {
        if (!isSafeRelativePath(relativePath))
        {
            error = "unsafe generated output path: " + relativePath.generic_string();
            return false;
        }

        const std::string normalizedPath = relativePath.lexically_normal().generic_string();
        if (std::find(m_outputs.begin(), m_outputs.end(), normalizedPath) != m_outputs.end())
        {
            error = "generated output collision: " + normalizedPath;
            return false;
        }

        const fs::path outputRoot = fs::absolute(m_output_root).lexically_normal();
        const fs::path outputPath = fs::absolute(outputRoot / relativePath).lexically_normal();
        std::error_code relativeError;
        const fs::path containmentPath = fs::relative(outputPath, outputRoot, relativeError);
        if (relativeError || !isSafeRelativePath(containmentPath))
        {
            error = "generated output escaped output root: " + outputPath.generic_string();
            return false;
        }

        if (!Utils::saveFile(content, outputPath, error))
        {
            return false;
        }
        m_outputs.emplace_back(normalizedPath);
        return true;
    }

    std::string GeneratorInterface::makeNamespaceOpen(const Namespace& nameSpace)
    {
        std::string result;
        for (const auto& part : nameSpace)
        {
            result += "namespace ";
            result += part;
            result += " { ";
        }
        return result;
    }

    std::string GeneratorInterface::makeNamespaceClose(const Namespace& nameSpace)
    {
        std::string result;
        for (auto iter = nameSpace.rbegin(); iter != nameSpace.rend(); ++iter)
        {
            result += "} // namespace ";
            result += *iter;
            result += "\n";
        }
        return result;
    }

    std::string GeneratorInterface::makeSerializerHeaderPath(const SchemaMoudle& schema)
    {
        return "_generated/serializer/" + replaceExtension(schema.outputIdentity, "gen.h");
    }

    std::string GeneratorInterface::makeSerializerSourcePath(const SchemaMoudle& schema)
    {
        return "_generated/serializer/" + replaceExtension(schema.outputIdentity, "gen.hpp");
    }

    std::string GeneratorInterface::makeReflectionHeaderPath(const SchemaMoudle& schema)
    {
        return "_generated/reflection/" + replaceExtension(schema.outputIdentity, "reflection.gen.h");
    }

    std::string GeneratorInterface::makeCompatibilitySerializerHeaderPath(const SchemaMoudle& schema)
    {
        return "_generated/serializer/" + replaceExtension(fs::path(schema.logicalSource).filename().generic_string(),
                                                             "gen.h");
    }

    std::string GeneratorInterface::makeCompatibilitySerializerSourcePath(const SchemaMoudle& schema)
    {
        return "_generated/serializer/" + replaceExtension(fs::path(schema.logicalSource).filename().generic_string(),
                                                             "gen.hpp");
    }

    std::string GeneratorInterface::makeCompatibilityReflectionHeaderPath(const SchemaMoudle& schema)
    {
        return "_generated/reflection/" +
               replaceExtension(fs::path(schema.logicalSource).filename().generic_string(), "reflection.gen.h");
    }
} // namespace Generator
