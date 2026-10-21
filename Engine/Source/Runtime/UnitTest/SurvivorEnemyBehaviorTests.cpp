#include "Hepch.h"

#include "Runtime/UnitTest/SurvivorEnemyBehaviorTests.h"
#include "Runtime/Scripting/SurvivorAgentBatch.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace HEngine::Tests
{
    namespace
    {
        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        void RequireNear(double actual, double expected, std::string_view message)
        {
            constexpr double tolerance = 1.0e-10;
            if (std::abs(actual - expected) > tolerance)
                throw std::runtime_error(std::string(message));
        }

        SurvivorAgentBatchState MakeAgent(
            uint32_t entityId,
            uint64_t activationId,
            SurvivorEnemyBehavior behavior,
            double x = -2.0,
            double z = 0.0)
        {
            SurvivorAgentBatchState state;
            state.Key = { entityId, activationId };
            state.Behavior = behavior;
            state.X = x;
            state.Z = z;
            state.Health = 100.0;
            state.MaxHealth = 100.0;
            state.Speed = 0.0;
            state.Damage = 10.0;
            state.Radius = 0.5;
            state.Attack.Range = 5.0;
            state.Attack.PreferredRange = 5.0;
            state.Attack.Cooldown = 0.4;
            state.Attack.SourceHitCooldown = 0.65;
            state.Attack.TelegraphDuration = 0.1;
            state.Attack.ActiveDuration = 0.1;
            state.Attack.RecoveryDuration = 0.1;
            state.Attack.ChargeSpeed = 10.0;
            state.Attack.ProjectileSpeed = 10.0;
            state.Attack.ProjectileLifetime = 1.0;
            state.Attack.ProjectileRadius = 0.10;
            state.Attack.ZoneRadius = 1.5;
            state.Attack.ZoneDuration = 1.0;
            state.Attack.ZoneTickInterval = 0.2;
            return state;
        }

        SurvivorAgentBatchInput MakeInput(
            double deltaTime,
            double hurtIFrame = 0.0,
            double playerX = 0.0,
            double playerZ = 0.0)
        {
            SurvivorAgentBatchInput input;
            input.PlayerX = playerX;
            input.PlayerZ = playerZ;
            input.PlayerRadius = 0.42;
            input.DeltaTime = deltaTime;
            input.PlayerHurtIFrameRemaining = hurtIFrame;
            input.PlayerHurtIFrameDuration = 0.45;
            return input;
        }

        void EnterActive(
            SurvivorAgentBatch& batch,
            SurvivorAgentBatchOutput& output,
            double telegraphDuration = 0.1)
        {
            batch.Update(MakeInput(0.01), output);
            batch.Update(MakeInput(telegraphDuration), output);
        }

        void AttackPhaseAndChargeContract()
        {
            SurvivorAgentBatch batch;
            SurvivorAgentBatchState charger = MakeAgent(
                101,
                1,
                SurvivorEnemyBehavior::Charger,
                -2.0);
            charger.Attack.ActiveDuration = 0.2;
            Require(batch.Activate(charger), "The charger activation was rejected.");

            SurvivorAgentBatchOutput output;
            batch.Update(MakeInput(0.01), output);
            const SurvivorAgentBatchState* state = batch.FindAgent({ 101, 1 });
            Require(state && state->AttackPhase == SurvivorAttackPhase::Telegraph,
                "A ready charger did not enter Telegraph.");
            Require(output.Telemetry.TelegraphsStarted == 1,
                "The charger telegraph was not counted exactly once.");

            batch.Update(MakeInput(0.1), output);
            state = batch.FindAgent({ 101, 1 });
            Require(state->AttackPhase == SurvivorAttackPhase::Active,
                "The charger did not transition Telegraph -> Active.");
            Require(output.Telemetry.AttacksActivated == 1,
                "The charger active transition was not counted.");

            batch.Update(MakeInput(0.1), output);
            Require(output.DamageEvents.empty(),
                "The charge hit before its body reached the player.");
            batch.Update(MakeInput(0.1), output);
            state = batch.FindAgent({ 101, 1 });
            Require(state->AttackPhase == SurvivorAttackPhase::Recovery,
                "The charger did not transition Active -> Recovery.");
            Require(output.DamageEvents.size() == 1 &&
                output.DamageEvents.front().Kind == SurvivorDamageKind::Charge,
                "The charger did not aggregate its single charge hit.");
            RequireNear(output.PlayerHurtIFrameRemaining, 0.45,
                "A charge hit did not publish the 0.45 second player hurt i-frame.");

            batch.Update(MakeInput(0.1), output);
            state = batch.FindAgent({ 101, 1 });
            Require(state->AttackPhase == SurvivorAttackPhase::Idle &&
                state->AttackCooldownRemaining > 0.0,
                "Recovery did not publish a per-agent attack cooldown.");
            batch.Update(MakeInput(0.2), output);
            Require(batch.FindAgent({ 101, 1 })->AttackPhase ==
                SurvivorAttackPhase::Idle,
                "The per-agent cooldown allowed an early second telegraph.");
        }

        void PlayerHurtIFrameSeparationContract()
        {
            SurvivorAgentBatch batch;
            SurvivorAgentBatchState first = MakeAgent(
                201,
                1,
                SurvivorEnemyBehavior::Chaser,
                -0.5);
            SurvivorAgentBatchState second = MakeAgent(
                202,
                1,
                SurvivorEnemyBehavior::Chaser,
                0.5);
            first.Damage = 37.0;
            second.Damage = 91.0;
            Require(batch.Activate(first) && batch.Activate(second),
                "The simultaneous melee fixture was rejected.");

            SurvivorAgentBatchOutput output;
            batch.Update(MakeInput(0.01), output);
            batch.Update(MakeInput(0.1), output);
            Require(output.DamageEvents.size() == 1,
                "One player hurt i-frame did not collapse simultaneous agent hits.");
            Require(output.DamageEvents.front().Source == SurvivorAgentKey{ 201, 1 },
                "Damage aggregation changed stable activation order.");
            RequireNear(output.DamageEvents.front().Damage, 37.0,
                "The batch pre-applied a Project-layer damage reduction.");
            Require(output.DamageEvents.front().Kind == SurvivorDamageKind::Melee,
                "The raw damage event lost its source kind.");
            RequireNear(output.PlayerHurtIFrameRemaining, 0.45,
                "The batch did not return its player hurt i-frame separately from damage.");
            RequireNear(batch.FindAgent({ 201, 1 })->SourceHitCooldownRemaining, 0.65,
                "The damaging source did not start its own cooldown.");
            RequireNear(batch.FindAgent({ 202, 1 })->SourceHitCooldownRemaining, 0.65,
                "The i-frame-blocked source did not retain its own cooldown.");

            SurvivorAgentBatchInput coolingInput = MakeInput(
                0.20,
                output.PlayerHurtIFrameRemaining);
            batch.Update(coolingInput, output);
            RequireNear(output.PlayerHurtIFrameRemaining, 0.25,
                "The caller-owned player hurt i-frame did not count down from its input.");
            RequireNear(batch.FindAgent({ 201, 1 })->SourceHitCooldownRemaining, 0.45,
                "The first source cooldown did not count down independently.");
            RequireNear(batch.FindAgent({ 202, 1 })->SourceHitCooldownRemaining, 0.45,
                "The second source cooldown did not count down independently.");
        }

        void RangedProjectileContract()
        {
            SurvivorAgentBatch batch;
            SurvivorAgentBatchState ranged = MakeAgent(
                301,
                1,
                SurvivorEnemyBehavior::Ranged,
                -1.0);
            ranged.Attack.TelegraphDuration = 0.05;
            Require(batch.Activate(ranged), "The ranged activation was rejected.");

            SurvivorAgentBatchOutput output;
            EnterActive(batch, output, 0.05);
            Require(output.ProjectileEvents.size() == 1 &&
                output.ProjectileEvents.front().Kind == SurvivorHazardEventKind::Spawned,
                "A ranged active frame did not aggregate a projectile spawn.");
            const SurvivorHazardHandle handle = output.ProjectileEvents.front().Handle;
            Require(batch.IsProjectileHandleCurrent(handle),
                "A freshly spawned projectile handle was not current.");

            batch.Update(MakeInput(0.05), output);
            Require(output.DamageEvents.size() == 1 &&
                output.DamageEvents.front().Kind == SurvivorDamageKind::Projectile,
                "The enemy projectile did not aggregate its player hit.");
            Require(!batch.IsProjectileHandleCurrent(handle),
                "A consumed projectile handle remained current.");
            Require(output.Telemetry.ProjectileHits == 1,
                "The projectile hit telemetry was not aggregated.");
        }

        void ZoneAndLobberContract()
        {
            SurvivorAgentBatch zoneBatch;
            SurvivorAgentBatchState zone = MakeAgent(
                401,
                1,
                SurvivorEnemyBehavior::Zone,
                -1.0);
            zone.Attack.TelegraphDuration = 0.05;
            Require(zoneBatch.Activate(zone), "The zone activation was rejected.");
            SurvivorAgentBatchOutput output;
            EnterActive(zoneBatch, output, 0.05);
            Require(output.ZoneEvents.size() == 1 &&
                output.ZoneEvents.front().Kind == SurvivorHazardEventKind::Spawned,
                "A zone attack did not aggregate its spawn event.");
            const SurvivorHazardHandle zoneHandle = output.ZoneEvents.front().Handle;
            Require(zoneBatch.IsZoneHandleCurrent(zoneHandle),
                "A freshly spawned zone handle was not current.");
            zoneBatch.Update(MakeInput(0.01), output);
            Require(output.DamageEvents.size() == 1 &&
                output.DamageEvents.front().Kind == SurvivorDamageKind::Zone,
                "A live zone did not aggregate its due damage tick.");
            Require(output.Telemetry.ZoneTicks == 1,
                "Zone tick telemetry was not aggregated.");

            SurvivorAgentBatch lobberBatch;
            SurvivorAgentBatchState lobber = MakeAgent(
                402,
                1,
                SurvivorEnemyBehavior::Lobber,
                -1.0);
            lobber.Attack.TelegraphDuration = 0.05;
            lobber.Attack.ProjectileSpeed = 10.0;
            Require(lobberBatch.Activate(lobber), "The lobber activation was rejected.");
            EnterActive(lobberBatch, output, 0.05);
            Require(output.ProjectileEvents.size() == 1,
                "A lobber did not create its generation-safe projectile.");
            lobberBatch.Update(MakeInput(0.1), output);
            Require(!output.ZoneEvents.empty() &&
                output.ZoneEvents.front().Kind == SurvivorHazardEventKind::Spawned,
                "A lobbed projectile did not resolve into a zone.");
        }

        void PreferredRangeAttackContract()
        {
            constexpr std::array behaviors = {
                SurvivorEnemyBehavior::Ranged,
                SurvivorEnemyBehavior::Lobber,
                SurvivorEnemyBehavior::Zone,
                SurvivorEnemyBehavior::Summoner
            };

            for (size_t index = 0; index < behaviors.size(); ++index)
            {
                SurvivorAgentBatch batch;
                SurvivorAgentBatchState agent = MakeAgent(
                    450 + static_cast<uint32_t>(index),
                    1,
                    behaviors[index],
                    -6.5);
                agent.Speed = 3.0;
                agent.Attack.Range = 1.0;
                agent.Attack.PreferredRange = 8.0;
                agent.Attack.TelegraphDuration = 0.05;
                Require(batch.Activate(agent),
                    "The preferred-range activation fixture was rejected.");

                SurvivorAgentBatchOutput output;
                batch.Update(MakeInput(0.01), output);
                const SurvivorAgentBatchState* state = batch.FindAgent({
                    450 + static_cast<uint32_t>(index),
                    1
                });
                Require(state && state->AttackPhase == SurvivorAttackPhase::Telegraph,
                    "A specialist outside contact range did not enter Telegraph at its preferred range.");
                RequireNear(state->X, -6.5,
                    "A specialist strafed at full speed inside its preferred range band.");
                RequireNear(state->Z, 0.0,
                    "A specialist drifted sideways inside its preferred range band.");

                batch.Update(MakeInput(0.05), output);
                Require(batch.FindAgent({
                    450 + static_cast<uint32_t>(index),
                    1
                })->AttackPhase == SurvivorAttackPhase::Active,
                    "A preferred-range specialist did not activate its attack.");
                if (behaviors[index] == SurvivorEnemyBehavior::Ranged ||
                    behaviors[index] == SurvivorEnemyBehavior::Lobber)
                {
                    Require(output.ProjectileEvents.size() == 1,
                        "A preferred-range projectile specialist did not spawn its projectile.");
                }
                else if (behaviors[index] == SurvivorEnemyBehavior::Zone)
                {
                    Require(output.ZoneEvents.size() == 1,
                        "A preferred-range zone specialist did not spawn its zone.");
                }
                else
                {
                    Require(output.SummonEvents.size() == 1,
                        "A preferred-range summoner did not publish its summon request.");
                }
            }
        }

        void ForwardBiasedSpecialistMovementContract()
        {
            constexpr std::array behaviors = {
                SurvivorEnemyBehavior::Flanker,
                SurvivorEnemyBehavior::Ghost
            };

            for (size_t index = 0; index < behaviors.size(); ++index)
            {
                SurvivorAgentBatch batch;
                SurvivorAgentBatchState agent = MakeAgent(
                    470 + static_cast<uint32_t>(index),
                    1,
                    behaviors[index],
                    -10.0);
                agent.Speed = 1.0;
                agent.Attack.Range = 1.0;
                agent.Attack.PreferredRange = 1.0;
                agent.MovementPhase = 1.5707963267948966;
                Require(batch.Activate(agent),
                    "The forward-biased movement fixture was rejected.");

                SurvivorAgentBatchOutput output;
                batch.Update(MakeInput(1.0), output);
                const SurvivorAgentBatchState* state = batch.FindAgent({
                    470 + static_cast<uint32_t>(index),
                    1
                });
                Require(state != nullptr,
                    "The forward-biased movement fixture lost its agent.");
                const double forwardTravel = state->X + 10.0;
                const double lateralTravel = std::abs(state->Z);
                Require(forwardTravel > lateralTravel * 3.0,
                    "A flanker or ghost still traveled primarily sideways.");
            }
        }

        void ShieldAndResistanceContract()
        {
            SurvivorAgentBatch batch;
            SurvivorAgentBatchState shield = MakeAgent(
                501,
                1,
                SurvivorEnemyBehavior::Shield);
            shield.Attack.ShieldStrength = 25.0;
            Require(batch.Activate(shield), "The shield activation was rejected.");
            const SurvivorAgentHitResult shieldHit = batch.ApplyHit(
                { 501, 1 },
                40.0,
                0.0,
                0.0);
            RequireNear(shieldHit.DamagePrevented, 25.0,
                "The shield did not report prevented damage.");
            RequireNear(shieldHit.DamageApplied, 15.0,
                "The shield applied the wrong health damage.");
            RequireNear(shieldHit.Health, 85.0,
                "The shield changed health before its capacity was exhausted.");

            SurvivorAgentBatchState resistant = MakeAgent(
                502,
                1,
                SurvivorEnemyBehavior::Chaser);
            resistant.Resistances.Damage = 0.25;
            resistant.Resistances.Freeze = 0.50;
            resistant.Resistances.Slow = 1.0;
            resistant.Resistances.Poison = 0.25;
            Require(batch.Activate(resistant), "The resistance activation was rejected.");
            const SurvivorAgentHitResult resistedHit = batch.ApplyHit(
                { 502, 1 },
                20.0);
            RequireNear(resistedHit.DamageApplied, 15.0,
                "Damage resistance changed its clamp or application order.");
            const SurvivorStatusApplyResult freeze = batch.ApplyStatus(
                { 502, 1 },
                SurvivorStatusKind::Freeze,
                2.0);
            RequireNear(freeze.AppliedDuration, 1.0,
                "Freeze resistance did not scale the status timer.");
            const SurvivorStatusApplyResult slow = batch.ApplyStatus(
                { 502, 1 },
                SurvivorStatusKind::Slow,
                2.0,
                0.4);
            Require(slow.bResisted && slow.AppliedDuration == 0.0,
                "Full slow resistance did not reject the status.");
            const SurvivorStatusApplyResult poison = batch.ApplyStatus(
                { 502, 1 },
                SurvivorStatusKind::Poison,
                2.0,
                3.5);
            RequireNear(poison.AppliedDuration, 1.5,
                "Poison resistance did not scale the status timer.");
            RequireNear(batch.FindAgent({ 502, 1 })->Status.PoisonMagnitude, 3.5,
                "Poison magnitude was not retained with its timer.");

            SurvivorAgentBatchOutput output;
            batch.Update(MakeInput(0.25), output);
            RequireNear(batch.FindAgent({ 502, 1 })->Status.Freeze, 0.75,
                "The applied freeze timer did not count down deterministically.");
            Require(output.Telemetry.StatusApplications == 2 &&
                output.Telemetry.StatusResists == 1 &&
                output.Telemetry.ShieldDamagePrevented == 25.0,
                "Status or shield telemetry was not forwarded to the next batch output.");
        }

        void SummonAndBossPhaseContract()
        {
            SurvivorAgentBatch summonBatch;
            SurvivorAgentBatchState summoner = MakeAgent(
                601,
                1,
                SurvivorEnemyBehavior::Summoner,
                -1.0);
            summoner.Attack.TelegraphDuration = 0.05;
            summoner.Attack.SummonCount = 3;
            Require(summonBatch.Activate(summoner), "The summoner activation was rejected.");
            SurvivorAgentBatchOutput output;
            EnterActive(summonBatch, output, 0.05);
            Require(output.SummonEvents.size() == 1 &&
                output.SummonEvents.front().Count == 3,
                "A summoner did not aggregate its requested cohort.");
            Require(output.Telemetry.SummonRequests == 3,
                "Summon telemetry did not count requested agents.");

            SurvivorAgentBatch bossBatch;
            SurvivorAgentBatchState boss = MakeAgent(
                602,
                1,
                SurvivorEnemyBehavior::Boss,
                -4.0);
            boss.Health = 100.0;
            boss.MaxHealth = 100.0;
            Require(bossBatch.Activate(boss), "The boss activation was rejected.");
            Require(bossBatch.FindAgent({ 602, 1 })->BossPhase ==
                SurvivorBossPhase::PhaseOne,
                "A boss did not initialize in phase one.");
            Require(bossBatch.SetHealth({ 602, 1 }, 60.0),
                "The boss health fixture update was rejected.");
            bossBatch.Update(MakeInput(0.0), output);
            Require(bossBatch.FindAgent({ 602, 1 })->BossPhase ==
                SurvivorBossPhase::PhaseTwo,
                "A boss did not enter phase two at 66% health.");
            Require(bossBatch.SetHealth({ 602, 1 }, 30.0),
                "The phase-three health fixture update was rejected.");
            bossBatch.Update(MakeInput(0.0), output);
            Require(bossBatch.FindAgent({ 602, 1 })->BossPhase ==
                SurvivorBossPhase::PhaseThree,
                "A boss did not enter phase three at 33% health.");
            Require(output.Telemetry.BossPhaseChanges == 1,
                "The phase-three transition was not counted exactly once.");
        }

        void StarDragonSkillAndEnrageContract()
        {
            const auto activateStarDragon = [](uint32_t entityId, double health)
            {
                SurvivorAgentBatchState boss = MakeAgent(
                    entityId,
                    1,
                    SurvivorEnemyBehavior::Boss,
                    -4.0);
                boss.Health = health;
                boss.MaxHealth = 100.0;
                boss.BossBehavior = SurvivorBossBehavior::StarDragon;
                boss.Attack.TelegraphDuration = 0.05;
                boss.Attack.ProjectileLifetime = 2.0;
                boss.Attack.SummonCount = 3;
                return boss;
            };

            SurvivorAgentBatch ringBatch;
            Require(ringBatch.Activate(activateStarDragon(610, 100.0)),
                "The ring-barrage Star Dragon activation was rejected.");
            SurvivorAgentBatchOutput output;
            EnterActive(ringBatch, output, 0.05);
            const SurvivorAgentBatchState* ring = ringBatch.FindAgent({ 610, 1 });
            Require(ring && ring->BossSkill == SurvivorBossSkill::RingBarrage &&
                output.ProjectileEvents.size() == 8 &&
                output.Telemetry.BossRingBarrages == 1,
                "High-health Star Dragon did not emit one deterministic radial barrage.");

            SurvivorAgentBatch breathBatch;
            Require(breathBatch.Activate(activateStarDragon(611, 60.0)),
                "The sweep-breath Star Dragon activation was rejected.");
            EnterActive(breathBatch, output, 0.05);
            const SurvivorAgentBatchState* breath = breathBatch.FindAgent({ 611, 1 });
            Require(breath && breath->BossPhase == SurvivorBossPhase::PhaseTwo &&
                breath->BossSkill == SurvivorBossSkill::SweepBreath &&
                output.ProjectileEvents.size() == 5 &&
                output.Telemetry.BossSweepBreaths == 1,
                "Mid-health Star Dragon did not emit one deterministic sweep breath.");

            SurvivorAgentBatch chargeBatch;
            Require(chargeBatch.Activate(activateStarDragon(612, 30.0)),
                "The final-charge Star Dragon activation was rejected.");
            EnterActive(chargeBatch, output, 0.05);
            const SurvivorAgentBatchState* charge = chargeBatch.FindAgent({ 612, 1 });
            Require(charge && charge->BossPhase == SurvivorBossPhase::PhaseThree &&
                charge->BossSkill == SurvivorBossSkill::FinalCharge &&
                output.ZoneEvents.size() == 1 &&
                output.SummonEvents.size() == 1 &&
                output.SummonEvents.front().Count == 3 &&
                output.Telemetry.BossFinalCharges == 1,
                "Low-health Star Dragon did not combine charge pressure, zone, and summons.");
            const double chargeStartX = charge->X;
            chargeBatch.Update(MakeInput(0.05), output);
            Require(chargeBatch.FindAgent({ 612, 1 })->X > chargeStartX,
                "The final-charge active phase did not move toward the player.");

            SurvivorAgentBatch enrageBatch;
            SurvivorAgentBatchState enraging = activateStarDragon(613, 100.0);
            enraging.X = -20.0;
            Require(enrageBatch.Activate(enraging),
                "The enrage Star Dragon activation was rejected.");
            enrageBatch.Update(MakeInput(119.0), output);
            Require(!enrageBatch.FindAgent({ 613, 1 })->bBossEnraged &&
                output.Telemetry.BossEnrages == 0,
                "Star Dragon enraged before 120 raw seconds.");
            enrageBatch.Update(MakeInput(1.0), output);
            const SurvivorAgentBatchState* enraged = enrageBatch.FindAgent({ 613, 1 });
            Require(enraged->bBossEnraged &&
                output.Telemetry.BossEnrages == 1,
                "Star Dragon did not enrage exactly at 120 raw seconds.");
            RequireNear(enraged->Damage, 12.0,
                "Star Dragon enrage did not apply the configured mild damage pressure.");
            enrageBatch.Update(MakeInput(5.0), output);
            Require(output.Telemetry.BossEnrages == 0,
                "Star Dragon enrage telemetry repeated after the threshold.");
        }

        void InterruptAndObjectivePersistenceContract()
        {
            SurvivorAgentBatch regularBatch;
            SurvivorAgentBatchState regular = MakeAgent(
                620,
                1,
                SurvivorEnemyBehavior::Chaser,
                -1.0);
            regular.bNoDistanceRetire = true;
            Require(regularBatch.Activate(regular),
                "The interrupt regular-agent activation was rejected.");
            Require(regularBatch.FindAgent({ 620, 1 })->bNoDistanceRetire,
                "An encounter agent lost its initial no-retire policy.");
            SurvivorAgentBatchOutput output;
            regularBatch.Update(MakeInput(0.01), output);
            Require(regularBatch.FindAgent({ 620, 1 })->AttackPhase ==
                SurvivorAttackPhase::Telegraph,
                "The interrupt fixture did not enter a telegraph.");
            const SurvivorStatusApplyResult regularInterrupt = regularBatch.ApplyStatus(
                { 620, 1 },
                SurvivorStatusKind::Interrupt,
                0.35);
            const SurvivorAgentBatchState* interrupted = regularBatch.FindAgent({ 620, 1 });
            Require(regularInterrupt.bHardInterrupted &&
                interrupted->AttackPhase == SurvivorAttackPhase::Recovery,
                "A regular agent did not convert interrupt into a short hard stagger.");
            RequireNear(interrupted->AttackPhaseRemaining, 0.35,
                "The regular-agent interrupt did not preserve its raw duration.");
            Require(regularBatch.SetDistanceRetirement({ 620, 1 }, false) &&
                regularBatch.FindAgent({ 620, 1 })->bNoDistanceRetire,
                "A live encounter agent could not disable distance retirement.");
            Require(regularBatch.SetDistanceRetirement({ 620, 1 }, true) &&
                !regularBatch.FindAgent({ 620, 1 })->bNoDistanceRetire,
                "A converted roaming agent could not restore distance retirement.");
            Require(!regularBatch.SetDistanceRetirement({ 620, 0 }, false),
                "A stale activation changed distance-retirement policy.");

            SurvivorAgentBatch bossBatch;
            SurvivorAgentBatchState boss = MakeAgent(
                621,
                7,
                SurvivorEnemyBehavior::Boss,
                -1.0);
            boss.BossBehavior = SurvivorBossBehavior::StarDragon;
            boss.bObjectiveBoss = true;
            boss.bFinalBoss = true;
            Require(bossBatch.Activate(boss),
                "The objective-boss interrupt fixture was rejected.");
            bossBatch.Update(MakeInput(0.01), output);
            const SurvivorStatusApplyResult bossInterrupt = bossBatch.ApplyStatus(
                { 621, 7 },
                SurvivorStatusKind::Interrupt,
                0.35);
            const SurvivorAgentBatchState* delayedBoss = bossBatch.FindAgent({ 621, 7 });
            Require(!bossInterrupt.bHardInterrupted &&
                delayedBoss->AttackPhase == SurvivorAttackPhase::Telegraph &&
                delayedBoss->Status.ActionDelay == 0.35,
                "A boss interrupt hard-canceled its action instead of delaying the next one.");
            Require(delayedBoss->bObjectiveBoss &&
                delayedBoss->bFinalBoss &&
                delayedBoss->bNoDistanceRetire,
                "Objective/final/no-retire flags did not survive native activation.");
            Require(!bossBatch.SetDistanceRetirement({ 621, 7 }, true) &&
                bossBatch.FindAgent({ 621, 7 })->bNoDistanceRetire,
                "The objective boss allowed distance retirement to be re-enabled.");
        }

        SurvivorHazardHandle SpawnRangedHandle(
            SurvivorAgentBatch& batch,
            SurvivorAgentBatchOutput& output,
            uint32_t entityId,
            uint64_t activationId)
        {
            SurvivorAgentBatchState ranged = MakeAgent(
                entityId,
                activationId,
                SurvivorEnemyBehavior::Ranged,
                -5.0);
            ranged.Attack.Range = 8.0;
            ranged.Attack.PreferredRange = 8.0;
            ranged.Attack.TelegraphDuration = 0.05;
            ranged.Attack.ProjectileSpeed = 0.1;
            ranged.Attack.ProjectileLifetime = 10.0;
            Require(batch.Activate(ranged), "The generation fixture ranged agent was rejected.");
            EnterActive(batch, output, 0.05);
            Require(output.ProjectileEvents.size() == 1,
                "The generation fixture did not spawn one projectile.");
            return output.ProjectileEvents.front().Handle;
        }

        SurvivorHazardHandle SpawnZoneHandle(
            SurvivorAgentBatch& batch,
            SurvivorAgentBatchOutput& output,
            uint32_t entityId,
            uint64_t activationId)
        {
            SurvivorAgentBatchState zone = MakeAgent(
                entityId,
                activationId,
                SurvivorEnemyBehavior::Zone,
                -5.0);
            zone.Attack.Range = 8.0;
            zone.Attack.PreferredRange = 8.0;
            zone.Attack.TelegraphDuration = 0.05;
            Require(batch.Activate(zone), "The generation fixture zone agent was rejected.");
            EnterActive(batch, output, 0.05);
            Require(output.ZoneEvents.size() == 1,
                "The generation fixture did not spawn one zone.");
            return output.ZoneEvents.front().Handle;
        }

        void HazardGenerationContract()
        {
            SurvivorAgentBatch batch;
            SurvivorAgentBatchOutput output;
            const SurvivorHazardHandle firstProjectile = SpawnRangedHandle(
                batch,
                output,
                701,
                1);
            Require(batch.DeactivateProjectile(firstProjectile),
                "The live projectile could not be deactivated.");
            Require(!batch.IsProjectileHandleCurrent(firstProjectile),
                "A deactivated projectile handle remained current.");

            Require(batch.Deactivate({ 701, 1 }),
                "The first ranged generation could not be deactivated.");
            const SurvivorHazardHandle secondProjectile = SpawnRangedHandle(
                batch,
                output,
                701,
                2);
            Require(secondProjectile.Slot == firstProjectile.Slot &&
                secondProjectile.Generation != firstProjectile.Generation,
                "A reused projectile slot did not advance its generation.");
            Require(!batch.IsProjectileHandleCurrent(firstProjectile) &&
                batch.IsProjectileHandleCurrent(secondProjectile),
                "A stale projectile handle aliased its replacement.");

            const SurvivorHazardHandle firstZone = SpawnZoneHandle(
                batch,
                output,
                702,
                1);
            Require(batch.IsZoneHandleCurrent(firstZone),
                "The generation fixture zone was not current.");
            batch.Clear();
            Require(!batch.IsProjectileHandleCurrent(secondProjectile) &&
                !batch.IsZoneHandleCurrent(firstZone),
                "Clear retained a stale projectile or zone handle.");

            const SurvivorHazardHandle resetProjectile = SpawnRangedHandle(
                batch,
                output,
                701,
                1);
            const SurvivorHazardHandle resetZone = SpawnZoneHandle(
                batch,
                output,
                702,
                1);
            Require(resetProjectile.Generation != secondProjectile.Generation &&
                resetZone.Generation != firstZone.Generation,
                "A new run reused a pre-reset hazard generation.");
        }

        struct ReferenceCharger
        {
            double X = -1.5;
            double AttackCooldown = 0.0;
            double PhaseRemaining = 0.0;
            SurvivorAttackPhase Phase = SurvivorAttackPhase::Idle;
            bool bCommitted = false;
        };

        void ReferenceChargerStep(ReferenceCharger& state, double deltaTime)
        {
            state.AttackCooldown = std::max(0.0, state.AttackCooldown - deltaTime);
            const double distance = std::abs(state.X);
            if (state.Phase == SurvivorAttackPhase::Active)
            {
                state.X = std::min(0.0, state.X + 2.0 * deltaTime);
                if (!state.bCommitted && std::abs(state.X) <= 0.92)
                    state.bCommitted = true;
            }
            else if (state.Phase == SurvivorAttackPhase::Idle)
            {
                state.X = std::min(
                    -1.0,
                    state.X + std::min(2.0 * deltaTime, std::max(0.0, distance - 1.0)));
            }

            switch (state.Phase)
            {
            case SurvivorAttackPhase::Idle:
                if (state.AttackCooldown <= 0.0 && distance <= 1.0)
                {
                    state.Phase = SurvivorAttackPhase::Telegraph;
                    state.PhaseRemaining = 0.25;
                    state.bCommitted = false;
                }
                break;
            case SurvivorAttackPhase::Telegraph:
                state.PhaseRemaining -= deltaTime;
                if (state.PhaseRemaining <= 0.0)
                {
                    state.Phase = SurvivorAttackPhase::Active;
                    state.PhaseRemaining = 0.25;
                }
                break;
            case SurvivorAttackPhase::Active:
                state.PhaseRemaining -= deltaTime;
                if (state.PhaseRemaining <= 0.0)
                {
                    state.Phase = SurvivorAttackPhase::Recovery;
                    state.PhaseRemaining = 0.25;
                }
                break;
            case SurvivorAttackPhase::Recovery:
                state.PhaseRemaining -= deltaTime;
                if (state.PhaseRemaining <= 0.0)
                {
                    state.Phase = SurvivorAttackPhase::Idle;
                    state.PhaseRemaining = 0.0;
                    state.AttackCooldown = 0.5;
                    state.bCommitted = false;
                }
                break;
            }
        }

        void DeterministicReferenceParityContract()
        {
            SurvivorAgentBatch batch;
            SurvivorAgentBatchState charger = MakeAgent(
                801,
                1,
                SurvivorEnemyBehavior::Charger,
                -1.5);
            charger.Speed = 2.0;
            charger.MovementPhase = 0.0;
            charger.Attack.Range = 1.0;
            charger.Attack.PreferredRange = 1.0;
            charger.Attack.Cooldown = 0.5;
            charger.Attack.TelegraphDuration = 0.25;
            charger.Attack.ActiveDuration = 0.25;
            charger.Attack.RecoveryDuration = 0.25;
            charger.Attack.ChargeSpeed = 2.0;
            Require(batch.Activate(charger), "The parity charger was rejected.");

            ReferenceCharger reference;
            SurvivorAgentBatchOutput output;
            for (int step = 0; step < 9; ++step)
            {
                ReferenceChargerStep(reference, 0.125);
                SurvivorAgentBatchInput input = MakeInput(0.125);
                input.Elapsed = 0.0;
                batch.Update(input, output);
                const SurvivorAgentBatchState* actual = batch.FindAgent({ 801, 1 });
                Require(actual != nullptr,
                    "The parity fixture lost its live charger.");
                RequireNear(actual->X, reference.X,
                    "The C++ batch diverged from the deterministic charger reference position.");
                Require(actual->AttackPhase == reference.Phase,
                    "The C++ batch diverged from the deterministic attack phase reference.");
                RequireNear(actual->AttackPhaseRemaining, reference.PhaseRemaining,
                    "The C++ batch diverged from the deterministic phase timer reference.");
                RequireNear(actual->AttackCooldownRemaining, reference.AttackCooldown,
                    "The C++ batch diverged from the deterministic cooldown reference.");
                Require(actual->bAttackCommitted == reference.bCommitted,
                    "The C++ batch diverged from the deterministic hit-commit reference.");
            }
        }

        void BehaviorCoverageContract()
        {
            constexpr std::array behaviors = {
                SurvivorEnemyBehavior::Chaser,
                SurvivorEnemyBehavior::Flanker,
                SurvivorEnemyBehavior::Armored,
                SurvivorEnemyBehavior::Bruiser,
                SurvivorEnemyBehavior::Zone,
                SurvivorEnemyBehavior::Charger,
                SurvivorEnemyBehavior::Ranged,
                SurvivorEnemyBehavior::Lobber,
                SurvivorEnemyBehavior::Ghost,
                SurvivorEnemyBehavior::Shield,
                SurvivorEnemyBehavior::Suicide,
                SurvivorEnemyBehavior::Summoner,
                SurvivorEnemyBehavior::Boss
            };
            Require(behaviors.size() == 13 &&
                static_cast<uint8_t>(behaviors.back()) == 12,
                "The compact behavior enum no longer covers the required roles.");
        }
    }

    void SurvivorEnemyBehaviorsContract()
    {
        BehaviorCoverageContract();
        AttackPhaseAndChargeContract();
        PlayerHurtIFrameSeparationContract();
        RangedProjectileContract();
        ZoneAndLobberContract();
        PreferredRangeAttackContract();
        ForwardBiasedSpecialistMovementContract();
        ShieldAndResistanceContract();
        SummonAndBossPhaseContract();
        StarDragonSkillAndEnrageContract();
        InterruptAndObjectivePersistenceContract();
        HazardGenerationContract();
        DeterministicReferenceParityContract();
    }
}
