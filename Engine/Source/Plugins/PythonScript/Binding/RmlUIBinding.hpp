#pragma once

#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Core/PresentationClock.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/UI/Rml/RmlUiManager.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace py = pybind11;

namespace HEngine
{
    static void RmlUIBinding(py::module_& HEngineModule)
    {
        py::module_ rmlUi = HEngineModule.def_submodule("RmlUI");

        rmlUi.def("GetPresentationTime", []()
        {
            return PresentationClock::GetInstance().GetElapsedSeconds();
        });

        rmlUi.def("GetPresentationDelta", []()
        {
            return PresentationClock::GetInstance().GetDeltaSeconds();
        });

        rmlUi.def("GetRenderCapabilities", []()
        {
            const RmlRenderCapabilities capabilities =
                RmlUiManager::GetInstance().GetRenderCapabilities();
            py::dict result;
            result["scissor"] = capabilities.Scissor;
            result["transform"] = capabilities.Transform;
            result["clipMask"] = capabilities.ClipMask;
            result["clipMaskRequiresStencilAttachment"] =
                capabilities.ClipMaskRequiresStencilAttachment;
            result["layers"] = capabilities.Layers;
            result["filters"] = capabilities.Filters;
            result["maskImage"] = capabilities.MaskImage;
            result["customShaders"] = capabilities.CustomShaders;
            return result;
        });

        rmlUi.def(
            "SetRenderTransition",
            [](const std::string& effect,
                float progress,
                bool bReduceMotion,
                bool bReduceFlash)
            {
                RuntimeUiTransitionEffect effectValue = RuntimeUiTransitionEffect::None;
                if (effect == "none")
                    effectValue = RuntimeUiTransitionEffect::None;
                else if (effect == "diagonal_sticker_wipe")
                    effectValue = RuntimeUiTransitionEffect::DiagonalStickerWipe;
                else if (effect == "star_petal_reveal")
                    effectValue = RuntimeUiTransitionEffect::StarPetalReveal;
                else if (effect == "halftone_dot_reveal")
                    effectValue = RuntimeUiTransitionEffect::HalftoneDotReveal;
                else
                    throw py::value_error("Unknown runtime UI transition effect: " + effect);
                if (!std::isfinite(progress))
                    throw py::value_error("Runtime UI transition progress must be finite");

                RmlUiManager::GetInstance().SetRuntimeUiTransition(
                    RuntimeUiTransitionParameters{
                        .Effect = effectValue,
                        .Progress = std::clamp(progress, 0.0f, 1.0f),
                        .bReduceMotion = bReduceMotion,
                        .bReduceFlash = bReduceFlash,
                    });
            },
            py::arg("effect"),
            py::arg("progress"),
            py::arg("reduce_motion") = false,
            py::arg("reduce_flash") = false);

        rmlUi.def("ClearRenderTransition", []()
        {
            RmlUiManager::GetInstance().ClearRuntimeUiTransition();
        });

        rmlUi.def("LoadMotionAsset", [](const std::string& assetPath) -> py::object
        {
            constexpr std::size_t c_MaxMotionBytes = 1024u * 1024u;
            std::filesystem::path fullPath;
            std::string normalizedPath;
            const Utils::PathValidationResult validation = Utils::TryResolveProjectRelativePath(
                ConfigManager::GetInstance().GetRootFolder(),
                assetPath,
                fullPath,
                normalizedPath);
            if (!validation)
                throw py::value_error("Unsafe UiMotion path: " + validation.Message);
            std::error_code sizeError;
            const std::uintmax_t size = std::filesystem::file_size(fullPath, sizeError);
            if (sizeError || size > c_MaxMotionBytes)
                throw py::value_error("UiMotion asset is missing or exceeds the 1 MiB limit: " + normalizedPath);
            std::ifstream stream(fullPath, std::ios::binary);
            if (!stream)
                throw py::value_error("Cannot open UiMotion asset: " + normalizedPath);
            const std::string text{
                std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()};
            try
            {
                const Json json = Json::parse(text);
                constexpr std::size_t c_MaxJsonDepth = 64;
                constexpr std::size_t c_MaxJsonNodes = 20000;
                std::size_t nodeCount = 0;
                std::function<py::object(const Json&, std::size_t)> toPython;
                toPython = [&toPython, &nodeCount](const Json& value, std::size_t depth) -> py::object
                {
                    if (depth > c_MaxJsonDepth || ++nodeCount > c_MaxJsonNodes)
                        throw std::invalid_argument("UiMotion JSON exceeds its structure budget");
                    if (value.is_null())
                        return py::none();
                    if (value.is_boolean())
                        return py::bool_(value.get<bool>());
                    if (value.is_number_unsigned())
                        return py::int_(value.get<std::uint64_t>());
                    if (value.is_number_integer())
                        return py::int_(value.get<std::int64_t>());
                    if (value.is_number_float())
                        return py::float_(value.get<double>());
                    if (value.is_string())
                        return py::str(value.get<std::string>());
                    if (value.is_array())
                    {
                        py::list result;
                        for (const Json& item : value)
                            result.append(toPython(item, depth + 1));
                        return std::move(result);
                    }
                    py::dict result;
                    for (const auto& [key, item] : value.items())
                        result[py::str(key)] = toPython(item, depth + 1);
                    return std::move(result);
                };
                return toPython(json, 0);
            }
            catch (const std::exception& error)
            {
                throw py::value_error(
                    "Invalid UiMotion JSON at '" + assetPath + "': " + error.what());
            }
        });

        rmlUi.def("Load", [](const std::string& uiPath) -> std::string
        {
            const UUID* owner = PythonScriptSystem::GetCurrentScriptOwner();
            if (!owner)
                throw py::value_error("HEngine.RmlUI.Load must be called from a Python script callback.");

            std::string documentId;
            if (!RmlUiManager::GetInstance().LoadDocument(uiPath, *owner, &documentId))
                return {};
            return documentId;
        });

        rmlUi.def("Show", [](const std::string& documentId)
        {
            return RmlUiManager::GetInstance().ShowDocument(documentId);
        });

        rmlUi.def("Hide", [](const std::string& documentId)
        {
            return RmlUiManager::GetInstance().HideDocument(documentId);
        });

        rmlUi.def("SetActive", [](const std::string& documentId)
        {
            return RmlUiManager::GetInstance().SetActiveDocument(documentId);
        });

        rmlUi.def("Reload", [](const std::string& documentId)
        {
            return RmlUiManager::GetInstance().ReloadDocument(documentId);
        });

        rmlUi.def("Unload", [](const std::string& documentId)
        {
            return RmlUiManager::GetInstance().UnloadDocument(documentId);
        });

        rmlUi.def("SetText", [](const std::string& documentId, const std::string& elementId, const std::string& text)
        {
            return RmlUiManager::GetInstance().SetElementText(documentId, elementId, text);
        });

        rmlUi.def("SetAttribute", [](const std::string& documentId, const std::string& elementId, const std::string& name, const std::string& value)
        {
            return RmlUiManager::GetInstance().SetElementAttribute(documentId, elementId, name, value);
        });

        rmlUi.def("SetClass", [](const std::string& documentId, const std::string& elementId, const std::string& className, bool enabled)
        {
            return RmlUiManager::GetInstance().SetElementClass(documentId, elementId, className, enabled);
        }, py::arg("documentId"), py::arg("elementId"), py::arg("className"), py::arg("enabled") = true);

        rmlUi.def("FocusElement", [](const std::string& documentId, const std::string& elementId)
        {
            return RmlUiManager::GetInstance().FocusElement(documentId, elementId);
        });

        rmlUi.def("ClickElement", [](const std::string& documentId, const std::string& elementId)
        {
            return RmlUiManager::GetInstance().ClickElement(documentId, elementId);
        });

        rmlUi.def("GetClass", [](const std::string& documentId, const std::string& elementId, const std::string& className) -> py::object
        {
            bool enabled = false;
            if (!RmlUiManager::GetInstance().GetElementClass(
                    documentId, elementId, className, enabled))
            {
                return py::none();
            }
            return py::bool_(enabled);
        });

        rmlUi.def("SetStyle", [](const std::string& documentId, const std::string& elementId, const std::string& name, const std::string& value)
        {
            return RmlUiManager::GetInstance().SetElementStyle(documentId, elementId, name, value);
        });

        rmlUi.def("GetStyle", [](const std::string& documentId, const std::string& elementId, const std::string& name) -> py::object
        {
            std::string value;
            if (!RmlUiManager::GetInstance().GetElementStyle(
                    documentId, elementId, name, value))
            {
                return py::none();
            }
            return py::str(value);
        });

        rmlUi.def("SetMouseCursorVisible", [](const std::string& documentId, bool bVisible)
        {
            return RmlUiManager::GetInstance().SetMouseCursorVisible(documentId, bVisible);
        });

        rmlUi.def("SetCustomMouseCursor", [](const std::string& documentId, const std::string& assetPath, uint32_t hotspotX, uint32_t hotspotY)
        {
            return RmlUiManager::GetInstance().SetCustomMouseCursor(documentId, assetPath, hotspotX, hotspotY);
        }, py::arg("documentId"), py::arg("assetPath"), py::arg("hotspotX") = 0, py::arg("hotspotY") = 0);
    }
}
