# HEngine 多项目与内容边界

HEngine 由一个只读的引擎内容根和一个可写的项目根组成。小树保卫者位于独立仓库 [hebohang/StarbloomSurvivor](https://github.com/hebohang/StarbloomSurvivor)，不再包含在引擎 `main` 中；本机放在 `C:/WorkSpace/HEngineProjects/StarbloomSurvivor`。引擎仓库默认使用 `Engine/Templates/Blank`。

示例仓库的 `main` 只维护 C# 玩法，`python` 分支只维护 Python 玩法；推荐分别克隆到不同目录。两个工程的默认场景和 `Config/Plugins.json` 分别选择自己的脚本后端。Python 分支可使用二进制 Editor 内的 Python 后端游玩；标准二进制 Game/Shipping 模板仍为 C#，Python Windows 独立打包使用源码引擎。

C# 示例把玩法目录保存在 `Config/GameplayCatalog.json`，构建时由 Node.js 导出内容。Web/微信的 `contentExporter` 支持 `.py`（Python）或 `.cjs`（Node.js），均通过项目相对路径解析，并使用相同的 `--output` 参数与 `HE_ENGINE_ROOT` 环境变量。

## 目录约定

```text
HEngineDev/
  dist/                      # gitignored 的可分发包根；默认项目发布到这里
  <build>/
    GeneratedContent/Engine/ # 当前构建独占的 DXIL/metadata；可删除重建
  Engine/
    Content/                 # 游戏运行时需要的内建资产、Shader、脚本库
    Resources/               # Editor/工具资源和引擎第三方许可证
    Source/
      ThirdParty/            # 引擎共享的第三方源码、固定 SDK 与 runtime
HEngineProjects/
  StarbloomSurvivor/          # 独立示例仓库；目录名可以更换
    Project.heproject
    Assets/
    Config/
    Scripts/                 # 可省略；没有项目 Python 时无需占位空目录
    Shaders/                 # 可省略；只放项目自定义 Shader
    Source/Game/
    Resources/Windows/
    Legal/
    Saved/                   # 不参与 cook
```

`Project.heproject` 是项目身份的唯一来源：

```json
{
  "SchemaVersion": 1,
  "ProjectId": "starbloom-survivor",
  "DisplayName": "Starbloom Survivor",
  "EngineVersion": "1.1.0",
  "GameConfig": "Config/Game.json",
  "PackageConfig": "Config/Package.json",
  "LegacySavedDirectory": "StarbloomSurvivor",
  "ExecutableName": "StarbloomSurvivor"
}
```

`ProjectId` 必须稳定，用于包所有权和用户存档隔离；`ExecutableName` 是安全的 Windows 文件名，不包含 `.exe`。`LegacySavedDirectory` 可省略，仅用于已有产品继续读取旧 `%LOCALAPPDATA%/HEngine/<name>/Saved`。

## 工作区默认项目

工作区是 Agent 的工作上下文，项目是可以独立搬移的游戏源码和内容，引擎提供运行时、Editor 和工具。**目录不必同级**。`HEngine.workspace.json` 提交到版本控制，默认指向 `Engine/Templates/Blank`。个人通过 `hectl project use -Project <目录>` 写入忽略提交的 `HEngine.workspace.local.json`，无需把机器路径加入共享配置。

选择顺序：显式 `-Project` → `HE_PROJECT_DIR` → 从调用目录向上找到的最近工作区或项目 → 引擎根工作区配置 → 存在时的传统 `<engine>/Project` → `Engine/Templates/Blank`。向上查找到引擎根即停止；每个目录先检查 `HEngine.workspace.local.json`，再检查 `HEngine.workspace.json`，最后检查 `Project.heproject`。工作区中的相对路径以配置所在目录为基准。本机配置的空 DefaultProject 表示恢复团队默认。

绑定失效或版本不兼容会报错，不会悄悄构建另一个游戏。`project manager/create/use/reset/list` 不依赖有效默认项目，可以修复绑定。环境变量会覆盖本机默认，遇到选择疑问时先查看 `current` 的 `selectedBy`。

```powershell
.\hectl.bat project current -Json
.\hectl.bat project manager
.\hectl.bat project create -Project D:\Games\MyGame -Name "我的游戏"
.\hectl.bat project use -Project D:\Games\MyGame
.\hectl.bat editor open          # 后续无需重复 -Project
.\hectl.bat game check
.\hectl.bat project reset        # 恢复团队默认 Engine/Templates/Blank
```

双击 `Win-ProjectManager.bat` 或使用 Editor 的“文件 → 项目管理…”可打开独立管理窗口，无需先构建引擎。它提供最近项目、descriptor 浏览、创建空白项目、设为默认、恢复默认、移除最近记录、后台构建、取消与失败日志。取消只作用于此窗口启动的构建进程树。没有可用默认项目时，`Win-RunEditor.bat` / `hectl editor open` 自动进入该窗口。打开另一项目会启动独立 Editor，保留原窗口和未保存内容；窗口标题显示项目名和渲染后端。

管理器采用“打开项目 / 创建项目”分页，默认 1080×720，并按屏幕工作区限制初始尺寸。默认表单不需要滚动，详细日志默认折叠；长列表按需滚动，滚动条在悬停或键盘操作时显示。窗口和发行 EXE 使用统一的 HEngine 图标。“创建并打开”会在项目创建成功后直接进入同一构建/启动流程，创建失败则保留表单和错误信息。

本机选择只写 gitignored 的 `HEngine.workspace.local.json`，不修改团队默认，也不自动切换 Codex 当前任务目录。现有任务可以按解析后的绝对路径工作；长期开发独立游戏时，建议在 Codex 中打开游戏目录，使源码上下文与项目一致。

最近项目支持单击查看详情、左键双击打开，以及选中后按 Enter 打开。双击使用所点击条目的项目路径，与“打开项目”按钮共用构建/启动流程；空白区域和右键双击不会打开项目，构建期间重复激活不会启动第二份构建。

默认在 Editor 加载初始场景、完成首帧并确认窗口可响应后，自动关闭项目管理器。勾选“打开后保留项目管理器”可继续管理其他项目；该本机偏好保存在 `<engine>/out/agent/projects/manager.json`，不改变工作区默认项目。构建成功不等于打开成功：启动失败、提前退出或启动超过 120 秒时保留管理器、恢复重试入口，并展开错误日志。“完整日志”可打开对应文件。构建日志旁的 `.editor.stdout.log`、`.editor.stderr.log` 和 `.state.json` 分别记录运行输出、错误输出和启动结果。

构建子进程、标准输出/错误管道、日志文件和界面读取统一使用 UTF-8。`ProjectProcess.cs` 并行排空两个输出管道并保留退出码；启动脚本使用 UTF-8 BOM 的 `.build.ps1`，避免 Windows PowerShell 5 的 OEM 解码、UTF-16 重定向和 `-EncodedCommand` 的 CLIXML 信息记录导致乱码。

同一项目的多个管理器通过项目锁避免并发构建/启动；若该项目 Editor 已运行，则提示继续使用现有窗口或关闭后重试，不覆盖正在使用的程序。不同项目仍可各自打开。取消只处理此次尚未交付的构建/启动进程；已确认就绪并交付给用户的 Editor 不随项目管理器关闭而退出。

新项目带有 `hectl.bat`、`AGENTS.md` 和本机 EngineRoot 关联。从任意目录调用项目端 hectl 都会选中该游戏。项目搬移后仍可使用；引擎搬移或换机器时，更新本机配置的 EngineRoot。共享 descriptor 不保存机器绝对路径。

## 输出位置与编译边界

| 内容 | 位置 |
| --- | --- |
| 仓库内 Project 的构建 | `<engine>/out/agent/game-relwithdebinfo`、`editor-relwithdebinfo`，保留现有入口 |
| 外部项目 CMake、对象、生成源码、Shader cache | `<engine>/out/agent/projects/<key>/<host>-<config>` |
| 外部项目 Agent 会话 | 对应 Game 构建目录中的 `sessions` |
| 日志、截图 | 引擎 `out/agent/logs` 或对应项目哈希目录的 `logs`、`captures` |
| 最近项目列表 | `<engine>/out/agent/projects/recent.json`，可重建的本机历史 |
| Editor/开发游戏本机状态 | `<project>/Saved` |
| Build Profiles 发布产品 | `<project>/Dist/<平台>` |
| `hectl package` 原生 cook 发布目录 | `<engine>/dist` 或 `dist/projects/<key>`，保留已有发布所有权检查 |

`key` 是 `ProjectId + 规范化绝对路径` 的 SHA-256 前 16 个十六进制字符。固定长度避免长项目 ID 触发 MSVC 输出路径限制。相同 ID 的两个 checkout 也不会共用缓存。旧版 `ProjectId-路径哈希` 缓存不会自动搬移或删除，新版首次使用外部项目需要重新构建。

构建缓存留在引擎工作区，与工具链、引擎版本和项目配置一起管理；不要放在项目同级目录，那样既不表达所有权，也容易混入其他项目。项目的可交付产品和本机编辑状态则归项目自身。清理继续使用 hectl 的保留/回收工作流。

项目管理窗口将 `project-open` 日志登记为外层工作流，启动完成后不立即执行全工作区回收，避免历史扫描阻塞 Editor 打开；后续普通 hectl 操作仍按已有策略回收。取消产生的未知终态证据保持保护。

源码构建的 Editor/Game 携带项目身份及静态原生插件配置，切换项目通过 hectl 配置并构建对应宿主。1.1.0 发行版的通用 Editor 由空白模板构建，随包带齐公开编辑插件，通过 `--project` 选择工程，无需重新编译 C++。项目名、场景、资源和 C# 程序集从所选工程读取。原生 C++ 项目扩展仍需源码构建；不支持运行时加载任意原生插件。

发行版入口为 `ProjectManager.exe`，安装目录中没有 `Project/` 也可正常新建或导入。预编译 C# 示例不要求系统安装 .NET；重新编译脚本需要指定 SDK 和项目构建依赖。成功编译的 C# 代际保存在项目 `Saved/Scripting/CSharp`，重启后继续使用；`Scripts/CSharp/Prebuilt` 是首次打开的后备。1.1.0 二进制 SDK 携带 Windows Shipping、Windows Development 和 Android 预编译模板，并支持 Web、微信小游戏导出。标准 C# 工程导出不需要 VS 或重编译引擎；脚本编译、内容 cook 和对应平台打包仍会使用随工具入口解析的固定工具链。原生 C++ 项目扩展需要源码构建。

通用场景生命周期、渲染和 Agent 接入位于 `Engine/Source/Game/Runtime/RuntimeGameLayer`，项目只添加自身行为。

游戏宿主的插件配置和应用初始化由 `Engine/Source/Game/Runtime/GameBootstrap.cpp` 完成。项目的 `Source/Game/Game.cpp` 只实现 `HEngine::CreateProjectGameLayer()`，返回项目 Layer；无需复制启动流程。空白模板和示例项目使用同一入口。

## 显式选择项目

```powershell
# 默认使用工作区绑定的项目（本仓库团队默认仍为 Project）
.\hectl.bat game check

# 目录和 descriptor 文件都可以；hectl 会自动隔离反射生成目录
.\hectl.bat editor open -Project D:\Games\MyGame
.\hectl.bat game check -Project D:\Games\MyGame\Project.heproject
.\hectl.bat package -Project D:\Games\MyGame

# 直接配置 CMake 时，codegen 与消费构建必须指向同一 build-local 目录
.\Win-CppCodeGen.bat --no-pause -Project D:\Games\MyGame -GeneratedSourceDir out\my-game\GeneratedSource
cmake -S . -B out\my-game -DHE_PROJECT_DIR=D:\Games\MyGame -DHE_GENERATED_SOURCE_DIR=out\my-game\GeneratedSource
```

开发版 Editor/Game 也接受 `--project=<目录或 Project.heproject>`，用于从移动后的目录启动同一个已构建项目；descriptor 的 `ProjectId`、`DisplayName`、`EngineVersion`、`ExecutableName` 和 `GameConfig` 必须与构建时一致。切换到另一个项目请使用 `hectl ... -Project` 重新配置并构建。不同外部项目使用 `ProjectId + 规范路径哈希` 隔离 CMake cache 和运行产物。

## 内容解析规则

- 相对读取先查当前项目，再回退到 `Engine/Content`，因此旧的 `Assets/...`、`Shaders/...` 和 `Scripts/...` 引用不需要批量改格式。
- 引擎内部可使用 `GetEngineContentFullPath` 强制读取引擎内容。
- 写入必须使用 `GetProjectFullPath` 或 `GetSavedPath`，不会回写共享的引擎内容。
- 项目可以用同名文件覆盖引擎默认内容；项目之间不会共享可写目录。引擎自有 Shader 源码/include 是保留逻辑路径，项目不可同名覆盖。
- 渲染设置是例外：`Engine/Content/Config/DefaultEngine.json` 是只读产品默认，项目不能用同名文件覆盖；`Config/BaseEngine.json` 只保存相对默认值的稀疏项目 override，`Saved/Config/UserEngine.json` 只属于本机用户且不进入源码控制或 Shipping 内容。
- Engine Shader 源码只从 `Engine/Content/Shaders` 读取；DXIL/metadata 写入当前 CMake build 的 `GeneratedContent/Engine/Shaders`，不会修改 Engine checkout，也不会被多个项目共享。项目 Shader 可引用引擎公共 include，并在独立生成目录产生 cache；编译器会拒绝项目/引擎之间的 base、variant 或 profile 产物名冲突。VS/PS/CS 默认使用 SM6.0，`#pragma hprofile <stage> <profile> [BASE|<variant>]` 才能按 stage/variant 提升 profile；非默认 profile 会写入独立的 `__<profile>` 产物和 metadata 映射。
- 打包会先安全清空该 package build 的 Shader cache，再从明确的引擎运行时清单和当前项目 Shader 源码完整重建；项目目录里历史遗留的 `Shaders/CACHES` 不会进入包。
- `Engine/Content/Config/EngineCook.json` 由 Engine 维护内建运行时 root/include（默认纹理、字体、primitive、Python 标准库和 Shader cache）；新项目不需要在自己的 `GameConfig` 重复这些条目。
- Effekseer 游戏特效属于各自 Project，不放入 Engine。显式设置 `HE_VFX_VALIDATE_PACKAGES=1` 时，引擎读取当前项目可选的 `Config/EffekseerRegression.json`；清单只列项目相对 `.efkpkg`，最多 64 个，并经过路径、存在性和重复项校验。需要其他清单时可用 `HE_VFX_VALIDATION_MANIFEST` 指定项目相对路径。

## 打包边界

`PackageGame` 不再把整个项目目录交给 HCooker。它先在当前项目专属的 build 目录生成一次性 overlay：

1. 复制 `Engine/Content` 中的 `Assets`、`Config`、`Scripts`、`Shaders`。
2. 叠加当前 build 的 `GeneratedContent/Engine`，加入已验证的 Shader 产物。
3. 再叠加当前项目的同名四个目录，项目内容优先；配置阶段会拒绝项目与保留的 Engine Shader/include 路径冲突，并始终重新发布 Engine 权威的 `Config/DefaultEngine.json`。
4. 明确排除 `SourceAssets`、`Saved`、`Dist`、工具、源码以及任何 `Config/UserEngine.json`。
5. 继续使用 HCooker 的依赖闭包、路径校验、manifest/hash 和单 `Game.pak` 格式；同时扫描原 Project、Engine/Content 和 GeneratedContent 绝对路径泄漏。
6. 用 descriptor 中的 `ProjectId`、`DisplayName` 和 `ExecutableName` 生成包目录、EXE、所有权 marker 与用户存档目录。

项目运行时 Python 入口必须显式列入 `Cook.RootFiles`，或放在专用运行时子目录并通过 `Cook.IncludeDirectories` 加入。入口静态导入的项目模块会进入依赖闭包，包括从 `Scripts` 根解析的绝对包导入（如 `from HEngineUI.layout import ...`）以及包内 leading-dot 相对导入（如 `from .layout import ...`）；越过 `Scripts` 根的相对导入会使 cook 失败。`importlib.import_module`、`__import__` 等动态导入无法由静态扫描可靠推断，目标模块必须显式加入 `RootFiles` 或专用运行时 `IncludeDirectories`。扫描不会把整个 `Scripts` 目录自动装包，编辑器工具、生成器、测试脚本和任何含凭据的文件都不得放入运行时 allow-list。

默认项目输出位于仓库根 `dist/<ExecutableName>`。外部项目输出位于
`dist/projects/<project-path-key>/<ExecutableName>`，路径键由 `ProjectId` 和规范项目
根共同生成，避免同名外部项目互相覆盖。`Project/Dist` 仅作为历史目录继续被 cook
排除，不再是发布目标。发布前仍执行根目录 allow-list、许可证集合、marker 校验和
原子替换；marker 不匹配的目录不会被删除。

`PackageGame` 在发布 staging 前还会以 `--package-check --warp` 真正执行一次发行版：
完成 pak 解包、isolated CPython、默认场景、RmlUi、DX12/NVRHI 和首个 gameplay frame，
再有界退出。该检查会清空 `PATH` 中的开发工具路径并移除 `PYTHONHOME/PYTHONPATH`，
因此 probe 或静态 DLL 扫描不能再掩盖遗漏的 Python/内容依赖。普通启动日志写入
`%LOCALAPPDATA%/HEngine/Projects/<ProjectId>/Saved/Logs/HEngine.log`。

Windows 包固定使用 Microsoft 官方稳定版
`Microsoft.Direct3D.D3D12` NuGet 1.619.5（`D3D12SDKVersion=619`），开发头文件与
`D3D12Core.dll` 来自同一个经 SHA-512 固定的包，不再单独使用 preview
DirectX-Headers。Game 和 Editor 主 EXE 都导出 `D3D12SDKVersion` 与
`D3D12SDKPath=.\\D3D12\\`；发布结构为：

```text
<ExecutableName>.exe
D3D12/
  D3D12Core.dll
```

Release allow-list 明确禁止 `D3D12SDKLayers.dll`。PE 门禁会解析普通与 delay-load
imports，递归验证所有 app-local EXE/DLL 均为 x64、间接依赖可解析，并直接比较主
EXE 与 `D3D12Core.dll` 导出的 SDK version，同时核对 Core 的精确 NuGet file-version
前缀。引擎和游戏继续全局 `/MT`；当前只有官方
Python 3.11 预编译 DLL 需要动态 `vcruntime140.dll`，它从当前选中的 MSVC 工具链的
`VCToolsRedistDir/x64/Microsoft.VC143.CRT` 复制，绝不从 `System32` 获取。若未来
Python 不再导入该 CRT，闭包门禁会把多余 DLL 判为失败并要求移除。

Agility SDK 619 的必要 headers、x64 runtime 与许可证固定在
`Engine/Source/ThirdParty/DirectXAgilitySDK/1.619.5`，因此 VS、Agent、codegen、Game、
Editor、test 和 package 的 configure 都不联网，也不会在不同 build 目录重复下载。
配置仍会核对 NuGet version、固定 Core SHA-256 和 header 中的
`D3D12_SDK_VERSION`。升级验证时可设置 CMake cache
`HE_D3D12_AGILITY_SDK_ROOT`；兼容性环境变量
`HE_D3D12_AGILITY_SDK_SOURCE_DIR` 仅在首次配置时为该 cache 提供默认值。两者都应
指向带 `build/native` 的官方解包根目录。

## 新建项目最低要求

项目管理器和 `hectl project create -Template` 支持 `Blank`、`Scripted3D`、`SharedArena`、`FirstPerson`、`TopDown`。用 `hectl project templates -Json` 查看可选模板。射击与俯视角模板共用引擎 Starter Content，详见 [StarterContent.md](StarterContent.md)。

优先使用 `hectl project create`，无需手工完成下列步骤。模板位于 `Engine/Templates/Blank`，不含 Survivor 资源、脚本或代码；游戏入口使用引擎 `RuntimeGameLayer`。项目 C++ 可以通过 `Source/Game/GameLayer/CMakeLists.txt` 中的 `GAME_ADD_SOURCE` 添加。项目创建只接受不存在的目录，失败时保留 `.he-create-*` 暂存目录供检查，不覆盖已有内容。

工具逻辑测试：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-Projects.ps1`。UI 创建、重复创建错误和渲染检查：`powershell -NoProfile -STA -ExecutionPolicy Bypass -File Tools/Agent/ProjectManager.ps1 -SmokeTest -CapturePath out/agent/project-manager.png`。原生集成仍需用选定项目的 `check/capture` 验证。

2026-10-15 Project Manager 更新验证：

- `unit-test` PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-ProjectProcess.ps1`。覆盖中文 stdout/stderr/原生命令、双管道大量输出、中文/空格/单引号参数及非零退出码。
- `static-check` PASS：`powershell -NoProfile -STA -ExecutionPolicy Bypass -File Tools/Agent/ProjectManager.ps1 -LayoutTest -PreviewPage Create -CapturePath out/agent/project-manager-refresh/create-final.png`。两个默认分页均无滚动溢出，窗口图标已加载；另以 `-PreviewPage Open` 检查打开页，已顺序查看实际渲染图。
- `test-layer` PASS：`powershell -NoProfile -STA -ExecutionPolicy Bypass -File Tools/Agent/ProjectManager.ps1 -SmokeTest`，验证模板选择、创建后的打开请求、重复创建拒绝与仅取消自有进程。
- `build-only` PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-ProjectManager.ps1 -Output C:/WorkSpace/HEngineSDKLab/free-ci-20261015/HEngine-1.1.0-Windows-x64/ProjectManager.exe -Architecture x64`；同入口以 `-Architecture arm64 -Output out/agent/project-manager-refresh/arm64/ProjectManager.exe` 编译 AnyCPU 启动器。已从 x64 EXE 提取并检查实际图标；AnyCPU 编译不等于 ARM64 设备运行。
- `test-layer` PASS：对上述本地 SDK 验证副本，运行 `powershell -NoProfile -STA -ExecutionPolicy Bypass -File <SDK>/Tools/Agent/ProjectManager.ps1 -SmokeTest -CreateLaunchTest`，再增加 `-CreateLaunchTemplate SharedArena` 复验 C# 工程。两次均由创建按钮启动真实 Editor，完成 WARP 首帧/窗口就绪握手、有界退出、保留窗口与自动关闭检查；SharedArena 实际脚本编译日志中文正常。这些模式只操作自有进程，最近项目列表在 finally 中恢复。
- `static-check` PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-Distribution.ps1 -Engine <SDK> -Json`，126 项布局检查；`unit-test` PASS：`py -3.11 Tools/Distribution/test_distribution.py`，17 项。

2026-10-16 已将新版管理器更新到 [v1.1.0 Windows x64 SDK](https://github.com/hebohang/HEngineDev/releases/tag/v1.1.0)。此次补丁以原发行 ZIP 为基线，保留原生引擎、脚本 API 和导出模板：11,793 个原文件逐个 SHA-256 比对未变，仅更新 6 个管理器相关文件并新增 4 个辅助文件。正式候选包重新通过 124 项布局检查、215 个 PE / 401 项导入闭包、615 个 PE 架构检查，以及空白和 SharedArena C# 项目的创建、编译、就绪和关闭检查；归档 CRC 和上传后的服务端哈希一致。

当前 ZIP 为 550,175,352 bytes、11,803 files，SHA-256 为 `fca5c1c51ac54ee4b8152bbada10cbb948ed702e975f422c8253efc065c5fde3`。附件仅保留 SDK ZIP 与 GitHub 自动源码归档，完整更新来源和命令在 SDK 的 `Docs/Release/RELEASE_PROVENANCE.json`。`v1.1.0` 标签仍对应原生引擎基线 `745f6b61`，管理器补丁源码为 `324653a14`；两个自动源码归档未改变。此次发布未触发 Actions 或使用付费资源。

- 复制并修改 `Project.heproject`，为项目分配唯一且稳定的 `ProjectId`。
- 提供带当前 RenderSettings Schema/SchemaVersion 的稀疏 `Config/BaseEngine.json`，以及 `Config/BaseEditor.json`、`Config/Game.json` 和 `Config/Plugins.json`；不要复制或修改 Engine 的 `Config/DefaultEngine.json`，也不要把 `Saved/Config/UserEngine.json` 提交到项目。
- 提供 descriptor 指向的 `Config/Package.json`；其中声明项目 notices 和项目自有许可证，Engine 的第三方许可证由 Engine 统一加入。
- 提供 `Assets`；`Scripts`、`Shaders` 没有项目内容时可省略。提供 `Source/Game/Game.cpp`、`Source/Game/GameLayer`。
- Windows manifest 放入 `Resources/Windows/Game.rc`、`Game.manifest`；品牌图标 `Game.ico` 可按项目添加。
- 项目许可证说明放入 `Legal/THIRD_PARTY_NOTICES.txt`。
- 不复制 `Engine/Content` 中的默认字体、primitive、脚本库或内建 Shader。

