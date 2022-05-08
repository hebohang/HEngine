#pragma once 

#include "Runtime/Renderer/UniformBuffer.h"
#include "Runtime/Library/Library.h"

#include <glm/glm.hpp>

namespace HEngine
{
    struct CameraData
    {
        glm::mat4 ViewProjection;
    };

    template<>
    class Library<UniformBuffer> : public LibraryBase<Library, UniformBuffer>
    {
    public:
        Library();

        [[nodiscard]] Ref<UniformBuffer> GetCameraUniformBuffer();
    };
}