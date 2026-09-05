#include "ModuleManager.h"

#include <stdexcept>
#include <utility>

namespace HEngine
{
	void ModuleManager::ConfigurePluginHost(PluginHostEnvironment environment)
	{
		if (mPluginManager)
			throw std::logic_error("Plugin host is already configured");
		mPluginManager = CreateScope<PluginManager>(std::move(environment));
	}

	bool ModuleManager::IsPluginHostConfigured() const noexcept
	{
		return mPluginManager != nullptr;
	}

	PluginManager& ModuleManager::GetPluginManager()
	{
		if (!mPluginManager)
			throw std::logic_error("Plugin host is not configured");
		return *mPluginManager;
	}

	const PluginManager& ModuleManager::GetPluginManager() const
	{
		if (!mPluginManager)
			throw std::logic_error("Plugin host is not configured");
		return *mPluginManager;
	}

	void ModuleManager::StartPluginsThroughPhase(PluginLoadPhase phase)
	{
		if (mPluginManager)
			mPluginManager->StartThroughPhase(phase);
	}

	void ModuleManager::StopPlugins()
	{
		if (!mPluginManager)
			return;
		// Reset host ownership before propagating a Stop failure so cleanup can
		// never leave a half-stopped manager that rejects later configuration.
		Scope<PluginManager> manager = std::move(mPluginManager);
		manager->StopAll();
	}

#ifdef WITH_EDITOR
	void ModuleManager::Init()
	{
		// Compatibility entry point. Static plugin anchors are registered by the
		// host bootstrap; Runtime must not include or construct concrete plugins.
		(void)ModuleManager::GetInstance();
	}
#endif

	Ref<IModule> HEngine::ModuleManager::GetModule(const std::string& moduleName)
	{
		if (mModuleMap.find(moduleName) == mModuleMap.end())
		{
			return nullptr;
		}
		if (!mModuleMap[moduleName].bLoaded)
		{
			return nullptr;
		}

		return mModuleMap[moduleName].module;
	}

	void ModuleManager::AddModule(const std::string& moduleName, const Ref<IModule> module)
	{
		HE_CORE_ASSERT(mModuleMap.find(moduleName) == mModuleMap.end() && "Already have this module in ModuleMap!");
		mModuleMap[moduleName].module = module;
	}

	void ModuleManager::LoadModule(const std::string& moduleName)
	{
		if (mModuleMap.find(moduleName) == mModuleMap.end())
		{
			return;
		}

		mModuleMap[moduleName].module->StartModule();
		mModuleMap[moduleName].bLoaded = true;
	}

	void ModuleManager::LoadModule(const std::string& moduleName, const Ref<IModule> module)
	{
		AddModule(moduleName, module);
		module->StartModule();
		mModuleMap[moduleName].bLoaded = true;
	}
}
