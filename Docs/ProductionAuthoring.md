# 制作工作流与桌面脚本接口

本文描述第一、第二阶段新增入口。最终验证状态以 `ProductionReadiness.md` 的证据为准。

## 资源整理

内容浏览器的重命名经过依赖扫描与事务写入。命令行可先查看计划：

```powershell
./Tools/Agent/hectl.ps1 asset plan -Project <项目> -Source Assets/Models/Old -Destination Assets/Models/New
./Tools/Agent/hectl.ps1 asset move -Project <项目> -Source Assets/Models/Old -Destination Assets/Models/New
./Tools/Agent/hectl.ps1 asset recover -Project <项目>
```

场景、Prefab、模型、动画、UI、配置等已知持久化资源字段会跟随移动；RML/RCSS 的相对依赖也会重算。
启动场景、Cook 清单和资源预热引用一起更新。未知 JSON 引用与脚本文字引用会阻止移动并给出位置，
需要先改成可维护的资源字段或显式整理引用；工具不会对任意字符串做全局替换。
`.he-assets/catalog.json` 保存移动资产的稳定作者身份，应随项目提交。运行时资源仍兼容现有路径引用，
这个目录不是新的运行时 GUID 加载器。失败恢复使用 `.he-assets/pending.json`；恢复冲突会保留材料，禁止手删 journal。

编辑器在 Play、脏场景、排队编辑或有资源编辑面板打开时拒绝重命名，避免覆盖尚未保存的工作。
操作完成后重载场景与资源缓存。重导入继续使用现有 `hectl content` 工作流；`Assets/Imported` 的不可变输出
不能用普通移动命令修改，作者覆盖由 content sync 的合并与冲突机制保留。

## 输入与导航

项目设置新增 Input Actions 和 Navigation 页面，Play 时禁止修改作者配置。
输入保存到 `Config/Input.json`，可编辑动作类型、上下文、键鼠/手柄绑定、死区、长按和双击阈值。
内置 CharacterMovement 使用 Move、Jump、Sprint。进入 Play 会重新加载项目映射；失去游戏输入区、
停止 Play 或切换上下文会清理瞬态状态，并等待已按住的按键释放，避免误触。
脚本读取与玩家重映射示例见 `InputActions.md`。

导航保存到 `Config/Navigation.json`。设置 Agent 尺寸、坡度、台阶、体素分辨率、网格预算及区域代价；
表面按实体 Tag 前缀选择，使用其真实静态网格和世界变换。规则按顺序匹配。
项目设置支持对当前编辑场景烘焙、显示导航三角形及错误原因。
运行时 `Navigation.Build()` 对当前场景建立独立网格，`FindPath()` 仅把完整可达路径报告为成功。
失败重建保留上一张有效网格；动态门或障碍改变后需要显式重新烘焙。本阶段不提供自动动态切片重建或人群避让。

## Prefab 与动画

Prefab 实例属性区显示模板、局部节点身份、修订与覆盖字段。可恢复单项或本实体全部覆盖、同步实例树、解包，
操作经过共享场景事务并支持 Undo/Redo。普通 Inspector 的已知字段编辑会同步记录覆盖。
模板变化与实例覆盖冲突会保留失败原因和字段位置，不能静默覆盖作者修改。

动画控制器 Motion 设置支持二维参数/样本坐标、分层权重参数、骨骼根遮罩和相对绑定姿势的加法层。
遮罩包含指定骨骼及后代；混合姿势使用归一化四元数。事件归属基础或过渡目标片段，混合样本和层不重复发事件。
暂停时重采样基础姿势后应用层，避免层结果逐帧累积。删除状态会清理引用它的样本和层。

## C# 场景接口

`Physics.SetVelocity/MoveCharacter/FloorHeight`、`Animator`、`Camera`、`Navigation`、`SceneFlow` 是桌面类型化接口。
`SceneSequence.Play(entity)` / `Stop(entity)` / `State(entity)` 操作带 `SequenceComponent` 的实体；Play 会拒绝缺失组件、重复启动及另一个正在播放的序列。`State` 返回包含 `Playing`、`Time`、`Duration` 和 `Error` 的状态；缺失组件会报错。交互脚本可在 `Playing` 时暂停自己的动作更新。序列资源仍通过场景编辑与保存，项目脚本只引用实体，不把特定角色或关卡写进引擎。`MechanicGirlLab/Scripts/CSharp/MechanicGirlDemo.cs` 展示距离交互触发和结束后恢复控制。
`IScriptPhysicsEvents.Collision/Trigger` 在脚本线程收到真实 Jolt/Box2D 接触，Phase 为 Enter/Stay/Exit；
回调异常由脚本故障边界处理。接触对方销毁后可能为无效 Entity，读取前检查 `IsValid`。
角色虚拟控制器的独立碰撞回调不等同于刚体接触通知。

```csharp
private UiDocument? hud;
public override void Start()
{
    hud = UiDocument.Load(this, new("Assets/UI/Hud.hui"));
    hud.SetText("status", "Ready");
    Camera.Follow(Entity, distance: 6, yaw: MathF.PI, pitch: -.4f);
}
```

`AudioPlayback.Play(this, ...)`、`VfxPlayback.Play(this, ...)` 和 `UiDocument.Load(this, ...)` 返回可释放对象。
`VfxPlayback.Play(..., color: new Vector4(r, g, b, a))` 可为支持全局颜色的效果设置有限范围RGBA，范围是0到1；底层VFX请求若同时提供`opacity`，透明度相乘。不支持全局颜色的效果会明确报错并释放新播放句柄。项目可把颜色声明为`[ScriptField(..., Color = true)] Vector4`供Inspector编辑、保存和重开；例如SICKA暖落叶使用此能力按实际场景明暗调色，而不修改源Fab贴图。
`VfxPlayback.Update(position, forward, color: tint)` 可在效果播放期间更新同一全局色；省略 `color` 时保持原色。项目脚本可只在色值改变时发送更新，避免每帧重复设置。SICKA脚本字段在生成的Editor registry中有颜色控件元数据，保存/重开操作仍应在具体项目验收。
它们绑定创建者及场景，脚本结束会自动停止/卸载；提前 `Dispose()` 支持重复调用。
持续播放大量短音效/粒子时仍应及时 Dispose，避免保留已结束的所有权记录。已释放对象拒绝更新。
场景切换通过 `SceneFlow.Load()` 排队到安全帧边界；资源依赖用 `ScriptDependency` 声明以供 Cook 收集。

Python 公共脚本可定义 `Collision(self, contact)` / `Trigger(self, contact)`，读取 `Phase`、`IsTrigger`、
`Other`、`Point` 和 `Normal`。上述新增 C# 类型化场景服务目前不宣称 Web/WeChat 支持，portable IL 审计会明确拒绝。

## 项目模板与导出预检

```powershell
./Tools/Agent/hectl.ps1 project create -Project D:/Games/MyGame -Name MyGame -Template Scripted3D
./Tools/Build/Publish.ps1 -Project D:/Games/MyGame -Profile windows -Action Check
./Tools/Build/Publish.ps1 -Project D:/Games/MyGame -Profile windows -Action Build
```

Scripted3D 使用标准场景宿主和 C# 玩家脚本，资源引用引擎内置内容；不依赖 Survivor 游戏代码。
当前 Windows 发布通过源引擎 Shipping/Cooker，需要文档规定的 C++/.NET 工具链。
这不是免编译器的二进制导出运行模板；二进制 Editor 的现有发布限制保持明确可见。

发布预检将资源、UI 生成文件、脚本插件和平台组件问题写入 `Saved/Build/<profile>-preflight.json`，
包括源文件、字段位置及修复建议。Android 使用其专用 gameConfig/pluginConfig。
预检不替代后续 Cook、托管 IL 审计、平台打包和真机运行；动态脚本依赖仍需声明。

跨项目测试现在可用 `hectl test <name> -Project <项目>`，构建和运行路径均隔离。
`PythonRuntime`、`CSharpRuntime` 使用 `Examples/RenderingTests` 中的公共脚本 fixture，普通游戏测试继续使用默认项目。

## 演出镜头的DDS预读

若演出镜头会首次露出大量贴图，可在该场景的`SequenceComponent.PrefetchTextures`填写项目相对的`.dds`路径数组。场景开始时引擎检查路径仍在项目内并在后台读取原始字节；正常纹理加载仍负责解析和GPU上传。预读按场景清理，全部待命字节上限96MiB、单张32MiB，超预算或读取失败时正常加载路径仍可使用。Cooker把这些路径视为运行时硬依赖，打包前应确认路径有效。该字段对其他演出与项目通用；内容作者应只列真正会在近期镜头用到的纹理，并使用性能回放核验首次播放。
