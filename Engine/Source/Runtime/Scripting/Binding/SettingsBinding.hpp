#pragma once

#include "Runtime/Audio/AudioEngine.h"
#include "Runtime/Core/AppFramework/Application.h"
#include "Runtime/UI/Rml/RmlUiManager.h"
#include "Runtime/Graphics/Settings/RenderSettingsService.h"

#include <pybind11/embed.h>

namespace py = pybind11;

namespace HEngine
{
    static void SettingsBinding(py::module_& HEngineModule)
    {
        py::module_ settings = HEngineModule.def_submodule("Settings");

        settings.def("SetMasterVolume", [](float volume)
        {
            AudioEngine::GetInstance().SetMasterVolume(volume);
        });
        settings.def("GetMasterVolume", []()
        {
            return AudioEngine::GetInstance().GetMasterVolume();
        });

        settings.def("SetVSync", [](bool bEnabled)
        {
            RenderSettingsService& service =
                RenderSettingsService::GetInstance();
            RenderSettingsDraft draft =
                service.CreateDraft(SettingsLayer::Session);
            draft.Patch[std::string(RenderSettingIds::VSync)] = bEnabled;
            const RenderSettingsApplyResult result =
                service.ApplyDraft(draft, false);
            if (!result.bSuccess)
                throw std::runtime_error(result.Message);
        });
        settings.def("GetVSync", []()
        {
            return RenderSettingsService::GetInstance()
                .GetSnapshot().Effective.bVSync;
        });

        settings.def("SetFullscreen", [](bool bEnabled)
        {
            Application::GetWindow().SetFullscreen(bEnabled);
            return Application::GetWindow().IsFullscreen();
        });
        settings.def("GetFullscreen", []()
        {
            return Application::GetWindow().IsFullscreen();
        });

        settings.def("SetUIScale", [](float scale)
        {
            RmlUiManager::GetInstance().SetUserScale(scale);
        });
        settings.def("GetUIScale", []()
        {
            return RmlUiManager::GetInstance().GetUserScale();
        });
    }
}
