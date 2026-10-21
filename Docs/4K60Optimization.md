# MechanicGirlLab 4K60 optimization

## Delivered configuration — 2026-09-29 23:38

For this machine, keep **Performance / 流畅** in the actual saved user settings:
DX12, borderless3840x2160, FSR3.1.4 Performance1920x1080→3840x2160,
sharpness0.2, texture streaming1536MiB, VSyncOn. AO/SSR/SSGI/SSS and local
exposure are off in this preset; deferred lighting, shadows, fog, tone mapping
and other project post-processing remain. The project base remains Balanced
(FSR2258x1270→4K); engine-wide defaults and unrelated projects were not retuned.
No frame generation is used.

Final Game SHA256:
`F9E6E2AA9D1682A30F4C2D95E2B0F9FDE99AB43E8B5A24B47D83F516182E7764`.

| Final realtime run | Mean FPS | Wall P95 / P99 | GPU P95 |
|---|---:|---:|---:|
|180s uncapped combat + exploration|74.48|18.38 /24.66ms|14.65ms|
|60s VSync combat + exploration on60Hz display|59.98|17.02 /18.90ms|14.89ms|

The uncapped30s slices averaged67.46–83.36FPS;9 kills/3 waves completed, zero
loading frames in either measured window. Uncapped9.72% of frames exceeded
16.67ms, maximum74.38ms; VSync maximum40.36ms. This supports generally60FPS in
the exercised scene on this hardware, **not a guarantee that every frame meets
16.67ms or every scene will do so**. Measurements include developer-agent work.
Compared with the previous run, CPU update mean fell6.85→5.12ms, but outfit,
camera trajectory and thermal state differ, so this is not an isolated A/B
measurement of the cache change.

Final48-frame visual sample uses actual Saved preferences and the complete
Performance profile:16 consecutive frames each for static/move/turn. FSR3.1.4,
1920x1080 internal and4K output verified for every image. Reviewed foliage,
roof and hair crops: no obvious broad flashing, broken geometry or persistent
character trails in these samples; Performance remains softer than Balanced.
Image-difference metrics are diagnostic, not an automated flicker verdict.

Reusable engine changes: texture-upload command-list reuse; parented local TRS
and shared-parent presentation caching; correct bounds invalidation after
detach/controller removal. Added owned realtime measurement, preset-persistence
and temporal-capture tools. The mesh-path contract passes, including the new
hierarchy/presentation cases; Game builds pass. No new commit or push.

## Objective and acceptance

User requests a high-quality, stable 4K fullscreen configuration that generally
stays above60FPS on the current machine, with no obvious flicker, delivered as a
Balanced or Performance preset. Keep investigating until gameplay and temporal
quality support that claim; a paused-camera benchmark alone is insufficient.

Hardware: RTX3060 Laptop6GiB, driver591.74, i7-11800H8C/16T.
Baseline engine commit afa10b889, Game SHA256
B79B2F53BDB6097AD8F904E424846359A754944050CD7B9D2310593ADB778E8B.
External project C:/WorkSpace/HEngineProjects/MechanicGirlLab.

Required evidence: real3840x2160 fullscreen output; actual FSR/provider/internal
extent; sustained realtime movement, camera turns, combat/reload and streaming;
frame-time distributions and hardware/memory context; sampled temporal image
sequences for foliage/hair/fine geometry; menu preset matching, persistence and
normal startup. Preserve lighting/material quality where it fits the measured
budget. Record outliers and test coverage rather than claiming universal60FPS.
Do not use frame-generation FPS to substitute for the requested rendering budget.

## References

- [AMD FSR3.1.4 integration](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/):
  temporal inputs, reactive masks and pipeline placement must remain correct as
  quality changes. Existing integration is retained while measuring.
- [Epic TSR documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine):
  lower internal resolution reconstructs4K, while post-upscale passes still have
  output-resolution cost. Measure pass costs rather than copying an unrelated
  game's quality labels.

## Initial measurements — 2026-09-29

New reusable driver `Tools/Agent/Measure-RenderPreset.ps1` applies explicit cases
inside an owned session and records timed windows, captures and effective settings.
`Tools/Agent/analyze_render_preset.py` joins native CSV timings by UTC window and
deduplicates GPU samples. Existing Session.ps1 gained optional `-Realtime`, which
omits the fixed simulation delta for gameplay performance; fixed-delta validation
remains the default. No game preset/user configuration has been changed yet.

First invocation failed before starting because project-display and explicit
resolution arguments conflicted. Driver now selects exactly one display route.
Second invocation completed, owned PID105744 stopped:

```powershell
$hash=(Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash
powershell -ExecutionPolicy Bypass -File Tools/Agent/Measure-RenderPreset.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/baseline-matrix-2 -CasesPath out/agent/4k60/initial-cases.json -UseProjectDisplaySettings
& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_render_preset.py out/agent/4k60/baseline-matrix-2
```

Runtime measurement and analysis completed, not final acceptance. Paused gameplay
after30 simulation steps, project fullscreen3840x2160, VSync off, no debug layer,
8s settling plus12s timed windows/case. Base scene SickaDynasty, original balanced
AO/SSS settings and512MiB streaming budget. Output/UI at4K in all cases.

| Case | Internal | Mean FPS | Wall P95 / P99 ms | GPU median ms |
|---|---|---:|---:|---:|
| Native TAA |3840x2160|33.89|30.70 /31.41|29.38|
| FSR Quality |2560x1440|46.25|46.78 /158.54|14.81|
| FSR Balanced |2258x1270|73.79|14.35 /14.77|13.42|
| FSR Performance |1920x1080|81.21|13.62 /17.56|11.98|

Native TAA GPU pass mean7.86ms. Balanced FSR3.41ms, GBuffer2.37ms,
directional shadow1.64ms, deferred lighting1.36ms, depth prepass1.09ms.
Native dedicated peak5363MiB exceeds reported5226MiB local budget. Quality's
recorded dedicated usage was3543MiB, but it had a387ms GPU/CPU-wait spike.
The cause is not established: do not remove this outlier or infer memory causality
from the preceding native case alone. Balanced is only a provisional candidate.
Reviewed Balanced1280px overview: scene, player and native HUD intact; not a
temporal quality assessment.

Next measurement runs real-time gameplay in Balanced, Performance, then Quality
to remove the preceding native4K workload and include animation/combat costs.
Remaining: hardware telemetry/repeatability, representative longer route,
temporal QA, final preset/user configuration, persistence and final acceptance.

## CPU bottleneck and upload reuse — 22:14

`gameplay-matrix` completed with realtime simulation, three30s windows. Balanced /
Performance / Quality meanFPS59.54 /62.55 /61.42; wallP9525.88 /24.44 /24.15ms.
GPU means12.54 /10.78 /14.43ms: reducing raster resolution alone does not remove
CPU spikes. Frame preparation P95 was11–12ms; resource-pump time stayed small
because texture upload submission occurs inside TextureManager::Collect instead.
Final Performance/Quality screenshots/logs showed late defeat; these are
exploratory measurements, not sustained active-combat acceptance. The driver now
samples healthbar width and uses ordinary F5 retry below35%, records the retries
and includes their costs. Future measurements must use that corrected driver.

Enabled HE_FRAME_PREPARATION_TIMINGS=1 and HE_RUNTIME_SYSTEM_TIMINGS=1 for one
additional Balanced30s run (`cpu-profile`, owned session cleanly stopped). Sampled
slow preparation total20.429ms included17.258ms in textureCollect, with objects
3.109ms. The owned NVIDIA sampler wrote `profile-gpu.csv`, then was stopped in
finally. The final incomplete CSV line must be ignored; no CPU frequency trace
was taken. P0 graphics clock during warm gameplay was around1.8GHz, GPU77–81C.

Unchanged engine with increased texture budgets (`budget-matrix`):
1024MiB mean68.36FPS/P9521.63ms;1536MiB mean73.04FPS/P9517.49ms.
Whole-process dedicated memory peak4130MiB, reported local budget5226MiB.
Larger working set improves quality and reduces churn, but does not yet prove
stable60FPS. Both exploratory phases used the earlier retry-free driver.

Implementation: TextureManager now retains one NVRHI upload command list across
asynchronous DDS/image uploads, allowing fence-completed native command allocators
and staging chunks to be reused. Resource/event-query publication and retirement
remain unchanged. Any preparation/submission exception clears the reusable list,
so an abandoned recording is not reopened. Synchronous initial-load helpers keep
their existing path. No ThirdParty edits.

Initial build failed because NVRHI RefCountPtr overloads operator&; corrected
the cache argument to std::addressof. **build-only PASS**,34.6s:

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab
```

Game SHA062BFDECFB9E4F54C1CB0455BCC187F299EB4DE389C66803B0F487F6D7F23DDC.
**build-only + runtime-manual PASS**:

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineFsrSmoke -Configuration RelWithDebInfo -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -Run -Wait -TimeoutSeconds 180
```

Owned PID75744 exited0. Log
`out/agent/projects/7e44ffe2e91e72bd/logs/run-game-relwithdebinfo-20260929-221316-511-90260.stdout.log`.
Both APIs passed streaming demotion/recovery of89MB texture, corrupt/missing
reload retries, cancellation and16 injected GPU-boundary failures/API. FSR
dispatch/readback/reset/resize passed at1080p/1440p/2160p. Not a driver-OOM claim.
New corrected-driver comparison512/1536MiB is `upload-reuse-matrix`; pending
analysis at this checkpoint. Final preset still not written; goal remains active.

## Candidate configuration and combat route — 22:33

`upload-reuse-matrix` active-frame results: 512MiB mean70.54FPS/P9518.21ms;
1536MiB mean73.85FPS/P9516.23ms. Each phase contained an F5 retry; analyzer
now separates loading frames (714/722) and checks full timestamp coverage.
These results are exploratory, not uninterrupted-combat acceptance.

Added optional `-AimCombat` to the measurement driver. It aims at real sentries,
fires and reloads through normal input, fails on low health, and never retries
inside the timed window. `aim-combat` completed three waves/nine kills/victory,
health100, zero loading frames;2196 frames across30.33s. Mean72.41FPS,
P9516.35ms/P9919.18ms/max43.25ms;4.10% above16.67ms. Internal2258x1270,
output3840x2160. This is one short combat run, not a thermal soak.

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/Measure-RenderPreset.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/aim-combat -CasesPath out/agent/4k60/combat-case.json -UseProjectDisplaySettings -Gameplay -AimCombat -MeasureSeconds 30
```

Project candidate presets now include FSR and1536MiB streaming: Balanced uses
Balanced upscaling with existing GTAO/SSS/local exposure; Performance uses
Performance upscaling and its existing lighter effects; High explicitly selects
Quality upscaling. Sharpness0.2. BaseEngine and this machine's UserEngine use
Balanced, RenderHeight0. VSync remains the user's/project's setting. The texture
budget menu adds1536/2048MiB. Original external-project files are preserved in
`Saved/Recovery/4K60-20260929`. Engine-wide defaults were not changed.

`Test-Fsr4KQuality.ps1` captures atomic16-frame static/move/turn sequences.
First run `temporal-4k` captured Balanced static/move but failed writing the turn
sequence due to disk space; owned PID78596 stopped. **Runtime capture FAIL**,
not a rendering acceptance pass. Provider metadata confirms active3.1.4,
2258x1270→3840x2160, no fallback in captured frames. Reviewed static crop strips:
roof/foliage stable at sampled frames; animated hair changes pose. Static ROI
mean adjacent RGB delta (0..255) foliage0.806, roof0.904; these metrics alone
cannot establish absence of motion flicker. Motion/turn and Performance pending.

Disk: lossless NTFS compression of current measurement evidence saved~0.57GB.
Full `clean stale` was stopped after several minutes without a result; do not
claim deletion success. An out inventory found no reparse points; protected
session/baseline evidence remains intact. Compressed idle current Engine object
and bin directories (no engine/build processes), preserving file contents:
Engine3.284GB→1.178GB, bin2.460GB→1.023GB. C free rose from~0.17GB to3.77GB.
The capture driver now applies stronger lossless compression after each sequence
and tests one quality per invocation to bound disk usage. No evidence deleted.

Remaining: finish temporal runs, warm sustained gameplay/exploration, default
startup/UI persistence and VSync60Hz presentation verification. Goal active.

## Visual and menu validation — 22:43

Candidate Game rebuild **build-only PASS**98.3s using the same `hectl game build`
command above; SHA365A0F8312F5DB1C6AB8C527E6393FAEBDA2FFB895F6A26DD77BC9556CE69686.

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-Fsr4KQuality.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/temporal-balanced -Quality Balanced
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-Fsr4KQuality.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/temporal-performance -Quality Performance
& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_fsr4k_quality.py out/agent/4k60/temporal-balanced
& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_fsr4k_quality.py out/agent/4k60/temporal-performance
```

**runtime-manual capture PASS + static-check integrity PASS** for both qualities:
16 consecutive frames each for static/move/turn, same scene and effects, native4K
output. Balanced2258x1270; Performance1920x1080. Both use actual FSR3.1.4 with no
fallback. Owned PID89440/114320 stopped. Reviewed Balanced static/move/turn and
Performance turn crop strips: no obvious whole-surface flashing, broken geometry
or persistent character trails in sampled frames. Performance has softer fine
detail; retain Balanced as the default candidate. ROI deltas include animation
and camera motion and are deliberately not labeled automated flicker tests.
These short sequences do not establish every camera angle/outfit is artifact-free.

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-4KRenderPresets.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/preset-ui-2
```

**runtime-manual PASS**: native4K borderless project startup, Balanced default,
virtual mouse selection of Performance then Balanced and1536MiB texture menu,
persisted settings, full process restart using saved preferences, correct FSR
internal/output extents after restart, VSync remains on, empty stderr. Reviewed
default menu preview: correct localized profile, activeFSR,2258x1270→3840x2160,
1536MiB and VSyncOn labels. First `preset-ui` attempt stopped its game but failed
the script's shutdown assertion because Select-String positional arguments were
reversed; corrected to explicit -LiteralPath and reran the complete test. No
engine failure was inferred from that harness error.

PowerShell AST parsing of the three new scripts and Python `py_compile` of the
two analyzers **static-check PASS**. `balanced-soak` is currently running180s of
realtime combat then walking/turning, with owned NVIDIA telemetry. Normal VSync
gameplay measurement remains pending; do not complete the goal at this checkpoint.

## Long-run results and CPU follow-up — 23:01

Completed both180s realtime routes: three waves/nine kills, then continued
walking/turning. Full CSV time coverage verified, no loading frames excluded.
Both owned processes stopped. Uncapped **runtime-manual measurements**:

| Preset | Mean FPS | Wall P95/P99 ms | >16.67ms | GPU P95/max ms |
|---|---:|---:|---:|---:|
| Balanced (`balanced-soak`) |64.49|18.75/25.94|17.83%|17.32/18.57|
| Performance (`performance-soak`) |70.38|18.12/21.44|13.09%|13.90/14.81|

Balanced30s-slice means62.21,60.35,67.09,66.67,60.17,70.38FPS. Performance
74.02,62.04,76.01,68.35,66.73,75.12FPS. Balanced telemetry rose to85C with clocks
around1.67–1.85GHz; Performance spot measurements82–84C/~1.8GHz. These results
do not establish stable60FPS in all heavy views. Balanced60s VSync run averaged
58.89FPS, P9519.25ms, GPU P9517.25ms. No pass label for final stability.

The largest Balanced wall spike284.37ms contained265.12ms OnPreUpdate with only
15.625ms measured thread CPU time. This phase includes developer-agent mailbox
work, so do not attribute it to GPU rendering or texture uploads. Keep the spike
in reported results. Performance maximum40.86ms; real ordinary CPU slow frames
also remain (344 sampled18–25ms frames had preUpdate<0.5ms).

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/Measure-RenderPreset.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/balanced-soak -CasesPath out/agent/4k60/soak-case.json -UseProjectDisplaySettings -Gameplay -AimCombat -ExploreAfterCombat -MeasureSeconds 180
powershell -ExecutionPolicy Bypass -File Tools/Agent/Measure-RenderPreset.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/performance-soak -CasesPath out/agent/4k60/performance-soak-case.json -UseProjectDisplaySettings -Gameplay -AimCombat -ExploreAfterCombat -MeasureSeconds 180
```

Additional60s `cpu-systems-profile` uses Performance and
`HE_RUNTIME_SYSTEM_TIMINGS=2` (restored afterward), same driver with
`-MeasureSeconds 60`.65 active fixed-tick diagnostic samples: median system
update7.1885ms, RenderSystem3D5.1156ms. Source audit points to render extraction
as the next bounded optimization target. In particular, extraction traverses
character-presentation ancestors for every mesh and recomputes local TRS for
parented static meshes. Consider per-frame shared-parent presentation caching
and correctly invalidated local-transform caching, with hierarchy/interpolation
tests; **neither optimization has been implemented**. Do not assume the source
audit proves their individual cost; confirm with measurement.

This machine's `Saved/Config/UserEngine.json` now selects the complete
Performance/流畅 profile (FSRPerformance,1536MiB, existing cheaper effects), as
the better current candidate. Project BaseEngine remains Balanced. This is a
provisional default while CPU tails are improved, not final goal completion.

Found an evidence limitation: Session starts with isolated empty Saved by
default. Earlier UI tests established project defaults and persistence within
the isolated run, not the real user's Saved overrides. New UI test explicitly
copies project Saved/Config for initial startup; measurement driver now does the
same. `default-performance-ui` failed because of missing Saved copy; corrected.
`default-performance-ui-2` then exposed a harness click after the texture menu
had scrolled the quality row outside its clipped viewport. Return through main
menu before selecting quality again. `default-performance-ui-3` **runtime-manual
PASS** for actual saved Performance default, mouse switches,1536MiB option,
return to Performance, persisted restart,4K output/1080p internal and VSyncOn.
Both owned processes stopped. Command is the UI command above plus
`-DefaultQuality Performance` and the new output directory.

Disk remains tight. Stronger lossless LZX compression of idle current runtime
library/PDB/ILK preserves content and stores2.020GB logical in338MB; C free was
about1.58GB before the final UI run. Do not delete protected/unknown evidence or
retry the previously rejected SDK ZIP deletion. No new commits/pushes.

Current next gate: `performance-vsync`60s is running using actual saved settings;
then continue CPU optimization, build and targeted hierarchy/interpolation
validation, repeat representative performance/visual checks. Goal remains active.

### Final checkpoint of this iteration — 23:03

`performance-vsync` completed with real Saved preferences, owned PID118792
stopped, actual1920x1080→3840x2160. **runtime-manual measurement completed**:
mean59.73FPS on60Hz display, wallP9518.09ms/P9921.52ms/max41.20ms;
GPU P9514.04ms/max14.84ms. Performance has sufficient GPU headroom; CPU tails
remain. Do not call this a strict all-frame60FPS pass.

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/Measure-RenderPreset.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/performance-vsync -CasesPath out/agent/4k60/performance-vsync-case.json -UseProjectDisplaySettings -RespectPresentationSettings -Gameplay -AimCombat -ExploreAfterCombat -MeasureSeconds 60
& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_render_preset.py out/agent/4k60/performance-vsync
```

Reviewed `default-performance-ui-3/restart-preview.jpg`: 流畅, FSR性能,
1920x1080→3840x2160 active,1536MiB,VSyncOn. Current user setting is Performance;
project base is Balanced. Final-process inventory found no owned Game or compact
processes. C free914,808,832 bytes; storage space must be addressed before a new
large build or capture sequence. `git diff --check` and updated UI script AST
parse PASS. No new codegen declarations or headless unit tests in this iteration.

Next: reduce RenderSystem3D/extraction CPU cost without changing appearance.
Per-frame shared-parent presentation caching is a concrete candidate; current
GetCharacterPresentationOffset walks ancestors independently for every mesh,
and Level::GetParent parses ParentUuid before lookup. Parented local TRS is also
recomputed each frame. Check parent→root reparenting/invalidation when touching
the cached transform path. Existing `RenderWorldExtractorMeshPath` contract has
character interpolation/attached visual/teleport coverage and should be extended
and run for any such changes. Re-run final temporal checks with actual Saved
preferences after engine changes (earlier temporal sequences used project-base
preferences). Goal remains active, not complete or blocked.

### Extraction cache implementation — 23:28

Implemented frame-local shared-parent presentation-offset caching and local TRS
matrix caching for parented meshes. World-matrix comparison handles parent-to-root
detach with unchanged local TRS; removing a presentation offset invalidates cached
bounds. Extended `RenderWorldExtractorMeshPath` with sibling inheritance, child
controller precedence, detach/reattach, parent movement and controller removal.
`HE_RUNTIME_SYSTEM_TIMINGS=2` now samples extraction phases every60 view frames.
No persistent/reflection fields changed. Performance impact is not yet established.

`build-only` PASS (139.5s, then37.3s incremental after the diagnostic edit):

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab
```

Current Game SHA256:
`F9E6E2AA9D1682A30F4C2D95E2B0F9FDE99AB43E8B5A24B47D83F516182E7764`.
Temporal-quality driver now copies actual project Saved preferences and applies
the complete selected quality profile, including its effects and streaming budget.

Cold headless test build failed on C drive exhaustion (C1088/C1085, then linker
in-page error C000007F). After space recovery the compilation passed but test
linking failed with corrupt `HEngineTests.pdb` (LNK1285). Automatic approval review
rejected deletion of that generated PDB/ILK; neither was deleted. A safer local
CMake configuration uses a fresh `recovery-symbols` PDB output directory, preserving
the damaged file. The same hectl test is being retried; no unit-test PASS yet.

Lossless LZX compression preserved historical captures: top-level captures
8.959GB logical→1.620GB stored; ended102304 session1.593→0.590GB;
ended103923 session1.593→0.583GB. Recompressed idle Game runtime library/PDB/ILK
1.975→0.337GB after rebuilding. C free reached0.411GB during the failed build,
then2.475GB before relinking. Concurrent build writes mean the free-space delta
is not an isolated compression savings measurement. No source, protected evidence
or SDK ZIP was deleted; ordinary automatic cleanup remains disabled.

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 test RenderWorldExtractorMeshPath -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -Configuration RelWithDebInfo
```

The fresh-symbol link subsequently passed (44.5s), then the contract failed in
an existing sky serialization assertion. The test translation unit omitted
`EnvironmentLightComponent.gen.h`, so its explicit generated serialization
specialization was not selected. Added that include and reran the command:
**build-only PASS16.4s; unit-test RenderWorldExtractorMeshPath PASS** at23:28.
The invalid camera-override warning is an exercised rejection case, not a test
failure. No generated files were hand edited. The Game artifact is unchanged
by this test-only include. New180s uncapped performance run is now in
`out/agent/4k60/extractor-performance-soak`, with actual Saved preferences.

### Final validation — 23:38

The final runtime measurements and visual review are complete; see the delivered
configuration at the start of this document for results and limits. Commands:

```powershell
$hash=(Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash
powershell -ExecutionPolicy Bypass -File Tools/Agent/Measure-RenderPreset.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/extractor-performance-soak -CasesPath out/agent/4k60/performance-soak-case.json -UseProjectDisplaySettings -Gameplay -AimCombat -ExploreAfterCombat -MeasureSeconds 180
& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_render_preset.py out/agent/4k60/extractor-performance-soak
powershell -ExecutionPolicy Bypass -File Tools/Agent/Measure-RenderPreset.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/extractor-performance-vsync -CasesPath out/agent/4k60/performance-vsync-case.json -UseProjectDisplaySettings -RespectPresentationSettings -Gameplay -AimCombat -ExploreAfterCombat -MeasureSeconds 60
& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_render_preset.py out/agent/4k60/extractor-performance-vsync
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-Fsr4KQuality.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -OutputDirectory out/agent/4k60/final-temporal-performance -Quality Performance
& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_fsr4k_quality.py out/agent/4k60/final-temporal-performance
git diff --check
```

- **runtime-manual PASS within the stated scope:** timed runs completed with
  full CSV coverage, no loading frames, FSR active and owned shutdown verified.
  Mean74.48FPS uncapped and59.98FPS with VSync. Occasional slow frames remain;
  this is not a strict all-frame60FPS pass.
- **runtime-manual visual review PASS for the48 sampled frames:** sequence
  continuity/provider/output checks passed; all three1200px crop sheets reviewed
  sequentially. Empty stderr, no matched validation/script/FSR failure messages.
  Static foliage/roof mean adjacent RGB differences0.688/0.742 on0..255 scale;
  animated hair3.332. No automatic flicker threshold or universal claim.
- **unit-test PASS:** `RenderWorldExtractorMeshPath`, exact command above.
- **build-only PASS:** Game and headless Tests, exact commands above. No Editor
  build, Shipping packaging or new Vulkan runtime run for the extraction change.
  Earlier dual-backend upload/FSR smoke evidence applies to upload-list reuse.
- **static-check PASS:** `git diff --check`; PowerShell
  `[System.Management.Automation.Language.Parser]::ParseFile` for Session,
  Measure-RenderPreset, Test-4KRenderPresets and Test-Fsr4KQuality; Python
  `ast.parse` for both analyzer source files.

All owned Game, Tests, compiler/linker and compression processes had exited at
the final inventory. C free3,505,541,120 bytes. No files were deleted in this
iteration; rejected cleanup was replaced by fresh diagnostic-symbol output.
Performance is selected in actual project Saved settings; project base Balanced
and global engine defaults remain separate. New changes remain uncommitted.
