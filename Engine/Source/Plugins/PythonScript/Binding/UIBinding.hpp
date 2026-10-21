#pragma once

#include <pybind11/embed.h>
#include <pybind11/stl.h>
#include <imgui.h>
#include <unordered_map>
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/EcsFramework/System/UI/UIViewportContext.h"
#include "Runtime/Graphics/Gui/NvrhiImGuiRenderer.h"

#include <algorithm>

namespace py = pybind11;

namespace HEngine
{
    static void UIBinding(py::module_& HEngineModule)
    {
        py::module_ ui = HEngineModule.def_submodule("UI");

        auto scaleValue = [](float value)
        {
            return value * UIViewportContext::UIScale;
        };

        auto scaleSize = [&](float w, float h)
        {
            return ImVec2(scaleValue(w), scaleValue(h));
        };

        auto scaleIfPositive = [&](float value)
        {
            return value > 0.0f ? scaleValue(value) : value;
        };

        auto scaleIfNonNegative = [&](float value)
        {
            return value >= 0.0f ? scaleValue(value) : value;
        };

        // -----------------------------------------------------------------
        // Window management
        // -----------------------------------------------------------------
        ui.def("Begin", [](const char* name, int flags) -> bool {
            return ImGui::Begin(name, nullptr, (ImGuiWindowFlags)flags);
        }, py::arg("name"), py::arg("flags") = 0);

        ui.def("End", []() { ImGui::End(); });

        ui.def("BeginChild", [](const char* id, float w, float h, bool border) -> bool {
            return ImGui::BeginChild(id, ImVec2(w, h), border);
        }, py::arg("id"), py::arg("w") = 0.0f, py::arg("h") = 0.0f, py::arg("border") = false);

        ui.def("BeginChildScaled", [=](const char* id, float w, float h, bool border) -> bool {
            return ImGui::BeginChild(id, scaleSize(w, h), border);
        }, py::arg("id"), py::arg("w") = 0.0f, py::arg("h") = 0.0f, py::arg("border") = false);

        ui.def("EndChild", []() { ImGui::EndChild(); });

        // -----------------------------------------------------------------
        // Text
        // -----------------------------------------------------------------
        ui.def("Text", [](const char* text) {
            ImGui::TextUnformatted(text);
        });

        ui.def("TextColored", [](float r, float g, float b, float a, const char* text) {
            ImGui::TextColored(ImVec4(r, g, b, a), "%s", text);
        });

        ui.def("LabelText", [](const char* label, const char* text) {
            ImGui::LabelText(label, "%s", text);
        });

        ui.def("BulletText", [](const char* text) {
            ImGui::BulletText("%s", text);
        });

        // -----------------------------------------------------------------
        // Buttons & toggles
        // -----------------------------------------------------------------
        ui.def("Button", [](const char* label, float w, float h) -> bool {
            return ImGui::Button(label, ImVec2(w, h));
        }, py::arg("label"), py::arg("w") = 0.0f, py::arg("h") = 0.0f);

        ui.def("ButtonScaled", [=](const char* label, float w, float h) -> bool {
            return ImGui::Button(label, scaleSize(w, h));
        }, py::arg("label"), py::arg("w") = 0.0f, py::arg("h") = 0.0f);

        ui.def("SmallButton", [](const char* label) -> bool {
            return ImGui::SmallButton(label);
        });

        ui.def("Checkbox", [](const char* label, bool v) -> std::pair<bool, bool> {
            bool changed = ImGui::Checkbox(label, &v);
            return { changed, v };
        });

        // -----------------------------------------------------------------
        // Sliders & inputs
        // -----------------------------------------------------------------
        ui.def("SliderFloat", [](const char* label, float v, float vMin, float vMax) -> std::pair<bool, float> {
            bool changed = ImGui::SliderFloat(label, &v, vMin, vMax);
            return { changed, v };
        });

        ui.def("SliderInt", [](const char* label, int v, int vMin, int vMax) -> std::pair<bool, int> {
            bool changed = ImGui::SliderInt(label, &v, vMin, vMax);
            return { changed, v };
        });

        ui.def("InputFloat", [](const char* label, float v) -> std::pair<bool, float> {
            bool changed = ImGui::InputFloat(label, &v);
            return { changed, v };
        });

        ui.def("InputInt", [](const char* label, int v) -> std::pair<bool, int> {
            bool changed = ImGui::InputInt(label, &v);
            return { changed, v };
        });

        ui.def("InputText", [](const char* label, std::string s) -> std::pair<bool, std::string> {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", s.c_str());
            bool changed = ImGui::InputText(label, buf, sizeof(buf));
            return { changed, std::string(buf) };
        });

        ui.def("DragFloat", [](const char* label, float v, float speed, float vMin, float vMax) -> std::pair<bool, float> {
            bool changed = ImGui::DragFloat(label, &v, speed, vMin, vMax);
            return { changed, v };
        }, py::arg("label"), py::arg("v"), py::arg("speed") = 1.0f, py::arg("vMin") = 0.0f, py::arg("vMax") = 0.0f);

        ui.def("DragFloatScaled", [=](const char* label, float v, float speed, float vMin, float vMax) -> std::pair<bool, float> {
            bool changed = ImGui::DragFloat(label, &v, scaleValue(speed), vMin, vMax);
            return { changed, v };
        }, py::arg("label"), py::arg("v"), py::arg("speed") = 1.0f, py::arg("vMin") = 0.0f, py::arg("vMax") = 0.0f);

        ui.def("DragInt", [](const char* label, int v, float speed, int vMin, int vMax) -> std::pair<bool, int> {
            bool changed = ImGui::DragInt(label, &v, speed, vMin, vMax);
            return { changed, v };
        }, py::arg("label"), py::arg("v"), py::arg("speed") = 1.0f, py::arg("vMin") = 0, py::arg("vMax") = 0);

        ui.def("DragIntScaled", [=](const char* label, int v, float speed, int vMin, int vMax) -> std::pair<bool, int> {
            bool changed = ImGui::DragInt(label, &v, scaleValue(speed), vMin, vMax);
            return { changed, v };
        }, py::arg("label"), py::arg("v"), py::arg("speed") = 1.0f, py::arg("vMin") = 0, py::arg("vMax") = 0);

        // -----------------------------------------------------------------
        // Progress
        // -----------------------------------------------------------------
        ui.def("ProgressBar", [](float fraction, float w, float h, const char* overlay) {
            ImGui::ProgressBar(fraction, ImVec2(w, h), overlay[0] == '\0' ? nullptr : overlay);
        }, py::arg("fraction"), py::arg("w") = -1.0f, py::arg("h") = 0.0f, py::arg("overlay") = "");

        ui.def("ProgressBarScaled", [=](float fraction, float w, float h, const char* overlay) {
            ImGui::ProgressBar(fraction, ImVec2(w, scaleIfNonNegative(h)), overlay[0] == '\0' ? nullptr : overlay);
        }, py::arg("fraction"), py::arg("w") = -1.0f, py::arg("h") = 0.0f, py::arg("overlay") = "");

        // -----------------------------------------------------------------
        // Image
        // -----------------------------------------------------------------
        static std::unordered_map<std::string, ImGuiTextureHandle> sUITextureCache;

        ui.def("LoadTexture", [](const std::string& relativePath) {
            if (sUITextureCache.find(relativePath) == sUITextureCache.end()) {
                const ImGuiTextureHandle texture =
                    Application::GetInstance().LoadImGuiTexture(relativePath, true);
                if (texture)
                    sUITextureCache[relativePath] = texture;
            }
        }, py::arg("relativePath"));

        ui.def("Image", [](const std::string& relativePath, float w, float h) {
            auto it = sUITextureCache.find(relativePath);
            if (it != sUITextureCache.end() && it->second)
                ImGui::Image(NvrhiImGuiRenderer::ToImTextureID(it->second), ImVec2(w, h));
        }, py::arg("relativePath"), py::arg("w"), py::arg("h"));

        ui.def("ImageScaled", [=](const std::string& relativePath, float w, float h) {
            auto it = sUITextureCache.find(relativePath);
            if (it != sUITextureCache.end() && it->second)
                ImGui::Image(NvrhiImGuiRenderer::ToImTextureID(it->second), scaleSize(w, h));
        }, py::arg("relativePath"), py::arg("w"), py::arg("h"));

        ui.def("ClearTextureCache", []() {
            for (const auto& [path, handle] : sUITextureCache)
                Application::GetInstance().ReleaseImGuiTexture(handle);
            sUITextureCache.clear();
        });

        // -----------------------------------------------------------------
        // Combo / Selectable
        // -----------------------------------------------------------------
        ui.def("BeginCombo", [](const char* label, const char* previewValue) -> bool {
            return ImGui::BeginCombo(label, previewValue);
        });
        ui.def("EndCombo", []() { ImGui::EndCombo(); });

        ui.def("Selectable", [](const char* label, bool selected) -> bool {
            return ImGui::Selectable(label, selected);
        }, py::arg("label"), py::arg("selected") = false);

        // -----------------------------------------------------------------
        // Tree / Collapsible
        // -----------------------------------------------------------------
        ui.def("CollapsingHeader", [](const char* label) -> bool {
            return ImGui::CollapsingHeader(label);
        });

        ui.def("TreeNode", [](const char* label) -> bool {
            return ImGui::TreeNode(label);
        });

        ui.def("TreePop", []() { ImGui::TreePop(); });

        // -----------------------------------------------------------------
        // Layout helpers
        // -----------------------------------------------------------------
        ui.def("Separator", []() { ImGui::Separator(); });
        ui.def("SameLine", [](float offsetX, float spacing) {
            ImGui::SameLine(offsetX, spacing);
        }, py::arg("offsetX") = 0.0f, py::arg("spacing") = -1.0f);
        ui.def("SameLineScaled", [=](float offsetX, float spacing) {
            const float scaledOffset = scaleIfPositive(offsetX);
            const float scaledSpacing = spacing >= 0.0f ? scaleValue(spacing) : spacing;
            ImGui::SameLine(scaledOffset, scaledSpacing);
        }, py::arg("offsetX") = 0.0f, py::arg("spacing") = -1.0f);
        ui.def("NewLine", []() { ImGui::NewLine(); });
        ui.def("Spacing", []() { ImGui::Spacing(); });
        ui.def("SpacingScaled", [=](float count) {
            const int n = std::max(0, (int)std::round(scaleIfPositive(count)));
            for (int i = 0; i < n; ++i)
                ImGui::Spacing();
        }, py::arg("count") = 1.0f);
        ui.def("Indent", [](float w) { ImGui::Indent(w); }, py::arg("w") = 0.0f);
        ui.def("Unindent", [](float w) { ImGui::Unindent(w); }, py::arg("w") = 0.0f);
        ui.def("IndentScaled", [=](float w) { ImGui::Indent(scaleIfPositive(w)); }, py::arg("w") = 0.0f);
        ui.def("UnindentScaled", [=](float w) { ImGui::Unindent(scaleIfPositive(w)); }, py::arg("w") = 0.0f);

        ui.def("BeginGroup", []() { ImGui::BeginGroup(); });
        ui.def("EndGroup", []() { ImGui::EndGroup(); });

        ui.def("Columns", [](int count, bool border) {
            ImGui::Columns(count, nullptr, border);
        }, py::arg("count") = 1, py::arg("border") = true);
        ui.def("NextColumn", []() { ImGui::NextColumn(); });
        ui.def("SetColumnWidth", [](int idx, float w) { ImGui::SetColumnWidth(idx, w); });
        ui.def("SetColumnWidthScaled", [=](int idx, float w) { ImGui::SetColumnWidth(idx, scaleValue(w)); });

        // -----------------------------------------------------------------
        // Window/cursor positioning
        // -----------------------------------------------------------------
        ui.def("SetNextWindowPos", [](float x, float y) {
            ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        });

        ui.def("SetNextWindowPosScaled", [=](float x, float y) {
            ImGui::SetNextWindowPos(scaleSize(x, y), ImGuiCond_Always);
        });

        ui.def("SetNextWindowSize", [](float w, float h) {
            ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        });
        ui.def("SetNextWindowSizeScaled", [=](float w, float h) {
            ImGui::SetNextWindowSize(scaleSize(w, h), ImGuiCond_Always);
        });
        ui.def("SetNextWindowBgAlpha", [](float alpha) {
            ImGui::SetNextWindowBgAlpha(alpha);
        });
        ui.def("SetNextWindowSizeConstraints", [](float minW, float minH, float maxW, float maxH) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(minW, minH), ImVec2(maxW, maxH));
        });
        ui.def("SetNextWindowSizeConstraintsScaled", [=](float minW, float minH, float maxW, float maxH) {
            ImGui::SetNextWindowSizeConstraints(scaleSize(minW, minH), scaleSize(maxW, maxH));
        });
        ui.def("SetCursorPos", [](float x, float y) { ImGui::SetCursorPos(ImVec2(x, y)); });
        ui.def("SetCursorPosScaled", [=](float x, float y) { ImGui::SetCursorPos(scaleSize(x, y)); });
        ui.def("GetCursorPos", []() -> std::pair<float, float> {
            ImVec2 p = ImGui::GetCursorPos();
            return { p.x, p.y };
        });
        ui.def("GetWindowSize", []() -> std::pair<float, float> {
            ImVec2 s = ImGui::GetWindowSize();
            return { s.x, s.y };
        });
        ui.def("GetContentRegionAvail", []() -> std::pair<float, float> {
            ImVec2 s = ImGui::GetContentRegionAvail();
            return { s.x, s.y };
        });
        ui.def("GetViewportOffset", []() -> std::pair<float, float> {
            return { UIViewportContext::X, UIViewportContext::Y };
        });
        ui.def("GetViewportSize", []() -> std::pair<float, float> {
            return { UIViewportContext::W, UIViewportContext::H };
        });
        ui.def("GetUIScale", []() -> float {
            return UIViewportContext::UIScale;
        });
        ui.def("Scale", [=](float value) -> float {
            return scaleValue(value);
        });

        // -----------------------------------------------------------------
        // Styling
        // -----------------------------------------------------------------
        ui.def("PushStyleColor", [](int idx, float r, float g, float b, float a) {
            ImGui::PushStyleColor(idx, ImVec4(r, g, b, a));
        });
        ui.def("PopStyleColor", [](int count) {
            ImGui::PopStyleColor(count);
        }, py::arg("count") = 1);

        ui.def("PushStyleVar", [](int idx, float val) {
            ImGui::PushStyleVar(idx, val);
        });
        ui.def("PushStyleVar2", [](int idx, float x, float y) {
            ImGui::PushStyleVar(idx, ImVec2(x, y));
        });
        ui.def("PopStyleVar", [](int count) {
            ImGui::PopStyleVar(count);
        }, py::arg("count") = 1);

        // -----------------------------------------------------------------
        // Style color index constants (ImGuiCol_*)
        // -----------------------------------------------------------------
        ui.attr("Col_Text")              = (int)ImGuiCol_Text;
        ui.attr("Col_WindowBg")          = (int)ImGuiCol_WindowBg;
        ui.attr("Col_ChildBg")           = (int)ImGuiCol_ChildBg;
        ui.attr("Col_PopupBg")           = (int)ImGuiCol_PopupBg;
        ui.attr("Col_FrameBg")           = (int)ImGuiCol_FrameBg;
        ui.attr("Col_FrameBgHovered")    = (int)ImGuiCol_FrameBgHovered;
        ui.attr("Col_FrameBgActive")     = (int)ImGuiCol_FrameBgActive;
        ui.attr("Col_TitleBg")           = (int)ImGuiCol_TitleBg;
        ui.attr("Col_TitleBgActive")     = (int)ImGuiCol_TitleBgActive;
        ui.attr("Col_Button")            = (int)ImGuiCol_Button;
        ui.attr("Col_ButtonHovered")     = (int)ImGuiCol_ButtonHovered;
        ui.attr("Col_ButtonActive")      = (int)ImGuiCol_ButtonActive;
        ui.attr("Col_Header")            = (int)ImGuiCol_Header;
        ui.attr("Col_HeaderHovered")     = (int)ImGuiCol_HeaderHovered;
        ui.attr("Col_HeaderActive")      = (int)ImGuiCol_HeaderActive;
        ui.attr("Col_PlotHistogram")     = (int)ImGuiCol_PlotHistogram;

        // -----------------------------------------------------------------
        // StyleVar index constants (ImGuiStyleVar_*)
        // -----------------------------------------------------------------
        ui.attr("StyleVar_Alpha")              = (int)ImGuiStyleVar_Alpha;
        ui.attr("StyleVar_WindowPadding")      = (int)ImGuiStyleVar_WindowPadding;
        ui.attr("StyleVar_WindowRounding")     = (int)ImGuiStyleVar_WindowRounding;
        ui.attr("StyleVar_WindowBorderSize")   = (int)ImGuiStyleVar_WindowBorderSize;
        ui.attr("StyleVar_FramePadding")       = (int)ImGuiStyleVar_FramePadding;
        ui.attr("StyleVar_FrameRounding")      = (int)ImGuiStyleVar_FrameRounding;
        ui.attr("StyleVar_ItemSpacing")        = (int)ImGuiStyleVar_ItemSpacing;
        ui.attr("StyleVar_ItemInnerSpacing")   = (int)ImGuiStyleVar_ItemInnerSpacing;
        ui.attr("StyleVar_IndentSpacing")      = (int)ImGuiStyleVar_IndentSpacing;

        // -----------------------------------------------------------------
        // Window flag constants (ImGuiWindowFlags_*)
        // -----------------------------------------------------------------
        ui.attr("WindowFlags_None")                  = (int)ImGuiWindowFlags_None;
        ui.attr("WindowFlags_NoTitleBar")            = (int)ImGuiWindowFlags_NoTitleBar;
        ui.attr("WindowFlags_NoResize")              = (int)ImGuiWindowFlags_NoResize;
        ui.attr("WindowFlags_NoMove")                = (int)ImGuiWindowFlags_NoMove;
        ui.attr("WindowFlags_NoScrollbar")           = (int)ImGuiWindowFlags_NoScrollbar;
        ui.attr("WindowFlags_NoScrollWithMouse")     = (int)ImGuiWindowFlags_NoScrollWithMouse;
        ui.attr("WindowFlags_NoCollapse")            = (int)ImGuiWindowFlags_NoCollapse;
        ui.attr("WindowFlags_AlwaysAutoResize")      = (int)ImGuiWindowFlags_AlwaysAutoResize;
        ui.attr("WindowFlags_NoBackground")          = (int)ImGuiWindowFlags_NoBackground;
        ui.attr("WindowFlags_NoSavedSettings")       = (int)ImGuiWindowFlags_NoSavedSettings;
        ui.attr("WindowFlags_NoBringToFrontOnFocus") = (int)ImGuiWindowFlags_NoBringToFrontOnFocus;
        ui.attr("WindowFlags_NoDecoration")          = (int)(ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                                            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse);
        ui.attr("WindowFlags_NoInputs")              = (int)ImGuiWindowFlags_NoInputs;
    }
}
