# FSR particle input validation

The renderer combines transparent alpha coverage with scene-linear color change
to form FSR reactive/composition inputs. This diagnostic checks the actual GPU
readbacks for built-in blend, add, subtract, multiply and distortion particles on
DX12/Vulkan, plus an opt-in development GPU-particle add case.

Generate isolated assets with the pinned official Effekseer 1.80.6 compiler:

```powershell
py -3 Tools/VFX/generate_fsr_mask_fixtures.py --project C:/WorkSpace/HEngineProjects/MechanicGirlLab
```

The project gets `Assets/Diagnostics/FsrParticles/Particles.he`, six packages and
emissive background materials. Editable sources, original v1 recipes, explicit
XML overrides, official EDIT roundtrips and compiler/package hashes are under
`SourceAssets/VFXAuthoring/FsrMaskValidation`. Recipe v1 has no blend/material
field; these are documented diagnostic XML overrides, not an undocumented recipe
extension. No external artwork, game scene or existing effect is replaced. The
GPU fixture pairs with the emitted CPU add fixture; it does not enable an automatic
fallback registry or certify Low quality. Both use a stationary finite emitter.

Build the Game when engine changes require it, then pin the actual executable:

```powershell
$hash=(Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrParticles.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -GraphicsApi DX12 -GpuProbe -ExistingBuildSha256 $hash -OutputDirectory out/agent/captures/fsr-particles-new-dx12
```

Repeat with Vulkan and a new directory. The driver resolves project isolation,
uses owned Session/Storage lifecycle, stops in finally, and restores the temporary
`HE_VFX_GPU_PARTICLES` environment override. Omit `-GpuProbe` for production-default
CPU paths. The development probe leaves `gpuValidated=false` and does not change
production capability policy. Native submission IDs/timing plus visible shader
output prove that the probe executed; unavailable GPU live-particle metrics are
not interpreted as zero particles.

The paused fixture captures peak/moved/naturally-ended CPU cases, GPU peak/zero-RGB/
ended cases, and empty/stopped controls. Transform and color changes are followed
by one simulation step: Effekseer does not update all particle state while paused.
Every input capture asserts active FSR and inactive standalone TAA. Explicit stop
also verifies no active effect handle remains. GPU allocator retirement and
physical memory are outside this diagnostic's assertions.

```powershell
python Tools/VFX/analyze_fsr_masks.py <DX12-evidence-directory> <Vulkan-evidence-directory>
```

Use Python with NumPy and Pillow. Analysis checks finite inputs, responsive pixels,
mask range, moved-mask centroids, zero masks after natural completion/stop, and
the implemented response formula in locally stable pixels. AOVs are acquired on
different render frames; edge/gradient exclusion accounts for different jitter
without treating it as a mask defect. Subtract/multiply must preserve zero alpha
while producing color-difference response. GPU zero-RGB pixels must still have
alpha coverage and response, so a nonzero color difference cannot hide a missing
coverage path. Compact contact sheets require separate visual inspection;
`analysis.json` deliberately does not declare visual acceptance.

These cases establish representative built-in particle inputs and lifecycle, not
fast-motion ghosting quality, arbitrary custom materials, every GPU particle shape,
or general production readiness of the experimental GPU simulation subsystem.
Current results and exact evidence identities are in `FSRStreamingCombatWork.md`.
