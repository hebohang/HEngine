# Runtime frame, task, and texture lifetime

## Frame ownership

The application main thread samples input once per displayed frame, updates UI and
layers, extracts RenderWorld, records RHI work, and submits the frame. The runtime
resource registry owns one or two IO/decoding workers; its finalizers run on
the Init thread. TextureManager owns one separate IO/decode worker. Large
RmlUi images share one bounded decode worker on hosts with at least eight
logical processors. Jolt owns
a separate job pool. `PlanRuntimeCpuBudget` reserves the main thread and the
texture worker, assigns one resource worker below six logical processors and
two otherwise, reserves the UI decode worker when eligible, then admits up
to three task workers and the remaining Jolt
workers. On three or four logical processors the plan has no task workers;
Jolt also has none on three. On one or two processors the resource and texture
IO workers remain available, so thread count can exceed core count while
those workers block on IO; CPU task and Jolt worker counts are zero. Task workers do not
call EnTT mutation, script, UI, Jolt queries, NVRHI, or callbacks that require
the main thread. HE_TASKS_SERIAL=1 keeps its tasks on the caller.

| Stage | Thread | Reads | Writes | Barrier and lifetime |
| --- | --- | --- | --- | --- |
| Input sample | Main | Window events | Input state | Before Level update; no worker holds input objects |
| Fixed behavior tree | Main | Prior completed Jolt state, EnTT, blackboard | Gameplay and desired movement state | PrePhysics in every fixed tick; authored tick interval retains fractional time |
| Fixed physics | Main plus Jolt pool | EnTT physics components, Jolt bodies | Jolt state, contacts | Jolt update and contact flush finish before character movement |
| Fixed character movement | Main | Latched input, completed Jolt state | Character and transform components | All ticks finish before display-rate systems |
| Fixed Python gameplay | Main | Completed Jolt state, character transform, latched input | Gameplay state, EnTT | Optional `FixedUpdate` runs after character movement in each tick; callbacks complete before display systems |
| Display script, sequence, animation | Main | EnTT, assets, physics queries | EnTT and gameplay state | Existing system order remains the barrier; animation raycasts stay on main |
| Pure targeting partitions | CPU task workers | Immutable target sample span | Disjoint validity and cell arrays | Stable caller merge finishes before spatial queries; no Level pointers escape |
| Large morph vertex partitions | CPU task workers | Immutable morph deltas and resolved weights | Disjoint ranges of one mesh vertex stream | Main thread owns mesh and waits at the graph barrier before bounds and GPU publication |
| Mesh index conversion | CPU task workers called by registry IO worker | Validated index bytes | Disjoint destination ranges | Registry finalization waits; no RHI call on workers |
| RenderPrepare | Main | Current components and render cache | RenderWorld snapshot and history caches | Runs after display-rate scripts and 2D systems, before RHI recording; GPU IDs stay on owner |
| Display UI | Main | Current scene and input state | UI state and draw commands | BeginFrame precedes Level; Update follows RenderWorld submission; GPU overlay recording happens later in Application |
| Texture CPU preparation | Dedicated texture worker | Validated absolute asset path, immutable settings | Owned decoded bytes and mip chain | At most two new preparations per Collect; no RHI or Image span escapes |
| Large RmlUi image decode | Shared dedicated UI worker | Owned encoded bytes and cancellation token | Premultiplied RGBA pixels in a future | Four outstanding jobs and 16 MiB encoded permit limit through publication or release; owner polls at BeginFrame and uploads only after decode |
| Texture RHI upload | RHI owner | Prepared bytes | Command list, GPU texture | At most four submissions and 32 MiB per Collect; GPU query precedes publication |
| Texture retirement and scene unload | RHI owner | Submission/query completion | Cache, descriptors, retired slots | Final reference and submission/query barriers protect GPU resources |

The renderer's `Renderer2D` collector is thread local and belongs to the
calling frame. Animation also reads mutable asset state, performs foot
raycasts, and uses static statistics. RenderWorld extraction owns history
caches and GPU identities. None of those full systems are worker tasks.

RenderGraph owns the RHI resource-state transitions for declared pass
accesses. The directional shadow pass declares the light-grid buffers and
previous bone matrices present in its shared binding sets, in addition to
mesh, material, and depth resources. NVRHI automatic barriers remain enabled;
the tested manual-barrier override showed no repeatable CPU gain.

Runtime prefab instantiation stays on the Level owner thread because it
mutates EnTT and may start scripts. Each Level caches the expanded and
canonicalized template of a non-nested prefab by absolute path and
runtime-resource generation. Instances still receive fresh UUIDs and
components. A new generation replaces the prepared template; nested prefabs
remain on the uncached dependency-expansion path so a changed child cannot
silently reuse a parent's old expansion. The cache dies with its Level.

The Level runtime system order remains the authority for display-rate work,
except that `RenderSystem3D` runs last as the explicit RenderPrepare stage.
Native, managed and Python scripts can therefore change a transform that enters
the RenderWorld snapshot submitted in the same displayed frame. Startup and
shutdown retain the original system order. Tracy names `PrePhysics`, `Physics`,
`PostPhysics`, and `RenderPrepare` mark the main-thread stage boundaries.
The runtime frame dependency chain is: input sample → UI BeginFrame → zero or
more (PrePhysics → Physics → PostPhysics → FixedGameplay) ticks → ordered sequence/animation/
camera/script/2D updates → RenderPrepare → immutable RenderWorld submission →
UI Update → RHI scene and UI recording → GPU completion and retirement.
`FrameStageGraph` now validates and executes the fixed-tick chain and the
display-system → RenderPrepare edge on the Level owner thread. Its edges are
built before any callback runs; a cycle or foreign handle is rejected, and a
throwing callback prevents later stages. The owner graph has no worker pool:
an audited pure-data stage may call the CPU `TaskScheduler` and join its work
before dependent ECS, script, physics, or render stages execute. The fixed
graph completes on every tick before the display graph starts. At most 32
owner stages fit in one graph, keeping construction and topological validation
bounded without a queue or background lifetime beyond the update call.
Texture CPU preparation can overlap that chain on its dedicated worker, but
RHI upload and frame-boundary publication rejoin on the owning thread.
The Survivor loading screen also preloads the enemy projectile and zone
telegraph Effekseer packages one per displayed warm-up frame, before combat
can emit their first event. This moves their synchronous package-open cost
to the existing bounded start-up sequence, not to the CPU task workers.
It also instantiates one hidden projectile visual per selected starting
weapon kind, one per loading frame, and stores it in the existing pool.
Prefab creation stays on the Level owner thread; a weapon obtained later
may still incur its first visual creation during gameplay.
Its behavior tree, physics and character systems form a separate fixed-tick
stage before the other systems. Each tree reads the Jolt state completed by
the previous tick, then updates gameplay movement intent before Jolt advances.
The physics system flushes contacts before character movement reads the
resulting state. Tree intervals above one fixed tick retain the remainder
between updates. The optional Python `FixedUpdate(Timestep)` callback runs
after physics and character movement in each fixed tick. Existing `Update`
callbacks retain their once-per-display-frame meaning. `SurvivorGame` uses
`FixedUpdate` for combat, spawn, objective, pickup, and domain-power timers;
its display `Update` handles menus, audio, warm-up, and HUD once per frame.
An Escape press that will pause during display `Update` skips that frame's
fixed gameplay callback. RenderWorld extraction and UI
run once per displayed frame. A Level owns its accumulator and resets it on
runtime start/stop, so scene replacement cannot carry simulation debt.

HE_SIMULATION_TICK_HZ accepts 30..120 Hz; the default is 60 Hz. At most four
ticks execute in one displayed frame. Additional accumulated time is recorded
as dropped rather than chased indefinitely. Character movement samples Move,
Sprint, Crouch and Jump once per displayed frame. All fixed ticks in that frame
reuse those held values; a Jump press edge survives zero-tick frames and is
consumed by the next tick only. Pause and runtime stop clear the latch.
Unconsumed physical key and mouse press events also survive until the next
display-frame `InputActions` snapshot. A press and release within one display
frame produces one Button `Pressed` edge (and `Released`) even if the final
held state is false. UI-consumed events do not enter that latch, and context
changes clear it. `InputActionState.PressCount` and managed `ActionState.PressCount`
retain multiple presses delivered before one snapshot; a fire handler can
emit one shot per count. `Pressed` remains true whenever the count is nonzero
for existing callers.
SimulationClock fixed elapsed seconds and
fixed tick count report executed simulation work; its frame number and legacy
elapsed seconds remain displayed-frame concepts. The application reports fixed
elapsed time as gameplay simulation time.

## CPU task graph

Construct a TaskGraph on the calling thread. After(child, prerequisite)
publishes explicit edges; Execute checks for cycles before scheduling any
node. Independent graphs may submit concurrently; each caller waits only at
its own graph barrier. Workers take frame-critical jobs before normal and
background jobs. The bounded queue reserves capacity for frame work: normal
jobs can occupy at most three quarters and background jobs at most half of
the queue. This prevents a streaming graph from filling all admission slots
before a frame graph arrives. `QueuedTaskCount` exposes the current length.
With zero workers, concurrent submissions are serialized for deterministic
fallback.
Task handles carry a graph identity as well as a node index; After rejects
handles from another graph before any work is published.
Workers never wait for child tasks. Exceptions cancel unscheduled nodes,
finish already queued work, and rethrow on the caller. External Shutdown
waits for active graphs and joins workers; a task may request stop, with a
later external call completing the join. Graphs execute once.
`TaskSchedulerStats` reports aggregate queue and
execution time, measured dependency-and-worker-lane critical path, wall time, the wall-minus-critical
residual, and aggregate worker busy fraction. The residual includes queue
delay and caller work, so it is a scheduling upper bound rather than a pure
dispatcher cost. Set `HE_TASK_TIMINGS=1` for per-graph log records.

SurvivorTargetingBatch::Rebuild computes validity and spatial cells from a
read-only sample span in disjoint partitions above 32768 samples. It merges the
validated records into the hash table in input order on the caller. The mesh
resource loader converts large index arrays in disjoint background-priority
partitions after file IO and validation, then finalizes the mesh on the
registry's owner thread.
The latter runs from a registry worker, not from a real-time task worker.
Neither path transfers Level, component, Mesh, or GPU pointers into a task.
`SubMesh::ApplyMorphWeights`, called by the runtime animation update, resolves
weights on the main thread and partitions at least 65536 vertices with two
active morph targets into 8192-vertex ranges. Workers read immutable morph
streams and write only their own vertex ranges. The call joins before bounds,
deformation revision, and GPU publication; mesh and target lifetime are pinned
by the owning call. `HE_MORPH_TASKS_SERIAL=1` forces this path to use its serial
loop independently of `HE_TASKS_SERIAL=1`.

Animation controller and legacy `.hanim` file reads and JSON parsing also
use `RuntimeResourceRegistry` workers for dynamically spawned entities.
Scene startup retains synchronous pose prewarming. Jobs capture asset paths
only, pin their generation in the Level resource group, and publish parsed
data through the registry's main-thread pump. Skeleton checks, animation
object construction, component writes and mesh pose publication remain on
the main thread. `HE_ANIMATION_PREPARE_ASYNC=0` selects the old serial path
for comparison. The resource workers may perform blocking file IO; the
real-time TaskScheduler workers never do.

The Survivor Region pressure script counts local alive agents using the
existing native targeting snapshot after matching entity ID, activation ID,
alive state, and encounter ID back to the script's monster registry. The
targeting snapshot is rebuilt on the main thread when native agent movement
marks it dirty; this query does not run Python or EnTT work on task workers.
The vertical range covers the full finite float position range because the
Region rule measures horizontal distance only. Set
`HE_SURVIVOR_LOCAL_ALIVE_VERIFY=1` in a bounded game run to compare each
native result with the original script enumeration and fail on a mismatch.
`HE_RUNTIME_SYSTEM_TIMINGS=2`, `HE_SURVIVOR_STAGE_TIMINGS=1`, and
`HE_SURVIVOR_OBJECTIVE_TIMINGS=1` provide opt-in fixed-system, script-stage,
and objective-stage timing logs respectively; they are off in normal runs.

## Buffer and delegate

Buffer owns only memory adopted with an explicit deleter or allocated by
Buffer::Allocate. Deleters must be `noexcept`, matching the nonthrowing
`Release` and destructor; Image wraps `stbi_image_free` in a nonthrowing
deleter. Borrowed bytes use std::span. Move assignment releases
the old allocation; `Write` bounds use subtraction and `Get<T>` checks the
index with division before multiplication, avoiding arithmetic overflow.
Image adopts stb pixels with stbi_image_free, uses stb's thread-local
vertical-flip setting, and exposes only a const pixel span. That span is valid
only while the Image is alive. NVRHI's DX12 and Vulkan writeTexture
implementations copy CPU pixels to their upload buffers during recording.
The RGBA8 and RGBA32F CPU mip decoders reject zero or overflowing dimensions
before multiplying width, height, and channel bytes. A Dev Agent-only CPU
validation helper returns the production RGBA8 mip generator's encoded bytes
so headless tests can inspect alpha coverage without an RHI device.

The RmlUi NVRHI backend keeps geometry and texture upload bytes in its retired
entry if a callback releases a handle before that frame is submitted. Retired
resources are collected only after every frame slot has been reacquired past
the relevant submission. Small geometries now suballocate a matching vertex
and index range from a pair of 1 MiB GPU buffers. The arena is bounded to
eight page pairs (16 MiB total). A released range returns to a sorted,
coalescing free list only at the frame-slot retirement barrier. Live compiled
geometry keeps its range and generation-specific handle; draws and uploads
use that range's buffer offset. Pages remain owned by the backend until
shutdown. Geometries above 256 KiB per stream, or beyond arena capacity,
retain the individual-buffer path.

That fallback path also keeps a bounded 16 MiB idle pool for released small
buffers, with at most 256 KiB per buffer. It searches the 256 most recent
free slots for a fitting pair. All allocation, command recording, and
retirement stay on the RHI owner thread. `HE_RML_RECORD_TIMINGS=1` adds
default-off aggregate timing and allocation counters for slow overlay
recordings. Neither allocator makes RmlUi document updates safe for workers.
For encoded UI images of at least 512 KiB, `LoadTexture` reads metadata and
allocates an RHI texture on the owner thread, then queues only stb decode and
premultiplication. While pending, Rml draws through a transparent 1×1 texture;
the full pixels enter the existing upload and frame-slot retirement path on
the first BeginFrame after decoding. The worker holds encoded bytes and a
cancellation token, never a backend slot or RHI pointer. Releasing a handle or
shutting down cancels pending work; a completed result is applied only to the
still-owned slot. On low-core hosts, queue backpressure, `HE_TASKS_SERIAL=1`,
or `HE_RML_ASYNC_DECODE=0`, the old synchronous decode path is used.
`HE_RML_RECORD_TIMINGS=1` logs queue, ready latency, and any fallback;
`HE_RML_ASYNC_DECODE_HOLD_MS` is a bounded diagnostic delay for cancellation
validation. This Rml path is separate from TextureManager's material texture
state machine and does not call RHI APIs on the worker.

Multicast delegate subscription management is synchronized. Each broadcast
copies a subscriber snapshot. Additions appear in the next broadcast,
including a nested one. Removals skip callbacks not yet entered. Callbacks run
outside the lock; exceptions propagate without retaining broadcast state.
Concurrent callbacks must synchronize their own state. A raw AddMember
target must outlive any in-flight callback; AddWeakMember is available for
shared objects. Move-only by-value and rvalue-reference multicast arguments
are rejected at compile time.

## Texture publication

The TextureManager owner thread handles NVRHI command recording, GPU submission,
completion polling, and descriptor publication. A dedicated worker reads DDS
or ordinary images, decodes/resizes/generates mips, and returns owned bytes.
At most 32 preparations are queued or running, 128 requests await preparation,
and 128 MiB of completed CPU bytes are retained by default. Set
`HE_TEXTURE_PREPARED_BUDGET_MIB` to 1–512 to tune this queue gate. Once the
retained total reaches the gate, the decoder waits until frame publication or
cancellation releases bytes; one decode already in progress may overshoot it.
An explicit ready request can take its still-waiting job onto the RHI owner
thread so that it cannot deadlock behind this gate. The worker starts at most two
new preparations per frame Collect, with at most four credits carried forward.
The owner submits at most four prepared textures and about 32 MiB in one
Collect; one oversize texture may use that Collect alone. A cache hit
shares the pending handle and its reference count. Newly submitted DDS and
ordinary images bind the default texture until a graphics queue event query
reports completion. Collect publishes the actual texture and descriptor at
a frame boundary. A released pending texture keeps its GPU resource and query
until the upload completes; a released live texture follows the existing
submission retirement rule. Default textures still use one startup GPU idle
wait. Cancelling a preparation that has not started removes it from the
worker's waiting queue immediately, releasing its queue slot for the next
scene. GetUploadStatistics exposes request, submission, completion, pending
byte, prepared-byte budget, decoder budget stalls, failure, CPU-preparation,
peak-queue and global-wait counters, plus request-to-publication mean and
rolling 256-completion nearest-rank P50/P95/P99/max latency.
`peakPending` is updated when an asynchronous request is admitted as well as
at Collect, so a burst created and consumed between two Collect calls remains
observable. The Agent-only `render.texture.burstProbe` can exercise distinct
cache keys on a real backend; its ready leases intentionally drain pending
work on the owner and are not a frame-paced streaming benchmark.
The Agent-only `render.texture.streamingProbe` instead retains async handles
between calls: begin requests distinct variants, status observes only the
published resource after normal frame Collect calls, and end releases them.
An active probe is also released before renderer shutdown.
The begin action accepts `baseDimension` to vary each cache key. The
`PreparationBudget.png` fixture has a 1 MiB decoded RGBA mip while remaining
quick to decode; with `HE_TEXTURE_PREPARED_BUDGET_MIB=1`, it exercises the
worker's prepared-byte gate on a real device without a ready lease.
The Dev Agent command `render.texture.readyProbe` requests one texture
asynchronously, asks for the same cache key with `bRequireReady=true`, and
releases both temporary references. For deterministic device coverage, set
`HE_TEXTURE_PREPARATION_HOLD_PATH` to that exact project-relative asset path
before starting the Agent; the decoder then leaves only that path waiting so
the owner can claim it. The command and hold hook compile only with
`HE_DEV_AGENT_RUNTIME`. `ownerClaims` counts successful claims.
The probe also accepts optional `colorSpace`, `generateMipmaps`,
`preserveAlphaCoverage`, `alphaCoverageCutoff`, and `maximumDimension` fields,
and returns the published width, height, mip count, and NVRHI format name.
These fields let device checks cover distinct cache keys and upload settings;
the probe releases both leases before returning.
Optional `readbackMips=true` creates a separate cache entry whose texture uses
tracked RHI states, then copies each RGBA8 mip to bounded staging memory and
waits only for that copy's event query. It returns tight-row byte hashes and
alpha-covered pixel counts for at most 16 MiB. This explicitly blocking Agent
validation path never changes ordinary texture cache keys or calls device
idle; normal textures retain their permanent ShaderResource state.
Environment IBL now requests its HDR with `bRequireReady=false`. After the
source publishes, the owner submits cubemap/irradiance/prefilter work and
polls its Graphics query in later frames. Until that query completes, the
source lease, command list, and compute binding sets remain owned by the
environment entry; only then are the derived textures published. A scene
change cancels unused CPU preparation, lets submitted IBL finish without
publishing it, and retires old ready maps after their last submitted frame.
An explicit HDR reload invalidates the cached derived maps, retains old maps
through the last submitted frame, and requests fresh source preparation. If
IBL is already submitted, its event query must complete before the old entry
is discarded and the same path is requested again. Failed source cache entries
are cleared for this explicit retry.

For a device lifecycle test, set `HE_ENVIRONMENT_IBL_QUERY_HOLD_PATH` to one
normalized HDR asset path, `HE_ENVIRONMENT_IBL_QUERY_HOLD_FRAMES` to a positive
preparation-frame count (at most 1000000000), and
`HE_ENVIRONMENT_IBL_QUERY_HOLD_AFTER_FIRST=1`. The first submission then
publishes normally; a later reload holds its completed query until that count
or cancellation. The hold delays CPU polling/publication only; it never delays
GPU execution. Cancellation bypasses the hold and polls the query so the
abandoned resources can be released. All three variables are unset in normal
runs.

`bRequireReady` callers still prepare synchronously if the asset has not been
requested already, then wait for that texture's query. Editor ImGui icons and
thumbnails now request asynchronously. Their stable ImGui handle initially
binds the managed texture fallback. Before the next ImGui CPU frame, Application
compares the published managed texture with the current binding and replaces
the NVRHI binding set in place. The old texture and binding set stay alive until
the last frame slot that could use them is reacquired. A failed request leaves
the fallback visible; a successful reload updates the same ImGui handle.
Ray-tracing material textures
request asynchronously: their material table first holds the fallback, then
`RayTracingScene::Prepare` observes the published managed texture and updates
the table generation. This rebuilds the offline binding set and material
record, including the BC5 normal-map flag, before sampling the new texture.
On a scene-generation change or RayTracingScene clear, it releases the old
managed texture leases using the last submitted frame for retirement, clears
the native table, and rebuilds it from the new scene.
Ordinary material and 2D primitive requests return a fallback immediately.

`RequestTextureReload` accepts a live asset path (or empty path for all live
textures), normalizes Windows path separators and case, and schedules fresh
CPU preparation while the existing handle and texture remain visible. The
replacement is published only after its upload query completes. The old GPU
texture is retained through the last submitted frame. A failed reload keeps
the old texture; another request follows the Failed → Preparing retry edge
with the same managed handle. An initially failed asynchronous request can
use that edge too. An explicit reload also clears matching failed-load cache
entries after scheduling live handles. This permits a later request to retry
an asset that failed before any handle remained, for example after replacing
a corrupt image at the same path. The reload count reports scheduled live
handles; zero does not mean that no failed cache entry was cleared. For a
project-relative reload, the failed-cache lookup checks validated
paths under both the project and engine content roots. This covers a source
that changes location while a failure is cached; an absolute reload checks
that absolute path only. Scene
unload cancels preparation and retires submitted uploads. The Agent
`render.texture.reload` command exercises the same owner-thread path. A
live pixel-change comparison and BC1, BC3, BC5, and BC7 DDS real-device
fixtures passed on DX12 and Vulkan. RGBA8 linear/sRGB, mip count, size cap,
and alpha-coverage settings also passed ready-probe and GPU mip-byte checks;
see `RuntimeTaskingResults.md` for the measured settings and visual limits.

## Design references

- Epic Tasks System: https://dev.epicgames.com/documentation/unreal-engine/tasks-systems-in-unreal-engine
- Epic Actor Ticking: https://dev.epicgames.com/documentation/unreal-engine/actor-ticking-in-unreal-engine
- Epic Physics Sub-Stepping: https://dev.epicgames.com/documentation/unreal-engine/physics-sub-stepping-in-unreal-engine
- Epic Threaded Rendering: https://dev.epicgames.com/documentation/unreal-engine/threaded-rendering-in-unreal-engine

These references guide dependency and lifetime choices. HEngine retains its
existing render submission thread ownership and Jolt pool.
