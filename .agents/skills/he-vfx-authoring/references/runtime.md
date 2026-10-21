# 现有运行接口与验收入口

仅在任务需要运行预览、Low/回退或参数检查时读取相应部分；离线配方工作不需要启动 Session。

## 基本预览与证据

使用真实当前候选哈希，不能照抄历史验收 SHA 或用旧二进制验证后续源码。以下为 DX12 示例；只运行任务选定或受影响的后端：

```powershell
$candidateHash = (Get-FileHash out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe -Algorithm SHA256).Hash
py -3 Tools/VFX/preview.py --backend DX12 --existing-build-sha256 $candidateHash --effect Assets/Effects/Authoring/<unique-id>.efkpkg --output out/agent/rendering-evidence/vfx-authoring/<unique-run> --frames <ascending-frame-list> --scale 1 --intensity 1
```

该入口使用 Session + Storage、固定 1/60 步进、finally stop；每次输出用新目录。预览 CLI 不支持 `--speed` 或 `--tier`，自定义播放值、实际 Low、重载/参数循环才使用下述公开调用。读取当前 `vfx.describe`/discovery 后，复用 `Tools/VFX/preview.py` 的 `Session(output, backend, executable_hash, scene=...)`、`Storage(ROOT).workflow_scope(...)`、`write_json`；不新建第二套 mailbox、进程控制或构建工具。外层记录包住启动、读回、native 日志复制及 finally stop，沿用现有异常和 ENV 恢复方式。

保存当前输入/包/场景/二进制哈希、精确命令、backend/configuration、能力快照、完整 native 双流日志及清理结果。将 commandStatus、人工视觉、性能/内存结论分开；实际审图后才补独立视觉结论，保留工具的 NOT_REVIEWED 原始报告。新效果或影响生命周期的修改，要确认自然终确实 `playing=false`，以及显式 stop 后活动效果/allocator 恢复；参数微调只报告实际检查范围。真实 GPU dispatch/draw 和新鲜计时不等于精确粒子 readback。

## CPU 回退

只有请求要求或改动影响 Low/GPU 回退时创建或更新回退。GPU 配方仍须引用匹配的 CPU 配方；新增时先 build CPU 配方，再令 GPU 配方的 `requirements.fallback_recipe` 指向该 id。工具从内置库或已发布作者目录查找，配方及包须匹配 build.json；仅填 id 不会生成或自动切换回退包。Low 配方应以较少粒子保留可读形状。

## 参数、质量和重载调用

以下是现有 `session.call(method, params)` 的参数对象，不是另一个 CLI：

| 调用 | 参数及必须检查的返回 |
| --- | --- |
| `vfx.describe` / `vfx.stats` | `{}`；保存当前支持/测量状态，缺值不视为零 |
| `vfx.quality` | `{}` 查询；`{"tier":"Low"}` 设置，检查 `accepted`、`tier`、`requested`、`enabled` |
| `vfx.play` | `{"path":"Assets/Effects/Authoring/<id>.efkpkg","seed":1806,"scale":1,"speed":1}`；检查合法 handle/playing |
| `vfx.control` | `{"handle":h,"action":"parameter","name":"intensity","value":0.6}`；检查 accepted |
| `vfx.control` | `{"handle":h,"action":"color","value":[1,0.5,0.2,1]}`；运行颜色用 0..1，检查支持与 accepted |
| `vfx.control` | `{"handle":h,"action":"status"}`；读取 playing/paused/实际参数及其可用性 |
| `vfx.control` | `{"handle":h,"action":"pause"}` / `resume` / `stop`；不是全局 runtime.pause |
| `runtime.step` | `{"frames":30}`；每次最多 120，现有 Session 的 hectl 路径等待完成 ticket |
| `capture.frame` | `{"filename":"<unique-stage>.png"}`；复制 receipt.path 到证据目录并记 SHA256 |
| `vfx.reload` | `{"path":"Assets/Effects/Authoring/<id>.efkpkg"}`；检查 accepted，旧 playing handles 留用旧版 |

更改质量要在无 active effects、停止后退休的边界调用公开 `vfx.quality`。当前 DevAgent 在 closed-host 的 pre-update 调度该命令；不要在任意游戏 Update 中绕过 setter。设置后再查询，确认实际 tier；Low 的 GPU actual enabled 必须为 false，播放显式 CPU 回退包。requested 与 enabled 含义不同；只在 High 环境播放一个名称含 low 的包不能算 Low 验证。若 setter 拒绝，保留旧状态，停止自有实例后重试一次；不能修改 Runtime 绕过限制。

命名参数当前包括 speed、scale、radius_m、intensity；以 Describe 的当前范围为准。radius_m 需要 recipe v1 作者半径元数据，映射为 target/authored uniform scale，并非实测外边界。GPU color/intensity 需要已编译的 NotBind_Root 支持；返回拒绝时改作者源并重新编译，不伪称动态修改成功。未支持的参数应解释并选择已有表达能力；不要给用户效果加隐藏引擎补丁。

同路径修改：保留旧 handle/图 → 用 authoring build 事务发布新包 → `vfx.reload` accepted → 新 play 对照；旧 handle 不会原地换粒子资源。重新加载失败时旧缓存/旧实例应保持，保存错误与旧图；不要通过停止全部用户实例隐藏事务失败。

`authoring.py validate-runtime <recipe> --capabilities <snapshot.json>` 接收当前嵌入式 `HEngine.VFX.GetStatsSnapshot()` 的键（如 particle_buffer_capacity、gpu_particles_capable、gpu_particles_enabled），**不是** Agent `vfx.stats` 的原始 JSON。没有该快照就报告这个检查 unavailable，使用公开 Agent 实况作另项诊断；不可编造转换后把 dev probe 当 verified 能力。编译粒子上限不是运行容量承诺。

## 选用已有诊断，不扩大普通创作任务

```powershell
py -3 Tools/VFX/parameter_acceptance.py --backend DX12 --existing-build-sha256 <current-candidate-hash> --output out/agent/rendering-evidence/vfx-authoring/<new-parameter-run>
& $imagePython Tools/VFX/resource_reload_pixels.py --backend DX12 --existing-build-sha256 <current-candidate-hash> --output out/agent/rendering-evidence/vfx-authoring/<new-reload-run>
py -3 Tools/VFX/quality_acceptance.py --backend DX12 --existing-build-sha256 <current-candidate-hash> --output out/agent/rendering-evidence/vfx-authoring/<new-quality-run> --tier Low
```

`$imagePython` 指向已带 Pillow 的 Python 3.11+；先用该解释器 `-c "from PIL import Image"` 检查。当前机器的 `py -3` 不保证有 Pillow，优先用可用的工作区依赖运行时，不为 help 检查全局安装依赖。这些工具各有固定 fixture；通过它们不能代替用户新效果的参数图像/自然结束验收。Vulkan 改 backend 单独运行。重载像素诊断见 [范围说明](../../../../Docs/VFXResourceReloadPixels.md)。

`template_matrix.py --recipe <canonical-id>` 只接受 canonical 库的精确 id，不能为外部 recipe 污染内置库；其 `stages`、`step`、`capture` helper 可供外部 recipe 的 owned 会话复用。完整时序检查按实际配方采集，不能只用默认帧表或峰图。起/峰/衰/终当前定义：1、ceil((attack+sustain/2)*60/speed)、ceil((attack+sustain+decay/2)*60/speed)、ceil(total*60/speed)+4；相同时刻合并。零 speed 是暂停、无自然结束；终时仍 playing 就记 FAIL 并调查真实寿命，不把后续强制 stop 图标成自然终。

性能、严格内存和发布包是独立验收。暂停固定步进不是 Release benchmark；统计 stable 不代表 GPU fence 退休字节；不要调整 warmup/阈值制造 PASS。需要这些验收时再使用对应现有 driver 和构建技能。
