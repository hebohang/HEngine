# FSR 3.1, streaming and combat upgrade

## Active objective (2026-09-29)

Deliver genuine FSR 3.1 on both DX12 and Vulkan; replace standalone TAA while
enabled; integrate exposure, jitter, history reset, resizing, reactive masks,
deforming-object motion and native-resolution UI. Expose mode, quality and
sharpness with verified live switching and capability/error fallback. Also
deliver runtime streaming and combat AI/animation upgrades reusable by projects.
The original acceptance required both backends. On 2026-09-29 the user explicitly
authorized checking/trying the latest NVRHI upstream, then deferring remaining
Vulkan problems in favor of DX12 quality. This supersedes the earlier ThirdParty
permission block and the Vulkan allocation-failure completion gate. Both APIs'
ordinary FSR paths remain implemented; the Vulkan allocation-failure defect is
deferred, not repaired. Latest dependency validation is recorded in
[NvrhiUpstreamValidation.md](NvrhiUpstreamValidation.md).

## Acceptance ledger

| Requirement | Evidence required | Status |
|---|---|---|
| Official FSR 3.1 DX12 + Vulkan | Pinned dependency/license; actual dispatch on each backend, validation and captures | Adapter smoke and main-scene dispatch PASS on both APIs |
| Temporal integration | No standalone TAA while FSR active; correct depth/motion/jitter/exposure; cut/resize reset tests | TAA exclusion, jitter cancellation, cut/resize and exposure/light-step stress PASS on both; representative moving/deforming-object evidence is recorded in the next row, with its residual/visibility limits retained |
| Characters, hair, cloth | Motion exports and moving/turning captures; disocclusion/ghosting comparison | Submitted history plus repeatable fast translation/turning with actual hair/cloth PASS on both APIs; per-section reprojection and Quality/NativeAA disocclusion comparison captured. Sampled visual review PASS; correspondence visibility limits and residual tails retained, not a universal no-ghosting claim |
| Transparency/particles | Reactive/composition masks captured with representative content, correct pass ordering | Low-contrast mesh/sprite plus CPU blend/add/subtract/multiply/distortion inputs and lifecycle PASS on both APIs; development GPU add/zero-RGB coverage PASS, without certifying GPU production readiness or fast-motion quality |
| Native UI | 1440p/2160p scene reconstruction with UI rendered at output resolution | PASS: frontend and real courtyard HUD native/FSR comparison on both APIs. Current Game successfully transitions to native 4K on both; opaque HUD interiors and layout match at1440p/2160p, with verified paused simulation, native raster dimensions and clean shutdown. Earlier allocation-failure leak remains a separate unresolved fallback defect |
| Settings and fallback | Persisted Off/FSR mode, quality, sharpness; live resize/toggle and injected failure tests on both APIs | Real menu modes/five qualities/sharpness clamps/cancel/resize and dispatch failure/recovery PASS on both; missing runtime and jitter-failure previously PASS; new-process load of saved mode/quality/sharpness and UI PASS on both; explicit runtime status and unsupported GI view/menu restoration PASS on both |
| Runtime streaming | Budgeted demand/residency, asynchronous IO/upload, safe retirement and diagnostics; traversal/pressure tests | Mip residency, partial DDS IO, persisted budget and descriptor retirement integrated; dual-backend pressure/recovery, >64 MiB upload/recovery, IO failure/retry and cancellation PASS. Query-before-submit tracking and 16 synthetic boundary failures/retries per API PASS. Continuous 12.7k/13.0k-frame DX12/Vulkan traversal and WDDM memory response verified; additional CPU/GPU-profiled13.1k-frame DX12 run passed, with no >50ms measured-phase frames. Prior pressure hitches were not reproduced and remain unattributed, not claimed fixed. Actual driver allocation-failure cleanup/recovery still requires the unapplied ThirdParty patch |
| Combat AI | Reusable perception/state/navigation, validated combat behaviours in MechanicGirlLab | Managed perception/memory/interruptible attacks integrated; real attacks plus 3 waves/9 kills/victory, defeat cleanup and retry PASS on both APIs; regional Recast build/shared follower and low-obstacle route PASS on both. Representative requested behaviour verified; no general crowd/dynamic-nav claim |
| Animation upgrade | Gameplay-synchronised events/transitions/aiming, cancellation/reset behaviour and visual validation | Shared reload/equip clock, cancellation/single commit and lifecycle tests PASS on both APIs. Independent state-machine outgoing-layer loss fixed with regression. Actual Game ADS/strafe/forward/sprint/stop, moving reload, switch cancellation, completed reload and holster/draw: 28 input-driven pose samples/API PASS, with identical state/parameter/palette/position evidence and sampled visual review. No universal all-pose/foot-contact/ghosting claim |
| Project portability | Engine modules contain no MechanicGirlLab paths; isolated second-project integration test | PASS: independent Scripted3D project, separate full build/cache/managed script, DX12 + Vulkan actual FSR 3.1.4 dispatch and 11 samples/API; five qualities, TAA restoration, 1440p/2160p resize, texture budget demotion/recovery, regional navigation and shared combat/action markers. No MechanicGirlLab content used |

## Initial evidence

- Existing working tree contains the earlier offline lighting implementation;
  preserve it. MechanicGirlLab is external: `C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
- Project cache `out/agent/projects/7e44ffe2e91e72bd`; no project switch needed.
- Current renderer has TAA, jitter, depth/motion and separate internal/output
  dimensions. Tone-map shader uses spatial Catmull-Rom when enlarging the scene;
  no FSR implementation exists yet.
- Native graphics handles/API calls belong only under Runtime/Platform.
- No previous goal implementation turn exists; first turn is discovery and setup.

## Work order

1. Pin SDK and implement shared upscaler contract plus DX12/Vulkan adapters.
2. Wire render graph, temporal/mask inputs, settings and diagnostics; verify both APIs.
3. Audit deformation and transparency; verify 2K/4K UI, resize and fallback.
4. Implement and validate streaming, then combat AI/animation improvements.
5. Run requirement-by-requirement completion audit with exact commands and evidence.

## Implementation checkpoint — 2026-09-29 13:39

The previous conversational answer only explained API support. This iteration
made implementation progress; the full goal remains active. None of the pending
rendering, streaming, AI or animation requirements is waived.

- `CMake/FidelityFX.cmake` acquires the official SDK 1.1.4 (FSR 3.1.4), extracts
  only public headers, the two signed x64 DLLs and license under `out/agent`.
  SHA256: `0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952`.
  Both DLL Authenticode signatures were Valid, signer Advanced Micro Devices.
  Game, Editor and tests deploy runtimes/licenses; `HE_FSR31=OFF` builds the
  unavailable-provider fallback. No vendored ThirdParty source was changed.
- `Graphics/RHI/TemporalUpscaler.h` exposes only NVRHI/data types. All FFX/native
  API code lives in `Platform/Interop/Fsr31Interop.cpp`. It loads the appropriate
  executable-relative runtime, creates/reports the 3.1 provider, queries jitter,
  transitions input/output resources, dispatches FSR and clears native binding
  state. Context destruction requires GPU completion by the caller.
- Fixed the SDK's Vulkan request for `vkGetBufferMemoryRequirements2KHR` on a
  core Vulkan 1.3 device by resolving its promoted core spelling. Both backend
  headers define duplicate enumerators; isolated header namespaces preserve the
  unmodified official headers in a dual-backend translation unit.
- `Upscaling.h` and the existing RenderSettings registry/default JSON now expose
  Off/FSR3.1, NativeAA/Quality/Balanced/Performance/UltraPerformance and sharpness.
  Defaults remain Off. These settings are NOT YET consumed by SceneRenderer.

### Verified evidence

- **build-only PASS + runtime-manual PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineFsrSmoke -Configuration RelWithDebInfo -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -Run -Wait -TimeoutSeconds 180`
  Final run logs: `out/agent/projects/7e44ffe2e91e72bd/logs/run-game-relwithdebinfo-20260929-133706-388-107308.{stdout,stderr}.log`.
  Provider 3.1.4 reported for both APIs. Each ran 8 frames at 1080p, 1440p,
  2160p output (Performance scale), including reset, sharpening, masks, jitter,
  context recreation, real 640x360 -> 800x450 window/swap-chain resize and invalid
  input rejection. Six PFM readbacks under the matching session's
  `Saved/Diagnostics/FsrSmoke` passed all-pixel finite/color checks with max error
  <0.00364 versus constant linear RGB (0.25, 0.5, 1). Final stderr empty; no Vulkan
  validation errors. DX12 native debug layer is unavailable on this machine;
  NVRHI validation was enabled. This constant-scene smoke does NOT prove moving
  geometry, UI resolution, reactive-mask authoring or gameplay quality.
- **unit-test PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name RenderSettings -Configuration RelWithDebInfo`
  Checks quality dimensions/native/minimized cases, defaults, settings round-trip
  and mode/quality versus sharpness invalidation plus existing registry tests.
  Initial test invocation used the C++ function name `RenderSettingsContract`
  instead of registered name `RenderSettings` (UNKNOWN); corrected invocation
  exposed a missing default JSON entry, which was fixed before this PASS.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.
- **build-only PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`
  Final HGame build passed (88.0s); both runtime DLLs and `Licenses/FidelityFX.txt`
  verified beside the project executable. No Game scene FSR runtime verification
  or Editor build has been performed for this integration checkpoint.

### Next implementation boundary

1. Thread Upscaling through runtime/scene descriptors and configuration switching.
   Both `SceneRenderer::RecordFrame` and candidate preparation in
   `SceneRendererSwitch.inl` resolve render extents. Candidate effects are swapped
   in `SceneGraphEffectBundle`; preserve rollback and fence retirement of FSR
   contexts rather than destroying contexts referenced by submitted commands.
2. Introduce a graph FSR pass with an R32_FLOAT device-depth snapshot and masks.
   The existing `RasterSurfaceData.hlsli` motion XY is previousUV-currentUV,
   suitable for FSR with render-width/height scaling. Audit deformation separately.
3. FSR must jitter opaque + transparent/VFX, skip standalone TAA/post-TAA depth,
   reconstruct after those draws, then keep display UI native. Existing
   `ScenePostProcessPass` applies exposure/bloom; ensure FSR input is scene-linear
   before that processing and preExposure matches its actual convention.
4. Add diagnostics, missing-DLL/injected dispatch fallback, live mode/quality/
   resize tests, then full game captures before claiming scene integration.
5. Streaming and combat AI/animation are still pending in their entirety.

## Implementation checkpoint — 2026-09-29 14:15

This iteration made source and runtime-validation progress. The previous answer
only explained API support and was not implementation progress. The full goal is
still active; streaming, combat and animation upgrades have not been implemented.

### Main renderer integration now present

- `SceneUpscalingPass` owns fenced FSR contexts and graph inputs/output. Runtime
  and scene descriptors carry settings, including transactionally prepared graph
  candidates; retired candidate effects retain contexts until GPU completion.
- FSR supplies temporal AA while active. Transparent/overlay/VFX rendering runs
  before reconstruction; reconstructed HDR then goes through post processing and
  tone mapping, followed by existing native-output UI composition. Shader inputs
  include copied R32 device depth and R8 reactive/composition masks. The masks
  currently measure opaque-versus-composited scene-linear color differences.
  This does not yet prove adequate coverage of low-contrast transparency or
  representative moving particles. No material-authored coverage mask exists yet.
- HDR input is unexposed, `preExposure=1`, with the provider's auto exposure;
  artistic scene exposure remains after reconstruction. Exposure-change visual
  stress remains to be run. FSR's jitter uses the official sample's negated query
  offset for both projection and dispatch.
- **Fixed during this iteration:** native motion input contains camera jitter.
  Added `TemporalUpscalerDesc::bJitteredMotionVectors`, enabled it for SceneRenderer
  and mapped it to the FFX motion-vector jitter-cancellation flag. The standalone
  smoke retains the default unjittered contract. Static paused-scene raw motion
  agrees with (previous-current) pixel jitter / render dimensions; after removing
  that component, p99 residual is 0.000255 px DX12 and 0.000222 px Vulkan over
  ~664k valid pixels. This is not verification of animated cloth/hair motion.
- Corrected HDR feature metadata: depth remains jittered under FSR although its
  reconstructed color is no longer jittered. Added reactive/composition capture
  source discovery. R8 raw readback was unsupported and crashed the first mask
  capture; implemented byte-exact one-channel export and a padded-row unit test.
- Native dispatch failure has a same-frame spatial fallback and next-frame native
  AA path, but that branch has not been fault-injected. `GetJitter` failure still
  throws; audit this error path before claiming complete graceful fallback.

### Verified evidence (current source)

- **build-only PASS + runtime-manual PASS**, each command includes build:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrScene.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -StartupScene Assets/Scenes/UECloseupAnimated.he -GraphicsApi DX12 -OutputDirectory C:/WorkSpace/HEngineDev/out/agent/captures/fsr-dx12-contract-2`
  and
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrScene.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -StartupScene Assets/Scenes/UECloseupAnimated.he -GraphicsApi Vulkan -OutputDirectory C:/WorkSpace/HEngineDev/out/agent/captures/fsr-vulkan-contract-1`.
  Both verify actual capture metadata, requested/effective settings and executing
  graph passes, not merely successful setting requests. Five quality dimensions
  at 2560x1440: 1706x960, 1505x847, 1280x720, 853x480, 2560x1440 (NativeAA).
  Off restores SceneTAA; FSR enables FSR31Upscale and excludes SceneTAA. Sharpness
  0/1 changes without target recreation/history invalidation. Real window resize
  1440p -> 4K -> 1080p -> 1440p succeeds. An eight-frame atomic camera-cut sequence
  verifies exactly one FSR reset and submitted history sequence 0..7.
  `.world.json`, graph pages, PNGs, masks, receipts and pixel-analysis reports are
  retained in the evidence directories. 4K image previews inspected sequentially;
  no black/corrupt output observed, but these are not animated quality acceptance.
  R8 masks are nonzero in this character scene (~300 pixels), which does not cover
  large transparent surfaces or particle effects.
  Owned session logs below `out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/sessions/`:
  `20260929-141023-209-f22c18c2aa1a` (DX12),
  `20260929-141119-375-ae39204cb87b` (Vulkan).
  Both stderr files empty; stdout search found no validation/device/FSR errors.
  NVRHI validation enabled; DX12 native debug layer unavailable on this machine.
- **runtime-manual PASS**, unavailable-runtime fallback on both APIs:
  `powershell -ExecutionPolicy Bypass -File out/agent/fsr-integration/Verify-MissingProvider.ps1`.
  Temporarily renamed only the appropriate deployed backend DLL while the target
  executable was stopped, then ran its pinned binary and restored the DLL in
  finally. Both selected TAA and reported inactive/failed FSR with a missing-runtime
  reason, while still producing a display capture. Restored DLL SHA256 checked.
  Binary SHA256 `6F2DB098E4A4A19A8A90CA617C45D29828C17BA2EBBDECD1B454A7B0ED3321AE`.
  Receipts: `out/agent/fsr-integration/missing-provider-{DX12,Vulkan}.json`.
  All owned sessions stopped. This does not cover device loss or failed dispatch.
- **unit-test PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name TextureReadbackPng -Configuration RelWithDebInfo`.
  Includes R8 single-channel exact bytes, odd width/padded rows and invalid pitch,
  plus existing readback contracts. Expected intentional-destructor-error log is
  part of the existing test; final registered test and runner both report PASS.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.

### Validation failures retained as findings, not successes

The earlier temporary `Verify-Live.ps1` captures do NOT prove live switching:
CommandLine settings overrode Session changes, so every quality stayed Performance.
The new checked-in `Test-FsrScene.ps1` avoids that override and asserts effective
values and actual renderer dimensions. Its first run also caught a test-side
float equality issue (0.2 versus float32 0.20000000298); fixed with tolerance.
The older Vulkan capture handle 77462 was polled to terminal PASS, but is not used
as proof of the latest jitter fix. R8 capture failure was fixed before final runs.

### Remaining work, unchanged full scope

1. Motion/deformation audit and moving evidence for body, hair and cloth, plus
   camera rotation/disocclusion; inspect sky motion rather than assuming zero is
   adequate. Compare image sequences, not one still. Correct previous-deformation
   state if CPU simulation cadence differs from successful raster submissions.
2. Representative alpha/particle/distortion mask coverage and temporal quality;
   add authored coverage where color-difference masks cannot capture it.
3. Native HUD/menu/text evidence at 1440p/4K; settings UI and persistence workflow;
   exposure stress, projection changes, minimize/restore, dispatch/jitter failure
   injection, unsupported-device handling. Actual resize already has coverage.
4. Runtime streaming, combat AI and animation upgrades remain entirely pending.
   Preserve cross-project abstractions and validate a second project. Editor
   integration has not been built/run for these FSR changes yet.
5. Do not mark the goal complete based on the rendering contract subset.

C: free space last observed ~1.2 GiB; captures use large uncompressed PNGs. No
cleanup was performed in this iteration. Follow the explicit storage policy if
space becomes insufficient; preserve source, current evidence and recovery files.

## Implementation checkpoint — 2026-09-29 14:34

This goal turn made implementation progress. Full rendering/streaming/combat scope
remains active; no requirement has been removed or declared complete by narrowing.

### Submitted deformation history

The previous vertex stream used `SubMesh::PreviousPosition` from the preceding CPU
simulation tick. Unlike the already submitted object/bone matrices, this was not
necessarily the preceding successful raster submission. Added a per-view mesh
cache `SubmittedDeformationHistory`: upload vertices use the last submitted local
positions, history advances only after submission, and aborted uploads force a
retry without advancing history. Multiple simulation ticks can now precede one
render without replacing the required prior position. Repeated rendering of an
unchanged pose uploads zero motion once, then reuses the settled vertex buffer.
Shared CPU mesh vertices are not mutated by the renderer. Static/skinned morph
and cloth uploads use the same mechanism. CPU deformation resets publish a
separate generation so a lagging view cannot retain old simulated cloth history.

The existing custom material shader applies previous bone matrices, previous
world transform and previous material vertex-offset time to PreviousPosition;
the new history fixes the local-position input to that path. This is a rendering
correctness improvement, not completion of the gameplay animation upgrade.

### Current-source verification

- **unit-test PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name RenderWorldSubmission -Configuration RelWithDebInfo`.
  Registered test includes skipped simulation updates, failed/aborted submission,
  repeated-pose settling, no redundant settled upload, independent views, camera
  cut, reset generation, changed vertex count and preserving shared CPU data.
  Test build PASS (145s), final registered test PASS.
- **build-only PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab` (130.8s).
- **runtime-manual PASS**, each backend:
  `powershell -ExecutionPolicy Bypass -File out/agent/fsr-integration/Capture-Deformation.ps1 -GraphicsApi DX12`
  and the same command with `-GraphicsApi Vulkan`.
  Runs use the freshly built pinned executable SHA256
  `AFE3C76160B5AA09D9D345A6A30C8DA69CBEBA6311F657C2E9478932E0421A46`.
  Scene `UECloseupAnimated.he`, FSR Quality, output 1280x720. Warm up 120 fixed
  simulation steps; reframe the existing camera to (0,1,8.6) without saving;
  capture eight display frames, eight raw-motion frames and a paused-motion frame.
  Each sequence advances one simulation step per captured frame. Earlier runs at
  camera (0,0.3,8) clipped the head and were superseded for visual review.
  Authoritative final directories:
  `out/agent/captures/fsr-deformation-DX12-20260929-142656`
  `out/agent/captures/fsr-deformation-Vulkan-20260929-142728`.
  Their receipts point to preserved session images/AOVs. Analysis and sequentially
  inspected 0/3/7 contact sheets are beside the receipts. Actual character palette
  changed in 112 matrices; the 8010-particle cloth solver position hash changed;
  solver diagnostics reported finite state and zero pin drift. Those CPU probes
  are supporting activity evidence, not direct GPU motion-vector truth.
  Raw GPU motion is finite on both APIs. After removing camera jitter, head/hair,
  torso and cloth screen regions all contain motion through the eight frames.
  Paused maximum residual: DX12 0.000273 px, Vulkan 0.000528 px. Mixed screen ROIs
  do not isolate hair/cloth material identity, and there is no reference-image
  ghosting metric. Fast movement, disocclusion and camera rotation remain pending.
  Analysis command:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/fsr-integration/Analyze-Deformation.py`.
  Both final session stderr logs empty and no stdout validation/device/FSR error
  matches. Sessions stopped in finally. Native DX12 debug layer remains unavailable.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.

No reflected persistent fields or ThirdParty source were changed in this turn.
Streaming and combat AI/gameplay animation upgrades remain pending. Next rendering
work includes sky/camera motion, transparent/particle authored mask coverage,
HUD/settings UI evidence, exposure stress and dispatch/jitter failure fallback.

### Storage maintenance — 2026-09-29 14:41

C: free space fell to 944,615,424 bytes (0.88 GiB), so the repository's explicit
maximal-cleanup workflow was applied. Inventory recorded logical sizes, compression,
reparse points and .he-keep files. No Game/Editor/compiler was running. Current
Game and test caches remain necessary for this active goal; protected sessions,
current rendering evidence, published content, recovery/source backups and unknown
legacy output were retained. No source/project persistent asset was deleted.

- `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 clean stale -Json`
  was **interrupted**, not reported as PASS. The owned Python process was verified
  by PID, exact command and creation time before stopping it. Source inspection
  showed a complete `plan()` recomputation for every deletion across ~4,000 run
  records; after several minutes it had only reclaimed a small amount. No user
  process was stopped. The storage mutex was released before subsequent cleanup.
- Preview then **PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 clean cache -CachePath C:/WorkSpace/HEngineDev/out/agent/projects/7e44ffe2e91e72bd/editor-relwithdebinfo -WhatIf -Json`
  followed by the same command without `-WhatIf`. Reclaimed 7,279,831,767 logical
  bytes; no failures. The idle Editor cache will rebuild when Editor validation
  is required; it is not current FSR validation evidence.
- **PASS**:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/native/powershell/pwsh.exe -NoProfile -File out/agent/fsr-integration/Clean-Rebuildable.ps1`.
  Removed verified idle Android native/dotnet and Web intermediates, dependency
  parser/shader test build caches and three duplicate SDK staging directories.
  Exact paths, logical bytes and results are in
  `out/agent/fsr-integration/cleanup-rebuildable.json`. The script resolves each
  absolute path beneath out, checks ancestors/children for reparse points and
  protection/recovery markers, checks processes and holds applicable build/install
  locks before native PowerShell deletion. All nine directories were removed.
- Rechecked cache inventory and retired directories: no retired-cache leftovers.
  Large retained caches are the current Game (~29.8 GB including protected/active
  goal history), current tests (~6.3 GB), and an unclassified old external-project
  directory (~467 MB, no CMakeCache; not assumed disposable). Other retained
  archives/evidence/source backups are documented by the inventory rather than
  inferred disposable from names alone. Package roots had no large cache to remove.
- Final C: free space 12,228,362,240 bytes (11.39 GiB), actual observed increase
  **11,283,746,816 bytes (10.51 GiB)** from the pre-cleanup observation. Ordinary
  automatic cleanup remains disabled (`autoEnabled: false`). Inventory and cleanup
  reports remain under `out/agent/fsr-integration/`. All owned sessions/processes
  from this iteration are terminal; no build, run or cleanup handle remains live.

### Sky motion and recoverable FSR failure — 2026-09-29 15:02

Implemented a separate RG16 FSR motion input. Opaque/deformed geometry keeps its
existing XY vectors; background pixels at conventional clear depth 1 receive
infinite-environment reprojection from camera rotation and raster jitter. Camera
translation is removed. `SubmittedSkyMotionHistory` advances only on successful
submission; resets use identity reprojection, and a material reset after staging
preserves the pending camera for the next submitted frame. The original four-channel
`motion` capture retains its previous-depth contract. New `upscale-motion` captures
the actual two-channel FSR input.

A failed jitter query now supplies zero raster jitter and routes the already-sized
frame through spatial fallback instead of throwing. The next frame retires FSR and
returns to native AA. Added one-shot, Dev Agent-only
`render.validation.upscalingFailure` (`jitter` / `dispatch`, optional atomic capture)
and a fallback-command count in diagnostics. This proves the recovery control flow;
it does not simulate device loss or an SDK failure after partially recording native
commands. The injection API is excluded when HE_DEV_AGENT_RUNTIME is disabled.

Validation on final Game SHA256
`C0D6BB78595533B320582B99C3A849FC717863CB058C91AE3053D576F32D2F03`:

- **build-only PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`
  (final build 80.7s). Both shader backends compiled. Earlier intermediate builds
  passed too, but final runtime evidence below uses this final executable.
- **unit-test PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name RenderWorldSubmission -Configuration RelWithDebInfo`
  (final test build 92.8s). Added independent world-direction projection checks for
  rotation, translation invariance, jitter, unsubmitted staging, committed history,
  first use and reset. No window is created by this test.
- **runtime-manual PASS**, both commands:
  `powershell -ExecutionPolicy Bypass -File out/agent/fsr-integration/Capture-SkyMotion.ps1 -GraphicsApi DX12`
  and the same command with `-GraphicsApi Vulkan`.
  Final evidence directories:
  `out/agent/captures/fsr-sky-DX12-20260929-145931` and
  `out/agent/captures/fsr-sky-Vulkan-20260929-150019`.
  Each includes static jitter, translation, yaw, pitch, reverse rotation and eight
  consecutive camera-cut frames. Atomic camera mutation/capture selects the actual
  moving frame. Readbacks are finite. Depth masks exclude geometric boundaries;
  independent projection from recorded camera matrices matches background vectors
  within one RG16 representable step per component plus 0.003 pixel float32 margin.
  The three rotations have median motion ~117, 60 and 231 pixels/frame. Largest
  Euclidean storage error is 0.2161 pixel in the fastest case. An initial flat
  0.2-pixel analysis bound failed there; the final bound follows the documented
  half-float representation precision instead of an arbitrary velocity-independent
  threshold. No engine adjustment was made to hide that initial analysis result.
  Final analysis command (**static-check PASS**, GPU evidence analysis):
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/fsr-integration/Analyze-SkyMotion.py out/agent/captures/fsr-sky-DX12-20260929-145931 out/agent/captures/fsr-sky-Vulkan-20260929-150019`.
  Earlier captures ending 145327/145348 are superseded for final-source evidence.
- **runtime-manual PASS**, both commands:
  `powershell -ExecutionPolicy Bypass -File out/agent/fsr-integration/Test-UpscalingFailure.ps1 -GraphicsApi DX12`
  and the same command with `-GraphicsApi Vulkan`.
  Evidence: `out/agent/captures/fsr-failure-DX12-20260929-145845` and
  `out/agent/captures/fsr-failure-Vulkan-20260929-145901`.
  For each injected phase: actual failed-frame RGBA16 HDR readback is 1280x720,
  finite and nonblack; exactly one spatial fallback is recorded; subsequent capture
  shows FSR failure latched, original reason and native 1280x720 TAA. Explicit
  Off -> FSR restores normal reconstruction. This uses Performance input 640x360.
  **static-check PASS** pixel analysis command:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/fsr-integration/Analyze-UpscalingFailure.py out/agent/captures/fsr-failure-DX12-20260929-145845 out/agent/captures/fsr-failure-Vulkan-20260929-145901`.
  Sequentially inspected both compact contact sheets (fallback HDR uses a simple
  inspection tone curve; adjacent native/recovered images use engine tone mapping).
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.

All four final owned runtime sessions stopped in finally. stderr files are empty.
Expected injection error messages are present; no validation-error/device-loss
messages were found. Vulkan reports pre-existing unconsumed-vertex-attribute
performance warnings. Native DX12 debug layer is unavailable; NVRHI validation is
enabled. No Editor, shipping build, other GPU or second project run was performed.

This is partial completion of the full goal. Remaining work includes actual
transparent/particle mask coverage (especially low-contrast transparency), exposure
stress, HUD/menu/text and settings UI evidence, fast character/hair/cloth motion and
disocclusion quality, second-project reuse, runtime streaming and combat AI/gameplay
animation upgrades. Streaming and combat remain pending; the rendering validations
above do not count as completion of those systems.

### Transparent coverage and legacy BLEND repair — 2026-09-29 15:20

FSR now preserves RGB and clears scene alpha immediately before transparent/world
sprite drawing. Existing alpha blending accumulates mesh/sprite coverage; ordinary
Effekseer blend/add rendering preserves maximum particle alpha. FSR inputs combine
that coverage with the existing scene-linear color-difference response. Reactive
uses max(color response, 0.9 * coverage); Composition retains full coverage.
The additional compute pass is only present when FSR is active. No ThirdParty code
was changed. `RenderFeatureBuildContext::bTransparencyCoverageInAlpha` documents the
SceneOverlay extension contract; full-screen extensions must preserve that alpha.
Subtract/multiply VFX and distortion still use the color-difference path when their
blending preserves destination alpha; this is not a separate coverage output for
every possible effect/material. Added `upscale-color` / `upscale-opaque` raw AOVs.
Public behavior and extension guidance are in RenderResolution.md and
RenderingPipelines.md, including the distinction from the legacy height cap.

Runtime evidence exposed a real V1 material bug: default `g_AlphaClip=0` replaced
authored `alphaMode=BLEND` with OPAQUE during property synchronization. Fixed this
without changing reflected fields/schema: disabling clipping now preserves BLEND;
explicit enabled clipping still selects MASK. Added tests for legacy BLEND, MASK,
OPAQUE, render snapshot selection, repeated property synchronization and V2 save.

Independent fixture assets are under the external project's
`Assets/Diagnostics/FsrCoverage/` (Coverage.he, two materials and fixture.json),
generated by `out/agent/fsr-integration/Create-CoverageFixture.py`. Existing scene
assets and startup configuration were not edited. The box resolves from Engine
Content, not the external project's missing Box file. Final evidence includes
hashes of fixture, resolved geometry, particle package and executable in inputs.json.
An initial Saved-relative fixture startup was rejected by the normal Assets-only
startup policy; the final fixture uses the supported path. The unused initial Saved
fixture is not runtime evidence. Initial captures before the material fix caught
the missing mesh coverage and are superseded by the final directories below.

Final executable SHA256:
`BC600B66376D12087FEEC96422F83EE29B3B29735456BB422893574B94E7995C`.

- **build-only PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`
  (final source build 36.1s; both backend shaders compiled). Subsequent contract
  drivers performed incremental builds, preserving the same executable hash.
- **unit-test PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name MaterialSerialization -Configuration RelWithDebInfo`
  (test build 108.6s). No reflected declarations/persisted fields changed, so no
  separate code-generation change is claimed.
- **runtime-manual PASS**, both commands:
  `powershell -ExecutionPolicy Bypass -File out/agent/fsr-integration/Capture-Coverage.ps1 -GraphicsApi DX12`
  and the same command with `-GraphicsApi Vulkan`.
  Final directories: `out/agent/captures/fsr-coverage-DX12-20260929-151446` and
  `out/agent/captures/fsr-coverage-Vulkan-20260929-151507`.
  At 853x480 internal / 1280x720 output, 135052 / 135216 interior pixels respectively
  demonstrate same-color transparent coverage despite RGB difference below 0.002.
  Individual opacity is 0.5; overlapping mesh/sprite coverage is 0.75. The initial
  analysis assumed all pixels had a single layer and rejected that legitimate
  overlap; final analysis tests both coverage values and excludes jittered edges.
  Moving the mesh moves its mask; removing mesh/sprite yields exactly zero coverage,
  reactive and composition across the opaque-only view.
  Existing `garden-gate-spark.efkpkg`, seed 1806, 12 fixed steps, then pause, produces
  actual mask pixels on both APIs (25 CPU instances, one recorded draw). Applying
  zero RGB but keeping root alpha one leaves 3129 / 3132 covered particle pixels
  with RGB difference below 0.002, proving coverage does not rely on color contrast.
  Explicit stop plus three steps restores all masks to zero. This validates this
  CPU-simulated particle package, not GPU-particle/template/lifecycle matrices.
  Both compact contact sheets were inspected sequentially.
- **static-check PASS**, analysis of those GPU readbacks:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/fsr-integration/Analyze-Coverage.py out/agent/captures/fsr-coverage-DX12-20260929-151446 out/agent/captures/fsr-coverage-Vulkan-20260929-151507`.
- **runtime-manual PASS**, current game-scene switching/size regression:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrScene.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -StartupScene Assets/Scenes/UECloseupAnimated.he -GraphicsApi DX12 -OutputDirectory C:/WorkSpace/HEngineDev/out/agent/captures/fsr-dx12-coverage-contract`
  and the corresponding Vulkan command with `-GraphicsApi Vulkan -OutputDirectory C:/WorkSpace/HEngineDev/out/agent/captures/fsr-vulkan-coverage-contract`.
  Added explicit graph assertions that FSRBeginTransparency/FSRPrepareInputs only
  execute in FSR mode. All quality levels, Off/On, sharpness 0/1, 4K/1080p/1440p
  resize and camera-cut history sequence passed. This is not new image-quality
  acceptance for hair or HUD.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.

All owned build/test/run handles are terminal, and final four Game PIDs were checked
absent after finally-stop. Final runtime stderr files are empty; no validation error,
device-loss or FSR-failure matches. Native DX12 debug remains unavailable. C: free
space was 10.59 GiB before the final small captures; no cleanup was needed.

The full goal remains active. Transparent coverage for tested mesh/sprite/particle
paths is now implemented and verified; broad special-blend/GPU-particle coverage,
temporal disocclusion/ghosting quality, exposure stress, native HUD/menu/settings UI,
second-project reuse, runtime streaming and combat AI/gameplay animation upgrades
remain open. Rendering masks do not satisfy streaming or combat requirements.

### Frontend controls and native menu rendering — 2026-09-29 15:40

Completed and verified the in-progress managed/public UI integration. The engine
exposes actual upscaler activation/failure via scene stats and `settings.display`,
typed managed setting IDs and `ReadDisplay`, plus non-focusing Rml scroll-into-view.
The external project's LabFrontend.cs/rml/rcss now provide localized mode, quality,
sharpness, actual render/output dimensions and provider fallback feedback. The
settings list scrolls for keyboard navigation; the legacy height control hides
while FSR is requested. Engine changes contain no project-specific paths.

The first visual run caught collapsed content widths: an unstyled Rml vertical
scrollbar consumed the entire content width. Explicit scrollbar sizing/track/thumb
styling fixes the real cause. Intermediate absolute width experiments were removed.
An initial test also incorrectly moved four entries from Quality and wrapped to
NativeAA; the final driver computes the displacement from the current enum value.
Only the final full runs below are acceptance evidence.

Executable SHA256: `80865B16B1C5F0019A1CBD11EBB84410F47858A1EDE35184B2344A65A5649FB7`.

- **build-only PASS**: `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`
  (88.6s, existing build session 14756 polled to exit 0, not restarted). Managed
  frontend changes compiled and loaded during the real Game runs.
- **static-check PASS**: `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/Validate-Frontend.py`
  (59 unique UI IDs, new control IDs, five outfits/two compatible meshes).
- **runtime-manual PASS**: `powershell -ExecutionPolicy Bypass -File out/agent/fsr-integration/Test-FrontendFsr.ps1 -GraphicsApi DX12`
  and the same command with `-GraphicsApi Vulkan`. These are bounded owned
  frontend sessions driven by actual virtual mouse/key events, not render-setting
  injection for the menu options. Final directories are
  `out/agent/captures/fsr-frontend-DX12-20260929-153632` and
  `out/agent/captures/fsr-frontend-Vulkan-20260929-153755`.
  Both pass Off/On, all five qualities, cancelled selection, sharpness increment
  and repeated upper/lower clamps, one injected dispatch failure with a visible
  fallback message, recovery through the menu, 2560x1440/3840x2160/960x540 resize,
  and keyboard scrolling to the final setting. Requested values and actual
  captured raster metadata were preserved separately.
- **static-check PASS**: `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/fsr-integration/Analyze-FrontendFsr.py out/agent/captures/fsr-frontend-DX12-20260929-153632 out/agent/captures/fsr-frontend-Vulkan-20260929-153755`.
  All five internal dimensions agree with their output ratios; 4K UltraPerformance
  uses 1280x720. Both report actual provider 3.1.4 with standalone TAA disabled.
  The same opaque corner text rectangle contains 2880 pixels: native vs
  UltraPerformance differs in exactly zero RGB channels on either API. This proves
  that region's native UI invariance, not an exhaustive HUD/UI visual comparison.
  4K, fallback and small-window previews were inspected sequentially.
  Persisted session UserEngine.json contains UltraPerformance and sharpness zero;
  Off is omitted as the default. Restart persistence was not exercised here.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.

Session.ps1 uses a fresh per-run `Saved` directory, so the game did not write the
user's external Saved config. The driver's additional backup/restore of those two
external preference files was unnecessary; future drivers can omit that step.
Both owned processes (81232, 60928) were stopped in finally and checked absent.
Both stderr logs are empty; no validation errors/device loss/managed compiler
errors. Vulkan reports 45 unused shader-output/vertex-attribute performance
warnings; these are not presented as a warning-free run. DX12 native debug support
remains unavailable. C: free space is 8.71 GiB; no cleanup performed.

No new Editor, unit-test, Shipping or second-project verification was performed.
Native gameplay HUD, restart persistence, unsupported-view UI messaging, exposure
stress and fast-motion temporal image quality remain to check. Special-blend/GPU
particles, runtime streaming and combat AI/animation upgrades remain in scope and
unfinished. The full goal stays active; this is a verified frontend milestone.

### Exposure, real HUD and preparation failure fixes — 2026-09-29 16:04

This turn made source and runtime evidence progress. No active process remains.
Final Game SHA256: `57615D64790A3DF2F47126BBFE792FC578674CB8B1875F5A92E714973487E0E6`.

Two real defects found and fixed outside ThirdParty:

1. Configuration preparation called `AbortFrame` after successfully realizing
   each candidate bank. This cleared its usage cursors; subsequent `Collect`
   could trim already proven resources. A transaction could commit, then fail
   allocating an earlier bank on its first real frame. Added
   `RenderGraphResourcePool::EndPreparation`, preserving prepared usage without
   fabricating GPU submission. The configuration transaction uses it only on
   success; actual failures still abort. The added bounded GPU smoke verifies
   three distinct banks remain/reuse the same handles under a zero cache budget,
   retain submission zero, and an explicit abort still makes its bank reclaimable.
2. Auto Exposure History was newly allocated/imported as ShaderResource even
   though Vulkan's actual initial layout was Undefined. Allocate/import it as
   Common until its first real submission; only then mark its submitted state and
   request one graph rebuild for subsequent SRV imports. Aborted work never
   advances this state. This removes the Vulkan InvalidImageLayout error observed
   when turning auto exposure on during FSR rendering.

Verification:

- **build-only PASS**: `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`
  (final source build 98.3s; earlier preparation-only build 68.2s).
- **build-only + runtime-manual PASS**: `powershell -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineFsrSmoke -Configuration RelWithDebInfo -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -Run -Wait -TimeoutSeconds 180`
  (final build 79.6s; run PID 75732, exit 0). Log prefix:
  `out/agent/projects/7e44ffe2e91e72bd/logs/run-game-relwithdebinfo-20260929-160201-722-12408`.
  Both APIs print FSR_PREPARED_BANKS_PASS, then pass existing FSR reset/resize/
  constant-color dispatch/readback checks (max error <0.003640); stderr empty.
  The first version of the tiny test lacked sampled texture capability and DX12
  rejected that allocation; the final fixture declares its intended SRV use.
- **runtime-manual PASS**: `powershell -ExecutionPolicy Bypass -File out/agent/fsr-integration/Capture-ExposureFsr.ps1 -GraphicsApi DX12`
  and the same with `-GraphicsApi Vulkan`. Final directories:
  `out/agent/captures/fsr-exposure-DX12-20260929-155938` and
  `out/agent/captures/fsr-exposure-Vulkan-20260929-155850`.
  UECloseupAnimated, 853x480 to 1280x720: manual compensation -4/0/+4 EV;
  auto exposure with directional light 0.03 -> 300 -> 3, eight HDR and eight
  display samples per change, then 120 simulation steps. Each backend has 36
  finite HDR readbacks with actual FSR active, preExposure=1 and no standalone TAA.
  Artistic exposure changes do not scale FSR input: mean absolute input difference
  relative to neutral mean stays below 1.17% (jitter/edges). Display brightness
  responds strongly to compensation. Auto exposure visibly adapts in the expected
  direction after light steps; capture pacing differs, so this is not an exact
  wall-clock adaptation-rate or image-quality proof. Both preview sheets inspected.
- **static-check PASS**: `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/fsr-integration/Analyze-ExposureFsr.py out/agent/captures/fsr-exposure-DX12-20260929-155938 out/agent/captures/fsr-exposure-Vulkan-20260929-155850`.
  Both final logs have zero validation errors and empty stderr. The earlier Vulkan
  directory `fsr-exposure-Vulkan-20260929-155320` is the failing pre-fix evidence.
- **runtime-manual PASS (DX12)**: `powershell -ExecutionPolicy Bypass -File out/agent/fsr-integration/Capture-HudFsr.ps1 -GraphicsApi DX12`.
  Final directory `out/agent/captures/fsr-hud-DX12-20260929-160234`, actual
  SickaDynasty gameplay. At 1440p and 2160p, native vs Performance FSR HUD rects
  match; 20437 opaque interior pixels across objective/weapon/ammo/vitals/
  crosshair/healthbar match exactly. The actual graph orders FSR -> HDR post ->
  ToneMapping -> RuntimeUiRender -> UiComposite -> Present; no SceneTAA pass.
  **static-check PASS**: `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/fsr-integration/Analyze-HudFsr.py out/agent/captures/fsr-hud-DX12-20260929-160234`.
  Glyph interiors exclude background-dependent anti-aliased edges. 4K preview
  inspected. A concurrent attempt during the smoke build was correctly refused
  by the storage lease before startup; final run was sequential after completion.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.

Unresolved pressure failure, not a passing Vulkan HUD regression:

`Capture-HudFsr.ps1 -GraphicsApi Vulkan` captured native 1440p and FSR 1440p/2160p,
then attempted native 2160p on the 6 GiB GPU. Before EndPreparation it committed
and subsequently failed creating a GBuffer; directory `fsr-hud-Vulkan-20260929-154447`.
After the fix it rejects the settings transaction before commit with success=false;
directory `fsr-hud-Vulkan-20260929-154903`. The NVRHI Vulkan createTexture function
leaks its raw Texture/VkImage when its memory allocation returns failure, causing
VkDevice destruction validation errors even after a correctly rejected transaction.
Prepared minimal RAII ownership patch:
`out/agent/fsr-integration/nvrhi-vulkan-texture-failure.patch`.
`git apply --check --ignore-space-change` passes. It is NOT applied: AGENTS.md
requires explicit permission to modify Engine/Source/ThirdParty. An asynchronous
question is pending; no permission has been received. The HUD driver now has an
explicit `-AllowNative4KRejection` branch that will check unchanged revision/hash
and continued FSR rendering across all banks; this new branch has not run yet.

All final owned Game PIDs (86720, 4948, 89648) and smoke PID 75732 checked absent;
failed owned sessions also terminated. C: free 5.36 GiB, no cleanup performed.
The smoke build last configured the shared project cache with render-smoke flags;
the recorded Game executable remains the separately built pinned artifact above.
No Editor, unit-test, Shipping, second-project or third-party-modified build claimed.

Streaming/combat inspection: TextureManager already bounds CPU staging/uploads and
publishes with queries/fence retirement; SceneRenderer retains a 256 MiB inactive
material cache for 300 frames. Textures still load complete chains with no active
view-demand mip residency budget. RuntimeResourceRegistry and warmup groups already
provide asynchronous CPU loaders, main-thread publication and generation-specific
leases. NavigationService exists; ShooterGame enemies still use sine patrol and
timer shots. Reuse these systems for active demand/residency and reusable combat
states/action synchronization; these upgrades have not been implemented by this
checkpoint. Fast motion/disocclusion, special-blend/GPU particles, restart settings,
unsupported-view messaging and second-project reuse remain open. Full goal active.

## Implementation checkpoint — 2026-09-29 16:22

The preceding API-support answer was no implementation progress. This turn
implemented and verified managed combat/action integration; the full goal remains
active. No ThirdParty permission was received and no ThirdParty file was changed.

- New reusable `HEngine.ScriptCore/CombatAgent.cs`: validated immutable tuning,
  cone/range/occlusion perception, continuous acquisition, last-seen memory,
  sound/damage investigation, locked aim during interruptible Windup, one-shot
  attack release, recovery, death/reset and spawn grace. It owns no scene/native
  handles, game paths or global state. One candidate target; caller owns navigation,
  collision, weapons and animation. Large steps cannot skip new warnings/burst fire.
- New `ActionTimeline.cs`: shared normalized animation/action time, commit/end
  pulses, cancellation before/after commitment, and bounded elapsed time.
- External `Scripts/CSharp/ShooterGame.cs`: sentries consume the brain instead of
  omniscient facing/timer fire. Bounded turning and movement, last-seen search,
  spawn leash, body-clearance ray samples, windup body pulse and existing-HUD
  warning; fire toward locked aim point with a fresh shot-path check. Damage
  supplies an investigation stimulus; death and wave reset clear brain state.
  Hovering agents do not use the ground navigation mesh. Ray samples are not a
  swept-volume collision guarantee. Existing projectile sweeps remain in use.
- Reload/equip use ActionTimeline. Reload animation and ammo transfer share its
  clock; commit remains at the clip end. Switching cancels an incomplete reload
  without changing ammo. Death cancels pending actions. No invented clip marker
  or claim of automatic animation-event extraction. Configuration defaults make
  existing project settings compatible. Usage/tuning documented in
  `Docs/ManagedCombat.md`.
- External pre-edit backups: `out/agent/combat-upgrade/backups/` (script and JSON;
  JSON was not changed). The external project is not a Git repository.

Evidence:

- **unit-test PASS**: `C:/Program Files/dotnet/dotnet.exe run --project Tools/Scripting/CombatTests/CombatTests.csproj --configuration Release --artifacts-path C:/WorkSpace/HEngineDev/out/agent/managed-combat`.
  Actual ScriptCore ProjectReference, 36 assertions: perception, interrupted
  acquisition/warning, fixed aim, memory/expiry, recovery/hitches, death/reset,
  investigation, invalid values, action marker uniqueness and cancellation.
- **build-only PASS**: `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
  Configure 5.4s, HGame build 93.3s. GameScripts built with ScriptCore successfully.
  Game SHA256 `AC376D41757B0CD2A5265B648835611BD0D3839BEF5F21B4BE2EA4510EA8A12F`;
  GameScripts SHA256 `b55088a5d475225c96647407366205eeb49a77acd73e26e8e42375347c9532ef`.
- **runtime-manual PASS**: `powershell -ExecutionPolicy Bypass -File out/agent/combat-upgrade/Test-Combat.ps1 -GraphicsApi DX12`
  and the same command with `-GraphicsApi Vulkan`. Owned paused/fixed-step sessions
  in SickaDynasty, FSR Quality active throughout five captures per API. Evidence:
  `out/agent/captures/combat-DX12-20260929-161810` and
  `out/agent/captures/combat-Vulkan-20260929-161846`.
  Actual input fires two shots, starts reload, switches away/back, reloads again.
  Both logs record cancel at ammo=28, exactly one commit at ammo=30/reserve=178,
  and all three agents acquiring, pursuing, warning, releasing and recovering.
  Both preview contact sheets inspected; reload pose/progress, weapon switch and
  incoming-fire text visible. These runs establish basic integration, not full
  motion-quality/dodging/occlusion stress or AAA animation quality.
- The first DX12 attempt `combat-DX12-20260929-161732` failed the driver's required
  reload-cancel observation: input held states worked, but Pressed edges were
  consumed while paused. Changed the driver to queue keys with `onStep=true`;
  the final two runs above pass. No engine input change was made.
- **static-check PASS**: `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/combat-upgrade/Analyze-Combat.py out/agent/captures/combat-DX12-20260929-161810 out/agent/captures/combat-Vulkan-20260929-161846`.
  Checks exact ammo/commit counts, every agent phase, FSR-active capture sidecars,
  empty stderr and absence of validation/script errors. Initial analyzer assumed
  UTF-8 logs; final analyzer matches ASCII evidence over lossless byte decoding.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.
- Owned final PIDs DX12 103292 and Vulkan 123872 confirmed absent;
  the failed initial owned session was also stopped. No user process touched.
  No Editor, Shipping, second-project or native C++ unit-test coverage claimed.

Next remaining work includes active view-demand texture mip residency/budgets,
combat navigation/obstacle and pause/death/wave scenarios, fast motion and
special-blend/GPU particle rendering tests, restart persistence and second-project
reuse. TextureManager already has stable generation handles, asynchronous prepare/
upload and safe reload publication/retirement; reuse those for streaming rather
than duplicating ownership. Its current MaximumDimension is a fixed load variant,
not runtime residency. The pending third-party allocation-failure cleanup patch
and Vulkan native-4K rejection regression are unchanged from the prior checkpoint.

## Implementation checkpoint — 2026-09-29 16:40

Previous turn was implementation progress (managed combat/action integration).
This turn implemented actual material mip residency changes, not just loading
settings. Full goal remains active; no ThirdParty source changed/permission received.

Implementation:

- `TextureLoadDesc` adds explicit streamable opt-in and an initial resident mip.
  Streamable and pinned cache identities differ. Ready, readback, environment
  proposal and non-mip consumers cannot opt into streaming accidentally.
  SceneRenderer opts legacy and material-parameter textures in at mip 2; UI,
  static lightmaps and environment inputs stay on their existing pinned path.
- `ManagedTexture` records source size and published first resident mip. DDS
  selects its existing compressed mip tail. Image preparation generates the full
  existing alpha-coverage-preserving mip chain before selecting the resident tail.
  Upload/publication continues through existing async preparation, GPU event
  queries, stable managed handles/descriptors and fence-retired old resources.
- `TextureStreamingPolicy.h` is a device-independent planner. It accounts for
  every mip and compressed block, prioritizes requested detail against a byte
  budget, clamps indices and reports an impossible minimum budget explicitly.
- Renderer gathers max projected pixel demand across all uses of each material
  texture, using output dimensions so FSR does not lower the texture target.
  Valid static bounds are clip-tested and projected; unknown/skinned bounds request
  full detail. Off-view demand persists 60 frames. Promotions/demotions have
  separate dwell times; failed changes back off. Two residency replacements at
  most are in flight. Published/pending/retiring payloads constrain promotions;
  large demotions can use a smaller intermediate tail to fit temporary headroom.
- Default streamable-material target: 512 MiB per TextureManager. This is payload
  accounting, NOT physical VRAM or a global device cap. Initial/pending versions,
  pinned resources, GPU alignment, render targets and old binding-set references
  require separate accounting/validation. Public EndStreamingFrame accepts a
  budget; development-only `HE_TEXTURE_STREAMING_BUDGET_MB` injects pressure.
  Added transition and periodic residency diagnostics. See `Docs/TextureStreaming.md`.

Verification:

- **build-only PASS**: `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
  First compile failed because the new demand block referred to an object-loop
  local warmup flag outside its scope. Corrected: demand visits only world.Objects,
  not warmup objects. Subsequent build 52.1s PASS. Final headroom refinement build
  17.7s PASS. Final Game SHA256:
  `91E6041E484FF24623778AB9982C6B03338342F1CF896D79254B8E5172926FB4`.
- **unit-test PASS**: `powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name TextureStreaming -Configuration RelWithDebInfo`.
  HEngineTests built in 131.6s; test PASS at 16:34:01. Coverage: non-square mip tails,
  BC block rounding, sufficient budget, pressure priority, impossible minimum,
  mip clamping/no promotion beyond demand, and empty demand. No GPU coverage is
  claimed for this pure planner test. Later manager headroom refinement did not
  change the tested policy/header.
- **runtime-manual PASS, initial residency code**:
  `powershell -ExecutionPolicy Bypass -File out/agent/streaming/Test-Streaming.ps1 -GraphicsApi DX12 -BudgetMB 512`
  -> `out/agent/captures/streaming-DX12-512-20260929-163145`;
  93 assets changed mip; settled streamable+retiring payload 532794948 bytes,
  under 536870912-byte target. Same driver with `-GraphicsApi Vulkan -BudgetMB 8`
  -> `streaming-Vulkan-8-20260929-163304`; 92 assets changed mip; settled 8212484
  bytes under 8388608. Both FSR Quality and existing combat/reload checks pass.
- **runtime-manual PASS, initial traversal**:
  `powershell -ExecutionPolicy Bypass -File out/agent/streaming/Test-StreamingTraversal.ps1 -GraphicsApi Vulkan -BudgetMB 512`
  -> `streaming-Vulkan-512-20260929-163420`. Actual WASD moves camera 8.78988 world
  units; 103 publications after movement began; settled payload 527453764 bytes.
  These three runs use prior SHA
  `8632F73720C091964FEBFF02CF0F605BCE59EB30E84AF8EB2709D9BA823A57F4`.
- Review then found the temporary per-resource 64 MiB restriction would prevent a
  large uncompressed texture from returning to mip zero even with room available.
  Removed that unconditional promotion ceiling; retain the aggregate headroom
  check, and allow a smaller intermediate demotion if its target exceeds scratch.
  Large uncompressed (>64 MiB) recovery still needs a dedicated GPU fixture.
- **runtime-manual PASS, final source**:
  `powershell -ExecutionPolicy Bypass -File out/agent/streaming/Test-StreamingTraversal.ps1 -GraphicsApi DX12 -BudgetMB 8`
  -> `out/agent/captures/streaming-DX12-8-20260929-163700`;
  92 assets changed mip, 257 transitions, 125 publications after movement began,
  8.78988-unit camera displacement, settled 8129028 bytes under 8388608.
  Same command with `-GraphicsApi Vulkan -BudgetMB 512`
  -> `out/agent/captures/streaming-Vulkan-512-20260929-163809`;
  93 assets changed mip, 228 transitions, 101 post-movement publications,
  same displacement, settled 527453764 bytes under 536870912. Sixteen promotion
  attempts waited for headroom; the final resident/retiring total equals the plan.
  Both final runs retain actual FSR, correct reload/cancel/commit and enemy attack
  logs, zero validation/script error matches and empty stderr. The final idle
  capture intentionally continues until the stationary player loses; it is not
  a new combat failure. Traversal capture occurs while the player is alive.
- **static-check PASS**: `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/streaming/Analyze-Streaming.py <capture-directory>`
  applied to each of the five exact directories above; JSON reports are stored
  beside captures. Checks real mip publication, budget convergence, actual camera
  displacement/continued changes where applicable, FSR state and error logs.
  All five sequentially generated <=1280 previews inspected. 8 MiB pressure
  visibly lowers world texture detail; HUD remains readable. No claim of native
  4K quality equivalence or measured physical VRAM reduction.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.
  Owned PIDs 71736, 114588, 31940, 69876 and 119096 all confirmed absent.
  C: free 4.98 GiB; no cleanup necessary. No Editor or Shipping validation.

Remaining streaming work: production/persisted budget controls and diagnostics,
partial DDS mip IO/decoded-tail reuse, sharper UV-density/dynamic-bound demand,
large-texture recovery fixture, cancellation/hot-reload/failure races, binding-set
retention/actual VRAM and long traversal/performance validation. Other goal items
remain open: broader combat/navigation and action edge cases, fast temporal motion,
special-blend/GPU particles, settings restart, second project, and the permission-
gated NVRHI failure cleanup/4K rejection regression. Do not declare the full goal
complete on the basis of these bounded scene tests.

## Implementation checkpoint — 2026-09-29 17:01

Previous turn made implementation progress with actual mip residency. This turn
completed persisted/live budget controls, diagnostics and saved-FSR restart checks.
Full objective remains active. No ThirdParty permission received/files changed.

- Added `Rendering.TextureStreaming.BudgetMiB`: integer [1,8192], default 512,
  Project/User/Session layers, Live impact. Registry normalization, equality,
  document round-trip, startup descriptors, per-view configuration rollback and
  commit now carry the budget. Live-only changes avoid graph preparation and call
  the budget setter; future views inherit the new base configuration. The setting
  is per TextureManager, not global GPU memory. World/camera writes are excluded.
- `RenderPreferences.TextureStreamingBudgetId` exposes the stable ID to reusable
  managed code. MechanicGirlLab menu adds 128/256/512/1024 MiB choices with Chinese/
  English labels, keyboard handling and normal user-layer persistence. External
  originals backed up in `out/agent/streaming-settings/backups/` before editing.
- Fresh raster capture metadata now includes `textureStreaming` target/planned/
  resident-pending-retiring bytes, counts, transitions, stalls, minimum overrun,
  pending and failed uploads. This explicitly describes payload/reservations,
  not physical VRAM. Updated `Docs/TextureStreaming.md`.
- `Session.ps1 -SavedConfigSource` optionally copies JSON preferences from a prior
  owned run into a fresh owned Saved/Config before starting a new process. Default
  session behavior is unchanged. Source preferences and user project Saved are
  untouched. Restart validation uses the normal native user-settings load path,
  no render-settings startup override or after-start patch.
- Visual QA caught budget choices clipped by the settings scroller. The script
  now scrolls the entire opened list into view. The engine's existing scripted
  ScrollElementIntoView resolves document layout first, so freshly inserted/shown
  content has current boxes. Final captures and rectangle checks show all four
  options inside the clip in both APIs; no ThirdParty RmlUi edit was needed.

Evidence:

- **unit-test PASS**: `powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name RenderSettings -Configuration RelWithDebInfo`.
  Build 144.2s; RenderSettings PASS at 16:48:55. New coverage checks budget equality,
  saved document round-trip, live impact with no GPU idle/target recreation/history
  reset, minimum normalization and wrong-type rejection. The later UI-only fix
  does not change this tested settings logic. No reflected resource schema changed.
- **build-only PASS**: `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
  First build was correctly rejected by the managed source-generation guard because
  frontend source changed while compilation was active. Rebuilt stable inputs:
  76.7s PASS. Final UI fix build 18.7s PASS. Final Game SHA256:
  `E16DDCB31A2694DDFF4F8B3995CC027E32CB837B4E72B72669DC16A16AC45AB7`.
  Final GameScripts SHA256:
  `939ada5220a1aaad0c734193365576610de6fa13ecc280f1c6d3dbaef38eb11a`.
- **static-check PASS**: `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/Validate-Frontend.py`.
  Five outfits, compatible skeleton/material references, 61 unique authored UI IDs.
- **runtime-manual PASS, final code**:
  `powershell -ExecutionPolicy Bypass -File out/agent/streaming-settings/Test-Settings.ps1 -GraphicsApi DX12`
  -> `out/agent/captures/streaming-settings-DX12-20260929-165700`;
  same command with `-GraphicsApi Vulkan`
  -> `out/agent/captures/streaming-settings-Vulkan-20260929-165804`.
  In both APIs, actual menu input selects 128 MiB; a wrong-type user-layer write
  returns invalid_type without changing revision/hash; runtime user-layer changes
  drop to 8 then recover to 256 MiB. All phases converge with zero pending/failed
  uploads, FSR Balanced active and sharpness 0.35. Residency payload by phase:
  512 -> 532799124 bytes; 128 -> 130229488; 8 -> 8207024;
  recovered 256 -> 264447216; restarted 256 -> 264535700.
  The FSR submitted-frame counter continues across live budget updates.
- Each API stops its first process, starts a new owned process with byte-identical
  saved JSON, and verifies FSR3.1/Balanced/0.35/256 MiB via the effective settings,
  actual raster dispatch/streaming metadata and menu screenshot. Saved UserEngine
  SHA256 `DCE586A1C98D194EDD6C8DBFE6B20C4BA696E6D2D124CCE61213B0B165075395`
  matches before/after new-process startup. This is a seeded private-Saved restart,
  not a claim about an untested user launcher or user's existing configuration.
- **static-check PASS**:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/streaming-settings/Analyze-Settings.py out/agent/captures/streaming-settings-DX12-20260929-165700`
  and the same command using `out/agent/captures/streaming-settings-Vulkan-20260929-165804`.
  Checks every budget's real diagnostics and convergence, FSR quality/sharpness,
  saved settings, invalid-type rejection, all dropdown-option clip rectangles,
  zero validation/script error matches and empty stderr in both processes.
  Final <=1280 previews inspected sequentially.
- Earlier driver attempts 165110 and 165211 failed because polling reused capture
  names; fixed unique numbered names (PowerShell postfix increment inside a
  string emits no value, so the counter is now advanced separately).
  Intermediate DX12 165255 and Vulkan 165400 passed persistence/pressure but their
  list was visibly clipped; final runs above supersede UI evidence.
  One analyzer command had a mistyped Vulkan directory suffix 165805; reran with
  actual 165804 successfully. No runtime rerun was needed for that path typo.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.
  Final DX12 PIDs 97060/101528 and Vulkan 70308/120168 confirmed absent; all six
  earlier owned processes from failed/intermediate runs also confirmed stopped.
  No user process touched. No Editor/Shipping/new-project validation claimed.

Remaining at this checkpoint: partial DDS mip IO/decoded-tail reuse; dedicated >64 MiB texture
recovery, cancel/hot-reload/failure and in-flight descriptor publication tests;
physical VRAM/binding retention and long traversal/performance validation. Other
open requirements remain broader combat/navigation/action transitions, fast motion
and disocclusion, special-blend/GPU particles, unsupported-view messaging and a
second project. Pending NVRHI allocation-failure cleanup remains permission-gated.

## Implementation checkpoint — 2026-09-29 17:12

The preceding question-answer turn only confirmed API support (no implementation
progress). Revalidated the current tree and completed partial DDS mip IO:

- `DdsTextureData::LoadMipTail` reads only the legacy 128-byte or DX10 148-byte
  header plus the selected contiguous mip tail. Header/layout validation checks
  the full declared chain against file length before allocating the tail. It
  retains the existing format, dimension, array/cube/volume and alpha constraints.
  Size limits select a logical source mip before the requested resident mip;
  upload offsets are rebased, pitches and compressed bytes remain unchanged.
- Streamable TextureManager DDS preparations use this path. Pinned DDS remains
  unchanged. Completed AssetReadAhead data is consumed without reopening the
  file, though that earlier prefetch still reads a whole file. Ordinary image
  decoding/mip generation is still full-chain and has no decoded-tail cache.
- `FileBytesRead` reports bytes requested by this reader (header + tail, or zero
  for consumed staged data), not OS cache misses or physical disk traffic.

Evidence:

- **unit-test PASS**: `powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name DdsTextureData -Configuration RelWithDebInfo`.
  Build 63.3s; test PASS at 17:09:22. Disk fixtures cover BC1/3/5/7, sRGB and linear
  encodings, RGBA16F, legacy headers, mip 0/1/2/out-of-range clamping, exact payload
  equality and every retained offset/pitch, maximum-dimension ordering, incomplete
  mip chains, missing files and truncated DX10 headers/payloads. Read-byte counts
  exclude omitted higher mips. The staged-tail test verifies zero additional reads
  and removal from staging. Existing full-load/malformed-header coverage also passes.
- **build-only PASS**: `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
  51.3s. Game SHA256 `90229C86D16919E3ECDD792592A71B17530D3CA0D9663FEDD55E4C645178CB9E`.
- **runtime-manual PASS**: `powershell -ExecutionPolicy Bypass -File out/agent/streaming-settings/Test-Settings.ps1 -GraphicsApi DX12`
  -> `out/agent/captures/streaming-settings-DX12-20260929-170943`;
  same command with `-GraphicsApi Vulkan`
  -> `out/agent/captures/streaming-settings-Vulkan-20260929-171045`.
  Actual SickaDynasty DDS publications exercise the changed path. Both APIs settle
  at the same payloads as the previous implementation: 512 MiB -> 532799124 bytes;
  128 -> 130229488; 8 -> 8207024; recovered 256 -> 264447216; restarted 256 ->
  264535700. FSR Balanced/.35 remains active, pending/failed uploads reach zero,
  and persisted settings restore in a new owned process. Menu option bounds also
  pass. These are runtime observations, not GPU race proof or physical VRAM tests.
- **static-check PASS**: `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/streaming-settings/Analyze-Settings.py out/agent/captures/streaming-settings-DX12-20260929-170943`
  and the same analyzer with `out/agent/captures/streaming-settings-Vulkan-20260929-171045`.
  Zero matched validation/script errors and empty stderr in all four processes.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.
  Owned PIDs 94916/112340/90964/93760 confirmed absent after finally cleanup.
  No ThirdParty source, project defaults, user processes or reflected schemas changed.

Next priority established by source audit: `PublishCompletedSlot` currently calls
`BindlessTextureTable::UpdateTexture` on an existing handle after the upload query,
without requiring all later submitted readers to finish. The table API explicitly
requires prior readers completed or a new handle with retirement of the old one.
NVRHI DX12 `writeDescriptorTable` immediately copies into its shader-visible heap.
Retaining the old texture alone does not protect that descriptor from overwrite.
Fix heap-direct publication by allocating a new physical descriptor, retiring the
old one at the last submission, and refreshing the published shader index through
the existing TextureManager revision/cache invalidation. Managed texture handles
can remain stable; physical descriptor identity need not. Bounded mode rebuilds
immutable binding sets and needs separate retention verification. This audit is
source evidence of an unguarded path, not a reproduced GPU corruption claim.

Other pending requirements retain their previous scope; the full goal remains active.

## Implementation checkpoint — 2026-09-29 17:22

Previous goal turn was progress (partial DDS IO and actual dual-backend validation).
Revalidated its publication finding and fixed the heap-direct descriptor hazard:

- `BindlessTextureTable::TryPublishTexture` uses a new physical descriptor when
  reported readers remain in flight, retiring the original at the last submitted
  fence. Bounded mode replaces its immutable binding set. In-place heap updates
  require completedSubmission >= lastSubmittedSubmission. An exhausted heap with
  readers returns false without changing the handle/resource; retry follows frame
  collection. The manager retains its pending upload/query and published texture.
- TextureManager publishes descriptors before completing/moving the resource,
  refreshes ShaderDescriptorIndex, and increments revision only on publication.
  Explicit ready requests report an exhausted in-flight heap instead of returning
  a placeholder. No device-idle wait was added. Managed handles stay stable.
- Both legacy and parameter material caches previously refreshed only normal
  encoding on revision changes. They now refresh physical shader indices too.
  Existing frame packing applies those indices before writing the new frame's
  GPU material data. Lightmap and other direct lookups already query the manager.
- New capture counters: descriptorReplacements and descriptorPublicationStalls.
  NVRHI source audit confirms bounded binding sets hold strong resource references
  and submitted Vulkan command buffers retain trackLiveness binding sets. No
  ThirdParty edits were needed.

Evidence:

- **build-only PASS + runtime-manual PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineFsrSmoke -Configuration RelWithDebInfo -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -Run -Wait -TimeoutSeconds 180`.
  Build 108.2s; owned PID 112988 exited 0. Logs:
  `out/agent/projects/7e44ffe2e91e72bd/logs/run-game-relwithdebinfo-20260929-171723-817-84756.{stdout,stderr}.log`.
  TEXTURE_PUBLICATION_PASS on both backends, FSR_SMOKE_PASS, empty stderr and no
  matched validation errors. New native-resource contract checks cover distinct
  heap indices, stale handle invalidation, no mutation under exhaustion, collection
  before/at retirement, recycled generation, safe idle overwrite and final zero
  live/retired slots. Both APIs check old/new bounded binding sets reference their
  respective textures and retain the old snapshot after table release. These
  checks use conservative synthetic submission numbers, not a GPU race injection
  or shader-based observation of a descriptor being overwritten mid-draw.
- During that build, separate manager diagnostic/cache changes were still being
  finalized. The tested table and smoke code were unchanged; the subsequent stable
  full Game build below compiled all final inputs before scene validation.
- **build-only PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
  137.9s. Final Game SHA256:
  `6E6B45BEEF6D988261B5FD1A8B97BB0B3152C2A49B7C55CB378E7E3EF1AFFB80`.
- **runtime-manual PASS**:
  `powershell -ExecutionPolicy Bypass -File out/agent/streaming-settings/Test-Settings.ps1 -GraphicsApi DX12`
  -> `out/agent/captures/streaming-settings-DX12-20260929-172017`;
  same command with `-GraphicsApi Vulkan`
  -> `out/agent/captures/streaming-settings-Vulkan-20260929-172125`.
  Both APIs converge at 512/128/8/recovered256/restarted256 MiB targets with zero
  pending/failed uploads; FSR Balanced/.35 remains active and saved settings restore.
  Payload results are unchanged from the 17:12 checkpoint. DX12 replacements by
  phase: 379, 468, 558, 648, 388 (last value is a new process). Vulkan's physical
  handle replacement counter stays zero because it replaces bounded binding sets.
  Publication stalls are zero throughout. Scene previews <=1280 inspected
  sequentially for both APIs; DX12 recovered-menu preview also inspected.
- **static-check PASS**:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/streaming-settings/Analyze-Settings.py out/agent/captures/streaming-settings-DX12-20260929-172017`
  and same with `out/agent/captures/streaming-settings-Vulkan-20260929-172125`;
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/descriptor-publication/Analyze-Publication.py out/agent/captures/streaming-settings-DX12-20260929-172017`
  and same with the Vulkan directory. Real replacement counters grow across the
  pressure/recovery phases; all four process stderr files are empty and error
  pattern checks pass. Owned PIDs 67224/47588/92744/111688 confirmed stopped.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.
  One initial multi-file patch was rejected for listing a target twice; no partial
  mutation occurred, and the consolidated patch succeeded. No headless test,
  Editor, Shipping or second-project run is claimed in this checkpoint.

Remaining: dedicated >64 MiB replacement/recovery and cancellation/hot-reload/IO
failure fixtures; long traversal and physical memory/performance checks; ordinary
image decoded-tail reuse; broad combat navigation/action edge cases; fast motion,
disocclusion and special-blend/GPU-particle quality; unsupported-view messaging and
second-project portability. The pending NVRHI allocation-failure cleanup still
requires the previously requested ThirdParty permission. Full goal remains active.

## Verification checkpoint — 2026-09-29 17:32

Previous goal turn was progress (descriptor publication and cache fixes). This
turn adds a reusable real-device streaming fixture to HEngineFsrSmoke and resolves
the dedicated >64 MiB, IO-retry/hot-reload and cancellation evidence gaps. No
production runtime source needed a change after the exercised paths passed.

- `StreamingValidation.cpp/.h` creates owned BC7 DDS fixtures in the run's Saved
  evidence folder and runs production TextureManager. The initial 8192x8192,
  complete mip chain uploads 89478512 bytes. It demotes to mip 2 (2048x2048,
  5592432 bytes) under an 8 MiB budget and returns to the full chain at 128 MiB.
  This directly verifies promotion of a replacement larger than the 64 MiB
  temporary-headroom constant. It does not cover every large intermediate-
  demotion combination or a hard GPU memory cap.
- Repeated loads share the same managed handle and reference ownership. A
  deliberately truncated live source causes reload failure while the old native
  resource, payload and publication revision remain unchanged. Repairing it to
  a 4096-square chain and invalidating the failure publishes 22369648 bytes on the
  original managed handle, with new source dimensions and a new native resource.
- A second fixture is released during preparation, then reloaded with a new
  managed generation and released while its real GPU query is still Submitted.
  The query is allowed to complete before its accounted payload disappears.
  Releasing the first fixture during a live reload also drains prepared data.
- A unique missing filename causes an actual open failure and preserves the
  default fallback. Writing a valid 4x4 DDS at that path and invalidating its
  cached failure publishes BC7 on the existing handle. Each backend observes
  exactly two expected failures (corrupt source and missing file), two budget
  transitions, final default payload 16 bytes and only three default textures.
  GlobalWaits stays at the one default-initialization wait; no streaming wait was
  added. All waits have 30-second per-condition deadlines and owned process limits.

Evidence:

- **build-only PASS + runtime-manual PASS**, final inputs:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineFsrSmoke -Configuration RelWithDebInfo -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -Run -Wait -TimeoutSeconds 180`.
  Final incremental build 7.4s; owned PID 89484 exits 0. Program SHA256:
  `2B079AA9067EF06E814F1F2EB8C56CD0A1A13A7BE53FE0D2FF811F9D7BCBF073`.
  Logs `out/agent/projects/7e44ffe2e91e72bd/logs/run-game-relwithdebinfo-20260929-173019-842-113228.{stdout,stderr}.log`.
  Both APIs print TEXTURE_STREAMING_PASS and TEXTURE_PUBLICATION_PASS; the normal
  FSR dispatch/reset/resize/readback checks also pass. Four expected warning lines
  correspond exactly to the two injected source failures per API. No matched
  validation errors and stderr is empty. DX12 native debug layer remains unavailable;
  NVRHI validation was active. Vulkan validation is active.
- Evidence JSONs inspected at
  `out/agent/projects/7e44ffe2e91e72bd/sessions/20260929-173019-842-113228/Saved/Diagnostics/FsrSmoke/Streaming/DX12/result.json`
  and sibling `Vulkan/result.json`: PASS; full/demoted/reloaded/final bytes as
  above, expectedFailures=2, transitions=2, all cancellation and missing-file
  retry flags true. These are real upload/lifetime assertions, not pixel-quality
  checks, shader reads of old descriptors or physical memory measurements.
- The first build (93.9s) and PID 65556 also passed the initial fixture, but it
  compiled before the missing-file case was added. It is superseded by the final
  stable-input build/run above. Earlier log suffix: 20260929-172938-727-580.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.
  PIDs 65556 and 89484 confirmed absent. No Game rebuild or new scene capture was
  needed because this turn changes only the smoke program, its source list and
  documentation; the production Game from 17:22 is unchanged. The shared external
  build cache is currently configured for the smoke target; hectl will configure
  it for the next requested Game operation. No Editor/Shipping/second-project
  validation is claimed. All generated binary assets stay on disk.

Remaining streaming work: device allocation/upload failure injection and large
intermediate-demotion cases, decoded ordinary-image tail reuse, long traversal
performance and physical VRAM/binding-retention measurement. Other open rendering,
combat/animation, unsupported-view and portability requirements retain full scope.
The NVRHI ThirdParty patch remains unapplied pending explicit permission. Goal active.

## Implementation checkpoint — 2026-09-29 17:48

Previous turn was progress (real-device large-texture/failure/cancellation evidence).
This turn addresses combat lifecycle gaps in the current external ShooterGame:

- Added one FinishCombat path for defeat/victory. It cancels reload/equip timelines,
  clears hostile projectiles and warning state, stops enemy brains and lets the
  existing action layers fade back to idle. Previously, projectiles froze visibly
  when UpdateEnemies stopped after completion. Defeat now returns before wave
  advancement, preventing a same-frame lethal hit and empty-wave condition from
  awarding new-wave healing. That simultaneous condition is guarded in source;
  the runtime fixture below does not deliberately synthesize both on one frame.
- New waves restore enemy transforms immediately and reset brains with the same
  staggered spawn grace as initial spawning. F5 returns immediately after asking
  to reload, so no further gameplay runs on the old scene in that script update.
- Extended shared CombatAgent regression cases to warning pause, death mid-warning,
  reset clearing locked aim/memory, spawn grace and target death during warning.
  The reusable module itself needed no change for these passing cases.
- External `Tools/Build-CombatLifecycleDiagnostics.py` generates separate death
  and three-wave diagnostic scenes from SickaDynasty. Controlled tuning makes
  lethal damage during a long reload and one-hit/zero-damage three-wave combat
  repeatable; production scene/settings are unchanged. ShooterGame pre-edit backup:
  `out/agent/combat-lifecycle/backups/ShooterGame.cs`.

Evidence:

- **unit-test PASS**:
  `dotnet run --project Tools/Scripting/CombatTests/CombatTests.csproj --configuration Release --artifacts-path C:/WorkSpace/HEngineDev/out/agent/managed-combat`.
  COMBAT_ACTION_TESTS_PASS checks=42 (previously 36), referencing actual ScriptCore.
- **static-check PASS** (fixture generation and single-script schema assertions):
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/Build-CombatLifecycleDiagnostics.py`.
- **build-only PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
  87.1s; final Game SHA256 `36EB1913567A0C256AE8DE6A4F3587BCCB552AB95500988510EEA0AC38E902B4`;
  GameScripts SHA256 `70412B52578DC7825ED02A54F36E5FE43F5D69C4EBD908CCD3A3D6FC79E54324`.
  Cache is restored to normal Game configuration.
- **runtime-manual PASS**, final driver:
  `powershell -ExecutionPolicy Bypass -File out/agent/combat-lifecycle/Test-Lifecycle.ps1 -GraphicsApi DX12`
  -> `out/agent/captures/combat-lifecycle-DX12-20260929-174334`;
  same command with `-GraphicsApi Vulkan`
  -> `out/agent/captures/combat-lifecycle-Vulkan-20260929-174616`.
  Actual menu pause for 480 simulation steps emits no attack/damage/reload-complete
  events. After resume, lethal damage cancels active reload with ammo=29, wave=1,
  and all 16 projectile entities have zero scale. Fire/reload/switch inputs after
  defeat are inert. F5 recreates combat and restores initial ammo; a new first
  shot again leaves 29 rounds. In a new controlled scene, actual pointer aiming
  and fire inputs produce 9 kills across 3 waves, exactly one victory, no residual
  projectile geometry and no post-victory attack/shot/reload events.
  Both APIs keep FSR Quality active in all six captured checkpoints.
- **static-check PASS**:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/combat-lifecycle/Analyze-Lifecycle.py out/agent/captures/combat-lifecycle-DX12-20260929-174334`
  and same with `out/agent/captures/combat-lifecycle-Vulkan-20260929-174616`.
  JSON flags, exact event counts, FSR capture state and no validation/script error
  matches pass; all four stderr files empty. Defeat/victory previews (<=1280)
  inspected sequentially for both APIs. This establishes lifecycle behavior and
  endpoint poses/UI, not continuous aiming/locomotion animation quality.
- Two earlier DX12 attempts (173832 and 174115) passed death/pause but failed
  pointer aiming in the wave fixture. Driver fixes preserve pointer position while
  releasing keys, recenter while the menu releases capture, and use explicit
  floating-point Math.Min/Max arguments (PowerShell selected integer overloads
  in the pitch calculation). Final driver above succeeds without changing game
  tuning or bypassing actual hit detection.
- Final owned PIDs 103092/123112/78196/119080 confirmed stopped. Earlier driver
  runs used PIDs 33460/118524/101136/106252, executed finally cleanup and are also
  confirmed stopped. No user process touched.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.
  No new reflected fields, ThirdParty changes, Editor/Shipping or second-project
  validation is claimed.

Next concrete combat work: integrate proper obstacle/path handling. Existing
ScriptCore Navigation.Build/FindPath and native Recast services are available;
current hovering sentries still use center/rim rays and can stop at obstacles
without finding a route around them. Remaining renderer, streaming-performance
and portability requirements retain their full scope. Goal remains active.

## Implementation checkpoint — 2026-09-29 18:13

Previous goal response only answered API compatibility (no implementation progress).
Revalidated the current sources and implemented reusable obstacle/path following:

- Navigation.Build(minimum, maximum) / native NavigationBounds now optionally
  limit a scene bake to a world-space volume. Distant submeshes are rejected by
  transformed bounds; intersecting triangles are clipped against all six planes,
  preserving winding and area flags. NavGround proxies also obey bounds. Invalid
  bounds/geometry preserve the previous usable mesh. Existing unbounded calls
  retain their behavior; no reflection fields or ThirdParty edits were needed.
- New ScriptCore NavigationPathFollower has per-agent routes, target-motion
  tolerance, bounded retry/replan interval, collision invalidation and explicit
  reset. Movement is bounded to one corner even during a hitch; failed queries
  return no movement. Queries can be supplied by another navigation provider.
- External ShooterGame builds the configured courtyard surfaces around player and
  spawn leashes, follows ground routes plus hover height, retains final ray-based
  body clearance and leash checks, and resets paths on wave/finish. Failed builds
  log and stop movement. Config/Navigation.json now includes actual courtyard
  floors/stairs/solid architecture and prop prefixes. Backups of both edited
  external files are under out/agent/combat-navigation/backups.
- External Tools/Build-CombatNavigationDiagnostics.py generates a separate clone
  with a low solid box in sentry 1's pursuit route. Production scene is unchanged.
  Docs/ManagedCombat.md documents the API, usage and remaining limitations.

Evidence:

- **unit-test PASS**:
  `dotnet run --project Tools/Scripting/CombatTests/CombatTests.csproj --configuration Release --artifacts-path C:/WorkSpace/HEngineDev/out/agent/managed-combat`
  -> COMBAT_ACTION_TESTS_PASS checks=57. The 15 added checks cover actual follower
  corner following, no hitch overshoot, reached-corner advancement, arrival,
  target update tolerance, failed/blocked retries, reset, pause and invalid input.
- **unit-test PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name NavigationGeometry -Configuration RelWithDebInfo`
  -> build PASS (100.7s), NavigationGeometry PASS. Added native clipped-floor/wall
  route, all vertex bounds, outside triangle rejection and invalid volume cases.
  The initial `hectl.ps1 test NavigationGeometry` Debug build was deliberately
  cancelled before running the test; it is not reported as a passing test.
- **static-check PASS**:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/Build-CombatNavigationDiagnostics.py`.
- **build-only PASS**:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`
  -> 125.9s. Game SHA256
  EA7DDA1D61A4B8274B73F9AF78DE06C280FA2DCF5D47622A3A1267E6F78E30ED;
  GameScripts SHA256
  1BF61E68BF14CDCFDC5E7172F0DD4B0F73C3789B825B17C6D8A152BBA5063538.
- **runtime-manual PASS**:
  `powershell -ExecutionPolicy Bypass -File out/agent/combat-navigation/Test-Navigation.ps1 -GraphicsApi DX12`
  -> out/agent/captures/combat-navigation-DX12-20260929-180524;
  same command with `-GraphicsApi Vulkan`
  -> out/agent/captures/combat-navigation-Vulkan-20260929-181002.
  Each records 36 position samples over 1080 explicit steps. Both trajectories
  detour to Z=2.95247 around box Z[-.5,2.5], cross to X<-.85 and finish at
  (-2.26360,1.60000,2.34951). No sampled expanded-box overlap. FSR Quality is
  active with no failure at start/detour/arrival. Both logs show bake/path success,
  no matched validation errors or ScriptException, and empty stderr. One DX12
  detour and one Vulkan arrival preview inspected sequentially. This verifies a
  static low obstacle in this scene; it does not prove general crowd navigation,
  dynamic rebakes, full-volume collision or all unreachable layouts.
- First two Vulkan attempts never reached gameplay because the earlier session
  lifecycle/collector lease was still held. They are not graphics failures. The
  final run above started after those handles were terminal and passed. Owned
  Game PIDs 106428/118428 and collector PID 71752 confirmed absent afterward.
- **static-check PASS**: `git -c core.safecrlf=false diff --check`.

Storage: the unnecessary fresh Debug test build reduced free C: space to
1,733,648,384 bytes. Stopped that owned build; the broad `clean stale` collector
was also stopped after it held registration/session leases for several minutes.
Exact `hectl.ps1 clean cache -CachePath out/agent/tests-debug -WhatIf --json`
then the same command without WhatIf validated the boundary/protection/process
checks and deleted 1,239,427,168 logical bytes, no failures. Observed free space
afterward was 2,909,179,904 bytes (+1,175,531,520 net amid build/capture writes).
`clean caches --json` inventory is saved at
out/agent/combat-navigation/cache-inventory.json. The two large remaining caches
are the current Game (33.70GB logical, includes assets/sessions) and current
RelWithDebInfo tests (6.24GB), both needed by ongoing validation. Other project
remnants are session evidence, not intact build caches. No retired cache remains;
package leftovers and android-toolchain were inspected (generated metadata and
historical diagnostic material, no remaining full SDK/build cache identified).
No source, project assets, recovery material or unknown evidence was deleted.

Remaining scope is unchanged: fast-motion/disocclusion rendering quality, special
particle blend coverage, streaming performance/physical VRAM and remaining failure
cases, broader animation/AI quality and second-project portability. Navigation is
a synchronous regional bake, not streamed/tiled navigation or crowd avoidance;
collision rays remain a limited guard. NVRHI patch still awaits explicit permission
and remains unapplied. Goal active; no full-objective completion claim.

## Implementation checkpoint — 2026-09-29 18:30

Previous turn was progress (regional navigation/shared follower and dual-API
obstacle evidence). This turn closes representative special-blend particle input
validation and adds a reusable, project-isolated diagnostic workflow. Rendering
source and the current Game executable did not need changes.

- Tools/VFX/generate_fsr_mask_fixtures.py creates six independent packages with
  the pinned official Effekseer compiler: CPU blend/add/subtract/multiply/distortion
  and a stationary finite GPU add counterpart. Procedural textures, v1 recipes,
  separately declared diagnostic XML overrides, editable sources, official EDIT
  roundtrip checks and compiler/package hashes are preserved. Recipe v1 was not
  silently extended. CPU/GPU counterparts are explicit diagnostic assets; no
  automatic fallback implementation or Low quality acceptance is claimed.
- Tools/Agent/Test-FsrParticles.ps1 uses existing Session/Storage ownership,
  resolves the selected project's executable/cache, pins its hash, and restores
  the temporary GPU development-probe environment in finally. It captures all
  required AOVs plus display PNGs, asserts FSR active/TAA inactive, checks natural
  completion and explicit stop, and preserves native logs. Production GPU policy
  remains unchanged (`gpuValidated=false`).
- Tools/VFX/analyze_fsr_masks.py verifies finite raw data, mask bounds, locally
  stable response values, moved-mask centroids, ended/stopped zero masks, and
  zero-RGB GPU coverage. It deliberately keeps pixel checks separate from visual
  review and emits compact <=1280 px contact sheets. Shader input AOVs are taken
  on separate render frames; locally stable neighborhoods exclude jittered edges.
- Docs/FsrParticleValidation.md describes generation, execution, analysis and
  limits. RenderingPipelines.md links it; acceptance ledger updated for particles
  and the previous turn's navigation evidence.

Evidence:

- **build-only PASS** (effect packages, not engine build):
  `py -3 Tools/VFX/generate_fsr_mask_fixtures.py --project C:/WorkSpace/HEngineProjects/MechanicGirlLab`
  -> officialCompile=PASS, EDITRoundtrip=PASS, effects=6.
  Source/report: external SourceAssets/VFXAuthoring/FsrMaskValidation;
  runtime assets: external Assets/Diagnostics/FsrParticles. Compiler/core hashes
  must equal Tools/VFX/recipes/toolchain.lock.json before generation.
- **runtime-manual PASS**, exact final commands (the directory suffixes are run
  labels; actual wall times are in the native logs):
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrParticles.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -GraphicsApi DX12 -GpuProbe -ExistingBuildSha256 EA7DDA1D61A4B8274B73F9AF78DE06C280FA2DCF5D47622A3A1267E6F78E30ED -OutputDirectory out/agent/captures/fsr-particles-DX12-gpu-20260929-1840`
  and the same command with `-GraphicsApi Vulkan` and
  `-OutputDirectory out/agent/captures/fsr-particles-Vulkan-gpu-20260929-1842`.
  Actual 960x540 output / 640x360 internal. All six effects play, naturally end,
  and explicitly stop. All captured frames use active FSR Quality with standalone
  TAA disabled. Final owned PIDs 119312/41060 confirmed absent; both stderr files
  empty and no validation error/device-loss/ScriptException matches.
- **static-check PASS** on real GPU readbacks:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/VFX/analyze_fsr_masks.py out/agent/captures/fsr-particles-DX12-gpu-20260929-1840 out/agent/captures/fsr-particles-Vulkan-gpu-20260929-1842`
  -> 20 phases PASS per API. Maximum per-phase P95 response error is .005594
  DX12 / .006007 Vulkan, below .02 including R8 quantization and inter-frame
  jitter. CPU moved masks shift at least 138.34 / 137.74 internal pixels.
  Subtract/multiply alpha remains exactly zero while color-difference response
  is nonzero. Natural-ended, empty and stopped alpha/reactive/composition are
  exactly zero. GPU zero-RGB leaves 10229 / 10149 low-contrast covered pixels,
  proving alpha coverage independently of color response.
- GPU stats show enabled=true, a positive lastGpuSimulationSubmission, real GPU
  timing samples and shader output; gpuValidated remains false. Current live GPU
  particle count is unavailable (status 0), not asserted to be zero. This is one
  built-in GPU add fixture, not every GPU shape/material or production certification.
- Both final compact contact sheets inspected sequentially: coverage aligns with
  the visible effects, subtract/multiply respond via color changes and distortion
  follows the striped background sampling. This is input/endpoint visual evidence,
  not a temporal ghosting-quality score. The analyzer's original NOT_REVIEWED field
  is preserved; this paragraph records the separate manual inspection.
- **static-check PASS**: final captured executable/scene/six package hashes match
  current artifacts after regeneration; Python AST parsing, PowerShell AST parsing,
  and `git -c core.safecrlf=false diff --check` pass. No new engine build/unit-test,
  Editor/Shipping run or ThirdParty change is claimed for this asset/tool-only turn.

Earlier evidence and corrections: first DX12 run (directory label 1824) passed
lifecycle but failed the moved-mask assertion because root transforms were changed
while particles remained paused. Driver now resumes for one simulation step before
pausing/readback. CPU-only runs labelled DX12-1830 and Vulkan-1832 passed masks, but
visual review showed overlapping coplanar background boxes (the engine box spans
[-1,1], not [-.5,.5]). Width was corrected before the final dual-API runs above.
Those final results supersede the earlier diagnostic inputs; no runtime workaround
was added to hide either fixture problem.

Storage: final captures reduced C: free space to 1,761,595,392 bytes. Applied NTFS
compression only to the five known stopped diagnostic evidence directories and
their owned native capture directories, after path/reparse/protection/PID checks.
`powershell -ExecutionPolicy Bypass -File out/agent/fsr-integration/Compress-ParticleEvidence.ps1`
preserved every file/path and freed an observed net 964,296,704 bytes; afterward
2,725,892,096 bytes were free. Details: out/agent/fsr-integration/particle-compression.json.
No content, recovery files, user process or persistent project assets were deleted.

Next rendering work: fast character/hair/cloth motion and disocclusion/ghosting
quality. Streaming performance/physical-memory and remaining failure cases,
animation/AI quality, unsupported-view UI and second-project portability remain
open. The permission-dependent NVRHI patch remains unapplied. Full goal active.

## Implementation checkpoint — 2026-09-29 18:54

The preceding conversational turn only confirmed API support (no implementation
progress). Continued the live fast-motion capture handle 23559: it had terminated
with a compression-log sharing violation, and its finally block stopped PID 73512.
The log was inside the wildcard being compressed. Collecting compact output before
writing the log fixes this diagnostic-tool failure; no renderer workaround added.

Added reusable Tools/Agent/Test-FsrMotion.ps1 and Tools/VFX/analyze_fsr_motion.py,
with Docs/FsrMotionValidation.md and RenderingPipelines.md links. Project resolution,
owned Session/Storage lease, pinned executable, strict graphics API, validation,
scene reload, pose signatures and continuous frame-history checks are explicit.
The manifest and executable/managed DLL/scene/manifest hashes are retained. No
MechanicGirlLab path or cache ID is embedded in the reusable engine tools.

External project changes: Scripts/CSharp/FsrMotionDiagnostic.cs and
Tools/Build-FsrMotionDiagnostics.py generate Assets/Diagnostics/FsrMotion with real
body/hair/cloth materials, 115 bone matrices, 8010 cloth particles, animation x2,
parent translation up to 4.4 m/s and rotation up to 4.55 rad/s. Each replay warms
120 simulation steps, resets jitter/history, renders 8 stationary settle frames,
then captures 16 consecutive simulation steps. Five passes compare Quality display,
motion and identity against NativeAA display/identity. Initial/final full palette
and cloth-position hashes match exactly across all five passes and both APIs.

Evidence:

- **build-only PASS**, command:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
  Authoritative result: out/agent/projects/7e44ffe2e91e72bd/logs/build-run-a8efac1008b349888b483d08a53059bd/result.json
  (success, final build log includes C# PASS and executable link).
  First attempt build-run-49e1574cd6354c629fc4ca477d920dca failed with CS1612;
  fixed assigning a property on an Entity struct return by using a local Entity.
  Final exe SHA256 3FC4B7B36C2510F90A2371EAD90D29A299E9CA1675DBCE2008ACF32EBA6EA41B;
  GameScripts 198155A647D858BA12CC833B4BC21316B3560D15EDD9F45B121027DBCD479DFA.
- **runtime-manual PASS**, exact final commands:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrMotion.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -GraphicsApi DX12 -ExistingBuildSha256 3FC4B7B36C2510F90A2371EAD90D29A299E9CA1675DBCE2008ACF32EBA6EA41B -OutputDirectory out/agent/captures/fsr-motion-DX12-final`
  and the same with `-GraphicsApi Vulkan -OutputDirectory out/agent/captures/fsr-motion-Vulkan-final`.
  Both final owned PIDs 10468/115880 stopped and confirmed absent. stderr empty;
  no validation error/device-loss/ScriptException/error log matches. DX12 native
  debug-layer limitation from earlier evidence still applies; not newly certified.
- **static-check PASS on GPU readbacks**, command:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/VFX/analyze_fsr_motion.py out/agent/captures/fsr-motion-DX12-final out/agent/captures/fsr-motion-Vulkan-final`.
  All 80 frame metadata records per API show active FSR, no standalone TAA,
  correct Quality 853x480 or NativeAA 1280x720 input and 1280x720 output, sharpness0.
  Frame-history chains and same-quality jitter match. Per-group footprint distance
  P95: body 1.153 px both, hair 1.339 DX12 / 1.319 Vulkan, cloth .967/.968 px.
  Zero-motion controls P95 17.58/15.85/16.94 px respectively. Paused jitter-corrected
  residual maximum .000185 px both. Pixel/replay checks PASS; visualReview stays
  NOT_REVIEWED in machine output, manual review is separately recorded here.
- Inspected final DX12 and Vulkan compact contact sheets sequentially (frames
  0/5/10/15): visible motion with no obvious large-area trail; differences mainly
  fine hair and silhouette. Disoccluded-background mean RGB absolute difference
  against NativeAA has maximum per-frame .00650; maximum per-frame P95 .05752.
  NativeAA is temporal and uses different jitter, not ground truth. About .9–1.3%
  of matched samples exceed 2px, maximum11.04px; no samples were hidden. Without
  depth-ground-truth occlusion classification these tails cannot all be called
  correct or incorrect motion. Common visible triangles only; no blanket all-pixel
  or all-content temporal quality claim. This sampled clip is ~.267s, not a long
  traversal or gameplay animation test.
- Earlier corrected prototype captures at fsr-fast-motion-DX12-20260929-184216
  and fsr-fast-motion-Vulkan-20260929-184314 also pass the analyzer; final generic
  script runs above are the primary evidence. All four successful runs and the
  first failed run preserve their captures. NTFS compression is non-destructive;
  current C free ~1.96GB. No ThirdParty edits, commits or user processes touched.

Remaining: streaming upload/allocation fault injection and performance/physical
VRAM, broader aiming/locomotion, unsupported-view UI status and second-project
integration. Investigation located the UI issue: LabFrontend treats inactive FSR
without bUpscalingFailed as APPLYING; SceneRenderer supplies only failure reason,
while Prepare intentionally disables unsupported views. No UI/status fix applied
this checkpoint. Permission-dependent NVRHI cleanup patch remains unapplied.
The full objective is still active and not complete.

Final static verification: PowerShell AST for Test-FsrMotion.ps1, Python AST for analyze_fsr_motion.py and the external generator, and git -c core.safecrlf=false diff --check PASS. Both final executable/managed DLL/scene hashes match current files; owned processes absent. No new unit-test, Editor or Shipping run was performed for these diagnostic changes.

## Implementation checkpoint — 2026-09-29 19:00

Previous turn was progress (fast-motion tools, scene, dual-API evidence). This
turn fixes the remaining unsupported-view status ambiguity in actual engine and
frontend code; it does not change the requested FSR eligibility policy.

- Added shared UpscalingStatus with Off/Pending/UnsupportedView/Suspended/Active/
  Failed. SceneUpscalingPass derives it from requested mode, view eligibility,
  output extent, provider failure and active context. SceneRendererStats and
  settings.display publish it, and frame diagnostics expose the same status.
- Clear stale provider reason when the failure latch is reset. Actual latched
  failures keep their reason until explicit mode/quality retry, as before.
- External LabFrontend uses the status for localized text. An ineligible view
  no longer displays APPLYING indefinitely. Returning to a supported composite
  view recreates/activates FSR normally. Runtime status is a shared engine API,
  not a MechanicGirlLab-specific engine branch. RenderResolution.md updated.

Evidence:

- **build-only PASS**, exact command:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`
  -> 88.2s. Log: out/agent/projects/7e44ffe2e91e72bd/logs/build-game-relwithdebinfo-HGame.log.
  Final executable SHA256 1CB6AC5A0FE9677D11D44B15AC4BFED6846B7D4E0C81F1CB5D0EAA2A0E18D2A5;
  GameScripts SHA256 39257F2861DED620E4D5ED4763B6B866F9764CEA29D2F6983DD2048519A3169A.
- **runtime-manual PASS**, exact commands:
  `powershell -ExecutionPolicy Bypass -File out/agent/fsr-integration/Test-UpscalingStatus.ps1 -GraphicsApi DX12`
  and the same with `-GraphicsApi Vulkan`.
  Evidence: out/agent/captures/fsr-status-DX12-20260929-185726 and
  out/agent/captures/fsr-status-Vulkan-20260929-185750. Both run actual Frontend.he,
  retain screenshots, frame metadata, status-row rectangles and logs. Tested
  Active -> UnsupportedView (Rendering.Debug.GI.View=1) -> Active (view=0), then
  injected dispatch Failed -> Off -> Active. Each capture advances 40 simulation
  steps for the menu's refresh interval. FSR active phases exclude standalone TAA;
  unsupported phase is non-failed native-size rendering; non-failed phases have
  no stale failure reason. Source-off mode remains hidden in the menu status row.
- Inspected compact screenshot crops for all six phases on each API sequentially.
  Chinese status text corresponds to actual Active/UnsupportedView/Failed; Off
  shows dimensions alone. No indefinite APPLYING message in unsupported view.
  This is visual/runtime observation, not an automated UI text assertion.
- Final PIDs 6044 and 96212 stopped via finally, then confirmed absent. stderr
  empty both. Logs contain the expected injected-dispatch-failure message; no
  other error/device-lost/exception match. Vulkan has existing shader-interface
  OutputNotConsumed performance warnings; no validation errors. DX12 native
  debug-layer limitation remains, as documented in earlier checks.
- **static-check PASS**: `git -c core.safecrlf=false diff --check` and PowerShell
  AST parsing of the diagnostic driver. No reflected/persisted field changes,
  no separate codegen or new unit-test claimed. No Editor/Shipping runtime test;
  the unsupported case exercised here is the main Game GI diagnostic view.
  Default workspace still Starbloom; no ThirdParty modifications or commits.

Remaining scope: streaming allocation/upload failure and long traversal / physical
VRAM evidence, broader aiming/locomotion animation quality, and isolated second
project integration. Earlier motion-tail caveats remain. NVRHI Vulkan allocation
cleanup patch is still unapplied pending explicit permission. Goal remains active;
this checkpoint closes the unsupported-view messaging item only.

## Implementation checkpoint — 2026-09-29 19:12

Previous goal turn was progress (shared FSR status and dual-backend UI recovery).
This turn fixes a real texture-upload ordering issue and validates recoverable
failure boundaries on both backends. Original scope remains intact.

Production change: TextureManager previously submitted an upload and only then
allocated its completion query. Query allocation failure could leave submitted
work outside manager tracking. Both prepared DDS and image paths now allocate
and reset the query before executeCommandList, reject an invalid zero submission,
and pass the existing query through asynchronous publication or synchronous
RegisterTexture. No additional global idle wait was added. Failed replacements
retain the existing texture/descriptor/revision and use the existing retry path.

Development-only, handle-specific one-shot fault injection can arm a Preparing
slot at TextureAllocation, CommandListCreation, CompletionQueryCreation or
BeforeSubmission. Null resource/query/list returns are simulated at their actual
production boundaries; BeforeSubmission rejects locally after recording. Slot
cancellation/reuse clears the probe. Hooks are absent without HE_DEV_AGENT_RUNTIME
and are not exposed as a production gameplay API or a driver-reset mechanism.

StreamingValidation.cpp now exercises four boundaries x two formats (DDS/TGA)
x initial/replacement = 16 cases per backend. Each asserts stable visible handle,
descriptor index and publication revision; no prepared/pending payload or submitted
byte increase; successful explicit retry on the original handle; release back to
defaults-only manager residency. Empty command-list markers bracket every rejected
upload: consecutive real queue submission IDs independently prove no upload was
submitted, rather than trusting the manager's own byte counters. Ready synchronous
DDS/image loads are covered too. Existing >64MiB pressure/recovery and cancellation
checks remain. Docs/TextureStreaming.md and the acceptance ledger updated.

Evidence:

- **build-only PASS + runtime-manual PASS**, exact command:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineFsrSmoke -Configuration RelWithDebInfo -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -Run -Wait -TimeoutSeconds 180`
  First build118.8s / run PID110436 PASS. After strengthening the test with queue
  markers, final build7.8s / run PID69428 PASS. Final logs:
  out/agent/projects/7e44ffe2e91e72bd/logs/run-game-relwithdebinfo-20260929-190751-777-6184.{stdout,stderr}.log.
  Final smoke SHA256 0603CA4AC9CE2D25DA0263D66D4B28E25122CDBD066B19D4B2D75E5430C378E9.
- Both final reports at
  out/agent/projects/7e44ffe2e91e72bd/sessions/20260929-190751-777-6184/Saved/Diagnostics/FsrSmoke/Streaming/{DX12,Vulkan}/result.json
  show PASS, injectedUploadFailures16, queueGapChecks16, synchronousReadyPaths2,
  expectedFailures18 (16 injected + corrupt reload + missing-file IO), final
  manager payload16 bytes/defaults only. Full source89478512 bytes, pressure
  tail5592432, repaired source22369648. No unexpected failures or global waits.
  Both FSR smoke backends also completed real dispatch/readback/reset/resize and
  invalid-input rejection; stderr empty, no validation error/device-loss matches.
- **build-only PASS + runtime-manual PASS**, exact command:
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game check -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -GraphicsApi Vulkan -StrictGraphicsApi -StartupScene Assets/Scenes/UECloseupAnimated.he -Frames 60`
  Game build86.4s, owned PID19808 reached Ready then completed60 frames and exited0.
  Log: out/agent/projects/7e44ffe2e91e72bd/logs/run-game-relwithdebinfo-20260929-191057-029-22672.stdout.log.
  Ready pendingTextures0; actual material streaming publications recorded; stderr
  empty and no error/validation-error/device-loss/script-exception matches.
  Current Game SHA256 65BCD32869AA9CDA626C02BC8CEC7D546E38778C17D87248056CAB18587364CF.
- All three owned PIDs confirmed absent. **static-check PASS**:
  `git -c core.safecrlf=false diff --check`. No ThirdParty source modifications.
  No new headless unit-test, Editor or Shipping validation claimed. Ordinary
  images and DDS have actual upload/ready evidence, but this smoke does not sample
  their pixels or measure physical VRAM. Native DX12 debug layer remains unavailable;
  existing NVRHI validation and Vulkan validation were used.

Storage: stopped smoke evidence had reduced C free to632074240 bytes. Verified
both exact session paths are within the intended sessions root, all ancestors and
descendants have no reparse/.he-keep protection, and the two owned PIDs are gone.
Applied non-destructive NTFS compression to those two evidence trees only, without
deleting files. Free space became1614086144 (+982011904 bytes). Record:
out/agent/fsr-integration/streaming-failure-compression.json. No source, recovery
material or running/user process touched. Game build thereafter succeeded.

Still open: long traversal and physical process GPU-memory/performance evidence,
remaining animation/aiming/locomotion quality, isolated second-project integration,
and permission-dependent NVRHI Vulkan actual allocation-failure cleanup. Synthetic
failure tests do not certify real driver OOM/device-loss recovery. Windows exposes
GPU Process Memory Dedicated/Shared/Local/Non Local/Total Committed counters for
future measurement; no samples were taken or claimed this turn. Existing short
out/agent/streaming/Test-StreamingTraversal.ps1 is combat/traversal evidence, not a
long physical-memory test. Full objective remains active and incomplete.

## Implementation checkpoint — 2026-09-29 19:39

The immediately preceding conversational turn confirmed API support but did not
advance implementation. This continuation resumed the actual stopped DX12 soak,
fixed its shutdown/evidence handling, and collected complete dual-backend runs.
The full goal remains active; performance and broader animation work are not waived.

Added reusable driver `Tools/Agent/Test-StreamingSoak.ps1`, analyzer
`Tools/Agent/analyze_streaming_soak.py` and `Docs/StreamingSoakValidation.md`.
External MechanicGirlLab adds `Scripts/CSharp/StreamingTraversalDiagnostic.cs`,
`Tools/Build-StreamingTraversal.py` and the generated diagnostic scene. The
3676-entity courtyard keeps geometry/lights/animated character while removing
scripts/sequences; an elevated camera orbits every ten simulation seconds.
This route is a residency diagnostic, not certified gameplay/collision traversal.
Each API runs warmup then 512 -> 128 -> 512 MiB phases, FSR Quality 853x480 ->
1280x720, GI Off, fixed delta1/60, VSync requested; no native validation overhead.
Actual system per-process GPU memory is sampled, separately from payload budgets.

Found and fixed an evidence-lifecycle defect: Session.ps1 previously allowed only
five seconds for graceful exit, then killed its owned process. The first DX12
run reached all four phases but was terminated while writing ~2.65 million CPU
statistics rows: its CPU CSV ends mid-field, no completion marker, exit1. This
run is FAILED_SHUTDOWN, not a pass. Its result and erroneous outer storage state
2a81938b8f144d09aad3ede021596b8b were corrected to failed with evidence preserved.
RenderStats sessions now allow a bounded60s to flush; ordinary sessions retain5s.
The driver records success only after verified process exit and copies logs even
on Stop failure. The analyzer rejects the original failed result as expected.
New DX12 and Vulkan runs both exit0 and have native statistics completion markers.
No native renderer change or additional synchronization was introduced here.

Evidence and exact commands:

- **build-only PASS** (fixture build before the interrupted soak):
  `py -3 C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/Build-StreamingTraversal.py`
  followed by
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
  Build34.1s. Current build log ends with successful C# build and Game link.
  Game SHA256 93829BA01003DB4E2530C4F005C3D15A456BF0603C892F2171395D65A3445DF1;
  GameScripts SHA256 87F6AAC3593C56B8A9180899E5A64F2CA4BD2841AB2930F4C3AC0C70CEBFD537;
  scene SHA256 7FAA3597311AF903CC564B04EEE8DBA43923FBB979240E3AF2F00483ECB6C02D.
  Current continuation changes only tools/docs; this build is fixture/native
  binary provenance, not a claim that tools need C++ compilation.
- **runtime-manual PASS for collection and structural checks**, commands:
  `$hash=(Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash`
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-StreamingSoak.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -GraphicsApi DX12 -ExistingBuildSha256 $hash -OutputDirectory out/agent/captures/streaming-soak-DX12-final-20260929`
  and the same command with `-GraphicsApi Vulkan -OutputDirectory out/agent/captures/streaming-soak-Vulkan-final-20260929`.
  DX12 owned PID9076/session20260929-192747-304-e61483f0b432;
  Vulkan PID116156/session20260929-193238-862-a2858f5b94fb. Both stopped and confirmed
  absent; initial failed PID67224 also absent. No user process was stopped.
- **static-check PASS / runtime-data checks PASS**:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_streaming_soak.py out/agent/captures/streaming-soak-DX12-final-20260929 out/agent/captures/streaming-soak-Vulkan-final-20260929`.
  Complete recorded frames12714/12970, publications7525/7109, telemetry58/57.
  Every endpoint has FSR active, TAA off, zero failed uploads, planned bytes within
  budget and resident/reserved payload within budget+64MiB. Lowering/restoring
  budgets changes payload in the expected direction. Both stderr files empty;
  no matched device-loss, script-exception or upload-failure messages. These
  timing runs did not enable Vulkan validation or the DX12 native debug layer.
- **runtime-manual visual observation**: sequentially inspected both four-row
  compact contact sheets (all phase endpoints). Courtyard/materials remain
  present, no obvious black/missing-resource corruption. The orbit intersects
  foreground foliage at some views; endpoints differ across APIs, so this is
  not a pixel-matched comparison or universal visual-quality certification.
- **static-check PASS**: PowerShell AST parse of Session.ps1 and the soak driver;
  `git -c core.safecrlf=false diff --check`. No reflection/codegen, new engine unit
  test, Editor or Shipping validation required/claimed for this tool change.

Measured phase summaries (warmup omitted; wall P95 and >50ms refer to the selected
CPU frames, excluding first120 phase frames and endpoint captures):

| API / phase | Payload+reserved MiB at endpoint | WDDM dedicated median MiB | CPU wall P95 ms | GPU unique-sample pass-sum P95 ms | Frames >50ms |
|---|---:|---:|---:|---:|---:|
| DX12 baseline | 561.6 | 1526.2 | 25.50 | 8.32 | 0/3604 |
| DX12 pressure | 191.8 | 1155.6 | 28.36 | 8.66 | 19/3610 |
| DX12 recovery | 554.2 | 1540.1 | 25.69 | 8.37 | 1/3582 |
| Vulkan baseline | 568.6 | 1127.0 | 23.56 | 9.34 | 1/3718 |
| Vulkan pressure | 144.8 | 739.5 | 22.14 | 9.34 | 0/3652 |
| Vulkan recovery | 563.2 | 1133.7 | 24.49 | 9.30 | 0/3667 |

DX12 phases20.99/66.31/70.34/66.25 wall seconds; Vulkan21.10/66.49/66.19/66.27.
Hardware: i7-11800H, RTX3060 Laptop6GiB, NVIDIA591.74, Windows11 10.0.26200.
WDDM counters are per-PID and preserve adapter instances; the Vulkan Intel adapter
instance contributes zero, discrete GPU supplies usage. DX12 native process query
medians1532.1/1147.6/1531.2MiB corroborate the budget response. Vulkan currently
inherits RenderBackend's unavailable QueryMemoryBudget; analyzer initially
asserted availability incorrectly, then was corrected to report unavailable
instead of zero usage. Mandatory WDDM evidence remains present on both APIs.
No Vulkan native heap-budget implementation or query result is claimed.

Performance remains qualified: DX12 pressure max85.915ms, recovery max64.010ms;
Vulkan baseline max85.473ms. DX12 pressure frames around7304-7314 show many GPU
passes slowing together (e.g. FSR~4.08ms versus~0.47ms nearby, shadow~15ms versus~2ms)
and frame-slot waits~33-41ms; frame7511 instead spends70.8ms in CPU preparation.
Resource-pump P99 is below0.066ms across both APIs' measured phases, but that does
not establish the cause of the other stalls. No GPU-clock/power/preemption trace
was captured, so neither streaming fault nor external contention is proven.
Retain these hitches for targeted follow-up rather than declaring performance
acceptance. WDDM phase trends alone also do not prove unlimited leak-free behavior.
Frequent transitions during the aggressive orbit remain observable (~7000/run).

After both processes stopped, verified the three exact evidence directories are
within captures with no reparse ancestors/descendants or .he-keep protections.
Applied NTFS compression only, keeping all files. C free858144768 ->1300369408,
net+442224640 bytes. Record: out/agent/fsr-integration/streaming-soak-compression.json.
Default project remains unchanged; no ThirdParty edit, commit or push.

Next: isolate DX12 pressure hitches with GPU clock/power or scheduling evidence,
then finish broader aiming/locomotion transitions and isolated second-project
integration. Permission-dependent NVRHI actual-allocation-failure cleanup remains
unapplied. Continuous budget response is verified; the overall goal is incomplete.

## Implementation checkpoint — 2026-09-29 19:56

Previous goal turn was progress. This turn added hardware-correlated streaming
evidence and fixed a reproducible action-layer defect in the reusable independent
AnimationStateMachine. Main Game visual transition acceptance remains outstanding.

`Test-StreamingSoak.ps1 -NvidiaTelemetry` now runs an owned hidden nvidia-smi
monitor every500ms, terminated through its own process handle after Game shutdown.
It records clocks, P-state, utilization, power, temperature, UUID and raw clock
reason masks; no driver settings are changed. The analyzer reports these per
phase and matches the nearest hardware samples to slow frames, preserving time
distance and GPU-wide scope. Docs/StreamingSoakValidation.md documents usage.

**runtime-manual PASS for collection/structural checks**, exact command:
`$hash=(Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash`
`powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-StreamingSoak.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -GraphicsApi DX12 -ExistingBuildSha256 $hash -NvidiaTelemetry -OutputDirectory out/agent/captures/streaming-soak-DX12-clocks-20260929`
Game SHA93829BA01003DB4E2530C4F005C3D15A456BF0603C892F2171395D65A3445DF1,
same scripts/scene as19:39. Owned PID96228/session20260929-194236-168-be96af2bdfed,
exit0, native statistics completed,12878 frames,7492 publications,58 WDDM samples.
Game and owned monitor confirmed gone; stderr empty. All endpoints retain FSR,
no standalone TAA and zero upload failures. Four compact endpoint images inspected:
no obvious missing-resource/black corruption; foreground foliage intersects some
views, and this is not a pixel-matched quality comparison.

Analysis command:
`C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_streaming_soak.py out/agent/captures/streaming-soak-DX12-clocks-20260929`.
Initial analysis FAIL exposed an incorrect analyzer assumption: recovery payload
577.6155MiB exceeded512+64 by1.6155MiB. EndStreamingFrame's actual admission formula
is budget+64MiB for promotions but max(budget,current residency)+64MiB for demotions,
allowing an over-budget pool to shrink. The existing documentation already says
this is a residency target, not a hard memory cap. Corrected the analyzer to
assert planned payload<=budget and explicitly report observed endpoint overcommit,
including an above-target-plus64 flag. This does not implement or certify a global
scratch cap. The original605673764-byte endpoint remains in raw captures and final
analysis; no production allocator change was made. The historical clock-diagnostic
file is labeled superseded, while analysis.json contains the corrected result.
Docs/TextureStreaming.md now spells out the two admission formulas. Previous
19:39 endpoint checks really passed for those runs, but the claim that64MiB was a
global invariant was incorrect and is superseded here.

Baseline/pressure/recovery CPU wall P95:25.22/25.32/24.89ms; P99:29.30/46.45/28.01ms;
>50ms:0/19/0. WDDM dedicated medians1531.7/1156.3/1527.2MiB. Resource-pump P99<.068ms.
Pressure graphics clocks dip from1987 to832MHz, memory clocks7001->6001MHz,
P-state P0->P3 for nine samples; peak temperature71C. Around19:44:45-49 local,
wall medians~42-50ms occur while GPU pass sums are~6-11ms. Earlier maximum62.05ms
frames already have P0/1987MHz nearby. Thus lowered clocks correlate with part of
the episode, but do not establish the initial cause; they can follow CPU stalls.
No thermal-throttling, scheduling, driver or streaming causality is claimed.
Performance attribution remains open; the monitor does not certify hitch-free play.

Animation production fix: independent AnimationStateMachine resampled both base
clips during crossfade but reapplied MotionBlend/layers only to the destination.
It now evaluates the outgoing state's blend/layers too, using the same action
progress and weights before local-pose blending. The new regression uses different
root rotations with a masked child action whose expected local offset is derived
independently from its shared clock. It varies action progress/weight through the
crossfade, checks repeated zero-delta evaluations, and checks transition completion.
The ECS AnimationSystem deliberately freezes its outgoing pose; it does not have
the same resampling defect. It remains unchanged. Earlier commentary suggesting
both paths lost layers was corrected after inspecting that policy.

**unit-test** exact command, run before and after the fix:
`powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name AnimationPoseContract -Configuration RelWithDebInfo`.
Before: build97.3s PASS, test FAIL with `Outgoing action layer lost its weight or
shared time during crossfade.` Log:
out/agent/logs/test-relwithdebinfo-AnimationPoseContract-20260929-195123-167-123544.stdout.log.
After: build10.7s PASS, test PASS, stderr empty. Log:
out/agent/logs/test-relwithdebinfo-AnimationPoseContract-20260929-195202-080-90416.stdout.log.
Final HEngineTests SHA23D7F969BD40C50E9424AFBC26CE83FDF21731A53797C0388DC4D7EF82E1F421.
This is headless pose-contract evidence, not a new Game/Editor visual check.
The GPU soak used the prior Game binary before this independent-state-machine fix.
No persisted/reflection fields changed; no separate codegen validation required.
**static-check PASS**: driver PowerShell AST parse; `git -c core.safecrlf=false diff --check`.

Verified stopped evidence path, ancestor/descendant reparse and .he-keep protection,
then applied NTFS compression only to the new DX12-clocks evidence tree. C free
1009778688->1193508864, net+183730176 bytes; all files retained. Record:
out/agent/fsr-integration/streaming-clocks-compression.json. No ThirdParty changes,
no commit/push, no default-project switch. Full objective remains active.

Next priority is actual Game aiming/locomotion/reload transition captures and
isolated second-project integration. Hardware-correlated DX12 hitches remain a
qualified performance observation, not an established engine defect. The pending
NVRHI allocation-failure cleanup still needs the earlier explicit permission.

## Implementation checkpoint — 2026-09-29 20:16

Previous goal turn was progress. This turn validates representative aiming,
locomotion and action transitions in the actual SickaDynasty Game on both APIs,
without changing the original scene, script rules, enemies or controller inputs.
The broader goal remains active, chiefly awaiting isolated second-project
integration and resolution of the documented allocation-pressure/fallback issue.

Added `Tools/Agent/Test-CombatAnimation.ps1`, `analyze_combat_animation.py` and
`Docs/CombatAnimationValidation.md`. Real held-key inputs with fixed simulation
steps cover28 samples/API: idle; ADS entry; aimed strafe/forward; sprint entry/exit;
stop; moving fire/reload; switch-cancelled reload; second-weapon fire and completed
reload; holster/draw. The actual F5 game restart separates the two sequences.
Enemies remain active and inflict damage; no immunity or synthetic state parameters
are injected. Game/Agent capture time is paused between samples, so this does not
replace continuous-motion/FSR evidence or certify every interpolated pose.

Development-only `render.animation.status` now exposes current CPU controller
state, transition and bounded numeric/bool parameters beside the last submitted
palette, with explicit timing/source distinction. This is read-only diagnostics,
not persisted fields. During validation, existing limb diagnostics incorrectly
resolved TargetBone constraints as default WorldTarget=(0,0,0), yielding a false
~10m support-hand error. Corrected target precedence to match the solver:
TargetBone -> TargetUUID -> WorldTarget, and added effective controller-scaled
weight. Bone-relative target and effector now use the same submitted palette.
No IK solver, authored offsets, game script or transition policy was changed.

Evidence:

- **build-only PASS**, exact command (incremental runs17.8s,24.7s,28.3s as previous
  state-machine and new diagnostic changes were incorporated):
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab`.
  Final Game SHA2561F03B5A7AD2FEE156950D9D641B9B8ABE757C22AB4AB09E1020C1C4FB7BE2554.
  Final build log: out/agent/projects/7e44ffe2e91e72bd/logs/build-game-relwithdebinfo-HGame.log.
- **runtime-manual PASS** for final DX12 and Vulkan, exact commands:
  `$hash=(Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash`
  `powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-CombatAnimation.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi DX12 -OutputDirectory out/agent/captures/combat-animation-DX12-final-20260929`
  and the same command with `-GraphicsApi Vulkan -OutputDirectory out/agent/captures/combat-animation-Vulkan-final-20260929`.
  Both runs:28 samples, exit0, owned PIDs121604/123896 confirmed absent afterward,
  stderr empty. FSR Quality853x480->1280x720 remains active and standalone TAA off
  at every sample. RenderValidation enabled; existing DX12 native debug-layer
  unavailability and Vulkan shader-interface performance-warning limits remain.
- **runtime-data checks PASS**, exact command:
  `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_combat_animation.py out/agent/captures/combat-animation-DX12-final-20260929 out/agent/captures/combat-animation-Vulkan-final-20260929`.
  Each directory was also analyzed individually with that command. Twelve settled
  state expectations pass; all sampled controller values and palettes finite.
  ADS FOV moves0.95->0.7037489radians after21 steps. Full-weight support-hand
  observations14/API, maximum error0.0001652367m to the configured hand_r-relative
  target, below the diagnostic5cm bound. This measures the configured target,
  not exact fingertip-to-weapon-mesh contact.
- Compared all28 paired samples: state/transition matches28/28, full bone-palette
  hash matches28/28, maximum parameter difference0, maximum player matrix
  difference0. Report: Vulkan-final/backend-comparison.json. This is pose/state
  equality, not a claim that DX12/Vulkan rendered colors are identical.
- **runtime-manual visual review PASS for these representative samples**:
  inspected all four compact pose sheets per API sequentially. ADS framing,
  aimed locomotion, sprint/stop, support-hand release during reload, weapon
  switching, completed reload, holster and draw correspond to reported states.
  No obvious limb explosion, missing weapon or mismatched settled pose seen.
  Actual logs show carbine shot29ammo, cancel retaining29, repeater shot11ammo,
  reload completing12ammo/71reserve, holster then carbine draw. manual-review.json
  records scope and limitations. Continuous ghosting/foot sliding is not inferred
  from paused screenshots; prior deformation-motion tests remain separate.
- Initial DX12 prototype(PID51712, exeSHA12EC121E22C3F70A671E795B695A50CB8B209B75B04998296B2F098D64FF2D6C)
  completed all28 controller checks but its limb diagnostics predate the TargetBone
  correction. Retained at combat-animation-DX12-20260929; it is not grip-error proof.
- **static-check PASS**: PowerShell AST parse of Test-CombatAnimation.ps1 and
  `git -c core.safecrlf=false diff --check`. No new reflection schema/codegen,
  headless unit-test, Editor or Shipping test required/claimed for these diagnostics.

Prototype evidence and its stopped owned session were boundary/reparse/.he-keep
checked, then NTFS compressed with every file retained. C free989425664->1028620288,
net+39194624 bytes; report out/agent/fsr-integration/combat-animation-prototype-compression.json.
After final captures C free fell to599056384 bytes. Read Tools/Agent/Storage.md
explicit-maximization rules and started the mandated `hectl clean stale` step;
cleanup continuation is recorded below when complete. Current source, relevant
validation evidence, user processes and protected/unknown materials must be retained.
No ThirdParty edit, default-project switch, commit or push occurred.

## Implementation checkpoint — 2026-09-29 20:41

The preceding conversational API-support answer was no implementation progress.
This continuation made concrete progress: storage was reclaimed, and the remaining
independent-project integration was implemented and verified on both backends.
The overall goal is still active; the permission-dependent NVRHI Vulkan allocation
failure cleanup and its real pressure/rejection regression remain unresolved.

### Storage continuation

Revalidated the existing collector through live unified session91333 and PID94316
(owned Python storage.py stale, started20:13:52). It remained CPU-active after more
than ten minutes and held the storage mutex; an exact cache preview failed with
`Storage collector/registration is busy`. Explicitly stopped this owned collector
after checking its executable and exact command, to unblock targeted reclamation.
Its session returned exit1; this is NOT reported as successful full-history cleanup.
Log: out/agent/fsr-integration/storage-stale-20260929-2014.log.

Then executed, in order:

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 clean cache -CachePath C:/WorkSpace/HEngineDev/out/agent/tests-relwithdebinfo -WhatIf -Json
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 clean cache -CachePath C:/WorkSpace/HEngineDev/out/agent/tests-relwithdebinfo -Json
```

Both PASS. The cooperative collector verified boundary/protection/evidence/locks
and process ownership. Deleted logical6,240,675,065bytes; measured C free
609,320,960->6,855,118,848, delta+6,245,797,888bytes. Unit-test logs and hashes outside
the cache remain. Original low-space observation was599,056,384bytes; intervening
disk activity explains the different pre-delete snapshot. Physical free-space
change is measured separately from logical deleted size.

Continued inventory of project caches, captures, scripting/tool/dependency caches,
out/package and out/pkg. Main Game build is needed for the pending regression;
its29.3GB sessions include protected .he-keep and current/unknown evidence. Retained
other-work scripting fixtures and source recovery. out/package was empty; out/pkg
contained46,615,151bytes of historical package evidence, not a complete CMake cache.
No retired cache units remained. A proposed deletion of the redundant470,858,939byte
FSR SDK download archive was rejected by automatic approval policy with only
`blocked by policy`; retained it and did not retry through another route. Extracted
verified SDK, DLLs and license remain available. Reports:
out/agent/fsr-integration/cleanup-portability-20260929.json and its referenced
inventory/preview/delete/space files. Ordinary automatic cleanup remains disabled.

### Independent project implementation and evidence

Added `Configure-FsrPortability.py`, `Fixtures/FsrPortability/Portability.cs`,
`Test-FsrPortability.ps1` under Tools/Agent and `Docs/FsrProjectPortability.md`.
Created a fresh Scripted3D project using hectl, then configured only that fixture.
It uses built-in box geometry and its own generated2048-square checker texture;
no engine content is copied into it, and no MechanicGirlLab content is consumed.
Its real script builds a bounded navmesh, uses NavigationPathFollower and
CombatAgent to travel into range, then drives a visible pulse and gameplay commits
from ActionTimeline. This supplements, not replaces, main-game skinned-pose checks.

Exact creation/configure commands:

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 project create -Project C:/WorkSpace/HEngineDev/out/agent/fsr-portability-project -Name FSRPortability -Template Scripted3D -Json
py -3 Tools/Agent/Configure-FsrPortability.py out/agent/fsr-portability-project
```

ProjectId game-e80ff5091790451884355bd996cef252, isolated cachef4c749e731be64fa.
**build-only PASS**, configure11.6s + full build481.1s, exact command:

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineDev/out/agent/fsr-portability-project
```

Logs: out/agent/fsr-integration/portability-build-20260929.log and
out/agent/projects/f4c749e731be64fa/logs/build-game-relwithdebinfo-HGame.log.
Gamee80ff509.exe SHA25677450903DC1AE20DA8DCAC20EB3B4C8DEF457F66CF2BF0124C9712EB99A07066.
Both FSR runtime DLLs and Licenses/FidelityFX.txt verified beside this binary;
CMakeCache HE_PROJECT_DIR points to this independent project. No cross-project
binary/generated-source/session reuse. Workspace default rechecked as Project/
(starbloom-survivor). New fixture script hash matches its source under Tools/Agent.

**runtime-manual PASS**, exact final commands:

```powershell
$hash=(Get-FileHash out/agent/projects/f4c749e731be64fa/game-relwithdebinfo/bin/Gamee80ff509.exe).Hash
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrPortability.ps1 -Project C:/WorkSpace/HEngineDev/out/agent/fsr-portability-project -ExistingBuildSha256 $hash -GraphicsApi DX12 -OutputDirectory out/agent/captures/fsr-portability-DX12-run3-20260929
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrPortability.ps1 -Project C:/WorkSpace/HEngineDev/out/agent/fsr-portability-project -ExistingBuildSha256 $hash -GraphicsApi Vulkan -OutputDirectory out/agent/captures/fsr-portability-Vulkan-20260929
```

Both exit0, result success,11 samples/API, stderr empty, owned PIDs103788/5868
verified absent after finally/STOPPED. Real FSR3.1.4 runtime log and frame metadata
confirm dispatch. Five qualities pass; FSR excludes standalone TAA, Off restores
TAA; Performance resize1280x720->2560x1440->3840x2160->1280x720 passes (4K uses
1920x1080 internal rendering). Texture budget512->1->512MiB produces payload
22,369,620->349,524->22,369,620bytes, transitions1->2->3, failedUploads0 on both.
These are payload/reserved replacement bytes, not physical VRAM measurements.

Both real scripts log navigation ready, Acquire/Pursue/Windup/Recover and exactly
one `PORTABILITY PASS attacks=2 commits=2 completed=2 x=1.967` marker. This proves
shared components execute in another project's managed runtime, rather than only
compiling. Sampled visual review of both1280x600 overview sheets shows the actor
and checker, softer low-budget texture, recovered detail, and no obvious black
frame/missing scene. Each final directory has manual-review.json with limitations.
Consolidated results/asset hashes:
out/agent/fsr-integration/portability-evidence-20260929.json.

Two initial DX12 attempts correctly remain failed: directory
fsr-portability-DX12-20260929(PID72716) used exact floating-point sharpness equality;
fsr-portability-DX12-final-20260929(PID122120) exceeded the120-frame per-call step
limit. Fixed the diagnostic driver to use tolerance and chunked steps. Both owned
processes stopped and were verified absent. No engine fix was needed for those
test-driver mistakes. A report extraction initially assumed UTF-8 runtime logs;
corrected it to extract the ASCII markers from raw bytes, preserving source logs.

**static-check PASS**: PowerShell AST; `git -c core.safecrlf=false diff --check`;
targeted shared-source search found no MechanicGirl/Sicka/absolute workspace/cache
identifiers. Pending NVRHI patch `git apply --check` PASS, still unapplied. Combined
combat-animation analyzer command from the20:16 checkpoint was also actually run
and both APIs PASS (same28 samples and0.0001652367m maximum full-weight target error).
No new headless test, Editor, Shipping or reflection-schema change is claimed.

After new build and captures C free was1,233,575,936bytes. No commit/push or
ThirdParty edit occurred. Explicit permission for the prepared ThirdParty patch
was requested again after independent work completed, citing AGENTS.md:10; no
answer has yet arrived. Do not treat elapsed time as permission or mark complete.


## Implementation checkpoint — 2026-09-29 21:07

Previous goal turn was progress (independent-project integration). This turn
closed the remaining actual Vulkan native4K/HUD comparison and added a CPU/GPU
profiled continuous-streaming run. No production renderer/gameplay code changed;
all runtime commands pinned current Game SHA
1F03B5A7AD2FEE156950D9D641B9B8ABE757C22AB4AB09E1020C1C4FB7BE2554.

### HUD shutdown and pause verification

Promoted the old out-only HUD driver into Tools/Agent/Test-FsrHud.ps1, with explicit
project/cache resolution and outer storage registration. The old helper could
write PASS before shutdown; the new driver records success only after STOPPED,
rejects validation errors and nonempty stderr including shutdown, and preserves
settings/rejection diagnostics in failures. analyze_fsr_hud.py separately records
native2160Compared and rejectionRollbackVerified. A rejected transaction is never
reported as a successful native4K comparison. Docs/FsrHudValidation.md documents
these contracts and commands.

**runtime-manual PASS**, exact commands:

```powershell
$hash=(Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrHud.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi DX12 -OutputDirectory out/agent/captures/fsr-hud-DX12-verified-20260929
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrHud.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi Vulkan -AllowNative4KRejection -OutputDirectory out/agent/captures/fsr-hud-Vulkan-verified-20260929
```

Both exit0, success, shutdownVerified=true, native4KRejected=false; PIDs109624/73300
confirmed absent. Actual native raster dimensions2560x1440 and3840x2160, native
TAA active, Performance FSR1280x720 and1920x1080 respectively with standalone TAA
off. Both logs/stderr clean. Current Vulkan successfully allocates the native4K
candidate; the old failure is not reproduced by this ordinary transition. This
is NOT proof that the unchanged NVRHI early-failure leak is fixed, nor does it
exercise the new rejection branch.

**runtime-data analysis PASS**, exact command:

```powershell
C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_fsr_hud.py out/agent/captures/fsr-hud-DX12-verified-20260929 out/agent/captures/fsr-hud-Vulkan-verified-20260929
```

Each backend:12 HUD regions across1440p/2160p, identical layout and zero changed
opaque glyph/stroke interior pixels (20,439 DX12;20,443 Vulkan). Exact active pass
order FSR31Upscale -> PostProcessComposite -> ToneMapping -> RuntimeUiRender ->
UiComposite -> Present, no SceneTAA. Both1440p and2160p compact previews per API
inspected sequentially; HUD/scene present and no obvious corruption. Each final
directory has manual-review.json. Translucent/antialiased edges are excluded.

Initial current captures in fsr-hud-{DX12,Vulkan}-current-20260929 completed and
stopped (PIDs114072/96356), but an initial analyzer incorrectly required constant
applicationSimulationFrame. Source inspection of SimulationClock::BeginFrame
proved that frame counter also increments on zero-delta render-only frames.
Corrected the driver/analyzer to record and require paused control, no pending
step, identical completedSteps and actual simulationTimeSeconds. Final runs above
include this stronger evidence; no simulation-clock engine change was made. The
final native-dimension assertion was also applied to existing final capture data;
no extra runtime repetition was needed for that data-only check.

### CPU/GPU-correlated streaming profile

Reviewed the prior clocks run's individual slow frames and CPU-pass data.
At frame6034,62.0524ms wall consisted chiefly of19.2773ms update and25.1978ms
frame preparation; resource pump0.0937ms, Present0.4394ms. Individual graph records
were small (largest BudgetedLocalShadows0.6958ms); these nested timings must not be
summed blindly. Saved hitch-pass-audit.json beside that older capture. This does
not identify why its CPU work took longer or establish a GPU-clock cause.

Added optional -CpuTelemetry to Test-StreamingSoak.ps1 and the bounded, hidden,
owned Monitor-CpuTelemetry.ps1. Read-only Windows PDH samples total and logical
processor frequency/performance/utility/limit flags every second. No power policy
or affinity changes. Both monitors are stopped by their owned handles after Game;
cleanup attempts both even if one fails. The analyzer requires phase coverage and
valid counters, preserves nearby raw processor samples and explicitly disclaims
causality from timestamp proximity. Docs/StreamingSoakValidation.md updated.

**runtime-manual PASS**, exact command:

```powershell
$hash=(Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-StreamingSoak.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi DX12 -NvidiaTelemetry -CpuTelemetry -OutputDirectory out/agent/captures/streaming-soak-DX12-cpu-clocks-20260929
```

Exit0, CAPTURED_AND_STOPPED,13,089 frames,7,600 texture publications,58 WDDM samples,
238 complete CPU samples. GamePID79604, NVIDIA monitor103696 and CPU monitor77228
all confirmed absent after shutdown. Same diagnostic scene SHA
7FAA3597311AF903CC564B04EEE8DBA43923FBB979240E3AF2F00483ECB6C02D.
**runtime-data analysis PASS**, exact command:

```powershell
C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_streaming_soak.py out/agent/captures/streaming-soak-DX12-cpu-clocks-20260929
```

Baseline/pressure/recovery observations (initial120 phase frames excluded):

- CPU wall P9524.93/22.38/25.17ms, P9928.07/25.64/28.69ms;
  maximum43.26/31.74/40.94ms, frames>50ms0/0/0.
- Resource-pump P99<=0.06124ms across these phases; pressure maximum0.1263ms.
- WDDM process dedicated medians1532.67/1146.89/1540.68MiB show pressure and recovery.
- CPU total Actual Frequency medians4180.9/4159.5/4141.3MHz; minimum across measured
  phases3832.9MHz. All sampled total Performance Limit Flags0.
- GPU stayedP0; measured-phase graphics clock minimum1980MHz. This run did not
  reproduce the old P3 interval or19 pressure frames>50ms. Warmup had a machine-wide
  utility spike and lower frequency; it is reported separately.

Inspected the960x1200 four-endpoint preview: courtyard/materials present, no obvious
black/missing resource corruption; foreground foliage intersects some endpoints.
manual-review.json records scope. Prior hitches remain unattributed and are NOT
claimed repaired. The bounded profile demonstrates budget response without the
previous slowdown in this run; it does not establish universal hitch-free behavior,
unthrottled API comparisons or actual driver-allocation recovery. An additional
end-of-driver failure exit guard was statically verified after the successful run;
it does not alter this run's already verified normal shutdown path.

### Storage and current blocking boundary

HUD captures reduced C free to449,409,024bytes. Whole-cache WhatIf for completed
independent project f4c749e731be64fa correctly refused its historical sessions.
Following Storage.md's explicit partial-cache rule, checked exact absolute paths,
all ancestors/descendants for reparse/protection/recovery, process absence, cache
lease and root locks. Removed only reproducible .obj/.pch/.lib/.pdb/.ilk/.exp/.res
files under that cache's Engine and deps:1,221 files,4,055,953,908 logical bytes.
C free became4,507,877,376, measured delta4,058,468,352bytes. Retained executable,
runtime DLLs/PDB in bin, source, generated source, CMake configuration, all sessions
and evidence. Report: out/agent/fsr-integration/portability-object-clean-20260929.json;
script: Clean-PortabilityObjects.ps1. The earlier rejected SDK archive deletion was
not retried. After the new soak C free4,206,235,648bytes.

**static-check PASS**: PowerShell AST for all changed drivers/monitor;
`git -c core.safecrlf=false diff --check`; pending NVRHI patch `git apply --check`.
No new engine build, headless unit test, Editor or Shipping run needed/claimed for
these diagnostic-only changes. No reflected schema or ThirdParty edits, commit,
push or workspace-default change.

Blocking audit: the same explicit ThirdParty authorization requirement was present
at20:16,20:41 and this21:07 checkpoint (and earlier). The20:41 asynchronous question
still has no answer. NVRHI createTexture still owns a raw Texture through failing
createImage/allocateTextureMemory returns; the reviewed local RAII patch remains
unapplied. Its destructor already releases a created image and any allocated memory.
Current native4K success does not cover those early returns. Independent project,
HUD and scoped traversal checks are now completed; remaining required failure-path
work depends on permission to edit AGENTS.md:10's protected ThirdParty location.
No meaningful authorized implementation of that concrete fix remains available.
The goal must be marked blocked awaiting that answer, not complete. Historical
performance qualifications remain explicit; no arbitrary extra benchmark target
or universal visual/performance guarantee has been introduced.

## Upstream trial and revised DX12-first scope — 2026-09-29 21:35

The user answered the previous blocker: check GitHub for a newer ThirdParty
version, try it, and defer Vulkan if necessary while prioritizing DX12. Updated
NVRHI from 8e8c36e to official main d0c8e30 (2026-09-23), preserving all three
locally patched source files. Complete upstream delta applied and Game build
passed in172.3s; SHA B79B2F53BDB6097AD8F904E424846359A754944050CD7B9D2310593ADB778E8B.

Both APIs passed native/FSR1440p and2160p switching and HUD comparison with clean
validation/shutdown. DX12 additionally passed the repeatable fast character,
hair and cloth motion suite and sampled visual review. Owned PIDs106916,103796
and38740 stopped. Exact commands, source audit, screenshots, metrics and test
limits are in [NvrhiUpstreamValidation.md](NvrhiUpstreamValidation.md).

The upstream createTexture function and allocator are unchanged: allocation
failure ownership is still defective, and this run did not trigger OOM. The
prepared local failure patch remains unapplied. Per the user's revised scope,
this Vulkan defect is deferred and no longer blocks delivery of the DX12-first
stage. Existing ordinary Vulkan functionality is retained; it is not certified
for allocation-failure recovery. Previous streaming/combat/portability evidence
remains in this ledger with its original build identities and limits; it was not
silently recertified against the dependency update. No Editor/Shipping/unit run
or commit/push was performed for this update.
