#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

namespace HEngine
{
    REFLECTION_TYPE(PrefabInstanceComponent)
    CLASS(PrefabInstanceComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(PrefabInstanceComponent);
    public:
        std::string TemplatePath;
        std::string LocalId;
        std::string InstanceRootUuid;
        std::string BaselineJson;
        std::string OverridePathsJson = "[]";
        std::string TemplateRevision;
        std::string TemplateDocumentJson;
    };
    REGISTER_COMPONENT_TYPE(PrefabInstanceComponent)
}
