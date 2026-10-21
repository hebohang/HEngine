# macOS 开发与打包

第一阶段支持 Apple Silicon（arm64），图形路径为 Vulkan → MoltenVK → Metal。
复用现有 Vulkan 渲染后端，不增加独立 Metal 渲染后端。
源码工作流需要 Xcode Command Line Tools、CMake、Ninja、Python 3.11+、PowerShell 7，
以及锁定的 .NET SDK 10.0.302 / Runtime 10.0.10。构建工具不会执行 Homebrew 安装或修改系统工具链。
仓库本地依赖位于 `out/agent/dependencies/macos`；所有命令都可传入 `-Project`，接受目录或 `Project.heproject`。

## 准备与复现

先准备以下宿主工具：Apple Command Line Tools（`xcode-select --install`）、CMake 3.24+、Ninja、
PowerShell 7、Python 3.11（包含嵌入开发库）、Node.js 18+、libclang 20+、MoltenVK arm64。
DXC 使用 macOS Vulkan SDK 中的 `macOS/bin/dxc`；本次验证采用 Vulkan SDK 1.4.363.0。
.NET 使用官方 `dotnet-sdk-10.0.302-osx-arm64.tar.gz` 解压目录；不要让系统中更高版本 SDK 替换项目锁定版本。
原生游戏需要的 .NET Runtime 10.0.10 由 `runtime.macos-arm64.lock.json` 固定官方 URL 和 SHA-512，
`PinnedRuntime.py` 校验后缓存，不依赖玩家安装 .NET。

`Setup.py` 只验证工具并保存当前机器的路径到被 Git 忽略的 `settings.json`；不会安装或升级系统软件。
`--install-node-tools` 仅在仓库 `Tools/Scripting` 下执行锁定的 `npm ci`。

```sh
python3 Tools/macOS/Setup.py \
  --dotnet-root /path/to/dotnet-10.0.302 \
  --dxc /path/to/VulkanSDK/1.4.363.0/macOS/bin/dxc \
  --moltenvk-root /path/to/moltenvk \
  --libclang-root /path/to/llvm \
  --python /path/to/python3.11 \
  --install-node-tools
```

已有仓库本地 .NET/VulkanSDK 和常规 Apple Silicon 工具目录时可直接运行
`python3 Tools/macOS/Setup.py`；`--check-only` 不更新保存的路径。
MoltenVK 目录须包含 `lib/libMoltenVK.dylib` 和 `LICENSE`。
CMake 配置显式使用 `arm64` 和 macOS 14.0 部署目标；最终 `.app` 的最低系统版本还会与所携带二进制的实际版本要求取最大值。

## 开发与发布

```sh
python3 Tools/macOS/hectl.py editor build -Project Engine/Templates/Blank
python3 Tools/macOS/hectl.py editor check -Project /path/to/MyGame
python3 Tools/macOS/hectl.py editor capture -Project /path/to/MyGame
# 人工开发：窗口保持开启，退出 Editor 后命令返回。
python3 Tools/macOS/hectl.py editor open -Project /path/to/MyGame
python3 Tools/macOS/hectl.py game check -Project /path/to/MyGame
python3 Tools/macOS/hectl.py package -Project /path/to/MyGame
```

`--cmake-arg=-DNAME=value` 可重复传入，以覆盖保存的 SDK 路径或可选插件配置。
构建默认最多并行 2 个任务，可用 `-Parallel` 调整。Editor 发布面板在 Mac 上为已有项目补充
`macos` 配置；点击检查/构建仍经过同一预检、Release/Cooker 和 `.app` 验证链路，输出位置可以在面板中设置。
Editor/Game 使用独立的项目缓存和 RelWithDebInfo；`package` 强制独立 Release、
`PACK_GAME=ON`、`HE_BUILD_DEV_GAME=OFF`，执行 `PackageGame`。
`RuntimeMode=Scene` 项目自动使用通用场景 Player（`HE_PORTABLE_PLAYER=ON`），不要求项目携带 C++ 启动源码；
自定义原生项目则沿用其自身的 Game 源码。

游戏发行物为 `dist/projects/<项目路径哈希>/<ExecutableName>.app`。
`Contents/MacOS` 包含原生宿主，`Contents/Frameworks` 包含其非系统动态库，
`Contents/Resources` 包含 `Game.pak`、manifest、许可证和可选的私有 C# Runtime。
发布前检查 cooked 资源闭包、Shipping 内容、arm64 动态依赖和本机 ad-hoc 签名，
再从临时工作目录、最小 PATH 运行实际 Vulkan `--package-check`。任何一步失败均保留旧包。
每次打包的 cook、启动日志和 `result.json` 留在对应 build 的 `macos-package-*` 目录。
旧成功包以 `.retired-*` 保留，不自动删除。

启用 Python 插件的 Shipping 还会携带 Python 3.11 标准库及其原生扩展依赖；不复制宿主机 site-packages。
项目使用的额外 Python 包必须纳入明确的项目资源和依赖契约。
本机 ad-hoc 签名用于开发和验收，不等于 Developer ID 签名或 Apple 公证。
对外发布应使用项目自己的 Developer ID 对最终产物签名、公证并重新验证。
Mac 上打开 Editor、导出原生 Mac 应用，与从 Mac 导出 Windows/Android/Web/微信是独立能力；
本入口只声明原生 macOS 工作流，不将既有 Windows 主机构建脚本视为已移植。

## 渲染能力与验证范围

设备能力在启动时检查。本次 M4 Pro 上 MoltenVK 不提供 counted indirect draw，
因此保留 GPU 剔除和实例化，使用逐组 indirect draw；设置中的请求值、实际生效值和回退原因分别保留。
不把 Vulkan 扩展名称存在视为功能已验证，也不声明 Windows DX12 或光线追踪路径已移植。
主场景离屏输出与实际 swapchain 格式一致，独立预览仍可使用 RGBA，避免 BGRA/RGBA 复制和临时视图生命周期问题。

本次实际宿主为 Apple Silicon M4 Pro、macOS 26.5.1。部署目标和动态库最低系统版本检查
不能替代在较旧 macOS 或其他 Apple GPU 上的实际验证。
MoltenVK 1.4.2 在整数附件上可能输出 blending 能力警告；其源码警告分支不检查
`blendEnable`，不能据此把禁用混合的整数附件改为开启混合或关闭整个渲染验证。

本次使用独立 C# 场景项目 StarbloomSurvivor 完成实际验收：Editor 加载场景、
文本属性编辑、Cmd+A、Cmd+S、关闭后重新加载、Play 开始游戏、停止返回编辑模式。
验收修改使用单独的场景副本，不改项目默认启动场景。
Shipping `.app` 复制到源码目录外，从临时工作目录、最小 PATH 且无 HE/DOTNET/DYLD 环境覆盖启动；
实际菜单点击、持续 W 移动、升级选择、战斗和 Escape 暂停通过。
移动验收须在 `playing` 状态执行；升级选择弹窗会暂停游戏，不能用该状态下的零位移判断输入故障。
54 个游戏特效包的实际 Vulkan 依赖加载、播放、停止和重播全部通过。
另有原生平台/输入契约、渲染设置、反射代码生成和 SPIR-V 编译器契约验证。
