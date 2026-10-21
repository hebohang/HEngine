# 横屏、区域加载与微信服务

## 源码归属与行为

引擎层提供 `ViewportLayout`（绘制/命中共用安全区变换）、微信 `Viewport`（胶囊与刘海避让、启动方向）、`InterestRegion`（显示范围）、`Advertising`（广告生命周期）和开放数据域排行榜组件。游戏层负责菜单布局、相机、广告触发时机、复活规则与排名指标。

微信默认横屏：`Project/Config/WeChat.json` 的 `orientation` 为 `landscape`。导出器写入 `game.json`，启动时也调用微信方向接口；不支持切换的宿主仍按实际窗口尺寸布局。横屏大厅保留左侧操作列表与三维预览，战斗摇杆位于左下、跳跃位于右下，暂停避开胶囊。尺寸变化清理旧触点，绘制与点击使用相同安全区坐标。

## 性能策略

项目配置为 `Project/Config/PortableRendering.json`：

| 平台 | 显示半径 | 预加载余量 | 骨骼更新频率 |
| --- | --- | --- | --- |
| Web | 65 | 25 | 30 Hz |
| 微信 | 55 | 25 | 20 Hz |
| Windows | 不启用 | — | 原路径 |

半径单位为游戏世界单位。引擎默认不启用区域筛选，项目给 Web/微信显式选择配置。静态场景网格按附近范围延迟加载；角色模型在首次需要显示时加载，等待期间使用临时几何体，加载失败明确报错。已访问资源保留缓存，不是完整的内存卸载/磁盘分块流式系统。分包下载仍在启动时完成，本轮优化减少模型解码、GPU 上传、绘制和骨骼计算，不减少包下载量。

C# 仅把显示范围加余量内的敌人、拾取物和飞行物传给前端，避免远处对象的显示高度计算与 JSON 传输。远处怪物继续参与原有战斗逻辑，没有被清除或冻结。微信 HUD 纹理最多更新 30 Hz，场景仍独立逐帧绘制；模式切换强制刷新。

压力对照使用实际导出包和真实 C# Wasm，在快照中加入 200 个远处怪物：关闭筛选 823 次绘制，开启后 165 次。此结果不是手机 FPS 或全场景 CPU 性能保证；近处聚集大量怪物时，战斗计算和绘制仍会增加，后续可据真机剖析再做批绘制与空间索引。

## 广告接入

用户已确认尚未开通流量主、没有广告位。配置入口是 `Project/Config/WeChat.json`：

- `ads.interstitialId`：开始冒险前的插屏广告 ID。空值、不支持、无填充或加载失败时直接开始冒险。
- `ads.rewardedId`：死亡复活激励视频 ID。空值时不启用广告复活选项，保持正常死亡结算；不会假装播放广告或自动发奖励。

后台具备接入资格并创建对应广告位后，将 ID 填入项目配置，通过编辑器原有微信打包入口重新打包预览。广告资格、广告位审核和实际填充以微信后台为准，不使用演示广告 ID 发布。

激励视频由玩家点击“观看广告复活”触发，仅 `onClose` 明确返回 `isEnded === true` 才发出游戏复活命令；取消、错误和缺失完成信息均不给奖励。广告服务防止并发播放并解绑事件。游戏每局最多一次广告复活，恢复一半生命并给予 3 秒受伤保护；角色原有复活能力优先。死亡选择期间暂停会话，放弃后才结算；结算后的延迟/重复奖励命令被拒绝。Web/Windows 未配置该能力，保持原死亡行为。

当前完成代码与模拟宿主验证，真实广告展示、填充、后台审核和正式收益链路未验收，因为缺少有效广告位。

## 好友排行榜

大厅“好友榜”使用微信开放数据域。首次点击请求 `scope.WxFriendInteraction` 权限，拒绝时提示在微信设置中开启。好友记录只在开放数据域读取、排序与绘制，主域只接收共享画布，不把好友名单传入游戏逻辑。开发者工具实测发现共享画布在开放域内尺寸只读，并禁止普通 2D HUD 的 `drawImage` 读取它；已改为主域设置尺寸、WebGL 直接合成，新增严格模拟测试覆盖这两个限制。

本项目按最长生存秒数降序排名；每次正常结算提交历史最好成绩。开放数据域读取云端旧成绩后只写更高值，避免低分覆盖历史记录。排行榜包含分页、空数据与读取失败提示。微信仅返回符合平台条件的同玩好友；这是客户端成绩榜，不是服务器防作弊排名。配置 `leaderboard.enabled/key/title/unit/metricLabel` 均属于项目；开放数据域代码及导出支持属于引擎，不硬编码本游戏的排名单位或指标。

官方参考（2026-09-15 对照微信维护的 API 类型说明）：

- [微信官方 API 定义](https://github.com/wechat-miniprogram/minigame-api-typings/blob/master/types/wx/lib.wx.api.d.ts)：`setDeviceOrientation`、`scope.WxFriendInteraction`、`getFriendCloudStorage`、`getUserCloudStorage`、`setUserCloudStorage`。
- [激励视频关闭回调](https://developers.weixin.qq.com/minigame/dev/api/ad/RewardedVideoAd.onClose.html)。
- [插屏广告](https://developers.weixin.qq.com/minigame/dev/api/ad/wx.createInterstitialAd.html)。
- [好友托管数据](https://developers.weixin.qq.com/minigame/dev/api/open-api/data/wx.getFriendCloudStorage.html)。

## 验证命令与证据

从仓库根目录执行；Node 使用本机 Node 24，`NODE_PATH` 指向含 Playwright 的依赖目录。

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| unit-test | `node Tools/Build/TestPortableFeatures.cjs` | PASS：横竖屏安全区命中、方向请求、ROI、骨骼节流、广告取消/失败/完成/并发、开放域排序与云端最高分 |
| unit-test | `dotnet run --project Tools/Scripting/SurvivorSessionTests/SurvivorSessionTests.csproj --configuration Release --artifacts-path out/agent/survivor-session-tests -- Project/Dist/Web/catalog.json` | PASS：54 项会话断言，含广告复活、重复结算保护与 ROI 保留真实对象 |
| build-only | `dotnet build Project/Scripts/CSharp/GameScripts.csproj -c Release --artifacts-path out/agent/landscape-native-managed` | PASS：Windows C# 脚本编译，无警告/错误 |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile web` | PASS |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Publish.ps1 -Project Project -Profile wechat` | PASS；目录占用时保留旧包，关闭项目窗口后重试成功 |
| runtime-manual | `node Tools/Scripting/TestSurvivorWeb.cjs` | PASS：Web 12 项浏览器交互回归 |
| runtime-manual | `node Tools/Build/TestCanvasOverlay.cjs` | PASS：真实 WebGL 贴图、骨骼、透明合成及状态恢复 |
| runtime-manual | `node Tools/Build/TestWeChatLandscape.cjs` | PASS：实际导出包/真实 C# Wasm、严格文件路径 Module、横屏大厅/开局/好友榜 UI/摇杆、竖屏尺寸切换与远处对象压力对照；此测试中的广告/好友服务是模拟宿主 |
| static-check | `node Tools/Build/WeChatJavaScript.cjs Project/Dist/WeChat` | PASS：17 个脚本 |
| static-check | `git -c core.safecrlf=false diff --check` | PASS |

日志：`out/agent/landscape-*-test.log`、`landscape-*-build.log`；浏览器截图：`out/agent/wechat-landscape-test/`。本轮未重新跑 Windows 原生游戏窗口，未进行手机 FPS 测试，未取得真实广告或跨好友账号排名验收证据。

runtime-manual：最终微信开发者工具已实际自动横屏显示大厅和三维角色；好友互动授权通过后，真实开放数据域显示“好友生存排行榜 / 暂无好友成绩”，不再刷共享画布限制警告。截图 `out/agent/wechat-landscape-test/devtools-friends.jpg`。空榜验证不等同于跨好友账号成绩排名验收。

runtime-manual：开发者工具实际开局后显示横屏场景、主角、怪物、拾取物与触控按钮，截图 `out/agent/wechat-landscape-test/devtools-game.jpg`。微信宿主禁用诊断像素回读（开放数据域会限制该操作），保留绘制次数、资源数和 GL 状态；浏览器像素回归仍启用回读。
