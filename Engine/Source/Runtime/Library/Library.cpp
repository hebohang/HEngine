#include "hepch.h"

#include "Runtime/Library/Library.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Renderer/Model.h"
#include "UniformBufferLibrary.h"


namespace HEngine
{
    template<typename LibType>
    Library<LibType>::Library()
    {
    }

    template<>
    Library<Model>::Library()
    {
        Add("Box", CreateRef<Model>("Assets/Models/Box.obj"));
    }
}