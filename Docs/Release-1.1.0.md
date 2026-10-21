# HEngine 1.1.0 发布与验收

脚本 SDK 的新增设计、双语言系统覆盖与二进制 AI 开发入口见 [ScriptingApiAudit-1.1.0.md](ScriptingApiAudit-1.1.0.md)、[ScriptingGuide.md](ScriptingGuide.md)、[AI_GAME_DEVELOPMENT.md](AI_GAME_DEVELOPMENT.md)。新增接口需要重建 Editor 和 Player 模板，不能沿用下面旧包的通过结论。

以下是既有发行物与历史验收记录。当前四端免引擎编译 SDK 的差距、实施方案及本轮修复见 [ReleaseSDK-1.1.0.md](ReleaseSDK-1.1.0.md)；旧 ZIP 不会随源码修改自动更新，尚不能视为四端 SDK 验收通过。

## 交付位置

最终发行目录是 `dist/Release-1.1.0/`：

- `HEngine-1.1.0-Windows-x64/`：入口 `ProjectManager.exe`；不包含游戏工程。
- `StarbloomSurvivor-1.1.0/`：独立仓库内容，导入 `Project.heproject`，包含预编译 C#。
- 对应 ZIP、两套 Windows 游戏 ZIP、C# Web ZIP，以及 `SHA256SUMS.txt`、`artifacts.json`。

Windows 游戏原始输出在 `Project/Dist/Windows`（C#）与 `Project/Dist/WindowsPython`（Python）；Web 在 `Project/Dist/Web`，需 HTTP 服务。渲染测试工程是 `Examples/RenderingTests/Project.heproject`。

本机另外保留 `C:/WorkSpace/HEngineProjects/StarbloomSurvivor` 作为独立路径验收工程。路径不写入共享项目身份；ZIP 排除 Saved、构建缓存、本机绑定、日志和个人布局。没有创建或推送远程仓库。

## 用户入口

解压引擎，运行 `ProjectManager.exe`，可以新建空白工程或导入独立工程。默认打开 C# 场景 `Assets/Scenes/MainGameCSharp.he`，包含完整菜单。Python 从 `Assets/Scenes/MainMenu.he` 启动，并切换到 `MainGame.he`。F6 播放，F8 停止并恢复原编辑场景。

预编译示例无需安装 VS 或系统 .NET；引擎附带 Python 与 .NET 运行时。修改 C# 需要 .NET SDK 10.0.302；此游戏的内容生成还使用 Python 3.11、Node.js 和 CMake。成功编译的代际保存在项目 Saved 中，重启后继续加载。Web 导出另需 wasm-tools。

**Windows Shipping 导出仍需源码版引擎和 VS/CMake；本版没有免原生工具链的 Windows 导出模板。** 二进制 Editor 可以新建、编辑和播放工程；原生 C++ 扩展也需要源码构建。原生验收平台为 Windows x64、DX12，不承诺所有操作系统、显卡或驱动均已实测。

## 重建命令

在源码引擎根目录执行，输出目录必须是新的；已有产物和失败 staging 不会被盲目覆盖。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Build-Editor.ps1 -Output dist/Release-1.1.0/HEngine-1.1.0-Windows-x64
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Export-Project.ps1 -Project Project -Output dist/Release-1.1.0/StarbloomSurvivor-1.1.0 -PrecompileCSharp
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile windows
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile windows-python
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile web
py -3.11 Tools/Distribution/Archive-Release.py --release dist/Release-1.1.0 --game Project
```

源码工具链为 VS2026 v145（可回退 VS2022 v143）、CMake 4.2+。打包由 Publish 调用既有 Package/cooker 门禁，验证 PACK_GAME、资源清单、依赖闭包、随包运行时和最小环境启动；开发 Game 不作为 Shipping 证据。

## 资源边界

用两套 Windows cook manifest、动态资产目录、脚本引用与递归依赖闭包筛选运行资源，并保留 Web 所需标题背景。游戏 Assets 从约 2148 MiB 缩减为约 42 MiB，保留三个游戏场景；Python 与 C# 源码、共享 UI、Web 导出代码和必要许可证均保留。

渲染、物理、导航、VFX 测试及约 11520 MiB 原始素材归入 `Examples/RenderingTests`；其共享依赖独立保存，游戏不依赖该目录。来源素材保留供重新导入，不进入引擎/游戏发行 ZIP。基准场景入口见该工程 README。两份历史轨迹清单的字节哈希与已跟踪文件不一致，已在确定性相机合同通过后由工具刷新为 1.1.0 输入基线；轨迹参数未改，新哈希不得混用旧性能证据。对应 .gitattributes 已迁移。

## 验收结果

以下均为本机已执行结果；精确交互启动参数和 EXE SHA256 另存于 `out/agent/release-1.1/*/owned-process.json`。

| 证据类型 | 精确命令或交互 | 结果 |
| --- | --- | --- |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-Projects.ps1` | PASS，新建、项目选择、版本和缓存隔离 |
| unit-test | `py -3.11 Tools/Build/TestSharedContent.py` | PASS，6 个用例，Windows 替代场景不绕过 Web 限制 |
| static-check | `py -3.11 Tools/Build/SharedContent.py Project` | PASS |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineEditor -Configuration RelWithDebInfo -Project Engine/Templates/Blank -PortableEditor -PluginConfig Tools/Distribution/Plugins.json` | PASS，最终通用 Editor |
| build-only | 上述三条 `Tools/Build/Publish.ps1` 命令，分别选择 windows、windows-python、web | PASS；PC Shipping 门禁与最小环境启动通过 |
| unit-test（浏览器集成） | `$env:NODE_PATH='C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules'; & 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Scripting/TestSurvivorWeb.cjs` | PASS，C# Web |
| runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Run-InteractiveCheck.ps1 -Executable Project/Dist/WindowsPython/StarbloomSurvivor.exe -Packaged -Evidence out/agent/release-1.1/pc-python` | PASS，菜单、战斗、升级选择 |
| runtime-manual | 同一工具，`-Executable Project/Dist/Windows/StarbloomSurvivor.exe -Packaged -Evidence out/agent/release-1.1/pc-csharp-final` | PASS，最终 C# PC 包菜单、战斗、升级 |
| runtime-manual | 同一工具，`-Executable dist/HEngine-1.1.0-Windows-x64/HEngineEditor.exe -Project C:/WorkSpace/HEngineProjects/StarbloomSurvivor -Scene Assets/Scenes/MainMenu.he -Evidence out/agent/release-1.1/editor-python-recheck` | PASS，菜单切换、战斗、升级、F8 恢复编辑、F6 再次播放，退出 0 |
| runtime-manual | 同一工具，`-Executable dist/Release-1.1.0/HEngine-1.1.0-Windows-x64/HEngineEditor.exe -Project dist/Release-1.1.0/StarbloomSurvivor-1.1.0 -Scene Assets/Scenes/MainGameCSharp.he -Evidence out/agent/release-1.1/editor-csharp-final` | PASS，干净预编译工程、菜单、战斗、F8，退出 0 |
| build-only + runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File dist/HEngine-1.1.0-Windows-x64/Tools/Scripting/CompileEditor.ps1 -Project C:/WorkSpace/HEngineProjects/StarbloomSurvivor`，随后重启 Editor 核对程序集哈希 | PASS，项目本地编译缓存可跨重启加载 |
| runtime-manual | `powershell -NoProfile -STA -ExecutionPolicy Bypass -File dist/Release-1.1.0/HEngine-1.1.0-Windows-x64/Tools/Agent/ProjectManager.ps1 -SmokeTest -Project dist/Release-1.1.0/StarbloomSurvivor-1.1.0 -CapturePath out/agent/release-1.1/project-manager-final.png` | PASS，新建、重复创建错误、导入、就绪握手、失败恢复和自动关闭 |
| runtime-manual | 实际 ProjectManager.exe 界面创建 `C:/WorkSpace/HEngineProjects/Blank 1.1 Acceptance`，点击打开 Editor | PASS，中文名称、空格路径，无需 C++ 编译 |
| codegen + build-only + runtime-manual | `.\hectl.bat editor check -Project Examples/RenderingTests -Frames 60 -TimeoutSeconds 180` | codegen PASS（273 个生成文件）；build PASS；运行退出 0 |
| runtime-manual | `.\hectl.bat editor capture -Project Examples/RenderingTests -StartupScene Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he -Frames 30 -Resolution 1280x720 -TimeoutSeconds 180 -CapturePath out/agent/release-1.1/rendering-sponza.png` | PASS，已检查渲染截图 |
| unit-test | `py -3.11 Tools/Assets/RenderingBenchmarks/verify_benchmark_camera.py --refresh-manifests` | PASS，两条保留轨迹、确定性重复、闭合周期、非法时间步 |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Benchmark-Rendering.ps1 -StartupScene Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he -BenchmarkInputPath Examples/RenderingTests/Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/benchmark_input.json -RenderSettingsPath Examples/RenderingTests/Assets/RenderingBenchmarks/Configurations/Native4KComparable.json -OutputDirectory out/agent/release-1.1/benchmark-prepare-final -PrepareOnly` | PASS，仅验证输入与隔离命令，不是性能测量 |

日志位于 `out/agent/logs/release-1.1-*.log`；交互运行日志位于 `out/agent/release-1.1/`。Web 用例覆盖移动、镜头、暂停、升级、结算存档、重载、再次开局和触屏；区域/Boss 胜利与多武器伤害使用明确的测试 fixture。PC 为目视交互验收，不写成整局自动通关或全硬件认证。按用户范围未验证 Python Web。
