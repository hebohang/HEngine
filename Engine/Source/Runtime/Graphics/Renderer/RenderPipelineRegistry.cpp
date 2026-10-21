#include "Hepch.h"
#include "RenderPipelineRegistry.h"
#include "SceneRenderer.h"

#include <map>
#include <mutex>
#include <stdexcept>

namespace HEngine
{
    namespace
    {
        std::mutex sPipelineRegistryMutex;
        std::map<std::string, RenderPipelineFactory>& PipelineFactories()
        {
            static std::map<std::string, RenderPipelineFactory> factories{
                {"realtime", [](const SceneRendererDesc& desc) { return CreateScope<SceneRenderer>(desc); }}
            };
#if defined(HE_DEV_AGENT_RUNTIME)
            static const bool registeredTestAlias = [&]() {
                const char* enabled = std::getenv("HE_DEV_RENDER_PIPELINE_TEST_ALIAS");
                if (enabled && std::string_view(enabled) == "1")
                    factories.emplace("test.realtime-alias",
                        [](const SceneRendererDesc& desc) { return CreateScope<SceneRenderer>(desc); });
                return true;
            }();
            (void)registeredTestAlias;
#endif
            return factories;
        }
    }
    void RegisterRenderPipeline(std::string type, RenderPipelineFactory factory)
    {
        if (type.empty() || !factory) throw std::invalid_argument("Pipeline requires type and factory");
        std::lock_guard lock(sPipelineRegistryMutex);
        if (!PipelineFactories().emplace(std::move(type), std::move(factory)).second)
            throw std::invalid_argument("Duplicate pipeline type");
    }
    std::vector<std::string> EnumerateRenderPipelineTypes()
    {
        std::lock_guard lock(sPipelineRegistryMutex);
        std::vector<std::string> types;
        types.reserve(PipelineFactories().size());
        for (const auto& [type, factory] : PipelineFactories()) types.push_back(type);
        return types;
    }
    bool IsRenderPipelineRegistered(std::string_view type)
    {
        std::lock_guard lock(sPipelineRegistryMutex);
        return PipelineFactories().contains(std::string(type));
    }
    Scope<RenderPipeline> CreateRenderPipeline(const SceneRendererDesc& desc)
    {
        RenderPipelineFactory factory;
        {
            std::lock_guard lock(sPipelineRegistryMutex);
            const auto found = PipelineFactories().find(desc.PipelineType);
            if (found == PipelineFactories().end()) throw std::invalid_argument("Unknown pipeline type: " + desc.PipelineType);
            factory = found->second;
        }
        auto pipeline = factory(desc);
        if (!pipeline) throw std::runtime_error("Pipeline factory returned no instance");
        return pipeline;
    }
}
