<p align="right"><strong>简体中文</strong> · <a href="README_EN.md">English</a></p>

<p align="center">
  <img src="Engine/Resources/Icons/EngineIcon.png" alt="HEngine Logo" width="128" />
</p>

<h1 align="center">HEngine</h1>

<p align="center">面向 Windows 的现代 C++20 游戏引擎：从可视化编辑到双后端渲染、物理、脚本与发布。</p>

<p align="center">
  <a href="RELEASE_NOTES.md"><img alt="Version 1.0.0" src="https://img.shields.io/badge/version-1.0.0-4c8bf5" /></a>
  <img alt="Platform Windows x64" src="https://img.shields.io/badge/platform-Windows%20x64-0078d4" />
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599c" />
  <img alt="Renderer DX12 and Vulkan 1.3" src="https://img.shields.io/badge/renderer-DX12%20%7C%20Vulkan%201.3-6f42c1" />
  <a href="LICENSE"><img alt="MIT License" src="https://img.shields.io/badge/license-MIT-blue" /></a>
</p>

<p align="center">
  <a href="#核心能力">核心能力</a> ·
  <a href="#快速开始">快速开始</a> ·
  <a href="#默认展示项目">展示项目</a> ·
  <a href="#文档">文档</a> ·
  <a href="RELEASE_NOTES.md">1.0.0 发布说明</a>
</p>

<p align="center">
  <img src="Docs/Images/README/material-showcase.png" alt="HEngine 材质展示场景" width="960" />
</p>

HEngine 是一个面向 Windows x64 的开源游戏引擎。它提供从场景编辑、资源导入和 C++/Python 玩法开发，到 DX12/Vulkan 渲染、Jolt 3D 物理以及可验证打包的一体化工作流。

**1.0.0** 是 HEngine 的首个正式公开版本。仓库内置轻量的 **HEngine Showcase**，默认打开 `Physics3DTest.he`，用于展示引擎基础能力，而不是绑定某个完整游戏。

## 核心能力

- **可视化编辑器**：场景层级、属性检查器、内容浏览器、资源预览，以及材质、动画控制器和运行时 UI 编辑工具。
- **现代渲染架构**：`RenderWorld -> SceneRenderer -> RenderGraph -> NVRHI`，同一套运行时支持 DirectX 12 与 Vulkan 1.3。
- **材质与动画**：PBR 材质、自定义 Surface Shader、材质热重载、骨骼动画和动画状态机。
- **3D 物理**：基于 Jolt Physics 的刚体、常用碰撞体、角色移动、射线/形状查询与编辑器调试绘制。
- **脚本与玩法层**：原生 C++ Game 模块、Python 运行时脚本、反射代码生成和序列化。
- **资源与内容管线**：Scene、Prefab、Mesh、Material、Texture、Audio 与 UI 资源管理，支持 glTF 导入和项目级内容覆盖。
- **UI、特效与音频**：集成 RmlUi、Effekseer 和引擎音频抽象，并统一接入运行时生命周期。
- **构建与发布**：`hectl` 提供隔离的 Game、Editor、测试、抓帧和 Package 工作流；Cooker 会校验依赖闭包、manifest、hash 与 `Game.pak`。

## 快速开始

### 环境要求

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Windows 10 1909（18363.1350+）或 Windows 11 x64 |
| 编译器 | Visual Studio 2022，v143 C++ 工具链 |
| 语言标准 | C++20 |
| 构建工具 | CMake 3.20 或更高版本 |
| 其他 | Python 3，可由 CMake 发现 |

### 克隆并验证

```powershell
git clone --recurse-submodules https://github.com/hebohang/HEngineDev.git
Set-Location HEngineDev
./hectl.bat doctor
./hectl.bat game check
```

`game check` 会配置并构建开发版 HGame，运行默认展示场景后自动退出。首次构建耗时取决于机器与本地编译缓存。

### 常用命令

| 命令 | 用途 |
| --- | --- |
| `./hectl.bat` | 构建默认的 HGame 开发版 |
| `./hectl.bat game check` | 构建并执行有界 Game 运行检查 |
| `./hectl.bat editor check` | 构建并执行有界 Editor 运行检查 |
| `./hectl.bat game open` | 交互式启动 Game，进程由用户关闭 |
| `./hectl.bat editor open` | 交互式启动 Editor，进程由用户关闭 |
| `./hectl.bat package` | 构建、Cook 并验证 Release 游戏包 |

反射代码生成已经接入构建图，正常构建会在编译消费者之前按需执行。只有需要独立检查时才使用 `hectl codegen status|check|run`。

## 默认展示项目

仓库中的 `Project/` 是 **HEngine Showcase**。其默认场景 [`Physics3DTest.he`](Project/Assets/Scenes/Physics3DTest.he) 提供一个小型、可直接运行和编辑的起点，重点展示：

- 3D 刚体、碰撞体、摩擦与弹性参数；
- Camera、环境光、方向光和点光源；
- Mesh、Material、Scene 序列化与资源加载；
- 可继续扩展的 Python 脚本、运行时 UI 与自定义 Shader 示例资源。

你可以直接修改该项目，也可以通过 `-Project` 指向仓库外的项目目录：

```powershell
./hectl.bat editor open -Project D:/Games/MyGame
```

多项目目录约定和内容覆盖规则见 [Docs/Projects.md](Docs/Projects.md)。

## 支持范围

| 能力 | 1.0.0 状态 |
| --- | --- |
| 平台 | Windows x64 |
| 图形后端 | DirectX 12（默认）与 Vulkan 1.3 |
| 开发构建 | HGame、HEngineEditor、无窗口逻辑/序列化测试 |
| 发布构建 | `PACK_GAME=ON` 的 Cook、manifest、pak 与启动检查 |
| 分发方式 | 源码优先；当前不承诺跨编译器或跨版本稳定二进制 ABI |

Game 与 Editor 在同一二进制中包含 DX12/Vulkan 后端，可通过 `--graphics-api=dx12|vulkan` 选择。Vulkan 需要显卡驱动提供 Vulkan 1.3 loader；完整能力基线与降级策略见 [Windows 图形兼容性说明](Docs/WindowsGraphicsCompatibility.md)。

## 文档

| 主题 | 文档 |
| --- | --- |
| 1.0.0 发布内容与边界 | [RELEASE_NOTES.md](RELEASE_NOTES.md) |
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
Project/  默认 HEngine Showcase 项目
Docs/     功能与工作流文档
Tools/    构建、验证和内容处理工具
```

## 参与贡献

欢迎提交 Issue 和 Pull Request。请保持改动聚焦，避免无关的大范围重构，并在提交前运行能够覆盖改动的最小构建或测试。仓库开发约定见 [AGENTS.md](AGENTS.md)。

## 许可证与致谢

HEngine 使用 [MIT License](LICENSE)。第三方代码与资源保留各自许可证，相关文本随对应组件或发布包分发。

感谢这些项目和资料对 HEngine 的启发与帮助：[Hazel](https://github.com/TheCherno/Hazel)、[Pilot](https://github.com/BoomingTech/Pilot)、[Godot](https://github.com/godotengine/godot)、[LearnOpenGL](https://github.com/JoeyDeVries/LearnOpenGL)、[LearningDirectX12](https://github.com/jpvanoosten/LearningDirectX12)、[DirectX11-With-Windows-SDK](https://github.com/MKXJun/DirectX11-With-Windows-SDK)、[Adria](https://github.com/mateeeeeee/Adria-DX12)、[Vulkan Guide](https://github.com/vblanco20-1/vulkan-guide)、[MoravaEngine](https://github.com/dtrajko/MoravaEngine) 与 [Ogre](https://github.com/OGRECave/ogre)。
