# First Person Starter

桌面 Windows C# 射击靶场。通过项目管理器选择 **FirstPerson** 创建独立工程，再打开 Editor 并 Play，或用 `hectl game check -Project <工程目录>` 验证启动。

| 操作 | 控制 |
| --- | --- |
| 移动 | W / A / S / D，相对当前水平朝向 |
| 视角 | 按住鼠标右键拖动；方向键也可连续转向 |
| 射击 | 鼠标左键或 Space，可按住连发 |
| 重置 | R，恢复出生位置、全部目标和计分 |

准星对准橙色方块。每 0.2 秒最多一发，最近目标命中后下沉，2 秒后复位。左上角显示命中数和射击数，黄色掩体遮挡射线并阻挡移动；边墙限制活动范围。按住右键时没有锁定系统光标，到窗口边缘可松开并重新拖动，或使用方向键。

`Assets/Scenes/Main.he`、`Config/Input.json`、`Scripts/CSharp`、HUD 都是新工程自己的文件，可以直接修改。脚本字段 MoveSpeed 可在 Inspector 调整。模板叠加到 Blank 工程，沿用其 GameScripts.csproj 与引擎绑定。

共享材质来自 `Assets/StarterContent/Materials/`，网格来自引擎自带 `Assets/Models/Box/Box.hmesh`。这些路径由引擎资源回退解析，不向每个工程复制基础资源。需要独立改色时，复制相应材质到项目 Assets 并更新 MaterialOverrides。StarterContent 下还提供 Target、Cover、Floor 三种可复用 prefab。

这是用于学习和扩展的平地靶场：角色移动、静态掩体和目标使用脚本轴对齐方盒检测，并非完整物理角色。保留 Target1/2/3 和 Cover 标签；目标和掩体应保持零旋转，若改变场地大小需同步 TargetRange.Move 的边界。新增任意模型或旋转障碍后，应接入 Physics 查询/角色控制。此模板使用桌面 InputActions 和 UI 服务，当前不承诺 Web/微信导出。

无窗口逻辑验证：在引擎根目录运行 `dotnet run --project Tools/Tests/StarterGameplayTests/StarterGameplayTests.csproj -c Release`。它编译真实 ScriptCore 与这份脚本，以 fake scene transport 检验移动、命中、遮挡、冷却、复位与 UI 生命周期；图形效果需另行原生运行检查。
