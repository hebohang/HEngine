# 构建与发布验收记录

更新时间：2026-09-15。以下区分规则测试、可玩宿主验证与正式打包，不将规则页面视为游戏。

## 规则与会话

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestSurvivorMigration.ps1` | PASS；桌面 .NET 与浏览器 Wasm 各 3,785 组 Python/C# 差分，含非法输入检查 |
| unit-test | `py -3.11 Project/Tools/ExportWeb.py --catalog-only --output out/agent/survivor-native/catalog` 后执行 `dotnet run --project Tools/Scripting/SurvivorSessionTests -c Release --artifacts-path out/agent/survivor-session-tests -- out/agent/survivor-native/catalog/catalog.json` | PASS；42 项会话断言，涵盖敌人攻击、首领三阶段、10 个角色、进化、四域能力和刷怪压力 |
| unit-test | `node Tools/Build/TestVirtualJoystick.cjs` | PASS；摇杆、指针所有权、死区、限幅、取消、失焦、拖动视角 |
| build-only / unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/TestPublish.ps1` | PASS；带空格路径独立项目、路径越界、未支持目标与缺失输入保护，旧输出保留 |
| runtime-manual | `node Tools/Build/TestPublishedFixture.cjs` | PASS；独立项目 C# Wasm 返回 42，并使用引擎公共模块 |

## 可玩宿主

| 证据类型 | 精确命令或操作 | 结果 |
| --- | --- | --- |
| runtime-manual（浏览器脚本驱动） | `node Tools/Scripting/TestSurvivorWeb.cjs` | PASS；12 项检查：原模型、键盘移动、右键视角、暂停、战斗拾取升级、结算与刷新、第二局独立结算、旧记录恢复、四域终局夹具、范围武器、移动摇杆、触屏视角；无页面运行错误 |
| build-only | `.\hectl.bat game build -Parallel 4` | PASS；原生 C# 游戏编译 |
| runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestSurvivorNative.ps1` | PASS；自有会话：角色页、原 3D 场景、移动、视角、暂停、升级、结算、返回大厅、第二局；finally 停止会话 |
| runtime-manual | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Preview.ps1 -Project Project -Profile wechat` 后在微信开发者工具操作 | PASS；原场景和角色、开局、战斗、多次升级、摇杆、拖动视角、暂停、结算、重新打开恢复金币；修复快速连点开始重复提交 |
| build-only / runtime-manual | `.\hectl.bat editor check -Frames 60 -TimeoutSeconds 120 -Resolution 1600x1000 -Parallel 4` | PASS，exit=0 |
| runtime-manual | Editor 顶部发布面板：保存微信 CLI 路径、构建、构建并预览 | PASS；真实导出后打开微信项目。此前 Web 构建、取消、停止预览也通过 |

浏览器脚本使用本机 Node / Playwright，设置 `NODE_PATH` 指向对应 `node_modules`。原生序列读取现有构建 SHA-256 后启动，不调用保留用户进程的开发 open 路由。

## 正式发布

三个命令均通过统一发布入口，输出回执在 `Project/Saved/Build/<Profile>.json`：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile web
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile windows
```

Web / 微信：build-only PASS。微信最终包约 17.83 MiB，保留资源许可证，采用真实 .NET Wasm 和分包资源。Windows：最终 Shipping 发布 PASS；PackageGame 依赖/文件检查、Unicode 路径与独立缓存 DX12/WARP 完整启动 PASS（exit=0）。输出中的 C# 源文件指纹与当前 `SurvivorNativeGame.cs` 一致，GameScripts SHA-256 为 `f3577571748c14b62dcb2b48760d06e884b2fb2e8f52eeb9598a8338a8f8eeeb`。

## 证据与范围

- 日志：`out/agent/survivor-native/*-complete*.log`、`*-final*.log`；各平台完整日志路径在构建回执中。
- 原生截图：`out/agent/survivor-native/latest`，对应会话原图路径在 `capture.frame.json`；已人工查看升级、结算、大厅与第二局。
- Web 截图与断言：`out/agent/survivor-web/evidence`。
- 微信截图：`out/agent/survivor-wechat/evidence`。
- static-check：`git -c core.safecrlf=false diff --check` — PASS。

微信开发者工具启动时曾出现工具基础库的 jsbridge/app.json 提示和 Wasm MIME 回退提示；游戏仍成功加载。没有将这些提示描述为游戏运行异常，也没有绕过安全设置。无手机真机测试、无微信后台上传或发布。终局夹具降低生命并缩短区域时间，不代表原参数完整通关。与 Python 的剩余表现/碰撞细节差异见 `SurvivorCSharpWebMigration.md`。

最后复测曾遇到开发者工具模拟器不再传入触摸事件，重新打开本次测试项目后恢复；未更改安全设置或关闭用户原有窗口。恢复后用最终导出包重新完成开局、触屏转向、升级、暂停、结算与返回大厅，金币从 33 增加到 34。该工具偶发问题没有作为游戏代码修复项；若再次遇到相同现象，应重新打开项目并复测输入。

## 本次修复的管线问题

- 发布日志读取与写入共享方式不一致，导致 Editor 触发构建失败；改为共享文件流。
- Wasm 随机 GUID 在微信缺少加密随机 API 时中断开局；改用本地结算幂等编号，并保留旧存档备份/修复路径。
- 微信触摸坐标、视角输入与连续点击处理，防止界面转换前重复提交按钮命令。
- Windows C# 发布显式采用 `Game.CSharp.json` / `Plugins.CSharp.json`；正式包禁用 Python 插件，开发参考入口保留。
- 原生结算信息与升级说明改为读取共享会话和内容目录，不再显示模板数值。
