#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
    REFLECTION_TYPE(AudioListenerComponent)
    CLASS(AudioListenerComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(AudioListenerComponent);
    public:
        bool bActive = true;
        bool bUsePrimaryCameraOrientation = true;
    };
    REGISTER_COMPONENT_TYPE(AudioListenerComponent)
}
