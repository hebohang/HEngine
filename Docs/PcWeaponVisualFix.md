# PC 新武器图标与弹体恢复（2026-09-17）

本次修复两个已确认的迁移遗漏：

- 背包新增武器只更新了图片地址，没有清除 RML 初始的 `empty` / `locked` 类；对应 CSS 会隐藏图片并显示菱形。现在按装备状态及解锁槽位更新这两个类。
- PC 表现层只播放粒子，遗漏了旧版同时使用的实体弹体。现在从原武器目录导出 `projectilePrefab` 并生成资源依赖，按投射物 ID 跟随、回收复用，离开战斗时销毁。恢复火球 0.62、月光刃 0.20 的实体尺寸及月光刃 0.69 的粒子尺寸。共享战斗规则不变。

验证：

1. **build-only PASS**，仓库根目录：
   `powershell -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HGame -Configuration RelWithDebInfo -PluginConfig Project/Config/Plugins.CSharp.json`
   日志：`out/agent/pc-restore/weapon-game-build.log`。
2. **unit-test PASS**，工作目录 `Engine/Managed`：
   `dotnet run --project ../../Tools/Scripting/SurvivorNativeTests/SurvivorNativeTests.csproj -c Release --artifacts-path ../../out/agent/weapon-visual-tests`
   测试真实 C# 游戏适配器和升级命令，通过内存主机检查新获得月光刃、火球后的图标状态、暂停保留、实体资源和尺寸，以及施法/弹体粒子提交。它不证明 GPU 渲染。
3. **runtime-manual PASS**，仓库根目录：
   `powershell -ExecutionPolicy Bypass -File Tools/Scripting/TestSurvivorWeaponVisuals.ps1`
   独立 Agent 会话中加载实际游戏场景，直接播放两种原始粒子包；状态均为 playing，截图分别可见蓝色及橙色粒子，已人工检查。证据：`out/agent/weapon-visual-native/`。测试会话在 finally 中停止，使用独立存档。

边界：实机资源检查直接调用播放接口，没有在完整实机对局中逐一取得所有武器；新武器的升级和显示绑定由上述 unit-test 覆盖。未执行全平台 Shipping 打包。
