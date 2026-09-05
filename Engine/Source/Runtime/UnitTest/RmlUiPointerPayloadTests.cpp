#include "Hepch.h"

#include "Runtime/UnitTest/RmlUiPointerPayloadTests.h"
#include "Runtime/UI/Rml/RmlUiEventPayload.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        const RmlUiEventPayloadValue* FindField(
            const std::vector<std::pair<std::string, RmlUiEventPayloadValue>>& fields,
            std::string_view name)
        {
            for (const auto& [fieldName, value] : fields)
            {
                if (fieldName == name)
                    return &value;
            }
            return nullptr;
        }
    }

    void RmlUiPointerPayloadContract()
    {
        RmlUiEventPayloadData pointer;
        pointer.DocumentId = "showcase";
        pointer.EventType = "mousescroll";
        pointer.TargetElementId = "preview_surface";
        pointer.CurrentElementId = "hub_root";
        pointer.MouseX = 0.0f;
        pointer.MouseY = 719.0f;
        pointer.WheelDelta = -1.5f;
        pointer.MouseButton = 1;
        const auto pointerFields = BuildRmlUiEventPayloadFields(pointer);
        Require(std::get<std::string>(*FindField(pointerFields, "targetElementId")) ==
            "preview_surface", "Pointer payload lost the exact event target.");
        Require(std::get<std::string>(*FindField(pointerFields, "currentElementId")) ==
            "hub_root", "Pointer payload lost the current event element.");
        Require(std::get<float>(*FindField(pointerFields, "mouseX")) == 0.0f,
            "A valid zero pointer coordinate was treated as absent.");
        Require(std::get<float>(*FindField(pointerFields, "mouseY")) == 719.0f,
            "Pointer Y coordinate changed during payload construction.");
        Require(std::get<float>(*FindField(pointerFields, "wheelDelta")) == -1.5f,
            "Wheel delta changed during payload construction.");
        Require(std::get<int>(*FindField(pointerFields, "mouseButton")) == 1,
            "Mouse button changed during payload construction.");

        RmlUiEventPayloadData animation;
        animation.DocumentId = "showcase";
        animation.EventType = "animationend";
        animation.TargetElementId = "page_characters";
        animation.CurrentElementId = "page_characters";
        animation.AnimationName = "hub_enter";
        animation.ElapsedTime = 0.25f;
        const auto animationFields = BuildRmlUiEventPayloadFields(animation);
        Require(FindField(animationFields, "animationName") != nullptr,
            "Animation payload lost its authored name.");
        Require(FindField(animationFields, "mouseX") == nullptr &&
            FindField(animationFields, "wheelDelta") == nullptr,
            "Non-pointer event retained stale pointer fields.");
    }
}
