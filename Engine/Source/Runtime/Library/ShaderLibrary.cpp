#include "hepch.h"

#include "Runtime/Library/ShaderLibrary.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"

namespace HEngine
{
    Library<Shader>::Library()
    {
        Add("Common", Shader::Create(AssetManager::GetFullPath("Shaders/Common.glsl")));
        Add("BasePBR", Shader::Create(AssetManager::GetFullPath("Shaders/BasePBR.glsl")));
    }

    Ref<Shader> Library<Shader>::GetDefaultShader()
    {
        return mLibrary["BasePBR"];
    }
}