# PC 表现恢复

Windows 默认入口仍使用共享 C# 会话，不切回 Python 玩法。桌面表现以原有 `SurvivorHUD` 和原生镜头系统为基线。

- 菜单、暂停、升级、结算显示原有自定义光标。游戏中由 GameLayer 捕获鼠标，直接移动鼠标即可转动镜头，Alt 释放光标。模式切换重新建立鼠标采样基准。
- PC 恢复原有暂停面板及其设置入口；Android/Web/微信继续使用共享暂停资源。
- 主菜单设置页保留原始布局，不再被说明文字替换。显示、音量、界面缩放通过平台服务实际应用；选项读取项目默认值和 Saved/Config/SurvivorGame.json，沿用原先字段并原子保存。退出脚本恢复进入前的显示设置。
- PC 镜头使用原生 FollowCamera/SpringArm 的遮挡检测与缓动；恢复 12.5 距离、0.0024 灵敏度、20° 初始俯角、-5° 至 75° 俯仰范围、目标高度与原先跟随参数。便携端继续使用原先的轨道参数。
- 原有本地化文本从 Python 源文件的静态表导出为数据，运行时不依赖 Python。

后续恢复已移除军械库、星光成长、任务和图鉴的简易 HTML 列表，改为绑定原有 RML 卡片、筛选、分页、详情及统计区域。购买、退款/全部退款、封印/解除封印和追踪操作仍由共享会话执行；退款保持钱包上限和重复调用约束，追踪保持三个任务上限。

PC 战斗重新使用原有武器施法、弹体、命中及敌方预警 Effekseer 资源。共享会话提供稳定的攻击对象 ID 和有界表现事件，PC 适配器负责效果位置、透明度、预算、暂停、回收；特效是否播放不决定伤害结果。Android 保留原来的便携表现入口，Web/微信仍由各自渲染后端表现同一会话。

特效路径直接来自旧武器目录和资源表，C# 构建自动生成 ScriptDependency 声明，已检查 45 个特效依赖均存在。没有修改特效资源本身。当前原生运行验证了默认武器的施法/弹体/命中加载和实际 VFX 绘制，不代表所有武器、进化组合和动画时序已经逐帧与 Python 版对比通过。

## 验证

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HGame -Configuration RelWithDebInfo -PluginConfig Project/Config/Plugins.CSharp.json` | PASS |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineEditor -Configuration RelWithDebInfo` | PASS |
| runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/TestSharedUINative.ps1` | PASS，菜单/暂停光标、无按键镜头转动、设置和恢复/重开/结算流程；已检查截图 |
| unit-test | 在 `Engine/Managed` 执行 `dotnet run --project ../../Tools/Scripting/SurvivorSessionTests/SurvivorSessionTests.csproj -c Release --artifacts-path ../../out/agent/pc-restore/session-tests -- ../../out/agent/pc-restore/catalog/catalog.json` | PASS，69 项，含 PC 镜头、便携端默认参数、退款防重复、退款合计、任务追踪保存、表现事件和攻击对象 ID |
| static-check | `git diff --check` | PASS |

证据在 `out/agent/pc-restore/` 与 `out/agent/shared-ui-native/`。镜头碰撞与缓动已接回并检查启用状态，尚未另做全部遮挡几何场景测试。此轮没有执行四端重新发布或真机检查。
测试使用的目录通过 `py -3.11 Project/Tools/ExportWeb.py --catalog-only --output out/agent/pc-restore/catalog` 生成。

追加原生证据：`hub-game-final.log`、`hub-editor-build.log`、`hub-runtime-final.log`、`restored-*.jpg`；原始截图路径记录在 `out/agent/shared-ui-native/*-capture.json`。`vfx-stats.json` 保存实际绘制计数，运行日志记录旧版彗星的弹体、施法和命中特效包加载。页面检查覆盖军械库标签/筛选、成长节点、任务追踪/分页、图鉴类别/统计及此前的设置/镜头流程。
