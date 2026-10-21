<p align="right"><strong>简体中文</strong> · <a href="README_EN.md">English</a></p>

<p align="center">
  <img src="Engine/Resources/Icons/EngineIcon.png" alt="HEngine Logo" width="128" />
</p>

<h1 align="center">HEngine</h1>

<p align="center">现代 C++20 游戏引擎：可视化编辑、C# 游戏开发，以及 Windows、macOS、Android、Web 和微信小游戏工作流。</p>

<p align="center">
  <a href="RELEASE_NOTES.md"><img alt="Version 1.1.0" src="https://img.shields.io/badge/version-1.1.0-4c8bf5" /></a>
  <img alt="Windows, macOS, Android, Web, WeChat" src="https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Android%20%7C%20Web%20%7C%20WeChat-0078d4" />
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599c" />
  <img alt="Renderer DX12 and Vulkan 1.3" src="https://img.shields.io/badge/renderer-DX12%20%7C%20Vulkan%201.3-6f42c1" />
  <a href="LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/license-MIT-blue" /></a>
</p>

<p align="center">
  <a href="#核心能力">核心能力</a> ·
  <a href="#快速开始">快速开始</a> ·
  <a href="#示例与项目模板">示例与模板</a> ·
  <a href="#文档">文档</a> ·
  <a href="RELEASE_NOTES.md">1.1.0 发布说明</a>
</p>

<p align="center">
  <img src="Docs/Images/README/material-showcase.png" alt="HEngine 材质展示场景" width="960" />
</p>

HEngine 是一个开源 C++20 游戏引擎，提供场景编辑、资源导入、C# / Python 脚本、原生 C++ 扩展、渲染、物理与游戏发布工作流。

**1.1.0** 提供独立的 Windows x64 Editor SDK、项目管理器和预编译游戏模板。引擎与示例游戏分开维护：默认使用 Blank 模板，[小树保卫者（StarbloomSurvivor）](https://github.com/hebohang/StarbloomSurvivor) 可独立下载导入。

**[下载 HEngine 1.1.0](https://github.com/hebohang/HEngine/releases/tag/v1.1.0)** · [平台支持](#支持范围) · [构建与发布](Docs/BuildAndPublish.md)

## 核心能力

- **可视化编辑器**：场景层级、属性检查器、内容浏览器、资源预览，以及材质、动画控制器和运行时 UI 编辑工具。
- **现代渲染架构**：`RenderWorld -> SceneRenderer -> RenderGraph -> NVRHI`，同一套运行时支持 DirectX 12 与 Vulkan 1.3。
- **材质与动画**：PBR 材质、自定义 Surface Shader、材质热重载、骨骼动画和动画状态机。
- **3D 物理**：基于 Jolt Physics 的刚体、常用碰撞体、角色移动、射线/形状查询与编辑器调试绘制。
- **脚本与玩法层**：C# 游戏 API、Python 编辑器脚本与原生 C++ 扩展，配套反射、序列化和 AI 开发文档；跨端导出以 C# 为主。
- **资源与内容管线**：Scene、Prefab、Mesh、Material、Texture、Audio 与 UI 资源管理，支持 glTF 导入和项目级内容覆盖。
- **UI、特效与音频**：集成 RmlUi、Effekseer 和引擎音频抽象，并统一接入运行时生命周期。
- **构建与发布**：`hectl` 提供隔离的 Game、Editor、测试、抓帧和 Package 工作流；Cooker 会校验依赖闭包、manifest、hash 与 `Game.pak`。

## 快速开始

### 使用预编译 SDK（Windows x64）

1. 从 [1.1.0 Release](https://github.com/hebohang/HEngine/releases/tag/v1.1.0) 下载 `HEngine-1.1.0-Windows-x64.zip` 并解压。
2. 运行 `ProjectManager.exe`，新建工程，或导入独立项目的 `Project.heproject`。
3. 在编辑器中用 **F6** 游玩、**F8** 停止；通过 **构建 → 构建与发布** 选择目标平台。

打开预编译示例无需安装 Visual Studio 或系统 .NET。修改 C#、导出游戏仍需相应工具；SDK 的 Windows / Android 模板避免重新编译原生引擎，Web / 微信需要 Wasm 工具链。依赖检查入口是 `hectl deps check -Profile SDK`，详见 [SDK 使用与导出范围](Docs/ReleaseSDK-1.1.0.md)。

### 从源码构建（Windows）

安装 Visual Studio / Build Tools 2026 的 C++ 桌面开发组件（v145；也支持 VS2022 v143）、Windows SDK；构建使用 C++20 和 CMake 4.2+。随后运行：

```powershell
git clone https://github.com/hebohang/HEngine.git
Set-Location HEngine
.\Win-InstallDependencies.bat -NoPause
.\hectl.bat editor check -Project Engine/Templates/Blank
```

依赖脚本准备锁定工具，不安装 Visual Studio。`editor check` 构建后执行有界启动检查。日常打开编辑器使用 `Win-RunEditor.bat`；通过 `-Project D:/Games/MyGame` 选择独立项目。[macOS 源码工作流](Tools/macOS/README.md) 使用单独的工具链与入口。

### 常用命令

| 命令 | 用途 |
| --- | --- |
| `.\hectl.bat project current -Json` | 查看当前项目和输出目录 |
| `.\hectl.bat editor check` | 构建并执行有界 Editor 启动检查 |
| `.\hectl.bat game check` | 构建并执行有界 Game 运行检查 |
| `.\hectl.bat publish check -BuildProfile web` | 检查项目 Web 导出配置与依赖 |
| `.\hectl.bat publish build -BuildProfile web` | 按项目配置导出 Web；其他配置见发布文档 |

反射代码生成已接入构建图，正常构建会按需生成后再编译。原生 C++ 扩展需要源码构建及匹配模板，不承诺跨版本稳定二进制 ABI。

## 示例与项目模板

- [Blank](Engine/Templates/Blank)：默认空白工程，适合从头开发。
- [SharedArena](Engine/Templates/SharedArena)：共享 C# 游戏规则的 Windows、Android、Web、微信四端入门模板。
- [小树保卫者](https://github.com/hebohang/StarbloomSurvivor)：独立游戏工程；`main` 为 C# 四端版本，`python` 分支用于 Python 编辑器玩法。
- 本公开源码包含完整引擎、开发工具和项目模板；大型 `RenderingTests` 测试工程与素材保留在开发仓库，不随本次精简源码分发。

项目管理器支持新建、导入和最近项目。项目边界与目录约定见 [项目文档](Docs/Projects.md)。

## 支持范围

平台工作流与本次提供的二进制下载分开列出；各平台的图形能力和工具要求不同。

| 平台 | 当前工作流 / 图形路径 | 1.1.0 交付与边界 |
| --- | --- | --- |
| **Windows x64** | Editor、游戏开发和 Shipping 导出；DX12 / Vulkan 1.3 | 提供预编译 Editor SDK、Windows Player 和项目管理器；本版主要二进制验收平台 |
| **macOS（Apple Silicon / ARM64）** | 原生 Editor、Game 和 `.app` 打包；Vulkan → MoltenVK → Metal | 提供源码工作流，已有 M4 Pro 实机编辑及游戏验收；本次不提供 Mac SDK 下载，正式分发需自行签名、公证 |
| **Android ARM64** | C# 游戏、原生 Vulkan 1.3、APK 导出 | Windows SDK 包含预编译原生模板；需 Android 工具链和项目签名，具体 SDK 与真机验证范围见文档 |
| **Web** | C# / .NET Wasm、WebGL 2，浏览器游戏 | SDK 包含导出工具；需 Wasm 工具链、项目适配和 HTTP 服务 |
| **微信小游戏（WeChat / weixin）** | C# / .NET Wasm、微信 WebGL 2、资源分包 | SDK 包含导出工具；需项目 AppID、微信开发者工具及上线前设备验证 |

Web / 微信使用便携渲染与宿主适配，不等同于原生端全部渲染、物理或插件能力；SharedArena 的共享规则也不代表任意游戏无需适配即可跨端运行。Android / 微信以本次 SDK 的导出验收为准，历史真机记录不替代最终项目验收。

**Windows ARM64** 的原生构建与验证流程已接入，尚无本次正式 SDK 附件；不把 Android ARM64 或 macOS ARM64 支持当作 Windows ARM64 支持。当前发布范围不含 Linux 和 iOS。

Windows Game / Editor 可用 `--graphics-api=dx12|vulkan` 选择图形后端，默认 DX12。硬件与系统基线见 [Windows 图形兼容性](Docs/WindowsGraphicsCompatibility.md)；跨端配置见 [构建与发布](Docs/BuildAndPublish.md)。

## 文档

| 主题 | 文档 |
| --- | --- |
| 1.1.0 发布内容与边界 | [RELEASE_NOTES.md](RELEASE_NOTES.md) |
| 独立 SDK 与四端导出 | [Docs/ReleaseSDK-1.1.0.md](Docs/ReleaseSDK-1.1.0.md) |
| macOS 开发与打包 | [Tools/macOS/README.md](Tools/macOS/README.md) |
| Android 工具链与签名 | [Docs/Android.md](Docs/Android.md) |
| Web / 微信与共享内容 | [Docs/BuildAndPublish.md](Docs/BuildAndPublish.md) |
| C# / Python 脚本与 AI 开发 | [脚本指南](Docs/ScriptingGuide.md) · [AI 开发](Docs/AI_GAME_DEVELOPMENT.md) |
| 图形后端与硬件能力 | [Docs/WindowsGraphicsCompatibility.md](Docs/WindowsGraphicsCompatibility.md) |
| 多项目与内容边界 | [Docs/Projects.md](Docs/Projects.md) |
| glTF / Mesh 导入 | [Docs/AssetImporting.md](Docs/AssetImporting.md) |
| Shader 驱动材质 | [Docs/ShaderMaterials.md](Docs/ShaderMaterials.md) |
| 运行时 UI 动效 | [Docs/RuntimeUiMotion.md](Docs/RuntimeUiMotion.md) |
| 渲染架构 | [Engine/Source/Runtime/Graphics/RenderingArchitecture.md](Engine/Source/Runtime/Graphics/RenderingArchitecture.md) |
| GM 开发工具 | [Docs/GM.md](Docs/GM.md) |

## 仓库结构

```text
Engine/   引擎源码、内建内容与工具资源
Examples/ 轻量脚本示例（默认项目为 Engine/Templates/Blank）
Docs/     功能与工作流文档
Tools/    构建、验证和内容处理工具
```

## 参与贡献

欢迎提交 Issue 和 Pull Request。请保持改动聚焦，避免无关的大范围重构，并在提交前运行能够覆盖改动的最小构建或测试。仓库开发约定见 [AGENTS.md](AGENTS.md)。

## 许可证与致谢

HEngine 使用 [MIT License](LICENSE)。第三方代码与资源保留各自许可证，相关文本随对应组件或发布包分发。

感谢这些项目和资料对 HEngine 的启发与帮助：[Hazel](https://github.com/TheCherno/Hazel)、[Pilot](https://github.com/BoomingTech/Pilot)、[Godot](https://github.com/godotengine/godot)、[LearnOpenGL](https://github.com/JoeyDeVries/LearnOpenGL)、[LearningDirectX12](https://github.com/jpvanoosten/LearningDirectX12)、[DirectX11-With-Windows-SDK](https://github.com/MKXJun/DirectX11-With-Windows-SDK)、[Adria](https://github.com/mateeeeeee/Adria-DX12)、[Vulkan Guide](https://github.com/vblanco20-1/vulkan-guide)、[MoravaEngine](https://github.com/dtrajko/MoravaEngine) 与 [Ogre](https://github.com/OGRECave/ogre)。
