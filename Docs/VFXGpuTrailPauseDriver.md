# GPU 拖尾暂停验收脚本

`Tools/VFX/trail_pause_acceptance.py` 使用现有 `preview.Session`：固定二进制
SHA-256、严格后端、1280×720、固定步长、RenderStats、RenderValidation 和
`VFXAuthoringPreview.he` 中性场景。外层 Storage 记录包含所有截图、统计及会话收尾；
finally 停止实例、推进 30 帧退休并核对 allocator 基线，最后停止受控进程。

使用含 numpy/Pillow 的 bundled Python，分别串行运行两个后端。以下 `<候选 SHA256>`
必须替换为主代理固定并记录的候选值；脚本不会构建或接受 hash 不同的程序：

```powershell
$python = 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
& $python Tools/VFX/trail_pause_acceptance.py --backend DX12 --existing-build-sha256 '<候选 SHA256>' --output out/agent/rendering-evidence/vfx-authoring/trail-pause-dx12-01
& $python Tools/VFX/trail_pause_acceptance.py --backend Vulkan --existing-build-sha256 '<候选 SHA256>' --output out/agent/rendering-evidence/vfx-authoring/trail-pause-vulkan-01
```

每个模式独立开始：目标在 x=-1.2m，以固定 seed 播放真正的 GPU fixture
`Assets/Effects/Stress/GpuTrailPause16.efkpkg` 并推进 32 帧；暂停或 speed=0 后
首先推进一次零时间 Update，明确检查首个冻结帧。目标单独成形时与空场景的 RGB
差分定义 bbox，四边加 12px；宽度超过画面一半或有效信号不足 20px 会失败。
之后在 x=+1.2m 放置第二实例，正常推进 8 帧，再在目标冻结时推进 32 帧。
截图比较同时覆盖首个零时间帧、第二实例出现后的基线以及后续 32 个零时间步。

冻结目标 intensity=0 后推进一步：其 ROI 应恢复为空场景，这也检查另一实例是否
污染 ROI。intensity=1 后推进一步应恢复原几何。恢复播放或 speed=1 并推进一步，
要求出现可见变化且旧轨迹仍有重叠。每个阶段保留完整 PNG 和独立 stats JSON；
报告记录 bbox、原始指标、SDK paused/speed 读回、实际 dispatch 和已完成 simulation
timer 的 submission。不是粒子状态 hash，也不依赖状态 marker 证明几何冻结。

阈值固定写入 report.json，不能在看到失败结果后静默放宽：

| 判定 | 阈值 |
| --- | --- |
| 粒子信号 | 对空场景最大 RGB 通道差 >12/255 |
| 稳定像素 | 两帧信号 union 上 RGB MAE ≤1.5/255；变化 >12 的像素比例 ≤1% |
| 稳定形状 | 信号 mask IoU ≥0.97 |
| 隐藏和隔离 | 隐藏后 ROI 信号 ≤可见信号的 3% |
| 恢复一步 | 至少 20 信号像素、mask IoU ≥0.25、RGB MAE >0 |

每次统计读取要求 allocation failure 三项为零。单实例实际 CPU allocator 预留应为
1 emitter/256 particles/4096 trails，两个实例为其两倍；停止并退休后必须回到初始值。
这些是 allocator 槽位所有权，不是 GPU 活粒子数或 fence 后释放的显存字节。

PASS 仅表示上述固定场景的 SDK 状态、真实命令/计时和 ROI 图像阈值通过。
未覆盖粒子状态读回、设备重建、全局 overdraw 或性能。环境的时序后处理或背景变化
可能使图像阈值失败；保留原图分析，不能把该情况自动改为 PASS。

本轮只执行 AST、bundled Python `--help` 和合成图像判定测试。稳定拖尾通过；
收缩拖尾、空画面被拒绝；隐藏图像信号为零。**GPU 实际运行尚未执行。**
