# Portable lighting and game quality work

## Objective

Build reusable HEngine lighting/content capabilities and validate them in
MechanicGirlLab. No project names or absolute project paths in runtime code.
Preserve existing unconfigured projects and imported UE lightmaps.

## Implementation sequence

1. Native linear-irradiance lightmaps alongside the existing UE directional format.
2. Reflected baked irradiance volumes, directional SH evaluation, bounded blending,
   deterministic selection and environment fallback for dynamic objects.
3. Project-independent offline scene adapter and Cycles baking (lightmaps,
   irradiance and reflection captures), dependency manifests and stale checks.
4. Reproducible fixture, mathematical/serialization tests, Game/Editor validation.
5. MechanicGirlLab integration and visual/performance comparison.
6. Continue game-quality work against measured problems; animation/combat,
   streaming and advanced dynamic lighting remain separately verifiable stages.

## Acceptance requirements

- No double counting baked diffuse and environment diffuse.
- Changing exposure must not change baked data; outputs are linear scene radiance.
- Missing/invalid data falls back safely; non-finite data never reaches shaders.
- Bake outputs are staged and only published after successful completion.
- Input hashes cover scene, mesh buffers, materials, textures and bake settings.
- Existing scenes without new fields retain their behavior.
- Report limitations honestly: SH volumes are not PRT or an adaptive probe grid;
  native RGB lightmaps do not reproduce UE directional lightmaps.

## Progress

2026-09-29: repository clean at start. Existing lightmap/probe renderer inspected.
Blender 5.2.1 LTS is installed locally; reuse Cycles as an offline solver rather
than adding a second path tracer to the engine. Runtime remains independent of
Blender and UE.

Implemented: reflected SH volumes (16/view), native RGB E/pi lightmap decoder,
RGBA16F DDS full-mip loading, separate reflection diffuse ownership, offline
Cycles scene adapter, derivative UV1 generation, budget/stale checks, guarded
publication and reusable command/validation entry points.

Evidence: Python math/container/publication tests PASS (11); actual RGB-axis Cycles
fixture maximum irradiance error 0.012402; RenderWorldSubmission and DdsTextureData
headless tests PASS. Native fixture source/baked runtime pair PASS at
MechanicGirlLab Saved/Diagnostics/Lighting/20260929-122233-916.
Initial fixture run exposed missing HDR DDS support (fixed); a subsequent capture
reused a protected world snapshot filename (failed); final comparison uses unique
directories and verifies texture upload logs, not merely process exit.

Completed: final scene comparison/replay, Editor loading and authoring controls,
default frontend integration, Forward smoke capture, independent-project offline
bake, cached no-op and consolidated project validation document:
`MechanicGirlLab/Validation/lighting-production-20260929.md`.
The final paired replay is `Saved/Diagnostics/Lighting/20260929-125217-703`.
Default frontend transition was checked through 5000 frames and 3/3 extracted
irradiance/reflection components. Native fixture output hashes remained identical
after material/coordinator fixes, preserving its earlier visual evidence.

Broader game animation/combat/streaming work is not implemented and must not be
described as complete. Current volumes are explicitly a bounded, uniform-volume
implementation, not adaptive volumetric lightmaps or a streaming solution.
See `Tools/Lighting/README.md` for supported representations and authoring limits.
