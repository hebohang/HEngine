#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    enum class SurvivorEnemyBehavior : uint8_t
    {
        Chaser,
        Flanker,
        Armored,
        Bruiser,
        Zone,
        Charger,
        Ranged,
        Lobber,
        Ghost,
        Shield,
        Suicide,
        Summoner,
        Boss
    };

    enum class SurvivorAttackPhase : uint8_t
    {
        Idle,
        Telegraph,
        Active,
        Recovery
    };

    enum class SurvivorBossPhase : uint8_t
    {
        None,
        PhaseOne,
        PhaseTwo,
        PhaseThree
    };

    enum class SurvivorBossBehavior : uint8_t
    {
        Generic,
        StarDragon
    };

    enum class SurvivorBossSkill : uint8_t
    {
        None,
        RingBarrage,
        SweepBreath,
        FinalCharge
    };

    enum class SurvivorDamageKind : uint8_t
    {
        Melee,
        Charge,
        Projectile,
        Zone,
        Suicide
    };

    enum class SurvivorStatusKind : uint8_t
    {
        Slow,
        Freeze,
        Poison,
        Interrupt
    };

    enum class SurvivorHazardEventKind : uint8_t
    {
        Spawned,
        Hit,
        Tick,
        Expired
    };

    struct SurvivorAgentKey
    {
        uint32_t EntityId = 0;
        uint64_t ActivationId = 0;

        bool operator==(const SurvivorAgentKey&) const = default;
    };

    struct SurvivorHazardHandle
    {
        static constexpr uint32_t InvalidSlot = std::numeric_limits<uint32_t>::max();

        uint32_t Slot = InvalidSlot;
        uint32_t Generation = 0;

        [[nodiscard]] bool IsValid() const
        {
            return Slot != InvalidSlot && Generation != 0;
        }

        bool operator==(const SurvivorHazardHandle&) const = default;
    };

    struct SurvivorEnemyAttackConfig
    {
        double Range = 1.0;
        double PreferredRange = 5.0;
        double Cooldown = 1.05;
        double SourceHitCooldown = 0.65;
        double TelegraphDuration = 0.35;
        double ActiveDuration = 0.08;
        double RecoveryDuration = 0.18;
        double ChargeSpeed = 9.0;
        double ProjectileSpeed = 7.5;
        double ProjectileLifetime = 2.4;
        double ProjectileRadius = 0.28;
        double ProjectileDamageScale = 1.0;
        double ZoneRadius = 1.6;
        double ZoneDuration = 2.0;
        double ZoneTickInterval = 0.5;
        double ZoneDamageScale = 0.75;
        double SuicideDamageScale = 1.5;
        double ShieldStrength = 0.0;
        double ShieldDuration = 0.0;
        uint32_t SummonCount = 2;
    };

    struct SurvivorEnemyResistances
    {
        double Damage = 0.0;
        double Slow = 0.0;
        double Freeze = 0.0;
        double Poison = 0.0;
        double Knockback = 0.0;
    };

    struct SurvivorEnemyStatusTimers
    {
        double Slow = 0.0;
        double SlowStrength = 0.0;
        double Freeze = 0.0;
        double Poison = 0.0;
        double PoisonMagnitude = 0.0;
        double ActionDelay = 0.0;
    };

    struct SurvivorAgentBatchState
    {
        SurvivorAgentKey Key;
        SurvivorEnemyBehavior Behavior = SurvivorEnemyBehavior::Chaser;
        SurvivorAttackPhase AttackPhase = SurvivorAttackPhase::Idle;
        SurvivorBossPhase BossPhase = SurvivorBossPhase::None;
        SurvivorBossBehavior BossBehavior = SurvivorBossBehavior::Generic;
        SurvivorBossSkill BossSkill = SurvivorBossSkill::None;
        SurvivorEnemyAttackConfig Attack;
        SurvivorEnemyResistances Resistances;
        SurvivorEnemyStatusTimers Status;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        double MovementPhase = 0.0;
        double Health = 1.0;
        double MaxHealth = 1.0;
        double Speed = 1.0;
        double Damage = 1.0;
        double Radius = 0.5;
        double AttackCooldownRemaining = 0.0;
        double SourceHitCooldownRemaining = 0.0;
        double AttackPhaseRemaining = 0.0;
        double LockedDirectionX = 0.0;
        double LockedDirectionZ = -1.0;
        double KnockX = 0.0;
        double KnockZ = 0.0;
        double Shield = 0.0;
        double ShieldTimeRemaining = 0.0;
        double BossActiveElapsed = 0.0;
        bool bAttackCommitted = false;
        bool bActive = true;
        bool bDead = false;
        bool bBossEnraged = false;
        bool bObjectiveBoss = false;
        bool bFinalBoss = false;
        bool bNoDistanceRetire = false;
    };

    struct SurvivorEnemyProjectileState
    {
        SurvivorHazardHandle Handle;
        SurvivorAgentKey Owner;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        double VelocityX = 0.0;
        double VelocityZ = 0.0;
        double TargetX = 0.0;
        double TargetZ = 0.0;
        double Damage = 0.0;
        double Radius = 0.0;
        double RemainingLifetime = 0.0;
        double ZoneRadius = 0.0;
        double ZoneDuration = 0.0;
        double ZoneTickInterval = 0.0;
        bool bCreatesZone = false;
        bool bActive = false;
    };

    struct SurvivorEnemyZoneState
    {
        SurvivorHazardHandle Handle;
        SurvivorAgentKey Owner;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        double Damage = 0.0;
        double Radius = 0.0;
        double RemainingLifetime = 0.0;
        double TickInterval = 0.0;
        double TickRemaining = 0.0;
        bool bActive = false;
    };

    struct SurvivorAgentBatchInput
    {
        double PlayerX = 0.0;
        double PlayerY = 0.0;
        double PlayerZ = 0.0;
        double PlayerRadius = 0.42;
        double DeltaTime = 0.0;
        double Elapsed = 0.0;
        double PlayerHurtIFrameRemaining = 0.0;
        double PlayerHurtIFrameDuration = 0.45;
    };

    struct SurvivorPlayerDamageEvent
    {
        SurvivorAgentKey Source;
        SurvivorHazardHandle Hazard;
        SurvivorDamageKind Kind = SurvivorDamageKind::Melee;
        double Damage = 0.0;
    };

    struct SurvivorProjectileEvent
    {
        SurvivorHazardHandle Handle;
        SurvivorAgentKey Owner;
        SurvivorHazardEventKind Kind = SurvivorHazardEventKind::Spawned;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
    };

    struct SurvivorZoneEvent
    {
        SurvivorHazardHandle Handle;
        SurvivorAgentKey Owner;
        SurvivorHazardEventKind Kind = SurvivorHazardEventKind::Spawned;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        double Radius = 0.0;
    };

    struct SurvivorSummonEvent
    {
        SurvivorAgentKey Source;
        SurvivorEnemyBehavior Behavior = SurvivorEnemyBehavior::Chaser;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        uint32_t Count = 0;
    };

    struct SurvivorEnemyTelemetry
    {
        uint32_t ActiveAgents = 0;
        uint32_t TelegraphsStarted = 0;
        uint32_t AttacksActivated = 0;
        uint32_t PlayerDamageEvents = 0;
        uint32_t ProjectileSpawns = 0;
        uint32_t ProjectileHits = 0;
        uint32_t ZoneSpawns = 0;
        uint32_t ZoneTicks = 0;
        uint32_t SummonRequests = 0;
        uint32_t BossPhaseChanges = 0;
        uint32_t BossRingBarrages = 0;
        uint32_t BossSweepBreaths = 0;
        uint32_t BossFinalCharges = 0;
        uint32_t BossEnrages = 0;
        uint32_t StatusApplications = 0;
        uint32_t StatusResists = 0;
        double ShieldDamagePrevented = 0.0;
    };

    struct SurvivorAgentBatchOutput
    {
        double PlayerHurtIFrameRemaining = 0.0;
        std::vector<SurvivorPlayerDamageEvent> DamageEvents;
        std::vector<SurvivorProjectileEvent> ProjectileEvents;
        std::vector<SurvivorZoneEvent> ZoneEvents;
        std::vector<SurvivorSummonEvent> SummonEvents;
        std::vector<SurvivorAgentKey> RetiredAgents;
        SurvivorEnemyTelemetry Telemetry;

        void ClearFrame(double hurtIFrameRemaining);
    };

    struct SurvivorAgentHitResult
    {
        double Health = 0.0;
        double DamageApplied = 0.0;
        double DamagePrevented = 0.0;
        bool bKilled = false;
    };

    struct SurvivorStatusApplyResult
    {
        double AppliedDuration = 0.0;
        bool bResisted = false;
        bool bHardInterrupted = false;
    };

    class SurvivorAgentBatch
    {
    public:
        void Clear();

        [[nodiscard]] bool Activate(SurvivorAgentBatchState state);
        [[nodiscard]] bool Deactivate(SurvivorAgentKey key);
        [[nodiscard]] bool Kill(SurvivorAgentKey key);
        [[nodiscard]] bool SetPosition(SurvivorAgentKey key, double x, double y, double z);
        [[nodiscard]] bool SetHealth(SurvivorAgentKey key, double health);
        [[nodiscard]] bool SetDistanceRetirement(
            SurvivorAgentKey key,
            bool bEnabled);

        [[nodiscard]] SurvivorAgentHitResult ApplyHit(
            SurvivorAgentKey key,
            double damage,
            double knockX = 0.0,
            double knockZ = 0.0);
        [[nodiscard]] SurvivorStatusApplyResult ApplyStatus(
            SurvivorAgentKey key,
            SurvivorStatusKind status,
            double duration,
            double strength = 0.0);

        void Update(const SurvivorAgentBatchInput& input, SurvivorAgentBatchOutput& output);

        [[nodiscard]] const SurvivorAgentBatchState* FindAgent(SurvivorAgentKey key) const;
        [[nodiscard]] std::span<const SurvivorAgentBatchState> GetAgents() const;
        [[nodiscard]] std::span<const SurvivorEnemyProjectileState> GetProjectiles() const;
        [[nodiscard]] std::span<const SurvivorEnemyZoneState> GetZones() const;
        [[nodiscard]] bool IsProjectileHandleCurrent(SurvivorHazardHandle handle) const;
        [[nodiscard]] bool IsZoneHandleCurrent(SurvivorHazardHandle handle) const;
        [[nodiscard]] bool DeactivateProjectile(SurvivorHazardHandle handle);
        [[nodiscard]] bool DeactivateZone(SurvivorHazardHandle handle);

    private:
        [[nodiscard]] SurvivorAgentBatchState* FindAgentMutable(SurvivorAgentKey key);
        void RebuildAgentIndices();
        void UpdateProjectileHazards(
            const SurvivorAgentBatchInput& input,
            SurvivorAgentBatchOutput& output);
        void UpdateZoneHazards(
            const SurvivorAgentBatchInput& input,
            SurvivorAgentBatchOutput& output);
        void UpdateAgent(
            SurvivorAgentBatchState& state,
            const SurvivorAgentBatchInput& input,
            SurvivorAgentBatchOutput& output);
        void BeginTelegraph(
            SurvivorAgentBatchState& state,
            double directionX,
            double directionZ,
            SurvivorAgentBatchOutput& output);
        void ActivateAttack(
            SurvivorAgentBatchState& state,
            const SurvivorAgentBatchInput& input,
            SurvivorAgentBatchOutput& output);
        void MoveAgent(
            SurvivorAgentBatchState& state,
            const SurvivorAgentBatchInput& input,
            double distance,
            double directionX,
            double directionZ);
        void TryDamagePlayer(
            SurvivorAgentKey source,
            SurvivorHazardHandle hazard,
            SurvivorDamageKind kind,
            double damage,
            const SurvivorAgentBatchInput& input,
            SurvivorAgentBatchOutput& output);
        SurvivorHazardHandle SpawnProjectile(
            const SurvivorAgentBatchState& state,
            const SurvivorAgentBatchInput& input,
            bool bLobbed,
            SurvivorAgentBatchOutput& output);
        SurvivorHazardHandle SpawnDirectedProjectile(
            const SurvivorAgentBatchState& state,
            double directionX,
            double directionZ,
            double targetX,
            double targetZ,
            double lifetime,
            bool bCreatesZone,
            SurvivorAgentBatchOutput& output);
        SurvivorHazardHandle SpawnZone(
            SurvivorAgentKey owner,
            double x,
            double y,
            double z,
            double damage,
            double radius,
            double duration,
            double tickInterval,
            SurvivorAgentBatchOutput& output);
        void UpdateBossPhase(
            SurvivorAgentBatchState& state,
            SurvivorAgentBatchOutput& output);

        static uint32_t NextGeneration(uint32_t generation);

        std::vector<SurvivorAgentBatchState> mAgents;
        std::unordered_map<uint32_t, size_t> mAgentIndices;
        std::unordered_map<uint32_t, uint64_t> mLatestActivationIds;
        std::vector<SurvivorEnemyProjectileState> mProjectiles;
        std::vector<SurvivorEnemyZoneState> mZones;
        uint32_t mProjectileGenerationSeed = 1;
        uint32_t mZoneGenerationSeed = 1;
        SurvivorEnemyTelemetry mPendingTelemetry;
    };
}
