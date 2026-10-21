# Windows graphics compatibility and capability policy

HEngine exposes one NVRHI-based renderer with DX12 and Vulkan platform
backends. The same Game/Editor binary contains both and defaults to DX12. The
supported operating-system baseline is Windows 10 1909 x64 with the Agility
loader update (build 18363.1350+) or Windows 11 x64. Windows 10 2004/20H2
require build 19041/19042 revision 789+. DX12 adapter eligibility uses
`D3D_FEATURE_LEVEL_11_0` only as the minimum argument to `D3D12CreateDevice`;
the renderer never creates an FL12 device and then rebuilds an FL11 device.

## Device selection and capability discovery

Hardware adapters are enumerated in DXGI high-performance preference order.
Every candidate is tested independently at FL11_0, and a rejected candidate
does not prevent trying the remaining hardware adapters. The shared selection
options already carry a preference index, adapter LUID and WARP flag so future
`--gpu`, `--adapter-luid` and `--warp` launch switches do not require another
device-selection implementation.

After device creation, the DX12 platform layer queries and records the actual
maximum feature level, Shader Model, Root Signature version, Resource Binding
Tier, DXR Tier, Mesh Shader Tier, VRS Tier, Sampler Feedback Tier, heap-direct
indexing prerequisites and Enhanced Barriers. Optional features are not inferred
from Feature Level. NVRHI is then created with only the supported heap-direct
and barrier modes enabled, and its independent feature queries are recorded for
acceleration structures, ray-tracing pipelines, RayQuery, meshlets, VRS,
heap-direct indexing, Enhanced Barriers, SER and opacity micromaps.

The selected adapter log and probe JSON include name, VendorId, DeviceId, LUID,
dedicated video memory, driver version and a reason for every enabled or disabled
advanced feature.

### Vulkan device selection

Vulkan requires an installed graphics-driver loader exposing Vulkan 1.3. The
backend requires a graphics/present queue with compute support,
`VK_KHR_swapchain`, timeline semaphores, dynamic rendering, synchronization2,
scalar block layout and sampled-image-array non-uniform indexing. Discrete and
integrated hardware adapters are scored before virtual/CPU devices; explicit
GPU index and Windows LUID fields use the same public selection structure as
DX12. Phase one deliberately selects bounded descriptor tables. Optional
ray-tracing, mesh, VRS, SER and OMM observations remain independent flags and a
raster-capable device is never rejected merely because they are absent.

Vulkan swap-chain image indices are independent from the three resource
lifetime frame slots. Binary acquire/render-finished semaphores are attached to
the matching NVRHI submission. Surface format and effective present mode are
reported upward; VSync selects FIFO, while VSync-off tries MAILBOX, IMMEDIATE,
then records a FIFO fallback. Out-of-date/suboptimal surfaces are recreated at
a GPU-safe boundary.

## Renderer paths

The coarse profile is diagnostic only; path decisions continue to use the
individual capability flags.

| Path | Requirements and behavior |
| --- | --- |
| Baseline Raster | DX12 FL11_0+/SM6.0 or the Vulkan 1.3 baseline above; bounded 120-SRV descriptor pages and a 15-sampler table; `vs_6_0`, `ps_6_0`, `cs_6_0`; no hardware-RT dependency. Visible materials are packed without splitting across as many pages as required, and draw batches bind their assigned page. DX12 uses legacy barriers when Enhanced Barriers are absent; Vulkan uses synchronization2 through NVRHI. |
| Modern Raster | Each of SM6.6, Binding Tier 3, heap-direct indexing, Enhanced Barriers, Mesh Shader and VRS is exposed independently. Heap-direct material pixels use explicit `ps_6_6` variants only when RS1.1, SM6.6, Tier 3 and the NVRHI feature all agree. |
| Hardware RT readiness | Native DXR Tier 1.0 exposes acceleration-structure and pipeline readiness through NVRHI; Tier 1.1 additionally exposes RayQuery. SER and OMM remain independent. GI stays Off by default, so the active renderer remains raster until a technique is explicitly selected. |

If heap-direct indexing is unavailable, material texture indices are relative
to the material's descriptor page and every bounded slot contains a valid
fallback. A scene may use more than 120 material textures in total; only one
material must fit within one page. Pages are cached while stable and retired by
GPU submission when their contents change. If
Enhanced Barriers are unavailable, NVRHI is configured for legacy resource
barriers. Neither downgrade is silent.

## Shader profiles

`HShaderCompiler` defaults VS/PS/CS to Shader Model 6.0 and libraries to
`lib_6_3`. `#pragma hprofile <stage> <profile> [BASE|<variant>]` is the explicit
opt-in mechanism. The target profile participates in the content key, metadata
and artifact stem; non-default outputs use `__<profile>` so they cannot replace
baseline DXIL. DXC container inspection verifies the requested shader model and
rejects post-SM6.0 requirements in baseline artifacts.

Each logical shader is emitted to `CACHES/DXIL` and `CACHES/SPIRV`; backend,
binary format, target profile and options participate in cache identity and
sidecar naming. DXC directly emits Vulkan 1.3 SPIR-V using the engine binding
shifts and DX memory layout. SPIRV-Tools validates artifacts offline.
SPIRV-Cross is intentionally not required by compilation, runtime reflection or
the shipping package in this phase. Material manifest v3 stores independent
DX12/DXIL and Vulkan/SPIR-V ArtifactSets under one logical material interface.

The current heap-direct material shaders are isolated in four wrapper sources.
Their nine pixel artifacts use `ps_6_6`; all other current runtime
stage/variants compile at their 6.0 defaults.

## Non-interactive probe

Development and packaged games support:

```powershell
StarbloomSurvivor.exe --dx12-probe --output GraphicsCaps.json
StarbloomSurvivor.exe --graphics-probe --graphics-api=vulkan --output VulkanCaps.json
```

`--dx12-probe` is the compatibility alias for the generic probe. It runs before
project content, Python or game initialization and uses the same backend device
selection, capability queries, NVRHI options and policy resolver as the game.
It creates no window or swap chain, writes adapter/native/NVRHI capabilities,
the selected raster profile and downgrade reasons, then exits. Failure returns
a non-zero exit code.

Normal startup accepts `--graphics-api=dx12|vulkan` (with `--renderer` as an
alias). CLI overrides User, Project and Engine defaults. Backend changes made
in Editor settings are saved to Project/User configuration and take effect
after process restart. A Vulkan failure performs one clearly logged DX12
fallback unless `--strict-graphics-api` is present. `--warp` is DX12-only and
conflicts with an explicit Vulkan request.

## Distribution and validation boundary

The package keeps the pinned Agility `D3D12/D3D12Core.dll`, both shader formats,
Python and its official app-local VC redistributable dependency. It never
contains `vulkan-1.dll`, Vulkan SDK tools or validation layers: Vulkan is
delay-imported and loaded only from System32 after that backend is selected, so
DX12 still starts on a machine without Vulkan support. PE validation walks
normal and delay imports recursively, rejects wrong-architecture/unreferenced
binaries, forbids build-machine dependency leakage and verifies Agility exports.

Hosted Windows CI builds and statically verifies the package
but does not claim DXR success because hosted runners do not provide a reliable
physical GPU. Optional self-hosted jobs run the real probe on the documented
Windows 10/11 Intel, NVIDIA, AMD, DXR and non-DXR classes for each selected API,
download the exact hosted-build package ZIP, run its bounded full-game check,
and upload their JSON reports.

Hardware ray-tracing work consumes `RenderCapabilities`,
`HardwareRayTracingRendererPath` readiness, `RenderDevice::GetCapabilities()` and the existing
NVRHI device. It must register an actual hardware-RT technique before changing the
active profile from raster; RayQuery, SER and OMM must remain separate gates.
The stable native DXR 1.2 OMM path is compiled into NVRHI. SER currently needs
the optional NVAPI SDK, which is not vendored; probe JSON reports this build-time
limitation separately instead of presenting it as a hardware-negative result.
