#pragma once

#include "Scene.h"
#include "Runtime/Core/Base/Base.h"

namespace HEngine
{
    class SceneSerializer
    {
    public:
        SceneSerializer(const Ref<Scene>& scene);

        void Serialize(const std::string& filepath);
        void SerializeRuntime(const std::string& filepath);

        bool Deserialize(const std::string& filepath);
        bool DeserializeRuntime(const std::string& filepath);
    private:
        Ref<Scene> m_Scene;
    };
}
