#include "GameBootstrap.h"
#include "Runtime/Core/Modules/ModuleManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"

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
            .Platform = GetCurrentPluginPlatform(),
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
#if HE_PORTABLE_PLAYER
        const auto& config = ConfigManager::GetInstance();
        const auto game = Utils::LoadJsonFile(config.GetRootFolder() / config.GetGameConfigPath());
        app.Init(game.at("Game").value("Name", std::string("HEngine Game")));
#else
        app.Init(HE_PROJECT_DISPLAY_NAME);
#endif
        app.PushLayer(CreateProjectGameLayer());
    }
}
