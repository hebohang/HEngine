# GPU trail zero-time correction (Effekseer 1.80.6)

The previous update shader wrote a trail sample and incremented its particle
update count even when emitter DeltaTime was zero. Common ComputeFrame also
advanced that emitter's circular trail phase. Thus pausing or setting speed to
zero for TrailLength updates overwrote its history with the head position.
Another active emitter was not required to trigger the defect.

The patch preserves Compute and root-color publication. At zero time the update
shader writes only particle Color; all geometry, velocity, age, flags/count and
trail storage remain unchanged. Parameter random draws retain their original
order before color calculation. Trail writes and CPU phase advancement require
positive time **for that emitter**, independently of other emitters.

There is a second phase distinction: CPU TrailPhase is the *next* write slot,
while the shared GPU emitter uniform used by Draw represents the *last* slot
written by the update. Positive-time upload happens before CPU phase increments.
A zero-time upload now uses a local snapshot with nextPhase - 1 modulo length;
CPU nextPhase is unchanged. This prevents a one-slot visual discontinuity on the
first paused frame. An emitter with no samples has updateCount zero and reads no
trail sample. Positive steps, paused steps and other active emitters can mix.

Files changed are Common GpuParticles.cpp, the DX11 reference update HLSL, DX12
update HLSL/header, and retained Vulkan update GLSL/header. No Runtime API changed.
DX11 is maintained as reference source; its embedded shader is outside the
DX12/Vulkan engine scope and was not rebuilt.

## Reproducible shader generation

Run `python Tools/VFX/rebuild_gpu_update_shader.py`. It invokes only shader tools,
not an engine build or GPU session. Required PATH tools on this machine:

- fxc: Windows SDK 10.0.19041.0, x86 compiler.
- glslangValidator, spirv-cross, spirv-val: Vulkan SDK 1.3.211.0.

The Vulkan compute GLSL had been absent although compile.py referenced it. The
retained source was reconstructed from the bundled original SPIR-V using
`spirv-cross original.spv --vulkan-semantics --version 450 --output source.comp`,
then patched equivalently to DX12. Future rebuilds use this retained source.
No SPIR-V words were patched by hand. Existing compile.py can also now compile
this compute input, but the bounded script additionally enforces ABI equality.

The script compares Vulkan recursive member offsets/array and matrix strides,
block sizes, descriptors and workgroup size to gpu_update_shader_abi.json, captured
from the original bundled binary. It compares fxc buffer/resource reflection to
gpu_update_shader_dx12_abi.txt, captured from the original bundled DX12 binary.
Publication is refused if either comparison fails. Vulkan storage strides remain
ParticleData=80 and TrailData=16; cb2=112, cb1=368, cb0=16; descriptor sets remain
uniform=0, sampled texture=1, storage=2; workgroup remains 256x1x1. DX12 compiler
optimizes unused cb0 away, identically to its original shader.

Shader-only compilation, spirv-val and both ABI comparisons: **PASS**. Two runs
produced identical embedded-header SHA-256 values:

- DX12: c3a6d8cf92f5a883585d094af9c003858f98cc245949d3933d4b3ec0976855a5
- Vulkan: 97a9ec449895d846dbfe7c0d24799400e97c7094648d27590360ed93ae8beb1b

Full fxc disassembly, Vulkan reflection and tool paths are in
out/agent/vfx-zero-delta-shaders/{dx12.disassembly.txt,vulkan.reflect.json,report.json}.
These are shader compilation evidence, not engine build/runtime acceptance.

## Official-compiled GPU fixture

Run `python Tools/VFX/generate_gpu_trail_fixture.py`. This calls the existing
Effekseer.exe 1.80.6 CUI compiler and official CLR EDIT decoder, never an engine.
It publishes `Assets/Effects/Stress/GpuTrailPause16.efkpkg`, with editable source,
compiled effect, roundtrip XML, texture, license and provenance under
Project/SourceAssets/VFXAuthoring/GpuTrailPause16.

The fixture enables GPU particles, disables CPU drawing, and uses EDIT
RenderShape.Shape=2 (Trail), TrailLength=16, 16 total emissions at 1/frame,
fixed lifetime=120 frames, speed=.04 and spread=20, root color binding=3.
Official EDIT omits default TrailLength=16; this default was verified directly
from the locked CLR RenderShapeParams type. The generated BIN_ also contains
unique enabled/basic and Trail=2,length=16,size=.04 encodings. These targeted
compiled checks are not a full SDK runtime load test. Expected allocator ownership
is one emitter, 256 reserved particles and 4096 reserved trail entries.

Official compilation and compiled EDIT/BIN checks: **PASS**. Package SHA-256:
b652f4aa90d0dee1ee8f057204a55bfcafe291c7d12c85f1168c3874d090c4f6.

## Runtime acceptance still required on DX12 and Vulkan

Use the controlled preview.Session/Storage lifecycle, a fixed camera, fixed seed
and fixed render conditions. Start two copies at separated positions. After 32
positive steps, capture both trails. Pause the first and step 32 frames with the
second active. Compare the first object's isolated geometry mask to its baseline;
it must retain its entire trail. Change only its root color/intensity and step:
color should change with geometry retained. Restore color, resume and step once;
the prior trail must continue, not restart at the head. Repeat using speed=0.
Check the zero-to-positive boundary and interleave multiple updates in one host
frame. Exact pixels can be affected by unrelated temporal rendering; either
disable it or use a controlled object mask and report any remaining limitation.

Check actual dispatch/timer submissions and allocator failure counters, then stop
and retire frames and require allocator ownership to return to baseline. Existing
CPU trail recipes do not cover this case. Engine build, GPU images, mixed-emitter
runtime pause/resume and device recreation are **NOT_RUN** in this patch stage.
