#pragma once

#include "Runtime/Renderer/Shader.h"
#include "Runtime/Core/Base/PublicSingleton.h"

#include <filesystem>

namespace HEngine
{
    class ShaderLibrary : public PublicSingleton<ShaderLibrary>
    {
    public:
        ShaderLibrary();

        static Ref<Shader> GetDefaultShader();

        void Add(const std::string& name, const Ref<Shader>& shader);
        void Add(const Ref<Shader>& shader);
        void Add(const std::filesystem::path& shaderPath);
        void Add(const std::string& name, const std::filesystem::path& shaderPath);
        Ref<Shader> Load(const std::string& filepath);
        Ref<Shader> Load(const std::string& name, const std::string& filepath);
        
        [[nodiscard]] Ref<Shader> Get(const std::string& name);

        bool Exists(const std::string& name) const;
    private:
        std::unordered_map<std::string, Ref<Shader>> mShaders;
    };
}