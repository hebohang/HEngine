#pragma once

#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <optional>

namespace HEngine
{
    // Also applies to attached meshes/weapons below a character in the hierarchy.
    inline std::optional<glm::vec3> GetCharacterPresentationOffset(Level& level, Entity entity)
    {
        while (entity)
        {
            if (entity.HasComponent<CharacterMovementComponent>())
            {
                const auto& movement = entity.GetComponent<CharacterMovementComponent>();
                return movement.RuntimePresentation.Offset(level.GetWorldTranslation(entity),
                    level.GetFixedInterpolationAlpha());
            }
            entity = level.GetParent(entity);
        }
        return std::nullopt;
    }
}
