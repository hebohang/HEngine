#include "hepch.h"

#include "Runtime/Renderer/ShaderLibrary.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"

namespace HEngine
{
    ShaderLibrary::ShaderLibrary()
    {
        Add("Common", AssetManager::GetFullPath("Shaders/Common.glsl"));
    }

    void ShaderLibrary::Add(const std::string& name, const Ref<Shader>& shader)
    {
        HE_CORE_ASSERT(!Exists(name), "Shader already exists!");
        mShaders[name] = shader;
    }

    void ShaderLibrary::Add(const std::filesystem::path& shaderPath)
    {
        Add(Shader::Create(shaderPath));
    }

    void ShaderLibrary::Add(const std::string& name, const std::filesystem::path& shaderPath)
    {
        Add(name, Shader::Create(shaderPath));
    }

    void ShaderLibrary::Add(const Ref<Shader>& shader)
    {
        auto& name = shader->GetName();
        Add(name, shader);
    }

    Ref<Shader> ShaderLibrary::Load(const std::string& filepath)
    {
        auto shader = Shader::Create(filepath);
        Add(shader);
        return shader;
    }

    Ref<Shader> ShaderLibrary::Load(const std::string& name, const std::string& filepath)
    {
        auto shader = Shader::Create(filepath);
        Add(name, shader);
        return shader;
    }

    Ref<Shader> ShaderLibrary::Get(const std::string& name)
    {
        HE_CORE_ASSERT(Exists(name), "Shader not found!");
        return mShaders[name];
    }

    bool ShaderLibrary::Exists(const std::string& name) const
    {
        return mShaders.find(name) != mShaders.end();
    }
}