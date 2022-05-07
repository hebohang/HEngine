#pragma once 

#include "Runtime/Core/Base/PublicSingleton.h"
#include "Runtime/Renderer/UniformBuffer.h"

#include <unordered_map>

namespace HEngine
{
    class UniformBufferLibrary : public PublicSingleton<UniformBufferLibrary>
    {
    public:
        UniformBufferLibrary();

        void AddUniformBuffer(const std::string& name, const Ref<UniformBuffer>& texture);

        [[nodiscard]] Ref<UniformBuffer> GetUniformBuffer(const std::string& name);

        [[nodiscard]] Ref<UniformBuffer> GetCameraUniformBuffer() { return mUniformBufferLibrary["CameraUniform"]; }
    private:
        std::unordered_map<std::string, Ref<UniformBuffer>> mUniformBufferLibrary;
    };
}