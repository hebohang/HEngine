# HEngine shader binding convention

HEngine shaders are HLSL compiled by DXC with `-Zpc -HV 2021` to backend-typed
DXIL or SPIR-V artifacts. CPU
matrices and HLSL matrices therefore use column-major storage, and transforms
use `mul(matrix, vector)`.

| Register space | Ownership | Stable contents |
| --- | --- | --- |
| `space0` | Frame / view | camera, lighting, shadow-view and other once-per-view constant buffers |
| `space1` | Material textures | sampled textures addressed by stable descriptor index; heap-direct variants address the resource heap directly |
| `space2` | GPU scene / RT scene | object, material, mesh, instance, geometry, light, TLAS, visible-draw and indirect-argument resources |
| `space3` | Pass local | render-pass SRV/UAV/CBV resources declared by RenderGraph |
| `space4` | Baseline material samplers | bounded SM6.0 sampler table; heap-direct variants address the sampler heap directly |

Rules:

- A binding layout is immutable and cached. A draw must not create a layout or
  pipeline.
- `space1` indices remain stable while a material can reference them. Released
  indices are recycled only after every frame that could use them completes.
  Baseline SM6.0 shaders read bounded texture (`space1`) and sampler (`space4`)
  descriptor tables. NVRHI assigns distinct register spaces to their distinct
  descriptor-set layouts; capability-gated SM6.6 variants address the same
  logical indices through the resource and sampler heaps directly.
- `space2` indices are stored in `RenderWorld`/GPU-scene records, not native
  descriptor handles.
- RenderGraph is the owner of `space3` resource-state declarations. Pass code
  may not transition a graph resource independently.
- NVRHI push-constant layouts use `b0` in the declared logical space for DXIL.
  The SPIR-V target compiles the same typed block with
  `[[vk::push_constant]]`; it has `PushConstant` storage and no descriptor-set
  decoration. Ordinary constant buffers continue to use the CBV register
  convention.
- `NvrhiImGuiRenderer` uses pass-local `space3` bindings `b0` (projection push
  constants), `t0` (registered texture), and `s0` (registered/default sampler).
- `NvrhiRmlRenderBackend` uses the same pass-local `b0`/`t0`/`s0` register
  shape for its projection/transform push constants and premultiplied UI texture.
- Native DX12 objects are never part of a shader/material/mesh public API.
- Ray-tracing hit groups use global `space2` tables and stable instance IDs;
  local root signatures and local SBT bindings are not supported.

The old slot-based shaders are migrated pass by pass. A pass is considered
modern only after all of its resources use the spaces above; the compatibility
slot layout is not a supported extension point.
