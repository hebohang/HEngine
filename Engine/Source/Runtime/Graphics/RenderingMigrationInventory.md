# Rendering Migration Inventory

This inventory records the local-workspace audit that preceded the DX12
rewrite and the resulting cleanup. It distinguishes behavior retained in the
modern renderer from implementations that were removed with the legacy path.

## Reused from the local NVRHI migration

- HLSL material property and variant parsing.
- NVRHI validation and message callback patterns from the local DX11 path.
- Existing mesh/material CPU resource ownership and scene component data.
- Existing shadow, IBL, skinned, instancing, outline, RmlUi and Effekseer
  behavior as feature requirements.

The DX11 implementation itself is not a retained backend. Reusable behavior
now lives in RenderWorld extraction, NVRHI resource/pipeline caches and
RenderGraph passes; the old backend files and public immediate-state layer have
been deleted.

## Legacy runtime call chain

```text
Application
  -> GraphicsContext::Create(native window)
  -> RendererAPI / RenderCommand immediate state
  -> GameLayer or EditorLayer legacy framebuffer
  -> Level::OnUpdate*
  -> RenderSystem3D legacy sequential graph
  -> Mesh::Draw / Material::Bind (removed)
  -> DX11 implementations
  -> ImGui/Rml/Effekseer DX11 backends
  -> GraphicsContext::SwapBuffers
```

`RenderSystem3D` previously queried ECS and issued GPU work in the same call.
It now publishes an immutable RenderWorld snapshot. `Mesh`, `SubMesh` and
`Material` are CPU asset records: they retain geometry, bounds, skinning data,
material factors and texture asset references, while NVRHI buffers, textures
and binding sets are owned by renderer caches. The old asset-level `Draw`,
`Bind` and constructor-time GPU allocation paths have been removed.

`Renderer2D` is now a CPU submission collector rather than an immediate GPU
renderer. Each Level owns a scoped collector; sprite, circle and debug-line
systems append project-root-relative texture paths and primitive data, then
`RenderSystem2D` or `RenderSystem3D` moves that data into
`RenderWorld::Primitives2D`. Physics, navigation and camera debug systems run
before extraction, so no ECS or stack-backed callback survives into rendering.
The old 2D texture/constant-buffer libraries and `SubTexture2D` GPU wrapper
have been removed.

## Removal set

- `Runtime/Platform/DirectX/DX11/` context, renderer API, buffers, textures,
  framebuffer, shaders, pipelines, GUI, Rml and VFX implementations.
- `Runtime/Platform/OpenGL/` and the OpenGL/Glad/shaderc/SPIRV-Cross runtime
  dependency chain.
- Superseded custom DX12 command list, command queue, descriptor allocator,
  resource-state tracker, root-signature, pipeline and resource wrappers.
- Public `GraphicsContext`, `RendererAPI`, `RenderCommand`, legacy framebuffer,
  texture/buffer/pipeline/pass interfaces. Their callers now use typed NVRHI
  resources or RenderWorld submission.
- Legacy sequential post-processing and C++ `RenderTech` wrappers. HDR tone
  mapping and post effects are graph passes owned by `SceneRenderer`.
- Runtime backend selector branches and configuration values for DX11/OpenGL.
- The unused GLSL shader tree and Cooker classification for GLSL/FXC-era
  outputs; HLSL source and DXC-produced `.vs`/`.ps`/`.cs` DXIL are the only
  engine shader formats.
- Test-only resource-registry and GPU-scene prototypes that duplicated the
  production RenderGraph pool, bindless manager or SceneRenderer GPU buffers.

## Third-party migration

- Dear ImGui: NVRHI renderer with GLFW platform input. Secondary platform
  swapchains require an explicit DX12 implementation before Editor
  multi-viewport can be enabled.
- RmlUi: NVRHI `RenderInterface`, injected after device creation.
- Effekseer: DX12 platform interop using the existing device, graphics queue
  and active command list; no D3D11On12. CPU-particle effects issue real DX12
  draws. GPU-particle packages are detected and explicitly rejected because the
  pinned 1.80.6 renderer asserts in its external-command-list draw flush.
- RenderDoc: remains a pre-device module and its trigger path targets the DX12
  process. Automated PNG capture is validated separately; a `.rdc` capture
  still requires a machine with RenderDoc installed and is reported as
  `runtime-manual` rather than an automated render assertion.
- Automated capture: NVRHI staging-texture readback and PNG encoding.

## Final cleanup checks

Cleanup verification searches engine-owned sources and generated link commands
(excluding ThirdParty compatibility declarations) for `D3D11`, `OpenGL`,
`nvrhi_d3d11`, `RendererAPIType`, legacy native-resource `void*` interfaces and
superseded DX12 wrappers. Source scans are paired with Game/Editor builds,
bounded runtime captures and executable import-table checks.

The production GPU scene is not a test adapter: SceneRenderer owns the actual
object, material, bounds, draw-data, compacted-visible-index and indirect-
argument buffers used by the DX12 scene smoke and Game/Editor paths. Static
opaque meshes use compute culling and indexed indirect draws. Skinned,
transparent, editor-special, VFX and UI work remains explicitly classified and
does not trigger a whole-scene fallback.
