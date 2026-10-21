# 真实 Editor 子树事务验收入口

此入口已完成下文所列候选的真实 Editor 运行。它进入 HEngineEditor 的主线程和真实场景写入/历史链路；历史证据仅覆盖对应候选，当前交付结论见 [收尾验收](VFXAuthoring-Delivery.md)。CPU/GPU 固定步进诊断使用同一 mailbox，详见 [Editor 特效诊断](VFXAuthoring-EditorDiagnostic.md)。

## 启用边界

EditorSceneMailbox 只编入 Editor executable；所有接收逻辑受 HE_ENABLE_GM 宏保护，默认没有环境变量即完全不启用。启用同时要求有限 ExitAfterFrames、32位小写hex token、已存在的证据 requests/responses 目录，以及对应token命名的独立 scratch scene 根。证据目录限定 `out/agent/rendering-evidence/vfx-authoring` 子目录；场景根限定 `Project/Assets/Effects/Authoring/EditorAcceptance/<token>`。无网络监听、后台线程、Python轮询线程或 shipping 接入。

请求字段复用 Game 协议习惯：protocolVersion=1、requestId、token、method、params。requestId 也是文件名，先写临时文件再原子发布。每帧最多处理1条、每次运行最多256条、每个请求256KiB、JSON深度48、回复8MiB。令牌不写ready/回复/终端；重复同ID相同请求复用原回执，不再执行。目录异常或预算超限会停止入口而非无限循环。

允许 scene.describe/query/plan/apply/diff/validate/save/undo/redo/result，通过现有 GMService 执行，然后由 EditorLayer 的 OnPreUpdate 消费 scene ticket。scene.result 返回真正提交结果。另有两条有限生命周期方法：editor.open 只允许本次scratch中的.he，调用现有 RequestOpenScene；session.stop 调用 Application.Close。scene.save 显式目标也限本次scratch。启动场景必须位于该scratch；不提供任意文件、任意Python或运行态操作。

## 运行步骤

先由主代理统一构建 Editor 候选、取得实际 HEngineEditor.exe SHA256，再运行：

```powershell
py -3.11 Tools/VFX/editor_prefab_acceptance.py --backend Vulkan --expected-editor-sha256 <Editor-SHA256> --output out/agent/rendering-evidence/vfx-authoring/editor-prefab-vulkan-01
```

DX12 使用另一个新output。driver 调用 `hectl editor check -Configuration RelWithDebInfo -Parallel 2 -Frames 6000 -TimeoutSeconds 180 -StartupScene <scratch>/EditorAcceptance.he -GraphicsApi <backend> -StrictGraphicsApi -RenderValidation`。该标准入口可能触发构建，所以必须先由主代理完成统一构建并稳定源码；driver 在启动前和mailbox ready后核对候选hash，hash变化拒绝验收。它不是跳过构建的固定二进制启动器。

driver 外层登记 Storage，保留所有不含token的请求/回复、新保存场景和scratch证据；finally发session.stop并等待hectl回收进程，异常时依靠hectl的180秒有界回收，最后才针对自己创建的子进程树兜底终止。未操作用户现有Editor进程。所有输入模板/场景复制到唯一scratch，模板候选更新后在finally恢复该scratch模板；不修改原角色文件。

验收顺序为真实Editor plan/apply创建两个嵌套实例→A整Translation显式覆盖→scratch内嵌套模板变更→两实例sync→undo→redo→save→editor.open→query→再次save并比较完整JSON。检查引用/UUID隔离、A覆盖保持、B新字段传播、新badge节点、速度与名称覆盖、显式override元数据保存重开。数组按整体覆盖，不宣称逐元素合并。Python脚本路径使用真实 `Scripts/VFXCharacterAcceptance.py`，不会放宽通用资源路径规则。

报告 `report.json` 的四个checks均需通过，且hectl owned过程exitCode=0；视觉仍为NOT_REVIEWED。如需布局/模型画面证据，主代理可对保留的scratch/Reopened.he追加标准 `hectl editor capture`，这与事务断言分别报告。

## 当前证据

- Editor04 Vulkan `editor-prefab-vulkan-02` 真实事务PASS4组；DX12旧尝试因入口重建导致hash不匹配而拒绝，须最终候选双API重跑。不得以Game事务替代Editor。
- Editor05/06动画桥在DX12/Vulkan均完成5组状态检查、7次实际presented-backbuffer截图，正常退出且输入/候选hash保持。逐版SHA和精确命令见实施记录。
- 根人工图像检查发现默认浮窗超工作区、时间轴被长路径元数据挤出首屏；Editor07工作区适配及时间轴前置已实际确认。后续诊断证实初始布局变化覆盖聚焦；Editor09等待真实canvas尺寸稳定后执行并核对全节点可见，Vulkan animation05/DX12 animation03 的初次及显式聚焦、人工视觉均PASS。窗口、时间轴、5节点完整可见，暂停视口RGB差为0。
- 本任务新增Character.hanimctrl只调整五个节点的显示位置以消除重叠；其他JSON语义不变，未改用户现有图的坐标。
- 原角色脚本统一为实际 `Scripts/VFXCharacterAcceptance.py`；最终Game角色/事件验收需要当前资源hash，早期成功不是当前资源的完整证据。

## 动画预览与控制器诊断

同一个严格认证入口支持 `.hanim` 的 `editor.preview` open/state/play/pause/scrub，调用真实AssetPreviewPanel。固定帧play会按实际preview update计数自动暂停；普通鼠标播放仍使用真实UI时间。`.hanimctrl` 仅支持open/state/focus；focus复用真实按钮，不改运行时动画状态或资源。Controller只保留最近帧画布、节点联合边界、可视范围和聚焦计数，不记录无界历史。

`editor.capture` / `editor.capture.result` 通过Application真实捕获及完成回执取图，路径限本次mailbox/captures；请求失败不伪装完成。`Tools/VFX/editor_animation_acceptance.py` 分别保存首次全图适配和显式聚焦结果，后者成功不能覆盖前者失败。命令成功与视觉审核分开；此桥不证明鼠标易用性、游戏事件或控制器过渡。
