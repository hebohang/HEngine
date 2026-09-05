#include "GameLayer/GameLayer.h"

#include "Runtime/Core/Modules/ModuleManager.h"

#include <HEngine.h>
#include <HEngine/ProjectBuildConfig.h>

namespace HEngine
{
	void RegisterStaticPluginsForDevGame(PluginManager& pluginManager);
	void RegisterStaticPluginsForGame(PluginManager& pluginManager);

	void MyAppInitialize(Application& app)
	{
		auto& moduleManager = ModuleManager::GetInstance();
		moduleManager.ConfigurePluginHost(PluginHostEnvironment{
#if defined(HE_PACKAGED_GAME)
			.Host = PluginHost::Game,
			.BuildDistribution = PluginBuildDistribution::Shipping,
#else
			.Host = PluginHost::DevGame,
			.BuildDistribution = PluginBuildDistribution::Development,
#endif
			.Platform = PluginPlatform::Windows,
#if HE_DEBUG
			.Configuration = PluginConfiguration::Debug,
#else
			.Configuration = PluginConfiguration::Release,
#endif
		});
#if defined(HE_PACKAGED_GAME)
		RegisterStaticPluginsForGame(moduleManager.GetPluginManager());
#else
		RegisterStaticPluginsForDevGame(moduleManager.GetPluginManager());
#endif
		app.Init(HE_PROJECT_DISPLAY_NAME);
		app.PushLayer(new GameLayer());
	}
}
