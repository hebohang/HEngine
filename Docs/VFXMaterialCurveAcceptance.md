# Original material / external curve compatibility fixtures

These fixtures incorporate no legacy sample artwork. Their material source, curve
control points and effect composition are original diagnostic content dedicated to
CC0 1.0; each package contains the declaration and provenance. Existing assets are
unchanged. The official 1.80.6 tool binaries remain under their existing licenses.

`Tools/VFX/generate_material_curve_fixtures.py` creates:

| Package under `Assets/Effects/MaterialCurveValidation/` | Actual authored dependency |
| --- | --- |
| `OriginalMaterial.efkpkg` | `OriginalCyan.efkmat`, unlit material type 128, constant cyan emissive output, no textures/uniforms |
| `OriginalCurve.efkpkg` | `OriginalLine.efkcurve`, location type 4, one CPU sprite follows an external degree-one rational curve at speed 0.02 |

The material is serialized by the public official CLR
`Effekseer.Utils.MaterialInformation.SaveToBytes()` and loaded back with `Load()`.
`Recipes/CreateDiagnosticMaterial.ps1` supplies portable Effekseer `$F1$` / `$F3$`
material source, not backend shader bytecode. Runtime's existing material compiler
must compile it for DX12 or Vulkan; offline effect compilation alone does not prove
that shader compilation succeeds.

The curve follows `Effekseer.CurveLoader.cpp:36-92`: little-endian int32 version 1,
two double4 points `(-1,0.3,0,1)` / `(1,0.3,0,1)`, four double knots `[0,0,1,1]`,
degree 1, step 32, type 0, dimension 3. Total 124 bytes. The generator checks the
exact size and serialized values. `Curve::GetValue` appends a terminal knot and
evaluates weighted B-spline bases. Positive weights and a clamped monotonic knot
sequence avoid a zero denominator over this segment. This is structural readback;
the acceptance driver supplies the actual SDK load/motion evidence. Embedded
FCurves are not a substitute for this external resource slot.

Both effects are compiled by the pinned official 1.80.6 CLI. Official compressed
EDIT readback verifies dependency paths, renderer/location type and curve
parameters. The dependency UTF-16 path must also occur in INFO and BIN_. Source,
roundtrip XML, dependencies, hashes and tool hashes are retained under
`Project/SourceAssets/VFXAuthoring/MaterialCurveValidation/`.

Reproduce fixture generation (no engine build or GPU session):

```powershell
$python = 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
& $python Tools/VFX/generate_material_curve_fixtures.py
```

Actual acceptance, performed serially against a fixed candidate:

```powershell
& $python Tools/VFX/material_curve_acceptance.py --backend DX12 --existing-build-sha256 '<candidate SHA256>' --output out/agent/rendering-evidence/vfx-authoring/material-curve-dx12-01
& $python Tools/VFX/material_curve_acceptance.py --backend Vulkan --existing-build-sha256 '<candidate SHA256>' --output out/agent/rendering-evidence/vfx-authoring/material-curve-vulkan-01
```

The driver uses the neutral Preview scene and `Session` validation collection,
GPU particles disabled, a Storage outer record, private per-run package copies,
and `finally` stop/30-frame retirement/session shutdown. It saves empty, step
1/2/30/60 and stopped PNGs with real SDK state and stats. The curve's authored
one-frame fade-in must initially be playing with zero image signal; visible
output is required from step 2. The constant material does not multiply vertex
alpha, so its initial image must already be visible. A pixel is signal when
any RGB channel differs from the empty background by more than 16/255; at least
50 signal pixels are required. The curve additionally requires more than five
pixels of horizontal centroid motion from step 2 to 60. These thresholds detect
visible output/motion, not correct material appearance or exact curve geometry;
`visualStatus` always remains `REQUIRES_REVIEW`.

For each dependency it deletes only that resource from a private copy, verifies
same-key reload rejection while the paused old instance survives, and verifies
rejection under a fresh cache key. The actual source package and pinned executable
hashes must remain unchanged. Complete run status still depends on Session's
native validation collection.

Coverage deliberately excludes compiled-only and wrong-platform compiled
materials. The repository contains no approved compiled cache fixture and its
official converter directory does not include a Vulkan compiler DLL. No platform
binary is synthesized. SDK `MaterialLoader::Load` tries `.efkmatd` first, then
falls back to `.efkmat`: wrong compiled data **with valid source** must succeed;
wrong compiled data without source must fail. Those cases require a separate
official cache-generation fixture before they can be reported covered.

Preparation evidence: official serialization/compile/readback and Python static
checks only. DX12/Vulkan actual rendering remains `NOT_RUN` until the parent
executes the commands and reviews the retained images.
