#pragma once

#include "Runtime/EcsFramework/Component/ComponentBase.h"

#include <fmod.hpp>

namespace HEngine
{
    class SoundComponent : public ComponentBase
    {
    public:
        SoundComponent() = default;
        SoundComponent(const SoundComponent&) = default;
        SoundComponent(const std::string& path)
            : Path(path)
        {}

        std::string Path = "None";
        bool Play;

        FMOD::Sound* Sound;
        FMOD::Channel* Channel = nullptr;
    };
}