# HEngine Cornell Box benchmark source

This directory owns the authored source and deterministic generator for the
Cornell Box benchmark. `CornellSceneManifest.v1.json` is the single source of
truth; files under `Project/SourceAssets/CornellBox/Generated` must not be
edited by hand.

## Provenance and license status

The geometry, camera, Lambertian reflectance spectra, light reflectance, and
four light-emission samples are transcribed from Cornell Bowers' official
[Computer Graphics Data](https://bowers.cornell.edu/computer-graphics/data)
page. Cornell describes these specifications and resources as being for public
use, but the page does not provide an SPDX license or explicit redistribution
terms. The light samples also have no SI radiometric unit stated on that page.

Accordingly, this repository includes attributed numeric data and its generated
derivatives only. It does not include Cornell's photographs, EXR/TIFF archives,
MATLAB data, RGBE image, or archived downloads. Do not describe the four
emission samples as W/sr/m^2, and obtain project/legal approval before adding or
redistributing upstream media.

`MeasuredSpectral` preserves Cornell's measured reflectances and the four
emission numbers as piecewise-linear PBRT spectra. Because PBRT samples
360-830 nm while Cornell publishes these values only over 400-700 nm, the
generated scene explicitly holds the nearest measured endpoint over the two
unmeasured tails; this numerical boundary policy is versioned in the manifest.
The glTF and HEngine use the manifest's linear-sRGB material and light values.
RGB is derived without per-channel tuning using the Wyman–Sloan–Shirley CIE
1931 analytic fit, Bradford adaptation for surface reflectance, and the method
recorded in the manifest. PBRT v4 always performs spectral transport: an `rgb`
parameter is upsampled to a spectrum before lighting. Therefore
`CornellBox_EngineParityRGB_SpectralProxy.pbrt` is retained only as an
RGB-authored spectral proxy and is not the strict HEngine algorithm truth.

Strict `EngineParityRGB` instead consists of three deterministic achromatic
PBRT scenes. The R scene sets every material and the light to `(R,R,R)`, the G
scene uses `(G,G,G)`, and the B scene uses `(B,B,B)`. After rendering, the
combiner takes R from the R image, G from the G image, and B from the B image.
This preserves PBRT v4's geometry, visibility, path sampling, Lambertian BSDF,
camera, and MIS implementation while making each transported scalar exactly the
corresponding HEngine RGB channel. `MeasuredSpectral` remains the separate
physical/spectral reference. All scenes are pre-tonemap and use fixed seed 1.
The one EngineParity emissive scale is 18.4, exactly the maximum of Cornell's
four published emission samples; it is not a per-channel/photo fit or an SI-unit
claim.

HEngine `MaxBounces` counts BSDF continuation samples and still evaluates
primary-vertex NEE when set to zero. PBRT v4 checks its `maxdepth` before NEE at
the terminal surface, so the manifest deterministically writes PBRT
`maxdepth = engineMaxDiffuseBounces + 1` (9 for the 8-bounce parity reference).

## Generate and verify

From the repository root:

```powershell
python Tools/Content/Cornell/generate_cornell_scene.py
python Tools/Content/Cornell/generate_cornell_scene.py --check
python -m unittest Tools/Content/Cornell/test_generate_cornell_scene.py
```

The generator validates the exact 0.001 mm-to-m conversion, official camera,
35 mm focal length and 25 x 25 mm film, bounds, material slots, triangle count,
room/block/light winding, exact downward light normal, 0.01365 m^2 light area,
flat per-triangle normals, hard-split vertices, mask coverage, and byte hashes.
It emits:

- an external-buffer glTF with one identity mesh node and baked world vertices;
- PBRT v4 `MeasuredSpectral`, RGB-authored `SpectralProxy`, and three strict
  achromatic `EngineParityRGB_R/G/B` scenes;
- stable material-ID, dilated edge-exclusion, ROI-label PGM masks, per-label
  binary material/ROI PGM masks, ROI metadata, and an exact moving-block
  disocclusion mask;
- SHA-256 values for every generated output.

Manifest and generator hashes canonicalize UTF-8 text line endings to LF, so
the same source has the same identity under Windows or Unix checkout policies.
Generated artifact hashes cover exact on-disk bytes.

## Render and combine strict EngineParityRGB

Render all three generated channel scenes with the same PBRT executable, spp,
thread policy, and output dimensions. The checked-in scenes default to 4096 spp;
`--spp` may be used for explicitly labeled engineering baselines:

```powershell
<pbrt.exe> --spp 4096 --outfile CornellBox_EngineParityRGB_R.exr `
  Project/SourceAssets/CornellBox/Generated/CornellBox_EngineParityRGB_R.pbrt
<pbrt.exe> --spp 4096 --outfile CornellBox_EngineParityRGB_G.exr `
  Project/SourceAssets/CornellBox/Generated/CornellBox_EngineParityRGB_G.pbrt
<pbrt.exe> --spp 4096 --outfile CornellBox_EngineParityRGB_B.exr `
  Project/SourceAssets/CornellBox/Generated/CornellBox_EngineParityRGB_B.pbrt

<imgtool.exe> convert --outfile CornellBox_EngineParityRGB_R.pfm CornellBox_EngineParityRGB_R.exr
<imgtool.exe> convert --outfile CornellBox_EngineParityRGB_G.pfm CornellBox_EngineParityRGB_G.exr
<imgtool.exe> convert --outfile CornellBox_EngineParityRGB_B.pfm CornellBox_EngineParityRGB_B.exr

python Tools/Content/Cornell/combine_engine_parity_rgb.py `
  --r CornellBox_EngineParityRGB_R.pfm `
  --g CornellBox_EngineParityRGB_G.pfm `
  --b CornellBox_EngineParityRGB_B.pfm `
  --output CornellBox_EngineParityRGB_Strict.pfm
```

The Python 3.7-compatible combiner rejects mismatched extents, truncated or
non-finite inputs, channel swaps, horizontal camera mirroring, and incorrect
PFM row orientation. It validates the red/green walls and top light against the
generated masks, writes little-endian float32 PFM, and records input, mask,
manifest, tool, decoded-data, and output SHA-256 values in `OUTPUT.pfm.json`.

The ceiling is split exactly around Cornell's published light-aperture loop, so
the light and ceiling never overlap coplanarly. Cornell labels the floor loops
as holes for discontinuity meshing; they are retained in the manifest but the
generated floor remains the measured full quad. The blocks have no bottom
faces, so this does not introduce a visible or coplanar surface.

ROI labels are surface-aware rather than simple screen rectangles. The red and
green bleeding ROIs contain only visible floor pixels, the stable ROI contains
only back-wall pixels, and the leak ROI requires every ray to a 3x3 sample grid
covering the measured rectangular light (boundary plus center) to hit a block.
All ROIs exclude the one-pixel-dilated material/surface/normal/depth-curvature
edge mask. A separate non-emissive mask unions white, green, and red materials
so the HDR emitter cannot dominate the primary surface-quality gate.

## Moving-block disocclusion contract

The versioned `short_block_negx50mm` scenario moves only the short block by
`[-0.05, 0, 0]` metres. Its moved AABB remains inside the room, with 32 mm of
clearance from the green wall and 25 mm from the tall block AABB. The generated
`CornellBox.disocclusion-short-block-negx50mm.pgm` is not a hand-authored
rectangle: the deterministic software rasterizer compares the official camera's
before/after first-hit surfaces. A pixel is included only when the old first hit
was `short_block`, the moved-scene first hit is `floor`, `back_wall`, or
`tall_block`, and neither the before nor after two-pixel-dilated edge mask marks
it. The accompanying JSON records the transform, exact pixel bounds/count,
surface breakdown, clearances, and runtime recovery settings.

At 512 x 512 the locked mask contains 3492 pixels in top-left bounds
`[250,331,296,470]`. HEngine warms SVGF history before applying the transform,
captures 16 post-move linear-HDR frames, and compares them against a fixed-seed
512-spp, eight-bounce, un-denoised Reference PT render of the moved scene. The
recovery gate is luminance relative L1 at or below 5% for three consecutive
frames, beginning at post-move frame zero, within eight frames. No exposure,
clamp, tone-map, or color adjustment is part of this contract.

## HEngine import boundary

The canonical generator emits the deterministic `.he` scene document with one
identity mesh entity, the measured square-film camera, and fixed exposure/zero
IBL. The `.hmesh` and `.hmat` resources still pass through HEngine's importer
and serializers; they are never synthesized by bypassing those formats.

After building the current `HAssetImporter`, import the deterministic glTF into
an existing directory under `Project/Assets`:

```powershell
<HAssetImporter.exe> `
  (Resolve-Path Project) `
  (Resolve-Path Project/SourceAssets/CornellBox/Generated/CornellBox.gltf) `
  (Resolve-Path Project/Assets/CornellBox)
```

The importer produces the engine mesh/material resources. Copy the generated
`CornellBox.he` beside them as `Project/Assets/CornellBox/CornellBox.he`. Do not
trust arbitrary glTF node transforms: this glTF deliberately has one identity
node and already-baked vertices. Loading/saving the scene through Editor remains
the reflected serializer compatibility check; the generator's scene fields are
covered by the headless contract test.

To behaviorally verify that the compiled Assimp path preserves
`KHR_materials_emissive_strength`, run:

```powershell
python Tools/Content/Cornell/verify_assimp_emissive_import.py `
  --importer <absolute-path-to-fresh-HAssetImporter.exe>
```

The verifier uses a uniquely named temporary directory inside `Project/Assets`
because that is the importer's required output boundary, checks that the light
material is HDR and numerically equals `emissiveFactor * emissiveStrength`, and
removes only that verified temporary directory in a `finally` block.
