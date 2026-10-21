"""Differential fixtures use the unchanged Python game as the reference oracle."""
import argparse
import json
import random
import subprocess
import sys
from pathlib import Path
from dataclasses import asdict
from types import SimpleNamespace, ModuleType

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Project/Scripts"))
import SurvivorBuildRules as builds
import SurvivorBalance as balance
import SurvivorEconomy as economy
import SurvivorRules as rules
from SurvivorContent import CHARACTER_DEFS
from SurvivorCatalogWorld import CHALLENGES
from SurvivorCatalogItems import WEAPONS, TOMES, EVOLUTIONS
import SurvivorRegionObjectives as regions
import SurvivorProfile as profiles
import SurvivorMetaProgression as meta
sys.path.insert(0, str(ROOT / "Project/Tools"))
import GenerateVerdantRift as terrain
sys.modules.setdefault("HEngine", ModuleType("HEngine"))
from SurvivorCombatHooks import SurvivorCombatHooksMixin


def fixtures():
    result = []
    rng = random.Random(7301)
    director=regions.EncounterDirector()
    for _ in range(300):
        order=rng.randrange(1,5);progress=rng.random();difficulty=rng.choice(['adventure','fierce','nightmare']);challenge=rng.uniform(.3,2);kills=rng.uniform(0,50)
        tier=regions._plain(regions.ENCOUNTER_TIERS[order-1]);scale=regions.DIFFICULTY_COUNT_MULTIPLIERS[difficulty]
        result.append(dict(kind='encounter',input=dict(tier=tier,progress=progress,difficulty=scale,challenge=challenge,kills=kills),expected=dict(target=director.local_target(order,progress,difficulty,challenge),rate=director.spawn_rate(order,difficulty,challenge,kills))))
    for _ in range(250):
        trinkets=rng.sample(["popcorn_kernel","glass_star","boss_bell","comet_tail"],rng.randrange(5))
        tags=rng.sample(["projectile","explosion","zone","tick"],rng.randrange(5))
        full,boss,elite=[rng.choice([True,False]) for _ in range(3)]; velocity=rng.randrange(20)
        reference=SimpleNamespace(trinket_slots=trinkets,weapon_tag_cache={"test":set(tags)},health=100 if full else 50,max_health=100,_tome_level=lambda _:velocity)
        expected=SurvivorCombatHooksMixin._trinket_damage_multiplier(reference,"test",{"boss":boss,"elite":elite})
        result.append(dict(kind="trinketDamage",input=dict(trinkets=trinkets,tags=tags,fullHealth=full,boss=boss,elite=elite,velocityLevel=velocity),expected=expected))
    for level in range(1, 101):
        reference = SimpleNamespace(level=level, character_hook_state={}, base_character_weapon_damage_multiplier=1.0)
        SurvivorCombatHooksMixin._hook_starpaw_level(reference, {"level": level})
        SurvivorCombatHooksMixin._hook_honey_level(reference, {"level": level})
        result.append(dict(kind="characterLevel", input=level, expected=dict(might=reference.character_hook_state["starpawMight"], gold=reference.character_gold_multiplier)))
    profile_catalog = dict(defaultProfile=profiles.default_profile(), metaPowerUps=meta.META_POWER_UP_RULES, challenges=CHALLENGES)
    profile_cases = [{}, {"version": 5}, {"version": True}, {"version": 4.0}]
    for version in (1, 2, 3, 4):
        for index in range(60):
            if version == 1:
                item = dict(version=1, total_kills=rng.choice([0, 750, "850", -1, True]), best_level=rng.choice([1, 20, "35"]), best_survival=rng.choice([0, 120, "600"]), boss_kills=rng.choice([0, 1]), selected_character="azura")
            else:
                item = profiles.default_profile()
                item.update(version=version, walletGold=rng.choice([-1, 100, 3000000000]), selectedWeaponSlotLimit=rng.choice([2, 3, 6, 10]), powerUps={"meta_weapon_slots": rng.choice([0, 1, 3]), "meta_might": 99}, powerUpsEnabled={"meta_weapon_slots": False})
                item["owned"]["weapons"] += ["starfall", "tornado", "starfall"]
                item["selectedChallenges"] = [c["id"] for c in reversed(CHALLENGES)][:3] + ["unknown"]
                item["lastRun"] = dict(runId="parity", rewardCommitted=False)
                item["trackedQuests"] = ["quest_weapon_tornado", "q", "q", "r", "s"]
            profile_cases.append(item)
    for field, value in [("walletGold", True), ("walletGold", 1.5), ("owned", []), ("characterTraining", {"x": True}), ("lastRun", {"runId": "x", "rewardCommitted": 1}), ("powerUpsEnabled", None), ("trackedQuests", [""]), ("career", {"runs": "4"})]:
        item = profiles.default_profile(); item[field] = value; profile_cases.append(item)
    for item in profile_cases:
        try: expected = dict(valid=True, profile=profiles.migrate_profile(item))
        except profiles.ProfileError: expected = dict(valid=False)
        result.append(dict(kind="profile", input=dict(profile=item, catalog=profile_catalog), expected=expected))
    terrain_data = dict(terrainPlates=[asdict(p) for p in terrain.TERRAIN_PLATES], terrainRamps=[asdict(p) for p in terrain.RAMP_SPECS])
    points = [(rng.uniform(-100,100),rng.uniform(-100,100)) for _ in range(1000)]
    points += [point for plate in terrain.TERRAIN_PLATES for point in plate.outline]
    result.append(dict(kind="terrain", input=dict(catalog=terrain_data,points=points), expected=[terrain.terrain_surface_height(*point) for point in points]))
    controller = regions.RegionObjectiveController()
    operations=[]
    for definition in regions.REGION_CONFIG.values():
        x,y,z=definition["authoredPosition"]
        operations += [(1.5,x,z),(20,x,z),(1,x+100,z),(2,x+100,z),(12,x,z),(1.5,x,z),(60,x,z)]
    operations += [(2,0,0),(3,0,0),(1,0,0)]
    expected=[]
    for dt,x,z in operations:
        controller.update(dt,x,z)
        snapshot=controller.snapshot()
        expected.append(dict(objectiveState=snapshot["objectiveState"],completed=snapshot["completedRegions"],
                             states=[snapshot["regions"][id]["state"] for id in regions.REGION_IDS]))
    result.append(dict(kind="regions",input=dict(catalog=dict(regions=regions._plain(regions.REGION_CONFIG),objective=regions._plain(regions.OBJECTIVE_CONFIG)),operations=operations),expected=expected))
    for index in range(100):
        recipes = EVOLUTIONS[:1 + index % len(EVOLUTIONS)]
        config = dict(recipes=recipes,
                      weapon_levels={entry["weaponId"]: rng.choice([1, 9, 10, 11]) for entry in recipes},
                      tome_levels={entry["tomeId"]: rng.choice([0, 4, 5, 6]) for entry in recipes},
                      weapon_slots=list(dict.fromkeys(entry["weaponId"] for entry in recipes)))
        resolver = builds.EvolutionResolver(**config)
        tiers = [0, 1, 2, 3, 3, 3, 3]
        camel = lambda key: key.split("_")[0] + "".join(word.title() for word in key.split("_")[1:])
        result.append(dict(kind="evolution", input=dict(config={camel(key): value for key, value in config.items()}, tiers=tiers),
                           expected=[resolver.resolve_chest(tier) for tier in tiers]))
    for seed in range(100):
        config = dict(weapons=WEAPONS, tomes=TOMES,
                      owned={"weapons": [entry["id"] for entry in WEAPONS], "tomes": [entry["id"] for entry in TOMES]},
                      seed=seed, rerolls=2, skips=1, banishes=2,
                      max_weapon_slots=seed % 7, max_tome_slots=seed % 7)
        pool = builds.UpgradePoolController(**config)
        operations = ["roll", "reroll", "banish", "skip", "roll", "reroll", "banish", "reroll", "skip"]
        receipts = []
        for operation in operations:
            if operation == "roll":
                receipts.append(dict(status="rolled", choices=pool.roll()))
            elif operation == "banish":
                receipts.append(pool.banish(pool.current_choices[0]["id"] if pool.current_choices else "missing"))
            else:
                receipts.append(getattr(pool, operation)())
        camel = lambda key: key.split("_")[0] + "".join(word.title() for word in key.split("_")[1:])
        result.append(dict(kind="upgrade", input=dict(config={camel(key): value for key, value in config.items()}, operations=operations),
                           expected=dict(receipts=receipts, rerollsRemaining=pool.rerolls_remaining,
                                         skipsRemaining=pool.skips_remaining, banishesRemaining=pool.banishes_remaining,
                                         rollSerial=pool.roll_serial)))
    for character_id, character in CHARACTER_DEFS.items():
        for training in range(6):
            result.append(dict(kind="character", input=dict(id=character_id, definition=character, training=training),
                               expected=rules.compile_character_start_state(character_id, character, training)))
    for minute in (-5, 0, 1, 7, 15, 30):
        for mask in range(1 << len(CHALLENGES)):
            selected = [entry for index, entry in enumerate(CHALLENGES) if mask & (1 << index)]
            result.append(dict(kind="challenge", input=dict(challenges=selected, minute=minute),
                               expected=rules.combine_challenge_modifiers(selected, minute)))
    for level in range(-2, 202):
        result.append(dict(kind="experience", input=level, expected=balance.experience_requirement(level)))
    for seed in (0, 1, -1, 7301, 4294967295, 4294967296):
        reference = builds.DeterministicRng(seed)
        result.append(dict(kind="rng", input=seed, expected=[reference.next_u32() for _ in range(32)]))
        for weights in ((1, 2, 3), (0, -1, 7), (0.1, 0.3, 0.9)):
            reference = builds.DeterministicRng(seed)
            result.append(dict(kind="weighted", input=dict(seed=seed, weights=weights),
                               expected=[reference.weighted_index(weights) for _ in range(32)]))
    for _ in range(1000):
        values = [rng.uniform(-5, 100), rng.random(), rng.uniform(-1, 1),
                  rng.uniform(-5, 100), rng.uniform(-1, 1), rng.uniform(-5, 100)]
        defense = rules.resolve_defense(*values)
        expected = {"damage": defense.damage, "evaded": defense.evaded,
                    "armorPrevented": defense.armor_prevented,
                    "reductionPrevented": defense.reduction_prevented,
                    "shieldAbsorbed": defense.shield_absorbed,
                    "shieldRemaining": defense.shield_remaining}
        result.append(dict(kind="defense", input=values, expected=expected))
    for index in range(1000):
        values = dict(survival_seconds=rng.choice([0, 59.99, 60, 900, 86401, rng.uniform(-10, 1800)]),
                      kills=rng.randrange(-10, 12000), elite_kills=rng.randrange(-1, 30),
                      boss_kills=rng.randrange(-1, 10), level=rng.randrange(-1, 80),
                      victory=bool(index % 2), revives_used=index % 3,
                      quest_gold=index * 2, bonus_gold=index,
                      greed_tome_bonus=rng.random(), honeybun_bonus=rng.random(),
                      global_greed_bonus=rng.random(), difficulty_reward=rng.uniform(-1, 8),
                      challenge_reward=rng.uniform(-1, 8),
                      end_reason=rng.choice(["death", "victory", "abandon", "restart", "return_hub"]),
                      weapon_slot_limit=3 + index % 4)
        if index % 3 == 0:
            values["challenge_rewards"] = [rng.uniform(-1, 5) for _ in range(index % 7)]
        if index % 11 == 0:
            values.update(kills=99_000_000_000, elite_kills=99_000_000_000,
                          boss_kills=99_000_000_000, quest_gold=99_000_000_000)
        camel = lambda key: key.split("_")[0] + "".join(word.title() for word in key.split("_")[1:])
        result.append(dict(kind="reward", input={camel(key): value for key, value in values.items()},
                           expected=economy.compute_run_reward(**values)))
    for gameplay in (1, 4, 20):
        for visual in (0, 2, 30):
            budget = rules.ProjectileBudget(gameplay, visual)
            operations = [rng.choice(["reserve", "reserve", "optional", "release", "releaseCore", "reset"]) for _ in range(300)]
            receipts = []
            for operation in operations:
                if operation in ("reserve", "optional"):
                    receipts.append(budget.reserve(operation == "reserve"))
                elif operation == "reset":
                    budget.reset()
                else:
                    budget.release(operation == "release")
            expected = dict(receipts=receipts, gameplayActive=budget.gameplay_active,
                            visualActive=budget.visual_active, retryCount=budget.retry_count,
                            visualDegradeCount=budget.visual_degrade_count,
                            rejectedCount=budget.rejected_count, generation=budget.generation)
            result.append(dict(kind="budget", input=dict(gameplay=gameplay, visual=visual, operations=operations), expected=expected))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixtures-only", action="store_true")
    args = parser.parse_args()
    work = ROOT / "out/agent/survivor-parity"
    work.mkdir(parents=True, exist_ok=True)
    path = work / "python-reference.json"
    cases = fixtures()
    path.write_text(json.dumps(cases, allow_nan=False), encoding="utf-8")
    print(f"Python reference: {len(cases)} cases", flush=True)
    if not args.fixtures_only:
        project = ROOT / "Tools/Scripting/SurvivorParity/SurvivorParity.csproj"
        subprocess.run(["dotnet", "build", str(project), "-c", "Release", "--artifacts-path", str(work / "artifacts"), "-o", str(work / "bin")], cwd=ROOT / "Engine/Managed", check=True)
        subprocess.run(["dotnet", str(work / "bin/SurvivorParity.dll"), str(path)], check=True)


if __name__ == "__main__":
    main()
