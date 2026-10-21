#pragma once

#include "LegacyReference.h"
#include "ReflectedComponents.h"

#include "Runtime/Camera/SceneCamera.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/EcsFramework/Entity/Entity.h"
#include "Runtime/AI/Blackboard/Blackboard.h"
#include "Runtime/EcsFramework/Component/AI/BehaviorTreeComponent.h"
#include "Runtime/EcsFramework/Component/Transform/TransformComponent.h"
#include "Runtime/EcsFramework/Component/Script/PythonScriptComponent.h"
#include "Runtime/EcsFramework/Component/Animation/AnimatorComponent.h"
#include "Runtime/EcsFramework/Component/Animation/SequenceComponent.h"
#include "Runtime/EcsFramework/Component/Character/CharacterMovementComponent.h"
#include "Runtime/EcsFramework/Component/Camera/FollowCameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/CameraComponent.h"
#include "Runtime/EcsFramework/Component/Camera/SpringArmComponent.h"
#include "Runtime/EcsFramework/Component/Light/PointLightComponent.h"
#include "Runtime/EcsFramework/Component/Shape/SpriteRendererComponent.h"
#include "Runtime/EcsFramework/Component/Physics/ThreeD/Rigidbody3DComponent.h"
#include "Runtime/EcsFramework/Component/Physics/TwoD/Rigidbody2DComponent.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Serializer/PrefabSerializer.h"
#include "Runtime/EcsFramework/System/Movement/CharacterMovementSystem.h"
#include "Runtime/EcsFramework/System/Script/PythonScriptSystem.h"
#include "Runtime/EcsFramework/System/Physics/PhysicSystem3D.h"
#include "Runtime/Profile/Profile.h"
#include "Runtime/Scripting/ScriptEngine.h"
#include "Runtime/Scripting/SurvivorAgentBatch.h"
#include "Runtime/Scripting/SurvivorPickupBatch.h"
#include "Runtime/Scripting/SurvivorTargetingBatch.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace py = pybind11;

namespace HEngine
{
#if HE_ENABLE_PROFILE
    enum class SurvivorProfileStage : uint8_t
    {
        Update,
        Bookkeeping,
        PendingPickupSpawns,
        WaveSpawn,
        Monsters,
        Pickups,
        Weapons,
        Hud
    };

    static py::object ProfileSurvivorStage(
        SurvivorProfileStage stage,
        const py::function& callback,
        const py::args& args,
        const py::kwargs& kwargs)
    {
        const auto invoke = [&]() -> py::object
        {
            return callback(*args, **kwargs);
        };

        switch (stage)
        {
        case SurvivorProfileStage::Update:
        {
            HE_PROFILE_SECTION("SurvivorGame::Update");
            return invoke();
        }
        case SurvivorProfileStage::Bookkeeping:
        {
            HE_PROFILE_SECTION("SurvivorGame::Phase::Bookkeeping");
            return invoke();
        }
        case SurvivorProfileStage::PendingPickupSpawns:
        {
            HE_PROFILE_SECTION("SurvivorGame::Phase::PendingPickupSpawns");
            return invoke();
        }
        case SurvivorProfileStage::WaveSpawn:
        {
            HE_PROFILE_SECTION("SurvivorGame::Phase::WaveSpawn");
            return invoke();
        }
        case SurvivorProfileStage::Monsters:
        {
            HE_PROFILE_SECTION("SurvivorGame::Phase::Monsters");
            return invoke();
        }
        case SurvivorProfileStage::Pickups:
        {
            HE_PROFILE_SECTION("SurvivorGame::Phase::Pickups");
            return invoke();
        }
        case SurvivorProfileStage::Weapons:
        {
            HE_PROFILE_SECTION("SurvivorGame::Phase::Weapons");
            return invoke();
        }
        case SurvivorProfileStage::Hud:
        {
            HE_PROFILE_SECTION("SurvivorGame::Phase::Hud");
            return invoke();
        }
        }
        throw py::value_error("Unknown Survivor profile stage.");
    }
#endif

    static float SurvivorSinApprox(float value)
    {
        constexpr float pi = 3.14159265358979323846f;
        constexpr float twoPi = pi * 2.0f;
        value = std::fmod(value + pi, twoPi);
        if (value < 0.0f)
            value += twoPi;
        value -= pi;
        const float valueSquared = value * value;
        return value * (1.0f - valueSquared / 6.0f + valueSquared * valueSquared / 120.0f);
    }

    static float SurvivorAtan2Approx(float y, float x)
    {
        constexpr float pi = 3.14159265358979323846f;
        const float absY = std::abs(y) + 0.0000001f;
        float angle = 0.0f;
        if (x >= 0.0f)
        {
            const float ratio = (x - absY) / (x + absY);
            angle = pi * 0.25f - pi * 0.25f * ratio;
        }
        else
        {
            const float ratio = (x + absY) / (absY - x);
            angle = pi * 0.75f - pi * 0.25f * ratio;
        }
        return y < 0.0f ? -angle : angle;
    }

    static float SurvivorRandomUniform(uint32_t& randomState, float minimum, float maximum)
    {
        randomState = 1664525u * randomState + 1013904223u;
        const float fraction = static_cast<float>(
            static_cast<double>(randomState) / 4294967295.0);
        return minimum + (maximum - minimum) * fraction;
    }

    static py::tuple UpdateSurvivorCrowd(
        const py::list& monsters,
        const glm::vec3& playerPosition,
        float deltaTime,
        float elapsed,
        float damageCooldown,
        float health,
        float damageReduction,
        uint32_t randomState,
        int maxAnimatedMonsters,
        float animationLodDistance,
        float animationLodReleaseDistance,
        float despawnDistanceSquared,
        const std::string& animationControllerPath)
    {
        py::list retired;
        int animatedCount = 0;
        for (const py::handle monsterHandle : monsters)
        {
            const py::dict monster = py::reinterpret_borrow<py::dict>(monsterHandle);
            if (py::cast<bool>(monster["animated"]) && !py::cast<bool>(monster["dead"]))
                ++animatedCount;
        }

        float damageFlashTimer = 0.0f;
        float bannerTimer = 0.0f;
        bool bGameOver = false;
        for (const py::handle monsterHandle : monsters)
        {
            py::dict monster = py::reinterpret_borrow<py::dict>(monsterHandle);
            Entity entity = py::cast<Entity>(monster["entity"]);
            if (!entity.HasComponent<TransformComponent>())
            {
                retired.append(monster);
                continue;
            }

            TransformComponent& transform = entity.GetComponent<TransformComponent>();
            glm::vec3 position = transform.Translation;
            const bool bAnimated = py::cast<bool>(monster["animated"]);
            const bool bDead = py::cast<bool>(monster["dead"]);
            float animationWarmup = py::cast<float>(monster["animation_warmup"]);

            if (bDead)
            {
                animationWarmup = std::max(0.0f, animationWarmup - deltaTime);
                monster["animation_warmup"] = animationWarmup;
                if (bAnimated &&
                    !py::cast<bool>(monster["death_anim_sent"]) &&
                    animationWarmup <= 0.0f)
                {
                    AnimatorComponent& animator = entity.GetComponent<AnimatorComponent>();
                    animator.SetBool("Attacking", false);
                    animator.SetBool("Dead", true);
                    monster["death_anim_sent"] = true;
                }

                float deathTimer = py::cast<float>(monster["death_timer"]) - deltaTime;
                monster["death_timer"] = deathTimer;
                if (deathTimer < 0.24f)
                {
                    const float fraction = std::max(0.0f, deathTimer / 0.24f);
                    const float scale = py::cast<float>(monster["base_scale"]) * fraction;
                    transform.Scale = glm::vec3(scale);
                }
                if (deathTimer <= 0.0f)
                    retired.append(monster);
                continue;
            }

            const float deltaX = playerPosition.x - position.x;
            const float deltaZ = playerPosition.z - position.z;
            const float distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
            if (distanceSquared > despawnDistanceSquared)
            {
                retired.append(monster);
                continue;
            }

            const float distance = std::max(0.001f, std::sqrt(distanceSquared));
            const float attackDistance = py::cast<float>(monster["radius"]) + 0.42f;
            const bool bInAttackRange = distance <= attackDistance;
            bool bCurrentlyAnimated = bAnimated;
            const bool bBoss = py::cast<bool>(monster["boss"]);
            const bool bShouldAnimate =
                bBoss ||
                (bCurrentlyAnimated && distance <= animationLodReleaseDistance) ||
                ((bInAttackRange || distance <= animationLodDistance) &&
                    animatedCount < maxAnimatedMonsters);

            if (bShouldAnimate && !bCurrentlyAnimated)
            {
                AnimatorComponent& animator = entity.GetComponent<AnimatorComponent>();
                animator.SetControllerPath(animationControllerPath);
                animator.SetSpeed(SurvivorRandomUniform(randomState, 0.95f, 1.22f));
                animator.Play();
                bCurrentlyAnimated = true;
                monster["animated"] = true;
                animationWarmup = 0.14f;
                ++animatedCount;
            }
            else if (bCurrentlyAnimated &&
                !bBoss &&
                !bInAttackRange &&
                distance > animationLodReleaseDistance)
            {
                AnimatorComponent& animator = entity.GetComponent<AnimatorComponent>();
                animator.SetControllerPath("None");
                animator.Stop();
                bCurrentlyAnimated = false;
                monster["animated"] = false;
                monster["attacking"] = false;
                animationWarmup = 0.0f;
                animatedCount = std::max(0, animatedCount - 1);
            }

            float knockX = py::cast<float>(monster["knock_x"]);
            float knockZ = py::cast<float>(monster["knock_z"]);
            if (knockX != 0.0f || knockZ != 0.0f)
            {
                position.x += knockX * deltaTime;
                position.z += knockZ * deltaTime;
                const float damping = std::max(0.0f, 1.0f - 9.0f * deltaTime);
                knockX *= damping;
                knockZ *= damping;
                monster["knock_x"] = knockX;
                monster["knock_z"] = knockZ;
            }

            if (distance > attackDistance)
            {
                const float inverseDistance = 1.0f / distance;
                const float tangentX = -deltaZ * inverseDistance;
                const float tangentZ = deltaX * inverseDistance;
                const float phase = py::cast<float>(monster["phase"]);
                const float weave = SurvivorSinApprox(elapsed * 1.7f + phase) * 0.12f;
                const float moveX = deltaX * inverseDistance + tangentX * weave;
                const float moveZ = deltaZ * inverseDistance + tangentZ * weave;
                const float moveDistance = std::min(
                    py::cast<float>(monster["speed"]) * deltaTime,
                    distance - attackDistance);
                position.x += moveX * moveDistance;
                position.z += moveZ * moveDistance;
                transform.Translation = position;
                transform.Rotation = glm::vec3(0.0f, SurvivorAtan2Approx(deltaX, deltaZ), 0.0f);
            }

            const float hitFlash = std::max(
                0.0f,
                py::cast<float>(monster["hit_flash"]) - deltaTime);
            monster["hit_flash"] = hitFlash;
            float pulse = hitFlash > 0.0f ? 1.13f : 1.0f;
            if (!bCurrentlyAnimated)
            {
                pulse *= 1.0f + SurvivorSinApprox(
                    elapsed * 4.0f + py::cast<float>(monster["phase"])) * 0.025f;
            }
            transform.Scale = glm::vec3(py::cast<float>(monster["base_scale"]) * pulse);

            float touchTimer = std::max(
                0.0f,
                py::cast<float>(monster["touch_timer"]) - deltaTime);
            float attackCooldown = std::max(
                0.0f,
                py::cast<float>(monster["attack_cooldown"]) - deltaTime);
            animationWarmup = std::max(0.0f, animationWarmup - deltaTime);
            monster["touch_timer"] = touchTimer;
            monster["attack_cooldown"] = attackCooldown;
            monster["animation_warmup"] = animationWarmup;

            const bool bWantsAttack = bInAttackRange && attackCooldown <= 0.0f;
            const bool bAttacking = py::cast<bool>(monster["attacking"]);
            if (bCurrentlyAnimated && animationWarmup <= 0.0f && bAttacking != bWantsAttack)
            {
                AnimatorComponent& animator = entity.GetComponent<AnimatorComponent>();
                animator.SetBool("Dead", false);
                animator.SetBool("Attacking", bWantsAttack);
                monster["attacking"] = bWantsAttack;
                if (bWantsAttack)
                    monster["attack_windup"] = bBoss ? 0.85f : 0.35f;
            }

            float attackWindup = py::cast<float>(monster["attack_windup"]);
            if (bWantsAttack)
                attackWindup = std::max(0.0f, attackWindup - deltaTime);
            else if (!bInAttackRange)
                attackWindup = 0.0f;
            monster["attack_windup"] = attackWindup;

            if (bWantsAttack &&
                attackWindup <= 0.0f &&
                touchTimer <= 0.0f &&
                damageCooldown <= 0.0f)
            {
                monster["touch_timer"] = 0.65f;
                monster["attack_cooldown"] = bBoss ? 0.82f : 1.05f;
                monster["attack_windup"] = 0.0f;
                damageCooldown = 0.45f;
                const float damage =
                    py::cast<float>(monster["damage"]) * (1.0f - damageReduction);
                health = std::max(0.0f, health - damage);
                damageFlashTimer = 0.2f;
                bannerTimer = 0.28f;
                if (health <= 0.0f)
                {
                    bGameOver = true;
                    break;
                }
            }
        }

        return py::make_tuple(
            retired,
            damageCooldown,
            health,
            damageFlashTimer,
            bannerTimer,
            bGameOver,
            randomState);
    }

    struct SurvivorAgentState
    {
        Entity RuntimeEntity;
        std::string AnimationControllerPath;
        uint32_t EntityId = 0;
        uint64_t ActivationId = 0;
        float Health = 0.0f;
        float MaxHealth = 0.0f;
        float Speed = 0.0f;
        float Damage = 0.0f;
        float Radius = 0.0f;
        float BaseScale = 0.0f;
        float TouchTimer = 0.0f;
        float AttackWindup = 0.0f;
        float AttackCooldown = 0.0f;
        float AnimationWarmup = 0.0f;
        float DeathTimer = 0.0f;
        float HitFlash = 0.0f;
        float KnockX = 0.0f;
        float KnockZ = 0.0f;
        float Phase = 0.0f;
        float GroundOffset = 0.0f;
        float GroundProbeTimer = 0.0f;
        float GroundHeight = 0.0f;
        float GroundX = 0.0f;
        float GroundZ = 0.0f;
        bool bAttacking = false;
        bool bBoss = false;
        bool bObjectiveBoss = false;
        bool bFinalBoss = false;
        bool bNoDistanceRetire = false;
        bool bAnimated = false;
        bool bDead = false;
        bool bDeathAnimationSent = false;
        bool bHasGroundHeight = false;
        bool bActive = false;
        SurvivorAttackPhase LastAttackPhase = SurvivorAttackPhase::Idle;
    };

    static std::vector<SurvivorAgentState> sSurvivorAgents;
    static std::unordered_map<uint32_t, size_t> sSurvivorAgentIndices;
    static SurvivorAgentBatch sSurvivorAgentBatch;
    static SurvivorAgentBatchOutput sSurvivorAgentBatchOutput;
    static SurvivorTargetingBatch sSurvivorTargetingBatch;
    static std::vector<SurvivorTargetSample> sSurvivorTargetSamples;
    static std::vector<SurvivorDensityTargetHit> sSurvivorDensityHits;
    static std::vector<SurvivorRadialTargetHit> sSurvivorRadialTargetHits;
    static std::vector<SurvivorCapsuleTargetHit> sSurvivorCapsuleTargetHits;
    static std::vector<SurvivorCorridorQuery> sSurvivorCorridorQueries;
    static std::vector<std::vector<SurvivorCorridorTargetHit>> sSurvivorCorridorHits;
    static SurvivorPickupBatch sSurvivorPickupBatch;
    static std::unordered_map<uint32_t, Entity> sSurvivorPickupEntities;
    static std::vector<SurvivorPickupIdentity> sSurvivorCollectedPickups;
    static bool sSurvivorTargetingDirty = false;
    static constexpr float kSurvivorGroundProbeInterval = 0.15f;

    static SurvivorAgentState* FindSurvivorAgent(Entity& entity)
    {
        const uint32_t entityId = static_cast<uint32_t>(entity);
        const auto iterator = sSurvivorAgentIndices.find(entityId);
        if (iterator == sSurvivorAgentIndices.end())
            return nullptr;
        return &sSurvivorAgents[iterator->second];
    }

    static void RebuildSurvivorTargetingBatch()
    {
        if (!sSurvivorTargetingDirty)
            return;

        sSurvivorTargetSamples.clear();
        sSurvivorTargetSamples.reserve(sSurvivorAgents.size());
        for (SurvivorAgentState& state : sSurvivorAgents)
        {
            if (!state.bActive || state.bDead ||
                !state.RuntimeEntity.HasComponent<TransformComponent>())
            {
                continue;
            }

            const glm::vec3& position =
                state.RuntimeEntity.GetComponent<TransformComponent>().Translation;
            sSurvivorTargetSamples.push_back({
                { state.EntityId, state.ActivationId },
                position.x,
                position.y,
                position.z,
                state.Radius,
                state.bActive,
                state.bDead
            });
        }

        sSurvivorTargetingBatch.Rebuild(sSurvivorTargetSamples);
        sSurvivorTargetingDirty = false;
    }

    static bool IsCurrentSurvivorTarget(const SurvivorTargetSample& target)
    {
        const auto stateIterator = sSurvivorAgentIndices.find(target.Key.EntityId);
        if (stateIterator == sSurvivorAgentIndices.end())
            return false;

        const SurvivorAgentState& state = sSurvivorAgents[stateIterator->second];
        return state.bActive &&
            !state.bDead &&
            state.ActivationId == target.Key.ActivationId;
    }

    static py::tuple MakeSurvivorTargetRow(
        double metric,
        const SurvivorTargetSample& target)
    {
        return py::make_tuple(
            metric,
            target.Key.EntityId,
            target.Key.ActivationId,
            target.X,
            target.Y,
            target.Z,
            target.Radius);
    }

    static py::list QuerySurvivorAgentsInRadius(
        double originX,
        double originY,
        double originZ,
        double radius,
        double verticalRange)
    {
        RebuildSurvivorTargetingBatch();
        sSurvivorTargetingBatch.QueryRadius(
            originX,
            originY,
            originZ,
            radius,
            verticalRange,
            sSurvivorRadialTargetHits);

        py::list results;
        for (const SurvivorRadialTargetHit& hit : sSurvivorRadialTargetHits)
        {
            if (!IsCurrentSurvivorTarget(hit.Target))
                continue;
            results.append(py::make_tuple(
                hit.DistanceSquared,
                hit.Target.Key.EntityId,
                hit.Target.Key.ActivationId));
        }
        return results;
    }

    static py::list SelectDensestSurvivorAgents(
        double originX,
        double originY,
        double originZ,
        double attackRange,
        double verticalRange,
        double clusterRadius,
        size_t targetCount)
    {
        RebuildSurvivorTargetingBatch();
        sSurvivorTargetingBatch.SelectDensest(
            originX,
            originY,
            originZ,
            attackRange,
            verticalRange,
            clusterRadius,
            targetCount,
            sSurvivorDensityHits);

        py::list results;
        for (const SurvivorDensityTargetHit& hit : sSurvivorDensityHits)
        {
            if (!IsCurrentSurvivorTarget(hit.Target))
                continue;
            results.append(py::make_tuple(
                hit.DistanceSquared,
                hit.Target.Key.EntityId,
                hit.Target.Key.ActivationId,
                hit.Target.X,
                hit.Target.Y,
                hit.Target.Z,
                hit.Target.Radius,
                hit.NeighborCount));
        }
        return results;
    }

    static py::list QuerySurvivorAgentCorridors(
        double candidateOriginX,
        double candidateOriginY,
        double candidateOriginZ,
        double candidateRange,
        double candidateVerticalRange,
        const py::list& corridorSpecs)
    {
        sSurvivorCorridorQueries.clear();
        sSurvivorCorridorQueries.reserve(corridorSpecs.size());
        for (const py::handle item : corridorSpecs)
        {
            const py::sequence spec = py::reinterpret_borrow<py::sequence>(item);
            if (spec.size() != 7)
            {
                throw std::invalid_argument(
                    "A Survivor corridor spec must contain seven values.");
            }
            sSurvivorCorridorQueries.push_back({
                py::cast<double>(spec[0]),
                py::cast<double>(spec[1]),
                py::cast<double>(spec[2]),
                py::cast<double>(spec[3]),
                0.12,
                py::cast<double>(spec[4]),
                py::cast<double>(spec[5]),
                py::cast<size_t>(spec[6])
            });
        }

        RebuildSurvivorTargetingBatch();
        sSurvivorTargetingBatch.QueryCorridors(
            candidateOriginX,
            candidateOriginY,
            candidateOriginZ,
            candidateRange,
            candidateVerticalRange,
            sSurvivorCorridorQueries,
            sSurvivorCorridorHits);

        py::list results;
        for (const auto& corridorHits : sSurvivorCorridorHits)
        {
            py::list corridorResults;
            for (const SurvivorCorridorTargetHit& hit : corridorHits)
            {
                if (IsCurrentSurvivorTarget(hit.Target))
                    corridorResults.append(MakeSurvivorTargetRow(hit.Along, hit.Target));
            }
            results.append(std::move(corridorResults));
        }
        return results;
    }

    static py::list SelectSurvivorAgentChain(
        double originX,
        double originY,
        double originZ,
        double attackRange,
        double verticalRange,
        double linkRange,
        size_t maximumTargets)
    {
        RebuildSurvivorTargetingBatch();
        sSurvivorTargetingBatch.SelectChain(
            originX,
            originY,
            originZ,
            attackRange,
            verticalRange,
            linkRange,
            maximumTargets,
            sSurvivorRadialTargetHits);

        py::list results;
        for (const SurvivorRadialTargetHit& hit : sSurvivorRadialTargetHits)
        {
            if (IsCurrentSurvivorTarget(hit.Target))
                results.append(MakeSurvivorTargetRow(hit.DistanceSquared, hit.Target));
        }
        return results;
    }

    static py::list QuerySurvivorAgentsInSweptAnnulus(
        double originX,
        double originY,
        double originZ,
        double previousRadius,
        double currentRadius,
        double edgeWidth,
        double verticalRange)
    {
        RebuildSurvivorTargetingBatch();
        sSurvivorTargetingBatch.QuerySweptAnnulus(
            originX,
            originY,
            originZ,
            previousRadius,
            currentRadius,
            edgeWidth,
            verticalRange,
            sSurvivorRadialTargetHits);

        py::list results;
        for (const SurvivorRadialTargetHit& hit : sSurvivorRadialTargetHits)
        {
            if (IsCurrentSurvivorTarget(hit.Target))
                results.append(MakeSurvivorTargetRow(hit.DistanceSquared, hit.Target));
        }
        return results;
    }

    static py::object FindSurvivorAgentCircleOverlap(
        double originX,
        double originY,
        double originZ,
        double padding,
        double verticalRange)
    {
        RebuildSurvivorTargetingBatch();
        sSurvivorTargetingBatch.QueryCircleOverlaps(
            originX,
            originY,
            originZ,
            padding,
            verticalRange,
            sSurvivorRadialTargetHits);

        for (const SurvivorRadialTargetHit& hit : sSurvivorRadialTargetHits)
        {
            if (IsCurrentSurvivorTarget(hit.Target))
                return MakeSurvivorTargetRow(hit.DistanceSquared, hit.Target);
        }
        return py::none();
    }

    static py::list QuerySurvivorAgentsInSweptCapsule(
        double startX,
        double startZ,
        double endX,
        double endZ,
        double sweepRadius)
    {
        RebuildSurvivorTargetingBatch();
        sSurvivorTargetingBatch.QuerySweptCapsule(
            startX,
            startZ,
            endX,
            endZ,
            sweepRadius,
            sSurvivorCapsuleTargetHits);

        py::list results;
        for (const SurvivorCapsuleTargetHit& hit : sSurvivorCapsuleTargetHits)
        {
            if (IsCurrentSurvivorTarget(hit.Target))
            {
                results.append(MakeSurvivorTargetRow(
                    hit.SegmentFraction,
                    hit.Target));
            }
        }
        return results;
    }

    static uint32_t RegisterSurvivorPickup(
        Entity entity,
        uint64_t activationId,
        double baseY,
        double phase,
        double rotation,
        double scaleBonus)
    {
        if (!entity.HasComponent<TransformComponent>())
            throw std::invalid_argument("A Survivor pickup requires a Transform component.");

        const uint32_t entityId = static_cast<uint32_t>(entity);
        const glm::vec3& position =
            entity.GetComponent<TransformComponent>().Translation;
        const bool bActivated = sSurvivorPickupBatch.Activate({
            entityId,
            activationId,
            position.x,
            position.y,
            position.z,
            baseY,
            0.0,
            phase,
            rotation,
            scaleBonus,
            false,
            true
        });
        if (!bActivated)
            throw std::invalid_argument("A Survivor pickup activation must be newer than its prior generation.");

        sSurvivorPickupEntities.insert_or_assign(entityId, entity);
        return entityId;
    }

    static bool SetSurvivorPickupScaleBonus(
        Entity entity,
        uint64_t activationId,
        double scaleBonus)
    {
        return sSurvivorPickupBatch.SetScaleBonus(
            static_cast<uint32_t>(entity),
            activationId,
            scaleBonus);
    }

    static bool DeactivateSurvivorPickup(Entity entity, uint64_t activationId)
    {
        const bool bDeactivated = sSurvivorPickupBatch.Deactivate(
            static_cast<uint32_t>(entity),
            activationId);
        if (bDeactivated)
            sSurvivorPickupBatch.PurgeInactive();
        return bDeactivated;
    }

    static void ResetSurvivorPickups()
    {
        sSurvivorPickupBatch.Clear();
        sSurvivorPickupEntities.clear();
        sSurvivorCollectedPickups.clear();
    }

    static py::list UpdateSurvivorPickups(
        const glm::vec3& playerPosition,
        double deltaTime,
        double magnetRange,
        double pickupRange,
        const glm::vec3& gemScale)
    {
        for (const SurvivorPickupState& state : sSurvivorPickupBatch.GetStates())
        {
            if (!state.bActive)
                continue;
            const auto entityIterator = sSurvivorPickupEntities.find(state.EntityId);
            if (entityIterator == sSurvivorPickupEntities.end() ||
                !entityIterator->second.HasComponent<TransformComponent>())
            {
                throw std::runtime_error("A registered Survivor pickup lost its Transform component.");
            }

            const glm::vec3& position =
                entityIterator->second.GetComponent<TransformComponent>().Translation;
            if (!sSurvivorPickupBatch.SetPosition(
                state.EntityId,
                state.ActivationId,
                position.x,
                position.y,
                position.z))
            {
                throw std::runtime_error("A registered Survivor pickup lost its active generation.");
            }
        }

        sSurvivorPickupBatch.Update({
            playerPosition.x,
            playerPosition.y,
            playerPosition.z,
            deltaTime,
            magnetRange,
            pickupRange
        }, sSurvivorCollectedPickups);

        for (const SurvivorPickupState& state : sSurvivorPickupBatch.GetStates())
        {
            Entity& entity = sSurvivorPickupEntities.at(state.EntityId);
            TransformComponent& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = glm::vec3(
                static_cast<float>(state.X),
                static_cast<float>(state.Y),
                static_cast<float>(state.Z));
            transform.Rotation = glm::vec3(
                0.0f,
                static_cast<float>(state.Rotation),
                0.7853982f);
            transform.Scale = glm::vec3(
                gemScale.x + static_cast<float>(state.ScaleBonus),
                gemScale.y + static_cast<float>(state.ScaleBonus),
                gemScale.z + static_cast<float>(state.ScaleBonus));
        }

        py::list collected;
        for (const SurvivorPickupIdentity& identity : sSurvivorCollectedPickups)
        {
            collected.append(py::make_tuple(
                identity.EntityId,
                identity.ActivationId));
        }
        sSurvivorPickupBatch.PurgeInactive();
        return collected;
    }

    static double SurvivorDictNumber(
        const py::dict& dictionary,
        std::string_view key,
        double fallback)
    {
        const py::str pythonKey(key.data(), key.size());
        return dictionary.contains(pythonKey)
            ? py::cast<double>(dictionary[pythonKey])
            : fallback;
    }

    static uint32_t SurvivorDictCount(
        const py::dict& dictionary,
        std::string_view key,
        uint32_t fallback)
    {
        const py::str pythonKey(key.data(), key.size());
        if (!dictionary.contains(pythonKey))
            return fallback;
        return static_cast<uint32_t>(std::max(0, py::cast<int>(dictionary[pythonKey])));
    }

    static bool SurvivorDictBool(
        const py::dict& dictionary,
        std::string_view key,
        bool fallback)
    {
        const py::str pythonKey(key.data(), key.size());
        return dictionary.contains(pythonKey)
            ? py::cast<bool>(dictionary[pythonKey])
            : fallback;
    }

    static SurvivorEnemyBehavior ParseSurvivorEnemyBehavior(const py::dict& monster)
    {
        if (!monster.contains(py::str("behavior")))
            return SurvivorEnemyBehavior::Chaser;

        const std::string behavior = py::cast<std::string>(monster["behavior"]);
        if (behavior == "chaser") return SurvivorEnemyBehavior::Chaser;
        if (behavior == "flanker") return SurvivorEnemyBehavior::Flanker;
        if (behavior == "armored") return SurvivorEnemyBehavior::Armored;
        if (behavior == "bruiser") return SurvivorEnemyBehavior::Bruiser;
        if (behavior == "zone") return SurvivorEnemyBehavior::Zone;
        if (behavior == "charger") return SurvivorEnemyBehavior::Charger;
        if (behavior == "ranged") return SurvivorEnemyBehavior::Ranged;
        if (behavior == "lobber") return SurvivorEnemyBehavior::Lobber;
        if (behavior == "ghost") return SurvivorEnemyBehavior::Ghost;
        if (behavior == "shield") return SurvivorEnemyBehavior::Shield;
        if (behavior == "suicide") return SurvivorEnemyBehavior::Suicide;
        if (behavior == "summoner") return SurvivorEnemyBehavior::Summoner;
        if (behavior == "boss") return SurvivorEnemyBehavior::Boss;
        throw py::value_error("Unknown Survivor enemy behavior: " + behavior);
    }

    static SurvivorBossPhase ParseSurvivorBossPhase(const py::dict& monster)
    {
        if (!monster.contains(py::str("boss_phase")))
            return SurvivorBossPhase::None;
        const int phase = py::cast<int>(monster["boss_phase"]);
        if (phase <= 0) return SurvivorBossPhase::None;
        if (phase == 1) return SurvivorBossPhase::PhaseOne;
        if (phase == 2) return SurvivorBossPhase::PhaseTwo;
        return SurvivorBossPhase::PhaseThree;
    }

    static SurvivorBossBehavior ParseSurvivorBossBehavior(const py::dict& monster)
    {
        if (!monster.contains(py::str("boss_behavior")))
            return SurvivorBossBehavior::Generic;
        const std::string behavior = py::cast<std::string>(monster["boss_behavior"]);
        if (behavior.empty() || behavior == "generic")
            return SurvivorBossBehavior::Generic;
        if (behavior == "star_dragon")
            return SurvivorBossBehavior::StarDragon;
        throw py::value_error("Unknown Survivor boss behavior: " + behavior);
    }

    static py::dict SurvivorNestedDict(const py::dict& dictionary, const char* key)
    {
        if (!dictionary.contains(py::str(key)) || dictionary[key].is_none())
            return {};
        return py::cast<py::dict>(dictionary[key]);
    }

    static uint32_t RegisterSurvivorAgent(Entity entity, const py::dict& monster)
    {
        const uint32_t entityId = static_cast<uint32_t>(entity);
        SurvivorAgentState* state = nullptr;
        const auto iterator = sSurvivorAgentIndices.find(entityId);
        if (iterator == sSurvivorAgentIndices.end())
        {
            sSurvivorAgentIndices[entityId] = sSurvivorAgents.size();
            sSurvivorAgents.emplace_back();
            state = &sSurvivorAgents.back();
        }
        else
        {
            state = &sSurvivorAgents[iterator->second];
        }

        state->RuntimeEntity = entity;
        state->AnimationControllerPath = monster.contains(py::str("animation_controller"))
            ? py::cast<std::string>(monster["animation_controller"])
            : std::string();
        state->EntityId = entityId;
        state->ActivationId = py::cast<uint64_t>(monster["activation_id"]);
        state->Health = py::cast<float>(monster["health"]);
        state->MaxHealth = monster.contains(py::str("max_health"))
            ? py::cast<float>(monster["max_health"])
            : state->Health;
        state->Speed = py::cast<float>(monster["speed"]);
        state->Damage = py::cast<float>(monster["damage"]);
        state->Radius = py::cast<float>(monster["radius"]);
        state->BaseScale = py::cast<float>(monster["base_scale"]);
        state->TouchTimer = py::cast<float>(monster["touch_timer"]);
        state->AttackWindup = py::cast<float>(monster["attack_windup"]);
        state->AttackCooldown = py::cast<float>(monster["attack_cooldown"]);
        state->AnimationWarmup = py::cast<float>(monster["animation_warmup"]);
        state->DeathTimer = py::cast<float>(monster["death_timer"]);
        state->HitFlash = py::cast<float>(monster["hit_flash"]);
        state->KnockX = py::cast<float>(monster["knock_x"]);
        state->KnockZ = py::cast<float>(monster["knock_z"]);
        state->Phase = py::cast<float>(monster["phase"]);
        state->GroundOffset = monster.contains(py::str("ground_offset"))
            ? py::cast<float>(monster["ground_offset"])
            : 0.0f;
        state->GroundProbeTimer = std::fmod(
            std::abs(state->Phase),
            kSurvivorGroundProbeInterval);
        state->GroundHeight = entity.HasComponent<TransformComponent>()
            ? entity.GetComponent<TransformComponent>().Translation.y
            : 0.0f;
        state->GroundX = entity.HasComponent<TransformComponent>()
            ? entity.GetComponent<TransformComponent>().Translation.x
            : 0.0f;
        state->GroundZ = entity.HasComponent<TransformComponent>()
            ? entity.GetComponent<TransformComponent>().Translation.z
            : 0.0f;
        state->bAttacking = py::cast<bool>(monster["attacking"]);
        state->bBoss = py::cast<bool>(monster["boss"]);
        state->bObjectiveBoss = SurvivorDictBool(
            monster,
            "objectiveBoss",
            SurvivorDictBool(monster, "objective_boss", false));
        state->bFinalBoss = SurvivorDictBool(
            monster,
            "finalBoss",
            SurvivorDictBool(monster, "final_boss", false));
        state->bNoDistanceRetire = SurvivorDictBool(
            monster,
            "no_distance_retire",
            state->bObjectiveBoss);
        state->bAnimated = py::cast<bool>(monster["animated"]);
        state->bDead = py::cast<bool>(monster["dead"]);
        state->bDeathAnimationSent = py::cast<bool>(monster["death_anim_sent"]);
        state->bHasGroundHeight = false;
        state->bActive = true;
        state->LastAttackPhase = SurvivorAttackPhase::Idle;

        const py::dict attack = SurvivorNestedDict(monster, "attack");
        const py::dict resistances = SurvivorNestedDict(monster, "resistances");
        SurvivorAgentBatchState batchState;
        batchState.Key = { entityId, state->ActivationId };
        batchState.Behavior = ParseSurvivorEnemyBehavior(monster);
        batchState.BossPhase = ParseSurvivorBossPhase(monster);
        batchState.BossBehavior = ParseSurvivorBossBehavior(monster);
        if (entity.HasComponent<TransformComponent>())
        {
            const glm::vec3& position = entity.GetComponent<TransformComponent>().Translation;
            batchState.X = position.x;
            batchState.Y = position.y;
            batchState.Z = position.z;
        }
        batchState.MovementPhase = state->Phase;
        batchState.Health = state->Health;
        batchState.MaxHealth = state->MaxHealth;
        batchState.Speed = state->Speed;
        batchState.Damage = state->Damage;
        batchState.Radius = state->Radius;
        batchState.Attack.Range = SurvivorDictNumber(
            attack,
            "range",
            state->Radius + 0.42);
        batchState.Attack.PreferredRange = SurvivorDictNumber(
            attack,
            "preferred_range",
            std::max(5.0, batchState.Attack.Range));
        batchState.Attack.Cooldown = SurvivorDictNumber(
            attack,
            "cooldown",
            state->bBoss ? 0.82 : 1.05);
        batchState.Attack.SourceHitCooldown = SurvivorDictNumber(
            attack,
            "source_cooldown",
            0.65);
        batchState.Attack.TelegraphDuration = SurvivorDictNumber(
            attack,
            "telegraph",
            state->bBoss ? 0.85 : 0.35);
        batchState.Attack.ActiveDuration = SurvivorDictNumber(
            attack,
            "active",
            0.08);
        batchState.Attack.RecoveryDuration = SurvivorDictNumber(
            attack,
            "recovery",
            0.18);
        batchState.Attack.ChargeSpeed = SurvivorDictNumber(
            attack,
            "charge_speed",
            9.0);
        batchState.Attack.ProjectileSpeed = SurvivorDictNumber(
            attack,
            "projectile_speed",
            7.5);
        batchState.Attack.ProjectileLifetime = SurvivorDictNumber(
            attack,
            "projectile_lifetime",
            2.4);
        batchState.Attack.ProjectileRadius = SurvivorDictNumber(
            attack,
            "projectile_radius",
            0.28);
        batchState.Attack.ProjectileDamageScale = SurvivorDictNumber(
            attack,
            "projectile_damage_scale",
            1.0);
        batchState.Attack.ZoneRadius = SurvivorDictNumber(
            attack,
            "zone_radius",
            1.6);
        batchState.Attack.ZoneDuration = SurvivorDictNumber(
            attack,
            "zone_duration",
            2.0);
        batchState.Attack.ZoneTickInterval = SurvivorDictNumber(
            attack,
            "zone_tick",
            0.5);
        batchState.Attack.ZoneDamageScale = SurvivorDictNumber(
            attack,
            "zone_damage_scale",
            0.75);
        batchState.Attack.SuicideDamageScale = SurvivorDictNumber(
            attack,
            "suicide_damage_scale",
            1.5);
        batchState.Attack.ShieldStrength = SurvivorDictNumber(
            attack,
            "shield",
            0.0);
        batchState.Attack.ShieldDuration = SurvivorDictNumber(
            attack,
            "shield_duration",
            0.0);
        batchState.Attack.SummonCount = SurvivorDictCount(
            attack,
            "summon_count",
            2);
        batchState.Resistances.Damage = SurvivorDictNumber(
            resistances,
            "damage",
            0.0);
        batchState.Resistances.Slow = SurvivorDictNumber(
            resistances,
            "slow",
            0.0);
        batchState.Resistances.Freeze = SurvivorDictNumber(
            resistances,
            "freeze",
            0.0);
        batchState.Resistances.Poison = SurvivorDictNumber(
            resistances,
            "poison",
            0.0);
        batchState.Resistances.Knockback = SurvivorDictNumber(
            resistances,
            "knockback",
            0.0);
        batchState.AttackCooldownRemaining = state->AttackCooldown;
        batchState.bObjectiveBoss = state->bObjectiveBoss;
        batchState.bFinalBoss = state->bFinalBoss;
        batchState.bNoDistanceRetire = state->bNoDistanceRetire;
        if (!sSurvivorAgentBatch.Activate(std::move(batchState)))
        {
            throw py::value_error(
                "A Survivor agent activation must be newer than its prior generation.");
        }
        sSurvivorTargetingDirty = true;
        return entityId;
    }

    static void DeactivateSurvivorAgent(Entity entity)
    {
        if (SurvivorAgentState* state = FindSurvivorAgent(entity))
        {
            (void)sSurvivorAgentBatch.Deactivate({
                state->EntityId,
                state->ActivationId
            });
            state->bActive = false;
            sSurvivorTargetingDirty = true;
        }
    }

    static void ResetSurvivorAgents()
    {
        constexpr size_t kExpectedSurvivorAgentCount = 512;
        sSurvivorAgents.clear();
        sSurvivorAgentIndices.clear();
        sSurvivorAgentBatch.Clear();
        sSurvivorAgentBatchOutput.ClearFrame(0.0);
        sSurvivorTargetingBatch.Clear();
        sSurvivorTargetSamples.clear();
        sSurvivorDensityHits.clear();
        sSurvivorRadialTargetHits.clear();
        sSurvivorCapsuleTargetHits.clear();
        sSurvivorCorridorQueries.clear();
        sSurvivorCorridorHits.clear();
        sSurvivorTargetingDirty = false;
        sSurvivorAgents.reserve(kExpectedSurvivorAgentCount);
        sSurvivorAgentIndices.reserve(kExpectedSurvivorAgentCount);
    }

    static float ApplySurvivorAgentHit(Entity entity, float damage, float knockX, float knockZ)
    {
        SurvivorAgentState* state = FindSurvivorAgent(entity);
        if (!state || !state->bActive || state->bDead)
            return 0.0f;

        const SurvivorAgentHitResult hit = sSurvivorAgentBatch.ApplyHit(
            { state->EntityId, state->ActivationId },
            damage,
            knockX,
            knockZ);
        state->Health = static_cast<float>(hit.Health);
        state->HitFlash = 0.09f;
        if (state->bAnimated &&
            state->AnimationWarmup <= 0.0f &&
            entity.HasComponent<AnimatorComponent>())
        {
            AnimatorComponent& animator = entity.GetComponent<AnimatorComponent>();
            if (animator.HasParameter("Hit"))
                animator.SetTrigger("Hit");
        }
        return state->Health;
    }

    static bool SetSurvivorAgentHealth(
        Entity entity,
        float health,
        uint64_t generation)
    {
        if (!std::isfinite(health) || health < 0.0f)
        {
            throw py::value_error(
                "Survivor agent health must be finite and non-negative.");
        }

        SurvivorAgentState* state = FindSurvivorAgent(entity);
        if (!state || !state->bActive || state->ActivationId != generation)
            return false;
        if (!sSurvivorAgentBatch.SetHealth(
                { state->EntityId, state->ActivationId },
                health))
        {
            return false;
        }

        const SurvivorAgentBatchState* batchState = sSurvivorAgentBatch.FindAgent({
            state->EntityId,
            state->ActivationId
        });
        if (!batchState)
            return false;
        state->Health = static_cast<float>(batchState->Health);
        state->bDead = batchState->bDead;
        sSurvivorTargetingDirty = true;
        return true;
    }

    static bool SetSurvivorAgentDistanceRetirement(
        Entity entity,
        bool bEnabled,
        uint64_t generation)
    {
        SurvivorAgentState* state = FindSurvivorAgent(entity);
        if (!state || !state->bActive || state->ActivationId != generation)
            return false;
        if (!sSurvivorAgentBatch.SetDistanceRetirement(
                { state->EntityId, state->ActivationId },
                bEnabled))
        {
            return false;
        }

        const SurvivorAgentBatchState* batchState = sSurvivorAgentBatch.FindAgent({
            state->EntityId,
            state->ActivationId
        });
        if (!batchState)
            return false;
        state->bNoDistanceRetire = batchState->bNoDistanceRetire;
        return true;
    }

    static py::tuple ApplySurvivorAgentStatus(
        Entity entity,
        const std::string& statusId,
        float magnitude,
        float duration,
        uint64_t generation)
    {
        SurvivorStatusKind status = SurvivorStatusKind::Slow;
        if (statusId == "slow")
            status = SurvivorStatusKind::Slow;
        else if (statusId == "freeze")
            status = SurvivorStatusKind::Freeze;
        else if (statusId == "poison")
            status = SurvivorStatusKind::Poison;
        else if (statusId == "interrupt" || statusId == "action_delay")
            status = SurvivorStatusKind::Interrupt;
        else
            throw py::value_error("Unknown Survivor status: " + statusId);

        if (!std::isfinite(magnitude) || magnitude < 0.0f ||
            !std::isfinite(duration) || duration < 0.0f)
        {
            throw py::value_error(
                "Survivor status magnitude and duration must be finite and non-negative.");
        }

        SurvivorAgentState* state = FindSurvivorAgent(entity);
        if (!state || !state->bActive || state->bDead ||
            state->ActivationId != generation)
        {
            return py::make_tuple(false, 0.0, false);
        }

        const SurvivorStatusApplyResult result = sSurvivorAgentBatch.ApplyStatus(
            { state->EntityId, state->ActivationId },
            status,
            duration,
            magnitude);
        return py::make_tuple(
            true,
            result.AppliedDuration,
            result.bResisted);
    }

    static float KillSurvivorAgent(Entity entity)
    {
        SurvivorAgentState* state = FindSurvivorAgent(entity);
        if (!state || !state->bActive)
            return 0.0f;
        if (state->bDead)
            return state->DeathTimer;

        state->bDead = true;
        state->bAttacking = false;
        state->DeathTimer = state->bAnimated ? 0.72f : 0.3f;
        state->KnockX *= 0.35f;
        state->KnockZ *= 0.35f;
        (void)sSurvivorAgentBatch.Kill({
            state->EntityId,
            state->ActivationId
        });
        sSurvivorTargetingDirty = true;
        return state->DeathTimer;
    }

    static py::tuple UpdateSurvivorAgents(
        const glm::vec3& playerPosition,
        float deltaTime,
        float elapsed,
        float damageCooldown,
        float health,
        float damageReduction,
        uint32_t randomState,
        int maxAnimatedMonsters,
        float animationLodDistance,
        float animationLodReleaseDistance,
        float despawnDistanceSquared,
        const std::string& animationControllerPath)
    {
        PhysicSystem3D* physicsSystem = nullptr;
        if (Level* activeLevel = PythonScriptSystem::GetActiveLevel())
            physicsSystem = activeLevel->TryGetSystem<PhysicSystem3D>();

        std::vector<uint32_t> retiredEntityIds;
        const auto queueRetired = [&retiredEntityIds](uint32_t entityId)
        {
            if (std::find(
                retiredEntityIds.begin(),
                retiredEntityIds.end(),
                entityId) == retiredEntityIds.end())
            {
                retiredEntityIds.push_back(entityId);
            }
        };
        int animatedCount = 0;
        for (SurvivorAgentState& state : sSurvivorAgents)
        {
            if (state.bActive && state.bAnimated && !state.bDead)
                ++animatedCount;
            if (!state.bActive || state.bDead ||
                !state.RuntimeEntity.HasComponent<TransformComponent>())
            {
                continue;
            }
            const glm::vec3& position =
                state.RuntimeEntity.GetComponent<TransformComponent>().Translation;
            (void)sSurvivorAgentBatch.SetPosition(
                { state.EntityId, state.ActivationId },
                position.x,
                position.y,
                position.z);
        }

        constexpr float damageFlashTimer = 0.0f;
        constexpr float bannerTimer = 0.0f;
        constexpr bool bGameOver = false;
        (void)damageReduction;
        sSurvivorAgentBatch.Update({
            playerPosition.x,
            playerPosition.y,
            playerPosition.z,
            0.42,
            deltaTime,
            elapsed,
            damageCooldown,
            0.45
        }, sSurvivorAgentBatchOutput);
        damageCooldown = static_cast<float>(
            sSurvivorAgentBatchOutput.PlayerHurtIFrameRemaining);
        py::list rawDamageEvents;
        for (const SurvivorPlayerDamageEvent& damageEvent :
            sSurvivorAgentBatchOutput.DamageEvents)
        {
            const char* kindId = "melee";
            switch (damageEvent.Kind)
            {
            case SurvivorDamageKind::Melee:
                kindId = "melee";
                break;
            case SurvivorDamageKind::Charge:
                kindId = "charge";
                break;
            case SurvivorDamageKind::Projectile:
                kindId = "projectile";
                break;
            case SurvivorDamageKind::Zone:
                kindId = "zone";
                break;
            case SurvivorDamageKind::Suicide:
                kindId = "suicide";
                break;
            }
            rawDamageEvents.append(py::make_tuple(
                damageEvent.Source.EntityId,
                damageEvent.Source.ActivationId,
                damageEvent.Damage,
                kindId));
        }
        const auto hazardEventName = [](SurvivorHazardEventKind kind)
        {
            switch (kind)
            {
            case SurvivorHazardEventKind::Spawned: return "spawned";
            case SurvivorHazardEventKind::Hit: return "hit";
            case SurvivorHazardEventKind::Tick: return "tick";
            case SurvivorHazardEventKind::Expired: return "expired";
            }
            return "expired";
        };
        py::list projectileEvents;
        for (const SurvivorProjectileEvent& event :
            sSurvivorAgentBatchOutput.ProjectileEvents)
        {
            projectileEvents.append(py::make_tuple(
                event.Handle.Slot,
                event.Handle.Generation,
                event.Owner.EntityId,
                event.Owner.ActivationId,
                hazardEventName(event.Kind),
                event.X,
                event.Y,
                event.Z));
        }
        py::list zoneEvents;
        for (const SurvivorZoneEvent& event :
            sSurvivorAgentBatchOutput.ZoneEvents)
        {
            zoneEvents.append(py::make_tuple(
                event.Handle.Slot,
                event.Handle.Generation,
                event.Owner.EntityId,
                event.Owner.ActivationId,
                hazardEventName(event.Kind),
                event.X,
                event.Y,
                event.Z,
                event.Radius));
        }
        py::list summonEvents;
        for (const SurvivorSummonEvent& event :
            sSurvivorAgentBatchOutput.SummonEvents)
        {
            summonEvents.append(py::make_tuple(
                event.Source.EntityId,
                event.Source.ActivationId,
                static_cast<uint8_t>(event.Behavior),
                event.X,
                event.Y,
                event.Z,
                event.Count));
        }
        py::dict enemyTelemetry;
        enemyTelemetry["activeAgents"] = sSurvivorAgentBatchOutput.Telemetry.ActiveAgents;
        enemyTelemetry["telegraphsStarted"] = sSurvivorAgentBatchOutput.Telemetry.TelegraphsStarted;
        enemyTelemetry["attacksActivated"] = sSurvivorAgentBatchOutput.Telemetry.AttacksActivated;
        enemyTelemetry["playerDamageEvents"] = sSurvivorAgentBatchOutput.Telemetry.PlayerDamageEvents;
        enemyTelemetry["projectileSpawns"] = sSurvivorAgentBatchOutput.Telemetry.ProjectileSpawns;
        enemyTelemetry["projectileHits"] = sSurvivorAgentBatchOutput.Telemetry.ProjectileHits;
        enemyTelemetry["zoneSpawns"] = sSurvivorAgentBatchOutput.Telemetry.ZoneSpawns;
        enemyTelemetry["zoneTicks"] = sSurvivorAgentBatchOutput.Telemetry.ZoneTicks;
        enemyTelemetry["summonRequests"] = sSurvivorAgentBatchOutput.Telemetry.SummonRequests;
        enemyTelemetry["bossPhaseChanges"] = sSurvivorAgentBatchOutput.Telemetry.BossPhaseChanges;
        enemyTelemetry["bossRingBarrages"] = sSurvivorAgentBatchOutput.Telemetry.BossRingBarrages;
        enemyTelemetry["bossSweepBreaths"] = sSurvivorAgentBatchOutput.Telemetry.BossSweepBreaths;
        enemyTelemetry["bossFinalCharges"] = sSurvivorAgentBatchOutput.Telemetry.BossFinalCharges;
        enemyTelemetry["bossEnrages"] = sSurvivorAgentBatchOutput.Telemetry.BossEnrages;
        enemyTelemetry["statusApplications"] = sSurvivorAgentBatchOutput.Telemetry.StatusApplications;
        enemyTelemetry["statusResists"] = sSurvivorAgentBatchOutput.Telemetry.StatusResists;
        enemyTelemetry["shieldDamagePrevented"] = sSurvivorAgentBatchOutput.Telemetry.ShieldDamagePrevented;
        py::list telegraphEvents;
        for (const SurvivorAgentKey& retired :
            sSurvivorAgentBatchOutput.RetiredAgents)
        {
            queueRetired(retired.EntityId);
        }

        for (SurvivorAgentState& state : sSurvivorAgents)
        {
            if (!state.bActive)
                continue;

            Entity& entity = state.RuntimeEntity;
            if (!entity.HasComponent<TransformComponent>())
            {
                queueRetired(state.EntityId);
                continue;
            }

            TransformComponent& transform = entity.GetComponent<TransformComponent>();
            const glm::vec3 previousPosition = transform.Translation;
            if (state.bDead)
            {
                state.AnimationWarmup = std::max(0.0f, state.AnimationWarmup - deltaTime);
                if (state.bAnimated &&
                    !state.bDeathAnimationSent &&
                    state.AnimationWarmup <= 0.0f &&
                    entity.HasComponent<AnimatorComponent>())
                {
                    AnimatorComponent& animator = entity.GetComponent<AnimatorComponent>();
                    if (animator.HasParameter("Attacking"))
                        animator.SetBool("Attacking", false);
                    if (animator.HasParameter("Dead"))
                        animator.SetBool("Dead", true);
                    else if (animator.HasParameter("Speed"))
                        animator.SetFloat("Speed", 0.0f);
                    state.bDeathAnimationSent = true;
                }

                state.DeathTimer -= deltaTime;
                if (state.DeathTimer < 0.24f)
                {
                    const float fraction = std::max(0.0f, state.DeathTimer / 0.24f);
                    transform.Scale = glm::vec3(state.BaseScale * fraction);
                }
                if (state.DeathTimer <= 0.0f)
                    queueRetired(state.EntityId);
                continue;
            }

            const SurvivorAgentKey batchKey = {
                state.EntityId,
                state.ActivationId
            };
            const SurvivorAgentBatchState* batchState =
                sSurvivorAgentBatch.FindAgent(batchKey);
            if (!batchState || !batchState->bActive)
            {
                queueRetired(state.EntityId);
                continue;
            }

            if (batchState->AttackPhase != state.LastAttackPhase)
            {
                const char* phaseId = "end";
                if (batchState->AttackPhase == SurvivorAttackPhase::Telegraph)
                    phaseId = "start";
                else if (batchState->AttackPhase == SurvivorAttackPhase::Active)
                    phaseId = "active";
                telegraphEvents.append(py::make_tuple(
                    state.EntityId,
                    state.ActivationId,
                    phaseId,
                    static_cast<uint8_t>(batchState->Behavior),
                    batchState->X,
                    batchState->Y,
                    batchState->Z,
                    batchState->Attack.Range,
                    batchState->AttackPhaseRemaining,
                    state.bBoss));
                state.LastAttackPhase = batchState->AttackPhase;
            }

            glm::vec3 position(
                static_cast<float>(batchState->X),
                static_cast<float>(batchState->Y),
                static_cast<float>(batchState->Z));
            state.Health = static_cast<float>(batchState->Health);

            const float deltaX = playerPosition.x - position.x;
            const float deltaZ = playerPosition.z - position.z;
            const float distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
            if (!state.bNoDistanceRetire &&
                distanceSquared > despawnDistanceSquared)
            {
                queueRetired(state.EntityId);
                continue;
            }

            const float distance = std::max(0.001f, std::sqrt(distanceSquared));
            const float attackDistance = static_cast<float>(batchState->Attack.Range);
            const float verticalDistance = std::abs(playerPosition.y - position.y);
            const bool bInAttackRange =
                distance <= attackDistance && verticalDistance <= 2.25f;
            const bool bShouldAnimate =
                state.bBoss ||
                (state.bAnimated && distance <= animationLodReleaseDistance) ||
                ((bInAttackRange || distance <= animationLodDistance) &&
                    animatedCount < maxAnimatedMonsters);

            if (bShouldAnimate && !state.bAnimated && entity.HasComponent<AnimatorComponent>())
            {
                AnimatorComponent& animator = entity.GetComponent<AnimatorComponent>();
                animator.SetControllerPath(
                    state.AnimationControllerPath.empty()
                        ? animationControllerPath
                        : state.AnimationControllerPath);
                animator.SetSpeed(SurvivorRandomUniform(randomState, 0.95f, 1.22f));
                animator.Play();
                if (animator.HasParameter("Speed"))
                    animator.SetFloat("Speed", 1.0f);
                state.bAnimated = true;
                state.AnimationWarmup = 0.14f;
                ++animatedCount;
            }
            else if (state.bAnimated &&
                !state.bBoss &&
                !bInAttackRange &&
                distance > animationLodReleaseDistance &&
                entity.HasComponent<AnimatorComponent>())
            {
                AnimatorComponent& animator = entity.GetComponent<AnimatorComponent>();
                animator.SetControllerPath("None");
                animator.Stop();
                state.bAnimated = false;
                state.bAttacking = false;
                state.AnimationWarmup = 0.0f;
                animatedCount = std::max(0, animatedCount - 1);
            }

            transform.Translation = position;

            state.GroundProbeTimer = std::max(0.0f, state.GroundProbeTimer - deltaTime);
            if (physicsSystem && (!state.bHasGroundHeight || state.GroundProbeTimer <= 0.0f))
            {
                float floorHeight = 0.0f;
                const glm::vec3 probePosition(
                    position.x,
                    position.y - state.GroundOffset,
                    position.z);
                if (physicsSystem->RaycastFloorHeight(probePosition, 8.0f, 24.0f, floorHeight))
                {
                    state.GroundHeight = floorHeight + state.GroundOffset;
                    state.GroundX = position.x;
                    state.GroundZ = position.z;
                    state.bHasGroundHeight = true;
                    state.GroundProbeTimer = kSurvivorGroundProbeInterval;
                }
                else
                {
                    if (state.bHasGroundHeight)
                    {
                        position.x = state.GroundX;
                        position.z = state.GroundZ;
                    }
                    else
                    {
                        position = previousPosition;
                    }
                    state.GroundProbeTimer = 0.0f;
                }
            }

            if (state.bHasGroundHeight)
            {
                const float groundFollowAlpha = std::min(1.0f, deltaTime * 18.0f);
                position.y = glm::mix(position.y, state.GroundHeight, groundFollowAlpha);
            }
            transform.Translation = position;
            float facingX = playerPosition.x - position.x;
            float facingZ = playerPosition.z - position.z;
            if (batchState->AttackPhase == SurvivorAttackPhase::Idle)
            {
                const float movementX = position.x - previousPosition.x;
                const float movementZ = position.z - previousPosition.z;
                if (movementX * movementX + movementZ * movementZ > 1.0e-8f)
                {
                    facingX = movementX;
                    facingZ = movementZ;
                }
            }
            if (facingX * facingX + facingZ * facingZ > 1.0e-8f)
            {
                transform.Rotation = glm::vec3(
                    0.0f,
                    SurvivorAtan2Approx(facingX, facingZ),
                    0.0f);
            }
            (void)sSurvivorAgentBatch.SetPosition(
                batchKey,
                position.x,
                position.y,
                position.z);

            state.HitFlash = std::max(0.0f, state.HitFlash - deltaTime);
            float pulse = state.HitFlash > 0.0f ? 1.13f : 1.0f;
            if (!state.bAnimated)
                pulse *= 1.0f + SurvivorSinApprox(elapsed * 4.0f + state.Phase) * 0.025f;
            transform.Scale = glm::vec3(state.BaseScale * pulse);

            state.TouchTimer = static_cast<float>(
                batchState->SourceHitCooldownRemaining);
            state.AttackCooldown = static_cast<float>(
                batchState->AttackCooldownRemaining);
            state.AttackWindup = batchState->AttackPhase ==
                SurvivorAttackPhase::Telegraph
                ? static_cast<float>(batchState->AttackPhaseRemaining)
                : 0.0f;
            state.AnimationWarmup = std::max(0.0f, state.AnimationWarmup - deltaTime);
            const bool bWantsAttack =
                batchState->AttackPhase == SurvivorAttackPhase::Telegraph ||
                batchState->AttackPhase == SurvivorAttackPhase::Active;
            if (state.bAnimated &&
                state.AnimationWarmup <= 0.0f &&
                state.bAttacking != bWantsAttack &&
                entity.HasComponent<AnimatorComponent>())
            {
                AnimatorComponent& animator = entity.GetComponent<AnimatorComponent>();
                if (animator.HasParameter("Dead"))
                    animator.SetBool("Dead", false);
                if (animator.HasParameter("Attacking"))
                    animator.SetBool("Attacking", bWantsAttack);
                else if (animator.HasParameter("Speed"))
                    animator.SetFloat("Speed", bWantsAttack ? 0.0f : 1.0f);
                state.bAttacking = bWantsAttack;
            }
        }

        sSurvivorTargetingDirty = true;

        return py::make_tuple(
            retiredEntityIds,
            damageCooldown,
            health,
            damageFlashTimer,
            bannerTimer,
            bGameOver,
            randomState,
            rawDamageEvents,
            projectileEvents,
            zoneEvents,
            summonEvents,
            telegraphEvents,
            enemyTelemetry);
    }

    // 生成的 PyBinding 仅暴露字段，此处补充 AnimatorComponent 方法绑定
    static void ExtendAnimatorComponentBinding(py::module_& HEngineModule)
    {
        py::object animCls = HEngineModule.attr("AnimatorComponent");
        animCls.attr("Play") = py::cpp_function(
            [](AnimatorComponent& self, int clipIndex) { self.Play(clipIndex); },
            py::is_method(animCls),
            py::arg("clipIndex") = -1
        );
        animCls.attr("Stop") = py::cpp_function(
            [](AnimatorComponent& self) { self.Stop(); },
            py::is_method(animCls)
        );
        animCls.attr("Pause") = py::cpp_function(
            [](AnimatorComponent& self) { self.Pause(); }, py::is_method(animCls));
        animCls.attr("Resume") = py::cpp_function(
            [](AnimatorComponent& self) { self.Resume(); }, py::is_method(animCls));
        animCls.attr("Step") = py::cpp_function(
            [](AnimatorComponent& self, float seconds) { self.Step(seconds); },
            py::is_method(animCls), py::arg("seconds"));
        animCls.attr("ConsumeEvents") = py::cpp_function(
            [](AnimatorComponent& self) -> py::object
            {
                return py::module_::import("json").attr("loads")(self.ConsumeEvents().dump());
            }, py::is_method(animCls));
        animCls.attr("SetSpeed") = py::cpp_function(
            [](AnimatorComponent& self, float speed) { self.SetSpeed(speed); },
            py::is_method(animCls),
            py::arg("speed")
        );
        animCls.attr("GetControllerPath") = py::cpp_function(
            [](const AnimatorComponent& self) -> std::string { return self.GetControllerPath(); },
            py::is_method(animCls)
        );
        animCls.attr("SetControllerPath") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& path) { self.SetControllerPath(path); },
            py::is_method(animCls),
            py::arg("path")
        );
        animCls.attr("get_controller_path") = py::cpp_function(
            [](const AnimatorComponent& self) -> std::string { return self.GetControllerPath(); },
            py::is_method(animCls)
        );
        animCls.attr("set_controller_path") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& path) { self.SetControllerPath(path); },
            py::is_method(animCls),
            py::arg("path")
        );
        animCls.attr("GetClipCount") = py::cpp_function(
            [](const AnimatorComponent& self) -> int { return self.GetClipCount(); },
            py::is_method(animCls)
        );
        animCls.attr("GetClipName") = py::cpp_function(
            [](const AnimatorComponent& self, int index) -> std::string { return self.GetClipName(index); },
            py::is_method(animCls),
            py::arg("index")
        );
        animCls.attr("GetProgress") = py::cpp_function(
            [](const AnimatorComponent& self) -> float { return self.GetProgress(); },
            py::is_method(animCls)
        );
        animCls.attr("set_bool") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name, bool value) { self.SetBool(name, value); },
            py::is_method(animCls),
            py::arg("name"), py::arg("value")
        );
        animCls.attr("SetBool") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name, bool value) { self.SetBool(name, value); },
            py::is_method(animCls),
            py::arg("name"), py::arg("value")
        );
        animCls.attr("set_int") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name, int value) { self.SetInt(name, value); },
            py::is_method(animCls),
            py::arg("name"), py::arg("value")
        );
        animCls.attr("set_float") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name, float value) { self.SetFloat(name, value); },
            py::is_method(animCls),
            py::arg("name"), py::arg("value")
        );
        animCls.attr("set_trigger") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name) { self.SetTrigger(name); },
            py::is_method(animCls),
            py::arg("name")
        );
        animCls.attr("SetTrigger") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name) { self.SetTrigger(name); },
            py::is_method(animCls),
            py::arg("name")
        );
        animCls.attr("reset_trigger") = py::cpp_function(
            [](AnimatorComponent& self, const std::string& name) { self.ResetTrigger(name); },
            py::is_method(animCls),
            py::arg("name")
        );
        animCls.attr("has_parameter") = py::cpp_function(
            [](const AnimatorComponent& self, const std::string& name) -> bool { return self.HasParameter(name); },
            py::is_method(animCls),
            py::arg("name")
        );
        animCls.attr("get_bool") = py::cpp_function(
            [](const AnimatorComponent& self, const std::string& name, bool defaultValue) -> bool { return self.GetBool(name, defaultValue); },
            py::is_method(animCls),
            py::arg("name"), py::arg("default_value") = false
        );
        animCls.attr("get_int") = py::cpp_function(
            [](const AnimatorComponent& self, const std::string& name, int defaultValue) -> int { return self.GetInt(name, defaultValue); },
            py::is_method(animCls),
            py::arg("name"), py::arg("default_value") = 0
        );
        animCls.attr("get_float") = py::cpp_function(
            [](const AnimatorComponent& self, const std::string& name, float defaultValue) -> float { return self.GetFloat(name, defaultValue); },
            py::is_method(animCls),
            py::arg("name"), py::arg("default_value") = 0.0f
        );
        animCls.attr("get_trigger") = py::cpp_function(
            [](const AnimatorComponent& self, const std::string& name) -> bool { return self.GetTrigger(name); },
            py::is_method(animCls),
            py::arg("name")
        );
        animCls.attr("get_current_state") = py::cpp_function(
            [](const AnimatorComponent& self) -> std::string { return self.GetCurrentState(); },
            py::is_method(animCls)
        );
    }

    static void ExtendSequenceComponentBinding(py::module_& HEngineModule)
    {
        py::object cls = HEngineModule.attr("SequenceComponent");
        cls.attr("play") = py::cpp_function([](SequenceComponent& self)
            { self.Playing = true; self.Paused = false; }, py::is_method(cls));
        cls.attr("stop") = py::cpp_function([](SequenceComponent& self)
            { self.Playing = false; }, py::is_method(cls));
        cls.attr("pause") = py::cpp_function([](SequenceComponent& self, bool paused)
            { self.Paused = paused; }, py::is_method(cls), py::arg("paused") = true);
        cls.attr("seek") = py::cpp_function([](SequenceComponent& self, float seconds)
            {
                if (!std::isfinite(seconds) || seconds < 0.0f || seconds > self.Duration)
                    throw py::value_error("Seek time must be finite and inside the sequence duration.");
                self.SeekTime = seconds;
            }, py::is_method(cls), py::arg("seconds"));
        cls.attr("time") = py::cpp_function([](const SequenceComponent& self)
            { return self.Time; }, py::is_method(cls));
        cls.attr("is_playing") = py::cpp_function([](const SequenceComponent& self)
            { return self.Playing; }, py::is_method(cls));
        cls.attr("error") = py::cpp_function([](const SequenceComponent& self)
            { return self.Error; }, py::is_method(cls));
        cls.attr("consume_events") = py::cpp_function([](SequenceComponent& self) -> py::object
            {
                py::object result = py::module_::import("json").attr("loads")(self.Events.dump());
                self.Events = Json::array();
                return result;
            }, py::is_method(cls));
    }

    static void ExtendBehaviorTreeComponentBinding(py::module_& HEngineModule)
    {
        py::object btCls = HEngineModule.attr("BehaviorTreeComponent");
        btCls.attr("get_blackboard") = py::cpp_function(
            [](BehaviorTreeComponent& self) -> Blackboard& { return self.mBlackboard; },
            py::is_method(btCls),
            py::return_value_policy::reference_internal
        );
        btCls.attr("is_enabled") = py::cpp_function(
            [](const BehaviorTreeComponent& self) -> bool { return self.bEnabled; },
            py::is_method(btCls)
        );
        btCls.attr("set_enabled") = py::cpp_function(
            [](BehaviorTreeComponent& self, bool enabled) { self.bEnabled = enabled; },
            py::is_method(btCls),
            py::arg("enabled")
        );
        btCls.attr("get_bt_res_path") = py::cpp_function(
            [](const BehaviorTreeComponent& self) -> std::string { return self.btResPath; },
            py::is_method(btCls)
        );
        btCls.attr("set_bt_res_path") = py::cpp_function(
            [](BehaviorTreeComponent& self, const std::string& path) { self.btResPath = path; },
            py::is_method(btCls),
            py::arg("path")
        );
    }

    static void EcsBinding(py::module_& HEngineModule)
    {
        py::class_<UUID>(HEngineModule, "UUID")
            .def(py::init<>())
            .def(py::init<const std::string&>(), py::arg("value"))
            .def("str", &UUID::str)
            .def("isValid", &UUID::isValid)
            .def("__str__", &UUID::str)
            .def("__eq__", [](const UUID& self, py::object other) {
                return py::isinstance<UUID>(other) && self == other.cast<const UUID&>();
            })
            .def("__ne__", [](const UUID& self, py::object other) {
                return !py::isinstance<UUID>(other) || self != other.cast<const UUID&>();
            })
            // No UUID mutators are exposed to Python, so value hashing is stable.
            .def("__hash__", [](const UUID& self) { return std::hash<std::string>{}(self.str()); });

        auto sceneCamera = py::class_<SceneCamera>(HEngineModule, "SceneCamera");
        py::enum_<SceneCamera::ProjectionType>(sceneCamera, "ProjectionType")
            .value("Perspective", SceneCamera::ProjectionType::Perspective)
            .value("Orthographic", SceneCamera::ProjectionType::Orthographic);
        sceneCamera.def(py::init<>())
            .def("SetOrthographic", &SceneCamera::SetOrthographic,
                py::arg("size"), py::arg("nearClip"), py::arg("farClip"))
            .def("SetPerspective", &SceneCamera::SetPerspective,
                py::arg("verticalFOV"), py::arg("nearClip"), py::arg("farClip"))
            .def("SetViewportSize", &SceneCamera::SetViewportSize, py::arg("width"), py::arg("height"))
            .def("SetAspectRatio", &SceneCamera::SetAspectRatio, py::arg("aspect"))
            .def_property("ProjectionTypeValue", &SceneCamera::GetProjectionType, &SceneCamera::SetProjectionType)
            .def_property("PerspectiveVerticalFOV", &SceneCamera::GetPerspectiveVerticalFOV, &SceneCamera::SetPerspectiveVerticalFOV)
            .def_property("PerspectiveNearClip", &SceneCamera::GetPerspectiveNearClip, &SceneCamera::SetPerspectiveNearClip)
            .def_property("PerspectiveFarClip", &SceneCamera::GetPerspectiveFarClip, &SceneCamera::SetPerspectiveFarClip)
            .def_property("OrthographicSize", &SceneCamera::GetOrthographicSize, &SceneCamera::SetOrthographicSize)
            .def_property("OrthographicNearClip", &SceneCamera::GetOrthographicNearClip, &SceneCamera::SetOrthographicNearClip)
            .def_property("OrthographicFarClip", &SceneCamera::GetOrthographicFarClip, &SceneCamera::SetOrthographicFarClip);

        py::enum_<FollowCameraMode>(HEngineModule, "FollowCameraMode")
            .value("ThirdPerson", FollowCameraMode::ThirdPerson)
            .value("TopDown", FollowCameraMode::TopDown);
        py::enum_<CollisionShape>(HEngineModule, "CollisionShape")
            .value("None_", CollisionShape::None)
            .value("Box", CollisionShape::Box)
            .value("Sphere", CollisionShape::Sphere)
            .value("ConvexHull", CollisionShape::ConvexHull)
            .value("TriangleMesh", CollisionShape::TriangleMesh);
        BindReflectedPythonComponents(HEngineModule);
        // UUID is an immutable Python value. Do not retain a reference into a
        // component whose reflected setter can replace the underlying UUID.
        auto followCamera = HEngineModule.attr("FollowCameraComponent");
        followCamera.attr("TargetUUID") = py::module_::import("builtins").attr("property")(
            py::cpp_function([](const FollowCameraComponent& self) -> UUID { return self.TargetUUID; },
                py::is_method(followCamera), py::return_value_policy::copy),
            py::cpp_function([](FollowCameraComponent& self, const UUID& value) { self.TargetUUID = value; },
                py::is_method(followCamera)));
        py::enum_<Rigidbody2DComponent::BodyType>(HEngineModule.attr("Rigidbody2DComponent"), "BodyType")
            .value("Static", Rigidbody2DComponent::BodyType::Static)
            .value("Dynamic", Rigidbody2DComponent::BodyType::Dynamic)
            .value("Kinematic", Rigidbody2DComponent::BodyType::Kinematic);
        py::enum_<Rigidbody3DComponent::Body3DType>(HEngineModule.attr("Rigidbody3DComponent"), "Body3DType")
            .value("Static", Rigidbody3DComponent::Body3DType::Static)
            .value("Dynamic", Rigidbody3DComponent::Body3DType::Dynamic)
            .value("Kinematic", Rigidbody3DComponent::Body3DType::Kinematic);
        ExtendAnimatorComponentBinding(HEngineModule);
        ExtendSequenceComponentBinding(HEngineModule);

        py::class_<Blackboard>(HEngineModule, "Blackboard")
            .def("set_float", &Blackboard::SetFloat, py::arg("key"), py::arg("value"))
            .def("get_float", &Blackboard::GetFloat, py::arg("key"), py::arg("default_val") = 0.0f)
            .def("set_int", &Blackboard::SetInt, py::arg("key"), py::arg("value"))
            .def("get_int", &Blackboard::GetInt, py::arg("key"), py::arg("default_val") = 0)
            .def("set_bool", &Blackboard::SetBool, py::arg("key"), py::arg("value"))
            .def("get_bool", &Blackboard::GetBool, py::arg("key"), py::arg("default_val") = false)
            .def("set_vec3", &Blackboard::SetVec3, py::arg("key"), py::arg("value"))
            .def("get_vec3", &Blackboard::GetVec3, py::arg("key"), py::arg("default_val") = glm::vec3(0.0f))
            .def("set_entity_handle", &Blackboard::SetEntityHandle, py::arg("key"), py::arg("value"))
            .def("get_entity_handle", &Blackboard::GetEntityHandle, py::arg("key"), py::arg("default_val") = static_cast<uint32_t>(entt::null))
            .def("has_key", &Blackboard::HasKey, py::arg("key"))
            .def("remove_key", &Blackboard::RemoveKey, py::arg("key"))
            .def("clear", &Blackboard::Clear)
            .def("get_all_keys", &Blackboard::GetAllKeys);

        ExtendBehaviorTreeComponentBinding(HEngineModule);

#if HE_ENABLE_PROFILE
        py::enum_<SurvivorProfileStage>(HEngineModule, "_SurvivorProfileStage")
            .value("Update", SurvivorProfileStage::Update)
            .value("Bookkeeping", SurvivorProfileStage::Bookkeeping)
            .value("PendingPickupSpawns", SurvivorProfileStage::PendingPickupSpawns)
            .value("WaveSpawn", SurvivorProfileStage::WaveSpawn)
            .value("Monsters", SurvivorProfileStage::Monsters)
            .value("Pickups", SurvivorProfileStage::Pickups)
            .value("Weapons", SurvivorProfileStage::Weapons)
            .value("Hud", SurvivorProfileStage::Hud);
        HEngineModule.def(
            "_ProfileSurvivorStage",
            &ProfileSurvivorStage,
            py::arg("stage"),
            py::arg("callback"));
        HEngineModule.def("_MarkSurvivorGameplayFrame", []()
        {
            HE_PROFILE_SECTION("SurvivorGame::GameplayFrame");
        });
#endif

        HEngineModule.def(
            "UpdateSurvivorCrowd",
            &UpdateSurvivorCrowd,
            py::arg("monsters"),
            py::arg("player_position"),
            py::arg("delta_time"),
            py::arg("elapsed"),
            py::arg("damage_cooldown"),
            py::arg("health"),
            py::arg("damage_reduction"),
            py::arg("random_state"),
            py::arg("max_animated_monsters"),
            py::arg("animation_lod_distance"),
            py::arg("animation_lod_release_distance"),
            py::arg("despawn_distance_squared"),
            py::arg("animation_controller_path"));
        HEngineModule.def(
            "RegisterSurvivorAgent",
            &RegisterSurvivorAgent,
            py::arg("entity"),
            py::arg("monster"));
        HEngineModule.def(
            "DeactivateSurvivorAgent",
            &DeactivateSurvivorAgent,
            py::arg("entity"));
        HEngineModule.def("ResetSurvivorAgents", &ResetSurvivorAgents);
        HEngineModule.def(
            "QuerySurvivorAgentsInRadius",
            &QuerySurvivorAgentsInRadius,
            py::arg("origin_x"),
            py::arg("origin_y"),
            py::arg("origin_z"),
            py::arg("radius"),
            py::arg("vertical_range"));
        HEngineModule.def(
            "SelectDensestSurvivorAgents",
            &SelectDensestSurvivorAgents,
            py::arg("origin_x"),
            py::arg("origin_y"),
            py::arg("origin_z"),
            py::arg("attack_range"),
            py::arg("vertical_range"),
            py::arg("cluster_radius"),
            py::arg("target_count"));
        HEngineModule.def(
            "QuerySurvivorAgentCorridors",
            &QuerySurvivorAgentCorridors,
            py::arg("candidate_origin_x"),
            py::arg("candidate_origin_y"),
            py::arg("candidate_origin_z"),
            py::arg("candidate_range"),
            py::arg("candidate_vertical_range"),
            py::arg("corridor_specs"));
        HEngineModule.def(
            "SelectSurvivorAgentChain",
            &SelectSurvivorAgentChain,
            py::arg("origin_x"),
            py::arg("origin_y"),
            py::arg("origin_z"),
            py::arg("attack_range"),
            py::arg("vertical_range"),
            py::arg("link_range"),
            py::arg("maximum_targets"));
        HEngineModule.def(
            "QuerySurvivorAgentsInSweptAnnulus",
            &QuerySurvivorAgentsInSweptAnnulus,
            py::arg("origin_x"),
            py::arg("origin_y"),
            py::arg("origin_z"),
            py::arg("previous_radius"),
            py::arg("current_radius"),
            py::arg("edge_width"),
            py::arg("vertical_range"));
        HEngineModule.def(
            "FindSurvivorAgentCircleOverlap",
            &FindSurvivorAgentCircleOverlap,
            py::arg("origin_x"),
            py::arg("origin_y"),
            py::arg("origin_z"),
            py::arg("padding"),
            py::arg("vertical_range"));
        HEngineModule.def(
            "QuerySurvivorAgentsInSweptCapsule",
            &QuerySurvivorAgentsInSweptCapsule,
            py::arg("start_x"),
            py::arg("start_z"),
            py::arg("end_x"),
            py::arg("end_z"),
            py::arg("sweep_radius"));
        HEngineModule.def(
            "RegisterSurvivorPickup",
            &RegisterSurvivorPickup,
            py::arg("entity"),
            py::arg("activation_id"),
            py::arg("base_y"),
            py::arg("phase"),
            py::arg("rotation"),
            py::arg("scale_bonus"));
        HEngineModule.def(
            "SetSurvivorPickupScaleBonus",
            &SetSurvivorPickupScaleBonus,
            py::arg("entity"),
            py::arg("activation_id"),
            py::arg("scale_bonus"));
        HEngineModule.def(
            "DeactivateSurvivorPickup",
            &DeactivateSurvivorPickup,
            py::arg("entity"),
            py::arg("activation_id"));
        HEngineModule.def("ResetSurvivorPickups", &ResetSurvivorPickups);
        HEngineModule.def(
            "UpdateSurvivorPickups",
            &UpdateSurvivorPickups,
            py::arg("player_position"),
            py::arg("delta_time"),
            py::arg("magnet_range"),
            py::arg("pickup_range"),
            py::arg("gem_scale"));
        HEngineModule.def(
            "ApplySurvivorAgentHit",
            &ApplySurvivorAgentHit,
            py::arg("entity"),
            py::arg("damage"),
            py::arg("knock_x"),
            py::arg("knock_z"));
        HEngineModule.def(
            "SetSurvivorAgentHealth",
            &SetSurvivorAgentHealth,
            py::arg("entity"),
            py::arg("health"),
            py::arg("generation"));
        HEngineModule.def(
            "SetSurvivorAgentDistanceRetirement",
            &SetSurvivorAgentDistanceRetirement,
            py::arg("entity"),
            py::arg("enabled"),
            py::arg("generation"));
        HEngineModule.def(
            "ApplySurvivorAgentStatus",
            &ApplySurvivorAgentStatus,
            py::arg("entity"),
            py::arg("status_id"),
            py::arg("magnitude"),
            py::arg("duration"),
            py::arg("generation"));
        HEngineModule.def(
            "KillSurvivorAgent",
            &KillSurvivorAgent,
            py::arg("entity"));
        HEngineModule.def(
            "UpdateSurvivorAgents",
            &UpdateSurvivorAgents,
            py::arg("player_position"),
            py::arg("delta_time"),
            py::arg("elapsed"),
            py::arg("damage_cooldown"),
            py::arg("health"),
            py::arg("damage_reduction"),
            py::arg("random_state"),
            py::arg("max_animated_monsters"),
            py::arg("animation_lod_distance"),
            py::arg("animation_lod_release_distance"),
            py::arg("despawn_distance_squared"),
            py::arg("animation_controller_path"));
        HEngineModule.def(
            "RaycastFloorHeight",
            [](const glm::vec3& position, float castDistanceAbove, float castDistanceBelow) -> py::object
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                    return py::none();

                PhysicSystem3D* physicsSystem = level->TryGetSystem<PhysicSystem3D>();
                if (!physicsSystem)
                    return py::none();

                float floorHeight = 0.0f;
                if (!physicsSystem->RaycastFloorHeight(
                    position,
                    castDistanceAbove,
                    castDistanceBelow,
                    floorHeight))
                {
                    return py::none();
                }

                return py::float_(floorHeight);
            },
            py::arg("position"),
            py::arg("cast_distance_above") = 8.0f,
            py::arg("cast_distance_below") = 24.0f);

        py::class_<Entity>(HEngineModule, "Entity")
            .def(py::init())
            .def("GetUUID", [](Entity& entity)
                {
                    if (!entity) throw py::value_error("Entity is no longer valid.");
                    return entity.UUID().str();
                })
            .def("GetWorldPosition", [](Entity& entity) -> glm::vec3
                {
                    if (!entity) throw py::value_error("Entity is no longer valid.");
                    return glm::vec3(entity.GetLevel()->GetWorldTransform(entity)[3]);
                })
            .def("GetComponent", [](Entity& entity, const std::string& compName) 
                -> py::object {
                if (compName == "Transform" || compName == "TransformComponent")
                    return LegacyComponentReference<TransformComponent>(entity);
                else if (compName == "PythonScript" || compName == "PythonScriptComponent")
                    return LegacyComponentReference<PythonScriptComponent>(entity);
                else if (compName == "Tag" || compName == "TagComponent")
                    return LegacyComponentReference<TagComponent>(entity);
                else if (compName == "Rigidbody" || compName == "Rigidbody3DComponent")
                    return LegacyComponentReference<Rigidbody3DComponent>(entity);
                else if (compName == "Animator" || compName == "AnimatorComponent")
                    return LegacyComponentReference<AnimatorComponent>(entity);
                else if (compName == "Sequence" || compName == "SequenceComponent")
                    return LegacyComponentReference<SequenceComponent>(entity);
                else if (compName == "BehaviorTree" || compName == "BehaviorTreeComponent")
                    return LegacyComponentReference<BehaviorTreeComponent>(entity);
                else if (compName == "CharacterMovement" || compName == "CharacterMovementComponent")
                    return LegacyComponentReference<CharacterMovementComponent>(entity);
                else if (compName == "Camera" || compName == "CameraComponent")
                    return LegacyComponentReference<CameraComponent>(entity);
                else if (compName == "FollowCamera" || compName == "FollowCameraComponent")
                    return LegacyComponentReference<FollowCameraComponent>(entity);
                else if (compName == "SpringArm" || compName == "SpringArmComponent")
                    return LegacyComponentReference<SpringArmComponent>(entity);
                else if (compName == "PointLight" || compName == "PointLightComponent")
                    return LegacyComponentReference<PointLightComponent>(entity);
                else if (compName == "SpriteRenderer" || compName == "SpriteRendererComponent")
                    return LegacyComponentReference<SpriteRendererComponent>(entity);
                throw py::value_error("Unsupported component name: " + compName);
                })
            .def("HasComponent", [](Entity& entity, const std::string& compName)
                {
                    if (compName == "Transform" || compName == "TransformComponent")
                        return entity.HasComponent<TransformComponent>();
                    if (compName == "PythonScript" || compName == "PythonScriptComponent")
                        return entity.HasComponent<PythonScriptComponent>();
                    if (compName == "CharacterMovement" || compName == "CharacterMovementComponent")
                        return entity.HasComponent<CharacterMovementComponent>();
                    if (compName == "Camera" || compName == "CameraComponent")
                        return entity.HasComponent<CameraComponent>();
                    if (compName == "FollowCamera" || compName == "FollowCameraComponent")
                        return entity.HasComponent<FollowCameraComponent>();
                    if (compName == "SpringArm" || compName == "SpringArmComponent")
                        return entity.HasComponent<SpringArmComponent>();
                    if (compName == "Tag" || compName == "TagComponent")
                        return entity.HasComponent<TagComponent>();
                    if (compName == "Rigidbody" || compName == "Rigidbody3DComponent")
                        return entity.HasComponent<Rigidbody3DComponent>();
                    if (compName == "Animator" || compName == "AnimatorComponent")
                        return entity.HasComponent<AnimatorComponent>();
                    if (compName == "Sequence" || compName == "SequenceComponent")
                        return entity.HasComponent<SequenceComponent>();
                    if (compName == "BehaviorTree" || compName == "BehaviorTreeComponent")
                        return entity.HasComponent<BehaviorTreeComponent>();
                    if (compName == "PointLight" || compName == "PointLightComponent")
                        return entity.HasComponent<PointLightComponent>();
                    if (compName == "SpriteRenderer" || compName == "SpriteRendererComponent")
                        return entity.HasComponent<SpriteRendererComponent>();
                    return false;
                })
            .def("SetMovementInputEnabled", [](Entity& entity, bool enabled)
                {
                    if (!entity.HasComponent<CharacterMovementComponent>())
                        return;

                    auto& movement = entity.GetComponent<CharacterMovementComponent>();
                    movement.bPlayerInputEnabled = enabled;
                    if (!enabled)
                    {
                        movement.RuntimeVelocity = glm::vec3(0.0f);
                        movement.RuntimeDesiredVelocity = glm::vec3(0.0f);
                        movement.RuntimeHasExternalVelocity = false;
                        movement.RuntimeHasLastInputYaw = false;
                    }
                })
            .def("SetVisual", [](Entity& entity,
                const std::string& meshPath,
                const std::string& animationControllerPath,
                float uniformScale)
                {
                    if (entity.HasComponent<MeshComponent>())
                    {
                        auto& mesh = entity.GetComponent<MeshComponent>();
                        if (mesh.Path != meshPath || !mesh.mMesh || mesh.mMesh->mSubMeshes.empty())
                        {
                            mesh.Path = meshPath;
                            mesh.meshResPath.clear();
                            mesh.mMesh = CreateRef<Mesh>(meshPath);
                        }
                    }

                    if (entity.HasComponent<AnimatorComponent>())
                    {
                        auto& animator = entity.GetComponent<AnimatorComponent>();
                        animator.SetControllerPath(animationControllerPath);
                        animator.bPlay = true;
                    }

                    if (entity.HasComponent<TransformComponent>())
                        entity.GetComponent<TransformComponent>().Scale = glm::vec3(uniformScale);
                },
                py::arg("mesh_path"),
                py::arg("animation_controller_path"),
                py::arg("uniform_scale"))
            .def("SetVisualSilhouette", [](Entity& entity, bool enabled)
                {
                    if (entity.HasComponent<MeshComponent>())
                        entity.GetComponent<MeshComponent>().bRuntimeBlackSilhouette = enabled;
                },
                py::arg("enabled"))
            .def("Teleport", [](Entity& entity, const glm::vec3& worldPosition)
                {
                    if (!entity.HasComponent<CharacterMovementComponent>())
                        return false;

                    Level* level = entity.GetLevel();
                    if (!level)
                        return false;

                    CharacterMovementSystem* movementSystem = level->TryGetSystem<CharacterMovementSystem>();
                    return movementSystem && movementSystem->Teleport(entity, worldPosition);
                })
            .def("Move", [](Entity& entity, const glm::vec3& velocity)
                {
                    if (entity.HasComponent<CharacterMovementComponent>())
                    {
                        auto& movement = entity.GetComponent<CharacterMovementComponent>();
                        movement.RuntimeDesiredVelocity = velocity;
                        movement.RuntimeHasExternalVelocity = true;
                        return;
                    }

                    if (entity.HasComponent<Rigidbody3DComponent>())
                    {
                        auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();
                        if (rb3d.RuntimeBody)
                        {
                            if (Level* level = entity.GetLevel())
                            {
                                if (auto* physics = level->TryGetSystem<PhysicSystem3D>())
                                    physics->SetBodyLinearVelocity(rb3d.RuntimeBody, velocity);
                            }
                        }
                    }
                })
            .def("Move", [](Entity& entity, float velocityX, float velocityY, float velocityZ)
                {
                    const glm::vec3 velocity(velocityX, velocityY, velocityZ);

                    if (entity.HasComponent<CharacterMovementComponent>())
                    {
                        auto& movement = entity.GetComponent<CharacterMovementComponent>();
                        movement.RuntimeDesiredVelocity = velocity;
                        movement.RuntimeHasExternalVelocity = true;
                        return;
                    }

                    if (entity.HasComponent<Rigidbody3DComponent>())
                    {
                        auto& rb3d = entity.GetComponent<Rigidbody3DComponent>();
                        if (rb3d.RuntimeBody)
                        {
                            if (Level* level = entity.GetLevel())
                            {
                                if (auto* physics = level->TryGetSystem<PhysicSystem3D>())
                                    physics->SetBodyLinearVelocity(rb3d.RuntimeBody, velocity);
                            }
                        }
                    }
                });

        HEngineModule.def("Instantiate", [](const std::string& prefabPath) -> Entity
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                {
                    HE_CORE_ERROR("[HEngine.Instantiate] No active level! Can only be called at runtime.");
                    return {};
                }

                return level->InstantiatePrefab(prefabPath);
            }, py::arg("prefabPath"));

        HEngineModule.def("Instantiate", [](const std::string& prefabPath, const glm::vec3& position) -> Entity
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                {
                    HE_CORE_ERROR("[HEngine.Instantiate] No active level!");
                    return {};
                }

                return level->InstantiatePrefab(prefabPath, position);
            }, py::arg("prefabPath"), py::arg("position"));

        HEngineModule.def("Instantiate", [](const std::string& prefabPath, const glm::vec3& position, const glm::vec3& rotation) -> Entity
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                {
                    HE_CORE_ERROR("[HEngine.Instantiate] No active level!");
                    return {};
                }

                return level->InstantiatePrefab(prefabPath, position, rotation);
            }, py::arg("prefabPath"), py::arg("position"), py::arg("rotation"));

        HEngineModule.def("Destroy", [](Entity entity)
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                {
                    HE_CORE_ERROR("[HEngine.Destroy] No active level!");
                    return;
                }

                if (!entity.IsValid())
                {
                    HE_CORE_WARN("[HEngine.Destroy] Entity is not valid.");
                    return;
                }
                if (entity.GetLevel() != level)
                {
                    HE_CORE_WARN("[HEngine.Destroy] Entity does not belong to the active level.");
                    return;
                }

                level->DestroyEntity(entity);
            }, py::arg("entity"));

        HEngineModule.def("FindInstanceEntity", [](Entity instanceMember, const std::string& localId) -> Entity
            {
                if (!instanceMember) throw py::value_error("Prefab instance member is no longer valid.");
                return PrefabSerializer::FindInstanceEntity(instanceMember, localId);
            }, py::arg("instanceMember"), py::arg("localId"));

        HEngineModule.def("FindEntityByTag", [](const std::string& tag) -> Entity
            {
                Level* level = PythonScriptSystem::GetActiveLevel();
                if (!level)
                {
                    HE_CORE_ERROR("[HEngine.FindEntityByTag] No active level!");
                    return {};
                }

                auto view = level->mRegistry.view<TagComponent>();
                for (auto e : view)
                {
                    auto& tagComp = view.get<TagComponent>(e);
                    if (tagComp.Tag == tag)
                        return Entity{ e, level };
                }

                return {};
            }, py::arg("tag"));
    }
}
