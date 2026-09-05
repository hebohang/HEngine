#pragma once

#include "ModuleInterface.h"
#include "Runtime/Core/Templates/PublicSingleton.h"
#include "Runtime/Core/Base/Base.h"
#include "Runtime/Core/Plugins/PluginManager.h"

#include <string>
#include <unordered_map>

namespace HEngine
{
	class ModuleInfo
	{
	public:
		bool bLoaded = false;
		Ref<IModule> module;
	};

	class ModuleManager : public PublicSingleton<ModuleManager>
	{
#ifdef WITH_EDITOR
	public:
		static void Init();
#endif
	public:
		void ConfigurePluginHost(PluginHostEnvironment environment);
		[[nodiscard]] bool IsPluginHostConfigured() const noexcept;
		[[nodiscard]] PluginManager& GetPluginManager();
		[[nodiscard]] const PluginManager& GetPluginManager() const;
		void StartPluginsThroughPhase(PluginLoadPhase phase);
		void StopPlugins();

		// if module not loaded, return nullptr
		Ref<IModule> GetModule(const std::string& moduleName);

		void AddModule(const std::string& moduleName, const Ref<IModule> module);

		void LoadModule(const std::string& moduleName);
		void LoadModule(const std::string& moduleName, const Ref<IModule> module);
	private:
		Scope<PluginManager> mPluginManager;
		std::unordered_map<std::string, ModuleInfo> mModuleMap;
	};
}
