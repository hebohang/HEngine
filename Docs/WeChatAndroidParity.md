# 微信小游戏与 Android 的表现对齐

## 最新：升级整卡点击（2026-09-17）

升级选项整张白卡（含图标、空白和底部按钮）现在共用一个点击目标，执行同一项升级命令。每个选项只登记一个点击区域，避免重叠触发。最新包已成功发布回 `Project/Dist/WeChat`，包含此前的高清及全屏布局修复；下文关于该目录仍为旧包的说明仅为历史记录。

- `unit-test` PASS：`& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestWeChatHud.cjs`。新增图标中心、白卡边缘、底部按钮命中同一升级 ID 的检查，覆盖三种视口宽度。
- `build-only` PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat`。
- `test-layer` PASS：使用下文相同 NODE_PATH，运行 `& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestWeChatLandscape.cjs Project/Dist/WeChat`。一次误用已被发布流程移动的暂存路径导致测试启动 FAIL，改用最终发布目录后 PASS。
- 新版整卡点击尚未完成微信真机验证；上述命中检查为自动测试，不能代替真机观察。

## 全屏布局修复

背景和弹窗遮罩现在使用整个画布的边界，交互内容继续使用安全区域，避免胶囊或刘海遮挡。主菜单改为横向导航与全宽内容网格，不再留侧栏；升级面板随安全区域宽度展开，选项卡片等分可用空间。

`unit-test` PASS：`& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestWeChatHud.cjs`，覆盖 390/1040/1280 宽度的背景覆盖、导航/选项展开及触控边界。

`test-layer` PASS：`TestWeChatLandscape.cjs out/agent/build-publish/02A39F226444/wechat/content/20260916-155235-7531/site`，使用下文相同 Node 和 NODE_PATH 环境。此阶段导出完成但正式目录替换两次被微信预览服务占用而 FAIL，因此明确验证暂存新包，不能将旧正式目录运行结果当作布局修复证据。

该新包已复制到 `Project/Dist/WeChat-Layout-20260916-155235`，逐项验证 `game-manifest.json` 中的 SHA256 全部 PASS。常用的 `Project/Dist/WeChat` 仍为之前的高清修复版，不能混淆；布局版需加载新的预览。布局修复已通过后续微信真机观察：用户扫码独立目录的新预览后，直接通过 USB 抓取 2670×1200 截图，确认主菜单背景全屏、顶部导航和内容网格展开；进入游戏自然触发升级，确认三张选项卡等宽展开、全屏遮罩无安全区留边。最终保留升级界面供用户检查。

## 真机高清修复（2026-09-16 后续验证）

- 原 UI Canvas 仅以逻辑像素绘制，随后放大到手机屏幕；主 Canvas 还限制在 1.5 倍像素密度。现 CanvasOverlay 按实际 WebGL framebuffer 尺寸绘制及上传，保持布局和触控使用逻辑坐标；微信主画布上限改为 3 倍。浏览器宿主仍保留自己的密度策略。
- 引擎 `WeChat/DebugSettings.js` 按导出配置调用 `wx.setEnableDebug`，默认关闭，项目导出配置显式 `debug: true` 时可重新启用。调试开关可能作用于正式版，不能以发布渠道代替明确配置。微信自己的性能工具不由引擎绘制。
- USB 已成功用于这台 Xiaomi 手机的截图和触控。实际操作路径：右上角 `…` → 底部图标横向滚到 `Develop and Debug` → `Disable Debugging`。微信退出小游戏后重新扫码；新预览菜单、游戏和弹窗中红色统计、vConsole 均消失。该结果不能单独区分关闭微信调试和新版初始化调用各自的作用。
- `build-only` PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat`。
- `test-layer` PASS：下文同一 `TestWeChatLandscape.cjs Project/Dist/WeChat` 命令；新增 3 倍像素密度、主画布/UI 均为 2880×1620、默认调试关闭的断言，原交互测试仍通过。
- 微信 CLI 预览 PASS：`& 'E:/Program Files (x86)/Tencent/微信web开发者工具/wechatidecli.cmd' preview --project C:/WorkSpace/HEngineDev/Project/Dist/WeChat --qr-format image --qr-output C:/WorkSpace/HEngineDev/out/agent/wechat-hidpi-preview-qr.png --info-output C:/WorkSpace/HEngineDev/out/agent/wechat-hidpi-preview.json`。
- `runtime-manual` PASS：用户扫码新版后，通过 USB 直接检查 2670×1200 手机截图；菜单、游戏 HUD、升级选择、移动和暂停正常，最终停在暂停面板。截图命令：`& out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell screencap -p /sdcard/he-wechat.png`，再以 `adb pull` 保存本地。触控验证包括 `shell input tap 900 1120`（开始）、`shell input tap 950 730`（升级）、`shell input swipe 545 955 730 955 1200`（移动）、`shell input tap 2510 230`（暂停）。这是人工观察证据，不是自动化引擎测试。
- 证据：`out/agent/wechat-phone-menu.png`（旧版）、`wechat-phone-hidpi.png`（新版菜单）、`wechat-phone-hidpi-game.png`、`wechat-phone-hidpi-paused.png`（实际为升级面板）、`wechat-phone-hidpi-final.png`（暂停），均位于 `out/agent`。未做持续帧率/功耗测试；更高像素密度会增加绘制与上传开销。3D 材质/阴影/特效差异仍然存在。

2026-09-16：微信发布包位于 `Project/Dist/WeChat`。

## 本次实现

- 菜单由固定 390 单位侧栏改为随安全区域展开，宽屏使用左侧导航、多列内容；窄屏使用横向导航、单列内容。复用 Android 的标题背景和物品图标。
- HUD 使用浅色生命面板、粉色生命条、居中计时、净化目标提示。暂停、升级、复活与结算面板居中，保留原有 C# 命令和广告、好友榜接入。
- 摇杆、跳跃和暂停使用与 Android 相同的布局比例，摇杆支持动态触点原点、径向死区，多指移动、转向和跳跃可同时使用。视角拖动统一使用逻辑坐标，避免屏幕缩放影响灵敏度。菜单状态切换、取消触控、窗口变化和切后台清理输入。
- 引擎 `Runtime/Platform/Web/Input/TouchLayout.js` 提供移动控制布局；项目 `WeChat/Hud.js` 只负责展示，游戏规则和存档仍在公共 C# 游戏代码中。没有新增第三方依赖。

## 边界

这不是 Android Vulkan 渲染的完整移植。微信继续使用现有 WebGL 2 SceneRenderer，光照、材质、阴影和粒子特效仍与原生后端存在差异；菜单/HUD 也不是原生 Rml UI 的逐像素复制，尚未将引擎 UI 资源解释器接入微信 Canvas。原生完整背包与目标详情等展示尚未完全覆盖。

## 验证

- `build-only` PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat`，正式目录已替换。包含 .NET Wasm 移动兼容检查与 17 个 JavaScript 文件的 ES2019 检查。首次运行因旧目录被开发者工具占用而 FAIL，关闭项目后重跑 PASS。
- `test-layer` PASS：下面命令运行最终发布包与真实 C# Wasm；覆盖菜单开始、好友榜、移动、多指转向/跳跃、暂停恢复、后台暂停、竖屏尺寸变化和远处物体过滤。

```powershell
$env:NODE_PATH='C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules'
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestWeChatLandscape.cjs Project/Dist/WeChat
```

测试依赖 Node 20+、Playwright 和本机 Chrome，仅用于验证，不进入游戏包。本机另一套 Node 16 无法运行 Playwright，首次测试启动失败，切换上述运行时后通过。

截图位于 `out/agent/wechat-landscape-test`，人工检查横屏菜单、游戏及暂停面板。该运行是在 Chrome 中模拟微信接口，不代表微信开发者工具或微信手机端验证；本次尚未验证微信真机帧率、广告服务或完整升级流程。

### 布局修复真机证据

`runtime-manual` PASS（画面人工检查，不是自动化测试）。菜单截图 `out/agent/wechat-layout-device.png`；升级截图 `out/agent/wechat-layout-upgrade-device.png`。

```powershell
& out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell input tap 900 1120
# 等待正常游戏触发升级，再采集；没有注入游戏状态。
& out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell input swipe 545 955 730 955 1200
& out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb shell screencap -p /sdcard/he-wechat.png
& out/agent/android-toolchain/sdk/platform-tools/adb.exe -s 4b56f2eb pull /sdcard/he-wechat.png out/agent/wechat-layout-upgrade-device.png
```
