#include <assert.h>

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Core/Meta/Reflection/Reflection.h"
#include "Runtime/Core/Meta/Reflection/ReflectionRegister.h"
//#include "Runtime/Core/Meta/Serializer/Serializer.h"

#include "_generated/serializer/all_serializer.hpp"
#include "_generated/reflection/all_reflection.h"

namespace HEngine
{
    namespace Reflection
    {
        void TypeMetaRegister::metaUnregister() { TypeMetaRegisterinterface::unregisterAll(); }
    }
}
