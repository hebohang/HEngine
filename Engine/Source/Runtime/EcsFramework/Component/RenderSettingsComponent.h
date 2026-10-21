#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
    // One component per scene. Device settings are rejected by the existing
    // render-settings layer contract; only World-writable fields are accepted.
    REFLECTION_TYPE(RenderSettingsComponent)
    CLASS(RenderSettingsComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(RenderSettingsComponent);
    public:
        PROPERTY(NoPython)
        Json Overrides = Json::object();
    };
    REGISTER_COMPONENT_TYPE(RenderSettingsComponent)
}
