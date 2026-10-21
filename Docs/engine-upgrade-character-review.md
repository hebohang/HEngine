# Character integration review — 2026-09-22

## Fixed defects

1. **Legacy constraint opt-out retained an external palette.** AnimationSystem
   previously cleared the override only if secondary-bone state existed. A
   character using only feet/look/limb constraints could retain a stale external
   palette after opt-out. The legacy branch now clears any external override.
2. **Zero-weight foot constraints still displaced the pelvis.** Disabled and
   malformed foot settings now skip physics and pose evaluation. Pelvis correction
   uses the contact weight. Negative MaxCorrection, non-finite weights, world or
   bone matrices, look axes and bind inverses are rejected before palette mutation.
3. **Tiny cloth timestep and unsafe spatial-grid conversion.** `dt < 1e-6` freezes
   state before XPBD computes compliance over `h²`. Self-collision hashes positions
   relative to the actor origin, performs subtraction/division in double and checks
   finiteness plus integer bounds before conversion, leaving room for neighbor offsets.
4. **Render seams tore the cloth simulation.** The first runtime image showed
   disconnected burgundy fragments around the character's feet. Native section 9
   contained 1,900 render vertices but only 525 bind positions. A new per-entity
   `ClothRenderState` caches a bind-position weld at 1e-5 units. Each frame aggregates
   skinned rest positions and minimum allowed motion onto physical particles,
   simulates the welded topology, and maps results back to render vertices. UVs and
   per-render-vertex normal/tangent reconstruction remain intact. Cache state follows
   component reset/stop/LOD lifecycle; no global pointer cache is used.
5. **The sample had no actual pin weights.** The original source GLB itself exported
   COLOR_0 as entirely white, even though the authoring data included another color
   layer. The main authoring task corrected the active render color layer and
   reimported. Readback now confirms **525 vertices, 525 unique bind positions,
   42 pinned vertices, COLOR_0.r range 0..1**. Engine welding remains necessary for
   other assets with valid UV or shading seams.

## Added regression coverage

`AnimationPoseTests.cpp` now covers zero-weight/invalid foot constraints, NaN look
parameters, invalid source bone matrices, subnormal cloth delta, distant world
coordinates, unrepresentable local grid cells, render-seam welding, preservation
of shared source mesh data and prior local positions for temporal reprojection.

Scoped `git diff --check` passed. These new tests require the main integration run
of `hectl test AnimationPoseContract`; a previous test pass predates these changes.
The original cloth image is a failure observation, not visual acceptance. Runtime
cloth shape, pause/teleport behavior and performance must be inspected again after
the fixes.

## Remaining scope limits

Distance opt-out clears solver state and preserves the last local deformed shape;
reactivation resets simulation to the current animated rest shape. This is bounded
but can visibly settle again. Particle self-collision is not full continuous
vertex/triangle cloth collision. Coincident bind-space layers within one material
are intentionally welded; layered garments that must remain separate should use
separate sections/materials or physically separated geometry.

## Python regression fixture correction

`PythonRuntimeContract` used the selected default game, but its catalog and observer
source belong to `Examples/RenderingTests`. The test now explicitly resolves that
fixture from the configured engine content root and requires both input files.
No runtime backend behavior or existing assertions were weakened.

- `unit-test`: `.\hectl.bat test PythonRuntime` PASS;
  `out/agent/upgrade-python-fixture-test.log`.
- `unit-test`: `.\hectl.bat test PhysicsScriptContact` PASS;
  `out/agent/upgrade-physics-contact-test.log`.
- Both commands incrementally built HEngineTests successfully with VS2026 v145.
