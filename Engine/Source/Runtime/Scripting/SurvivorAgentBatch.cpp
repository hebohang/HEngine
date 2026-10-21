#include "Hepch.h"

#include "Runtime/Scripting/SurvivorAgentBatch.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace HEngine
{
    namespace
    {
        constexpr double kMinimumTimer = 0.001;
        constexpr double kBossEnrageSeconds = 120.0;
        constexpr uint32_t kStarDragonRingProjectileCount = 8;
        constexpr uint32_t kStarDragonBreathProjectileCount = 5;
        constexpr double kTau = 6.28318530717958647692;

        double ClampResistance(double value)
        {
            return std::clamp(value, 0.0, 1.0);
        }

        double HorizontalDistanceSquared(
            double leftX,
            double leftZ,
            double rightX,
            double rightZ)
        {
            const double deltaX = rightX - leftX;
            const double deltaZ = rightZ - leftZ;
            return deltaX * deltaX + deltaZ * deltaZ;
        }

        void NormalizeDirection(double& directionX, double& directionZ)
        {
            const double lengthSquared =
                directionX * directionX + directionZ * directionZ;
            if (lengthSquared <= 1.0e-12)
            {
                directionX = 0.0;
                directionZ = -1.0;
                return;
            }

            const double inverseLength = 1.0 / std::sqrt(lengthSquared);
            directionX *= inverseLength;
            directionZ *= inverseLength;
        }

        bool UsesPreferredAttackRange(SurvivorEnemyBehavior behavior)
        {
            switch (behavior)
            {
            case SurvivorEnemyBehavior::Ranged:
            case SurvivorEnemyBehavior::Lobber:
            case SurvivorEnemyBehavior::Zone:
            case SurvivorEnemyBehavior::Summoner:
                return true;
            default:
                return false;
            }
        }
    }

    void SurvivorAgentBatchOutput::ClearFrame(double hurtIFrameRemaining)
    {
        PlayerHurtIFrameRemaining = std::max(0.0, hurtIFrameRemaining);
        DamageEvents.clear();
        ProjectileEvents.clear();
        ZoneEvents.clear();
        SummonEvents.clear();
        RetiredAgents.clear();
        Telemetry = {};
    }

    void SurvivorAgentBatch::Clear()
    {
        mAgents.clear();
        mAgentIndices.clear();
        mLatestActivationIds.clear();
        mProjectiles.clear();
        mZones.clear();
        mProjectileGenerationSeed = NextGeneration(mProjectileGenerationSeed);
        mZoneGenerationSeed = NextGeneration(mZoneGenerationSeed);
        mPendingTelemetry = {};
    }

    bool SurvivorAgentBatch::Activate(SurvivorAgentBatchState state)
    {
        const auto latestIterator = mLatestActivationIds.find(state.Key.EntityId);
        if (latestIterator != mLatestActivationIds.end() &&
            state.Key.ActivationId <= latestIterator->second)
        {
            return false;
        }

        const auto currentIterator = mAgentIndices.find(state.Key.EntityId);
        if (currentIterator != mAgentIndices.end())
        {
            mAgents.erase(mAgents.begin() +
                static_cast<std::ptrdiff_t>(currentIterator->second));
            RebuildAgentIndices();
        }

        state.MaxHealth = std::max(0.001, state.MaxHealth);
        state.Health = std::clamp(state.Health, 0.0, state.MaxHealth);
        state.Speed = std::max(0.0, state.Speed);
        state.Damage = std::max(0.0, state.Damage);
        state.Radius = std::max(0.0, state.Radius);
        state.Attack.Range = std::max(0.0, state.Attack.Range);
        state.Attack.PreferredRange = std::max(
            state.Attack.Range,
            state.Attack.PreferredRange);
        state.Attack.Cooldown = std::max(0.0, state.Attack.Cooldown);
        state.Attack.SourceHitCooldown = std::max(
            0.0,
            state.Attack.SourceHitCooldown);
        state.Attack.TelegraphDuration = std::max(
            kMinimumTimer,
            state.Attack.TelegraphDuration);
        state.Attack.ActiveDuration = std::max(
            kMinimumTimer,
            state.Attack.ActiveDuration);
        state.Attack.RecoveryDuration = std::max(
            kMinimumTimer,
            state.Attack.RecoveryDuration);
        state.Attack.ZoneTickInterval = std::max(
            kMinimumTimer,
            state.Attack.ZoneTickInterval);
        state.Attack.ProjectileLifetime = std::max(
            kMinimumTimer,
            state.Attack.ProjectileLifetime);
        state.AttackCooldownRemaining = std::max(
            0.0,
            state.AttackCooldownRemaining);
        state.SourceHitCooldownRemaining = std::max(
            0.0,
            state.SourceHitCooldownRemaining);
        state.BossActiveElapsed = std::max(0.0, state.BossActiveElapsed);
        state.Status.ActionDelay = std::max(0.0, state.Status.ActionDelay);
        state.Resistances.Damage = ClampResistance(state.Resistances.Damage);
        state.Resistances.Slow = ClampResistance(state.Resistances.Slow);
        state.Resistances.Freeze = ClampResistance(state.Resistances.Freeze);
        state.Resistances.Poison = ClampResistance(state.Resistances.Poison);
        state.Resistances.Knockback = ClampResistance(state.Resistances.Knockback);

        if (state.Behavior == SurvivorEnemyBehavior::Armored)
            state.Resistances.Damage = std::max(0.35, state.Resistances.Damage);
        if (state.Behavior == SurvivorEnemyBehavior::Shield)
        {
            const double configuredShield = state.Attack.ShieldStrength > 0.0
                ? state.Attack.ShieldStrength
                : state.MaxHealth * 0.30;
            state.Shield = std::max(state.Shield, configuredShield);
            state.ShieldTimeRemaining = state.Attack.ShieldDuration;
        }
        if (state.Behavior == SurvivorEnemyBehavior::Boss &&
            state.BossPhase == SurvivorBossPhase::None)
        {
            state.BossPhase = SurvivorBossPhase::PhaseOne;
        }
        if (state.Behavior == SurvivorEnemyBehavior::Boss &&
            state.bObjectiveBoss)
        {
            state.bNoDistanceRetire = true;
        }
        if (state.Behavior != SurvivorEnemyBehavior::Boss)
        {
            state.BossBehavior = SurvivorBossBehavior::Generic;
            state.BossPhase = SurvivorBossPhase::None;
            state.BossSkill = SurvivorBossSkill::None;
            state.BossActiveElapsed = 0.0;
            state.bBossEnraged = false;
            state.bObjectiveBoss = false;
            state.bFinalBoss = false;
        }

        state.bActive = true;
        state.bDead = state.Health <= 0.0;
        state.AttackPhase = SurvivorAttackPhase::Idle;
        state.AttackPhaseRemaining = 0.0;
        state.bAttackCommitted = false;
        mLatestActivationIds[state.Key.EntityId] = state.Key.ActivationId;
        mAgentIndices[state.Key.EntityId] = mAgents.size();
        mAgents.push_back(std::move(state));
        return true;
    }

    bool SurvivorAgentBatch::Deactivate(SurvivorAgentKey key)
    {
        SurvivorAgentBatchState* state = FindAgentMutable(key);
        if (!state || !state->bActive)
            return false;
        state->bActive = false;
        state->AttackPhase = SurvivorAttackPhase::Idle;
        return true;
    }

    bool SurvivorAgentBatch::Kill(SurvivorAgentKey key)
    {
        SurvivorAgentBatchState* state = FindAgentMutable(key);
        if (!state || !state->bActive)
            return false;
        state->bDead = true;
        state->Health = 0.0;
        state->AttackPhase = SurvivorAttackPhase::Idle;
        state->AttackPhaseRemaining = 0.0;
        return true;
    }

    bool SurvivorAgentBatch::SetPosition(
        SurvivorAgentKey key,
        double x,
        double y,
        double z)
    {
        SurvivorAgentBatchState* state = FindAgentMutable(key);
        if (!state || !state->bActive)
            return false;
        state->X = x;
        state->Y = y;
        state->Z = z;
        return true;
    }

    bool SurvivorAgentBatch::SetHealth(SurvivorAgentKey key, double health)
    {
        SurvivorAgentBatchState* state = FindAgentMutable(key);
        if (!state || !state->bActive)
            return false;
        state->Health = std::clamp(health, 0.0, state->MaxHealth);
        state->bDead = state->Health <= 0.0;
        return true;
    }

    bool SurvivorAgentBatch::SetDistanceRetirement(
        SurvivorAgentKey key,
        bool bEnabled)
    {
        SurvivorAgentBatchState* state = FindAgentMutable(key);
        if (!state || !state->bActive)
            return false;
        if (state->bObjectiveBoss && bEnabled)
            return false;
        state->bNoDistanceRetire = !bEnabled;
        return true;
    }

    SurvivorAgentHitResult SurvivorAgentBatch::ApplyHit(
        SurvivorAgentKey key,
        double damage,
        double knockX,
        double knockZ)
    {
        SurvivorAgentBatchState* state = FindAgentMutable(key);
        if (!state || !state->bActive || state->bDead)
            return {};

        const double resistedDamage = std::max(0.0, damage) *
            (1.0 - state->Resistances.Damage);
        const double shieldAbsorbed = std::min(state->Shield, resistedDamage);
        state->Shield -= shieldAbsorbed;
        const double healthDamage = resistedDamage - shieldAbsorbed;
        state->Health = std::max(0.0, state->Health - healthDamage);
        const double knockScale = 1.0 - state->Resistances.Knockback;
        state->KnockX += knockX * knockScale;
        state->KnockZ += knockZ * knockScale;
        state->bDead = state->Health <= 0.0;
        mPendingTelemetry.ShieldDamagePrevented += shieldAbsorbed;
        return {
            state->Health,
            healthDamage,
            std::max(0.0, damage) - healthDamage,
            state->bDead
        };
    }

    SurvivorStatusApplyResult SurvivorAgentBatch::ApplyStatus(
        SurvivorAgentKey key,
        SurvivorStatusKind status,
        double duration,
        double strength)
    {
        SurvivorAgentBatchState* state = FindAgentMutable(key);
        if (!state || !state->bActive || state->bDead)
            return {};

        double resistance = 0.0;
        switch (status)
        {
        case SurvivorStatusKind::Slow:
            resistance = state->Resistances.Slow;
            break;
        case SurvivorStatusKind::Freeze:
            resistance = state->Resistances.Freeze;
            break;
        case SurvivorStatusKind::Poison:
            resistance = state->Resistances.Poison;
            break;
        case SurvivorStatusKind::Interrupt:
            break;
        }

        const double appliedDuration = std::max(0.0, duration) *
            (1.0 - ClampResistance(resistance));
        if (appliedDuration <= 0.0)
        {
            ++mPendingTelemetry.StatusResists;
            return { 0.0, true, false };
        }

        switch (status)
        {
        case SurvivorStatusKind::Slow:
            state->Status.Slow = std::max(state->Status.Slow, appliedDuration);
            state->Status.SlowStrength = std::max(
                state->Status.SlowStrength,
                std::clamp(strength, 0.0, 0.95));
            break;
        case SurvivorStatusKind::Freeze:
            state->Status.Freeze = std::max(state->Status.Freeze, appliedDuration);
            break;
        case SurvivorStatusKind::Poison:
            state->Status.Poison = std::max(state->Status.Poison, appliedDuration);
            state->Status.PoisonMagnitude = std::max(
                state->Status.PoisonMagnitude,
                std::max(0.0, strength));
            break;
        case SurvivorStatusKind::Interrupt:
            if (state->Behavior == SurvivorEnemyBehavior::Boss)
            {
                // Bosses keep their current telegraph/active action. The delay is
                // consumed only while idle, so it always postpones a future attack.
                state->Status.ActionDelay = std::max(
                    state->Status.ActionDelay,
                    appliedDuration);
            }
            else
            {
                // Ordinary and elite agents enter a short non-moving recovery,
                // canceling any telegraph or active attack deterministically.
                state->AttackPhase = SurvivorAttackPhase::Recovery;
                state->AttackPhaseRemaining = appliedDuration;
                state->bAttackCommitted = false;
            }
            break;
        }
        ++mPendingTelemetry.StatusApplications;
        return {
            appliedDuration,
            false,
            status == SurvivorStatusKind::Interrupt &&
                state->Behavior != SurvivorEnemyBehavior::Boss
        };
    }

    void SurvivorAgentBatch::Update(
        const SurvivorAgentBatchInput& input,
        SurvivorAgentBatchOutput& output)
    {
        const double deltaTime = std::max(0.0, input.DeltaTime);
        output.ClearFrame(std::max(
            0.0,
            input.PlayerHurtIFrameRemaining - deltaTime));
        output.Telemetry = mPendingTelemetry;
        mPendingTelemetry = {};

        UpdateProjectileHazards(input, output);
        UpdateZoneHazards(input, output);
        for (SurvivorAgentBatchState& state : mAgents)
        {
            if (!state.bActive || state.bDead)
                continue;
            ++output.Telemetry.ActiveAgents;
            UpdateAgent(state, input, output);
            if (!state.bActive)
                output.RetiredAgents.push_back(state.Key);
        }
    }

    const SurvivorAgentBatchState* SurvivorAgentBatch::FindAgent(
        SurvivorAgentKey key) const
    {
        const auto iterator = mAgentIndices.find(key.EntityId);
        if (iterator == mAgentIndices.end())
            return nullptr;
        const SurvivorAgentBatchState& state = mAgents[iterator->second];
        return state.Key.ActivationId == key.ActivationId ? &state : nullptr;
    }

    std::span<const SurvivorAgentBatchState> SurvivorAgentBatch::GetAgents() const
    {
        return { mAgents.data(), mAgents.size() };
    }

    std::span<const SurvivorEnemyProjectileState> SurvivorAgentBatch::GetProjectiles() const
    {
        return { mProjectiles.data(), mProjectiles.size() };
    }

    std::span<const SurvivorEnemyZoneState> SurvivorAgentBatch::GetZones() const
    {
        return { mZones.data(), mZones.size() };
    }

    bool SurvivorAgentBatch::IsProjectileHandleCurrent(
        SurvivorHazardHandle handle) const
    {
        return handle.IsValid() &&
            handle.Slot < mProjectiles.size() &&
            mProjectiles[handle.Slot].bActive &&
            mProjectiles[handle.Slot].Handle == handle;
    }

    bool SurvivorAgentBatch::IsZoneHandleCurrent(SurvivorHazardHandle handle) const
    {
        return handle.IsValid() &&
            handle.Slot < mZones.size() &&
            mZones[handle.Slot].bActive &&
            mZones[handle.Slot].Handle == handle;
    }

    bool SurvivorAgentBatch::DeactivateProjectile(SurvivorHazardHandle handle)
    {
        if (!IsProjectileHandleCurrent(handle))
            return false;
        mProjectiles[handle.Slot].bActive = false;
        return true;
    }

    bool SurvivorAgentBatch::DeactivateZone(SurvivorHazardHandle handle)
    {
        if (!IsZoneHandleCurrent(handle))
            return false;
        mZones[handle.Slot].bActive = false;
        return true;
    }

    SurvivorAgentBatchState* SurvivorAgentBatch::FindAgentMutable(
        SurvivorAgentKey key)
    {
        const auto iterator = mAgentIndices.find(key.EntityId);
        if (iterator == mAgentIndices.end())
            return nullptr;
        SurvivorAgentBatchState& state = mAgents[iterator->second];
        return state.Key.ActivationId == key.ActivationId ? &state : nullptr;
    }

    void SurvivorAgentBatch::RebuildAgentIndices()
    {
        mAgentIndices.clear();
        mAgentIndices.reserve(mAgents.size());
        for (size_t index = 0; index < mAgents.size(); ++index)
            mAgentIndices[mAgents[index].Key.EntityId] = index;
    }

    void SurvivorAgentBatch::UpdateProjectileHazards(
        const SurvivorAgentBatchInput& input,
        SurvivorAgentBatchOutput& output)
    {
        const double deltaTime = std::max(0.0, input.DeltaTime);
        for (SurvivorEnemyProjectileState& projectile : mProjectiles)
        {
            if (!projectile.bActive)
                continue;

            projectile.X += projectile.VelocityX * deltaTime;
            projectile.Z += projectile.VelocityZ * deltaTime;
            projectile.RemainingLifetime -= deltaTime;

            if (!projectile.bCreatesZone)
            {
                const double hitRadius = projectile.Radius + input.PlayerRadius;
                if (HorizontalDistanceSquared(
                    projectile.X,
                    projectile.Z,
                    input.PlayerX,
                    input.PlayerZ) <= hitRadius * hitRadius)
                {
                    TryDamagePlayer(
                        projectile.Owner,
                        projectile.Handle,
                        SurvivorDamageKind::Projectile,
                        projectile.Damage,
                        input,
                        output);
                    projectile.bActive = false;
                    output.ProjectileEvents.push_back({
                        projectile.Handle,
                        projectile.Owner,
                        SurvivorHazardEventKind::Hit,
                        projectile.X,
                        projectile.Y,
                        projectile.Z
                    });
                    ++output.Telemetry.ProjectileHits;
                    continue;
                }
            }

            if (projectile.RemainingLifetime > 0.0)
                continue;

            if (projectile.bCreatesZone)
            {
                SpawnZone(
                    projectile.Owner,
                    projectile.TargetX,
                    projectile.Y,
                    projectile.TargetZ,
                    projectile.Damage,
                    projectile.ZoneRadius,
                    projectile.ZoneDuration,
                    projectile.ZoneTickInterval,
                    output);
            }
            projectile.bActive = false;
            output.ProjectileEvents.push_back({
                projectile.Handle,
                projectile.Owner,
                SurvivorHazardEventKind::Expired,
                projectile.X,
                projectile.Y,
                projectile.Z
            });
        }
    }

    void SurvivorAgentBatch::UpdateZoneHazards(
        const SurvivorAgentBatchInput& input,
        SurvivorAgentBatchOutput& output)
    {
        const double deltaTime = std::max(0.0, input.DeltaTime);
        for (SurvivorEnemyZoneState& zone : mZones)
        {
            if (!zone.bActive)
                continue;

            zone.RemainingLifetime -= deltaTime;
            zone.TickRemaining -= deltaTime;
            if (zone.TickRemaining <= 0.0 && zone.RemainingLifetime > 0.0)
            {
                const double hitRadius = zone.Radius + input.PlayerRadius;
                if (HorizontalDistanceSquared(
                    zone.X,
                    zone.Z,
                    input.PlayerX,
                    input.PlayerZ) <= hitRadius * hitRadius)
                {
                    TryDamagePlayer(
                        zone.Owner,
                        zone.Handle,
                        SurvivorDamageKind::Zone,
                        zone.Damage,
                        input,
                        output);
                }
                zone.TickRemaining += std::max(kMinimumTimer, zone.TickInterval);
                output.ZoneEvents.push_back({
                    zone.Handle,
                    zone.Owner,
                    SurvivorHazardEventKind::Tick,
                    zone.X,
                    zone.Y,
                    zone.Z,
                    zone.Radius
                });
                ++output.Telemetry.ZoneTicks;
            }

            if (zone.RemainingLifetime > 0.0)
                continue;
            zone.bActive = false;
            output.ZoneEvents.push_back({
                zone.Handle,
                zone.Owner,
                SurvivorHazardEventKind::Expired,
                zone.X,
                zone.Y,
                zone.Z,
                zone.Radius
            });
        }
    }

    void SurvivorAgentBatch::UpdateAgent(
        SurvivorAgentBatchState& state,
        const SurvivorAgentBatchInput& input,
        SurvivorAgentBatchOutput& output)
    {
        const double deltaTime = std::max(0.0, input.DeltaTime);
        state.AttackCooldownRemaining = std::max(
            0.0,
            state.AttackCooldownRemaining - deltaTime);
        state.SourceHitCooldownRemaining = std::max(
            0.0,
            state.SourceHitCooldownRemaining - deltaTime);
        state.Status.Slow = std::max(0.0, state.Status.Slow - deltaTime);
        if (state.Status.Slow <= 0.0)
            state.Status.SlowStrength = 0.0;
        state.Status.Freeze = std::max(0.0, state.Status.Freeze - deltaTime);
        state.Status.Poison = std::max(0.0, state.Status.Poison - deltaTime);
        if (state.Status.Poison <= 0.0)
            state.Status.PoisonMagnitude = 0.0;
        if (state.Attack.ShieldDuration > 0.0 && state.Shield > 0.0)
        {
            state.ShieldTimeRemaining = std::max(
                0.0,
                state.ShieldTimeRemaining - deltaTime);
            if (state.ShieldTimeRemaining <= 0.0)
                state.Shield = 0.0;
        }

        UpdateBossPhase(state, output);
        if (state.Behavior == SurvivorEnemyBehavior::Boss)
        {
            state.BossActiveElapsed += deltaTime;
            if (!state.bBossEnraged &&
                state.BossActiveElapsed >= kBossEnrageSeconds)
            {
                state.bBossEnraged = true;
                state.Damage *= 1.20;
                state.Speed *= 1.10;
                state.Attack.Cooldown *= 0.80;
                state.Attack.ChargeSpeed *= 1.10;
                ++output.Telemetry.BossEnrages;
            }
            if (state.AttackPhase == SurvivorAttackPhase::Idle)
            {
                state.Status.ActionDelay = std::max(
                    0.0,
                    state.Status.ActionDelay - deltaTime);
            }
        }

        if (state.KnockX != 0.0 || state.KnockZ != 0.0)
        {
            state.X += state.KnockX * deltaTime;
            state.Z += state.KnockZ * deltaTime;
            const double damping = std::max(0.0, 1.0 - 9.0 * deltaTime);
            state.KnockX *= damping;
            state.KnockZ *= damping;
        }

        double directionX = input.PlayerX - state.X;
        double directionZ = input.PlayerZ - state.Z;
        const double distance = std::max(
            0.001,
            std::sqrt(directionX * directionX + directionZ * directionZ));
        directionX /= distance;
        directionZ /= distance;

        const bool bChargeActive =
            state.AttackPhase == SurvivorAttackPhase::Active &&
            (state.Behavior == SurvivorEnemyBehavior::Charger ||
                (state.Behavior == SurvivorEnemyBehavior::Boss &&
                    ((state.BossBehavior == SurvivorBossBehavior::Generic &&
                        state.BossPhase == SurvivorBossPhase::PhaseOne) ||
                        (state.BossBehavior == SurvivorBossBehavior::StarDragon &&
                            state.BossSkill == SurvivorBossSkill::FinalCharge))));
        if (bChargeActive && state.Status.Freeze <= 0.0)
        {
            const double speedScale = state.Status.Slow > 0.0
                ? 1.0 - state.Status.SlowStrength
                : 1.0;
            state.X += state.LockedDirectionX * state.Attack.ChargeSpeed *
                speedScale * deltaTime;
            state.Z += state.LockedDirectionZ * state.Attack.ChargeSpeed *
                speedScale * deltaTime;
            const double chargeHitRadius = state.Radius + input.PlayerRadius;
            if (!state.bAttackCommitted &&
                HorizontalDistanceSquared(
                    state.X,
                    state.Z,
                    input.PlayerX,
                    input.PlayerZ) <= chargeHitRadius * chargeHitRadius)
            {
                state.bAttackCommitted = true;
                state.SourceHitCooldownRemaining = state.Attack.SourceHitCooldown;
                TryDamagePlayer(
                    state.Key,
                    {},
                    SurvivorDamageKind::Charge,
                    state.Damage,
                    input,
                    output);
            }
        }
        else if (state.AttackPhase == SurvivorAttackPhase::Idle &&
            state.Status.Freeze <= 0.0)
        {
            MoveAgent(state, input, distance, directionX, directionZ);
        }

        if (state.Status.Freeze > 0.0)
            return;

        switch (state.AttackPhase)
        {
        case SurvivorAttackPhase::Idle:
        {
            const bool bVerticalMatch = std::abs(input.PlayerY - state.Y) <= 2.25;
            const double activationRange =
                UsesPreferredAttackRange(state.Behavior) ||
                (state.Behavior == SurvivorEnemyBehavior::Boss &&
                    state.BossBehavior == SurvivorBossBehavior::StarDragon &&
                    state.BossPhase != SurvivorBossPhase::PhaseThree)
                ? std::max(state.Attack.Range, state.Attack.PreferredRange)
                : state.Attack.Range;
            if (state.AttackCooldownRemaining <= 0.0 &&
                state.Status.ActionDelay <= 0.0 &&
                distance <= activationRange &&
                bVerticalMatch)
            {
                BeginTelegraph(state, directionX, directionZ, output);
            }
            break;
        }
        case SurvivorAttackPhase::Telegraph:
            state.AttackPhaseRemaining -= deltaTime;
            if (state.AttackPhaseRemaining <= 0.0)
            {
                state.AttackPhase = SurvivorAttackPhase::Active;
                state.AttackPhaseRemaining = state.Attack.ActiveDuration;
                ++output.Telemetry.AttacksActivated;
                ActivateAttack(state, input, output);
            }
            break;
        case SurvivorAttackPhase::Active:
            state.AttackPhaseRemaining -= deltaTime;
            if (state.AttackPhaseRemaining <= 0.0)
            {
                state.AttackPhase = SurvivorAttackPhase::Recovery;
                state.AttackPhaseRemaining = state.Attack.RecoveryDuration;
            }
            break;
        case SurvivorAttackPhase::Recovery:
            state.AttackPhaseRemaining -= deltaTime;
            if (state.AttackPhaseRemaining <= 0.0)
            {
                state.AttackPhase = SurvivorAttackPhase::Idle;
                state.AttackPhaseRemaining = 0.0;
                state.AttackCooldownRemaining = state.Attack.Cooldown;
                state.bAttackCommitted = false;
            }
            break;
        }
    }

    void SurvivorAgentBatch::BeginTelegraph(
        SurvivorAgentBatchState& state,
        double directionX,
        double directionZ,
        SurvivorAgentBatchOutput& output)
    {
        NormalizeDirection(directionX, directionZ);
        state.LockedDirectionX = directionX;
        state.LockedDirectionZ = directionZ;
        state.AttackPhase = SurvivorAttackPhase::Telegraph;
        state.AttackPhaseRemaining = state.Attack.TelegraphDuration;
        state.bAttackCommitted = false;
        if (state.Behavior == SurvivorEnemyBehavior::Boss &&
            state.BossBehavior == SurvivorBossBehavior::StarDragon)
        {
            switch (state.BossPhase)
            {
            case SurvivorBossPhase::PhaseOne:
                state.BossSkill = SurvivorBossSkill::RingBarrage;
                break;
            case SurvivorBossPhase::PhaseTwo:
                state.BossSkill = SurvivorBossSkill::SweepBreath;
                break;
            case SurvivorBossPhase::PhaseThree:
                state.BossSkill = SurvivorBossSkill::FinalCharge;
                break;
            case SurvivorBossPhase::None:
                state.BossSkill = SurvivorBossSkill::None;
                break;
            }
        }
        if (state.Behavior == SurvivorEnemyBehavior::Shield)
        {
            const double configuredShield = state.Attack.ShieldStrength > 0.0
                ? state.Attack.ShieldStrength
                : state.MaxHealth * 0.30;
            state.Shield = std::max(state.Shield, configuredShield);
            state.ShieldTimeRemaining = state.Attack.ShieldDuration;
        }
        ++output.Telemetry.TelegraphsStarted;
    }

    void SurvivorAgentBatch::ActivateAttack(
        SurvivorAgentBatchState& state,
        const SurvivorAgentBatchInput& input,
        SurvivorAgentBatchOutput& output)
    {
        const double distanceSquared = HorizontalDistanceSquared(
            state.X,
            state.Z,
            input.PlayerX,
            input.PlayerZ);
        switch (state.Behavior)
        {
        case SurvivorEnemyBehavior::Chaser:
        case SurvivorEnemyBehavior::Flanker:
        case SurvivorEnemyBehavior::Armored:
        case SurvivorEnemyBehavior::Bruiser:
        case SurvivorEnemyBehavior::Ghost:
        case SurvivorEnemyBehavior::Shield:
            if (distanceSquared <= state.Attack.Range * state.Attack.Range)
            {
                state.SourceHitCooldownRemaining = state.Attack.SourceHitCooldown;
                state.bAttackCommitted = true;
                TryDamagePlayer(
                    state.Key,
                    {},
                    SurvivorDamageKind::Melee,
                    state.Damage,
                    input,
                    output);
            }
            break;
        case SurvivorEnemyBehavior::Charger:
            if (distanceSquared <=
                (state.Radius + input.PlayerRadius) *
                (state.Radius + input.PlayerRadius))
            {
                state.SourceHitCooldownRemaining = state.Attack.SourceHitCooldown;
                state.bAttackCommitted = true;
                TryDamagePlayer(
                    state.Key,
                    {},
                    SurvivorDamageKind::Charge,
                    state.Damage,
                    input,
                    output);
            }
            break;
        case SurvivorEnemyBehavior::Ranged:
            SpawnProjectile(state, input, false, output);
            state.bAttackCommitted = true;
            break;
        case SurvivorEnemyBehavior::Lobber:
            SpawnProjectile(state, input, true, output);
            state.bAttackCommitted = true;
            break;
        case SurvivorEnemyBehavior::Zone:
            SpawnZone(
                state.Key,
                input.PlayerX,
                input.PlayerY,
                input.PlayerZ,
                state.Damage * state.Attack.ZoneDamageScale,
                state.Attack.ZoneRadius,
                state.Attack.ZoneDuration,
                state.Attack.ZoneTickInterval,
                output);
            state.bAttackCommitted = true;
            break;
        case SurvivorEnemyBehavior::Suicide:
        {
            SpawnZone(
                state.Key,
                state.X,
                state.Y,
                state.Z,
                state.Damage * state.Attack.SuicideDamageScale,
                state.Attack.ZoneRadius,
                std::max(0.1, state.Attack.ActiveDuration),
                std::max(0.05, state.Attack.ActiveDuration),
                output);
            const double hitRadius = state.Attack.ZoneRadius + input.PlayerRadius;
            if (distanceSquared <= hitRadius * hitRadius)
            {
                TryDamagePlayer(
                    state.Key,
                    {},
                    SurvivorDamageKind::Suicide,
                    state.Damage * state.Attack.SuicideDamageScale,
                    input,
                    output);
            }
            state.bAttackCommitted = true;
            state.bActive = false;
            break;
        }
        case SurvivorEnemyBehavior::Summoner:
            output.SummonEvents.push_back({
                state.Key,
                SurvivorEnemyBehavior::Chaser,
                state.X,
                state.Y,
                state.Z,
                state.Attack.SummonCount
            });
            output.Telemetry.SummonRequests += state.Attack.SummonCount;
            state.bAttackCommitted = true;
            break;
        case SurvivorEnemyBehavior::Boss:
            if (state.BossBehavior == SurvivorBossBehavior::StarDragon)
            {
                if (state.BossSkill == SurvivorBossSkill::RingBarrage)
                {
                    const double phaseOffset = std::fmod(
                        std::abs(state.MovementPhase),
                        kTau);
                    for (uint32_t index = 0;
                        index < kStarDragonRingProjectileCount;
                        ++index)
                    {
                        const double angle = phaseOffset +
                            kTau * static_cast<double>(index) /
                            static_cast<double>(kStarDragonRingProjectileCount);
                        const double directionX = std::cos(angle);
                        const double directionZ = std::sin(angle);
                        SpawnDirectedProjectile(
                            state,
                            directionX,
                            directionZ,
                            state.X + directionX * state.Attack.ProjectileSpeed *
                                state.Attack.ProjectileLifetime,
                            state.Z + directionZ * state.Attack.ProjectileSpeed *
                                state.Attack.ProjectileLifetime,
                            state.Attack.ProjectileLifetime,
                            false,
                            output);
                    }
                    state.bAttackCommitted = true;
                    ++output.Telemetry.BossRingBarrages;
                }
                else if (state.BossSkill == SurvivorBossSkill::SweepBreath)
                {
                    constexpr double kHalfSweepRadians = 0.62;
                    for (uint32_t index = 0;
                        index < kStarDragonBreathProjectileCount;
                        ++index)
                    {
                        const double fraction = static_cast<double>(index) /
                            static_cast<double>(kStarDragonBreathProjectileCount - 1);
                        const double angle = -kHalfSweepRadians +
                            fraction * kHalfSweepRadians * 2.0;
                        const double cosine = std::cos(angle);
                        const double sine = std::sin(angle);
                        const double directionX =
                            state.LockedDirectionX * cosine -
                            state.LockedDirectionZ * sine;
                        const double directionZ =
                            state.LockedDirectionX * sine +
                            state.LockedDirectionZ * cosine;
                        SpawnDirectedProjectile(
                            state,
                            directionX,
                            directionZ,
                            input.PlayerX,
                            input.PlayerZ,
                            state.Attack.ProjectileLifetime,
                            false,
                            output);
                    }
                    state.bAttackCommitted = true;
                    ++output.Telemetry.BossSweepBreaths;
                }
                else
                {
                    const double hitRadius = state.Radius + input.PlayerRadius;
                    if (distanceSquared <= hitRadius * hitRadius)
                    {
                        state.SourceHitCooldownRemaining = state.Attack.SourceHitCooldown;
                        state.bAttackCommitted = true;
                        TryDamagePlayer(
                            state.Key,
                            {},
                            SurvivorDamageKind::Charge,
                            state.Damage,
                            input,
                            output);
                    }
                    SpawnZone(
                        state.Key,
                        input.PlayerX,
                        input.PlayerY,
                        input.PlayerZ,
                        state.Damage * state.Attack.ZoneDamageScale,
                        state.Attack.ZoneRadius,
                        state.Attack.ZoneDuration,
                        state.Attack.ZoneTickInterval,
                        output);
                    const uint32_t summonCount = std::max(
                        uint32_t{ 1 },
                        state.Attack.SummonCount);
                    output.SummonEvents.push_back({
                        state.Key,
                        SurvivorEnemyBehavior::Chaser,
                        state.X,
                        state.Y,
                        state.Z,
                        summonCount
                    });
                    output.Telemetry.SummonRequests += summonCount;
                    ++output.Telemetry.BossFinalCharges;
                }
                break;
            }
            if (state.BossPhase == SurvivorBossPhase::PhaseOne)
            {
                if (distanceSquared <=
                    (state.Radius + input.PlayerRadius) *
                    (state.Radius + input.PlayerRadius))
                {
                    state.SourceHitCooldownRemaining = state.Attack.SourceHitCooldown;
                    state.bAttackCommitted = true;
                    TryDamagePlayer(
                        state.Key,
                        {},
                        SurvivorDamageKind::Charge,
                        state.Damage,
                        input,
                        output);
                }
            }
            else if (state.BossPhase == SurvivorBossPhase::PhaseTwo)
            {
                SpawnProjectile(state, input, false, output);
                state.bAttackCommitted = true;
            }
            else
            {
                SpawnZone(
                    state.Key,
                    input.PlayerX,
                    input.PlayerY,
                    input.PlayerZ,
                    state.Damage * state.Attack.ZoneDamageScale,
                    state.Attack.ZoneRadius,
                    state.Attack.ZoneDuration,
                    state.Attack.ZoneTickInterval,
                    output);
                output.SummonEvents.push_back({
                    state.Key,
                    SurvivorEnemyBehavior::Chaser,
                    state.X,
                    state.Y,
                    state.Z,
                    state.Attack.SummonCount
                });
                output.Telemetry.SummonRequests += state.Attack.SummonCount;
                state.bAttackCommitted = true;
            }
            break;
        }
    }

    void SurvivorAgentBatch::MoveAgent(
        SurvivorAgentBatchState& state,
        const SurvivorAgentBatchInput& input,
        double distance,
        double directionX,
        double directionZ)
    {
        double moveX = directionX;
        double moveZ = directionZ;
        double speedScale = state.Status.Slow > 0.0
            ? 1.0 - state.Status.SlowStrength
            : 1.0;
        double desiredDistance = state.Attack.Range;

        switch (state.Behavior)
        {
        case SurvivorEnemyBehavior::Flanker:
        {
            const double flankSign = std::sin(state.MovementPhase) >= 0.0 ? 1.0 : -1.0;
            moveX = directionX * 0.92 - directionZ * 0.25 * flankSign;
            moveZ = directionZ * 0.92 + directionX * 0.25 * flankSign;
            break;
        }
        case SurvivorEnemyBehavior::Armored:
            speedScale *= 0.72;
            break;
        case SurvivorEnemyBehavior::Bruiser:
            speedScale *= 0.78;
            break;
        case SurvivorEnemyBehavior::Ghost:
        {
            const double driftSign = std::sin(state.MovementPhase) >= 0.0 ? 1.0 : -1.0;
            speedScale *= 1.08;
            moveX += -directionZ * 0.08 * driftSign;
            moveZ += directionX * 0.08 * driftSign;
            break;
        }
        case SurvivorEnemyBehavior::Ranged:
        case SurvivorEnemyBehavior::Lobber:
        case SurvivorEnemyBehavior::Zone:
        case SurvivorEnemyBehavior::Summoner:
            desiredDistance = state.Attack.PreferredRange;
            if (distance < desiredDistance * 0.75)
            {
                moveX = -directionX;
                moveZ = -directionZ;
            }
            else if (distance <= desiredDistance)
            {
                return;
            }
            break;
        case SurvivorEnemyBehavior::Boss:
            if (state.BossBehavior == SurvivorBossBehavior::StarDragon &&
                state.BossPhase != SurvivorBossPhase::PhaseThree)
            {
                desiredDistance = state.Attack.PreferredRange;
                if (distance < desiredDistance * 0.75)
                {
                    moveX = -directionX;
                    moveZ = -directionZ;
                }
                else if (distance <= desiredDistance)
                {
                    moveX = -directionZ * 0.25;
                    moveZ = directionX * 0.25;
                }
            }
            break;
        default:
        {
            const double weave = std::sin(input.Elapsed * 1.7 + state.MovementPhase) * 0.12;
            moveX += -directionZ * weave;
            moveZ += directionX * weave;
            break;
        }
        }

        NormalizeDirection(moveX, moveZ);
        double moveDistance = state.Speed * speedScale *
            std::max(0.0, input.DeltaTime);
        if (moveX * directionX + moveZ * directionZ > 0.0)
            moveDistance = std::min(moveDistance, std::max(0.0, distance - desiredDistance));
        state.X += moveX * moveDistance;
        state.Z += moveZ * moveDistance;
    }

    void SurvivorAgentBatch::TryDamagePlayer(
        SurvivorAgentKey source,
        SurvivorHazardHandle hazard,
        SurvivorDamageKind kind,
        double damage,
        const SurvivorAgentBatchInput& input,
        SurvivorAgentBatchOutput& output)
    {
        if (damage <= 0.0 || output.PlayerHurtIFrameRemaining > 0.0)
            return;
        output.DamageEvents.push_back({ source, hazard, kind, damage });
        output.PlayerHurtIFrameRemaining = std::max(
            0.0,
            input.PlayerHurtIFrameDuration);
        ++output.Telemetry.PlayerDamageEvents;
    }

    SurvivorHazardHandle SurvivorAgentBatch::SpawnProjectile(
        const SurvivorAgentBatchState& state,
        const SurvivorAgentBatchInput& input,
        bool bLobbed,
        SurvivorAgentBatchOutput& output)
    {
        double directionX = input.PlayerX - state.X;
        double directionZ = input.PlayerZ - state.Z;
        const double distance = std::sqrt(
            directionX * directionX + directionZ * directionZ);
        NormalizeDirection(directionX, directionZ);
        const double projectileSpeed = std::max(0.01, state.Attack.ProjectileSpeed);
        const double lifetime = bLobbed
            ? std::max(kMinimumTimer, distance / projectileSpeed)
            : state.Attack.ProjectileLifetime;
        return SpawnDirectedProjectile(
            state,
            directionX,
            directionZ,
            input.PlayerX,
            input.PlayerZ,
            lifetime,
            bLobbed,
            output);
    }

    SurvivorHazardHandle SurvivorAgentBatch::SpawnDirectedProjectile(
        const SurvivorAgentBatchState& state,
        double directionX,
        double directionZ,
        double targetX,
        double targetZ,
        double lifetime,
        bool bCreatesZone,
        SurvivorAgentBatchOutput& output)
    {
        uint32_t slot = SurvivorHazardHandle::InvalidSlot;
        for (uint32_t index = 0; index < mProjectiles.size(); ++index)
        {
            if (!mProjectiles[index].bActive)
            {
                slot = index;
                break;
            }
        }

        SurvivorEnemyProjectileState projectile;
        mProjectileGenerationSeed = NextGeneration(mProjectileGenerationSeed);
        if (slot == SurvivorHazardHandle::InvalidSlot)
        {
            slot = static_cast<uint32_t>(mProjectiles.size());
            projectile.Handle = { slot, mProjectileGenerationSeed };
        }
        else
        {
            projectile.Handle = { slot, mProjectileGenerationSeed };
        }

        NormalizeDirection(directionX, directionZ);
        const double projectileSpeed = std::max(0.01, state.Attack.ProjectileSpeed);
        projectile.Owner = state.Key;
        projectile.X = state.X;
        projectile.Y = state.Y;
        projectile.Z = state.Z;
        projectile.VelocityX = directionX * projectileSpeed;
        projectile.VelocityZ = directionZ * projectileSpeed;
        projectile.TargetX = targetX;
        projectile.TargetZ = targetZ;
        projectile.Damage = state.Damage * state.Attack.ProjectileDamageScale;
        projectile.Radius = std::max(0.0, state.Attack.ProjectileRadius);
        projectile.RemainingLifetime = std::max(kMinimumTimer, lifetime);
        projectile.ZoneRadius = state.Attack.ZoneRadius;
        projectile.ZoneDuration = state.Attack.ZoneDuration;
        projectile.ZoneTickInterval = state.Attack.ZoneTickInterval;
        projectile.bCreatesZone = bCreatesZone;
        projectile.bActive = true;

        if (slot == mProjectiles.size())
            mProjectiles.push_back(projectile);
        else
            mProjectiles[slot] = projectile;

        output.ProjectileEvents.push_back({
            projectile.Handle,
            projectile.Owner,
            SurvivorHazardEventKind::Spawned,
            projectile.X,
            projectile.Y,
            projectile.Z
        });
        ++output.Telemetry.ProjectileSpawns;
        return projectile.Handle;
    }

    SurvivorHazardHandle SurvivorAgentBatch::SpawnZone(
        SurvivorAgentKey owner,
        double x,
        double y,
        double z,
        double damage,
        double radius,
        double duration,
        double tickInterval,
        SurvivorAgentBatchOutput& output)
    {
        uint32_t slot = SurvivorHazardHandle::InvalidSlot;
        for (uint32_t index = 0; index < mZones.size(); ++index)
        {
            if (!mZones[index].bActive)
            {
                slot = index;
                break;
            }
        }

        SurvivorEnemyZoneState zone;
        mZoneGenerationSeed = NextGeneration(mZoneGenerationSeed);
        if (slot == SurvivorHazardHandle::InvalidSlot)
        {
            slot = static_cast<uint32_t>(mZones.size());
            zone.Handle = { slot, mZoneGenerationSeed };
        }
        else
        {
            zone.Handle = { slot, mZoneGenerationSeed };
        }
        zone.Owner = owner;
        zone.X = x;
        zone.Y = y;
        zone.Z = z;
        zone.Damage = std::max(0.0, damage);
        zone.Radius = std::max(0.0, radius);
        zone.RemainingLifetime = std::max(kMinimumTimer, duration);
        zone.TickInterval = std::max(kMinimumTimer, tickInterval);
        zone.TickRemaining = 0.0;
        zone.bActive = true;

        if (slot == mZones.size())
            mZones.push_back(zone);
        else
            mZones[slot] = zone;

        output.ZoneEvents.push_back({
            zone.Handle,
            zone.Owner,
            SurvivorHazardEventKind::Spawned,
            zone.X,
            zone.Y,
            zone.Z,
            zone.Radius
        });
        ++output.Telemetry.ZoneSpawns;
        return zone.Handle;
    }

    void SurvivorAgentBatch::UpdateBossPhase(
        SurvivorAgentBatchState& state,
        SurvivorAgentBatchOutput& output)
    {
        if (state.Behavior != SurvivorEnemyBehavior::Boss)
            return;

        const double healthRatio = state.Health / std::max(0.001, state.MaxHealth);
        SurvivorBossPhase nextPhase = SurvivorBossPhase::PhaseOne;
        if (healthRatio <= 0.33)
            nextPhase = SurvivorBossPhase::PhaseThree;
        else if (healthRatio <= 0.66)
            nextPhase = SurvivorBossPhase::PhaseTwo;
        if (nextPhase == state.BossPhase)
            return;
        state.BossPhase = nextPhase;
        ++output.Telemetry.BossPhaseChanges;
    }

    uint32_t SurvivorAgentBatch::NextGeneration(uint32_t generation)
    {
        ++generation;
        if (generation == 0)
            ++generation;
        return generation;
    }
}
