# 四端共享规则模板

这是可编辑的 2D 守护游戏起点：绿色核心、蓝色玩家、红色敌人；靠近敌人自动攻击，
每击败五个敌人升级攻击范围，保护核心到计时结束。WASD / 方向键移动，移动设备
使用触摸摇杆；可暂停、重开，纪录写入项目独立存档。图形使用原生 Sprite 或 Canvas
矩形表现，声音为本模板自行合成的短提示音，不含外部美术素材。

游戏规则只改 `Scripts/CSharp/Rules/GameSession.cs`，参数在 `Config/GameRules.json`。
四端编译同一份规则源码。`Game.cs` 连接原生场景；Web/微信使用引擎提供的
PortableView 宿主。模板不会把原生物理、VFX 或完整 3D 渲染移植到 Canvas。

`hectl publish check -BuildProfile windows` 检查平台，`hectl publish build -BuildProfile windows`
导出。其他 profile 为 `android`、`web`、`wechat`。先按 `hectl capabilities -Json` 的
能力范围设计，再实际构建和运行。Android 默认开发签名；微信游客 AppID 仅用于
本地验证，上线前填写自己的 AppID 和签名资料。

AI 使用项目的 `AGENTS.md` 和 SDK 的 `Tools/Distribution/AgentInstructions.md`，
在独立开发会话中验证交互，再交付四端产物；不要把仅创建代码当作完成游戏。
