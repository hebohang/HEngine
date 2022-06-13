#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Component/ComponentConcept.h"

namespace HEngine
{
    class SceneHierarchyPanel
    {
    public:
        SceneHierarchyPanel() = default;
        SceneHierarchyPanel(const Ref<Level>& context);

        void SetContext(const Ref<Level>& context);

        void OnImGuiRender(bool* pOpen, bool* pOpenProperties);

		Entity GetSelectedEntity() const { return mSelectionContext; }
		void SetSelectedEntity(Entity entity);
    private:
        template <Component componentType>
        void MenuAddComponent(const char* menuName, const char* menuItemName);
        void DrawEntityNode(Entity entity);
        void DrawComponents(Entity entity);
    private:
        Ref<Level> mContext;
        Entity mSelectionContext;
    };
}
