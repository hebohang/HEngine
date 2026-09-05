#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "BasePanel.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/EcsFramework/Level/Level.h"
#include "Runtime/EcsFramework/Entity/Entity.h"

#include <unordered_map>
#include <vector>
#include <limits>

namespace HEngine
{
    class SceneHierarchyPanel : public BasePanel
    {
        REGISTER_PANEL(SceneHierarchyPanel)
    public:
		struct MeshSelectionContext
		{
			bool bValid = false;
			uint32_t SubMeshIndex = 0;
			uint32_t MaterialIndex = 0;
			bool bExpandMaterialTree = false;
		};

        SceneHierarchyPanel(EditorLayer* parent, std::string panelName, bool* pOpen)
            : BasePanel(parent, panelName, pOpen) 
        {
        }
        void SetContext(const Ref<Level>& context);
        [[nodiscard]] Ref<Level> GetContext() const { return mContext; }

        Entity GetSelectedEntity() const;
        void SetSelectedEntity(Entity entity);
		void SetSelectedEntity(Entity entity, const MeshSelectionContext& meshSelectionContext);
		MeshSelectionContext GetMeshSelectionContext() const;
		MeshSelectionContext ConsumeMeshSelectionContext();
		void ClearSelection();
		Json ExportEditorState() const;
		void ImportEditorState(const Json& state);
		IEditorUndoClient* GetUndoClient() override;
		bool IsEntityVisible(Entity entity) const;
		bool IsEntityLocked(Entity entity) const;
		bool CanSelectEntity(Entity entity) const;
		bool IsSelectedEntityLocked() const;
		Entity DuplicateEntityHierarchy(Entity entity);
	protected:
        void OnImGuiRenderImpl() override;
	private:
		size_t ComputeHierarchySignature() const;
		void RebuildHierarchyCache();
        Entity CreateFolder(Entity parent = {});
        bool MatchesFilter(Entity entity) const;
        void DrawEntityNode(Entity entity);
    private:
        Ref<Level> mContext;
		std::vector<Entity> mHierarchyRoots;
		std::unordered_map<uint32_t, std::vector<Entity>> mHierarchyChildren;
		size_t mHierarchyEntityCount = std::numeric_limits<size_t>::max();
		size_t mHierarchySignature = std::numeric_limits<size_t>::max();
		uint32_t mHierarchySignatureCountdown = 0;
		Entity mPendingScrollEntity;
    };

    class PropertiesPanel : public BasePanel
    {
        REGISTER_PANEL(PropertiesPanel)
    public:
        PropertiesPanel(EditorLayer* parent, std::string panelName, bool* pOpen)
            : BasePanel(parent, panelName, pOpen)
        {
        }
        void OnImGuiRender() override;
        IEditorUndoClient* GetUndoClient() override;
    protected:
        void OnImGuiRenderImpl() override;
    private:
        template <typename componentType>
        void MenuAddComponent(const char* menuName, const char* menuItemName);
        void DrawComponents(Entity entity);
        bool mFirstOpen = true;
    };
}
