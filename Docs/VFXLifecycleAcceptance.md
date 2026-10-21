# VFX 生命周期验收

`Tools/VFX/lifecycle_acceptance.py` 复用 `preview.Session`，以指定 SHA-256 的现有二进制启动一个受控 Dev Agent 会话。脚本不构建、不修改场景或效果包。所有命令与返回日志留在新的证据目录；外层 Storage 记录直到取证、输入 hash 复核和 `finally` 会话停止后才结束。仅采样匹配 ownerTag、PID 和启动时间的进程内存，不输出会话认证数据。

## 串行执行

以下 PowerShell 命令必须在已完成构建且无其他受控 GPU 验证会话时执行。SHA 应来自本轮候选清单；不要为了绕过 hash 不匹配重新取当前文件 hash。替换占位 hash 和新的证据目录后，分别运行两个后端：

```powershell
python Tools/VFX/lifecycle_acceptance.py --backend DX12 --cpu-effect Assets/Effects/Authoring/hit-ember.efkpkg --gpu-effect Assets/Effects/Stress/GpuSparks20k.efkpkg --nominal-particles 20000 --existing-build-sha256 <candidate-sha256> --output out/agent/rendering-evidence/vfx-authoring/lifecycle-dx12-20k-01
python Tools/VFX/lifecycle_acceptance.py --backend Vulkan --cpu-effect Assets/Effects/Authoring/hit-ember.efkpkg --gpu-effect Assets/Effects/Stress/GpuSparks20k.efkpkg --nominal-particles 20000 --existing-build-sha256 <candidate-sha256> --output out/agent/rendering-evidence/vfx-authoring/lifecycle-vulkan-20k-01
```

效果必须是有限生命周期、在 `--sample-frames 15` 时仍活跃且在 `--natural-end-frames 3600` 内结束的夹具。上述路径存在，但该脚本交付时未运行它们证明这些前提；循环效果应替换为有限效果，不能把自然结束失败改为通过。固定时间步为 1/60 秒。`report.json` 保存完整实际命令、候选 hash、资源 hash、源码状态及预算；每个底层调用见 `commands.json`。

## 判定

- CPU/GPU：暂停前后 SDK `GetPaused` 为真，跨自然生命周期窗口仍存活；恢复后 SDK 状态为假且最终自然结束。分别读回速度 0、0.5、1。停止后句柄不再存活；相同 seed 新句柄重播并保存图像。
- 参数：负缩放、过大速度、错误位置长度、int32 seed 越界、负速度、越界 dynamic input、无效颜色、未知 action 均需拒绝且会话继续响应。传输失败不能作为拒绝证据。
- 并发：CPU 和 GPU 句柄同时存在，并有真实 native dispatch 录制增量；GPU simulation timer 必须有新的 `lastGpuSimulationSubmission` 和正时间。第二 view 的绘制 timer 单独比较 `lastGpuRenderSubmission`，不使用混合提交编号给旧样本背书；模拟 timer 可能在此时退休此前工作，不把它归因到第二 view。
- 第二视图：不推进模拟，创建/捕获/销毁一个 640×360 view，核对 simulationTimeSeconds 不变、dispatch 不增长、graph/capture 完成和 draw 前进。全局 draw 计数不证明每个视图的 VFX 像素；该限制保留在报告。
- Reload：未改包的重新加载成功，旧句柄继续存活、新句柄可创建；随机不存在路径失败，不修改原包。此项不覆盖同一路径损坏内容的事务回滚或资源内容变化后的视觉效果。
- 30 次 CPU/GPU 播放与停止：前 5 次作为预热，其后活动计数归零、缓存数量不继续增加。进程 private bytes 不得超过首个 settled 样本，首末 5 次中位数差及线性趋势不得为正；不存在 64 MiB/512 KiB 容差。进程内存是代理指标，GPU 粒子数量也是 CPU 估计，均不能证明 GPU buffer 已回收。
- 每次 `vfx.stats` 都要求 `gpuAllocation` 三类失败计数保持基线，初始基线必须为零；任一分配失败或计数回退立即失败，不能因 handle 有效而继续报告命令通过。全部 30 次循环停止并推进 30 帧后，`allocatedEmitters/reservedParticles/reservedTrails` 必须回到初始基线，预热不豁免此检查。它精确验证 CPU allocator 已提交槽所有权，不代表设备内存或 fence 退休字节。GPU system 重建会使计数失去可比性，该脚本不执行重建。

`commandStatus=PASS` 表示已覆盖的命令断言通过，退出码为 0。由于 seed 状态一致性、GPU 分配回收等缺少直接证据，完整 `acceptanceStatus` 保持 `PARTIAL`；不得把退出码 0 宣称为全生命周期验收通过。任何失败返回 1。未执行的场景切换、窗口 resize、设备重建、同路径损坏回滚、shipping 性能仍不在覆盖范围。截图只有文件与 hash，尚未人工视觉审核。

## 生命周期审计与最小修复方向

1. **多次 Update、一次 Render 的时间丢失。** 本地 Effekseer 1.80.6 的 Manager 每次 Update 调用 GPU `SetDeltaTime(GetNextDeltaFrame)`，Common GPU 系统直接覆盖 emitter.Data.DeltaTime，而 Compute 才推进 TimeCount。仅一个 `bGpuComputePending` 可避免同帧多 view 重复 Compute，不能保留多次 Update。最低限度应累加并消费 delta、清空已消费值，防止静默丢失时间；但大步长与固定步数的积分、spawn/stop 时刻及中间变换不等价。若要求 CPU/GPU 时间与逐步语义一致，应排队每次更新的 emitter 参数快照（delta、变换、控制/发射状态、资源引用），按顺序录制 Compute；队列需上限和明确背压策略，不能丢最旧更新或重复使用最终快照假装重放。另一实现路线是每次更新安排独立于 view 的 compute-only 提交。补丁由主代理实施，脚本不直接验证隐藏的 update-only 路径。
2. **失败提交。** SDK Compute 会推进 CPU 侧 GPU emitter 状态；如果其后 command list 中止，不能只把 pending 清零。成功录制与成功提交是不同阶段；需在成功提交后确认消费，失败则使 GPU 会话失效并显式重建/重播，或者拥有可回滚的更新状态。该脚本无故障注入，不标通过。
3. **Seed。** 在 Play 后首次 Update 前设置 seed 能影响首次模拟，但 SDK seek/reset 可能使用 DrawSet 保存的旧 seed。API 配置回显不能证明模拟一致；需要受控状态摘要或隔离全局时间和后处理后的图像比较。当前报告明确不验证这一点。
4. **计时与资源生命周期。** query 只能在对应 fence 完成后取值/复用；slot 重用需清理 submission 和有效标记。native dispatch 录制量、GPU 时间、活跃粒子估计是三种证据。新增 CPU allocator 精确槽账目可证明 emitter 槽归还；设备资源释放证明仍需 fence-retired buffer 字节计数。
5. **Reload/disable/shutdown。** 旧实例持有旧资源的引用应独立于 cache 替换；失败加载恢复 cache 不能伤害现有 handle。关闭 GPU 后缓存命中仍需检查 capability，shutdown/disable 必须清空 pending，资源须等待其实际 GPU 使用结束。现有脚本覆盖未改包 reload 和不存在路径，不冒充异常 rollback/设备销毁验证。

## 本次工具验证

最新根独立生命周期 CPU 单测 9 项 PASS。Game15 的 `lifecycle-vulkan-20k-01` 完整30周期因统计历史常驻内存增长约626MB而FAIL。Game16流式统计后的 `lifecycle-vulkan-20k-02` 仍严格FAIL：首末窗口中位差+4096 bytes、斜率-97.673846 bytes/cycle，后段已平台化；GPU工作、allocator归零和已观测buffer池全部通过，但不豁免private proxy失败。完整165538帧、165574提交、12865701 pass记录校验PASS；精确命令和二进制身份见实施记录。

## 锁定30周期压力契约

每份运行必须显式 `--nominal-particles 20000` 或 `50000`，与相应GpuSparks20k/50k包搭配。调用当前生成器的 `validate_compiled_stress` 验证官方1810完整载荷布局，并交叉检查PROVENANCE的rate/lifetime/emit count/owner lifetime/seed；拒绝旧1800样例及非法sampler。不能只凭文件名或metadata断言20k/50k，也不宣称GPU实时粒子计数读回。

每周期推进至少201帧到达200帧life的发射平台，要求GPU handle仍活、sim/render各自新的submission与Exact正有限timer、native累计dispatch/draw增长；保存全部前/活跃/退休stats。每次stop后30帧要求allocator三项ownership全零、activeEffects/cpuInstances零，所有采样failure counters不增。丢指标失败。CPU one-shot可在平台采样前结束，压力周期仅要求GPU持续；CPU/GPU共存已由前置独立检查覆盖。

预算SHA256开闭均记录验证。移除64MiB/512KiB容差：warmup后任何private bytes超过首个settled样本，或首尾median增长、正斜率均FAIL。测量噪声也不自动豁免。CPU分配器恢复不等于GPU fence字节读回，故最终acceptanceStatus仍PARTIAL，不能声称整体验收通过。

每后端两次运行分别使用新输出目录 `out/agent/rendering-evidence/vfx-authoring/lifecycle-<backend>-20k` 和 `...-50k`，固定同一RWD候选hash。组合入口（不启动引擎）：`py -3.11 Tools/VFX/lifecycle_acceptance.py --combine <20k/report.json> <50k/report.json> --output <combined.json>`。强制两种nominal、同后端/候选/当前预算及完整30周期PASS检查；缺一档FAIL。旧文档运行示例需添加nominal参数且使用上述Storage路径。RWD只做生命周期诊断，Release性能另由benchmark验证。

unit-test：`py -3 Tools/VFX/lifecycle_acceptance_test.py` PASS（9 tests）。实际进程证据独立报告。

显式 `vfx.stats` 还采集已登记 LLGI storage/uniform backing 的去重数量、GetSize逻辑字节和snapshot数，要求预热后零增长；常驻池不要求Stop时归零。可用的OS当前进程local/nonlocal用量独立检查，不可用记NOT_VERIFIED，不能填零。此统计不包含native padding、纹理、vertex/index、command-only upload、frame pools或pipeline/layout缓存，也不是fence-retired字节。
