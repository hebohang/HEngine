# Vulkan20k02 CPU allocation audit

2026-09-14, source/log read only. No engine build, GPU session, OS trace, threshold change, or production edit. Evidence: `out/agent/rendering-evidence/vfx-authoring/lifecycle-vulkan-20k-02/report.json`. The strict private-memory result remains FAIL. This document identifies concrete allocations for a controlled follow-up; it does not assign the complete 4096-byte median difference to an unmeasured owner.

## Observed shape

Post-warmup cycles 5..10: 1084940288 then 1084977152 (cycles 6..10); cycles 11..21: 1085014016; cycles 22..28: 1084981248; cycle29:1084944384. Last-five minus first-five median is +4096. Values include both positive and negative committed-page transitions. Neither constant SDK pools nor a final decrease makes that criterion pass.

## Actual per-cycle allocation sites

1. `DevAgentRuntime.cpp:788` appends a retained `Handled <method>.` log for each mailbox request. `:2143` appends an additional message for every runtime.step. `AgentEventLog.cpp:35..45` constructs four strings and pushes a deque Entry before popping the oldest when size exceeds capacity. Default capacity is 256 (`AgentEventLog.h:21`), message limit1536 (`:41`). This is bounded entry count, not fixed storage: every saturated insertion still allocates a new entry/string and frees the old. The engine/client sink (`AgentSpdlogSink.cpp:19`) writes into this same container, so retained string sizes also depend on log mix.

   `lifecycle_acceptance.py:398..412` sends 11 calls per cycle: 3 stats, 2 play, 3 step (120+81+30), 1 control status, 2 stop. Thus at least14 event appends/replacements per cycle, 420 over30 cycles, independently of GPU allocator ownership. Existing output has99 stats,178 step,153 control calls over the full session. A fixed maximum count must not be used to claim zero byte growth.

   Bounded fix candidate: retain identical256-entry/1536-byte semantics but initialize a fixed ring of entries and store bounded message/source/logger/level data in fixed storage, copying into the selected slot. Alternatively pre-reserve per-slot bounded strings once, then overwrite without creating temporaries. Preserve cursor ordering, truncation and locking. Do not clear or disable logs during acceptance. Before choosing this change, expose or locally count live string capacities, ring/deque allocation count and total appended bytes; capture at identical cycle sample points.

2. `DevAgentRuntime.cpp:791` fully serializes each response with `response.dump(...)` only to test its length, discarding that string. `FileMailboxAgentTransport.cpp:280` then serializes the same object again to write the response. Each vfx.stats therefore creates two complete JSON buffers plus its JSON object tree; this happens3 times per lifecycle cycle. These are real temporary allocations, not a retained response history. Their deallocation is not evidence that the CRT returns committed pages immediately.

   Bounded fix candidate: serialize once, enforce the same maximum-response size on that exact byte string, and pass it to the mailbox write operation. Preserve structured error handling for oversize output. A reusable transport serialization buffer with a bounded retained capacity can remove repeated growth; do not remove request/output limits. Measure serialization buffer capacities and allocation counts before/after. This improves a demonstrated allocation path but needs the unchanged acceptance run to establish whether it affects the observed pages.

3. `EffekseerRuntime.cpp:2334` inserts each fresh handle into ActiveEffects with a package-path string; `:1454/:1473` erase entries on stop/reclamation. SDK `Effekseer.Manager.cpp:2114` creates InstanceGlobal on every Play; `:232` deletes it in GCDrawSet. These objects are outside the exact GPU reserved-particle counters. There is no evidence in the inspected path that they stay alive after30 updates, but they cause real per-play heap churn. Add cumulative created/deleted and live ActiveEffects node/path capacity counts before proposing pooling. Do not relabel GPU allocator-zero as coverage for these CPU objects.

## Excluded false leads and limits

- FileMailbox responses are serialized to disk and requests archived; the response Json is loop-local (`DevAgentRuntime.cpp:772`). No growing in-process successful-request/receipt map was found. `mRequestReadFailures` is only the retry map and erases on success/final rejection; disk file counts are not process-memory ownership.
- Same-key effect/package/PlayedEffectPaths caches do not add a new path every replay. Do not disable those caches to chase monotonically increasing public handles.
- RenderDevice completed-timing vector is swapped out by TakeSubmissionTimings (`RenderDevice.cpp:240`), consumed in Application (`Application.cpp:1509`); not an unbounded retained frame-history explanation. Swap-out can itself create recurring vector allocations; verify timing mode before prioritizing.
- Python report samples, handle set and retained screenshots are in the driver process. owned_memory samples the Game PID, so driver accumulation cannot explain its PrivateBytes.
- No allocation trace or live container byte telemetry was collected in this audit. Exact attribution of the4096-byte difference remains unproven. The next admissible result is a source-location allocation/capacity observation followed by the same strict criterion, not a larger warmup, reduced diagnostics, or tolerance.
# 按需主线程观测（2026-09-14）

Game21 的原30循环 DX12 20k 验收仍 FAIL：privateBytes 峰值比第6个样本增加73728字节，斜率为正；完整序列保存于 `out/agent/rendering-evidence/vfx-authoring/lifecycle-game21-full-01/dx12-20k/report.json`。这是进程代理指标，不是分配栈或泄漏归因。

新增开发 Agent `runtime.memory`：`{"sample":true}` 请求下一次 Tick 在 Poll 前、PublishStatus 后各采一次；`{}` 查询最新完成的 `completedSequence` 与 `beforeMailbox`/`afterMailbox`。尚未完成时两者为 null，pending 时重复请求返回 accepted=false；平台不可用仍完成序号并报告 availability/error，不能把计数0解释为已测得0。只在明确请求时调用系统查询，普通生命周期验收没有新增采样。

平台返回进程 private/working-set 和默认堆 allocated/committed/reserved；Windows API 留在 WindowsPlatformProcess.cpp，HeapSummary 动态解析以兼容没有该入口的系统。各查询先后发生、其他线程继续工作，默认堆不包含其他堆和直接/驱动分配；这不是原子总账。原外部精确PID/start-time样本、30轮/5轮预热与零增长门槛保持不变。诊断新增请求本身会扰动分配，不能与原序列合并制造 PASS。

build-only `.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2` PASS53.5s，Game22 SHA B01C09A0099E811C6A321A173B88EEE9C7FAC19A2FBE4331557C7BB2D85FD479。实际按需诊断运行待执行；编译成功不证明任何内存原因。
