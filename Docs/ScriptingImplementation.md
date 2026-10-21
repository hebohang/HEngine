# 脚本实现与验收记录

2026-09-11，进行中。PC、Web、微信开发者工具已执行真实 C# 游戏；完整发布矩阵、编辑器交互和真机验收仍未完成。
运行时兼容性探针证据独立记录在 [Scripting.md](Scripting.md)，不能代替游戏验收。

## 当前结构与依赖

- `Runtime/Scripting/ScriptCore`：只依赖 C++20/STL 的生命周期、通用字段和错误隔离。
  `ScriptBackendRegistry` 供项目选择的静态插件注册，拒绝在消费者仍持有后端时注销。
- `ScriptComponent` 保存后端 ID、类型 ID、schema=1 的类型化字段和 Enabled。
  一个实体最多一个此组件；原有 PythonScriptComponent、NativeScriptComponent 保留。
- `ScriptSystem` 连接 Level/ECS；`ScriptWorld` 分配单调递增的不透明实体句柄。
  entt ID 销毁通知永久废弃句柄，复用 entt 槽位不能恢复旧引用。场景字段中的实体引用保存 UUID。
- `Plugins/CSharpScript`：PC hostfxr/CoreCLR，SDK 10.0.302、运行时 10.0.10。
  `Runtime/Platform/Scripting/CoreClrHost` 使用受支持的 hostfxr 原生宿主接口。
  Windows 原生类型不出平台实现。程序集对象只在托管实例表中生根，原生端持有整数句柄。
- `Plugins/PythonScript`：既有 CPython 3.11、pybind11 实现迁入私有目录。
  Python headers/lib/DLL 由被选择的插件引入，公共组件和 ScriptEngine 门面不再包含 pybind。
  新 PythonBackend 接公共生命周期；旧 ScriptEngine 通过无语言类型的 ILegacyScriptRuntime 保持资产兼容。
  旧 Python Entity 带场景弱生命期，七种已暴露组件返回逐次解析的安全视图；嵌套字段与黑板方法不保留 ECS 地址。
- `Tools/Scripting/Api.schema.json` 是原生请求布局、版本和操作号的唯一声明。
  CMake 生成 C++/C# ABI。两种语言的自然语法包装调用同一 ScriptEngineApi 实现。
  当前 API 包括实体有效性、批量 Transform/Sprite、输入、实体销毁/查询、纹理引用、存档、日志和事件。

## 生命周期与字段

字段恢复在 Initialize 前完成；Initialize 后，Enabled=true 才调用 Enable。
停用触发一次 Disable，重新启用再触发 Enable，不重复 Initialize。编辑态不运行生命周期。
Update 异常记录含来源的诊断并隔离该实例，避免每帧重复日志；Stop 仍执行清理和 Release。
创建失败、回调失败均不得省略释放。场景停止/切换销毁所有实例，重进重新创建并恢复场景字段，
游戏存档由 Initialize 按样例约定覆盖计分。PC 运行时常驻进程；场景重复运行已测试，程序集重载未实现。
修改 C# 后需重新构建并重启宿主，不宣称可回收 AssemblyLoadContext 或动态 DLL 卸载。
Python 旧组件的显式模块重载保留；新公共 Python 后端当前同样声明 CanReload=false。

回调中请求销毁实体立即令其脚本句柄无效，实际 ECS 销毁在回调后的屏障执行。
公共核心对回调中的 Remove、Stop、启停操作延后排空；ScriptSystem 用快照避免组件修改破坏迭代。
缺失插件/类型时保留组件及原始字段，显示诊断；不会悄悄删除绑定。
公共脚本按本次实例注册次序更新，旧 Python、C++ NativeScript 保持各自系统路径；
跨实体顺序不作为玩法依赖，事件消费者必须容许下个更新才观察到消息。

字段支持 bool、int64、float64、UTF-8 string、float vec2/3/4、稳定资源/实体 ID、枚举类型 ID+int64。
不支持任意对象图、委托、裸指针。未知字段随场景保留；缺失字段使用声明默认值；
可识别的类型冲突保留旧数据并诊断。稳定字段 ID 与 FormerIds 支持重命名迁移。
C# 构建工具生成直接构造/读写访问代码，类型与字段不靠运行时扫描注册；完整发布裁剪验收仍待完成。
Python 公共类型显式列于 `Project/Scripts/PythonScripts.json`，字段绑定 Python 属性名；
此目录不会导入所有旧脚本，未迁移资产继续使用原组件。

## ABI 约定

所有引擎 API 在创建宿主的引擎线程同步执行。C++ 和托管异常在各自原生边界捕获，
返回非零状态与 UTF-8 诊断；Python traceback 在 GIL 内转成普通错误文本。
Request 使用 pack=8，PC x64 大小 160 字节；版本、长度和输出容量均检查。
指针只借用至同步调用返回，不保存托管对象地址；输出由调用方分配、被调用方复制。
字符串上限 1 MiB，日志/字段不反向保存解释器对象。Transform/Sprite 使用固定 13 个 float32 和写入掩码，
无逐字段字符串查找；编辑/保存字段使用低频类型化 JSON。
Web 使用独立 Mono 10.0.10 与 C++ Emscripten 4.0.23 Wasm 模块，各有独立线性内存。
WasmEngineApi 复用 PC 的类型化 EngineApi 语义；JSImport MemoryView 的 18 个 double 槽位
由 engine-transport.js 同步复制进引擎内存，再经固定操作号调用 ScriptEngineApi。
字符串单独复制，跨模块不传对象地址。冷路径的类型/字段描述使用类型化 JSON。
解释器的三项 jiterpreter 加速开关关闭；SIMD/线程关闭，Mono 模块仍使用 Wasm EH。
这套正式宿主已在浏览器及微信开发者工具执行真实游戏；微信设备支持仍需独立真机证据。
事件为场景内有界的每名称最新值+序号邮箱，不是无损事件队列。
C#/Python 生命周期回调绑定当前实体上下文，事件只能到达同一个 ScriptWorld；
停止清空邮箱，重进不继承旧消息，失效实体上下文被拒绝。

## 编写、构建和运行

`Project/Config/Plugins.json` 选择 `he.script.csharp` / `he.script.python`，当前本地配置均启用。
API 程序集为 `Engine/Managed/HEngine.ScriptCore`，玩法为 `Project/Scripts/CSharp/GameScripts.csproj`。
添加公共参数为空的 ScriptBehaviour，标注 ScriptType 和需持久化字段的 ScriptField。
构造函数不能调用引擎：构建注册工具要读取默认字段。引擎交互放在 Initialize 以后。
新 Python 类型使用公共 Initialize/Enable/Update(seconds)/Disable/Destroy；Entity 自动绑定为稳定句柄包装。
旧 Python Start(Entity)/Update(Timestep)/UI/Stop 不必重写。

```powershell
.\hectl.bat test ScriptCore
.\hectl.bat test ScriptEcs
.\hectl.bat test CSharpRuntime
.\hectl.bat test PythonRuntime
.\hectl.bat test SystemLifecycle
.\hectl.bat test SurvivorContentCatalog
.\hectl.bat game capture -Frames 30 -StartupScene Assets/WeChatSample/ScriptTouch.he -CapturePath out/agent/scripting/csharp-pc.png
.\hectl.bat game check -Frames 30 -StartupScene Assets/WeChatSample/ScriptCoexist.he
.\hectl.bat editor check -Frames 5
.\hectl.bat scripting matrix-pc
.\hectl.bat scripting build-web
.\hectl.bat scripting serve-web
```

BuildManaged.ps1 由 C# 插件 CMake 节点驱动：生成 ABI→编译 GameScripts→生成显式注册→发布运行时程序集。
各构建目录隔离 .NET artifacts，最终核对发布 GameScripts.dll 的 SHA256 与生成注册时身份，拒绝旧程序集混用。
global.json 和 RuntimeFrameworkVersion 固定 SDK/运行时；BuildManaged 使用 packages.lock.json 和 RestoreLockedMode=true。
新增/调整托管依赖时显式执行 `dotnet restore <project.csproj> --use-lock-file --force-evaluate`，审查 lock diff 后再构建。
Dev 使用机器固定版本 hostfxr；Shipping 使用框架依赖程序集，并在 Runtime/ 内携带
host/fxr/10.0.10 和 shared/Microsoft.NETCore.App/10.0.10，hostfxr 显式接收该根目录。
原 self-contained 发布实测触发注册/程序集身份不一致，且不符合当前原生宿主 API 的支持范围，已替换。
[官方原生宿主说明](https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting)
要求此类组件使用 framework-dependent 方式；随包携带固定共享运行时使玩家无需另装 .NET。
BuildManaged -Shipping 已实际产出该目录和逐文件 SHA256 manifest、许可文本；
运行时来源现为 runtime.lock.json 固定的微软官方 Windows x64 zip，下载和每次缓存使用均校验 SHA-512。
GetPinnedRuntime.ps1 只提取 host/fxr、shared/Microsoft.NETCore.App 和许可，不分发 dotnet.exe。
包内逐文件 SHA256 与受校验的原始压缩包身份一同记录；游戏包收集链仍需补齐。
不能把宿主探针或开发运行当作完整游戏分发验收。

## 已取得证据

证据位于 `out/agent/scripting/`；下面结果仅覆盖记录时的源码状态。

| 类型 | 命令或观测 | 结果 |
| --- | --- | --- |
| unit-test | `hectl.bat test ScriptEcs` | PASS，script-ecs-api-test.log；场景序列化、启停、缺失后端、实体复用；fixture 后端 |
| unit-test | `hectl.bat test CSharpRuntime` | PASS，csharp-runtime-test.log；真实 CoreCLR，移动/点击/存档、20 次强制 GC、异常隔离、重进/销毁 |
| unit-test | `hectl.bat test SystemLifecycle` | PASS，python-migration-tests.log；旧 Python 迁移回归，早于公共 PythonBackend 新增 |
| unit-test | `hectl.bat test SurvivorContentCatalog` | PASS，python-survivor-regression.log；原 Python 资产内容契约 |
| runtime-manual | `hectl.bat game check -Frames 5` | PASS，python-plugin-game.log；旧 Python 私有插件后的 HGame 启动退出 |
| runtime-manual | `hectl.bat editor check -Frames 5` | PASS，python-plugin-editor.log；早于新 ScriptInspector，不能代替其验收 |
| runtime-manual | Agent 启动 ScriptTouch，D 30 帧，点击(748,360)，重载同场景 | PASS 此子集；x=1.500002、计分1、变色画面、重进 score=1、生命周期清理；csharp-pc-interaction.png |
| runtime-manual | 初次 ScriptTouch capture | FAIL：黑屏。已发现缺少 3D 下 Sprite 收集和默认曝光太低，后续真实采集修复 |
| unit-test | `hectl.bat test CSharpRuntime`，两插件启用 | PASS，csharp-python-coexist-test-v2.log；真实 Python 读取 C# 计分事件并更新 ECS，停用/重启/清理 |
| unit-test | `hectl.bat test PythonRuntime` | PASS，python-common-runtime-test-final.log；真实 Python 字段恢复、异常、自删除后旧句柄拒绝访问、解释器存活保护 |
| build-only / 受控运行 | `hectl.bat scripting matrix-pc` | PASS，matrix-20260911-113211-740/results.json；none/python/csharp/both 各自全新输出，30 帧，编译/链接/运行文件依赖隔离；未替代 shipping 验证 |
| build-only / 受控运行 | `hectl.bat editor check -Frames 5` | PASS，script-inspector-editor-check.log；新 Inspector 已构建，尚未实际 UI 编辑/Play/Stop 验收 |
| build-only | Web 原生 `cmake --build out/agent/web-script-release --target HEngineWeb --parallel 4` + Mono publish | PASS，web-engine-build-v3.log、wasm-managed-publish-v2.log；正式插件和真实轻量引擎 |
| 有窗口浏览器集成自动化 | `node Tools/Scripting/test-web.cjs http://127.0.0.1:8768` | PASS，web-evidence/result.json；Chrome，真实键鼠、存档重载、启停/销毁、20 次 GC、异常单次记录、纹理绘制像素检查；截图人工画面审核仍 PENDING |
| build-only | `hectl.bat scripting build-web` | PASS，web-workflow-event-scope.log；完整入口包含 codegen、ABI、C# 注册、裁剪、引擎构建和导出 |
| unit-test | `hectl.bat test ScriptEcs` | PASS，script-event-scope-test-v2.log；两场景事件隔离、停场景上下文拒绝、重进消息清空；首次运行在构建后因缓存 lease 竞争未执行，独立重试通过 |
| unit-test | `hectl.bat test CSharpRuntime` / `hectl.bat test PythonRuntime` | PASS，csharp-python-event-scope-test.log、python-event-scope-test.log；场景事件修改后真实双语言回归 |
| 有窗口浏览器集成自动化 | `node Tools/Scripting/test-web.cjs http://127.0.0.1:8768 out/agent/scripting/web-evidence-event-scope` | PASS，场景事件修改后重新导出并执行全部交互回归，画面审核仍 PENDING |
| runtime-manual 画面审核 | 查看上述 Chrome initial.png / clicked.png | PASS，visual-review.json；青色居中棋盘精灵变为向右移动的橙色精灵，纹理清晰可见；人工图像审核与自动化分别留证 |
| 微信正式游戏 | 同一 C# 源码驱动 C++ Sokol | 未完成；已有探针结果不覆盖此项 |
| Android/iOS 真机 | C# 游戏 | 未执行 |

PC 交互源码标记为 `csharp-touch-v1`，当时发布 GameScripts SHA256
`a452823b89cdb13f54f2dda0fe6dfeeb6c501fcf39fae9e500e29729a969c936`。
20 次 GC 单测程序集为 `ba3a1344022bc3327809a5da7c254055f574313290ac7f0c9d251736c72d9e6b`。
二者来自不同构建配置，不能宣称跨平台产物身份一致。后续新增初始化事件后身份将变化，必须重新取证。

## 剩余验收

微信正式宿主与轻量引擎整合、语言/API 导出完整闭包拒绝、发布依赖隔离、
Python 旧引用失效安全、新 Inspector 实际编辑/保存/Play/Stop、PC 正式发布/裁剪、
C# 行为变更三平台同身份验证、游戏包增量/首帧/总内存/跨语言调用和 GC 帧停顿、Android/iOS。
这些主要是仍需完成的工程工作，目前不能概括为“仅缺账号或设备”。

## 包内 CoreCLR 宿主验证

`Tools/Scripting/BuildManaged.ps1 -Project C:/WorkSpace/HEngineDev/Project -WorkDirectory C:/WorkSpace/HEngineDev/out/agent/scripting/shipping-managed-v2 -OutputDirectory C:/WorkSpace/HEngineDev/out/agent/scripting/shipping-managed-v2/output -Shipping`
build-only PASS（shipping-managed-v2.log），GameScripts SHA256
`a2c63c77f3a3342c155ae0db52e6118274c83e859900e079142a06c56b85edc8`。

`HEngineManagedProbe.exe <Runtime>/host/fxr/10.0.10/hostfxr.dll <probe>/HEngine.ManagedProbe.runtimeconfig.json <probe>/HEngine.ManagedProbe.dll <Runtime>`
实际运行 PASS（app-local-host-runtime-v3.log）；framework 路径指向上述输出 Runtime/shared/…/System.Private.CoreLib.dll。
该探针执行 10 次真实 CoreCLR 调用、GC 根与受控异常，只证明宿主和运行时目录可用，不代表 HGame 发布通过。
hostfxr 参数含本机宿主路径和显式 dotnet_root。Windows 平台层把所有路径 make_preferred；
未规范化的正斜杠路径实测在 CoreCLR 创建时返回 0x80070057，修复后相同包通过。
PackageGame 现已按选中的 C# 插件调用 StageRuntime，Python DLL、CRT 和 pybind 许可按 Python 开关收集。
VerifyGamePackage 的根目录白名单随插件配置变化；Scripting 仅允许 CSharp 子目录。
Windows PE 检查通过 VerifyPackageRuntime 对照固定官方归档重新验证 CLR 文件，
只允许清单认证的 IL-only AnyCPU 程序集及 CLR 动态加载根；普通本地二进制仍须 x64 并通过导入闭包。
完整 `hectl package` 正在执行，尚未把这些接入标为完整发布 PASS。

2026-09-11 后续验证：`GetPinnedRuntime.ps1` 实际下载官方 35.17 MiB zip，SHA-512
`2161dfa1cf027cdc074de7195b5f206b17ebd829ae415b9e7c9ee5f06d3952b6583030022dbe0d6e9221b5c577c411d7cd5322241f6d2299d9c886641215699b`
与[微软发布元数据](https://builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json)一致。
Shipping 再构建 PASS（shipping-pinned-runtime-v2.log）；与构建机安装目录解耦。

`Tools/Scripting/StageRuntime.ps1 -SourceDirectory out/agent/scripting/shipping-managed-v2/output -DestinationDirectory out/agent/scripting/package-stage/Scripting/CSharp`
PASS（stage-runtime.log、stage-runtime-repeat.log），复制和核对 200 个文件，并生成 package-files.json。
源程序集、运行时清单身份、文件 hash、路径和额外文件均检查；拒绝覆盖未登记目录及已修改的旧暂存文件。
负向验证 stage-runtime-negative.json：元数据被改动、Runtime 混入额外文件均被拒绝，随后恢复测试源。
staged-official-runtime-host.log 确认真实 CoreCLR 从这个暂存目录加载，10 次调用/GC/异常通过。
后续完整 `hectl package` 已通过（见下文）；本段保留独立宿主验证的证据边界。

VerifyPackageRuntime 独立验证 PASS：200 个文件；package-runtime-verifier-negative.json
记录伪造 coreclr 清单哈希被固定归档校验拒绝，测试后恢复暂存文件。
为本轮完整打包，按 `hectl clean cache -CachePath out/agent/game-scripting-matrix-relwithdebinfo -WhatIf --json`
预览后执行了对应显式冷缓存清理。matrix-cache-clean-result.json 记录释放 6,946,081,717 逻辑字节；
部分 retired 目录删除失败且后续手动删除被策略拒绝，残余保留。矩阵历史结果/日志仍在 scripting/，
矩阵二进制和编译缓存需按 matrix-pc 入口重新生成；原结果不代表这些产物仍保存在原路径。

## Web 产物与测量快照

`export-web.cjs` 从 .NET boot manifest 收集并校验实际资源，拒绝来源哈希不匹配；
检查样例可达组件、脚本后端/类型/字段类型及资源路径。正式 API/IL 不支持项审计仍待补齐。
导出只覆盖上一份 manifest 登记且未修改的文件，拒绝覆盖未知或已编辑文件。
`game-manifest.json` 包含全部文件 SHA256 和生成注册时的 GameScripts 身份。
该 DLL 身份用于本次发布防旧包混用，不代表不同目标配置的 DLL 字节必然相同。
Web 使用静态 C# 插件注册，不含 Python 或 CoreCLR 桌面宿主。

首份正式包 buildId `abf2b0f0bc12f0bf43a97dafa9aac6e560425098e28f51a238b4f5341b9eedd4`，
GameScripts `b785f3bcd83913b423eb53b9098cb399b9f233f6340fe6fb1bd9f83859a86a20`。
28 个文件共 9,546,899 字节（未压缩，不含 manifest；不是相对 C++ 基线增量）。
Chrome 有窗口自动化记录首帧 145.6 ms；另一个内置浏览器观测 357.9 ms。
计时从 main.js 开始执行至首次 he_frame 返回并 gl.finish，包含 Mono/引擎加载，不含 HTML 初始导航。
引擎与 Mono 线性内存容量各 33,554,432 字节；这不是整个进程或峰值 RSS。
20 次显式 GC 的托管堆约 4.26 MB，同步耗时 0.4–3.0 ms。
记录期 193 次桥接调用累计 9.9 ms、最大 0.7 ms；这包含校验、复制及被调引擎操作，
且测试覆盖了多次场景切换，不是纯 steady-state 吞吐基准。
所有数据属于桌面 Chrome 快照，不能当作微信或手机性能。

后续修复：轻量宿主 schema=1 存档新增可选 sprite 对象，仍通过生成的 SpriteRendererComponent
序列化器读写；旧存档缺失该对象时使用场景初值。运行中的 C# 样例本就会按 Score 重算颜色，
所以新增回归特意在禁用脚本后保存/重载，证明无需 Update 重算也能恢复 Sprite。
新增 he_script_is_enabled 查询使浏览器按钮与重载后的真实启停状态一致。
`web-evidence-sprite-save-v5/result.json` 有窗口 Chrome 回归 PASS；包含 Sprite/Transform/字段、
停用重载、20 次 GC、旧格式存档、越出导出纹理范围的存档拒绝。
画布 framebuffer 重载前后尺寸和 SHA256 完全一致（reload-framebuffer.json）。
DOM 截图会因页面滚动条造成半像素布局差异，不能用 PNG 字节相等替代实际游戏 framebuffer 检查；
前两次截图比较失败日志保留，v3 的旧格式颜色预期也在核对 C# Update 后修正。
v4 的重载截图人工审核确认橙色棋盘精灵实际存在，不能把这项有限审核扩展为手机结果。

`hectl scripting serve-web` 使用 Node 内置 HTTP，绑定 127.0.0.1:8768，正确返回 application/wasm，
只提供导出清单中的文件。避免本机旧 Python MIME 配置导致 .NET 回退到 ArrayBuffer 加载。
该入口下 v5 全套回归再次通过且无 MIME 警告；首帧 138.8 ms（口径同上，不是稳定性能目标）。
当前 Web buildId `9c97bbbf197b6b5fdc2ce79976a0cfa98b9bc300b00e62162f903896682e967d`，
36 个文件共 9,569,439 字节，GameScripts DLL 身份仍为 b785…。

场景事件隔离及许可证补全后的当前包 buildId
`e57463b68deb1d5e246cf529715a9990459dd16557b9d7560a0993e5836e7241`，
36 个文件共 9,566,755 字节；GameScripts 源码未变，DLL 身份仍为上面的 b785…。
此次 Chrome 复验首帧 146.3 ms，20 次 GC 0.4–2.8 ms，193 次桥接累计 8.7 ms。

有窗口回归依赖 Playwright 和 pngjs（可由测试机 NODE_PATH 提供），启动自己的 Chrome 并在 finally 关闭。
截图保存在 web-evidence/initial.png、clicked.png；像素变化和至少三种内部颜色检查排除纯背景，
但仍需人工看图确认最终画面。页面按钮是验收宿主操作，玩法始终由 GameScripts 执行。


## 2026-09-11 PC 正式发布与微信真实游戏接入

`hectl package` 完整 PASS，日志 `out/agent/scripting/full-package.log`：HCooker
1890 个文件、51,789,334 字节；C# 运行时 200 个文件；194 个应用内 PE 的依赖闭包，
84 个经身份校验的 IL-only 程序集；DX12/WARP 完整启动 exit=0。
产物 `dist/StarbloomSurvivor/StarbloomSurvivor.exe`。此包使用原 Python MainMenu 入口，
且在后续 C# v2 改动之前构建，不作为 C# v2 shipping 样例运行证据。
`package-disabled-runtime-tests.json` 确认关闭任一插件时拒绝其残余发布文件。
优化 CMake 清单验证只缓存各 Files 元素，原校验规则保留；对相同 staging 的独立验证
PASS，31.924 秒（package-manifest-verify-timing.json）。

微信入口：`hectl scripting build-wechat` 先完整构建 Web 游戏，再将已校验产物导出到
`out/agent/scripting/game-wechat`。共享同一份 GameScripts 和引擎 Wasm；通过已有微信
host.js 的可选脚本适配钩子接入，不创建第二个画布/主循环。触摸、存档、前后台及
Sokol/WebGL 2 渲染由引擎宿主负责，玩法来自真实 Mono 托管调用。
`Engine/Source/Runtime/Platform/WeChat/ManagedRuntime.cjs` 的全局代理仅供 .NET loader 使用，WXWebAssembly 只实例化
已知包内模块；三个固定特性探针单独编译，未知动态 Wasm 被拒绝。Mono/引擎分别拥有内存，
沿用 Web 的 MemoryView 显式复制桥接。

预览实测发现两项差异并已修复：
- 普通单包 12,141 KB 被平台 4 MB 主包限制拒绝（preview-v2 日志）。Mono 改为普通 runtime/
  分包，先等待 wx.loadSubpackage 成功再加载；主包保留真正的 C++ 引擎。
- 上传校验器拒绝 Emscripten 的 optional chaining，尽管本地 IDE 能执行。导出用固定
  esbuild 0.25.12 将所有宿主 JS 降到 ES2019；不翻译 C#，不修改 Wasm。上传语法失败的
  完整日志很长，只能提取错误摘要，不直接打印。
微信 readFile 对原生 .wasm 文件的实际读取 PASS，因此移除重复 .wasm.bin，减少
2,864,704 字节。ICU 分区仍随包分发；未用不完整国际化配置换取体积。

最终此轮包 buildId `65fb2263524ba248fe8a27b50e9a011ce95cb02521f478042d2863170a18ad71`。
`wechat-game-preview-es2019.log` 的 auto_preview PASS：平台报告总计 9,625,209 字节，
主包 2,289,760、runtime 分包 7,335,449。已向已登录开发者微信推送预览；没有发布正式版。
这不是 Android/iOS 运行证据。

开发者工具可复现入口（使用本机安装路径）：

```powershell
$env:HE_WECHAT_CLI = 'E:/Program Files (x86)/Tencent/微信web开发者工具/wechatide.cmd'
& $env:HE_WECHAT_CLI -c Codex open_project_window --project "$PWD/out/agent/scripting/game-wechat"
# 等待真实精灵出现后，执行有实际触摸的回归；默认最后关闭该项目窗口。
pwsh -NoProfile -File Tools/Scripting/TestWeChatGame.ps1
```

回归前场景应处于启用状态；若保存过禁用状态，可在开发者工具控制台执行
`HEWeChatSession.reload(false)`。CLI automation_evaluate 的外层上下文通过
`globalThis.__gameContextWindow.HEWeChatSession` 访问真正游戏，不能假设两个全局对象相同。
`scripts.snapshot()/enabled(bool)/collect()/fault()/destroy()` 只供本样例验收操作。
`wechat-game-evidence-es2019/result.json` 记录真实触摸移动、计数、禁用、恢复位置/颜色、
再次启用、20 次 GC、异常后持续帧、销毁重进，全部 PASS。
另 `wechat-game-refresh-result.json` 记录完整模拟器重新编译后存档完全相等。
人工看过 `wechat-game-initial.jpg` 的实际蓝色棋盘精灵；后续截图仍只做有限自动化验证，
不能把 IDE 图像或无错误日志扩展为手机画面审核。

## 同源码行为变化与性能证据范围

玩法已从 `csharp-touch-v1` 每次 +1 改为 `csharp-touch-v2-three-points` 每次 +3。
PC `hectl test CSharpRuntime` PASS（csharp-runtime-v2-test.log）：分数 10→13，真实 Python
观察者经同场景事件读取到 13；`hectl game check -Frames 30 -StartupScene Assets/WeChatSample/ScriptTouch.he`
PASS（csharp-pc-game-v2.log）。Web 有窗口 Chrome 回归 PASS（web-evidence-behavior-v2b），
微信实触摸回归 PASS（wechat-game-evidence-es2019），均观察到 +3。
源码身份 `36ce8817ea469ab1267d395d4ed300057a9ad7b2824722e5b6d0223309f21e75` 在三平台相同；
cross-platform-source-identity-v2.json 可复核。该身份来自 MSBuild 实际评估的 Compile 输入
及 GameScripts.csproj 的相对路径/文件 SHA256，不含引擎与运行时源码。注册元数据内也嵌入该身份；
包仍以各自实际 DLL hash 和完整文件 manifest 检测旧产物，不能仅靠源码身份判断包完整。
Web/微信 DLL SHA256 `8bd3b86f76a783d633999accfbe3c2c17d5c52260de06cc119add9f48363d8ea`。

微信 IDE 最终转换包首个 he_frame + gl.finish 为 2623 ms（从 createHost 起，含分包/Mono启动，
不含 IDE 编译与网络预览下载；此前非分包 818 ms、首轮分包 1635 ms，属于单次观测）。
引擎与 Mono 线性内存各 33,554,432 字节；不是峰值进程 RSS。20 次显式 GC 和桥接统计
见 result.json，毫秒计时不适合作为细粒度性能承诺。相对 C++ 样例的发布增量、峰值内存、
代表性持续帧分布和手机 GC 暂未形成完整基准，仍待完成。


最新补验：`wechat-game-evidence-final-b/result.json` 共 13 项 PASS，额外证明一次故障只新增
一条异常诊断、含 GameScripts.Update 调用栈，以及销毁后触摸不会终止引擎。
CLI 的 `grep -A` 返回空匹配，改用文档支持的 `grep -n .` 后在本地计算日志差值；
早先采集断言失败原样保留。触摸结束通过 wx.showToast 展示原生保存字段中的真实 C# 分数。
基础库固定为 3.17.2。此最终包 buildId
`de40d4cb7612a06e3e74350a5cb56aa23d74a378fc322bda3eb2e8fff50f022e`；
`wechat-game-preview-final.log` auto_preview 再次 PASS，平台总量 9,625,669 字节，
主包 2,290,220、托管分包 7,335,449。真机问题已向用户提出，尚未收到设备实测反馈。

相对原 C++ 微信导出：逐文件校验原 23 个文件 SHA256 均一致后，基线 1,726,060 字节，
新包 9,697,636 字节，净增 7,971,576 字节（wechat-size-comparison.json）。口径为导出文件
有效载荷，均不含自身 manifest；基线含辅助浏览器文件，未把此口径当作平台下载量。

PC v2 自有 Agent 会话实际执行 D 键 20 帧、点击、2 帧、采集并停止；pc-v2c-* 为有效命令证据，
`csharp-pc-v2-interaction.log` 记录 v2 创建、score=3 saved=true、Disable/Destroy。
`csharp-pc-v2-final.png` 已复制保留，并经 1280×720 预览人工审核：黑色背景中的橙色棋盘精灵位于画面中心右侧。Get-FileHash 因 VS 环境引起的
PowerShell 模块自动加载失败，Session.ps1 改用等价 SHA256 文件流后启动成功。
最初同一 PowerShell 管道等待启动进程的包装未继续，后续操作在另一工具调用里针对已确认
自有会话完成并 finally 停止；不能把 pc-v2b 的后续无活动会话错误当作有效交互证据。
`hectl test SystemLifecycle` 最新 PASS（python-legacy-system-v2.log）。

## 旧 Python 引用安全（2026-09-11 补验）

Entity 保存 Level 的弱生命期标记，场景释放后 IsValid=false、GetLevel=nullptr；标记不进入序列化。
组件仅在首次暴露 Python 视图时分配生命期标记。拷贝产生独立身份，移动保留身份，替换使旧身份失效。
七种原有 GetComponent 类型及嵌套向量、黑板视图每次按 Entity 和字段路径重新解析，故 entt 存储搬移
不会留下悬空指针。持有的黑板绑定方法同样重新校验，组件替换、实体销毁、场景释放后抛 ReferenceError。
旧 Start 参数改为 Python 拥有的 Entity 值副本。公开脚本接口不引入 Python 类型。

兼容范围：保留 GetComponent 字符串别名、字段读写、方法调用以及 vec2/3/4、mat4 的复制与隐式参数转换。
安全视图的实际类型是 HEngine._ComponentView；不承诺 type()/isinstance() 等同原生组件类型。
不使用 __class__ 伪装：实测该属性使嵌入模块初始化失败，已撤回并保留失败日志。
应用不应从脚本保存原生地址；新增返回内部对象的绑定方法必须显式接入视图解析，不能直接返回借用引用。

原绑定巨型翻译单元拆分为模块注册与反射组件绑定，以修复 MSVC LNK1163 COMDAT 错误；
PythonModule 链接锚点保证静态插件仍保留嵌入模块注册。运行中的访问违规定位为 pybind 属性访问器
借用了已释放的临时接收者，现明确持有真正的绑定方法对象，再调用 Python ABI。

unit-test：`hectl.bat test PythonRuntime` PASS（python-legacy-reference-test-final-b.log），覆盖活视图写回、
存储移动、替换、实体槽复用、场景释放、保留方法和 100 次调用/10 次 GC，以及公共后端自删除。
`hectl.bat test SystemLifecycle` PASS（reference-system-lifecycle.log）；
`hectl.bat test CSharpRuntime` PASS（reference-csharp-runtime.log），真实 Python 再次观察 C# score=13。
这些结果不替代本次改动后的完整游戏、编辑器和可移植目标验证。

本次引用修复后的补验：`hectl.bat game check -Frames 180 -StartupScene Assets/Scenes/MainGame.he`
PASS（reference-python-main-game.log），原 SurvivorGame 实际完成 warm-up、adventure started、180 帧及结算退出。
`hectl.bat test SurvivorEnemyBehaviors` PASS；`hectl.bat editor check -Frames 30` PASS
（reference-editor-check.log），仅启动退出验证，尚未覆盖 Inspector 的实际编辑、保存和 Play/Stop。
`hectl.bat scripting build-wechat` PASS（reference-lifetime-portable-build-b.log），包括 Web 构建；
DevGame codegen 本轮 changed=8、deleted=0，之后实际引擎编译通过，不宣称 hash/mtime no-op。

有窗口 Chrome 新产物 14 项 PASS：web-evidence-reference-lifetime/result.json；clicked.png 已人工核对。
Web buildId bce75fbbad50b07fa77a608003c4b36f27b5616ed62671ff825e5896183ba7e7。
微信开发者工具新产物 13 项 PASS：wechat-game-evidence-reference-lifetime-d/result.json；
buildId 9278e7e52bb8ec75450e3781c0988ace67c27452bc9b8357607b66051d35a909。
托管程序集与玩法源码身份仍与 v2 一致。微信本轮截图未人工审核，不能复用旧图作为新图证据。

微信回归工具新增等待真实游戏帧，以及保存原状态证据后重新进入样例初始场景，避免上一轮
故障字段 throwOnUpdate=true 让下一轮触摸测试从已故障实例开始。三个前置失败保留在
wechat-game-evidence-reference-lifetime、-b、-c。-d 的 startup=2276ms 实际是 fixture 重入，
不能当作冷启动。后续脚本已分别记录 startup/reentryStartup，此记录修正尚待下次完整运行验证。
浏览器本轮首帧 192.2ms、20 次显式 GC 0.5–3ms；引擎/Mono 线性内存各 32MiB。
它们是单次本机测试口径，不是进程峰值内存或手机性能结论。

## 公共脚本的 PC cook 依赖检查

HCooker 现在直接复用无运行时依赖的 ScriptFieldsJson 解码器；公共 ScriptComponent 中 resource
字段进入强依赖闭包，包括没有 SpriteRendererComponent 等其他引用者的脚本独有资源。
未知字段类型和缺失资源会拒绝导出，诊断包含场景路径与实体/组件 JSON 位置。
新增 `--csharp-plugin enabled|disabled`，正式 PackageGame 将选中后端状态传给 Cooker；可达场景
引用关闭或未知后端时拒绝导出，即使 Enabled=false。旧 Python 文件检查继续保留。
当前检查不等于完整的程序集 API/类型/字段注册闭包验证，该部分仍未全部实现。

验证：build-only `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HCooker -Configuration RelWithDebInfo`
PASS（script-cooker-build.log）；unit-test `out/agent/editor-relwithdebinfo/bin/Programs/HCooker/HCooker.exe --self-test`
PASS（script-cooker-self-test.log）。
unit-test `pwsh -NoProfile -File Tools/Scripting/TestCookScriptExport.ps1` PASS
（cook-script-20260911-140657-088/results.json）：实际 cook 成功并检查报告中的脚本资源边；关闭插件、
资源缺失、非法字段各以 exit=1 拒绝。首次隔离输入缺少必需配置文件的失败保留，不计为脚本检查结果。

本轮 Windows computer-use 在 kernel 初始化时无法写入资产路径，重置后同样失败；未启动或操作
编辑器窗口，完整 Inspector 实际交互验收仍未完成。不是引擎界面测试失败，也不以单元测试替代。

## 最新 PC 插件矩阵（2026-09-11）

四种配置 build-only、owned runtime 和依赖边界均 PASS。汇总及每个可执行文件/截图身份见
out/agent/scripting/matrix-current-summary.json。none/python 来自 matrix-20260911-140043-047，
csharp 来自 matrix-20260911-141845-627，both 来自 matrix-20260911-142056-112。
有窗口截图的 1280×760 合成预览已人工审核：原生粉色、Python 绿色、C# 蓝色棋盘精灵；
双插件场景同时有绿色与蓝色两个精灵。共存结果同时检查真实 Python 读取 C# 事件日志。
运行命令分别是 `hectl.bat scripting matrix-pc` 与磁盘故障后的
`pwsh -NoProfile -File Tools/Scripting/TestPluginMatrix.ps1 -Profile csharp`、`-Profile both`。
矩阵不等同 shipping 验证，原四配置全量命令在 csharp 阶段曾因 No space left on device 失败。

正式 `hectl.bat package` 本轮已完成 Release 链接及着色器生成，但在准备完整项目快照时磁盘写满，
因此 FAIL，日志 script-export-full-package.log。Project/Assets + Scripts 为 14,107 文件、
5,531,225,930 逻辑字节，远大于最终游戏资源闭包；不得把此前较小 cook 包体当成构建空间需求。
tests-debug 独立缓存经 clean cache -WhatIf 后正式回收了 7,152,874,388 逻辑字节，
仍有工具报告的长路径残余目录，未手动删除。测试证据仍保留，下一次测试需重建该缓存。
矩阵四个已完成 PDB 的 NTFS 压缩不改变文件内容；原主要缓存 PDB 已压缩，未假定仍能重复节省空间。

PrepareCookInput 的逐文件复制改用 CMake 内置 COPY_FILE ONLY_IF_DIFFERENT，保留内容比较、
覆盖顺序和隔离输入边界，移除每个文件启动一个 cmake -E 子进程的开销；当前待正式包重试验证。

快照复制补验：`pwsh -NoProfile -File Tools/Scripting/TestCookScriptExport.ps1` 共五项 PASS，
证据 cook-script-20260911-142829-622/results.json。新增 snapshot-overlay-isolation 直接调用
PrepareCookInput.cmake，核对项目同名纹理覆盖、引擎源文件不变、引擎默认配置优先以及 DevOnly 排除。
故 COPY_FILE 变更已有隔离验证，正式 `hectl package` 重试仍待完成。
四个矩阵可执行文件启用 NTFS 压缩后再次核对 SHA256，均与运行验收记录一致。

## C# 依赖注册与可达类型检查

`[ScriptDependency("Assets/...")]` 可重复声明运行中动态选用的资源；继承声明与已暴露
ResourceReference 字段的非空默认值统一生成排序去重的 dependencies。生成器拒绝越界、绝对或
非规范路径，不再固定写空依赖数组。玩法源码身份仍为36ce8817ea469ab1267d395d4ed300057a9ad7b2824722e5b6d0223309f21e75；
API/注册器改动后的程序集字节身份已变化，旧运行包不能充当这批新程序集的运行证据。

HCooker 新增 `--csharp-metadata`，PackageGame 自动传入本次构建的 GameScripts.metadata.json。
可达 C# 脚本没有注册表或类型未注册时拒绝导出；生成依赖进入强资源闭包，包含没有场景字段覆盖的默认资源。
`TestCookScriptExport.ps1` 八项 PASS（cook-script-20260911-143959-364/results.json）。
`TestRegistryDependencies.ps1` 两项 PASS（dependency-managed/dependency-contract-20260911-144501-722/results.json），
使用真实编译程序集验证继承、默认值、显式依赖去重以及越界路径诊断。HCooker 和正式托管工程均已编译 PASS。
平台 API 调用闭包与所有字段注册兼容验证仍未全部完成。

## 发布恢复与脚本样例入口

script-export-full-package-retry.log 中 cook 1890 文件/51,789,334 字节、194 个 app-local PE 闭包、
84 个 IL-only 程序集及 WARP full-start 均 PASS；最终替换因旧包归属检查不认识 Scripting 目录而失败。
VerifyOwnedGamePackage 已增加严格的 CSharp 目录形状和既有 runtime/build 清单验证，拒绝未知目录与未列出文件。
对旧包检查 PASS 后，复用完全相同的已验证暂存包执行 PublishGamePackage.cmake 成功
（script-package-publish-resume.log），本地 dist/StarbloomSurvivor 已更新。此包构建早于上述依赖注册改动。

新的样例发布入口为 `hectl.bat package -GameConfig Config/Game.ScriptSample.json`。
GameConfig 必须是项目内 JSON 路径，只用于 PACK_GAME；不改 Project.heproject。
输出按项目与配置身份隔离到 dist/projects/<key>/StarbloomSurvivor，复用受锁保护的 Release 缓存。
无该参数的正常打包仍输出原目录；配置覆盖每次显式传入/清空。样例入口 ScriptCoexist.he，
同时包含 C# 与公共 Python 脚本，并声明所需 Python 目录清单与模块。当前正式样例构建进行中，尚未 PASS。

## 2026-09-11 15:11 增量验收

Windows 正式样例在 Unicode 验收目录中解包的着色器路径为 261 个 UTF-16 字符，普通 CRT
fstream 创建失败。PlatformPaths::ToFileIoPath 在 Windows 平台层提供规范绝对扩展路径，
PackagedProject 的文件流统一使用它；场景与包清单仍保存逻辑相对路径。修复后真实 WARP 启动完成。
Test-PackagedGame 改为核验该包清单中全部文件的存在和 StoredSize，不再强制任何项目携带原游戏的
HEngineUI/layout.py；原游戏可达时该模块仍被清单核验。sample-package-long-path 中保存了失败及成功 stdout。

正式 C#/Python 样例已恢复发布到 dist/projects/e0eb064065faf2b3/StarbloomSurvivor。
证据：long-path-shipping-build.log（HGame build PASS）、long-path-shipping-runtime-retry.log
（full-start PASS）、sample-package-final-policy.log、sample-package-final-dependencies.log
（194 PE、84 IL closure PASS）、sample-package-final-publish-retry.log（publish PASS）。
恢复发布第一次数值参数的输出根错误被安全检查拒绝；第二次使用准确的 namespace 根成功，未放宽安全检查。
原 hectl 样例执行仍记 FAIL；正在用 script-sample-shipping-final.log 重跑完整入口，不能预先算 PASS。
正式运行同时记录 Python observer Initialize/Enable、观察 C# score、Disable/Destroy 与 C# 对应回调。
这仅是有界真实启动/共存证据，不代替完整 PC shipping 输入验收。

默认 EntityReference/ResourceReference 的 Id=null 现在编码为空字符串；真实托管构建及
TestRegistryDependencies.ps1 PASS（dependency-contract-20260911-145317-513）。
HCooker 验证注册字段默认类型与已保存字段类型、枚举身份，按 FormerIds 查找旧名称；
缺失新字段和未知旧字段不因此丢失。字段失败定位到场景、实体、脚本与字段。
TestCookScriptExport.ps1 共11项 PASS（cook-script-20260911-151059-852），包括字段类型不匹配、
旧名称恢复和未知旧字段可达资源。仍未实现完整 .NET API/IL 平台闭包检查。

最新 Web/WX build-only：hectl.bat scripting build-wechat PASS（registry-portable-build-retry.log）。
首次构建因旧缓存租约占用 FAIL；释放后生成代码 changed=0/deleted=0，此处未声称 hash/mtime no-op 证据。
Web headed 浏览器14项 PASS，web-evidence-registry-final/result.json；手工核验 clicked.png 的真实纹理实体。
微信 DevTools13项 PASS，wechat-game-evidence-registry-visual/result.json；手工核验 touched.jpg。
最初 DevTools 就绪调用超时保留 FAIL；重开后运行成功。截图工具现在接收绝对路径且检查文件确实存在，
避免 CLI 将相对路径写入其他工作目录。测试结束已关闭项目窗口及本次 HTTP 服务。
Web buildId=7c861504b322cdf4e17040f1c519d6ee6baa5ac27cacba5dca7f5137848f16d9；
WX buildId=0dc8f77f2fe15a0d1dabb48427a131fe9bfdfe62344e17989befb8329dd364b1；
两者 managedIdentity=386db5c9777e65ed990ac0a037d2553a7e3a11b131d96a854b5048445ff873c1，
玩法源码身份仍为36ce8817ea469ab1267d395d4ed300057a9ad7b2824722e5b6d0223309f21e75。
PC CoreCLR 的构建程序集 SHA 不同于 Mono 发布产物，不声称不同目标二进制逐字节相同。

measurements-registry.json 保存口径和数据：Web 清单文件9,575,350字节，WX9,703,547字节；
PC样例全目录132,354,867字节，其中CSharp目录80,423,489字节。均为原始文件字节，不是压缩下载/上传包，
也不是相对C++基线的增量。Web首真实帧155.7ms，微信本机DevTools1549ms；后者宿主重进同为1549ms。
Web20次强制GC耗时0.3–3.1ms，WX为0–3ms（计时分辨率限制，不代表真正零耗时）；
微信引擎和Mono分别32MiB线性内存，排除JS、GPU与进程RSS。Android/iOS仍无真机证据。

成功恢复后使用存储工具完成原CookInput记录并按package-intermediate分类回收，磁盘恢复约6.57GB空闲。
随后开始的正式入口重跑会重新创建受保护快照；不得手工清理活动目录。

15:14 更新：script-sample-shipping-final.log 已记录 HCooker 1262文件、24,296,268字节、
194PE/84IL闭包、full-start以及正式Publish全部通过，工作流进程仍在自动清理收尾。
该入口编译发生在最新HCooker字段检查修改之前；最新字段检查已有独立HCooker编译与11项测试，
不要将这次包的cook行为作为新增字段拒绝检查的证据。样例运行代码/托管程序集为本轮版本。

## 2026-09-11 15:24 增量实现
Package.ps1/hectl 增加 -PluginConfig，按配置原始字节哈希隔离输出，并在证据目录保存用于构建的同一份配置快照。
无覆盖参数时明确清空 HE_PLUGIN_CONFIG。新增 Game.ScriptNative/Python/CSharp.json，不修改当前 Plugins.json。
TestShippingPluginMatrix.ps1 -Profile none|python|csharp|both|all 验证正式发布、依赖闭包、运行时文件及有界真实启动；
明确不代替交互验收。none 日志已到 full-start/Publish PASS，进程仍在自动收尾，待核对最终摘要。

PortableApiAudit.cs 使用 Reflection IL，无额外 NuGet 依赖，生成 portableApiContract=1 和 portableDiagnostics。
Web 导出逐可达脚本检查审计，微信使用通过审核的 Web 源包；PC 允许生成带平台诊断的注册表。
遍历游戏调用链、静态初始化、虚调用、接口实现和泛型游戏参与类型；未纳入可移植集合的 I/O、网络、线程、
反射及不支持的 unsafe 指令被拒绝，并附场景、实体、脚本和方法位置。这是保守 API 支持契约，不宣称
通用 CLR 可移植性证明；扩展允许集合仍需审核和对应平台测试。
真实注册器夹具验证间接 File.ReadAllText 被识别，安全数学脚本不被无关不兼容脚本污染。
TestPortableApiExport.cjs 调用真实导出器，验证 unsupported-call/missing-audit 均拒绝且无半发布输出；
dependency-contract-20260911-152134-192/export-rejections/results.json 两项 PASS。
接口分派新增夹具的最终验证仍待执行。此前 Web/WX 正常运行证据早于本次注册器修改，需刷新。

15:27 验证更新：shipping none 组合的外层退出码0、summary.json status=success。
证据 shipping-matrix-20260911-151643-450：1092个cooked文件、23,794,082字节；
递归闭包仅2个app-local PE，Python=disabled、C#=disabled、IL-only=0；
原生玩法真实有界启动和发布均PASS，未执行shipping交互。
发布路径 dist/projects/ab391f859a3cdfe6/StarbloomSurvivor，exe SHA256
61bed1313662e3f41c2df5e67767acb8fceb77f8191a5b1c30d4fdb4d143249e。

PortableApiAudit 接口分派最终测试PASS：dependency-contract-20260911-152459-741。
sample.touch和test.interface-access各产生1条File.ReadAllText诊断，test.dependencies/test.safe-math为0。
同目录export-rejections两项真实导出拒绝PASS，确认未发布目录。
仅Python shipping构建和Web/WX审计元数据刷新仍在执行，不预先记为PASS。

## 2026-09-11 15:35 当前验收边界
仅Python shipping矩阵PASS，shipping-matrix-20260911-152506-410/summary.json：
dist/projects/66816c6aeb9f6f32/StarbloomSurvivor，
exe SHA256=630a64c02d12a12dda89e24ad03d9a176a0bcf45398b6f50a7545423bfa8f1a2，
contentHash=c90228dfdd298e07。收尾的自动清理因Storage collector忙而延后，不将清理警告当成游戏运行失败。

带API审计的Web/WX build-only PASS（portable-api-platform-build.log）。
Web正向14项PASS并核验clicked.png，web-evidence-portable-audit/result.json，
buildId=dc0174a326ccff4515755d640eabf75b7912cb19d0d7abd12fb58460ef97ead6；
GameScripts身份仍386db5c9777e65ed990ac0a037d2553a7e3a11b131d96a854b5048445ff873c1。
该次并行有shipping构建，出现maxFrameCpuMilliseconds=85；首帧166.1ms、强制GC最大3.8ms。
85ms异常尚未隔离定位，不能把有后台构建干扰的记录当作稳定性能验收。

最新微信两轮自动化未通过：automator连接超时，未取得游戏帧，控制台为空。
TestWeChatGame就绪检查改为90秒限时的短只读请求轮询，仅重试明确的automator timeout；每次保留ready-N日志。
手工核验的wechat-audit-diagnostic.jpg是全黑模拟器，不是游戏运行PASS；此前版本13项PASS仍保留但不替代本次。
显式simulator_refresh已返回success，刷新后的验证仍在进行；不假定刷新成功等于游戏运行成功。

15:37 更新：显式 simulator_refresh 后，最新微信包13项运行检查PASS，
wechat-game-evidence-portable-audit-refresh/result.json；画面文件已保存，尚待下一轮视觉核验。
此前黑屏/空控制台与刷新后恢复表明宿主编译/加载状态影响了本次验证。
TestWeChatGame已将同样的刷新请求置于就绪轮询之前，仍必须观察真实首帧与所有交互，不把refresh回执当成PASS。

## 2026-09-11 15:51 进度与测量
仅C# shipping矩阵PASS：shipping-matrix-20260911-153756-814/summary.json，
dist/projects/ffb1ce85baf64add/StarbloomSurvivor；
exe SHA256=d87c3a391b872f9cd7d49625078672b028e830fb4a9cd49d842625e7b1bf8641。
both组合正式矩阵仍在执行。前三个包可用MeasureShippingSizes.ps1按回执验证exe/content身份并复现文件大小：
原生44,219,459字节，Python51,903,096（增量7,683,637），C#124,721,008（增量80,501,549），
C#目录80,435,965字节。原生/C# cooked StoredSize求和分别30,971,712/30,971,811字节，明确存在99字节场景差异。
这些为原始发布文件，不是压缩传输或NTFS分配，不能推断Web/微信增量。

Test-PackagedGame新增可选-MeasureMemory，对真实已发布进程约每50ms采样WorkingSet、PrivateBytes和当时OS峰值。
前三包验证通过，shipping-memory各目录保存逐样本JSON和stdout；所有采样包含DX12 WARP渲染器，
不是单独脚本堆：采样PrivateBytes最大值none329,994,240、Python335,495,168、C#334,938,112字节。
none为首次驱动/缓存启动，采样时长不同；不据此推断脚本启动时间或稳定驻留内存。

修复ScriptInspector重新选择当前后端/当前类型会清空字段的问题；editor-selection-preservation-check.log
记录HEngineEditor编译及ScriptCoexist场景30帧退出0。computer-use工具仍在初始化前返回kernel assets路径不存在，
所以实际Inspector点击、编辑和Play/Stop仍未完成，不能用启动检查替代。
已核验最新微信刷新后touched.jpg，实际纹理实体可见；此前13项自动化证据现补visualReview PASS。

新增MeasureWebGame.ps1/cjs：采集真实headed浏览器600帧的enabled/disabled/forcedGc三阶段，
统计引擎调用CPU、帧间隔、真实桥接与内存；仅包装转发引擎调用，不替代玩法。
强制GC20次，记录随后帧间隔。脚本注册存储外层运行并拒绝在编译进程存在时启动；
先用hectl scripting serve-web启动HTTP服务，采集结束后关闭服务。当前尚未执行测量，不记PASS。

## 2026-09-11 15:58 收尾
当前精简入口/证据/缺口改由ScriptingStatus.md汇总，本文件保留历史明细。
四种shipping矩阵均终态PASS，both为shipping-matrix-20260911-154609-051，
dist/projects/e9f91506cc9efb2b/StarbloomSurvivor，exe f477ae876dc6623a7c490a4b2b6903003de78ca5fbb203832438cdcd26496819。
shipping-sizes-all-profiles.json已校验四包身份，both原始文件132,367,343字节。
无编译进程时Web性能采集PASS，web-performance-20260911-155350-425；
enabled 600帧平均1.365ms/P99 2.8ms，真实桥接1803次/25.9ms，GC20次0.4–1.6ms。
本次预热后未重现85ms异常，不等同于根因已定位。HTTP服务和浏览器均已结束。
Windows四个真实包内存采样完成，both采样PrivateBytes峰值341,913,600、WorkingSet203,554,816字节，含WARP。

## 16:22 Inspector 字段兼容审计

枚举字段不能只比较 ScriptValue 的 variant index，还必须比较 ScriptEnum.Type。
DrawScriptInspector 现与运行时/cooker 的枚举身份判断一致。FormerIds 找到的旧值即使类型变化也保留在编辑视图，
显示类型不匹配并允许显式 Use Default，不再静默显示默认值掩盖旧值。
运行时 Add 对类型不匹配报告 InvalidField 后使用默认值；cooker 拒绝不兼容已知字段，二者故障策略不同。
`hectl.bat editor check -Configuration RelWithDebInfo -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30 -TimeoutSeconds 120`
build-only PASS、runtime-manual 有界启动 PASS（editor-field-migration-check.log，exit=0），不是实际点击验证。
computer-use 初始化本轮再次在执行前失败 kernel assets 路径不存在。
进一步源审计确认：C# 编译仍由 BuildManaged/CMake 进行；Inspector 只有类型/加载错误展示，编译任务和编译器诊断面板未实现，列入剩余工程工作。

## 编辑器异步编译与诊断（16:34）

Inspector 的 Compile scripts 通过独立的 IScriptCompiler 可选能力获取语言插件提供的编译命令。
只有 WITH_EDITOR 接口含此能力；公共接口使用路径/参数值，没有 .NET/Python 头文件。
C# 插件提供已配置的 PowerShell、SDK/CMake 工具入口，Tools/Scripting/CompileEditor.ps1 调用正式 BuildManaged，
真实编译玩法、生成注册表及发布程序集到独立 out/agent/scripting/editor-compile-*，登记外层 Storage 记录。
已有程序集不被替换，需保存场景、关闭编辑器，再经正常编辑器构建入口重建并启动；Play/Stop 不更新代码。
Python 后端不声明编译器能力，继续使用原有解释器工作流。

Editor worker 使用 jthread，退出时停止并 join；PlatformProcess::RunCaptured 在 Windows 平台层通过隐藏进程和 Job Object 管理子进程树，
只继承指定管道句柄，正常完成/取消/300秒超时均清理子进程。每轮限量读取输出以保留取消响应；最多保留1MiB尾部文本。
面板通过 ImGuiWrapper::S 缩放诊断区域。显示完整编译日志路径、错误位置和编译器消息，编译通过仍明确提示重启。

`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestProcessCapture.ps1`
unit-test PASS（process-capture-final-tests.log）：参数含空格、引号、反斜杠、空字符串，非零退出/stderr，超过1MiB输出尾部，取消、超时及可执行文件缺失。
`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestEditorCompiler.ps1`
unit-test PASS（editor-compiler-contract-20260911-163110-416）：经实际 RunCaptured 运行真实 C# 编译成功；复制隔离项目并增加语法错误后收到 error CS 和非零退出。
测试未修改 Project/Scripts/CSharp。上述证据不替代真实按钮点击、Play/Stop 或真机验收。
最终 `hectl.bat editor check -Configuration RelWithDebInfo -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30 -TimeoutSeconds 120`
build-only/有界运行 PASS（editor-compiler-complete-check.log）。
`hectl.bat game check -Configuration RelWithDebInfo -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30 -TimeoutSeconds 120`
build-only/有界运行 PASS（compiler-interface-game-check.log，92.2秒构建，exit=0），非Editor目标真实C#和Python初始化并通过事件协作。

## 编译任务退出复核（16:40）

RunCaptured 增加异常路径的显式进程等待，保证捕获文本分配异常也不会只依赖 Job 句柄关闭；
取消/异常诊断追加后仍统一限制输出到1MiB。实际测试启动二级子进程持续写心跳，父任务超时后确认心跳停止；
持续输出、启动前取消及此前参数/错误路径一起通过（process-tree-final-tests.log）。测试后未发现残留 ProcessCaptureTests.exe。
CompileEditor 传入 BuildManaged -IsolatedCompiler，使用本机10.0.302的 build/publish --disable-build-servers，
UseSharedCompilation=false 和 nodeReuse:false；元数据求值也禁用节点复用，防止交互取消依赖任务外的构建服务器。
普通CMake构建仍使用原来的增量路径。
`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestEditorCompiler.ps1`
unit-test PASS，editor-compiler-contract-20260911-163916-059（真实编译成功与语法错误）。
`hectl.bat editor check -Configuration RelWithDebInfo -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30 -TimeoutSeconds 120`
build-only / 有界运行 PASS，compiler-process-reap-editor-check.log。
相同参数 `hectl.bat game check` build-only / 有界运行 PASS，compiler-process-reap-game-check.log（11.4秒，exit=0）。

## PC 脚本区段实测（17:15）

为 CSharpBackend Dispatch/Update/Lifecycle、ScriptEngineApi 和 ScriptSystem 同步/快照增加现有Tracy区段。
未向无运行时依赖的 HEngineScriptCore 目标引入Tracy。普通构建和Wasm中这些宏关闭。
独立Tracy冷构建537秒 PASS（script-tracy-build.log）；为腾出空间，经官方cache WhatIf后回收独立editor-relwithdebinfo缓存，
报告实际删除7,829,661,873字节、1项失败保留；源码、发布包及既有日志保留。编辑器后续需要重新构建。

`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/ProfileScripts.ps1`
runtime-manual Tracy PASS：pc-profile-20260911-171423-674/result.json、scripts.tracy、zones.csv。
使用隔离Agent会话、预热120帧、真实TCP连接校验、固定D输入和600个Script.UpdateAndSnapshot区段。
采集与导出退出码均为0，游戏已通过自己的Stop入口退出。早期包装器的参数、stderr和退出句柄失败记录保留，不作为PASS。

| 区段 | 调用次数 | 平均μs | P99 μs | 最大μs |
| --- | ---: | ---: | ---: | ---: |
| Script.UpdateAndSnapshot | 600 | 124.803 | 215.680 | 368.105 |
| Script.Synchronize | 600 | 2.105 | 5.756 | 21.913 |
| Script.Snapshot | 600 | 113.124 | 179.391 | 320.694 |
| Script.CSharp.Update | 600 | 7.859 | 21.701 | 155.738 |
| Script.CSharp.Dispatch | 4800 | 3.583 | 16.143 | 155.526 |
| Script.EngineApi | 1800 | 1.549 | 5.465 | 145.605 |

P99使用nearest-rank；都是包含子区段的墙钟时间，包含instrumentation开销，不能相加。
它是带受控输入的PC开发版脚本更新采集，不是shipping实际键鼠验收、完整渲染帧时间或GC事件归因。
程序集和可执行文件身份在result.json；玩法源仍为36ce8817ea469ab1267d395d4ed300057a9ad7b2824722e5b6d0223309f21e75。
实测快照占主要开销。源码确认每帧逐字段进行7次ReadField跨运行时调用，再分别包装/解析JSON；后续应实现有验证的批量快照，
保留Python逐字段读取的重入语义、字段类型校验与未知字段保存。尚未把该优化标为完成。


### 批量字段快照（2026-09-11，已完成运行时回归）

PC Tracy 基线显示每帧7个字段逐一读取占用了主要脚本系统时间。
新增独立的可选 `IScriptFieldSnapshot` 接口，不改变 `IScriptBackend` 的虚表；
公共层只接收 `ScriptFields`，逐项验证声明字段集合、值类型和枚举身份，并保留未知持久化字段。
批量读取仅适用于无玩法回调、无实体生命周期副作用的生成字段访问器。
Python继续使用原逐字段读取路径，保留getter中删除实例的延迟回收语义。
C#的 `ReadFields=9` 操作在CoreCLR和MonoWasm使用同一生成字段注册表、UTF-8 JSON schema 1及1MiB上限，
一次跨运行时调用返回全部字段，原生侧只解码一次。独立Wasm内存仍通过现有复制桥接，不共享地址。
该优化没有降低保存频率，也不改变字段恢复与Initialize的顺序。

验证（证据均位于 `out/agent/scripting/`）：

- build-only：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HGame -Configuration RelWithDebInfo -EnableTracy` PASS，batch-snapshot-tracy-build.log。
- unit-test：`hectl.bat test ScriptCore -Configuration RelWithDebInfo`、`hectl.bat test CSharpRuntime -Configuration RelWithDebInfo`、`hectl.bat test PythonRuntime -Configuration RelWithDebInfo` 全部PASS，对应batch-snapshot-core/csharp/python-test.log。Core覆盖错误字段集合/类型及失败后保存，真实双语言覆盖GC、事件、场景停止重进、自删除与旧引用。
- build-only / unit-test：导入 `Tools/Agent/Toolchain.ps1` 的VS工具链后，`cmake --build out/agent/scripting/probe-pc-native --target HEngineScriptCoreContracts` 及 `out/agent/scripting/probe-pc-native/HEngineScriptCoreContracts.exe` PASS；dumpbin导入表只有Windows/VC运行库，没有Python/.NET。此项是公共层隔离契约，不能替代整个游戏插件矩阵。
- build-only：`hectl.bat scripting build-wechat` PASS，batch-snapshot-portable-build-retry.log。首次并行调用因共享codegen缓存lease占用失败，串行重试成功。
- runtime-manual：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestWebGame.ps1 -EvidenceDirectory out/agent/scripting/web-evidence-batch-snapshot` PASS，14项有窗口浏览器交互，clicked.png代表画面已检查；自有HTTP服务和浏览器已回收。
- runtime-manual：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestWeChatGame.ps1 -Cli "E:/Program Files (x86)/Tencent/微信web开发者工具/wechatide.cmd" -EvidenceDirectory out/agent/scripting/wechat-game-evidence-batch-snapshot-retry` PASS，13项，touched.jpg已检查，开发工具窗口已关闭。首次缺少显式打开窗口导致自动化连接超时；保留FAIL记录，入口已补齐打开和一次连接后刷新，仍要求真实引擎帧。

当前Web buildId：e4299a5abaa32f33b19f3454ddcd2b72df7bdb380f2b2469d301ae4719eb7d20。
微信 buildId：e02dc59c3fc3d1bee421ca568a6839113c8068e322eb3da06a49256885411f01。
共同managedIdentity：7c833ea9e24f1fa3ffbaf43d195cc2f3b94b100450fef9c99d93efe904a3491d；玩法源码身份仍为36ce8817ea469ab1267d395d4ed300057a9ad7b2824722e5b6d0223309f21e75。
这些新身份替代此前API-v2产物用于最新运行时验收；此前shipping矩阵、编辑器构建及内存/首帧测量仍属于各自旧产物。

`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/ProfileScripts.ps1` 两次空闲采集PASS：
pc-profile-20260911-174028-775和174157-812；比较见batch-snapshot-comparison.json。
与171423-674基线相比，每600帧Dispatch从4800降到1200；Snapshot平均113.124μs降为74.319/69.119μs。
P99分别为基线179.391μs、新版267.638/123.475μs，尾部存在波动，不能保证普遍改善。
所有区间为带Tracy的PC开发版包容墙钟时间，120帧预热、600帧持D；不能相加嵌套区间，也不是shipping或GC事件归因。
173007采集时有并行冷构建，已标记为性能比较无效，仅保留真实运行trace。
ProfileScripts现在拒绝在编译进程存在时开始。完成后对专用Tracy缓存先WhatIf再显式回收，删除5,689,137,259字节，1项失败残留保留，原始trace和证据未删除。

逐文件SHA256验证后的batch-snapshot-portable-sizes.json：Web 9,581,884字节，微信9,710,096字节；
相对原生样例差值7,837,109/7,968,311字节，未压缩、排除manifest自身，包含场景/宿主差异。
编辑器原生界面工具再次在初始化前报kernel assets路径不存在，实际UI验收和Android/iOS真机仍未通过。


### Editor与公共库的接口布局回归

实际Ninja配置证明HEngineScriptCore不定义WITH_EDITOR，而CSharpScript和Editor定义它。
旧版在IScriptBackend内部条件添加CompileCommand，造成虚表错位；独立混合配置程序实际复现
“CompileCommand called from runtime lifecycle: ABI mismatch”，随后实例创建失败（editor-script-abi-before-test.log）。
已将编译能力拆为独立IScriptCompiler，Inspector经dynamic_cast查询，公共IScriptBackend在所有配置中布局一致。
HEngineEditorScriptCoreContracts将后端以WITH_EDITOR=1编译，链接不带Editor宏的公共库；
验证创建、七次生命周期回调、销毁以及可选接口，修复后PASS（editor-script-abi-fixed-test.log）。
这是ABI契约验证，不是编辑器UI点击证据。


双插件正式发布复验：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestShippingPluginMatrix.ps1 -Profile both`。
首次在Windows PowerShell 5.1中把CMake stderr警告误当终止异常（shipping-matrix-20260911-175245-490，FAIL）。
外层现完整捕获日志、恢复错误处理偏好并检查等待后的实际进程退出码，随后真实复验PASS：
shipping-matrix-20260911-175501-950/summary.json，构建、cook、发布、递归PE/托管闭包、Unicode目录DX12/WARP启动exit0及C#/Python协作均通过。
包目录 `dist/projects/264dca7dd50ca1c9/StarbloomSurvivor`，exe SHA256为4abec837f4fedd45b7907ca180495eb26e4c7f1477118a75ec3f999217bf9e7d，内容hash为4a52e55c267d4d88。
interactiveAcceptance仍为NOT_TESTED；后续Editor专属编译能力接口修复不改变非Editor预处理接口。
经实际exe/hash及发布manifest核验，batch-snapshot-shipping-sizes.json：双插件132,370,415字节，纯C++旧基线44,219,459字节，差88,150,956字节，未压缩且场景不同。
本次完成验证的tests-relwithdebinfo缓存经WhatIf后显式回收5,485,805,804字节，1项失败残留保留；单测证据与发布包保留。


Editor最终build-only及有界runtime-manual：`hectl.bat editor check -Configuration RelWithDebInfo -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30 -TimeoutSeconds 120`。
冷构建580.3秒与随后确保最新头文件的增量构建17.0秒均PASS，两个实例均exit0；
batch-snapshot-editor-check.log、editor-script-abi-final-check.log。没有点击Play/Stop或编译按钮，不能替代UI验收。
最终混合ABI契约还验证三个接口并存时的批量快照调用，editor-script-abi-final-test.log PASS。
再次 `hectl.bat scripting build-wechat` PASS（editor-script-abi-portable-check.log）；
editor-script-abi-portable-identities.json证明Web、微信buildId及托管身份与本轮已观察产物完全一致，无需重复相同字节的交互。
Windows界面工具重置内核后再次初始化仍报kernel assets路径不存在，未取得额外UI证据。

本轮交互回执中的内存：Web和微信引擎/Mono线性内存均各32MiB，不是进程RSS。
Web测试保存并重载后的模块计时到首个完成帧为158ms（不是首次冷加载），20次强制GC最大3ms，返回托管堆约4.26MB。
微信该次宿主首次帧记录3391ms，fixture重进不重置该指标；不把它当独立第二次启动样本。
这些交互观测不能替代分阶段性能采集、自然GC事件归因或手机测量。


### 最新便携性能与真机预览（2026-09-11）

`powershell -NoProfile -File Tools/Scripting/MeasureWebGame.ps1`现在启动并回收自己的HTTP服务（默认8772），
核对运行中buildId/托管身份及测量结束后的manifest，拒绝与构建或存储回收同时测量。
`-Trace`另行采集浏览器时间线；原始JSON经[CDP Tracing](https://chromedevtools.github.io/devtools-protocol/tot/Tracing/)与
[IO流](https://chromedevtools.github.io/devtools-protocol/tot/IO/)分块落盘，拒绝事件丢失和超过256MiB的输出。
时间单位及事件格式参照[Perfetto对Chrome JSON的说明](https://perfetto.dev/docs/getting-started/other-formats)。

runtime-manual性能采集PASS：web-performance-20260911-182254-211，Chrome152.0.7977.84，新浏览器配置/上下文，OS文件及GPU缓存未控制。
游戏模块启动到第一个已完成帧388.7ms，不包含更早的导航和模块导入时间。
启用/禁用/强制GC各600个引擎帧；启用阶段引擎调用平均1.257ms/P99 2.50ms/最大4.40ms。
强制GC阶段帧间隔最大17.30ms；20次GC平均0.640ms/最大1.50ms，20个GC后帧间隔均有记录，最大17.00ms。
引擎和Mono线性内存各32MiB；不是RSS，禁用阶段不是纯C++玩法基线。

独立诊断采集PASS：web-performance-20260911-182518-559，时间线11,118,080字节，无事件丢失。
`node Tools/Scripting/SummarizeWebTrace.cjs <证据目录>`可复算trace-summary.json。
观测到22个主线程V8顶层GC事件（3 Major/19 Minor），最长1.532ms；20个显式托管GC宿主标记最长2.399ms。
三个阶段时钟标记偏移差约77μs，子毫秒区间重叠只能视为近似。
当前捕获类别中无主线程完整事件超过50ms；最长10ms引擎调用没有与所记录的GC区间重叠。
这不归因自然Mono GC，也不证明剩余时间来自特定运行时、调度或GPU。诊断开销与无时间线结果分开。
历史85ms记录实际来自并行shipping构建期间，已从稳定性能比较排除，旧事件根因仍未证明。

runtime-manual微信性能采集PASS：`powershell -NoProfile -File Tools/Scripting/MeasureWeChatGame.ps1 -Cli <wechatide.cmd>`，
wechat-performance-20260911-182718-817，`node Tools/Scripting/SummarizeWeChatPerformance.cjs <result.json>`复算。
首次已完成游戏帧2856ms；首批300帧CPU均值1.513ms/P99 4ms/最大6ms。
启用阶段300引擎帧900次托管→引擎桥接，总14.60ms；不涵盖全部反向调用或GPU完成时间。
20次强制GC均值1.05ms/最大2ms，19个已观察GC后回调间隔最大19ms；最后一次没有后续观察样本。
两线性内存仍各32MiB。该数据来自Windows开发工具，不能当作手机性能。
所有这轮Web/微信运行身份均与批量快照最终产物一致。

调试预览准备PASS：当前AppID的`create_preview_qrcode --qr-format image --qr-output <本地路径>`实际成功，
证据wechat-preview-20260911-183111-433；主包2,298,072字节，runtime分包7,340,057字节，工具报告总计9,638,129字节。
这里报告的是info.size，未测量网络传输压缩字节。原始返回中的二维码数据仅保留在磁盘日志，不进入对话或发布输入。
首次无窗口预览后的关闭请求失败，随后显式打开/关闭同一项目均成功，清理回执保留。
新维护入口是 `hectl.bat scripting preview-wechat`（先设置HE_WECHAT_CLI），也可直接运行PrepareWeChatPreview.ps1。
入口已完成只读验证和“未声明文件必须在上传前拒绝”的实际负向检查，测试文件清理后原包再次通过。
尚未重复其上传分支，以保留已交付的二维码用于设备验证；既有CLI预览流程已实际执行。
详见 [真机验收步骤](ScriptingDeviceValidation.md)。Android/iOS仍为NOT_TESTED，预览准备不算真机运行。


公共诊断及装载契约补齐（真实后端回归已通过）：
`ScriptBackendDescriptor.RuntimeLoading` 可不启动运行时直接查询：None 表示无外部运行时，
LinkedInterpreter 表示链接的解释器（Python；第一次创建时启动），BackendHosted 表示后端启动宿主
（PC C# hostfxr），HostProvided 表示外层宿主必须先启动运行时（Web/微信 Mono）。
这不是动态卸载许可；两个语言当前仍声明 CanReload=false、CanExecuteAsync=false。

`ScriptException` 是原生C++适配边界内的拥有文本的异常，携带 `ScriptError`；禁止穿越原生语言ABI。
公共层保留 Kind/Message/Stack，错误消费者抛异常也不阻断清理。旧 std::exception 消费者仍从what()获得完整诊断。
C# NativeEntry/WasmEntry通过共用显式Utf8JsonWriter输出scriptError=1消息和完整异常链，
Wasm宿主复制该诊断，不把它包成另一个JavaScript异常；宿主自身失败仍保留JS诊断。
Python在GIL内从异常类型和值提取简短消息，将pybind11原生格式化的真实调用栈复制到Stack，复制后不保留Python引用。
项目精简标准库没有traceback.py，诊断不新增该模块依赖；消息格式化失败仍保留原始调用栈。
Python该路径保持原有pybind11单异常栈的范围，不宣称展开Python的__cause__/__context__链。
Stack是可显示的格式化诊断（可包含错误标题、内部异常与源码定位），不是可执行协议或任意对象图。
本变更保留独立IScriptCompiler接口，未把WITH_EDITOR条件虚函数重新放入IScriptBackend。

ScriptCore/CSharpRuntime/PythonRuntime无窗口测试均PASS，最终Python适配也已在HGame重新构建和运行。
Web真实浏览器15项、微信开发者工具13项及代表画面复核PASS，具体命令、身份与失败后修复证据见ScriptingStatus.md。
最终Editor已再次冷构建并运行：final-refresh-editor-check.log，589.9秒、exit0，包含最后的Python格式化修复。仍无Editor UI点击证据。

PC真实窗口回归入口：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestPcGame.ps1`。
先用`hectl.bat game check -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30`构建当前源代码。
回归固定该开发版可执行文件SHA，复用真实Agent输入、ECS投影和runtime.scene.load接口，不加载模拟脚本。
它从ScriptCoexist生成独立UUID目录的场景变体，并使用会话Saved目录中的独立存档键；原场景与用户存档不变。
场景加载提交后显式完成一帧才读取ScriptSystem字段快照。finally停止同一ownerTag的会话并清理生成目录，截图和场景输入复制到证据目录。
本入口证明真实窗口内玩法及场景级生命周期；同实例启停由真实语言单元测试覆盖，不替代Inspector点击或shipping UI验收。
Session.ps1应在同一PowerShell进程调用；原生命令重定向可能等待长寿命子进程的继承输出句柄。判断Call成功使用响应ok，不使用遗留LASTEXITCODE。
