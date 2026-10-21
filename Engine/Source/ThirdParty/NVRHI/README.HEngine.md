# HEngine NVRHI source record

- Upstream: https://github.com/NVIDIA-RTX/NVRHI
- Commit: `d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076`
- Snapshot date: 2026-09-29 (upstream commit dated 2026-09-23)
- License: MIT (`LICENSE.txt`)

This directory is a source snapshot with upstream Git metadata and build artifacts excluded. HEngine configures static NVRHI with validation; Direct3D 12 and Vulkan follow `HE_RENDER_DX12` and `HE_RENDER_VULKAN`. Direct3D 11, NVAPI, Nsight Aftermath, RTXMU, and install rules are disabled. Upstream tests remain disabled by default.

Retained HEngine compatibility patch: the Direct3D 11 backend uses `LESS_EQUAL` for comparison samplers, preserving the existing shadow sampler behavior if that backend is enabled again.

2026-09-13 Vulkan unsubmitted-recording lifetime patch:
`CommandList::open` no longer stores a self-reference in its current tracked
command buffer. `Queue::submit` adds that reference before publishing the buffer
to its lifetime tracker, preserving upload/scratch ownership until GPU retirement.
An unsubmitted list can consequently destruct; its destructor atomically clears
only volatile-buffer versions matching its own queue and recording ID. Submitted
versions and other recordings remain untouched. No cross-backend API changes.

2026-09-29 upstream update: applied the complete upstream delta from
`8e8c36e37558acec333204619b95d9d2fcdc4a79`, retaining both patches above.
The only source differences from the new upstream snapshot are in
`src/d3d11/d3d11-device.cpp`, `src/vulkan/vulkan-commandlist.cpp`, and
`src/vulkan/vulkan-queue.cpp`. This update includes component mappings,
memory queries, D3D12 barrier fixes and Vulkan descriptor updates. It does not
fix Vulkan `Device::createTexture` ownership on allocation failure: that function
and `vulkan-allocator.cpp` are unchanged from the previous snapshot. See
`Docs/FSRStreamingCombatWork.md` at the repository root for validation and the
user-approved DX12-first scope; the pending local texture-failure patch is not
part of this snapshot.
