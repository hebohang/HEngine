# 场景编辑服务 v1

入口为 `Runtime/EcsFramework/Serializer/SceneEditingService`。核心不依赖 ImGui、CLI 或 Agent 类型。`SceneEditingLevel` 是 ECS 生命周期适配器；Agent 使用已有 `AgentCommandRegistry`，Editor 使用已有 GM/DevCommand 注册与 `IEditorUndoClient`，CLI 使用已有文件邮箱会话。

## 状态与边界

| 状态 | 权威对象 | 更新规则 |
| --- | --- | --- |
| Editor 作者场景 | `EditorLayer::mEditorScene` | 服务保存序列化观察缓存和受影响实体历史；没有第二个长期同步 ECS 世界 |
| Agent 作者文档 | `scene.open` 创建的独立 `SceneEditingService` | 明确打开项目内 `.he`；不读取游戏运行世界作为作者状态 |
| 播放/预览 | Editor 原有播放副本；Agent 可丢弃的渲染预览 Level | 不向作者文档写回；Agent 预览不启动物理、脚本、动画 |
| 磁盘文件 | 指定项目路径 | 只有显式 save 发布；播放暂停不等于保存 |

`sceneId` 是一次打开/绑定的 UUID，`path` 是项目路径。重新打开、场景切换会更换 sceneId 并清空会话历史。Editor 和 Agent 可以打开同一路径，但**不是跨进程共享未保存的 ECS 世界**；另一个进程保存后，本会话的保存冲突检查会拒绝覆盖。要操作 Editor 的未保存内容，使用 Editor 注册的 `scene.*` GM 命令。

每个非空 apply、undo、redo、已观察到的手动修改增加单调 revision。plan/query/describe/diff/validate/save 不增加 revision。相机 viewport aspect 是投影缓存，不制造作者 revision。Editor 播放时返回 `authoring_during_play`。外部模拟数据不得传给 `Observe`。

dirty 按作者持久内容与最后保存内容比较；savedRevision 记录最近保存时的 revision。保存保留撤销历史；保存后 undo 会 dirty，redo 回到保存内容则 clean，即使 revision 已继续增长。新分支清除旧 redo。历史不跨进程或重新打开保留。

## 操作和组件

| op | 参数与语义 |
| --- | --- |
| create | name、parent、components 可选；temporaryId 使用 `$name` |
| rename | entity/entities、name |
| delete | entity/entities、children=`reject` 或 `subtree`，必须显式选择 |
| duplicate | 同上；生成新 UUID，可用 temporaryId 引用复制根；只重映射子树内部父 UUID，保留外部父引用 |
| add / patch / remove | entity/entities、component；add/patch 使用 patch 对象 |
| transform | entity/entities、patch；space=`local`/`world`，mode=`absolute`/`relative` |
| reparent | entity/entities、parent（空字符串表示根）、mode=`keepLocal`/`keepWorld` |
| material | entity/entities、slot（零起始）、path（空字符串清除覆盖） |
| prefab | path 指向现有单实体 `.hprefab` 快照；复用现有路径和反射组件格式，不引入 Prefab 实例链接机制 |

开放 `TagComponent`、`TransformComponent`、`MeshComponent`、`CameraComponent`、`PointLightComponent`、`DirectionalLightComponent`、`EnvironmentLightComponent` 等组件。字段默认值与形状来自生成的组件序列化器。`describe(component=...)` 补充只读、范围、单位、资源约束。相机支持投影参数嵌套补丁，aspect 只读；近远裁剪区间和 FOV 校验后由宿主刷新投影。环境灯的距离/高度雾字段支持场景编辑、保存和重新打开，密度、距离及高度衰减按编辑器控件范围校验。`SecondaryFogColor`、`SecondaryFogDensity`、`SecondaryFogStartDistance`、`SecondaryFogBaseHeight`、`SecondaryFogHeightFalloff` 是独立的第二层解析距离/高度雾，可给近景空气不同于远景的色调；默认密度0，不改变旧场景。这不是遮挡感知的体积雾，不能代替阴影光束。`FogDirectionalColor`、`FogDirectionalIntensity`（0–16）及 `FogDirectionalExponent`（1–64）控制朝向主方向光的天空和不透明物体距离雾散射；方向取光线传播方向的反向，默认强度0，不改变旧场景。`SkyColorTint`（各通道0–4）只调天空画面颜色，默认(1,1,1)，不改变HDR环境贴图的IBL照明。方向雾仍是解析近似，没有局部灯光散射、遮挡阴影或真正体积雾的积分。

Tag/Transform 不允许移除。ParentUuid 必须经 reparent；MaterialOverrides 必须经 material；meshResPath 不直接写入。Mesh 新增/Path 修改仅开放 `.hmesh`，通过已有 CPU 解码器验证数据和槽位。资源路径遵循项目优先、已配置的 Engine/Content 回退；独立无配置的文档宿主只解析自己的项目根。材质使用现有 `.hmat` 反射资源字段及类型/版本约束，宿主经 Mesh 生命周期应用实例覆盖。服务不改写资源文件。

Transform 使用米、Euler 弧度和现有 `glm::quat(vec3)` 旋转约定，矩阵为 T·R·S。局部相对修改为 oldLocal·delta；世界相对修改为 delta·oldWorld。世界到局部转换检查父矩阵可逆性，分解后重组比较；剪切、奇异或不能表达为 TRS 的结果拒绝。层级环和悬空父引用拒绝。

批量操作按数组顺序执行，后续操作可使用之前 create/duplicate 的 `$temporaryId`。实体筛选只用于 query；apply 必须绑定明确 UUID 列表及 baseRevision。不能在 apply 中重新运行名称筛选。

结构性删除/复制的目标若含没有可靠适配器的组件，返回 `unsafe_lifecycle`；删除还检查范围外序列化 UUID 引用并返回 `entity_referenced`。脚本通过名字、外部文件或不可序列化运行时状态建立的引用不属于自动修复范围。Prefab 同样限制为开放组件集合。物理、脚本、音频、动画不开放通用写入。

## 请求、并发与历史

所有操作除发现外必须带 sceneId。发现回执说明绑定路径；指定 sceneId 的发现也会校验目标。写入必须带 baseRevision 和 requestId；plan 使用同一批请求但不消费 requestId。服务在 apply 时重做预检，不信任先前 plan。

requestId 记录作用域是 sceneId 生命周期，保留最多 1024 个已受理写请求（含失败回执），不驱逐旧记录。相同内容重试返回原回执；不同内容返回 `idempotency_conflict`。容量满返回 `idempotency_capacity`，保存并重新打开后用新 sceneId 重新查询；旧 sceneId 请求不会在新会话执行。不要把同一 requestId 当作可以变更内容的草稿名。

预检使用受影响实体覆盖层，不修改 ECS、磁盘、撤销或持久资源。提交前准备历史及 JSON 节点；宿主写入失败调用逆向实体数据恢复。恢复失败返回 `recovery_failed` 并阻止后续写入，必须重新打开恢复可信状态。GPU、资源缓存、预览帧和磁盘不属于作者事务的一部分。

apply 是一个撤销单元。历史最多 64 个事务、16 MiB；增量事件另有相同容量。单事务最多 1024 个受影响实体、8 MiB 修改后数据及 16 MiB 前后历史。容量淘汰后 diff 返回 `delta_expired`，不伪造完整增量。过大的实体 diff 返回 output_limit，可改用投影 query。

保存复用 `Utils::SaveJsonToFile` 的原子替换；新文件使用 CreateNew。替换前与打开/上次保存的磁盘内容及文件时间比较，发现外部修改返回 `external_file_changed`。这不是与其他不合作进程共同持有的跨进程文件事务锁。Save As 使用 save 的可选 `path`，保留历史，但目标必须是新路径；现存目的文件必须先显式打开。失败不改变绑定路径或保存基线。

核心绑定创建线程，所有访问在宿主主线程完成；Editor 在 `OnPreUpdate` 排空请求，Agent 在既有命令帧边界执行。提交或外部变更观察完成后发布 revision 通知；通知期间拒绝重入写入，观察者异常作为 warning 报告。Editor 据此刷新渲染 revision 和预热帧。作者编辑不启动物理模拟；已有编辑态碰撞调试绘制读取当前 Transform，播放时原有系统从播放副本建立物理状态。

## CLI 和 Agent 示例

PowerShell 自动化可直接调用 `Tools/Agent/hectl.ps1`，避免将长寿命进程启动的批处理输出放进 PowerShell 原生命令管道。交互终端可使用 `hectl.bat`。所有会话放在 try/finally 内回收：

```powershell
try {
    & .\Tools\Agent\hectl.ps1 agent start
    # open.json: {"path":"Assets/Scenes/MyScene.he"}
    & .\Tools\Agent\hectl.ps1 scene open -RequestFile open.json
    & .\Tools\Agent\hectl.ps1 scene describe -ParamsJson '{}'
    # 用 open 返回的 sceneId 和 revision 填写 batch.json
    & .\Tools\Agent\hectl.ps1 scene plan -RequestFile batch.json
    & .\Tools\Agent\hectl.ps1 scene apply -RequestFile batch.json
    & .\Tools\Agent\hectl.ps1 scene preview -RequestFile preview.json
    & .\Tools\Agent\hectl.ps1 agent capture -AgentCaptureName authored.png
    & .\Tools\Agent\hectl.ps1 scene save -RequestFile save.json
} finally {
    & .\Tools\Agent\hectl.ps1 agent stop
}
```

同一 Agent 方法调用：

```powershell
& .\Tools\Agent\hectl.ps1 agent call -Method scene.query -RequestFile query.json
& .\Tools\Agent\hectl.ps1 agent call -Method scene.apply -RequestFile batch.json
```

项目路径均相对当前配置项目根；CLI 请求文件是严格 UTF-8 JSON，不需要复杂 shell 转义。现有 Agent 传输请求上限为 64 KiB（含信封），核心允许至 256 KiB；通过 Agent 使用时遵守较小限制。

请求正文示例（替换 `<sceneId>`、revision、UUID）：

```json
{"sceneId":"<sceneId>","component":"TransformComponent"}
```

```json
{"sceneId":"<sceneId>","name":"Cube","component":"MeshComponent","fields":["uuid","name","TransformComponent"],"limit":32}
```

第二页必须带上一页 revision 和 nextOffset；revision 变化返回 `revision_conflict`，重新查询。

```json
{"sceneId":"<sceneId>","revision":3,"offset":32,"fields":["uuid","name"],"limit":32}
```

```json
{"sceneId":"<sceneId>","baseRevision":3,"requestId":"arrange-01","operations":[{"op":"transform","entities":["<uuid-1>","<uuid-2>"],"patch":{"Scale":[0.5,0.5,0.5]}}]}
```

plan/apply 均可使用该正文。默认 apply 只返回 revision、transactionId、affectedCount、dirty、temporaryIds 与完成状态。

```json
{"sceneId":"<sceneId>","transactionId":"<transactionId>","limit":16}
```

diff 也可使用 sinceRevision；分页带 revision、offset。save/undo/redo 各使用新的 requestId 和当前 revision：

```json
{"sceneId":"<sceneId>","baseRevision":4,"requestId":"save-01"}
```

preview.json 使用 `{"sceneId":"<sceneId>","revision":4}`。preview 显式物化最新作者快照，回执 `resourcesMaterialized` 不等于已经呈现；随后 `capture.frame`/CLI agent capture 使用现有帧完成屏障。后续编辑使预览失效，必须重新 preview。预览仅包含本版本开放的可视组件集合，其他组件保留在文档中但不运行。

## Editor 适配与未迁移入口

`PostProcessingComponent` 可由 scene apply 创建、编辑和保存。`bOverrideTemperature=true` 时，`Temperature` 使用 -1 到 1 的冷暖范围（负值偏冷），可与 `bOverrideContrast`、`bOverrideSaturation` 分别启用；场景值由反射序列化，保存并重新打开后仍生效。没有覆写的场景继续使用项目渲染设置。SICKA 项目给 `Environment` 实体保存这组参数，不需修改其他项目的全局色温。

后处理面板可分别开启 Bloom、对比度、饱和度、冷暖色和 Vignette 覆盖；`VignetteIntensity` 为 0..1，0 保持原画面，边角先于画面中心变暗。它由通用 RenderWorld 提取、Volume 混合与后处理着色器接入，未开启覆盖的旧场景保持默认 0。来自 UE 的强度值要按场景同机位校准。

面板也提供独立的 RGB `ColorGain`（每通道0..2）和 `ColorOffset`（每通道-0.25..0.25）；它们在已曝光的线性颜色上、色温/饱和度/对比度之后应用。默认分别为(1,1,1)与(0,0,0)，旧场景不变。两个覆盖开关、体积权重、反射保存与重开走同一通用后处理流程。UE 的同名调色参数不保证采用相同色彩空间或运算顺序，导入关卡应对实际视口单独校准。

Editor 的 scene.describe/query/plan/apply/diff/validate/save/undo/redo 通过已有 GM 注册，返回 ticket；下一次 `OnPreUpdate` 执行，通过 scene.result 的 `{"ticket":"..."}` 取回结果。结果最多保留 64 项。核心请求与 Agent 相同，不带 GM 存档 StateDomains，不将存档事务当作作者事务。

空实体、立方体、点光源创建、Hierarchy 重命名、普通 Transform 层级拖放、属性面板 Transform、普通实体复制/删除、材质拖放/选择，以及相机和基础灯光的组件添加/移除调用公共服务。普通 UI 操作使用当前作者 UUID，在下一帧合并成批；外部调用严格使用提供的 baseRevision。保存和 Save As 也进入帧边界队列，排在此前的编辑操作之后。现有撤销菜单/快捷键使用同一历史，不再保留整场景 Level 快照历史。

保留的边界：Gizmo、部分组件控件、Folder、原始 OBJ 预置、内容浏览器及插件的直接写入仍可能走原有入口。已报告完成的修改通过 `Observe(..., true)` 进入同一历史；没有通知的变化通过 `Observe(..., false)` 增加 revision，并清除不再可信的撤销/增量历史。因此这些未迁移控件的连续拖动可能形成历史失效边界，不保证与已迁移操作相同的交互合并行为。Editor UI 状态（选择、展开）不是作者事务数据。

## 验证与调用成本

```powershell
.\hectl.bat test SceneEditing
.\hectl.bat game check -Frames 30
.\hectl.bat editor check -Frames 30
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-SceneEditing.ps1
```

无窗口测试覆盖批量临时引用、零修改失败、旧 revision、幂等/冲突、删除恢复、变换/环、保存重开、dirty、查询分页、增量过期、手动历史、模拟隔离、提交/回滚故障注入、ECS 适配器与分支历史。端到端脚本保存请求、回执、场景和截图到 `out/agent/scene-editing/<时间>/`，finally 停止会话；只删除自己创建的唯一临时场景。

例如同一组 100 个实体缩放：传统逐个读取/修改需要至多 200 次实体调用，或传输整份场景；本接口使用一次只投影 UUID 的 query、一次共享 Scale 补丁的 plan 和一次 apply，共 3 次服务请求。只有查询分页超过上限时增加查询次数。diff/观察/保存是独立的可选请求，不混入这个比较。此处是请求结构对比，不是实测 token 比例。端到端 summary.json 报告实际请求数量、序列化请求字节及场景字节。
