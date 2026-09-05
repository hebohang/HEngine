#include "EditorLayer.h"
#include "Runtime/Core/Modules/ModuleManager.h"

#include <HEngine.h>

namespace HEngine
{
	void RegisterStaticPluginsForEditor(PluginManager& pluginManager);

	void MyAppInitialize(Application& app)
	{
		auto& moduleManager = ModuleManager::GetInstance();
		moduleManager.ConfigurePluginHost(PluginHostEnvironment{
			.Host = PluginHost::Editor,
			.BuildDistribution = PluginBuildDistribution::Development,
			.Platform = PluginPlatform::Windows,
#if HE_DEBUG
			.Configuration = PluginConfiguration::Debug,
#else
			.Configuration = PluginConfiguration::Release,
#endif
		});
		RegisterStaticPluginsForEditor(moduleManager.GetPluginManager());

		auto editorLayer = new EditorLayer();
		app.Init("HEngine Editor");
		app.PushLayer(editorLayer);
		const std::string windowTitle = std::string("HEngine Editor [") +
			ToString(app.GetRenderBackend()) + "]";
		app.GetWindow().SetWindowTitle(windowTitle.c_str());
	}
}
