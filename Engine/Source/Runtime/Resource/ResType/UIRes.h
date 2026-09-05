#pragma once

#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <algorithm>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace HEngine
{
    enum class UIWidgetType
    {
        Canvas = 0,
        Panel,
        Text,
        Image,
        Button
    };

    inline void to_json(Json& json, const UIWidgetType& type)
    {
        switch (type)
        {
        case UIWidgetType::Canvas: json = "Canvas"; break;
        case UIWidgetType::Panel: json = "Panel"; break;
        case UIWidgetType::Text: json = "Text"; break;
        case UIWidgetType::Image: json = "Image"; break;
        case UIWidgetType::Button: json = "Button"; break;
        default: json = "Panel"; break;
        }
    }

    inline void from_json(const Json& json, UIWidgetType& type)
    {
        const std::string typeName = json.is_string() ? json.get<std::string>() : std::string("Panel");
        if (typeName == "Canvas") type = UIWidgetType::Canvas;
        else if (typeName == "Text") type = UIWidgetType::Text;
        else if (typeName == "Image") type = UIWidgetType::Image;
        else if (typeName == "Button") type = UIWidgetType::Button;
        else type = UIWidgetType::Panel;
    }

    REFLECTION_TYPE(UIAnchorRect)
    STRUCT(UIAnchorRect, Fields)
    {
        REFLECTION_BODY(UIAnchorRect);
        glm::vec2 AnchorMin = glm::vec2(0.0f, 0.0f);
        glm::vec2 AnchorMax = glm::vec2(0.0f, 0.0f);
        glm::vec2 Pivot = glm::vec2(0.5f, 0.5f);
        glm::vec2 OffsetMin = glm::vec2(0.0f, 0.0f);
        glm::vec2 OffsetMax = glm::vec2(0.0f, 0.0f);
    };

    [[nodiscard]] inline glm::vec2 ResolveUILayoutSize(const UIAnchorRect& layout)
    {
        // The current editor authors OffsetMin as local position and OffsetMax as width / height.
        // Keep runtime export and editor preview aligned with that authored data.
        return glm::max(layout.OffsetMax, glm::vec2(0.0f, 0.0f));
    }

    [[nodiscard]] inline glm::vec2 ResolveUILayoutPosition(const UIAnchorRect& layout)
    {
        return layout.OffsetMin;
    }

    struct UILayoutRect
    {
        glm::vec2 Position = glm::vec2(0.0f, 0.0f);
        glm::vec2 Size = glm::vec2(0.0f, 0.0f);
    };

    [[nodiscard]] inline UILayoutRect ResolveUILayoutRect(const UIAnchorRect& layout, const glm::vec2& parentSize)
    {
        const glm::vec2 anchorMin = parentSize * layout.AnchorMin;
        const glm::vec2 anchorMax = parentSize * layout.AnchorMax;

        UILayoutRect rect;
        if (layout.AnchorMin != layout.AnchorMax)
        {
            rect.Position = anchorMin + layout.OffsetMin;
            rect.Size = glm::max((anchorMax - anchorMin) + layout.OffsetMax - layout.OffsetMin, glm::vec2(0.0f, 0.0f));
            return rect;
        }

        rect.Size = ResolveUILayoutSize(layout);
        rect.Position = anchorMin + ResolveUILayoutPosition(layout) - layout.Pivot * rect.Size;
        return rect;
    }

    REFLECTION_TYPE(UIStyleRes)
    STRUCT(UIStyleRes, Fields)
    {
        REFLECTION_BODY(UIStyleRes);
        std::string ClassName;
        std::string BackgroundColor = "rgba(32, 32, 32, 220)";
        std::string TextColor = "#ffffffff";
        std::string BackgroundImage;
        std::string FontPath = "Assets/Fonts/opensans/OpenSans-Regular.ttf";
        float FontSize = 24.0f;
        float BorderRadius = 0.0f;
        float BorderWidth = 0.0f;
        std::string BorderColor = "#00000000";
        std::string ExtraStyle;
    };

    REFLECTION_TYPE(UINodeRes)
    STRUCT(UINodeRes, Fields)
    {
        REFLECTION_BODY(UINodeRes);
        int Id = 0;
        int ParentId = -1;
        UIWidgetType Type = UIWidgetType::Panel;
        std::string Name = "Element";
        std::string ElementId;
        std::string Text;
        std::string ImagePath;
        bool bInteractable = true;
        bool bVisible = true;
        UIAnchorRect Layout;
        UIStyleRes Style;
    };

    REFLECTION_TYPE(UIRes)
    STRUCT(UIRes, Fields)
    {
        REFLECTION_BODY(UIRes);
        static std::string GetExtName() { return "hui"; }

        int Version = 1;
        std::string Name = "NewUI";
        std::string GeneratedRmlPath;
        std::string GeneratedRcssPath;
        std::vector<UINodeRes> Nodes;
    };

    bool LoadUIRes(const std::filesystem::path& fullPath, UIRes& outRes, std::string* outError = nullptr);
    bool SaveUIRes(const UIRes& res, const std::filesystem::path& fullPath, std::string* outError = nullptr);
    bool ExportUIRes(const UIRes& res, const std::filesystem::path& sourcePath, std::string* outError = nullptr);
}
