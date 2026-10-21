# HEngine 1.1.0 脚本开发指南

先读 [能力审计](ScriptingApiAudit-1.1.0.md) 选择目标平台。二进制包提供 `Docs/Api/index.md`（逐类型参考）和 `Docs/Api/api-index.json`（可搜索签名），由实际 ScriptCore DLL 和 XML 生成，包含 public 数据属性、返回类型、泛型约束和默认参数；源码维护者使用文末生成入口。

## 创建与注册

先创建目标父目录，再使用 `hectl.bat project create -Project C:/Games/MyGame -Name MyGame -Template FirstPerson` 创建原生场景游戏；该工具不会自动创建 `C:/Games`。TopDown/Scripted3D 提供其他玩法起点。SharedArena 用于共享 C# 规则和 PortableView 导出。不要从零拼写程序集工程或场景序列化格式，先复制模板中已工作的一例。

C# 放在项目 `Scripts/CSharp`，继承 `ScriptBehaviour`，声明稳定的 `[ScriptType("game.my-type")]`。需要 Inspector 持久化的字段/属性声明 `[ScriptField("stable-field-id")]`，更名保留 former IDs。使用 `ResourceReference`/`EntityReference` 保存资源/实体身份，运行时 `Entity.Handle` 只能临时使用。场景添加 ScriptComponent，选择 `he.script.csharp` 后端及脚本类型。通过 Editor 编译按钮或 `Tools/Scripting/CompileEditor.ps1 -Project C:/Games/MyGame` 构建；查看真实诊断再进入 Play。

本轮 1.1.0 的新增脚本 API、教程及二进制游戏导出以 C# 为准；Python 扩展暂缓，现有接口继续保留。


## 生命周期与所有权

构造与恢复声明字段后 Initialize；启用调用 Enable，开始更新前 Start，之后进入 FixedUpdate/Update/LateUpdate。Disable/Enable 不重新 Initialize。暂停/恢复、场景卸载、销毁按宿主通知。异常由脚本边界报告并隔离出错实例，不能把日志中的异常当作成功。C# Reset/OnValidate 只能处理编辑数据，不能调用运行时 API。

引擎 API 在引擎主线程的有效脚本上下文调用。不要从 Task.Run、线程或普通 Python 后台线程操作场景。C# 的调度方法与 Lifetime 用于脚本管理的任务；对象销毁应取消任务并释放资源。Python 用回调 seconds 驱动计时，勿阻塞帧循环。

Entity 是会话句柄；删除请求后即失效，实体槽复用不会复活旧引用。C# 组件视图额外校验 generation，删后重加组件不会让旧视图恢复。`Entity.State` 是副本，修改后必须 Apply/赋回。位置和缩放是本地空间，旋转为弧度；`Transform.WorldPosition`/`Scene.WorldPosition` 查询求值后的世界空间。物理速度/射线采用世界空间，角色移动应走 Physics，不能逐帧直接覆盖动态刚体 Transform。

C# 优先用 `AudioPlayback.Play(this, ...)`、`VfxPlayback.Play(this, ...)` 和 `UiDocument.Load(this, ...)`，随脚本/场景销毁释放。显式 Dispose 可提前结束且允许重复调用；Dispose 后操作抛异常。Disable 不必然等于销毁；需要停止禁用对象的声音时在 Disable 释放，在 Enable 重建。


## 常用系统

| 目的 | C# |
| --- | --- |
| 查找对象 | `SceneServices.FindTag("Player")` |
| 读取动作 | `InputActions.Read("Move")` |
| 改变本地位置 | `var t = Entity.GetComponent<Transform>(); t.LocalPosition = p;` |
| 控制角色 | `Physics.MoveCharacter(Entity, velocity)` |
| 设置动画参数 | `new Animator(Entity).SetFloat("Speed", speed)` |
| 世界空间声音 | `Audio.Play(sound, new AudioOptions { Position = p })` |
| UI 文本 | `document.SetText("score", text)` |
| 导航 | `Navigation.FindPath(start,end)` |
| 切场景 | `SceneFlow.Load(new("Assets/Scenes/Next.he"))` |
| 存档 | `Engine.Api.WriteSave("progress", json)` |

动作先在 Config/Input.json 声明，名称区分大小写。UI click 在 `ui.event` 最新值通道中，通过 `Engine.Api.ReadEvent` 读取并保存游标；同名的新消息会替换尚未读取的旧值，不是可靠事件队列。动画事件的 ConsumeEvents 则是破坏性读取，应指定一个消费者。查找对象和同步资源加载不要每帧执行。

## 资源与导出

资源路径为项目相对路径，如 `Assets/Audio/Click.wav`；不使用安装目录、机器绝对路径或 `../`。动态字符串拼出的资源不会自动被 cook 发现。固定资源声明 ScriptDependency，或使用被序列化的 ResourceReference 字段。脚本示例里的占位资源必须替换为项目实际文件。

存档键为 1–128 个 ASCII 字母、数字、短横线或下划线；值上限 1 MiB。设置保存用 Config/*.json 对象，覆盖落在 Saved，不修改已安装资源。保存稳定 ID 与玩法数据，不保存 Entity、音频 voice 或服务 lease。

二进制工作流先 `hectl publish check -Project <path> -BuildProfile windows`，再 build；用 `hectl capabilities -Json` 和项目 BuildProfiles 确认平台。资源在 Editor 中可读不代表已进入导出包。复制旧 DLL 不能替代脚本编译；只更新 Editor 不能替代重建 Shipping/Development 模板。

## 调试与错误

缺对象先查 IsValid/TryGetComponent；没有资源/设备时根据 Play 返回 0 或 owned Play 异常显示可恢复诊断。非法参数、错线程、过期句柄、错误 owner、未知服务会失败，不会静默创建缺失系统。Python 公共 scene API 通常抛 RuntimeError，Python 参数转换可能抛 TypeError/ValueError；C# 的实体视图使用 InvalidEngineObjectException，底层服务异常携带原生诊断。

C# 使用 SDK 模板和 XML IntelliSense。遇到能力缺口先查审计和安装清单，再决定是否需要源码扩展。

## 维护者生成入口

正常构建 SDK 会自动执行下面两步。仅有 XML 的降级生成不包括反射提取的完整签名；正式发行必须包含 csharp-signatures.json，并通过输入哈希校验。

```powershell
dotnet run --project Tools/Scripting/ReferenceGenerator/ReferenceGenerator.csproj -c Release -- --assembly Engine/Managed/HEngine.ScriptCore/bin/Release/net10.0/HEngine.ScriptCore.dll --xml Engine/Managed/HEngine.ScriptCore/bin/Release/net10.0/HEngine.ScriptCore.xml --output out/agent/script-reference/csharp-signatures.json
py -3.11 Tools/Scripting/GenerateReference.py --xml Engine/Managed/HEngine.ScriptCore/bin/Release/net10.0/HEngine.ScriptCore.xml --output out/agent/script-reference
```
