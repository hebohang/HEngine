# RenderGraph contents and allocation contracts

## Attachment contents

New color/depth passes should use `RenderGraphBuilder::ColorAttachment` or
`DepthAttachment` with `RenderGraphAttachmentOps`:

```cpp
// Blended overlay: preserve existing pixels (used by EditorOutline).
data.Color = builder.ColorAttachment(color, {});

// New complete fullscreen output: the execute callback must write every sample.
RenderGraphAttachmentOps output;
output.Load = RenderGraphAttachmentLoad::Discard;
output.bFullOverwrite = true;
data.Color = builder.ColorAttachment(color, output);

// Partial geometry on a new image: graph records the clear before the callback.
RenderGraphAttachmentOps clear;
clear.Load = RenderGraphAttachmentLoad::Clear;
clear.ClearColor = nvrhi::Color(0.f);
data.Color = builder.ColorAttachment(color, clear);
```

`Load` requires initialized contents and declares `ReadWrite`; `Clear` records a
real NVRHI color or depth/stencil clear for the selected mip/slice range. Depth
clear defaults to 1; reversed-depth callers must explicitly choose 0. `Discard`
with `Store` requires an explicit full-overwrite promise. `Store::Discard`
invalidates contents: later reads and exports fail compilation until a new
producer initializes the range. Discard is a contents contract, not a promise
that a native discard optimization is issued. One attachment declaration per
texture per pass is currently supported.

Legacy `Write`/`RenderTarget`/`DepthWrite` still declare production of the selected
range and conservatively retain prior writer dependencies. They do not request
native discard or enable dead-write elimination. Partial draws requiring old
contents must use `Load`/`ReadWrite`. The graph validates declared contracts; it
cannot prove a draw covers every pixel.

Initialization, hazards and observable export roots are tracked per mip/slice.
Writing one mip does not initialize its neighbors. Disjoint mip reads/writes are
legal; overlapping incompatible accesses and out-of-bounds ranges are rejected.
NVRHI owns state tracking and barrier emission.

## Pool policy and measurements

Buffer capability normalization preserves indirect, vertex, index, constant,
UAV and acceleration-structure-input creation flags, including compatibility
checks when selecting a cached allocation.

`SetTransientCacheBudget(bytes)` sets the budget for unused transient cache.
The default is unlimited, preserving the existing policy. `Collect(completed)`
may trim only unused allocation tails belonging to completed, non-recording
frame slots. Active working sets and in-flight work stay protected above budget.
Changing budget does not wait for the GPU. Resource handles retained elsewhere
may keep an allocation alive after the pool drops its reference.

`GetStats()` reports nominal texture/buffer bytes, unused cached bytes, retired
bytes, cache budget and cumulative bytes removed from the pool. These are not
actual heap residency or a hardware memory budget. Existing texture-object
reuse is not heap aliasing. Compare unlimited and a chosen bounded budget using
the same warmed workload, then a high-to-low configuration switch; record
cache/retired/trimmed bytes, allocation peaks, CPU recording, GPU frame span,
frame-time percentiles and next-switch allocation cost. Do not infer a speedup
from lower nominal bytes alone.

## Queue evidence

Create a `RenderGraphFeatureScope(graph, typeId, instanceId)` around a Feature's
pass construction. Passes capture both identifiers; nested scopes and exception
unwinding restore prior ownership. Empty identifiers describe core passes. This
is diagnostic ownership, independent of display names and resource dependencies.

Diagnostic `Queue` is the requested logical queue. `RecordedQueue` comes from
the actual NVRHI command list and is valid only with `bHasRecordedCommands`.
Recording does not prove submission or GPU completion. The executor records
into one supplied command list; it does not implement asynchronous multi-queue
submission, ownership transfers or fences.

## Regression commands

- `hectl.bat test RenderGraph -Parallel 2`
- `hectl.bat test RenderGraphResourcePool -Parallel 2`

These are CPU unit contracts, not GPU clear/resize/rendering evidence. Validate
production attachment recording with the controlled Game/Editor runtime routes.

## Profile cooking

HCooker recognizes `.hepipeline` without requiring a project extension-list
change. Selected profiles in project rendering settings and per-view overrides
are hard runtime dependencies. Missing custom profiles fail; built-in profile
names need no asset unless a matching asset exists. Unknown Feature payloads
are retained. Explicit `Dependencies`/`dependencies` string arrays anywhere in
Feature parameters establish hard asset dependencies; ordinary labels are not
heuristically treated as assets.

Renamed assets use `AliasTarget` containing a same-directory stem. Cooking
follows that target and preserves the original alias document. The retained
old settings do not add stale dependencies. Invalid names, missing targets,
cycles and chains exceeding 64 assets fail. Schema-zero `Values` and current
`Settings` are both recognized.

Build the HCooker target using `Tools/Agent/Build.ps1 -Target HCooker` and run
its `--self-test` for dependency/round-trip coverage. This is not shipping
package validation; that still requires the explicit package workflow.
