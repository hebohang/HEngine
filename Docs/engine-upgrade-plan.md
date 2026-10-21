# 角色与关卡质量升级

**2026-09-26 SICKA当前状态：** 从当前正式场景生成UE固定机位复核后，HE树冠已无旧矩形叶片；UE参考自身块状树冠不作为目标。项目后处理调亮暗部、减轻冷色，前后机位道路/中景区域均更接近UE；另加2处前景落叶布置。新增布置后Editor900预热后0帧超过33.333ms；Game900首轮出现41帧连续GPU慢段，复测GPU恢复但有3帧CPU超时。按宽松30FPS口径有正常运行证据，不能写成所有运行逐帧保证。GI、真实体积雾、近景发片和UE动态逐帧对照仍未完成。

**2026-09-26 头发优先级：** 口腔按正常驱动、近景无明显错误验收；优先提升MechanicGirl发束运动。引擎已加入瞬时大角度转向的历史重置、碰撞修正不注入弹射速度，`AnimationPoseContract`通过；项目17根发骨按根到梢分级调参。近景第45/90帧检查与正式SICKA Game/Editor300帧、Game900帧验证通过。本轮900帧0帧超33.333ms，既有同场景CPU偶发慢段未解释。UE逐帧动态一致性和发片精细度仍未完成，不能据此宣布AAA人物完成。

**2026-09-26 发片覆盖补正：** UE `M_HAIR.opacity_mask_clip_value`只读确认为0.3333，项目旧值0.48让发片偏稀。隔离近景A/B和多角色画面检查后，项目五款正式发色与模板统一改为0.3333，重建BC7/mip并跑Game900、Editor300通过。引擎V2材质加载对顶层阈值和typed参数不一致改为显式拒绝，单测及Game/Editor回归通过。发丝精细度、跨帧闪烁和UE动作同机位自然度仍需验收。

**2026-09-25 用户更新验收口径：** SICKA关卡以关闭`MI_PP_Wave`/`MI_PP_Sharpen`后的UE编辑器画面“基本一致”为准；性能要求预热后稳定超过30FPS，允许宽松评估并将显卡温度/降频等不可控设备事件单列。先前同场景Game和Editor各900帧复核的840统计帧均0帧超过33.333ms，走/跑/跳实际截图PASS；树冠/GI及近景人物美术仍有差异。后来Game900帧第825–900帧又出现GPU连续慢段，76帧超过33.333ms；不能以先前正常短测宣称所有设备状态下稳定30FPS。UE参考视口自身有异常大块树冠，不作为HE目标；HE零阈值整卡矩形已在通用遮罩路径修复并经Game/Editor诊断验证。此口径不等于AAA人物美术已经完成。

**2026-09-25 人物近景补充：** 同尺度中性/张口/单眼眨眼三态已在Game和Editor运行复核；人物演出专用太阳阴影偏移调低后，口腔由过亮转为接近UE的暗部，Game/Editor各330帧预热后均0帧超33.333ms。UE舌头/牙齿覆盖与HE仍不同，发片精细度仍待提升；不把该特写灯光修正记作最终AAA人物美术通过。

**2026-09-23 更新：** 优先修引擎通用缺陷。足锁停步后残留、自定义角色材质固定 UV0、UV1 法线切线基底已进入引擎并通过针对性检查；Selunae 139 骨、裙摆/发骨/脸部演出实际运行通过。单主角在同二进制/场景的两轮完整基准严格 PASS，但第二轮距 33.33ms 只有 0.299ms 余量；双演员场景严格性能 FAIL。Aster Editor 和默认游戏有界运行 PASS。近景美术及本地角色正式发布仍未完成，整体状态未完成。详见进度和验收记录。

本轮目标：可控角色、可信近景、室内外关卡、可复用制作流程及明确配置下稳定30FPS。两场冻结内容性能已通过，最终美术与最终内容发布尚未全部通过，**整体任务未完成**。

本页是2026-09-22当前状态汇总，不以旧阶段结论覆盖新缺陷。精确命令、构建版本、失败历史与检查边界保存在 [验收记录](engine-upgrade-validation.md)；人物/渲染对照见 [渲染记录](engine-upgrade-rendering.md)。下表证据路径除说明外相对 `out/agent/`。

**最新集成关口（06:57后）：** Game8E6E/Editor01c2及核心回归已PASS。Rain新灯光演出063046 PASS；头皮修复已canonical导入，19项nonhair+31资源字节不变，064518共32帧有限PASS。Rain064927、Carla065318完整性能均严格PASS，声明输入575/597前后不变。underlay与DentalV1/V2视觉FAIL未采用，Carla仍原Combined；眼部小灯只证明响应、未采用。package6 Release/cook及WARP/硬件DX12隔离启动均PASS；整体AAA美术未完成。

|本轮核心复验|最新证据（相对out/agent）|状态与剩余问题|
|---|---|---|
|构建/逻辑回归|`upgrade-sole-orientation-game-build.log`、`upgrade-sole-orientation-editor-build.log`、`upgrade-sole-orientation-unit.log`；SequencePlayback15.2s|build-only/codegen及针对性unit-test PASS|
|默认游戏|`upgrade-final-default-sole-check.log`|120帧runtime-manual PASS、PID101292 exit0，不代表玩法穷尽覆盖|
|Carla第二角色重导入|`carla-canonical-reimport-acceptance/report.json`|真实Importer PASS；15产物hash不变，材质/场景/helper保护及5morph/7clip/skin结构PASS|
|步态/实际交互|`carla-gait/20260922-060349/gait-summary.json`；`carla-interaction/20260922-061042/final-contact-result.json`|修穿地/旧跳点及最终交互有限PASS；左脚严格锁窗口不足；正式Rain接触061530/演出061646功能PASS，美术缺陷单列|
|Editor制作链|`rendering-evidence/vfx-authoring/upgrade-editor-9b8634efd9d0434f8042c49e7ed8d03f/report.json`（Carla）；同目录`upgrade-editor-d607a1039dd34162a5cc5ca676e34596/report.json`（Rain）|两报告PASS，0 validationError、均STOP；静态morph几何/稳定revision/保存重开已验|
|最新性能/发布|`aster-benchmark/20260922-064927`（Rain）、`20260922-065318`（Carla）|11700帧完整窗口均PASS：Rain81.18848FPS/P9915.45531ms/max24.975ms；Carla86.36475FPS/P9914.344704ms/max25.8948ms；各0超预算。package6隔离启动PASS（非完整Shipping玩法/性能证据）|
|当前美术缺陷/候选|正式头皮canonical修复；motion06451832帧|限定窗口PASS，19nonhair+31资源字节不变。underlay/DentalV1/V2视觉FAIL未采用；眼部小灯仅响应诊断未采用。AAA艺术仍未完成|

下表保留基础能力和对应版本证据；涉及最新复验状态以上表为准。

|核心能力|当前实现与本轮补充|前置依赖|验收证据|状态与剩余问题|
|---|---|---|---|---|
|P0 构建及既有功能回归|隔离Aster项目，保留默认Starbloom；修复旧场景空Sequence句柄兼容、测试fixture与资源类型问题|项目路径隔离、最新源码构建|默认Game `upgrade-default-game-final-build.log` / `upgrade-default-game-final-check.log`；PythonRuntime、PhysicsScriptContact、SceneEditing等单测见验收记录|已验证范围PASS；120帧原游戏运行不代表所有旧玩法穷尽覆盖|
|P0 角色导入、表情与稳定引用|Rain原生79骨、7动画、6表情；morph导入/解码/实例独立形变与前帧历史，坏源回滚与稳定重导入|原生mesh资源、骨架映射、合法资产|MeshCpuDecode / AnimationPoseContract单测；重导入fixture；`upgrade-sequence/20260922-023911/result.json`真实几何形变|功能PASS；角色仍为风格化美术，最终近景表演质量另验|
|P1 动作、接触与交互|状态机混合、事件、脚锁/IK/骨盆、头眼注视与手部约束接入动画更新；两演员可复用绑定|碰撞/骨架映射、场景目标、控制器|`upgrade-contact/20260922-022326/contact-playback.json`；Sequence023911两演员/交互/输入恢复PASS|功能PASS；非全身IK或Motion Matching，有限窗口不能排除全部穿透|
|P1 衣物与次级动态|CPU布料、碰撞、重置/降级、骨骼次级动态；正规导出固定点、马尾rest和围巾前部拟合；两实例共享物理配置|morph后形变生命周期、正确ClothMotion流、身体碰撞参数|`rain-scarf-native-clearance.json`：801固定点、自由尾部最小前向间距18.5486mm；`upgrade-motion/20260922-024925`与`deformation-stress/20260922-025004/result.json` PASS|数值与限定正面/急转视觉PASS；不是所有动作和毫米级碰撞保证。旧侧面PASS未覆盖的正面缺陷已修复|
|P1 皮肤、眼睛、发片与衣物材质|Skin/HairCards/Eye/Cloth正式shader与资源生命周期；曲面分层发片、原创atlas；局部皮肤散射近似；修正非布料COLOR_0污染|正确颜色/切线/遮罩与作者材质、真实近景资产|材质实机对照见渲染记录；`rain-mouth-color-fixed/0.jpg`与native流检查关闭cyan口腔回归|实现可用；guide152发片已冻结且有限运动/眨眼验收PASS，整齐轮廓/细发质量仍非AAA；不是发丝级系统或测量皮肤模型|
|P1 光照、阴影、反射与环境|复用PBR/HDR/曝光/色调映射、阴影、IBL/AO；补局部漫反射探针与动态人物采样；代表场景室内外/局部灯/台阶植被|Surface材质与关卡光照布置|代表场景运行与逆光/接触连续帧；材质和局部环境光对照见验收/渲染记录|已检查路径可用；当前配置SSR/GI关闭，以IBL和局部探针为基础，未宣称通用动态GI或重要镜面场景完成|
|P1 时间稳定与镜头|物体/骨骼/morph/布料前帧信息及TAA历史重置；跟随与避障、演出镜头；修正portrait→orbit连续构图/补光淡出|前帧形变、相机切换、冻结脚本与资产|已有48连续帧和cut窗口记录；脚本几何连续性static-check PASS|新开场64帧与既有portrait→orbit窗口有限复验PASS；细发锯齿仍在，不能宣称头发/睫毛全面时间稳定|
|P1 演出与数据制作|可保存Sequence编排镜头、动画参数、面部曲线、音频/事件；相对演员舞台绑定、停止恢复控制；第二演员/序列复用|可用角色morph、动画与绑定、音频资源|`upgrade-sequence/20260922-023911/result.json` PASS；Editor持久化证据如下|旧版本功能PASS；Rain061646演出功能PASS，新灯光063046已PASS，030250与030201仅对应旧窗口。音频流程示例不等于真人口型质量|
|P1 编辑、保存与资源生命周期|真实MaterialPanel共享提交/保存路径；stable-ID严格类型与范围校验；嵌套反射参数、场景覆盖引用和缓存更新|Editor作者通道、资源缓存、scratch路径边界|`rendering-evidence/vfx-authoring/upgrade-editor-ea4ecacd2e734ea8adb9207e99bee6c2/report.json` PASS，0验证错误、会话停止|Skin/Hair含cutoff、方向/点灯、Sequence/Animator/cloth保存重开及完整序列化等价PASS；原素材未变。此前cutoff覆盖缺陷已解除|
|P1 观测与优化|CPU动画/形变、GPU pass、内存预算、CPU图逐pass/提交身份与App分段；有界结构化采集退出写盘，Query/Buffer子计时|最新Game与冻结配置|041620完整v2采集与CPU图身份PASS；范围严格301–12000|采集成本在wall内，结构缓冲不丢记录；先前失败史保留，不宣称所有旧峰根治|
|P1 正式性能验收|Detailed原生1080p、TAA/AO/shadows/localprobes、SSR/GIoff|Game8E6E、独占窗口、冻结资源|Rain064927/Carla065318 result/identity/inputs-comparison/memory-summary|两场301–12000完整11700帧、演出覆盖、分辨率、声明输入稳定均PASS；不泛化至所有硬件/长时稳定|
|P1 发布|Release/PACK_GAME、碰撞重烘焙、依赖闭包、Dev隔离、独立Unicode目录启动|冻结正式 Rain 源码、脚本与资产|`upgrade-final-package4.log`、`upgrade-package4-hardware.log` PASS；2107文件，pak hash dd03f0dae4ba5b6f|旧正式 Rain 内容已 recook；package5已通过Release/WARP启动但不含新灯光，最新最终内容package6已recook且WARP/硬件隔离启动PASS。package4 WARP/硬件 DX12 均2帧启动exit0，启动不作性能证据，包不覆盖本地 Carla 候选|
|P0 最终美术与完整交付|合法Rain角色、室内外代表关卡、专用人物材质与可玩演出已集成|作者资产质量与独立实机视觉判断|现有分项截图/运行证据，不能用接口或编译替代|AAA美术要求尚未达成；发片仍有平行刘海、稀疏冠顶。限制明确保留，不将占位或风格化演示标记为AAA完成|

## 剩余实施与验收顺序

1. 当前正式 Rain 资产及场景冻结，发片/围巾/演出/相机已有限复验。独立本地 Carla 研究继续真实脸骨、眼部/发际线与动作重定向，未通过前不替换正式角色，不将研究结论写成AAA美术通过。
2. 暂停生命周期和停止后单步/足骨失效清理已修复，完整 v2 基准 041620 已通过。Editor 停止/无播放时手动表情预览回归已修复，单测与实际作者通道验证 PASS：真实几何修改、稳定 revision、保存重开一致；最新默认游戏构建和120帧运行 PASS。Carla最新步态/交互与Editor制作链已有上表有限证据；左脚严格锁窗口不足，最新Rain/Carla性能均PASS，见当前表。
3. 正式 Rain 旧版本内容 package4 已重新 cook、隔离 WARP/硬件 DX12 启动与依赖检查 PASS。本轮接触源码已变化，package5 Release/WARP启动已通过，但新灯光及正式头皮修复的package6已recook且WARP/硬件隔离启动PASS；本地 Carla 资产不自动进入可分发包。
4. 汇总实现完成/验证受阻/尚未完成三类结果。只有核心功能、制作流程、代表场景、必要回归、性能和最终内容发布均满足要求，且无阻断问题，才能结束整体任务。

基础算法与生产流程优先复用现有实现；暂不扩展开放世界、海量角色、复杂联网、路径追踪或未有收益证据的高级系统。账号、付费与资产许可条件不得虚构；详细历史失败与已修原因继续保留在验收记录中。

## 2026-09-24 追加：Mechanic Girl / SICKA DYNASTY 路径

|优先级 / 能力|已有证据与本次实现|依赖与验收|状态 / 剩余问题|
|---|---|---|---|
|P0 资产与稳定重导入|UE只读导出→稳定ID glTF→content_sync；115骨角色与105种关卡网格、47灯、1260结构碰撞；最新 plan 零差异|项目资产/原 UE 授权；Game/Editor 打开、重复 plan|引擎流程已验证；原始 UE/E 未改，跨项目搬迁稳定ID未验|
|P0 可控人物与碰撞相机|走跑跳、跟随镜头、动态衣发；相机穿龙雕像由结构碰撞修复，脚本与通用 Animator 改用真实碰撞后位移速度|Jolt 静态三角网格、角色控制器；Agent 输入回放与截图|限定路线可用；快转、所有台阶和交互仍需验|
|P0 帧时间与卡顿|保守视锥裁剪、BC7/mip、环境雾；运行开始前预载动画控制器并预热形变拓扑；Editor空闲增量改动检测、按需拾取更新、层级子列表裁剪；网格局部矩阵与静态世界包围盒缓存、提交对象运动历史修正及可选逐系统计时；只读Windows电池事件关联工具|i7-11800H/RTX3060 Laptop/DX12/RelWithDebInfo/1280×720；Game固定回放及Editor多次900/3600帧检查|最新Gain/Offset正式版Game/Editor各1800帧、预热后各1740帧均0帧超33.333ms；此前Game3600帧有84帧GPU超时，与同秒电池计数0→1及GPU时钟下降重合，另有CPU短峰。跨供电状态的严格稳定30FPS尚未证明；须区分引擎CPU瓶颈与外部设备电源状态|
|P1 场景画面|完整布局、PBR贴图、直接光与双层雾已对照；UE编辑器视口两机位校准正式环境光0.55；通用六面HDR探针导入已在独立HE场景验收；69棵树共享的Foliage顶点风动、21处原UE落叶发射器按最近树木主叶材绑定颜色，另有2处项目可编辑前景布置与6处旋涡近似效果；6盏器皿灯经UE数值核对和通用glTF准备工具重导入；298处UE枯叶贴花经通用离线工具烘为73组静态透明网格并接入正式场景|可用的天空/GI/局部反射/贴花/VFX资产及渲染接入|GI、SkyAtmosphere、局部捕获和真正的投射贴花仍未等效迁入；平面贴花不能自动贴合复杂台阶和墙面；旋涡/落叶为近似效果而非Niagara逐参数复现，正式树冠与整体画质仍有明显差异|
|P1 人物近景与演出|Mechanic Girl 骨骼动画、52 morph 名称、发片与衣发有限动态；通用 C# `SceneSequence` 控制和项目佛像交互/近景演出已接入|优先做头发急转/低帧率运动及细边逐帧 UE/HE 对照，再验表情曲线、镜头/音频同步；口腔以正常驱动和近景无明显穿帮为准|交互与短演出已可重复回放；发片自然度和时间稳定性仍未达到可宣布 AAA 的证据，口腔不阻塞头发工作；首次演出镜头切换有 CPU 慢帧|
|P1 制作与回归|项目数据和脚本外置；环境雾字段已接入反射序列化；Game/Editor、新场景、横向展台检查PASS；Aster检查PASS|ClockworkGarden/默认项目检查、编辑保存重开、最终持续运行|部分验证进行中；不得用编译或单个镜头替代整体验收|

优先顺序：头发运动自然度与跨帧稳定性、首次演出镜头的 CPU 慢帧；再处理关卡关键 GI/贴花/VFX、角色近景、编辑保存和长时运行。首播第75帧现已定位到新可见DDS的冷文件读取，须实现有界异步预读并在镜头使用前完成；第62帧的管线录制峰值另行处理。口腔按基础表情与近景无明显错误验收，不用舌齿细节拖延头发；其余核心缺口不降级为可选扩展。

进度更新：通用有界DDS字节预读、演出字段反射及Cooker依赖已接入，三张项目纹理在正式演出中`3/3`命中；首播峰值在一次同条件Game500测量中76.28→53.26ms，但仍有14帧超33.333ms，P0帧时项保持未完成。下一步分析第62帧首次图录制及余下CPU慢帧，并验证发布包使用的资源路径和真实首次演出。头发自然度仍比口腔精细度优先。

最新归因：通用帧统计已确认首次佛像镜头14个CPU慢帧均对应约10ms的RenderGraph重建；新网格进入缓存及首次GPU上传是主要触发，另有第75帧纹理表变化。需在正常资源管理路径中设计有限、可取消的静态网格GPU预热，覆盖镜头将见的资产并保持场景切换/打包生命周期；完成后重测首播及Editor。人物侧仍优先处理发束急转、碰撞与跨帧细边自然度，口腔仅按宽松标准检查。

2026-09-26 用户将本次验收放宽并要求优先交付完整目标：现有可控角色、正式SICKA场景、UE固定机位基本对齐、树木与落叶、通用UE/Fab导入工具和文档、Game/Editor预热后900帧及最终Release包均有实际验证。当前漫游Game840测量帧仅1帧轻微超过33.333ms，Editor840帧0超时，按“基本稳定30FPS”可接受。首播佛像镜头14个图重建慢帧、发片细边粗糙、GI/粒子与UE不完全等效列为明确的后续画质/性能限制，不再把严格逐帧或逐像素等效当作本次宽松交付的阻断条件；后续优化仍应在引擎通用层实现。


### 2026-09-28 渲染 / 60 Hz / 前端与换装

基线 HEAD a8185d17；外部项目 MechanicGirlLab；原 UE/Fab 资源只读。
UE5.4本地BRDF/HeightFog源和SceneTest Lumen配置用于审计；本轮采用适合现有架构的实现，不宣称同名效果与UE逐参数等效。

| 核心项 | 本轮实际实现 | 证据与状态 |
|---|---|---|
| PBR / 雾 | 统一相关Smith直接光/IBL；解析高度雾；皮肤UV1/two-sided法线一致 | RenderingShaderMath PASS；Game/Editor画面已验证 |
| HDR / 皮肤 | 双边亮度局部曝光、EV补偿、参数持久化；分离diffuse的屏幕空间皮肤扩散，保留高光/雾/对象边界 | RenderSettings PASS；1080p A/B运行PASS；皮肤两pass约0.37ms；均衡档默认启用 |
| 间接光 / 接触 | 复用IBL、GTAO、SSGI、SSR；以实测配置三档 | Balanced使用IBL+GTAO；High增加SSGI/SSR，当前硬件不满足60FPS |
| CPU / 60 Hz | 按资源索引图编译；加载期真实GPU预热；共享只读材质快照；材质当帧去重；PostTAA深度实例化 | 1080p Balanced实测1380帧：98.33%≤16.667ms，CPU P95 15.52ms，GPU最大13.09ms；仍23短慢帧，不是严格逐帧或长时保证 |
| 菜单 / 制作 | RmlUi中文/英文原创铜金界面，轻量独立展示台，面板专属姿态，实际设置持久化；加载页Rml overlay | 720p/1080p及120%UI、完整场景往返PASS；11→3676→11实体 |
| 换装 | 骨架兼容验证、私有形变实例缓存、不可见资源预热、兼容mesh保留动画时钟和状态 | AnimationPoseContract PASS；五套外观（两种兼容mesh+材质变体）实际切换PASS；不兼容绑定继续拒绝 |
| 移动 / 射击稳定性 | fixed-step呈现插值；稳定四元数→Euler转换下沉Math及CameraFollow；冲刺注视权重平滑 | 多帧率/传送/暂停/父子mesh/导入负尺度测试PASS；同回放实际视线异常跳动2.976°降为正常回正最大0.06494° |
| 回归 | Game/Editor、默认项目、小型逻辑及已有导入契约 | Game最终build/runtime PASS；ImportedQuarterTurnTransform PASS；最终Editor build/900帧capture PASS（详见验证记录） |

UE参考文档：https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
启动与性能复测：外部项目 FRONTEND.md、Tools/Measure-RenderUpgrade.ps1；精确命令/硬件/限制见 engine-upgrade-validation.md。
最终画质仍有发片细边、屏幕空间反射/GI覆盖与UE差异；本轮没有完整复制Lumen/虚拟阴影，也没有宣称最终角色达到可证明的AAA美术质量。Shipping未新增执行，此轮为开发版Game/Editor交付。

### 2026-09-29 display/performance acceptance scope

| Item | Implementation / acceptance | Status |
| --- | --- | --- |
| Default fullscreen, saved window mode | Engine startup policy + GLFW borderless | PASS: runtime roundtrip/Saved values; startup precedence unit-tested |
| Quality dropdown | Rml choices, mouse/keyboard, explicit apply/cancel | PASS: actual mouse/keyboard and effective settings |
| Native UI, lower-resolution 3D | RenderHeight cap, aspect preservation, tone-map upscale, temporal reset and config preparation | PASS: actual native/720/1080 extents, TAA/FXAA/SMAA, 4K image QA |
| 4K performance | Same 1500-step replay, 120 warmup, native vs1080 internal, actual extents/CPU/GPU/VRAM | PASS measured target: Performance99.64%, Balanced98.84% within60FPS budget; residual slow CPU frames documented |
| Regression | RenderSettings/GameDisplaySettings, hidden-resource warmup, spatial AA, Editor native path | PASS: unit/build/runtime image checks; native D3D debug layer unavailable |
