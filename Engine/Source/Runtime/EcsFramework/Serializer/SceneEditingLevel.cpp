#include "Hepch.h"
#include "Runtime/EcsFramework/Serializer/SceneEditingLevel.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "all_serializer.h"

#include <stdexcept>

namespace HEngine
{
    namespace
    {
        template<Component... C>
        void ReplaceComponents(ComponentGroup<C...>, Entity entity, const Json& value)
        {
            ([&]() {
                const std::string name = C::GetTypeName();
                if (!value.contains(name))
                {
                    if (entity.HasComponent<C>()) entity.RemoveComponent<C>();
                    return;
                }
                if (entity.HasComponent<C>())
                {
                    Json current; Serializer::Serialize(current, entity.GetComponent<C>());
                    if (current == value.at(name)) return;
                }
                C component; Serializer::Deserialize(value.at(name), component);
                entity.AddOrReplaceComponent<C>(component);
            }(), ...);
        }
    }
    void ApplySceneEditingValues(const Ref<Level>& level, const Json& values)
    {
        if (!level) throw std::invalid_argument("Author Level is unavailable.");
        // Detach explicitly deleted nodes before Level::DestroyEntity, whose
        // existing contract recursively destroys transform children.
        for (auto& [id, value] : values.items())
            if (value.is_null())
            {
                Entity entity = level->TryGetEntityByUUID(UUID(id));
                if (entity && entity.HasComponent<TransformComponent>())
                    entity.GetComponent<TransformComponent>().ParentUuid.clear();
            }
        for (auto& [id, value] : values.items())
        {
            if (value.is_null()) continue;
            Entity entity = level->TryGetEntityByUUID(UUID(id));
            if (!entity) entity = level->CreateEntityWithoutTrans(UUID(id), value.at("TagComponent").at("Tag"));
            ReplaceComponents(ComponentsSerialized{}, entity, value);
        }
        for (auto& [id, value] : values.items())
            if (value.is_null())
            {
                Entity entity = level->TryGetEntityByUUID(UUID(id));
                if (entity) level->DestroyEntity(entity);
            }
#if WITH_EDITOR
        level->InvalidateEditorEntityStateCache();
#endif
    }
}
