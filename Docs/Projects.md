# HEngine 多项目与内容边界

HEngine 由一个只读的引擎内容根和一个可写的项目根组成。项目不需要位于引擎仓库内；仓库中的 `Project` 是以 `Physics3DTest.he` 为入口的最小展示项目。

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
  Project/                   # 示例项目；外部项目使用相同结构
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
  "ProjectId": "hengine-showcase",
  "DisplayName": "HEngine Showcase",
  "EngineVersion": "1.0.0",
  "GameConfig": "Config/Game.json",
  "PackageConfig": "Config/Package.json",
  "ExecutableName": "HEngineDemo"
}
```

`ProjectId` 必须稳定，用于包所有权和用户存档隔离；`ExecutableName` 是安全的 Windows 文件名，不包含 `.exe`。`LegacySavedDirectory` 可省略，仅用于已有产品继续读取旧 `%LOCALAPPDATA%/HEngine/<name>/Saved`。

## 选择项目

```powershell
# 默认使用仓库内的 Project
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
Python 3.11 预编译 DLL 需要动态 `vcruntime140.dll`，它从当前 v143 工具链的
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

- 复制并修改 `Project.heproject`，为项目分配唯一且稳定的 `ProjectId`。
- 提供带当前 RenderSettings Schema/SchemaVersion 的稀疏 `Config/BaseEngine.json`，以及 `Config/BaseEditor.json`、`Config/Game.json` 和 `Config/Plugins.json`；不要复制或修改 Engine 的 `Config/DefaultEngine.json`，也不要把 `Saved/Config/UserEngine.json` 提交到项目。
- 提供 descriptor 指向的 `Config/Package.json`；其中声明项目 notices 和项目自有许可证，Engine 的第三方许可证由 Engine 统一加入。
- 提供 `Assets`；`Scripts`、`Shaders` 没有项目内容时可省略。提供 `Source/Game/Game.cpp`、`Source/Game/GameLayer`。
- Windows 品牌资源放入 `Resources/Windows/Game.rc`、`Game.manifest`、`Game.ico`。
- 项目许可证说明放入 `Legal/THIRD_PARTY_NOTICES.txt`。
- 不复制 `Engine/Content` 中的默认字体、primitive、脚本库或内建 Shader。
