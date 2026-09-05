#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
    REFLECTION_TYPE(PostProcessingComponent)
    CLASS(PostProcessingComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(PostProcessingComponent);
    public:
        PostProcessingComponent() = default;
        PostProcessingComponent(const PostProcessingComponent&) = default;

        bool bEnabled = true;
        // Legacy scenes used bGlobal=false as an explicit "do not apply"
        // state. Local volume evaluation therefore requires this separate,
        // opt-in flag instead of reinterpreting old scene data.
        bool bGlobal = true;
        bool bLocalVolumeEnabled = false;
        float Priority = 0.0f;
        float BlendRadius = 0.0f;
        float BlendWeight = 1.0f;
        // Defaults to true so a legacy enabled global component keeps its
        // exact exposure override after reflection-driven migration.
        bool bOverrideExposure = true;
        float Exposure = 0.8f;
    };
    REGISTER_COMPONENT_TYPE(PostProcessingComponent)
}
