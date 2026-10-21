#include "EditorLayer.h"
#include "Editor/BinaryPluginSettings.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

#include <HEngine.h>
#include <HEngine/ProjectBuildConfig.h>

namespace HEngine
{
	void RegisterStaticPluginsForEditor(PluginManager& pluginManager);

	void MyAppInitialize(Application& app)
	{
		auto& moduleManager = ModuleManager::GetInstance();
		moduleManager.ConfigurePluginHost(PluginHostEnvironment{
			.Host = PluginHost::Editor,
			.BuildDistribution = PluginBuildDistribution::Development,
			.Platform = GetCurrentPluginPlatform(),
#if HE_DEBUG
			.Configuration = PluginConfiguration::Debug,
#else
			.Configuration = PluginConfiguration::Release,
#endif
		});
		RegisterStaticPluginsForEditor(moduleManager.GetPluginManager());
#if HE_PORTABLE_EDITOR
        if (!BinaryPluginSettings::OfflineRendererEnabled())
            moduleManager.GetPluginManager().Unregister("he.offline-reference-renderer");
        HE_CORE_INFO("Binary Editor offline renderer: {}",
            moduleManager.GetPluginManager().Contains("he.offline-reference-renderer") ? "enabled" : "disabled");
#endif

		auto editorLayer = new EditorLayer();
		app.Init("HEngine Editor");
		app.PushLayer(editorLayer);
		const auto descriptor = Utils::LoadJsonFile(ConfigManager::GetProjectFullPath("Project.heproject"));
		const std::string windowTitle = descriptor.value("DisplayName", "HEngine") + " - HEngine Editor [" +
			ToString(app.GetRenderBackend()) + "]";
		app.GetWindow().SetWindowTitle(windowTitle.c_str());
	}
}
