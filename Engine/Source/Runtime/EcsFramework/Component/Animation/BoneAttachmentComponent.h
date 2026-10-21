#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"
#include <glm/glm.hpp>
#include <string>

namespace HEngine
{
    // The transform parent is the animated mesh; offsets use its bone-local units.
    REFLECTION_TYPE(BoneAttachmentComponent)
    CLASS(BoneAttachmentComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(BoneAttachmentComponent);
    public:
        std::string Bone;
        glm::vec3 Offset = glm::vec3(0.0f);
        glm::vec3 Rotation = glm::vec3(0.0f);
        glm::vec3 Scale = glm::vec3(1.0f);
        bool bEnabled = true;
    };
    REGISTER_COMPONENT_TYPE(BoneAttachmentComponent)
}
