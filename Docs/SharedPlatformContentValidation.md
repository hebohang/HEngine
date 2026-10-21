# 共享场景/UI 验证记录（2026-09-17）

工作目录：`C:\WorkSpace\HEngineDev`。
本记录只覆盖本批新增共享场景来源、能力检查和共享暂停界面，不代表整个引擎已实现四端功能一致。

## 静态和逻辑检查

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| static-check | `git diff --check` | PASS |
| static-check | `node Tools/Build/CompileSharedUI.cjs Project --check` | PASS（通过各平台发布前检查执行） |
| static-check | `py -3.11 Tools/Build/SharedContent.py Project` | PASS，8 项显式组件降级 |
| unit-test | `node Tools/Build/TestSharedUI.cjs` | PASS |
| unit-test | `py -3.11 Tools/Build/TestSharedContent.py` | PASS，5 项 |
| unit-test | `node Tools/Build/TestCanvasUI.cjs` | PASS |
| unit-test | `node Tools/Build/TestWeChatHud.cjs` | PASS |

UI 测试覆盖固定/拉伸锚点、层级错误、未知能力拒绝、可见性、裁剪、事件区域、文本修改、RML 转义、只读检查和重新生成。
场景测试覆盖统一来源、平台配置分叉、未知组件/脚本、父节点缺失和环。

## 构建与运行

以下构建命令的日志保存在 `out/agent/shared-ui-*-build.log`；详细日志和最终回执由原发布流程保存到 `Project/Saved/Build/`。

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile web` | PASS |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat` | PASS |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile windows` | PASS，包含既有 shipping 包完整性/启动门禁 |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile android` | PASS，ARM64 APK、签名/清单、12 个原生库、16 KB ELF 和内容 hash 检查 |
| runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HGame -Configuration RelWithDebInfo -PluginConfig Project/Config/Plugins.CSharp.json -Run -Wait -TimeoutSeconds 90 -ExitAfterFrames 120 -StartupScene Assets/Scenes/MainGameCSharp.he -NoCursorCapture` | PASS，构建及有界启动 |
| runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineEditor -Configuration RelWithDebInfo -Run -Wait -TimeoutSeconds 90 -ExitAfterFrames 30 -StartupScene Assets/Scenes/MainGameCSharp.he` | PASS，构建及有界启动 |
| runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/TestSharedUINative.ps1` | PASS，暂停/继续/重新开始/结算、返回菜单输入；截图已人工检查 |

原生证据：`out/agent/shared-ui-native-test.log`、`out/agent/shared-ui-native/capture.json`、`out/agent/shared-ui-native/logs.json`。
Game/Editor 启动检查不是 UI 编辑操作覆盖；本次没有通过 Editor UI 面板逐项操作所有属性。

浏览器工具运行环境和精确命令：

```powershell
$env:NODE_PATH='C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules'
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestSharedUIBrowser.cjs
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestWeChatLandscape.cjs
```

- `test-layer`：`TestSharedUIBrowser.cjs` PASS。实际导出游戏加载 C# Wasm，覆盖共享资源文字/颜色替换、横竖屏和暂停/继续/重新开始/结算按钮。
- `test-layer`：`TestWeChatLandscape.cjs` PASS。模拟 WX API 的浏览器加载实际导出小游戏，覆盖启动、菜单、触摸、暂停/恢复、后台切换和竖屏；不是微信设备验证。
- `runtime-manual`：原生和模拟微信暂停截图已查看；微信安全区外遮罩修复后重新检查。

浏览器日志：`out/agent/shared-ui-browser-test.log`、`out/agent/shared-ui-wechat-test.log`。

## 已解决的验证失败

- 首次并发 Windows/Android 打包争用 host package 缓存锁；改为串行。
- 原生隐藏暂停文档后活动文档未恢复；显式恢复主文档，并通过交互复验。
- 浏览器布局测试在 resize 事件处理前读取矩形；改为等待渲染帧再断言。
- 首次 Editor 检查传入仅 Game 支持的 `-NoCursorCapture`；移除后构建/启动通过。

未执行 Android 真机、微信开发者工具/微信真机测试，也未进行上传、审核或上线。Android 配置仍为 Development 签名。
