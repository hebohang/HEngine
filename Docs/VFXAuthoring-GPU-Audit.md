# GPU 粒子接入审计与最小补丁

日期：2026-09-13。范围：本地 Effekseer 1.80.6、随附 LLGI、HEngine Vulkan external-command-list 接入。未升级依赖。此记录中的源码审计与补丁不是 GPU 运行通过证明；主实施记录负责最终双后端验收。

## 源码结论

- `ManagerImplemented::Compute` 按 `nextComputeCount_` 调用 `GpuParticleSystem::ComputeFrame`；clear / spawn / update 经 `GraphicsDevice::Dispatch` 直接记录 native dispatch。
- `ManagerImplemented::Draw` 最后调用 GPU `RenderFrame`，经 `GraphicsDevice::Draw` 直接记录 draw。CPU standard renderer 的批处理在 EndRendering 才 flush，但“GPU emitter draw deferred flush 导致不可支持”的旧说明不符合这条当前调用链。
- 引擎 `ConfigureGpuParticles(true)` 原先直接拒绝，GPU factory 仅用 detector。恢复系统与 factory 由主实施负责，不能仅改 capability 开关。
- 原 Vulkan `BeginFrame` 已开始 native render pass，之后 Runtime 才 Compute，导致 compute dispatch、上传和部分 barrier 位于不合法的 render pass 范围。
- LLGI `BufferVulkan::ResourceBarrier` 构造 buffer barrier 后传入 count=0，实际没有记录 buffer dependency。
- LLGI UniformBuffer 原先直接覆写同一 MapWrite 分配。clear/spawn、多模拟子步、多视图及在途帧会持有相同 GPU 地址；命令对象引用保活不等于内容快照。
- DX12 当前已有 UAV barrier、SRV/UAV 转换与每录制 descriptor bank；本次没有发现需凭猜测新增的 DX12 原生同步补丁。两后端都受 UniformBuffer 覆写影响。

## 实施补丁

1. `VulkanEffekseerInterop.cpp`：BeginFrame 只准备并绑定命令列表；BeginRendering 先通过 backend Begin/EndComputePass 刷新排队上传，再开始 native render pass；EndRendering 先 flush renderer 再结束 native pass；EndFrame 对异常路径仅在实际开启时结束 pass。
2. Effekseer Vulkan `BeginCommandList` 增加默认 true 的 `insideRenderPass` 参数，旧源码调用保持兼容；新增 `BeginExternalRenderPass` / `EndExternalRenderPass` 只调整 LLGI 外部 pass 状态。引擎传 false，以明确分离 compute 和 graphics。
3. LLGI Vulkan buffer barrier count 改为 1；每次 Dispatch 后立即将绑定 storage 过渡到 ShaderRead，使 compute-to-graphics dependency 留在 native pass 外，后续 compute 写会重新过渡。未建立独立 queue 或旁路提交。
4. LLGI UniformBuffer 维护 CPU 字节副本和 backing 快照池；只复用 intrusive reference count 为 1（池自身持有、无 LLGI 录制引用）的 buffer。部分更新保留其他字段；录制仍读取自己的稳定快照；池随 UniformBuffer 析构释放。池达到最大在途需求后复用，资源回收/压力峰值必须通过运行验证。
5. LLGI CommandList 增加单调 `GetRecordedDrawCount()` / `GetRecordedDispatchCount()`，在 native backend 成功走到基类记录末尾时递增。它们表示录制成功，不表示 GPU 完成。
6. GPU system 增加单调 `GetDispatchRequestCount()` / `GetDrawRequestCount()` / `GetVertexRequestCount()`；补齐 GPU Draw 对 renderer aggregate draw/vertex 统计的贡献。请求计数需与成功录制计数和 GPU 执行证据区分，不能用 CPU 粒子数量估算冒充 GPU readback。
7. 公共 `EffekseerRenderInteropStatus` 增加 `RecordedDrawCount` / `RecordedDispatchCount` / `RecordingSerial`；两后端在 BeginFrame 采基线、EndFrame 保存本次成功录制差值并递增 serial。字段不是累计总 draw，也不代表提交完成；异常清理可能完成一次 partial recording，调用者必须结合 RenderBackendStatus 判断。

## 接口与语义

Runtime 的 simulation 段使用 `BeginComputeFrame -> Manager::Compute -> EndFrame`，view 段使用 `BeginFrame -> BeginRendering -> Manager::Draw -> EndRendering -> EndFrame`。Application 在所有 OnPreUpdate（Agent step/场景切换）之后、Layer Update 之前调用 `RuntimeRenderer::BeginSimulationFrame`，建立真实帧槽和打开的 NVRHI command list；RenderFrame 重用并提交同一 context。异常 RAII 调用 AbortSimulationFrame。无主摄像机也执行当前 Level 的 VFX Update；每次 Update 的 Compute 由主代理 Runtime/RenderGraph 实现立即录制，不把多个 Update 压成最后一份参数。失败录制不能自动推断已经执行或可安全重播。

两个 interop 的 `BeginComputeFrame` 均不需要 framebuffer，复用每帧逐记录 lease；Vulkan 不开启 native render pass，结束 compute-only 段时在 pass 外完成上传/屏障。RenderFrame 兼容旧直接调用者，未显式开 simulation scope 时自行 Begin。窗口/offscreen resize、AA/GI/settings/GI history reset、main offscreen enable 和视图销毁在已开帧时保存值/handle，下一 Begin 前执行；销毁时再次验证 handle generation。延迟的配置拒绝写入错误日志，设备运行错误仍传播。显式 idle 和 shader refresh 仍要求闭帧（Editor shader watcher 原本在 OnPreUpdate 执行）；不伪装等待已完成。Shutdown/Clean 先 Abort 未提交帧再等待设备。Runtime Configure 的 GPU 重建与失败后 desync 策略由主代理接口负责。

自动 World override、环境和反射探针资源准备也在 Begin 前执行：普通 Application 路径使用上一帧收到的 RenderWorld，因此新 World 的渲染覆盖/环境准备延后一帧生效，模拟与变换仍逐 Update 录制。不能在已录制 VFX Compute 的 host list 活跃时执行 `PrepareConfigurationGraphTransaction` 的 idle/旧存储重用。旧直接 RenderFrame 调用者仍用当前输入准备后自行 Begin。

GPU 实例颜色并非完全不支持：`Effekseer.Instance.cpp` 在 RenderColor.ColorInherit 为 `BindType::NotBind_Root` 时将 GlobalColor 传给 GPU SetColor，其他继承模式使用 ColorInheritance。能力描述应约束到已验证的配方继承模式；不能宣称任意 GPU 资源支持全局颜色修改。修改在下一次模拟更新应用，暂停时的即时显示必须单独验证。

主代理 Runtime 审阅建议：GPU 缓存命中必须仍检查当前 enable 状态；关闭/重建后清空 pending compute；多次 Update 无 Compute 会覆盖 SDK 下一次模拟数据，不应称为逐步精确预览；录制失败时 CPU emitter 已推进，不能只重配 interop 接着复用旧 GPU 状态。SDK `SetRandomSeed` 仅更新当前随机对象，没有同步保存到 DrawSet.RandomSeed，首次 Play 后设置可用，但 SDK seek/reset 会回退旧种子；新 Play 加固定步进是当前可验证的预览路径。

## 验证状态与剩余检查

### GPU allocator 失败与保留槽遥测（本地 1.80.6 补丁）

`Effekseer::GpuParticleSystem::GetAllocationStats()` 返回同一锁下的一致 CPU 账目快照：

- `EmitterSlotFailureCount`、`ParticleBlockFailureCount`、`TrailBlockFailureCount`：每个 GPU system 对象生命周期内单调累计，分别对应 NewEmitter 的槽耗尽、粒子连续块失败、trail 连续块失败；也有对应 `Get...FailureCount()` getter。
- `AllocatedEmitterCount`、`ReservedParticleCount`、`ReservedTrailCount`：成功创建 emitter 后持有的精确分配槽数量，FreeEmitter 完成原归还逻辑后扣除；不是 GPU 活粒子、GPU 分配字节或 fence-retired 资源读回。

NewEmitter 分配 trail 失败仍先退还粒子块，不增加成功分配账目；所有原 InvalidID 返回、free-list 操作和释放顺序保持原义，不抛出新的业务异常。计数采用独立 mutex，避免 getter 再次获取已有 allocator mutex 导致重入死锁，也覆盖 ComputeFrame 释放路径；快照读取不会改变原分配行为。这不宣称修复 SDK 其他并发分配约束。

Runtime 应在 GPU system 重建时记录新的对象代次；跨代不能直接相减单调计数。一个有效 Play handle 不证明后续 GPU emitter 成功分配，验收需比较相应运行窗口的三类失败计数增量，并检查停止后的保留槽账目恢复。静态容量预检不能覆盖连续块碎片、同时播放与延迟生成。

本补丁仅完成静态 diff 格式检查；未自行构建或启动 GPU，主代理负责集成验证。

- 静态检查：`git diff --check -- Engine/Source/ThirdParty/Effekseer Engine/Source/Runtime/Platform/Interop/Vulkan`，PASS（只检查补丁格式，不是 build-only 或 runtime 测试）。
- build-only：尚未执行；由主代理统一构建当前 Game / Editor。
- runtime-manual / GPU：尚未执行；必须分别检查 DX12、Vulkan validation、非零成功 dispatch、多个时间点图像变化、CPU/GPU 共存、停止与重播、缩放、场景切换、多在途帧和资源稳定性。
- GPU model 首次使用的 lazy upload 路径已前移到 ComputeFrame update dispatch 之前；Sprite/Trail 的内部模型在 InitSystem 预创建。该路径仍需额外运行覆盖，不能将静态修改视为模型粒子通过。
- 每个 backend 的 GPU capability 只能在其最终验收通过后标为 verified。以上补丁尚不构成性能或视觉通过证据。

## Game07 Vulkan rejected-load mipmap lifetime failure (2026-09-13)

Game07 parameter acceptance is **FAIL**: 38 checks did not assert a failure, but
the owned process ended with DeviceRemoved. Successful command assertions cannot
override process/device failure. Evidence: session
`out/agent/sessions/20260913-225341-790-b9cdf36a4f98/game.stdout.log` and stderr.
The earliest validation error (stdout line 33, 22:53:49) says vkCmdBeginRenderPass
was added to command buffer `0x185dc0ea370` already invalidated because referenced
VkImage `0xc5283b00000003e0` was destroyed. Later vertex/index/timestamp/barrier
errors on that buffer are consequences, not independent first causes.

The bounded log shows GPU-disabled initialization, GPU package
explosion-ember loaded/rejected at 22:53:46, then CPU fallback explosion-ember-low
loaded at 22:53:48. Source establishes the matching lifetime chain:

1. LLGI GraphicsDevice texture initialization queues mipmap generation, retaining
   a shared texture reference (GraphicsDevice.cpp QueueMipMapGeneration).
2. Rejection releases the effect's texture owner. The mipmap queue can now be its
   last owner; delaying until the next CPU draw is valid only if ownership passes
   to the recording command list.
3. VulkanEffekseerInterop BeginRendering flushes pending mipmaps outside the
   native render pass, including GPU-disabled CPU-only operation.
4. CommandListVulkan::GenerateMipMap previously recorded barriers/blits but did
   not RegisterReferencedObject(src). FlushPendingMipMapGenerations then cleared
   its queue, releasing the last reference; TextureVulkan's destructor immediately
   destroyed the image. The next vkCmdBeginRenderPass reports an already-invalid
   buffer. The report does not prove that the destroyed image was a framebuffer
   attachment: it can be any earlier referenced image, including the mip source.

The minimum patch registers src only when RecordMipmapGenerationCommands returns
true. It does not suppress loading, skip mipmaps, change Runtime rejection or add
waitIdle. RegisterReferencedObject adds an LLGI strong reference. EffekseerFrameResources
retains each recording's command list in a per-host-slot bank; RecordLeases prevents
reuse until LastSubmission <= completed. EndFrame does not clear those references.
Abort closes the lease without discarding its storage or previous submission fence;
the host then discards the unsubmitted native list. A later safe record reuse or
shutdown after idle releases the retained references. This also covers rejected or
unloaded resources that never reach a draw binding.

Interop framebuffer cache entries already own NVRHI FramebufferHandle, texture
entries own NVRHI TextureHandle, and active handles persist through EndFrame.
This audit found no missing ownership in those paths explaining the first error.
DX12 GenerateMipMap already registers src after successful recording; it is left
unchanged. Its descriptor-allocation early-error branches occur before that final
registration and are a separate error-path consideration, not demonstrated by
this Vulkan run.

Patch-format check: `git diff --check -- Engine/Source/ThirdParty/Effekseer/src/3rdParty/LLGI/src/Vulkan/LLGI.CommandListVulkan.cpp`, PASS.
No engine build or GPU run was performed for this patch. Root will build Game08
and repeat the same GPU-disabled rejected-package -> CPU-fallback sequence with
validation; only a clean process completion can close this regression.
# Vulkan early abort: unsubmitted NVRHI ownership cycle (2026-09-13)

## Early08 sampler admission and exception flush (2026-09-14)

Actual stderr established that old stress version 1800 reaches renderer-common
16 bytes early: Depth occupies BIN 671..702, Kill None/scale flag occupies 703..710,
but an additional 16 bytes remain before actual renderer-common at 727. Current
`CollisionsParameter::Load` returns immediately below 1801. The loader therefore
reads common at 711; its correct relative sampler offset 40 reads texture-index
values -1. COPY, NODE_LOADED and COLLECTOR agree (BasicParameter size 200, filter
offset 52, wrap offset 84). This is not sampler creation, Vulkan-Hpp ABI or a
post-load overwrite. The sample prefix differs only in seed/lifetime from the
official old sample; current Parameter/ForceField loaders have no local edits.

Runtime now rejects invalid public CPU built-in sampler enums before candidate
publication, preserving the existing load/reload cache transaction. No blanket
1800 rejection or guessed legacy collision parsing is introduced. The original
bad stress package is retained separately while authoring migrates to actual
official compilation of matching editable source.

The same failed draw exposed map(size=0)/unmap VUIDs during cleanup. StandardRenderer
had already uploaded vertices, then SetTextures threw; pending renderInfos remained
and EndRendering retried them. A second Upload had no new bytes. Both shared vertex
buffer upload implementations now skip zero-byte UpdateData, and pending batches
clear by RAII on success or exception, preventing replay of partially recorded
draws. These fixes handle the failure path itself; admission rejection is not used
as its substitute. Preparation evidence is static diff/help checks, not GPU PASS.

`early-vulkan-01` built successfully but failed at its first no-camera update:
the standalone probe omitted Level's Renderer2D collector scope. That smoke-only
omission is fixed in `HEngineDX12SceneSmoke/Main.cpp`. Its exceptional abort also
exposed two distinct native child-object leaks (command buffer and command pool;
stdout/stderr duplicated the same validation messages).

NVRHI snapshot `8e8c36e37558acec333204619b95d9d2fcdc4a79` stored `this` in
`m_CurrentCmdBuf->referencedResources` during `CommandList::open`. The owning list
also held the tracked buffer. `close` did not break the cycle; only submitted
`executed` detached the list and queue retirement cleared resources. Vulkan's
abort correctly discarded old work before an empty acquire-semaphore drain, but
replacing the never-submitted list could not destroy this self-cycle. Waiting for
idle cannot retire a recording that was never submitted.

Minimal Vulkan-only patch moves this self-reference to `Queue::submit`, before
the tracked buffer is published to its lifetime tracker. It deliberately does
not add the reference later in `executed`, after GPU submission/possible retirement.
The submitted path retains the list's upload/scratch managers as before. The
unsubmitted path naturally destroys its native command pool through
`TrackedCommandBuffer::~TrackedCommandBuffer`.

Unsubmitted destruction additionally CAS-clears the volatile-buffer slots that
still contain exactly its own pending queue/recording ID, while referenced buffers
are alive. Otherwise shared volatile buffers would retain permanently unavailable
pending versions even after the native leak was fixed. It never pretends that
discarded work executed, nor modifies another recording's or submitted version.
Upload/scratch managers are privately owned and release their chunks on destruction.

The early Vulkan probe now writes the same `maxVersions=1` volatile buffer in
both aborted frames. Its second write exposes failure to return the first pending
version through NVRHI's exhausted-version diagnostic. Both write markers, the
existing SDK abort/reinitialize assertions and **zero validation diagnostics**
are required together; the marker alone is not a success assertion. Existing
queue submission/device-loss terminal semantics are unchanged by this patch.

Validation at preparation: static diff check only; no engine build or GPU run.
The failed pre-patch run is not a passing abort lifecycle result.
