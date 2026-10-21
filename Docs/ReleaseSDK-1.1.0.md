# 1.1.0 独立引擎、四端导出与 AI 开发方案

范围：Windows、Web、微信小游戏、Android。本文记录设计、实现和分项验收；不代表所有原生渲染能力已经四端一致。AI 首版使用外部 AI 调用引擎工具，编辑器内对话入口仍待产品讨论。

## 当前实际能力

| 能力 | 代码现状 | 距离目标的缺口 |
| --- | --- | --- |
| 引擎不带游戏工程 | `Build-Editor.ps1` 从 Blank 构建通用 Editor；入口是 ProjectManager.exe | 最终 ZIP 独立解压到中文和空格路径，真实入口、打开项目与再次导出 PASS |
| 选择独立项目 | Project.heproject、项目管理器、portable Editor 的 `--project` | 已有；不要把机器路径写进共享 descriptor |
| Windows Shipping | 发行版 Publish → 预编译模板 → C# 编译/HCooker/包验证；源码保留 Package.ps1 | 最终 SDK 的外部星核护航项目导出、模板 EXE hash、Shipping 策略与完整启动 PASS；未在全新无 VS 机器复验 |
| Web | 项目 C# Wasm 工程、通用 Web 渲染/输入模块和内容导出器 | 源外中文 SDK 已导出小树并通过 WebGL2 实际交互；需要 .NET SDK/wasm-tools、内容工具及项目适配 |
| 微信 | 共用 C# Wasm 规则、WX 宿主与资源分包；锁定工具依赖 | 最终 SDK 已导出星核护航并通过 Wasm/JS 构建兼容检查；手机验收未完成 |
| Android | `Build-AndroidTemplate.ps1` 生成 ARM64 通用模板；发行版 Build 跳过原生构建，复用 `Export-Native.ps1` cook | 最终 SDK 的实际 APK 导出、原生库 hash、签名及 16 KiB 对齐 PASS；开发签名，真机未验证 |
| AI 编辑项目 | Development 模板、项目脚本准备、发行版 Agent 会话路由 | 最终 SDK 的创建/打开、原生开始/移动/暂停/恢复及截图 PASS；小树保卫者也由发行版 Editor 打开并截图 |
| 一句话生成四端游戏 | SharedArena 共用 C# 规则、PortableView 及四端 profile；AI 星核护航独立项目 | 已完成有范围的创建→修改玩法→规则测试→运行→四端导出案例；不承诺任意游戏一次生成成功 |

`Docs/Release-1.1.0.md` 的历史验收针对当时具体产物，不覆盖后续源码或本文目标。旧文档曾写 Android 没有后端，已与现有代码不符。不能从编辑器显示四个平台，推导出四端模板、组件及最终运行全部可用。

## 发布产品与源码边界

建议提供三个独立下载项：

1. **HEngine Editor**：项目管理器、通用编辑器、运行时、内建内容、文档、脚本 API/类型信息、项目模板、开发用 AI 控制接口。包根不含 Project、工作区默认项目或个人配置。
2. **Export Templates / SDK**：按引擎版本和平台匹配的预编译 Player、cooker、shader/compiler 工具、平台适配和锁定依赖。可提供完整离线包，也可按需安装；安装清单与 SHA-256 固定，不能下载 latest 后混用。
3. **Source**：供修改引擎、调试底层和编译自定义 C++ 插件。小树保卫者是独立示例项目，可另行下载。源码可选，不能成为普通脚本项目导出的隐式依赖。

源码有助于 AI 理解底层和修复引擎，但让 AI 每次阅读整个 C++ 仓库成本高，也容易把游戏逻辑写进引擎。正常 AI 开发优先使用公开 API、带类型的场景/资源协议、教程和完整小模板；遇到引擎缺陷时再进入源码工作流。是否公开源码、采用什么许可证，是另一项发布决策；本轮不变更 LICENSE。

“无需编译引擎”允许编译游戏 C#、生成 Shader、cook 资源、重新组装和签名包。“无需任何工具链”是更高目标，不应混为一谈。工具可随 SDK 提供或由工具管理器检查/安装；签名密钥和应用账号属于项目用户。

## 通用 Player 与导出流程

Windows 通用模板使用独立构建模式，不能拿小树保卫者的现成 EXE 改名后当作任意项目模板：

- 通用 Player 使用引擎 `RuntimeGameLayer`；不编译用户 `Source/Game`。
- 项目 ID、名称、GameConfig、存档隔离和兼容版本从经过校验的包元数据读取。`HE_PORTABLE_PLAYER` 已实现通用模板路径，源码项目继续使用构建期身份作为默认值。
- 启动前验证引擎版本、脚本 API/原生 ABI 指纹、插件集合、渲染后端、目标架构与内容格式。错误必须定位到字段，不能静默换模板或回退源码构建。
- 预编译开发 Player 保留受控 AI 接口；Shipping Player 必须是 PACK_GAME=ON、HE_BUILD_DEV_GAME=OFF、GM=OFF。不能为免编译而把开发 EXE 当发行游戏。
- 对需要额外 C++ 类型/插件的项目，在导出前明确拒绝或要求匹配的自定义模板；不能忽略 Source/Game 后输出行为不完整的游戏。

导出使用同一条公共入口，供 Editor 和 CLI/AI 调用：检查能力/工具 → 编译游戏脚本 → 编译项目 Shader → 准备内容 → cook → 组装预编译宿主 → 包依赖/Shipping 检查 → 原子替换输出 → 写结构化回执。现有 `Publish.ps1` 的锁、旧输出保留和回执机制继续复用；cooker 和现有资源闭包检查不绕过。

| 平台 | 预编译交付 | 用户端仍执行 | 额外验收 |
| --- | --- | --- | --- |
| Windows x64 | Shipping Player、HCooker、HShaderCompiler、运行时及图形依赖 | C# 编译、Shader、cook、组装 | 无 VS/无引擎源码的机器启动导出包；无 Dev Agent |
| Web | 通用 Web 宿主、受支持渲染/输入/API、Wasm 构建依赖 | 游戏 C# → Wasm、内容导出、生成站点 | HTTP 服务运行、输入/音频/存档/重载 |
| 微信 | 微信宿主、加载器适配、分包工具、兼容 Wasm 工具版本 | C# Wasm、便携资源处理、分包 | 开发者工具及手机；实际 Wasm EH/SIMD/线程限制 |
| Android arm64 | 通用 libhegame.so、Android 宿主工程/模板、cooker | 游戏 C#、资源、包名/图标、APK/AAB 组装与签名 | ELF/APK 对齐、Manifest、签名、真机输入/生命周期 |

Android 目标是日常脚本游戏无需 NDK 编译引擎；APK/AAB 组装仍可能需要 JDK、Android SDK、.NET Android workload。直接分发一个固定 APK 不能满足不同项目的包名、图标、资源和签名。Web/微信当前规则与表现通过项目桥接，并非桌面 C++ 渲染器的完整移植；须如实定义支持面。

## 四端共同开发约定

建议 1.1.0 的四端主路径使用 C#。Python 保留为已支持宿主的选择，不能承诺现有 Python 游戏自动导出为四端。自定义 C++ 走源码/自定义模板路径。

新增 SharedArena 四端模板，包含输入动作、镜头、角色移动、规则层命中判定、UI、音频、存档、开始/暂停/胜负/重开，以及四份 Build Profile。游戏规则只维护一份；平台适配属于引擎，项目声明自身内容和配置。Scripted3D 仍是 Windows 模板。SharedArena 当前使用矩形精灵视图协议，不能据此宣称任意原生场景、物理或特效都可在 Web/微信运行。

四端共用机器可读的组件/API 能力表，同时供新建模板、AI 查询、Editor 和 Preflight 使用。定义“支持”“有显式替代实现”“不支持”，并核查实际资源引用与脚本调用。完整原生物理、导航、VFX、自定义 Shader 等逐项核对；不得以 portable fallback 声明本身代替实现和测试。

## AI 开发体验

第一阶段优先让 Codex 等外部 AI 调用稳定 CLI/本地协议。不要先把 API key 或某个模型绑定进引擎。编辑器内的对话窗口以后复用同一操作层；它是交互入口，不是生成能力本身。

需要公开并可发现的操作：

- 引擎/项目/平台能力与工具诊断、API 搜索、模板列表。
- 新建项目、添加或修改 C# 脚本、资源导入与引用安全更新。
- 场景查询、校验、差异预览、事务应用、保存和回滚。
- 脚本编译的结构化诊断；运行、输入注入、日志、截图、有界帧推进。
- 按 profile 检查和导出；可取消的进度、失败阶段和证据路径。

已有 hectl 和 Agent 协议尽量复用，但发行版要能启动预编译开发宿主，不能调用源码构建。宿主应使用独立、有限生命周期的 session；可写范围归项目，日志不包含凭证；AI 操作不打断用户手动开的编辑器。Shipping 包不携带控制通道。

“一句话做游戏”的可验收范围应是：在明确支持的模板和资源范围内生成可玩原型。例：“做一个俯视角守护小树的生存游戏，有移动、敌人、波次、升级和失败重开，导出四端。”流程是需求结构化 → 选择模板与平台约束 → 生成规则和内容 → 编译/检查 → 运行输入与截图 → 根据错误修复 → 导出 → 交付四份产物和验证结果。缺少资源时使用明确标识的占位资源，不能把占位和生成文件数量当完成证据。

不承诺任意一句话生成任意规模商业成品，也不能把四个平台构建成功等同于画面、输入和规则一致。首个验收应使用一个新的独立项目，防止流程只是复制小树保卫者。

## 实施顺序与完成门槛

1. **发行与项目搬移基础**：完整工具布局、稳定的项目到引擎引用、ZIP 内容检查。在无 sibling Engine/Tools、中文和空格路径下验证。最终发行包与独立解压验收已完成，结果见文末。
2. **Windows 通用导出模板**：迁移包身份、构建通用 Shipping/Dev Player、模板清单和工具、二进制导出后端；分别验证 Blank、新脚本游戏和小树保卫者。必须保留并实际执行原有 Shipping 门禁。
3. **Android 通用模板**：将 libhegame.so 与游戏身份/C++ Layer 解耦，跳过用户端原生构建；验证包名/图标/存档隔离、签名和真机生命周期。
4. **四端模板与公共能力表**：Web/微信脱离项目专用入口和源码目录，完成工具环境管理；同一个新项目实际导出四端。
5. **发行版 AI 闭环**：预编译开发宿主、稳定工具入口、API/示例文档、受控运行与修复；外部 AI 只拿发行包和项目完成一句话验收。
6. **最终发行门禁**：在没有 VS/引擎源码的干净环境解压最终 ZIP，从 ProjectManager 新建/导入，编辑并导出四端；移动整个安装和项目后复验。生成哈希清单，再决定是否正式发布。商店上传/远程 Release 发布是独立动作。

Windows 与 Android 预编译模板、四端导出及本机独立解压验证已完成。第 6 步的全新无 VS 机器与移动端真机验收仍是正式发布前的剩余检查；旧 release 不因本轮工具修改自动更新。

## 本轮基础修复与证据

- 发行组装补齐 `Tools/Dependencies`、`Tools/Content`、微信平台模块和根 hectl 入口；保留 npm 工具自己的 bin 目录。
- `Test-Distribution.ps1` 检查关键依赖、manifest 和不携带项目的边界，接入组装完成前；只声明 static-check。
- 小树保卫者微信导出器按 `HE_ENGINE_ROOT` 找引擎；游戏/HUD 使用 `@hengine/web`、`@hengine/wechat`，由引擎构建器统一解析。
- Publish 使用统一依赖环境；发行版 Android 路径明确报告尚无通用原生模板，避免进入缺文件/源码构建流程。
- ZIP 不再全局排除 bin，避免删除 esbuild 或未来 SDK 的实际执行文件。
- 发行包根 AGENTS.md 和新项目指引明确区分二进制/源码能力，提供脚本与 API 入口，避免 AI 在二进制安装中误用源码构建命令。

| 证据 | 精确命令 | 结果 |
| --- | --- | --- |
| unit-test | `node Tools/Build/TestEngineImports.cjs` | PASS；项目入口搬到引擎外且带空格的临时目录后完成真实 JS bundle，导出脚本按选定引擎解析；启动器转发用 fixture，不是完整 Wasm 导出 |
| unit-test | `node Tools/Build/TestWeChatHud.cjs` | PASS；HUD bundle 和布局断言 |
| unit-test | `py -3.11 Tools/Build/test_preflight.py` | PASS；8 个资源/平台诊断用例 |
| unit-test | `py -3.11 Tools/Distribution/test_distribution.py` | PASS；6 个布局/坏 manifest/缺依赖/ZIP 用例；模拟文件不是可运行引擎 |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-Distribution.ps1 -Engine dist/Release-1.1.0/HEngine-1.1.0-Windows-x64 -Json` | FAIL；旧包缺少当前布局所需文件，其中包括本轮新增校验工具和 AI 指引；不能作为当前源码的完整 SDK 发布 |

上述表格仅记录最初基础修复时的状态；完整发行 ZIP、四端最终游戏导出及解压搬迁的完成证据见文末。真机与全新机器检查仍未完成，不能据此宣称第 2–6 步所有发布门禁均已通过。

## 通用 Windows Player 实现与验收进度

已增加 `HE_PORTABLE_PLAYER`、动态项目身份与运行时指纹检查、`Build-WindowsTemplate.ps1`、不可变模板文件哈希清单、`Export-Windows.ps1` 和发行版 Publish 分支。`RuntimeMode=Scene` 项目使用引擎通用 GameLayer，C# 是标准模板的脚本后端；预检拒绝项目 NativeScriptComponent 和不匹配插件。导出保留原有资源 cook、Shipping 依赖/策略门禁和 WARP 完整启动检查。

`Build-Editor.ps1` 现要求 Windows 导出模板；SDK 依赖模式不要求 VS/libclang，并支持锁定 CMake。发行版复用包内已核验的 .NET Runtime。旧发行包未被替换；仅改工具不会更新旧二进制。

| 证据 | 精确命令 | 结果 |
| --- | --- | --- |
| unit-test + build-only | `hectl test ProjectIdentity -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -Configuration RelWithDebInfo -Parallel 2 -TimeoutSeconds 120` | PASS；项目身份、版本、指纹和路径负例；使用该项目自己的缓存 |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PlayerManifest.ps1` | PASS；写入、无效配置/开发模板拒绝和失败保留 |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-Template.ps1` | PASS；模板平台、Shipping 标记、文件哈希和路径负例 |
| build-only + test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-WindowsTemplate.ps1` | PASS；首次失败在 manifest 原子替换，重试修复后通过；日志 `out/agent/sdk-windows-template-retry.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Export-Windows.ps1 -Project "C:/WorkSpace/HEngineProjects/SDK 验收 Scripted Game" -Template out/agent/export-templates/Windows-x64 -OutputDirectory out/agent/sdk-scripted-export -WorkDirectory out/agent/sdk-scripted-work` | PASS；独立 C# 项目复用 Player，cook、依赖/策略和完整启动均通过；不是交互玩法验收 |
| static-check | `hectl deps install -Profile SDK -Project Engine/Templates/Blank -Json` | PASS；仅脚本/导出依赖，证据 `out/agent/sdk-dependencies.json` |

`Test-WindowsExporter.ps1` 将实际导出工具、模板、内容和脚本 SDK 搬到源码外的中文路径；目录不含引擎 C++ 源码或根 CMakeLists.txt，保留所需 Web/微信 JavaScript 模块，并从 PATH 去掉 VS 后调用 Publish，核对最终 EXE 与模板 SHA-256 一致。这是导出集成验收工具，不是完整 Editor 发行包，也不能代替真实干净机器和 GPU 验收。

搬移验收发现并修复了冗余临时目录 UUID 导致的长路径，以及 HCooker 向 UTF-8 ZIP API 传本地编码路径的问题。库代码未修改。HCooker 自测现在使用中文临时目录。Windows PowerShell 5 的 `Get-ChildItem -LiteralPath -Include` 不能用于该测试的原生源码排除断言，已改为显式文件名过滤。

| 证据 | 精确命令 | 结果 |
| --- | --- | --- |
| build-only | `"C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build out/pkg/a1191cf516ce --config Release --target HCooker --parallel 2` | PASS；仅重建受影响 cooker，日志 `out/agent/sdk-cooker-unicode-build.log` |
| unit-test | `out/pkg/a1191cf516ce/bin/Programs/HCooker/Release/HCooker.exe --self-test` | PASS；中文路径中的实际 cook、写 pak 与重读验证 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-WindowsTemplate.ps1 -SkipBuild -Output out/agent/export-templates/Windows-x64-r2` | PASS；更新 cooker 后重新组装，重新执行包策略和完整启动检查 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-WindowsExporter.ps1 -Project "C:/WorkSpace/HEngineProjects/SDK 验收 Scripted Game" -Template out/agent/export-templates/Windows-x64-r2 -ValidationRoot "C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2" -SkipAssembly` | PASS；前一步已组装 fixture，修复断言后复用；`result.json` 记录原生 Player SHA-256 一致，Publish/Shipping/完整启动通过 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File "C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2/Tools/Distribution/Export-Windows.ps1" -Project C:/WorkSpace/HEngineDev/Project -OutputDirectory "C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2/out/agent/small-tree-package" -WorkDirectory "C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2/out/agent/small-tree-work"` | PASS；从 PATH 移除 VS，小树保卫者 C#、项目 Shader、2,463 个资源、Shipping/完整启动通过；日志 `out/agent/sdk-small-tree-export-r2.log` |

当前已验证的 Windows 模板位于 `out/agent/export-templates/Windows-x64-r2`。首次 `Windows-x64` 模板保留了旧 cooker，不能用它替代上述中文路径验收过的模板。组装当前 Editor 时显式传入 `-WindowsTemplate out/agent/export-templates/Windows-x64-r2`，或执行默认新构建生成另一份模板。

两项目的原生 EXE 均与同一个模板相同，SHA-256 为 `284e7f4a656535ce3bf2962f3193cc50ae3b48ad2b2fd54e226fc6bcfb0ce5f2`；游戏脚本、资源、名称、GameConfig 和存档身份由项目提供。WARP 自动启动验证不等于交互玩法、原生 GPU 或四端一致性验收。剩余核心任务仍为 Windows 开发 Player/AI 会话、Android 通用模板、四端公共模板/能力表、工具环境闭环及最终 Editor/SDK ZIP 的完整验收。

## Android 与开发宿主接入进度（2026-10-01）

Windows 原导出逻辑已提取为 `Export-Native.ps1`，Windows 包装入口不变。
独立中文路径 fixture 上重新执行 `Test-WindowsExporter.ps1` 后，原 Shipping 门禁及
WARP 完整启动仍为 PASS（`out/agent/sdk-native-export-regression.log`）。

Android 发行版分支复用通用模板、脚本编译和 cook，再由 .NET Android 宿主组装、
签名和校验 APK/AAB；不导入 VS，也不要求用户安装 NDK。NDK 只供引擎模板生产者使用。
包验证新增实际 `libhegame.so` SHA-256 与内容运行时指纹检查；12 个 `test_package.py`
用例通过。随后实际 ARM64 模板完成编译和组装，中文路径的独立 Scripted3D 项目完成
无源码 APK 导出；单元测试与实际导出证据分别记录如下。

开发模板和 Shipping 模板由同一个清单校验器明确区分：前者必须 `PackGame=false`、
`DevAgent=true`，后者必须相反，不能互用。`Prepare-DevPlayer.ps1` 将开发宿主复制到
独立会话，按 Windows profile 编译项目 C#，使用 `--project` 和受路径校验约束的
`--game-config` 启动。脚本和运行结果归当前会话，不改用户手动运行的进程。
`Test-DevPlayer.ps1` 已实际发现 API、读取实体、注入输入、推进帧和截图，且正常停止会话。
原生 EXE SHA-256 保持 `05898210e20bd8bdfb3fbb8de3af9d44e837c1c735a2dc4c9f7bc62f83c88cdf`。
Scripted3D 实体在 30 帧后从 `[0,0.5,0]` 移动到 `[0,0.5,-1.5000024]`；截图人工检查
确认显示方块和地面。这是已有独立模板的脚本运行验证，尚不是完整四端游戏验收。

精确命令（test-layer，PASS）：
`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-DevPlayer.ps1 -Engine "C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2" -Project "C:/WorkSpace/HEngineProjects/SDK 验收 Scripted Game" -MovementEntity 3a6c5e51-8da7-4c11-9099-a0987f5ec261 -UseWarp`。
日志 `out/agent/sdk-dev-session-test-r6.log`；结构化证据在 fixture 内
`out/agent/projects/312c017d16ea9917/sessions/dev-49853730/result.json`。
验收中修复了 PowerShell 5 对深层资源复制及 Agent 原子 JSON 请求的长路径限制。

工具链安装已在 `E:/HEngineSDKTools/Android` 完成：`Setup.ps1 -AcceptLicenses` PASS，
日志 `out/agent/sdk-android-setup-verified.log`。安装流程增加断点续传、传输超时，且将
工作负载解压临时文件与 NuGet 缓存放到选定工具链盘；二进制安装默认不下载 NDK。
锁定版 SDK 管理器通过 `InstallSdk.py` 的明确字节输入处理许可应答，避免当前
PowerShell 子进程交互失败。`adb devices -l` 未发现连接设备，真机验证尚未执行。

开发宿主首次构建发现 ConfigManager 头文件只在 Editor 路径引入的问题，已修复。
随后增量构建因 Android 工作负载在系统 TEMP 解压而耗尽 C 盘失败，不能记作 PASS；
环境修复后开发宿主构建及组装 PASS（`out/agent/sdk-dev-player-build-final.log`）。Android
模板实际构建发现缺少 HEArchive 依赖和 ARM64 上 uint64_t/std::clamp 类型推导问题，已修复，
实际编译和模板组装已通过。当前 Editor 发行组装要求 Windows、Android 和 Development
三类真实模板，旧 release 未更新。

Android 模板位于 `out/agent/export-templates/Android-arm64`。`sdk-android-template-build-r3.log`
中的原生编译/链接成功，但随后清单检查因 CRLF 正则错误失败；修复后用以下命令完成
组装，不能将之前整个失败命令记为 PASS：

`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-AndroidTemplate.ps1 -SkipBuild -WindowsTemplate out/agent/export-templates/Windows-x64-r2 -HostParser out/agent/host-tools/HEngineParser/HEngineParser.exe`

结果为 test-layer PASS（真实 ARM64 ELF、16 KB 段对齐、Shipping 依赖策略、模板文件清单），
日志 `out/agent/sdk-android-template-stage-r2.log`。

源外、无引擎 C++/CMakeLists/Project 的中文 SDK fixture 导出命令：

`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-AndroidExporter.ps1 -Engine "C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2" -Project "C:/WorkSpace/HEngineProjects/SDK 验收 Scripted Game"`

设置 `HE_ANDROID_TOOLCHAIN_ROOT=E:/HEngineSDKTools/Android`；test-layer PASS，日志
`out/agent/sdk-android-export-test-r3.log`，fixture 证据
`out/agent/android-validation/dce15184b2ec417a891f6890c2c82a25/result.json`。
测试移除 VS 路径，并将 NDK 指向不存在的目录；实际 APK 内的原生库哈希与模板一致。
脚本编译、cook、包指纹、签名、Manifest、12 个 native library 的 16 KB ELF 对齐和
资源哈希通过。使用 Development 签名，不是商店提交包；未连接真机，未验证设备玩法。

小树保卫者复用同一 SDK 和同一原生模板，也通过实际导出（2,463 个 cook 文件）：
`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-AndroidExporter.ps1 -Engine "C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2" -Project C:/WorkSpace/HEngineDev/Project`。
test-layer PASS，日志 `out/agent/sdk-android-smalltree-export-r2.log`；fixture 证据
`out/agent/android-validation/7526489c329e44c08fc3dd0b3b28f9d7/result.json`。
产物 `Project/Dist/Android/com.hengine.starbloomsurvivor-Signed.apk`。
两个 APK 的原生库 SHA-256 均为
`9f7f2c76465b64d1c90077f21deac6b4de3bea1be1c1da8bfb186386bd9cde2f`。
首次小树导出发现 PowerShell 5 读取中文包元数据缺少显式 UTF-8，修复后重跑全流程通过。

首次导出暴露 Windows AAPT2 不接受中文暂存路径的问题。`PackageWorkspace.ps1`
现在选择短 ASCII 缓存并复制完整宿主输入，验证通过后仅发布签名包和回执；用户 SDK
与项目目录不必改名。成功时清理带所有权标记的当前暂存目录，失败时保留诊断路径。
特殊环境可通过 `HE_ANDROID_BUILD_ROOT` 指定缓存。Manifest 校验同时支持 Android
系统默认图标和自定义图标：默认图标没有 APK 内的文件名，但必须存在真实资源 ID。

新增 `hectl capabilities -Json`，无需当前项目，数据来自 `Tools/Build/Capabilities.json`。
Build Profile 可声明 `requiredFeatures`；Preflight 拒绝未知或目标不支持的能力。
表中 supported 是实现支持面，不是设备验收结果；adapter 需要项目实际提供并验证。
未列出的 API 不推定支持。15 个 Preflight 用例通过，覆盖四端共享规则、原生场景的
Web 拒绝、未知能力、非法声明与 Android 禁用脚本插件。

| 证据 | 精确命令 | 结果 |
| --- | --- | --- |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Android/Tests/Test-PackageWorkspace.ps1 -Root E:/HEngineSDKTools/Android` | PASS；短路径、所有权负例、Unicode/长路径/UNC 拒绝及清理 |
| unit-test | `py -3.11 Tools/Android/Tests/test_package.py` | PASS；12 个 APK/ELF/指纹检查用例 |
| unit-test | `py -3.11 Tools/Build/test_preflight.py` | PASS；15 个导出预检用例 |
| unit-test | `py -3.11 Tools/Distribution/test_distribution.py` | PASS；6 个发行布局/归档用例，65 个必需文件 |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File "C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2/Tools/Agent/hectl.ps1" capabilities -Json` | PASS；无项目的二进制 SDK 返回四端能力契约 |

## 同一无源码 SDK 的 Web/微信实包复验（2026-10-01）

`Test-PortableExporter.ps1` 从 PATH 排除 VS/NDK，确认安装内没有原生游戏/启动源码和
根 CMakeLists.txt，再执行实际 Publish。它只声明实包构建证据，不将构建成功写成手机运行。

| 证据 | 精确命令 | 结果 |
| --- | --- | --- |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PortableExporter.ps1 -Engine "C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2" -Project C:/WorkSpace/HEngineDev/Project -Profile web` | PASS；实际 Wasm publish 和 517 个原始资源；日志 `out/agent/sdk-web-smalltree-export.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PortableExporter.ps1 -Engine "C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2" -Project C:/WorkSpace/HEngineDev/Project -Profile wechat` | PASS；17.93 MiB，JavaScript EH、无 SIMD/线程、17 个 JS 文件的 ES2019 上传检查；日志 `out/agent/sdk-wechat-smalltree-export.log` |

fixture 中的结构化结果分别为
`out/agent/portable-validation/52d2e1444b264b4e965e32c76abe88a2/result.json` 和
`out/agent/portable-validation/9252c90aaa704605bd26443ab981f878/result.json`。
.NET Wasm 的 C runtime glue 仍会由 workload 自带工具链接，这不是编译 HEngine 原生引擎。

对本次源外 SDK 发布到 `Project/Dist/Web` 的实际站点，继续执行浏览器交互验证：

```powershell
$env:NODE_PATH='C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules'
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestSurvivorWeb.cjs webgl2
```

runtime-manual PASS，日志 `out/agent/sdk-web-runtime.log`。已有浏览器脚本实际操作
菜单/设置、移动、跳跃、镜头、暂停/恢复、失焦、升级选择、触摸与窗口尺寸切换，
并检查无页面异常；自有浏览器与服务器正常关闭。人工查看本轮
`out/agent/web-parity-test/webgl2/game.png` 和 `mobile-game.png`，角色、地形、HUD
及触摸控件可见。该验证使用桌面 Chrome 的触摸模拟，不是微信或 Android 手机测试。

至此小树保卫者的四端发布链路均有无原生引擎源码 SDK 的实包证据，但它仍是已有项目。
当时通用四端新项目模板、工具环境闭环、最终 Editor ZIP 和独立“一句话创建游戏”验收仍待完成；后续完成证据记录如下。

## SharedArena 与独立 AI 原型验收（2026-10-01）

已增加 `SharedArena` 项目模板，以及 SDK 公共 `PortableView` 协议、原生 C# 表现宿主、
Web/微信 Canvas 宿主。规则编译自同一个 `GameSession.cs`；菜单、移动、自动攻击、
升级、胜负、暂停、重开和纪录存档由规则层负责。原生 SDK 仍使用预编译 Player。
表现边界是矩形 Sprite、文字/按钮和音效，不是所有原生组件在四端的完整映射。
`hectl capabilities -Json` 和 `requiredFeatures` 能检查这一边界。

项目管理器新增模板选择；`hectl publish check|build -BuildProfile <id>` 接入 Editor
已有发布后端。SDK 依赖安装增加校验 SHA256 的 Pillow 11.3.0 wheel 和锁定的
wasm-tools 10.0.110。Pillow 在无源码 fixture 中实际下载/安装通过；Wasm 检查使用
已有的锁定 workload，本轮尚无全新机器安装全部 workload 的证据。版本锁定机制参考
[.NET workload install](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-workload-install)。

两个源外项目：

- `C:/WorkSpace/HEngineProjects/SDK 四端 Shared Arena`：直接从二进制 SDK 创建的标准模板。
- `C:/WorkSpace/HEngineProjects/SDK AI 星核护航`：独立 AI 验收原型，需求记录在项目
  `AI-ACCEPTANCE.md`。创建后只改项目 C#，增加移动核心和敌人追踪，不改 SDK 或编译引擎。
  这是有范围的“一句话需求 → 修改玩法 → 验证 → 导出”实例，不代表任意游戏一次生成成功。

以下 `$SDK` 为 `C:/WorkSpace/HEngineSDKLab/Windows SDK 验收 2`，`$Arena`、`$Escort`
分别为上述项目。fixture 仍标记为 ExportIntegrationFixture，并非最终 Editor ZIP。

| 证据 | 精确命令 | 结果与日志 |
| --- | --- | --- |
| unit-test | `dotnet run --project Tools/Build/Tests/PortableRules/RulesTests.csproj --artifacts-path out/agent/shared-arena-tests -c Release` | PASS；规则、存档、胜负、非法视图；`out/agent/shared-arena-rules-r2.log` |
| unit-test | `node Tools/Build/TestPortableView.mjs` | PASS；有限坐标、重复 ID、颜色、预算及首帧校验 |
| unit-test | `py -3.11 Tools/Distribution/test_distribution.py` | PASS；8 个布局/归档用例，75 个必需文件；包括不传游戏项目的 `--engine-only` 实际 CLI 调用和原生源码拒绝检查 |
| unit-test | `py -3.11 Tools/Build/test_preflight.py` | PASS；15 个导出预检用例 |
| test-layer | `powershell -NoProfile -STA -ExecutionPolicy Bypass -File Tools/Agent/ProjectManager.ps1 -SmokeTest -CapturePath out/agent/sdk-project-manager.png` | PASS；四端模板选择/创建、重复路径拒绝、只取消自有进程；未指定项目，未测试 Editor 启动 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-SharedArenaNative.ps1 -Engine $SDK -Project $Arena` | PASS；原生开始/移动/暂停/恢复；`out/agent/sdk-shared-arena-native-r2.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PortableExporter.ps1 -Engine $SDK -Project $Arena -Profile web` | PASS；`out/agent/sdk-shared-arena-web-export-r3.log` |
| test-layer | `node Tools/Build/TestSharedArenaWeb.cjs $Arena` | PASS；浏览器运行、触摸/尺寸、暂停、失焦、存档；`out/agent/sdk-shared-arena-web-runtime-r4.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PortableExporter.ps1 -Engine $SDK -Project $Arena -Profile wechat` | PASS；真实 Wasm/小游戏包及 ES2019 检查；`out/agent/sdk-shared-arena-wechat-export-r2.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-AndroidExporter.ps1 -Engine $SDK -Project $Arena` | PASS；实际 APK、原生模板 hash、签名/对齐；`out/agent/sdk-shared-arena-android-export.log` |
| unit-test | `$env:HEEngineRoot=$SDK; dotnet run --project "$Escort/Tests/RulesTests.csproj" --artifacts-path out/agent/ai-escort-tests -c Release` | PASS；移动目标、暂停/恢复、重开复位、胜利可达；`out/agent/sdk-ai-escort-rules.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-SharedArenaNative.ps1 -Engine $SDK -Project $Escort` | 交互 PASS；`out/agent/sdk-ai-escort-native.log`；随后发现同深度矩形遮挡并修复，复验见后续记录 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PortableExporter.ps1 -Engine $SDK -Project $Escort -Profile web` | PASS；`out/agent/sdk-ai-escort-web-export.log` |
| test-layer | `node Tools/Build/TestSharedArenaWeb.cjs $Escort out/agent/ai-escort-web` | PASS；`out/agent/sdk-ai-escort-web-runtime.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PortableExporter.ps1 -Engine $SDK -Project $Escort -Profile wechat` | PASS；`out/agent/sdk-ai-escort-wechat-export.log` |

浏览器截图 `out/agent/shared-arena-web/game.png`、`touch.png` 已人工查看。原生初次
截图暴露了正交投影使用 OpenGL 深度区间导致近半空间被 DX12/Vulkan 裁剪的问题；
SceneCamera 已改用 `glm::orthoRH_ZO`。新的 Development Player 构建及交互通过，
截图 `out/agent/shared-arena-native-r2-preview.png` 可见角色、核心和敌人。
移动核心样例还暴露了相同深度遮挡，NativeViewPresenter 改为按协议数组顺序分层。

新 Windows Shipping、Editor、相机 headless 回归测试使用 E 盘新缓存，映射记录在
`out/agent/sdk-external-cache-map.json`；没有迁移或删除旧受保护缓存。首次 codegen
拒绝目录联接，CMake 现将生成输出和状态目录转换成真实路径，CodeGenDriver 的
reparse 检查保持不变。第二次 codegen 已为 Tests/Editor 各生成 321 个文件。
编译、无变化生成校验和最终发行包检查仍需各自完成后记录，不能以 codegen 成功代替。

发行版路径检查还修复了 Editor 的工具定位：PowerShell 使用当前机器系统目录，
资源重命名通过 SDK 环境解析 Python，预编译原生插件组合固定；依赖修复版增加离线渲染插件的运行时启用开关。SDK 外的
Unicode 资源重命名入口实测 PASS，日志 `out/agent/sdk-asset-tool-rename.log`。

这一历史检查点尚未完成最终 Editor ZIP 搬迁验收；该项现已通过，结果见文末。
Android/微信真机玩法仍未验证，不能据此章节宣布所有正式发布门禁已通过。

### 后续修复与历史检查点

- `hectl.bat test SceneCameraOrthographicDepth -Project Engine/Templates/Blank -Configuration RelWithDebInfo -Parallel 2 -TimeoutSeconds 120`：
  unit-test PASS；完整 HEngineTests 编译 PASS，日志 `out/agent/sdk-camera-depth-test-r2.log`。
- 使用 pwsh 独立进程执行 `out/agent/TestSDKCodegenNoop.ps1`：static-check PASS，
  changed=0、deleted=0、321 个输出的 hash/mtime 相同；证据位于
  `E:/HEngineBuildCache/HEngineDev-1.1.0/evidence/codegen`。先前一次诊断传入不同的 parser
  路径且通过嵌套 PowerShell 调用，未作为无变化成功证据；另一次捕获脚本没有接到
  Console 输出，亦未计 PASS。最终复验使用同一工具路径、独立 pwsh 进程和前后快照。
- 星核护航 Android 实包 test-layer PASS：
  `Test-AndroidExporter.ps1 -Engine $SDK -Project $Escort`，日志
  `out/agent/sdk-ai-escort-android-export.log`；尚未包含后来更新的视图表现代码及精灵排序，
  最终包仍需新模板重新导出。
- 原生 HUD 曾因每帧调用 SetInnerRML 重建相同内容而在截图中缺失。NativeViewPresenter
  现在仅更新变化的文字/样式，原生 r4 交互 PASS 且截图恢复 HUD；日志
  `out/agent/sdk-ai-escort-native-r4.log`，截图
  `E:/HEngineBuildCache/HEngineDev-1.1.0/evidence/escort/native-r4-preview.png`。
  原生精灵采用无深度写入的混合，原有 ECS 顺序会遮挡玩家；SceneRenderer 已增加
  同层、深度测试场景图元的远到近排序。最终新原生模板已完成视觉复验，见文末。
- Editor 发布、SDK C# 编译和资源重命名现在使用当前机器上的 PowerShell/Python。
  `RunAssetTool.ps1` 的 Unicode 文件重命名实测 PASS。

C 盘容量耗尽使 Windows 包的 HCooker 写入失败，失败日志
`out/agent/sdk-windows-camera-template-build-r2.log` 保留。生产端 Package.ps1 新增
`-OutputRoot`，Build-WindowsTemplate.ps1 新增 `-PackageOutputRoot`；默认路径不变，
显式 E 盘输出仍有项目 hash 命名空间、nonce staging 和所有权检查。新构建命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-WindowsTemplate.ps1 -Output E:/HEngineBuildCache/HEngineDev-1.1.0/templates/Windows-x64-r4 -PackageOutputRoot E:/HEngineBuildCache/HEngineDev-1.1.0/packages
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-DevPlayer.ps1 -BuildVariant sdk-final -Output E:/HEngineBuildCache/HEngineDev-1.1.0/templates/Development-Windows-x64-r3
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-AndroidTemplate.ps1 -Output E:/HEngineBuildCache/HEngineDev-1.1.0/templates/Android-arm64-r3 -WindowsTemplate out/agent/export-templates/Windows-x64-r2 -HostParser out/agent/host-tools/HEngineParser/HEngineParser.exe
```

Android 构建的 `HE_ANDROID_TOOLCHAIN_ROOT=E:/HEngineSDKTools/Android`；新 Windows
构建的 TEMP/TMP 使用 E 盘。Development 新缓存逻辑路径
`out/agent/projects/e1caca9aebe7fcb3/game-portable-sdk-final-release` 映射到
`E:/HEngineBuildCache/HEngineDev-1.1.0/dev-final`。只有新建路径，旧缓存没有移动或删除。
后续 Editor/ZIP 与项目搬迁验证使用 E 盘，源外项目已复制到
`E:/HEngineBuildCache/HEngineDev-1.1.0/validation/星核护航`（不复制 Dist/Saved）。

空间清理：常规 `hectl clean stale -Json` 超过一小时未返回完成报告，已停止该自有扫描；
未计为清理成功或报告释放量。随后缓存只读预览因检测到活动构建而拒绝。自动审批拒绝了删除已结束
Development Player 缓存三个编译子目录的请求，仅返回 blocked by policy；该操作
没有执行，也未改用其他方式重试。不要把未完成扫描或预估缓存大小记成已释放空间。

后续自动审批也拒绝了两个已识别的 Node/CMake 安装暂存目录的清理，未执行删除。
Android-arm64-r3 模板已 build-only PASS。独立 Editor 完整构建 build-only PASS，
日志 `out/agent/sdk-editor-final-build-r2.log`；因精灵排序在该目标编译期间更新，继续
执行增量构建，结果记录于 E 盘 `evidence/editor-incremental.log`。
E 盘 `Validate-FinalSDK.ps1` 仅在三个新模板及 Editor 增量构建全部成功后顺序执行
组装、依赖、项目管理器、原生交互和四端导出；最终阶段状态在
`evidence/final-sdk-stage.txt`。流程开始或中间通过不等于最终 ZIP 验收通过。
Windows 新包首次在 StageRuntime 的 reparse 检查处失败：CMake 传入了联接路径。
现由 CMake 将脚本构建根解析成真实路径，StageRuntime 本身的拒绝规则不变；重试
日志 `evidence/windows-template-retry.log`。SDK 组装还明确过滤 Web/微信目录下的
旧原生源码，Test-Distribution 会拒绝混入原生代码的发行目录。

## 最终验收结果

最终 SDK：`E:/HEngineBuildCache/HEngineDev-1.1.0/release/HEngine-1.1.0-Windows-x64`。
下表 `$FinalSDK` 指该目录，`$FinalProject` 指
`E:/HEngineBuildCache/HEngineDev-1.1.0/validation/星核护航`。

| 证据 | 精确命令 | 已观察结果 |
| --- | --- | --- |
| build-only | `Build-Editor.ps1 -SkipBuild -WindowsTemplate E:/HEngineBuildCache/HEngineDev-1.1.0/templates/Windows-x64-r4 -AndroidTemplate E:/HEngineBuildCache/HEngineDev-1.1.0/templates/Android-arm64-r3 -DevelopmentTemplate E:/HEngineBuildCache/HEngineDev-1.1.0/templates/Development-Windows-x64-r3 -Output $FinalSDK` | PASS，独立 Editor 与三个已验证原生模板；`evidence/final-assembly.log` |
| unit-test | `py -3.11 Tools/Distribution/test_distribution.py` | PASS，9 项、76 个必需文件；`evidence/distribution-tests-r4.log` |
| unit-test | `py -3.11 Tools/Dependencies/test_install_archive.py` | PASS，长路径、运行时合并、非法 archive 路径及符号链接拒绝；`evidence/archive-installer-tests-r2.log` |
| static-check | `powershell -NoProfile -File "$FinalSDK/Tools/Dependencies/Dependencies.ps1" -Action install -Profile SDK -Project $FinalProject -Json` | PASS；实际安装 Node/Pillow/CMake，核查本机 .NET/wasm-tools；`evidence/final-dependencies.log`。Pillow 使用 hash 校验的既有下载缓存；未执行全新 wasm workload 安装 |
| test-layer | `powershell -NoProfile -STA -File "$FinalSDK/Tools/Agent/ProjectManager.ps1" -SmokeTest -Project $FinalProject -CapturePath E:/HEngineBuildCache/HEngineDev-1.1.0/evidence/final-manager.png` | PASS，创建/选择、启动就绪、失败恢复、保留窗口/自动关闭；`evidence/final-manager.log`；截图已查看 |
| test-layer | `powershell -NoProfile -File Tools/Distribution/Test-SharedArenaNative.ps1 -Engine $FinalSDK -Project $FinalProject -EvidenceRoot E:/HEngineBuildCache/HEngineDev-1.1.0/evidence/final-native` | PASS，开始、移动、暂停、恢复；`evidence/final-native/arena-be3f9bc0/result.json`；JPEG 预览已查看，HUD 与角色覆盖正确 |
| runtime-manual | `powershell -NoProfile -File E:/HEngineBuildCache/HEngineDev-1.1.0/Check-SmallTreeEditor.ps1` | PASS；真实 SDK Editor `--project=C:/WorkSpace/HEngineDev/Project --graphics-api=DX12 --resolution=1280x720 --no-cursor-capture --exit-after-frames=60 --capture-frame=45 --capture-path=E:/HEngineBuildCache/HEngineDev-1.1.0/evidence/final-smalltree-editor.png`；场景 MainGameCSharp 截图已查看；不代表点击 Play 或发布按钮 |
| test-layer | `powershell -NoProfile -File Tools/Distribution/Test-WindowsExporter.ps1 -Project $FinalProject -ValidationRoot $FinalSDK -SkipAssembly` | PASS；192 个 app-local PE 依赖闭包、Shipping 策略、完整 WARP 启动，EXE hash 等于模板；`evidence/final-windows.log` |
| test-layer | `powershell -NoProfile -File Tools/Distribution/Test-AndroidExporter.ps1 -Engine $FinalSDK -Project $FinalProject` | PASS；实际 APK、原生库 hash、12 个库与 16 KiB 对齐、Manifest 和开发签名；`evidence/final-android.log`；无 NDK、无原生编译 |
| test-layer | `powershell -NoProfile -File Tools/Distribution/Test-PortableExporter.ps1 -Engine $FinalSDK -Project $FinalProject -Profile web` | PASS；`evidence/final-web.log` |
| test-layer | `powershell -NoProfile -File Tools/Distribution/Test-PortableExporter.ps1 -Engine $FinalSDK -Project $FinalProject -Profile wechat` | PASS；实际 Wasm/小游戏包与 JS 兼容检查；`evidence/final-wechat.log` |
| test-layer | `node Tools/Build/TestSharedArenaWeb.cjs $FinalProject E:/HEngineBuildCache/HEngineDev-1.1.0/evidence/final-web-runtime` | PASS；开始/移动/暂停/失焦/存档重载/触摸与尺寸、无 page error；两张截图已查看 |

实际搬迁暴露并修复了两处路径限制：依赖解压由支持 Windows 长路径的 Python 工具
执行；Windows/Android 导出的临时工作区改用项目 hash 与独立 nonce，避免重复日志
目录层级，并保持原发布锁和原子输出替换。Android cook 第一次仍被过长目录阻断，
后续进一步缩短 Android 临时目录；失败记录 `evidence/android-longpath-failure.log`。
发行目录已同步这些修复。归档命令为
`py -3.11 Tools/Distribution/Archive-Release.py --release E:/HEngineBuildCache/HEngineDev-1.1.0/release --engine-only`；
static-check PASS，日志 `evidence/final-archive-r2.log`。归档现在先剪除不应发布的
缓存目录，再枚举文件，避免遍历被排除缓存时触发长路径错误；临时 ZIP 经 CRC 和
SHA-256 检查后才成为最终产物。

最终产物：`E:/HEngineBuildCache/HEngineDev-1.1.0/release/HEngine-1.1.0-Windows-x64.zip`，
430,148,010 bytes、9,059 个文件；SHA-256：
`1ce739b0fc942a8506cad3cc3699dec51686ed77daa8cb9fae055dc52c8b07de`。
同目录 `artifacts.json`、`SHA256SUMS.txt` 记录这些信息。ZIP 不包含根 Project、
个人工作区绑定、原生引擎源码或 out 缓存；归档内容在最终验收后保持不变。

解压安装：`E:/HEngineSDK 验收 1.1.0/HEngine-1.1.0-Windows-x64`，仅从上述 ZIP 提取。
执行 `powershell -NoProfile -ExecutionPolicy Bypass -File E:/HEngineBuildCache/HEngineDev-1.1.0/Verify-RelocatedSDK.ps1`：
test-layer PASS，`evidence/relocation-stage.txt` 与 `evidence/relocation-pipeline.log`
记录最终完成状态。分项证据如下：

- static-check PASS：解压时 CRC/SHA-256、完整布局、三个模板文件哈希检查；
  `evidence/final-extract.log`、`evidence/relocated-layout.log`。
- static-check PASS：新安装目录使用锁定缓存归档安装自己的 Node、CMake、Pillow；
  使用本机已有 Python/.NET/wasm-tools，`evidence/relocated-dependencies.log`。
- runtime-manual PASS：真实 `ProjectManager.exe` 窗口标题及响应正常，正常关闭 exit 0；
  `evidence/relocated-launcher.log`。
- test-layer PASS：项目管理器 smoke test、外部星核护航项目 Editor 启动就绪；
  `evidence/relocated-manager.log`。
- test-layer PASS：解压后的 SDK 再次导出 Windows，EXE 哈希与预编译模板相同，
  Shipping 策略、依赖及完整 WARP 启动通过；`evidence/relocated-windows.log`。

包内文档是归档前的实现快照；本仓库文档额外记录最终 ZIP 与解压后的验收证据。
没有连接 Android 设备，微信手机与 Android 真机玩法未验收；Android 验收 APK
使用开发签名，正式上架需使用项目自己的签名配置。全新 Python/.NET/wasm workload
安装未在干净机器重复执行；不能把当前机器上的依赖检查当作干净机器证明。

## C 盘交付位置（2026-10-02）

按用户要求复制到 `C:/WorkSpace/HEngineReleases/1.1.0`，保留 E 盘原件。
ZIP 及 SHA-256 与上述最终产物相同，9,059 个解压文件逐一校验通过。
ZIP 自带顶级目录，因此实际引擎 `$CSDK` 位于
`C:/WorkSpace/HEngineReleases/1.1.0/HEngine-1.1.0-Windows-x64/HEngine-1.1.0-Windows-x64`。
独立示例 `$CProject` 位于 `C:/WorkSpace/HEngineReleases/1.1.0/Projects/星核护航`，
包括四端 Dist；复制的 1,167 个项目文件逐一校验通过，本机引擎绑定已更新。
项目 Saved、bin/obj 和旧本机绑定没有复制。

| 证据 | 精确命令 | 结果 |
| --- | --- | --- |
| static-check | `py -3.11 out/agent/Copy-ReleaseToC.py` | PASS，ZIP SHA-256、解压文件与项目复制完整性；交付根目录 `copy-verification.json` |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-Distribution.ps1 -Engine $CSDK -Json` | PASS，76 个必需文件；首次误用外层解压目录时 FAIL，改用实际内层 SDK 后通过 |
| test-layer | `powershell -NoProfile -STA -ExecutionPolicy Bypass -File "$CSDK/Tools/Agent/ProjectManager.ps1" -SmokeTest -Project $CProject` | PASS，创建/打开、启动就绪、失败恢复、保留窗口/自动关闭；交付根目录 `project-manager-check.log` |

自动审批拒绝了展平解压目录的移动及空目录删除命令，仅返回 `blocked by policy`；
未执行该操作，保留现有目录结构。复制后 C 盘可用空间约 33.7 GiB。
本次仅复制与搬迁验证，没有重新编译引擎或重复四端导出。

## 发布依赖修复（2026-10-03）

此前归档的 Windows Shipping 模板启用了 FSR，却未复制通过动态加载使用的
FidelityFX DLL，普通 PE import 检查不能发现这类遗漏。此次为 Editor、Development
Player 和 Shipping Player 增加 `HEngine.Runtime.json`，记录实际编译功能；发布检查
按该声明验证 Agility、FSR 与 OIDN 的锁定 SHA-256、许可证及 PE 依赖闭包。
缺少动态库、文件被修改、禁用后残留动态库均阻止组装。FSR 也进入模板兼容指纹。
新增的闭包检查进一步发现 FSR/OIDN 依赖 `msvcp140.dll`、`vcruntime140.dll` 和
`vcruntime140_1.dll`。它们现在从所选 VS 的正式 x64 redistributable 目录部署到应用
旁边；不复制 System32 文件，也不依赖用户机器已安装开发环境。

OIDN 归属于 `OfflineReferenceRenderer`。官方二进制 Editor 编入该插件，但项目可以
在插件面板勾选/取消，重启生效；偏好保存在项目 `Saved/Config/EditorPlugins.json`，
首次使用回退到项目 `Config/Plugins.json`。仅启用离线渲染不会立即加载 OIDN，首次
CPU 降噪时才按引擎安装目录加载。当前仅发布 CPU 后端，三个 OIDN DLL 放在
`Plugins/OfflineReferenceRenderer`，不携带 CUDA/HIP/SYCL。共享 oneTBB 放在程序
根目录，统一使用 OIDN 2.5.1 配套版本，并验证它与 Embree 共存。Shipping 不携带
离线插件和 OIDN；插件运行时文件及许可证在增量构建中也会恢复，不再依赖重新链接。

二进制用户无需完整 FSR/OIDN 开发 SDK，也无需编译引擎；头文件、导入库仍用于
源码构建。复选框控制本次启动是否注册插件，不删除安装文件。安装器将来可以按组件
安装这些文件；不得把运行时开关误写成已经实现按需下载或卸载。

修复版使用 C 盘新的交付目录，保留此前归档和验收记录。原 ZIP 的历史 PASS
不代表它覆盖了此次发现的 FSR 动态加载问题。

已完成的源码侧验证：

| 证据 | 精确命令 | 结果 |
| --- | --- | --- |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineEditor -Configuration RelWithDebInfo -Project Engine/Templates/Blank -PortableEditor -PluginConfig Tools/Distribution/Plugins.json -Parallel 2` | PASS；`out/agent/release-deps-editor-final-build.log` |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name OfflineReferenceRendererSession -Project Engine/Templates/Blank -PluginConfig Project/Config/Plugins.json -Configuration RelWithDebInfo -Parallel 2 -TimeoutSeconds 120` | PASS；实际启用 OIDN，包含异步 CPU 降噪与误差下降断言；`out/agent/release-deps-oidn-test-retry.log` |
| unit-test | `py -3.11 Tools/Distribution/test_distribution.py` | PASS，14 项；包括缺少 FSR、篡改 DLL、禁用后残留、额外 OIDN GPU 后端和 Shipping 携带 OIDN 的拒绝；`out/agent/release-deps-distribution-tests.log` |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-Template.ps1` | PASS，模板契约 fixture；不代表设备运行 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-WindowsTemplate.ps1 -Output C:/WorkSpace/HEngineDev/out/agent/export-templates/Windows-x64-deps-fix -PackageOutputRoot C:/WorkSpace/HEngineDev/out/agent/release-dependency-packages` | PASS，197 个 app-local 二进制依赖闭包、Shipping 策略、WARP 完整启动与模板哈希；`out/agent/release-deps-windows-crt-retry.log` |

首次 Shipping 重试被新增 PE 门禁正确拒绝：FSR 缺少 `MSVCP140.dll`，日志
`out/agent/release-deps-windows-retry.log`。之后补齐 CRT；不能将这个失败记录记作
成功，也不能把使用开发机系统 DLL 的运行观察当成发行包完整性证明。

修复后的 `$DependencySDK` 为
`C:/WorkSpace/HEngineReleases/1.1.0-dependency-fix/HEngine-1.1.0-Windows-x64`，
`$DependencyProject` 为同一交付根目录的 `Validation/SharedArena`。
使用新的 Windows Shipping / Development 模板及既有已验证的 Android-arm64-r3
模板组装。二进制原生代码和运行库的实际验收如下：

| 证据 | 精确命令 | 结果 |
| --- | --- | --- |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HGame -Configuration Release -Project Engine/Templates/Blank -PortablePlayer -PluginConfig Tools/Distribution/PlayerPlugins.json -Parallel 1 -BuildVariant sdk-final`；随后 `Build-DevPlayer.ps1 -BuildVariant sdk-final -SkipBuild -Output C:/WorkSpace/HEngineDev/out/agent/development-templates/Windows-x64-deps-fix` | PASS；`out/agent/release-deps-dev-player-retry.log` |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-Distribution.ps1 -Engine $DependencySDK -Json` | PASS，85 个必需文件、无根 Project；`out/agent/release-deps-final-layout.log` |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-EditorDependencies.ps1 -Engine $DependencySDK -Json` | PASS，215 个二进制、401 条 imports，包含动态运行库和独立工具；`out/agent/release-deps-final-pe.log` |
| test-layer | `py -3.11 Tools/Dependencies/Test-OidnRuntime.py --engine $DependencySDK` | PASS，OIDN 2.5.1 CPU 实际降噪，平方误差 162.727 → 2.293，同进程 Embree 场景构建成功；`out/agent/release-deps-final-oidn.log` |
| runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File out/agent/Check-ReleaseNativeDeps.ps1 -Engine $DependencySDK -EditorOnly -Evidence C:/WorkSpace/HEngineReleases/1.1.0-dependency-fix/evidence-runtime-r2` | PASS，实际启动分别确认插件 enabled/disabled；材质场景离线渲染 320×180、完成 8 spp，面板显示 OIDN CPU ready、DenoisedSamples=8；三张截图预览已查看。这是启动配置与视觉验收，不是自动点击复选框测试 |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File out/agent/Build-ReleaseFsrProbe.ps1` | PASS；`out/agent/release-deps-fsr-build-dev.log`。诊断工具复用 Development Player 缓存，临时选项在 finally 恢复，不进入 SDK |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File out/agent/Run-ReleaseFsrProbe.ps1 -Engine $DependencySDK -Evidence C:/WorkSpace/HEngineReleases/1.1.0-dependency-fix/evidence-fsr-r2` | PASS，使用实际 SDK DLL；FSR 3.1.4 DX12/Vulkan 各验证 1080p、1440p、2160p 派发/回读、重置、resize、非法输入拒绝；最大回读误差 0.003640。日志 `out/agent/release-deps-fsr-runtime-r2.log` 与交付根目录 `evidence-fsr-r2/fsr-smoke.stdout.log` |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-WindowsExporter.ps1 -Project $DependencyProject -ValidationRoot $DependencySDK -SkipAssembly` | PASS，无原生源码导出，EXE 与预编译模板哈希一致，197 个 app-local 二进制依赖、Shipping 策略及 WARP 启动通过；`out/agent/release-deps-final-windows-export.log` |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File "$DependencySDK/Tools/Dependencies/Dependencies.ps1" -Action check -Profile SDK -Project $DependencyProject -Json` | PASS；`out/agent/release-deps-final-sdk-deps-check.log`；使用本机 Python/.NET/wasm workload，不能视为干净机器安装证明 |

补充发现并修复了 CMake 安装完整性自检：只有 `--version` 不足以证明安装完整。
中断的安装曾输出 `Could not find CMAKE_ROOT` 但仍被旧检查计为通过；新增
`Tools/Dependencies/CheckCMake.cmake` 真实执行脚本模式并核对模块树。
`release-deps-incomplete-cmake-check.log` 记录正确拒绝该不完整安装，随后
`Dependencies.ps1 -Action install -Profile SDK -Project $DependencyProject -Json`
使用 SHA-256 验证的 CMake 4.2.0 缓存归档修复，最终独立 check 通过。

诊断工具初次误用了 Editor 缓存，因其不含 Dev Agent 的纹理故障注入 API 而编译
失败；改用 Development Player 缓存后完整诊断通过，没有删除或跳过断言。
第一次便携诊断运行缺少相对默认目录，补上 SDK 的 Blank 项目和配置后通过。
两次错误调用均不计作成功证据。Android、Web、微信此次未重复设备验收；此次
修复针对 Windows x64 原生运行库及 SDK 安装检查，未新增 ARM64 Editor 或安装器。

修复版最终归档：
`C:/WorkSpace/HEngineReleases/1.1.0-dependency-fix/HEngine-1.1.0-Windows-x64.zip`，
483,802,253 bytes、9,193 个文件，SHA-256：
`3caf25d32b242d9f8705ae579ca87bf87edd3bd088044c554eb7417872062ec9`。
命令 `py -3.11 Tools/Distribution/Archive-Release.py --release C:/WorkSpace/HEngineReleases/1.1.0-dependency-fix --engine-only`
static-check PASS（CRC、归档 SHA-256）；日志 `out/agent/release-deps-archive.log`。
归档本体另外逐项检查 85 个必需文件及 7 个锁定运行库的 SHA-256，并确认不含根
Project、out 缓存和 FSR 诊断程序；交付根目录 `archive-dependency-check.json`。
归档后的哈希记录仅追加到仓库文档，未修改已验收的 ZIP。C 盘剩余约 20.2 GiB。

## 设计参考

预编译 Player 与项目数据分开是成熟的导出方式，可参考 [Godot Windows 导出说明](https://docs.godotengine.org/en/stable/tutorials/export/exporting_for_windows.html)。本文只参考产品结构，不声称 HEngine 已实现相同功能。

.NET Wasm 工具仍承担原生重链接/AOT 等工作，见 [.NET runtime 的 Wasm 功能文档](https://github.com/dotnet/runtime/blob/main/src/mono/wasm/features.md)。具体版本以本仓库锁定配置为准，不能用上游最新文档替换已有兼容性验收。

## 2026-10-12 当前源码重新构建与独立工程迁移

日期：2026-10-12（Asia/Hong_Kong）。这是本轮重建记录，历史 2026-10-03 SDK 的结果不作为新包的通过证据。

引擎源目录：`C:/WorkSpace/HEngineDev`。游戏目录：`C:/WorkSpace/HEngineProjects/StarbloomSurvivor`。
发行目录：`C:/WorkSpace/HEngineReleases/1.1.0-final-20261012`。
游戏提交：`c6f7449c7fff9d7c9b5dd32e89638d7a654f7f9a`。

## 已完成

| 证据类型 | 精确命令或检查 | 结果 |
| --- | --- | --- |
| static-check | 原 Project 的 751 个版本控制文件迁移前后 SHA-256 和大小逐项比较 | PASS，原目录已移出；本地 Saved/Dist 保留 |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-Projects.ps1` | PASS，Blank 默认项目、外部工程选择和缓存隔离 |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/BuildManaged.ps1 -Project C:/WorkSpace/HEngineProjects/StarbloomSurvivor -WorkDirectory C:/WorkSpace/HEngineDev/out/agent/release110-migration/managed-precompile -OutputDirectory C:/WorkSpace/HEngineDev/out/agent/release110-migration/managed-precompile/output -EditorHost -IsolatedCompiler` | PASS，独立工程当前 C# 预编译程序集 |
| static-check | `py -3.11 Tools/Build/Preflight.py --project C:/WorkSpace/HEngineProjects/StarbloomSurvivor --profile <profile> --output out/agent/release110-migration/preflight-<profile>.json`；profile 逐项为 `windows`、`android`、`web`、`wechat` | 四项 PASS，各 218 个资源 |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineEditor -Configuration RelWithDebInfo -Project Engine/Templates/Blank -PortableEditor -PluginConfig Tools/Distribution/Plugins.json -Parallel 2` | PASS，新 Editor 与历史 SDK 的 EXE SHA-256 不同 |
| unit-test | `hectl.bat test <name> -Project Engine/Templates/Blank -Configuration RelWithDebInfo -Parallel 2 -TimeoutSeconds 120`；name 逐项为 `DirectionalShadowMath`、`AnimationPoseContract`、`NavigationGeometry`、`PhysicsScriptContact`、`PhysicsRayQuery` | 五项 PASS |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-WindowsTemplate.ps1 -Output C:/WorkSpace/HEngineDev/out/agent/export-templates/Windows-x64-release110-20261012 -PackageOutputRoot C:/WorkSpace/HEngineDev/out/agent/release110-packages` | PASS，当前源码 Shipping；cooker 自检、197 个本地 PE 依赖闭包、Shipping 策略和 WARP 完整启动 |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-DevPlayer.ps1 -Output C:/WorkSpace/HEngineDev/out/agent/development-templates/Windows-x64-release110-20261012 -BuildVariant sdk-final` | PASS，当前源码 Development Player |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File out/agent/release110-migration/correct-dev-template.ps1`；调用修正后的 `Build-DevPlayer.ps1 -Output C:/WorkSpace/HEngineDev/out/agent/development-templates/dev110-r2 -BuildVariant sdk-final -SkipBuild` | PASS，排除 10 个历史诊断文件；新编译的 Player SHA-256 未改变；原模板保留为恢复材料 |
| static-check | `out/agent/release110-migration/clean-checkout-check.json`：从独立仓库的已推送提交建立干净检出，与本地 762 个 tracked 文件逐项 SHA-256 比较 | PASS；不含 Saved/Dist。GitHub main 的 SHA 已另由 `git ls-remote` 核对 |
| static-check | `git diff --cached --check`；`git ls-files -- Project` | PASS；引擎暂存版本中 Project 路径数量为 0 |

## 后续结果

以下命令中的 `$SDK` 为 `C:/WorkSpace/HEngineReleases/1.1.0-final-20261012/HEngine-1.1.0-Windows-x64`，`$Project` 为上述独立游戏目录，`$Evidence` 为 `C:/WorkSpace/HEngineDev/out/agent/release110-migration`。Android 相关命令设置进程环境 `HE_ANDROID_TOOLCHAIN_ROOT=E:/HEngineSDKTools/Android`。

| 证据类型 | 精确命令或检查 | 结果 |
| --- | --- | --- |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-AndroidTemplate.ps1 -Output C:/WorkSpace/HEngineDev/out/agent/export-templates/Android-arm64-release110-20261012 -WindowsTemplate C:/WorkSpace/HEngineDev/out/agent/export-templates/Windows-x64-release110-20261012 -HostParser C:/WorkSpace/HEngineDev/out/agent/projects/e1caca9aebe7fcb3/editor-portable-relwithdebinfo/bin/Programs/HEngineParser/HEngineParser.exe` | PASS，当前源码 arm64 模板、16 KiB ELF、Shipping 和插件契约检查 |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-Editor.ps1 -Output $SDK -SkipBuild -WindowsTemplate C:/WorkSpace/HEngineDev/out/agent/export-templates/Windows-x64-release110-20261012 -AndroidTemplate C:/WorkSpace/HEngineDev/out/agent/export-templates/Android-arm64-release110-20261012 -DevelopmentTemplate C:/WorkSpace/HEngineDev/out/agent/development-templates/Windows-x64-release110-20261012` | PASS；这里仅组装已重新编译的 Editor/模板。85 个必需文件、215 个 PE、401 条 imports 检查通过 |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File "$SDK/Tools/Dependencies/Dependencies.ps1" -Action check -Profile SDK -Project $Project -Json` | PASS，本机锁定依赖；不是全新机器离线安装证明 |
| test-layer | `powershell -NoProfile -STA -ExecutionPolicy Bypass -File "$SDK/Tools/Agent/ProjectManager.ps1" -SmokeTest -Project "$Evidence/sample-clean-checkout" -CapturePath "$Evidence/project-manager.png"` | PASS，新建 SharedArena 四端配置、重复创建拒绝、取消隔离、最近项目、真实 Editor 就绪握手、失败恢复、保持窗口和自动关闭 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File "$Evidence/validate-gameplay.ps1" -Engine $SDK -Project "$Evidence/sample-clean-checkout" -Evidence "$Evidence/gameplay"` | PASS，二进制 SDK Player 与模板 SHA-256 一致；菜单开始、移动 4.59 米、暂停冻结、继续后恢复移动；自有会话 finally 停止 |
| runtime-manual | 顺序查看暂停及恢复后的 1280×720 JPEG 预览 | PASS，暂停面板正确绘制，恢复后关闭；不等于 Editor F6 按键验收 |

| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-WindowsExporter.ps1 -Project $Project -ValidationRoot $SDK -SkipAssembly` | PASS，真实 C# 编译/cook/导出，模板 EXE SHA-256 一致、197 个 PE 依赖、Shipping 策略和 DX12/WARP 完整启动 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-AndroidExporter.ps1 -Engine $SDK -Project $Project` | PASS，真实 APK、模板原生库哈希一致、开发签名、16 KiB 对齐；隐藏 NDK/VS 原生编译路径后导出 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PortableExporter.ps1 -Engine $SDK -Project $Project -Profile web` | PASS，实际 managed Wasm 发布，index.html、main.js、dotnet.js 和内容清单齐全 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-PortableExporter.ps1 -Engine $SDK -Project $Project -Profile wechat` | PASS，实际 managed Wasm 发布，game.js、game.json、project.config.json 和游戏清单齐全 |

四项均使用本轮重新构建的无原生源码 SDK。新产物位于独立工程的 `Dist/Windows`、`Dist/Android`、`Dist/Web`、`Dist/WeChat`。最终 ZIP 校验和远端提交/发布证据作为发行附件提供；不能将导出通过等同于浏览器游玩、Android/微信真机或商店审核通过。

## 重试与限制

- 首次 C# 预编译把输出写入源树，输入一致性检查失败；改为外部输出后通过。
- 两项 RelWithDebInfo 链接同时运行导致内存压力；中止自有测试构建，编辑器完成后串行重试五项测试通过。
- 第一次 Android 模板构建未指定已安装的 E 盘工具链，报缺少 NDK；确认 `E:/HEngineSDKTools/Android` 的锁定版本后重试。
- Android 编译被主动中断的回合终止，保留缓存并从已有对象继续；中断不计为通过。
- 开发模板修正的首次调用失败；改用更短的临时输出目录后重组通过。
- 游玩测试初次误用 `ui.element.rect.visible` 判断父面板隐藏；RmlUi 默认只查询元素自身。改用暂停时停止移动、继续后恢复移动的实际行为断言，并查看截图后通过；未修改产品代码绕过测试。
- 首次 Windows 导出因历史 Dist 记录旧工程路径而正确拒绝覆盖。四个平台原产物分别保留为 `Dist/<平台>.before-project-split-20261012`，再在标准路径导出；未删除历史产物。
- 现有 E 盘构建缓存通过目录联接访问，storage 登记有非阻断警告；未删除或绕过目录联接保护。
- 本轮未运行 GitHub Actions，YAML 解析依赖不可用；工作流修改经过定向 diff 检查。
- Android/微信手机验收、商店审核和用户生产签名需要单独执行；开发签名不能作为商店发布凭据。
