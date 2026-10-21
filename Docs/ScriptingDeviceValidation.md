# C# 微信真机验收

当前状态：预览准备通过；Android、iOS均未取得真机运行证据。扫码成功本身不算游戏验收。
二维码固定在下列身份；本地导出目录已更新结构化异常实现，不会自动替换该预览。

## 当前可运行预览

- [预览二维码](../out/agent/scripting/wechat-preview-20260911-183111-433/preview.png)
- [预览回执](../out/agent/scripting/wechat-preview-20260911-183111-433/result.json)
- AppID：`wx54887856924392ee`。扫码微信需要相应开发/预览权限。
- buildId：`e02dc59c3fc3d1bee421ca568a6839113c8068e322eb3da06a49256885411f01`
- 托管身份：`7c833ea9e24f1fa3ffbaf43d195cc2f3b94b100450fef9c99d93efe904a3491d`
- C# 行为标识：`csharp-touch-v2-three-points`。

二维码由当前开发者工具实际生成，有效期以微信服务为准。它不是正式发布版本。
工具报告总大小9,638,129字节，其中main为2,298,072字节，runtime分包为7,340,057字节。
这些是工具的`info.size`值，不能据此宣称网络传输压缩后大小。

## 首轮设备检查

1. 记录设备型号、系统版本、微信版本，以及是否首次运行该预览。
2. 扫码后确认实际棋盘纹理精灵出现；空白画布、启动背景或运行时加载日志不算通过。
3. 按住并拖动手指，确认精灵向触摸位置移动。每次新的按下增加3分，颜色在青色和橙色间交替。
4. 如能查看日志，记录初始化分数，以及`[C#] csharp-touch-v2-three-points score=... saved=true`。已有存档不保证从0开始，应比较增量。
5. 切到后台再恢复，确认画面和触摸仍响应；关闭并重新打开同一预览，确认初始化日志恢复已保存分数。单次恢复颜色相同不足以证明存档正确。

首次只需反馈设备信息和上述实际观察；没有日志或无法验证的项目保持待验收。

## 有真机调试连接时补齐

必须先确认控制台连接到真实手机。开发工具模拟器中的同名对象不构成真机证据。
在真实游戏上下文取得会话，并核对`identity()`与上方回执一致：

```javascript
const s = (typeof GameGlobal === 'undefined' ? globalThis : GameGlobal).HEWeChatSession;
s.scripts.identity();
s.scripts.snapshot();
```

依次执行并记录前后快照及实际触摸结果：

- `s.scripts.enabled(false)`后触摸不移动；`s.scripts.enabled(true)`后恢复移动，Enable计数只增加一次。
- `await s.reload(true)`后字段、Transform、Sprite和禁用状态保持；重新启用后仍可交互。
- `s.scripts.collect()`触发真实托管GC；分散执行20次并检查后续操作，避免把循环强制GC的阻塞当作自然帧预算。
- `s.scripts.fault()`后产生一次含`GameScripts.ScriptTouchSample.Update`的异常，帧数仍增加且无持续错误刷屏；用`await s.reload(false)`恢复。
- `s.scripts.destroy()`后精灵消失、触摸不崩溃；`await s.reload(false)`后创建新实例并可继续交互。

记录`s.getMetrics()`及`s.scripts.memory()`。后者仅表示两个Wasm线性内存的已提交尺寸，不是手机RSS、系统峰值或独立托管堆。
设备峰值内存和GC停顿需要实际设备性能工具及对应日志，不使用桌面浏览器或模拟器数据代替。

## 重新准备预览

```powershell
.\hectl.bat scripting build-wechat
powershell -NoProfile -File Tools/Scripting/PrepareWeChatPreview.ps1 -VerifyOnly
$env:HE_WECHAT_CLI = 'E:/Program Files (x86)/Tencent/微信web开发者工具/wechatide.cmd'
.\hectl.bat scripting preview-wechat
```

入口逐文件检查manifest、大小和SHA256，拒绝未声明文件及重定向路径，再请求实际预览码。
证据写到项目外的独立目录，日志中的二维码数据留在磁盘；不会作为发布文件混入游戏。
首次预览的原始CLI流程已执行成功；维护入口的只读输入校验已通过。尚未再次上传同一包，以免干扰已交付二维码的设备验证。

| 平台 | 当前结论 | 仍需记录 |
| --- | --- | --- |
| Android 微信 | NOT_TESTED | 型号/系统/微信版本、画面、输入、存档、生命周期、异常、GC与内存 |
| iOS 微信 | NOT_TESTED | 型号/系统/微信版本、画面、输入、存档、生命周期、异常、GC与内存 |
## 预览身份说明

当前提供的二维码是 `wechat-preview-20260911-183111-433` 的固定预览，仍对应 buildId `e02dc59c3fc3d1bee421ca568a6839113c8068e322eb3da06a49256885411f01`。
后续结构化异常修复已更新本地 `game-wechat` 导出目录；该变更不自动更新已经生成的二维码。
请用预览目录中的 `identity.json` 对照此二维码，不能用当前导出目录的 manifest 为旧二维码背书。
