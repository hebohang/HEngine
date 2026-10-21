# Runtime tasking validation log (2026-09-26–27)

## Acceptance snapshot (2026-09-27)

| Requirement | Current evidence | Limit |
| --- | --- | --- |
| Delegate and Buffer correctness | `DelegateBuffer` Debug and optimized regressions; focused MSVC ASan harness; subsequent Debug Game/Editor builds | No dedicated concurrency sanitizer was available for the Windows delegate path. |
| Fixed 3D simulation time | `FixedSimulationClock`, `PhysicsScriptContact`, `InputActions`, `PythonRuntime`, `SurvivorFixedUpdate`, and 30/120 FPS plus 200 ms DX12 Game runs | Python now has an optional fixed callback; other native/managed script callbacks remain display-rate. No physical input-to-display measurement exists. |
| CPU task graph | `TaskScheduler` and owner-thread `FrameStageGraph` Debug/optimized contracts; mesh decode, targeting and morph paths; 30/60/120 Hz plus scene-switch device checks | The default dense fixture has no large active morph mesh, so its whole-frame gain cannot be attributed to task workers. Low-core plans have pure tests, without a low-core device run. |
| Async runtime textures | `TextureUploadState` regressions, DX12/Vulkan reload and scene-switch sessions, final-source GPU mip byte readback on both backends, BC1/3/5/7 visual/device probes, 48-variant ready and frame-paced probes, bounded RmlUi image decode, plus paired old/new scene-switch frame samples | One startup default-texture device-wide wait remains. The paired scene-switch sample includes non-texture work; filtered transparent-edge appearance at a controlled screen footprint was not measured. |
| Performance and lifetime report | Paired dense DX12 frame and process-memory runs, paired scene-switch DX12 frame and GPU-memory runs, GPU spans, task queue/critical-path timing, upload latency, final-source DX12/Vulkan captures and 218-frame samples | DX12 results vary across repeats; final Vulkan dense CPU P95 is 34.57 ms and misses 16.667 ms. Physical input-to-display latency is unmeasured. The old synchronous loader had no queue to measure. |

An attempt to capture software display events with the installed WPR
`CPU+GPU+DesktopComposition` profiles failed before any game run: WPR returned
`0xc5585011` (could not enable the system performance profiling policy).
`GPU` alone failed with the same code. `wpr -status` reported no recording
afterward. The bounded start logs are retained in
`out/agent/latency-wpr-20260927-a/` and `-b/`. PresentMon was installed
later for the separate attempt documented below. These WPR failures are not
input latency measurements; hardware
scanout was not observed.

The existing input replay path was exercised on the optimized DX12 Game with
render validation enabled. The 120-frame fixture held no keys for 60 frames,
held `w` for 30, then released it for 30. The log records all three segment
transitions and a clean frame-limit exit. This validates replay ingestion and
the bounded Game run, but provides no display timestamp or input latency value.
The exact command was:

```powershell
.\hectl.bat game check -Frames 120 -InputReplayPath C:\WorkSpace\HEngineDev\out\agent\latency-input-replay-20260927.json -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Resolution 1280x720 -RenderValidation -TimeoutSeconds 180
```

Evidence: `out/agent/logs/run-game-relwithdebinfo-20260927-110137-163-106032.stdout.log`;
`build-only` HGame PASS and `runtime-manual` bounded Game PASS. A relative
`-InputReplayPath out/agent/...` was rejected by `hectl` before launch; the
absolute path above was used for the actual run.

### Short burst texture queue and device lifetime (2026-09-27)

An Agent-only `render.texture.burstProbe` requests 48 cache-distinct variants
of an existing PNG before waiting for any of them. It then acquires a ready
lease with the same key for each variant, checks the published dimensions,
and releases both leases, including on exceptions. The short burst exposed a
sampling blind spot: `peakPending` was only updated by `Collect`, so a burst
that was drained within one command reported a peak of one. `LoadTexture`
now updates that peak as it admits an asynchronous request.

On both strict DX12 and strict Vulkan Agent sessions with NVRHI validation,
the command returned `requested=48, ready=48, shared=48` and the texture log
reported `peakPending=48`, `failed=0`, `globalWaits=1`, and zero pending at
the end. That one global wait belongs to startup default texture creation.
The session stderr files were empty. Vulkan enabled its validation layer;
the D3D12 native debug layer was unavailable on this host, so DX12 used
NVRHI validation. Sessions:
`out/agent/sessions/20260927-111241-477-5cbd2c5c5547/` (DX12) and
`out/agent/sessions/20260927-111201-994-1a5bde28639d/` (Vulkan).
The exact route on each backend was `.\hectl.bat agent start -StartupScene
Assets/Scenes/MainMenu.he -GraphicsApi <DX12|Vulkan> -StrictGraphicsApi
-RenderValidation`, then
`& .\Tools\Agent\hectl.ps1 agent call -Method render.texture.burstProbe
-ParamsJson (@{path='Assets/UI/Icons/Survivor/Characters/azura.png';count=48}
| ConvertTo-Json -Compress)`, followed by `agent step` and `agent stop` in a
`finally` block; `HE_TEXTURE_UPLOAD_TIMINGS=1` was set for the session.
This is `runtime-manual` PASS for the ready/lease path. Explicit ready leases
can prepare deferred work on the owner, so this result does not measure the
frame-paced asynchronous drain of a sustained burst.
After hardening exception cleanup to remove each handle from the temporary
list before release, an owned DX12 session submitted 48 variants of a
nonexistent PNG. The command returned the expected failure. In the same
session, 48 valid variants then returned `ready=48, shared=48`, with a clean
agent stop and no stderr. This is `runtime-manual` PASS for failed-decode
cleanup followed by reuse, in
`out/agent/sessions/20260927-114023-819-ef3340dd58bc/`. The final narrow
cleanup change was compiled by that session's optimized HGame build;
Debug Game/Editor and optimized Editor builds listed below predate it.

### Frame-paced texture burst without ready leases (2026-09-27)

An Agent-only `render.texture.streamingProbe` has `begin`, `status`, and `end`
actions. `begin` holds 48 async handles without a ready lease; `status`
inspects only the published resource dimensions, which become nonzero in
`TextureManager::PublishCompletedSlot` after its GPU event query; `end`
releases all leases. Application cleanup also releases an active probe before
renderer shutdown. The probe is limited to 96 variants and compiled only
with `HE_DEV_AGENT_RUNTIME`.

With `HE_TEXTURE_UPLOAD_TIMINGS=1`, both optimized DX12 and Vulkan Agent
sessions returned `ready=0, pending=48` at `begin`; after the application
rendered normally, the first later status call returned `ready=48, pending=0`
and `end` returned the same. No `bRequireReady` request occurred. Final logs
reported `peakPending=48`, `ownerClaims=0`, `globalWaits=1`, and zero pending;
stderr was empty. This is `runtime-manual` PASS for frame-boundary publication
and natural queue draining on two real backends. The script slept 100 ms
before each status call, so that poll interval cannot establish exact
request-to-ready latency. The aggregate logged P95 also includes startup
textures. Sessions:
`out/agent/sessions/20260927-114603-884-62529e434681/` (DX12) and
`out/agent/sessions/20260927-114634-824-2bd6d584a9ff/` (Vulkan).

A separate DX12 session began 48 requests, ended the probe with 8 still
pending, immediately began 48 again, and stopped the application while that
second batch remained owned. The stop returned normally and stderr was empty.
This is `runtime-manual` PASS for pending cancellation, reuse, and application
shutdown, in `out/agent/sessions/20260927-114725-750-3d27adca5ecf/`.
The exact route for each backend was `.\hectl.bat agent start -StartupScene
Assets/Scenes/MainMenu.he -GraphicsApi <DX12|Vulkan> -StrictGraphicsApi
-RenderValidation`, then `& .\Tools\Agent\hectl.ps1 agent call -Method
render.texture.streamingProbe -ParamsJson $json` with `$json` made by
`@{action='begin';path='Assets/UI/Icons/Survivor/Characters/azura.png';count=48}
| ConvertTo-Json -Compress`; status and end used action-only JSON objects.
Each owned session ended with `.\hectl.bat agent stop` in `finally`.
`.\hectl.bat game build -Configuration RelWithDebInfo` and
`.\hectl.bat editor build -Configuration RelWithDebInfo` passed on this source
(`build-only`). Debug Game/Editor builds remain to be rerun after this change.

The same frame-paced probe was repeated with the project's approximately
2 MB `Assets/UI/Backgrounds/StarbloomMenuGarden.png`, requesting 48
maximum-dimension variants. Both DX12 and Vulkan started at 48 pending and
reached 48 ready after seven status polls spaced by 250 ms. During the run,
texture logs sampled `preparing=32` and `deferred=16`, demonstrating that the
32-job queue filled and 16 requests waited for admission. Both ended with
`pending=0`, `ownerClaims=0`, `globalWaits=1`, and empty stderr. Cumulative
`cpuPrepareMs` reached 2824.669 (DX12) and 2815.861 (Vulkan), including
startup textures; these are summed worker CPU times, not one frame's duration
or a paired performance speedup. The polls bound observation to roughly
250 ms intervals plus command latency, so exact per-texture latency cannot be
inferred. Prepared-byte budget stalls remained zero because each output was
downscaled; the separate state tests cover its threshold behavior. Sessions:
`out/agent/sessions/20260927-115110-913-b1610775cd0e/` (DX12) and
`out/agent/sessions/20260927-115154-810-f56e0b190702/` (Vulkan).
The command sequence above was unchanged except that the begin JSON used
`path='Assets/UI/Backgrounds/StarbloomMenuGarden.png'`; status calls were
spaced by `Start-Sleep -Milliseconds 250` until ready, then end released the
handles.

The prepared-byte gate was then tested with a generated 512×512 RGBA PNG,
`Assets/TestData/TextureUpload/PreparationBudget.png` (1,820 bytes on disk,
1 MiB decoded). `Tools/Agent/GenerateTextureUploadFixtures.py` reproduces it.
The streaming probe began 48 cache-distinct requests with
`baseDimension=512`, while `HE_TEXTURE_PREPARED_BUDGET_MIB=1` and
`HE_TEXTURE_UPLOAD_TIMINGS=1` were set. Both DX12 and Vulkan went from
`ready=0, pending=48` to `ready=48, pending=0` without ready leases. Each
logged `decodeBudgetStalls=47`, `peakPending=48`, `preparing=32`,
`pendingBytes` up to 1,048,576, `ownerClaims=0`, and `globalWaits=1`.
Session stderr was empty. This is `runtime-manual` PASS for real-device
prepared-byte backpressure and eventual publication, not a measurement of
memory peak or input latency. The two 100 ms status polls only bound the
observation interval. Sessions:
`out/agent/sessions/20260927-115710-978-fc02dfd98378/` (DX12) and
`out/agent/sessions/20260927-115806-087-ddcf9aa62c8e/` (Vulkan). The exact
start command was the DX12/Vulkan route above, with the two environment
variables set before launch; begin JSON was
`@{action='begin';path='Assets/TestData/TextureUpload/PreparationBudget.png';
count=48;baseDimension=512} | ConvertTo-Json -Compress`, followed by status
at 100 ms intervals, end, and stop in `finally`.
The final `baseDimension` probe source passed
`.\hectl.bat game build -Configuration RelWithDebInfo` and
`.\hectl.bat editor build -Configuration RelWithDebInfo` (`build-only`).

Before the streaming-probe addition, the source passed
`.\hectl.bat game build -Configuration RelWithDebInfo`
and `.\hectl.bat editor build -Configuration RelWithDebInfo` (`build-only`),
`.\hectl.bat game build -Configuration Debug`, and
`.\hectl.bat editor build -Configuration Debug` (`build-only`), and
`.\hectl.bat test TextureUploadState` (`unit-test`, Debug target). After the
Debug test passed, its idle, unprotected `out/agent/tests-debug` build cache
was previewed and removed with `.\hectl.bat clean cache -CachePath
out/agent/tests-debug -WhatIf --json` and the same command without `-WhatIf`.
The cleanup reported 6,380,669,462 logical file bytes deleted; test logs
remain under `out/agent/logs`.
After the Debug Game and Editor builds passed, their idle caches were also
previewed and removed with `.\hectl.bat clean cache -CachePath
out/agent/<game-debug|editor-debug> -WhatIf --json`, followed by the same
commands without `-WhatIf`. The collector reported 6,292,593,480 and
7,233,814,970 logical file bytes deleted, with no failures or residual
retired directories. C: free space rose from 6.86 to 19.47 GiB for this
last pair of removals (12.61 GiB actual disk-space gain); the successful
build logs remain in `out/agent/logs`.

## Reproducible frame sample

Windows x64, Intel Core i7-11800H (8 cores/16 logical), NVIDIA RTX 3060
Laptop GPU (5994 MiB), DX12, 1280×720, default `Project` game,
`MainGame.he`, capture automation fixed at 1/60 second. The level contained
roughly 430 entities. Two 120-frame agent steps ran in one pinned process for
each build. CSV frame IDs 120–450 were selected to exclude startup and late
control frames. GPU times are the renderer's GPU frame span; CPU times are the
frame wall clock. This is one run per build, not a confidence interval or proof
that individual code changes caused the difference.

| Sample | CPU P50/P95/P99/max (ms) | >16.667 ms | GPU P50/P95/P99 (ms) | Update P50/P95/P99 (ms) |
| --- | --- | ---: | --- | --- |
| Before, `baseline-main-game-2.csv` | 7.423/13.579/22.737/50.418 | 7/331 | 2.484/3.017/3.114 | 1.316/2.493/6.515 |
| After, `post-main-game-1.csv` | 7.075/11.745/15.298/27.264 | 1/331 | 2.358/2.894/3.024 | 1.225/2.036/4.501 |

The pre-change game executable SHA-256 was
`74557BF61AFE5E46F373CA09E4D443674AA7CD4CC8466CFED12D2749EF034A85`;
the measured post-change executable was
`CBEF44824C44CADAD570EB7AE7F0E55B8922109605279555AC82E1DE25FD52C1`.
The post-change source has since received further correctness edits, so the
sample must be repeated for final attribution. Peak reported local GPU memory
was 587.6 MiB before and 586.7 MiB after. CPU process memory, input-to-display
latency, resource queue depth, and texture upload completion latency were not
captured by these CSVs. The scene is a regular gameplay sample, not a dense
collision or active scene-streaming benchmark.

## Dense collision fixture: controlled two-run comparison

A detached baseline worktree at commit `8b8070e5` and the final working tree
both ran the project's `HE_SURVIVOR_CAPTURE_SCENARIO=region_4_final` fixture.
Both logs confirmed a 192/192 monster prefill and completion at frame 500.
The scene's canonical Git blob was the same (`7ff67d13402a5199e10d112a1b92533ad256febc`),
and both render-stat sidecars had identical requested/effective settings with
hash `b691fb04ff5498d9`. Hardware was the i7-11800H/RTX 3060 laptop above;
DX12, validation, 1280×720, default quality, fixed automation dt 0.0166667.
Each run used `HE_SURVIVOR_CAPTURE_SCENARIO=region_4_final` with
`.\hectl.bat game check -Frames 500 -PerformanceWarmupFrames 120
-StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi
-RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667
-RenderStats -RenderStatsPath <case>.csv -TimeoutSeconds 240`.
The order was baseline, final, final, baseline. Each row is the same 330
frame-ID 120–450 slice, in milliseconds.
Baseline CSVs, sidecars, and logs are retained in `out/agent/baseline-dense/`;
final CSVs are `out/agent/final-region-final.csv` and
`out/agent/final-region-final-2.csv`. The temporary baseline worktrees were
removed after copying and hash-checking the evidence.

| Run | CPU P50/P95/P99/max | >16.667 ms | GPU P50/P95/P99 | Level update P50/P95/P99 |
| --- | --- | ---: | --- | --- |
| Baseline 1 | 19.280/51.315/54.952/80.051 | 208/330 | 6.552/7.649/8.145 | 6.776/13.279/36.372 |
| Final 1 | 19.656/50.702/56.048/78.416 | 215/330 | 6.578/8.235/8.994 | 6.716/13.238/18.684 |
| Final 2 | 19.279/44.229/52.409/70.103 | 208/330 | 6.234/8.350/9.174 | 6.640/11.584/16.755 |
| Baseline 2 | 19.590/47.516/53.355/71.438 | 209/330 | 6.619/8.461/9.290 | 6.862/12.747/18.717 |

The CPU distributions overlap and the fixture still misses 60 FPS on most
sampled frames. The data do not show a reliable whole-frame improvement. GPU
local-memory peaks were 647.3/642.6/606.9/603.9 MiB in run order;
CPU process-memory peak was not recorded. The baseline executable SHA-256 was
`C79EA587DF1E8D9C0F8D4A694F3906DD40DEDEFEC5E6AB6246C2397CB9AA05FD`;
the final executable was
`E574DA7E490DCEEDF306CFDCAD65E548BE1DEE29E77B89B950A51F4121C83D44`.
This comparison does not isolate any one code change.

## Completed checks

The following checks passed before the final Debug rebuild; each command's
output and build log remain under `out/agent/logs` where applicable.

| Evidence | Command | Result |
| --- | --- | --- |
| unit-test | `.\hectl.bat test DelegateBuffer` | PASS |
| unit-test | `.\hectl.bat test FixedSimulationClock` | PASS |
| unit-test | `.\hectl.bat test TaskScheduler` | PASS, including after enqueue/constructor exception cleanup edit |
| unit-test | `.\hectl.bat test SurvivorTargetingBatch` | PASS |
| unit-test | `.\hectl.bat test MeshCpuDecode` | PASS |
| unit-test | `.\hectl.bat test PhysicsScriptContact` | PASS |
| unit-test | `.\hectl.bat test MaterialDescriptorPaging` | PASS |
| unit-test | `.\hectl.bat test RuntimeResourceRegistryLifecycle` | PASS |
| build-only | `.\hectl.bat editor build` | PASS, RelWithDebInfo |
| build-only | `.\hectl.bat game build -Configuration Debug` | PASS after adding `/bigobj` to DevAgentRuntime |
| build-only | `.\hectl.bat editor build -Configuration Debug` | PASS after adding `/bigobj` to Editor |
| runtime-manual | `.\hectl.bat game check -Frames 180 -GraphicsApi DX12 -StrictGraphicsApi -RenderStats -RenderStatsPath out/agent/post-main-check-2.csv -Resolution 1280x720` | PASS |
| runtime-manual | `.\hectl.bat game check -Frames 120 -GraphicsApi Vulkan -StrictGraphicsApi -Resolution 1280x720 -RenderValidation` | PASS, validation layer enabled |
| runtime-manual | `.\hectl.bat editor check -Frames 60 -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation` | PASS |

Pinned DX12 Agent Runtime sessions also switched `MainGame` → `MainMenu`
(scene generation 2 → 5, 426 → 7 entities) and `MainMenu` → `MainGame`
(generation 2 → 5, 425 entities after 120 stepped frames). Sessions were
stopped in `finally`. The latter continued to 480 stepped frames without a
reported device or validation error. These are runtime observations, not
automated assertions of every resource lifetime transition.

After the upload-statistics edit, `.\hectl.bat game check -Frames 180
-GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -Resolution 1280x720`
passed again. A separate `HE_TEXTURE_UPLOAD_TIMINGS=1` pinned DX12 run
started `MainGame.he`, stepped 120 plus 60 frames, captured
`out/agent/sessions/20260926-130543-190-79525a32fab4/mailbox/captures/tasking-main-game.png`,
and stopped the owned session. A preview of the 1280×720 capture was visually
inspected: terrain, player, enemies, and UI rendered normally. The run logged
53 texture requests, 9 completed unique uploads, zero pending bytes, and
22,932,132 submitted payload bytes. The `TextureManager` instrumented
device-wide wait count remained one (the startup default-texture upload),
so no additional wait through that manager was observed in those 180 game
steps. Shader hot-reload's separate idle barrier is outside this counter and
was not exercised. Mean request-to-publication latency for
the nine uploads was 118.854 ms; it includes the current synchronous CPU
decode/mip work and is not a streaming tail-latency measurement.

The project-provided dense fixture was run with
`HE_SURVIVOR_CAPTURE_SCENARIO=region_4_final`,
`HE_RUNTIME_SYSTEM_TIMINGS=2`, `HE_TASK_TIMINGS=1`, and
`HE_TEXTURE_UPLOAD_TIMINGS=1` in a pinned DX12 Agent session with validation.
The session used `agent start -StartupScene Assets/Scenes/MainGame.he
-Resolution 1280x720 -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation
-RenderStats -RenderStatsPath out/agent/post-region-4-final.csv`, two
`agent step -AgentStepFrames 120` calls, `agent capture`, and `agent stop`
in `finally`. The fixture log reported a prefill of 192/192 monsters;
after 240 steps the world had 752 entities. The inspected capture showed
191 nearby monsters and complete scenery/UI. The frame-ID 60–240 CSV slice
contains 181 observations: CPU P50/P95/P99/max 22.738/45.716/61.139/174.182
ms, with 175 frames above 16.667 ms; GPU span 6.756/7.855/8.369/8.429 ms;
Level update 7.625/17.809/39.210/159.971 ms. CPU runtime render P50/P95/P99
was 13.398/23.120/25.651 ms. Runtime system timing logs show occasional
PythonScriptSystem 158 ms and AnimationSystem 144 ms frames. These are
observations from one post-change Agent fixture run without a paired baseline;
the controlled comparison above supersedes that limitation. The 60 FPS CPU budget
was not met in this fixture. The project's 320-monster cap is far below the
32768-sample threshold of the parallel targeting batch, so that parallel
path did not contribute to this gameplay run.

In that dense CSV slice (frames 60–240), `cpuAnimationMs` was
0.219/0.592/30.667 ms at P50/P95/P99, while `cpuDeformationMs` was
0.022/0.029/0.051 ms. `deformationInstanceUpdates` and
`deformationVertexUpdates` were zero throughout the slice. A later serial
dense Agent run reached 753 entities after 240 fixed steps and logged
isolated AnimationSystem durations of 18–83 ms while RenderSystem3D stayed
around 1.5–2.8 ms in those samples. These timings do not prove the cause
of the animation spikes. `RenderWorldExtractor` never crossed its 11 ms
warning threshold in that run, so the pure skinned-bounds expansion there
is not yet a measured parallelization target. Its EnTT reads, historical
caches and GPU-index assignment remain serial.

`HE_ANIMATION_INSTANCE_TIMINGS=1` was added for an owned DX12 dense run with
the same 240 fixed steps. Nine entity ticks exceeded the 2 ms logging
threshold (2.598–7.784 ms); all nine had `loadedBefore=false` and
`loadedAfter=true`. No already-loaded entity tick crossed that threshold.
This points to first-use animation initialization as the measured spike
site in that run. The initialization path reads and parses controller/clip
assets and updates a shared cache, so it cannot be placed on a general
frame worker unchanged. An asynchronous resource preparation path or startup
prefetch is still required before claiming a frame-critical parallel win.
The instrumentation-only edit passed optimized and Debug Game and Editor
builds. Its log records are opt-in and did not run in the earlier frame
statistics capture.

After moving controller and `.hanim` reads/JSON parsing to the existing
resource workers, the same 240-step dense fixture reached 753 entities,
produced a visually inspected 1280×720 capture with enemies/player/UI, and
logged zero animation entity ticks above the 2 ms threshold. A same-build
serial switch (`HE_ANIMATION_PREPARE_ASYNC=0`) retained the spikes. In two
serial/async run pairs at 1280×720 DX12/NVRHI validation, the first pair's
animation CPU P99 was 7.966/1.254 ms and the second pair's was 5.893/0.783
ms; the second pair's whole-frame CPU P99 was 47.076/47.359 ms. These are
separate runs with 214–240 captured active rows, not a paired frame-by-frame
experiment, and the last ~24 simulation steps were absent from the quick-stop
CSV in the second pair. The first async statistics session accumulated over
8000 paused display frames before stop and returned exit 1 during shutdown;
its statistics CSV was readable but this run is not a clean lifecycle pass.
A prompt repeat completed with exit zero. This evidence supports removal of
the measured animation first-use spike, not an overall FPS improvement.

After the second animation preparation edit, an owned DX12 dense scene switch
from `MainGame.he` at generation two to `MainMenu.he` at generation five
completed with seven menu entities and exit zero. Final-source optimized
Game, Debug Game, optimized Editor, Debug Editor, and Debug HEngineTests builds
passed. The `FixedSimulationClock`, `RuntimeResourceRegistryLifecycle`,
`RuntimeResourceWarmup`, `AnimationPoseContract`, and
`SurvivorCharacterStartState` tests passed. A mistaken invocation of
`test RuntimeResourceRegistry` returned UNKNOWN; the actual registered test
name is `RuntimeResourceRegistryLifecycle`, which passed.

## CPU texture preparation continuation

The later texture path moves ordinary-image decode, resize, alpha processing,
mip generation, HDR proposal creation, and DDS file reading to a dedicated
worker. The NVRHI calls remain on the owner thread. The headless
`.\hectl.bat test TextureUploadState` and existing
`.\hectl.bat test DdsTextureData` passed in Debug. An optimized Game build,
DX12 180-frame `MainGame.he` check, and Vulkan 120-frame `MainGame.he` check
passed after the first async integration. The subsequent state-machine edit
and live-reload integration passed optimized Game builds. The latter also
passed a DX12 Agent device run, detailed below. Current Debug and optimized
build results are recorded next.

After the path-normalized reload change, `game build` in RelWithDebInfo and
Debug, `editor build` in RelWithDebInfo and Debug, and Debug
`TextureUploadState`, `DdsTextureData`, `DelegateBuffer`, `TaskScheduler`,
`FixedSimulationClock`, and `PhysicsScriptContact` tests all passed. The
first Debug Editor attempt failed at final link (`LNK1116`, disk error 112)
when C: had 0.12 GiB free. NTFS compression of the new `out/agent/editor-debug`
cache reduced its 5,995,357,130 logical bytes to 2,092,221,738 stored bytes;
the subsequent exact Debug Editor build passed. The repository's normal
`clean stale` scan was interrupted after several minutes; `clean cache`
refused an old external project cache because it contains historical session
evidence. An attempted targeted deletion of build portions was rejected by
automatic approval review, so no historical cache was manually deleted.

The final texture retirement byte-accounting edit passed optimized and
Debug `game build`, optimized and Debug `editor build`, and Debug
`test TextureUploadState`. The six-test regression set above and both
real-device reload runs preceded that accounting-only edit. Its final source
also passed `game check -Frames 90 -StartupScene Assets/Scenes/MainGame.he
-GraphicsApi DX12|Vulkan -StrictGraphicsApi -Resolution 1280x720
-RenderValidation` on both APIs (run as two separate commands).

With `HE_TEXTURE_UPLOAD_TIMINGS=1`, the later DX12 `MainGame.he` check logged
53 texture requests, nine completed unique uploads, no failures, peak pending
count five, peak prepared CPU storage 5,596,496 bytes, 22,932,132 submitted
payload bytes, 165.396 ms cumulative CPU preparation time on the dedicated
worker, and one `TextureManager` device-wide wait from default-texture startup.
The mean request-to-publication latency was 122.832 ms. These counters are
from one run; they do not give P95/P99 streaming latency or isolate GPU upload
time. A later owned Agent switch from `MainGame` to `MainMenu` completed at
scene generation five with seven menu entities; its log ended with no pending
preparation, no failed upload, and the session stopped cleanly. This is a
runtime cancellation observation, not a deterministic failure/retry assertion.

Live texture reload was exercised on the RTX 3060 Laptop GPU using an owned
DX12/NVRHI-validation Agent session. `render.texture.reload` with an empty
path queued five resident assets; 90 fixed simulation steps completed and the
1280×720 PNG capture was visually inspected with complete scenery and UI.
The actual resident asset paths used Windows separators, so an initial
forward-slash exact request returned `queued=0`. After path normalization,
`render.texture.reload {"path":"Assets/Textures/VerdantRift/StylizedMacroAlbedo.png"}`
returned `queued=1` twice, with 60 steps between calls. A subsequent
`runtime.scene.load` of `Assets/Scenes/MainMenu.he` completed at generation
five with seven entities while the second reload was in progress; the Agent
session stopped cleanly. This validates queue admission, frame publication
and live scene-transition behavior on DX12. No native D3D12 debug layer was
available; NVRHI validation was enabled. The test did not alter source pixels
or prove that a visibly different texture was displayed. A separate Vulkan
Agent run with validation enabled returned `queued=1` for the same exact
path, completed 60 fixed steps, returned `queued=1` again, and produced an
inspected 1280×720 capture with complete scenery and UI. Its owned session
stopped normally. This earlier check did not change source pixels; the later
comparison below did.

The first Debug Game attempt failed with MSVC C1128 in `DevAgentRuntime.cpp`;
the target-local `/bigobj` CMake change fixed it. An attempted `RenderRhiContract`
test command failed because that test name does not exist; the relevant
`MaterialDescriptorPaging` test passed. Neither failed invocation is counted
as a product regression.

A later owner-thread retry fix lets `RequestReload` accept a live `Failed`
texture slot while preserving its handle and any old published texture. The
Debug `.\hectl.bat test TextureUploadState` passed with a Failed → Retry →
Submitted → Ready reload sequence. On DX12/NVRHI validation, an owned Agent
session first loaded `StylizedMacroAlbedo.png`, then temporarily moved that
clean project asset to an `out/agent` backup and requested an exact reload.
The request returned `queued=1`; the log recorded one decode failure while
completed uploads stayed at nine. After the asset was restored, the second
request returned `queued=1` and completed uploads rose to ten with pending
count zero. The only device-wide wait counter remained at its startup value
of one. The Agent stopped normally and SHA-256 verification confirmed the
project asset was restored byte for byte. This tests failure and retry with a
real device; it does not include a pixel-change display assertion. The same
owned failure/restore/retry sequence on Vulkan validation also returned
`queued=1` twice, logged one decode failure, reached ten completed uploads
with pending count zero, retained one startup global wait, stopped normally,
and restored the asset with matching SHA-256.
After the retry edit, `.\hectl.bat game build -Configuration Debug`,
`.\hectl.bat editor build`, and `.\hectl.bat editor build -Configuration Debug`
all passed. The optimized Game build was included in both owned Agent starts.

A separate live pixel-change test ran on both DX12 and Vulkan with NVRHI
validation at 1280×720. After the scene loaded, an owned Agent capture saved
`texture-before.png`. The 16×16 `StylizedMacroAlbedo.png` used by twelve
scene materials was temporarily replaced with the existing 16×16 blue
`StylizedMacroNormal.png`; `render.texture.reload` returned `queued=1`.
After three fixed steps, `texture-swapped.png` visibly showed blue-purple
terrain and structures. Restoring the original bytes and requesting another
reload (`queued=1`) returned the scenery to its original colors in
`texture-restored.png`. Each original PNG was 1280×720; previews were
inspected sequentially. DX12 mean absolute RGB differences were
44.338/46.804/6.409 for before versus swapped and 0.709/0.650/0.609 for
before versus restored. Vulkan gave 44.376/46.833/6.456 and
0.561/0.514/0.503 respectively. The DX12 captures are in session
`20260926-184820-415-1aaab2e8cee3`; Vulkan captures are in
`20260926-185055-389-1d3a65cc11a2`, each under `mailbox/captures`.
Vulkan logs reported completed uploads 9→10→11, pending zero and no new
device-wide wait beyond the startup count of one. Both sessions stopped
normally, temporary backups were removed, and the project asset hash
matched its original SHA-256. This validates visible publication and
restoration, not all material color-space/mip combinations.

### BC3, BC5 and BC7 device coverage (2026-09-27)

`python Tools/Agent/GenerateTextureUploadFixtures.py` creates 16×16 DX10 DDS
fixtures with five offline mip levels under
`Project/Assets/TestData/TextureUpload/`. The script uses the checked-in
`texconv.exe` with `-nogpu`; two consecutive generations produced identical
DDS SHA-256 hashes. The four DDS fixtures are 516 bytes each (BC3 color,
BC5 mild/strong normals, BC7 color). An optimized Game build passed
with `.\hectl.bat game build` (`build-only`).

Owned `MainMenu.he` Agent sessions on DX12 and Vulkan, each with
`-StrictGraphicsApi -RenderValidation`, called `render.texture.readyProbe`
for BC3 sRGB 16×16/5 mip, BC3 linear capped to 8×8/4 mip, BC5 linear
16×16/5 mip, BC7 sRGB 16×16/5 mip, and BC7 linear capped to 8×8/4 mip.
Every call returned `ready=true`, `sharedHandle=true`, and the expected
NVRHI format and dimensions; both sessions stopped with rendering validation
passing. The sessions were `20260927-084449-460-7c01786c0a8e` (DX12) and
`20260927-084513-250-4f26025ec8e2` (Vulkan). This verifies the real-device
upload/publication path and descriptor format, but the probe does not read back
compressed payloads.
Follow-up sessions `20260927-085206-002-624737f49d42` (DX12) and
`20260927-085234-688-3d7e4f964e44` (Vulkan) confirmed `generateMipmaps=false`
publishes only one BC3 or BC7 mip, including the selected 8×8 first mip.
BC5+sRGB was rejected on both backends, and a subsequent BC5+linear request
returned ready with five mips; both sessions stopped with rendering validation
passing. The DX12 PowerShell wrapper reported a false failure after the
successful recovery response because `$LASTEXITCODE` retained the preceding
expected rejection; the Agent's JSON response and stop result were successful.

For shader sampling, the `MeadowLight.hmat` albedo and normal references were
temporarily switched to BC3+BC5 and BC7+BC5. Each backend started an owned
`MainGame.he` session, stepped 30 fixed ticks, captured 1280×720, passed NVRHI
validation, and stopped. A matching capture with the original material was
also taken. BC3+BC5 versus original mean absolute RGB differences were
16.559/18.307/11.602 (DX12) and 16.471/18.231/11.539 (Vulkan). With the
final CPU-encoded BC7+BC5 fixtures they were 16.371/17.809/11.299 and
16.330/17.780/11.270 respectively. BC3+BC5 cross-backend RGB differences
were 0.194/0.188/0.156; final BC7+BC5 cross-backend differences were
0.146/0.140/0.122. The captures are in the following session directories
under `mailbox/captures/`: BC3+BC5 `20260927-084604-335-e1212b12c86c`
and `20260927-084650-996-46df3360947e`; original
`20260927-084840-748-b50eb2fd040f` and
`20260927-084856-227-8d15bae32d18`; final BC7+BC5
`20260927-085011-253-0960374b149d` and
`20260927-085027-255-d02d695e8e2f` (DX12 then Vulkan). Previews were
inspected sequentially. The material was restored byte-for-byte after every
temporary edit; its final SHA-256 is
`FFF20946E53606075FD60BADE3FE87B9F604447CF2E0F535C56C7316556FE36E`.
BC3 alpha was then exercised by changing that material's `alphaMode` from
`OPAQUE` to `MASK` with its existing 0.5 cutoff. DX12 and Vulkan sessions
`20260927-085411-634-3c7607002a40` and
`20260927-085427-017-e66ce3760aa0` each stepped 30 fixed ticks, captured,
passed NVRHI validation, and stopped. The previews show the expected masked
holes in the terrain. Relative to BC3+BC5 OPAQUE, RGB mean absolute differences
were 0.944/2.801/3.572 (DX12) and 0.937/2.795/3.575 (Vulkan), with about
34,700 pixels changing by more than 20 in one channel. The original material
bytes were restored afterward.
The mild BC5 normal fixture changed too few pixels to isolate its effect from
capture variation. A strong BC5 normal fixture was therefore compared against
the original PNG normal while keeping BC3 albedo and OPAQUE mode constant.
DX12/Vulkan sessions `20260927-085732-114-39b173b74ad5` and
`20260927-085747-527-e4b428debafe` passed rendering validation and showed
the expected strong normal-driven lighting change. Relative to the matched
BC3+original-normal captures in `20260927-085552-632-305a967ae288` and
`20260927-085607-992-317a3c9ef380`, RGB mean absolute differences were
2.884/4.978/4.359 and 2.755/4.871/4.265; over 125,000 pixels changed by
more than 10 in a channel on each backend. The backend difference on the
strong-normal output was only 0.051/0.044/0.039. The material was restored.

An attempted compressed staging readback on DX12 caused device removal
(`0x887a0005` when mapping). The experimental probe change was reverted, and
the failed session `20260927-084301-955-0e35a39f0b29` was stopped. The
production compressed upload path was unchanged. Compressed byte-perfect GPU
readback remains unverified.

A real-device DDS fixture was also exercised on DX12 and Vulkan. The
temporary 312-byte legacy DXT1/BC1 file encoded a solid-blue 16×16 texture
with a complete five-level mip chain (184 compressed payload bytes). One
visible `MeadowLight.hmat` material temporarily referenced it. Both owned
Agent sessions started `MainGame.he`, completed 30 fixed steps, captured a
1280×720 image, and stopped normally with NVRHI validation. The inspected
DX12 and Vulkan previews both showed only that material's terrain in blue.
Each run reported 53 texture requests, ten completed uploads, zero failures,
zero pending uploads, 22,932,316 submitted bytes (184 more than the ordinary
scene run), and one device-wide wait from startup. Sessions were
`20260926-190017-483-f6eef39e9574` (DX12) and
`20260926-190118-762-f773ce2d6a36` (Vulkan); captures are under
`mailbox/captures/dds-dx12.png` and `dds-vulkan.png`. The material hash was
verified restored after each run; the temporary DDS was removed. The later
BC3/BC5/BC7 device coverage is recorded above.

After replacing the environment IBL device-wide wait with a Graphics queue
event query, the final source passed optimized Game checks that started the
HDR environment scene `Assets/Scenes/MainGame.he` on both Vulkan and DX12:
`.\hectl.bat game check -Frames 120 -StartupScene Assets/Scenes/MainGame.he
-GraphicsApi Vulkan -StrictGraphicsApi -Resolution 1280x720 -RenderValidation`
and the same command with `DX12`. The Vulkan log confirmed its validation
layer was enabled. The final source also passed `.\hectl.bat game build
-Configuration Debug`, `.\hectl.bat editor build`, and `.\hectl.bat editor
build -Configuration Debug` after that edit. Unit tests preceded this last
SceneRenderer query change; the two device checks cover the changed path.

## Performance caveats and remaining verification

The later texture-queue instrumentation adds a bounded 256-completion
request-to-frame-publication latency window with nearest-rank P50/P95/P99 and
maximum values. `TextureUploadState` passed in Debug, including cancellation
of a waiting job from a full 32-job preparation queue and admission of a
replacement job. The cancellation test uses a caller-locked queue state with
simulated jobs; it does not use a graphics device. After this change, bounded
240-frame optimized Game checks on the RTX 3060 Laptop GPU passed with
NVRHI validation on both DX12 and Vulkan. The PowerShell commands were
`$env:HE_TEXTURE_UPLOAD_TIMINGS='1'; .\hectl.bat game check -Frames 240
-StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12
-StrictGraphicsApi -Resolution 1280x720 -RenderValidation` and the same
command with `-GraphicsApi Vulkan`. Each run had 53 requests, nine completed uploads, zero
pending and failed uploads, and one startup device-wide wait. DX12 recorded
peak pending five, P50 165.949 ms, P95/P99/max 237.110 ms; Vulkan recorded
peak pending five, P50 131.232 ms, P95/P99/max 167.570 ms. These are only
nine startup completions per backend, so the tail numbers do not establish
streaming behavior. The corresponding stdout logs are
`out/agent/logs/run-game-relwithdebinfo-20260926-212111-601-77412.stdout.log`
and `out/agent/logs/run-game-relwithdebinfo-20260926-212222-150-102036.stdout.log`.
An owned DX12 Agent run then queued five live reloads, immediately requested
`MainMenu.he`, completed 60 fixed steps, observed scene generation five with
seven menu entities, and stopped cleanly. Its session was
`20260926-213129-194-59d38dc883d3`; this tests live unload and cancellation
without claiming every queued job was waiting at cancellation time.
After this instrumentation edit, `.\hectl.bat game build -Configuration Debug`,
`.\hectl.bat editor build -Configuration Debug`, and
`.\hectl.bat editor build` passed (the optimized Game build was part of both
bounded checks). `git -c core.safecrlf=false diff --check` passed. Earlier
regression and image tests are described above; they were not repeated for
this statistics and waiting-queue change.
The four final builds temporarily reduced C: free space to 8.3 GiB.
`hectl clean stale --json` was interrupted after its historical scan made no
progress. Managed `hectl clean cache -CachePath ... --json` then removed the
five completed, rebuildable `editor-debug`, `game-debug`,
`editor-relwithdebinfo`, `tests-debug`, and `game-relwithdebinfo` caches;
the tool reported 32,568,925,441 logical bytes deleted. C: free space rose
to 38.67 GiB (about 30.4 GiB physically available again). Logs, captures,
baseline evidence, and historical sessions remain. An external-project cache
could not be removed as one unit because it contains historical session
evidence; an attempted build-only removal was blocked by automatic command
review, so that directory was retained. Other large old directories have
unconfirmed provenance and were retained.

### Sustained live texture reload sampling

After the cache cleanup, an optimized Game Agent start rebuilt HGame and ran
`MainGame.he` at 1280×720 on the RTX 3060 Laptop GPU with
`$env:HE_TEXTURE_UPLOAD_TIMINGS='1'` and `-RenderValidation`. One owned session
used `-GraphicsApi DX12 -StrictGraphicsApi`; the other used
`-GraphicsApi Vulkan -StrictGraphicsApi`. In each paused Agent session, 60
sequential calls to `render.texture.reload` with `{"path":""}` each returned
`queued=5`. The calls were made through
`& .\Tools\Agent\hectl.ps1 agent call -Method render.texture.reload
-ParamsJson (@{path=''} | ConvertTo-Json -Compress)`; Agent call overhead
spaced requests approximately one second apart, so this is a sustained
five-texture-per-call test rather than a simultaneous 300-texture burst.
The sessions were stopped with `.\hectl.bat agent stop`.

Both backends ended at 353 total requests, 309 completed uploads (nine
startup plus 300 reloads), zero pending, zero failures, peak pending five,
358,803,732 submitted payload bytes, and one device-wide wait from startup.
DX12 had 2,706.040 ms cumulative CPU preparation and a 256-completion
rolling latency P50/P95/P99/max of 50.941/71.946/73.597/76.641 ms.
Vulkan had 2,803.410 ms cumulative CPU preparation and
56.220/80.341/82.986/84.688 ms. Logs are under owned sessions
`20260926-222815-717-ae556b2695e3` (DX12) and
`20260926-223256-422-f0a443e2f76d` (Vulkan). DX12 used NVRHI validation;
the native D3D12 debug layer was unavailable. Vulkan reported its validation
layer enabled. This samples the current implementation on one machine, not
a baseline comparison or input-to-display latency measurement. The test did
not saturate the 32-job CPU queue or prove rendering output after every reload;
the separate live pixel-change tests above cover visible publication.

### Ray-tracing material texture publication

`RayTracingScene::ResolveTexture` now requests managed material textures
asynchronously. Its table holds the default texture until `TextureManager`
publishes the upload at a frame boundary. `RayTracingScene::Prepare` then
refreshes the native texture table, increments its generation and rebuilds
the material record; the latter updates the BC5 normal-map flag from the
published format. Cache insertion rolls back the newly acquired lease on
allocation failure. A scene-generation change or explicit clear now releases
the previous scene's managed leases using the last submitted frame for GPU
retirement and clears the native table. At this stage, environment IBL and
ImGui still used explicit-ready requests; the environment path was changed
later, as documented below.

After the final exception rollback edit, Debug
`.\hectl.bat test HardwareRayTracing` and optimized `.\hectl.bat game build`
passed. The earlier edit passed 90-frame `MainGame.he` Game checks on DX12
and Vulkan with `-StrictGraphicsApi -RenderValidation`; these raster runs do
not prove the ray-tracing material path. A DX12 Editor capture before the
scene-release edit used `.\hectl.bat editor capture -Frames 240 -Resolution 1280x720
-StartupScene Assets/Scenes/MainGame.he -StartupViewportMode
he.viewport.path-tracing -GraphicsApi DX12 -StrictGraphicsApi
-RenderValidation -CapturePath out/agent/rt-async-main-repeat.pfm
-TimeoutSeconds 300`. It passed and its inspected HDR preview showed complete
terrain and scenery. With `HE_RT_TEXTURE_PUBLICATIONS=1`, the device log
recorded published material-texture batches of 2, 1 and 1, advancing the
table generation to 12. A 180-frame capture and two additional
180-frame repeats also passed at the process level. Of six 180/240-frame
HDR captures inspected by row statistics, five had no dark rows and a median
adjacent-row residual near 0.001; one 180-frame capture had 306 dark rows
and residual 0.139, visibly appearing as horizontal bands. Its capture
command still returned success and the log did not report a validation error.
The 240-frame repeat was clean. This intermittent artifact remains
unattributed; these runs do not establish flawless path-tracing output or
prove whether the cause is capture timing, progressive rendering, or texture
publication. Vulkan path-tracing publication was not exercised.

After the scene-release edit, Debug `.\hectl.bat test HardwareRayTracing`,
optimized `.\hectl.bat game build`, and a 240-frame optimized Editor DX12
path-tracing capture passed. The last capture was
`out/agent/rt-async-scene-release.pfm`; its 1280×720 pixels were all finite,
its row-residual P50/P95 was 0.0012/0.0044 with zero dark rows, and its
preview showed complete scenery. The corresponding Editor build and run are
included in the capture command above with only `-CapturePath` changed.
An in-process Editor scene switch was not exercised after this lease edit;
that lifecycle path remains verified by code review and compilation, not
device-level scene-transition evidence.
After moving scene-generation lease release ahead of the publication scan,
the final source passed Debug `.\hectl.bat test HardwareRayTracing`, optimized
`.\hectl.bat game build`, and
`.\hectl.bat editor check -Frames 90 -Resolution 1280x720
-StartupScene Assets/Scenes/MainGame.he -StartupViewportMode
he.viewport.path-tracing -GraphicsApi DX12 -StrictGraphicsApi
-RenderValidation -TimeoutSeconds 300`. The last command includes an
optimized Editor build and bounded device run; it did not capture an image.

### Asynchronous HDR environment and IBL preparation

The later `SceneRenderer::PrepareEnvironment` change requests source HDRs
asynchronously, starts IBL compute only after `TextureManager` publishes the
source, and polls a Graphics event query in subsequent frames. The source
lease, command list, and compute binding sets remain owned until that query
completes. A scene-generation change cancels unused source preparation,
marks submitted IBL as abandoned until its query completes, and retires
unused ready maps after the last submitted frame. Normal preparation no
longer calls `waitEventQuery` or `waitForIdle`.

On the RTX 3060 Laptop GPU, optimized Game checks passed separately on DX12
and Vulkan with `HE_TEXTURE_UPLOAD_TIMINGS=1` and
`.\hectl.bat game check -Frames 240 -StartupScene
Assets/Scenes/MainGame.he -GraphicsApi <DX12 or Vulkan>
-StrictGraphicsApi -Resolution 1280x720 -RenderValidation -RenderStats
-RenderStatsPath out/agent/async-env-<backend>.csv` (one backend per run).
Each final render summary reported `environmentReady=1`; each upload log
reported 53 requests, nine completions, zero pending and failed uploads,
and `globalWaits=1` for default-texture startup. The observed CPU frame
distributions are not paired with a baseline and do not prove a frame-time
gain. Matching 240-frame `game capture` commands with
`-CapturePath out/agent/async-env-dx12.png` and
`out/agent/async-env-vulkan.png` passed; their previews were inspected
sequentially and showed complete scenery, sky, player, and UI.

Owned Agent sessions on both backends loaded `MainGame.he` → `MainMenu.he`
→ `MainGame.he`, stepping 30 and 60 fixed frames after the respective
requests. Both observed scene generations five and eight, seven menu and
430 returned-game entities, zero final pending/failed uploads, and one
startup global wait. The DX12 return frame was captured and inspected.
Sessions `20260927-001417-416-a4dc7740b941` (DX12) and
`20260927-001912-320-045bd591526c` (Vulkan) stopped cleanly. These runs
exercise ready-map retirement and subsequent scene preparation; they do not
prove that a particular HDR decode or IBL query was still in flight at the
instant of unload.
After the cancellation and retirement edit, final-source Debug
`.\hectl.bat test TextureUploadState` passed. Final-source
`.\hectl.bat editor check -Frames 90 -Resolution 1280x720
-StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12
-StrictGraphicsApi -RenderValidation -TimeoutSeconds 180` passed its
optimized Editor build and bounded device run. The DX12/Vulkan scene-switch
sessions above used the cancellation and retirement edit.


An additional frame-task audit measured two candidate renderer computations
on DX12/NVRHI validation. `MainGame.he` had 138 render objects but only one
unique static SubMesh eligible for the ray-tracing content revision; its
serial revision stage measured 0.285/0.287 ms at frames 60/120. A prototype
pre-scan for parallel hashing did no worker work and raised those observations
to 0.536/0.510 ms. The prototype was removed; a follow-up with the serial
path measured 0.283/0.310 ms. In `region_4_final`, frames 60/120 had
328/330 render objects and zero eligible static SubMeshes. A second prototype
split skinned-bounds expansion after RenderWorld extraction. The dense scene
had 198/199 skinned objects with 1006/1013 total bone matrices, yet the whole
serial expansion took only 0.025/0.022 ms. That split was also removed.
Neither candidate provides evidence for a useful frame-critical task graph
integration on the current fixture. `HE_RT_REVISION_TIMINGS=1` remains as a
small diagnostic for revision count and time.

The Debug `RenderWorldExtractorMeshPath` test initially failed at a directional
light serialization assertion. The test source lacked that component's
generated serializer declaration, so its encoded value was null. Adding the
generated include made the exact test PASS; no generated file was hand-edited.
After removing both unhelpful parallel prototypes, final-source
`.\hectl.bat game check -Frames 90 -StartupScene Assets/Scenes/MainGame.he
-GraphicsApi DX12 -StrictGraphicsApi -Resolution 1280x720 -RenderValidation`
passed its optimized build and bounded run. `git diff --check` passed.

The later fixed-input edit snapshots Move, Sprint, Crouch and Jump at the
display-frame boundary. A `FixedActionLatch` regression in
`.\hectl.bat test FixedSimulationClock` passed in Debug: short presses survive
a zero-tick frame, a held press spans multiple ticks, and pause clears the
pending edge. Final-source `.\hectl.bat game check -Frames 90 -StartupScene
Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Resolution
1280x720 -RenderValidation` passed (build and bounded device run). Final-source
optimized and Debug Game builds and optimized and Debug Editor builds passed.
The bounded device run did not replay a jump command, so the input behavior
is verified by the latch unit test and compilation rather than gameplay
interaction capture.

The 100k-sample targeting microbenchmark in a Debug test process reported
parallel P50 18.721 ms versus serial P50 21.896 ms. The final optimized
test process, with 20 repetitions and scheduler instrumentation enabled,
reported parallel P50/P95 1.673/4.259 ms versus serial 2.087/4.560 ms.
The parallel graph reported a representative 0.377 ms critical execution
chain and 0.054 ms wall-minus-chain remainder. A 9000-sample trial
regressed (parallel P95 5.881 ms versus serial 4.258 ms); therefore parallel
partitioning is enabled only from 32768 samples. This is a microbenchmark,
not a measured frame-time gain. Task scheduler reports queue, execution, and
wall times when `HE_TASK_TIMINGS=1`.

The animation runtime's morph vertex calculation is the first frame-path
TaskScheduler user. It runs only for meshes with at least 65536 vertices and
two active morph targets; 8192-vertex ranges have disjoint writes and join
before mesh bounds/GPU publication. Debug and optimized
`AnimationPoseContract` passed for 16384 and 65536 vertices, checking every
position, normal, tangent, and bitangent. With `HE_MORPH_BENCHMARK=1`, the
same optimized 65536-vertex, two-target test process measured 20 updates per
run. Two alternating serial (`HE_MORPH_TASKS_SERIAL=1`) runs reported P50/P95
3.018/3.301 and 3.001/3.196 ms; two parallel runs reported 1.587/1.654 and
1.595/1.684 ms. The first optimized parallel run reported 1.686/1.866 ms.
The sample times include base-stream reset and bounds computation. They show
a benefit for this synthetic workload on this machine; actual game frame
benefit depends on whether a visible character has a morph workload above
the threshold. With `HE_TASK_TIMINGS=1`, a representative eight-partition
update logged 2.264 ms aggregate task execution, 3.259 ms aggregate queue
time, 1.063 ms graph wall time, 0.849 ms measured critical path, 0.213 ms
wall-minus-critical residual, and 71.0% aggregate worker utilization.

The full frame deformation entry point has an additional regression with a
65536-vertex, two-target mesh. It calls `UpdateCharacterDeformation` at a
1/60-second simulation step, changes both weights each tick, and checks the
published vertex count and representative output positions. The same Debug
test binary passed both serial and parallel runs. With
`HE_MORPH_BENCHMARK=1`, 20 timed updates through this entry point reported
serial P50/P95 16.062/16.865 ms (`HE_MORPH_TASKS_SERIAL=1`) and parallel
P50/P95 8.176/8.627 ms (serial override unset). Commands were
`$env:HE_MORPH_BENCHMARK='1'; $env:HE_MORPH_TASKS_SERIAL='1'; .\hectl.bat test AnimationPoseContract`
and
`$env:HE_MORPH_BENCHMARK='1'; Remove-Item Env:HE_MORPH_TASKS_SERIAL -ErrorAction SilentlyContinue; .\hectl.bat test AnimationPoseContract`.
The default dense combat scene has no large active morph mesh, so these
measurements establish the real call path's behavior under a constructed
load and are not evidence of a frame-time gain in that scene.
The optimized `RelWithDebInfo` HEngineTests target was then built and the
same two commands were repeated with `-Configuration RelWithDebInfo`. Both
passed. Full entry-point serial P50/P95 was 1.963/2.063 ms and parallel
P50/P95 was 1.163/1.226 ms; the direct section microbenchmark in those
runs was 3.379/3.595 versus 1.644/1.771 ms. These are 20-update CPU
test-process measurements, not whole-frame measurements.

The behavior tree now runs on the main thread in each fixed tick's PrePhysics
stage, ahead of Jolt and character movement. Its authored tick interval keeps
fractional remainder, so a 0.1-second tree runs ten times in one simulated
second regardless of 30/60/120 Hz presentation. Debug
`.\hectl.bat test FixedSimulationClock` passed with a real `BehaviorTreeSystem`
and probe node at all three presentation rates. A `BTMoveDirectlyToTarget`
entity also reached the same position after one simulated second at all three
rates. Optimized Game DX12 and Vulkan 90-frame `game check` runs passed after
this change. Optimized and Debug Game and Editor builds passed; the optimized
Editor DX12 60-frame `editor check` passed. These game fixtures do not exercise a loaded
behavior-tree character, so gameplay interaction for that path remains a
separate integration gap.

A same-control-mode attempt to repeat the early Agent statistics sample failed:
the paused Agent continued rendering, exhausted its 4,000,000-row CPU graph
statistics buffer before the first 120-step command completed, then exited.
The owned session was stopped in `finally`. This is a failed measurement, not
an engine test pass. A bounded final-source run used
`.\hectl.bat game check -Frames 500 -PerformanceWarmupFrames 120
-StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi
-Resolution 1280x720 -FixedDeltaSeconds 0.0166667 -RenderStats
-RenderStatsPath out/agent/final-main-game-check.csv -TimeoutSeconds 180`.
It passed. CSV frame IDs 120–450 contained 330 samples: CPU P50/P95/P99/max
7.821/12.308/15.280/19.990 ms, 3 frames above 16.667 ms; GPU span
2.422/2.982/3.200 ms; Level update 1.540/2.920/4.387 ms. Reported local GPU
memory peaked at 627.9 MB (598.8 MiB). The bounded check and the earlier
Agent sample differ in runtime control and workload timing; their numbers
cannot be used as a controlled before/after performance claim.

After the morph task path landed, the same bounded command with
`-RenderStatsPath out/agent/final-main-game-morph.csv` passed on the final
optimized Game source. Frame IDs 120–450 contained 330 samples: CPU
P50/P95/P99/max 7.448/11.904/13.766/17.773 ms, one frame above 16.667 ms;
GPU span 2.371/2.947/3.019 ms; Level update 1.429/2.779/4.098 ms. Local GPU
memory peaked at 616.7 MB (588.2 MiB). The earlier bounded check and this
run are single samples with natural run-to-run variation; they do not isolate
the morph change or prove a frame-rate gain.

After converting the editor icon/thumbnail path to asynchronous managed
textures and stable ImGui binding replacement, `git -c core.safecrlf=false
diff --check` passed. `.\hectl.bat editor check -Frames 90 -Resolution
1280x720 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12
-StrictGraphicsApi -RenderValidation -TimeoutSeconds 180` passed its optimized
Editor build and bounded run. The device reported NVRHI validation enabled;
the native D3D12 debug layer was unavailable. Bounded
`.\hectl.bat editor capture -Frames 150 -Resolution 1280x720 -StartupScene
Assets/Scenes/MainGame.he -GraphicsApi DX12|Vulkan -StrictGraphicsApi
-RenderValidation -CapturePath out/agent/async-ui-<backend>.png
-TimeoutSeconds 180` runs passed separately on both backends. The captures
show the editor's file-folder and scene-panel icons after asynchronous
publication. Thumbnail browsing and live UI hot reload were not exercised.
The `HEngineImGuiDX12Smoke` target was enabled in the existing optimized
Editor CMake cache with `cmake -S . -B out/agent/editor-relwithdebinfo
-DHE_BUILD_RENDER_SMOKES=ON`, then built with `cmake --build
out/agent/editor-relwithdebinfo --target HEngineImGuiDX12Smoke --parallel 4`.
After adding its stable-handle replacement case, the target built and the
device smoke printed `IMGUI_DX12_SMOKE_PASS`. Its first direct launch failed
with a missing runtime DLL search path; the passing launch prepended
`out/agent/editor-relwithdebinfo/bin` to `PATH` for `embree4.dll`.
The final UI source also built in the Debug `HEngineTests` target and
`.\hectl.bat test DelegateBuffer` passed. `.\hectl.bat editor build` restored
the standard optimized Editor cache (`HE_BUILD_RENDER_SMOKES=OFF`) and passed;
`.\hectl.bat game build` passed with the same final Runtime source.
The final optimized Game also passed `.\hectl.bat game check -Frames 500
-PerformanceWarmupFrames 120 -StartupScene Assets/Scenes/MainGame.he
-GraphicsApi DX12 -StrictGraphicsApi -Resolution 1280x720
-FixedDeltaSeconds 0.0166667 -RenderStats -RenderStatsPath
out/agent/final-main-game-env-ui.csv -TimeoutSeconds 180`. For frame IDs
120–450 (330 samples), CPU wall P50/P95/P99/max was
7.666/11.808/14.229/16.092 ms, with zero frames above 16.667 ms. GPU span
P50/P95/P99/max was 2.524/3.151/3.216/3.271 ms. Sampled local GPU usage
peaked at 586.95 MiB. This is one bounded run, and its slightly different
numbers from earlier runs do not establish an improvement due to the UI or
environment changes.

The environment cache now participates in `render.texture.reload`, including
failed-source retry and per-query retirement of in-flight IBL work. In owned
DX12 and Vulkan Agent sessions on `Assets/Scenes/MainGame.he`, reloading
`Assets/Textures/Hdr/VerdantRift.hdr` returned `queued=1` and later captures
showed complete scenes; both sessions stopped cleanly. The DX12 session path
was `out/agent/sessions/20260927-005530-816-0da8a9f35a21`, and Vulkan was
`out/agent/sessions/20260927-010022-766-1012dc2d2111`. These runs exercised
ready-entry invalidation. They did not deterministically interleave reload
with an in-flight IBL dispatch or a failed HDR decode.
After the in-flight reload flag and failed-source cache invalidation edits,
`.\hectl.bat game build` and `.\hectl.bat editor build` passed in the optimized
configuration, and `.\hectl.bat test TextureUploadState` built and passed in
Debug. The earlier bounded DX12 120-frame Game check passed before those final
edits. On the final source, an owned DX12 Agent session at
`out/agent/sessions/20260927-011811-041-716626d3b27f` stepped 120 fixed
frames, reloaded `VerdantRift.hdr` (`queued=1`), stepped 60 more, captured a
complete gameplay scene and stopped cleanly. The capture confirms a valid
rendered scene after the request; it does not prove a pixel-level change to
the HDR itself.
Task handles now include a graph identity to reject accidental cross-graph
dependencies. `.\hectl.bat test TaskScheduler` passed in Debug with a new
foreign-handle assertion; the optimized `.\hectl.bat game build` and
`.\hectl.bat editor build` both passed after that scheduler edit.
Full final-source Debug targets also passed:
`.\hectl.bat game build -Configuration Debug` (352.6 seconds) and
`.\hectl.bat editor build -Configuration Debug` (437.7 seconds). Their
corresponding optimized Game and Editor builds passed above. These are
build-only checks; the real-device runs used optimized binaries.
The later physical-button edge edit changed the final source again. It keeps
unconsumed key/mouse down events until the next display-frame input update,
then clears them once. `.\hectl.bat test InputActions` passed in Debug with
short Jump and mouse Fire pulses and context suppression assertions. On this
final input source, optimized `.\hectl.bat game check -Frames 120
-StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi
-Resolution 1280x720 -RenderValidation -TimeoutSeconds 180` passed build and
bounded run; the analogous optimized `.\hectl.bat editor check -Frames 90`
with the same scene/backend/resolution/validation/timeout passed build and
bounded run. `.\hectl.bat game build -Configuration Debug` and
`.\hectl.bat editor build -Configuration Debug` then passed incrementally.
No automated OS-window replay that presses and releases a physical key
between two polls was run.
The input regression was then expanded to inject real KeyPressed/KeyReleased
and MouseButtonPressed engine events into the event latch, including an
UI-handled press and an OS key repeat. `.\hectl.bat test InputActions` passed
again in Debug. On this last source, optimized `game build` and `editor build`,
and Debug `game build -Configuration Debug` and
`editor build -Configuration Debug`, all passed. Device checks immediately
preceded this testability-only input refactor; there was no new device run
after it.
The final button-count extension exposes `PressCount` to native and managed
actions without changing existing `Pressed` flags. `InputActions` Debug
regression passed with two presses in one display frame and duplicate
bindings. `dotnet build Engine/Managed/HEngine.ScriptCore/HEngine.ScriptCore.csproj
-c Debug --no-restore` and the same Release command passed with zero warnings
and errors (using the VS2026 CMake executable). On this final source,
`.\hectl.bat game check -Frames 120 -StartupScene
Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi
-Resolution 1280x720 -RenderValidation -TimeoutSeconds 180` passed optimized
build and bounded device run; optimized `editor build`, Debug `game build`,
and Debug `editor build` also passed. UI/editor capture and Vulkan device
checks preceded only this input-count extension.
An additional owned 500-frame DX12 Game check ran under a hidden PowerShell
wrapper that sampled the `StarbloomSurvivor` process every 50 ms. Its maximum
observed working set was 394.17 MiB; maximum sampled private bytes were
1117.52 MiB. The wrapper exited successfully and the Game check passed. In
frame IDs 120–450, CPU wall P50/P95/P99/max was
7.578/11.826/13.892/20.015 ms, with one frame above 16.667 ms. The polling
run is a memory observation, not a controlled frame-time comparison; no
equivalent pre-change CPU memory sample exists.

The sustained reload sample above supplies one 256-completion rolling P95/P99
observation per backend, but not a burst-load queue time series or a baseline
comparison. Input-to-display latency, whole-Game ASan, and concurrency detection remain
to be verified. More compressed DDS formats and deterministic
headless manager-level failure/retry coverage remain.

### Deterministic in-flight IBL scene retirement (2026-09-27)

An opt-in query-publication hold keeps a *submitted* IBL entry pending after
a hot reload. With `HE_ENVIRONMENT_IBL_QUERY_HOLD_FRAMES=1000000000`,
`HE_ENVIRONMENT_IBL_QUERY_HOLD_PATH=Assets/Textures/Hdr/VerdantRift.hdr`, and
`HE_ENVIRONMENT_IBL_QUERY_HOLD_AFTER_FIRST=1`, the first MainGame HDR prepares
normally. Each owned session used `.\hectl.bat agent start -StartupScene
Assets/Scenes/MainGame.he -GraphicsApi <DX12 or Vulkan> -StrictGraphicsApi
-RenderValidation`. The Agent called `render.texture.reload` for that HDR and
`runtime.scene.load` for `Assets/Scenes/MainMenu.he` with `forceReload=true`;
`agent stop` closed each session. Both backends returned `queued=1` for reload
and completed the scene transition. The logs show a pending query on the old
entry, followed by cancellation, GPU query completion, and discard:

| Backend | Session log | Submitted pump / release gate | Cancel pump | Result |
| --- | --- | ---: | ---: | --- |
| DX12 | `out/agent/sessions/20260927-024403-319-c12e47125508/game.stdout.log` | 1323 / 1000001323 | 3099 | PASS, completed query and discarded |
| Vulkan | `out/agent/sessions/20260927-024453-231-277c50ece047/game.stdout.log` | 788 / 1000000788 | 2749 | PASS, completed query and discarded |

The optimized Game build command `.\hectl.bat game build` passed before these
runs. On the final source, `.\hectl.bat editor build`, `.\hectl.bat game build
-Configuration Debug`, and `.\hectl.bat editor build -Configuration Debug`
also passed (`build-only`). This is `runtime-manual` device lifecycle evidence. The hold is a test
control; it does not measure normal request latency or cover every possible
scene-switch interleaving. A preliminary global hold also delayed the
destination scene's required IBL, so its transaction could not commit; that
session was stopped and is not counted as a pass.

The Debug `unit-test` command `.\hectl.bat test TaskScheduler` also passed on
the current source. Its expanded contract ran with zero, one, and three
workers over a two-slot queue: a 513-task fan-out/fan-in graph, cancellation
while a root was executing (128 descendants stayed unrun), and two concurrent
128-task graph submissions, in addition to the earlier DAG, cycle, exception,
shutdown, and deterministic-output checks. This is bounded scheduler stress
evidence, not a concurrency sanitizer result.

MSVC 14.51's `/fsanitize=address` was also available locally. A focused
standalone harness under `out/agent/asan/` compiled the repository's
`TaskScheduler.cpp` and included the actual `Buffer.h` and `Delegate.h` using
`.\out\agent\asan\BuildRuntimeContracts.cmd`; the final build had no warnings.
`.\out\agent\asan\RunRuntimeContracts.cmd` exited 0 (`unit-test`, AddressSanitizer
instrumented). It repeated 1000 Buffer custom-deleter/move cycles, 1000
mutating nested delegate broadcasts, 100 bounded 257-task DAGs, and an
in-flight task cancellation with 128 descendants. This is focused memory
instrumentation, not a whole-Game or Image/TextureManager ASan run. The
throwaway harness and binary live under `out/agent/asan/`; the permanent
headless regression for the scheduler is the `TaskScheduler` test above.

After tightening `Buffer::Deleter` to a `noexcept` function pointer, Image's
stb deleter and the test deleter were adapted. The first Debug
`.\hectl.bat test DelegateBuffer` build failed because `Buffer::Allocate`'s
own lambda lacked `noexcept`; that was fixed. The rerun passed (`unit-test`),
as did the final focused ASan build and run. On this final Buffer source,
`.\hectl.bat game build`, `.\hectl.bat editor build`, and the same two
commands with `-Configuration Debug` all passed (`build-only`).
The later `Buffer::Get<T>` bound check was tightened to reject an index via
division before calculating its byte offset. Added valid typed-access and
maximal-`uint32_t` typed-index assertions passed in
`.\hectl.bat test DelegateBuffer -Configuration RelWithDebInfo` (`unit-test`).
The focused MSVC AddressSanitizer harness was rebuilt with
`.\out\agent\asan\BuildRuntimeContracts.cmd` and
`.\out\agent\asan\RunRuntimeContracts.cmd` exited 0. These checks cover
the new header on optimized and instrumented builds; the preceding full
Game/Editor builds predate this narrow `Get<T>` change, and no production
call site of `Buffer::Get<T>` was found in the runtime, Editor, or Game.

### Prepared-byte backpressure (2026-09-27)

The CPU preparation worker now checks its retained prepared-byte budget
*before taking another queued job*. Frame publication and cancellation wake
the worker after releasing bytes. An explicit ready request steals its job
from the waiting queue and prepares it on the owner thread, avoiding a wait
behind a budget only that owner can release. The budget defaults to 128 MiB;
`HE_TEXTURE_PREPARED_BUDGET_MIB` accepts 1–512 for tuning and stress runs.
The new `preparedByteBudget` and `decodeBudgetStalls` counters make the gate
observable. A decode already in progress can exceed the gate by one asset.

`unit-test`: `.\hectl.bat test TextureUploadState` passed. `runtime-manual`:
`.\hectl.bat game check -Frames 120 -StartupScene Assets/Scenes/MainGame.he
-GraphicsApi DX12 -StrictGraphicsApi -Resolution 1280x720 -RenderValidation
-TimeoutSeconds 180` passed with `HE_TEXTURE_UPLOAD_TIMINGS=1` and
`HE_TEXTURE_PREPARED_BUDGET_MIB=1`. Its final log at
`out/agent/logs/run-game-relwithdebinfo-20260927-031642-548-67364.stdout.log`
reported budget 1,048,576 bytes, one decoder budget stall, peak prepared
5,592,404 bytes, 53 requests, nine completed uploads, zero pending or failed,
and one total global wait (the default-texture startup wait). The 5.6 MiB
peak is a single-decode overshoot; the test does not establish a strict
1 MiB memory cap. A normal-budget DX12 check and a Vulkan validation-layer
check on the same source both passed before this counter-only addition, with
peak prepared 5,596,496 bytes and zero pending or failed by the last sample.
At this stage, no device test had forced the explicit-ready owner-claim path.
After the statistics fields were added, the final-source Debug
`.\hectl.bat test TextureUploadState` rerun passed. The final-source optimized
`.\hectl.bat editor build` and Debug `.\hectl.bat game build -Configuration
Debug` / `.\hectl.bat editor build -Configuration Debug` also passed. The
low-budget DX12 check built the optimized Game on that final source.

The budget gate and owner claim are now operations on
`TexturePreparationQueueState`, the same class used by the live queue. An
expanded `TextureUploadState` unit test passed after simulating a one-job
overshoot, blocked decode, owner claim of exactly one waiting job, and resume
after byte release (`.\hectl.bat test TextureUploadState`). A further DX12
`game check` with the same 1 MiB stress setting passed on this refactor; its
last sample in
`out/agent/logs/run-game-relwithdebinfo-20260927-032740-232-83388.stdout.log`
reported one budget stall, 53 requests, nine completed, zero pending/failed,
and the single startup global wait. The unit test covers the shared queue
state; the device path was exercised subsequently as recorded below.

The refactor's final source also passed `.\hectl.bat editor build`,
`.\hectl.bat game build -Configuration Debug`, and `.\hectl.bat editor
build -Configuration Debug` (`build-only`); the low-budget `game check` built
the optimized Game.

### Explicit-ready owner claim on DX12 and Vulkan (2026-09-27)

An Agent-only `render.texture.readyProbe` command now issues an asynchronous
request followed immediately by `bRequireReady=true` on the same cache key,
then releases both leases. The `HE_TEXTURE_PREPARATION_HOLD_PATH` test hook
keeps that exact asset waiting for the owner; it and the probe are compiled
only with `HE_DEV_AGENT_RUNTIME`. The normal queue path has no hold.

Each owned session used `.\hectl.bat agent start -StartupScene
Assets/Scenes/MainMenu.he -GraphicsApi <DX12 or Vulkan> -StrictGraphicsApi
-RenderValidation` with `HE_TEXTURE_UPLOAD_TIMINGS=1` and the hold path set to
`Assets/Textures/VerdantRift/StylizedMacroAlbedo.png`. The Agent call was
`render.texture.readyProbe` with that path and `maximumDimension=129`, then
`.\hectl.bat agent stop`. Both commands returned `ready=true` and
`sharedHandle=true`. Both logs reported `ownerClaims=1`, zero pending/failed,
and `globalWaits=1` (the default-texture startup wait):

| Backend | Session log | Validation |
| --- | --- | --- |
| DX12 | `out/agent/sessions/20260927-033940-852-483e7aef74f3/game.stdout.log` | NVRHI validation, PASS |
| Vulkan | `out/agent/sessions/20260927-034040-800-69f2b8afddad/game.stdout.log` | Vulkan validation layer enabled, PASS |

An earlier unheld DX12 probe returned the same ready/shared result but
`ownerClaims=0`: the worker won that race. It was not counted as evidence for
the owner-claim branch. The controlled runs prove that the queued job can be
claimed and completed without deadlock on both backends; they do not measure
normal streaming latency or additional queue pressure.
After limiting the probe and hold to `HE_DEV_AGENT_RUNTIME`, the final-source
optimized `.\hectl.bat game build` and `.\hectl.bat editor build`, plus both
targets with `-Configuration Debug`, passed (`build-only`).

## Current-tree dense-scene repeat (2026-09-27)

After the prepared-byte queue and owner-claim changes, two more optimized Game
checks used the same i7-11800H/RTX 3060 laptop, DX12 validation, 1280×720,
default quality, `region_4_final` 192/192 enemy prefill, fixed automation
delta 0.0166667, and frame IDs 120–450 (330 samples). Each check passed
(`runtime-manual`):

`$env:HE_SURVIVOR_CAPTURE_SCENARIO='region_4_final'; .\hectl.bat game check
-Frames 500 -PerformanceWarmupFrames 120 -StartupScene
Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi
-RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667
-RenderStats -RenderStatsPath <case>.csv -TimeoutSeconds 240`

| Current-tree run | CPU P50/P95/P99/max (ms) | >16.667 ms | GPU P50/P95/P99 (ms) | Level update P50/P95/P99 (ms) | Peak local GPU memory |
| --- | --- | ---: | --- | --- | ---: |
| `out/agent/final-current-dense-1.csv` | 19.639/51.489/56.462/78.408 | 215/330 | 6.589/8.027/9.317 | 6.862/11.847/18.395 | 647.32 MiB |
| `out/agent/final-current-dense-2.csv` | 19.414/45.587/53.407/72.035 | 210/330 | 6.561/7.899/9.508 | 6.767/11.333/17.262 | 603.88 MiB |

The second run had an independent 50 ms process-memory poll from launch:
200 samples, peak working set 485,072,896 bytes (462.6 MiB) and private bytes
1,241,133,056 (1183.6 MiB), retained in
`out/agent/final-current-dense-2.process-memory.json`. This is an observed
polling peak, not a guaranteed instantaneous peak; polling also changes the
timing conditions slightly. The uninstrumented first run remains the cleaner
frame-time repeat. Its stdout is
`out/agent/logs/run-game-relwithdebinfo-20260927-035140-707-91548.stdout.log`;
the monitored run's is
`out/agent/logs/run-game-relwithdebinfo-20260927-035553-801-97700.stdout.log`.

The current-tree CPU distributions again overlap both baseline runs above;
there is no demonstrated whole-frame gain, and the 60 FPS target is not met
in this dense fixture. In run 1's 329 completed render-graph record samples
for world frames 120–450, `RuntimeUiRender` took 2,360.1 ms in aggregate,
with P50/P95/P99/max 3.958/39.656/43.359/45.984 ms. It is the largest
record pass in that sample. Run 2's corresponding 329 pass samples were
3.878/36.999/40.984/49.261 ms. The frame CSV's `cpuGraphRecordMs` reached
P95 43.24 ms, whereas `cpuFrameSlotWaitMs` and `cpuAcquireImageMs` were
near zero. This points to main-thread UI command recording as a significant
long-tail contributor in this fixture; it does not identify which RmlUi
operation causes those spikes. GPU span P95 remained below 9 ms. The
TaskScheduler workloads and texture queue cannot be credited with a measured
whole-frame improvement from these data.

No physical input-to-display latency measurement was made: the in-engine
frame and submission timestamps stop before actual display scanout. A
photodiode/high-speed camera or externally calibrated display trace would be
needed for that acceptance item. The historical baseline lacked CPU-process
memory samples; a later paired reconstruction is recorded below.

### Paired process-memory reconstruction (2026-09-27)

The old commit `8b8070e5540192408c4ee1bcf5b68eef192713aa` was checked out
as an isolated worktree on E: and built in RelWithDebInfo. The first build
failed because the clean checkout lacked the ignored `Tools/Scripting/node_modules/esbuild`;
its `Tools/Scripting/package-lock.json` hash matched the current checkout,
so the 10.3 MiB installed dependency directory was copied into that worktree.
The incremental `.\hectl.bat game build` then passed (`build-only`). No current
source file or user's process was altered by this baseline build.

The baseline and current code each ran the same DX12 strict-validation
`region_4_final` fixture for 500 fixed 1/60 s frames, with a 50 ms poll of
the owned Game process. Both logs confirm 192/192 enemy prefill and completed
frame limit 500. The `MainGame.he` Git blob was
`7ff67d13402a5199e10d112a1b92533ad256febc` in both checkouts; their
requested/effective render-setting maps were byte-equivalent after JSON
normalization. The monitor requires the PID reported by `hectl`, matches the
executable path and process start time, and records the OS peak working set
plus sampled private bytes. Its reusable implementation is
`Tools/Agent/MeasureDenseProcessMemory.ps1`. The final current-tree command
was `powershell -ExecutionPolicy Bypass -File Tools/Agent/MeasureDenseProcessMemory.ps1
-Prefix out/agent/final-dense-memory-e` (`runtime-manual`: PASS). The two
baseline runs used its original task-local copy with
`-Repository E:\HEngineBaselineMemory` and unique prefixes; each run's `hectl`
check and render validation passed. The baseline CSVs, setting snapshots,
per-pass timings, process samples, build and run logs were hash-verified after
copying to `out/agent/baseline-dense-memory-20260927/`.

| Run | CPU P50/P95/P99/max (ms) | >16.667 ms | GPU span P95 (ms) | Peak working set (MiB) | Peak sampled private (MiB) |
| --- | --- | ---: | ---: | ---: | ---: |
| Baseline E: first | 19.640/53.199/109.228/230.346 | 208/329 | 9.648 | 466.38 | 1245.29 |
| Baseline E: second | 19.276/49.340/54.713/71.361 | 207/329 | 8.578 | 465.50 | 1166.20 |
| Current C: first | 12.149/16.375/19.588/33.213 | 15/329 | 4.937 | 459.36 | 1184.04 |
| Current C: second | 13.007/18.983/24.113/34.351 | 34/329 | 5.212 | 455.18 | 1180.59 |
| Current C: tracked-script repeat | 12.098/16.440/19.920/34.042 | 14/329 | 4.830 | 462.59 | 1237.64 |

Rows use the same 329 world-frame IDs 120–450. Baseline peak local GPU memory
was 642.77/606.06 MiB and current peak was 604.24/621.05 MiB in the first
two runs, based on sparse renderer budget queries. The observed working-set
ranges are 465.50–466.38 MiB baseline and 455.18–462.59 MiB current. Private
byte ranges overlap, and 50 ms polling can miss a short allocation peak; no
private-memory reduction is claimed. E: versus C: storage and cold-cache state
also differ, so the unusually high first baseline P99 is not attributed to
engine changes. Two preliminary task-local monitor attempts failed to produce
memory evidence because the new process path was not yet queryable and then
because PowerShell returned a null wrapper exit code; the monitor was fixed
before these five recorded runs. Those Game checks themselves passed.

### Runtime UI long-tail diagnosis

An additional `runtime-manual` Game check passed with the same dense-scene
command and `-RenderStatsPath out/agent/final-rml-diagnosis.csv`, with
`HE_RML_RECORD_TIMINGS=1`. This default-off diagnostic logs only overlay
recordings above 20 ms, splitting Begin/Render/End and backend counters. The
owned run log is
`out/agent/logs/run-game-relwithdebinfo-20260927-040213-998-75320.stdout.log`.
The 330-frame sample had CPU P50/P95/P99/max
19.159/49.042/54.792/71.168 ms, 207/330 frames above 16.667 ms, and GPU
P50/P95/P99 6.506/7.595/9.417 ms. The instrumentation changes timing
slightly, so this run is diagnostic rather than a new speed comparison.

The render-pass sidecar isolates 21 consecutive `RuntimeUiRender` record
samples above 20 ms at world frames 304–324. The slow frames commonly had
246 UI draws and 69–70 geometry uploads. Typical logged values in the
cluster were approximately 27–31 ms inside `RmlUi::Render`, 10–13 ms in
`BeginGpuFrame` on later frames, and under 0.01 ms in `EndGpuFrame`.
Textures, filters, layer composites, and custom shader draws were normally
zero in that cluster. This identifies repeated UI geometry recording/upload
as associated with the spike, but does not yet prove which document elements
cause the churn or whether a reuse strategy would improve total frame time.
The diagnostic-source optimized `game check` built the Game, and
`build-only` commands `.\hectl.bat editor build`, `.\hectl.bat game build
-Configuration Debug`, and `.\hectl.bat editor build -Configuration Debug`
all passed. This new diagnostic code was not separately exercised in the
Editor or Debug binaries; it is disabled unless the environment flag is set.

### Frame-safe RmlUi geometry buffer reuse

The RmlUi NVRHI backend now transfers retired small geometry buffers back to
their generation-tagged free slot only after all three frame slots have
completed. The idle pool is bounded to 16 MiB total and 256 KiB per buffer.
Released geometry and texture upload bytes remain in the retired entry until
that same completion point. `GeometryBuffersReused` counts individual buffer
reuses. These changes keep RmlUi and NVRHI work on the main/RHI owner thread.

The same 500-frame DX12 fixture and 330-frame analysis window were used for
the pre-reuse diagnostic and two post-reuse runs. The first post-reuse run
had `HE_RML_RECORD_TIMINGS=1`; the second had it unset. Both `game check`
commands passed (`runtime-manual`, `-RenderValidation`). The post-reuse log
`out/agent/logs/run-game-relwithdebinfo-20260927-041013-503-113612.stdout.log`
records up to 85 buffer reuses in one slow UI frame. The long-tail render
pass contains the same 246 UI draws during the upgrade overlay.

| Run | CPU P50/P95/P99/max (ms) | >16.667 ms | `RuntimeUiRender` record P50/P95/P99 (ms) | Peak local GPU memory |
| --- | --- | ---: | --- | ---: |
| Before reuse, diagnostic | 19.159/49.042/54.792/71.168 | 207/330 | 3.876/35.914/41.035 | 606.05 MiB |
| After reuse, diagnostic (`out/agent/final-rml-reuse-dx12.csv`) | 15.504/24.517/39.277/67.850 | 133/330 | 0.783/5.700/21.353 | 620.23 MiB |
| After reuse, diagnostic off (`out/agent/final-rml-reuse-dx12-repeat.csv`) | 15.581/23.809/38.419/61.319 | 131/330 | 0.782/5.405/25.530 | 620.23 MiB |

The two post-reuse DX12 results support a substantial improvement for the
upgrade-overlay spike in this fixture. They do not establish a statistically
stable whole-game gain across other scenes; 131–133 sampled frames still
missed 16.667 ms. The observed local GPU memory peak rose by 14.18 MiB,
consistent with the bounded idle pool. A 500-frame Vulkan validation-layer
`game check` also passed (`out/agent/final-rml-reuse-vulkan.csv`), but its CPU
P50/P95/P99 was 32.218/42.802/48.001 ms, so the DX12 performance result
must not be generalized to Vulkan.

`runtime-manual`: DX12 `game capture -Frames 320` and Vulkan
`game capture -Frames 320`, each with the same scene, strict backend,
validation, 1280x720 and fixed dt, passed. The resulting
`out/agent/final-rml-reuse-frame320.png` and
`out/agent/final-rml-reuse-vulkan-frame320.png` show the complete level-up
overlay. Their mean absolute RGB pixel differences were 0.00086/0.00067/
0.00046 on a 0–255 channel scale. The final source then passed optimized
`.\hectl.bat editor build` and Debug `.\hectl.bat game build -Configuration
Debug` / `.\hectl.bat editor build -Configuration Debug` (`build-only`).
Exact commands (one per backend):

```powershell
$env:HE_SURVIVOR_CAPTURE_SCENARIO='region_4_final'; .\hectl.bat game capture -Frames 320 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667 -CapturePath out/agent/final-rml-reuse-frame320.png -TimeoutSeconds 240
$env:HE_SURVIVOR_CAPTURE_SCENARIO='region_4_final'; .\hectl.bat game capture -Frames 320 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi Vulkan -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667 -CapturePath out/agent/final-rml-reuse-vulkan-frame320.png -TimeoutSeconds 240
```

### Dense combat objective query (2026-09-27)

After RmlUi buffer reuse, the no-diagnostic DX12 run had 113/183 over-budget
frames before the level-up panel (world frames 120–303), 18/21 during the
panel (304–324), and none in the 126 paused frames afterward. Its update
P50/P95 was 6.464/10.877 ms. `HE_RUNTIME_SYSTEM_TIMINGS=2` on the same fixture
showed typical fixed AI/Jolt/movement sums below 0.1 ms, while
`PythonScriptSystem` commonly took 4–7 ms. The owned log is
`out/agent/logs/run-game-relwithdebinfo-20260927-042511-634-118900.stdout.log`.
These are sampled diagnostic timings, not a claim that Jolt has no cost in
other collision scenes.

`HE_SURVIVOR_STAGE_TIMINGS=1` localized the script's steady cost to Region
objectives (about 4.3–5.0 ms per frame across four 60-frame windows).
`HE_SURVIVOR_OBJECTIVE_TIMINGS=1` then measured `LocalAlive` at about
4.0–4.3 ms, while controller, beacons, and diagnostic publication were each
below 0.1 ms. The old path materialized a Python dictionary per monster and
read each transform through Python bindings. A direct Python loop without
caching anchor coordinates regressed to about 5.7–6.2 ms for `LocalAlive` and
200/330 over-budget frames; caching the anchor returned it to about
3.6–4.0 ms but did not establish a whole-frame gain. That candidate was
replaced, not retained.

The current path reuses the existing native `QuerySurvivorAgentsInRadius`
snapshot and filters returned IDs by generation, death, and encounter in
Python. The same diagnostic reported `LocalAlive` about 0.14–0.15 ms per
frame across four windows. An opt-in `HE_SURVIVOR_LOCAL_ALIVE_VERIFY=1`
500-frame DX12 `game check` compared every native result with the old
enumeration in the dense fixture and passed (`runtime-manual`); log:
`out/agent/logs/run-game-relwithdebinfo-20260927-043730-164-121660.stdout.log`.
This verifies that run, not arbitrary custom monster mutations. A headless
`python Project/Scripts/SurvivorLocalAliveTests.py` passed two filter and
mismatch-detection tests (`unit-test`). The existing
`python Project/Scripts/SurvivorRegionObjectiveContractTests.py` passed 32
tests (`unit-test`). The project-level `SurvivorRegionObjectives` test initially
failed because its source-inspection path still named the old
`Runtime/Scripting/Binding/EcsBinding.hpp`; after updating that path to
`Plugins/PythonScript/Binding/EcsBinding.hpp`,
`.\hectl.bat test SurvivorRegionObjectives` passed (`unit-test`). Running the
whole Project contract Python file directly is unsupported because it needs
the embedded HEngine module; that attempt failed at import, and the bridge
test above is the valid result.

Two independent 500-frame optimized DX12 `game check` runs without the
diagnostic/verification flags used the same 192/192 prefill, fixed dt,
1280x720, validation, and frame IDs 120–450 as the RmlUi comparison:

| Run | CPU P50/P95/P99/max (ms) | >16.667 ms | Update P50/P95/P99 (ms) | GPU P50/P95/P99 (ms) |
| --- | --- | ---: | --- | --- |
| Before native query, `final-rml-reuse-dx12-repeat.csv` | 15.581/23.809/38.419/61.319 | 131/330 | 6.464/10.877/18.068 | 5.305/6.341/6.770 |
| Native query 1, `final-native-localalive-uninstrumented-1.csv` | 12.504/21.101/36.714/53.059 | 37/330 | 2.644/6.795/13.291 | 4.321/5.140/5.716 |
| Native query 2, `final-native-localalive-uninstrumented-2.csv` | 12.112/21.246/35.452/53.763 | 36/330 | 2.570/6.789/13.239 | 4.192/4.844/5.087 |

The repeated lower update and CPU frame times support a gain in this fixture,
but its P95 still exceeds 16.667 ms. In native-query run 2, 19/183 combat
frames and 17/21 level-up frames exceeded the budget; all 126 later paused
frames met it. Peak local GPU memory samples varied between 620.23 MiB
before and 642.73/625.01 MiB after; these sparse samples do not establish
an attributable memory change. A same-settings DX12 `game capture -Frames
320` passed (`runtime-manual`); its PNG
`out/agent/final-native-localalive-frame320.png` was pixel-identical to the
pre-query `out/agent/final-rml-reuse-frame320.png` in RGB.

An additional read-only analysis of `final-native-localalive-uninstrumented-2.csv`
isolated the remaining tail in frame IDs 120–449 (329 samples). The frame
P95/P99 were 21.246/35.452 ms; render-submit-present P95/P99 were
14.832/28.998 ms, and graph-record P95/P99 were 11.828/26.060 ms.
Frame 304 took 49.772 ms overall, with 42.398 ms in graph recording and
4.169 ms in update. Frame 192 instead spent 25.098 of 35.452 ms in update.
Thus the measured misses have at least two causes; the upgrade overlay's
recording tail remains the clearest rendering target, while some combat
frames need separate update profiling. This analysis is `static-check` of
the existing CSV, not a new runtime sample or a claim of a further speedup.

The final source passed optimized Game `check` and `.\hectl.bat editor build`,
plus Debug `.\hectl.bat game build -Configuration Debug`,
`.\hectl.bat editor build -Configuration Debug`, and the Debug test build
included by `.\hectl.bat test SurvivorRegionObjectives` (`build-only` for the
three explicit builds; `runtime-manual`/`unit-test` as above).

With `HE_SURVIVOR_CAPTURE_SCENARIO=region_4_final` and
`HE_SURVIVOR_LOCAL_ALIVE_VERIFY=1`, an owned DX12/NVRHI-validation Agent
session started `MainGame.he` with 192/192 prefilled monsters, stepped 120
frames, loaded `MainMenu.he`, reloaded `MainGame.he`, stepped another 120,
force-reloaded `MainGame.he`, and stepped a final 120. `world.summary`
reported scene generations 2, 5, 8, and 11 at the committed transitions.
All three game starts logged 192/192 prefill; no native/reference count
mismatch or RHI validation error appeared. The session stopped normally.
Evidence: `out/agent/sessions/20260927-045110-208-370a6d33d65f/game.stdout.log`
and its empty stderr log (`runtime-manual`). This covers cache reset and
entity generation reuse in this controlled transition sequence, not every
possible custom scene mutation.

### Failed texture request retry (2026-09-27)

A real-device retry probe used an existing but corrupt PNG at
`Project/Assets/Textures/AgentFailureRetryProbe.png`. The first
`render.texture.readyProbe` failed as expected. After replacing the bytes at
the same path with a valid 4×4 RGBA PNG, the next request still failed:
`SceneRenderer::RequestTextureReload` cleared failed-load cache entries only
when it had invalidated an environment entry. The failed ordinary-texture
entry had no live handle, so `render.texture.reload` returned `queued=0` and
left the cached error. The pre-fix owned Agent session is
`out/agent/sessions/20260927-045528-108-83074906da5d/`.

`RequestTextureReload` now schedules live texture retries, then clears
matching failed-load cache entries even if no environment entry was found.
On optimized DX12 and Vulkan Agent sessions with NVRHI validation, the
same corrupt-to-valid replacement sequence returned `ready=true` and
`sharedHandle=true` on the next request after reload. `queued=0` correctly
reported no live handle scheduled. Both sessions stopped normally; evidence:
`out/agent/sessions/20260927-045912-568-6fe484b84d90/` (DX12) and
`out/agent/sessions/20260927-050021-576-2983ab97c810/` (Vulkan)
(`runtime-manual`). The corrupt-image decode failures were expected; no RHI
validation error was reported. The optimized Game build used by the DX12
Agent start passed (`build-only`). After this code change,
`.\hectl.bat editor build`, `.\hectl.bat game build -Configuration Debug`,
and `.\hectl.bat editor build -Configuration Debug` passed (`build-only`).
`.\hectl.bat test TextureUploadState` passed, including its Debug test
build (`unit-test`). This unit test covers the upload state machine; the
same-path failed-cache retry is covered by the device sessions.

A follow-up audit found that a relative failed-load key may belong to the
project root or the engine content root. `InvalidateFailedLoads` now derives
and validates both candidate paths when clearing a relative asset; absolute
requests retain their single resolved path. This avoids leaving an old
failure cached when a relative asset moves between the two roots. After the
change, optimized and Debug Game/Editor builds and the Debug
`.\hectl.bat test TextureUploadState` passed (`build-only` and `unit-test`).
Owned DX12 and Vulkan Agent sessions under NVRHI validation passed
`render.texture.reload` plus `render.texture.readyProbe` for the project
probe asset and the existing engine-root `Assets/Textures/DefaultTexture.png`,
each returning `ready=true` and `sharedHandle=true` (`runtime-manual`). In
the DX12 session, a deliberately missing `Icons/Trans.png` resolved to
`Engine/Content/Icons/Trans.png`; a probe failed, reload returned `queued=0`,
and a second probe produced a second decode-failure log entry. This confirms
that the engine-root negative cache was invalidated, while the missing asset
correctly remained unavailable. Both Agent sessions stopped normally, stderr
was empty, and the Vulkan log confirmed its validation layer was enabled.
Evidence: `out/agent/sessions/20260927-051355-504-4f8d188e7793/` (DX12)
and `out/agent/sessions/20260927-051542-768-63bfec09d4c2/` (Vulkan).
The specific file-relocation sequence was reviewed in code but not reproduced
on device because no existing source file was removed for that probe.

### RmlUi geometry free-slot selection (2026-09-27)

The retired geometry pool previously took its last free slot even when another
free slot already had both a fitting vertex buffer and a fitting index buffer.
`CompileGeometry` now selects the fitting pair with the least unused capacity
among at most the 256 most recent free slots; if no pair fits, its existing
last-free-slot growth path remains. The search cap bounds work per compiled
geometry on unusually large UI documents. All buffer
selection and upload still occur on the RHI owner thread, after the frame-slot
retirement gate.

The same i7-11800H/RTX 3060 Laptop GPU, DX12 validation, 1280×720,
`region_4_final` 192/192 prefill, fixed 0.0166667 s automation delta,
and world-frame 120–449 window were used. The command for each run was
`$env:HE_SURVIVOR_CAPTURE_SCENARIO='region_4_final'; .\hectl.bat game check
-Frames 500 -PerformanceWarmupFrames 120 -StartupScene
Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi
-RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667
-RenderStats -RenderStatsPath <case>.csv -TimeoutSeconds 240`.
The first pair also set `HE_RML_RECORD_TIMINGS=1`; the other two runs left it
unset. All four checks passed (`runtime-manual`). Nearest-rank values below
use 329 frame rows; `RuntimeUiRender` record uses 328 completed pass rows.

| Run | CPU P50/P95/P99 (ms) | >16.667 ms | Graph-record P95 (ms) | RuntimeUiRender record P95 (ms) |
| --- | --- | ---: | ---: | ---: |
| Before, diagnostic (`final-rml-bestfit-before.csv`) | 12.909/23.568/38.233 | 46/329 | 12.974 | 6.411 |
| After, diagnostic (`final-rml-bestfit-after.csv`) | 12.280/18.913/36.758 | 26/329 | 8.169 | 1.403 |
| After, diagnostic off 1 (`final-rml-bestfit-uninstrumented-1.csv`) | 12.244/18.938/36.273 | 24/329 | 8.245 | 1.394 |
| After, diagnostic off 2 (`final-rml-bestfit-uninstrumented-2.csv`) | 12.099/17.537/35.693 | 20/329 | 7.969 | 1.374 |

For context, the two earlier diagnostic-off pre-selection runs
`final-native-localalive-uninstrumented-1.csv` and `-2.csv` had CPU P95
21.101/21.246 ms, 37/36 over-budget frames, and graph-record P95
12.271/11.828 ms. The repeated lower tails support keeping the selection
change in this fixture; they do not prove a gain for other UI documents or
backends. The P95 remains above the 16.667 ms target. Sparse local GPU
memory peaks differed between runs (642.73 MiB before, 603.88 MiB after)
and cannot establish an attributable memory reduction.

With the same fixture and frame 320, optimized DX12 and Vulkan
`game capture -Frames 320` passed under strict backend selection and NVRHI
validation (`runtime-manual`). The new DX12 and Vulkan PNGs were each
pixel-identical in RGB to their respective pre-selection capture. The Vulkan
log confirmed its validation layer was enabled and showed no validation
error. Optimized `.\hectl.bat editor build` and Debug
`.\hectl.bat game build -Configuration Debug` /
`.\hectl.bat editor build -Configuration Debug` passed (`build-only`), as did
`git -c core.safecrlf=false diff --check` (`static-check`).
After adding the 256-slot search cap, a further uninstrumented check with
the same command passed: `final-rml-bestfit-bounded.csv` recorded CPU
P50/P95/P99 12.260/18.640/37.860 ms, 24/329 over-budget frames, and
`RuntimeUiRender` record P95 1.379 ms. These fall within the variation of
the two uncapped after-runs; the cap does not establish another speedup.
On this capped final source, optimized Editor and Debug Game/Editor builds
passed again (`build-only`). DX12 and Vulkan 320-frame captures under strict
backend selection and NVRHI validation also passed; their RGB outputs were
pixel-identical to the corresponding uncapped captures (`runtime-manual`).
The final images are `out/agent/final-rml-bestfit-bounded-frame320.png` and
`out/agent/final-rml-bestfit-bounded-vulkan-frame320.png`.

### PNG upload setting coverage (2026-09-27)

The Dev Agent `render.texture.readyProbe` now accepts color space, mip
generation, alpha-coverage preservation/cutoff, and maximum dimension, then
returns the *published* texture's width, height, mip count, and NVRHI format
name. It still checks that an asynchronous request and immediate ready lease
share a handle and releases both leases. This extension is compiled only in
the Dev Agent runtime.

The existing transparent 96×96
`Assets/UI/Icons/Survivor/Characters/azura.png` was probed in owned DX12 and
Vulkan sessions with NVRHI validation. Each backend returned the same results
(`runtime-manual`):

| Settings | Published format | Size | Mips | Shared/ready |
| --- | --- | --- | ---: | --- |
| Linear, no generated mips | `RGBA8_UNORM` | 96×96 | 1 | true/true |
| sRGB, no generated mips | `SRGBA8_UNORM` | 96×96 | 1 | true/true |
| sRGB, generated mips | `SRGBA8_UNORM` | 96×96 | 7 | true/true |
| sRGB, generated mips, alpha coverage 0.5, max dimension 32 | `SRGBA8_UNORM` | 32×32 | 6 | true/true |

Sessions `out/agent/sessions/20260927-053909-786-a442b7116bde/` (DX12)
and `out/agent/sessions/20260927-053945-236-77f43f9669f2/` (Vulkan)
stopped normally; stderr was empty, no RHI validation error was logged, and
the Vulkan validation layer was enabled. Optimized `.\hectl.bat game build`
and `.\hectl.bat editor build`, plus their
`-Configuration Debug` counterparts, passed (`build-only`). These probes
verify format, dimensions, mip count, upload completion, and shared-handle
behavior for these PNG variants. They do not inspect mip texel values or
prove that alpha-coverage preservation improves the rendered edge; pixel
readback or a controlled image comparison is still needed for that claim.

### CPU mip alpha-coverage regression (2026-09-27)

The RGBA8 and RGBA32F decode paths now reject zero or overflowing dimensions
before multiplying pixel counts by channel bytes. A Dev Agent-only validation
helper returns the encoded mip bytes from the same production RGBA8 generator
used by asynchronous texture preparation. The Debug
`.\hectl.bat test TextureUploadState` passed (`unit-test`): for a deterministic
64×64 RGBA fixture, the test independently counted pixels above the 0.5
alpha cutoff in every generated mip. Preserving coverage never increased the
absolute difference from the source coverage relative to ordinary area
filtering, and at least one mip improved. It also checked all six mip
dimensions/byte counts and rejected `UINT32_MAX × UINT32_MAX` dimensions
with an empty source span. This covers CPU-generated alpha bytes, without
claiming GPU readback or rendered-edge quality.

After this source change, optimized Game/Editor and Debug Game/Editor builds
passed (`build-only`). The owned DX12 and Vulkan NVRHI-validation sessions
`out/agent/sessions/20260927-055443-497-3dd9ba585193/` and
`out/agent/sessions/20260927-055518-029-97f3815cc421/` each repeated the
32×32 sRGB alpha-coverage probe and returned `SRGBA8_UNORM`, six mips,
`ready=true`, and `sharedHandle=true` (`runtime-manual`). Both stopped
normally. GPU mip texel readback and a controlled transparent-edge render
comparison remain unverified.
The same final source also passed 120-frame DX12 and Vulkan `game check`
runs on `Assets/Scenes/MainGame.he`, whose scene document references the
existing `Assets/Textures/Hdr/VerdantRift.hdr` (`runtime-manual`). To verify
the changed RGBA32F decode path directly, separate owned DX12 and Vulkan
Agent sessions probed that HDR asset and both returned `RGBA32_FLOAT`,
256×128, one mip, `ready=true`, and `sharedHandle=true`. Those sessions,
`out/agent/sessions/20260927-060225-115-6a9048494e25/` and
`out/agent/sessions/20260927-060257-554-913e19032358/`, stopped normally.

### Concurrent task graphs and frame priority (2026-09-27)

The scheduler previously held one execution mutex until a whole graph's
barrier completed. A background mesh-index conversion graph could therefore
admit itself first and delay a later frame-critical targeting or morph graph,
even though individual tasks carried priorities. Independent graphs now
submit concurrently to one bounded, priority-ordered worker queue. Normal
jobs can occupy at most three quarters of its slots, background jobs at most
half, leaving admission room for frame-critical work. `MeshLoader` marks its
large index-conversion batches as background; frame morph and targeting work
retain frame-critical priority. Each graph retains its own dependency state,
cancellation, exception propagation, and barrier. Zero-worker fallback
serializes concurrent callers; an in-task Shutdown requests stop, and an
external call completes worker joins. `QueuedTaskCount` exposes queue depth.

`unit-test`: `.\hectl.bat test TaskScheduler` passed, then passed five more
consecutive runs. The expanded contract holds a background worker while
two background jobs occupy their allowed queue slots, admits a concurrent
frame graph into the reserved slot, and verifies its task runs before the
queued background jobs. It also checks concurrent zero-worker fallback,
worker-requested shutdown, and invalid-priority rejection. Existing DAG,
fan-in/out, cancellation, exception, low-core, and 513-node pressure checks
remain in that test. `.\hectl.bat test MeshCpuDecode` passed (`unit-test`).
After adding a concurrent same-graph publication check, the final
`.\hectl.bat test TaskScheduler` rerun passed; the second Execute is rejected
while the first has a task in flight.
The focused MSVC AddressSanitizer harness was rebuilt against this
TaskScheduler source with `.\out\agent\asan\BuildRuntimeContracts.cmd` and
`.\out\agent\asan\RunRuntimeContracts.cmd` exited 0 (`unit-test`, focused
ASan). It covers the older DAG/cancellation stress in that harness; the new
cross-graph priority case is covered by `HEngineTests`, not by ASan.

`runtime-manual`: a 500-frame optimized DX12/NVRHI-validation dense check
passed with the same fixture, 1280×720, fixed automation delta, and frame
window as the preceding geometry-pool comparison. Its 329-frame CPU
P50/P95/P99 was 11.930/19.378/37.611 ms, with 27 frames above 16.667 ms;
the preceding `final-rml-bestfit-bounded.csv` was
12.260/18.640/37.860 ms and 24 frames. This one-run difference does not
establish a task-scheduler speedup or a regression. Evidence:
`out/agent/final-concurrent-task-graph-dense.csv`.

An owned DX12 Agent session started the dense MainGame scene, advanced 120
fixed steps, switched to MainMenu for 60 steps, then returned to MainGame
for 120 more. Committed scene generations were 2, 5, and 8; the final game
contained 649 entities. The session stopped normally with empty stderr and
no scheduler or RHI validation error. Its stdout includes two unrelated
`ToonWater.efkpkg` preload warnings; no claim is made that all VFX loaded.
Evidence: `out/agent/sessions/20260927-061518-736-64bfd9aefeba/`.
The optimized Game build used by the check and Agent passed, as did optimized
Editor and Debug Game/Editor builds (`build-only`).
An additional final-source Vulkan/NVRHI-validation
`.\hectl.bat game check -Frames 120 -StartupScene
Assets/Scenes/MainGame.he -GraphicsApi Vulkan -StrictGraphicsApi
-RenderValidation -Resolution 1280x720 -TimeoutSeconds 180` passed
(`runtime-manual`); stdout is
`out/agent/logs/run-game-relwithdebinfo-20260927-062242-849-69604.stdout.log`.

The temporary 78-byte valid probe PNG remains untracked at the path above.
Automatic approval review rejected the attempted `Remove-Item` cleanup,
including an absolute literal-path attempt. It was not removed or treated
as a checked-in test fixture. The review returned only “blocked by policy”;
no more specific reason was available.

### Runtime RenderPrepare ordering (2026-09-27)

The old 3D system list extracted RenderWorld before native, managed, and
Python scripts ran. Runtime display updates now defer only `RenderSystem3D`
until the remaining display-rate systems finish. Startup, shutdown, editor
preview, the fixed PrePhysics/Physics/PostPhysics sequence, and the separate
once-per-display UI pump retain their respective order and frequency. Tracy
marks the four main-thread stage boundaries. A new `RenderWorldSubmission`
integration case binds a native camera script, executes two whole Level
updates, and asserts each submitted snapshot contains that same frame's
camera movement (x=3 then x=6). `.\hectl.bat test RenderWorldSubmission`
passed (`unit-test`).

Before this final profiling-label patch, DX12 and Vulkan frame-320 captures
matched the preceding build pixel for pixel. A single dense DX12 500-frame
check using Region 4, 1280×720, fixed automation delta, warmup 120, and
frames 120–449 gave CPU P50/P95/P99 11.773/17.647/36.658 ms, 21/329 frames
above 16.667 ms, update P95 6.826 ms, and GPU P95 4.630 ms. The preceding
same-fixture capture was 11.930/19.378/37.611 ms, 27/329 above budget,
update P95 7.388 ms, and GPU P95 4.951 ms. These one-run changes do not
establish a stable speedup. CSVs: `out/agent/final-render-stage-dense.csv`
and `out/agent/final-concurrent-task-graph-dense.csv` (`runtime-manual`).

Final-source validation: `.\hectl.bat game check -Frames 120 -StartupScene
Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi
-RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667
-TimeoutSeconds 180` passed; the same command with `-GraphicsApi Vulkan`
passed (`runtime-manual`). `.\hectl.bat editor build`, `.\hectl.bat game
build -Configuration Debug`, and `.\hectl.bat editor build -Configuration
Debug` passed (`build-only`). The DX12 check also built optimized Game.
`.\hectl.bat test FixedSimulationClock`, `.\hectl.bat test
PresentationClock`, and `.\hectl.bat test PhysicsScriptContact` passed
(`unit-test`). A first attempt with the nonexistent name `SimulationClock`
returned UNKNOWN; no test assertion failed in that invocation.

### GPU mip byte readback (2026-09-27)

The Dev Agent `render.texture.readyProbe` accepts `readbackMips=true` for an
RGBA8 texture. This uses a distinct cache key and tracked RHI state so that
an explicit validation copy can read every mip into staging. It waits on the
copy's event query, never device idle, and returns each mip's width, height,
tight-row FNV-1a byte hash, and alpha-covered pixel count. The readback is
bounded to 16 MiB; normal managed textures keep their existing cache keys and
permanent ShaderResource state.

Owned DX12 and Vulkan Agent sessions loaded the existing transparent
`Assets/UI/Icons/Survivor/Characters/azura.png`, resized to 32×32, with
sRGB/mips and alpha coverage disabled/enabled. Both backends returned the
same six hashes for each setting, `ready=true`, `sharedHandle=true`, and
`SRGBA8_UNORM`; both stopped with rendering validation PASS (`runtime-manual`).
Mip-0 alpha coverage was 381/1024. At 16×16, ordinary filtering gave 97/256
and preservation 95/256; at 4×4, 4/16 versus 6/16; at 2×2, 0/4 versus 1/4.
The 4×4 preservation ratio is closer to mip-0 coverage (0.372) than ordinary
filtering (0.250 versus 0.375), and the 2×2 case also improves. DX12 also
read back a linear, single-mip variant as `RGBA8_UNORM`, 32×32. Evidence:
`out/agent/sessions/20260927-065328-285-d4e34ae0d9ea/` (Vulkan) and
`out/agent/sessions/20260927-065355-568-2d1c5b843cb3/` (DX12).

The initial DX12 readback attempt produced NVRHI permanent-state validation
errors; the first Vulkan attempt produced invalid return-to-Common layout
transitions. Both defects were corrected before the passing sessions above.
This evidence verifies uploaded mip bytes and alpha coverage. It does not
measure filtered on-screen edge appearance at a controlled pixel footprint.
After retaining the pre-existing normal cache key and adding the final 16 MiB
overflow guard, owned DX12 and Vulkan Agent probe sessions passed again with
the same six preserved-mip hashes and rendering validation enabled:
`out/agent/sessions/20260927-070233-722-dba271393c3f/` and
`out/agent/sessions/20260927-070249-320-379e0305013b/`. With
`HE_TEXTURE_UPLOAD_TIMINGS=1`, each reported `globalWaits=1`, the existing
default-texture startup wait; this readback added no device-wide wait.
For each backend the final run used `.\hectl.bat agent start -StartupScene
Assets/Scenes/MainMenu.he -GraphicsApi <DX12|Vulkan> -StrictGraphicsApi
-RenderValidation`, then built
`$parameters=@{path='Assets/UI/Icons/Survivor/Characters/azura.png';colorSpace='srgb';generateMipmaps=$true;preserveAlphaCoverage=$true;alphaCoverageCutoff=0.5;maximumDimension=32;readbackMips=$true} | ConvertTo-Json -Compress`
and called
`& .\Tools\Agent\hectl.ps1 agent call -Method render.texture.readyProbe
-ParamsJson $parameters`, and finally `.\hectl.bat agent stop` in a
`finally` block.
Final-source 120-frame Game checks on both DX12 and Vulkan passed
(`runtime-manual`), as did optimized Editor and Debug Game/Editor builds
(`build-only`). `.\hectl.bat test TextureUploadState` passed (`unit-test`).

After the later UI decode and queue changes, the same Dev Agent
`render.texture.readyProbe` was rerun against the final optimized Game
source on strict DX12 and Vulkan with NVRHI validation. For each backend,
`azura.png` at 32×32 was probed with sRGB, six generated mips, GPU
readback, and alpha-coverage preservation first off, then on. All four
calls returned `ready=true`, `sharedHandle=true`, and `SRGBA8_UNORM`;
both owned sessions stopped normally with empty stderr and zero logged
validation errors (`runtime-manual`). The two backends' full result JSON
objects were identical for each setting, including every mip hash and
coverage count. Ordinary coverage at 32/16/8/4/2/1 was
381/97/24/4/0/0 pixels; preservation yielded 381/95/24/6/1/0.
The 4×4 ratio moves from 0.250 to 0.375, nearer the base ratio 0.372.
The exact command route was `agent start` on `MainMenu.he` with
`-GraphicsApi <DX12|Vulkan> -StrictGraphicsApi -RenderValidation`, two
`render.texture.readyProbe` calls with `readbackMips=true`, then owned
`agent stop`. The four returned JSON files and Game logs are pinned in
`out/agent/current-final-mip-readback-20260927/`. The first attempted
route piped `agent start` through `Out-Host` and never submitted a probe;
its owned session was stopped and excluded. GPU readback proves uploaded
mip bytes and alpha coverage, not the final filtered edge on screen.

### Enemy VFX first-use stall moved into adventure warm-up (2026-09-27)

The dense DX12 fixture still had isolated 31/37 ms combat frames after the
task and UI changes. `HE_SURVIVOR_STAGE_TIMINGS=1` now enables the existing
60-frame Survivor stage summary even when Tracy hooks are compiled. A
default-off nested `SurvivorBatchTiming` record showed the agent batch itself
took about 0.07 ms, while processing its first enemy effect events took
18.074 and 23.804 ms. The same log placed first Effekseer loads for
`EnemyZoneTelegraph` and `EnemyProjectileOrange` at those events. These
observations are diagnostic, not a whole-frame speed comparison.

Those two authored effects now enter the existing one-effect-per-display-frame
adventure warm-up list. In the follow-up diagnostic run, both package-load
records precede `adventure started after warm-up`; no `SurvivorBatchTiming`
record exceeded the 3 ms threshold, and the 60-frame `Monsters` maximum
dropped from 23.907 to 0.348 ms in the matching active interval. The loading
screen gains two warm-up frames and the associated package-open time. Neither
package is dispatched to a real-time TaskScheduler worker.

With `HE_SURVIVOR_CAPTURE_SCENARIO=region_4_final`, DX12 validation,
1280×720, fixed 0.0166667 s automation delta, and 500 frames, two
diagnostic-off `game check` repeats passed (`runtime-manual`). The exact
command was `.\hectl.bat game check -Frames 500
-PerformanceWarmupFrames 120 -StartupScene Assets/Scenes/MainGame.he
-GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -Resolution 1280x720
-FixedDeltaSeconds 0.0166667 -RenderStats -RenderStatsPath <case>.csv
-TimeoutSeconds 240`. Because the new warm-up adds two displayed frames,
the comparison uses old world frames 121–449 and new 123–451, each 329 rows:

| Run | CPU P50/P95/P99/max (ms) | >16.667 ms | Update P95 (ms) | GPU P95 (ms) |
| --- | --- | ---: | ---: | ---: |
| Before, `final-render-stage-dense.csv` | 11.77/17.65/36.66/52.97 | 21 | 6.83 | 4.63 |
| After 1, `final-enemy-vfx-preload-1.csv` | 11.88/16.97/25.87/56.29 | 18 | 6.49 | 4.79 |
| After 2, `final-enemy-vfx-preload-2.csv` | 11.96/16.96/26.60/55.68 | 18 | 6.44 | 4.82 |

The repeated P99 and combat-frame reduction supports this targeted fix in
the fixture. P95 remains above the 16.667 ms goal, maximum frame time did
not improve, and GPU P95 did not decrease. The remaining 18 over-budget
frames in run 2 include 13 combat frames and five first-show upgrade UI
frames; they need separate work rather than crediting this preload.

An additional default-off `SurvivorStageSpike` diagnostic logged three
late combat updates above 8 ms at gameplay time 3.47–3.52 s. In each,
`Objectives` consumed 6.2–6.9 ms and pending pickup spawn handling about
2.3 ms. With `HE_SURVIVOR_OBJECTIVE_TIMINGS=1`, the objective's `Pressure`
substage reached 6.846 ms while `LocalAlive` remained under 0.21 ms.
Evidence: `out/agent/logs/run-game-relwithdebinfo-20260927-072045-652-95436.stdout.log`
(`runtime-manual`, diagnostic). Region pressure already limits itself to
eight main-thread ECS activations per frame, and pickup creation to four;
this sample does not justify moving EnTT mutations to workers or reducing
the authored spawn count. The remaining UI frames are a distinct path.
After the diagnostic-only spike logging was added, `.\hectl.bat test
SurvivorRegionObjectives` passed again (`unit-test`), a direct `compile()` of
`SurvivorGame.py` and `SurvivorWorld.py` passed (`static-check`), and
`git -c core.safecrlf=false diff --check` passed (`static-check`). The
diagnostic-on final-source 500-frame DX12 `game check` also passed
(`runtime-manual`, CSV `final-objective-stage-diagnosis.csv`).

`.\hectl.bat test SurvivorRegionObjectives` passed (`unit-test`). Strict
DX12 and Vulkan `game capture -Frames 320` and `-Frames 322` both passed
(`runtime-manual`). The added warm-up shifts the upgrade animation by two
frames. Comparing old frame 320 with new frame 322 gave RGB mean absolute
differences of 0.000073/0.000290/0.000431 on a 0–255 scale in both backends.
The aligned captures are `out/agent/final-enemy-vfx-preload-dx12-frame322.png`
and `out/agent/final-enemy-vfx-preload-vulkan-frame322.png`; the corresponding
old captures are `final-render-stage-after-frame320.png` and
`final-render-stage-vulkan-frame320.png`.

## Region prefab preparation

The Region pressure diagnostic was split further under the default-off
`HE_SURVIVOR_OBJECTIVE_TIMINGS=1` switch. At gameplay time 3.467–3.517 s,
each of three consecutive updates planned and activated eight enemies.
Position selection cost about 0.22–0.26 ms per update; activation cost
4–7 ms. The sampled activations all missed the entity pool. Individual
`HEngine.Instantiate` calls cost about 0.6–0.96 ms before the change, while
`RegisterSurvivorAgent` cost about 0.013–0.02 ms. These are diagnostic
observations, not full-frame speed claims.

`PrefabSerializer` now keeps a per-Level, resource-generation-keyed prepared
template for non-nested runtime prefabs. It avoids repeating document
expansion and component canonicalization for every pool miss. Nested prefabs
still expand on each instance to honor child invalidation. A temporary C++
timing probe, removed after measurement, found that the cached document's
per-instance ID/remap step took about 0.02 ms, entity identity creation
0.005 ms, and component deserialization 0.25–0.32 ms. The Python diagnostic
measured about 0.50–0.76 ms per instantiate afterward. The remaining cost
includes resource lookup and Python binding work as well as components.

`runtime-manual`: two diagnostic-off DX12 `game check` runs passed using the
same 500-frame Region fixture command and world-frame window 123–451 above.
For comparison, the prior two runs are `final-enemy-vfx-preload-1.csv` and
`final-enemy-vfx-preload-2.csv`:

| Run | CPU P50/P95/P99/max (ms) | >16.667 ms | Update P95 (ms) | GPU P95 (ms) |
| --- | --- | ---: | ---: | ---: |
| Before 1 | 11.88/16.97/25.87/56.29 | 18 | 6.49 | 4.79 |
| Before 2 | 11.96/16.96/26.60/55.68 | 18 | 6.44 | 4.82 |
| Prepared cache 1, `final-region-prefab-cache-raw-a.csv` | 12.13/16.29/26.33/56.48 | 16 | 6.38 | 4.84 |
| Prepared cache 2, `final-region-prefab-cache-raw-b.csv` | 11.89/17.21/25.86/55.51 | 17 | 6.28 | 4.80 |

The Update P95 is lower in both new runs, but whole-frame P95 varies in
opposite directions. The fixture still misses the 16.667 ms P95 target; no
whole-frame gain is claimed. `unit-test`: `.\hectl.bat test
RuntimePrefabResource` passed with added assertions for distinct instance
UUIDs, independent component values, and generation invalidation in one
Level. `build-only`: `.\hectl.bat game build` and
`.\hectl.bat editor build` passed. `unit-test`:
`.\hectl.bat test SurvivorRegionObjectives` passed after the
diagnostic-only Python changes. `static-check`: direct Python `compile()`
for `SurvivorGame.py`, `SurvivorWorld.py`, and `SurvivorObjectives.py`, plus
`git -c core.safecrlf=false diff --check`, passed. No new Vulkan device check
was performed for this prefab-only change.
After the cache key was extended from the project-relative name to the
resolved absolute path, the final source passed `.\hectl.bat test
RuntimePrefabResource` (`unit-test`), `.\hectl.bat game check -Frames 300
-StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12
-StrictGraphicsApi -RenderValidation -Resolution 1280x720
-FixedDeltaSeconds 0.0166667 -TimeoutSeconds 240` with
`HE_SURVIVOR_CAPTURE_SCENARIO=region_4_final` (`runtime-manual`), and
`.\hectl.bat editor build` (`build-only`). The 300-frame run is a
lifecycle check, not an additional frame-time comparison.
Both `.\hectl.bat game build -Configuration Debug` and
`.\hectl.bat editor build -Configuration Debug` passed (`build-only`) on
the final source.

The next diagnostic-only DX12 run (`HE_RML_RECORD_TIMINGS=1`, 340 frames,
`out/agent/final-ui-tail-diagnostic.csv`) passed (`runtime-manual`). At
world frames 305–309, `cpuRuntimeRenderMs` fell from 40.05/46.49/33.03/
23.48/17.85 ms, while `gpuFrameSpanMs` stayed near 4–5 ms. The RmlUi
backend logged 89, 96, then 69 geometry uploads during the first three
slow overlay frames, with 58, 0, then 8 buffer reuses. The associated
`RmlUi::Render` calls took 31.25, 37.19, and 24.20 ms. This supports
first-show geometry churn as the remaining render-recording tail; it does
not isolate the cost of each allocation versus upload or justify changing
the visual animation without a controlled comparison.

### Shared RmlUi geometry pages

A controlled CSS probe removed the upgrade modal's entry animation for one
run, then restored the original file byte-for-byte. The 305–309 render tail
shortened from five frames to two, but the first UI frame grew from 57.08
to 61.61 ms. A transform-only entry animation produced the same pattern.
Neither visual change was kept. A default-off backend timing probe then
measured 21–33 ms inside `createBuffer` on three slow upgrade frames: each
created about 120–192 vertex/index buffers for just 50–96 KiB of geometry.
`writeBuffer` took under 2 ms per sampled frame. This identified native
buffer creation, rather than byte copy, as the dominant work in that path.

The RmlUi NVRHI backend now suballocates small geometry into bounded 1 MiB
vertex/index page pairs. A generation-tagged compiled geometry retains its
offset; a released offset cannot be reused until all relevant frame slots
complete. Oversize/full-arena geometry uses the previous individual-buffer
path. The diagnostic-on DX12 340-frame run passed (`runtime-manual`), and
no upgrade-frame `RmlRecordTiming` exceeded the 20 ms logging threshold.
The first upgrade frame's overall CPU time fell from 57.08 to 34.17 ms in
that diagnostic comparison; its update stage still took 16.01 ms.

Two diagnostic-off DX12 500-frame `game check` runs passed (`runtime-manual`)
with the same scene, validation, fixed delta, and world-frame window 123–451
as the prefab-cache comparison:

```powershell
$env:HE_SURVIVOR_CAPTURE_SCENARIO='region_4_final'; .\hectl.bat game check -Frames 500 -PerformanceWarmupFrames 120 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667 -RenderStats -RenderStatsPath out/agent/final-ui-arena-raw-a.csv -TimeoutSeconds 240
```

The second run used `final-ui-arena-raw-b.csv` for `-RenderStatsPath`.

| Run | CPU P50/P95/P99/max (ms) | >16.667 ms | Update P95 (ms) | GPU P95 (ms) |
| --- | --- | ---: | ---: | ---: |
| Before 1, `final-region-prefab-cache-raw-a.csv` | 12.13/16.29/26.33/56.48 | 16 | 6.38 | 4.84 |
| Before 2, `final-region-prefab-cache-raw-b.csv` | 11.89/17.21/25.86/55.51 | 17 | 6.28 | 4.80 |
| Arena 1, `final-ui-arena-raw-a.csv` | 11.82/16.09/19.20/32.59 | 12 | 6.17 | 4.81 |
| Arena 2, `final-ui-arena-raw-b.csv` | 11.78/15.84/19.21/36.32 | 10 | 6.26 | 4.71 |

This establishes a repeatable DX12 P99 and first-show tail reduction in the
selected fixture. Both new P95 samples meet the 16.667 ms target there; the
result does not generalize to Vulkan, whose CPU recording remains slower.
The sparse local GPU memory samples peaked at 603.88 MiB before and
604.24 MiB after, a measured 0.36 MiB difference that is too sparsely sampled
to be a precise arena cost. Strict DX12 and Vulkan 322-frame `game capture`
runs passed (`runtime-manual`). Both 1280×720 RGB outputs were pixel-identical
to the same-frame pre-arena captures: mean absolute difference [0,0,0],
empty difference bounding box. The outputs are
`final-ui-arena-dx12-frame322.png` and
`final-ui-arena-vulkan-frame322.png` under `out/agent`.

An optimized `HEngineRmlDX12Smoke` build and bounded run passed
(`runtime-manual`), including NVRHI validation, generation recycling,
aborted-upload rollback, resize, and renderer shutdown. Its DX12 native
debug layer was unavailable; NVRHI validation remained enabled. The final
range-overlap guard passed a second optimized smoke build and bounded run.
The exact supported command was `powershell -ExecutionPolicy Bypass -File
Tools/Agent/Build.ps1 -Target HEngineRmlDX12Smoke -Configuration RelWithDebInfo
-Run -Wait -TimeoutSeconds 120`.
A 340-frame strict Vulkan Game check passed with no validation error
(`runtime-manual`); Vulkan's 300–315 CPU frames remained about 29–49 ms,
so the DX12 performance claim does not apply there.
An additional strict Vulkan 340-frame diagnostic used
`HE_SURVIVOR_CAPTURE_SCENARIO=region_4_final`,
`HE_RML_RECORD_TIMINGS=1`, `HE_RUNTIME_SYSTEM_TIMINGS=2`, and
`.\hectl.bat game check -Frames 340 -PerformanceWarmupFrames 120
-StartupScene Assets/Scenes/MainGame.he -GraphicsApi Vulkan -StrictGraphicsApi
-RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667
-RenderStats -RenderStatsPath out/agent/vulkan-ui-tail-diagnostic.csv
-TimeoutSeconds 240` (`runtime-manual`: PASS). In the 218 samples with world
frames 120–339, CPU P50/P95/P99/max were 29.837/34.046/39.485/51.488 ms;
all 218 exceeded 16.667 ms. GPU span P50/P95 was 6.448/11.507 ms.
CPU graph record P50/P95 was 22.679/25.098 ms, whereas graphics submission
P50/P95 was 0.267/0.333 ms. Across world frames 298–320, the top mean CPU
record passes were `DirectionalShadow` 9.932 ms, `RasterGBuffer` 3.485 ms,
`SceneDepthPrepass` 3.129 ms, and `VFXTransparent` 2.415 ms. Matched DX12
arena data for the same frame IDs had 2.521, 0.792, 0.836, and 1.105 ms.
`RuntimeUiRender` averaged 3.894 ms on Vulkan, but its first-show maximum
was 11.012 ms. The persistent Vulkan tail is therefore concentrated in graph
draw recording, especially directional shadows; RmlUi is a separate first-show
spike. This is diagnosis, not evidence of the exact Vulkan driver/API call
responsible, and no speculative backend change was made.
The existing `Rendering.GpuBatching.ShadowsEnabled` setting was tested as a
session-only command-line override in
`out/agent/vulkan-shadow-batching-on.json` containing
`{"Rendering.GpuBatching.ShadowsEnabled":true}`. A matched strict Vulkan 340-frame
`game check` with `-RenderSettingsPath` and
`-RenderStatsPath out/agent/vulkan-shadow-batching-on.csv` passed
(`runtime-manual`). The effective setting was true.
The exact measured command was:

```powershell
$env:HE_SURVIVOR_CAPTURE_SCENARIO='region_4_final'; .\hectl.bat game check -Frames 340 -PerformanceWarmupFrames 120 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi Vulkan -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667 -RenderSettingsPath C:\WorkSpace\HEngineDev\out\agent\vulkan-shadow-batching-on.json -RenderStats -RenderStatsPath out/agent/vulkan-shadow-batching-on.csv -TimeoutSeconds 240
```

Median directional shadow
indirect draw calls fell from 252 to 6 and graph record P50/P95 from
22.679/25.098 to 21.678/23.985 ms. Whole CPU frame P50/P95/P99 changed
from 29.837/34.046/39.485 to 28.502/32.914/37.230 ms. In frames 298–320,
directional shadow record mean changed from 9.932 to 8.832 ms. A strict
Vulkan 322-frame capture with the override was RGB pixel-identical to the
same-frame default capture; both used `region_4_final` and fixed 1/60 s.
The option is left at its existing default because this single fixture still
misses 16.667 ms and does not establish performance or visual equivalence
across other scenes and adapters. The bounded per-draw Vulkan recording path
remains the main performance target.
The main geometry batcher was then added to the same session-only override
(`out/agent/vulkan-geometry-batching-on.json` contains both
`Rendering.GpuBatching.Enabled=true` and
`Rendering.GpuBatching.ShadowsEnabled=true`). The matched strict Vulkan
340-frame `game check` passed (`runtime-manual`) with
`-RenderSettingsPath C:\WorkSpace\HEngineDev\out\agent\vulkan-geometry-batching-on.json`
and `-RenderStatsPath out/agent/vulkan-geometry-batching-on.csv`; all other
arguments and the `region_4_final` fixture matched the preceding command. CPU
P50/P95 became 28.108/32.358 ms, graph record P50/P95 21.289/23.411 ms,
and GPU span P50/P95 5.892/10.714 ms. The frame-298–320 depth-prepass and
GBuffer record means fell from 3.129/3.485 to 2.755/3.013 ms. A strict
Vulkan 322-frame capture with both settings enabled was RGB pixel-identical
to the default capture. The settings remain opt-in: even combined, this
fixture exceeds the 16.667 ms budget. At world frame 310, the renderer still
reported about 322 CPU directional shadow casters per cascade on three
cascades. Static draw compaction therefore cannot remove most shadow recording
work in this fixture. A later matched diagnostic measured the per-draw cost
below.
The same two opt-in flags were also tried on the matched DX12 500-frame
`region_4_final` fixture with strict validation and fixed 1/60 s. The
`game check` passed (`runtime-manual`), storing
`out/agent/dx12-geometry-batching-on.csv`. In world frames 123–451, CPU
P50/P95/P99 were 11.280/15.841/19.632 ms with 11/329 frames above 16.667 ms.
The two default-setting runs had 11.830/16.109/19.204 (12/329) and
11.799/15.876/19.211 (10/329) ms. Render graph record P50/P95 improved
from 6.364/7.463 and 6.368/7.650 to 5.686/6.658 ms, while the whole-frame
P95 and over-budget counts overlapped the defaults and P99 was slightly
higher. The strict DX12 frame-322 capture with both flags was pixel-identical
to the default capture. The DX12 default remains unchanged on this evidence.
### Matched Vulkan/DX12 directional-shadow recording breakdown (2026-09-27)

A temporary, default-off `HE_SHADOW_DRAW_TIMINGS=1` probe split the CPU
directional-shadow loop into visibility, graph-resource lookup, volatile
constant write, pipeline lookup, state construction, NVRHI
`setGraphicsState`, push constants, and `drawIndexed`. The probe was removed
after capture, leaving the normal hot loop unchanged. Both optimized Game
checks used the same `region_4_final` fixture, 340 frames, 120 warm-up frames,
1280×720, fixed 1/60 s, strict API selection, and NVRHI validation. Each
passed (`build-only` HGame and `runtime-manual` bounded Game); both stderr
logs were empty. The measured window was world frames 298–320 inclusive,
23 shadow passes on each API. Mean CPU shadow draws per pass were 917.87 on
both APIs, ranging from 870 to 1005.

| 23-pass mean (ms) | Vulkan | DX12 |
| --- | ---: | ---: |
| Whole directional-shadow callback | 11.979 | 2.850 |
| Visibility + resource lookup + constant write | 0.373 | 0.216 |
| Pipeline lookup + state construction | 0.986 | 0.632 |
| `setGraphicsState` | 5.573 | 1.119 |
| Push constants | 0.823 | 0.042 |
| `drawIndexed` | 2.515 | 0.163 |

The Vulkan `setGraphicsState` call is the largest individually measured CPU
section; `drawIndexed` is second. The callback also includes cascade clears,
GPU-batched draws, loop overhead, and instrumentation overhead, so the rows
need not sum to the whole callback. This probe establishes a backend-specific
recording cost under this fixture, not a driver-independent cause or a
whole-frame speedup. NVRHI's checked-in Vulkan implementation performs
automatic barrier checks and updates volatile-buffer binding sets in this
path. Reducing the number of CPU shadow draws would address the measured
submission cost; the existing static GPU shadow batching leaves these
dynamic CPU casters, so that is a separate design change requiring output
and device validation. No third-party NVRHI file was modified.

A second temporary eligibility counter ran in the normal DX12
`region_4_final` 320-frame Game check (`runtime-manual`: PASS), then was
removed. At world frame 310 it counted 678 shadow items: 388 already
GPU-eligible and 290 skinned CPU fallbacks; zero custom-material, alpha-mask,
or missing-GPU-row fallbacks. Three directional cascades explain the 870 CPU
draws in the timing sample. The current GPU shadow path uses static vertex
input and geometry-arena pages, while skinned meshes use their own vertex
buffers and require a per-instance bone-palette offset. Reclassifying the
290 items as static GPU draws would render the wrong character poses. A
future skinned GPU path would require its own shader permutation, matching
input layout and object rows, instance-safe bone offsets, and matched visual
and lifetime tests. The eligibility log is
`out/agent/logs/run-game-relwithdebinfo-20260927-155128-127-123624.stdout.log`.

The exact routes were:

```powershell
$env:HE_SURVIVOR_CAPTURE_SCENARIO='region_4_final'; $env:HE_SHADOW_DRAW_TIMINGS='1'; .\hectl.bat game check -Frames 340 -PerformanceWarmupFrames 120 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi Vulkan -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667 -RenderStats -RenderStatsPath out/agent/vulkan-shadow-draw-breakdown-final-20260927.csv -TimeoutSeconds 240
$env:HE_SURVIVOR_CAPTURE_SCENARIO='region_4_final'; $env:HE_SHADOW_DRAW_TIMINGS='1'; .\hectl.bat game check -Frames 340 -PerformanceWarmupFrames 120 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667 -RenderStats -RenderStatsPath out/agent/dx12-shadow-draw-breakdown-final-20260927.csv -TimeoutSeconds 240
```

The corresponding stdout logs are
`out/agent/logs/run-game-relwithdebinfo-20260927-154240-209-91272.stdout.log`
and `out/agent/logs/run-game-relwithdebinfo-20260927-154314-596-89456.stdout.log`.
Diagnostic whole-frame P95 is deliberately excluded from the performance
comparison because per-draw clock sampling and logging affect frame time.

After removing the probe, the same source passed matched strict DX12 and
Vulkan 340-frame `game check` runs (`runtime-manual`), using the same commands
without `HE_SHADOW_DRAW_TIMINGS` and writing
`dx12-shadow-normal-after-probe-20260927.csv` and
`vulkan-shadow-normal-after-probe-20260927.csv`. In world frames 120–339,
the 218 valid Vulkan CPU frames had P50/P95/P99
31.637/38.399/41.462 ms, graph-record P95 27.222 ms, and 218 frames above
16.667 ms. The corresponding DX12 values were
13.697/18.844/24.727 ms, graph-record P95 8.871 ms, and 32 over-budget
frames. These are single post-probe runs and do not demonstrate an
improvement; they confirm that the uninstrumented Vulkan fixture still
misses 60 FPS. All four CSVs and stdout logs are pinned in
`out/agent/current-shadow-breakdown-20260927/` with `.he-keep`.

On the final shared-page source, `.\hectl.bat editor build`,
`.\hectl.bat game build -Configuration Debug`, and
`.\hectl.bat editor build -Configuration Debug` passed (`build-only`).
The first smoke command included an unsupported `-RenderValidation` option
and was rejected before build/run; the supported `-Run -Wait` smoke command
above passed.
Final-source `.\hectl.bat game check -Frames 340 -StartupScene
Assets/Scenes/MainGame.he -GraphicsApi <DX12|Vulkan> -StrictGraphicsApi
-RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667
-TimeoutSeconds 240` passed for both APIs with
`HE_SURVIVOR_CAPTURE_SCENARIO=region_4_final` (`runtime-manual`).

## Shared low-core CPU budget (2026-09-27)

The resource registry, Jolt, and TaskScheduler now use the same
`PlanRuntimeCpuBudget` result. For reported 4/5 logical processors, resource
workers drop from two to one, keeping the main thread, texture worker,
Jolt worker, and (at five cores) one task worker within the core count. At
three cores Jolt and TaskScheduler use their caller-thread fallbacks. The
current 16-logical-processor machine retains two resource, three task, and
nine Jolt workers. One or two-core machines still have separate blocking
resource and texture IO threads; no real low-core device run was made.
The checked-in Jolt `JobSystemThreadPool` implementation explicitly supports
zero worker threads and executes barrier jobs on the caller.

`unit-test`: `.\hectl.bat test TaskScheduler -Configuration RelWithDebInfo`
and `.\hectl.bat test TaskScheduler` both passed with budget assertions for
reported core counts 0, 1, 2, 3, 4, 5, 6, 8, and 16, alongside existing DAG
and low-worker scheduling contracts. `build-only`: `.\hectl.bat editor build`,
`.\hectl.bat game build -Configuration Debug`, and
`.\hectl.bat editor build -Configuration Debug` passed. `runtime-manual`:
`.\hectl.bat game check -Frames 120 -StartupScene Assets/Scenes/MainGame.he
-GraphicsApi <DX12|Vulkan> -StrictGraphicsApi -Resolution 1280x720
-RenderValidation -TimeoutSeconds 180` passed for each backend; both commands
also built optimized Game. This device evidence exercises the current
16-core budget, while the low-core allocations are covered by pure tests.

After these builds, the just-completed Debug Game, Editor, and Tests caches
were removed with separate `hectl clean cache -CachePath out/agent/<name>
-Json` calls. Their build logs remain under `out/agent/logs`; optimized
Game/Editor and Tests caches remain available for further verification.

## Disk space after validation

The first optimized HEngineTests build reduced C: free space enough to
threaten another full build. `Get-Volume -DriveLetter C` reported 3.04 GiB
free before cleanup. A repository `clean stale` attempt kept rescanning a
large historical record set and did not finish; the owned collector was
stopped, so no stale-clean success is claimed. `hectl clean cache` removed
the six idle local Game, Editor, and Tests build caches with zero failures.
It refused two other whole caches because they contained historical sessions
or registered evidence. After verifying absolute paths, process absence,
reparse points and `.he-keep`, only their known `Engine`, `deps`, `bin`,
`GeneratedContent`, and package `Release` build portions were removed;
sessions and evidence stayed. About 45.6 GiB of logical cache files were
deleted. C: free space rose to 38.85 GiB, an observed physical increase of
35.81 GiB. Other large directories such as `upgrade-motion`, `build-publish`
and `aster-benchmark` were retained because they contain historical output
whose ownership and recovery value were not established. The later controlled
dense benchmark rebuilt the current Game and temporary baseline worktrees;
after removing those two worktrees, C: had 33.42 GiB free.

The later optimized HEngineTests first build was stopped while C: had only
5.69 GiB free, before a space failure. The required `hectl clean stale -Json`
attempt spent over 20 seconds scanning historical records without finishing;
the owned collector was interrupted. The idle Debug Editor, Game, and Tests
caches were verified under `C:\WorkSpace\HEngineDev\out\agent`, without
reparse points, `.he-keep`, or owning build/game processes, and removed with
`hectl clean cache -CachePath out/agent/<editor-debug|game-debug|tests-debug> -Json`
in separate calls. All three reported zero failures and no `.retired-*`
residual. Logical bytes deleted totaled 20,403,417,382 (19.00 GiB). C:
free space rose from 5.69 to 24.71 GiB, an observed increase of 19.02 GiB.
The optimized test target then built and passed; its new cache reduced free
space to about 19.57 GiB. Optimized Game/Editor caches remain in use for
runtime checks. `upgrade-motion`, `build-publish`, and `aster-benchmark`
were retained because their ownership and recovery value are unknown.

### Fixed Python gameplay callback and same-scene timing (2026-09-27)

`Level` now invokes optional Python `FixedUpdate(Timestep)` after each
completed Jolt and character-movement tick. `SurvivorGame` moved combat,
spawn, pickup, domain-power, and region-objective simulation into this
callback. Display `Update` still handles menus, pause, audio, warm-up, and
HUD. The gameplay callback consumes exactly the configured fixed dt;
an Escape edge that will pause in display `Update` skips gameplay on that
frame. This changes the region objective clock from unscaled presentation
time to simulated time, including deliberate time drops after long stalls.
Native and managed scripts without a fixed callback retain their existing
display update semantics. `HE_RUNTIME_SYSTEM_TIMINGS=3` logs every frame's
`FixedTicks`, `FixedDroppedSeconds`, and separate `FixedGameplay` cost.

The following Debug `unit-test` commands passed after the migration:

```powershell
.\hectl.bat test PythonRuntime
.\hectl.bat test SurvivorFixedUpdate
.\hectl.bat test FixedSimulationClock
.\hectl.bat test SurvivorRegionObjectives
```

The Python runtime test verifies two 1/60 callbacks and Timestep delivery.
The project contract checks 1/60, 1/60, and 1/30 dt propagation through
bookkeeping, objectives, weapons, and powers, with pause, unfinished
warm-up, and Escape suppression. An old region-objective static expectation failed on the
intentional clock change and was updated; its final command passed.

Optimized `build-only` Game and `runtime-manual` bounded DX12 checks passed
with the command below and `-FixedDeltaSeconds` values of `0.0333333`
(60 frames), `0.00833333` (240 frames), and `0.2` (50 frames), each with
`HE_RUNTIME_SYSTEM_TIMINGS=3` and `-RenderValidation`. After warm-up, the
30 FPS run had 42 frames with two fixed ticks and one pause transition with
zero; the 120 FPS run had 111 frames with one tick and 112 with zero. The
200 ms run had 32 frames with four ticks and one pause transition with zero;
its maximum reported dropped time was 0.133333 seconds. All three had zero
stderr lines and no Python exception. Logs:
`out/agent/logs/run-game-relwithdebinfo-20260927-122509-*.stdout.log`,
`...-122531-*.stdout.log`, and `...-122632-*.stdout.log`.

```powershell
$env:HE_RUNTIME_SYSTEM_TIMINGS = '3'
.\hectl.bat game check -Frames 60 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Resolution 1280x720 -RenderValidation -FixedDeltaSeconds 0.0333333 -TimeoutSeconds 180
.\hectl.bat game check -Frames 240 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Resolution 1280x720 -RenderValidation -FixedDeltaSeconds 0.00833333 -TimeoutSeconds 180
.\hectl.bat game check -Frames 50 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Resolution 1280x720 -RenderValidation -FixedDeltaSeconds 0.2 -TimeoutSeconds 180
```

The optimized dense fixture was checked three times after the callback
change, using `HE_SURVIVOR_CAPTURE_SCENARIO=region_4_final` and:

```powershell
$env:HE_SURVIVOR_CAPTURE_SCENARIO = 'region_4_final'
.\hectl.bat game check -Frames 500 -PerformanceWarmupFrames 120 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667 -RenderStats -RenderStatsPath out/agent/fixed-python-dense-20260927.csv -TimeoutSeconds 240
```

The same command used output suffixes `-b` and `-c` for repeats. In the
matched world-frame 123–451 interval (329 samples), the two older dense
runs had CPU frame P50/P95/P99 of 11.830/16.109/19.204 and
11.799/15.876/19.211 ms, with 12 and 10 frames above 16.667 ms.
New repeat A was 12.973/21.926/28.172 ms, with 55 misses; repeats B and C
were 11.867/15.941/19.787 and 11.857/16.035/19.503 ms, with 11 and 13
misses. Old/new median object counts matched (192 enemies, 328 GPU-driven
instances, 283 visible, 1203 shadow draws, 433 input-to-render objects).
This is a mixed result: two current runs are near the old range, one is
substantially slower. It does not establish a stable gain or regression.
The older runs predate other recent edits, so this comparison cannot isolate
the fixed callback's cost. The first slow run had Update P95 8.235 ms versus
6.292/6.217 ms on repeats B/C; GPU span P95 was 5.285 versus 4.738/4.735
ms. No input-to-physical-display latency is inferred from the
input-to-render object count.

Final-source `build-only` commands passed: `.\hectl.bat game build
-Configuration Debug` (416.4 s), `.\hectl.bat editor build -Configuration
Debug` (381.7 s), and `.\hectl.bat editor build` (82.9 s). The strict
Vulkan `runtime-manual` check passed with `build-only` optimized HGame
and 120 rendered frames on NVIDIA GeForce RTX 3060 Laptop GPU:

```powershell
.\hectl.bat game check -Frames 120 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi Vulkan -StrictGraphicsApi -Resolution 1280x720 -RenderValidation -TimeoutSeconds 180
```

Vulkan validation was enabled, adventure started after warm-up, exit was
zero, and stderr was empty; the log is
`out/agent/logs/run-game-relwithdebinfo-20260927-125208-548-76600.stdout.log`.
After the last Python guard/test edit, a fresh Debug HEngineTests build
passed (325.1 s), and all four commands above passed again. Python AST
parsing for the game and project contract plus
`git -c core.safecrlf=false diff --check` passed (`static-check`).

Before the Debug Editor build, C: free space was about 7.6 GiB. A bounded
`hectl clean stale -Json` scan did not finish and was interrupted; it is
not claimed as a successful cleanup. The idle `game-debug` and
`tests-debug` caches were verified as direct children of the workspace
`out/agent` directory, without reparse points or `.he-keep`, and with no
Game, Editor, Tests, compiler, or linker processes. Exact `hectl clean
cache -CachePath out/agent/game-debug -Json` and the analogous
`tests-debug` command passed with zero failures, reporting 6,292,482,880
and 6,381,475,871 logical bytes deleted. C: free space rose to 19.43 GiB;
no `.retired-*` directory remained. Those two caches were rebuilt for
the final checks; the current free space is therefore lower. Build and
run logs under `out/agent/logs` were retained.

### Paired scene streaming and old global-wait count (2026-09-27)

An unchanged detached baseline at commit `8b8070e5` in
`E:\HEngineBaselineMemory` and the current worktree each ran the same
optimized DX12 Agent route on the RTX 3060 Laptop GPU, 1280×720,
strict backend selection, NVRHI validation, VSync off, and 1/60-second
Agent steps. Both started `MainMenu.he`, requested `MainGame.he`, and
advanced two groups of 120 simulation steps. The processes continued
rendering while Agent control was paused; their total displayed-frame
counts therefore differ. The analysis aligns the first CSV frame whose
`inputRenderObjects` exceeds 50 (the scene submission frame), then takes
the next 120 displayed frames. The object count at the end of that window
was 138/139 for the two baseline runs and 138 for both current runs;
all final world summaries reported 437 entities. This is a scene-streaming
sample, not a texture-only benchmark.

The exact `runtime-manual` route, run in each worktree with a distinct
`-RenderStatsPath`, was:

```powershell
$env:HE_TEXTURE_UPLOAD_TIMINGS = '1'
$case = 'baseline-streaming-scene-20260927-b' # Set per run as listed below.
try {
    .\hectl.bat agent start -StartupScene Assets/Scenes/MainMenu.he -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -RenderStats -RenderStatsPath "out/agent/$case.csv" -PerformanceWarmupFrames 0 -TimeoutSeconds 240
    $json = @{path='Assets/Scenes/MainGame.he'} | ConvertTo-Json -Compress
    & .\Tools\Agent\hectl.ps1 agent call -Method runtime.scene.load -ParamsJson $json
    .\hectl.bat agent step -AgentStepFrames 120
    .\hectl.bat agent step -AgentStepFrames 120
    .\hectl.bat agent world
} finally {
    .\hectl.bat agent stop
}
```

`<case>` was `baseline-streaming-scene-20260927-b`,
`baseline-streaming-scene-20260927-c`,
`current-streaming-scene-20260927`, and
`current-streaming-scene-20260927-b` respectively. All four Game builds,
Agent starts, scene requests, 240 steps, 437-entity final summaries,
and owned stops passed. Original frame and memory CSVs and relevant logs
are pinned in `out/agent/baseline-streaming-20260927/`.

| Run | Scene submission CPU wall ms | Following 120 CPU P50/P95/P99/max ms | Frames >16.667 ms | GPU span P95 ms | Sampled local GPU memory peak MiB |
| --- | ---: | --- | ---: | ---: | ---: |
| Baseline B | 1067.537 | 9.392 / 212.303 / 390.098 / 543.988 | 28 | 10.888 | 633.94 |
| Baseline C | 954.799 | 9.755 / 62.983 / 186.616 / 191.274 | 23 | 4.814 | 614.29 |
| Current A | 334.664 | 8.033 / 35.942 / 63.898 / 74.066 | 18 | 3.607 | 644.00 |
| Current B | 291.752 | 7.498 / 28.067 / 44.887 / 49.486 | 14 | 2.879 | 621.50 |

The sampled window shows fewer long CPU frames in the current tree, but
both versions vary considerably. Sampled local GPU memory overlaps and
does not show a proven reduction. The engine, scripts, scene transaction,
and texture implementation all differ between the two revisions; none of
the frame-time difference can be assigned solely to eliminating a GPU
global wait. The baseline loader was synchronous and had no pending
preparation queue. Current logs reported `peakPending=5`, final
`pending=0`, `globalWaits=1` in both runs; that one wait creates startup
defaults. Current aggregate request-to-publication P95 was 246.663 and
210.906 ms, respectively, across 11 completed uploads including startup,
so these values do not isolate the scene request.

For an exact old wait count, a separate diagnostic build of the E: baseline
logged the return of each of its three existing `TextureManager`
`waitForIdle` sites when `HE_TEXTURE_UPLOAD_TIMINGS=1`; it did not change
upload or publication behavior. The same scene route produced 12 global
waits in total: three before the scene request and nine after it. The nine
post-request waits summed to 3.668 ms, with a 1.244 ms maximum. This is
the measured wait duration itself, not the scene transaction's CPU cost.
That diagnostic stdout and empty stderr are pinned with the CSVs. The
temporary E: source edit was restored after the run; the uninstrumented
baseline CSVs above remain the performance evidence.

The same transition was separately sampled for process memory using
`Tools/Agent/MeasureSceneStreamingMemory.ps1`. This helper verifies the
Game executable path and start time, samples the owned PID every 50 ms,
advances the same two 120-step groups, and stops the Agent session in
`finally`. Its first development attempts failed while capturing a child
stdout pipe, expecting a PID from active status, and comparing serialized
DateTimes across a PowerShell Job. They are not evidence of successful
memory measurements. The corrected helper passed once per revision with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:/WorkSpace/HEngineDev/Tools/Agent/MeasureSceneStreamingMemory.ps1 -Repository E:/HEngineBaselineMemory -Prefix E:/HEngineBaselineMemory/out/agent/baseline-streaming-memory-20260927-d
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:/WorkSpace/HEngineDev/Tools/Agent/MeasureSceneStreamingMemory.ps1 -Repository C:/WorkSpace/HEngineDev -Prefix C:/WorkSpace/HEngineDev/out/agent/current-streaming-memory-20260927
```

| Run | 50 ms samples | OS working-set high-water MiB | Sampled private-byte peak MiB |
| --- | ---: | ---: | ---: |
| Baseline | 137 | 458.50 | 1356.00 |
| Current | 114 | 450.97 | 1362.75 |

These are process-lifetime values including startup and scene loading, not
an isolated per-texture memory cost. The small, opposite-direction
differences do not establish a memory improvement. The two passing JSON
summaries, CSVs and stdout logs are pinned with the scene-streaming frame
samples. Both sessions ended normally and reported 437 final entities.

After evidence capture, C: free space was 6.59 GiB. The completed Debug
Editor and Tests caches were direct workspace `out/agent` children with
CMakeCache files, no reparse points or `.he-keep`, and no owning build or
engine process. Exact `hectl clean cache -CachePath
out/agent/editor-debug -Json` and the analogous `tests-debug` call both
passed with zero failures, deleting 7,233,829,097 and 6,381,386,536
logical bytes. Actual C: free space rose to 19.28 GiB; no retired cache
directory remained. Optimized Game/Editor executables, logs and pinned
comparison CSVs were retained.

### Attempted input-to-display capture with PresentMon

The local Windows WPR profiles still fail with policy error `0xc5585011`
before a trace starts. The official portable PresentMon 2.6.0 CLI was
downloaded to `out/agent/tools/PresentMon/` from the
[Intel release](https://github.com/GameTechDev/PresentMon/releases/tag/v2.6.0)
and matched the release SHA-256
`b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af`.
Its timed ETW policy probe started and stopped successfully. PresentMon
defines `MsAllInputToPhotonLatency` as the time from an input contributing
to a frame until that frame is displayed; it requires a qualifying input
event in the trace ([metric definition](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md)).

A 240-frame optimized DX12 Game check with the existing no-key/W/release
replay passed, and PresentMon recorded 125 displayed frames for the correct
Game PID; all 125 input-latency entries were `NA`. That replay injects
engine input rather than a physical OS device event. A separate bounded
1200-frame DX12 Game check passed after a helper sent 24 W-key events to
the foreground Game window after the adventure warm-up log. PresentMon
completed its own timed capture and recorded 454 displayed frames for the
correct PID; all 454 `MsAllInputToPhotonLatency` and
`MsClickToPhotonLatency` entries were `NA`. The helper, CSVs, Game result,
and PresentMon stdout/stderr are pinned in
`out/agent/input-latency-20260927/`. Both captures reported a warning
about non-elevated access to short-lived or other-account processes, but
did identify the actual Game PID. The attempted synthetic events were not
attributed to displayed frames. No input-to-display percentile is reported;
the metric remains unverified without qualifying device input or an
alternative trusted measurement route.

### Scene transition tail: first projectile visual

The follow-up DX12 diagnostic used the same Menu -> MainGame Agent route,
`HE_SURVIVOR_STAGE_TIMINGS=1`, 1280x720, validation on, and 240 stepped
simulation ticks. The initial gameplay `comet` cast took 37.676 ms; its
first `HEngine.Instantiate` of the projectile visual took 35.772 ms.
`graphBuildReasonMask=64` and `256` in adjacent slow frames mean VFX and
texture-table changes respectively (see `SceneGraphRebuildReason`), and
`cpuGraphRecordMs` also has separate spikes. Thus the projectile prefab
explains one gameplay spike, not the entire transition tail.

The selected starting weapons now prewarm one projectile visual of each
used kind, one per loading frame, hide it, and place it in the existing
pool before adventure readiness. The unchanged first-use diagnostic
logged the comet visual's 36.201 ms instantiate in the loading stage;
there was no gameplay `SurvivorWeaponSpike` over the 3 ms diagnostic
threshold. A real-device strict DX12 240-tick run passed with no Python
exception and 437 final entities. The `unit-test`
`.\hectl.bat test SurvivorProjectileVisualWarmup` passed, checking
one visual per tick, hidden pooled objects, stable kind order, and the
resources-stage transition. Its first run failed because the test double
lacked `pending_effects`; the fixture was fixed and the same command
passed. An earlier diagnostic run before the change was invalidated by
a missing timing flag import in `SurvivorWeapons.py`; it was not used for
comparison.

For the first 120 displayed frames after the scene submission frame,
the no-prewarm diagnostic gave CPU P50/P95/P99/max
7.430/28.531/42.051/45.981 ms and 17 frames over 16.667 ms. The
prewarm run gave 6.767/27.249/39.986/51.813 ms and 13 over-budget
frames. These are single, variable runs with different warm-up timing;
the scene submission itself was 301.875 vs 302.732 ms. The change is
retained for removal of the first gameplay prefab cost, not as proof of
a lower overall transition P95. The transition still exceeds the 60 FPS
frame budget, mainly during synchronous loading stages and occasional
render graph recording. Frame CSVs and logs are in
the pinned `out/agent/current-visual-warmup-20260927/` directory;
the original Agent sessions were `20260927-141148-102-c0130f911f32`
and `20260927-141313-523-a0b3be50cec3`.
An additional `runtime-manual` strict DX12 session started in MainGame,
stepped 120 ticks, loaded MainMenu for 15 ticks, then loaded MainGame for
120 ticks and stopped its owned process. It passed both loads, both
adventure warm-ups, and render validation; the middle Menu had 7
entities and final MainGame had 433, with no Python exception or stderr.
Its CSV and log are pinned beside the paired evidence under
`out/agent/current-visual-warmup-20260927/`.

### Large RmlUi image decode moved off the recording thread

Per-pass CPU graph rows in the scene-transition capture attributed one
17.935 ms graph-recording frame to a 15.228 ms `RuntimeUiRender` callback.
Default-off `HE_RML_RECORD_TIMINGS=1` then exposed a larger first-render
cost: the 1672×941 menu and loading PNGs each needed roughly 46–50 ms of
stb decode inside Rml's synchronous `LoadTexture`. Those are measured CPU
stalls, not NVRHI global GPU waits.

The Rml backend now reads image metadata and creates the handle on its
owner thread, sends only large-image decode/premultiplication to one bounded
worker, draws a transparent pending texture, and uploads/publishes the final
pixels at a later `BeginFrame`. The shared queue permits at most four
outstanding results and 16 MiB encoded request bytes through publication
or release; if full, it decodes synchronously.
`HE_TASKS_SERIAL=1` and `HE_RML_ASYNC_DECODE=0` select the old path.
`PlanRuntimeCpuBudget` reserves the extra worker only from eight logical
cores; the `TaskScheduler` Debug unit test passed with the updated budget.

Optimized `.\hectl.bat game build` and `.\hectl.bat editor build` passed
(`build-only`). Strict DX12 and Vulkan Menu -> MainGame Agent runs with
validation, 240 simulated ticks, and 437 final entities passed
(`runtime-manual`); no RHI validation, Python, or stderr error was logged.
The DX12 menu background was queued in 1.878–1.996 ms owner time and
became ready in 60.689–84.327 ms; the loading background was queued in
1.741–3.540 ms. The Vulkan backgrounds were queued in 1.785/1.446 ms
and ready in 59.089/55.631 ms. A `HE_TASKS_SERIAL=1` 90-frame DX12
Game check passed and logged the menu PNG on the synchronous path at
53.525 ms. With a diagnostic 5-second decode delay, stopping an owned
DX12 Agent session logged `releasedPending` for the menu background and
stopped cleanly with empty stderr. The Menu -> MainGame route with the
same delay completed 240 ticks and 437 entities; its UI document kept
the background handle across that transition, so this was a retained
resource, not a cancellation event.

Two paired same-source DX12 comparisons used the exact Menu -> MainGame
route above with `HE_RML_RECORD_TIMINGS=1`; the control set only
`HE_RML_ASYNC_DECODE=0`. Both modes used the same engine, scene, settings,
and two 120-step calls. The first frame with more than 50 input render
objects was the scene-submission frame; the following 120 displayed frames
form each sample:

| Run | Scene submission CPU ms | Following 120 CPU P50/P95/P99/max ms | Frames >16.667 ms | GPU span P95 ms | Sampled local GPU peak MiB |
| --- | ---: | --- | ---: | ---: | ---: |
| Synchronous A | 302.475 | 6.758 / 27.519 / 48.708 / 58.388 | 14 | 2.730 | 602.80 |
| Asynchronous A | 238.792 | 6.772 / 29.015 / 45.923 / 47.397 | 15 | 2.941 | 621.56 |
| Synchronous B | 280.044 | 7.117 / 26.082 / 45.709 / 46.807 | 13 | 2.801 | 599.06 |
| Asynchronous B | 230.784 | 6.985 / 27.854 / 46.812 / 82.247 | 13 | 2.477 | 599.06 |

The two scene-submission samples are about 49–64 ms shorter with async
UI decode, consistent with the measured PNG decode time. The following
120-frame P95 does not improve and one async maximum is worse; other
loading and rendering costs remain. Async and serial 90-frame deterministic
DX12 MainMenu captures passed (`runtime-manual`) and their 1280×720 PNGs
were identical at every pixel. That verifies the eventual displayed menu
image, not the transient transparent placeholder or GPU mip texels.
Source and logs for these runs are pinned in
`out/agent/current-rml-async-20260927/`.

After the final UI decode source change, `build-only`
`.\hectl.bat game build -Configuration Debug` passed in 334.2 s and
`.\hectl.bat editor build -Configuration Debug` passed in 387.3 s.
Final-source incremental optimized `.\hectl.bat game build` and
`.\hectl.bat editor build` passed in 24.6 and 23.9 s. The optimized
`.\hectl.bat game check -Frames 120 -StartupScene
Assets/Scenes/MainMenu.he -GraphicsApi Vulkan -StrictGraphicsApi
-RenderValidation -Resolution 1280x720 -TimeoutSeconds 180` passed
(`runtime-manual`) on that final source. The same command with `DX12`
also passed (`runtime-manual`).
The final queue-permit adjustment was followed by incremental
`build-only` Game/Editor optimized and Debug builds, all PASS
(`game build`: 19.7 s, `editor build`: 25.5 s,
`game build -Configuration Debug`: 19.7 s,
`editor build -Configuration Debug`: 26.0 s). A 120-frame strict DX12
Game check with `HE_RML_RECORD_TIMINGS=1` passed, logging a queued
and ready 1672×941 image with no permit error. A second held-decode
owned DX12 shutdown passed and logged `releasedPending`; both had
empty stderr. The same final-source 120-frame strict Vulkan Game check
also passed. The outstanding quota now persists until publication or
release, so completed but unconsumed futures remain under backpressure.
Before these builds, the idle `tests-debug` and `tests-relwithdebinfo`
caches were checked for path containment, reparse points, `.he-keep`,
and running engine/build processes, then removed with the exact
`hectl clean cache -CachePath out/agent/tests-debug -Json` and analogous
`tests-relwithdebinfo` command. Both returned zero failures and no
remaining `.retired-*` directory, deleting 6,427,302,885 and
5,928,131,629 logical bytes. C: free space rose from 12.95 to 24.47 GiB
at the cleanup checkpoint. Protected evidence and optimized Game/Editor
caches were retained; Debug Game/Editor caches were built afterward.

## Owner-thread frame stage DAG (2026-09-27)

`FrameStageGraph` is a bounded, owner-thread DAG for the runtime stage
barriers. Level now builds `PrePhysics → Physics → PostPhysics → FixedGameplay`
for each fixed tick and `DisplaySystems → RenderPrepare` for each displayed
frame. It topologically validates all edges before invoking callbacks,
rejects cycles, wrong-thread use, foreign/stale handles, and reuse, and
propagates exceptions without entering successors. Each graph ends within
the update call; stage callbacks may join the separately audited CPU
`TaskScheduler` work. No Jolt, EnTT, script, UI, or RHI callback was moved to
a worker by this change. The stage handles use a process-unique graph ID,
not a stack address that can be reused on a later tick.

`unit-test` PASS: `.\hectl.bat test TaskScheduler -Configuration
RelWithDebInfo` and `.\hectl.bat test TaskScheduler -Configuration Debug`.
The test adds stage nodes out of topological order, joins an actual worker
`ParallelFor` from an owner-stage callback, rejects a cycle before any
callback, rejects a foreign handle and wrong-thread execution, and checks
exception propagation and single-use behavior. The owner-thread rejection
case was added after the final optimized test-cache cleanup and was run in
Debug; the same stage implementation and prior cases passed optimized.
`unit-test` PASS in both configurations for the exact
`FixedSimulationClock`, `PhysicsScriptContact`, `InputActions`, and
`PythonRuntime` hectl test commands. The PythonRuntime fixture logs expected
negative-path import and invalid-script-path diagnostics before its PASS.

`build-only` PASS on the final stage ID source:
`.\hectl.bat game build` (via the strict Vulkan check),
`.\hectl.bat editor build`, `.\hectl.bat game build -Configuration Debug`,
and `.\hectl.bat editor build -Configuration Debug`.
`runtime-manual` PASS: strict DX12 120-frame `game check` with
`-FixedDeltaSeconds 0.0333333`, strict DX12 480-frame check with
`-FixedDeltaSeconds 0.0083333`, and strict Vulkan 340-frame check with
`-FixedDeltaSeconds 0.0166667`, all using `MainGame.he`,
`HE_SURVIVOR_CAPTURE_SCENARIO=region_4_final`, 1280×720, and
`-RenderValidation`. The first two ran before the graph-ID refinement;
the final-source Vulkan check passed afterward. All three stderr logs were
empty. These runs confirm bounded application/renderer operation at the
three presentation deltas; exact tick/input sequence assertions are supplied
by the focused unit tests rather than inferred from frame-count exits.

A final-source owned strict DX12 Agent session stepped MainGame 120 ticks,
loaded MainMenu and stepped 15, then loaded MainGame and stepped 120.
The scene generations were 2 → 5 → 8 and entity counts 649 → 7 → 638;
the owned process stopped cleanly, with empty stderr and no Python exception
or NVRHI validation error (`runtime-manual` PASS). The session is
`out/agent/sessions/20260927-163115-355-8026369d7f06/`.
It logged two existing optional `ToonWater.efkpkg` prewarm failures, also
present in the earlier non-stage-graph DX12 run; they are not counted as a
successful prewarm or attributed to this stage change.

For an uninstrumented frame-time check, two strict DX12 340-frame runs used
the same `region_4_final` scene, 120 warm-up frames, fixed 1/60 s, 1280×720,
validation, and world-frame interval 120–339 as the prior no-stage-graph
sample. All three runs passed (`runtime-manual`).

| Run | CPU P50/P95/P99 (ms) | Update P50/P95 (ms) | Graph record P95 (ms) | >16.667 ms |
| --- | --- | --- | ---: | ---: |
| Before, single run | 13.697 / 18.844 / 24.727 | 3.509 / 8.002 | 8.871 | 32/218 |
| Stage DAG A | 12.355 / 16.958 / 21.678 | 3.049 / 7.659 | 7.800 | 13/218 |
| Stage DAG B | 12.443 / 17.559 / 20.564 | 3.105 / 7.640 | 7.962 | 15/218 |

No stage-graph frame-rate gain is claimed from a single older comparator and
two variable new runs. The new P95 values do not reliably meet 16.667 ms;
the measured DX12 path shows no gross regression. These CSVs predate the
graph-ID refinement; the final source passed the 340-frame Vulkan functional
check, but its frame-time distribution was not remeasured. These paired CSVs and
the scene-switch log are pinned under
`out/agent/current-frame-stage-graph-20260927/` with `.he-keep`.

### Directional shadow resource states and barrier experiment (2026-09-27)

The directional shadow pass uses the shared frame/object binding sets. Its
RenderGraph declaration now includes the light-grid constants, light records,
cluster masks, and previous bone matrices that those binding sets reference.
The default white texture was already among the imported bindless material
textures. RenderGraph commits declared resource states before the pass callback.
This closes a pass dependency gap without changing shader output or CPU/GPU
ownership.

An opt-in experiment temporarily disabled NVRHI automatic barriers inside the
directional shadow callback after those graph barriers. Two paired strict
Vulkan 340-frame checks used `region_4_final`, fixed 1/60 s, 1280×720,
validation, 120 warm-up frames, and identical world-frame range 120–339
(`runtime-manual`: all PASS). The switch was removed because the result did
not reproduce:

| Pair | Automatic CPU P50/P95/P99 (ms) | Manual CPU P50/P95/P99 (ms) | Automatic / manual graph-record P95 (ms) |
| --- | --- | --- | --- |
| A | 30.86 / 38.61 / 48.41 | 29.39 / 34.22 / 37.55 | 27.14 / 24.92 |
| B | 30.34 / 38.11 / 44.39 | 31.41 / 38.45 / 42.71 | 27.58 / 28.26 |

All four samples contained 218 rows and every CPU frame exceeded 16.667 ms.
The first/last sampled scene work counts matched within each pair (331/459
input render objects and 1212/1125 shadow draws). The recorded CSVs are
`out/agent/vulkan-shadow-barriers-{auto,manual}-{a,b}.csv`. The temporary
manual-barrier switch is absent from the final source. This experiment does
not establish a frame-time gain; the remaining Vulkan shadow-recording cost
needs a different optimization with output validation.

On the retained resource-declaration source, `build-only` PASS:
`\.\hectl.bat game build` (49.2 s), `\.\hectl.bat editor build` (44.0 s),
and their `-Configuration Debug` variants (41.2 and 51.0 s).
`runtime-manual` PASS: strict DX12 and Vulkan `game capture -Frames 322`
with `MainGame.he`, `region_4_final`, fixed 1/60 s, 1280×720, and
`-RenderValidation`; both exited 0 with empty stderr. The capture paths
are `out/agent/final-shadow-deps-{dx12,vulkan}-frame322.png`.
Downscaled previews were inspected sequentially: both show the expected
level-up UI and scene, with no obvious missing resources. The modal UI
obscures most shadow casters, so these screenshots are a device/lifecycle
smoke check rather than a pixel-level shadow comparison.
The four performance CSV sets and both final captures/previews are also
pinned under `out/agent/current-shadow-barrier-audit-20260927/` with
`.he-keep`.

The retained final source was also profiled without the experimental switch:

```powershell
$env:HE_SURVIVOR_CAPTURE_SCENARIO='region_4_final'
.\hectl.bat game check -Frames 340 -PerformanceWarmupFrames 120 -StartupScene Assets/Scenes/MainGame.he -GraphicsApi <DX12|Vulkan> -StrictGraphicsApi -RenderValidation -Resolution 1280x720 -FixedDeltaSeconds 0.0166667 -RenderStats -RenderStatsPath out/agent/<backend>-shadow-deps-final.csv -TimeoutSeconds 240
```

Both checks passed (`runtime-manual`) with empty stderr. For world frames
120–339 (218 rows; nearest-rank sample positions as used above):

| Backend | CPU P50/P95/P99/max (ms) | GPU span P50/P95/P99 (ms) | Graph record P95 (ms) | CPU frames >16.667 ms |
| --- | --- | --- | ---: | ---: |
| DX12 | 12.55 / 16.52 / 19.69 / 34.51 | 4.14 / 4.92 / 5.10 | 7.84 | 10/218 |
| Vulkan | 29.86 / 34.57 / 40.81 / 47.70 | 6.22 / 11.83 / 12.59 | 25.57 | 218/218 |

The CSV sets and process logs are pinned alongside the captures. DX12 P95 is
under the 16.667 ms target in this one sample, with ten long frames; Vulkan
remains far over budget on the CPU. The single-run values are measurements,
not a cross-backend speedup claim.

To make room for the required Debug test build, the idle
`out/agent/tests-relwithdebinfo` cache was verified within the workspace,
with its CMakeCache present, zero reparse points or `.he-keep` files, and no
engine/build process. `.\hectl.bat clean cache -CachePath
out/agent/tests-relwithdebinfo -WhatIf -Json` reported 5,943,356,254
logical bytes reclaimable; the corresponding non-WhatIf command reported the
same deleted logical bytes and zero failures. C: free space rose from 5.97
to 11.51 GiB, with no remaining `.retired-*` cache. The Debug tests were
then built; optimized Game/Editor, Debug Game/Editor, and retained evidence
were preserved.
