#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/EcsFramework/Level/Level.h"

namespace HEngine
{
    class SceneSerializer
    {
    public:
        SceneSerializer(const Ref<Level>& level);

        void Serialize(const std::string& filepath);
        void SerializeRuntime(const std::string& filepath);

        bool Deserialize(const std::string& filepath);
        bool DeserializeRuntime(const std::string& filepath);
    private:
        Ref<Level> mLevel;
    };
}
