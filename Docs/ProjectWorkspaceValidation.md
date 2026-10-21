# Engine / Project 工作区验证（2026-09-21）

## 项目管理器启动交接补充

以下为本轮修改后的验证；后文保留此前 Engine/Project 分离的验证记录。

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| unit-test / runtime-manual | `powershell -NoProfile -STA -ExecutionPolicy Bypass -File Tools/Agent/ProjectManager.ps1 -SmokeTest -Project 'out/agent/Blank Project Test' -CapturePath out/agent/project-manager-ready.png` | PASS：内部 hectl 构建 Editor；初始场景、首帧标记、可响应窗口、有界正常退出；WPF 失败恢复、保留窗口与默认关闭分支。日志 `out/agent/project-manager-ready-test.log` |
| runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/ProjectLaunch.ps1 -Project 'out/agent/Blank Project Test' -Check -StartupTimeoutSeconds 5 -Log 'C:\WorkSpace\HEngineDev\out\agent\project-manager-missing-scene.log'` | PASS（预期失败）：测试夹具 DefaultScene 临时指向 `Assets/Scenes/ProjectManager-Missing.he`，得到 failed 回执/非零退出，确认本次 Editor 已停止；finally 按原始字节恢复配置 |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/ProjectLaunch.ps1 -Project Project -Check -Log 'C:\WorkSpace\HEngineDev\out\agent\project-manager-running-editor.log'` | PASS（预期拒绝）：已运行项目返回 already running；调用前后 PID 76900 及启动时间一致，用户 Editor 保持运行 |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/ProjectLaunch.ps1 -Project 'out/agent/Blank Project Test' -Check -Log 'C:\WorkSpace\HEngineDev\out\agent\project-manager-concurrent.log'` | PASS（预期拒绝）：测试父进程独占打开项目的 `out/agent/projects/launch-locks/baf9ad4e697908e0.lock`，子进程返回 already being opened，finally 释放锁 |
| static-check | `git -c core.safecrlf=false diff --check` | PASS；两个修改的 PowerShell 脚本另通过 `System.Management.Automation.Language.Parser.ParseFile` 语法检查 |

界面截图已人工检查。WPF 分支测试通过注入失败回执和真实有界启动回执执行；未宣称对每种启动崩溃时机、120 秒超时或不同 DPI 进行自动覆盖。此次仅修改 Editor 启动握手及管理器，不重复构建 Game/Shipping。默认项目首次尝试受用户运行的 Editor 阻止，因此在独立空白项目构建并验证修改；没有更新或结束用户正在运行的默认 Editor。

设计、命令和目录归属见 [Projects.md](Projects.md)。此次保留 `HEngineDev/Project` 为团队默认项目；未设置本机默认覆盖，未提交或推送。

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-Projects.ps1` | PASS：创建、唯一身份、选择优先级、向上发现、相同 ID 的不同路径隔离、长 ID 短缓存键、失效绑定、版本拒绝、禁止覆盖、相对目录、最近记录去重与损坏恢复、模板独立性 |
| unit-test | `powershell -NoProfile -STA -ExecutionPolicy Bypass -File Tools/Agent/ProjectManager.ps1 -SmokeTest -CapturePath out/agent/project-manager.png` | PASS：WPF 实际创建按钮、重复创建报错、取消只结束自身进程、界面渲染；另一个同名进程保持运行 |
| runtime-manual | `powershell -NoProfile -STA -ExecutionPolicy Bypass -File Tools/Agent/ProjectManager.ps1 -SmokeTest -Project Project -CapturePath out/agent/project-manager.png` | PASS：实际 Open 按钮通过子进程调用有界 Editor check，自动退出；没有保留手动进程 |
| runtime-manual | `.\hectl.bat editor capture -Frames 30 -Resolution 1280x720` | PASS：原项目 Editor 构建、启动、截图与退出 |
| runtime-manual | `.\hectl.bat game check -Frames 60` | PASS：原项目 Game 构建、READY 与退出；通用运行层提取后仍正常 |
| runtime-manual | `.\hectl.bat editor capture -Project 'out\agent\Blank Project Test' -Frames 30 -Resolution 1280x720` | PASS：空白项目首次配置/编译、空白场景、独立 Assets、共享引擎内容与退出 |
| runtime-manual | `.\hectl.bat game check -Project 'out\agent\Blank Project Test' -Frames 60` | PASS：空白项目首次配置/编译、`Assets/Scenes/Main.he` READY 与退出 |
| runtime-manual | `powershell -NoProfile -STA -ExecutionPolicy Bypass -File Tools/Agent/ProjectManager.ps1 -SmokeTest -Project 'out/agent/Blank Project Test' -CapturePath out/agent/project-manager.png` | PASS：包含空格的外部项目路径通过真实按钮、进程参数和独立 Editor check |
| static-check | `git -c core.safecrlf=false diff --check` | PASS |

最近项目激活补充验证：`powershell -NoProfile -STA -ExecutionPolicy Bypass -File Tools/Agent/ProjectManager.ps1 -SmokeTest -Project Project`，unit-test / runtime-manual PASS。真实 WPF 双击事件通过共享启动流程完成当前项目的有界 Editor check；验证双击以项目条目为准、空白区域/右键不启动，以及构建期间双击/Enter 不重复启动。日志：`out/agent/project-manager-activation-test.log`。PowerShell 语法检查和 `git -c core.safecrlf=false diff --check` 均 PASS。

日志位于 `out/agent/project-logic-test.log`、`project-manager-test.log`、`project-manager-ui-test.log`、`project-editor-validation.log`、`project-game-validation.log`、`project-blank-editor-validation.log`。空白项目的独立编译日志位于 `out/agent/projects/baf9ad4e697908e0/logs`。截图经过缩略后人工检查；引擎运行结果不宣称为场景行为自动测试。

修复过程中发现并重测：旧外部项目缓存名过长导致 MSVC C1083；PowerShell 5.1 将成功命令的 stderr 警告当作异常；项目打开等待全工作区回收。分别通过固定长度哈希、以子进程退出码判断结果、使用现有外层存储工作流并延后自动回收修复。

本次未执行 Shipping/cook、Android/Web/微信发布或不同 Windows DPI 显示器的人工交互检查。未改动第三方源码、反射声明或持久化资源字段；构建正常执行现有 codegen，但没有单独扩大反射测试范围。

空白项目 Agent 的 runtime-manual 验证也为 PASS：READY；world 返回 `Assets/Scenes/Main.he`、entityCount=0；步进完成 2 帧；finally 停止该 owner 的会话，最终状态 stopped。使用刚构建并验证的 EXE 哈希固定运行；此步骤本身不额外宣称源码构建证明。执行的命令如下（owner 为该次测试生成的 GUID）：

```powershell
$project = 'C:\WorkSpace\HEngineDev\out\agent\Blank Project Test'
$exe = 'C:\WorkSpace\HEngineDev\out\agent\projects\baf9ad4e697908e0\game-relwithdebinfo\bin\Gameca38e40a.exe'
$sha = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
$owner = [guid]::NewGuid().ToString('N')
.\hectl.bat agent start -Project $project -ExistingBuildSha256 $sha -AgentOwnerTag $owner
.\hectl.bat agent world -Project $project -AgentOwnerTag $owner
.\hectl.bat agent step -Project $project -AgentOwnerTag $owner -AgentStepFrames 2
.\hectl.bat agent stop -Project $project -AgentOwnerTag $owner
```

证据：`out/agent/project-blank-agent-{start,world,step,stop}.log`、`project-blank-game-validation.log`、`project-manager-external-test.log`。测试编排首次将持续会话的 start 放在一个重定向管道内，管道在 READY 后未返回；随后只结束该测试编排宿主，独立执行 world/step，并在 finally 中停止同一个会话。没有终止用户已有的 Editor/Game。
