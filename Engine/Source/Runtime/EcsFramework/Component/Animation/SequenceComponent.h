#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <glm/glm.hpp>

#include <map>
#include <string>
#include <vector>

namespace HEngine
{
    REFLECTION_TYPE(SequenceKey)
    STRUCT(SequenceKey, Fields)
    {
        REFLECTION_BODY(SequenceKey);
        float Time = 0.0f;
        float Value = 0.0f;
        glm::vec3 Position = glm::vec3(0.0f);
        glm::vec3 Rotation = glm::vec3(0.0f);
    };

    REFLECTION_TYPE(SequenceTrack)
    STRUCT(SequenceTrack, Fields)
    {
        REFLECTION_BODY(SequenceTrack);
        // camera: local transform + FOV degrees; float/expression: scalar curve;
        // trigger/audio/event: discrete keys, Name is parameter/path/event name.
        std::string Kind = "camera";
        std::string Target = "Camera";
        std::string Name;
        bool Smooth = true;
        std::vector<SequenceKey> Keys;
    };

    REFLECTION_TYPE(SequenceComponent)
    CLASS(SequenceComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(SequenceComponent);
    public:
        float Duration = 5.0f;
        bool AutoPlay = false;
        bool Loop = false;
        float BlendIn = 0.25f;
        float BlendOut = 0.25f;
        std::string ReferenceBinding;
        glm::vec3 ReferencePosition = glm::vec3(0.0f);
        glm::vec3 ReferenceRotation = glm::vec3(0.0f);
        glm::vec3 ReferenceScale = glm::vec3(1.0f);
        std::map<std::string, std::string> Bindings;
        std::vector<SequenceTrack> Tracks;
        // Project-relative DDS assets revealed by this sequence. The runtime
        // stages their file bytes ahead of playback, within a shared CPU budget.
        std::vector<std::string> PrefetchTextures;

        PROPERTY(Disable)
        bool Playing = false;
        PROPERTY(Disable)
        bool Paused = false;
        PROPERTY(Disable)
        float Time = 0.0f;
        PROPERTY(Disable)
        float SeekTime = -1.0f;
        PROPERTY(Disable)
        Json Events = Json::array();
        PROPERTY(Disable)
        std::string Error;
    };
    REGISTER_COMPONENT_TYPE(SequenceComponent)
}
