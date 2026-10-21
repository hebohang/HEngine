# HGame rendering measurements

Use the owned HGame development route, with native client dimensions and no
VSync. A requested `-Resolution` now explicitly restores and verifies the GLFW
client size after window creation. A desktop work area must not silently turn
`3840x2160` into `3840x2130`. A window can extend past the physical desktop;
render and capture dimensions remain the requested native dimensions.

```powershell
.\hectl.bat game check -Frames 180 -Resolution 3840x2160 -StartupScene Assets/Scenes/YourScene.he -RenderPath Deferred -RenderStats -PerformanceWarmupFrames 30 -RenderStatsPath C:\WorkSpace\HEngineDev\out\agent\benchmark-frames.csv
```

`StartupScene` is passed to HGame without changing `Game.json`. `RenderPath`
is a command-line RenderSettings override and is independent of `GraphicsApi`.
Use a new CSV path for each run; existing statistics are not overwritten.
The short command above checks the measurement route, not thermal stability.

The following short comparison uses one 600-frame warmup cycle and two complete
measured cycles. Run from `C:\WorkSpace\HEngineDev`, sequentially, with the same
quality input. Intel retains its BC5 normal / LOD-capable assets (LOD disabled
by Comparable); Bistro uses the explicitly corrected Mask variant. Both retain
full geometry and 2K textures while the main view remains native 4K.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Benchmark-Rendering.ps1 -StartupScene Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he -BenchmarkInputPath Project/Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/benchmark_input.json -RenderSettingsPath Project/Assets/RenderingBenchmarks/Configurations/Native4KComparable.json -RenderPath Deferred -GraphicsApi DX12 -Resolution 3840x2160 -WarmupFrames 600 -MeasureFrames 1200 -TimeoutSeconds 300 -PowerModeNote "Vendor performance mode not independently verified; AC and power-scheme metadata captured separately"
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Benchmark-Rendering.ps1 -StartupScene Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BCGPUMaskFixed/Benchmark.he -BenchmarkInputPath Project/Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BCGPUMaskFixed/benchmark_input.json -RenderSettingsPath Project/Assets/RenderingBenchmarks/Configurations/Native4KComparable.json -RenderPath Deferred -GraphicsApi DX12 -Resolution 3840x2160 -WarmupFrames 600 -MeasureFrames 1200 -TimeoutSeconds 300 -PowerModeNote "Vendor performance mode not independently verified; AC and power-scheme metadata captured separately"
```

Repeat those exact commands with `-RenderPath Forward`. The short interval
tests complete workload cycles and records startup/residual spikes; its normal
thermal result is `INSUFFICIENT_DATA`, not thermal PASS. The PowerModeNote above
states the present uncertainty; a formal report must replace it with the
actually observed vendor mode and retain captured AC/power-scheme metadata.

For a sustained capture (36,000 warmup / 18,000 measured frames, two adjacent
120-second thermal windows):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Benchmark-Rendering.ps1 -StartupScene Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he -BenchmarkInputPath Project/Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/benchmark_input.json -RenderSettingsPath Project/Assets/RenderingBenchmarks/Configurations/Native4KComparable.json -RenderPath Deferred -GraphicsApi DX12 -Resolution 3840x2160 -WarmupFrames 36000 -MeasureFrames 18000 -TimeoutSeconds 3600 -ThermalWindowSeconds 120 -PowerModeNote "Vendor performance mode not independently verified; AC and power-scheme metadata captured separately"
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Benchmark-Rendering.ps1 -StartupScene Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BCGPUMaskFixed/Benchmark.he -BenchmarkInputPath Project/Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BCGPUMaskFixed/benchmark_input.json -RenderSettingsPath Project/Assets/RenderingBenchmarks/Configurations/Native4KComparable.json -RenderPath Deferred -GraphicsApi DX12 -Resolution 3840x2160 -WarmupFrames 36000 -MeasureFrames 18000 -TimeoutSeconds 3600 -ThermalWindowSeconds 120 -PowerModeNote "Vendor performance mode not independently verified; AC and power-scheme metadata captured separately"
```

Repeat with `-RenderPath Forward` and the identical scene, quality file and authored camera input. `BenchmarkInputPath` validates scene, trajectory and camera-script hashes. The supplied static scenes contain no random components, and record `randomSeed=not_applicable`; the old metadata-only seed parameter is removed. `RenderingBenchmarkCamera.py` actually moves the camera using integer ticks and a 600-frame closed trajectory, never wall-clock time. The harness explicitly passes the trajectory's fixed 1/60-second step, requires whole warmup/measurement cycles, and checks the script's final tick count and hashes. A short runtime check must establish that the script tick count equals all requested frames before a long measurement is accepted.

`RenderSettingsPath` is a flat stable-ID settings object, copied and hashed per run. The harness checks the actual renderer's shutdown settings snapshot, dimensions and requested/effective values (including CLI path/API/VSync precedence); it rejects unreported quality fallbacks. `Native4KComparable.json` explicitly selects TAA, half-resolution SSGI (2 rays, 32 steps), half-resolution GTAO and SSR (32 steps, 5 refinements), bloom and remaining declared controls, with LOD/occlusion disabled. `Native4KOptimized.json` enables LOD and leaves occlusion disabled pending whole-frame benefit. `Native4KOcclusionCandidate.json` additionally enables occlusion for a separate comparison. All profiles explicitly retain 4 GTAO slices / 8 steps per side, SSR guide cache off, main GPU batching off, `Rendering.GpuBatching.ShadowsEnabled=false` and `Rendering.GI.SVGF.OptimizedKernels=false` until measured. The last two switches are mandatory explicit inputs and participate in requested/effective validation, so a runtime default cannot silently contaminate an A/B comparison. Neither changes native 3840x2160. These are reproducible test inputs, not a performance claim.

The default warmup is 36,000 rendered simulation frames. The analyzer compares the last two 120-second device-telemetry windows: temperature mean drift <=2 degrees C, final temperature slope <=0.5 C/minute, graphics-clock drift <=5%, power drift <=10%, with sufficient samples. It reports PASS_TELEMETRY_PROXY, FAIL_UNSTABLE_TELEMETRY or INSUFFICIENT_DATA. This is a recorded screening proxy; frame count or utilization alone never proves stable performance. `result.json` leaves overall acceptance unevaluated until visual quality and motion evidence are added. `-PrepareOnly` validates input hashes and records the exact command without building or running HGame.

The output directory contains the exact command, source/scene hashes, machine
and power configuration, CPU frame CSV, sampled hardware telemetry, logs, and
Type 7 P95/P99/max/over-budget statistics. The measured CSV is buffered and
written after the timed loop. GPU statistics collection uses already completed
submission data during rendering; the final reporting drain happens outside
the measured frame interval.

`frames.csv.passes.csv` supplies actual completed GPU query samples (`gpuSample,passName,milliseconds`). The analyzer streams this long table, selects exactly the GPU IDs used for graph-span statistics, and reports each pass's mean/P95/P99/max. A final log summary is never treated as per-frame evidence. `frames.csv.settings.json` supplies requested/effective settings and shutdown dimensions; `camera.runtime.json` supplies actual trajectory ticks/hash. Resource statistics report draw/batch/visibility, transient reservation and LOD triangle counts; CPU extraction before GPU culling is distinguished from delayed GPU opaque-visible triangles. >50ms and >100ms frames remain explicit alongside strict 60/90Hz budget failures.

Interpretation:

- `cpuWallFrameMs` is application frame wall time, including GPU backpressure
  and present. It is not isolated CPU compute time. The update, render/submit/
  present and window columns partition selected CPU phases.
- Rendering now has disjoint CPU subintervals: `cpuAcquireImageMs`,
  `cpuFrameSlotWaitMs` (the actual NVRHI completion-query wait),
  `cpuGraphicsSubmitMs`, `cpuPresentMs`, `cpuGarbageCollectMs`, and
  `cpuCommandListSetupMs` (open plus close). `cpuRenderOtherMs` is the remainder
  of `renderSubmitPresentMs`, including scene preparation, graph command
  recording and CPU UI. It is not a hardware CPU-busy measurement. Acquisition,
  submission and presentation can contain OS/driver pacing. The analyzer checks
  the partition and reports mean/P95/P99/max for every phase. Equivalent Tracy
  zones identify these intervals in a thread timeline when built with Tracy.
- `gpuFrameSpanMs` brackets the complete RenderGraph, including gaps between
  passes. `gpuPassSumMs` separately sums individual passes. Both use delayed
  completed queries tagged by `gpuSample`. The analyzer deduplicates IDs and
  excludes IDs up to `gpuWarmupCutoffSample` (the first measured GPU sample plus
  frames in flight), conservatively removing GPU work from the CPU warmup.
  Present/external GPU work is outside this interval. Never add GPU span to CPU
  wall time. Delayed GPU values do not identify the same CPU row's visibility.
- Device memory, power, temperature and utilization from `nvidia-smi` cover the
  whole GPU, including other processes. Telemetry is sampled about once per
  second. Process peak working set is an OS high-water mark; private bytes are
  sampled and can miss short allocation spikes.
- The strict 60 Hz budget result fails when any measured frame exceeds
  16.667 ms. Average FPS alone never establishes stable 60 FPS. Longest
  consecutive overruns and the complete CSV expose sustained stalls.
- Changing render dimensions aborts the benchmark. VRS, upscaling and dynamic
  resolution are not introduced by these tools. Record any separately enabled
  engine effects in the benchmark configuration.

## HDR post processing configuration

`ScenePostProcessPass` consumes native, single-sample scene-linear HDR after
temporal AA and before tone mapping/UI. Each view owns its exposure history.
The default bloom intensity is 0.05 with scene-linear threshold 1.0 and a
50% soft knee. Bloom downsamples to ceil(width/4) by ceil(height/4), applies two
separable nine-tap Gaussian filters, and composites at native resolution.

Auto exposure is disabled by default. When enabled, it uses area-weighted log
luminance of the quarter-resolution average colors, GPU tile/group reductions,
and a one-pixel floating-point history updated every rendered frame. It targets
middle gray 0.18, clamps exposure to 0.05–16, and adapts exponentially in log
exposure at rate 2/second. Partial downsample footprints and reduction tiles are
counted explicitly. Camera cuts, scene changes and resize reset adaptation.
This is a geometric-average meter, not a percentile histogram meter.

Manual exposure retains the existing scene/volume/camera override order. Auto
exposure replaces the nonzero manual multiplier before grading and compensates
for the existing final tone-map multiplication, so light is exposed once.
Explicit manual exposure zero stays black. Warm/cool balance is an artistic
RGB multiplier in [-1,1], followed by luminance-based saturation and contrast
about middle gray. It is not calibrated Kelvin white balance or a 3D LUT.
ACES and the explicit sRGB output transfer remain in `SceneToneMap`.

All post-process bindings and history retired by graph changes retain their
last GPU submission ownership. No CPU readback or GPU idle is added to exposure.
Diagnostic material-property views should bypass bloom/grading in the caller.

## GTAO configuration

`SceneAmbientOcclusionPass` reconstructs world positions from normal-Z raster
depth and decodes the shared world-space octahedral normal. Radius defaults to
1 world unit, intensity to 1 and normal bias to 0.01 world units. Projection and
view depth convert the world radius into a screen radius; orthographic cameras
use a depth-independent projected radius.

The horizon integral is evaluated at ceil(width/2) by ceil(height/2), using four
angular slices and eight samples on each side (64 depth samples per half-size
pixel). It uses a fixed spatial rotation, no temporal subsampling and no mip
selection. The nearest covered sample in each 2x2 native footprint supplies the
guide. A 3x3 depth/normal bilateral filter and 3x3 guided native reconstruction
produce R8_UNORM visibility, with 1 denoting unoccluded/background pixels.
No compatible guide yields neutral visibility rather than foreground bleeding.

The integral follows the [Activision GTAO technical report](https://research.activision.com/publications/2020-03/practical-real-time-strategies-for-accurate-indirect-occlusion).
The published normal/horizon formulation was also checked against
[Intel XeGTAO](https://github.com/GameTechDev/XeGTAO). The pass is an engine
implementation, not the XeGTAO library. Screen-space height-field limitations
remain: offscreen/hidden geometry is unavailable, and thin geometry can still
over-occlude or be undersampled. This pass produces visibility only; lighting
must apply it to suitable indirect terms, preserving direct light and emissive.

## SSR and local reflection probes

`SceneReflectionPass` runs after opaque lighting/GI and before transparent
objects and the main TAA resolve. It traces at ceil(width/2) by ceil(height/2)
into RGBA16_FLOAT radiance/confidence, then reconstructs with a 3x3 depth,
surface-plane and normal guide into native RGBA16_FLOAT HDR. The main TAA
stabilizes the result; there is no separate reflection history or ray tracing.
The input remains the original opaque HDR throughout the pass, preventing
recursive screen-space feedback.

Defaults are 64 quadratically spaced world-space steps over 50 world units,
five binary refinement steps, 0.2 world-unit view-depth thickness, 0.03 normal
bias, roughness cutoff 0.6 and screen-edge fade 0.08 of the viewport. The final
quarter of the roughness range fades into prefiltered environment reflection.
Successful hits use a five-tap depth-guided radiance footprint of at most eight
native pixels. This is a bounded spatial roughness approximation, not a GGX
multi-ray convolution. These parameters are exposed under
`Rendering.Reflections.SSR.*`; `Rendering.Reflections.Probes.Enabled` controls
local volumes independently.

`ReflectionProbeComponent` references an existing project-relative linear HDR
equirectangular asset captured at `CaptureOffset` in entity space. The existing
environment cache performs cubemap conversion and roughness prefiltering.
`BoxHalfExtents` transforms into a world-axis-aligned box (rotation expands the
box to contain its transformed corners), and `CaptureOffset` becomes the world
capture position. The shader intersects the reflection ray with this box and
looks from the capture position toward the exit point, providing box parallax
correction. Overlapping volumes blend by smooth distance to their boundaries;
insufficient local weight fades continuously to global IBL. `BlendDistance`
is measured in world units; local `Intensity` scales the asset radiance.

The pass accepts at most eight view-selected probes and rejects a larger input.
The caller must publish its selection/budget diagnostic. Invalid/unavailable
probe textures are excluded; offscreen rays, depth misses, rejected back faces,
rough surfaces and screen borders use local probes or the global environment.
The component consumes authored/offline captured HDRs; automatic scene cubemap
baking and animated probe capture are not implemented.

Composition replaces the global indirect specular term using the same shared
BRDF LUT, Fresnel factor and material-AO times GTAO specular-occlusion formula
as opaque lighting. It preserves direct light, indirect diffuse and emissive.
When no local or SSR contribution exists, the reflection delta is exactly zero.
Forward reconstructs this factor from the compact quantized raster surface,
whereas its initial lighting can use unquantized material values; replacement
therefore has a small precision difference on affected pixels. Deferred uses
the same packed inputs. Unlit receiving surfaces bypass replacement.

These passes require single-sample inputs; the renderer must report any
effective AA fallback from Forward MSAA, and the pass explicitly rejects
unresolved multisample inputs. Transparent materials retain their forward
lighting and existing refraction behavior; they are not SSR receiving surfaces
and do not appear in the opaque SSR input. A separate native-resolution TAA
resolve does not recover hidden or offscreen geometry.

## GPU visibility comparison controls

Run-specific global settings use `-RenderSettingsPath <absolute JSON file>` on
`hectl game check` or `hectl agent start` (native CLI
`--render-settings=<absolute JSON file>`). The file is a flat stable-ID patch,
for example `{"Rendering.Path":"Deferred","Rendering.Occlusion.Enabled":true}`.
Unknown IDs, malformed JSON and files larger than 1 MiB are rejected. Explicit
CLI path/backend/no-vsync options override file entries; omitted path/backend
options preserve file values. Existing check/session harnesses explicitly set
no-vsync for repeatable timing. No project or user settings are persisted.

The CSV sidecar `<RenderStatsPath>.settings.json` records requested, effective,
sources, diagnostics and renderer fallback reasons. `effective` uses actual
renderer AA/GI/backend and window VSync; `serviceEffective` preserves the
settings service's unadjusted result. The serialized `hash` identifies the
service snapshot, not the renderer-adjusted `effective` object. The file is
written at startup and updated to `renderer.phase="shutdown"` after GPU drain;
`renderer.width/height` record the actual render extent. Scene camera/volume
post-process overrides are properties of the archived scene/trajectory and
must be kept with the global configuration evidence.

`Rendering.Occlusion.Enabled` defaults to `false` pending paired measurements.
When enabled, the complete current-frame opaque/alpha-mask depth prepass still
runs from the original GPU frustum list. A separate normal-Z MAX depth pyramid
starts at ceil(width/4) by ceil(height/4); every native pixel contributes,
including odd edges. It reduces farther mips conservatively. A sphere's
projected enclosing box reads all covered texels at its chosen mip and is
rejected only if its nearest depth exceeds the maximum occluder depth plus
0.0001. Background, unknown bounds and near-plane crossings remain visible.
Reset/cull rewrites the existing compact visible list and indexed indirect
arguments; the asynchronous statistics copy runs afterwards, so
`gpuVisibleObjects` describes the final main-view GPU list. CPU fallback and
skinned paths remain independently recorded. No prior-frame depth or CPU
occlusion readback is used. Requested MSAA falls back explicitly to native TAA
when this option is enabled because single-sample coverage cannot prove
occlusion of every MSAA sample.

`Rendering.LOD.Enabled` also defaults to `false`; `Rendering.LOD.MaxPixelError`
defaults to 1 native pixel and accepts 0.1..8. Compare LOD independently from
occlusion and report the changed geometry/error setting with captures and
timings. These settings use the existing render-settings JSON override and
runtime settings transaction. They do not change the main resolution.

## Runtime regression controls

Inside a session owned by the test script, these commands exercise the real
runtime. The surrounding script must stop its Agent session in `finally`.

```powershell
& Tools/Agent/hectl.ps1 agent call -Method window.resize -ParamsJson '{"width":1279,"height":719}'
& Tools/Agent/hectl.ps1 agent call -Method camera.cut -ParamsJson '{}'
& Tools/Agent/hectl.ps1 agent call -Method runtime.scene.load -ParamsJson '{"path":"Assets/Scenes/RenderingRegression.he","forceReload":true}'
```

Use an existing scene asset for the last command. `window.resize` accepts exact
windowed client dimensions in 64..8192, including odd dimensions. It verifies
the GLFW client extent and defers the renderer's resize event until the current
frame has finished. Poll `status` fields `renderWidth` and `renderHeight` before
capturing. A fullscreen surface reports an explicit error.

`camera.cut` marks the active runtime or author-preview `RenderSystem3D` camera
for temporal invalidation at the next extraction. `runtime.scene.load` accepts
only existing project-relative `Assets/*.he` files. Its response means queued,
not committed. Poll `world.summary` for the requested scene and a changed
`sceneGeneration`, and inspect logs for transaction failures. With `forceReload`
the scene document receives a fresh resource generation before the existing
candidate-Level transaction commits. Ordinary same-scene requests keep their
prior no-op behavior. These commands do not save or publish an author document.

Author preview now imports environment, spot-light and reflection-probe
components through the canonical serialized component list. Its response
explicitly records that physics, scripts and animation do not advance there;
runtime scene loading is required to validate animated content.

## Threading and queue audit (2026-09-08)

The integration owner's preliminary Sponza sample recorded CPU Update about 0.9 ms, RenderOther 6.3 ms, FrameSlotWait 11.3 ms and GPU graph span 18.9 ms. That capture contained startup outliers and is not a strict steady-state A/B baseline. Even completely overlapping the 0.9 ms update cannot remove the GPU's roughly 2.23 ms deficit against 16.667 ms. FrameSlotWait is a measured GPU completion wait, not a nonblocking statistics poll, and must remain until ownership changes safely reduce backpressure.

`RenderWorldSubmission` already publishes an owned immutable frame snapshot and bone/material values. Resource references to `Material`, `SubMesh` and mesh buffers still have mutable cache/hot-reload lifetimes. `SceneRenderer::RecordFrame` mutates frame-slot ownership, graph callbacks, temporal state, descriptor/scene caches and statistics; graph lambdas capture that same renderer. `EffekseerRuntime::Update` and `Render` share manager/renderer/view/interoperability state. `Application`'s runtime UI recorder also reads live Rml/ImGui state instead of a separately owned draw packet. Moving the current call to another thread without transferring these owners is incorrect.

The bounded parallel preparation candidates are immutable per-object bounds/LOD error calculation, light/shadow candidate culling and unique-material packing. Stable ID assignment, descriptor allocation and publication must retain deterministic single-owner order. These tasks should be individually timed before selecting a worker count; the broad RenderOther remainder does not prove that all 6.3 ms is parallelizable CPU work. Existing shader/pipeline caches already use mutexes; duplicating them would not establish resource ownership.

NVRHI permits recording distinct command lists on distinct threads. The DX12 backend already creates compute/copy queues. The current RenderGraph execution, however, records passes serially into one command list; queue labels alone do not implement cross-queue resource ownership, fences or submission. Depth/GBuffer -> GTAO/SSGI/SSR -> lighting/TAA/post is a dependent GPU chain with competing shader/cache/bandwidth use. Splitting this chain cannot be presented as measured async-compute benefit. Shadow/surface preparation overlap is a concrete candidate only after a dependency/fence plan and whole-frame comparison. Cold texture upload waits are real GPU lifetime boundaries; asynchronous upload may improve startup, but current warm-frame measurements do not establish upload as the bottleneck.

Decision for this measured configuration: retain bounded existing frame submission and serialized renderer ownership; do not enable an extra Render/RHI thread, parallel graph recording or async compute by name alone. Prioritize measured GPU draw/memory work and the existing CPU cache/dirty-range improvements. No empty thread/queue interface is counted as an implementation. Revisit a specific preparation task only when its measured cost can repay scheduling and ownership overhead.

## Indirect-light and compressed-normal quality contracts

SSGI now consumes the existing active scene-linear environment cube on a screen-space miss, using trilinear mip `min(3,last)` and the same IBL intensity as raster lighting. This explicitly smooths environment angular frequencies for the configured low ray count. A screen hit and its environment fallback are blended by hit confidence, then all rays are averaged exactly once; the former up-to-2x hit-count compensation is removed. Raw alpha records screen-hit coverage, while SVGF coverage records a valid transport observation, including an environment miss, so an off-screen view does not indefinitely preserve stale illumination. Screen-space misses still cannot know hidden geometry; GTAO attenuates the environment fallback, and off-screen light leaks remain a screen-space limitation.

Material AO modulates reflected indirect diffuse. GTAO applies to unobserved environment fallback; already observed screen-space transport has traced visibility. Direct analytic light and the surface's own emission remain separate. Lit+emissive materials retain diffuse reception; Unlit is the explicit emissive-guide alpha model flag. SSR/probes replace global specular via `source + (chosen - global) * EnvironmentSpecularFactor`, with the same material/GTAO factor as raster IBL, instead of adding a second environment term.

BC5 preserves the positive-Z normal direction through XY reconstruction but cannot preserve the average Z component needed by Toksvig mip variance. The derivative-based specular AA contribution still works; mip-variance equivalence must not be claimed. Linear BC7 XYZ and linear, unnormalized offline mips use the same 8 bits/texel and retain that missing moment within compression error. The independent BC7-normal configuration preserves the BC5 configuration for comparison. Realtime RT secondary hits currently use interpolated vertex normals and the restricted RT material table; they do not gain normal-map or animation coverage from this raster fix.

The irradiance generator stores the cosine integral (`PI * sampleAverage`). The integration owner corrected both raster IBL consumers to apply Lambert's `/PI`; the previous diffuse environment brightness was too large. Captures before this correction are not final brightness A/B evidence. Both paths retain the disclosed fixed exposure 0.31; exposure is not silently increased to cancel this correction.

Workload settling is checked separately from the temperature proxy: at least two complete identical 600-frame camera cycles are required. First/last CPU/GPU cycle mean drift must be <=5%, P95 drift <=10%, and any unexplained measured CPU frame >50 ms rejects this proxy. Every original frame remains in the full percentiles and budget counts; a late steady suffix is never substituted for the requested measurement. Loading attribution needs correlated upload/loading evidence. Short 600-frame checks report insufficient repeat-cycle data, and the earlier static warmup-180 capture with 100-150 ms residual frames is not a strict steady-state baseline.

Realtime RTGI still has its existing restricted secondary scene/material coverage and a constant environment miss term. In the ordinary non-offline renderer path that constant is currently zero; realtime RTGI does not acquire the new SSGI cubemap fallback. This remains an explicit limitation, not an advertised RT lighting improvement.

The three Bistro Mask failures are now resolved in separately named `ORCABistro2048BCGPUMaskFixed` and `ORCABistro2048BC7NormalsMaskFixed` assets. The original failed data is retained. Corrected point/bilinear/trilinear metrics, byte-identical large mips and the rejected whole-texture BC3 alternative are documented in `Project/Assets/RenderingBenchmarks/README.md`. Their matching fixed-camera resources are under `Trajectories/BistroExterior2048BCGPUMaskFixed` and `Trajectories/BistroExterior2048BC7NormalsMaskFixed`; use their own `benchmark_input.json` hashes.

Ordinary raster SSGI no longer requires RT emissive/geometry tables. Its unused legacy structured bindings use correctly sized 96-byte/32-byte zero records, initialized once with explicit NVRHI Common -> CopyDest -> permanent ShaderResource tracking on the graphics submission queue. This is not a RayTracingScene selection dependency or a per-frame table upload. The legacy RT-index surface path still requires matching real tables. Initial missing-table and resource-tracking runtime failures remain failure evidence; only a subsequent successful HGame run can establish the corrected runtime behavior.

## Corrected Bistro environment and queue-depth comparisons

For new Bistro runs use `Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BCGPUMaskFixedEnvironment/Benchmark.he` and its `benchmark_input.json`. This derivative restores the official HDR at intensity10 and preserves the original scene entities. Earlier trajectory directories without the Environment suffix lack the source environment and fail high-quality acceptance. The restoration command and hashes are documented in Tools/Assets/RenderingBenchmarks/README.md.

`Native4KTwoFrames.json` is the same ComparableReuse quality with `Rendering.Presentation.FramesInFlight=2`; default remains3. `Native4KTwoFramesSvgf.json` additionally enables only SVGF optimization. `Native4KTwoFramesCandidates.json` combines2frames, transient reuse, TAA gather, SSR guide caching, shadow batching and SVGF optimization; geometry/LOD/main batching/ray counts/effect resolutions remain explicitly recorded in the JSON. Treat it as a candidate until whole-frame and quality verification, not a silent replacement baseline.

Memory budget CSVs now include automationFrame. The loader's frame counter resets when the scene becomes ready, so frame number alone cannot isolate measured memory. Analysis requires automationFrame1 and measured frame range; files without the phase column report UNAVAILABLE_PHASE_TAG for that range, while separately retaining all-phase usage peaks/overbudget counts. This correction does not alter CPU/GPU timing rows, which already require automationFrame. Raw logs and the prior erroneous aggregates are retained with explicit correction JSONs.
