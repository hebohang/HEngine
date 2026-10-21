# Character rendering upgrade — 2026-09-22

## Audited call chains

| Capability | Actual implementation and evidence | Status / remaining validation |
|---|---|---|
| Texture color and normals | `SceneRenderer::ResolveMaterialTextures` loads base/emission as sRGB, normal/data as linear; custom property fixups use declared color space. `TextureManager` generates color-aware mip chains, normal variance and coverage-preserving masked mips. `MaterialSurfaceCommon::HE_BuildWorldTbn` orthogonalizes tangent, preserves handedness, flips double-sided normals. | Existing implementation; newly compiled Surface variants reuse it. Visual mip transitions still require runtime checks. |
| HDR/PBR | `RasterSurfaceLighting::ShadeSceneSurface` evaluates clustered lights, irradiance/prefiltered cubemaps and BRDF LUT in linear HDR; `SceneToneMap` owns exposure/ACES/output transfer. | Existing connected path, not a new feature claim. |
| Shadows | `EvaluateDirectionalShadow` blends cascades and fades final cascade; geometric normal bias is independent of material normal maps. Local point/spot shadows are evaluated in the clustered light loop. | Existing; test contact/bias/cascade transitions in representative level. |
| Local reflections | `SceneReflectionPass` validates and uploads up to 8 probes; `SceneReflections::ProbeRadiance` box-projects, blends volume edges and falls back to global environment. SSR confidence blends over that result. | Existing connected implementation. No claim of perfect mirrors or automatic scene probe capture. |
| Dynamic indirect light | Global irradiance cubemap plus optional screen-space/ray-traced GI paths exist. | Extended existing probe lifecycle with local cosine-convolved irradiance replacement (below). No baked lightmap system is claimed. |
| Motion/TAA | `MaterialSurface` computes previous world and bone palette positions. `SceneRenderer::PrepareAntiAliasingFrame` resets on cut, jitter generation or scene change. `SceneTaa::IsHistoryGuideValid` rejects identity/depth/normal disagreement. | Existing skeletal chain; morph/cloth previous positions are integrated by the deformation workstream. Runtime hair/disocclusion checks remain mandatory. |
| Character material models | Previously standard isotropic metal/roughness PBR only on the sample. | Implemented and run through Game below; specific frozen-pose skin/hair regressions pass. Motion stability and final art acceptance remain separate. |

## Implemented shared material models

`Engine/Content/Shaders/Materials/Character/` supplies editable `Skin`, `HairCards`, `Cloth`, `Eye` Surface materials. They use the normal asset/material loading, shader manifests, editor property metadata, shadow/depth variants and Forward/Deferred lifecycle. No project name or character name occurs in engine shading.

`Modern/CharacterLighting.hlsli` supplies shared direct-light responses:

- Skin: normalized RGB wrapped diffuse, controlled by an explicit diffusion mask; sharp dielectric specular remains. This is a **local diffusion approximation**, not profile diffusion, volumetric scattering or measured skin.
- Hair: two shifted strand specular lobes with dielectric Fresnel and modest backlit transmission. Strand direction comes from a user-editable UV angle and the skinned tangent frame. Both normal and strand derivatives broaden roughness to limit specular aliasing. Alpha coverage follows the same deterministic mask in shading/depth/shadow passes and existing coverage-preserving mip generation.
- Cloth: editable fiber sheen with a small base-energy reduction. This is shading, independent of cloth simulation.
- Eye: low-roughness dielectric surface with imported base and normal texture. The asset must provide cornea/iris geometry and correct eyelid fit; the shader cannot supply missing anatomy or make the old model a realistic digital human.

The existing RGBA16F emission/model MRT carries negative model IDs (`-2` skin, `-3` hair, `-4` cloth) and non-positive model payload. Existing lit/unlit (`0`/`1`) and positive emission contracts remain intact; GI and reflection consumers that clamp/detect positive emission do not mistake the payload for light. These three character models intentionally do not support emission. No extra MRT or per-object GPU buffer was added. Forward receives the same fields directly.

## Authoring and sample integration

`Examples/AsterCourtyard/Config/CharacterMaterials.json` is the material profile. `Tools/configure_character_materials.py --project <project> --profile <json>` reapplies shader assignments and parameter overrides after reimport while retaining texture sampling and stable file references. A second character can use the same tool with another profile; the engine does not know these names.

`Tools/create_skin_mask.py <source.glb> <mask.png> --material <name> --joints <joint names>` bakes a diffusion mask from dominant bone weights and UV triangles. Input is read as data, not executed. Pillow is required. This is an authoring starting point that artists should inspect and paint, not a substitute for thickness measurement.

For the existing Sintel asset, 7,234 head triangles produce a 1024² face mask. An inspected overlay (`out/agent/skin-mask-preview.jpg`) confirms clothing remains black. Existing skin, three hair materials, eyelashes, eyes and scarf now reference the shared shaders. This makes engine features observable on the existing sample; **the old asset is not AAA art acceptance evidence**.

## Executed checks

- `build-only PASS`: existing `out/agent/game-relwithdebinfo/bin/Programs/HShaderCompiler/HShaderCompiler.exe --rootPath Engine/Content/Shaders --shader Engine/Content/Shaders/<file> --outputPath out/agent/character-shader-validation --binary-format dxil`, individually for `Materials/Character/{Skin,HairCards,Cloth,Eye}.hlsl`, `Modern/SceneDeferred.hlsl`, and regression `Materials/BuiltInShowcase/ShowcasePBR.hlsl`. Shader manifests report 24/40/24/24/24 material artifacts respectively, plus Deferred VS/PS. Log: `out/agent/character-shader-validation-final.log`. These are actual DXIL compilation/cache validations, not an engine executable build.
- `static-check PASS`: `python Examples/AsterCourtyard/Tools/create_skin_mask.py Examples/AsterCourtyard/SourceAssets/Sintel/Prepared/Aster.glb Examples/AsterCourtyard/Assets/Characters/Aster/skin_diffusion_mask.png` (7,234 triangles), then `python Examples/AsterCourtyard/Tools/configure_character_materials.py` (7 materials).
- `static-check PASS`: scoped `git diff --check` for modified shader files and new authoring tools.
- `runtime-manual PASS`, scoped: integrated Game skin irradiance and rear-hair regression captures below passed after actual builds. These are frozen-pose checks, not motion-stability or AAA acceptance. Editor persistence and performance results are recorded in `engine-upgrade-validation.md`; compilation alone does not establish either.

## Required integrated checks

Capture matched forward/deferred closeups with diffusion 0/1 and strand response 0/1; orbit under a side light and backlight; cross shadow cascades; check eyelashes and hair silhouettes at multiple distances. Use slow and fast rotation plus camera cuts to inspect highlights, ghosting and mask mip coverage. Confirm material parameter edit/save/reopen and profile reapplication. Compare standard PBR showcase and original game to catch model payload or lighting regressions. Record current hardware/resolution/frame times after all deformation work is integrated.

## Local diffuse probe integration

The existing environment cache already creates a true cosine-convolved irradiance cubemap for each probe HDR, in addition to its GGX-prefiltered reflection cubemap. The upgrade passes that existing resource through `SceneReflectionProbe::Irradiance` to `SceneReflectionPass`. Eight irradiance bindings and the global irradiance binding are added to the existing composition pass; no additional scene capture or per-frame convolution is performed.

`SceneReflections::ProbeIrradiance` samples by the shading normal, blends using the existing editable box bounds/BlendDistance/Intensity, and falls back continuously to global irradiance outside the volumes. Composition replaces **only** the corresponding global diffuse IBL term using exactly the base lighting Fresnel/metalness/roughness/AO convention. Active GI replacement disables this correction to avoid duplicate energy. Missing optional irradiance on callers' probe structs is tracked with a mask and leaves diffuse unchanged. Resources participate in graph dependencies, binding retirement, cache identity changes and clear/reset.

`SceneRenderer` prepares GI before probe constants so the diffuse replacement weight reflects the current frame, rather than stale GI state. Existing local reflection probe editing, serialization and HDR preparation serve both specular and diffuse, so static interiors can use an interior HDR probe and dynamic characters receive its lighting. This is spatially blended environment lighting, not geometry-aware visibility, automatic baking or dynamic bounce GI. Probe placement and captures must represent the actual room; overlapping boxes cannot prevent all wall leakage.

`build-only PASS`: `out/agent/game-relwithdebinfo/bin/Programs/HShaderCompiler/HShaderCompiler.exe --rootPath Engine/Content/Shaders --shader Engine/Content/Shaders/Modern/SceneReflections.hlsl --outputPath out/agent/character-shader-validation --binary-format dxil`; log `out/agent/character-probe-shader-validation.log`. Subsequent Game builds and the actual captures below exercised this integration. A visually reviewed crossing of a probe boundary, including wall leakage and transition continuity, is still required; a static image does not validate that transition.

## Performance observation chain audit

Evidence below is a source/call-chain audit, not measured performance results.

| Capability | Actual producer and export | Scope / remaining gap |
|---|---|---|
| CPU frame stages | `Application.cpp` steady-clock frame/update/render/window intervals; `RenderFrameTiming.h` backend acquire/wait/submit/present/GC/setup scopes; frames CSV | Wall-clock CPU intervals, not GPU execution. OS thread/process busy time is separately queried by `WindowsPlatformUtils.cpp`. |
| GPU stages | `SceneRenderer.cpp::GpuRenderGraphInstrumentation` NVRHI timer queries, completion-associated sample IDs; `.passes.csv`; `RenderDevice.cpp` submission markers to `.submissions.csv` | Completed GPU samples can lag CPU frames. Pass sum is distinct from frame span; sample/submission IDs must be retained. Shadow/transparent pass timings cover rendering, not isolated skinning instructions. |
| GPU memory | `DX12RenderBackend::QueryMemoryBudget` calls DXGI QueryVideoMemoryInfo local/nonlocal; `Application.cpp` `.memory.csv` | Availability and processScope are explicit. Budget/current usage are not a sum of texture sizes. |
| Process memory | `WindowsPlatformProcess.cpp` K32GetProcessMemoryInfo; `DevAgentRuntime.cpp` `runtime.memory` command | Real private/working-set snapshots; not automatically joined to frame CSV. Default heap excludes other heaps/driver allocations. Runtime agent is unavailable in Shipping. |
| Upload / residency | `SceneRendererStats` sceneTable/indirect/shadow upload bytes, geometry arena and rendergraph residency; frames CSV | Selected producers only: not all texture, deformed vertex or bone-palette uploads; no total upload-bandwidth claim. |
| Animation / deformation CPU | New owner-thread `AnimationCpuTimings` in `RenderFrameTiming.h`; `AnimationSystem::OnUpdateRuntime`; frame CSV | Enabled only with render stats; reset before each application frame, sums all simulation ticks. `cpuAnimationMs` includes pose/IK/secondary motion and `cpuDeformationMs`; do not add both. Deformation timer wraps actual morph/cloth update including inactive/reset checks. |
| Animation workload | New `animationInstanceUpdates`, `deformationInstanceUpdates`, `deformationVertexUpdates` columns | Update counts, not unique scene populations. Vertex count is active deformed section vertices processed per tick, not upload bytes or GPU skinning vertex count. Zero is valid when no runtime animation ticks occur. Editor animation preview is outside this runtime scope. |
| Resource warmup | `RuntimeProjectResourceWarmup` dispatch and `RuntimeResourceWarmupManager` group progress/failure lifecycle | Actual resource readiness exists, but reviewed paths do not establish a complete shader/PSO prewarm coverage metric or per-resource decode/upload latency histogram. `gpuWarmupCutoffSample` only excludes initial performance samples; it does not prove PSO prewarming. |

The animation additions retain existing Tracy instrumentation, but unlike
`HE_PROFILE` they do not require TRACY_ENABLE. Counters belong to the current
owner thread; a future worker-thread animation scheduler must explicitly merge
worker samples. No timing values or FPS have been inferred from this audit.

`static-check PASS`: scoped `git diff --check` for timing/animation/application.
Main integration subsequently built and captured RenderStats with animation/deformation columns; measured CPU deformation and vertex counts are recorded in `engine-upgrade-validation.md`. This source audit itself did not measure GPU performance. Counter semantics are update work, not resident vertices; revisions to the active-work counter require the final measurement to use the corresponding build. The latest performance verdict must come from the dedicated measurement workstream, including spikes, rather than HUD FPS or these visual sessions.

## Rain closeup investigation

The face-and-dialogue runtime image showed unacceptable mottled skin, while a
contact-route view showed smooth skin. Native body1003 albedo is the correct
face UDIM tile and marked SRGB; its preview is clean. It has no normal texture
and uses the exact neutral RGBA16F normal fallback. Base normal squared lengths
range approximately 0.99999985 to 1.00000022, mean vertex-to-geometric-face normal
dot is 0.978 over 37,902 triangle corners. Face morph normals have localized
deltas; no vertices with stationary target positions had normal deltas above
0.1. Body1001/1002 have no face targets but also appeared mottled in the failing
view. At that stage these checks narrowed the investigation without proving the cause; the controlled runtime isolation below subsequently identified it.

Two independent asset-fidelity defects were corrected: Skin now samples the
linear glTF metallic-roughness texture green channel while keeping dielectric
metalness zero; Rain body's factor is 1, preserving its authored roughness rather
than forcing 0.48. Missing maps remain white, preserving other characters. The
legacy textured Rain_hair multiplier is restored to white instead of multiplying
the already dark albedo by (0.16, 0.09, 0.055). The profile generator retains both
changes after reimport. Runtime stage-isolation captures were then completed as follows.

The controlled runtime stage captures at a paused R-performance pose are now in
`out/agent/rain-shading-debug/`: base color (1), world normal (2) and specular IBL
(8) are smooth; the mottling is reproduced by diffuse IBL (7). Direct light (6)
is almost absent in this shaded gallery and directional shadow (9) is black.
`out/agent/rain-shading-ao/12.jpg` has smooth screen AO; disabling AO in `0.jpg`
retains the mottling. This rules out the candidate face morph/normal/shadow/AO
causes for this observed frame. Both owned sessions were stopped in finally.

`SceneEnvironmentPrecompute.hlsl` irradiance mode now integrates fixed world-space
sphere directions instead of rotating a finite cosine hemisphere sample kernel
with every output normal. Adjacent normals share radiance samples and differ by
continuous clamped cosine weights, eliminating rotated quadrature noise. It uses
1024 directions, source mip selected from each sample's 4*pi/N solid angle and
clamped to the actual cubemap mip count. Sum is multiplied by 4*pi/N, so a
constant environment yields pi*L, preserving the lighting's albedo/pi convention.
GGX reflection and BRDF precomputation are unchanged. This adds startup work only.

- `build-only PASS`: HShaderCompiler DXIL CS for SceneEnvironmentPrecompute;
  `out/agent/rain-irradiance-shader.log`.
- `static-check PASS`: 300 normal directions, 1024 constant-radiance quadrature
  samples; maximum relative error to pi 0.0006674 (rounded up), mean 0.0001597;
  `out/agent/rain-irradiance-quadrature.json`.
- Post-fix runtime visual comparison was completed below. Numerical normalization
  and compilation alone were not used as visual acceptance.

Post-fix `runtime-manual PASS`: `powershell -NoProfile -ExecutionPolicy Bypass
-File out/agent/rain-shading-fixed.ps1` started an owned 1280x720 Game session,
triggered R, advanced 1+80+80 simulation frames, and captured normal rendering
and isolated diffuse IBL at the same paused pose. The build succeeded. Both
`out/agent/rain-shading-fixed/0.jpg` and `7.jpg` show smooth forehead, cheeks,
neck and arms, whereas corresponding pre-fix captures contain strong bands.
AO and shadows remain enabled. This confirms the observed defect was irradiance
quadrature noise and that the shared engine fix resolves it in this scene.
The session stopped in finally (PID 85092). Log: `rain-shading-fixed.log`.

This is visual regression evidence for the reported artifact, not a final AAA
art claim, a motion/performance benchmark, or a claim that all environments are
validated. The stylized character remains visibly stylized; hair silhouette and
portrait lighting still require artistic review. Ignore transient HUD FPS for
performance acceptance. Main integration must rebuild Shipping shader caches as
well as the development cache before its final package validation.

## Hair backlight correction

The contact-route screenshot revealed a pale solid-looking ponytail. Read-only
inspection found the atlas/base color was dark brown, while the direct strand
lobe used a cos-power normalization on a cylindrical sin-power band. At authored
roughness 0.45, the old lobe's sphere integral was 7.302, rather than one; grazing
Fresnel could make its white primary response dominate the base color.

`CharacterLighting.hlsli` now normalizes the symmetric cylindrical band by
`1 / (2*pi*Beta(1/2,n/2+1))`, using a gamma-ratio expansion accurate over both
lobes' exponent range. Primary/secondary shifted highlights remain, as does
base-colored backlight transmission. Both diffuse and transmission use the
remaining `(1-F)` scattering budget; a degenerate antipodal half-vector cannot
produce an arbitrary bright highlight. `HairCards.hlsl` computes authored fibre
direction from the unflipped UV basis, independently of the two-sided normal map,
so the root-to-tip axis does not reverse on backfaces.

- `static-check PASS`: numerical band integration at exponents 0.9..78 has
  maximum absolute error 0.0001792 from unity; `rain-hair-normalization.json`.
- The first full model unit-radiance integration exposed a 1.031625 extreme
  white-fibre result before the transmission budget correction. The corrected
  model's 48 roughness/view cases with 4096 sphere samples each have a maximum
  0.982575 (rounded up); `rain-hair-furnace.json`, reproducible with
  `out/agent/rain-hair-furnace2.py`. This is sampled numerical evidence, not a
  claim that this practical Kajiya-style model equals a full fibre BSDF.
- `build-only PASS`: HairCards 40 DXIL artifacts and Deferred VS/PS after lobe
  normalization (`rain-hair-shader.log`, `rain-hair-deferred-shader.log`). The
  final `(1-F)` transmission adjustment was included in the successful final Game build recorded below.
- Contact360 initial corrected runtime captures show dark brown ponytail with
  direct highlights retained (`out/agent/rain-hair-fixed/0.jpg`, `6.jpg`).
  Final Contact300 comparison was subsequently completed below.

Final `runtime-manual PASS`: `powershell -NoProfile -ExecutionPolicy Bypass
-File out/agent/rain-hair-final.ps1` built and started UpgradeContact.he at
1280x720 with fixed delta 1/60, paused, advanced three 100-frame batches and
captured normal/base/direct/specular IBL views. `rain-hair-final/0.jpg` reproduces
the original horizontal ponytail pose from `upgrade-contact-fixed-preview.jpg`
and shows brown hair rather than the pale block. `rain-hair-final/8.jpg` retains
the environment highlight, so the fix did not disable specular lighting. The
Game build passed and the owned process PID 98700 stopped in finally; full log
`out/agent/rain-hair-final.log`. GPU ownership was returned to main for exclusive
performance validation; no further source/asset changes follow this checkpoint.


## Final visual evidence boundary and remaining observations

This checkpoint updates evidence only; it does not run another build or GPU session.

| Observation | Current evidence | Still missing |
|---|---|---|
| Skin bands and rear hair overbrightness | Matched frozen-pose debug/final captures above identify and resolve the reported defects. | Final-shader frontal and opposing-light portraits; close eye/cornea/eyelid review. |
| Hair/eyelash temporal stability | Shared masked mip, velocity and TAA paths exist. | Ordered moving-frame review of silhouettes, specular shimmer, disocclusion trails and first frames after cuts. A paused capture with render delay is not this test. |
| Probe/indoor-outdoor continuity | Actual captured HDRs and local diffuse/specular probe composition run in the scene. | Same-character boundary-crossing sequence, forward and return; inspect exposure continuity and leakage near the gallery wall. |
| Deformation and locomotion | Main sequence/contact/stress checks validate numeric playback, contact, independent instances and reset behavior. | Numeric stability does not prove appealing animation, cloth silhouette or temporal image stability. |
| Two character material profiles | Rain and original Sintel profiles exist. Two actors in UpgradeCourtyard use the same Rain asset. | Re-run a distinct asset/profile after the final shared shader changes; two Rain instances do not satisfy distinct-character art validation. |
| Forward/Deferred parity | Both shader paths compile and share character lighting. | Matched runtime images with identical lights, pose, exposure and parameter toggles. |
| Authoring | Main Editor save/reopen checks passed; see validation document and its report. | This does not establish that every material edit produces the intended visual result, or final packaged-image parity. |

Rain remains a stylized character. Stylization itself is compatible with the user's
AAA-style target, but the current hair silhouette/detail and closeup presentation
have not passed that target. Passing the two concrete artifact regressions is not
overall art acceptance. Skin uses a local scattering approximation; authored face anatomy,
eye layers, hair density/silhouette and material detail still determine quality.
A separate realistic portrait candidate is a useful next content experiment, not
an automatic substitute for tested animation, hair, cloth or facial performance.

## Small repeatable capture plan — NOT EXECUTED at this checkpoint

Use one owned fixed-60-Hz 1280x720 Game session at a time, normal production
settings and a recorded `render.settings.get` snapshot. Save original PNGs plus
world snapshots, build identity and exact simulation-frame indices. Every
`runtime.step` batch must be <=120. Stop the session in `finally`. These captures
are visual checks, not a performance benchmark; the dedicated performance runner
retains exclusive GPU ownership until it returns it.

1. **Matched static controls:** repeat `out/agent/rain-shading-fixed.ps1`
   (UpgradeCourtyard, R then 1+80+80 ticks) and `rain-hair-final.ps1`
   (UpgradeContact, 3x100 ticks). Keep exposure, AO, shadows and camera unchanged;
   normal/base/direct/specular views provide controls. This confirms the known
   regressions remain fixed in the final build, without asserting motion quality.
2. **Opposing views and rapid camera rotation:** load UpgradeBenchmark with its
   existing deterministic playback. Its orbit starts at simulated 23 seconds and
   rotates at 180 degrees/second until 30 seconds. Advance to just before the
   orbit and use `capture.sequence {prefix:"orbit-entry",frames:16,format:"png",
   cameraCut:false}`; repeat for a 16-frame window one second later (opposing
   camera direction). Inspect all ordered frames, including pre/post onset,
   scalp/eyelash edges and newly revealed background. Preserve actual light and
   actor orientation in snapshots: opposing camera directions alone do not prove
   front/back lighting. Select visible lit-side and rim/backlit views from this
   path; if the gallery occludes the light, add an explicitly saved local portrait
   setup instead of labelling a shadowed image a backlight test. Repeat a short
   window with `cameraCut:true,settleFrames:0` to examine history invalidation.
3. **Probe transition:** load UpgradeContact from reset. Advance in 10-tick
   batches until the hero approaches the east-gallery probe boundary near
   z=-3.5; record the actual transform and start a 16-frame PNG sequence just
   before crossing. Continue adjacent windows through the blend band toward
   z=-4.25, then repeat on the return route. Compare cheek/garment diffuse level
   and specular response against the saved world positions; check for steps,
   transient halos and wall leakage. The route also includes stairs, making
   cloth and foot contact available for visual review. Do not compensate with
   exposure changes between frames.
4. **Distinct profile:** load original Courtyard (Sintel/Aster) separately and
   capture a fixed portrait plus an 8-frame moving window under its saved lights.
   Compare skin, eyes, hair mask and cloth behavior with Rain, retaining each
   profile's own authored textures. This is cross-asset shader regression only,
   not Sintel AAA art approval. The independent Carla portrait research project,
   once imported and visually inspected, can add a realistic-material candidate;
   it cannot be counted as validated before it actually runs.

`capture.sequence` requires paused fixed-delta mode and no queued simulation
steps; it then advances one simulation tick per captured frame. Poll
`session.status`'s `capture` member to completion before issuing new steps. `capture.frame` delay
while paused supplies additional presentation frames only. For suspected ghosting,
repeat the same deterministic path with a raw motion/normal capture and correlate
it to PNG frame indices; do not mistake a camera-cut-only test for a moving camera.
Mark each observation PASS/FAIL with its frame names and visible criterion. Keep
motion, material parity, final art and performance verdicts separate.


## Local realistic portrait research

**Historical portrait-only stage (2026-09-22, before facial/locomotion integration).**
The following captures establish only the capabilities present at that stage.
For current character status and dated evidence, see
[CarlaStudy.md](../Examples/AsterCourtyard/Tools/CarlaStudy.md#current-status-index-2026-09-22)
and [the integration validation record](engine-upgrade-validation.md).

Carla was subsequently imported into ignored `Assets/LocalStudies/Carla`, without
changing production scenes or cook roots. Twelve actual frozen PBR/Skin portraits
(two six-image rounds) were captured with a hash-pinned existing Game binary;
there was no C++ build. See `Examples/AsterCourtyard/Tools/CarlaStudy.md` for the
exact binary, commands, remap rationale and limitations. The initial study accidentally lit the subject from below; its material/light
quality verdict is superseded by the corrected-light study in that document. The
scan provides more realistic facial anatomy than Rain, but solid scanned hair,
eye detail and the facial/gameplay animation not yet tested **at that stage**
prevented using those portraits as AAA character completion. Subsequent studies
have validated limited facial morphs and locomotion; they do not retroactively
expand these static captures. These captures add distinct-asset/static-material evidence;
they do not close temporal, integrated-character or packaged-image acceptance.


## Opaque hair-cap material correction

Native Rain_hair_cap.hmat was still Default/BasePBR with uniform base color and no
albedo/normal textures; the profile matcher omitted hair_cap. The independent
`create_hair_cap_textures.py` derives 2048px cap RGB/normal from the original card
atlas, forcing alpha=255 in every texel. `configure_rain_materials.py` includes
this cap in the existing HairCards strand model with SRGB/BaseColor and
Linear/Normal metadata, flow pi/2 matching UV +V crown-to-rim, normal strength .25,
roughness .5 and RGB factor .25. Texture report records alpha extrema [255,255].
No geometry or engine shader changed; complete coverage is retained.

Actual fixed-build comparisons: `out/agent/hair-cap-before.ps1` / `hair-cap-after.ps1`
(R161 face/eyelid-visible closeup), plus `hair-cap-contact-before.ps1` /
`hair-cap-contact-after.ps1` (Contact300). All use SHA256
848ED0B6C8C8FF00EB8B78BD22B43C81FDC130BAF025CE93F7ED9DC73F3AD195,
1280x720, fixed 1/60, no build, owned finally stop. PNG/JPG results are in matching
folders. No new coverage hole was observed, but visual change is subtle: the cap
is largely covered by outer cards. **The helmet-like silhouette is NOT resolved**
by this shading change. Uniform fitted card placement/outline remains a content
quality limitation; no AAA hair-completion claim follows from the material fix.

## Mouth cyan regression isolation

The same R161 closeup revealed cyan inside the mouth in both before/after cap
images. `out/agent/rain-mouth-debug.ps1` captured normal/baseColor/worldNormal and
separate direct/diffuseIBL/specularIBL at the same paused pose using the same pinned
binary, no build; owned PID85564 stopped in finally. BaseColor `1.jpg` already
shows saturated cyan, establishing that the issue is present before lighting.
CPU inspection by the deformation workstream found mouth COLOR_0=(0,1,1,1) from
cloth-motion authoring. Native teeth use standard BasePBR, whose active modern
SceneOpaque path multiplies base RGB by vertex RGB; gums/tongue use Skin and do
not multiply vertex color. This is an authoring color/cloth-mask semantic defect,
not an IBL hue diagnosis. Repair/verification belongs to that workstream; these
images do not establish the mouth fixed. Do not suppress real vertex colors in
the shared standard renderer to conceal the authoring mistake.


## Author-guided crown/fringe geometry — final frozen candidate

`extract_rain_hair_guides.py` reads the immutable authored Prepared/Rain.blend and
writes versioned `Config/RainHairGuides.json`: ten original connected crown locks
and the independent left strand. The first UV-slice extraction was rejected after
actual rendering showed floating loops (`out/agent/guided-hair-after/0.jpg`).
Final extraction follows connected mesh-edge shortest paths from high root to low
tip, then resamples arc length. Sampled interpolated centerlines lie within
4.102mm of their own source component surface (65 samples per guide;
`hair-guide-cpu/surface-distance.json`). This is not hand-drawn placeholder hair.

Generator changes retain 46 inner coverage cards and alpha-one cap; the outer
layers use 40 guide-following cards and eight left fringe cards. Existing ponytail,
atlas, bone/material structure and white non-cloth color are retained. Consuming
the removed radial cards' original 92 random draws preserves ponytail geometry:
old/new pony vertex SHA256 both
897e6d41875a6f167be45416dbc23e1da57fd88e7f510b4db3a35bff1b1d16f7.
The isolated old-crown comparison in `hair-guide-cpu/validation.json` uses the new
fringe for both variants and must not be mistaken for a full old asset count.
Full prior card budget was 13,536 triangles; final is 10,208 (8,112 vertices,
152 cards), plus unchanged 2,048-triangle cap.

Normal refresh ran with idempotent scarf fit, followed by canonical Prepared/Rain.glb
native reimport and the versioned material profile. Final export validation retains
79 joints, seven clips, six morph names, 801 pinned scarf vertices and neutral
non-cloth colors. Logs: `guided-hair-refresh3.log`, `guided-hair-import3.log`,
`guided-hair-profile3.log`. No engine shader/C++ build was needed.

Fixed SHA848ED0B6C8C8FF00EB8B78BD22B43C81FDC130BAF025CE93F7ED9DC73F3AD195,
1280x720, 60Hz simulation: `guided-hair-final.ps1` captures R161 and then waits
until R641 before starting T, advances 41 ticks and captures 16 consecutive frames.
This avoids T being rejected while R owns the sequence. Eyes close around frames
6-9 and reopen by14-15; no obvious large lid gap or persistent lash trail was
visible in this bounded view (`guided-hair-final/blink-detail.jpg`). Initial T
attempt during R is not valid blink evidence. Separate final orbit16 frames:
`upgrade-motion/20260922-025922`, no obvious scalp hole/large ghost observed.
All owned sessions stopped; last orbit PID90260. Final asset hashes and scoped
verdict: `guided-hair-final/frozen-assets.json`, `visual-review.json`.

**Art target remains unmet.** Floating crown loops were removed and independent
locks are visible, but left fringe still reads as parallel bent strands and crown
detail is sparse. Main and independent review agree this is a frozen functional
candidate, not AAA hair acceptance. Card counts, successful import and valid morph
contracts do not establish aesthetic success. Further silhouette/clump authoring
and all-distance temporal QA remain necessary.

### CPU render graph recording observation (2026-09-22, implementation pending build/runtime validation)

The strict 12,000-frame run exposed one measured CPU wall frame above 33.333 ms (frame 6526: 36.69 ms, graph recording 27.7859 ms). Existing GPU pass queries cannot attribute this CPU recording spike. New optional `IRenderGraphCpuObserver` measures Compile, Realize, Validate, Setup, each pass Barriers and Record, and Trailing. Record includes attachment clears, marker/query commands and the pass callback; Barriers includes state tracking and commit commands. These are CPU wall durations, including scheduling interruption, not GPU execution or CPU busy time. Realize includes resource-pool validation/acquisition; Validate is the separate post-realization validation. Scope gaps, observer overhead and local-container teardown remain unattributed residual relative to the outer graph timer. No attribution to PSO creation or GPU work is inferred.

The explicit `bEnableCpuGraphTiming` flag is enabled only by Application RenderStats, propagated to the main renderer, disabled on previews. With the flag off, the new scopes do not read clocks or allocate. Enabled collectors reuse phase/name storage; names are interned once for CSV output. The current CPU frame and `counterWorldFrame` are recorded together; actual graph submission comes from `NotifyFrameSubmitted`, independent of delayed GPU samples or frame-marker submissions. Unknown submission is 0 with submissionValid=0; unavailable/failed graph records do not become valid zero durations. Existing GPU `.passes.csv` is unchanged.

New `<frames.csv>.cpu-passes.csv` header: `cpuTimingFrameId,worldFrame,graphSubmissionId,submissionValid,phase,executionIndex,passName,milliseconds,completed`. Global rows use executionIndex=4294967295 and an empty name. CPU rows append into pre-reserved compact storage during measurement and are written only at shutdown; no per-row filesystem output occurs during measured frames. Capacity is 256 rows times the bounded exit-frame request, capped at 4,000,000 rows (48-byte POD rows on x64: at most 192,000,000 reserved bytes, plus unique names). Unbounded diagnostics use that cap. Overflow throws explicitly rather than dropping samples or reallocating during measurement. This diagnostic memory and observer/append overhead are not normal gameplay costs and must be reported when comparing runs.

Validation at this checkpoint: targeted `git diff --check` and source-contract inspection only (static-check); no build, runtime result or quantified overhead is claimed. Main agent owns subsequent build and bounded runtime/analyzer validation.
