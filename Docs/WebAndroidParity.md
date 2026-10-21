# Web 体验对齐（2026-09-17）

最新包：`Project/Dist/Web`。用户运行仓库根目录 `Win-RunWebGame.bat` 构建并打开浏览器预览，不直接打开 HTML 文件。

## 实现与归属

- 引擎 `Runtime/Platform/Web/BrowserHost.js` 提供浏览器逻辑视口和像素密度策略（上限 3）；项目不再自行修补渲染分辨率。DOM 文本由浏览器按显示密度绘制。
- 引擎 `VirtualJoystick` 新增可选 `autoRadius`，根据实际控件大小设置输入半径，窗口变化时清理触点。原调用默认行为保持不变。
- 项目改为全屏标题背景、顶部导航、展开的角色网格、浅色 HUD 与升级面板；升级选项仍为整张原生 button，可点击图标、文字和空白。保留 DOM 的键盘焦点样式。
- 项目移动端新增跳跃按钮，摇杆和跳跃位置采用与 Android 一致的比例；支持移动与视角多指并行。状态切换、失焦、隐藏页面及窗口变化清理输入，防止继续移动。
- 项目新增浏览器全屏入口；补齐已拥有装备的候选池启用/停用与武器槽设置，未解锁槽数不可选；保留键盘 WASD/方向键、空格跳跃、右键转向和 Esc 暂停。
- 手机安全边距用 CSS safe-area 环境值，标题和游戏界面覆盖全屏；修复 body 无高度导致背景未覆盖，以及窄屏计时与暂停重叠。

## 验证

`build-only` PASS：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile web
```

`test-layer` PASS：

```powershell
$env:NODE_PATH='C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules'
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe' Tools/Build/TestSurvivorWeb.cjs
```

真实 Web 发布包与 C# Wasm 在 Chrome 运行；检查全宽菜单、2 倍渲染分辨率、设置、键盘移动/跳跃/转向/暂停、失焦暂停、实际升级图标选择、3 倍密度移动环境的跳跃与多指移动/转向、横竖屏尺寸变化。初次测试错误地尝试选择未购买的第四槽而 FAIL，随后禁用锁定选项并改为验证合法槽数，最终 PASS。

人工检查截图在 `out/agent/web-parity-test`：`menu.png`、`upgrade.png`、`portrait-game.png` 等。测试浏览器及临时服务器在 finally 中关闭。未进行手机真实浏览器、Safari/Firefox、持续帧率、功耗、全屏 API 和音频听感验证。提高分辨率会增加 GPU 开销。

3D 仍使用现有 WebGL 2 SceneRenderer，尚未与原生 Vulkan 的完整材质、阴影、后处理和粒子特效对齐。本次没有把 WebGL 画面优化包装成渲染后端完全一致。
