# 微信托管运行时手机兼容性

最新状态：用户已确认 Module 路径修正后的手机预览成功。随后做源码归属整理，将宿主适配移至 `Engine/Source/Runtime/Platform/WeChat/ManagedRuntime.cjs`，不改变其逻辑；分包工具抽取至 `Tools/Build/ExportWeChatGame.cjs`。见 [引擎与项目边界](EngineProjectBoundary.md)。历史各阶段的失败与未验证说明保留如下。

2026-09-15：用户手机出现 `doesn't support WASM exception handling`，根因为共用 Wasm 配置遗漏异常 ABI 设置，.NET 10 默认使用原生 Wasm EH；桌面开发者工具支持该扩展，未能暴露手机能力差异。

引擎改为 .NET 支持的 JavaScript 异常 ABI，重新链接实际运行时。没有把特性探测强行改为成功，也没有只删除启动断言。参考 [.NET 官方异常处理构建说明](https://learn.microsoft.com/zh-cn/aspnet/core/blazor/webassembly-build-tools-and-aot?view=aspnetcore-10.0)。

同日用户第二次手机预览报告 `performance is not defined`：Emscripten 直接读取 `performance`，与 .NET 自己读取 `globalThis.performance` 的路径不一致。新增引擎公共 `Engine/Source/Runtime/Platform/WeChat/Clock.js`，同时绑定托管运行时环境与生成模块中的直接引用；原生宿主的脚本传输计时也使用该组件。缺少浏览器计时器时使用毫秒时钟并防止回拨，不修改宿主全局对象。

同日上传检查发现新时钟辅助文件中的可选链未被转换。修复公共托管导出器的统一写入路径：所有 `.js` 文件（包括直接复制的辅助文件）都经过 ES2019 转换。两条导出流程复用 `WeChatJavaScript.cjs`；公共发布入口在项目导出完成后检查整个包的 JavaScript，发现需要降级的语法或编译警告时拒绝发布并保留旧包。该检查使用语法转换结果比较，不使用会误报字符串/注释的正则搜索。

手机随后出现“逻辑计时正常、只有 HUD、场景不可见”。针对渲染阶段状态依赖和手机 Canvas 纹理透明通道差异，公共 `SceneRenderer` / `CanvasOverlay` 显式设置深度、混合、裁剪、视口和目标缓冲；微信 HUD 使用 RGBA 像素上传，避免平台隐式 Canvas 上传覆盖场景。新增通用 `LoadingScreen`，项目接入原加载背景、资源/场景/角色进度以及大厅角色与装备图标。新增资源纳入内容清单和分包，不使用远程占位图。手机结果仍需新版预览复测，不能把桌面像素测试当作该手机已通过。

## 公共发布约束

### 20:41 手机反馈后的补充修复

前一轮 HUD 合成调整没有解决用户手机无场景的问题。日志确认该宿主缺少 `WXWebAssembly.compile`，但游戏计数正常，因此尚不能把这个警告认定为场景缺失的根因。

- 公共托管适配器在缺少 `compile` 时使用包路径 `instantiate` 探测可选特性。不把不可用特性强行设为可用。此前依赖 API 类型定义将 Module 改为二进制参数，经 20:53 手机报错证明不符合实际宿主；见下方修正。
- 引擎微信资源宿主默认启用 `compatibilityGeometry`：显式绑定顶点/索引缓冲，能够使用 16 位索引时优先使用；骨骼动画在 CPU 计算后上传，避开大骨骼 uniform 数组。Web 默认仍走 GPU 骨骼路径。该策略增加 CPU 开销，手机性能尚未实测。
- 游戏在模式切换时记录 `[HE render]`，其中公共 renderer 诊断包含绘制次数、贴图数量、场景/HUD 合成前后采样像素与 GL 错误。日志用于下一次手机确认，不表示根因已被证实。
- runtime-manual：开发者工具实际看到原加载图、角色图标、带贴图的地形、角色、敌人和等级 2 升级界面。战斗诊断为 469 次绘制、76 个网格缓存、17 张贴图、GL error 0；场景中心像素 `[225,225,225,255]` 在 HUD 合成后保持一致。
- runtime-manual：`node Tools/Build/TestManagedWeChat.cjs out/agent/wechat-compatibility-fixture/publish/wwwroot/_framework Project/Dist/WeChat` PASS，新增缺少 `WXWebAssembly.compile` 覆盖。证据 `out/agent/wechat-no-compile-test.log`、`out/agent/wechat-compatibility-test/1789476604294/result.json`。
- runtime-manual：`node Tools/Build/TestCanvasOverlay.cjs` PASS，覆盖保守路径的 CPU 骨骼、贴图、状态恢复和透明 HUD；证据 `out/agent/wechat-compatible-geometry-test.log`。`node Tools/Scripting/TestSurvivorWeb.cjs` 的 12 项回归 PASS，证据 `out/agent/wechat-compatible-web-test.log`。
- build-only：上述 Web/微信 `Publish.ps1` 命令 PASS；日志 `out/agent/wechat-compatible-web-build.log`、`out/agent/wechat-compatible-geometry-build.log`。发布过程中目录占用失败会保留旧包，关闭项目窗口后重试成功。
- 当前边界：手机新版效果仍待用户再次预览，不能宣称手机空白已修复；Windows 本轮未改动、未重新运行。

### 20:53 Module 路径回归修正

手机在加载 92% 报 `WXWebAssembly.Module: only support file type .wasm or .wasm.br`。上一轮把浏览器的二进制 Module 构造参数传给微信，造成启动回归；此前测试直接使用浏览器 Module，未覆盖手机的路径约束。

公共适配器的 `compile` 回退和 `Module` 入口现在均把已登记的二进制缓冲映射回包内 `.wasm` 路径，再调用微信 API。未知缓冲仍拒绝。两条公共导出流程共用该适配器，修复不在项目脚本中。

测试宿主移除 `compile`，并强制 Module/instantiate 只接受 `runtime/*.wasm` 或 `.wasm.br` 字符串；测试实际读取包内二进制并运行 .NET，记录 Module 路径调用次数。旧实现启动超时 FAIL，新实现 PASS，C# 异常/finally/JSON 异常/跨宿主传播和恢复仍成功。

- runtime-manual：`node Tools/Build/TestManagedWeChat.cjs out/agent/wechat-compatibility-fixture/publish/wwwroot/_framework Project/Dist/WeChat`；修复前 FAIL（`out/agent/wechat-module-path-before.log`），修复后 PASS（`out/agent/wechat-module-path-after.log`，结果 `out/agent/wechat-compatibility-test/1789477005977/result.json`）。
- build-only：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat` PASS，日志 `out/agent/wechat-module-path-build.log`。
- static-check：`node Tools/Build/WeChatJavaScript.cjs Project/Dist/WeChat` PASS（16 个脚本）；`git -c core.safecrlf=false diff --check` PASS。检查最终包两个 Module 调用均使用 `file`；开发者工具已重新打开新包。
- 手机新版仍待确认；本轮不修改渲染、Web 或 Windows，未重复运行其回归。

- `Engine/Managed/WebAssembly.props` 默认关闭原生 Wasm EH，覆盖可复用于微信的 Web 构建。
- `Tools/Build/Publish.ps1` 对所有微信项目强制兼容设置；检查 native loader 后才调用项目内容导出器。
- `Tools/Build/ExportManagedWeChat.cjs` 和 `Tools/Scripting/export-wechat.cjs` 共用实际产物检查，并输出兼容性元数据。未知格式拒绝导出，升级 SDK 需要重新验证。
- 独立 fixture 不引用 Survivor，实现 C# 抛出/捕获、finally、JSON 异常、向 JavaScript 传播异常及后续调用，证明异常语义仍有效。

## 本次验证

以下命令从仓库根目录执行。Node 使用本机支持 `--no-experimental-wasm-legacy-eh`、`--no-experimental-wasm-exnref` 的版本，`NODE_PATH` 指向已安装 Playwright 的目录。

```powershell
dotnet publish Tools/Build/Fixtures/WeChatCompatibility/Fixture.csproj -c Release -p:HEngineRoot=C:/WorkSpace/HEngineDev -p:HEPlatform=WeChat -p:PublishTrimmed=true -p:InvariantGlobalization=true --artifacts-path out/agent/wechat-compatibility-fixture/artifacts -o out/agent/wechat-compatibility-fixture/publish
node Tools/Build/TestManagedWeChat.cjs out/agent/wechat-compatibility-fixture/publish/wwwroot/_framework Project/Dist/WeChat
node Tools/Build/TestWeChatClock.cjs
node Tools/Build/TestWeChatJavaScript.cjs
node Tools/Build/WeChatJavaScript.cjs Project/Dist/WeChat
node Tools/Build/TestCanvasOverlay.cjs
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile web
node Tools/Scripting/TestSurvivorWeb.cjs
git -c core.safecrlf=false diff --check
```

- build-only：独立 fixture、微信正式导出、Web 导出均 PASS。首次微信发布因旧目录被开发者工具占用而失败，旧包保留；关闭对应窗口后重新发布 PASS。
- unit-test / runtime-manual-browser-harness：兼容性测试 PASS。拒绝 EH/SIMD 不兼容及未知 loader；真实 VM 关闭原生 EH 后可编译 fixture 与最终游戏 Wasm，反向特性样本确实失败；浏览器中的微信包路径适配器运行真实 .NET，三个可选特性探测均失败时，异常测试返回 42，异常传播及恢复成功，无页面错误。
- unit-test：公共时钟毫秒单位、回拨、原生方法接收对象、非有限值及宿主全局隔离 PASS。
- runtime-manual-browser-harness：同时移除 `performance`、`TextEncoder`、`TextDecoder`、`crypto` 的回归 PASS。URL 和完整无 DOM 环境不包含在这个模拟器覆盖范围内。
- runtime-manual：新包在微信开发者工具加载原场景和角色，实际开局战斗到等级 2 并出现升级选择。
- runtime-manual-browser：Web 的 12 项交互回归 PASS。
- static-check：diff --check PASS。
- unit-test：JavaScript 语法回归 PASS；旧 `host-clock.js` 被发布检查准确拒绝，可选链/空值合并转换后行为保持一致，字符串内容不会被误判。
- static-check / build-only：最终导出包全部 16 个 JavaScript 文件通过 ES2019 检查，统一发布 PASS。日志：`out/agent/wechat-upload-syntax-build.log`；公共转换后的真实 .NET 回归：`out/agent/wechat-upload-syntax-runtime-test.log`。微信服务端上传结果仍需重新预览确认。
- runtime-manual-browser：真实 WebGL 像素回归 PASS；128/256 两种画布大小、状态污染与尺寸变化后，绿色贴图物体中心像素在 HUD 合成前后均为 `[26,200,52,255]`，HUD 黄色像素可见，加载页绘制无 GL 错误。命令 `node Tools/Build/TestCanvasOverlay.cjs`；日志 `out/agent/wechat-render-composition-test.log`。本次微信/Web 发布日志分别为 `out/agent/wechat-render-build.log`、`out/agent/wechat-render-web-build.log`。
- runtime-manual：新版微信开发者工具实际显示原加载背景与 44% 角色加载进度、大厅角色图标、战斗地形与角色，截图 `out/agent/wechat-render-evidence/loading.png`、`game.png`。Web 12 项回归 PASS，日志 `out/agent/wechat-render-web-test.log`；新微信包约 18.04 MiB，全部 16 个 JavaScript 文件通过上传语法检查。
- 未执行：本次修复后的手机真机验证、后台正式发布。不能据此承诺所有手机/微信版本均兼容。

证据：`out/agent/wechat-compatibility-test/1789474396759/result.json`、`out/agent/wechat-compatibility-missing-globals-test.log`、`out/agent/wechat-mobile-compatibility-build.log`、`out/agent/wechat-compatibility-web-test.log`。最终游戏 native Wasm SHA-256 为 `1bba612c47cdc3db6fdf75254244495042b68de3f17930d0682cd03b9802d960`。
