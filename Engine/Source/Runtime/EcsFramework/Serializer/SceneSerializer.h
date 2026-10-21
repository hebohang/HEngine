#pragma once

#include "Runtime/Core/Base/Base.h"
#include "Runtime/EcsFramework/Level/Level.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace HEngine
{
    class RuntimeResourceRegistry;
	class RuntimeResourceCatalogSnapshot;
    class SceneDocument;
    class Entity;

    enum class SceneMaterializationState : std::uint8_t
    {
        Empty,
        WaitingForResources,
        Materializing,
        Ready,
        Failed,
        Cancelled
    };

    struct SceneMaterializationBudget
    {
        std::chrono::nanoseconds TimeBudget = std::chrono::milliseconds(2);
        size_t MaxEntities = 16;
    };

    struct SceneMaterializationProgress
    {
        SceneMaterializationState State = SceneMaterializationState::Empty;
        size_t TotalEntities = 0;
        size_t MaterializedEntities = 0;
        RuntimeResourceGroupProgress Resources;
        float Ratio = 0.0f;
    };

    // Main-thread staged scene construction. A completed Level is published
    // only through TakeCompletedLevel and can be taken once.
    class IncrementalSceneMaterializer final
    {
    public:
        IncrementalSceneMaterializer();
        ~IncrementalSceneMaterializer();

        IncrementalSceneMaterializer(const IncrementalSceneMaterializer&) = delete;
        IncrementalSceneMaterializer& operator=(const IncrementalSceneMaterializer&) = delete;

        void Start(
            RuntimeResourceRegistry& registry,
            std::shared_ptr<const SceneDocument> document);
		void Start(
			RuntimeResourceRegistry& registry,
			std::shared_ptr<const SceneDocument> document,
			const std::filesystem::path& projectRoot,
			std::shared_ptr<const RuntimeResourceCatalogSnapshot> catalog);
        [[nodiscard]] SceneMaterializationProgress Pump(
            SceneMaterializationBudget budget = {});
        [[nodiscard]] SceneMaterializationProgress GetProgress() const noexcept;
        void RequestCancel();
        void RethrowFailure() const;
        [[nodiscard]] Ref<Level> TakeCompletedLevel();

    private:
        struct Impl;
        Scope<Impl> mImpl;
    };

    class SceneSerializer
    {
    public:
        SceneSerializer(const Ref<Level>& level);

        [[nodiscard]] Json SerializeToJson() const;
        [[nodiscard]] Json SerializeEntityToJson(Entity entity) const;
        void Serialize(const std::string& filepath);
        void SerializeRuntime(const std::string& filepath);

        bool DeserializeFromJson(const Json& json);
        bool Deserialize(const std::string& filepath);
        bool DeserializeRuntime(const std::string& filepath);
    private:
        Ref<Level> mLevel;
    };
}
