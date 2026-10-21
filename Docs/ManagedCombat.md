# Managed combat and action timing

`HEngine.ScriptCore` provides two scene-independent components. They use world
units, seconds and `System.Numerics.Vector3`; neither owns scene handles, physics,
animation clips, UI or a weapon. Any project referencing ScriptCore can use them.

## Perception and attack decisions

Create one `CombatAgent` per enemy with immutable `CombatAgentSettings`. Call
`Step(deltaSeconds, observation)` once per simulation update. Supply the observer's
actual position and facing, the candidate target position/alive state, and a
caller-computed line-of-sight result that includes world collision. The sight cone
is a 3D cone, not just a planar angle. Zero facing cannot acquire a target.

The phases are Patrol, Acquire, Pursue, Windup, Recover, Search and Dead.
Continuous sight is needed during Acquire; interrupted detection restarts its
reaction time. Sight distance has retention hysteresis. Occlusion or leaving the
cone immediately cancels Windup. Search uses the last observed destination until
memory expires; it does not follow the supplied unseen target position.
`Investigate(position)` records a sound/damage location without granting sight.

On entry to Windup, the agent locks `AimPoint`. Animate a warning from
`WindupProgress`, and use that locked point when `AttackReleased` pulses. The
caller must still check weapon availability and the shot path from the actual
muzzle. Do not redirect the shot to the target's latest position: that removes
the opportunity to dodge. A release produces one pulse; a hitch cannot skip a
new warning or emit a catch-up burst. Recovery begins even if the caller's weapon
cannot emit (for example, a full projectile pool).

`HasDestination` gates access to the remembered destination. Move/look toward it
through the project's locomotion and collision system. `Kill()` disables all
decisions until `Reset()`, which also clears memory and locked aim. Its optional
initial recovery supplies spawn grace. Pass `TargetAlive=false` only when the
target is invalid/dead, not merely when it is occluded.

This component handles one candidate target. Multi-target selection, ground
navigation, cover selection and crowd avoidance remain caller responsibilities.

## Shared animation/gameplay clock

`ActionTimeline.Start(duration, commitProgress)` begins an action. `Advance(dt)`
returns one-shot Committed and Completed markers; its `Progress` drives sampled
animation. A large interval may cross both markers in one call. Paused/zero time
emits nothing. Call `Cancel()` before replacing an active action; cancellation
prevents future markers and does not undo an already committed effect.

Choose the normalized commit point from an authored animation/gameplay contract.
The default is the end of the action. This component does not extract events from
clips or replace the engine Animator's event support.

## MechanicGirlLab integration

`Scripts/CSharp/ShooterGame.cs` consumes these APIs for hovering sentries, reload
and equip timing. Sentries react to sight and damage, pursue/search within their
spawn leash, turn at a bounded rate and stop translating during attack warnings.
Their body pulses during Windup and the existing HUD displays an incoming-fire
message. Shot directions use the locked warning point. New waves reset brains,
positions and projectiles, preserving the staggered spawn grace. Combat completion
cancels both action timelines, stops enemy decisions, clears all hostile projectiles
and removes attack warnings. Defeat returns immediately before wave advancement,
so a lethal hit cannot also award a new wave's healing. Restart requests return
before any further old-scene gameplay is processed. Movement follows a Recast route
using a ground anchor plus hover height. Collision ray samples remain a final
body-clearance guard; they are not a swept-volume collision solver.

## Reusable path following

`Navigation.Build(minimum, maximum)` builds the current scene's configured
navigation surfaces inside a world-space box. Static submesh bounds reject distant
geometry and triangle clipping bounds straddling geometry, preserving winding and
area flags. Existing unbounded `Build()` behavior is unchanged. Invalid bounds,
geometry or build budgets return a diagnostic and retain the last usable mesh.
This is a synchronous per-scene bake, not a streamed/tiled navmesh or offline cache.

`NavigationPathFollower` owns one agent's route. `Step(dt, groundPosition, goal,
speed)` returns a displacement limited to the next corner. Apply collision before
updating the agent position; pass that resolved position into the next step. An
unreachable path emits no movement and retries at a bounded interval. Moving
destinations replan after the target tolerance and cooldown. `Invalidate()` stops
a dynamically blocked route without bypassing the retry cooldown. Call `Reset()`
on teleport, death, respawn or scene replacement. Zero time and zero speed are
inert. An injectable query delegate allows reuse with another navigation provider.

MechanicGirlLab configures courtyard floor, stairs and solid architecture/props in
`Config/Navigation.json`. ShooterGame bounds the bake around the player and all
spawn leashes, then creates one follower per enemy. Bake failure is logged and
movement stops; it does not fall back to moving blindly toward the player. The
existing leash and body-clearance guard still constrain each resolved movement.
Dynamic navmesh updates, crowd avoidance and choosing an alternate route entirely
inside a circular leash remain separate work.

Optional `ShooterSettings` fields (defaults preserve older configuration files):

| Field | Default | Meaning |
|---|---:|---|
| EnemySightAngle | 160 | Full sight cone, degrees |
| EnemyReactionSeconds | 0.3 | Continuous acquisition time |
| EnemyWindupSeconds | 0.65 | Attack warning duration |
| EnemyMemorySeconds | 3 | Occluded target memory |
| EnemyMoveSpeed | 1.1 | Movement units per second |
| EnemyLeashRadius | 8 | Horizontal range from spawn |
| EnemyPreferredRange | 8 | Pursuit stopping distance |

Existing EnemyRange controls attack range; acquisition/retention add 4/8 units.
EnemyInterval is recovery time after release, so the warning adds to the shot
interval. ReloadProgress and ammunition transfer now use the same timeline;
transfer remains at the clip end. Switching weapons cancels incomplete reloads
without changing ammunition. Opening the existing pause menu freezes gameplay
updates and therefore the timelines.

## Verification

Pure managed regression checks, without a window or GPU:

```powershell
dotnet run --project Tools/Scripting/CombatTests/CombatTests.csproj --configuration Release --artifacts-path C:/WorkSpace/HEngineDev/out/agent/managed-combat
```

The test references the actual ScriptCore project. It covers occlusion/cone/range,
continuous detection, interrupted warning, last-seen memory and expiry, locked
aim, cooldown/hitches, death/reset/investigation, invalid input, and action
markers/cancellation, corner following without hitch overshoot, goal updates,
failed-query cooldown, collision invalidation and reset. Real game integration evidence is recorded separately in
`Docs/FSRStreamingCombatWork.md`; unit checks do not establish animation quality
or collision correctness in a scene.

`MechanicGirlLab/Tools/Build-CombatLifecycleDiagnostics.py` generates separate
diagnostic scenes from SickaDynasty. One uses lethal projectiles and a long reload
for pause/death/retry checks; the other uses one-hit sentries and zero incoming
damage for repeatable three-wave input validation. Production tuning is unchanged.

`MechanicGirlLab/Tools/Build-CombatNavigationDiagnostics.py` clones the courtyard
with a low solid box across sentry 1's pursuit route. Runtime evidence records its
trajectory around the box, arrival beyond it and active FSR on both graphics APIs.
