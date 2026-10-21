# HEngine 1.1.0

公开下载：[HEngine 1.1.0](https://github.com/hebohang/HEngine/releases/tag/v1.1.0)。附件、源码快照与二进制来源差异见 [公开发布记录](Docs/Release/1.1.0-publication.md)。平台工作流见 [README](README.md#支持范围)，包括 macOS Apple Silicon 的源码开发与打包。

1.1.0 将引擎发行包与示例工程分开：Windows 用户解压引擎后，通过 `ProjectManager.exe` 创建工程，或导入 [小树保卫者独立工程](https://github.com/hebohang/StarbloomSurvivor)。引擎 `main` 不包含游戏 `Project/`，默认使用 Blank 模板。

## 本次变化

- 通用 Editor 从所选工程读取身份、场景、资源和 C# 程序集；原生 C++ 扩展仍使用源码构建。
- 项目管理器提供新建、导入、最近项目、启动就绪检查和错误恢复。
- 小树保卫者保留 Python 与 C# 两套实现，共用运行资源。Python 使用 `MainMenu.he` 和 `MainGame.he`；C# 使用含菜单的 `MainGameCSharp.he`。
- 运行资源从约 2148 MiB 缩减为约 40 MiB，原始素材、RenderBench 和其他测试归入开发仓库的 `Examples/RenderingTests`，不随本次精简公开源码分发。许可证保留在独立工程中。
- 修复独立工程的 C# 引擎引用与 Web 内容导出路径；提供预编译 C# 示例和随包 .NET/Python 运行时。
- 修复编辑器首次布局、运行时场景切换；F6 运行、F8 停止，并恢复原编辑场景。
- 提供独立项目导出与 Windows Editor 发行包构建脚本。
- 二进制 SDK 提供 Windows Shipping / Development Player 和 Android arm64 预编译模板，以及 Web/微信 Wasm 导出工具；SharedArena 模板包含四端配置。
- 发行依赖检查覆盖动态加载的 FSR、OIDN、CRT、许可证和完整依赖闭包。
- 改善方向光级联阴影的稳定性、级联交界与偏移；可播放的动画入口按条件执行状态转换。
- 改善导航几何顶点复用，并扩展 C# 的碰撞体启停、射线排除和足部接触权重接口。
- 开发模板排除历史图形诊断程序，保证发行文件清单与实际 SDK 内容一致。

## 使用和构建

Windows x64 为本次原生验收平台；默认 DirectX 12，也包含 Vulkan 1.3 后端。所需图形能力与降级策略见 [Windows 图形兼容性](Docs/WindowsGraphicsCompatibility.md)。本轮游戏交互验收使用 DX12，不代表所有显卡或驱动均已实测。

普通用户打开预编译示例不需要 VS 或系统 .NET。修改 C# 需要 .NET SDK 10.0.302；小树保卫者的内容生成还使用 Python 3.11 和 Node.js。Web 导出需要 .NET wasm-tools。

Windows 与 Android 脚本游戏导出复用 SDK 的预编译原生模板，无需 VS 或重新编译引擎；仍需编译游戏 C#、cook 资源，以及平台组装和签名工具。SDK 锁定依赖由 `hectl deps check -Profile SDK` 检查。源码构建优先 VS2026 v145（可回退 VS2022 v143）、CMake 4.2+；原生 C++ 扩展需要匹配模板，不提供跨版本稳定二进制 ABI。

本次使用可解压发行包；历史 `Installer.nsi` 不作为 1.1.0 安装入口。

四端导出支持面及实际验收见 [独立 SDK 与四端导出](Docs/ReleaseSDK-1.1.0.md)；较早的记录见 [1.1.0 发布与验收](Docs/Release-1.1.0.md)。四端主路径为 C#，Web/微信使用便携视图适配，不承诺完整原生渲染功能；Android/微信手机及商店审核需要项目发布者另行验收。
