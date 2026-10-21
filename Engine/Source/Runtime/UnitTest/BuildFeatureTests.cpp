#include "Hepch.h"
#include "Runtime/Core/BuildFeatures.h"
#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/EcsFramework/Serializer/SceneSerializer.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/AI/Navigation/NavigationService.h"
#include "Runtime/EcsFramework/System/AI/BehaviorTreeSystem.h"
#include "Runtime/UI/Rml/RmlBuildFeatures.h"
#include "all_serializer.h"

#include <stdexcept>
#include <string>

namespace HEngine::Tests
{
    namespace
    {
        template<class F> void ExpectUnsupported(F&& action, BuildFeature feature)
        {
            try { action(); }
            catch (const std::runtime_error& error)
            {
                if (std::string(error.what()).find(BuildFeatureOption(feature)) != std::string::npos) return;
                throw;
            }
            throw std::runtime_error("Disabled feature did not reject its operation");
        }
        template<class C> void CheckComponent(Entity entity)
        {
            if (IsBuildFeatureEnabled(C::RequiredBuildFeature))
            {
                entity.AddComponent<C>();
                if (!entity.HasComponent<C>()) throw std::runtime_error("Enabled component was not added");
                entity.RemoveComponent<C>();
            }
            else
            {
                ExpectUnsupported([&] { entity.AddComponent<C>(); }, C::RequiredBuildFeature);
                if (entity.HasComponent<C>()) throw std::runtime_error("Rejected component mutated registry");
                ExpectUnsupported([&] { entity.AddOrReplaceComponent<C>(); }, C::RequiredBuildFeature);
                auto loaded = CreateRef<Level>();
                Json value = Json::object();
                TagComponent tag;
                tag.Tag = "unsupported-scene-component";
                Serializer::Serialize(value["TagComponent"], tag);
                Serializer::Serialize(value[C::GetTypeName()], C{});
                Json scene = {{UUID().str(), value}};
                SceneSerializer serializer(loaded);
                ExpectUnsupported([&] { serializer.DeserializeFromJson(scene); }, C::RequiredBuildFeature);
            }
        }
    }
    void BuildFeatureContract()
    {
        auto level = CreateRef<Level>();
        auto entity = level->CreateEntity("feature-contract");
        CheckComponent<Rigidbody2DComponent>(entity);
        CheckComponent<BoxCollider2DComponent>(entity);
        CheckComponent<CircleCollider2DComponent>(entity);
        CheckComponent<Rigidbody3DComponent>(entity);
        CheckComponent<CapsuleCollider3DComponent>(entity);
        CheckComponent<CharacterMovementComponent>(entity);
#if !HE_ENABLE_PHYSICS_3D
        if (level->TryGetSystem<PhysicSystem3D>()) throw std::runtime_error("Disabled physics system registered");
#endif
#if !HE_ENABLE_NAVIGATION
        BehaviorTreeSystem behavior(level.get());
        behavior.OnRuntiemStart();
        behavior.OnRuntimeStop();
        auto& navigation = NavigationService::Get();
        if (navigation.IsReady()) throw std::runtime_error("Disabled navigation is ready");
        ExpectUnsupported([&] { navigation.BuildNavMesh(level.get()); }, BuildFeature::Navigation);
        std::vector<glm::vec3> path{glm::vec3(1)};
        ExpectUnsupported([&] { navigation.FindPath({}, {}, path); }, BuildFeature::Navigation);
        if (!path.empty()) throw std::runtime_error("Disabled navigation left a stale path");
#endif
        for (auto feature : {BuildFeature::Svg, BuildFeature::Lottie})
        {
            if (!IsBuildFeatureEnabled(feature))
            {
                UnsupportedUiElementInstancer instancer(feature);
                ExpectUnsupported([&] { instancer.InstanceElement(nullptr, feature == BuildFeature::Svg ? "svg" : "lottie", {}); }, feature);
            }
        }
    }
}
