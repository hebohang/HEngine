# 1.1.0 C# SDK 验收记录

日期：2026-10-14 至 2026-10-15，Asia/Hong_Kong。本轮范围为 C# 和 Windows、Android、Web、微信导出；Python 新增接口/导出已按用户要求撤出，现有 Python 功能保留。参考设计和边界见 [系统审计](ScriptingApiAudit-1.1.0.md)。

当前状态：C# 接口、离线文档、Windows API 游戏验收及四端共享项目导出均通过。以下记录候选发行内容的验证；公开发布/移动标签不作为已完成事项。ZIP 的大小、CRC 与 SHA-256 由发行目录中的归档清单单独记录，避免归档自引用。

## 路径

- `$Repo`：`C:/WorkSpace/HEngineDev`。
- `$Evidence`：`$Repo/out/agent/scripting-sdk110`。
- `$Release`：`C:/WorkSpace/HEngineReleases/1.1.0-scripting-20261014`（以任务开始日期命名）。
- `$SDK`：`$Release/HEngine-1.1.0-Windows-x64`。
- `$Shared`：`$Release/Validation/SharedArena`，项目 ID `game-8f54f06b8ec545f29f7b37a45e3113e2`。
- `$Probe`：`$Release/Validation/ScriptApi-r2/Project`。
- `$TestPlugins`：`$Evidence/deferred-python/TestPlugins.json`，内容为 `{"he.script.python":{"Tests":true},"he.script.csharp":{"Tests":true}}`；这是测试缓存配置，不是发行 Player 配置。

## 已完成的检查

| 证据 | 精确命令/范围 | 结果 |
| --- | --- | --- |
| unit-test | `dotnet run --project Tools/Scripting/WorkflowTests/WorkflowTests.csproj --configuration Release` | PASS；含 C# 示例编译、64 位音频句柄、owned 资源上下文与释放、线程局部回调上下文；`managed-final.log` |
| unit-test | `hectl.bat test AudioLifecycle -Project Engine/Templates/Blank -Configuration RelWithDebInfo -PluginConfig Tools/Distribution/Plugins.json -Parallel 2 -TimeoutSeconds 120` | PASS；音频参数拒绝、总线、暂停/恢复、游标/时长与原生 backend 转发。该次命令发生在修复 hectl 的 PluginConfig 转发前；测试不依赖脚本插件，不能作为插件启用证据 |
| unit-test | `hectl.bat test ScriptEcs -Project Engine/Templates/Blank -PluginConfig $TestPlugins -Configuration RelWithDebInfo -Parallel 2 -TimeoutSeconds 120` | PASS；动画状态/速度/暂停、序列暂停与排队 Seek、非法请求及场景所有权；`scene-service-test.log` |
| unit-test | `hectl.bat test PhysicsScriptContact -Project Engine/Templates/Blank -PluginConfig $TestPlugins -Configuration RelWithDebInfo -Parallel 2 -TimeoutSeconds 120` | PASS；真实 Box2D/Jolt 接触、速度读回、冲量改变速度、施力及静态/失效身体拒绝；`physics-test.log` |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name PhysicsScriptContact -Project Engine/Templates/Blank -Configuration RelWithDebInfo -Parallel 2 -TimeoutSeconds 120 -PluginConfig $TestPlugins` | PASS；新增原生实现收敛到 Platform 后复验；`physics-platform-test.log` |
| unit-test | `py -3.11 Tools/Scripting/TestReference.py`；`py -3.11 Tools/Distribution/test_distribution.py`；`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-Template.ps1` | PASS，参考生成 5 项、发行布局 16 项、模板版本/API 哈希拒绝；fixture 不等于设备验证 |
| static-check | `git diff --check`；变更 PowerShell 文件的 `System.Management.Automation.Language.Parser.ParseFile` | PASS |
| static-check | `cmake -DOUTPUT_DIR=Engine/Managed/HEngine.ScriptCore/obj/Api -P Tools/Scripting/GenerateApi.cmake`，比较消费端生成文件前后 SHA-256/mtime | PASS，changed=0、deleted=0；`codegen-idempotence.json` |
| static-check | `git hash-object --no-filters Tools/Scripting/Api.schema.json` 与 `git hash-object --path=Tools/Scripting/Api.schema.json Tools/Scripting/Api.schema.json` | PASS；精确字节契约不因 Git 换行过滤改变；`schema-checkout-contract.json` |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineEditor -Configuration RelWithDebInfo -Project Engine/Templates/Blank -PortableEditor -PluginConfig Tools/Distribution/Plugins.json -Parallel 2` | codegen PASS、build PASS；最终记录 `editor-r3.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-WindowsTemplate.ps1 -Output $Repo/out/agent/export-templates/Windows-x64-scripting110-r3 -PackageOutputRoot $Evidence/packages-r3` | PASS；当前 Shipping、cooker 自检、197 个本地 PE 依赖、发布策略和 WARP 启动；`windows-template-r3.log` |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-DevPlayer.ps1 -BuildVariant sdk-final -Output $Repo/out/agent/development-templates/Windows-x64-scripting110-r3` | PASS；`development-template-r3.log` |
| build-only / static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-AndroidTemplate.ps1 -Output $Repo/out/agent/export-templates/Android-arm64-scripting110-r3 -WindowsTemplate $Repo/out/agent/export-templates/Windows-x64-scripting110-r3 -HostParser $Repo/out/agent/projects/e1caca9aebe7fcb3/editor-portable-relwithdebinfo/bin/Programs/HEngineParser/HEngineParser.exe` | PASS；ARM64 原生模板及发布策略；`android-template-r3.log` |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-Editor.ps1 -SkipBuild -Output $SDK -WindowsTemplate $Repo/out/agent/export-templates/Windows-x64-scripting110-r3 -DevelopmentTemplate $Repo/out/agent/development-templates/Windows-x64-scripting110-r3 -AndroidTemplate $Repo/out/agent/export-templates/Android-arm64-scripting110-r3` | PASS；仅组装上述新产物，120 个必需文件、215 个 PE/401 条 imports；C# 参考 672 个成员/77 个页面；`sdk-assembly.log` |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-Distribution.ps1 -Engine $SDK -Json` | PASS；加入本验收记录后的最终布局为 121 个必需文件；`final-layout.json` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File $SDK/Tools/Distribution/Test-ScriptSdk.ps1 -Engine $SDK -Evidence $Release/Validation/ScriptApi-r2 -UseWarp` | PASS；无原生源码的开发会话和 Shipping 包实际执行实体/组件/层级、摄像机、设置、事件、UI、音频控制；运行状态注入 E 后只计数一次；自有会话 finally 停止 |
| runtime-manual | 顺序查看上述验收的 640×360 捕获（922,098 bytes） | PASS，三维物体和输入计数 1 已绘制；标签初版偏窄，生成夹具随后显式设置 260px 宽度。这不是全游戏视觉验收 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-WindowsExporter.ps1 -Project $Shared -ValidationRoot $SDK -SkipAssembly` | PASS；SharedArena 原生 Player 与模板 SHA-256 一致，编译/cook/依赖闭包/发布策略/WARP 启动通过；`shared-windows.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-AndroidExporter.ps1 -Project $Shared -Engine $SDK` | PASS；隐藏 NDK/VS 路径后使用模板生成实际 APK，签名、内容、原生库哈希和 16 KiB 对齐通过；`shared-android.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PortableExporter.ps1 -Project $Shared -Engine $SDK -Profile web` | PASS；实际 managed Wasm 发布，入口/运行库/内容清单齐全；`shared-web.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PortableExporter.ps1 -Project $Shared -Engine $SDK -Profile wechat` | PASS；实际 managed Wasm 发布及微信导出，game.js/game.json/项目配置/游戏清单齐全；`shared-wechat.log` |

API 探针的 `audio` 实际结果为 `playback-controls-passed`；音频使用静音 PCM 测试播放状态、暂停、位置、音高、时长及 seek，不是可听音质验收。Development Player SHA-256：`637aaa6c05702ddbbb35982aa49fefcae9ef13c9df4205ee85f7841c579b1d74`。

四端产物分别在 `$Shared/Dist/Windows`、`Dist/Android`、`Dist/Web`、`Dist/WeChat`。Android 原生库 SHA-256 为 `4acc5f91360e7bb135eea8e6fd2183c7801c87010bc40e5ae3344a0af61dcfb0`。APK 使用 Development 签名；不作为商店发布凭据。

结构化记录（相对于 SDK）：`out/agent/windows-validation/17c87ab9a889461cbc90efeaa9492cb9/result.json`、`out/agent/android-validation/2343d59155ef41559f16bb3d8512e86c/result.json`、`out/agent/portable-validation/3317f8dca5b04f72b731611619a427bd/result.json`、`out/agent/portable-validation/1f26482b28b94dc48cd71fd046ccf2d5/result.json`。这些 QA 缓存不进入 SDK ZIP。

## 环境与重试

- 构建缓存的既有 E 盘目录联接触发 storage 非阻断登记警告；未绕过策略或删除联接。
- Editor 首次缺少 OIDN 开发包。官方下载停滞后，核对本机已解压 SDK 的四个锁定运行库 SHA-256，再恢复依赖缓存；指定发行插件配置的依赖检查通过。
- .NET 运行库原始下载停滞。增加显式 `RuntimeDirectory` / 源码构建环境 `HE_SCRIPT_RUNTIME_ROOT`，复用既有 SDK 中通过 192 个文件哈希验证的 10.0.10 运行库。安装版 SDK 默认始终验证/使用自己携带的运行库，不继承该环境覆盖。
- Android 原工具链不在本机。复制系统 .NET SDK 的尝试继承了 MSI 安装标记，曾由安装器加入部分系统 Android 工作负载包；随后系统安装请求返回“操作已被用户取消”，该路径已停止，未擅自卸载系统包。复制件保留在 `E:/HEngineSDKTools/Android/dotnet-10.0.302-msi-copy`。
- 最终从 Microsoft 官方存储取得 297,545,270-byte ZIP，SHA-512 与 `toolchain.lock.json` 一致，以真正的便携 SDK 完成安装。`Setup.ps1` 新增 MSI 模式拒绝检查；最终 `android-setup-final.log` 为 PASS。JDK、NDK、SDK、工作负载位于 `E:/HEngineSDKTools/Android`；Android 命令设置 `HE_ANDROID_TOOLCHAIN_ROOT` 指向该目录。
- CMake 4.2、Node 22.14、Pillow 11.3 的验证缓存来自本机既有安装并经过依赖检查；不作为全新机器离线安装证明。第一次 SDK 依赖检查发现未部署 Pillow 验证缓存，补齐后通过。缓存位于 SDK 的 out 目录，归档时排除。
- 首次 API 输入探针在暂停时注入 Pressed，再等待单步，边沿已被呈现帧采样消费；改为恢复运行、注入、帧间查询、清除后暂停，保持“一次按下计数一次”断言，重试通过。首次无字体的 RML 夹具亦已补齐字体。

## 证据边界

尚未记录为 PASS 的项目不应推断成功。Android/微信手机、商店审核、用户生产签名和 macOS 发行验收不由 Windows 构建或本页自动证明。Web/微信的标准导出是共享 C# 规则与 PortableView 呈现，不代表原生 3D 场景 API 自动跨平台。
