# 引擎与游戏项目边界

引擎和公共工具提供能力，项目提供内容与策略。公共运行时、打包器不能导入 Survivor 的脚本、目录或资源名。

## Web 浏览器宿主边界（2026-09-17）

Web 项目统一接入以下引擎组件，不复制当前游戏的 DOM 事件代码：

| 引擎组件 | 通用职责 | 留给项目的策略 |
| --- | --- | --- |
| `Web/BrowserHost.js` | CSS 逻辑视口、DPR 与渲染像素密度上限 | 质量预算；默认上限 3 |
| `Web/Input/BrowserInput.js` | 键盘状态、编辑框过滤、触点归属、鼠标/触屏拖动、失焦/隐藏/尺寸变化时清理、销毁监听器 | 跳跃/暂停键、允许开始转向的区域、拖动如何控制相机 |
| `Web/Input/VirtualJoystick.js` | 摇杆死区、触点捕获、按实际尺寸计算半径（autoRadius）、取消与销毁 | 控件位置、外观、移动方向的游戏映射 |
| `Web/BrowserLifecycle.js` | 全屏 API、页面失焦/隐藏/退出通知、清理输入和解除绑定 | 是否暂停、保存哪些数据、音频处理、提示文案 |
| DOM/CSS 与 Canvas UI 后端 | 浏览器原生文本栅格化或引擎高清 Canvas 合成 | 菜单/升级视觉主题，装备池、武器槽与经济规则 |

`BrowserInput` 的 `canStartDrag` 决定触点是否能取得拖动控制；`canDrag` 决定当前状态是否允许继续。已经取得控制的触点可以跨过起始区域边界，不会中途丢失拖动。`data-he-input-block` 标记自定义交互控件，避免触控穿透至游戏视角；原生按钮/表单控件自动排除。所有输入量都是 CSS 像素，不应由项目乘 DPR。

`bindBrowserLifecycle` 返回解绑函数，`BrowserInput` 和 `VirtualJoystick` 提供 `dispose()`；项目退出或替换会话时释放。宿主只发通知，不直接执行 Survivor 的 pause/save。当前 Web 项目已迁移到这些组件，保留游戏动作映射和项目 UI。

验证：`unit-test` PASS（无游戏资源的浏览器宿主测试），`build-only` PASS（正式 Web 发布成功），`test-layer` PASS（最终发布包真实 C# Wasm 与桌面/移动模拟浏览器交互）。精确命令：

```powershell
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestBrowserHost.cjs
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile web
$env:NODE_PATH='C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules'
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestSurvivorWeb.cjs
```

独立测试覆盖普通键盘、输入框隔离、触点跨区域归属、失焦清理、解除监听、生命周期通知与全屏调用分派；全屏分派使用模拟对象，不等同于所有浏览器全屏能力的实机验证。本次未新增第三方依赖，未修改原生 Android/Windows 渲染后端。

## 分辨率、全屏 UI 与点击（2026-09-17）

| 引擎通用能力 | 项目选择 |
| --- | --- |
| `SceneRenderer` 的像素密度策略、`CanvasOverlay` 的高清栅格化及纹理上传 | 使用何种画质预算；不得在项目内补一次像素缩放 |
| `WeChat/Viewport` 读取胶囊、刘海和安全边距 | 哪些内容应避开安全边距 |
| `ViewportLayout` 提供同一逻辑坐标系下的 `fullRect`、`safeRect`、`point()`、`paint()` | UI 设计尺寸、导航方向、列数、卡片间距 |
| `CanvasUI` 提供图片 cover、面板、文本、按钮、独立点击边界及最上层单一命中 | 背景图片、颜色、字体、卡片内容和点击命令 |
| `WeChat/DebugSettings` 和导出器提供默认关闭调试的配置 | 显式启用开发调试；不会把游戏日志画成 UI |
| 输入组件提供触点归属、死区、拖动与布局辅助 | 控制位置、是否有跳跃、如何驱动角色 |

当前项目已改为调用这些引擎接口，删除了项目中的背景覆盖计算、反向安全边距计算、按钮命中判断及基础绘制实现。按钮的 `hitBounds` 可以覆盖整张卡片，而可见按钮仍使用自己的矩形；只注册一个点击目标。项目仍决定“点击整卡选择升级”，引擎不认识升级 ID 或 Survivor 规则。

新 Canvas 项目接入示例（省略模块导入）：

```js
const layout = viewportLayout(hostViewport, {landscapeHeight: 720, portraitWidth: 480});
overlay.draw(hostViewport.width, hostViewport.height, ctx => {
    layout.paint(ctx, () => {
        const ui = new CanvasUI(ctx);
        ui.imageCover(background, layout.fullRect); // 背景覆盖完整屏幕
        ui.button('打开', {x: 20, y: 200, w: 260, h: 40}, {
            action: 'open', hitBounds: {x: 10, y: 20, w: 280, h: 230}
        });
        targets = ui.targets;
    });
});
const point = layout.point(pointerX, pointerY); // 宿主逻辑像素，不乘 DPR
const target = hitTest(targets, point.x, point.y);
if (target) dispatch(target.action, target.value);
```

分辨率和点击逻辑由组件复用，不能用当前游戏的 Hud.js 作为新项目的引擎模板。该组件是 Web/微信 Canvas 后端的基础设施，尚不是原生 Rml UI 的完整资源解释器；原生 UI Slot/锚点与 Canvas 的统一资源接入仍需后续实现，不能声称任意原生 UI 资源已经自动跨平台运行。

独立于游戏数据的检查：`node Tools/Build/TestCanvasUI.cjs`；当前游戏布局检查：`node Tools/Build/TestWeChatHud.cjs`。前者用不同设计尺寸和普通文档按钮验证安全区域、背景覆盖、坐标转换、异常后的绘制状态恢复、整卡及重叠命中，不依赖任何 Survivor 资源。

本次验证（2026-09-17）：

```powershell
# unit-test PASS（两项）
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestCanvasUI.cjs
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestWeChatHud.cjs
# test-layer PASS：真实 C# Wasm + 模拟微信接口，验证暂存的新包
$env:NODE_PATH='C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules'
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestWeChatLandscape.cjs out/agent/build-publish/02A39F226444/wechat/content/20260916-161454-9976/site
```

`build-only`：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat` 的编译、导出和 ES2019 校验完成，但两次正式目录替换因开发者工具占用而 FAIL；常用发布目录未包含本次组件抽取。暂存包运行测试 PASS，菜单截图人工检查通过；本轮没有重复微信真机验证，也未改动/构建原生 C++ 后端。

| 内容 | 源码归属 |
| --- | --- |
| 摇杆数学、死区、多指跟踪、视角拖动 | `Engine/Source/Runtime/Platform/Web/Input/`，Web 和微信共用 |
| 网格、贴图、骨骼、HUD 合成、加载页组件 | `Engine/Source/Runtime/Platform/Web/` |
| 微信文件、图片、音频、时钟、C# Wasm 宿主适配 | `Engine/Source/Runtime/Platform/WeChat/` |
| 托管公共 API 与编译配置 | `Engine/Managed/` |
| 编辑器打包面板 | `Engine/Source/Editor/Panels/BuildPublishPanel.*` |
| 发布编排、资源优化、托管导出、微信分包与打包校验 | `Tools/Build/`，属于引擎开发工具，与 `Engine/` 同级 |
| C# 玩法、成长、装备、战斗、存档规则 | `Project/Scripts/CSharp/` |
| 场景、模型、贴图、音频 | `Project/Assets/` |
| 游戏 HUD、菜单、输入到游戏命令的映射、游戏相机策略 | `Project/Web/`、`Project/WeChat/` |
| 平台设置、AppID、项目名称、质量选择 | `Project/Config/` |
| 项目内容选择、规则表导出、构建入口 | `Project/Tools/`、`Project/Build/` |
| 发布产物 | `Project/Dist/`，由工具生成，不作为源码维护 |

摇杆所在位置、是否显示跳跃按钮、摇杆如何驱动角色属于游戏策略；归一化输入、死区和触点状态属于通用组件。加载页组件归引擎，其背景图片、文案和要加载的场景归项目。

## 微信发布接入

项目的 `Tools/ExportWeChat.cjs` 仅调用公共 `Tools/Build/ExportWeChatGame.cjs`，传入内容目录、托管目录、输出目录、游戏入口、配置及额外内容文件列表。公共导出器读取 `content-manifest.json`，校验内容哈希，生成分包和索引，打包入口脚本并检查总容量；不识别任何游戏 catalog 字段或场景名称。

项目的 `ExportWeb.py` 仍负责选择当前游戏的场景和规则表，这是项目内容策略。`Publish.ps1` 负责调用项目导出钩子，应用引擎资源优化与平台兼容检查。运行时 `ManagedRuntime.cjs` 由公共托管导出器复制为产物中的 `wechat-runtime.js`；产物名称不要求与源码路径相同。

独立项目验证：`node Tools/Build/TestWeChatGame.cjs out/agent/wechat-compatibility-fixture/publish/wwwroot/_framework`，unit-test PASS。测试项目不引用 Survivor，无 `catalog.json`，使用独立入口、资源、名称和横屏设置；验证分包内容与非空输出保护。

搬移运行时验证：`node Tools/Build/TestManagedWeChat.cjs out/agent/wechat-compatibility-fixture/publish/wwwroot/_framework Project/Dist/WeChat`，runtime-manual PASS。仍覆盖手机缺失 `compile`、Module 只接受包路径、真实 C# 异常及恢复。

日志：`out/agent/wechat-separation-project-test.log`、`out/agent/wechat-separation-runtime-test.log`。本轮未修改或重新构建 Windows、Web 渲染与玩法。

当前游戏验证：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat`，build-only PASS（`out/agent/wechat-separation-build.log`）；`node Tools/Build/WeChatJavaScript.cjs Project/Dist/WeChat`，static-check PASS（16 个脚本）；`git -c core.safecrlf=false diff --check`，static-check PASS。
