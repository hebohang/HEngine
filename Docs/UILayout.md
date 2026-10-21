# 自适应 UI：Canvas 与 Slot

游戏视口跟随窗口尺寸。Android 默认横屏，但不强制 16:9；场景与 UI 根视口铺满窗口；控件边缘留白由 UI Slot 配置。画面比例不应由 Android Activity 固定。

引擎内置 `.hui` 组件树、Canvas、Panel、Text、Image、Button 和每个节点的 `Layout`（Canvas Slot）。编辑器的锚点、Pivot、偏移与尺寸配置会导出为父容器相对约束，窗口变化时由 UI 布局系统重新计算。游戏不需要逐个定位控件。

## Slot 规则

两个坐标轴分别计算：

| 配置 | 含义 |
| --- | --- |
| 某轴 AnchorMin = AnchorMax | 固定尺寸；OffsetMin 是锚点到 Pivot 的偏移，OffsetMax 是该轴尺寸 |
| 某轴 AnchorMin ≠ AnchorMax | 拉伸；OffsetMin 是起边偏移，OffsetMax 是末边偏移（向内留白用负值） |
| Pivot | 固定尺寸轴的对齐位置：0 起边、0.5 居中、1 末边 |

例如顶部状态条：`AnchorMin=(0,0)`、`AnchorMax=(1,0)`、`Pivot=(0,0)`、`OffsetMin=(24,16)`、`OffsetMax=(-24,80)`。无论 16:9、超宽手机还是竖屏窗口，都保留左右 24 的边距、顶部 16 的偏移和高度 80。

全铺满面板：`AnchorMin=(0,0)`、`AnchorMax=(1,1)`，两组 Offset 都为 0。右下按钮：两个 Anchor 均为 `(1,1)`、Pivot 为 `(1,1)`，OffsetMin 为负边距，OffsetMax 为按钮尺寸。

根 Canvas 填充视口；嵌套 Canvas 像其他控件一样遵循父节点 Slot。父节点改变尺寸时，子节点继续按自身锚点计算。尺寸使用引擎 UI 的 dp 缩放，现有参考尺寸 1280×720 与用户缩放仍生效；增加屏幕宽度会提供更多布局空间，而非拉宽每个按钮和字体。

## 创作与兼容

在 UI 编辑器选择控件，使用水平、垂直或全拉伸锚点预设，然后调整边距。保存/导出 `.hui` 后，生成的 RML 保留百分比锚点，不再固定成设计分辨率的坐标。不要手工修改生成的 RML/RCSS。

旧版已经导出的节点 UI 需重新导出才能获得动态约束。仅单轴拉伸的旧资源需检查另一轴的尺寸：该轴现在正确采用固定尺寸语义。仓库当前项目没有这种需要迁移的已存节点资源。

`SurvivorHUD.hui` 的 Nodes 为空，指向项目手工维护的 RML/RCSS；它使用 100% 根布局，因此窗口全屏后自动使用全部可用区域。节点式 UI 与手写 RML 共用同一个引擎 UI 运行时。

这项改动完善现有 Canvas Slot，不代表已经具备 UE Widget Blueprint 的全部组件、可视化逻辑和动画编辑功能。

## 验证记录

2026-09-16：Xiaomi 24129PN74C / Android 16 真机。视口从 1792×1008 扩展至实际 2670×1200，窗口灰色留边消除，菜单布局随屏幕比例变化，点击开始冒险成功进入游戏与升级界面。截图是运行时观察，不等价于完整触控或长时间性能测试。

| 证据类型 | 命令 | 结果 |
| --- | --- | --- |
| unit-test | `.\hectl.bat test RuntimeUiResource` | PASS：横屏、超宽、竖屏布局；固定尺寸、Pivot、四边边距；导出保留相对约束 |
| runtime-manual | `.\hectl.bat editor check -GraphicsApi Vulkan -StrictGraphicsApi` | PASS：编辑器构建和有界启动退出；未声称自动操作了 UI 编辑器面板 |
| build-only | `.\hectl.bat android build` | 主机 shipping cook 和 Android 原生编译 PASS；宿主 Java 阶段因旧 JNI 缓存重复类而 FAIL |
| build-only / static-check | `out/agent/android-toolchain/Publish-Fullscreen.ps1` | 清理宿主后重新 publish PASS；签名、zipalign、Manifest、12 个 ARM64 库/16 KB 段与资源哈希 PASS |
| runtime-manual | `out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb install -r out/agent/android-device-fix/com.hengine.starbloomsurvivor-Signed.apk` | PASS：最终包已覆盖安装到手机 |
| runtime-manual | `out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell am start -W -n com.hengine.starbloomsurvivor/org.hengine.runtime.GameActivity` | PASS：冷启动，日志 viewport=2670x1200 |
| runtime-manual | `out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell input tap 1990 1025` | PASS：开始冒险，游戏与升级界面显示 |
| static-check | `git -c core.autocrlf=false diff --check` | PASS |

正式 `Tools/Android/Build.ps1` 已增加宿主 clean 再 publish，防止切换 JNI 注册模式后旧 Java 文件进入 APK。清理不影响独立的原生和 cooker 缓存。最终包由本次成功的主机 cook、原生库和重新发布的宿主组成，通过校验后复制到 `Project/Dist/Android`；上表如实保留最初完整发布命令的失败记录。

最终 APK：`Project/Dist/Android/com.hengine.starbloomsurvivor-Signed.apk`，79,043,224 字节，SHA-256 `fb3a21230141198232eaba8b379e39f6c86956bfb4736bcc06f915b07b677127`。开发签名。

证据位于 `out/agent/android-toolchain/`：`ui-slot-tests.log`、`ui-editor-check.log`、`ui-host-clean.log`、`ui-final-package.log`、`ui-device.log`、`ui-fullscreen-menu-preview.png`、`ui-fullscreen-gameplay-preview.png`。测试会话已停止，手机保留最终安装包与应用数据。
