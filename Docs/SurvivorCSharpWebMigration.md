# Starbloom C# Web 迁移

Windows、Web、微信小游戏现有共用 C# 会话的可玩版本，使用原场景和角色资源，并通过编辑器同一发布面板构建。规则差分与实际宿主验收分别记录；这不是只显示规则结果的验证页面，也不声称与 Python 逐帧、逐像素相同。

## 运行

- Editor 顶部 **发布…** → **Web 浏览器** → **构建并预览**。
- 或运行仓库根目录 `Win-RunWebGame.bat`。
- 最终站点：`Project/Dist/Web`，本地预览地址通常为 `http://127.0.0.1:8769/`。
- `Tools/Scripting/SurvivorParityWeb/index.html` 只是规则验证源文件，不是可玩游戏。游戏必须通过 HTTP 访问，不能双击 HTML 使用 file://。

完整操作、项目边界和新项目接入方式见 [构建与发布](BuildAndPublish.md)。

## 架构与范围

原桌面开发入口仍是 Python，用于对照；`Win-RunCSharpGame.bat` 启动原生 C# 入口 `MainGameCSharp.he`。Windows 发布配置已切换到该入口，并使用关闭 Python 插件的配置。三端游戏逻辑来自 `Project/Scripts/CSharp/Survivor/WebGameSession.cs`；原生通过 `SurvivorNativeGame` 连接场景和 RmlUI，Web / 微信编译真实 .NET Wasm，JavaScript 负责平台渲染、输入与展示。

引擎公共 WebGL 场景渲染、音频与摇杆位于 `Engine/Source/Runtime/Platform/Web`。游戏专属 UI、表现映射、浏览器入口和内容导出器分别位于 `Project/Web`、`Project/Build/Web`、`Project/Tools/ExportWeb.py`。引擎源码和共用构建器不引用 Survivor 类或固定 Project 目录。

导出保留 518 个原项目资源，资源约 30.6 MiB，不含 .NET runtime。云默认关闭，原桌面 Editor 在关闭云时隐藏相关细节参数。

已接入角色初始属性与成长、训练和强化开关、升级池、饰品、进化、分类型敌人攻击、首领阶段、区域状态机、区域刷怪压力与四种区域能力。结算页显示会话实际奖励和战斗统计。C# 支持 v1–v4 存档规范化和旧测试结算恢复，各宿主使用独立存储，不自动同步，也不覆盖 Python 存档。运行时不需要 Python；当前项目仍使用 Python 在构建阶段导出已有内容数据。

### 对齐边界

- 三个宿主使用同一 C# 规则代码；Windows 使用原生渲染和原 RmlUI，Web/微信使用轻量 WebGL 展示。
- 特效、碰撞轨迹、击退和部分区域特殊波次仍有简化。C# 地形使用板块/坡道高度查询，未复刻原物理求解器的每一项碰撞行为。原生 C# 特效也尚非原 Python 表现的一比一复刻。
- 完整原参数通关、所有装备组合和设置面板选项未逐项验收。已有终局检查使用缩短时间、降低首领生命的隔离夹具，不能当作原始难度通关证明。
- 因此，当前证据证明三端可玩与发布管线可用、所列规则与交互通过，不证明所有 Python 行为完全等价。

## 验证

- `unit-test`：`py -3.11 Tools/Scripting/TestSurvivorParity.py` — 3,785 组 Python/C# 差分 PASS。覆盖战斗算式、角色初始属性与等级加成、饰品伤害、区域压力、挑战、奖励、升级池、宝箱进化、地形高度、区域状态机和 v1–v4 存档规范化。
- `runtime-manual`（浏览器脚本驱动）：`node Tools/Scripting/TestSurvivorWeb.cjs` — PASS。实际启动原场景，键盘移动、战斗拾取升级、暂停冻结、放弃结算、返回大厅、刷新恢复存档、第二局独立奖励、旧测试存档修复、移动视口摇杆和触屏视角均通过；无浏览器运行错误。
- 终局分支通过独立上下文缩短区域计时、降低 Boss 生命验证四区域→星龙→胜利；不是原始参数完整通关证明。
- `unit-test`：`node Tools/Build/TestVirtualJoystick.cjs` — PASS，覆盖死区、径向限幅、多指隔离、取消、丢失捕获、失焦、隐藏与销毁。
- `build-only` / `unit-test`：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/TestPublish.ps1` — PASS。带空格路径的独立项目成功构建；越界输出、未支持平台、缺失输入均拒绝，旧输出保留。
- `runtime-manual`：`node Tools/Build/TestPublishedFixture.cjs` — PASS。独立项目在浏览器执行 C# 返回 42，并加载共用摇杆/渲染模块。

- `unit-test`：`dotnet run --project Tools/Scripting/SurvivorSessionTests -c Release --artifacts-path out/agent/survivor-session-tests -- out/agent/survivor-native/catalog/catalog.json` — 42 项 PASS，覆盖敌人攻击、首领阶段、进化槽位、10 个角色、四域伤害独立性与区域压力。运行前用 `py -3.11 Project/Tools/ExportWeb.py --catalog-only --output out/agent/survivor-native/catalog` 导出夹具数据。
- `runtime-manual`：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestSurvivorNative.ps1` — PASS。使用已构建的 C# 开发目标，在自有会话内验证角色预览、移动、视角、暂停、战斗升级、奖励结算、大厅和第二次开局，结束时自动停止会话。

游戏截图和回执在 `out/agent/survivor-web/evidence`。浏览器测试使用 Playwright；本机可将 `NODE_PATH` 指向已有工具环境的 `node_modules`，并用该环境的 Node 执行。构建日志与各平台最新状态见 `Project/Saved/Build/*.json`。

Windows Shipping 和 Editor 最新界面验证记录见 `Docs/BuildPublishValidation.md`；不将规则测试当作游戏或平台打包验收。
