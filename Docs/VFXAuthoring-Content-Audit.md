# VFX 创作阶段 6 内容能力源码核查

## 后续集成方案（只读检查；2026-09-13）

### Python 方法与事件适配

真实扩展位置是 `Engine/Source/Plugins/PythonScript/Binding/EcsBinding.hpp:1752` 的 `ExtendAnimatorComponentBinding`。新增 `Pause()`、`Resume()`、`Step(seconds)`、`ConsumeEvents()` 应仿已有 Stop 的 `py::cpp_function(..., py::is_method(animCls))`，同步 StubGen。ReflectedComponents 生成绑定仅含字段。Json drain 返回必须转换成 Python list/dict，不要假定项目已注册 Json caster。可以复用已有转换函数，否则边界处显式 `json.loads(self.ConsumeEvents().dump())`。

`Level.cpp:42/:48` 和 `:273/:279` 均先更新 AnimationSystem 再更新 PythonScriptSystem；同帧 Update 可以消费事件。一个实体只设一个事件消费 owner，drain 一次后分发给粒子和音频，不能两个监听者各自 drain 并假定这是广播。无全局回调注册，重载不会重复注册。

下列是绑定完成后的最小集成示意，尚未运行；只演示根实体，骨骼挂点必须另用明确的世界空间挂点查询：

```python
animator = self.entity.GetComponent("Animator")
pos = self.entity.GetComponent("Transform").Translation
for event in animator.ConsumeEvents():
    if event["name"] == "vfx":
        self.vfx_handles.append(HEngine.VFX.PlayEffect(event["payload"], pos.x, pos.y, pos.z))
    elif event["name"] == "audio":
        self.audio_handles.append(HEngine.Audio.Play3D(event["payload"], pos))
```

Start 初始化句柄数组；owner teardown/取消分别调用 Animator.Stop、VFX.StopEffect、Audio.Stop。真实已有入口在 AudioBinding.hpp:52/:74/:108 及 VFXBinding.hpp。payload 是表现资源引用，不执行任意代码。适配器应在播放成功后才登记句柄，并清理自然结束句柄。

### 阶段 6C 协议与文件边界

建议每 texture slot 保存 `TexCoord`（0或1）、`UvRow0/UvRow1`（仿射2×3，先 scale 再 rotation 再 offset）和 `Sampler{WrapS,WrapT,MinFilter,MagFilter}`。glTF transform 的 texCoord override 优先。默认 UV0、单位变换、repeat/linear/trilinear。COLOR_0 为线性颜色乘 baseColor 和 alpha，缺省白色。显式选择 UV1 但缺失时应报错，不静默换 UV0。sampler 应覆盖 glTF nearest/linear、6 种 minFilter、repeat/mirror/clamp，非法枚举报错；不能用 frac 冒充硬件 wrap，避免边界插值和导数错误。

| 范围 | 文件与修改协议 |
| --- | --- |
| native glTF | `Plugins/Importers/MeshImporter/GltfSceneImporter.cpp:138` 增加 KHR_texture_transform；`:171` 输出 per-slot UV/transform/sampler；`:287` 支持 TEXCOORD_1、COLOR_0（VEC3/4，normalized或float，VEC3补alpha=1）；验证所选UV存在。 |
| Blender | `Tools/Content/blender_export.py:52` 当前拒绝 color_attributes，`:142` export_attributes=False。按真实支持解除限制，确保颜色和第二UV导出；未支持材质节点继续要求烘焙。 |
| legacy模型 | `Plugins/Importers/MeshImporter/AssimpImporter.cpp:691` 已读取sampler，`:1506`仅UV0输出；补第二UV/颜色、每贴图UV选择/transform，保持骨骼动画入口。 |
| 反射资源 | `Runtime/Resource/ResType/Material.h` 三类TextureInfo已有texCoord，新增共用采样描述；`Texture.h`已有sampler枚举可复用；`BasePbrMaterialBuilder.cpp` AddTexture将完整元数据写入typed Textures。正式codegen。 |
| mesh解码 | `Runtime/Graphics/Mesh/MeshResourceValidator.cpp:286` 新属性类型/维度校验；`MeshLoader.cpp:370/:524` 两类顶点解码；`MeshVertex.h` StaticVertex、`SubMesh.h:22` SkinnedVertex增加UV1/Color并设旧资源默认。 |
| 材质运行态 | `Graphics/Material/Material.h/.cpp` MaterialTextureReference、typed Textures读写、迁移/热重载、`:1237` snapshot；`Graphics/Scene/RenderWorld.h:130` TextureReference带同一采样元数据。fingerprint/revision必须含采样参数。 |
| GPU数据 | `Graphics/Renderer/SceneRenderer.cpp:281` GpuMaterialData当前96 bytes，`:4882`仅单sampler；增加5个PBR slot的UV行及sampler index；同步所有CPU/HLSL布局和static_assert，纹理slot顺序保持。 |
| GPU顶点 | `SceneRenderer.cpp:3574` 主布局及`:3652` depth/shadow、`:3698`相关布局，所有offsetof/stride同步；GPU-driven及skinned路径均贯通，避免新semantics与已有ObjectIndex/velocity冲突。 |
| 当前主shader | `Modern/SceneOpaque.hlsl:249`是真实内建PBR采样路径；增加UV1/Color和per-slot采样。`SceneDepthPrepass.hlsl`、`SceneShadow.hlsl` alpha也必须同UV/transform/sampler/vertexAlpha，否则表面、深度和阴影不一致。 |
| 自定义共享结构 | `Modern/MaterialSurfaceCommon.hlsli:116` 同GPU材质ABI，`:158` HEMaterialInput加UV1/Color；`Modern/MaterialSurface.hlsli`顶点/varying填充。自定义shader明确选择使用新采样helper，不能盲改所有custom表达式。 |
| 旧shader兼容 | `Include/Common.hlsli`、`BasePBR.hlsl`、`ShadowDepth.hlsl`相关输入和采样同步；只改这些旧文件不足以证明当前主渲染支持。 |
| sampler管理 | `Graphics/RHI/TextureManager.h/.cpp:529`只有一个material sampler，需要规范化descriptor缓存及生命周期；`MaterialDescriptorPaging.h/.cpp`仅有TextureKeys，需SamplerKeys和本地indices，选页同时满足texture和sampler容量；`SceneRenderer.cpp:5066` remap同步。baseline仅15samplers，超单页可分页，单材质超预算报错。 |
| shader访问/ABI | 复用 `Modern/MaterialDescriptorAccess.hlsli` 的HE_MATERIAL_SAMPLER，不另造绑定槽。若扩material parameter ABI，HShaderCompiler/Main.cpp、MaterialParameterPacking、manifest/hash/cache identity一起修改。 |
| 光追一致性 | `Graphics/RayTracing/RayTracingScene.cpp:228`独立材质record/纹理表；其头文件record及 `Shaders/RayTracing/HardwareRayTracingRealtime.hlsl` 顶点读取、重心插值、alpha和采样同协议。StaticVertex stride变更不能留下旧offset；未贯通组合要明确拒绝，不能静默错图。 |

夹具应可自生成：UV0/UV1不同坐标格、非白顶点色与alpha、非均匀scale+rotation+offset、非对称wrap与nearest/linear/mip区别。覆盖Static/legacy Skinned、DX12/Vulkan、标准表/heap支持范围、前向/深度/阴影一致性。继续使用content_sync测首次/noop mtime、材质/geometry修改、rename、增删以及脚本实例覆盖。以上仍是实施方案，没有写入或修改这些实现文件。

日期：2026-09-13。范围：只读核查 Prefab、动画、常用导入；仅本报告为新增文件。遵循根 AGENTS.md；未构建、未运行测试、未进行 GPU 验收。所有“存在”均为源码存在、本次尚未运行验证，不能据此填写运行 PASS。行号为核查时源码位置，后续实现可能移动。

## 结论与实施边界

三个子阶段均有可复用基础，但均存在本任务要求的实际实现缺口。已有单实体 Prefab、场景事务、TRS 单动画采样、参数状态转移、PBR 因子、静态内容增量发布不能等同于子树 Prefab、局部姿态混合和 UV1 等导入端到端支持。

### A. 子树 Prefab

| 必需项 | 当前状态及源码依据 |
| --- | --- |
| 单实体保存/实例化 | 已存在未验证。`Engine/Source/Runtime/EcsFramework/Serializer/PrefabSerializer.cpp:41` 保存组件对象，`:58` 只序列化传入实体；`:95` 只创建一个实体，`:98` 反序列化组件。 |
| 多实体子树 | 当前 Prefab 路径不支持：没有子实体枚举、模板实体表或实例化循环。`Component/ComponentGroup.h:46` 说明 UUID 不在组件保存集内；`SceneEditingService.cpp:453` 仍要求 Prefab 是组件对象。 |
| 稳定模板身份和内部引用重映射 | 不支持。Prefab 当前只生成一个新 UUID，不建立模板 ID → 实例 ID 表。保存的 ParentUuid 也没有 Prefab 内部重映射。可复用 `SceneEditingService.cpp:589` 子树 duplicate 的 UUID 映射，但该分支只改 ParentUuid 和 FolderUuid（`:593`、`:597`），不是通用实体引用重映射。 |
| 嵌套、循环依赖 | 未发现 Prefab 资源嵌套表示/依赖展开；现有 `SceneEditingService.cpp:549`、`:868` 是场景父子循环校验，不能代替资源嵌套循环。 |
| 局部覆盖、模板传播、删除冲突 | 当前没有持久模板身份、实例基线、覆盖表与同步 API，因此未支持。材质槽覆盖检查（`SceneEditingService.cpp:483`）不等同于任意 Prefab 局部覆盖。 |
| 失败恢复 | 单实体反序列化失败回滚已存在：`PrefabSerializer.cpp:111` 捕获并销毁部分实例。保存路径边界和 Registry invalidation 已存在（`:48`、`:63`）；没有子树原子提交。 |
| 保存重开、撤销 | 场景服务已有 snapshot/change/revision/undo 基础（`SceneEditingService.cpp:666`、`:906`），需接入 Prefab 操作，不能新建旁路。 |
| 资源生命周期 | 已存在 `RuntimePrefabResource.cpp:110` acquire、`:123` async queue；实例化在 `PrefabSerializer.cpp:168` acquire 并由场景 group pin。 |

最小实施方案：保留旧组件对象读入为 v1 单节点；新格式使用 version/root/entities/nested 及稳定模板局部 ID。先完整展开并校验依赖和字段引用，再分配所有 UUID，最后一次性实例化/提交；失败销毁全部新实体。把引用类型放进既有反射/场景描述契约，明确拒绝不能静态解析的脚本引用，不能全局替换任意字符串。持久实例元数据应记录 template path/version、local ID 映射、基线与显式覆盖；模板更新用三方比较形成场景事务，子对象删除/覆盖字段删除返回冲突且不部分应用。Editor 和 Agent 调用相同服务。

已有测试入口：`Engine/Source/Runtime/UnitTest/TestMain.cpp:1022` 注册 `RuntimePrefabResource`，`:1030` 注册 `SceneEditing`（Editor 限制标志）。`RuntimePrefabResourceTests.cpp:97` 覆盖路径、缓存代次、lease/pin、`:237` 坏资源实例化回滚。本次未执行。还需新增子树内部引用、两个实例隔离、嵌套循环、覆盖三方合并、删除冲突、保存重开/撤销测试及角色实际场景验收。

### B. 动画

| 必需项 | 当前状态及源码依据 |
| --- | --- |
| 单 clip 局部采样/层级重建 | 已存在未验证：`Engine/Source/Runtime/Animation/Animator.h:80` 默认 node bind transform，`:86` 用 Bone 局部采样替换，`:113` parent × local，`:121` global × inverse-bind offset。 |
| 旋转插值 | 单 Bone 使用 slerp：`Animation/Bone.h:188`。 |
| 状态机最终矩阵线性混合 | 明确存在缺陷：`Animation/AnimationStateMachine.cpp:17` 执行 `a[i] * (1-alpha) + b[i] * alpha`，`:147` 过渡调用。需修复，即使当前 ECS 另有实际路径。 |
| ECS 实际混合 | `EcsFramework/System/Animation/AnimationSystem.cpp:978` 获取双方最终骨骼矩阵，`:990` 调用 BlendSkinnedBoneMatricesInPlace；`:356`、`:357` 去掉 offset 后仍是 **全局** transform，`:358` 混合再乘 offset。`:281`–`:283` 虽是平移/四元数/缩放混合，但没有局部层级重建，不能标为本任务所要求的正确局部姿态混合。 |
| 缺失通道 | `Animator.h:80` 支持完全无该 Bone 时 bind transform；但 `Bone.h:162`、`:176`、`:195` 仅处理 1 或多个关键帧，没有零关键帧分支，不能宣称缺失 TRS 子通道可靠。 |
| 连续运动参数混合 | 控制器有参数（`Resource/ResType/AnimController.h:65`）和条件转移，但 node（`:75`）只有 clipPath，没有阈值样本/混合轴。离散过渡不能替代连续 locomotion blend。 |
| 动画事件 | `Resource/ResType/Animation.h:68` clip 只有 name/duration/ticks/channels，没有事件轨；所核查 Animation/AnimatorComponent/AnimationSystem 未发现动画事件分发。UI animationend 为另一系统。 |
| 循环/停止 | `Animator.h:35` 无条件 fmod；`AnimationStateMachine.cpp:165` 在已经取模的 progress 上判断 non-loop，存在跨末帧风险。事件需要独立未取模时钟和跨周期区间处理。 |

最小实施方案：在 Animator/Bone 共享采样层输出按骨架 node 的 LocalPose（T/Q/S）；缺失子通道用 bind pose 分量。两条混合调用都共享局部姿态 lerp/slerp，再一次父先子后重建，禁止继续混合最终 skinning 矩阵。明确骨架不兼容诊断。扩展当前控制器节点为有限 1D 连续速度混合（保留 State），重用参数表。clip 事件加稳定 ID/time/name/payload，runtime 按 `(previous,current]` 分发，跨多个循环也可确定；暂停不发、显式固定步进发区间事件、取消清空该播放实例、重载按新播放代次防重复。通过现有事件/表现适配层触发 VFX/音频，保留玩法判定职责。

本次未找到引擎内专门 Animation 单元测试注册（非第三方），需新增合成骨架大角度、父子局部约束、缺失通道、循环多步与事件取消测试；仍必须通过真实角色 idle/move/attack、挂点、变步长视觉验收。资源字段修改需正式 codegen 和受影响 build 分别取证。

### C. 导入与渲染贯通

| 必需项 | 当前状态及源码依据 |
| --- | --- |
| UV0 | 已存在未验证：`Plugins/Importers/MeshImporter/GltfSceneImporter.cpp:287` 允许 TEXCOORD_0；`Runtime/Graphics/Mesh/MeshLoader.cpp:370`、`:524` 解码。 |
| UV1 | 当前明确拒绝：上述 importer `:287` 仅允许 POSITION/NORMAL/TANGENT/TEXCOORD_0；`Runtime/Graphics/Mesh/MeshVertex.h:14` 只有一份 TexCoord。不能只解除 importer 校验。 |
| 顶点色 | 同一属性白名单拒绝 COLOR_0；StaticVertex 无颜色。当前无端到端支持。 |
| 纹理坐标选择、transform | `GltfSceneImporter.cpp:171` 要求 texcoord==0 且 !has_transform。`Resource/ResType/Material.h:24`、`:39`、`:56` 已有 texCoord 字段，但 `BasePbrMaterialBuilder.cpp:72` 的 runtime texture entry 不传此字段；`Engine/Content/Shaders/BasePBR.hlsl:211` 所有贴图从 input.TexCoord 采样。 |
| sampler | `GltfSceneImporter.cpp:173`–`:176` 明确仅 repeat/linear/trilinear。Assimp 可读 wrap/filter（`AssimpImporter.cpp:691`–`:713`），`Resource/ResType/Texture.h:60` 有 sampler，但 BasePBR texture entry 未传递，shader `:211`、`:219`、`:227` 固定 g_LinearWrapSampler。解析不等于呈现。 |
| 常用 PBR 因子 | 已存在未验证：`BasePbrMaterialBuilder.cpp:123` 起写 baseColor/metallic/roughness/AO/normal/emissive/alpha；BasePBR shader `:211`–`:235` 消费。仍需本次序列化和图像验证。 |
| 动画与蒙皮导入 | 场景模式明确拒绝（`GltfSceneImporter.cpp:141`），引导 legacy baked-model；`AssimpImporter.cpp:1506` 存在原模型 UV0 输出，Animation/Skeleton 资源和运行通道存在。不能声称 native 场景模式支持动画，更不能删旧入口。 |
| 稳定 ID、增量、覆盖与恢复 | 已存在未验证：`Tools/Content/content_sync.py:112` merge，`:159` 材质槽身份覆盖冲突，`:164` 字段归属冲突，`:169` 删除引用，`:180` 恢复，`:412` 依赖 hash，`:434` noop，`:454` 发布前外部变更检查，`:458` journal。 |

最小实施方案：从 mesh schema/validator（`MeshResourceValidator.cpp:286` 属性限制）、两个导入入口、StaticVertex/skin vertex 到 GPU input layout、共享 shader vertex/varying 贯通 UV1/Color，旧资源默认 UV1=UV0 或明确缺失选择错误、Color=white。Material 每 texture slot 表达 UV set、offset/rotation/scale、wrap/filter，BasePBR builder 保留这些值，材质 packing/binding 和 shader 实际使用。不要把某一纹理的 sampler 错当所有材质全局 sampler。新增固定可生成 glTF 小场景，各测试块用不同 UV/color/transform/sampler 可辨识图案；正常映射切线基与所选 UV 必须一致或报可操作错误。用原 content_sync 发布事务验收首次/noop/材质/几何/改名/增删以及脚本覆盖保留，动画原入口重导入独立验证。

已有测试入口（仅源码核查，未运行）：

- `python Tools/Content/test_content_sync.py`：源码列出 noop/脚本保留、field conflict、slot reorder、delete references、recovery、timeout reaping 等测试（`:29` 起）。
- `python Tools/Content/test_native_import.py`：stride/normalized/accessor、依赖失败、路径、身份、TRS、材质槽、碰撞、相机。`:18` 固定使用 `out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe`，运行前必须确认当前源码构建产物，不能用旧文件当证据。
- `python Tools/Content/test_legacy_import.py`：既有 obj/glTF baked model 兼容性；不覆盖本任务所需完整角色动画重导入。
- `Engine/Source/Runtime/UnitTest/TestMain.cpp:999` 注册 `MeshResourceValidation`；另有 MaterialSerialization/MaterialParameterPacking 等已有测试可按改动范围运行。

## 验收状态

本报告属于源码审计，不属于 build-only/runtime-manual/unit-test/test-layer PASS。阶段 6 的全部运行和视觉验收均 **未执行**；缺口不能被文档描述或已有测试文件替代。最小代码完成后仍须 Game、Editor、正式反射生成、DX12/Vulkan 场景和最终发布包分别取证。未改任何实现、资源、权限或总控记录。


## 动画实施增补（首轮集成稳定点，尚未编译验收）

主代理后续授权了动画有界实现。上文核查保留为修改前证据；本节描述已落盘实现，不能替代最终验收。

- 新增 `Animation/LocalPose.h`：局部 T/Q/S，四元数 slerp，混合后重建层级。
- `Bone.h`：缺失通道保留 bind pose 分量；首尾采样 clamp；不再越过末关键帧 assert；同 Bone 的采样不再修改共享数据。
- `Animator.h`：保留旧 API，增加 `SetLoop`、`HasFinished`、事件 drain；forward step 使用未取模时间计算跨周期区间，non-loop clamp 到末姿态。`UpdateAnimationAbsolute` 是以 ticks 为单位的 pose-only seek，不发送历史事件；reset 后首次正向步进包括 timestamp=0，之后用 (previous,current]。
- 两条状态机路径均调用局部姿态混合；ECS 旧全局矩阵混合/offset cache 已移除。新增 `MotionBlend.h`，以 `blendParameter` 连续采样相邻阈值；samples 引用同控制器已加载 State node IDs，按该 State 主 clip 的归一化相位采样。主 clip 独占时长与事件；混合样本仅提供姿态，避免权重变化重复脚步声。
- 反射新增 `AnimEventRes{id,timeStamp,name,payload}`，`AnimClipRes.events` 默认空；`AnimBlendSampleRes{nodeId,threshold}`，State 的 `blendParameter` 默认空、`blendSamples` 默认空。旧动画与控制器不需要迁移。事件时间单位 ticks，要求唯一非空 id/name 和 [0,duration) 时间。
- 动画资源原手写 clip 字段读取已换为正式 generated Serializer；仅既有递归 node children 仍用专门递归入口（该字段原本 Disable）。没有通用序列化字段补丁，也没有手改 generated 文件。
- `AnimatorComponent` 增加 `Pause/Resume/Step(seconds)/ConsumeEvents()`；Step 用实际秒，再乘 playSpeed；一次累积不超过 60 秒。组件事件是本次动画更新的 JSON 队列，消费后清空；下一次有效更新替换，普通脚本应每帧 drain。字段包括 id/name/timeStamp/payload/cycle/clipIndex。目标状态拥有过渡期事件，旧状态事件取消。Stop/Play 清空旧队列，natural end 保持末姿态。
- 暂无事件全局订阅注册表，因此不会因资源重载重复注册。VFX/音频适配层应消费此队列并调用现有表现入口；公开 Python bindings 和真实动作触发由主代理集成，不在本子任务中伪称已完成。

示例控制器 State 扩展：`"blendParameter":"speed","blendSamples":[{"nodeId":1,"threshold":0},{"nodeId":2,"threshold":4}]`；nodeId=1/2 对应已有 State clipPath。事件示例：`"events":[{"id":"attack_hit","timeStamp":12,"name":"vfx","payload":"Assets/Effects/Hit.efkefc"}]`。name/payload 是表现层契约，不运行任意代码；动画系统不做伤害判定。

新增 `UnitTest/AnimationPoseTests.cpp/.h`，函数 `HEngine::Tests::AnimationPoseContract`。测试注册委托主代理（避免竞争修改 TestMain/CMake）：添加源文件并注册名称 `AnimationPose`。覆盖 170/-170 度最短四元数路径、父子长度、缺失通道、首尾 clamp、2.5 循环事件、暂停/seek/drain/cancel、non-loop末帧、连续混合、反射新旧字段往返。

待主代理统一执行（本子代理没有执行构建或GPU）：

| 证据类型 | 精确命令 | 状态 |
| --- | --- | --- |
| codegen（单独报告） | `.\hectl.bat codegen run --json` | 未执行 |
| unit-test | `.\hectl.bat test AnimationPose` | 未执行，需先注册 |
| runtime-manual | `.\hectl.bat game check` | 未执行 |
| runtime-manual | `.\hectl.bat editor check` | 未执行 |

静态验证：`git diff --check -- Engine/Source/Runtime/Animation Engine/Source/Runtime/EcsFramework/Component/Animation Engine/Source/Runtime/EcsFramework/System/Animation Engine/Source/Runtime/Resource/ResType/Animation.h Engine/Source/Runtime/Resource/ResType/AnimController.h` 返回 0；这仅为空白/diff检查，不属于编译或测试 PASS。仍需实际角色动作、挂点、事件表现、重载、双后端视觉与性能验收。

## 阶段 6C 实施检查点（2026-09-13，待主代理集成）

以下为已落盘实现，取代前文对应导入缺口的“未实施”状态；并不代表引擎或 GPU 验收通过。

- `Resource/ResType/TextureSampling.h` 新增正式反射资源字段：UV0/1、KHR 的 scale→rotation→offset、Assimp affine 行、S/T 独立 repeat/clamp/mirror、6 种 min 和 2 种 mag。默认 UV0/单位变换/repeat/trilinear/linear。`Material.h` 三种 texture info 保留旧 texCoord，并追加 sampling；builder 与材质 typed Textures 的 Sampling 均走生成 Serializer，无通用序列化补丁。
- `GltfSceneImporter.cpp:225` 解析每 slot KHR_texture_transform 与覆盖 texCoord；`:245` 传 normal scale/AO strength；`:295` 起解码 UV1、RGB/RGBA 顶点色，校验形状、长度、finite 与材质使用通道。仍明确拒绝 UV2+、本阶段之外的扩展、静态 scene 管线中的 skin/morph 等既有不支持项。
- `AssimpImporter.cpp:1051` 读取 UVWSRC、sampler 和 Assimp 中心变换矩阵，UV1 缺失明确失败；`:1550` 附近输出 UV1/颜色 accessor。既有 animated/legacy 入口仍保留，未改第三方库。Assimp 坐标约定仍需与实际资产对比，尚无运行验收。
- `MeshVertex.h:15` 与 `SubMesh.h:27` 添加 UV1 默认零、Color 默认白。`MeshLoader.cpp:363/:528` 分别按 accessor 解码 static/skinned 的新增属性；旧 hmesh 的二进制是独立属性流，不是持久化 StaticVertex/SkinnedVertex blob，因此旧资源无需 stride 迁移；旧 UV0/骨骼 accessor 流保持原读取规则。新增属性缺失时保留默认。
- `SceneRenderer.cpp:291/:367/:4975` CPU/GPU Material ABI 追加 5×48 byte slot sampling，总长 336；全部 static/skinned/GPU-driven/depth/shadow input layouts 与 shader 声明顺序同步（Vulkan 按 declaration 分配 location）。MaterialShaderAbiVersion 升为 2，需要重新生成 shader 缓存。
- `Shared/TextureSampling.hlsli` 以 SRGB SRV Load 在解码后插值；整数 texel wrapping 处理负 UV 与 mirror 边界，最近/双线性/mip 最近/三线性分别实现，保留 min/mag 区别。默认 repeat/trilinear 使用既有硬件各向异性 sampler 快路径；其他组合为 isotropic 导数 LOD，最多 8 taps，glTF 不提供各向异性字段。
- Modern SceneOpaque/DepthPrepass/Shadow 的 alpha 均使用相同 slot 变换、sampler 和 vertex alpha；normal UV1/变换使用导数切线空间。MaterialSurface 输入公开 UV1/Color，未自动改写用户自定义材质图表达式。
- RT vertex 96 bytes/material 416 bytes，公共 CPU/三个 HLSL 结构同步。离线参考 renderer 完整插值 UV0/1 和颜色、逐 slot 采样、变换 normal tangent basis；payload 96 bytes，Session pipeline max payload 同步。保持原 fixed LOD0 语义（以 mag filter 采样），未声称导数 LOD。
- **Realtime GI 仍使用原常量材质近似**，此次仅同步公共 ABI；它没有精确 UV、颜色纹理或 sampler 光传输支持。离线参考与光栅的覆盖不可泛化到 realtime GI。

可重建源资产：`Project/SourceAssets/VFXAuthoring/Sampling/sampling.gltf`、sampling.bin、grid.png、normal.png 与 manifest.json；12 个面片含全部 minFilter、UV1、渐变色、变换、独立 wrap、alpha mask、UV1 normal。生成命令：`py -3.11 Tools/Content/generate_sampling_fixture.py --output Project/SourceAssets/VFXAuthoring/Sampling`。默认相机在 manifest；输入纹理为确定生成的极小 RGBA 图，不含外部版权依赖。

| 证据类型 | 精确命令 | 状态 |
| --- | --- | --- |
| unit-test（源资产） | `py -3.11 Tools/Content/test_sampling_fixture.py` | PASS，3 tests；首次/无变化 bytes+mtime、身份稳定与增删改名、sampler/属性矩阵 |
| unit-test（数值 oracle） | `py -3.11 Tools/Content/test_sampling_reference.py` | PASS，3 tests；负 UV/mirror、sRGB 先解码、min/mag/mip 数值；不是 GPU shader 执行 |
| unit-test（引擎） | `.\hectl.bat test MaterialSampling` | 待主代理注册并运行；108 组合、变换顺序、生成序列化默认/往返、builder slot 传递 |
| codegen | `.\hectl.bat codegen run --json` | 待主代理统一运行 |
| importer/merge integration | `py -3.11 Tools/Content/test_sampling_import.py --importer <当前构建HAssetImporter.exe绝对路径> --project Project --report out/agent/sampling-import-report.json` | 待当前导入器构建；首次、无变化、材质/几何、改名、增删、脚本/材质覆盖保留及 UV3 拒绝；调用实际 importer + merge_scene，不冒称完整 journal/publish 测试 |
| runtime-manual | `.\hectl.bat game check` / `.\hectl.bat editor check` | 待主代理统一构建与 DX12/Vulkan 实际小资产采集 |

新增测试注册文件所有权在主代理，本子代理没有运行 codegen、C++ 编译或 GPU。源 diff whitespace 检查通过。最后交付仍需实际 shader 编译、旧 static/skinned 资源加载、定量图像和两后端证据；特别是新结构布局不能只靠静态断言验收。
