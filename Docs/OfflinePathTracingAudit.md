# Progressive path tracing audit and validation ledger

Date: 2026-09-09. This is an implementation/validation ledger, not a claim that
Cycles-like acceptance has been achieved. Unchecked items remain required work.

PBRTest follow-up implementation and current support: [PBRTestPathTracing.md](PBRTestPathTracing.md).

## Machine and baseline

Windows x64; Intel i7-11800H (8 cores / 16 logical processors); NVIDIA RTX 3060
Laptop, 6144 MiB, driver 591.74; VS2026 v145, MSVC 14.51.36231. An Intel integrated
adapter is also present. No vendor power-mode or thermal-stability claim.

The original persisted JSON really contained SamplesPerFrame=46, RenderScale=1,
MaxBounces=8 and TargetSamples=256. The original tracked tree was clean; existing
untracked benchmark/imported assets were preserved.

`hectl editor check -Frames 120 -RenderStats -RenderStatsPath C:\WorkSpace\HEngineDev\out\agent\pt-baseline-editor.csv -TimeoutSeconds 180`
passed at 1920x1080, **in the default raster mode**. Its 90 measured CPU rows had
wall-frame P50/P95/P99 = 6.545/7.754/17.383 ms, frame-slot wait =
0.0008/0.0017/0.0027 ms and Present call = 0.125/0.207/0.374 ms. The delayed GPU
IDs must be deduplicated for a GPU distribution. These are not a PT baseline,
camera-response measurements, or evidence of improved PT performance. The
original binary was not measured in PT mode before modifications.

## Source diagnosis

All observations in this table are source-confirmed. Performance consequences
are hypotheses unless separately backed by a runtime artifact below.

| Location / function | Observation and consequence | Change / validation |
|---|---|---|
| Session.cpp / ResolveReferenceOfflineDispatchCount, AddPasses | Persisted batch 46 drove full-frame DispatchRays loops and per-AOV UAV barriers; long shared-queue batches could propagate through frame-slot waits | One in-flight interleaved row band, delayed timing, conservative fallback; scheduler unit tests and GPU smoke |
| Session.cpp / PrepareFrame; SceneRenderer / GI configuration | Generic GI denoiser must be None; preview shader is color conversion, not a denoiser | Separate plugin-owned OIDN CPU chain; raw output remains independent |
| Session.cpp / fingerprint, controls, outputs retirement | Camera/integral fingerprint, control state and submission retirement already existed | Reuse these; generation-tagged row ledger and denoise snapshots |
| Reference shader / main path loop | Metallic removed diffuse with no corresponding specular; roughness unused | Shared GGX/diffuse Evaluate/Sample/Pdf; energy/PDF imaging regressions still required |
| HardwareRayTracing / ClassifyRayTracingGeometry | Flat-normal scan excludes smooth, textured, alpha and skinned content despite hit-shader interpolation | PBRTest follow-up adds opt-in smooth/texture/alpha-mask admission and bindings; default realtime admission unchanged |
| Reference shader / lighting | Environment only on miss; DirectOnly missed direct environment. Emissive CDF and hit PDF searched linearly | Uniform hemisphere environment NEE/MIS; binary search for both CDF and identity-sorted emissive hit PDF. PBRTest follow-up adds directional/point/spot lights; luminance environment distribution remains unfinished |
| Reference shader / outputs | Full-frame sampleIndex and last jittered primary-hit guides | A row sweep visits each pixel exactly once; min/average/max counts. Albedo/normal now pixel-filtered running averages, used as noisy guides |
| Session / NotifyFrameSubmitted and control state | Submission advanced completed progress; completed-to-rendering target changes could leave a cached graph without a trace pass | Separate reservation/submission/completion; aborted reservations do not advance; all state transitions dirty the graph; actual GPU pixel-count and asynchronous export regressions |
| RuntimeRenderer / activate/deactivate | Both forced device idle; scene then destroyed GI/RT resources | Retire complete illumination ownership group at last submission; poll external work before release. Shutdown/resize waits are still present |
| EditorLayer; SceneRenderer / PrepareSceneSurfaceFrame | Offline mode forced ECS extraction; identical snapshots rehashed meshes | Remove offline-only extraction trigger; cache RT revisions and fingerprint for identical immutable FrameNumber/SceneGeneration. New camera snapshots still scan mesh contents |
| SceneRenderer / BuildGraph | Raster lighting/shadows/AO could execute before PT replacement; reflections could execute afterward | Actual DX12 graph excludes shadow, deferred lighting, AO and SSR. Retain depth/editor outline, display postprocess and SceneLightUpload constants needed by shared bindings |
| ViewportRendererExtension / registration Release | Release only identified provider by ID | Independent registration token; same-ID replacement regression |
| Plugin entry / support query | DXIL-only checks, repeated filesystem lookups; static registration and draining already implemented | Active-backend artifact directory, one-second bounded cache; preserve static plugin architecture. This is not DLL hot unload |

## Current data flow and ownership

Render-thread-only session and registry calls -> immutable scene snapshot ->
Graphics queue AS/upload + bounded PT row band -> raw linear running means and
per-pixel sample counts -> display composition. The next row reservation is
blocked while the previous PT submission is in flight. Reset increments the
generation and discards its CPU reservation, but keeps an old submitted band
alive until its real completion. No preemption or concurrent GPU queue is claimed.

The row permutation uses a stride coprime to image height, spreading updates over
the view. The pixel RNG depends on pixel, sample and seed, independent of the
order in which bands execute. A sweep boundary is a uniform completed sample.
Between boundaries, completed min/average/max spp differ; floor(spp) is not
presented as an all-pixel average. Dynamic resolution creates a new pixel grid
and a new generation; different grids are never averaged together.

Navigation uses a low-resolution complete-view preview sized to fit the estimated
batch budget. Without timing it starts with <=4096 pixels. After the configurable
200 ms settle period it restores the target grid. A low-resolution preview is
not a reference image. Both policies share the same shader and bounce settings.

After a completed sweep and the configured interval, one frame records three
immutable staging copies. Mapping is attempted only after that graphics fence.
Tightly packed RGBA32 rows are copied on the render thread; the worker never
accesses ECS, ImGui, NVRHI or shader managers. The OIDN RT filter consumes linear
HDR RGB, pixel-filtered albedo and world normals with cleanAux=false. One CPU
task (four threads, no affinity) is allowed; requests coalesce to the newest
eligible sweep. The device/filter persist across jobs. Result upload targets a
separate RGBA32 texture and publication waits for the upload fence and generation
check. No filtered value is fed back into raw radiance.

Extra denoise snapshots are limited to 4 megapixels; higher resolutions retain
raw tracing and explain the limit. Current resident image cost is approximately
112 bytes/pixel before staging, CPU snapshot and driver allocations. During
denoising, staging contributes 48 bytes/pixel and CPU inputs/output up to 64
bytes/pixel. Retirement, AS, raster/editor attachments and library memory must be
added. This is not yet a complete adaptive VRAM allocator.

Normal mode changes cancel new work and move the old scene/technique/guide
ownership into a retirement group. Both the last graphics submission and external
CPU/export work must finish before destruction. Service revocation prevents new
lookups; it does not revoke shared objects already handed out. Existing leases
remain alive while draining. Device shutdown may still wait.

## Configuration and dependency

v2 adds Interactive, TargetFramesPerSecond (60), WorkBudgetMilliseconds (4),
InteractiveScale (0.25), SettleMilliseconds (200), Denoise and
DenoiseIntervalMilliseconds (500). v1 fields are retained, including the user's
46 batch preference; no configuration file is overwritten merely to change a
default. All viewport work is now bounded by the scheduler, so the legacy batch
value cannot cause 46 full-frame dispatches. The panel shows Saved Legacy Batch
as read-only compatibility data. Both Interactive and Reference obey the budget;
Reference keeps its fixed pixel grid and deterministic seed.

The checked-in settings file was migrated to v2 by the normal editor settings
path; all original field values, including seed, SPF=46 and raw preview choice,
remain unchanged. Raw preview stays selected for this existing configuration;
select Denoised Beauty in the new top-level Preview control to display OIDN.

Target/preview/output/denoise controls do not change the integral fingerprint.
Actual resolution, camera, seed, environment and bounce changes do. Explicit Apply
persists configuration; sliders do not synchronously save every interaction.

OIDN 2.5.1 Windows x64 official archive, SHA-256
`f11f91bc072a5e3a564515724cb72ab8fcfbc445c84a84c197ff9b16cc01396f`.
CMake fetches and verifies the archive only when this developer plugin is built
and HE_OFFLINE_OIDN=ON. The archive cache is under out/agent/dependencies/archives.
Enabled hosts receive declared DLLs and license notices. HE_OFFLINE_OIDN=OFF
builds the raw-only backend with a visible reason. Game/Shipping host isolation
remains governed by the existing DeveloperOnly/Never plugin manifest.

## Evidence ledger

- `unit-test`: `hectl test OfflineReferenceRendererSession` — PASS, including
  actual CPU OIDN execution, reduced constant-image error, raw-input invariance,
  bounded work, Abort, partial-count normalization and generation rejection.
- `runtime-manual`: baseline editor command above — PASS, raster only.
- `unit-test`: `hectl test ViewportRendererExtension` — PASS, including old
  registration token release after same-ID replacement and existing lease draining.
- `unit-test`: `.\hectl.bat test HardwareRayTracing` — PASS, existing shared
  capability, geometry cache, history, emissive distribution and composition contracts.
- `runtime-manual`: `.\hectl.bat editor check -Frames 240 -Resolution 1280x720 -StartupScene Assets/CornellBox/CornellBox.he -StartupViewportMode he.viewport.path-tracing -TimeoutSeconds 180`
  — final build/run PASS, `out/agent/pt-editor-final.log`, stderr empty. Latest
  `.\hectl.bat test OfflineReferenceRendererSession` also passes in
  `out/agent/pt-test-final.log` after the export-generation change.
- `unit-test`: `cmake -P CMake/Tests/HEnginePlugins/RunContract.cmake` — PASS,
  manifest/host isolation fixtures. This is not a full shipping or missing-DLL run.
- `runtime-manual`: `$env:HE_OFFLINE_PROGRESSIVE_SMOKE='1'; try { powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Run -TimeoutSeconds 360 } finally { Remove-Item Env:HE_OFFLINE_PROGRESSIVE_SMOKE }`
  — PASS. It contains automatic GPU assertions, but is reported as runtime-manual
  under the repository's build/run route. Latest expanded run log:
  `out/agent/pt-final-smoke.log`; underlying stderr
  `out/agent/logs/run-game-relwithdebinfo-20260909-221509-365-60640.stderr.log`
  is empty. Covers 1/4/16/64/256 raw and OIDN stages, 24 camera snapshots,
  settle restoration, display-only reset isolation, three departures while
  OIDN/export work is active (including provider revocation), final lease draining,
  actual GPU pixel counts at every stage, obsolete export rejection, and raw metallic/environment reflection
  regressions. Export pumping uses real frame-slot completions; device idle occurs
  only at final test shutdown.
- `runtime-manual`: `hectl editor capture -Frames 1200 -Resolution 1280x720 -StartupScene Assets/CornellBox/CornellBox.he -StartupViewportMode he.viewport.path-tracing -CapturePath C:\WorkSpace\HEngineDev\out\agent\captures\pt-editor-linear.pfm -TimeoutSeconds 180`
  — PASS, DX12 editor PT. Linear capture is finite, non-black, mean RGB 0.04239,
  maximum 18.6719; sidecar verifies editor camera and exposure multiplier 1.
  Earlier PNG was heavily obscured by the floating settings panel. The compact
  panel controls were built and run; its revised visual layout has not been
  separately image-inspected.
- `runtime-manual`: `hectl game check -Frames 120 -TimeoutSeconds 180` — PASS,
  shared DX12 Game path, `out/agent/pt-game-check.log`. Game binary directory
  does not receive the plugin's OIDN DLLs.
- `runtime-manual`: `hectl editor capture -Frames 600 -Resolution 1280x720 -StartupScene Assets/CornellBox/CornellBox.he -StartupViewportMode he.viewport.path-tracing -GraphicsApi Vulkan -StrictGraphicsApi -RenderStats -RenderStatsPath C:\WorkSpace\HEngineDev\out\agent\pt-editor-vulkan.csv -CapturePath C:\WorkSpace\HEngineDev\out\agent\captures\pt-editor-vulkan.pfm -TimeoutSeconds 180`
  — capability fallback PASS; **PT acceptance NOT RUN**. Actual Vulkan/NVRHI
  profile is Baseline Raster with AS/pipeline/ray-query disabled. Mode selection
  explicitly reports required capabilities unavailable; process success must
  not be mistaken for PT success. No Vulkan PT/denoise quality claim.

Earlier diagnostic failures were retained in run logs: the original broad raster
smoke expected a legacy 21-pass Forward graph but this local configuration uses
Deferred; the dedicated PT test now runs through its own explicit switch. An
initial offline graph removed SceneLightUpload too aggressively, causing NVRHI
read-before-write errors; retaining its shared constant-buffer upload fixed the
validation errors. The first synthetic trajectory accidentally began at the
unchanged camera and incorrectly waited for a new generation; its first pose was
corrected to actually move. The 200 ms target was not relaxed. Asynchronous export
pumping then exposed the cached-graph target-increase bug; centralized state
transition invalidation fixed it. Revocation verification also now recognizes
that the registry removes a drained entry instead of returning a zero count.

## Measured current implementation (not matched before/after)

RTX 3060 Laptop, DX12, Cornell, 256x256, fixed seed, 8 bounces; CPU denoise four
threads, 100 ms interval in the smoke. Final expanded smoke, no concurrent build:

| Metric | P50 / P95 / P99 (ms), or value |
|---|---|
| Offscreen CPU frame | 0.419 / 0.678 / 1.705 |
| Unique delayed GPU frame span | 0.099 / 0.669 / 0.686 |
| Trace pass including frames without tracing | 0 / 0.556 / 0.572 |
| Completed spp/s including denoise waits, excluding export frames | 330.35 |
| OIDN CPU duration maximum | 51.52 ms |
| Synthetic camera snapshot -> completed whole 64x64 preview | 2.457 / 2.803 / 5.225 |
| Maximum PT submissions in flight | 1 |
| Old-generation bands retired during 24-pose trajectory | 23 |

The synthetic trajectory advances when each current-camera preview completes;
it does not simulate OS mouse-event latency or measure actual display publication.
The 1280x720 editor run (932x362 docked viewport) measured CPU wall
0.858/1.450/2.209 ms, GPU frame span 0.332/1.224/1.556 ms, frame-slot wait
0.00045/0.00070/0.00105 ms and Present call 0.092/0.163/0.294 ms. Its roughly
half-second sampling window is too short for a sustained-interaction claim.
The raster baseline used another resolution/mode; **no improvement ratio is valid**.

Linear RGB RMSE against the finite, correlated 256-spp raw image:

| spp | Raw | OIDN |
|---|---:|---:|
| 1 | 0.22917 | 0.13815 |
| 4 | 0.11425 | 0.07518 |
| 16 | 0.06017 | 0.04848 |
| 64 | 0.02698 | 0.02517 |
| 256 | 0 by definition | 0.01307 |

This is a convergence/denoise diagnostic, not independent ground truth or proof
of texture/edge preservation. The stage mosaic is `stages-preview.jpg`.
`bsdf.csv` and `metal-{0,1,2}.pfm` record a full-view flat metallic plane in unit
constant environment, 64x64/256 spp: smooth F0=(0.75,0.5,0.25) returns
(0.749888,0.499927,0.249965). At roughness .8, NEE+BSDF MIS gives
(0.417692,0.278515,0.139338), while DirectOnly NEE gives
(0.417476,0.278371,0.139266). This checks the former black-metal bug and an
environment MIS consistency case; it is not a complete BSDF energy/PDF suite.

Recompute numeric evidence with the existing PFM reader:
`python Tools/Rendering/summarize_progressive_pt.py out/agent/captures/progressive-pt --editor-csv out/agent/pt-baseline-editor.csv --editor-csv out/agent/pt-editor-after.csv`.
`summary.json` deduplicates delayed GPU IDs and labels measurement domains.

The bounded GPU smoke uses `HE_OFFLINE_PROGRESSIVE_SMOKE=1` with the owned
`Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Run -TimeoutSeconds 360`
route. Outputs: `out/agent/captures/progressive-pt`, including raw and denoised
PFM at 1/4/16/64/256 spp and CPU/progress CSV. Its offscreen frame timing excludes
interactive editor input; it must not be substituted for input-to-current-view
latency or editor frame statistics.

Editor checks can explicitly select a provider with
`hectl editor check -StartupViewportMode he.viewport.path-tracing`.

## Acceptance still open

General material/texture coverage beyond the PBRTest BasePBR implementation;
environment luminance sampling and rotation;
precise separated diffuse/specular/emission/environment AOVs; BSDF/PDF reference
tests; interactive motion trajectories and input/display timestamps; published
image age/frequency; staged shader/large-AS initialization; visibility/minimize
scheduling; resize/Play stress; dependency-missing and allocation-failure runs;
long-run memory measurements; Vulkan PT runtime and matched before/after PT data.
The OIDN dependency is linked normally: a missing runtime DLL can still prevent
host startup; a runtime optional-loader fallback has not been implemented. The
four-thread limit is per denoiser/session, not yet a process-wide worker budget.
The budget adjusts PT bands using delayed trace and aggregate frame timing, but
AS construction and full-image snapshot/upload operations are still indivisible.
Their latency is not strictly bounded at large resolutions/scenes. CPU phase
timings, actual displayed spp/publication age and per-stage AS/upload GPU timings
remain incomplete. Current exports bind the image generation and reject a changed
view; a self-contained scene/camera/settings export manifest is still missing.
Owned check runs exited and reaped processes successfully, but their existing
storage-receipt helper emitted a Get-FileHash command-resolution warning; automatic
history-registration/retention completeness is not claimed for these artifacts.
The current DirectDiffuse/IndirectDiffuse IDs are legacy path-depth partitions
and now include specular contributions; the panel must not imply lobe isolation.
This ledger deliberately does not mark the complete requested task accepted.

## PBRTest activation crash, 2026-09-09

Reproduced against `Assets/Scenes/PBRTest.he` on the same DX12 RTX 3060 Laptop.
The scene contains Sponza. Runtime classification excluded all 25 sections:
22 first matched smooth shading normals, and 3 matched textured material.
These categories describe the first rejection, not mutually exclusive material
capabilities. With no admitted geometry `RayTracingScene::IsReady()` is false.
`ReferencePathTracer::PrepareFrame` previously threw an uncaught logic_error:
`ReferencePathTracer requires a prepared RayTracingScene`. This was a crash bug,
not an acceptable requirement for authoring scenes.

The no-work state exposed a second host contract error: RuntimeRenderer allowed
offscreen reuse when the technique required no updates, but SceneRenderer
rejected it merely because GI was active. Its guard now checks the technique's
actual continuous-update requirement. Pending plugin graph/display changes also
request an update, so a stopped technique can still apply display settings.

The plugin now publishes a cleared 1x1 placeholder with zero samples and a Failed
status explaining the exclusion counts. It does not bind a null TLAS, schedule
tracing/denoising, export an invalid image, or retain an old scene's beauty as
the current result. Entering this state invalidates the accumulation generation;
already submitted resources retire through the existing completion mechanism.
Returning to supported geometry starts a fresh generation automatically. The
placeholder is not an environment render or a successful PBRTest render.
At this earlier checkpoint material textures, smooth shading, alpha and analytic
lights remained open. They are implemented for BasePBR by the subsequent
[PBRTest follow-up](PBRTestPathTracing.md); the empty-scene fallback remains valid.

Validation commands and evidence:

```powershell
# runtime-manual: before fix FAIL, uncaught exception; after fix PASS, exit=0
.\hectl.bat editor check -Frames 240 -Resolution 1280x720 -StartupScene Assets/Scenes/PBRTest.he -StartupViewportMode he.viewport.path-tracing -TimeoutSeconds 180
# runtime-manual: shared host raster path, PASS
.\hectl.bat game check -Frames 120 -TimeoutSeconds 180
# unit-test: session contracts, PASS
.\hectl.bat test OfflineReferenceRendererSession
# test-layer: actual DX12 GPU regression, PASS with validation enabled
$env:HE_OFFLINE_PROGRESSIVE_SMOKE='1'
try { powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Run -TimeoutSeconds 360 }
finally { Remove-Item Env:HE_OFFLINE_PROGRESSIVE_SMOKE }
```

Logs: `out/agent/pt-pbrtest-repro.log`, `pt-pbrtest-final.log`,
`pt-pbrtest-unit.log`, `pt-pbrtest-game.log`, `pt-pbrtest-regression.log`.
The GPU regression covers activation on fully excluded geometry, invalid export
rejection, subsequent 1/4/16/64/256 spp raw/OIDN convergence, removing all geometry
after a valid result, and resuming sampling when supported geometry returns.
It retains the prior generation, camera, pixel-count and retirement assertions.
The fix is built in the isolated Agent directories; existing Visual Studio
binaries must be rebuilt before they contain it. Vulkan runtime remains untested.

## Design references

- [Cycles render scheduling](https://developer.blender.org/docs/features/cycles/render_scheduling/)
- [Cycles kernel scheduling](https://developer.blender.org/docs/features/cycles/kernel_scheduling/)
- [OIDN API and RT filter semantics](https://www.openimagedenoise.org/documentation.html)
- [PBRT: A Better Path Tracer](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer)
