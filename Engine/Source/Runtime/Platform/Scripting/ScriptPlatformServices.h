#pragma once

#include <cstdint>
#include <string>

namespace HEngine
{
    class Entity;
    class Event;
    struct ScriptInputSnapshot
    {
        float MoveX = 0, MoveY = 0, PointerX = 0, PointerY = 0;
        bool Pressed = false;
        std::uint64_t PressSequence = 0;
    };

    class ScriptPlatformServices final
    {
    public:
        static ScriptInputSnapshot ReadInput(Entity context);
        static void ObserveEvent(Event& event);
        static std::string ReadSave(const std::string& key);
        static void WriteSave(const std::string& key, const std::string& value);
        static std::string Execute(std::uint64_t context, const std::string& name, const std::string& request);
    };
}
