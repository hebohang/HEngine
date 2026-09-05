#pragma once

#include "Runtime/Audio/AudioTypes.h"
#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <string>

namespace HEngine
{
    REFLECTION_TYPE(SoundComponent)
    CLASS(SoundComponent : public ComponentBase, Fields)
    {
        REFLECTION_COMPONENT_BODY(SoundComponent);
    public:
        SoundComponent() = default;
        SoundComponent(const SoundComponent&) = default;
        SoundComponent(SoundComponent&&) noexcept = default;
        SoundComponent& operator=(const SoundComponent&) = default;
        SoundComponent& operator=(SoundComponent&&) noexcept = default;

        SoundComponent(const std::string& path)
            : Path(path)
        {}

        std::string Path = "None";
        bool Play = true;
        bool bLooping = false;
        bool bSpatialized = false;
        bool bStream = false;
        float Volume = 1.0f;
        float Pitch = 1.0f;
        AudioBus Bus = AudioBus::Gameplay;
        AudioAttenuationModel Attenuation = AudioAttenuationModel::Inverse;
        float MinDistance = 1.0f;
        float MaxDistance = 50.0f;
        float Rolloff = 1.0f;
        float DopplerFactor = 1.0f;
    };
    REGISTER_COMPONENT_TYPE(SoundComponent)
}
