# Hardware Ray Tracing and Global Illumination

## Boundary and fallback

Hardware ray tracing is an optional render feature below `SceneRenderer`; it is
not a replacement renderer backend. Graphics, RenderGraph, scene, material, GI,
and shader code use NVRHI only. Native API capability tiers stay in
`Runtime/Platform/DirectX/DX12` and `Runtime/Platform/Vulkan`; shader artifacts
and binding contracts remain backend-neutral above those directories.

`GlobalIlluminationSettings` defaults to `Off`. An unsupported hardware request
resolves explicitly to SSGI only when an SSGI implementation is registered;
otherwise it resolves to `Off`, keeps Raster active, and records the reason.
Neither result is reported as Hardware RTGI. Vulkan devices without the
independently enabled ray-tracing extensions continue on the bounded raster
path; every HLSL shader is compiled and validated as both DXIL and SPIR-V.

All current RenderGraph queue labels execute on the one graphics command list
provided by `RenderDevice`. The RT/GI passes make no asynchronous-compute claim.

## Modules

- `Graphics/RenderGraph`: generation-safe imported acceleration structures,
  build-input buffers, build-write/read dependencies, state transitions,
  culling, lifetime reporting, debug dumps, and stale-handle validation.
- `Graphics/RayTracing/RayTracingScene`: static-opaque BLAS cache, stable
  instance mapping, per-frame TLAS build/update, GPU geometry/material/instance
  tables, emissive-triangle power distribution, deferred resource retirement,
  and build/reuse statistics.
- `Plugins/OfflineReferenceRenderer/Source/OfflineReferenceRendererSession`: deterministic one-sample
  increments into persistent `RGBA32_FLOAT` pre-tonemap accumulation and AOVs.
- `Graphics/GlobalIllumination/RealtimeRayTracedGlobalIllumination`: Raster
  primary visibility with one RT sample per pixel at full or half scale and two
  to four diffuse bounces.
- `Graphics/GlobalIllumination/SceneSurfaceDataPass`: depth,
  normal/roughness, base-color/metalness, emissive, motion, and material ID for
  the supported static opaque subset.
- `Graphics/GlobalIllumination/SvgfDenoiser`: motion/depth/normal/material
  reprojection, history validation and neighborhood clamp, disocclusion history
  fix, luminance moments/variance, multi-iteration edge-aware A-Trous, and
  bilateral half-resolution upsampling.
- `Graphics/GlobalIllumination/GlobalIlluminationCompositePass`: additive RTGI
  composition or Reference/debug replacement before Transparent/VFX/ToneMap.

No local root signature, local SBT binding, GPU virtual address, Vulkan handle,
or DX12 type crosses these modules. Closest-hit shaders use `InstanceID()` and
global space-2 scene tables. The NVRHI shader table contains one global hit
group without per-material local bindings.

## Binding and matrix ABI

All shader artifacts use DXC `-Zpc -HV 2021`, column-major storage, and
`mul(matrix, vector)`:

| Space | Owner |
| --- | --- |
| `space0` | frame/view/path-tracing constants |
| `space1` | bindless material textures |
| `space2` | TLAS and global geometry/material/instance/light tables |
| `space3` | pass-local Surface Data, UAV outputs, histories, and guides |
| `space4` | samplers |

SPIR-V uses the NVRHI register offsets (SRV 0, sampler 128, CBV 256, UAV 384)
for descriptor resources in sets 0-4 and `-fvk-use-dx-layout`. NVRHI
push-constant bindings instead compile to SPIR-V `PushConstant` storage with no
descriptor decoration, while DXIL retains the matching `b0` root-constant ABI.
Contract artifacts check that distinction together with matrix
layout/lowering, front-face, barycentrics, instance custom index, and all
supported RT library stages. No wave-size assumption is present.

## Supported vertical slice

The validated slice is static, opaque, triangle geometry with constant built-in
PBR factors and emissive triangles. Textured BasePBR materials remain excluded
until closest-hit shaders consume the global bindless material texture table.
Reference mode provides primary rays, area-light
next-event estimation, cosine-weighted Lambertian sampling, power-heuristic MIS,
multiple diffuse bounces, Russian roulette, geometric versus shading normals,
scale-aware origin offsets, fixed seed, direct-only mode, bounce ladder, and
persistent linear-HDR accumulation. Reference mode never selects a denoiser,
auto exposure, dynamic resolution, firefly clamp, or hidden energy scale.

Skinned geometry, smooth shading normals, Alpha Mask/Blend, textured materials,
procedural geometry, VFX, and custom material shaders are counted as excluded
and stay on documented Raster paths.
They are never silently inserted into an incorrect BLAS/material model.

Real-time RTGI disables diffuse IBL while active to avoid double counting. It
is composited before transparent geometry, VFX, and tone mapping. Reference
mode replaces the HDR scene and omits Transparent/VFX from its graph.

History resets cover first frame, resize, render scale, explicit camera cuts,
projection or jitter generation, scene reload, topology, material, emissive/light
generation, technique, bounce count, resolution mode, denoiser, sampling, and
explicit reset. Realtime GI keeps history through ordinary object/camera motion
and relies on motion vectors and surface validation. Reference PT instead
fingerprints the full camera/integrator and current instance transforms, so any
change to the integral restarts accumulation at one sample.

## Cornell benchmark

`Tools/Content/Cornell/CornellSceneManifest.v1.json` is the single authored
source. The deterministic generator writes the identity-node, baked-world,
hard-split glTF/bin, PBRT v4 `MeasuredSpectral`, an RGB-authored
`SpectralProxy`, and strict achromatic R/G/B `EngineParityRGB` channel scenes,
plus the HEngine scene, material/edge/ROI masks, metadata, and SHA-256
inventory. A fresh
`HAssetImporter` produces the checked-in `.hmesh`/`.hmat` resources and verifies
`KHR_materials_emissive_strength` behavior and repeatability.

The official Cornell pages describe the data as available for public use but
do not provide an SPDX license or explicit media-redistribution terms. Only
attributed numeric specifications and generated derivatives are included; no
official photo/archive is redistributed. Cornell does not state an SI unit for
the four light-emission samples, so the manifest does not invent one.

`MeasuredSpectral` is the physical spectral reference. The `SpectralProxy`
retains the exact authored RGB inputs but is explicitly not algorithm truth,
because PBRT transports them spectrally. Strict `EngineParityRGB` renders three
achromatic scenes carrying the engine R, G and B transport independently, then
the deterministic Python 3.7 combiner takes R/G/B from the corresponding float
PFMs. It validates dimensions, row orientation, non-finite values, wall
orientation and artifact hashes. This preserves HEngine's channel-wise
Lambertian transport in expectation without per-channel fitting or exposure
tuning.

## Capture, metrics, and current gate status

`TextureReadbackQueue` writes scene-linear float PFM or raw-float plus JSON
metadata without PNG quantization. `Tools/Rendering/hdr_compare.py` computes
global/interior NRMSE, relative L1, luminance/chromaticity, bleeding/leak ROIs,
temporal stability, disocclusion recovery, and heatmaps. HDR-FLIP remains
explicitly unavailable unless an approved external adapter result and policy
are supplied; no substitute is labeled HDR-FLIP.

The DX12 smoke executes real BLAS/TLAS builds, an intentional non-submitted
recording rollback, `dispatchRays`, reference and real-time float readback,
Reference camera/object accumulation invalidation, 120-frame Cornell history,
eight static float frames, all-quadrant half-resolution debug/composite checks,
1080p/1440p performance sampling, and integrated SceneRenderer pass ordering
under NVRHI validation. The local Windows Graphics Tools component is absent,
so the native D3D12 debug layer is reported unavailable while NVRHI validation
remains enabled.

Official PBRT v4 commit `5f7a606806a4ac7b939131ded9d7a30ebd02416e`
(Apache-2.0) was built outside the source tree. All generated scenes parse and
render; strict EngineParityRGB rendered each R/G/B scene at 512x512, 256 spp,
PBRT max depth 9 (matching 8 engine BSDF continuations) and combined float
EXR/PFM outputs without tone mapping.
Current pre-tonemap results are intentionally recorded as failures:

- strict PBRT EngineParityRGB vs HEngine Reference PT: full RGB NRMSE `1.213%`,
  luminance NRMSE `1.179%`, emitter-dominated edge-excluded RGB NRMSE `0.718%`,
  and the primary non-emissive interior RGB/luminance NRMSE
  `7.038%`/`6.615%`. The fully area-light-occluded ROI has zero positive
  systematic mean excess; its per-pixel half-wave diagnostic is `10.614%` and
  is not gated because independent Monte-Carlo noise makes it positively
  biased. This passes the initial Balanced non-emissive NRMSE target but not
  High. HDR-FLIP remains unavailable, so the overall Balanced gate is not
  promoted to PASS.
- Realtime RTGI vs HEngine Reference PT: full RGB NRMSE `16.026%`, luminance
  NRMSE `15.900%`, non-emissive interior RGB/luminance NRMSE
  `55.350%`/`52.477%`, and fully-occluded systematic leak `2.606%`; these fail
  the initial Balanced and leak targets.
- Eight post-history static frames: normalized temporal standard deviation
  `2.652%`, above the initial `1%` target.
- Moving the short block -50 mm on X produces a deterministic 3492-pixel
  before/after first-hit disocclusion ROI. Against a moved 512 spp Reference,
  the 16-frame 1 spp sequence first reaches 10% luminance relative-L1 at frame
  4 and confirms it at frame 6: the Balanced 8-frame recovery gate passes.
  The stricter 5% gate never recovers in 16 frames and fails.
- HDR-FLIP remains unavailable and is not replaced by a differently named
  metric.

GPU timestamps are completed-submission RenderGraph pass-body times on the one
Graphics command list. They exclude the pass's preceding graph barriers,
Present and CPU time. Dirty-only BLAS cold-build events, TLAS build, TLAS
updates, trace, SVGF and composite values are exposed separately; clean BLAS
passes are omitted from rebuilt graphs rather than recorded as zero-time
samples. Memory payload/reserved fields remain descriptor-derived logical
estimates. Separate NVRHI queries report active RT buffer and BLAS/TLAS
allocation requirements; neither category is total VRAM because AS scratch,
Reference/SVGF imported histories, retired resources, pipeline/SBT, descriptor
and driver overhead remain excluded.

Latest Debug + NVRHI-validation samples on an RTX 3060 Laptop GPU, driver
32.0.15.9174:

- two-instance synthetic RTGI, 1920x1080, 1 spp, 2 bounces, 0.5 scale:
  summed pass-body median/P95/P99 `1.631/1.821/2.159 ms`;
- the same workload at 2560x1440: `2.721/2.843/2.879 ms`;
- Cornell Reference PT, 512x512, 256 accumulated spp, 8 bounces:
  `0.926/1.517/1.560 ms`, with Reference trace
  `0.827/1.409/1.446 ms`;
- Cornell realtime RTGI, 512x512, 1 spp, 4 bounces, 0.5 scale, four A-Trous
  iterations: `0.470/0.476/0.568 ms`, with RT trace
  `0.171/0.173/0.177 ms`.

These are quality/correctness measurements, not Shipping frame-time promises.

The RT/GI modules and their DXIL/SPIR-V artifacts are backend-neutral and their
NVRHI push-constant ABI is validated. Raster, material, ImGui, RmlUi and VFX
consumers select artifacts through the active backend. Vulkan ray-tracing
features remain independent gates; a Vulkan raster success is not reported as
hardware-RT success.
