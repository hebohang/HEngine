# Physical lighting migration

Authoring now uses directional illuminance in lux, point/spot intensity in
candela or lumens, and exposure in EV100 plus compensation in stops. Local
lights default to 800 cd. The direction light's 100,000 lx default is this
engine's daylight preset; it is not a claim about another engine's universal
component or template default. PBRTest uses 100,000 lx and EV100 12.

Local lights are converted to candela once during RenderWorld extraction.
Point lumens use 4π steradians; spot lumens use 2π(1−cos(outer half-angle)).
The explicit Unitless selection converts at 625 units per candela. It is an
authored selectable unit, not an implicit old-scene interpretation. Runtime
attenuation is inverse square with a finite radius window. Inspector unit
changes preserve candela; intensities accept finite nonnegative float values.
EV100 is limited to −24..32, compensation to −24..24. HDR emissive material
editing accepts values above one, labelled cd/m².

Persisted `Exposure` and `FalloffExponent` are removed. Camera, environment and
post-process components store EV100/compensation. Internal renderer exposure
remains a linear multiplier. `Migrate-PhotometricLighting.py` is the explicit
one-time asset migration, with backups under `out/agent/photometric-migration-*`;
there is no runtime compatibility decoder. Scenes that inherited the old
exposure have received an explicit environment component. Asset generators and
benchmark manifests were updated as well. Re-running migration is a no-op.

Two numeric fixes accompany physical values: manual exposure smaller than
0.00001 is no longer treated as disabled, and SVGF luminance-square moments use
RG32_FLOAT, avoiding half-precision overflow in daylight.

## Validation, 2026-09-09

| Evidence | Exact command | Result |
|---|---|---|
| unit-test | `hectl.bat test PbrLightingReflection` | PASS |
| unit-test | `hectl.bat test RenderWorldExtractorMeshPath` | PASS |
| unit-test | `hectl.bat test RenderSettings` | PASS |
| unit-test | `hectl.bat test CornellSceneSerialization` | PASS |
| unit-test | `hectl.bat test SceneEditing` | PASS |
| runtime-manual | `& Tools/Agent/Test-PhotometricLighting.ps1 -OutputDirectory out/agent/photometric-contracts` | PASS, seven GPU captures |
| build-only | `hectl.bat editor build` | PASS, final RelWithDebInfo build, `out/agent/photometric-editor-final-build.log` |
| build-only, codegen | `hectl.bat codegen run -CodeGenHost DevGame -Configuration RelWithDebInfo --json` | PASS, changed=0, deleted=0, all 237 manifest-listed output hashes and mtimes unchanged |

`out/agent/photometric-contracts/pixel-report.json` records pixel-identical
point/spot cd/lm/unitless fixtures, a nonblack local-light control, forward vs
deferred agreement (mean absolute normalized RGB error 0.0000632), reciprocal
sun intensity/exposure agreement (maximum 1/255), and visible EV100 20 output.
These are controlled GPU comparisons, not photometer calibration or offline
path-tracing equivalence. Earlier editor LNK1168 was caused by the live editor
holding its executable; final build was performed after saving and closing it.

Current-camera SSGI observations are documented in `SSGI-Static-Validation.md`.
