#include "hepch.h"

#include "Runtime/Library/UniformBufferLibrary.h"

namespace HEngine
{
    Library<UniformBuffer>::Library()
    {
        Ref<UniformBuffer> CameraUniformBuffer = UniformBuffer::Create(sizeof(CameraData), 0);
        Add("CameraUniform", CameraUniformBuffer);
    }

    Ref<UniformBuffer> Library<UniformBuffer>::GetCameraUniformBuffer()
    {
        return mLibrary["CameraUniform"];
    }
}