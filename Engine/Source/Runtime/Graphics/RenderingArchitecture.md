# HEngine Rendering Architecture

## Supported backends

The runtime renderer uses NVRHI as its only GPU abstraction. The same Windows
Game and Editor binaries compile both `DX12` and `Vulkan`; DX12 is the default.
`--graphics-api=dx12|vulkan` is the command-line override. A non-strict Vulkan
startup records the exact loader/device failure and performs one DX12 fallback;
`--strict-graphics-api` turns that condition into a startup failure.

Platform device, queue, swap-chain and native resource interop code belongs in
`Runtime/Platform/`. Public render code must not include D3D12 types or expose
native resource/view pointers.

```text
ECS / Level / Assets
        -> RenderWorld extraction
        -> GPU Scene / Renderer / RenderGraph passes
        -> thin HEngine resource and pipeline caches
        -> NVRHI
        -> DX12 or Vulkan platform backend
```

## Device and frame ownership

`RenderDevice` owns one platform `IRenderBackend`. Each backend owns its native
instance/device/queues, NVRHI device and swap chain. `FrameContext::FrameIndex`
is a resource-lifetime slot; `SwapChainImageIndex` is independent on Vulkan.
A frame follows this order:

1. `BeginFrame` acquires the back buffer, waits for that frame slot and opens
   its NVRHI graphics command list.
2. RenderGraph and explicit integration passes record into that command list.
3. `SubmitAndPresent` closes and executes the list, records its submission
   instance, runs NVRHI garbage collection and presents.
4. Deferred resource and descriptor releases may be recycled only after their
   recorded submission is complete.

Frame submission is transactional. A recording failure calls the backend's
`AbortFrame` before any GPU execute and aborts the matching RenderGraph pool
slot. Once `executeCommandList` succeeds, the submission instance is published
to every participating scene and readback owner even if the completion-query
or `Present` step later fails; the backend then enters an explicit terminal
frame-error state instead of reusing resources with an incorrect lifetime.

Queue types are represented in RenderGraph. Cross-queue scheduling and async
compute execution are not enabled yet; active graphics and compute passes
execute on a graphics queue that supports both operations.

Swap-chain back buffers are imported NVRHI textures. The actual format is
reported through `SwapChain::GetFormat()` and passed into upper-level pipeline
creation, so DX12 RGBA and Vulkan BGRA surfaces share the same renderer. Resize
waits for GPU safety, releases imported framebuffer references, recreates the
native chain and imports the new images.

## Resources and RenderGraph

Persistent engine resources use typed generation handles backed by NVRHI
handles. They carry an immutable creation descriptor, debug name and explicit
ownership. Transient and persistent graph resources are allocated by
`RenderGraphResourcePool`; imported resources remain externally owned.

RenderGraph setup declarations are the only upper-level source of resource
states. Passes declare reads/writes and intended usages; compilation builds a
DAG, detects cycles, culls passes unreachable from graph outputs, computes
first/last use and maps usages to `nvrhi::ResourceStates`. Transitions are
committed before each pass. Imported resources declare both initial and final
states, and the swap-chain image finishes in `Present`.

Pass execution must only resolve resources declared during setup. Debug builds
reject stale handles, undeclared access, read-before-write and invalid imported
state contracts. Graph debug dumps include the compiled pass order and resource
lifetimes. Timestamp queries have an explicit per-pass interface; consumers
must not report GPU timings unless a query was actually recorded and resolved.

## RenderWorld and GPU scene

ECS extraction owns all per-frame render data, including current/previous
transforms, bounds, stable object/material/mesh indices, editor selection and
an owning copy of each skinned object's bone palette. Pass execution never
queries the ECS registry or a mutable animator.

`SceneRenderer` is the single production GPU-scene implementation. It packs
object transforms/indices, materials and bindless texture indices, bounds and
draw metadata into structured buffers. A compute pass compacts visible object
indices and writes indexed-indirect arguments per static-opaque batch; the
forward pass consumes those GPU-owned results. The removed standalone
`GpuScene` prototype was not retained alongside this path.

Explicit CPU render paths remain for skinned meshes, sorted transparent meshes,
special editor geometry, third-party VFX and UI. They share the same cached
NVRHI pipelines, material buffer and bindless descriptor tables; they are not a
whole-scene or legacy-backend fallback.

The default graph records scene upload, directional shadow, depth prepass,
opaque HDR, skinned HDR, extracted 2D, sorted transparency, editor outline,
VFX, tone mapping, offscreen/capture, UI and present passes. The main scene
target is `RGBA16_FLOAT`; output conversion occurs only in tone mapping.
Explicit Hardware RTGI adds RayTracingScene build/update, Surface Data, RT
trace, temporal/moments/A-Trous, and GI composite before Transparent/VFX.

## Shader and binding convention

HLSL is compiled by DXC to backend-typed DXIL/SPIR-V artifacts using
column-major matrix layout (`-Zpc`).
DXC performs the language-to-binary compilation directly; SPIRV-Tools validates
Vulkan 1.3 artifacts offline. SPIRV-Cross is intentionally not part of the
compile or runtime path in this phase.
Raster and compute stages default to `vs_6_0`, `ps_6_0` and `cs_6_0`;
libraries default to `lib_6_3`. A shader opts into another target explicitly:

```hlsl
#pragma hprofile ps ps_6_6
#pragma hprofile ps ps_6_6 HEAP_DIRECT
```

The two-argument form applies to every compilation of that stage. The optional
`BASE` or declared `hvariant` condition has higher priority and limits the
override to that artifact. Non-default profiles use
`Shader_VARIANT__profile.stage` names, while default-profile artifacts retain
the compatibility name `Shader_VARIANT.stage`. Cache identity includes source
and recursive include contents, logical paths, variant macros, entry point,
target profile and compile options. After compilation, DXC container reflection
must report the requested shader model; baseline profiles reject requirements
such as resource/sampler heap-direct indexing. Heap-direct shaders are separate
SM6.6 artifacts and are selected only by the capability-gated renderer path.

Register spaces are stable across graphics and compute pipelines:

- `space0`: per-frame and per-view resources.
- `space1`: heap-direct textures on the capable DX12 path, or the active
  baseline material descriptor page. Baseline material indices are page-local.
- `space2`: GPU scene object, material, mesh, instance and draw data.
- `space3`: pass-local resources.
- `space4`: samplers.

The main scene target is linear `RGBA16_FLOAT`. Material/PBR shaders write
linear HDR values. Exposure, ACES tone mapping and output transfer conversion
belong to the dedicated tone-mapping pass, never to individual materials.

## Third-party interop

ImGui and RmlUi renderers consume NVRHI handles and backend-selected shader
artifacts. Effekseer uses `IEffekseerRenderInterop`; native implementations are
isolated in `Runtime/Platform/Interop/DX12` and `.../Vulkan`, reusing the
engine-owned device, graphics queue and active command list. D3D11On12 and
secondary devices are not permitted. CPU-particle effects are supported.
Effekseer 1.80.6 GPU-particle effects are rejected during initialization or
package loading: both the unmodified vendor sample and the derived stress asset
hit a vendor `StandardRenderer` bounds assertion with the external DX12 command
list path, so the runtime reports the limitation instead of hanging or silently
substituting a CPU effect.

## Vulkan boundary

The Vulkan 1.3 backend implements the same `IRenderBackend`, `RenderDevice`,
`FrameContext` and imported swap-chain contracts as DX12. Vulkan-native handles
remain in `Runtime/Platform/Vulkan` or platform interop. Renderer, RenderGraph,
Material, Mesh, RenderWorld and ECS code consume NVRHI only. Baseline Vulkan
uses 120-SRV material descriptor pages: visible materials are packed without
splitting one material, GPU-driven and CPU passes bind the assigned page, and
old page binding sets retire only after their last submission completes. This
removes the total-scene 120-texture correctness limit without requiring runtime
descriptor arrays. DX12 heap-direct indexing remains independent; a future
Vulkan descriptor-indexing mode is a performance optimization and must be
introduced as a separate capability, not inferred from the DX12 path.
