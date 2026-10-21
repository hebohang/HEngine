# CPU soft/distortion/model control experiment

New scene `Assets/Scenes/VFXCpuPathValidation.he` copies the existing preview's
camera/light/exposure and Cube mesh/material. The camera is moved to (0,2.5,-4.15)
with the original viewing rotation; the 2-unit stationary particle should occupy
hundreds rather than tens of pixels. The thin ground remains below the origin,
intersecting the lower part of the billboard. Five narrow background cubes at
z=2 provide edges behind it. All use existing Cube.hmesh/Cube.hmat and its existing
512x512 base-color texture (19,332 distinct RGB colors); no new material/model or
script dependency is introduced. Visibility is a planned setup, pending capture.

Run these commands serially with the fixed candidate hash:

```powershell
$python='C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
& $python Tools/VFX/cpu_path_acceptance.py --backend DX12 --existing-build-sha256 '<candidate SHA256>' --output out/agent/rendering-evidence/vfx-authoring/cpu-path-dx12-01
& $python Tools/VFX/cpu_path_acceptance.py --backend Vulkan --existing-build-sha256 '<candidate SHA256>' --output out/agent/rendering-evidence/vfx-authoring/cpu-path-vulkan-01
```

Session enables RenderValidation and RenderStats, pins the executable, uses this
scene at 1280x720, pauses global simulation and advances fixed steps. GPU particles
are explicitly disabled: this is CPU particle rendering through DX12/Vulkan.
Each fixture is played separately at the origin, seed 1806, and captured at effect
frames 1,30,90 with its own empty background and stopped image. Finally stop any
remaining handles, advance 30 retirement frames, verify no active CPU instances,
and stop the owned session before completing outer Storage. No existing scene,
fixture or parameter layout was changed for this experiment.

The driver verifies source control/soft equality after removing only their names
and soft settings. Fixed color, opacity, fade-in/out, scale, sprite texture,
position, life and emission parameters are otherwise identical. Official-compiled
provenance is included in the report: soft far=.375, near=.0625, nearOffset=.03125;
the unique triple is also verified in the compiled BIN by the fixture generator.
The camera is far beyond the near-soft range: look for localized ground-contact
fading, not uniform attenuation. An image difference alone cannot establish this.

Each frame records actual SDK playing/paused/speed/position/intensity where exposed,
and vfx.stats. The public SDK status has no per-handle simulation-frame getter or
soft-distance readback: exact elapsed steps are commanded, and compiled distances
are provenance, not falsely labeled runtime readback. At frame 30 the driver saves
paged immutable graph diagnostics and requires recorded, nonculled VFXTransparent;
soft additionally needs VFXDepthSnapshot, distortion needs VFXBackgroundCopy.
Native draw totals must increase separately for each fixture.

Pixel reports contain full-image RGB mean absolute difference, changed-pixel count
(max channel difference >12/255), and changed bbox. They cover empty-to-effect,
effect-to-stopped and matched-time soft/control images. These are diagnostic
measurements, **not automatic visual acceptance**. `commandStatus=PASS` can coexist
with `visualStatus=REQUIRES_REVIEW`, which is always retained until external review.

Review both backends for: contact-localized soft fade versus ordinary control;
actual background-edge warping versus sprite blending; textured arrow model
silhouette and occlusion; matching empty/stopped background; no validation errors.
Reject temporal rendering, shadow drift or background changes as confounds rather
than interpreting them as soft/distortion proof. Resize, global overdraw and
performance are outside this test. This preparation ran only import/help and
source-equivalence checks; no engine build or GPU session has been run.
