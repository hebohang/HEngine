#include "hepch.h"

#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Library/UniformBufferLibrary.h"

#include <glm/glm.hpp>

namespace HEngine
{
    struct CameraData
    {
        glm::mat4 ViewProjection;
    };

    UniformBufferLibrary::UniformBufferLibrary()
    {
        Ref<UniformBuffer> CameraUniformBuffer = UniformBuffer::Create(sizeof(CameraData), 0);
        AddUniformBuffer("CameraUniform", CameraUniformBuffer);
    }

    void UniformBufferLibrary::AddUniformBuffer(const std::string& name, const Ref<UniformBuffer>& texture)
    {
        HE_CORE_ASSERT(mUniformBufferLibrary.find(name) == mUniformBufferLibrary.end());
        mUniformBufferLibrary[name] = texture;
    }

    Ref<UniformBuffer> UniformBufferLibrary::GetUniformBuffer(const std::string& name)
    {
        HE_CORE_ASSERT(mUniformBufferLibrary.find(name) != mUniformBufferLibrary.end());
        return mUniformBufferLibrary[name];
    }
}