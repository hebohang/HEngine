# 1.1.0 脚本 API 设计与覆盖审计

根据本次发布范围，1.1.0 优先完成 C#；Python 扩展和二进制导出暂缓，现有实现保持兼容。本页描述 C# 源码实现及发行契约，测试结果见[本轮验收记录](ScriptingSdkValidation-1.1.0.md)。API 存在、编译通过、桌面运行、手机验收是不同证据。不能把这一页当成所有平台均已通过的证明。

## 产品决策

普通游戏开发者和 AI 使用二进制 SDK、项目模板、版本匹配的 API 参考和运行验证工具。无需下载引擎 C++ 源码。SDK 保留公共 C# 包装代码，便于查看签名、示例和调试；这是小型脚本 SDK，不是要求用户阅读或重编整个引擎。需要自定义原生渲染后端、原生插件或新增底层系统的用户另选源码发行版，并重新构建匹配的 Player。

参考 Unity 的 [Component](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Component.html)、[AudioSource](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/AudioSource.html) 和 [SceneManager](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/SceneManagement.SceneManager.html) 的职责划分：实体/组件负责身份和状态，系统服务负责跨对象行为，播放实例负责自己的生命周期。保留 HEngine 的命名、ECS、资源与平台契约，不承诺 Unity 源码兼容，也不复制其全部功能范围。

## 逐系统覆盖

C# 根命名空间为 `HEngine`。下表按系统列出已实现入口与真实限制。Python 不作为本轮新增能力或发行验收的承诺。

| 系统 | C# 公共入口 | 范围与差异 |
| --- | --- | --- |
| 对象与层级 | `Entity.Create/Instantiate/Destroy/Parent/GetChildrenSnapshot` | 会话句柄，销毁立即失效；跨场景父子关系拒绝；动态创建不在受限 Wasm 场景宿主中支持 |
| 组件与变换 | `GetComponent<T>/TryGetComponent<T>`、`Transform`、`Sprite` | C# 泛型组件视图目前只支持 Transform/Sprite，其他系统通过专门服务访问；不伪装成任意反射组件的泛型支持 |
| 输入 | `InputActions`、`Engine.Input` | 动作 Held/Pressed/Released/HoldPerformed/DoubleTapped/PressCount，输入映射与 UI 捕获遵守原生规则；支持重绑定与保存 |
| 时间/生命周期 | `ScriptBehaviour`、`Time`、调度/协程与 Lifetime | 帧/固定步、暂停、游戏时间与实时时间等待；销毁时取消任务；宿主控制帧时钟 |
| 场景流 | `SceneFlow.Load`、`SceneServices.FindTag/Instantiate/Camera` | 安全帧边界切场景，单场景流程；未实现 Unity 式 additive 多场景和 DontDestroyOnLoad |
| 资源/预制体 | `ResourceReference`、`ScriptDependency`、`PrefabLoad` | 可取消异步预制体与兼容同步加载。声明 cook 依赖，不能靠运行时字符串猜测资源 |
| 物理/角色 | `Physics`、`IScriptPhysicsEvents` | 2D/3D 速度读写、质心力/冲量、角色移动、射线与接触事件；原生物理系统须启用；没有身体不会隐式创建；尚无通用 Overlap/关节脚本接口 |
| 动画 | `Animator`、动画事件、骨骼附着辅助 | 控制已有 Animator；消费事件是破坏性读取，应指定一个消费者；不保证全部动画图编辑能力 |
| 序列 | `SceneSequence` | 控制已有 SequenceComponent，同一场景单个活动序列 |
| 音频 | `Audio`、`AudioOptions`、`AudioPlayback` | 2D/3D、总线、暂停、音量、音高、声像、位置/速度、锥体、进度与监听器；C# owned 播放随脚本回收；旧 raw handle 要主动 Stop |
| UI | `UiDocument`、`SceneServices.SetText/SetStyle/...` | 公共 scene UI 发 `ui.event` click 消息；原生 UI 与 PortableView 不是同一组件树 |
| 特效 | `VfxPlayback` | Effekseer 插件/资源必须存在；owned 播放由脚本/场景结束回收，也可提前 Dispose |
| 摄像机 | `Camera`、`CameraPreview` | 主摄像机 follow/spring arm，角度单位见具体方法；不会创建缺失的主摄像机 |
| 网格/材质/渲染 | `PreparedAppearance`、`RenderPreferences` | 预加载外观、兼容骨架换装、用户渲染设置；不是任意 GPU/RHI 调用；完整脚本 shader/material 参数体系仍需专门扩展 |
| 导航/AI | `Navigation`、`NavigationPathFollower`、`CombatAgent` | 导航插件及场景系统必需；目前没有完整 BehaviorTree 黑板强类型包装 |
| 存档/事件 | `SaveSlot<T>`、`ScriptEvent<T>`、`Engine.Api.ReadSave/WriteSave` | 有界值复制，稳定存档键；实体/资源运行时句柄不可保存；不是通用云存档/联网 |
| 设置/平台 | `GameSettings`、`RenderPreferences` | 项目默认值与 Saved 用户覆盖；窗口操作依赖宿主；无支付/广告/账号/网络复制 SDK 的统一保证 |
| 编辑/构建/自动化 | `hectl`、Editor、构建配置 | binary Development Player 用 agent 会话；Shipping 禁止携带开发控制端 |

## 这次补齐的发布缺口

- C# 从基础 play/stop 扩展到完整原生音频控制；保留旧 `SceneServices.PlayAudio` 的循环音乐总线默认行为。新 `AudioOptions` 默认 Gameplay，显式选择 Music/UI。
- C# 增加本地变换便捷属性和用户配置/显示设置入口。用户设置写入必须是 JSON 对象，避免写成功后读取时被丢弃。
- Animator 增加播放状态、暂停/恢复/停止/重播、速度、整数参数和类型检查的参数读取/触发器复位；序列增加暂停/恢复及帧边界 Seek；Camera 增加 FOV 与中心射线；owned UI 文档补 markup/attribute/scroll/cursor 操作。
- Physics 增加 2D/3D 速度查询、质心 AddForce/AddImpulse；仅向活动动态刚体施力，拒绝静态/运动学/已删除刚体，不向脚本暴露原生物理指针。
- 发行 SDK 包含从当前程序集 XML 生成的逐类型 C# API 文档和机器可读索引、教程、示例、`llms.txt`。缺少新版接口会阻断组装。
- ABI 请求布局仍是 version 1；`Api.schema.json` 的 sceneServiceVersion 升为 2，使公共服务扩展参与现有 ScriptApiHash 和 Player 指纹。组装和导出均拒绝旧模板；以后扩展服务契约也必须更新此版本，而非仅在字符串分派中加分支。

## 不应以“API 完整”掩盖的边界

1.1.0 的完整性标准是：公开承诺的游戏开发流程可以闭环、接口可发现、失败可诊断、资源可导出、每个平台有明确支持边界。它不等于覆盖 Unity 数十年积累的所有系统。

上表列出的 additive 场景、通用组件反射写入、关节/重叠查询、运行时材质参数、网络复制等不应由空壳 API 占位。需要这些能力的游戏必须先做原生扩展并增加运行和导出验证，或调整设计。它们属于后续明确需求驱动的引擎开发，不是二进制用户能够通过“让 AI 读源码”自动补上的能力。

Windows Scene 和标准 Android Scene 使用原生 C# 服务；实际插件与工具链以当前包为准。Web/微信的标准导出使用共享 C# IGameSession + PortableView，不能直接运行上述完整原生场景 API。Python 的新增接口与二进制导出暂缓，现有 Editor Python 功能不代表标准 Player 包含 Python。macOS 源码支持也不能替代独立的构建与设备验收。
