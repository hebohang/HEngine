#pragma once

#include "Runtime/EcsFramework/System/System.h"

#include <glm/glm.hpp>

namespace HEngine
{
    class Entity;

    class CharacterMovementSystem : public System
    {
    public:
        CharacterMovementSystem(Level* level)
            : System(level)
        {
        }

        virtual ~CharacterMovementSystem() = default;

        void OnUpdateRuntime(Timestep ts) override;
        void OnRuntimeStop() override;
        [[nodiscard]] bool Teleport(Entity entity, const glm::vec3& worldPosition);
    };
}
