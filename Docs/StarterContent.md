# Starter Content 与玩法模板

项目管理器的新建项目页提供独立玩法模板。场景、配置和 C# 源码会复制到新工程；创建时分配独立的项目身份、存档位置和构建缓存。公共搭建资源位于只读的 `Engine/Content/Assets/StarterContent`，通过现有项目优先、引擎回退的相对路径复用。

## 创建工程

```powershell
.\hectl.bat project templates -Json
.\hectl.bat project create -Project D:\Games\MyShooter -Name "我的射击游戏" -Template FirstPerson
.\hectl.bat project create -Project D:\Games\MyTopDown -Name "我的俯视角游戏" -Template TopDown
.\hectl.bat game check -Project D:\Games\MyShooter
```

目标父目录必须存在，目标工程目录必须不存在。也可以运行 `Win-ProjectManager.bat`，选择模板并创建。新工程中的 `README.md` 说明控制方式和扩展入口。

| 模板 | 起点 |
| --- | --- |
| Blank | 空白场景 |
| Scripted3D | 基础 C# 移动示例 |
| SharedArena | 共享规则与便携视图的四端 2D 示例 |
| FirstPerson | 第一人称移动、视角控制、射击靶场 |
| TopDown | 俯视角移动、瞄准、射击靶场 |

FirstPerson 和 TopDown 首先面向 Windows 原生 C# 路径，沿用 Blank 的 Windows 构建配置。SharedArena 的 Web/微信能力不代表这些原生 3D 场景已经完成跨平台适配。

## 资源归属

- `Engine/Content/Assets/StarterContent/`：共享的原创基础材质和搭建预制体，使用引擎内建几何体，不包含 Unreal Engine 资源。
- `Engine/Templates/FirstPerson/`、`Engine/Templates/TopDown/`：叠加在 Blank 之上的场景、输入配置、UI 和玩法脚本。
- 新工程中的 `Assets/Scenes/Main.he` 和 `Scripts/CSharp/`：可直接修改的游戏内容。

定制公共资源时先复制到项目 `Assets` 并更新引用，也可以用相同项目相对路径覆盖引擎资源。不要为某个游戏回写共享内容。新建工程不会把 Starter Content 整包复制进去；SDK 组装时会携带 Engine Content 和 Templates。各模板的 `TargetRange.cs` 是可编辑的项目玩法实现，公共材质与预制体由引擎共享。

首批模板是可继续开发的小型靶场，展示基础输入、碰撞和命中逻辑；不包含完整角色动画、联网对战、武器库或商业美术包。

## 验证

`Validate starter templates` GitHub Actions 工作流对相关 PR 执行：

1. `Tools/Agent/Test-Projects.ps1`：创建、身份隔离、源模板不变、路径拒绝、公共资源引用。
2. `Tools/Agent/ProjectManager.ps1 -SmokeTest`：操作实际 WPF 创建入口并输出截图。
3. `dotnet run --project Tools/Tests/StarterGameplayTests/StarterGameplayTests.csproj -c Release`：通过可控引擎接口运行真实模板脚本的玩法测试。
4. 对实际新建工程执行 `dotnet build`，验证模板叠加后的完整 C# 项目。
5. 单独构建一个通用 DX12 Development Player，使用 WARP 为两个模板执行 `Tools/Agent/Test-StarterTemplatesNative.ps1` 的有界输入、世界状态、截图和会话清理验证。

配置不等于通过记录，以相应提交的 Actions 结果为准。无窗口测试不覆盖原生输入/渲染集成；WARP 可以验证软件渲染启动与玩法集成，画面质量和真实显卡性能还需相应 GPU 环境。UI 创建截图与游戏截图分别输出，不能相互替代。
