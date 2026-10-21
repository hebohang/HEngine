#include "UIPanel.h"

#include "Editor/EditorLayer.h"
#include "Editor/EditorTheme.h"
#include "Editor/ImGuiWrapper/ImGuiWrapper.h"
#include "Editor/Manager/IconManager.h"
#include "_generated/serializer/UIRes.gen.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Resource/RuntimeResourceRegistry/RuntimeUiResource.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"
#include "Runtime/Utils/PlatformUtils.h"

#include <imgui/imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace HEngine
{
    namespace
    {
        constexpr float kDefaultCanvasWidth = 1280.0f;
        constexpr float kDefaultCanvasHeight = 720.0f;
        constexpr float kMinNodeSize = 8.0f;
        constexpr float kPreviewPadding = 12.0f;
        constexpr float kResizeHandleSize = 10.0f;
        constexpr float kMinPreviewZoom = 0.25f;
        constexpr float kMaxPreviewZoom = 3.0f;
        constexpr float kPreviewGridStep = 100.0f;
        constexpr int kPreviewGridMajorLineEvery = 5;
        constexpr float kPreviewCheckerSize = 18.0f;

        struct AnchorPreset
        {
            const char* Label = "";
            glm::vec2 AnchorMin = glm::vec2(0.0f, 0.0f);
            glm::vec2 AnchorMax = glm::vec2(0.0f, 0.0f);
            glm::vec2 Pivot = glm::vec2(0.5f, 0.5f);
        };

        struct PivotPreset
        {
            const char* Label = "";
            glm::vec2 Pivot = glm::vec2(0.5f, 0.5f);
        };

        static const std::array<AnchorPreset, 9> kFixedAnchorPresets = {{
            { "TL", glm::vec2(0.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f, 0.0f) },
            { "TC", glm::vec2(0.5f, 0.0f), glm::vec2(0.5f, 0.0f), glm::vec2(0.5f, 0.0f) },
            { "TR", glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 0.0f) },
            { "ML", glm::vec2(0.0f, 0.5f), glm::vec2(0.0f, 0.5f), glm::vec2(0.0f, 0.5f) },
            { "MC", glm::vec2(0.5f, 0.5f), glm::vec2(0.5f, 0.5f), glm::vec2(0.5f, 0.5f) },
            { "MR", glm::vec2(1.0f, 0.5f), glm::vec2(1.0f, 0.5f), glm::vec2(1.0f, 0.5f) },
            { "BL", glm::vec2(0.0f, 1.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f, 1.0f) },
            { "BC", glm::vec2(0.5f, 1.0f), glm::vec2(0.5f, 1.0f), glm::vec2(0.5f, 1.0f) },
            { "BR", glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, 1.0f) },
        }};

        static const std::array<AnchorPreset, 3> kHorizontalStretchAnchorPresets = {{
            { "HT", glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.5f, 0.0f) },
            { "HM", glm::vec2(0.0f, 0.5f), glm::vec2(1.0f, 0.5f), glm::vec2(0.5f, 0.5f) },
            { "HB", glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.5f, 1.0f) },
        }};

        static const std::array<AnchorPreset, 3> kVerticalStretchAnchorPresets = {{
            { "VL", glm::vec2(0.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f, 0.5f) },
            { "VC", glm::vec2(0.5f, 0.0f), glm::vec2(0.5f, 1.0f), glm::vec2(0.5f, 0.5f) },
            { "VR", glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, 0.5f) },
        }};

        static const std::array<AnchorPreset, 1> kFullStretchAnchorPreset = {{
            { "Full", glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.5f, 0.5f) },
        }};

        static const std::array<PivotPreset, 9> kPivotPresets = {{
            { "TL", glm::vec2(0.0f, 0.0f) },
            { "TC", glm::vec2(0.5f, 0.0f) },
            { "TR", glm::vec2(1.0f, 0.0f) },
            { "ML", glm::vec2(0.0f, 0.5f) },
            { "MC", glm::vec2(0.5f, 0.5f) },
            { "MR", glm::vec2(1.0f, 0.5f) },
            { "BL", glm::vec2(0.0f, 1.0f) },
            { "BC", glm::vec2(0.5f, 1.0f) },
            { "BR", glm::vec2(1.0f, 1.0f) },
        }};

        struct PreviewNodeState
        {
            UILayoutRect LocalRect;
            ImVec2 ScreenMin;
            ImVec2 ScreenMax;
        };

        static bool NearlyEqual(const glm::vec2& lhs, const glm::vec2& rhs, float epsilon = 0.001f)
        {
            return std::abs(lhs.x - rhs.x) <= epsilon && std::abs(lhs.y - rhs.y) <= epsilon;
        }

        static bool MatchesAnchorPreset(const UINodeRes& node, const AnchorPreset& preset)
        {
            return NearlyEqual(node.Layout.AnchorMin, preset.AnchorMin) &&
                NearlyEqual(node.Layout.AnchorMax, preset.AnchorMax) &&
                NearlyEqual(node.Layout.Pivot, preset.Pivot);
        }

        static bool MatchesPivotPreset(const UINodeRes& node, const PivotPreset& preset)
        {
            return NearlyEqual(node.Layout.Pivot, preset.Pivot);
        }

        static glm::vec2 Clamp01(const glm::vec2& value)
        {
            return glm::clamp(value, glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f));
        }

        static void SanitizeAnchors(glm::vec2& anchorMin, glm::vec2& anchorMax)
        {
            anchorMin = Clamp01(anchorMin);
            anchorMax = Clamp01(anchorMax);
            if (anchorMin.x > anchorMax.x)
                std::swap(anchorMin.x, anchorMax.x);
            if (anchorMin.y > anchorMax.y)
                std::swap(anchorMin.y, anchorMax.y);
        }

        static glm::vec2 SanitizePivot(const glm::vec2& pivot)
        {
            return Clamp01(pivot);
        }

        static bool IsStretchX(const UINodeRes& node)
        {
            return !NearlyEqual(glm::vec2(node.Layout.AnchorMin.x, 0.0f), glm::vec2(node.Layout.AnchorMax.x, 0.0f));
        }

        static bool IsStretchY(const UINodeRes& node)
        {
            return !NearlyEqual(glm::vec2(node.Layout.AnchorMin.y, 0.0f), glm::vec2(node.Layout.AnchorMax.y, 0.0f));
        }

        static void CollectPreviewStatesRecursive(const UINodeRes& node,
            const UILayoutRect& parentRect,
            const std::unordered_map<int, std::vector<const UINodeRes*>>& childrenMap,
            const ImVec2& canvasOrigin,
            float canvasScale,
            std::unordered_map<int, PreviewNodeState>& outStates);
        static glm::vec2 ResolveParentSize(const UINodeRes& node,
            const glm::vec2& canvasSize,
            const std::unordered_map<int, PreviewNodeState>& states);
        static UILayoutRect ResolveRelativeRect(const UINodeRes& node,
            const glm::vec2& parentSize,
            const std::unordered_map<int, PreviewNodeState>& states);
        static void ApplyLayoutFromRect(UINodeRes& node,
            const UILayoutRect& relativeRect,
            const glm::vec2& parentSize,
            const glm::vec2& anchorMin,
            const glm::vec2& anchorMax,
            const glm::vec2& pivot);

        static const char* WidgetTypeName(UIWidgetType type)
        {
            switch (type)
            {
            case UIWidgetType::Canvas: return "Canvas";
            case UIWidgetType::Panel: return "Panel";
            case UIWidgetType::Text: return "Text";
            case UIWidgetType::Image: return "Image";
            case UIWidgetType::Button: return "Button";
            default: return "Panel";
            }
        }

        static std::string TrimCopy(const std::string& value)
        {
            size_t begin = 0;
            while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
                ++begin;

            size_t end = value.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
                --end;

            return value.substr(begin, end - begin);
        }

        static std::string ToLowerCopy(std::string value)
        {
            for (char& c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return value;
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

        static bool TryParseCssColor(const std::string& value, ImVec4& outColor)
        {
            const std::string trimmed = TrimCopy(value);
            if (trimmed.empty())
                return false;

            if (trimmed[0] == '#')
            {
                std::string hex = trimmed.substr(1);
                if (hex.size() == 3 || hex.size() == 4)
                {
                    std::string expanded;
                    expanded.reserve(hex.size() * 2);
                    for (char c : hex)
                    {
                        expanded.push_back(c);
                        expanded.push_back(c);
                    }
                    hex = expanded;
                }

                if (hex.size() != 6 && hex.size() != 8)
                    return false;

                char* end = nullptr;
                const long r = std::strtol(hex.substr(0, 2).c_str(), &end, 16);
                if (!end || *end != '\0') return false;
                const long g = std::strtol(hex.substr(2, 2).c_str(), &end, 16);
                if (!end || *end != '\0') return false;
                const long b = std::strtol(hex.substr(4, 2).c_str(), &end, 16);
                if (!end || *end != '\0') return false;
                const long a = (hex.size() == 8) ? std::strtol(hex.substr(6, 2).c_str(), &end, 16) : 255;
                if (!end || *end != '\0') return false;

                outColor = ImVec4(
                    std::clamp(static_cast<float>(r) / 255.0f, 0.0f, 1.0f),
                    std::clamp(static_cast<float>(g) / 255.0f, 0.0f, 1.0f),
                    std::clamp(static_cast<float>(b) / 255.0f, 0.0f, 1.0f),
                    std::clamp(static_cast<float>(a) / 255.0f, 0.0f, 1.0f));
                return true;
            }

            int r = 0, g = 0, b = 0, a = 255;
            if (std::sscanf(trimmed.c_str(), "rgba(%d,%d,%d,%d)", &r, &g, &b, &a) == 4)
            {
                outColor = ImVec4(
                    std::clamp(static_cast<float>(r) / 255.0f, 0.0f, 1.0f),
                    std::clamp(static_cast<float>(g) / 255.0f, 0.0f, 1.0f),
                    std::clamp(static_cast<float>(b) / 255.0f, 0.0f, 1.0f),
                    std::clamp(static_cast<float>(a) / 255.0f, 0.0f, 1.0f));
                return true;
            }

            if (std::sscanf(trimmed.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3)
            {
                outColor = ImVec4(
                    std::clamp(static_cast<float>(r) / 255.0f, 0.0f, 1.0f),
                    std::clamp(static_cast<float>(g) / 255.0f, 0.0f, 1.0f),
                    std::clamp(static_cast<float>(b) / 255.0f, 0.0f, 1.0f),
                    1.0f);
                return true;
            }

            return false;
        }

        static std::string FormatCssColor(const ImVec4& color, const std::string& original)
        {
            const int r = static_cast<int>(std::round(std::clamp(color.x, 0.0f, 1.0f) * 255.0f));
            const int g = static_cast<int>(std::round(std::clamp(color.y, 0.0f, 1.0f) * 255.0f));
            const int b = static_cast<int>(std::round(std::clamp(color.z, 0.0f, 1.0f) * 255.0f));
            const int a = static_cast<int>(std::round(std::clamp(color.w, 0.0f, 1.0f) * 255.0f));

            char buffer[64] = {};
            const std::string lowered = ToLowerCopy(original);
            if (lowered.rfind("rgba", 0) == 0)
            {
                std::snprintf(buffer, sizeof(buffer), "rgba(%d, %d, %d, %d)", r, g, b, a);
                return buffer;
            }
            if (lowered.rfind("rgb", 0) == 0)
            {
                std::snprintf(buffer, sizeof(buffer), "rgb(%d, %d, %d)", r, g, b);
                return buffer;
            }

            std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", r, g, b, a);
            return buffer;
        }

        static const UINodeRes* FindRootNode(const UIRes& res)
        {
            for (const UINodeRes& node : res.Nodes)
            {
                if (node.Type == UIWidgetType::Canvas)
                    return &node;
            }

            return res.Nodes.empty() ? nullptr : &res.Nodes.front();
        }

        static glm::vec2 ResolveCanvasSize(const UINodeRes* root)
        {
            if (!root)
                return glm::vec2(kDefaultCanvasWidth, kDefaultCanvasHeight);

            glm::vec2 canvasSize = ResolveUILayoutSize(root->Layout);
            if (canvasSize.x <= 0.0f || canvasSize.y <= 0.0f)
                canvasSize = glm::vec2(kDefaultCanvasWidth, kDefaultCanvasHeight);
            return canvasSize;
        }

        static std::unordered_map<int, std::vector<const UINodeRes*>> BuildChildrenMap(const UIRes& res)
        {
            std::unordered_map<int, std::vector<const UINodeRes*>> childrenMap;
            for (const UINodeRes& node : res.Nodes)
            {
                if (node.ParentId >= 0)
                    childrenMap[node.ParentId].push_back(&node);
            }
            return childrenMap;
        }

        static std::unordered_map<int, PreviewNodeState> BuildPreviewStates(const UIRes& res,
            const ImVec2& canvasOrigin,
            float canvasScale,
            std::unordered_map<int, std::vector<const UINodeRes*>>* outChildrenMap = nullptr,
            const UINodeRes** outRoot = nullptr,
            glm::vec2* outCanvasSize = nullptr)
        {
            auto childrenMap = BuildChildrenMap(res);
            std::unordered_map<int, PreviewNodeState> states;
            const UINodeRes* root = FindRootNode(res);
            const glm::vec2 canvasSize = ResolveCanvasSize(root);

            if (root)
            {
                UILayoutRect rootRect;
                rootRect.Position = glm::vec2(0.0f, 0.0f);
                rootRect.Size = canvasSize;
                CollectPreviewStatesRecursive(*root, rootRect, childrenMap, canvasOrigin, canvasScale, states);
            }

            if (outChildrenMap)
                *outChildrenMap = childrenMap;
            if (outRoot)
                *outRoot = root;
            if (outCanvasSize)
                *outCanvasSize = canvasSize;

            return states;
        }

        static void ApplyLayoutChangePreservingRect(UIRes& res,
            UINodeRes& node,
            glm::vec2 anchorMin,
            glm::vec2 anchorMax,
            const glm::vec2& pivot)
        {
            SanitizeAnchors(anchorMin, anchorMax);
            const glm::vec2 sanitizedPivot = SanitizePivot(pivot);

            const std::unordered_map<int, PreviewNodeState> states = BuildPreviewStates(res, ImVec2(0.0f, 0.0f), 1.0f);
            const glm::vec2 canvasSize = ResolveCanvasSize(FindRootNode(res));
            const glm::vec2 parentSize = ResolveParentSize(node, canvasSize, states);
            const UILayoutRect relativeRect = ResolveRelativeRect(node, parentSize, states);
            ApplyLayoutFromRect(node, relativeRect, parentSize, anchorMin, anchorMax, sanitizedPivot);
        }

        static glm::vec2 ResolveParentSize(const UINodeRes& node,
            const glm::vec2& canvasSize,
            const std::unordered_map<int, PreviewNodeState>& states)
        {
            if (node.ParentId < 0)
                return canvasSize;

            const auto parentIt = states.find(node.ParentId);
            if (parentIt == states.end())
                return canvasSize;
            return parentIt->second.LocalRect.Size;
        }

        static UILayoutRect ResolveRelativeRect(const UINodeRes& node,
            const glm::vec2& parentSize,
            const std::unordered_map<int, PreviewNodeState>& states)
        {
            const auto stateIt = states.find(node.Id);
            if (stateIt == states.end())
                return ResolveUILayoutRect(node.Layout, parentSize);

            UILayoutRect rect = stateIt->second.LocalRect;
            if (node.ParentId >= 0)
            {
                const auto parentIt = states.find(node.ParentId);
                if (parentIt != states.end())
                    rect.Position -= parentIt->second.LocalRect.Position;
            }
            return rect;
        }

        static void ApplyLayoutFromRect(UINodeRes& node,
            const UILayoutRect& relativeRect,
            const glm::vec2& parentSize,
            const glm::vec2& anchorMin,
            const glm::vec2& anchorMax,
            const glm::vec2& pivot)
        {
            glm::vec2 sanitizedAnchorMin = anchorMin;
            glm::vec2 sanitizedAnchorMax = anchorMax;
            SanitizeAnchors(sanitizedAnchorMin, sanitizedAnchorMax);

            node.Layout.AnchorMin = sanitizedAnchorMin;
            node.Layout.AnchorMax = sanitizedAnchorMax;
            node.Layout.Pivot = SanitizePivot(pivot);

            for (int axis = 0; axis < 2; ++axis)
            {
                if (node.Layout.AnchorMin[axis] == node.Layout.AnchorMax[axis])
                {
                    node.Layout.OffsetMax[axis] = std::max(relativeRect.Size[axis], 0.0f);
                    node.Layout.OffsetMin[axis] = relativeRect.Position[axis] - parentSize[axis] * node.Layout.AnchorMin[axis]
                        + node.Layout.Pivot[axis] * node.Layout.OffsetMax[axis];
                }
                else
                {
                    node.Layout.OffsetMin[axis] = relativeRect.Position[axis] - parentSize[axis] * node.Layout.AnchorMin[axis];
                    node.Layout.OffsetMax[axis] = relativeRect.Position[axis] + relativeRect.Size[axis]
                        - parentSize[axis] * node.Layout.AnchorMax[axis];
                }
            }
        }

        static void DrawCheckerboard(ImDrawList* drawList,
            const ImVec2& min,
            const ImVec2& max,
            float cellSize,
            ImU32 colorA,
            ImU32 colorB)
        {
            if (cellSize <= 0.0f)
                return;

            drawList->PushClipRect(min, max, true);
            int rowIndex = 0;
            for (float y = min.y; y < max.y; y += cellSize, ++rowIndex)
            {
                int columnIndex = 0;
                for (float x = min.x; x < max.x; x += cellSize, ++columnIndex)
                {
                    const ImU32 color = ((rowIndex + columnIndex) % 2 == 0) ? colorA : colorB;
                    const ImVec2 cellMin(x, y);
                    const ImVec2 cellMax(std::min(x + cellSize, max.x), std::min(y + cellSize, max.y));
                    drawList->AddRectFilled(cellMin, cellMax, color);
                }
            }
            drawList->PopClipRect();
        }

        static void DrawPreviewGrid(ImDrawList* drawList,
            const ImVec2& canvasOrigin,
            const ImVec2& scaledCanvasSize,
            const glm::vec2& canvasSize,
            float canvasScale)
        {
            if (canvasScale <= 0.0f)
                return;

            drawList->PushClipRect(canvasOrigin, ImVec2(canvasOrigin.x + scaledCanvasSize.x, canvasOrigin.y + scaledCanvasSize.y), true);

            const int verticalLines = static_cast<int>(std::ceil(std::max(canvasSize.x, 0.0f) / kPreviewGridStep));
            for (int i = 0; i <= verticalLines; ++i)
            {
                const float x = canvasOrigin.x + i * kPreviewGridStep * canvasScale;
                const ImU32 color = (i % kPreviewGridMajorLineEvery == 0) ? IM_COL32(94, 102, 116, 150) : IM_COL32(70, 76, 88, 90);
                drawList->AddLine(ImVec2(x, canvasOrigin.y), ImVec2(x, canvasOrigin.y + scaledCanvasSize.y), color);
            }

            const int horizontalLines = static_cast<int>(std::ceil(std::max(canvasSize.y, 0.0f) / kPreviewGridStep));
            for (int i = 0; i <= horizontalLines; ++i)
            {
                const float y = canvasOrigin.y + i * kPreviewGridStep * canvasScale;
                const ImU32 color = (i % kPreviewGridMajorLineEvery == 0) ? IM_COL32(94, 102, 116, 150) : IM_COL32(70, 76, 88, 90);
                drawList->AddLine(ImVec2(canvasOrigin.x, y), ImVec2(canvasOrigin.x + scaledCanvasSize.x, y), color);
            }

            drawList->PopClipRect();
        }

        static void DrawSelectedLayoutGizmo(const UINodeRes& node,
            const std::unordered_map<int, PreviewNodeState>& states,
            ImDrawList* drawList,
            float canvasScale)
        {
            const auto stateIt = states.find(node.Id);
            if (stateIt == states.end())
                return;

            const PreviewNodeState& state = stateIt->second;
            const float pivotHalfSize = ImGuiWrapper::S(6.0f);
            const float anchorRadius = ImGuiWrapper::S(4.0f);

            const ImVec2 pivotPos(
                state.ScreenMin.x + node.Layout.Pivot.x * state.LocalRect.Size.x * canvasScale,
                state.ScreenMin.y + node.Layout.Pivot.y * state.LocalRect.Size.y * canvasScale);

            if (node.ParentId >= 0)
            {
                const auto parentIt = states.find(node.ParentId);
                if (parentIt != states.end())
                {
                    const PreviewNodeState& parentState = parentIt->second;
                    drawList->AddRect(parentState.ScreenMin, parentState.ScreenMax, IM_COL32(130, 140, 150, 96), 0.0f, 0, 1.0f);

                    const ImVec2 anchorMinPos(
                        parentState.ScreenMin.x + node.Layout.AnchorMin.x * parentState.LocalRect.Size.x * canvasScale,
                        parentState.ScreenMin.y + node.Layout.AnchorMin.y * parentState.LocalRect.Size.y * canvasScale);
                    const ImVec2 anchorMaxPos(
                        parentState.ScreenMin.x + node.Layout.AnchorMax.x * parentState.LocalRect.Size.x * canvasScale,
                        parentState.ScreenMin.y + node.Layout.AnchorMax.y * parentState.LocalRect.Size.y * canvasScale);

                    if (NearlyEqual(node.Layout.AnchorMin, node.Layout.AnchorMax))
                    {
                        drawList->AddCircleFilled(anchorMinPos, anchorRadius, IM_COL32(255, 196, 64, 255));
                        drawList->AddLine(anchorMinPos, pivotPos, IM_COL32(255, 196, 64, 180), 1.0f);
                    }
                    else
                    {
                        const ImVec2 rectMin(std::min(anchorMinPos.x, anchorMaxPos.x), std::min(anchorMinPos.y, anchorMaxPos.y));
                        const ImVec2 rectMax(std::max(anchorMinPos.x, anchorMaxPos.x), std::max(anchorMinPos.y, anchorMaxPos.y));
                        drawList->AddRectFilled(rectMin, rectMax, IM_COL32(255, 196, 64, 24));
                        drawList->AddRect(rectMin, rectMax, IM_COL32(255, 196, 64, 220), 0.0f, 0, 1.0f);
                        drawList->AddCircleFilled(anchorMinPos, anchorRadius, IM_COL32(255, 196, 64, 255));
                        drawList->AddCircleFilled(anchorMaxPos, anchorRadius, IM_COL32(255, 196, 64, 255));
                        const ImVec2 anchorCenter((rectMin.x + rectMax.x) * 0.5f, (rectMin.y + rectMax.y) * 0.5f);
                        drawList->AddLine(anchorCenter, pivotPos, IM_COL32(255, 196, 64, 180), 1.0f);
                    }
                }
            }

            drawList->AddLine(ImVec2(pivotPos.x - pivotHalfSize, pivotPos.y), ImVec2(pivotPos.x + pivotHalfSize, pivotPos.y), IM_COL32(255, 90, 90, 255), 1.5f);
            drawList->AddLine(ImVec2(pivotPos.x, pivotPos.y - pivotHalfSize), ImVec2(pivotPos.x, pivotPos.y + pivotHalfSize), IM_COL32(255, 90, 90, 255), 1.5f);
        }

        static bool HitAnchorMinHandle(const UINodeRes& node,
            const std::unordered_map<int, PreviewNodeState>& states,
            const ImVec2& point,
            float radius)
        {
            if (node.ParentId < 0)
                return false;

            const auto parentIt = states.find(node.ParentId);
            if (parentIt == states.end())
                return false;

            const PreviewNodeState& parentState = parentIt->second;
            const ImVec2 anchorMinPos(
                parentState.ScreenMin.x + node.Layout.AnchorMin.x * (parentState.ScreenMax.x - parentState.ScreenMin.x),
                parentState.ScreenMin.y + node.Layout.AnchorMin.y * (parentState.ScreenMax.y - parentState.ScreenMin.y));
            const ImVec2 delta(point.x - anchorMinPos.x, point.y - anchorMinPos.y);
            return ImLengthSqr(delta) <= radius * radius;
        }

        static bool HitAnchorMaxHandle(const UINodeRes& node,
            const std::unordered_map<int, PreviewNodeState>& states,
            const ImVec2& point,
            float radius)
        {
            if (node.ParentId < 0 || NearlyEqual(node.Layout.AnchorMin, node.Layout.AnchorMax))
                return false;

            const auto parentIt = states.find(node.ParentId);
            if (parentIt == states.end())
                return false;

            const PreviewNodeState& parentState = parentIt->second;
            const ImVec2 anchorMaxPos(
                parentState.ScreenMin.x + node.Layout.AnchorMax.x * (parentState.ScreenMax.x - parentState.ScreenMin.x),
                parentState.ScreenMin.y + node.Layout.AnchorMax.y * (parentState.ScreenMax.y - parentState.ScreenMin.y));
            const ImVec2 delta(point.x - anchorMaxPos.x, point.y - anchorMaxPos.y);
            return ImLengthSqr(delta) <= radius * radius;
        }

        static bool HitPivotHandle(const UINodeRes& node,
            const std::unordered_map<int, PreviewNodeState>& states,
            const ImVec2& point,
            float radius)
        {
            const auto stateIt = states.find(node.Id);
            if (stateIt == states.end())
                return false;

            const PreviewNodeState& state = stateIt->second;
            const ImVec2 pivotPos(
                state.ScreenMin.x + node.Layout.Pivot.x * (state.ScreenMax.x - state.ScreenMin.x),
                state.ScreenMin.y + node.Layout.Pivot.y * (state.ScreenMax.y - state.ScreenMin.y));
            const ImVec2 delta(point.x - pivotPos.x, point.y - pivotPos.y);
            return ImLengthSqr(delta) <= radius * radius;
        }

        static PreviewNodeState MakePreviewNodeState(const UINodeRes& node, const UILayoutRect& parentRect, const ImVec2& canvasOrigin, float canvasScale)
        {
            PreviewNodeState state;
            if (node.Type == UIWidgetType::Canvas && node.ParentId < 0)
            {
                state.LocalRect = parentRect;
            }
            else
            {
                UILayoutRect localRect = ResolveUILayoutRect(node.Layout, parentRect.Size);
                state.LocalRect.Position = parentRect.Position + localRect.Position;
                state.LocalRect.Size = localRect.Size;
            }

            state.ScreenMin = ImVec2(
                canvasOrigin.x + state.LocalRect.Position.x * canvasScale,
                canvasOrigin.y + state.LocalRect.Position.y * canvasScale);
            state.ScreenMax = ImVec2(
                state.ScreenMin.x + state.LocalRect.Size.x * canvasScale,
                state.ScreenMin.y + state.LocalRect.Size.y * canvasScale);
            return state;
        }

        static void CollectPreviewStatesRecursive(const UINodeRes& node,
            const UILayoutRect& parentRect,
            const std::unordered_map<int, std::vector<const UINodeRes*>>& childrenMap,
            const ImVec2& canvasOrigin,
            float canvasScale,
            std::unordered_map<int, PreviewNodeState>& outStates)
        {
            const PreviewNodeState state = MakePreviewNodeState(node, parentRect, canvasOrigin, canvasScale);
            outStates[node.Id] = state;

            const auto childIt = childrenMap.find(node.Id);
            if (childIt == childrenMap.end())
                return;

            for (const UINodeRes* child : childIt->second)
                CollectPreviewStatesRecursive(*child, state.LocalRect, childrenMap, canvasOrigin, canvasScale, outStates);
        }

        static bool ContainsPoint(const PreviewNodeState& state, const ImVec2& point)
        {
            return point.x >= state.ScreenMin.x && point.y >= state.ScreenMin.y &&
                point.x <= state.ScreenMax.x && point.y <= state.ScreenMax.y;
        }

        static const UINodeRes* HitTestNodeRecursive(const UINodeRes& node,
            const std::unordered_map<int, std::vector<const UINodeRes*>>& childrenMap,
            const std::unordered_map<int, PreviewNodeState>& states,
            const ImVec2& point)
        {
            if (!node.bVisible)
                return nullptr;

            const auto childIt = childrenMap.find(node.Id);
            if (childIt != childrenMap.end())
            {
                for (auto it = childIt->second.rbegin(); it != childIt->second.rend(); ++it)
                {
                    if (const UINodeRes* hit = HitTestNodeRecursive(**it, childrenMap, states, point))
                        return hit;
                }
            }

            const auto stateIt = states.find(node.Id);
            if (stateIt != states.end() && ContainsPoint(stateIt->second, point))
                return &node;
            return nullptr;
        }

        static ImGuiTextureHandle LoadPreviewTexture(const std::string& assetPath)
        {
            const std::string normalized = Utils::NormalizeAssetPath(assetPath);
            if (normalized.empty())
                return {};

            const std::filesystem::path fullPath = ConfigManager::GetFullPath(normalized);
            if (!std::filesystem::exists(fullPath))
                return {};

            return IconManager::GetInstance().LoadOrFindTexture(normalized);
        }

        static ImVec4 ResolvePreviewColor(const std::string& color, const ImVec4& fallback)
        {
            ImVec4 parsed = fallback;
            if (!TryParseCssColor(color, parsed))
                parsed = fallback;
            return parsed;
        }

        static bool AdjustAnchorValue(float& value, float speed)
        {
            float localValue = value;
            if (!ImGui::DragFloat("##Value", &localValue, speed, 0.0f, 1.0f, "%.2f"))
                return false;

            value = std::clamp(localValue, 0.0f, 1.0f);
            return true;
        }

        static bool AdjustVector2(glm::vec2& value, float speed, float minValue, float maxValue, const char* format)
        {
            float temp[2] = { value.x, value.y };
            if (!ImGui::DragFloat2("##Value", temp, speed, minValue, maxValue, format))
                return false;

            value = glm::vec2(temp[0], temp[1]);
            return true;
        }

        static void DrawPreviewNodeRecursive(const UINodeRes& node,
            const std::unordered_map<int, std::vector<const UINodeRes*>>& childrenMap,
            const std::unordered_map<int, PreviewNodeState>& states,
            ImDrawList* drawList,
            float canvasScale,
            int selectedNodeId)
        {
            if (!node.bVisible)
                return;

            const auto stateIt = states.find(node.Id);
            if (stateIt == states.end())
                return;

            const PreviewNodeState& state = stateIt->second;
            const ImVec2 size = ImVec2(state.ScreenMax.x - state.ScreenMin.x, state.ScreenMax.y - state.ScreenMin.y);
            const float rounding = std::min(node.Style.BorderRadius * canvasScale, 0.5f * std::min(size.x, size.y));

            if (node.Type != UIWidgetType::Canvas)
            {
                const ImVec4 backgroundColor = ResolvePreviewColor(node.Style.BackgroundColor, ImVec4(0.18f, 0.18f, 0.18f, 0.0f));
                const bool bTreatAsTransparentTextBackground = node.Type == UIWidgetType::Text && node.Style.BackgroundImage.empty();
                const ImVec4 effectiveBackgroundColor = bTreatAsTransparentTextBackground
                    ? ImVec4(backgroundColor.x, backgroundColor.y, backgroundColor.z, 0.0f)
                    : backgroundColor;
                DrawCheckerboard(
                    drawList,
                    state.ScreenMin,
                    state.ScreenMax,
                    ImGuiWrapper::S(kPreviewCheckerSize),
                    IM_COL32(72, 76, 86, 120),
                    IM_COL32(52, 56, 66, 120));
                const ImU32 fillColor = ImGui::GetColorU32(effectiveBackgroundColor);
                if (effectiveBackgroundColor.w > 0.0f)
                    drawList->AddRectFilled(state.ScreenMin, state.ScreenMax, fillColor, rounding);

                std::string imagePath;
                if (node.Type == UIWidgetType::Image)
                    imagePath = node.ImagePath;
                else if (!node.Style.BackgroundImage.empty())
                    imagePath = node.Style.BackgroundImage;

                if (const ImGuiTextureHandle texture = LoadPreviewTexture(imagePath))
                {
                    drawList->AddImage(IconManager::ToImTextureID(texture), state.ScreenMin, state.ScreenMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
                }
                else if (node.Type == UIWidgetType::Image)
                {
                    drawList->AddRectFilled(state.ScreenMin, state.ScreenMax, IM_COL32(75, 80, 88, 255), rounding);
                    drawList->AddLine(state.ScreenMin, state.ScreenMax, IM_COL32(180, 180, 180, 180), 1.5f);
                    drawList->AddLine(ImVec2(state.ScreenMax.x, state.ScreenMin.y), ImVec2(state.ScreenMin.x, state.ScreenMax.y), IM_COL32(180, 180, 180, 180), 1.5f);
                }

                const ImVec4 borderColor = ResolvePreviewColor(node.Style.BorderColor, ImVec4(0.8f, 0.8f, 0.8f, 0.0f));
                const float borderThickness = std::max(node.Style.BorderWidth * canvasScale, 1.0f);
                if (node.Style.BorderWidth > 0.0f && borderColor.w > 0.0f)
                    drawList->AddRect(state.ScreenMin, state.ScreenMax, ImGui::GetColorU32(borderColor), rounding, 0, borderThickness);
                else if (effectiveBackgroundColor.w <= 0.0f && imagePath.empty())
                    drawList->AddRect(state.ScreenMin, state.ScreenMax, IM_COL32(160, 160, 160, 80), rounding, 0, 1.0f);

                if (!node.Text.empty())
                {
                    const ImVec4 textColor = ResolvePreviewColor(node.Style.TextColor, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    const float fontSize = std::max(node.Style.FontSize * canvasScale, 8.0f);
                    const float padding = std::max(canvasScale * 4.0f, 2.0f);
                    const float wrapWidth = std::max(size.x - padding * 2.0f, 0.0f);

                    ImVec2 textPos = ImVec2(state.ScreenMin.x + padding, state.ScreenMin.y + padding);
                    if (node.Type == UIWidgetType::Button)
                    {
                        const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, wrapWidth, node.Text.c_str());
                        textPos = ImVec2(
                            state.ScreenMin.x + std::max((size.x - textSize.x) * 0.5f, padding),
                            state.ScreenMin.y + std::max((size.y - textSize.y) * 0.5f, padding));
                    }

                    drawList->AddText(ImGui::GetFont(), fontSize, textPos, ImGui::GetColorU32(textColor), node.Text.c_str(), nullptr, wrapWidth);
                }

                if (selectedNodeId == node.Id)
                {
                    drawList->AddRect(state.ScreenMin, state.ScreenMax, IM_COL32(82, 168, 255, 255), rounding, 0, 2.0f);
                    const float handleSize = ImGuiWrapper::S(kResizeHandleSize);
                    const ImVec2 handleMin(state.ScreenMax.x - handleSize, state.ScreenMax.y - handleSize);
                    drawList->AddRectFilled(handleMin, state.ScreenMax, IM_COL32(82, 168, 255, 255), 2.0f);
                }
            }

            const auto childIt = childrenMap.find(node.Id);
            if (childIt == childrenMap.end())
                return;

            if (node.Type != UIWidgetType::Canvas)
                drawList->PushClipRect(state.ScreenMin, state.ScreenMax, true);

            for (const UINodeRes* child : childIt->second)
                DrawPreviewNodeRecursive(*child, childrenMap, states, drawList, canvasScale, selectedNodeId);

            if (node.Type != UIWidgetType::Canvas)
                drawList->PopClipRect();
        }
    }

    std::string UIPanel::BuildPanelKey(const std::string& uiPath)
    {
        return std::string("UI:") + Utils::NormalizeAssetPath(uiPath);
    }

    UIPanel::UIPanel(EditorLayer* parent, std::string uiPath)
        : BasePanel(parent, BuildPanelKey(uiPath), &mOpen), mUiPath(Utils::NormalizeAssetPath(uiPath))
    {
        mUiFullPath = ConfigManager::GetFullPath(mUiPath);
        std::string err;
        if (!std::filesystem::exists(mUiFullPath) || !LoadUIRes(mUiFullPath, mUiRes, &err))
        {
            mUiRes.Name = mUiFullPath.stem().string();
            EnsureDefaultCanvas();
            mDirty = true;
        }
        else if (mUiRes.Nodes.empty())
        {
            EnsureDefaultCanvas();
        }

		if (!mUiRes.Nodes.empty())
			mSelectedNodeId = mUiRes.Nodes.front().Id;

		ResetHistory("Open UI");
    }

    void UIPanel::OnImGuiRender()
    {
        if (!(*pOpen))
            return;

        const std::string title = std::string(EditorLayer::GetText("UI")) + " - " +
            std::filesystem::path(mUiPath).filename().string() + "###UIPanel:" + mUiPath;
        ImGuiWrapper::SetNextWindowSizeConstraints(ImVec2(880.0f, 560.0f), ImVec2(FLT_MAX, FLT_MAX));
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

    void UIPanel::OnImGuiRenderImpl()
    {
        DrawToolbar();
        ImGui::Separator();

        const ImGuiTableFlags flags = ImGuiTableFlags_Resizable |
            ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("UISplit", 3, flags))
        {
            ImGui::TableSetupColumn("##UIHierarchy", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("##UIPreview", ImGuiTableColumnFlags_WidthStretch, 1.8f);
            ImGui::TableSetupColumn("##UIInspector", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            DrawHierarchy();

            ImGui::TableSetColumnIndex(1);
            DrawPreview();

            ImGui::TableSetColumnIndex(2);
            DrawInspector();

			ImGui::EndTable();
		}

		if (mPendingInteractiveHistory && !ImGui::IsAnyItemActive())
		{
			mHistory.CaptureSnapshot(SerializeSnapshot(),
				mPendingInteractiveHistoryLabel.empty() ? "Edit UI" : mPendingInteractiveHistoryLabel.c_str(),
				false);
			mPendingInteractiveHistory = false;
			mPendingInteractiveHistoryLabel.clear();
		}
    }

    void UIPanel::DrawToolbar()
    {
		EditorTheme::DrawSectionHeader(EditorLayer::GetText("UI Builder"), std::filesystem::path(mUiPath).filename().string().c_str());
		ImGui::Spacing();

		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGuiWrapper::S(6.0f));
		const std::string addPanelLabel = std::string(EditorLayer::GetText("Add Panel")) + "###AddPanel";
		const std::string addTextLabel = std::string(EditorLayer::GetText("Add Text")) + "###AddText";
		const std::string addImageLabel = std::string(EditorLayer::GetText("Add Image")) + "###AddImage";
		const std::string addButtonLabel = std::string(EditorLayer::GetText("Add Button")) + "###AddButton";
		const std::string deleteLabel = std::string(EditorLayer::GetText("Delete")) + "###Delete";
		const std::string saveLabel = std::string(EditorLayer::GetText("Save")) + "###Save";
		if (ImGui::Button(addPanelLabel.c_str(), ImGuiWrapper::S(92.0f, 0.0f))) AddNode(UIWidgetType::Panel);
		ImGui::SameLine();
		if (ImGui::Button(addTextLabel.c_str(), ImGuiWrapper::S(92.0f, 0.0f))) AddNode(UIWidgetType::Text);
		ImGui::SameLine();
		if (ImGui::Button(addImageLabel.c_str(), ImGuiWrapper::S(92.0f, 0.0f))) AddNode(UIWidgetType::Image);
		ImGui::SameLine();
		if (ImGui::Button(addButtonLabel.c_str(), ImGuiWrapper::S(92.0f, 0.0f))) AddNode(UIWidgetType::Button);
		ImGui::SameLine();
		if (mSelectedNodeId <= 1)
			ImGui::BeginDisabled();
		if (ImGui::Button(deleteLabel.c_str(), ImGuiWrapper::S(84.0f, 0.0f)) && mSelectedNodeId > 1) DeleteSelectedNode();
		if (mSelectedNodeId <= 1)
			ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(saveLabel.c_str(), ImGuiWrapper::S(84.0f, 0.0f))) Save();
		ImGui::PopStyleVar();

		ImGui::SameLine();
		EditorTheme::DrawPill(EditorLayer::GetText(mDirty ? "Modified" : "Saved"),
			mDirty ? ImVec4(0.42f, 0.25f, 0.12f, 0.95f) : ImVec4(0.16f, 0.33f, 0.24f, 0.95f));
    }

    void UIPanel::DrawHierarchy()
    {
        ImGui::BeginChild("##UIHierarchy");
        for (const UINodeRes& node : mUiRes.Nodes)
        {
            if (node.ParentId >= 0)
                continue;

            DrawHierarchyNode(node);
        }
        ImGui::EndChild();
    }

    void UIPanel::DrawHierarchyNode(const UINodeRes& node)
    {
        bool bHasChildren = false;
        for (const UINodeRes& child : mUiRes.Nodes)
        {
            if (child.ParentId == node.Id)
            {
                bHasChildren = true;
                break;
            }
        }

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!bHasChildren)
            flags |= ImGuiTreeNodeFlags_Leaf;
        if (mSelectedNodeId == node.Id)
            flags |= ImGuiTreeNodeFlags_Selected;

        const char* defaultName = WidgetTypeName(node.Type);
        const char* displayName = node.Name == defaultName
            ? EditorLayer::GetText(defaultName)
            : node.Name.c_str();
        const bool bOpen = ImGui::TreeNodeEx((void*)(intptr_t)node.Id, flags, "%s", displayName);
        if (ImGui::IsItemClicked())
            mSelectedNodeId = node.Id;

        if (!bOpen)
            return;

        for (const UINodeRes& child : mUiRes.Nodes)
        {
            if (child.ParentId == node.Id)
                DrawHierarchyNode(child);
        }

        ImGui::TreePop();
    }

    void UIPanel::DrawPreview()
    {
        ImGui::BeginChild("##UIPreview", ImVec2(0.0f, 0.0f), true);
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 childPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        const UINodeRes* root = FindRootNode(mUiRes);
        const glm::vec2 canvasSize = ResolveCanvasSize(root);
        const float previewPadding = ImGuiWrapper::S(kPreviewPadding);
        const float resizeHandleSize = ImGuiWrapper::S(kResizeHandleSize);

        ImGui::InvisibleButton("##UIPreviewCanvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        const bool bPreviewHovered = ImGui::IsItemHovered();
        const bool bPreviewActive = ImGui::IsItemActive();
        const ImVec2 mousePos = ImGui::GetIO().MousePos;

		if (bPreviewHovered && std::abs(ImGui::GetIO().MouseWheel) > FLT_EPSILON)
		{
			mPreviewZoom = std::clamp(mPreviewZoom + ImGui::GetIO().MouseWheel * 0.1f, kMinPreviewZoom, kMaxPreviewZoom);
			CaptureHistory("UI Preview Zoom", true);
		}

        const ImVec2 paddedAvail(std::max(avail.x - previewPadding * 2.0f, 1.0f), std::max(avail.y - previewPadding * 2.0f, 1.0f));
        const float fitScale = std::min(paddedAvail.x / std::max(canvasSize.x, 1.0f), paddedAvail.y / std::max(canvasSize.y, 1.0f));
        const float canvasScale = std::max(fitScale * mPreviewZoom, 0.01f);
        const ImVec2 scaledCanvasSize(canvasSize.x * canvasScale, canvasSize.y * canvasScale);
        const ImVec2 centeredCanvasOrigin(
            childPos.x + std::max((avail.x - scaledCanvasSize.x) * 0.5f, 0.0f),
            childPos.y + std::max((avail.y - scaledCanvasSize.y) * 0.5f, 0.0f));
        const ImVec2 canvasOrigin(
            centeredCanvasOrigin.x + mPreviewPan.x,
            centeredCanvasOrigin.y + mPreviewPan.y);

        std::unordered_map<int, std::vector<const UINodeRes*>> childrenMap;
        std::unordered_map<int, PreviewNodeState> states = BuildPreviewStates(mUiRes, canvasOrigin, canvasScale, &childrenMap);

        drawList->AddRectFilled(childPos, ImVec2(childPos.x + avail.x, childPos.y + avail.y), IM_COL32(28, 30, 35, 255));
        drawList->AddRect(childPos, ImVec2(childPos.x + avail.x, childPos.y + avail.y), IM_COL32(90, 96, 110, 255));
        drawList->AddRectFilled(canvasOrigin, ImVec2(canvasOrigin.x + scaledCanvasSize.x, canvasOrigin.y + scaledCanvasSize.y), IM_COL32(40, 44, 52, 255));
        drawList->AddRect(canvasOrigin, ImVec2(canvasOrigin.x + scaledCanvasSize.x, canvasOrigin.y + scaledCanvasSize.y), IM_COL32(120, 128, 142, 255));
        DrawPreviewGrid(drawList, canvasOrigin, scaledCanvasSize, canvasSize, canvasScale);

        if (root)
        {
            DrawPreviewNodeRecursive(*root, childrenMap, states, drawList, canvasScale, mSelectedNodeId);
            if (const UINodeRes* selectedNode = FindNode(mSelectedNodeId))
                DrawSelectedLayoutGizmo(*selectedNode, states, drawList, canvasScale);
        }

        const UINodeRes* hitNode = nullptr;
        if (root && bPreviewHovered)
            hitNode = HitTestNodeRecursive(*root, childrenMap, states, mousePos);

        const float gizmoRadius = ImGuiWrapper::S(8.0f);
        if (bPreviewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mSelectedNodeId > 0)
        {
            if (const UINodeRes* selectedNode = FindNode(mSelectedNodeId))
            {
                if (HitPivotHandle(*selectedNode, states, mousePos, gizmoRadius))
                {
                    mPreviewManipulation = PreviewManipulation::Pivot;
                    mPreviewInteractionNodeId = selectedNode->Id;
                    mLastPreviewMousePos = glm::vec2(mousePos.x, mousePos.y);
                }
                else if (HitAnchorMaxHandle(*selectedNode, states, mousePos, gizmoRadius))
                {
                    mPreviewManipulation = PreviewManipulation::AnchorMax;
                    mPreviewInteractionNodeId = selectedNode->Id;
                    mLastPreviewMousePos = glm::vec2(mousePos.x, mousePos.y);
                }
                else if (HitAnchorMinHandle(*selectedNode, states, mousePos, gizmoRadius))
                {
                    mPreviewManipulation = PreviewManipulation::AnchorMin;
                    mPreviewInteractionNodeId = selectedNode->Id;
                    mLastPreviewMousePos = glm::vec2(mousePos.x, mousePos.y);
                }
            }
        }

        if (bPreviewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        {
            mPreviewManipulation = PreviewManipulation::Pan;
            mPreviewInteractionNodeId = -1;
            mLastPreviewMousePos = glm::vec2(mousePos.x, mousePos.y);
        }

        if (bPreviewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mPreviewManipulation == PreviewManipulation::None)
        {
            if (hitNode)
            {
                mSelectedNodeId = hitNode->Id;
                mPreviewInteractionNodeId = hitNode->Id;

                const auto stateIt = states.find(hitNode->Id);
                if (stateIt != states.end())
                {
                    const PreviewNodeState& state = stateIt->second;
                    const bool bResizeHandle = mousePos.x >= state.ScreenMax.x - resizeHandleSize && mousePos.y >= state.ScreenMax.y - resizeHandleSize;
                    mPreviewManipulation = bResizeHandle ? PreviewManipulation::Resize : PreviewManipulation::Move;
                }
            }
            else
            {
                mPreviewInteractionNodeId = -1;
                mPreviewManipulation = PreviewManipulation::None;
            }

            mLastPreviewMousePos = glm::vec2(mousePos.x, mousePos.y);
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Middle) || !bPreviewActive)
        {
            mPreviewManipulation = PreviewManipulation::None;
            mPreviewInteractionNodeId = -1;
        }

		if (mPreviewManipulation == PreviewManipulation::Pan && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
		{
			const glm::vec2 currentMouse(mousePos.x, mousePos.y);
			const glm::vec2 delta = currentMouse - mLastPreviewMousePos;
			mPreviewPan += delta;
			mLastPreviewMousePos = currentMouse;
			CaptureHistory("UI Preview Pan", true);
		}
        else if ((mPreviewManipulation == PreviewManipulation::AnchorMin ||
            mPreviewManipulation == PreviewManipulation::AnchorMax ||
            mPreviewManipulation == PreviewManipulation::Pivot) &&
            mPreviewInteractionNodeId > 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (UINodeRes* node = FindNode(mPreviewInteractionNodeId))
            {
                const auto stateIt = states.find(node->Id);
                if (stateIt != states.end())
                {
                    const PreviewNodeState& state = stateIt->second;
                    const glm::vec2 currentMouse(mousePos.x, mousePos.y);

					if (mPreviewManipulation == PreviewManipulation::Pivot)
					{
						glm::vec2 newPivot(
                            (currentMouse.x - state.ScreenMin.x) / std::max(state.ScreenMax.x - state.ScreenMin.x, 1.0f),
                            (currentMouse.y - state.ScreenMin.y) / std::max(state.ScreenMax.y - state.ScreenMin.y, 1.0f));
						ApplyLayoutChangePreservingRect(mUiRes, *node, node->Layout.AnchorMin, node->Layout.AnchorMax, newPivot);
						CaptureHistory("Move Pivot", true);
					}
                    else if (node->ParentId >= 0)
                    {
                        const auto parentIt = states.find(node->ParentId);
                        if (parentIt != states.end())
                        {
                            const PreviewNodeState& parentState = parentIt->second;
                            glm::vec2 normalizedAnchor(
                                (currentMouse.x - parentState.ScreenMin.x) / std::max(parentState.ScreenMax.x - parentState.ScreenMin.x, 1.0f),
                                (currentMouse.y - parentState.ScreenMin.y) / std::max(parentState.ScreenMax.y - parentState.ScreenMin.y, 1.0f));

							if (mPreviewManipulation == PreviewManipulation::AnchorMin)
								ApplyLayoutChangePreservingRect(mUiRes, *node, normalizedAnchor, node->Layout.AnchorMax, node->Layout.Pivot);
							else
								ApplyLayoutChangePreservingRect(mUiRes, *node, node->Layout.AnchorMin, normalizedAnchor, node->Layout.Pivot);
							CaptureHistory(mPreviewManipulation == PreviewManipulation::AnchorMin ? "Move Anchor Min" : "Move Anchor Max", true);
						}
					}

                    mLastPreviewMousePos = currentMouse;
                    mDirty = true;
                }
            }
        }
        else if (mPreviewManipulation != PreviewManipulation::None && mPreviewInteractionNodeId > 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (UINodeRes* node = FindNode(mPreviewInteractionNodeId))
            {
                const glm::vec2 currentMouse(mousePos.x, mousePos.y);
                const glm::vec2 delta = (currentMouse - mLastPreviewMousePos) / canvasScale;
                const glm::vec2 parentSize = ResolveParentSize(*node, canvasSize, states);
                UILayoutRect relativeRect = ResolveRelativeRect(*node, parentSize, states);

				if (mPreviewManipulation == PreviewManipulation::Move)
				{
					relativeRect.Position += delta;
					CaptureHistory("Move Widget", true);
				}
				else if (mPreviewManipulation == PreviewManipulation::Resize)
				{
					relativeRect.Size = glm::max(relativeRect.Size + delta, glm::vec2(kMinNodeSize, kMinNodeSize));
					CaptureHistory("Resize Widget", true);
				}

                ApplyLayoutFromRect(*node, relativeRect, parentSize, node->Layout.AnchorMin, node->Layout.AnchorMax, node->Layout.Pivot);
                mLastPreviewMousePos = currentMouse;
                mDirty = true;
            }
        }

        const std::string canvasLabel = std::string(EditorLayer::GetText("Canvas")) + " " +
            std::to_string((int)canvasSize.x) + "x" + std::to_string((int)canvasSize.y);
        const std::string zoomLabel = std::string(EditorLayer::GetText("Zoom")) + " " +
            std::to_string((int)std::round(mPreviewZoom * 100.0f)) + "%";
        drawList->AddText(ImVec2(childPos.x + ImGuiWrapper::S(8.0f), childPos.y + ImGuiWrapper::S(8.0f)), IM_COL32(220, 220, 220, 255), canvasLabel.c_str());
        drawList->AddText(ImVec2(childPos.x + ImGuiWrapper::S(8.0f), childPos.y + ImGuiWrapper::S(26.0f)), IM_COL32(170, 190, 220, 255), zoomLabel.c_str());
        drawList->AddText(ImVec2(childPos.x + ImGuiWrapper::S(8.0f), childPos.y + ImGuiWrapper::S(44.0f)), IM_COL32(150, 165, 185, 255), EditorLayer::GetText("LMB move/resize  MMB pan  Wheel zoom"));
        ImGui::EndChild();
    }

    void UIPanel::DrawInspector()
    {
        ImGui::BeginChild("##UIInspector");
        UINodeRes* node = FindNode(mSelectedNodeId);
        if (!node)
        {
            ImGui::TextDisabled("%s", EditorLayer::GetText("No Selection"));
            ImGui::EndChild();
            return;
        }

        const auto browseAsset = [](const char* filter, const std::string& currentValue) -> std::string
        {
            const std::filesystem::path initialDir = currentValue.empty()
                ? ConfigManager::GetFullPath("Assets")
                : ConfigManager::GetFullPath(Utils::NormalizeAssetPath(currentValue)).parent_path();
            const std::filesystem::path selected = FileDialogs::OpenFile(filter, initialDir.string());
            if (selected.empty())
                return {};
            return Utils::NormalizeAssetPath(ConfigManager::GetRelativePath(selected));
        };

        auto applyAnchorPreset = [&](const AnchorPreset& preset)
        {
            ApplyLayoutChangePreservingRect(mUiRes, *node, preset.AnchorMin, preset.AnchorMax, preset.Pivot);
            CaptureHistory("Apply Anchor Preset");
        };

        auto applyPivotPreset = [&](const PivotPreset& preset)
        {
            ApplyLayoutChangePreservingRect(mUiRes, *node, node->Layout.AnchorMin, node->Layout.AnchorMax, preset.Pivot);
            CaptureHistory("Apply Pivot Preset");
        };

        auto drawColorProperty = [&](const char* label, std::string& value)
        {
            ImVec4 color;
            const bool bParsed = TryParseCssColor(value, color);
            if (!bParsed)
                color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

            ImGuiWrapper::PropertyRow(EditorLayer::GetText(label), [&]()
            {
                if (ImGui::ColorEdit4((std::string("##") + label).c_str(), &color.x, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf))
                {
                    value = FormatCssColor(color, value);
                    CaptureHistory("Change UI Color");
                }
            });
        };

        auto drawPathProperty = [&](const char* label, std::string& value, const std::string& filter)
        {
            char buffer[260] = {};
            std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
            ImGuiWrapper::PropertyRow(EditorLayer::GetText(label), [&]()
            {
                ImGui::PushID(label);
                ImGui::SetNextItemWidth(-ImGuiWrapper::S(84.0f));
                if (ImGui::InputText("##Path", buffer, sizeof(buffer)))
                {
                    value = Utils::NormalizeAssetPath(buffer);
                    CaptureHistory("Change Asset Path");
                }
                ImGui::SameLine(0.0f, ImGuiWrapper::S(6.0f));
                const std::string browseLabel = std::string(EditorLayer::GetText("Browse")) + "###Browse";
                if (ImGui::Button(browseLabel.c_str(), ImGuiWrapper::S(78.0f, 0.0f)))
                {
                    const std::string selected = browseAsset(filter.c_str(), value);
                    if (!selected.empty())
                    {
                        value = selected;
                        CaptureHistory("Pick Asset Path");
                    }
                }
                ImGui::PopID();
            }, 118.0f);
        };

        char nameBuffer[256] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", node->Name.c_str());
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Name"), [&]() { if (ImGui::InputText("##Name", nameBuffer, sizeof(nameBuffer))) { node->Name = nameBuffer; CaptureHistory("Rename UI Widget"); } });

        char idBuffer[256] = {};
        std::snprintf(idBuffer, sizeof(idBuffer), "%s", node->ElementId.c_str());
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Element Id"), [&]() { if (ImGui::InputText("##ElementId", idBuffer, sizeof(idBuffer))) { node->ElementId = idBuffer; CaptureHistory("Change Element Id"); } });

        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Type"), [&]() { ImGui::TextUnformatted(EditorLayer::GetText(WidgetTypeName(node->Type))); });
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Visible"), [&]() { if (ImGui::Checkbox("##Visible", &node->bVisible)) CaptureHistory("Toggle Visibility"); });
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Interactable"), [&]() { if (ImGui::Checkbox("##Interactable", &node->bInteractable)) CaptureHistory("Toggle Interactable"); });

        ImGui::Separator();
        ImGui::TextUnformatted(EditorLayer::GetText("Layout"));

        const bool bStretchX = IsStretchX(*node);
        const bool bStretchY = IsStretchY(*node);

        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Anchor Preset"), [&]()
        {
            auto drawPresetRow = [&](const auto& presets, int columns, const char* rowId)
            {
                ImGui::PushID(rowId);
                if (ImGui::BeginTable("##AnchorPresetRow", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX))
                {
                    int index = 0;
                    for (const auto& preset : presets)
                    {
                        ImGui::TableNextColumn();
                        const bool bSelected = MatchesAnchorPreset(*node, preset);
                        if (bSelected)
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.45f, 0.72f, 1.0f));
                        const std::string presetLabel = std::string(EditorLayer::GetText(preset.Label)) + "###" + preset.Label;
                        if (ImGui::Button(presetLabel.c_str(), ImVec2(-FLT_MIN, 0.0f)))
                            applyAnchorPreset(preset);
                        if (bSelected)
                            ImGui::PopStyleColor();
                        ++index;
                    }
                    while (index < columns)
                    {
                        ImGui::TableNextColumn();
                        ++index;
                    }
                    ImGui::EndTable();
                }
                ImGui::PopID();
            };

            drawPresetRow(kFixedAnchorPresets, 3, "FixedAnchors");
            drawPresetRow(kHorizontalStretchAnchorPresets, 3, "HorizontalStretchAnchors");
            drawPresetRow(kVerticalStretchAnchorPresets, 3, "VerticalStretchAnchors");
            drawPresetRow(kFullStretchAnchorPreset, 1, "FullStretchAnchor");
        });

        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Anchor Min"), [&]()
        {
            glm::vec2 newAnchorMin = node->Layout.AnchorMin;
            if (AdjustVector2(newAnchorMin, 0.01f, 0.0f, 1.0f, "%.2f"))
            {
                ApplyLayoutChangePreservingRect(mUiRes, *node, newAnchorMin, node->Layout.AnchorMax, node->Layout.Pivot);
                CaptureHistory("Edit Anchor Min", true);
            }
        });
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Anchor Max"), [&]()
        {
            glm::vec2 newAnchorMax = node->Layout.AnchorMax;
            if (AdjustVector2(newAnchorMax, 0.01f, 0.0f, 1.0f, "%.2f"))
            {
                ApplyLayoutChangePreservingRect(mUiRes, *node, node->Layout.AnchorMin, newAnchorMax, node->Layout.Pivot);
                CaptureHistory("Edit Anchor Max", true);
            }
        });
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Pivot"), [&]()
        {
            glm::vec2 newPivot = node->Layout.Pivot;
            if (AdjustVector2(newPivot, 0.01f, 0.0f, 1.0f, "%.2f"))
            {
                ApplyLayoutChangePreservingRect(mUiRes, *node, node->Layout.AnchorMin, node->Layout.AnchorMax, newPivot);
                CaptureHistory("Edit Pivot", true);
            }
        });
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Pivot Preset"), [&]()
        {
            if (ImGui::BeginTable("##PivotPresetRow", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadInnerX))
            {
                for (const PivotPreset& preset : kPivotPresets)
                {
                    ImGui::TableNextColumn();
                    const bool bSelected = MatchesPivotPreset(*node, preset);
                    if (bSelected)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.45f, 0.72f, 1.0f));
                    const std::string presetLabel = std::string(EditorLayer::GetText(preset.Label)) + "###" + preset.Label;
                    if (ImGui::Button(presetLabel.c_str(), ImVec2(-FLT_MIN, 0.0f)))
                        applyPivotPreset(preset);
                    if (bSelected)
                        ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }
        });
        ImGuiWrapper::PropertyRow(EditorLayer::GetText(bStretchX || bStretchY ? "Offsets" : "Position"), [&]()
        {
            if (AdjustVector2(node->Layout.OffsetMin, 1.0f, 0.0f, 0.0f, "%.0f"))
                CaptureHistory("Edit Offset Min", true);
        });
        ImGuiWrapper::PropertyRow(EditorLayer::GetText(bStretchX || bStretchY ? "Size Delta" : "Size"), [&]()
        {
            if (AdjustVector2(node->Layout.OffsetMax, 1.0f, 0.0f, 0.0f, "%.0f"))
            {
                if (!bStretchX) node->Layout.OffsetMax.x = std::max(node->Layout.OffsetMax.x, kMinNodeSize);
                if (!bStretchY) node->Layout.OffsetMax.y = std::max(node->Layout.OffsetMax.y, kMinNodeSize);
                CaptureHistory("Edit Size", true);
            }
        });

        ImGui::Separator();
        ImGui::TextUnformatted(EditorLayer::GetText("Content"));

        char textBuffer[512] = {};
        std::snprintf(textBuffer, sizeof(textBuffer), "%s", node->Text.c_str());
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Text"), [&]()
        {
            if (ImGui::InputTextMultiline("##Text", textBuffer, sizeof(textBuffer), ImVec2(-FLT_MIN, ImGuiWrapper::S(90.0f))))
            {
                node->Text = textBuffer;
                CaptureHistory("Edit Text");
            }
        });

        const std::string imageFilter = BuildFileDialogFilter("Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga");
        const std::string fontFilter = BuildFileDialogFilter("Font", "*.ttf;*.otf");
        drawPathProperty("Image", node->ImagePath, imageFilter);

        char classBuffer[256] = {};
        std::snprintf(classBuffer, sizeof(classBuffer), "%s", node->Style.ClassName.c_str());
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Class"), [&]() { if (ImGui::InputText("##Class", classBuffer, sizeof(classBuffer))) { node->Style.ClassName = classBuffer; CaptureHistory("Edit UI Class"); } });

        ImGui::Separator();
        ImGui::TextUnformatted(EditorLayer::GetText("Style"));

        drawColorProperty("Background", node->Style.BackgroundColor);
        drawColorProperty("Text Color", node->Style.TextColor);
        drawColorProperty("Border Color", node->Style.BorderColor);
        drawPathProperty("Font", node->Style.FontPath, fontFilter);
        drawPathProperty("Background Image", node->Style.BackgroundImage, imageFilter);

        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Font Size"), [&]() { if (ImGui::DragFloat("##FontSize", &node->Style.FontSize, 1.0f, 1.0f, 256.0f, "%.0f")) CaptureHistory("Edit Font Size", true); });
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Border Radius"), [&]() { if (ImGui::DragFloat("##BorderRadius", &node->Style.BorderRadius, 1.0f, 0.0f, 200.0f, "%.0f")) CaptureHistory("Edit Border Radius", true); });
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Border Width"), [&]() { if (ImGui::DragFloat("##BorderWidth", &node->Style.BorderWidth, 1.0f, 0.0f, 100.0f, "%.0f")) CaptureHistory("Edit Border Width", true); });

        char extraStyleBuffer[512] = {};
        std::snprintf(extraStyleBuffer, sizeof(extraStyleBuffer), "%s", node->Style.ExtraStyle.c_str());
        ImGuiWrapper::PropertyRow(EditorLayer::GetText("Extra Style"), [&]()
        {
            if (ImGui::InputTextMultiline("##ExtraStyle", extraStyleBuffer, sizeof(extraStyleBuffer), ImVec2(-FLT_MIN, ImGuiWrapper::S(80.0f))))
            {
                node->Style.ExtraStyle = extraStyleBuffer;
                CaptureHistory("Edit Extra Style");
            }
        });

        ImGui::EndChild();
    }

    void UIPanel::EnsureDefaultCanvas()
    {
        if (!mUiRes.Nodes.empty())
            return;

        UINodeRes canvas;
        canvas.Id = 1;
        canvas.Type = UIWidgetType::Canvas;
        canvas.Name = "Canvas";
        canvas.ElementId = "canvas_root";
        canvas.Style.ClassName = "hui-canvas";
        canvas.Layout.AnchorMin = glm::vec2(0.0f, 0.0f);
        canvas.Layout.AnchorMax = glm::vec2(1.0f, 1.0f);
        canvas.Layout.OffsetMin = glm::vec2(0.0f, 0.0f);
        canvas.Layout.OffsetMax = glm::vec2(1280.0f, 720.0f);
        mUiRes.Nodes.push_back(canvas);
    }

    UINodeRes* UIPanel::FindNode(int nodeId)
    {
        auto it = std::find_if(mUiRes.Nodes.begin(), mUiRes.Nodes.end(), [&](const UINodeRes& node) { return node.Id == nodeId; });
        return it != mUiRes.Nodes.end() ? &(*it) : nullptr;
    }

    const UINodeRes* UIPanel::FindNode(int nodeId) const
    {
        auto it = std::find_if(mUiRes.Nodes.begin(), mUiRes.Nodes.end(), [&](const UINodeRes& node) { return node.Id == nodeId; });
        return it != mUiRes.Nodes.end() ? &(*it) : nullptr;
    }

    int UIPanel::AllocateNodeId() const
    {
        int maxId = 0;
        for (const UINodeRes& node : mUiRes.Nodes)
            maxId = std::max(maxId, node.Id);
        return maxId + 1;
    }

    void UIPanel::AddNode(UIWidgetType type)
    {
        EnsureDefaultCanvas();
        UINodeRes node;
        node.Id = AllocateNodeId();
        node.ParentId = mSelectedNodeId > 0 ? mSelectedNodeId : 1;
        node.Type = type;
        node.Name = WidgetTypeName(type);
        node.ElementId = std::string("node_") + std::to_string(node.Id);
        node.Style.ClassName = std::string("hui-") + std::to_string(node.Id);
        node.Layout.OffsetMin = glm::vec2(80.0f + (float)(node.Id * 10), 80.0f + (float)(node.Id * 8));
        node.Layout.OffsetMax = glm::vec2(220.0f, 64.0f);
        if (type == UIWidgetType::Text)
            node.Text = "Label";
        else if (type == UIWidgetType::Button)
            node.Text = "Button";
		mUiRes.Nodes.push_back(node);
		mSelectedNodeId = node.Id;
		mDirty = true;
		CaptureHistory("Add Widget");
    }

    void UIPanel::DeleteSelectedNode()
    {
        if (mSelectedNodeId <= 1)
            return;

        std::unordered_set<int> idsToDelete = { mSelectedNodeId };
        bool bChanged = true;
        while (bChanged)
        {
            bChanged = false;
            for (const UINodeRes& node : mUiRes.Nodes)
            {
                if (idsToDelete.contains(node.ParentId) && !idsToDelete.contains(node.Id))
                {
                    idsToDelete.insert(node.Id);
                    bChanged = true;
                }
            }
        }

		mUiRes.Nodes.erase(
			std::remove_if(mUiRes.Nodes.begin(), mUiRes.Nodes.end(), [&](const UINodeRes& node) { return idsToDelete.contains(node.Id); }),
			mUiRes.Nodes.end());

		mSelectedNodeId = !mUiRes.Nodes.empty() ? mUiRes.Nodes.front().Id : -1;
		mDirty = true;
		CaptureHistory("Delete Widget");
    }

    void UIPanel::Save()
    {
        std::string err;
		bool bSaved = false;
		try
		{
			const RuntimeUiPath path = ResolveRuntimeUiWrittenPath(
				ConfigManager::GetInstance().GetRootFolder(), mUiFullPath);
			bSaved = SaveRuntimeUiDefinition(RuntimeResourceRegistry::GetInstance(), path, mUiRes, &err);
		}
		catch (const std::exception& exception)
		{
			err = exception.what();
		}

		if (bSaved)
		{
			mDirty = false;
			mSavedSnapshot = SerializeSnapshot();
			HE_CORE_INFO("UIPanel: saved '{0}'", mUiPath);
		}
        else
		{
			HE_CORE_ERROR("UIPanel: save failed '{0}', {1}", mUiPath, err);
		}
    }

	void UIPanel::CaptureHistory(const char* label, bool bInteractive)
	{
		mDirty = true;
		if (bInteractive)
		{
			mPendingInteractiveHistory = true;
			mPendingInteractiveHistoryLabel = label ? label : "Edit UI";
			return;
		}

		mPendingInteractiveHistory = false;
		mPendingInteractiveHistoryLabel.clear();
		mHistory.CaptureSnapshot(SerializeSnapshot(), label ? label : "Edit UI", false);
	}

	void UIPanel::ResetHistory(const char* label)
	{
		mSavedSnapshot = SerializeSnapshot();
		mHistory.Reset(mSavedSnapshot, label ? label : "Open UI");
		mDirty = false;
	}

	std::string UIPanel::SerializeSnapshot() const
	{
		Json root;
		Serializer::Serialize(root["UiRes"], mUiRes);
		return root.dump();
	}

	bool UIPanel::ApplySnapshot(const std::string& snapshot)
	{
		try
		{
			const Json root = Json::parse(snapshot);
			Serializer::Deserialize(root.at("UiRes"), mUiRes);
			if (!FindNode(mSelectedNodeId))
				mSelectedNodeId = !mUiRes.Nodes.empty() ? mUiRes.Nodes.front().Id : -1;
			mDirty = SerializeSnapshot() != mSavedSnapshot;
			return true;
		}
		catch (const std::exception& ex)
		{
			HE_CORE_ERROR("UIPanel: apply snapshot failed '{0}', {1}", mUiPath, ex.what());
			return false;
		}
	}

	bool UIPanel::CanUndo() const
	{
		return mHistory.CanUndo();
	}

	bool UIPanel::CanRedo() const
	{
		return mHistory.CanRedo();
	}

	bool UIPanel::Undo()
	{
		return mHistory.Undo([this](const std::string& snapshot) { return ApplySnapshot(snapshot); });
	}

	bool UIPanel::Redo()
	{
		return mHistory.Redo([this](const std::string& snapshot) { return ApplySnapshot(snapshot); });
	}

	bool UIPanel::IsUndoContextDirty() const
	{
		return mDirty;
	}
}
