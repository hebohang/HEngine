#pragma once

#include "Runtime/Renderer/Shader.h"
#include "Runtime/Library/Library.h"

namespace HEngine
{
    template<>
    class Library<Shader> : public LibraryBase<Library, Shader>
    {
    public:
        Library();

        [[nodiscard]] Ref<Shader> GetDefaultShader();
    };
}