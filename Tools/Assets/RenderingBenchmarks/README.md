# Intel Sponza scene import

`import_gltf_scene.py` converts the official static glTF source through the existing compiled `HAssetImporter`. It keeps each shared mesh in local space and preserves all original nodes and parent references. It does not import the whole scene as one mesh.

From the repository root:

```powershell
python Tools/Assets/RenderingBenchmarks/import_gltf_scene.py prepare --texture-size 2048
python Tools/Assets/RenderingBenchmarks/import_gltf_scene.py import --importer C:/WorkSpace/HEngineDev/out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe
python Tools/Assets/RenderingBenchmarks/import_gltf_scene.py verify
python Tools/Assets/RenderingBenchmarks/verify_importer_material_factors.py --importer C:/WorkSpace/HEngineDev/out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe
```

Use a freshly built importer for final acceptance; the tool records the executable SHA-256 and every invocation in `import_progress.json`. Builds and rendering sessions belong to the repository's `hectl` validation workflow and are not launched by this converter.

Defaults:

- Source: `Examples/RenderingTests/SourceAssets/RenderingBenchmarks/IntelSponza/extracted/main_sponza/NewSponza_Main_glTF_003.gltf`.
- Prepared intermediates: `Examples/RenderingTests/SourceAssets/RenderingBenchmarks/IntelSponza/prepared/textures2048`.
- Engine assets: `Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048`.
- Scene: `IntelSponza.he`.
- Lit benchmark variant: `IntelSponzaDaylight.he`; settings and the preserved authored cameras are in `benchmark_config.json`.
- Material quality: 2048 maximum source texture dimension, linear-light Lanczos filtering for sRGB color/emissive inputs and linear filtering for data inputs; original 4096 textures remain unchanged. This does not change the main view resolution. Runtime mip generation and runtime quality/performance still require validation.

Preparation requires Python 3.7+ and Pillow. It uses bounded per-image processing and shared geometry once per source mesh. The source's 72 original RGBA8 textures would consume about 6 GiB with full mip chains; the explicit 2048 variant estimates 1.5 GiB. These are texture allocation estimates, not measured VRAM peaks.

The import is resumable only for the same source hash and texture configuration, and refuses unrelated pre-existing output. It writes one material import with 28 small fixture primitives, then imports each source mesh with temporary material identities and remaps those identities to the shared imported materials. Temporary material files referenced by each newly created mesh are the only files removed. Full geometry, texture, and per-invocation logs stay on disk.

After rebuilding a material importer fix, run `import --refresh-materials --importer <absolute-executable>` with the same prepared and output settings. It creates `Materials-<executable-hash-prefix>`, imports real material values, and relinks the owned mesh references to that version. Older materials remain available for diagnosis; no scalar values are hand-patched. `prepare --reuse-prepared-textures` reuses an earlier texture conversion only after matching the source, dimensions, and source/output hashes.

Verification checks source/imported local position bounds, triangle counts, required tangent/normal/UV data presence, binary sizes and references, node TRS and parent links, shared mesh identities, material factors, alpha/double-sided state, texture channel mappings, texture bytes, and color spaces. It is structural conversion evidence; it does not claim image quality, motion stability, or rendering performance.

Unsupported animation, skinning, morph targets, node matrices requiring shear decomposition, meaningful vertex colors, non-UV0 material mappings and unknown extensions are rejected. The current official base asset uses UV0 for all materials and contains only white vertex colors, checked across all 59 affected primitives. Additional UVs remain preserved in the official source and are not used by the imported materials.

All 24 light intensities in this source are zero. Their original metadata remains in `conversion.json`; the generated scene retains their transform nodes. A declared benchmark lighting setup must be added separately. Six original perspective cameras are retained; the first is primary. No lighting performance claim follows from a successful import.

The separately named Daylight variant provides one directional light with linear color `(1, 0.95, 0.85)`, intensity `3`, four 2048 shadow cascades, and an 80-metre shadow distance. It reuses PBRTest's `Assets/Textures/Hdr/snowy_field_2k.hdr` at manual exposure `0.31` and IBL intensity `1`. The first camera retains its authored pose/FOV, with declared near/far planes `0.1/100` metres. Original scene nodes, geometry and zero-light nodes are retained. The benchmark manifest declares native 3840x2160, without upscale, dynamic resolution, frame generation or VRS; the runtime harness must verify these settings and record the effects configuration.

Keep the original `credits_license.txt` with the derived data. The source notice includes the CC BY 4.0 text and introductory usage language that is not identical to that license; this intake is for local development benchmarking. See `Examples/RenderingTests/SourceAssets/RenderingBenchmarks/README.md` and `IntelSponza/source_manifest.json` for provenance and the complete review.

## ORCA Bistro and compressed texture variants

The independent exterior scene is described in `Examples/RenderingTests/SourceAssets/RenderingBenchmarks/Bistro/README.md`, including the complete repeatable official-FBX export, source-metadata restoration, time-zero sampling, instance retention, material-channel restoration and import commands. Its first RGBA8 variant is explicitly `BistroExteriorStaticPose1024`; the prepared 2048 images remain available for the compressed configuration.

`compress_benchmark_textures.py` produces a separate variant from an imported scene and a named prepared image configuration. It preserves material factors and all scene objects and references the existing immutable geometry buffers. Only mesh metadata/material paths select the alternate texture resources. Geometry is neither flattened nor copied into a new aggregate. Official Microsoft DirectXTex `texconv` 2026.5.8.1 and its MIT notice/hash manifest are under `Examples/RenderingTests/SourceAssets/RenderingBenchmarks/Tools/DirectXTexMay2026`. No engine third-party source is changed.

```powershell
python Tools/Assets/RenderingBenchmarks/compress_benchmark_textures.py --input-assets Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048 --prepared Examples/RenderingTests/SourceAssets/RenderingBenchmarks/IntelSponza/prepared/textures2048 --output Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048BCGPU --gpu 0
python Tools/Assets/RenderingBenchmarks/compress_benchmark_textures.py --input-assets Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro1024 --prepared Examples/RenderingTests/SourceAssets/RenderingBenchmarks/Bistro/prepared/textures2048 --output Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BCGPU --gpu 0
python Tools/Assets/RenderingBenchmarks/verify_compressed_benchmark.py --variant Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048BCGPU
python Tools/Assets/RenderingBenchmarks/verify_compressed_benchmark.py --variant Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BCGPU
```

GPU conversion is scheduled separately from HGame measurements. Without `--gpu`, the tool uses a single CPU worker. The optional `--bc7-mode6` switch produces an explicitly different encoding configuration and is not used by the accepted GPU-default variants. Every actual encoder command, input/output SHA-256, encoded format, complete mip layout, duration and mip-zero error metric is recorded in `compression.json`. Default BC7 is used for sRGB color/emission and linear packed maps; BC5 stores tangent normal XY, matching the existing shader's Z reconstruction. No input dimensions are changed during compression. All mips use explicit BOX filtering with the appropriate sRGB conversion.

Mask textures preserve continuous BC7 alpha and use the authored cutoff in `--keep-coverage`. Before compression, rejected RGB is filled from normalized valid-color mip levels to avoid black straight-alpha fringes; accepted RGB and all alpha values remain unchanged. Mask output records decoded alpha coverage for every mip. Very small mips cannot represent arbitrary coverage fractions, so this is evidence to inspect rather than an unconditional coverage PASS.

At runtime, `DdsTextureData` strictly validates BC1/3/5/7 UNORM 2D containers (legacy DXT1/DXT5/ATI2 and explicit extended headers), bounds and mip payloads. `TextureManager` uploads compressed block rows directly via existing NVRHI calls and uses the same cache, bindless registration and GPU retirement path. Sampled DDS files require a full offline mip chain. Compressed vertical flipping, arrays, cubes, volumes, signed formats and premultiplied-alpha DDS are explicitly rejected. Material color-space metadata remains authoritative. Alpha coverage must already be authored offline; runtime does not decode/recompress DDS.

`ManagedTexture::PayloadBytes` and `GetResidentTexturePayloadBytes()` expose texel/block payload accounting, including resources pending GPU retirement, not driver allocation or total VRAM usage. `hectl test DdsTextureData` is the headless binary-format contract test; the integration owner runs the required target builds and HGame/DX12/Vulkan validation before claiming runtime support.

## Normal XYZ candidate without repeated color/data encoding

The following commands create independent 2K variants and require the same explicit exclusive GPU encoding window used for other texconv conversions. They do not run HGame. `--reuse-compressed` verifies SHA/format and hard-links compatible DDS, encoding only changed normal maps. Before any future re-encode, the tool removes only its own hard link so it cannot truncate a shared baseline. Full source PNGs and the BC5 benchmark remain immutable.

```powershell
python Tools/Assets/RenderingBenchmarks/compress_benchmark_textures.py --input-assets Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048 --prepared Examples/RenderingTests/SourceAssets/RenderingBenchmarks/IntelSponza/prepared/textures2048 --output Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048BC7Normals --normal-format BC7 --reuse-compressed Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048BCGPU --gpu 0
python Tools/Assets/RenderingBenchmarks/compress_benchmark_textures.py --input-assets Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro1024 --prepared Examples/RenderingTests/SourceAssets/RenderingBenchmarks/Bistro/prepared/textures2048 --output Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BC7Normals --normal-format BC7 --reuse-compressed Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BCGPU --gpu 0
node Tools/Assets/RenderingBenchmarks/generate_mesh_lods.mjs --source Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048BC7Normals --output Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048BC7NormalsLOD
python Tools/Assets/RenderingBenchmarks/verify_compressed_benchmark.py --variant Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048BC7Normals
python Tools/Assets/RenderingBenchmarks/verify_compressed_benchmark.py --variant Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BC7Normals
python Tools/Assets/RenderingBenchmarks/verify_mesh_lods.py --variant Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048BC7NormalsLOD
python Tools/Assets/RenderingBenchmarks/verify_normal_mip_moments.py --variant Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048BC7Normals
python Tools/Assets/RenderingBenchmarks/verify_normal_mip_moments.py --variant Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BC7Normals
python Tools/Assets/RenderingBenchmarks/generate_benchmark_trajectory.py --scene Examples/RenderingTests/Assets/RenderingBenchmarks/IntelSponza2048BC7NormalsLOD/IntelSponzaDaylight.he --output Examples/RenderingTests/Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BC7NormalsLOD --far 100
python Tools/Assets/RenderingBenchmarks/generate_benchmark_trajectory.py --scene Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BC7Normals/BistroExteriorStaticPose1024.he --output Examples/RenderingTests/Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BC7Normals --far 200
python Tools/Assets/RenderingBenchmarks/verify_benchmark_camera.py
```

Use the bundled numpy/Pillow Python runtime documented above for compression/geometry quality tools. BC7 normal direction error is measurably worse on some source maps; BC7 retains mip moments, but remains a candidate pending rendered comparison. Detailed metric/reference distinctions and unresolved Mask mip failures are in `Examples/RenderingTests/Assets/RenderingBenchmarks/README.md`.

## Mask post-compression correction

Use the pinned bundled numpy/Pillow runtime, for example:

```powershell
$benchmarkPython = 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
& $benchmarkPython Tools/Assets/RenderingBenchmarks/repair_mask_mips.py --source Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BCGPU --output Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BCGPUMaskFixed
& $benchmarkPython Tools/Assets/RenderingBenchmarks/repair_mask_mips.py --source Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BC7Normals --output Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BC7NormalsMaskFixed --reuse-repairs Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BCGPUMaskFixed
& $benchmarkPython Tools/Assets/RenderingBenchmarks/verify_compressed_benchmark.py --variant Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BCGPUMaskFixed
& $benchmarkPython Tools/Assets/RenderingBenchmarks/verify_mask_filtering.py --variant Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BCGPUMaskFixed
& $benchmarkPython Tools/Assets/RenderingBenchmarks/compare_mask_alpha_codecs.py --variant Examples/RenderingTests/Assets/RenderingBenchmarks/ORCABistro2048BCGPUMaskFixed
```

This correction uses only CPU encoding of <=32px mip images; it does not reserve the GPU. It preserves source mip0..5 payload bytes and checks both nearest representable point coverage and periodic filtered area. Per-attempt hashes/commands and independent filtering/codec measurements remain under each fixed variant. Material names, exact failure/correction numbers, tiny-mip limits, trilinear checks and their remaining rendered-validation limits are recorded in `Examples/RenderingTests/Assets/RenderingBenchmarks/README.md`.

## Bind and audit the existing YBot validation clip

```powershell
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' Tools/Assets/RenderingBenchmarks/prepare_ybot_validation.py --project Project
```

**unit-test PASS**: author an independent single-state controller from the existing generated-schema controller values, bind the existing split walk clip to its matching mesh skeleton, and check every real joint/weight plus frame 20/40 numerical skinning. The tool changes only the YBot controller/legacy animation path and descriptive tag in `SurfaceCoverage.he`; it asserts all other entity data is unchanged. Original clips, mesh/binary and skeleton are immutable and SHA-recorded in `Validation/ybot_animation_validation.json`. Repeated runs are byte/mtime stable. No generated code, generic serializer or runtime loader is patched. Original YBot asset provenance is unavailable in this checkout; no new distribution license is asserted. Actual GPU palette, silhouette and motion-vector validation still requires the owned HGame integration run.

## Restore the official Bistro environment

`python Tools/Assets/RenderingBenchmarks/restore_bistro_environment.py` creates the independent `Trajectories/BistroExterior2048BCGPUMaskFixedEnvironment` input and copies the official `san_giuseppe_bridge_4k.hdr` into the shared Environments directory. It verifies the official script's declared environment/intensity without executing that script, preserves all original1309entities, and writes source/output hashes and structural assertions to `environment-restoration.json`. Existing output is refused to preserve evidence. Use the derivative benchmark_input.json for further Bistro quality/performance work. Earlier exterior benchmark scenes omitted this environment and are not valid high-quality performance evidence. Environment intensity10 follows the source script; manual engine exposure remains0.31. Official source license/attribution manifests remain applicable; no new asset license is asserted.
