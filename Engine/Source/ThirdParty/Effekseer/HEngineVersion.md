# Effekseer Runtime provenance

- Version: `1.80.6`
- Release tag: `1806`
- Release commit: `93dd29c7b4e353fcef6186fe4ac74c9757243550`
- Source archive: `EffekseerForCpp1.80.6.zip`
- Source URL: `https://github.com/effekseer/Effekseer/releases/download/1806/EffekseerForCpp1.80.6.zip`
- SHA-256: `B0004A4961F549AA44031956196388AA07125AAA7E2A364670F03A3602727C70`
- Retrieved: `2026-08-13`

The archive hash matches the digest published by the official GitHub Release API.
Runtime licensing is recorded in `LICENSE.txt`, with renderer-specific notices in
`LICENSE_RUNTIME_DIRECTX.txt` and `LICENSE_RUNTIME_VULKAN.txt`.

HEngine configures the Runtime from `Engine/Source/ThirdParty/CMakeLists.txt`:
examples, the optional network server/client, DX9, DX11, OpenGL, Vulkan, and
Metal are disabled; only the DX12 renderer is built for the HEngine runtime.
Effekseer receives HEngine's existing native DX12 device, graphics queue, and
active command list through the platform interop adapter; it never creates a
DX11 device and does not use D3D11On12.

One bounded local patch is applied in
`src/EffekseerRendererCommon/EffekseerRendererCommon/EffekseerRenderer.GpuParticles.cpp`:

- preserve a newly created GPU emitter when its first compute receives zero
  delta time, so it can emit on the next update;
- decrement the remaining free block after a split allocation and fully merge
  returned blocks;
- commit emitter state only after particle/trail allocations succeed.

The zero-delta issue was reproduced with the pinned 1.80.6 GPU sample as an
emitter that was freed before its first non-zero update. The allocator fixes
prevent phantom emitters, overlapping allocations, and lost full-capacity
blocks during the bounded 20k/50k stress and shutdown tests. No other official
Runtime source is modified.
