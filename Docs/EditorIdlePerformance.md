# 编辑器静止帧性能排查（2026-09-11）

## 场景与脚本

基于 `08e97e61`，默认 `Assets/Scenes/MainGame.he`，编辑模式、不进入 Play。
主菜单和主游戏仍挂载 `PythonScriptComponent`，入口为
`Scripts.SurvivorGame.py`。新增的 C# 插件和 `ScriptTouch.he` 示例没有替换现有主游戏。
编辑状态不调用 Python/C# 的 `OnUpdateRuntime`。

机器：RTX 3060 Laptop GPU，DX12，RelWithDebInfo，关闭 VSync。
性能使用真实墙钟时间，而非自动化的固定模拟步长。300 FPS 对应平均帧耗时 3.33 ms。

## 原因及修复

1. `EditorLayer::OnPreUpdate` 每帧完整序列化作者场景，再做 JSON 差异观察。
   此路径源于 `2f98e207` 的场景编辑事务整合。1080p 基线中帧前处理平均
   5.410 ms，总帧耗时 8.853 ms，场景更新仅 0.046 ms。
   改为：正常界面编辑下一帧观察、场景命令执行前强制观察；未报告的旧接口写入
   每 500 ms 兜底检查。交互拖拽仍合并提交，序列化格式及事务服务保持原样。
   旧接口未报告写入的脏标记最多延迟约 500 ms，保存及命令边界仍先同步。
2. `fb3e4d13` 的体积云以 `WindSpeed > 0 || EvolutionSpeed > 0` 请求持续渲染，
   但云时间来自不可变 `RenderWorld::TimeSeconds`。静止编辑器没有提交新快照时，
   反复绘制得到的是同一个时间的云，却阻止了整个主场景离屏结果的复用。
   4K 对照中，即使已修复场景观察，旧云逻辑仍导致 GPU 平均 9.896 ms，约 98 FPS。
   改为相同快照只在历史收敛期间持续绘制；新快照重启收敛计数，实时预览和游戏继续刷新。
   收敛至少 64 帧，并根据历史权重将初始历史残留压至 0.0001 以下；不改采样质量参数。

以上定位说明两个确定的开销来源，并非对用户此前约 160 FPS 的构建做过逐提交二分。

## 验证记录

原始 1080p 基线：`out/agent/editor-idle-before.csv`。
仅修复场景观察的 4K 云对照：`out/agent/editor-idle-fix/cloud-before-4k.csv`。
画面对照：`out/agent/editor-idle-fix/cloud-before-4k.png` 与 `final-4k.png`。
两次渲染设置快照 JSON 完全一致（包括 requested、effective 和 hash）。
同一视口区域 `(0,220)-(3310,1480)` 的 RGB 平均绝对像素差分别为
`0.001916 / 0.001936 / 0.001999`（8 位 0–255 标度）；缩略图人工检查构图与效果一致。
差异很小，但不声称时间累积结果逐像素完全相等，也不将此一次静止截图当作所有相机状态的覆盖。
截图采集帧包含读回开销，不用截图帧作为最终性能验收。

最终长测在其他构建、测试及产物回收退出后顺序运行，排除前 500 帧：

| 条件 | 有效帧数 | 平均 FPS | 平均帧 ms | P95 ms | P99 ms | 最大帧 ms | GPU span 平均 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1080p 修改前 | 1300 | 113 | 8.853 | 10.194 | 12.352 | 18.638 | 3.112 |
| 1080p 最终修复 | 7500 | 1288 | 0.776 | 1.201 | 1.349 | 6.741 | 0.167 |
| 4K 最终修复 | 9500 | 1265 | 0.791 | 1.218 | 1.328 | 6.503 | 0.436 |

4K 最终帧前处理平均 0.009 ms。偶发整场景兜底观察及系统调度仍会产生尖峰，
本结果是静止界面的平均帧率和分位数，不承诺每一帧的绝对硬实时下限。
原始长测位于 `out/agent/editor-idle-fix/stable-4k.csv`、`stable-1080p.csv`，
汇总为同目录的 `summary.json`。

证据类型、精确命令与结果：

```powershell
# runtime-manual PASS — 1080p 修改前基线（在修改源码前执行）
.\hectl.bat editor check -Frames 1600 -PerformanceWarmupFrames 300 -RenderStats -RenderStatsPath out/agent/editor-idle-before.csv -TimeoutSeconds 180

# runtime-manual PASS — 最终 4K 截图及配置快照
.\hectl.bat editor capture -Frames 600 -Resolution 3840x2160 -RenderStats -PerformanceWarmupFrames 250 -RenderStatsPath out/agent/editor-idle-fix/final-capture-4k.csv -CapturePath out/agent/editor-idle-fix/final-4k.png -TimeoutSeconds 180

# runtime-manual PASS — 最终 4K 长测（build 同时 PASS）
.\hectl.bat editor check -Frames 10000 -Resolution 3840x2160 -PerformanceWarmupFrames 500 -RenderStats -RenderStatsPath out/agent/editor-idle-fix/stable-4k.csv -TimeoutSeconds 180

# runtime-manual PASS — 最终 1080p 长测（build 同时 PASS）
.\hectl.bat editor check -Frames 8000 -Resolution 1920x1080 -PerformanceWarmupFrames 500 -RenderStats -RenderStatsPath out/agent/editor-idle-fix/stable-1080p.csv -TimeoutSeconds 180

# runtime-manual PASS — 主游戏菜单启动，180 帧正常退出（build 同时 PASS）
.\hectl.bat game check -Frames 180 -TimeoutSeconds 180

# unit-test PASS — 场景事务、保存重开、撤销重做等既有合约（build 同时 PASS）
.\hectl.bat test SceneEditing
```

复测请使用新的 CSV 输出名：工具有意拒绝覆盖既有统计文件。
中间两次运行分别因历史产物回收占用、已有 CSV 的覆盖保护而未完成；最终采用独立输出重跑通过。
场景编辑合约不等同于编辑器所有控件的自动化 UI 覆盖，相机刷新规则还经过代码路径检查。

修复版产物为 `out/agent/editor-relwithdebinfo/bin/HEngineEditor.exe`；
使用仓库 `Win-RunEditor.bat` 启动同一 RelWithDebInfo 编辑器。

## 使用范围

目标是默认场景收敛后的静止编辑器界面帧率。首次加载、相机移动、编辑、实时预览、
持续更新的自定义材质或 GI，以及离线路径追踪的工作量不同，不能以静止结果承诺其帧率。
画质设置、效果开关、分辨率和项目场景均不通过此修复降低或替换。
