#include "Hepch.h"

#include "Runtime/Library/Library.h"
#include "Runtime/Graphics/Mesh/Mesh.h"

namespace HEngine
{
    template<typename LibType>
    Library<LibType>::Library()
    {
    }

    template<>
    Library<Mesh>::Library()
    {
        Add("Box", CreateRef<Mesh>("Assets/Models/Box/Box.hmesh"));
    }
}
