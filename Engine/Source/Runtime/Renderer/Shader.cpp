#include "hepch.h"

#include "Runtime/Renderer/Shader.h"
#include "Runtime/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLShader.h"

namespace HEngine
{
    Ref<Shader> Shader::Create(const std::filesystem::path& filepath)
    {
        return Create(filepath.string());
    }

    Ref<Shader> Shader::Create(const std::string& filepath)
    {
        switch (RendererAPI::Current())
        {
        case RendererAPI::RendererAPIType::None:    HE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
        case RendererAPI::RendererAPIType::OpenGL:  return std::make_shared<OpenGLShader>(filepath);
        }

        HE_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<Shader> Shader::Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc)
    {
        switch (RendererAPI::Current())
        {
        case RendererAPI::RendererAPIType::None:    HE_CORE_ASSERT(false, "RendererAPI::None is currently not supported!"); return nullptr;
        case RendererAPI::RendererAPIType::OpenGL:  return std::make_shared<OpenGLShader>(name, vertexSrc, fragmentSrc);
        }

        HE_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    void ShaderLibrary::Add(const std::string& name, const Ref<Shader>& shader)
    {
        HE_CORE_ASSERT(!Exists(name), "Shader already exists!");
        mShaders[name] = shader;
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

    ShaderUniform::ShaderUniform(const std::string& name, ShaderUniformType type, uint32_t size, uint32_t offset)
        : mName(name), mType(type), mSize(size), mOffset(offset)
    {
    }

    const std::string& ShaderUniform::UniformTypeToString(ShaderUniformType type)
    {
        if (type == ShaderUniformType::Bool)
        {
            return "Boolean";
        }
        else if (type == ShaderUniformType::Int)
        {
            return "Int";
        }
        else if (type == ShaderUniformType::Float)
        {
            return "Float";
        }

        return "None";
    }
}