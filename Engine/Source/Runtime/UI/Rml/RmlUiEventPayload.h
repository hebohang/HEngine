#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace HEngine
{
    using RmlUiEventPayloadValue = std::variant<std::string, float, int>;

    struct RmlUiEventPayloadData
    {
        std::string DocumentId;
        std::string EventType;
        std::string TargetElementId;
        std::string CurrentElementId;
        std::string Property;
        std::optional<std::string> AnimationName;
        std::optional<float> ElapsedTime;
        std::optional<float> MouseX;
        std::optional<float> MouseY;
        std::optional<float> WheelDelta;
        std::optional<int> MouseButton;
    };

    inline std::vector<std::pair<std::string, RmlUiEventPayloadValue>>
        BuildRmlUiEventPayloadFields(const RmlUiEventPayloadData& data)
    {
        std::vector<std::pair<std::string, RmlUiEventPayloadValue>> fields = {
            { "documentId", data.DocumentId },
            { "eventType", data.EventType },
            { "targetElementId", data.TargetElementId },
            { "currentElementId", data.CurrentElementId },
            { "property", data.Property }
        };
        if (data.AnimationName)
            fields.emplace_back("animationName", *data.AnimationName);
        if (data.ElapsedTime)
            fields.emplace_back("elapsedTime", *data.ElapsedTime);
        if (data.MouseX)
            fields.emplace_back("mouseX", *data.MouseX);
        if (data.MouseY)
            fields.emplace_back("mouseY", *data.MouseY);
        if (data.WheelDelta)
            fields.emplace_back("wheelDelta", *data.WheelDelta);
        if (data.MouseButton)
            fields.emplace_back("mouseButton", *data.MouseButton);
        return fields;
    }
}
