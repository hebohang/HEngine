#pragma once
#include "Common/Precompiled.h"
class TemplateManager
{
public:
    static TemplateManager* getInstance()
    {
        static TemplateManager instance;
        return &instance;
    }

    void configure(const fs::path& templateDirectory);

    bool loadTemplates(const std::string& templateName, std::string& error);

    bool renderByTemplate(const std::string& templateName,
                          Mustache::data& templateData,
                          std::string& output,
                          std::string& error) const;

private:
    TemplateManager() {}
    TemplateManager(const TemplateManager&);
    TemplateManager&                             operator=(const TemplateManager&);
    fs::path                                     m_template_directory;
    std::unordered_map<std::string, std::string> m_template_pool;
};
