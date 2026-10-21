# Early-frame GPU 验收入口

新增 `HEngineDX12SceneSmoke` 的独立模式，默认不开启，不运行原 CPU/场景长测试。原 `HEffekseerDX12InteropSmoke` 仍是 CPU 绘制及 GPU 拒绝回归，不能替代该验收；它也不在 `Tools/Agent/Build.ps1` 的 target 白名单。`hectl` 没有公开 scene-smoke scope，已有受控入口为 `Build.ps1 -Target HEngineDX12SceneSmoke -Run -Wait`。

已实际运行原始检查、上传异常恢复和质量切换检查。最新 Vulkan `early-vulkan-11` 为 114 项 PASS，完整双流零验证层错误；DX12 `early-dx12-02` 为 113 项 PASS（仅 NVRHI validation），最新 Vulkan 缓存修复后的 DX12 回归待运行。精确候选与失败历史见实施记录。以下命令须在其他 GPU 会话结束后串行执行，构建与运行证据分别记录。

```powershell
$probeKeys = @('HE_VFX_EARLY_SMOKE','HE_VFX_EARLY_SMOKE_BACKEND','HE_VFX_GPU_PARTICLES','HE_VFX_DISABLE_STATS')
$probeSaved = @{}
foreach ($key in $probeKeys) { $probeSaved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
try {
    $env:HE_VFX_EARLY_SMOKE = '1'
    $env:HE_VFX_EARLY_SMOKE_BACKEND = 'DX12'
    $env:HE_VFX_GPU_PARTICLES = '1'
    $env:HE_VFX_DISABLE_STATS = '0'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Parallel 2 -Run -Wait -TimeoutSeconds 180
    if ($LASTEXITCODE -ne 0) { throw 'DX12 early GPU probe failed' }
    $env:HE_VFX_EARLY_SMOKE_BACKEND = 'Vulkan'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Parallel 2 -Run -Wait -TimeoutSeconds 180
    if ($LASTEXITCODE -ne 0) { throw 'Vulkan early GPU probe failed' }
} finally {
    foreach ($key in $probeKeys) { [Environment]::SetEnvironmentVariable($key, $probeSaved[$key], 'Process') }
}
```

该现有 Build workflow 登记 Storage、拥有进程、等待完成并收尾。不要手动启动保留窗口的进程。程序默认使用 `Assets/Effects/Stress/GpuSparks20k.efkpkg`，可通过 `HE_VFX_EARLY_SMOKE_EFFECT` 指定另一个已经审核的有限 GPU 效果；其资源 hash 应记录在外层验收候选清单。

## 精确覆盖

1. 构造没有 CameraComponent 的 Level，调用真实 `RenderSystem3D::OnUpdateRuntime`。单一打开的 host frame 内运行四次 Update，中间设置 transform、暂停、恢复和速度。每次 Update serial 推进；四次合计恰有四个 compute 录制段；前两次有真实 dispatch 增量。全程不调用 VFX Draw，仍正常提交。此项不是四个 `runtime.step` 帧的替代计数。
2. 同一个 host frame 执行两个拥有独立纹理、framebuffer 和资源池的 VFX RenderGraph。两次均要求真实 draw 增量，Update serial 和 dispatch 不增加。保存的是命令证据，不是两视图像素证明。
3. 正常退休帧槽后，模拟、绘制各自要求正 GPU timer 和对应 submission。输出 `EARLY_VFX_STATS` 包含独立计时提交、Update/record serial、native draw/dispatch 和 CPU allocator 精确账目。
4. 在已 Compute 的 host frame 调用 Abort，不提交这份 list；必须进入 Unconfigured。下一次拥有有效 host scope 的 Update 必须在 CPU Update serial 推进前被拒绝。
5. 同一进程、同一设备等待已有提交退休，Shutdown/Initialize VFX。重新播放后必须产生新的 native dispatch、draw 和已完成 GPU 模拟计时。不是重启进程，也不是设备重建。
6. StopAll 后推进/退休，三类分配失败仍为零，已分配 emitter、保留 particle/trail 槽回零。该计数不代表 GPU live particle readback 或设备字节完全回收。

只有全部断言通过才输出 `EARLY_VFX_SMOKE_PASS`。任何失败抛出至原 smoke 错误处理并返回非零。每个图和 callback 持有者在退出前等待提交退休，异常帧先释放 host scope，再丢弃未提交 list。

## 仍需实际会话覆盖的边界

已有 `lifecycle_acceptance.py` 可串行覆盖 CPU/GPU 常规生命周期、两 view、reload、30 次资源趋势。新增模式不覆盖以下项目，末行显式列为 NOT_VERIFIED：

- **resize**：现有 Agent `window.resize`。保持 GPU effect 播放，固定步进，执行 1280×720→800×450→1280×720；等待 `session.status.renderWidth/renderHeight` 匹配，逐次捕获并核对新 dispatch/timer、分配失败为零及句柄仍有效。还应在 Editor 手工拖动 offscreen viewport，验证 next-Begin 延迟尺寸生效。单独调用 backend Resize 不能替代 Application 延迟路径。
- **scene switch**：现有 `runtime.scene.load`，先播放 GPU，再强制重载预览场景，等待 `world.summary` 的场景 generation 真正改变；检查旧句柄/owner 清理，再播放并检查新 GPU dispatch/timer。仅收到 `completion=queued` 不算完成。随后载入临时无 camera 场景可额外验证 Application 全链；应复制预览文件并将 CameraComponent.Primary 改为 false，不能覆盖原场景。
- **图像和事件效果**：中间 transform/pause 命令与逐步 Compute 顺序均实际执行，但尚无粒子状态 readback 或一致性图像断言。seed 一致性、具体轨迹、视觉正确性不能由 native counts 宣称通过。
- **设备重建**：本模式只重建 VFX runtime；不重建 NVRHI device/swapchain/窗口。

最短执行组合是两个后端各跑一次本模式，再用固定候选的受控 Agent 会话完成 resize 与 scene switch；各项独立报告 `runtime-manual + 精确命令 + PASS/FAIL/NOT_VERIFIED`。

## Vulkan aborted acquire recovery

The original AbortFrame discarded the command list but retained an acquired image
and its signaled binary acquire semaphore. The candidate now replaces that list
with an empty synchronization submission waiting on the acquire semaphore, arms
the frame-slot completion query, and requests swap-chain recreation before the
next acquire. The abandoned SDK/render commands are never submitted; no undefined
image is presented. The existing recreation boundary waits for device idle.
This follows the [Khronos swap-chain destruction requirement](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroySwapchainKHR.html)
that outstanding uses of acquired images have completed before destruction.

A Vulkan abort may therefore set bSubmissionIssued for synchronization only.
The probe tracks an explicit aborted-host flag, never calls SDK Notify for that
case, and requires the timing to remain aborted and SDK simulation to require
reinitialization. DX12 still discards without this WSI drain. Both paths remain
pending actual probe/validation-layer execution.

The early probe now enables the existing RenderDevice submission timing ring and
calls BeginGpuTimingFrame for each host frame. Its cancellation scope also covers
BeginFrame failure. After the two intentional aborts it waits for idle and consumes
TakeSubmissionTimings, requiring both exact sample IDs to retire as Aborted with a
real begin submission and one marker submission. It prints EARLY_VFX_ABORT_TIMING
with begin/end IDs; milliseconds are explicitly INVALID. DX12 may have no end
submission, while Vulkan's end ID may name the synchronization-only drain. Neither
is accepted as a Complete sample. This uses the existing production timing path.
Allocation failure counters are additionally checked after abort/retirement and
before VFX reinitialization can reset them. These new assertions are source-only
until the main agent builds and executes the probe.

## Explicit post-upload exception recovery (development only)

Actual Vulkan execution passed in `early-vulkan-09` (49 checks, one injected
exception, exit 0) with candidate SHA256
`2979172C203AF9DB7A44EC0C0C6C72AF0254C6EEC6CE3CF00451FD5BFC0FAD75`.
Complete native stdout/stderr were retained and scanned: Vulkan validation was
enabled and no validation/device errors occurred. The same executable passed
`early-dx12-01` (48 checks, one injection, exit 0), with both native streams
retained and no errors. DX12 used NVRHI validation only because the native D3D12
debug layer is unavailable on this machine (0x887A002D).
`HE_EFFEKSEER_DEV_TEST_HOOKS` is defined only when
`HE_BUILD_DEV_GAME && HE_ENABLE_GM && !PACK_GAME`. Effekseer, LLGI and every enabled SDK renderer target receive the same private
definition, keeping SDK inline definitions consistent without propagating the macro
to Runtime/Game/PCH consumers. SceneSmoke does not include these inline headers
and needs no macro. Shipping builds cannot activate the fault or sampler/load tracing
through environment variables.

Set both `HE_VFX_EARLY_SMOKE=1` and
`HE_VFX_EARLY_SMOKE_INJECT_UPLOAD_FAILURE=1`. The latter defaults off. After a
nonempty StandardRenderer CPU batch completes its real vertex Upload, before
any draw for that batch, one process-wide atomic one-shot throws exactly
`HE_VFX_EARLY_UPLOAD_FAILURE`. This is inside the pending-batch cleanup scope.
The probe loads the selected valid GPU effect plus `CpuSpriteControl.efkpkg`;
a Load rejection or any other exception is a failure, never an expected sentinel.

The extra prelude verifies actual GPU dispatches occurred before the fault,
SDK/host abort and Unconfigured status, Update rejection inside a new valid host
frame without advancing its serial, zero allocation failures before reset, and
the exact real timing sample retiring as Aborted. It waits idle and reinitializes
VFX, then uploads and draws the CPU-only fixture through both views and retires a
positive draw timer. It subsequently executes every original early assertion,
including the two independent explicit aborts, GPU recovery and final allocator
zero baseline. Thus enabling injection does not replace the normal coverage.

Example (root's serialized build/run and Storage workflow; run separately per backend):

```powershell
$env:HE_VFX_EARLY_SMOKE='1'
$env:HE_VFX_EARLY_SMOKE_INJECT_UPLOAD_FAILURE='1'
$env:HE_VFX_EARLY_SMOKE_BACKEND='Vulkan' # repeat with DX12
$env:HE_VFX_GPU_PARTICLES='1'
$env:HE_VFX_DISABLE_STATS='0'
$env:HE_DX12_SCENE_SMOKE_DISABLE_VALIDATION='0'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Parallel 2 -Run -Wait -TimeoutSeconds 180
```

Select a known-valid current official GPU fixture through `HE_VFX_EARLY_SMOKE_EFFECT`
if the old legacy stress package is still installed: rejecting its bad sampler is
not this regression's coverage. Require exactly one
`EARLY_VFX_UPLOAD_FAILURE_INJECTED`, the exact sentinel CHECK, retired Aborted
sample, `EARLY_VFX_UPLOAD_FAILURE_RECOVERY_PASS` and original
`EARLY_VFX_SMOKE_PASS`. Independently scan the complete native stdout **and**
stderr for zero validation errors (especially map size zero, unmap, command pool
or descriptor VUIDs). Exit code and the printed markers alone do not prove native
validation passed. Device recreation, particle-state readback and visual fidelity
remain outside this test.

## Quality policy and descriptor-cache regression

The same process now checks High → Low → Medium → High, explicit disable across
tiers, a Low-tier enable request followed by restoration, and rejection during
active effects or an open host frame. Enabled stages must produce fresh actual
dispatches, draws and completed positive GPU timers. Stopped allocator ownership
must retire to baseline. Require `EARLY_VFX_QUALITY_POLICY_PASS` as well.

`early-vulkan-10` passed all state assertions but failed native validation:
`VUID-VkWriteDescriptorSet-dstSet-00320`. LLGI reused cached descriptor sets after
their allocation layout's pipeline had been destroyed by quality reconfiguration.
The local 1.80.6 patch makes each bounded cache entry retain that pipeline until
the descriptor pool has been destroyed. Fixed graphics and compute binding schemas
allow existing cross-pipeline reuse; cache capacity and reset behavior are unchanged.
This intentionally retains a bounded set of pipeline/shader/layout objects, rather
than promising all native resources disappear on quality changes.

`early-vulkan-11` exercises this path with zero native validation errors, candidate
SHA256 `855182C29BDB6EEA06783318FD5F36331D2AEFAB6B97B629E2FB03AB9CE21B94`.

Latest DX12 rerun `early-dx12-04` passes 113 assertions, one deliberate upload
failure, normal exit and zero errors across both native streams. Candidate SHA256
`69E047CFDB695CE6E3045B0814C993A800413DDA6B1A5DC96AC14F2A5AC4FEBE`
includes the bounded descriptor owner patch; build completed in 15.8 seconds.
This remains an early-frame/state/command check, not a visual or stress budget pass.
