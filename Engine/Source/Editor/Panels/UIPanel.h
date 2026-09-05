#pragma once

#include "Editor/EditorHistory.h"
#include "BasePanel.h"
#include "Runtime/Resource/ResType/UIRes.h"

#include <glm/glm.hpp>

#include <filesystem>

namespace HEngine
{
    class UIPanel : public BasePanel, public IEditorUndoClient
    {
    public:
        enum class PreviewManipulation
        {
            None = 0,
            Move,
            Resize,
            Pan,
            AnchorMin,
            AnchorMax,
            Pivot
        };

        UIPanel(EditorLayer* parent, std::string uiPath);
        void OnImGuiRender() override;
        void OnImGuiRenderImpl() override;
        IEditorUndoClient* GetUndoClient() override { return this; }

        static std::string BuildPanelKey(const std::string& uiPath);

    private:
        void DrawToolbar();
        void DrawHierarchy();
        void DrawHierarchyNode(const UINodeRes& node);
        void DrawInspector();
        void DrawPreview();
        void EnsureDefaultCanvas();
        UINodeRes* FindNode(int nodeId);
        const UINodeRes* FindNode(int nodeId) const;
        int AllocateNodeId() const;
        void AddNode(UIWidgetType type);
        void DeleteSelectedNode();
        void Save();
        void CaptureHistory(const char* label, bool bInteractive = false);
        void ResetHistory(const char* label);
        [[nodiscard]] std::string SerializeSnapshot() const;
        bool ApplySnapshot(const std::string& snapshot);
    public:
        [[nodiscard]] bool CanUndo() const override;
        [[nodiscard]] bool CanRedo() const override;
        bool Undo() override;
        bool Redo() override;
        [[nodiscard]] const char* GetUndoContextName() const override { return "UI"; }
        [[nodiscard]] bool IsUndoContextDirty() const override;

    private:
        std::string mUiPath;
        std::filesystem::path mUiFullPath;
        bool mOpen = true;
        bool mDirty = false;
        int mSelectedNodeId = -1;
        float mPreviewZoom = 1.0f;
        glm::vec2 mPreviewPan = glm::vec2(0.0f, 0.0f);
        PreviewManipulation mPreviewManipulation = PreviewManipulation::None;
        int mPreviewInteractionNodeId = -1;
        glm::vec2 mLastPreviewMousePos = glm::vec2(0.0f, 0.0f);
        UIRes mUiRes;
        EditorHistoryStack mHistory;
        std::string mSavedSnapshot;
        bool mPendingInteractiveHistory = false;
        std::string mPendingInteractiveHistoryLabel;
    };
}
