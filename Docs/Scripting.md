# 统一脚本层实施记录

新游戏开发请从 [1.1.0 脚本指南](ScriptingGuide.md) 和 [逐系统能力审计](ScriptingApiAudit-1.1.0.md) 开始。本页是历史实施证据，下面的阶段状态不是当前公开 API 参考。

状态：部分实现，整体交付未完成。2026-09-11 已接入正式 PC CoreCLR 插件、
Python 私有兼容插件、公共 ECS 脚本组件和真实 PC 样例；当前实现与验证见
[ScriptingStatus.md](ScriptingStatus.md)（当前入口、结果与缺口）及
[ScriptingImplementation.md](ScriptingImplementation.md)（详细证据）。本页以下保留初始审计和
Web/微信最小运行时探针证据，不能将探针结果当成三平台脚本游戏验收。

## 审计与工作区保护

2026-09-10 初始状态保存在 `out/agent/scripting/initial-status.txt`。
已有修改包含 PBRTest 场景及大量未跟踪渲染资源，本任务不修改这些资源。
实际组件为 PythonScriptComponent 与 NativeScriptComponent，没有通用 ScriptComponent。
PythonScriptSystem 已处理 Start/Update/UpdatePaused/OnUI/Stop、owner 快照、自删除及启动回滚。
ScriptEngine 管理 CPython 3.11 和 UUID 对象表；编辑态不执行每帧更新，但存在 Inspector 模块重载。
PythonScriptComponent 当前持久化 Path；不能假定已有通用脚本字段持久化。

依赖泄漏位置包括 ScriptEngine.h、ComponentBase.h、反射生成模板、Editor 日志、RmlUI、
ThirdParty 的公共 headers/libs、Game 发布 Python DLL/标准库操作。
现有静态插件管理器支持清单、服务、host 和阶段，应扩展现有系统。
微信使用独立 CMake/WeChat.cmake，复用轻量 Level、SceneDocument、NativeScript 和 Sokol GLES3。
已有浏览器诊断宿主及真实微信 AppID；旧文档中 AppID 为空的描述已经过时。

## 实施路线与选型

1. 固定 SDK 10.0.302 / runtime 10.0.10，先构建真实 Mono Wasm C# 探针并检查微信宿主。
2. 通用值、字段描述、生命周期、后端能力与实例句柄；保留 NativeScript 直接调用。
3. 迁移 Python 私有实现，复用现有插件机制；接入 CoreCLR 的 hostfxr 原生宿主。
4. Web/微信采用独立的 .NET 与引擎 Wasm，分别拥有线性内存，经结构化值及句柄复制。
5. 补编辑器、导出检查、真实场景样例以及各配置/平台验收。

独立 Wasm 避免把引擎 Emscripten 4.0.23 与 .NET workload 工具链误认为 ABI 兼容。
渲染和帧循环仍归引擎宿主；托管端不得另建图形上下文。
解释器可能启用动态 Jiterpreter，因此探针显式关闭 Jiterpreter、线程和 SIMD，
并请求 native relink；这些设置仅是初始探针配置，不代表微信平台永久不支持这些特性。

官方依据（2026-09-10 核对）：
- [受支持的 CoreCLR hostfxr/nethost 宿主](https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting)
- [独立 .NET Wasm 的 JSExport/JSImport](https://learn.microsoft.com/en-us/aspnet/core/client-side/dotnet-interop/wasm-browser-app?view=aspnetcore-10.0)
- [Jiterpreter 设计与关闭设置](https://github.com/dotnet/runtime/blob/main/docs/design/mono/jiterpreter.md)

## 证据边界

`Tools/Scripting/Probe` 只验证真实 C# 执行、异常与 GC，不是游戏逻辑替代物，
其结果不能记作 PC/Web/微信完整游戏验收。所有游戏、插件组合、编辑器、Android/iOS
真机验收当前均尚未执行。不得用已有 C++ TouchSample 的通过结果替代 C# 验收。

## 当前代码与生命周期

`HEngineScriptCore` 是独立 C++20 静态库。公共头只有 STL；字段 JSON 编解码私有使用
仓库 nlohmann/json，不包含 Python、pybind11 或 .NET。HEngineRuntime 已链接此库，
但 Level/ECS 尚未调用它。独立契约程序的 PE 导入只有系统与 MSVC CRT，没有脚本运行时。
这仅证明公共核心的边界，不代表整个 HGame 已支持关闭 Python。

ScriptCore 实现注册检查、类型/字段 ID 冲突检查、平台检查、创建和回收、字段默认值/重命名恢复、
生命周期调度、异常后停止该实例更新、单次错误报告和禁止带活实例卸载后端。
实体有效性由宿主提供 validator；64 位实体句柄的分配和与 Level 的安全绑定尚未实现。
实例 ID 在一个 ScriptCore 会话内不复用，不可跨会话保存。序列化实体引用是独立字符串标识。

当前契约：先创建并恢复字段，再 Initialize，然后按 enabled 调用 Enable；disabled 实例
仍完成 Initialize。Update 只更新已初始化、启用且无故障的实例。Disable/Enable 不重复 Initialize。
回调内删除延迟到回调返回；批量更新遍历 ID 快照。停止时先禁用再 Destroy，无论回调是否抛异常
都 Release。字段 getter 自删除也延迟回收。回调里新建的实例最迟在下一次更新边界初始化。
Stop 回调过程禁止创建新实例；完成后允许重新开始。编辑态、场景停止/重开与字段 Inspector
目前没有与这套契约连接，不能宣称 Editor Play/Stop 已支持。

字段载荷为 schema=1，类型标签为 bool/int64/float64/string/vec2/vec3/vec4/entity/resource/enum。
只序列化值，不序列化运行时对象。限定 1 MiB、1024 字段、256 字节字段 ID 和有限嵌套；
拒绝不支持的标签、整数越界、非有限浮点和向量维度错误。未知字段在核心 Save 中保留；
已知字段类型不匹配时诊断并使用默认值。该编解码器尚未写入 .he 场景的正式脚本组件。

## 已实测的运行时路线

PC：SDK 10.0.302 / CoreCLR 10.0.10。`Platform/Scripting/CoreClrHost` 使用 hostfxr 的
initialize-for-runtime-config、load-assembly-and-get-function-pointer 和 UnmanagedCallersOnly。
原生宿主连续调用托管入口 10 次，每次经过分配/GC/异常捕获，验证了真实跨 ABI 返回。
hostfxr 上下文会关闭，CoreCLR 仍为进程级运行时；这不是可回收 AssemblyLoadContext 的验证。

Web/微信：Mono browser-wasm 10.0.10，解释执行，禁用线程、SIMD 与三个 Jiterpreter 加速开关，
保留 Wasm exception handling；native relink 使用 .NET Emscripten 3.1.56。
Web 与微信使用同一份 Program.cs 和同一份发布程序集/Wasm；PC 探针是单独的 PcEntry.cs，
尚未完成同一玩法源码的 PC/Web/微信复用。

微信实测问题与解决：

1. 没有全局 WebAssembly，WXWebAssembly.compile 接收包内路径，不接收 ArrayBuffer。
   导出时把 .NET 的三个固定 Wasm 特性探针写为包内文件，实际编译获得能力结果。
   编译/实例化只接受已记录的包内模块，未知动态 Wasm 明确拒绝。
2. 直接提供 instantiateWasm 会绕过当前 .NET 版本的 linker placeholder 替换。
   曾出现 Main 真正执行但 JSExport 表为空，不能据 Main 成功宣称互操作可用。
   修复为保留默认初始化流程，仅适配 WebAssembly 操作和模块/资源加载。
3. ESM/dynamic import 转为受控 CommonJS 映射；esbuild 固定 0.25.12，npm lock 带完整性信息。
4. .NET shims 放在私有模块环境中，保留原全局 fetch/WebAssembly。微信宿主全局对象 getter
   需要真实 receiver，因此继承代理使用 Reflect.get(target,key,target)。实测 hostGlobalsUnchanged=true。

没有改写 C# 为 JS，没有替换 CLR，也没有引入 Unity IL2CPP 或传统 Mono embedding。
微信探针画面为诊断文本画布，**不是引擎游戏画面**。适配尚未与现有 Sokol 目标共同运行。

## 复现入口

```powershell
# 首次工具安装；要求本机已有 SDK 10.0.302，global.json 禁止自动升级
.\hectl.bat scripting setup
# 原生宿主 + 独立公共核心测试
.\hectl.bat scripting probe-pc
# Web 发布
.\hectl.bat scripting probe-web
python -m http.server 8766 --bind 127.0.0.1 --directory out/agent/scripting/probe-web/wwwroot
# 浏览器打开 http://127.0.0.1:8766/index.html，点击 Execute C# Step(+7)
# 微信发布（仅本地构建导出，不打开 IDE、不上传）
.\hectl.bat scripting probe-wechat
# 在开发者工具中导入 out/agent/scripting/probe-wechat，点击画布
.\hectl.bat test ScriptCore
.\hectl.bat game check -Frames 5
```

Node 要求 18+；默认优先使用本机 Codex 的 Node，或设置 HE_SCRIPT_NODE。
默认 PATH 中 Node 16 不可用。构建器只复制清单可达的当前运行时资产，逐个核对上游 SHA-256，
记录转换前后 JS hash，并仅清理先前由该导出器登记的旧文件。导出随附 .NET MIT、第三方和
esbuild 许可证。PC 探针依赖本机 .NET 安装，不是 self-contained 游戏发布。

微信本机调用的真实入口为 `E:/Program Files (x86)/Tencent/微信web开发者工具/wechatide.cmd`。
本次已执行 `check_wechatide_status --skill-version 0.3.9`、`open_project_window`、
`simulator_refresh`、`get_simulator_console`、`automation_game_action` 和 `simulator_screenshot`。
完整参数与结果日志在 `out/agent/scripting/`；没有上传正式版本，也没有生成本轮手机预览。

## 验证记录（2026-09-10）

| 证据类型 | 精确入口/范围 | 结果 |
| --- | --- | --- |
| unit-test | `hectl.bat test ScriptCore` | PASS；core-tests-final.log。生命周期、字段恢复/编解码、异常隔离、旧实体失效、自删除、回调中 Stop、重复开始、平台/插件缺失 |
| unit-test | `out/agent/scripting/probe-pc-native/HEngineScriptCoreContracts.exe` | PASS；core-isolated-runtime.log；独立公共核心，无脚本运行时 |
| build-only | `cmake --build out/agent/scripting/probe-pc-native --target HEngineScriptCore` | PASS；core-isolated-build.log |
| runtime-manual | `hectl.bat scripting probe-pc` | PASS；pc-host-runtime.log，10 次真实 CoreCLR 调用 |
| build-only | `hectl.bat scripting probe-wechat` | PASS；wechat-workflow.log、probe-build.log、probe-export.log |
| runtime-manual | 浏览器 HTTP 页面，点击 Execute C# Step(+7) | PASS **仅探针**；7→18→25，GC 根与受控异常有效 |
| runtime-manual | 微信开发者工具探针启动、坐标触摸、截图 | PASS **仅探针**；wechat-touch-result-final.log / wechat-probe-final.jpg，7→18→25、hostGlobalsUnchanged=true |
| runtime-manual | `hectl.bat game check -Frames 5` | PASS 既有玩法回归；game-regression-receipt.log，READY、exit=0；未运行 C# 游戏 |
| Android / iOS 真机 | 本次 C# | 未执行 |

HGame 首次回归的 Storage 回执写入出现 Get-FileHash 不可用警告。工具链导入会重写进程环境，
回执不应依赖之后的 PowerShell cmdlet 自动加载；Build.ps1 已改用流式 SHA256 并回收流/hasher。
修复后重新执行同一 Game check，READY/exit=0，通过且不再出现回执警告。

性能仅有探针口径：浏览器首次记录 169 ms，后续重载 112.7 ms；微信最终记录 491 ms
（从探针 JS 开始到托管校验完成，包含诊断日志，不是首个游戏帧）。GC.GetTotalMemory(false)
浏览器 4,211,080 字节、微信 4,217,144 字节，仅为托管堆估计，排除 Wasm 线性内存、JS、图形和宿主。
探针清单列出的 24 项运行时/宿主/许可证合计 10,033,279 字节（不含清单自身、未知旧文件和 IDE 文件，
不是微信上传压缩包体）。尚未测正式游戏包增量、设备总内存、更新开销或 GC 帧停顿。

## 2026-09-10 的待办快照（当前状态见实施文档）

- C#/Python 正式可选插件、五种组合配置和关闭后的 HGame 构建/发布依赖验证。
- 既有 Python 私有化迁移、代表性玩法回归、Python/C# 同场景协作。
- ECS/Level ScriptComponent 与 ScriptSystem 接入、实体世代句柄、场景保存/恢复。
- 正式 HEngine.ScriptCore / GameScripts、统一绑定生成、引擎 API、脚本编译与发布增量链。
- Editor 添加/选择/字段编辑、错误显示、Play/Stop/重开。
- C# TouchSample 控制真实 C++ 引擎/Sokol，输入、变色计分、存档、重进、可控异常。
- 同一 C# 玩法的 PC、Web、微信运行与行为变更身份核对，以及 Android/iOS 真机。
- 正式发布裁剪、平台导出能力拒绝、游戏包体/首帧/总内存/GC 和跨语言调用性能。

当前没有账号权限阻塞；上述大部分是尚未完成的工程工作，不能描述为已完成或仅缺真机。
