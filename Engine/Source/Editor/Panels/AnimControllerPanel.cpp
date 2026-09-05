#include "AnimControllerPanel.h"

#include "Editor/EditorLayer.h"
#include "Editor/EditorTheme.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "_generated/serializer/AnimController.gen.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/PlatformUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <imgui/imgui.h>
#include <imgui_node_editor.h>

#include <algorithm>

#include <nlohmann/json.hpp>

namespace ed = ax::NodeEditor;

namespace HEngine
{
	const char* AnimControllerPanel::GetUndoContextName() const
	{
		return EditorLayer::GetText("Anim Controller");
	}

	namespace
	{
		static ImVec4 NodeTitleColor(const std::string& type)
		{
			if (type == "Entry") return ImVec4(0.24f, 0.55f, 0.94f, 0.90f);
			if (type == "State") return ImVec4(0.31f, 0.67f, 0.43f, 0.90f);
			if (type == "Blend") return ImVec4(0.86f, 0.63f, 0.27f, 0.90f);
			if (type == "Output") return ImVec4(0.78f, 0.35f, 0.35f, 0.90f);
			return ImVec4(0.47f, 0.47f, 0.47f, 0.90f);
		}

        static const char* ParamTypeNames[] = { "Bool", "Int", "Float", "Trigger" };
        static const char* ConditionOps[] = { "==", "!=", ">", ">=", "<", "<=" };

        static const char* GetFileDisplayName(const std::string& path)
        {
            if (path.empty())
                return EditorLayer::GetText("None");

            const size_t slash = path.find_last_of('/');
            if (slash == std::string::npos)
                return path.c_str();
            return path.c_str() + slash + 1;
        }

        static std::string LocalizedLabel(const char* key, const char* stableId)
        {
            return std::string(EditorLayer::GetText(key)) + "###" + stableId;
        }

        static std::string BuildFileDialogFilter(const char* displayKey, const char* pattern)
        {
            std::string filter = std::string(EditorLayer::GetText(displayKey)) + " (" + pattern + ")";
            filter.push_back('\0');
            filter += pattern;
            filter.push_back('\0');
            filter.push_back('\0');
            return filter;
        }

        static std::string GetNodeNameSafe(const AnimControllerRes& controller, int nodeId)
        {
            auto iter = std::find_if(controller.nodes.begin(), controller.nodes.end(),
                [&](const AnimControllerNodeRes& node) { return node.id == nodeId; });
            if (iter == controller.nodes.end())
                return std::to_string(nodeId);
            if (iter->name.empty() || iter->name == iter->type)
                return EditorLayer::GetText(iter->type.c_str());
            return iter->name;
        }

        static AnimControllerParamType GetParamTypeByName(const AnimControllerRes& controller, const std::string& paramName)
        {
            auto iter = std::find_if(controller.parameters.begin(), controller.parameters.end(),
                [&](const AnimControllerParameterRes& param) { return param.name == paramName; });
            if (iter == controller.parameters.end())
                return AnimControllerParamType::Float;
            return iter->type;
        }
    }

    std::string AnimControllerPanel::BuildPanelKey(const std::string& controllerPath)
    {
        return std::string("AnimController:") + Utils::NormalizeAssetPath(controllerPath);
    }

    AnimControllerPanel::AnimControllerPanel(EditorLayer* parent, std::string controllerPath)
        : BasePanel(parent, BuildPanelKey(controllerPath), &mOpen)
    {
        mControllerPath = Utils::NormalizeAssetPath(controllerPath);
        mControllerFullPath = ConfigManager::GetFullPath(mControllerPath);

        if (std::filesystem::exists(mControllerFullPath))
        {
            std::string error;
            mLoaded = LoadAnimControllerRes(mControllerFullPath, mController, &error);
            if (!mLoaded)
            {
                HE_CORE_ERROR("AnimControllerPanel: load failed '{0}': {1}", mControllerPath, error);
                mController = {};
            }
        }
        else
        {
            mController.name = mControllerFullPath.stem().string();
            mController.targetSkeletonPath.clear();
            mController.entryNodeId = 1;
            mController.nodes.push_back({ 1, "Entry", "Entry", { 60.0f, 120.0f }, "" });
            mDirty = true;
            mLoaded = true;
        }

		EnsureGraphCaches();
		ResetHistory("Open Anim Controller");
    }

    AnimControllerPanel::~AnimControllerPanel()
    {
        if (mNodeEditorContext)
            ed::DestroyEditor(mNodeEditorContext);
        mNodeEditorContext = nullptr;
    }

    void AnimControllerPanel::OnImGuiRender()
    {
        if (!(*pOpen))
            return;

        const std::string title = std::string(EditorLayer::GetText("Anim Controller")) + " - " +
            std::filesystem::path(mControllerPath).filename().string() + "###AnimControllerPanel:" + mControllerPath;
        ImGuiWrapper::SetNextWindowSizeConstraints(ImVec2(780.0f, 520.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (!ImGui::Begin(title.c_str(), pOpen))
        {
            ImGui::End();
            return;
        }

		mParent->NotifyUndoClientActivity(
			this,
			ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows),
			ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows));

        OnImGuiRenderImpl();

        ImGui::End();
    }

	void AnimControllerPanel::OnImGuiRenderImpl()
	{
		EnsureEditorContext();
		EnsureGraphCaches();

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImGuiWrapper::S(6.0f, 4.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImGuiWrapper::S(6.0f, 4.0f));

		DrawToolbar();

		ImGui::Separator();
		if (ImGuiWrapper::BeginResizableSplit("AnimControllerMainSplit", 1.9f, 1.0f, ImGuiTableFlags_BordersInnerV))
		{
			DrawNodeEditor();

			ImGui::TableSetColumnIndex(1);
			DrawInspector();
			DrawParameters();
			DrawControllerProperties();

			ImGuiWrapper::EndResizableSplit();
		}

		if (mPendingInteractiveHistory && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
		{
			mHistory.CaptureSnapshot(SerializeSnapshot(),
				mPendingInteractiveHistoryLabel.empty() ? "Edit Anim Controller" : mPendingInteractiveHistoryLabel.c_str(),
				false);
			mPendingInteractiveHistory = false;
			mPendingInteractiveHistoryLabel.clear();
		}

		ImGui::PopStyleVar(2);
	}

    void AnimControllerPanel::EnsureEditorContext()
    {
        if (mNodeEditorContext)
            return;

        ed::Config config;
        config.SettingsFile = nullptr;
        config.CanvasSizeMode = ed::CanvasSizeMode::CenterOnly;
        config.DragButtonIndex = 0;
        config.SelectButtonIndex = 0;
        config.NavigateButtonIndex = 2;
        config.ContextMenuButtonIndex = 1;
        mNodeEditorContext = ed::CreateEditor(&config);
        mPendingNavigateToContent = true;
    }

	void AnimControllerPanel::EnsureGraphCaches()
	{
		if (mGraphCacheValid)
			return;

		mGraphNodes.clear();
		mGraphLinks.clear();

        for (const auto& node : mController.nodes)
        {
            GraphNodeUI ui;
            ui.NodeId = node.id;
            ui.InputPinId = AllocatePinId();
            ui.OutputPinId = AllocatePinId();
            mGraphNodes.push_back(ui);
            mNextNodeId = std::max(mNextNodeId, node.id + 1);
        }

		for (const auto& transition : mController.transitions)
		{
            GraphLinkUI link;
            link.LinkId = AllocateLinkId();
            link.FromNodeId = transition.fromNodeId;
            link.ToNodeId = transition.toNodeId;
			mGraphLinks.push_back(link);
		}

		mGraphCacheValid = true;
	}

    int AnimControllerPanel::AllocateNodeId() { return mNextNodeId++; }
    int AnimControllerPanel::AllocateLinkId() { return mNextLinkId++; }
    int AnimControllerPanel::AllocatePinId() { return mNextPinId++; }

    void AnimControllerPanel::DrawToolbar()
    {
		EditorTheme::DrawSectionHeader(EditorLayer::GetText("Animation Graph"), std::filesystem::path(mControllerPath).filename().string().c_str());
		ImGui::Spacing();

		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGuiWrapper::S(6.0f));
		if (ImGui::Button(LocalizedLabel("Add Entry", "AddEntry").c_str(), ImGuiWrapper::S(96.0f, 0.0f))) AddNode("Entry", ImVec2(80.0f, 80.0f));
		ImGui::SameLine();
		if (ImGui::Button(LocalizedLabel("Add State", "AddState").c_str(), ImGuiWrapper::S(96.0f, 0.0f))) AddNode("State", ImVec2(280.0f, 120.0f));
		ImGui::SameLine();
		if (ImGui::Button(LocalizedLabel("Add Blend", "AddBlend").c_str(), ImGuiWrapper::S(96.0f, 0.0f))) AddNode("Blend", ImVec2(280.0f, 260.0f));
		ImGui::SameLine();
		if (ImGui::Button(LocalizedLabel("Add Output", "AddOutput").c_str(), ImGuiWrapper::S(96.0f, 0.0f))) AddNode("Output", ImVec2(520.0f, 140.0f));
		ImGui::SameLine();
		if (ImGui::Button(LocalizedLabel("Focus Graph", "FocusGraph").c_str(), ImGuiWrapper::S(106.0f, 0.0f)))
            mPendingNavigateToContent = true;
		ImGui::SameLine();

		if (ImGui::Button(LocalizedLabel("Save", "Save").c_str(), ImGuiWrapper::S(84.0f, 0.0f)))
		{
			std::string err;
			if (SaveAnimControllerRes(mController, mControllerFullPath, &err))
			{
				mDirty = false;
				mSavedSnapshot = SerializeSnapshot();
				HE_CORE_INFO("AnimControllerPanel: saved '{0}'", mControllerPath);
			}
            else
            {
                HE_CORE_ERROR("AnimControllerPanel: save failed '{0}', {1}", mControllerPath, err);
			}
		}
		ImGui::PopStyleVar();
		ImGui::SameLine();
		EditorTheme::DrawPill(EditorLayer::GetText(mDirty ? "Modified" : "Saved"),
			mDirty ? ImVec4(0.42f, 0.25f, 0.12f, 0.95f) : ImVec4(0.16f, 0.33f, 0.24f, 0.95f));
    }

    void AnimControllerPanel::DrawNodeEditor()
    {
        ed::SetCurrentEditor(mNodeEditorContext);

        ed::Style& style = ed::GetStyle();
		style.Colors[ed::StyleColor_Bg] = ImColor(22, 24, 29, 255);
		style.Colors[ed::StyleColor_Grid] = ImColor(80, 92, 112, 72);
		style.Colors[ed::StyleColor_HovLinkBorder] = ImColor(255, 210, 90, 220);
		style.Colors[ed::StyleColor_SelLinkBorder] = ImColor(255, 130, 70, 240);
		const ImVec2 scaledNodePadding = ImGuiWrapper::S(14.0f, 10.0f);
		style.NodePadding = ImVec4(scaledNodePadding.x, scaledNodePadding.y, scaledNodePadding.x, scaledNodePadding.y);
		style.NodeRounding = ImGuiWrapper::S(10.0f);
		style.NodeBorderWidth = ImGuiWrapper::S(1.0f);
		style.PinRounding = ImGuiWrapper::S(4.0f);
		style.FlowMarkerDistance = ImGuiWrapper::S(28.0f);

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ed::Begin("AnimControllerNodeEditor", ImVec2(avail.x, avail.y));

        for (auto& node : mController.nodes)
        {
            auto uiIter = std::find_if(mGraphNodes.begin(), mGraphNodes.end(), [&](const GraphNodeUI& ui) { return ui.NodeId == node.id; });
            if (uiIter == mGraphNodes.end())
                continue;

            ImGui::PushID(node.id);
			ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(NodeTitleColor(node.type)));
			ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0.14f, 0.16f, 0.20f, 1.0f));
			ed::BeginNode(ed::NodeId(node.id));

			EditorTheme::DrawPill(EditorLayer::GetText(node.type.c_str()), NodeTitleColor(node.type));
			const bool bDefaultName = node.name.empty() || node.name == node.type;
			ImGui::Text("%s", bDefaultName ? EditorLayer::GetText(node.type.c_str()) : node.name.c_str());

            if (node.type == "State")
                DrawStateNodeInlineClipUI(node);

            ed::BeginPin(ed::PinId(uiIter->InputPinId), ed::PinKind::Input);
            ImGui::Text("->");
            ed::EndPin();
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(40.0f, 1.0f));
            ImGui::SameLine();
            ed::BeginPin(ed::PinId(uiIter->OutputPinId), ed::PinKind::Output);
            ImGui::Text("->");
            ed::EndPin();

			ed::EndNode();
			ed::PopStyleColor(2);
			ImGui::PopID();

            if (mApplySavedNodePositions)
            {
                if (node.position.size() < 2)
                    node.position = { 0.0f, 0.0f };
                ed::SetNodePosition(ed::NodeId(node.id), ImVec2(node.position[0], node.position[1]));
            }
        }

        mApplySavedNodePositions = false;

        for (const auto& link : mGraphLinks)
        {
            auto fromNodeUi = std::find_if(mGraphNodes.begin(), mGraphNodes.end(), [&](const GraphNodeUI& n) { return n.NodeId == link.FromNodeId; });
            auto toNodeUi = std::find_if(mGraphNodes.begin(), mGraphNodes.end(), [&](const GraphNodeUI& n) { return n.NodeId == link.ToNodeId; });
            if (fromNodeUi == mGraphNodes.end() || toNodeUi == mGraphNodes.end())
                continue;

            ed::Link(ed::LinkId(link.LinkId), ed::PinId(fromNodeUi->OutputPinId), ed::PinId(toNodeUi->InputPinId), ImColor(170, 190, 240, 220), 2.0f);
        }

        if (mPendingNavigateToContent)
        {
            ed::NavigateToContent();
            mPendingNavigateToContent = false;
        }

        if (ed::ShowBackgroundContextMenu())
        {
            ed::Suspend();
            ImGui::OpenPopup("AnimControllerBackgroundContextMenu");
            ed::Resume();
        }

        ed::Suspend();
        if (ImGui::BeginPopup("AnimControllerBackgroundContextMenu"))
        {
            const ImVec2 canvasPos = ed::ScreenToCanvas(ImGui::GetMousePos());
            if (ImGui::MenuItem(LocalizedLabel("Add Entry", "ContextAddEntry").c_str())) AddNode("Entry", canvasPos);
            if (ImGui::MenuItem(LocalizedLabel("Add State", "ContextAddState").c_str())) AddNode("State", canvasPos);
            if (ImGui::MenuItem(LocalizedLabel("Add Blend", "ContextAddBlend").c_str())) AddNode("Blend", canvasPos);
            if (ImGui::MenuItem(LocalizedLabel("Add Output", "ContextAddOutput").c_str())) AddNode("Output", canvasPos);
            ImGui::EndPopup();
        }
        ed::Resume();

        const bool bCreateContext = ed::BeginCreate();
        if (bCreateContext)
        {
            ed::PinId startPin, endPin;
            if (ed::QueryNewLink(&startPin, &endPin))
            {
                if (startPin && endPin && startPin != endPin)
                {
                    const int startPinId = (int)startPin.Get();
                    const int endPinId = (int)endPin.Get();

                    auto fromNodeUi = std::find_if(mGraphNodes.begin(), mGraphNodes.end(),
                        [&](const GraphNodeUI& n) { return n.OutputPinId == startPinId; });
                    auto toNodeUi = std::find_if(mGraphNodes.begin(), mGraphNodes.end(),
                        [&](const GraphNodeUI& n) { return n.InputPinId == endPinId; });
                    if (fromNodeUi == mGraphNodes.end() || toNodeUi == mGraphNodes.end())
                    {
                        fromNodeUi = std::find_if(mGraphNodes.begin(), mGraphNodes.end(),
                            [&](const GraphNodeUI& n) { return n.OutputPinId == endPinId; });
                        toNodeUi = std::find_if(mGraphNodes.begin(), mGraphNodes.end(),
                            [&](const GraphNodeUI& n) { return n.InputPinId == startPinId; });
                    }

                    if (fromNodeUi != mGraphNodes.end() && toNodeUi != mGraphNodes.end() && fromNodeUi->NodeId != toNodeUi->NodeId)
                    {
                        const bool bHasDuplicate = std::any_of(mController.transitions.begin(), mController.transitions.end(),
                            [&](const AnimControllerTransitionRes& t)
                            {
                                return t.fromNodeId == fromNodeUi->NodeId && t.toNodeId == toNodeUi->NodeId;
                            });

                        if (bHasDuplicate)
                        {
                            ed::RejectNewItem();
                        }
                        else
						if (ed::AcceptNewItem())
						{
							AnimControllerTransitionRes transition;
                            transition.fromNodeId = fromNodeUi->NodeId;
                            transition.toNodeId = toNodeUi->NodeId;
                            transition.duration = 0.1f;
                            mController.transitions.push_back(transition);

                            GraphLinkUI link;
                            link.LinkId = AllocateLinkId();
                            link.FromNodeId = transition.fromNodeId;
                            link.ToNodeId = transition.toNodeId;
                            mGraphLinks.push_back(link);
							mSelectedLinkId = link.LinkId;
							mSelectedNodeId = -1;
							mDirty = true;
							CaptureHistory("Add Transition");
						}
                    }
                    else
                    {
                        ed::RejectNewItem();
                    }
                }
            }
        }
        ed::EndCreate();

        const bool bDeleteContext = ed::BeginDelete();
        if (bDeleteContext)
        {
            ed::LinkId linkId;
            while (ed::QueryDeletedLink(&linkId))
            {
				if (ed::AcceptDeletedItem())
				{
					RemoveTransitionByLinkId((int)linkId.Get());
					mDirty = true;
					CaptureHistory("Delete Transition");
				}
            }

            ed::NodeId nodeId;
            while (ed::QueryDeletedNode(&nodeId))
            {
				if (ed::AcceptDeletedItem())
				{
					RemoveNode((int)nodeId.Get());
					mDirty = true;
					CaptureHistory("Delete Node");
				}
            }
        }
        ed::EndDelete();

        ed::NodeId selectedNode;
        ed::LinkId selectedLink;
        const int selectedLinkId = (ed::GetSelectedLinks(&selectedLink, 1) > 0) ? (int)selectedLink.Get() : -1;
        int selectedNodeId = (ed::GetSelectedNodes(&selectedNode, 1) > 0) ? (int)selectedNode.Get() : -1;
        if (selectedLinkId >= 0)
            selectedNodeId = -1;

        mSelectedLinkId = selectedLinkId;
        mSelectedNodeId = selectedNodeId;

        for (auto& node : mController.nodes)
        {
            const ImVec2 pos = ed::GetNodePosition(ed::NodeId(node.id));
            if (node.position.size() < 2)
                node.position = { pos.x, pos.y };
			else
			{
				node.position[0] = pos.x;
				node.position[1] = pos.y;
			}
		}
		if (ed::GetSelectedObjectCount() > 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
			CaptureHistory("Move Node", true);

        ed::End();
        ed::SetCurrentEditor(nullptr);
    }

    void AnimControllerPanel::DrawInspector()
    {
        ImGui::TextUnformatted(EditorLayer::GetText("Inspector"));
        ImGui::Separator();

        if (mSelectedLinkId >= 0)
        {
            if (AnimControllerTransitionRes* transition = FindTransitionByLinkId(mSelectedLinkId))
            {
                DrawTransitionInspector(*transition);
                return;
            }
            mSelectedLinkId = -1;
        }

        auto nodeIter = std::find_if(mController.nodes.begin(), mController.nodes.end(),
            [&](const AnimControllerNodeRes& n) { return n.id == mSelectedNodeId; });
        if (nodeIter == mController.nodes.end())
        {
            ImGui::TextDisabled("%s", EditorLayer::GetText("Select a node"));
            return;
        }

        char nameBuffer[128] = {};
        strcpy_s(nameBuffer, sizeof(nameBuffer), nodeIter->name.c_str());
		if (ImGui::InputText(LocalizedLabel("Name", "NodeName").c_str(), nameBuffer, sizeof(nameBuffer)))
		{
			nodeIter->name = nameBuffer;
			CaptureHistory("Rename Node");
		}

        ImGui::Text("%s: %s", EditorLayer::GetText("Type"), EditorLayer::GetText(nodeIter->type.c_str()));

        if (nodeIter->type == "State")
        {
            char clipBuffer[260] = {};
            strcpy_s(clipBuffer, sizeof(clipBuffer), nodeIter->clipPath.c_str());
			if (ImGui::InputText(LocalizedLabel("Clip Path", "clipPath").c_str(), clipBuffer, sizeof(clipBuffer)))
			{
				nodeIter->clipPath = Utils::NormalizeAssetPath(clipBuffer);
				CaptureHistory("Edit Clip Path");
			}

            if (ImGui::BeginDragDropTarget())
            {
                std::string dragPath;
				if (TryGetAssetPathFromPayload(dragPath) && std::filesystem::path(dragPath).extension() == ".hanim")
				{
					nodeIter->clipPath = dragPath;
					CaptureHistory("Assign Clip");
				}
                ImGui::EndDragDropTarget();
            }

            if (ImGui::Button(LocalizedLabel("Pick .hanim", "PickAnimationClip").c_str()))
            {
                const std::string animationFilter = BuildFileDialogFilter("Animation", "*.hanim");
                std::string filePath = FileDialogs::OpenFile(animationFilter.c_str(), ConfigManager::GetFullPath("Assets").string()).string();
                const size_t assetsPos = filePath.find("Assets");
                if (assetsPos != std::string::npos)
                    filePath = Utils::NormalizeAssetPath(filePath.substr(assetsPos));
				if (!filePath.empty() && std::filesystem::path(filePath).extension() == ".hanim")
				{
					nodeIter->clipPath = filePath;
					CaptureHistory("Pick Clip");
				}
			}
        }

        int outgoingCount = 0;
        for (const auto& transition : mController.transitions)
        {
            if (transition.fromNodeId == nodeIter->id)
                ++outgoingCount;
        }
        ImGui::Text("%s: %d", EditorLayer::GetText("Outgoing transitions"), outgoingCount);
    }

    void AnimControllerPanel::DrawStateNodeInlineClipUI(AnimControllerNodeRes& node)
    {
        const std::string normalized = Utils::NormalizeAssetPath(node.clipPath);
        ImGui::TextDisabled("%s: %s", EditorLayer::GetText("Clip"), GetFileDisplayName(normalized));

        if (ImGui::BeginDragDropTarget())
        {
            std::string dragPath;
            if (TryGetAssetPathFromPayload(dragPath) && std::filesystem::path(dragPath).extension() == ".hanim")
            {
                node.clipPath = dragPath;
				CaptureHistory("Assign Clip");
            }
            ImGui::EndDragDropTarget();
        }
    }

    void AnimControllerPanel::DrawTransitionInspector(AnimControllerTransitionRes& transition)
    {
        ImGui::TextUnformatted(EditorLayer::GetText("Transition"));
        ImGui::Separator();
        ImGui::Text("%s: %s", EditorLayer::GetText("From"), GetNodeNameSafe(mController, transition.fromNodeId).c_str());
        ImGui::Text("%s: %s", EditorLayer::GetText("To"), GetNodeNameSafe(mController, transition.toNodeId).c_str());

		if (ImGui::DragFloat(LocalizedLabel("Duration", "TransitionDuration").c_str(), &transition.duration, 0.01f, 0.0f, 5.0f, "%.2f"))
			CaptureHistory("Edit Transition Duration", true);

        if (ImGui::Button(LocalizedLabel("Add Condition", "AddCondition").c_str()))
        {
            AnimControllerConditionRes condition;
            if (!mController.parameters.empty())
                condition.param = mController.parameters.front().name;
            condition.op = "==";
            condition.value = true;
			transition.conditions.push_back(condition);
			CaptureHistory("Add Condition");
		}

        int removeIndex = -1;
        for (int i = 0; i < static_cast<int>(transition.conditions.size()); ++i)
        {
            auto& condition = transition.conditions[i];
            ImGui::PushID(i + 60000);

            int selectedParamIndex = -1;
            for (int p = 0; p < static_cast<int>(mController.parameters.size()); ++p)
            {
                if (mController.parameters[p].name == condition.param)
                {
                    selectedParamIndex = p;
                    break;
                }
            }
            if (!mController.parameters.empty())
            {
                if (selectedParamIndex < 0)
                {
                    selectedParamIndex = 0;
							condition.param = mController.parameters[0].name;
							CaptureHistory("Repair Condition Parameter");
						}

                if (ImGui::BeginCombo(LocalizedLabel("Param", "ConditionParam").c_str(), mController.parameters[selectedParamIndex].name.c_str()))
                {
                    for (int p = 0; p < static_cast<int>(mController.parameters.size()); ++p)
                    {
                        const bool selected = (p == selectedParamIndex);
                        if (ImGui::Selectable(mController.parameters[p].name.c_str(), selected))
                        {
							condition.param = mController.parameters[p].name;
							CaptureHistory("Change Condition Parameter");
						}
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            else
            {
                ImGui::TextDisabled("%s", EditorLayer::GetText("No parameters"));
            }

            int opIndex = 0;
            for (int oi = 0; oi < IM_ARRAYSIZE(ConditionOps); ++oi)
            {
                if (condition.op == ConditionOps[oi])
                {
                    opIndex = oi;
                    break;
                }
            }
			if (ImGui::Combo(LocalizedLabel("Op", "ConditionOp").c_str(), &opIndex, ConditionOps, IM_ARRAYSIZE(ConditionOps)))
			{
				condition.op = ConditionOps[opIndex];
				CaptureHistory("Change Condition Operator");
			}

            const AnimControllerParamType paramType = GetParamTypeByName(mController, condition.param);
            if (paramType == AnimControllerParamType::Bool || paramType == AnimControllerParamType::Trigger)
            {
                bool value = condition.value.is_boolean() ? condition.value.get<bool>() : false;
				if (ImGui::Checkbox(LocalizedLabel("Value", "ConditionValue").c_str(), &value))
				{
					condition.value = value;
					CaptureHistory("Change Condition Value");
				}
            }
            else if (paramType == AnimControllerParamType::Int)
            {
                int value = condition.value.is_number_integer() ? condition.value.get<int>() : 0;
				if (ImGui::DragInt(LocalizedLabel("Value", "ConditionValue").c_str(), &value, 1.0f))
				{
					condition.value = value;
					CaptureHistory("Change Condition Value", true);
				}
            }
            else
            {
                float value = condition.value.is_number() ? condition.value.get<float>() : 0.0f;
				if (ImGui::DragFloat(LocalizedLabel("Value", "ConditionValue").c_str(), &value, 0.05f))
				{
					condition.value = value;
					CaptureHistory("Change Condition Value", true);
				}
            }

            if (ImGui::Button(LocalizedLabel("Remove Condition", "RemoveCondition").c_str()))
                removeIndex = i;

            ImGui::Separator();
            ImGui::PopID();
        }

        if (removeIndex >= 0)
        {
			transition.conditions.erase(transition.conditions.begin() + removeIndex);
			CaptureHistory("Remove Condition");
		}
    }

    void AnimControllerPanel::DrawParameters()
    {
        ImGui::Spacing();
        ImGui::TextUnformatted(EditorLayer::GetText("Parameters"));
        ImGui::Separator();

        if (ImGui::Button(LocalizedLabel("Add Parameter", "AddParameter").c_str()))
        {
            AnimControllerParameterRes param;
            param.name = "newParam";
            param.type = AnimControllerParamType::Float;
            param.defaultValue = 0.0f;
			mController.parameters.push_back(param);
			CaptureHistory("Add Parameter");
		}

        int removeIndex = -1;
        for (int i = 0; i < static_cast<int>(mController.parameters.size()); ++i)
        {
            auto& p = mController.parameters[i];
            ImGui::PushID(i);

            char name[128] = {};
            strcpy_s(name, sizeof(name), p.name.c_str());
			if (ImGui::InputText("##ParamName", name, sizeof(name)))
			{
				p.name = name;
				CaptureHistory("Rename Parameter");
			}
            ImGui::SameLine();

            int typeIndex = static_cast<int>(p.type);
            const char* localizedParamTypeNames[] = {
                EditorLayer::GetText(ParamTypeNames[0]),
                EditorLayer::GetText(ParamTypeNames[1]),
                EditorLayer::GetText(ParamTypeNames[2]),
                EditorLayer::GetText(ParamTypeNames[3])
            };
			if (ImGui::Combo("##ParamType", &typeIndex, localizedParamTypeNames, IM_ARRAYSIZE(localizedParamTypeNames)))
			{
				p.type = static_cast<AnimControllerParamType>(typeIndex);
				CaptureHistory("Change Parameter Type");
			}

            if (p.type == AnimControllerParamType::Bool || p.type == AnimControllerParamType::Trigger)
            {
                bool value = p.defaultValue.is_boolean() ? p.defaultValue.get<bool>() : false;
				if (ImGui::Checkbox(LocalizedLabel("Default", "ParameterDefault").c_str(), &value))
				{
					p.defaultValue = value;
					CaptureHistory("Change Parameter Default");
				}
            }
            else if (p.type == AnimControllerParamType::Int)
            {
                int value = p.defaultValue.is_number_integer() ? p.defaultValue.get<int>() : 0;
				if (ImGui::DragInt(LocalizedLabel("Default", "ParameterDefault").c_str(), &value, 1.0f))
				{
					p.defaultValue = value;
					CaptureHistory("Change Parameter Default", true);
				}
            }
            else
            {
                float value = p.defaultValue.is_number() ? p.defaultValue.get<float>() : 0.0f;
				if (ImGui::DragFloat(LocalizedLabel("Default", "ParameterDefault").c_str(), &value, 0.01f))
				{
					p.defaultValue = value;
					CaptureHistory("Change Parameter Default", true);
				}
            }

            if (ImGui::Button(LocalizedLabel("Delete", "DeleteParameter").c_str()))
                removeIndex = i;

            ImGui::Separator();
            ImGui::PopID();
        }

        if (removeIndex >= 0)
        {
			mController.parameters.erase(mController.parameters.begin() + removeIndex);
			CaptureHistory("Delete Parameter");
		}
    }

    void AnimControllerPanel::DrawControllerProperties()
    {
        ImGui::Spacing();
        ImGui::TextUnformatted(EditorLayer::GetText("Controller"));
        ImGui::Separator();

        char nameBuffer[128] = {};
        strcpy_s(nameBuffer, sizeof(nameBuffer), mController.name.c_str());
		if (ImGui::InputText(LocalizedLabel("Name", "ControllerName").c_str(), nameBuffer, sizeof(nameBuffer)))
		{
			mController.name = nameBuffer;
			CaptureHistory("Rename Controller");
		}

        char skeletonBuffer[260] = {};
        strcpy_s(skeletonBuffer, sizeof(skeletonBuffer), mController.targetSkeletonPath.c_str());
		if (ImGui::InputText(LocalizedLabel("Target Skeleton Path", "targetSkeletonPath").c_str(), skeletonBuffer, sizeof(skeletonBuffer)))
		{
			mController.targetSkeletonPath = Utils::NormalizeAssetPath(skeletonBuffer);
			CaptureHistory("Edit Skeleton Path");
		}

        if (ImGui::BeginDragDropTarget())
        {
            std::string p;
			if (TryGetAssetPathFromPayload(p) && std::filesystem::path(p).extension() == ".hskel")
			{
				mController.targetSkeletonPath = p;
				CaptureHistory("Assign Skeleton");
			}
            ImGui::EndDragDropTarget();
        }
    }

	void AnimControllerPanel::AddNode(const std::string& type, const ImVec2& position)
	{
        AnimControllerNodeRes node;
        node.id = AllocateNodeId();
        node.type = type;
        node.name = type;
        node.position = { position.x, position.y };
		mController.nodes.push_back(node);
        if (type == "Entry")
            mController.entryNodeId = node.id;
		mDirty = true;
		mGraphCacheValid = false;
		mApplySavedNodePositions = true;
		mPendingNavigateToContent = true;
		EnsureGraphCaches();
		CaptureHistory("Add Node");
	}

	void AnimControllerPanel::RemoveNode(int nodeId)
	{
        mController.nodes.erase(std::remove_if(mController.nodes.begin(), mController.nodes.end(),
            [&](const AnimControllerNodeRes& n) { return n.id == nodeId; }), mController.nodes.end());

        mController.transitions.erase(std::remove_if(mController.transitions.begin(), mController.transitions.end(),
            [&](const AnimControllerTransitionRes& t) { return t.fromNodeId == nodeId || t.toNodeId == nodeId; }), mController.transitions.end());

        if (mController.entryNodeId == nodeId)
            mController.entryNodeId = mController.nodes.empty() ? 0 : mController.nodes.front().id;

		mSelectedNodeId = -1;
		mSelectedLinkId = -1;
		mGraphCacheValid = false;
		mPendingNavigateToContent = true;
		EnsureGraphCaches();
    }

    void AnimControllerPanel::RemoveTransitionByLinkId(int linkId)
    {
        auto linkIter = std::find_if(mGraphLinks.begin(), mGraphLinks.end(),
            [&](const GraphLinkUI& l) { return l.LinkId == linkId; });
        if (linkIter == mGraphLinks.end())
            return;

        const int fromId = linkIter->FromNodeId;
        const int toId = linkIter->ToNodeId;

        mController.transitions.erase(std::remove_if(mController.transitions.begin(), mController.transitions.end(),
            [&](const AnimControllerTransitionRes& t) { return t.fromNodeId == fromId && t.toNodeId == toId; }),
            mController.transitions.end());

        mGraphLinks.erase(linkIter);
        if (mSelectedLinkId == linkId)
            mSelectedLinkId = -1;
    }

	void AnimControllerPanel::CaptureHistory(const char* label, bool bInteractive)
	{
		mDirty = true;
		if (bInteractive)
		{
			mPendingInteractiveHistory = true;
			mPendingInteractiveHistoryLabel = label ? label : "Edit Anim Controller";
			return;
		}

		mPendingInteractiveHistory = false;
		mPendingInteractiveHistoryLabel.clear();
		mHistory.CaptureSnapshot(SerializeSnapshot(), label ? label : "Edit Anim Controller", false);
	}

	void AnimControllerPanel::ResetHistory(const char* label)
	{
		mSavedSnapshot = SerializeSnapshot();
		mHistory.Reset(mSavedSnapshot, label ? label : "Open Anim Controller");
		mDirty = false;
	}

	std::string AnimControllerPanel::SerializeSnapshot() const
	{
		Json root;
		Serializer::Serialize(root["Controller"], mController);
		return root.dump();
	}

	bool AnimControllerPanel::ApplySnapshot(const std::string& snapshot)
	{
		try
		{
			const Json root = Json::parse(snapshot);
			Serializer::Deserialize(root.at("Controller"), mController);
			mGraphCacheValid = false;
			mApplySavedNodePositions = true;
			mPendingNavigateToContent = false;
			EnsureGraphCaches();
			const bool bHasSelectedNode = std::any_of(mController.nodes.begin(), mController.nodes.end(), [this](const AnimControllerNodeRes& node) { return node.id == mSelectedNodeId; });
			if (!bHasSelectedNode)
				mSelectedNodeId = -1;
			const bool bHasSelectedLink = std::any_of(mGraphLinks.begin(), mGraphLinks.end(), [this](const GraphLinkUI& link) { return link.LinkId == mSelectedLinkId; });
			if (!bHasSelectedLink)
				mSelectedLinkId = -1;
			mDirty = SerializeSnapshot() != mSavedSnapshot;
			return true;
		}
		catch (const std::exception& ex)
		{
			HE_CORE_ERROR("AnimControllerPanel: apply snapshot failed '{0}', {1}", mControllerPath, ex.what());
			return false;
		}
	}

	bool AnimControllerPanel::CanUndo() const
	{
		return mHistory.CanUndo();
	}

	bool AnimControllerPanel::CanRedo() const
	{
		return mHistory.CanRedo();
	}

	bool AnimControllerPanel::Undo()
	{
		return mHistory.Undo([this](const std::string& snapshot) { return ApplySnapshot(snapshot); });
	}

	bool AnimControllerPanel::Redo()
	{
		return mHistory.Redo([this](const std::string& snapshot) { return ApplySnapshot(snapshot); });
	}

	bool AnimControllerPanel::IsUndoContextDirty() const
	{
		return mDirty;
	}

    bool AnimControllerPanel::TryGetAssetPathFromPayload(std::string& outPath) const
    {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM");
        if (!payload)
            return false;

        auto relative = std::filesystem::path((const wchar_t*)payload->Data);
        outPath = Utils::NormalizeAssetPath((std::filesystem::path("Assets") / relative).string());
        return true;
    }

    AnimControllerTransitionRes* AnimControllerPanel::FindTransitionByLinkId(int linkId)
    {
        auto linkIter = std::find_if(mGraphLinks.begin(), mGraphLinks.end(),
            [&](const GraphLinkUI& link) { return link.LinkId == linkId; });
        if (linkIter == mGraphLinks.end())
            return nullptr;

        auto transitionIter = std::find_if(mController.transitions.begin(), mController.transitions.end(),
            [&](const AnimControllerTransitionRes& transition)
            {
                return transition.fromNodeId == linkIter->FromNodeId && transition.toNodeId == linkIter->ToNodeId;
            });
        if (transitionIter == mController.transitions.end())
            return nullptr;
        return &(*transitionIter);
    }

}
