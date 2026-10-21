# 构建与发布

在 Editor 顶部菜单栏点击 **发布…**，或选择 **构建 → 构建与发布…**；快捷键为 **Ctrl+P**。

选择左侧的平台配置，检查输出目录，然后点击 **检查环境**、**构建** 或 **构建并预览**。窗口可以关闭，构建仍在后台进行；关闭 Editor 会取消它启动的构建和预览服务。失败与取消保留上次成功输出。完整错误可通过 **定位完整日志** 查看。

配置保存在当前项目的 `Config/BuildProfiles.json`。配置可以复制、重命名；Web 可选择 Debug / Release、预览端口和项目适配文件。输出必须位于当前项目的 `Dist/` 子目录。构建器只替换属于同一项目、同一配置的输出目录；旧输出移至该次构建的 `previous-output`。

| 平台 | 当前支持 | 默认项目输出 |
| --- | --- | --- |
| Web | C# / .NET Wasm、WebGL 2、原场景 Mesh 与骨骼动画、音频、触摸摇杆、浏览器存档 | `Project/Dist/Web` |
| Windows | 发行版使用预编译 C# Player；源码保留 Release / Shipping PackageGame 验证链 | `Project/Dist/Windows` |
| 微信小游戏 | C# / .NET Wasm、微信 WebGL 2、分包资源、触摸摇杆、本地存档 | `Project/Dist/WeChat` |
| Android | 原生 Vulkan、C#；发行版复用 ARM64 模板组装 APK/AAB，源码模式保留 NDK 构建 | `Project/Dist/Android` |

默认项目 Windows 发布配置使用 `Game.CSharp.json` 和 `Plugins.CSharp.json`，以原生 C# 宿主运行与 Web、微信相同的游戏会话，Shipping 不加载 Python 插件。原始 Python 开发入口保留用于对照；运行 `Win-RunCSharpGame.bat` 可启动 C# 开发入口。Android 配置与工具链见 [Android.md](Android.md)；iOS 尚无构建后端。

新发行组装支持 Windows/Android Scene 项目免编译引擎导出，要求安装匹配的 Windows-x64/Android-arm64 模板；中文路径的源外 SDK 已实际导出 Windows 游戏和 Android APK。旧发行包不会自动获得这些功能，最终 Editor ZIP 仍需重新构建验收。四端独立 SDK 与 AI 开发的现状和验收边界见 [ReleaseSDK-1.1.0.md](ReleaseSDK-1.1.0.md)。

`hectl capabilities -Json` 查询当前四端能力边界，无需选择项目。Profile 可用 `requiredFeatures` 声明依赖，例如 `game.shared-session`；预检拒绝未知或不支持的能力。能力声明不代替实际导出和设备验收。

## 无需 AI 的使用方法

本机开发环境准备好后，日常打包只需上述 Editor 按钮。Web 的 **构建并预览** 会启动本地 HTTP 服务并打开浏览器，**停止预览** 会关闭本次服务。也可以运行仓库根目录 `Win-RunWebGame.bat`，它调用相同构建器。

Web 输出整个目录上传到支持静态文件的服务器即可发布；服务器需将 `.wasm` 作为 `application/wasm` 提供。禁止用 `file://` 打开 .NET Web 游戏。构建按钮不会自动上传到云、商店或微信后台。

所需工具：

- Web：`Engine/Managed/global.json` 指定的 .NET SDK、`wasm-tools` workload、Python 3.11+。在 `Engine/Managed` 目录执行 `dotnet workload install wasm-tools` 安装工作负载。
- 本地预览：Node.js；可用 `HE_SCRIPT_NODE` 指定路径。
- Python 可用 `HE_BUILD_PYTHON` 指定路径，默认检测系统 Python / Python Launcher，再回退本机已有工具路径。
- Windows 二进制 SDK：匹配的导出模板、锁定 .NET SDK、CMake、Python；用 `hectl deps check|install -Profile SDK -Project <项目>` 检查或安装工具，不要求 VS。项目须声明 `RuntimeMode=Scene`，标准模板支持 C#；自定义原生代码/插件需要相应的自定义模板或源码流程。
- Windows 源码流程：仓库要求的 Visual Studio C++ 工具链与 CMake，由原有打包工作流检查。
- Android：当前还需要源码、锁定的 Android NDK、JDK、Android SDK 和 .NET Android workload；详见 Android 文档。
- 微信：上述 Wasm 工具、Node.js、`Tools/Scripting` 的 npm 依赖；便携资源处理需要 Python 3.11/3.12 和 Pillow。开发者工具 CLI 路径在发布面板填写，或设置 `HE_WECHAT_CLI`；**构建并预览** 会打开导出项目，之后在开发者工具里编译、预览或上传。没有配置 CLI 时仍可构建并手动导入输出目录。

微信手机兼容性由引擎公共构建层负责：`WebAssembly.props` 使用 JavaScript 异常处理 ABI（`WasmEnableExceptionHandling=false`），保留 C# 的 try/catch/finally 语义；不要求手机具备原生 Wasm EH。公共发布入口对所有微信项目强制关闭原生 EH、SIMD 和线程并重新链接运行时，导出前检查实际 native loader，而不是仅相信项目配置。两个托管微信导出入口共用 `ManagedWeChatCompatibility.cjs`，不兼容或无法识别的 SDK 产物会使构建失败并保留旧包。导出包包含 `runtime-compatibility.json`。这是目标平台兼容性要求，切换画质档位不会解除它。

更新包后需要在微信开发者工具重新生成预览二维码；以前的二维码不会自动变成新包。开发者工具的 Wasm 能力可能高于手机，模拟器通过不等于真机验收。引擎升级 .NET SDK 时运行 [微信兼容性回归](WeChatManagedCompatibility.md)，并保留设备抽样验证。

## 平台与画质分档

项目导入 `Engine/Managed/Platform.props`（Wasm 项目由 `WebAssembly.props` 自动导入），得到 `HE_WINDOWS`、`HE_WEB`、`HE_WECHAT` 和 `HE_QUALITY_DESKTOP` / `HE_QUALITY_PORTABLE` 编译符号。`HEngine.Platform.BuildFeatures` 提供运行时查询。导出器显式传入目标，不根据构建机器的操作系统推断。

画质档位仅应用于表现资源和可选视觉数量，不改变伤害、奖励、随机规则或存档格式。微信便携导出把纹理限制为 512、音频转换为单声道 PCM，并检查分包总量。选择桌面档可能超出包体预算，构建器会保留原有成功输出并报告失败。

Windows 配置可指定项目内 `gameConfig` 和 `pluginConfig`。项目在 `Config/ManagedInputs.json` 的 `inputs` 数组声明 C# 编译前数据生成依赖（项目相对路径，可含 glob），以便数据变化触发重新构建。

## 引擎与项目边界

- `Tools/Build/Publish.ps1`：通用构建器，读取当前项目配置、检查工具、执行平台后端、分阶段构建、保留旧输出、写入构建回执。Windows 在发行版调用 `Tools/Distribution/Export-Windows.ps1`；源码委托 `Tools/Agent/Package.ps1`。二者都执行资源、Shipping 依赖/策略和完整启动门禁。
- `Tools/Build/Preview.ps1`、`ServeWeb.cjs`：通用本地 HTTP 预览，端口冲突直接报错，不关闭其他进程。
- `Engine/Managed/WebAssembly.props`：共用 C# Wasm 构建设置；无 Project 依赖。
- `Engine/Source/Runtime/Platform/Web/SceneRenderer.js`：WebGL 平台实现，场景、模型列表和相机由调用方传入。
- `Engine/Source/Runtime/Platform/Web/AudioBank.js`：通用音频银行。
- `Engine/Source/Runtime/Platform/Web/Input/VirtualJoystick.js`：通用二维指针摇杆，含死区、径向归一化、多指隔离、取消/失焦复位和销毁。项目通过 `onChange(x, y)` 消费输入；宿主元素应使用 `touch-action: none`。
- `AnalogStick.js` 和 `PointerDrag.js` 是浏览器与微信共用的摇杆、视角拖动输入核心；两个宿主仅翻译各自的指针事件。`Engine/Managed/Shared/OrbitCamera.cs` 提供共用环绕视角和相机相对移动。
- `Engine/Source/Runtime/Platform/WeChat` 提供微信包资源和音频访问；`Tools/Build/ExportManagedWeChat.cjs` 导出真实 .NET Wasm 运行时。
- `Project/Build/Web`：项目 C# 浏览器入口；`Project/Scripts/CSharp/Survivor`：游戏规则。
- `Project/Web`：游戏 UI 与场景表现映射；`Project/Tools/ExportWeb.py`：项目自己的内容目录和资源依赖导出。

`Engine/Managed/WebAssembly/GameSessionBridge.cs` 封装 C# Wasm 导出的会话调用，项目的 `Program.cs` 只保留 `JSExport` 方法和会话工厂。`Engine/Source/Runtime/Platform/Web/ManagedGameSession.js` 供浏览器与微信共同处理快照、命令、帧推进和存档版本；宿主提供自己的存储 API。`Tools/Build/PortableContent.py` 负责便携版资源依赖遍历、文件哈希与许可证复制，项目导出脚本仍生成本游戏的 catalog 和内容选择。

新项目提供自己的 `.csproj`、带 `--output` 参数的 Python 内容导出器，以及 `index.html` / `main.js`。构建器传入 `HEngineRoot` MSBuild 属性；项目可导入 `$(HEngineRoot)/Engine/Managed/WebAssembly.props`。引擎共用浏览器模块复制到发布目录 `engine/`。项目文件路径均相对于项目根目录；本轮已验证与引擎同分区、带空格的外部项目路径，跨分区发布未验收。游戏所需的场景清单由项目导出器负责。

微信项目入口使用 `@hengine/web/...` 和 `@hengine/wechat/...` 引用引擎模块，由 `EngineImports.cjs` 在 bundle 时解析。项目导出脚本通过 `HE_ENGINE_ROOT` 调用所选引擎工具，不依赖项目旁边存在 Engine/Tools 目录。浏览器发布后的 `./engine/...` 引用仍保留。

构建脚本与 Editor 使用同一入口：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile web
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile windows
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Preview.ps1 -Project Project -Profile web -OpenBrowser
```

回执位于 `<Project>/Saved/Build/<Profile>.json`；工作目录和日志位于 `out/agent/build-publish/<ProjectHash>/<Profile>`。每次发布的阶段产物位于其 `content/` 历史目录，并注册到仓库统一保留策略；构建缓存独立保留。

## 二进制 SDK 创建四端游戏

项目管理器的“2D 守护游戏 · 四端共享规则”创建 `SharedArena` 模板。
也可以从 SDK 目录执行：

```powershell
.\hectl.bat project create -Project D:/Games/MyGame -Name 我的游戏 -Template SharedArena
.\hectl.bat deps install -Profile SDK -Project D:/Games/MyGame
.\hectl.bat publish check -Project D:/Games/MyGame -BuildProfile web
.\hectl.bat publish build -Project D:/Games/MyGame -BuildProfile web
```

平台配置为 `windows`、`android`、`web`、`wechat`。Windows/Android 使用预编译
Player，项目 C# 仍需编译；Web/微信使用锁定的 Wasm workload 编译同一份规则。
`deps install -Profile SDK` 包含 Python、Pillow、Node、esbuild、.NET 和 Wasm 工具；
Android 的 JDK、Android SDK 和 workload 仍通过 `hectl android setup` 安装。
Android 开发签名和微信游客 AppID 只供开发验收，上架需项目自己的发布配置。

本模板支持矩形 Sprite、文字/按钮、音效及共享游戏状态，适合 AI 创建可运行的
二维玩法原型。修改项目的 `Scripts/CSharp/Rules/GameSession.cs` 与配置即可改变玩法，
不需要修改引擎源码。它不意味着原生场景的所有渲染、物理和特效都能自动迁移到四端。

## FSR 与离线降噪的发行依赖

Windows 构建的 `HEngine.Runtime.json` 记录编译时的 FSR、图形后端和 OIDN 支持。
启用 FSR 时，编辑器、Development Player 和 Shipping 游戏都必须带对应的官方
`amd_fidelityfx_dx12.dll` / `amd_fidelityfx_vk.dll` 及 `Licenses/FidelityFX.txt`。
这些库通过动态加载使用，不能只检查 EXE 的静态导入表。发行校验会检查锁定的
SHA-256、许可证和递归 PE 依赖；缺失或被替换时拒绝组装/导出。
FSR/OIDN 所需的 `msvcp140.dll`、`vcruntime140.dll`、`vcruntime140_1.dll`
也随应用部署，来源是所选 VS 的正式 x64 redistributable 目录，不从 System32 复制。

OIDN 属于 `he.offline-reference-renderer` 的 CPU 降噪后端。源码构建按插件配置
和 `HE_OFFLINE_OIDN` 决定是否编译；二进制 Editor 将该插件预编译后，在插件面板
提供“离线渲染 / OIDN CPU 降噪”开关，重启当前项目编辑器后生效。本机覆盖写入
项目 `Saved/Config/EditorPlugins.json`，未覆盖时读取项目插件配置。

OIDN 三个 CPU 运行库放在 `Plugins/OfflineReferenceRenderer`，仅在实际请求降噪
时从该目录加载，不搜索工作目录或 PATH。未交付 CUDA/HIP/SYCL 后端。Embree
和 OIDN 共用根目录的一份锁定 oneTBB；对应许可位于
`licenses/OfflineReferenceRenderer`。关闭插件不会删除安装文件，但不会注册
离线渲染器；未请求降噪不会加载 OIDN。Shipping 明确排除该插件和 OIDN。

发行版只需要这些运行库，不需要向用户交付完整 FSR/OIDN 开发 SDK。可从源码或
安装目录执行 `Tools/Dependencies/Test-OidnRuntime.py --engine <安装目录>`，验证
CPU 降噪和 Embree/oneTBB 共存；该命令需要 Python，不替代编辑器/设备运行验证。
`Tools/Distribution/Test-EditorDependencies.ps1 -Engine <安装目录>` 检查 Editor 的
原生和托管运行时依赖。损坏的预编译运行库应通过同版本发行包修复，不能随机替换 DLL。

## 界面参考

采用项目内平台预设、当前配置设置和构建操作的组织方式，参考 [Godot 导出预设](https://docs.godotengine.org/en/stable/tutorials/export/exporting_projects.html)、[Unity Build Profiles](https://docs.unity3d.com/cn/6000.0/Manual/build-profiles-reference.html) 和 [Unreal 打包流程](https://dev.epicgames.com/documentation/unreal-engine/packaging-your-project)。界面只展示本引擎实际支持的选项。
