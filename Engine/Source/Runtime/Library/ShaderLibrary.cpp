#include "hepch.h"

#include "Runtime/Library/ShaderLibrary.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"

namespace HEngine
{
    Library<Shader>::Library()
    {
        Add("BasePBR", Shader::CreateNative(AssetManager::GetFullPath("Shaders/BasePBR.glsl")));
        Add("SkyBox", Shader::Create(AssetManager::GetFullPath("Shaders/SkyBox.glsl")));

        Add("IBL_background", Shader::CreateNative(AssetManager::GetFullPath("Shaders/IBL/background.glsl")));
        Add("IBL_brdf", Shader::CreateNative(AssetManager::GetFullPath("Shaders/IBL/brdf.glsl")));
        Add("IBL_equirectangularToCubemap", Shader::CreateNative(AssetManager::GetFullPath("Shaders/IBL/equirectangularToCubemap.glsl")));
        Add("IBL_irradiance", Shader::CreateNative(AssetManager::GetFullPath("Shaders/IBL/irradiance.glsl")));
        Add("IBL_prefilter", Shader::CreateNative(AssetManager::GetFullPath("Shaders/IBL/prefilter.glsl")));

        Add("Post_Outline", Shader::CreateNative(AssetManager::GetFullPath("Shaders/PostProcessing/Outline.glsl")));
        Add("Post_Cartoon", Shader::CreateNative(AssetManager::GetFullPath("Shaders/PostProcessing/Cartoon.glsl")));
        Add("Post_GrayScale", Shader::CreateNative(AssetManager::GetFullPath("Shaders/PostProcessing/GrayScale.glsl")));
        Add("Post_GaussianBlur", Shader::CreateNative(AssetManager::GetFullPath("Shaders/PostProcessing/GaussianBlur.glsl")));
        Add("NormalOutline", Shader::CreateNative(AssetManager::GetFullPath("Shaders/NormalOutline.glsl")));
        Add("NormalOutline_anim", Shader::CreateNative(AssetManager::GetFullPath("Shaders/NormalOutline_anim.glsl")));

        Add("CSM_Depth", Shader::CreateNative(AssetManager::GetFullPath("Shaders/Shadow/CSM_Depth.glsl")));
        Add("Debug_Depth", Shader::CreateNative(AssetManager::GetFullPath("Shaders/Shadow/Debug_Depth.glsl")));
    }

    Ref<Shader> Library<Shader>::GetDefaultShader()
    {
        return mLibrary["BasePBR"];
    }

    Ref<Shader> Library<Shader>::GetSkyBoxShader()
    {
        return mLibrary["SkyBox"];
    }
}