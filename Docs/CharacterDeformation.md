# Character deformation and facial performance

## Implemented pipeline

Model import (`MeshImporter` / `HAssetImporter`) reads Assimp `aiAnimMesh` replacement positions/normals/tangents and writes native **delta** target accessors, names and default weights per primitive. Native hmesh decode validates target references, vertex counts, finite values and a 256 MiB per-section decoded target budget. Scene mode remains static-only and rejects targets explicitly; import characters in model mode.

`Mesh::EnsureUniqueDeformation()` clones shared sections once per Mesh instance. Run `SubMesh::BeginDeformation()` once, apply named morph weights, then cloth, then `EndDeformation(resetHistory)`. This preserves bind data and previous **final** deformed positions; it never accumulates shape offsets or modifies another character. Positions, normals and tangents are blended before skinning. Main/default and custom-material shaders use previous deformed positions with previous bone/world/view transforms for velocity. Reset history after teleport/seek and when entering pause/distance suspension.

`FacialPerformanceRes` (`.hface`) is reflected data:

```json
{"Duration":1.0,"Loop":false,"Curves":[{"Target":"JawOpen","Smooth":true,"Keys":[{"Time":0,"Value":0},{"Time":0.12,"Value":0.65},{"Time":0.3,"Value":0}]}]}
```

`FacialPlayer` loads a file once on path change, evaluates deterministic curves, applies explicit named weights and optional asymmetric blink. Use Reset after resource reload or Seek for cinematic time; errors are logged and exposed by GetError. Explicit weights override curves. Blink takes the maximum, so an authored eye closure remains closed. This is authored/externally generated viseme support, not speech recognition. Corrective-only shape keys are not evidence of final facial art quality.

## Import and reimport

The first model import writes `<source-stem>.himport` with the canonical source and owned output names. Repeating import from the same source to that destination performs staged reimport with stable paths, rewrites dependencies back to the live directory, preserves existing artist-edited `.hmat` files, backs up live outputs and atomically publishes each new file. A publication failure rolls back; if restoring a file fails, the recovery stage is retained and logged. Removed source outputs remain rather than breaking existing references. New source material slots must be reviewed by an artist. Reimport is a tool operation; close/reopen or reload consumers after publishing.

Use a dedicated empty asset directory for a new source. Legacy directories without a source manifest keep create-new behavior; they are not silently adopted. A different source is refused if it would claim an existing manifest. This avoids guessing which user assets can be overwritten.

## Validation entry points

- `hectl test MeshCpuDecode`: native shape decode, malformed target, instance isolation, actual vertex deformation, prior-position and reset, curve interpolation/loop, reflected round-trip and blink envelope.
- `python Tools/Tests/Validate-MorphReimport.py --importer <HAssetImporter.exe> --project <project-root> --output <new project Assets subfolder>`: real glTF morph import, changed source reimport, stable filenames, preserved material edits, and invalid-source preservation. The output is deliberately retained.
- Renderer validation must include default and CustomSurface characters, paused/resumed and rapidly changing morph weights. A unit/build result alone does not establish visual quality.

## Cost and limits

This CPU path targets a small number of hero characters. A changed skinned section uploads its complete vertex buffer; indices upload once. Static deformed sections retain the existing arena allocation upload. Dynamic sections remain declared in the render graph without rebuilding the graph per frame; target streams remain CPU resident. It is not a GPU sparse morph implementation. Shape weights are clamped to [-2,2], authored facial curve keys to the same range. The native format supports static and skinned targets. Hardware ray-tracing deformation refit is not covered; the representative demo uses rasterization. The shape set and vertex topology must remain coherent across poses and LOD index streams.


## Rain character production example

`Examples/AsterCourtyard/Tools/prepare_rain.py` evaluates Blender Studio Rain with upstream Python disabled, reduces 392 original deform bones to 79 game bones, excludes mask/selection groups from skinning, normalizes four influences, converts UDIM skin tiles to native-compatible materials, and bakes six real evaluated facial poses: BlinkLeft, BlinkRight, JawOpen, Smile, BrowUp, MouthPucker. Face correctives alone are not exported as expressions. The prepared blend contains no upstream scripts/control rig. Cornea/viewport-eye duplicate meshes are omitted; final eye rendering belongs to the engine material.

`retarget_rain.py` transfers the existing reviewed Sintel seven-clip library. It keeps Rain bone lengths, uses global bind deltas for the torso and actual source limb axes plus bind-roll correction to account for different rest poses. It applies an explicit source heading correction and scales root translation by 0.915717. Hair, eyes and scarf retain their bind orientation relative to their animated parents, then engine secondary motion/cloth can act. `PreparedAnimated/retarget.json` records the explicit mapping and per-clip first/middle/last global matrix reconstruction errors (observed maximum 6.11e-6). The four inspected poses were Idle, Walk, Run and Attack; game collision/contact and all frame timing still require runtime validation.

The retarget pipeline invokes `create_hair_cards.py`, producing an original alpha/normal atlas and layered card geometry. `refresh_rain_hair.py` rebuilds just the generated cards/cap in the animated artifact while preserving all seven animation clips and six morphs. The exported card material is MASK, cutoff 0.4, double-sided. `secondary.json` declares the hair chain for the engine spring solver. `ClothMotion` is exported as COLOR_0: scarf red ranges from 0 (neck anchors) to 1 (tips), with 2013 pinned and 2855 movable loops; all other sections have red 0. These colors are simulation data and must not tint the character albedo.

Use Blender with `--disable-autoexec` for both source and generated artifacts:

```powershell
& out/agent/tools/blender/blender.exe --background 'Examples/AsterCourtyard/SourceAssets/Rain/Rain v3.3/rain_v3.2.blend' --disable-autoexec --python Examples/AsterCourtyard/Tools/prepare_rain.py
& out/agent/tools/blender/blender.exe --background Examples/AsterCourtyard/SourceAssets/Rain/Prepared/Rain.blend --disable-autoexec --python Examples/AsterCourtyard/Tools/retarget_rain.py
```

The final export is `SourceAssets/Rain/PreparedAnimated/Rain.glb`. For an existing import manifest bound to `Prepared/Rain.glb`, publish the reviewed final export to that canonical generated GLB path before reimporting. Keep the neutral `Prepared/Rain.blend` as the retarget input. The engine importer regenerates tangents for the jeans where Blender cannot export them. The art is stylized; these results do not establish photorealistic AAA facial quality. Attack animations require the scene's held-prop interaction to supply a weapon.
# CPU 形变缓存（待主线构建与实测）

纯 morph 分支按实际生效权重精确缓存，包含默认权重、有限值处理、钳制和既有零贡献规则。变化帧保留旧位置；首个稳定帧单独将 previous position 收敛到 current 并发布一次；后续稳定帧不重算、不扫描 bounds、不增加 GPU 上传 revision。设置目标、外部形变 Begin/End、实例复制和重新激活会使缓存失效。布料与 morph 混合的 section 仍走完整形变流程。

布料临时数组改为每实例缓冲复用，焊接顶点贡献数量随拓扑缓存，法线拉回采用等价的 `transpose(skin)`。模拟子步、约束迭代、碰撞及质量参数未降低。`deformationVertexUpdates` 现在只计本 tick 发布的顶点（包含一次 history settle），缓存命中计零；`deformationInstanceUpdates` 只计实际发布的实例。这与优化前的活跃顶点数口径不同，不能把计数下降直接当作耗时提升。

新增待运行检查位于 MeshCpuDecode 与 AnimationPoseContract：静态/蒙皮历史收敛、精确权重变化、默认/非法权重、目标替换、实例隔离、暂停/距离恢复、实际更新计数、布料非均匀缩放与 shear 法线。收益以重新构建后的相同回放耗时测量为准。

第二批将每 Step 不变的 stretch/bend alpha、迭代数、有效胶囊的轴/轴长平方/膨胀半径提前计算，保留原除法与投影顺序，不以倒数乘法替换。AnimationPoseContract 内新增冻结旧 solver 的逐帧对照与 CPU microbenchmark：144 粒子、180 帧、变化步长和碰撞体、无效配置回退、暂停及长帧重置；逐顶点最大偏差阈值为 10 微米。日志记录旧/新 Step 累计毫秒及最大偏差，耗时仅诊断，不设机器相关门槛。当前第二批尚未执行构建或测试。
