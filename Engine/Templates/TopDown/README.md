# Top Down Starter

桌面 Windows C# 俯视角双摇杆式靶场。通过项目管理器选择 **TopDown** 创建独立工程，再打开 Editor 并 Play，或用 `hectl game check -Project <工程目录>` 验证启动。

| 操作 | 控制 |
| --- | --- |
| 移动 | W / A / S / D，沿世界 X/Z 平面 |
| 瞄准 | 方向键，支持斜向；松开后保留最近朝向 |
| 射击 | 鼠标左键或 Space，可按住连发 |
| 重置 | R，恢复出生位置、朝向、全部目标和计分 |

青色方块是玩家，黄色短杆标示枪口朝向。移动和瞄准独立，固定俯视相机能看到整个靶场。射击使用当前朝向，不使用鼠标位置。橙色目标命中后下沉，2 秒后复位；每 0.2 秒最多一发。黄色掩体遮挡射线并阻挡移动，场地边墙限制活动范围。左上角 HUD 显示操作和计分。

`Assets/Scenes/Main.he`、`Config/Input.json`、`Scripts/CSharp`、HUD 都是新工程自己的文件，可以直接修改。脚本字段 MoveSpeed 可在 Inspector 调整。模板叠加到 Blank 工程，沿用其 GameScripts.csproj 与引擎绑定。

共享材质来自 `Assets/StarterContent/Materials/`，网格来自引擎自带 `Assets/Models/Box/Box.hmesh`，通过引擎资源回退解析。若需独立改色，将材质复制到项目 Assets 并更新 MaterialOverrides。共享 StarterContent 下还提供 Target、Cover、Floor 三种 prefab，不向每个工程重复复制基础资源。

这是平地玩法基础，移动、静态掩体和目标使用脚本轴对齐方盒检测，并非完整物理角色。保留 Target1/2/3 和 Cover 标签；目标和掩体保持零旋转，若改变场地大小需同步 TargetRange.Move 的边界。新增旋转障碍/复杂模型后应接入 Physics 查询和角色控制。当前桌面 InputActions、UI 服务不承诺 Web/微信导出。

无窗口逻辑验证：在引擎根目录运行 `dotnet run --project Tools/Tests/StarterGameplayTests/StarterGameplayTests.csproj -c Release`。测试编译真实 ScriptCore 与模板脚本，使用 fake scene transport；图形效果需另行原生运行检查。
