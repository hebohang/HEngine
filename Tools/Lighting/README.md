# Offline lighting

`hectl lighting bake -Project <project>` reads `Config/LightingBake.json`.
`hectl lighting check -Project <project>` checks source, output and tool hashes.
Use `-Source <project-relative-config>` for another bake and `-BlenderExe <path>`
to select Blender. Python 3.10+ runs the coordinator (stdlib only); set
`HE_LIGHTING_PYTHON` when the system Python is older. Blender supplies NumPy.
The validated solver is Cycles in Blender 5.2.1. Runtime needs neither Blender
nor Unreal Engine. No source scene, imported mesh or source material is overwritten.

## Configuration

```json
{
  "version": 1,
  "scene": "Assets/Scenes/MyLevel.he",
  "output": "Assets/Generated/MyLighting",
  "samples": 64,
  "probeResolution": 64,
  "bounces": 4,
  "device": "CPU",
  "probes": [
    {"id":"room", "position":[0,1.5,0],
     "boundsMin":[-4,0,-4], "boundsMax":[4,4,4], "blendDistance":0.5}
  ],
  "lightmaps": [
    {"entity":"entity-uuid", "resolution":256, "padding":8, "generateUV1":true}
  ]
}
```

`CPU` is portable; `OPTIX` requests a supported NVIDIA device and fails if absent.
The output `Scene.he` is a derivative scene containing the original game entities
plus baked bindings. Select it with `hectl game capture -StartupScene ...` or a
project's normal scene configuration. The source remains the authoring scene.
Unchanged dependencies reuse the completed bake without starting Cycles. The
Python entry point accepts `--force` when a fresh solver run is needed. Changed
dependencies currently invalidate the complete bake set, not individual tiles.
The fixed seed aids comparisons; GPU solver reruns are not guaranteed to be
byte-identical. Cached generations are verified by their exact output hashes.
Rebake after editing it; `check` reports stale inputs. An active `.bake.lock`
prevents concurrent publication. Failed/timeout stages retain an on-disk
log and do not replace the last completed output. After an abnormal coordinator
termination, verify its PID has exited before removing its lock. Previous
generations are retained as `.previous-*` recovery directories, not auto-deleted.
Directory-swap exceptions restore the previous output. If the entire coordinator
is killed between the two renames, the `.previous-*` directory is the recovery
copy; verify the lock owner has exited before restoring it.

## Representations and energy contract

- Native lightmaps are full-mip RGBA16F DDS, encoding 1, linear **E/pi** without
  receiver albedo. Cycles diffuse indirect plus a separate direct environment
  bake excludes analytic light direct illumination, which remains realtime.
- Existing encoding 0 UE directional atlases retain their original decoder.
- A baked irradiance volume stores nine cosine-convolved RGB SH coefficients
  in world coordinates. The basis order is `1,y,z,x,xy,yz,3z²-1,xz,x²-y²`.
  It replaces environment diffuse inside its world AABB, fading at the boundary;
  overlapping volumes blend normalized values instead of adding energy.
- Baked reflection captures have `bAffectDiffuse=false`, because the SH volume
  owns diffuse. Existing reflection components default to true for compatibility.
- Neither camera exposure, tone mapping, fog nor color grading enters baking.
- Realtime GI retains its existing ambient replacement policy. Do not assume
  enabling SSGI adds to this baked baseline; validate that mode independently.

## Authoring and limitations

These are authored uniform SH volumes, **not an adaptive probe grid or PRT**.
Place a capture in free space, keep its box within one connected lighting region,
and use overlapping boxes for transitions. The first implementation selects at
most 16 volumes per view, nearest bounds then stable entity ID. Reflection has
its separate existing eight-probe budget. Bounds are explicitly world-space;
editing the entity transform does not rotate the baked coefficients. Rebuild
lighting after moving geometry/lights, including doors that change occlusion.

The native RGB lightmap is nondirectional, so a changed normal map cannot
reorient its baked response. UV1 is either validated at bake texel resolution
or generated on a derivative mesh. Generated UVs split vertices and remove old
index LODs; regenerate LODs for that derivative if needed. UV0 and other vertex
attributes remain attached to their original triangle corners. Charts need
adequate padding for the selected resolution/mips. Texel-scale validation cannot
prove the absence of subpixel overlaps. Native lightmaps currently share the
existing DX12 heap-direct/deferred implementation; other backends fall back to
ordinary lighting. SH volumes use the common Forward/Deferred surface lighting.
`maxLightmapBytes` defaults to 256 MiB of estimated RGBA16F mip storage, and
`maxOutputBytes` defaults to 512 MiB of published assets. These are offline
budgets, not a runtime texture streaming implementation. Split large regions
or choose lower lightmap resolutions when a budget is exceeded.

The scene adapter handles static triangle `.hmesh` data, parent transforms,
common PBR color/normal/emission, packed metallic/roughness plus scalar-map
overrides, affine UV0/UV1 transforms and cutout alpha, environment, sun/point/spot
lights. Animated characters and bone-attached meshes are excluded. Explicit
`excludeEntities` and `excludeTagPrefixes` handle other moving content.
Arbitrary shader graphs, thin transmission and local-light range/spot falloff
are not exactly equivalent to Cycles. The default refuses known approximations;
`allowMaterialApproximation:true` explicitly permits them and records every
category in the manifest. Treat these outputs as preview-quality until material
and lighting comparisons pass. An external solver is not proof of engine parity.
Material AO is not multiplied into path-traced bounce reflectance; geometry
occlusion is solved by Cycles. Normal-map tangent generation and BSDFs may still
differ between renderers. Mixed/mirrored wrap filtering at seams is approximate.

The Editor's Add Component / Light menu exposes Baked Irradiance Volume and
Reflection Probe. Inspect enabled state, bounds, fade and intensity; drag a
project `.hdr` onto a reflection probe. SH coefficients are solver output, not
hand-painted controls. Edits to a generated scene will be replaced on rebake;
make lasting authoring changes in the source scene and bake configuration.

## Verification

`python -m unittest discover -s Tools/Lighting -p 'test_*.py'` needs NumPy.
`python Tools/Lighting/make_fixture.py --project <project>` creates a small
colored-room fixture under `Assets/Generated/LightingFixture`; bake its
`Bake.json` with `-Source Assets/Generated/LightingFixture/Bake.json`.
Use both baked and source scene captures, inspect runtime texture/shader errors,
and test a moving character through volume boundaries. A process exit code alone
does not demonstrate that its lightmap uploaded successfully.

`Tools/Lighting/Validate-Lighting.ps1 -Project <project> -Config <config>` creates
unique source/baked captures, statistics and a validation report, checks renderer
volume counts, and rejects asynchronous texture-upload errors. Its timings are
not controlled benchmarks while other builds or GPU jobs are active.
`python Tools/Lighting/analyze_comparison.py <evidence-directory>` summarizes
settled median/p95 CPU and GPU timings and verifies the capture camera/frame match.

For the analytic coordinate test, bake the generated `DirectionBake.json`, then
run `python Tools/Lighting/verify_direction_fixture.py --project <project>`.
This compares actual Cycles captures against the known cosine integral of an
RGB-axis environment and catches camera handedness and sky mapping errors.
