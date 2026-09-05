
#include "TemplateManager.h"
void TemplateManager::configure(const fs::path& templateDirectory)
{
    m_template_directory = templateDirectory;
    m_template_pool.clear();
}

bool TemplateManager::loadTemplates(const std::string& templateName, std::string& error)
{
    error.clear();
    if (m_template_directory.empty())
    {
        fs::path currentPath(__FILE__);
        m_template_directory = currentPath.parent_path().parent_path().parent_path() / "Template";
    }

    const fs::path templatePath = m_template_directory / (templateName + ".mustache");
    std::string templateText;
    if (!Utils::loadFile(templatePath, templateText, error))
    {
        return false;
    }
    if (templateText.empty())
    {
        error = "template is empty: " + templatePath.generic_string();
        return false;
    }

    Mustache::mustache parsedTemplate(templateText);
    if (!parsedTemplate.is_valid())
    {
        error = "invalid template " + templatePath.generic_string() + ": " + parsedTemplate.error_message();
        return false;
    }

    m_template_pool.insert_or_assign(templateName, std::move(templateText));
    return true;
}

bool TemplateManager::renderByTemplate(const std::string& templateName,
                                       Mustache::data& templateData,
                                       std::string& output,
                                       std::string& error) const
{
    const auto templateIter = m_template_pool.find(templateName);
    if (templateIter == m_template_pool.end())
    {
        error = "template was not loaded: " + templateName;
        return false;
    }

    Mustache::mustache templateRenderer(templateIter->second);
    if (!templateRenderer.is_valid())
    {
        error = "invalid template " + templateName + ": " + templateRenderer.error_message();
        return false;
    }

    output = templateRenderer.render(templateData);
    if (!templateRenderer.is_valid())
    {
        error = "template render failed for " + templateName + ": " + templateRenderer.error_message();
        output.clear();
        return false;
    }
    if (output.empty())
    {
        error = "template rendered empty output: " + templateName;
        return false;
    }
    return true;
}
