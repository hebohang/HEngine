# Blender → HEngine content workflow (v1)

## Skeletal animation retargeting

`retarget_animation.py` converts an imported `.hanim` or a directory of clips to any target `.hskel`
using an explicit JSON `boneMap`. It evaluates source and target bind poses in
world space, transfers each mapped joint's rotation delta, and bakes a regular
`.hanim` with the target hierarchy and inverse-bind palette. The optional
`hipTarget`, `hipMotion` (`none`, `vertical`, `full`), `upAxis`, and
`translationScale` settings control hip travel. Gameplay-driven movement
usually uses `vertical` so the visual mesh does not walk away from its capsule.
Optional `poseAlignment` maps a target parent bone to its direct child and
aligns that segment with the source bind direction, useful when transferring
motions between A-pose and T-pose rigs.
`rotationMode: "absolute"` copies source world rotations when the source clip
already uses the target rig's world coordinate space; the default `bindDelta`
mode transfers motion between different bind poses.
Source FBX namespace prefixes are accepted when a mapped short name is unique.
Unmapped joints retain the target bind pose. The tool rejects ambiguous or
missing mapped nodes and does not infer a humanoid mapping from names.

```powershell
python Tools/Content/retarget_animation.py --source <imported.hanim> --skeleton <target.hskel> --config <mapping.json> --output <retargeted.hanim>
python Tools/Content/retarget_animation.py --source <imported-directory> --skeleton <target.hskel> --config <mapping.json> --output <retargeted-directory>
python Tools/Content/test_retarget_animation.py
```

The legacy `HAssetImporter` converts animation-only FBX to `.hanim` even when
there is no mesh or `.hskel` in the source; its hierarchy is enough for this
offline retarget. Import and retarget are separate steps so the authored FBX
and its raw animation remain available for inspection. Pose matching, hands on
weapon, foot contact, and animation choice still require visual review.

从仓库根目录运行。Blender 制作并保存源文件；`hectl content` 负责导出、cgltf 转换、原生资源发布和验证。游戏不加载 `.blend`、glTF 或 cgltf。

默认导入报告在完整验证结束后遵循 [hectl 历史保留策略](../Agent/Storage.md)，显式 `-Report` 输出保留。`Project/Assets/Imported`、`.he-content/state.json` 和 pending/journal 均不参与垃圾回收；这里只轮换完成任务的报告和证据，不做内容寻址资源 GC。

```powershell
# 第一次初始化稳定身份：明确允许向 .blend / glTF / GLB 保存 he_id 元数据
.\hectl.bat content sync -Source Project/SourceAssets/BlenderAcceptance/Playable.blend -InitializeIds -Verify runtime -TimeoutSeconds 240

# 日常一次调用；自动发现 Blender，自动增量构建导入工具
.\hectl.bat content sync -Source Project/SourceAssets/BlenderAcceptance/Playable.blend -Verify runtime -TimeoutSeconds 240
.\hectl.bat content plan -Source Project/SourceAssets/BlenderAcceptance/Playable.blend
.\hectl.bat content sync -Source D:/Art/Room.glb -InitializeIds -Verify visual

# 精确工具路径（也支持 HE_BLENDER 和 Steam 安装目录发现）
.\hectl.bat content sync -Source D:/Art/Room.blend -BlenderExe D:/Blender/blender.exe -Verify none

# 机器请求文件，避免路径/JSON shell 转义
.\hectl.bat content sync -RequestFile Tools/Content/example-request.json
.\hectl.bat content diagnose -Report <report.json> -ErrorCode owned_field_conflict
.\hectl.bat content diagnose -Report <conversion.json> -ObjectId <source-uuid>
```

运行时加载输出摘要中的 `scene`。`-StartupScene` 不修改项目默认场景：

```powershell
.\hectl.bat game check -StartupScene Assets/Imported/<document-id>/Scene.he
```

`-Project` 支持其他项目的目录或 Project.heproject。Agent 会话现在接收解析后的项目、产物和会话目录，不再强制使用示例游戏的固定路径。首版不做运行中的热更新；导入前保存并关闭正在编辑目标 Scene.he 的会话，验证使用新进程。不要把 `-ImporterExe` 指定的旧二进制当作当前源码构建证据；导入记录会保存其 SHA-256。

## 制作契约

- Blender 4.5–5.x；当前实际验证版本见 VALIDATION.md。适配器会查询当前 glTF 导出操作的参数，记录实际使用的参数与版本。
- 一 Blender unit = 一米，`unit_settings.scale_length=1`。Blender 的 Z-up 只在其 glTF 导出器转换一次；cgltf 与 HEngine 使用右手 Y-up、米，摄像机看向局部 -Z。直接 glTF 不额外翻 UV 或做 Assimp 轴向处理。
- 范围是当前场景中可见且不隐藏渲染的对象；不建立 Collection 到 Transform 的伪父级。可见子节点的隐藏父级会报错。所有父子关系与局部 TRS 保留，剪切/零缩放拒绝。世界镜像使用共享的反向索引版本以匹配当前固定 CCW 渲染管线。
- 支持通过 Blender 自身求值导出的普通修改器。Geometry Nodes、Collection Instance 必须先显式 Realize/转换为网格；链接对象/网格/材质必须 Make Local。动画、蒙皮、morph、未实现的扩展均拒绝；不会降级到 Assimp。
- 网格支持三角 primitive，索引或无索引；cgltf 解码 accessor 步长、分量类型、归一化和 sparse，输出现有 float 属性与 uint32 索引。必须有 POSITION/NORMAL。使用 normal map 时要求 UV0 和导出的 tangent。超大输入有明确大小上限。
- BasePBR：基础色、metallic/roughness、normal、AO、emissive（含 KHR_materials_emissive_strength）、OPAQUE/MASK/BLEND、双面。使用现有 `BuildBasePbrMaterialResV2` 与生成序列化器。normal scale / AO strength 仅接受 1；请先烘焙其他强度。
- PNG/JPEG 支持外部 URI 或 GLB bufferView；缓冲支持外部文件、GLB 和 base64 data URI。图片 data URI 暂拒绝。URI 必须在源目录内，不能联网或越界。重复图片按实际字节去重。
- 只支持 UV0，无顶点色或 texture transform。sampler 只支持 repeat、linear magnification、trilinear minification（包括 glTF 默认）；其余报错。扩展未贯通渲染时一律拒绝，即使 cgltf 可以解析。
- 材质以直接 Principled BSDF / 图片 / normal map / 通道分离为边界。任意节点图不保证转换；不支持节点或扩展属性要求烘焙，不能忽略导出错误。
- 透视与正交相机均保留投影、垂直 FOV/高度、宽高比和有限裁剪面。验证固定 1280×720，制作测试相机也应为 16:9。恰好一个摄像机设置 `he_primary=true`。
- 点光：glTF cd → HEngine `IntensityUnits=1`，必须有限 range；Blender 对象的 `he_light_range` 指定截断半径，默认 10 米。方向光：glTF lux → HEngine lux。不把 spot/area 静默近似成点光。曝光可在引擎侧配置；首次导入可用 `he_exposure_ev100` 表达相机曝光意图。Blender 最终渲染与 HEngine 不做逐像素相等承诺。

## 身份与元数据

Blender World/background 不导出，会返回 `world_lighting_not_exported` 警告；请在引擎配置环境光。验收场景使用固定 EV100=10、点光与方向光，背景为黑色。

导出 extras 使用扁平命名空间：`he_schema=1`、`he_id=<UUID>`。对象、mesh datablock、材质、场景文档分开分配身份；名字和数组下标不承担身份。glTF 缺少 ID 时，先显式 `-InitializeIds`；这会原子写回源文件。存在重复 glTF ID 时拒绝猜测，应在制作端修复。

Blender 使用不导出的 `.he_identity_owners` Text datablock 保存 ID → datablock 持久引用。复制对象带来的重复属性可据此区分原始对象，即使它改名；新副本分配新 ID，共享 mesh 继续保持自己的 ID。新建/复制后再次用 `-InitializeIds` 保存新身份。首次保存使用同目录临时 `.blend` 和原子替换，并核对源文件是否被其他进程更改。导出选择、求值等临时操作发生在后台副本中，不保存到源文件。

引擎实体 UUID = 文档 UUID 命名空间下的 source node UUID 映射。primitive 槽位键为 mesh ID + material ID；一个 mesh 内同材质的多个 primitive 当前要求先合并，否则拒绝缺乏唯一槽位身份的输入。实例材料可不同，几何按实际求值输出去重，不能仅凭 datablock ID 合并修改器不同的结果。

其他制作语义：

| 属性 | 意义 |
|---|---|
| `he_collision="none"` | 默认无碰撞 |
| `he_proxy=true`, `he_collision="box"` | 不渲染的单位立方体代理；对象 scale 表示全尺寸 |
| `he_proxy=true`, `he_collision="sphere"` | 不渲染的单位半径球代理；只接受均匀世界缩放 |
| `he_collision="mesh"` | 静态三角网格碰撞，保留房间空腔、门洞和台阶 |
| `he_motion="static"` / `"dynamic"` | 默认 static；dynamic 只接受根级 Box/Sphere 代理 |
| `he_character=true` | 根级角色测试入口；创建现有 CharacterMovement 与 CapsuleCollider，后续玩法参数由引擎维护 |

代理几何应以原点为中心。使用 Empty 代理加可选渲染子对象，位置/方向通过对象变换表达；代理不从渲染网格启动时重算边界。碰撞世界剪切和镜像拒绝，先 Apply 这些变换；渲染对象的镜像仍支持。不要给整个有空腔的关卡标记凸包。

`.hcollision` v1 保存魔数、Jolt build ABI 和离线加速结构。运行时校验后恢复，不每次建三角 BVH；兼容实例通过 RuntimeResourceRegistry 的 Generic key 和场景持有组共享基础 shape。缩放 shape 轻量包装基础资源。Jolt ABI 改变必须重新导入，旧文件不会被错误解释。Shape 原枚举值不变，TriangleMesh 追加；`CollisionAssetPath` 默认空，旧场景兼容，字段仍由反射生成器序列化。

## 归属、增量与发布

每个文档的场景位于 `Assets/Imported/<document-id>/Scene.he`；资源为该目录 `Resources/<SHA256>.<ext>`。暂存位于 `.he-content/staging/<run-id>`，不会被场景浏览或 cook 发现。`plan` 会执行导出/转换，但不保存 ID、不发布资源、不改目标场景；诊断与临时工作文件仍会写磁盘。

`.he-content/<document-id>/state.json` 是 v1 持久导入记录，包含源文档/对象/mesh/material 映射、UUID、槽位、工具哈希、导出选项、依赖内容哈希、原生文件清单和上次源基线。Git 规则保留 state.json，忽略 staging、锁和恢复日志：应将源文件、state.json 和对应 Assets/Imported 一起版本管理/迁移，不能丢弃基线后覆盖已有场景。记录还保存字段归属 schema 与诊断。它不是一次会话或 requestId 缓存。

- Blender 拥有名字、Transform、基础 mesh 与实例材质绑定、相机投影/主相机、灯光颜色/强度、碰撞意图。
- 引擎保留其他组件、脚本，以及相机曝光、光照阴影配置、物理质量/摩擦、角色玩法参数等非托管字段。
- 对托管字段做三方比较：上次源基线 / 当前引擎文件 / 新源。引擎已修改且不同于新源的字段会拒绝发布，报告实体与字段。
- 材质覆盖按稳定槽位键重映射。源材质身份更换/槽位删除时若存在引擎覆盖，拒绝盲目套用到新槽。
- 删除源实体之前检查引擎修改、附加组件和场景引用。其他 `.he` 文件中的明确 UUID 引用也会阻止删除。任意脚本逻辑中动态构造的 UUID 无法静态证明，需由作者维护。
- 相同输入不改写场景、原生资源或 state.json。变换只改场景；材质只改材质及绑定；相同求值几何的新增实例复用 mesh。物理/镜像需要的不同几何版本独立计数。
- 导入器、适配器、驱动和源依赖均记录内容哈希。外部缓冲/图片在读取前建立快照，发布前复查；不只看 `.blend` mtime。

发布协议：先生成和校验完整资源 → 写入不可变文件 → 使用 SceneDocument/原生 MeshResourceValidator 预检私有候选场景 → 写 pending.json（旧、新场景哈希与新记录）→ 原子替换 Scene.he → 更新 state.json → 移除 pending。单文件替换只是提交点，不是多文件事务；旧场景引用的旧资源一直保留，所以资源提前发布或中断不会混合旧场景的依赖。下次 sync 根据磁盘场景哈希恢复 pending；如果又有外部修改则保留日志并拒绝猜测。

**没有自动资源 GC**：旧版本可能被其他场景或运行时持有，不做危险清理。同步文件锁避免同一文档的多个 importer 并发发布；不支持与不遵守该锁的编辑器同时保存同一个目标场景。

Importers 插件保持 Editor/Tools、DeveloperOnly、PackagePolicy=Never；cgltf 是固定 v1.15 提交的本地 vendored 头文件，MIT license 和 SHA-256 随依赖保存。旧 `HAssetImporter <project> <model> <output>` / IMeshImporterService 仍是兼容的 Assimp 烘焙模型模式（包括既有动画）；新场景同步从不自动回退。删除了未编译的旧 glTF JSON 导入器。运行时只依赖现有渲染/物理与原生资源。

## 验证、Agent 输出与测试

默认 stdout 是一行 JSON：`ok,status,changed,changes,newResources,scene,sceneSha256,importVersion,metrics,errors,warnings,report,screenshot`。完整诊断和中间报告位于 `out/agent/content/<run>/`。验证失败时 `published=true` 明确表示资源与场景已成功发布，失败的是随后运行验证。

退出码：0=成功（status 区分 changed/unchanged/planned），2=参数或制作工具缺失，3=导入/冲突/发布失败，4=运行验证失败，5=外部工具超时。Blender 使用 `--python-exit-code 7` 且检查结果文件；不是只看 Blender 退出消息。

`visual` 是作者预览截图；不能作为物理、动画或脚本通过证据。`runtime` 显式启动发布场景，核对实体/世界矩阵/材料引用，真实生命周期后执行 240 个固定步进，再截图。会话携带唯一 owner tag，在 finally 中停止；截图等待呈现完成屏障。报告绑定导入版本、场景 SHA-256、scene generation、session 和 render frame。缺主相机、资源未就绪、API 拒绝或后端验证错误均失败。

可选与源同名 `.verify.json` 定义 runtime 的进一步行为断言：schema=1，最多32个 phases，每段0..120帧，heldKeys，断言对象 UUID/唯一名字，以及 minimum/maximum 世界位置。详见 Playable.verify.json。没有配方时，只能报告生命周期/有限状态检查，不能声称门洞或碰撞已验收。

```powershell
# 可重复生成源关卡；覆盖目标制作文件，应使用自己的输出路径
& 'E:/Steam/steamapps/common/Blender/blender.exe' --background --factory-startup --python-exit-code 7 --python Tools/Content/make_acceptance_scene.py -- Project/SourceAssets/BlenderAcceptance/Playable.blend

python Tools/Content/test_content_sync.py
python Tools/Content/test_native_import.py
python Tools/Content/test_workflow.py
python Tools/Content/test_blender_workflow.py
python Tools/Content/test_legacy_import.py
.\hectl.bat test MeshCpuDecode
.\hectl.bat test ReflectedResourceSchemaCompatibility
.\hectl.bat test CornellSceneSerialization
.\hectl.bat test SceneDocumentIncremental
```

验收样例有凹形门框、台阶、共享几何实例、实例材料、镜像展示、相机/点光/方向光、静态代理与三角碰撞、动态落体、两个角色入口。生成器刻意新建源文档；重导入测试应在同一个已初始化 `.blend` 上修改，不应每次删除后重建源文档。

Agent 修改可复用 `blender_batch.py` 的请求文件：支持有限数量 translate/rotate/scale、rename、duplicate（可共享几何）、parent、delete 和元数据操作；空 operations 为只读 inspect。所有修改一次安全保存，批次失败不覆盖原文件。示例：

```json
{"source":"C:/Art/Room.blend","result":"C:/Art/batch-result.json","operations":[{"op":"translate","id":"existing-source-object-uuid","value":[1,2,3]}]}
```

调用 Blender 后，执行一次 `hectl content sync -Source <file> -Verify runtime`，只读摘要；新增或复制对象后带 `-InitializeIds`，明确允许适配器保存新增身份。仅在错误时用 `diagnose` 按 ID/错误码读取完整报告。不要手写原生资源、实体 UUID 或映射表。

验收生成器还创建了打包在 .blend 内的棋盘纹理，导出为 GLB 内嵌 PNG，再进入原生材质。图像验证会记录像素采样并拒绝全黑截图；这只是空白画面防护，不是通用美术质量判定。固定宽高比相机由 Level 组件生命周期重建投影，正交 near/far 顺序也有回归测试。

未测量的加载/帧指标为 null；原生字节和几何资源数量是实际产物统计，不等价于内存占用或 draw call 合并。资源共享复用现有 RenderWorld/批处理，没有宣称额外实现 GPU instancing 优化。实际执行结果与仍未验证的事项见 VALIDATION.md。


### Layered actions and attachments

`AnimLayerRes.progressParameter` optionally names a numeric [0,1] controller parameter.
It samples a non-looping overlay independently of locomotion phase; an empty value
retains phase synchronization. The base clip continues to own root motion and events.
An action driver should set its weight to zero after fade-out and own action completion.

`BoneAttachmentComponent` uses the transform parent as the animated mesh. Its authored
bone-local offset/rotation/scale is resolved after pose constraints each runtime frame.
This also keeps entity UUID remapping inside the ordinary transform-parent contract.
`LimbConstraint.TargetBone` samples another bone in the current pose, and
`WeightParameter` can fade the constraint (for example, releasing a support hand during reload).

The follow camera filters its shared orbit/look pivot with `LagSpeed` and
`VerticalLagSpeed`. `LagResetDistance` resets the spring on a teleport; the orbit itself
responds directly to input. Collision clearance contracts immediately and recovers at
`CollisionRecoverSpeed`. These are generic component settings, with no scene or asset names.
