# HEngine 1.0.0

发布日期：2026-09-03

HEngine 1.0.0 是项目的首个正式公开版本。本次发布聚焦一套可从源码复现的 Windows x64 引擎工作流：使用 HEngineEditor 创建内容，以 HGame 运行项目，并通过统一工具完成代码生成、构建、运行检查、Cook 与打包验证。

## 发布内容

- HEngine Runtime 与 HEngineEditor 源码。
- 面向项目的 HGame 开发构建和 Release 打包流程。
- 轻量的 HEngine Showcase，默认场景为 `Assets/Scenes/Physics3DTest.he`。
- Windows 构建、无窗口逻辑/序列化测试、确定性抓帧和发布包检查入口。
- 引擎内建内容、项目内容覆盖规则及相应的第三方许可证集合。

## 核心能力

- **编辑器**：场景层级、属性检查器、内容浏览器、资源预览，以及材质、动画控制器和运行时 UI 工具。
- **渲染**：基于 RenderGraph 与 NVRHI 的统一渲染路径；同一套 Game/Editor 支持 DirectX 12 与 Vulkan 1.3，并默认使用 DX12。
- **材质与动画**：PBR、Shader 驱动材质、骨骼动画与动画状态机。
- **3D 物理**：基于 Jolt Physics 的刚体、常用碰撞体、角色移动、空间查询与调试绘制。
- **脚本与反射**：原生 C++ Game 模块、Python 运行时脚本、构建图内反射代码生成和资源序列化。
- **资源管线**：Scene、Prefab、Mesh、Material、Texture、Audio 与 UI 资源管理，以及 glTF/Mesh 导入流程。
- **运行时集成**：RmlUi、Effekseer 和引擎音频抽象具有统一的启动、更新与清理边界。
- **Cook 与 Package**：依赖闭包、路径约束、manifest/hash、单 `Game.pak` 与真实启动检查共同组成发布验证流程。

## 默认展示项目

1.0.0 将仓库内 `Project/` 定位为通用的 **HEngine Showcase**，不再以完整游戏作为默认内容。`Physics3DTest.he` 提供可直接打开、运行和修改的基础场景，用于展示 3D 刚体、碰撞、光照、材质和资源序列化；通用 Python、运行时 UI 与自定义 Shader 示例作为可选起点一并保留。

展示项目只承担最小示例和开发验证职责。实际游戏可以放在仓库外，并通过 `hectl ... -Project <path>` 选择；不同项目拥有隔离的构建缓存、生成代码、运行产物和存档目录。

## 支持范围

| 项目 | 1.0.0 支持状态 |
| --- | --- |
| 操作系统 | Windows 10 1909（18363.1350+）或 Windows 11 x64 |
| 工具链 | Visual Studio 2022 v143、CMake 3.20+、C++20 |
| 图形后端 | DirectX 12（默认）与 Vulkan 1.3 |
| 开发产物 | HEngineEditor 与读取 loose project 内容的 HGame |
| 发布产物 | `PACK_GAME=ON` 生成并验证的 Cooked Game 包 |
| SDK / ABI | 源码优先；不承诺跨编译器或跨版本稳定二进制 ABI |

Vulkan 运行需要显卡驱动提供 Vulkan 1.3 loader。DX12/Vulkan 的设备选择、能力降级和 Shader 产物规则见 `Docs/WindowsGraphicsCompatibility.md`。

## 构建与发布验证

```powershell
./hectl.bat codegen run
./hectl.bat game check
./hectl.bat editor check
./hectl.bat game capture
./hectl.bat package
```

- `codegen run` 只在生成内容变化时发布新的反射产物。
- `game check` 与 `editor check` 是会自动回收进程的有界运行检查。
- `game capture` 用于生成确定性的运行画面，属于运行时观察证据。
- `package` 执行 Release 构建、Cook、manifest/pak 校验和打包后启动检查。

构建、运行观察和自动测试属于不同证据类型；发布记录应分别报告其命令与结果。

## 已知边界

- 1.0.0 的正式支持平台是 Windows x64，尚未提供其他桌面或移动平台发布承诺。
- Vulkan 由系统显卡驱动提供 loader；发布包不会携带 Vulkan SDK、验证层或开发工具。
- HEngineRuntime 当前作为源码级静态库参与同一工具链构建，不是可独立分发的稳定二进制 SDK。
- 可选的高级图形能力会按设备能力启用或降级；基础光栅路径不要求硬件光线追踪。
- `Installer.nsi` 是旧式开发辅助脚本，不属于 1.0.0 的发布或安装验证证据。

更多使用方式与边界说明见根目录中英文 README 及 `Docs/`。
