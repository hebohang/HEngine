# Runtime material texture residency

SceneRenderer now opts material textures into TextureManager's asynchronous mip
residency path. The implementation is shared by DX12 and Vulkan. UI textures,
lightmaps, environment precompute sources and explicit ready/readback consumers
retain their existing pinned loading path.

## Lifetime and data

`TextureLoadDesc::bStreamable` opts a sampled, asynchronous mip chain into demand
tracking. `FirstResidentMip` selects the initial tail (SceneRenderer starts at
mip 2). The managed handle and generation remain stable across replacements.
Physical shader descriptor indices may change and must be refreshed when the
manager revision changes. An existing published texture stays available while
the next tail is prepared and uploaded. Publication follows upload completion;
the previous texture is retained through the last submitted graphics fence.
Heap-direct publication allocates a new descriptor while prior readers are in
flight and retires the old descriptor at that fence. A full heap defers publication
without losing the pending upload or replacing the current texture. In-place
publication is allowed after all reported readers complete. A synchronous ready
request reports descriptor exhaustion instead of silently returning a placeholder.
Bounded mode creates a new immutable binding set; submitted command lists retain
their prior sets and textures. Both material texture caches refresh shader indices
on manager revision changes before packing new frame data.
Release/cancellation uses the existing TextureManager ownership path.

The upload completion query is allocated before submitting commands. A failed
query allocation must not leave a submitted upload without manager tracking.
Creation or pre-submission failure retains the published resource, descriptor
and revision; explicit failed-load invalidation retries on the same managed
handle. This applies to DDS and decoded images, including ready consumers.

DDS uses its offline mip chain and compressed blocks. Streamable DDS loads read
the 128/148-byte header, validate the entire mip layout against the file length,
then seek directly to the selected tail. Only the tail payload is allocated and
read; offsets are rebased for upload. The explicit maximum dimension is applied
before the requested resident mip. Completed scene read-ahead data can supply
the tail without another file read; that earlier read-ahead still stages a full
file. Pinned DDS consumers retain their existing loading path.

Ordinary images still decode/generate their complete chain before selecting the
tail on every residency change, preserving alpha coverage relative to the authored
base mip. A decoded-mip disk cache is not yet implemented.

`ManagedTexture` records source dimensions and the published first resident mip,
alongside actual resource dimensions, mip count and payload bytes. Streamable and
pinned loads have distinct cache identities; incompatible ready/precompute/
readback requests cannot silently acquire a streamable texture.

## Demand and budget

SceneRenderer brackets demand collection with `BeginStreamingFrame()` and
`EndStreamingFrame(budgetBytes)`. Each visible material reports projected object
size through `RequestTextureDetail(handle, pixels)`. Multiple uses of one texture
take the largest demand in the frame. Projection uses output dimensions, so FSR's
lower internal resolution does not automatically reduce texture detail.

Static valid bounds receive a conservative clip-volume check and projected sphere
estimate. Unknown/skinned bounds request full detail. Demand is an approximation
with two-times pixel headroom; it does not yet measure per-triangle UV density or
arbitrary shader UV/displacement operations. Off-view demand persists for 60
render frames, then requests a small tail. Demotions have a 60-frame dwell;
promotions can start after eight frames. Failed replacements back off for 120.

The pure `TextureStreamingPolicy` plans complete mip tails, including block
alignment for compressed formats. It reduces low-priority detail per byte saved
until the target fits. An impossible budget retains valid minimum tails and
reports the excess. At most two residency replacements are in flight. Promotions
check resident, pending and retiring payloads plus temporary headroom. Large
demotions may first publish a smaller intermediate tail before restoring the
planned detail after old resources retire.
The 64 MiB admission allowance is not a global cap on excess residency:
promotions compare against budget+64MiB, while demotions compare against
max(budget, current accounted residency)+64MiB so an already over-budget pool
can shrink. Temporary endpoint overcommit is reported by the soak analyzer.

The current default target is **512 MiB per TextureManager for streamable
material payloads**. This is not a hard process/GPU memory cap: driver alignment,
pinned assets, render targets, geometry and old binding-set references are outside
that target, and initial loads/replacements can temporarily exceed it. Runtime
memory telemetry is covered by the bounded dual-backend traversal diagnostic
described in [StreamingSoakValidation.md](StreamingSoakValidation.md); it is not
a physical-memory cap or unlimited leak-free guarantee. Cross-view/global
budgeting still needs further work.
The public end-frame API accepts a different budget. The persisted setting
`Rendering.TextureStreaming.BudgetMiB` accepts integer values from 1 to 8192,
defaults to 512, and is writable in Project, User and Session layers. World/camera
overrides cannot set it. Budget changes apply live and do not themselves reset
temporal history or recreate render targets; reaching the new target is asynchronous.
MechanicGirlLab's settings menu offers 128/256/512/1024 MiB and saves through the
shared `RenderPreferences` API. This controls each TextureManager, not a global
budget shared across all views.

## Diagnostics and verification

`TextureUploadStatistics` includes streaming target/planned bytes, tracked texture
count, transitions and budget stalls. Logs report published mip/dimensions and
periodic planned/resident/retiring payload totals. Development Agent builds accept
`HE_TEXTURE_STREAMING_BUDGET_MB` (integer 1–8192) for owned pressure tests. This is
not a saved player preference and is omitted from shipping code.

Fresh raster capture sidecars include `recordedRaster.textureStreaming`: target,
planned and resident/pending/retiring payload bytes, texture/transition/stall
counts, minimum-budget overrun, pending uploads and failed uploads. Descriptor
replacement and deferred-publication counters expose the heap-direct lifetime path.
The combined
accounting includes reserved replacements and is not measured physical VRAM.

For process-restart tests, `Tools/Agent/Session.ps1 -SavedConfigSource <directory>`
copies the source directory's JSON preferences into a fresh owned Saved/Config
before launch. The source remains unchanged. This permits a new process to load
the exact bytes written by the prior run without touching the user's Saved data.

Run the pure planner test through the standard headless runner:

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name TextureStreaming -Configuration RelWithDebInfo
```

The test covers non-square tails, BC block accounting, priority under pressure,
impossible budgets, mip clamping and empty demand. Real scene evidence, exact
commands and remaining failure/traversal/quality checks are recorded in
`Docs/FSRStreamingCombatWork.md`.

`Run-Test.ps1 -Name DdsTextureData -Configuration RelWithDebInfo` additionally
checks partial DDS file reads for every supported BC/RGBA16F format, legacy and
DX10 headers, exact payload/pitch/offset preservation, requested-mip clamping,
size-limit ordering, truncated/incomplete/missing sources and staged tail reuse.

The bounded `HEngineFsrSmoke` program also exercises the real TextureManager on
both APIs. Its generated 8192-square BC7 mip chain exceeds 64 MiB, demotes under
an 8 MiB budget, and recovers at 128 MiB. It verifies corrupt-reload preservation,
changed-dimension retry, missing-file fallback/hot-add retry, cancellation during
preparation and observed pending GPU submission, and cancellation of a live
reload. Final manager payload must return to the defaults-only baseline.

Development builds additionally expose handle-specific, one-shot
`InjectUploadFailureForValidation` while an asynchronous upload is preparing.
The smoke covers null texture/command-list/completion-query creation and rejection
immediately before submission. For DDS and ordinary TGA images, each boundary is
tested on both initial load and live replacement (16 cases per backend), followed
by successful retry and release. Independent empty-command queue markers must
have consecutive submission IDs around each rejected upload; this detects an
upload submitted before tracking allocation fails, even if manager counters are
unchanged. Synchronous ready loading is checked for both formats as well.

These are synthetic failure returns at production upload boundaries, not real
driver out-of-memory or device-loss injection. There is no new global idle wait.
The fixture uses no shader sampling or pixel comparisons. Assertions describe
residency/lifetime and actual upload completion, not image quality, driver cleanup
under real allocation pressure, or physical VRAM consumption.
