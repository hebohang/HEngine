# C# 工作流升级实施记录

2026-09-21，本轮本机实现与验证记录。纯净 Windows VM 和手机真机仍为 NOT RUN，不能据此宣称完整设备验收通过。开始时 `git status --short` 为空。
当前项目由 `hectl.bat project current -Json` 确认为 `Project`。

## 现状审计

- `Runtime/Scripting/ScriptCore` 已有字段恢复、异常隔离、重入清理和未知字段保留。
- `ScriptWorld` / `ScriptSystem` 已有稳定句柄、场景邮箱及回调后销毁屏障。
- `EditorLayer::OnScenePlay` 已用 `Level::Copy` 隔离运行场景，Stop 恢复编辑场景；需要补齐选择上下文、Pause/Step 和显式应用字段。
- `HEngine.ScriptRuntime` 静态引用生成注册表和 GameScripts，场景重启不清空程序集静态状态；尚无可回收上下文。
- `CompileEditor.ps1` 已有独立暂存构建，`PlatformProcess` 已有取消和进程树回收；尚未将成功产物接入装载。
- `ScriptInspector` 已有基础值、FormerIds 与类型冲突提示，复杂字段、元数据、事务化多选编辑仍需扩展。
- `BuildManaged.ps1` 已核对 SDK、锁文件、源码身份与生成注册程序集哈希；模块依赖图尚未实现。
- Shipping 已携带固定 CoreCLR 并校验普通/延迟 PE 导入与文件哈希；纯净 VM 验收不能复用开发机结果。

## 分阶段计划

1. 公共生命周期阶段、固定步进、稳定类型执行顺序、暂停状态与独立契约测试。
2. 桌面可回收装载上下文、根回收诊断、事务式编译/发布、Stop/重启契约。
3. Editor 状态机、编译策略、选择恢复、Apply Play Values 和 Undo/Redo。
4. 字段 schema 与元数据、编辑事务、多选和真实 UI 回归。
5. 稳定 ID 模块图、增量缓存、平台注册/导出闭包及确定性测试。
6. 类型安全 API、等待调度与完整样例；插件/平台/Shipping 回归与文档。

所有历史测试仅作为架构背景。本轮结果在执行后追加，未执行不记 PASS。
公共 ScriptCore 不引入语言运行时或 Editor 条件虚表；Wasm 保留独立内存和生成绑定。

## 已接入的工作流

Inspector 的 Compile Scripts 将编译放在独立 `out/agent/scripting/editor-compile-*/output`。
取消、失败或输入在编译期间变化均拒绝候选版本。Edit 中成功后加载；Play 中普通编译保留当前版本到 Stop，
Recompile and Restart Play 在成功后 Stop、加载、重新复制编辑场景并 Play。
DLL 不覆盖正在执行的版本。诊断回执是暂存目录中的 `compile-result.json`，完整输出在 `compiler.log`。

Editor 的稳定 hostfxr 入口位于 `HEngine.ScriptHost`，ScriptCore、Runtime 和所有玩法 DLL 属于同一个
collectible AssemblyLoadContext。新版本先验证清单/hash、API schema hash、源码身份、注册表身份及模块闭包，
再在预检上下文执行显式 Describe 并比较内嵌注册元数据；不扫描生命周期方法。
卸载释放调度器任务、取消令牌和协程，清空实例根/入口委托，再用 WeakReference 和 GC 验证回收。
仍存在任务根时保留旧入口并拒绝卸载；已经请求卸载但上下文无法回收时阻止脚本调用并提示重启 Editor。
这不是方法级热补丁。

`ScriptPlaySession` 是无窗口状态契约：Edit → Starting → Playing ↔ Paused → Stepping → Paused；
Playing/Paused/Stepping → Stopping → Edit。Starting 失败回 Edit，非法重入抛错。
编辑场景用 Level::Copy 隔离，Stop 恢复进入 Play 前的选择上下文。
Apply Play Values 只复制原实体上同一脚本的已注册字段，不复制新对象、未知字段或运行期实体引用，
通过 SceneEditingService 原有事务历史提交，Stop 后可 Undo/Redo。
持久化存档不参加场景回滚。再次 Play 重载托管上下文以清空静态状态；未声明跨场景持久化的对象随场景销毁。
运行控制使用独立工具栏行，长场景名在空间不足时省略，Pause 状态有明确标记。
Play/Paused 中 Ctrl+O 可以选择另一场景；取消或加载失败保留当前会话，成功验证新场景后先执行普通 Stop，再打开新场景进入 Edit。

## 生命周期顺序契约

当前执行顺序：字段恢复 → Initialize → Enable → SceneLoaded；
每帧 BeginFrame → 尚未启动的启用实例 Start → 0..N 次 FixedUpdate → 所有 Update → 所有 LateUpdate →
ScriptWorld 延迟销毁屏障和字段快照 → 场景后续系统/渲染提交。
FixedUpdate 为 1/60 秒；累计追帧上限 0.25 秒。暂停不运行场景更新，Step 提交一个 1/60 秒场景帧。
每个更新阶段按类型 ExecutionOrder、类型 ID、实体句柄、实例句柄排序；不要让玩法依赖同顺序实例的先后。
进入场景的创建批次按 ExecutionOrder、类型 ID、稳定场景 UUID 排序。
Disable 在实例失去启用资格时执行；Stop 执行 SceneUnloading → Disable（若曾启用）→ Destroy → Release。
单实例异常使该实例停止普通更新，但仍执行清理；编辑器正常关闭在 Stop 前发送 ApplicationQuit。
事件仍为场景邮箱，按发布序号读取，并不是隐式广播回调。
Reset/OnValidate 使用显式 ABI 回调槽；Reset 在首次选型或显式重置时执行，OnValidate 在字段事务提交时执行。验证上下文禁止引擎运行 API、协程和任务，拒绝递归验证；异常保留原字段。

类型属性可写 `[ScriptExecutionOrder(-10, Before = new[] { "game.consumer" })]`，也支持 `After`。
构建在所有模块注册合并后执行拓扑排序，检测缺失 ID、自依赖、重复约束和依赖环；未被约束的可运行类型按数值和稳定类型 ID 排序。
最终排名同时写入磁盘元数据和内嵌注册表，原数值保留为 `defaultExecutionOrder`。

## 模块定义 v1

模块定义放在项目 Scripts 下，所有路径相对项目根目录，引用使用 `id`：

```json
{
  "schema": 1,
  "id": "game.combat",
  "assembly": "Game.Combat",
  "category": "Runtime",
  "sources": ["Scripts/Combat"],
  "references": ["game.shared"],
  "api": ["HEngine.ScriptCore"],
  "includePlatforms": ["Windows", "Web", "WeChat"],
  "excludePlatforms": [],
  "defines": ["COMBAT_V1"],
  "unsafe": false,
  "generatedInputs": []
}
```

无 `.hemodule` 时继续使用 GameScripts.csproj。有模块时按拓扑构建，再生成逐模块注册表并显式合并。
重复程序集名/类型 ID、缺失引用、环、平台矛盾和 Runtime 对 Editor/Tests 的依赖拒绝构建。
Inspector 显示已加载类型所属模块及执行排名；编译回执包含各模块 built/cached/failed 状态和按稳定模块 ID 标注的诊断。
模块编译错误包含源文件、行列、CS 错误代码和该模块完整 compiler.log；依赖图错误使用 HEMOD001，并保留完整外层日志。
缓存身份覆盖源文件、定义、依赖身份、生成输入、API/schema/生成器、平台、配置和固定 SDK；
命中时仍验证输出 DLL hash。Editor 模块仅在 EditorHost 编译中可达，Shipping 清单只允许 Runtime 模块。
Web 和微信采用各自的平台筛选与构建缓存。`TestPortableModules.ps1 -Platform Web|WeChat` 构建两个实际 Runtime 程序集，
运行 Wasm 玩法交互并验证平台/Editor 排除，以及跨模块不支持 API 在输出发布前被拒绝。
便携入口在创建脚本前比较完整生成注册表和源码身份，不能只凭空壳 GameScripts.dll 的身份判断模块代次一致。

## 字段及对象语义

标量场景字段继续写 schema 1；含结构/集合时写 schema 2，读取兼容两版。稳定字段 ID、FormerIds、未知字段保留与枚举身份检查保持不变。
切换脚本类型保留字段，类型冲突在 Inspector 提示并允许显式 Use Default。
已接入 DisplayName/Tooltip/Header/Space/Range/Multiline/ReadOnly/HideInInspector/Color 和枚举选项；
资源/实体引用有选择、拖放和清除入口。`[ScriptStruct("stable.id")]` 仅允许公开值结构和显式 `[ScriptField]` 成员；List/一维 Array 使用稳定类型身份、typed items 和生成的 prototype。
聚合值上限为 1 MiB、嵌套深度 4、总值数 4096、单层字段/集合长度 256；拒绝循环、任意对象图和身份错配。未知嵌套成员及不兼容旧值在原生持久化合并中保留。
Ctrl 点击层级对象进行多选，同类型脚本可编辑并显示 mixed values；只传播实际编辑字段，各对象独立验证，整批通过现有场景事务入 Undo。当前多选仅在 Edit 支持。
编辑器原生插件可通过 `RegisterScriptFieldDrawer`/`UnregisterScriptFieldDrawer` 为稳定结构或枚举身份提供绘制器；回调只编辑草稿，必须在插件卸载前注销。Runtime/Shipping 不包含该接口。
ResourceType 在提交时检查资源类别；不匹配时保留原值并显示拒绝原因。AllowNull=false 同样在提交时检查。
扩展绘制器采用原生 Editor 插件接口，未提供托管 Editor 绘制器桥接。

`Entity.GetComponent<Transform>()` / `TryGetComponent<Sprite>()` 等通过数值绑定 ID 调用。
组件视图保存独立世代；移除后重加不会使旧视图重新有效。对象失效抛结构化异常，不使用假 null。
Transform.Snapshot 批量读写；Entity.Create/Instantiate、Parent、GetChildrenSnapshot 当前 Windows 实现，
可移植 API 审计明确拒绝未实现的平台调用。等待支持下一帧、固定更新、游戏/真实时间和条件，
由 Behaviour 生命周期取消并释放迭代器。后台任务必须 TrackTask 并响应 Lifetime；卸载使用所有任务共享的 250 ms 宽限期，
超时报告来源并拒绝卸载，任务结束后允许重试。旧 Native/Wasm API transport 在 Detach 后拒绝调用。
未通过 TrackTask 注册的后台 Thread/Task 无法枚举其内部引用；若其持有旧上下文，WeakReference 回收检查仍会失败并隔离该版本，要求重启 Editor。

`PrefabLoad.Begin(this, resource)` 使用现有 RuntimeResourceRegistry 的原生异步 Prefab 队列，`yield return load.Wait` 等待完成，`load.Instantiate()` 创建场景实例。
lease 不保存原生指针，采用独立请求 ID 和实体所有权检查；Dispose、脚本 Release、实体销毁和场景 Close 释放 pin。共享原生加载可以安全结束，不回调被释放的脚本。每场景最多 1024 个请求。
Prefab 异步 API 当前限定 Windows，构建时可移植审计拒绝 Web/微信调用。
`SaveSlot<T>` 与 `ScriptEvent<T>` 使用显式 `IScriptData<T>` 编解码契约、版本化 schema 和 64 KiB 上限，不依赖运行时对象扫描；邮箱订阅由协程所有者取消。
完整样例 `ScriptWorkflowSample.cs` 使用 `WorkflowProgress` 同时演示 typed save 与 typed event，存档键为 `workflow-progress-v1`。
`Entity.Instantiate` 复制最多 4096 个实体、深度最多 64 的同场景层级，重映射标量/结构/集合中的内部实体引用；外部实体引用保持不变。
任一字段 payload 非法则撤销已创建的副本，不遗留半完成层级。资源引用和普通字符串不会被当作实体 ID 改写。

`IScriptPhysicsEvents`、`ScriptContact` 和 `ScriptPhysics.Dispatch` 是显式类型安全扩展接口，契约夹具覆盖碰撞/触发器分流和释放后拒绝。
当前物理层尚未向该接口投递实际接触事件；这不是物理接入验收证据。

### 最小类型化存档/事件示例

```csharp
public readonly record struct Score(long Value) : IScriptData<Score>
{
    public static string Schema => "game.score/v1";
    public static string Encode(Score value) => value.Value.ToString(System.Globalization.CultureInfo.InvariantCulture);
    public static Score Decode(System.Text.Json.JsonElement value) => new(value.GetInt64());
}
// 在 Behaviour 的 Initialize 中订阅；Stop/Destroy 自动取消订阅。
// Subscribe(new ScriptEvent<Score>("score.changed"), score => Engine.Log($"Score={score.Value}"));
// 在游戏线程回调中发布和保存。
// new ScriptEvent<Score>("score.changed").Publish(new Score(42));
// new SaveSlot<Score>("score").Write(new Score(42));
```

缺失存档返回 false；损坏或 schema 不同的存档抛异常并保持磁盘内容。邮箱只保留最新值，多次发布可能合并，不能用作可靠消息队列。
所有引擎对象操作仅允许引擎线程；工作线程通过取消契约结束自己的计算，不能直接访问原生世界。

## 本轮验证检查点

- `unit-test PASS`：`hectl.bat test ScriptCore -Configuration RelWithDebInfo -Parallel 4`。
- `unit-test PASS`：`hectl.bat test ScriptEcs -Configuration RelWithDebInfo -Parallel 4`，包含组件世代和 Apply/Undo/Redo。
- `unit-test PASS`：`dotnet run --project Tools/Scripting/ReloadTests -c Release --artifacts-path out/agent/scripting/reload-tests -p:GeneratedApiDir=C:/WorkSpace/HEngineDev/out/agent/scripting/workflow-managed/api -- C:/WorkSpace/HEngineDev/out/agent/scripting/workflow-managed/output`，12 轮真实回收、活实例拒绝、失败候选保留。
- `unit-test PASS`：`dotnet run --project Tools/Scripting/WorkflowTests -c Release --artifacts-path out/agent/scripting/workflow-tests -p:GeneratedApiDir=C:/WorkSpace/HEngineDev/out/agent/scripting/workflow-managed/api`。
- `unit-test PASS`：`py -3.11 Tools/Scripting/TestModules.py`，10 项，包含真实 DLL 缓存和损坏失效。
- `runtime-manual PASS`（窗口启动检查）：`hectl.bat editor check -Configuration RelWithDebInfo -StartupScene Assets/WeChatSample/ScriptWorkflow.he -Frames 30 -TimeoutSeconds 120 -Parallel 4`。
- `runtime-manual PASS`（本轮 sky 实际点击）：Play/Pause/Step（Counter 547→548）、Apply、Stop（动态子对象消失，选择恢复）、Undo（0）、Redo（548）、保存（磁盘 counter=548，实体仍为 2）、Compile/Cancel。
- `build-only PASS`：`hectl.bat package -GameConfig Config/Game.ScriptWorkflow.json`，发布到 `dist/projects/8ed489dabc0a5318/StarbloomSurvivor`；实际完成 Cooker、200 项 C# 闭包文件校验、独立包白名单、PE 策略和含非 ASCII/空格路径的 WARP 启动。此前发现并修复 HCooker 生成头依赖、无模块 receipt 读取及空 PluginConfig 参数传递错误。
- 新增聚合字段后，独立 `HEngineScriptCoreContracts`/`HEngineEditorScriptCoreContracts` PASS；ReloadTests 再次 PASS，增加嵌套 FormerIds 和 List/Array 默认值恢复；TestModules.py 10 项再次 PASS。
- 真 UI Reset 将 Counter 548 恢复为 0，并保存 schema 2 字段；首次复制操作发现 ScriptComponent 不在 authoring adapter 白名单，已补适配并添加 patch/duplicate/remove/Undo 契约，复验中。
- 所有后续修改仍需相应复验；这里不是完整矩阵验收结论。没有性能/包体对比测量。

### 2026-09-21 04:38 检查点

- `unit-test PASS`：`py -3.11 Tools/Scripting/TestExecutionOrder.py out/agent/scripting/workflow-managed/generator/RegistryGenerator.dll`，3 个测试组，包含默认排序、跨模块约束以及非法图不改写元数据。
- `unit-test PASS`：ReloadTests 再次完成 12 次真实卸载；WorkflowTests 增加类型化 payload 与 Prefab lease 完成/取消/重复释放/失效拒绝测试并通过。
- `runtime-manual PASS`：`hectl.bat game check -Configuration RelWithDebInfo -StartupScene Assets/WeChatSample/ScriptWorkflow.he -Frames 120 -TimeoutSeconds 120 -Parallel 4`。日志 `run-game-relwithdebinfo-20260921-042825-223-83160.stdout.log` 实际包含异步 Prefab 实例化、存档写入及 Disable/Destroy；不是纯净 VM 验证。
- 真 UI 已验证异步 Prefab 出现在层级，Stop 删除动态实例并恢复选择；Vector2 1.25、枚举 Vertical、数组扩为 3 项均保存到磁盘，重开后保留。
- 真 UI 发现单对象编辑未进入 Undo 历史；改成场景 patch 后发现缺少生成序列化声明导致空 patch，已修复两处，仍需实际 Undo/Redo 复验。此前多选 Undo/Redo 已通过。
- ScriptEcs 新增真实原生异步加载测试；首轮因 headless 没有项目配置而失败，已改成独立临时配置夹具，复验中。
- 插件矩阵 none/python/csharp 的实际构建、运行及依赖边界已通过；both 构建曾撞上执行顺序解析器局部变量编译错误，错误已修复，both 复验中。最新原生资源改动后仍须核对全矩阵。
- 最新 Shipping 包复验中；此前 Shipping PASS 不代表后续修改已验收。
- Web 工具链下载曾返回 HTTP 200，但完整下载随后多次 TLS EOF 失败；不能将响应探测当作安装或构建 PASS。Web/微信完整导出仍未完成。

### 2026-09-21 11:25 验证检查点（覆盖前面的待复验记录）

- `unit-test PASS`：ScriptCore、ScriptEcs 的 RelWithDebInfo 测试，新增层级实例化内部引用重映射、保留外部引用、非法数据回滚和真实异步 Prefab 生命周期；日志 `workflow-core-remap.log`、`workflow-ecs-remap-release.log`。
- `unit-test PASS`：ReloadTests 最新 12 轮真实回收、任务超时/恢复、预检失败保留、Reset/OnValidate、元数据身份及残留上下文隔离；`workflow-reload-api-final.log`。对真正拆分程序集的夹具再次 BuildManaged 和 ReloadTests，`workflow-module-reload-final.log` PASS。
- `unit-test PASS`：WorkflowTests 包含失效 Entity 的结构化异常，正常实体访问仍只有一次 ABI 调用；等待、typed payload、Prefab lease、物理接口夹具和释放后拒绝，见 `workflow-api-final.log`。
- `unit-test PASS`：`py -3.11 Tools/Scripting/TestModules.py`，10 项（`workflow-modules-final.log`）；缓存键新增 ScriptCore 项目和公共 props/targets 设置。`TestExecutionOrder.py` 3 个测试组 PASS。
- `unit-test PASS`：`Tools/Scripting/TestEditorCompiler.ps1`，成功、语法失败、模块失败的稳定 ID/行列/日志回执，以及 PlatformProcess 取消、超时、持续输出和子进程退出；`workflow-compiler-final.log`，最终外层退出码 0。
- `unit-test PASS`：`Tools/Scripting/TestRegistryDependencies.ps1 -ManagedWork out/agent/scripting/workflow-managed`；真实生成器拒绝间接文件 API、接口/BCL 回调中的不支持调用，并保留无关安全类型。`TestPortableApiExport.cjs` 的 3 种真实导出拒绝 PASS，证据 `workflow-portable-rejections/results.json`。
- `unit-test PASS`：`Tools/Scripting/TestCookScriptExport.ps1 -Cooker out/package/build/bin/Programs/HCooker/Release/HCooker.exe -EvidenceDirectory out/agent/scripting/workflow-cook-final` 的 11 个 Cooker 用例结果 PASS。该脚本最后一个预期失败原生命令会留下非零 LASTEXITCODE，不能把调用方直接转发该值当作测试断言失败；结果以逐项 results.json 和脚本未抛异常为准。
- `build-only + runtime-manual PASS`：`Tools/Scripting/TestPluginMatrix.ps1 -Profile all`，none/python/csharp/both，构建、运行、依赖边界全部通过，4 张画面逐张检查，证据 `matrix-20260921-105523-314/results.json` 和 `visual-review.json`。
- `build-only + runtime-manual PASS`：Editor 样例 30 帧检查，`workflow-editor-final.log`；代表性 C# Survivor 场景 `hectl.bat game check -Configuration RelWithDebInfo -StartupScene Assets/Scenes/MainGameCSharp.he -Frames 120 -TimeoutSeconds 120 -Parallel 4`，`workflow-game-regression.log`，退出码 0 且日志确认真实 C# 会话初始化。
- `runtime-manual PASS`：真实 Inspector 操作补齐单对象 Undo/Redo、不同值 Copy/Paste/Undo/Redo（List 1↔2、vec4 1↔2.75）、类型切换 Undo、缺失类型诊断/恢复及磁盘字段等值；结构/List 中文文本、Min 限制、错误资源类型拒绝、多选 mixed、保存重开均验证。临时场景已移至 `out/agent/scripting/ui-fixtures`，不加入项目资产。
- `runtime-manual PASS`：Pause/Step、取消打开场景保留 Pause、Play 中打开新场景先执行 Disable/Destroy 再回 Edit，重进 Play 恢复持久化存档。不同于场景数据，存档不被 Stop 回滚。自有 UI 会话最终退出码 0。
- `build-only PASS`：Web 与微信工具链已完成固定哈希校验并实际构建，早先 TLS 下载失败已解决。微信显式使用 HEPlatform=WeChat、独立暂存目录；便携托管产物关闭 Wasm EH/SIMD/threads，导出器实际检查 native loader。
- `runtime-manual PASS`：`Tools/Scripting/TestWebGame.ps1 -EvidenceDirectory out/agent/scripting/workflow-web-registration-retry`，16 项浏览器交互检查；新增同 DLL identity、不同注册代次在首次实例创建前拒绝的测试。
- `runtime-manual PASS`：`Tools/Scripting/TestWeChatGame.ps1 -Cli 'E:/Program Files (x86)/Tencent/微信web开发者工具/wechatide.cmd' -EvidenceDirectory out/agent/scripting/workflow-wechat-registration`，13 项真实模拟器交互检查。不是手机真机证据。
- `build-only + runtime-manual PASS`：最终 `hectl.bat package -GameConfig Config/Game.ScriptWorkflow.json`，日志 `workflow-package-final-retry.log`，退出码 0；200 个固定 C# 闭包文件、PE 依赖策略、独立发布白名单、中文/空格路径 WARP 启动通过。上一轮编译期间输入改变被正确拒绝，未发布不一致候选。包内无 ScriptHost、Editor、Tests、CS/CSProj/PowerShell 文件。
- `static-check PASS`：`git diff --check`。ScriptCore 开启 XML 文档与 CS1591 错误门禁，最新托管构建通过；生成 API 文档仍由单一 schema 生成。

上述记录不把模拟器等同手机，也不把本机 Shipping 启动等同纯净 VM。没有进行性能或包体差值测量。

### 便携模块最终复验

以下均为本轮实际执行，不复用历史平台结论：

- `unit-test + runtime-manual PASS`：`Tools/Scripting/TestPortableModules.ps1 -Platform Web`，证据 `portable-modules-Web-20260921-112156-810`。
- `unit-test + runtime-manual PASS`：`Tools/Scripting/TestPortableModules.ps1 -Platform WeChat`，证据 `portable-modules-WeChat-20260921-112339-535`。两者均构建 shared/gameplay 两个实际 DLL、运行 16 项浏览器 Wasm 交互，验证 Windows-only/Editor 排除及跨模块 File API 拒绝，不产生失败输出目录。
- `runtime-manual PASS`：将后一夹具的 `package` 用 `node Tools/Scripting/export-wechat.cjs <package> <wechat>` 导出，执行 `Tools/Scripting/TestWeChatGame.ps1 -Cli 'E:/Program Files (x86)/Tencent/微信web开发者工具/wechatide.cmd' -ProjectDirectory out/agent/scripting/portable-modules-WeChat-20260921-112339-535/wechat -EvidenceDirectory out/agent/scripting/portable-modules-WeChat-20260921-112339-535/wechat-evidence`。实际多模块微信模拟器 13 项交互 PASS，截图逐张检查，关闭窗口回执 success。
- `runtime-manual PASS`：注册表篡改测试首次断言错误地要求未建立的 running 属性必须等于 false，已修正为“不运行且从未 Initialize”；复验 16 项通过。保留原 FAIL 日志 `workflow-web-registration`，不将它标为 PASS。

## 支持边界与后续设备验收

- 重载是整个玩法上下文卸载/重装；没有方法级热补丁。不可回收时拒绝后续调用并要求重启编辑器。
- 组件绑定目前覆盖 Transform、Sprite；扩展组件应增加单一 ABI schema/显式绑定及生命周期测试。Windows 层级创建、复制和异步 Prefab 在 Web/微信导出审计中被拒绝。
- 物理碰撞/触发器目前是已测试的显式接口与夹具；没有声明原生物理事件已经接入。
- 资源身份沿用项目相对稳定资源标识；ResourceType 使用引擎已知资源类别校验。任意托管对象、裸指针、运行句柄不能进入持久化字段。
- 自定义绘制器是原生 Editor 插件扩展。聚合 UI 支持嵌套值与增删，尚不提供集合拖拽排序及嵌套成员的全部外观属性。
- 所有场景实体随场景停止释放，没有隐式跨场景常驻对象。需跨场景的数据通过显式存档恢复，不能携带旧场景句柄。
- 未登记 Thread/Task 的内部根无法枚举；登记任务有取消/250 ms 宽限期/来源诊断，未知根由真实回收检查发现并安全隔离。
- 下一步设备验收：在断网纯净 Windows VM 打开下述 wsb；在微信手机端运行导出包并核对启动、触摸、存档和退出。两项尚未执行，不以本机或模拟器结果替代。

## 开发者使用入口

1. 打开 `Assets/WeChatSample/ScriptWorkflow.he`，选中 CSharp Workflow Sample；或添加 ScriptComponent，Backend 选择 `he.script.csharp`，类型选择 `sample.workflow`。
2. 在 Edit 修改字段，一次控件提交形成一个 Undo 事务；Reset 和 Use Default 同样可以撤销。Ctrl 多选相同脚本类型只传播实际编辑过的字段。
3. 修改项目 Scripts 下 C#，通过 Compile Scripts 编译。失败在 Inspector 显示错误代码、模块、源文件行列与日志，打开诊断可定位源代码。Cancel 取消整个构建进程树。
4. Play 从编辑场景复制运行副本。Pause 后 Step 推进一个固定帧；Stop 丢弃运行对象并恢复编辑选择。要保留原对象的注册字段，先 Apply Play Values，再 Stop；编辑态可以 Undo/Redo。
5. Play 中 Compile Scripts 保留旧代码直到 Stop；Recompile and Restart Play 成功后安全重启。任务超时先修正取消逻辑；若提示旧上下文仍存活，隔离状态下不能继续调用它，需要重启 Editor。
6. 引入模块时先创建 shared 定义，再让 gameplay 的 references 引用它的稳定 ID；不要用文件名作为引用。原 GameScripts.csproj 在无模块时继续使用。
7. 发布使用 `hectl.bat package -GameConfig Config/Game.ScriptWorkflow.json`；玩家运行包内 EXE。脚本编译需要开发机 SDK，玩家包只带固定运行时。

关键实现入口：`ScriptCore.cpp` / `ScriptSystem.cpp` 负责原生调度，`ScriptPlayState.h` / `EditorLayer.cpp` 负责编辑运行状态，
`HEngine.ScriptHost/NativeEntry.cs` 负责可回收装载，`CompileEditor.ps1` / `BuildManaged.ps1` / `Modules.py` 负责构建与身份，
`ScriptInspector.cpp` / `SceneEditingService.cpp` 负责 UI 事务，`RegistryGenerator` 负责注册、字段 codec 和可移植 API 审计。
不通过运行时按方法名反射发现生命周期，不保存托管地址，不跨 Wasm 内存传递指针。

## 纯净 Windows 验收

使用 `hectl.bat package -GameConfig Config/Game.ScriptWorkflow.json` 构建专用工作流样例包。
在开发机运行：

```powershell
Tools/Packaging/New-CleanWindowsSandbox.ps1 -PackageDir dist/projects/8ed489dabc0a5318/StarbloomSurvivor -EvidenceDirectory out/agent/scripting/clean-windows
```

打开生成的 Workflow.wsb，或将包与 Test-CleanWindowsGuest.ps1 拷入断网纯净 VM 执行。
脚本检查开发工具缺席，从中文含空格路径运行两次，要求 C# 样例、存档写入/恢复日志、退出码及 app-local CoreCLR，
采样的模块路径必须来自包或 Windows；未观察到 CoreCLR 会失败，不会推定通过。
验收脚本是外部测试工具，不复制进玩家包。首次生成结果固定为 `NOT RUN`，只有 Guest 实际执行成功才写 PASS。
VM 运行目前 **NOT RUN**；本机启动不能证明纯净机器闭包。
