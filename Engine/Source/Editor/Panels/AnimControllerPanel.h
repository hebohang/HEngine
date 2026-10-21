#pragma once

#include "Editor/EditorHistory.h"
#include "BasePanel.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Resource/ResType/AnimController.h"

#include <imgui/imgui.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ax::NodeEditor
{
    struct EditorContext;
}

namespace HEngine
{
    class AnimControllerPanel : public BasePanel, public IEditorUndoClient
    {
    public:
        AnimControllerPanel(EditorLayer* parent, std::string controllerPath);
        ~AnimControllerPanel() override;

        Json AutomationGraph(const Json& params);
        void OnImGuiRender() override;
        void OnImGuiRenderImpl() override;
        IEditorUndoClient* GetUndoClient() override { return this; }

        static std::string BuildPanelKey(const std::string& controllerPath);

    private:
        struct GraphNodeUI
        {
            int NodeId = 0;
            int InputPinId = 0;
            int OutputPinId = 0;
        };

        struct GraphLinkUI
        {
            int LinkId = 0;
            int FromNodeId = 0;
            int ToNodeId = 0;
        };

        void EnsureEditorContext();
        void EnsureGraphCaches();
        void DrawToolbar();
        void DrawNodeEditor();
        void DrawInspector();
        void DrawParameters();
        void DrawControllerProperties();
        void DrawMotionSettings(AnimControllerNodeRes& node);
        void DrawStateNodeInlineClipUI(AnimControllerNodeRes& node);
        void DrawTransitionInspector(AnimControllerTransitionRes& transition);

        int AllocateNodeId();
        int AllocateLinkId();
        int AllocatePinId();

        void AddNode(const std::string& type, const ImVec2& position);
        void RemoveNode(int nodeId);
        void RemoveTransitionByLinkId(int linkId);
        bool TryGetAssetPathFromPayload(std::string& outPath) const;
        AnimControllerTransitionRes* FindTransitionByLinkId(int linkId);
        void CaptureHistory(const char* label, bool bInteractive = false);
        void ResetHistory(const char* label);
        [[nodiscard]] std::string SerializeSnapshot() const;
        bool ApplySnapshot(const std::string& snapshot);
    public:
        [[nodiscard]] bool CanUndo() const override;
        [[nodiscard]] bool CanRedo() const override;
        bool Undo() override;
        bool Redo() override;
        [[nodiscard]] const char* GetUndoContextName() const override;
        [[nodiscard]] bool IsUndoContextDirty() const override;

        std::string mControllerPath;
        std::filesystem::path mControllerFullPath;
        bool mOpen = true;
        bool mLoaded = false;
        bool mDirty = false;

        AnimControllerRes mController;

        ax::NodeEditor::EditorContext* mNodeEditorContext = nullptr;
        std::vector<GraphNodeUI> mGraphNodes;
        std::vector<GraphLinkUI> mGraphLinks;

        int mNextNodeId = 1000;
        int mNextLinkId = 5000;
        int mNextPinId = 10000;

        int mSelectedNodeId = -1;
        int mSelectedLinkId = -1;
        bool mApplySavedNodePositions = true;
        bool mGraphCacheValid = false;
        bool mPendingNavigateToContent = true;
        Json mAutomationCanvasState = Json::object();
        std::uint64_t mGraphFocusRequests = 0;
        std::uint64_t mGraphFocusExecutions = 0;
        std::uint64_t mGraphFocusFrame = 0;
        std::uint64_t mGraphCanvasSizeChanges = 0;
        bool mInitialGraphFitActive = true;
        bool mInitialGraphFitExhausted = false;
        unsigned mInitialGraphFitFrames = 0;
        unsigned mInitialGraphFitAttempts = 0;
        unsigned mGraphStableSizeFrames = 0;
        ImVec2 mPreviousCanvasSize = ImVec2(0, 0);
        void RequestGraphFocus();
        EditorHistoryStack mHistory;
        std::string mSavedSnapshot;
        bool mPendingInteractiveHistory = false;
        std::string mPendingInteractiveHistoryLabel;
    };
}
