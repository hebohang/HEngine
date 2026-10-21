# Gameplay animation transition validation

`Tools/Agent/Test-CombatAnimation.ps1` operates the actual MechanicGirlLab
SickaDynasty scene through owned Agent input. It does not replace the player
controller, remove enemies or inject animation parameters. The original scene,
scripts, weapons, physics, attachments and animation controller run normally.

Build the external Game, then pin its executable for each backend:

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab
$hash = (Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-CombatAnimation.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi DX12 -OutputDirectory out/agent/captures/combat-animation-DX12
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-CombatAnimation.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi Vulkan -OutputDirectory out/agent/captures/combat-animation-Vulkan
```

Each output directory must be new. The test uses 1280x720 output, FSR Quality,
GI Off and render validation. It samples 28 points across idle, ADS entry,
aimed strafe/forward movement, sprint entry/exit, stopping, moving fire/reload,
switch-cancelled reload, completed reload, holster and draw. F5 restarts the
actual game between locomotion and action sequences to keep the observation
short enough to avoid a combat defeat; enemies are still active. A defeat,
missing required action log, invalid FSR state or failed shutdown fails the run.

`render.animation.status` now includes bounded numeric/bool controller parameters,
current/target state IDs, state name, transition time and progress. These describe
the current CPU AnimatorComponent; the adjacent palette diagnostics describe the
last successfully submitted render world, and can differ by one frame. Parameter
count is reported; at most 64 parameters are exported, with bounded names. These
fields are development diagnostics, not persisted gameplay resources.

Limb diagnostics follow the solver's target precedence: TargetBone, then
TargetUUID, then WorldTarget. They report the configured weight and effective
weight after its controller parameter. Bone-relative targets use the same submitted
palette/world as the measured effector. Entity targets still use their current
world transforms. This corrects the former diagnostic error that treated a
support-hand TargetBone as the default world origin; it does not change the IK
solver or authored attachment offsets.

Run `Tools/Agent/analyze_combat_animation.py <output-directory> ...` with Python
and Pillow. It verifies settled states, finite poses/parameters, FSR/TAA selection
and render extents, and reports support-hand error for samples with effective
weight at least 0.95. The diagnostic acceptance bound is 5 cm to the configured
target; this is not proof that every finger contacts the rendered weapon mesh.
It emits four compact pose sheets and analysis.json for manual review.

Simulation is paused between stepped input samples and screenshots. Therefore
these checks establish controller progression and sampled poses, not continuous
temporal reconstruction, ghosting, event ordering at every possible frame rate,
or collision-free traversal. FSR deformation/motion tests and combat lifecycle
tests provide separate evidence. The actual ECS controller deliberately freezes
the outgoing pose during a crossfade; the independent AnimationStateMachine has
a moving outgoing clip and separately tests its action-layer preservation.

See `FSRStreamingCombatWork.md` for executed runs, failures, visual observations
and outstanding scope. A generated contact sheet alone is not visual acceptance.
