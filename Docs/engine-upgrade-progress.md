# 升级进度

## 2026-09-28 MechanicGirl UE 渲染与发束升级

- 对外部独立项目只读审查了 UE5.4 `SceneTest`：DX12/SM6、Lumen GI/反射、虚拟阴影、命令行加载的 TSR 方法值 4；头发母材质是双面遮罩 `MSM_HAIR`，皮肤是 Subsurface Profile。分析与有效实例参数在 `C:\WorkSpace\HEngineProjects\MechanicGirlLab\Validation\character-render-study.md`，不能以物理资产引用推断 UE 正在模拟发束。
- 通用 `HairCards` 增加可选线性 Strand ID 贴图方向扰动和有效 Albedo 开关，默认材质参数保留旧行为；项目五款发型重建并绑定压缩 ID 图。项目 TAA 替代 FXAA，目标是改善发片细边。HE 仍没有 UE 的 TSR、Lumen、虚拟阴影及 Subsurface Profile 全部效果。
- 通用 `SecondaryMotion` 修复厘米骨架 0.01 缩放下发梢半径未缩放的问题；阻尼改为发梢相对发根速度，并增加默认关闭、可保存的 `MotionDrag` 跑动气流。项目 17 根发骨按根梢分级配置。急转/持续跑动近景同输入 On/Off 捕获有可见侧发变化；统一大幅放松刚度的候选压到衣领，未采用。正式 3.8 m 跟随镜头头发只占几十像素，仍不能宣称 UE 动态自然度或最终 AAA 发片已达成。
- `AnimationPoseContract` 无窗口单测含缩放碰撞、移动气流及反射往返，PASS；MechanicGirlLab Game 近景捕获、正式场景 Editor120 均 build/run PASS。正式4K/RTX3060 Laptop/DX12/RelWithDebInfo/走跑跳300帧，预热60后240帧 CPU/GPU P95 为31.101/22.701ms，6个CPU帧超过33.333ms；720p/900帧后段所有GPU通道同步变慢，原因未定，严格稳定30FPS仍未通过。项目材质和场景生成器重跑后分别保持22份材质与16个场景聚合哈希不变。
- 第二次同配置720p/900帧同步采集 NVIDIA 时钟，840测量帧 GPU P95/max8.17/9.25ms、0帧超33.333；测量期间22个时钟样本均P0、1.98–2.00GHz，后段慢帧未复现。前段仍有10个CPU帧超33.333，首个慢段原因未完全解决；慢帧那次没有时钟采样，不能断言设备降频。通用二级运动改动后外部AsterCourtyard独立Game90帧构建/运行PASS；默认项目及其它角色的完整视觉回归未重测。
- 后续只读UE关卡审查新增灯光/后处理证据：2方向、8点、3聚光、1天空光和1个全局后处理体。HE脸部旧对照场景仅1方向光+环境光，不能把全部肤色差异归于着色器。外部项目五个固定近景布光候选Game90帧capture均PASS；全舞台灯光过亮、仅主光头发过黑，均未覆盖正式场景。UE皮肤Subsurface Profile、TSR、Lumen和光照/背景对齐继续为未完成项。

## 2026-09-26 通用遮罩变更后的跨场景回归

- 同一版引擎下，外部`MechanicGirlLab`的UE人物`UEFacePerformance.he` Game90帧构建/运行/截图PASS，发片与面部无新整卡矩形；正式`SickaDynasty.he`走/跑/跳输入回放Game300帧构建/运行/截图PASS，后240统计帧CPU中位/P95/max23.477/25.402/32.031ms、GPU最大12.179ms，0帧超过33.333ms。正式场景Editor300帧构建/运行PASS、`dirty=false`，后240帧CPU中位/P95/max19.797/23.210/26.007ms、GPU最大12.193ms，0帧超过33.333ms。该短测不抹去此前Game900末段设备慢帧，长期稳定性仍按异常单列。
- 默认Starbloom项目仍由`HEngine.workspace.json`指向仓库内`Project`，但本轮冷Game构建在1184目标中途因C盘可用空间降至约0.4GB主动停止，不能记作默认游戏回归PASS。`hectl clean cache`清掉已停止的默认Game部分缓存916085965逻辑字节，C盘可用空间从约394969088增至约1320574976字节；旧`.retired-*`残留与已登记证据的package缓存未动。现有默认项目编译/运行回归仍需有空间后补做。

## 2026-09-25 遮罩树叶矩形与UE参考核对

- 正式SICKA右侧48棵密树冠的项目材质曾把`AlphaCutoff`设为0；HLSL `clip(0)`保留完全透明像素，且源4096²叶片纹理空白区含1/255、2/255等残余alpha，HE近景因此出现整张矩形叶片卡。引擎`MaterialSurface.hlsli`现统一在GBuffer、前向、轮廓和深度/阴影路径中对遮罩覆盖率应用1%下限；项目密树冠改为0.01并由生成器拒绝低于0.01的配置。近景同机位0/0.01对照见外部项目`Saved/Diagnostics/SickaTreeCutoff00vs01Matched.preview.jpg`；专门把材质重置为0的诊断场景经引擎修复后与0.01场景在1280×720第90帧RGB图逐像素相同（MAE 0），`SickaZeroCutoffTreeMaskFloor-90.preview.jpg`已目视叶片轮廓。
- UE原生编辑器视口参考`SourceAssets/SickaDynastyUE/References/street-final-ldr-viewport.png`本身也有粗大树冠块面；独立SceneCapture原始/预热8次/强制69棵树LOD0的三张参考图均保留块面，不能归因为简单的预热不足。这些大块形状是参考图异常，不作为HE画质目标，也不能用像素MAE优化去复制。HE按遮罩边界、深度/阴影/前向与GBuffer裁剪一致性，以及后处理后的视觉结果验收。HE此前的近景整卡矩形有单独的透明裁剪原因；UE源资产与原始资源均未修改。
- 零阈值回归场景Game/Editor各90帧构建运行PASS；旧/新引擎并排图`Saved/Diagnostics/SickaZeroCutoffEngineFixAB.preview.jpg`清楚显示整卡矩形消失。正式SICKA Game900帧画面可用，但第825–900帧连续GPU慢段导致76/840测量帧超33.333ms；结合此前正常短测，仍只能把全程30FPS记为设备状态下未验证通过，不能以画质修复宣称性能问题消失。

## 2026-09-25 SICKA落叶按实际渲染树材质绑定

- 找出旧色表校验缺口：UE对象`SM_Tree_11`/`Tree04_1`在HE导入后分别以显示标签`SM_Tree_3`/`Tree04_52`出现；树的世界位置、网格仍正确，但旧色表只检查UE原材质，未确认HE实际树材质。生成器现在以UE最近树位置匹配唯一HE树实体，依据其当前叶材选择暖/绿效果并记录HE实体UUID、标签和材质；校验同时核对实际叶槽与唯一脚本发射器。当前正式场景仍为21暖/0绿、3568实体、2112光照图绑定，重建两次场景SHA稳定`7BF563A676959FC4B5AD1D9857DC8BAF967C3BE69A84122764FCBF976C7A6987`。
- 项目生成器新增绿色普通/密树冠材质，继承相应AlphaCutoff并把原叶纹理保持绿色；单独暖/绿成对诊断场景使用同一树与镜头，暖/绿效果包分别RGB8(64,52,35)/(56,63,43)，Game各240帧运行PASS。有/无绿色发射器同帧图差异7042个RGB通道采样，证明绿叶包实际播放；树冠近景仍有粗发片形状且粒子很小，此证据不代表UE美术等效。正式关卡树和落叶外观未改，原UE/Fab资源未动。
- 正式900帧Game首次840测量帧1帧33.807ms、复测1帧34.797ms，GPU均<11.1ms；Editor首次有第493–587帧95连帧GPU 42–115ms突发，未同步采到时钟，原因未确认。Editor复测附`nvidia-smi`遥测时840帧0超限，CPU中位/P95/max19.984/23.587/28.160ms、GPU最大11.852ms；不能用复测抹去首次异常，也不能在无同段遥测时断言降频。性能目标按用户宽松口径接近满足，跨设备状态的严格稳定性仍未证明。

## 2026-09-25 SICKA落叶尺寸同帧对照

- 复核引擎链路：Effekseer在HDR色缓冲、色调映射前绘制；SDK受光材质使用纹理色乘环境光，正式场景IBL强度0.55。此前受光版变近黑并不能单独证明引擎传参错误，保留当前不受光的暖褐叶片方案。
- 同一正式场景900帧无输入Game捕获比较0.10m与0.13m叶片，后840帧两者都构建/运行成功。双图像整帧RGB8 MAE 0.00775、变化1673通道采样；并排预览`Saved/Diagnostics/LeafSizeAB/side-by-side.preview.jpg`未见值得增加30%尺寸的视觉收益。0.13m试验的CPU最大37.285ms有1帧超33.333ms，0.10m最大27.754ms无超限；不能据此单次A/B证明尺寸造成慢帧。正式配置和效果包已恢复0.10m，包SHA与试验前备份一致。下一步应优先解决粒子受光/雾/形状与UE差距，而非单纯放大叶片。

## 2026-09-25 UE人物脸部近景验收镜头

- 近景口腔亮度根因拆分：UE`M_SKIN`的MatID八色默认全白，缺该分支不解释口腔偏亮。同镜头Sun关阴影、单调ShadowDistance15、CascadeLambda1、ShadowMapSize4096均几乎不改变舌面；单调DepthBias0或NormalBias0有变化，二者叠加使口腔转暗。完全零偏移产生肩颈脏影。项目人物特写正式选择2048阴影图、DepthBias=.0002、NormalBias=.02，在保留150m阴影范围下舌面ROI均色RGB8从(168.9,113.1,94.4)降至(79.7,44.2,35.3)，见外部项目`Saved/Diagnostics/UEFaceMouthFinalComparison.preview.jpg`。该值写入`Configure-UnrealMechanicGirl.py`的`face_preview()`与`UEFacePerformance.he`，重建的表情诊断沿用。引擎已有每盏灯可编辑偏移/分辨率，横向展台与SICKA不改，不能把项目特写灯光冒充引擎通用阴影修复。
- 正式人物演出Game/Editor各330帧、预热60后270帧均0帧超33.333ms；Game CPU中位/P95/max7.612/8.601/13.243ms、GPU max3.585ms；Editor5.798/10.038/12.136ms、GPU max3.176ms，Editor`dirty=false`。HE口腔暗度已改善，但UE参考的舌头覆盖/牙齿轮廓及发片精细度仍有差异，AAA近景美术未验收完成。
- 原HE表情诊断镜头人物头部过小，不能直接据此评估UE近景质量。外部`MechanicGirlLab/Tools/Build-FaceReferenceFraming.py`从已验收的绑定姿势与张口形变场景生成同40°视角、接近UE面部屏幕尺度的中性/张口/左眼眨眼三张独立诊断场景；生成两次SHA一致，不改角色、原UE资源或正式场景。
- 三场Game各90帧capture build/run PASS，Editor张口场景60帧check build/run PASS且`dirty=false`。同帧表情ROI相对中性图>5 RGB8变化像素：张口18406/32000、左眼眨眼2851/16800；UE/HE六图并排见外部项目`Saved/Diagnostics/UEHEFaceReferenceFraming3x2.preview.jpg`。形变、眼睑动作可用，但HE口腔内部更亮、发片轮廓更粗且舞台灯光/背景不同，近景AAA美术仍未通过，不能以此结束整体目标。

## 2026-09-25 落叶颜色与树叶材质复核

- UE导出的`MI_TreeBranch2`确实使用偏绿的叶纹理（遮罩内中位RGB8 101/115/78），但原材质`diffuse color`为(1/.702458/.233331)，正式场景树冠因而呈暖橄榄/褐色。21个Niagara位置附近的树全部使用该材质；正式HE场景21个对应发射器全部引用暖色包，包的生成基色RGB8为64/52/35。独立绿色包RGB8 56/63/43留给绿色树材质，不误用于这些暖树。地面298处另用原UE金黄枯叶贴花，和空中落叶生命周期不同。
- 同一900帧回放A/B发现Effekseer方向受光把暖叶压成近黑轮廓，即使通用VFX环境光改为直接跟随场景IBL仍偏暗。项目落叶因此选不受光的暖褐发片，保留可编辑色值与原UE遮罩纹理；引擎通用VFX环境光不再额外四分之一缩放。第900帧并排图见外部项目`Saved/Diagnostics/SickaLeafColor900AB.preview.jpg`，棕叶可辨但不是UE Masked/Default Lit完整等效。旧Fab/UE源文件未改。
- 更暗的树冠材质0.8/0.6诊断仅带来固定双机位整图MAE约0.04–0.14的微小变化，视觉收益不足，未改正式树材质。Game/Editor均已重新构建运行；Game 900帧预热60后的840帧CPU最大31.564ms、0帧超33.333，GPU最大10.924ms。近景头发、脸部AAA美术质量仍需独立验收。

## 2026-09-25 宽松目标的正式场景短测

- 用户更新SICKA画质为与关闭Wave/Sharpen的UE视口基本一致、预热后稳定30FPS且设备降频可单列。新Game/Editor各900帧、预热60帧的840统计帧均0帧超33.333ms；Game走跑跳固定第140/220/270帧的可视截图PASS，旧人物横向展台Game/Editor各120帧PASS，Editor场景`dirty=false`。UE/HE两机位并排审查结构基本对应，树冠/GI和暖光仍有美术差异；此前长测硬件降频事件保留为条件限制。人物近景AAA质量和全部旧场景回归仍不可由这些宽镜头直接认定。

## 2026-09-25 UE/Fab导入重名资源可靠性

- 通用`Tools/UE/ExportLevelTextures.py`对不同UE包下同名贴图生成稳定路径哈希后缀，并在`texture-export.json`写相对输出；`PrepareLevelForHE.py --texture-manifest`按完整UE包路径解析，旧无重名导出兼容。缺清单时遇到同名贴图明确失败；短名相同的UE材质也不再静默覆盖，可由项目profile的`materialSourcePaths`显式指定；重复glTF材质名拒绝生成重复HE稳定ID。原UE资源只读。
- SICKA现有53张贴图无重名，带新参数实际重跑派生glTF成功，3463节点/105网格/20材质/53贴图；源glTF、材质/贴图清单及派生glTF/.bin前后SHA256均不变，现有项目引用不受该改动影响。尚未在真正含重名Fab包的UE编辑器内执行导出，头less模拟只验证导出命名/manifest合同。

## 2026-09-25 夜：Editor持续帧率定位

- 冷重建Editor后900帧短测仅1个CPU慢帧，3600帧长测复现94个连续GPU慢帧。独立NVIDIA遥测与帧CSV按本地时间对齐，核心时钟/功耗在慢段从约1980MHz/105W降到210–1005MHz/25–39W，限制位包括软件功率与温度；绘制量不变；System日志同段电池计数1→0→1，需区分时间相关与故障定因。Editor严格30FPS仍FAIL，当前证据指向设备降频参与慢段，不能通过虚报优化完成目标。下步需稳定供电/散热条件与可保画质的负载控制实验。
- C盘约1.19GB时，清理器因历史会话证据拒绝删除整套Game缓存；对闲置Game缓存`Engine`/`deps`的安全定向清理遭自动审批`blocked by policy`，未绕过。仍可进行只读分析和文档工作，后续构建可能受磁盘余量限制。

## 2026-09-25 晚：树叶配色与固定镜头诊断修正

- UE原关卡21个落叶点附近树的主叶材质全部为暖色`MI_TreeBranch2`；项目色表的暖叶由偏紫灰RGB8(61,50,54)调整为棕色(64,52,35)，绿色备用变体仍为(56,63,43)。生成器与校验中的棕色通道次序修正，`Build-SickaLeaves.py`和`Validate-SickaLeafPalette.py`PASS；近景Game捕获PASS，无白片但远景粒子仍是小暗点。
- 发现前期固定街景镜头与玩家出生点重合，局部黑色“叶片”其实是近距离角色发片。移除角色可视实体后重新捕获的前/后视Contrast1.15→1.25整图MAE分别18.845→18.128、18.328→18.184，原有1.25决定仍成立；旧含角色的树冠阈值A/B与局部归因作废。无角色树冠阈值.005/.02可消除巨大块面，但与所留UE视口截图的块面形状差异更大，未修改正式树材质。
- 修正项目固定前/后街景诊断基线：各移除与相机同位的角色可视实体，重跑五个诊断生成器；Contrast1.25生成场景与修正过的无角色场景哈希一致，正式可玩场景不变。
- 通用Game Dev Agent只读`render.object.lookup`可由surface-id对象序号查实体UUID、标签、材质与网格路径；编译和运行时反查PASS。项目总体目标仍未完成：Editor 30FPS长段慢帧、角色美术、树冠与UE高质量画面对齐仍需处理。


## 2026-09-25 SICKA双机位对比度与树冠问题

- 前后固定512²画面将项目Contrast由1.15试至1.25/1.35，1.25使对UE整图RGB8 MAE前视19.731→19.089、后视19.184→19.028；道路区域小幅变差，1.35整体和道路均较差。正式项目生成器/场景采用1.25，3568实体仅Environment一个字段变化，二次重建SHA256均`7bf563a676959fc4b5ad1d9857dc8baf967c3be69a84122764fcbf976c7a6987`。Game900固定回放840统计帧CPU/GPU最大31.841/10.862ms、0超标。
- Editor900冷构建/运行PASS且打开正式场景`dirty=false`，但第726–825帧连续100个GPU慢帧、最大GPU114.396ms/CPU114.524ms，严格30FPS FAIL；本次时段未发现Kernel-Power 521事件，不能照搬先前供电切换解释。前视明显悬空深色树卡定位到`SM_Tree_4`影响区域；密树冠遮罩阈值0→.005/.02使双机位误差显著恶化且卡片仍在，已否决。后续隔离材质槽/网格导出，不能以删树或实心化取代修复。

## 2026-09-25 UE关卡重导入稳定标识

- 通用`Tools/UE/PrepareLevelForHE.py`为新项目增加可选配置`identityKey`，以稳定UE资产路径而非临时导出glTF绝对路径生成场景/节点/网格/材质`he_id`；旧项目未配置时保留原路径算法，SICKA既有引用不变。文档明确派生glTF的内容同步路径仍须稳定。单元测试验证同键跨导出目录ID一致、不同键ID不同、旧算法精确保持，并继续验证遮罩阈值行为。
- 对最新Editor900帧CSV的751/752帧分解：CPU更新约7→13–14ms且渲染提交约12→20–22ms一起变慢，GPU跨度仍约11ms、可见物/批次/阴影绘制不变；现有证据不足以将此两帧归为某一引擎子系统，严格Editor30FPS尚未通过。

## 2026-09-25 落叶与树冠颜色复核

- 对只读UE地图检查21处落叶发射器的最近树及主叶材质，全部为`MI_TreeBranch2`暖色树冠，正式场景应使用暖色粒子；绿色包只为未来真实绿叶树保留。原树叶底图不透明像素中位RGB8(101,115,78)，实例底色因子(1,0.702458,0.233331)，而UE粒子背光基调为(58,48,70)。旧暖色包RGB8(162,156,58)在HE走跑跳镜头显著偏亮黄绿。
- 项目`Build-SickaLeaves.py`现从树材质暖色与UE粒子基调混合，默认暖色RGB8(61,50,54)，绿色(56,63,43)不变；21处脚本绑定不变。正式Game第900帧与近景第180帧受控捕获均运行PASS，目视暖叶暗红褐且有叶形、未见白面片；完整树冠照明与UE仍有差异。`SickaTreeColor`材质全局G/B乘1.4的双机位A/B使整图及上部MAE均变差，未采纳。

## 2026-09-25 SICKA正式方向雾起点校准

- 对树冠遮罩对齐后的同机位A/B：全局关阴影使树冠红色过亮且绿蓝缺口仍在；UE雾起点0直接套到HE则严重过雾（前视整图RGB8 MAE16.833→38.246）。HE项目雾起点12米相对原18米让前视整图16.833→16.559、后视20.103→17.899，后视上部28.861→23.498，后视道路10.640不变；前后并列预览已目视。此为项目标定而非引擎算法变更。`Build-SickaDynastyScene.py`写入12米，正式3568实体仅Environment一个字段变化，二次重建SHA256均`8d367e82492058205053ab7e802b1d1d9b54053c2ef14d1b84fd47ad3cf9a0c7`，落叶绑定21处不变。
- 新正式场景Game第180帧固定走跑跳回放画面捕获与旧18米同帧并列PASS；900帧Game预热后840帧CPU/GPU最大26.267/10.951ms、0慢帧。Editor冷构建与900帧场景运行PASS，`dirty=false`，GPU最大11.592ms/0慢帧，但CPU第751–752帧36.185/35.598ms、2慢帧，严格Editor30FPS仍FAIL，未以本次短测掩盖此前3600帧供电相关慢段。Editor缓存结束后按`hectl clean cache`回收5,904,187,597逻辑字节，C盘空闲实测7,583,485,952字节，保留外部项目资产/CSV与Game缓存。

## 2026-09-25 方向雾阴影候选与剩余误差定位

- 在引擎着色器中隔离尝试3点沿视线方向雾密度加权阴影采样，仅当方向雾强度>0才激活。Game受影响shader真实构建/运行PASS；与未遮挡方向雾的同机位诊断相比，前视整图UE RGB8 MAE14.6982→14.6302、后视16.0444→16.0473，前视微弱改善、后视退步，三列预览肉眼几乎无差异。逐像素额外阴影查询缺少足够收益，已撤回候选并重建Game；正式场景/UE原件均未改。
- 两个512²固定机位按相同矩形区域统计，顶部树冠/天空混合区RGB8 MAE前视23.084、后视26.382，街道中段约17.9，两机位道路约7.18/8.19。上半部是剩余大误差区域；这只能用于排查优先级，混合矩形同时包含天空和几何，不能据此把误差全部归为树叶材质。

## 2026-09-25 UE直方图曝光与HE自动曝光隔离对照

- UE只读`lighting-settings.json`标明PostProcess使用`AEM_HISTOGRAM`、亮度范围0.6–0.8；HE已有通用GPU自动曝光，但当前项目关闭。用项目`Saved/Diagnostics/SickaAutoExposure.patch.json`仅在受控Game capture中启用HE自动曝光（Key0.18、EV范围-2至4、适应速度4），正式项目配置/场景未改。同一512²前视第90帧相对UE参考整图RGB8 MAE由手动17.746升至自动69.255，预览显示地面和雾明显过曝。该参数/算法不能直接视作UE直方图等效，暂不采用；下一步聚焦局部树冠、间接光和雾，不能把全局提亮算作画面对齐。

## 2026-09-25 长时Game/Editor复测与供电事件隔离

- Game开启现有`HE_RUNTIME_SYSTEM_TIMINGS=1`/`HE_LAYER_UPDATE_TIMINGS=1`再跑同一3600帧回放，3540统计帧CPU中位22.084ms/最大30.123ms、GPU中位9.672ms/最大10.894ms，均0帧超过33.333ms。系统日志唯一62.3ms脚本峰值发生在正式统计前的启动帧；前次第365帧40.28ms CPU峰值未复现，故尚不能断言其根因或声明跨次稳定通过。
- 外部项目Editor冷构建及3600帧场景运行PASS，编辑器日志确认打开`SickaDynasty.he`且`dirty=false`；3540统计帧CPU中位20.435ms、P95 25.424ms、最大110.452ms（94慢帧），GPU中位10.943ms、最大109.672ms（89慢帧）。连续GPU慢帧1551–1639对应本机19:19:20.783–19:19:26.194，Windows Kernel-Power 521有效电池计数在19:19:21.100变0、19:19:27.135变1；第二次出现精确时间相关，仍未证明物理供电/驱动因果，严格Editor30FPS FAIL。另有不在该连续区间的CPU慢帧需要独立处理。
- Editor非GPU慢段的少量CPU帧分别有帧357–358渲染提交升至约20–22ms、帧1680有约18ms未被更新/渲染提交分项覆盖、帧1681/3537 ImGui约7.7/8.2ms；不能把这些峰值归咎于供电事件，也不能只用GPU段剔除后宣称严格通过。
- 为Editor冷构建腾空间，核验无人使用、无联接/`.he-keep`后使用`hectl clean cache`回收默认Game可重建缓存，逻辑5,763,806,150字节，C盘空闲约2.23→7.89GB；Editor构建后空间降至约1.3GB，再按相同检查回收闲置Editor缓存，逻辑5,904,465,989字节，实测空闲7,212,527,616字节。外部Game缓存与项目资源/验收CSV保留。

## 2026-09-25 正式场景长时性能复测

- 同一硬件、DX12/RelWithDebInfo/1280×720及3600帧回放再次运行Game，60帧预热后3540帧CPU中位21.997ms、P95 23.765ms、最大40.281ms，仅1帧>33.333ms；GPU中位9.660ms、最大11.060ms、0帧>33.333ms。上次GPU连续97慢帧未复现，供电事件相关性不能当作根因结论。唯一CPU慢帧365的更新阶段28.91ms、动画8.55ms、形变8.00ms、布料求解5.15ms，对照相邻帧更新约11ms、动画约4ms，仍需分离瞬态调度/脚本与模拟成本。严格稳定30FPS仍FAIL，未降低场景或效果配置。

## 2026-09-25 落叶与树冠色谱复核

- 当前外部MechanicGirlLab项目的树叶源贴图不透明像素中位RGB8为(101,115,78)，但`MI_TreeBranch2`基色系数(1,.702458,.233331)令树冠呈暖橄榄色。21个正式落叶发射器全部绑定附近该叶材，正式包暖叶基色RGB8为(162,156,58)，绿色变体(56,63,43)尚无对应绿叶树发射器。`Validate-SickaLeafPalette.py`核查最近树、场景脚本、源纹理/材质/外观配置指纹通过；没有发现把绿色树误配成褐叶的现存实例。近景同帧开关捕获可见叶片替换背景蓝色，受光和雾仍影响最终观感；本次只确认色谱绑定，不宣称与UE Niagara完全同貌。

## 2026-09-25 树冠遮罩通道与长时帧率复核

- 只读UE5.4再次导出`M_FloraTS`调用的内建`FuzzyShading`函数图，报告SHA256前后同为`44841F535C4933A39C11B7637E9A48127551AB4CA0CBC3D48960311343C856B1`；其中心压暗/边缘提亮结构与HE当前材质公式一致。UE公开Python接口仍不开放reroute内部遮罩通道，故项目隔离复制引擎叶材shader，仅对`SM_Tree_4`使用红通道或alpha通道、相同0.333阈值做实际Game截图。UE单树顶部180行遮罩45391像素，HE alpha图36276/IoU0.75775，红图7565/IoU0.16646；红通道整图UE误差59.761 vs alpha20.796 RGB8，明确不采用红通道。正式材质与UE/Fab原件不变；该对照未解决HE采用0.1时的剩余遮挡差异。
- 新GBuffer版本正式SICKA Game 3600帧固定走跑跳回放在i7-11800H/RTX3060 Laptop、DX12/RelWithDebInfo/1280×720、60帧预热后3540统计帧CPU/GPU各97帧>33.333ms，严格稳定30FPS FAIL。GPU慢区间3459–3555帧，最大104.918ms；Kernel-Power 521有效电池数量于同窗口1→0→1，仅为时间相关，不能证明物理或驱动根因。另有CPU独立超时2697帧38.001ms（update26.291、动画10.282、形变9.725、布料求解4.870）和3030帧33.501ms（update16.977、渲染提交16.248），仍需针对峰值优化或复测。正常区间GPU中位9.694ms，未见新GBuffer成为稳定瓶颈。

## 2026-09-25 foliage 烘焙漫反射与现有场景回归

- 当前SICKA树木已导入有效静态光照图、UV1/atlas变换及0.2混合权重，但引擎自定义foliage GBuffer只保存透射载荷，原先仅标准Lit材质读取烘焙漫反射。通用GBuffer新增RGBA16F `StaticDiffuse`，对foliage解码现有UE HQ光照图并独立传递RGB及IBL替代权重，延迟光照保持树叶透射模型；标准Lit旧编码保留。DX12 Game/Editor实际shader构建与运行均PASS。正式0.2权重的180帧截图相对前版几乎不变（整图RGB8 MAE约0.001），隔离把69树权重设1后顶部180行约2212像素差值>4，证明通路生效但正式配置的画质收益极小。
- 前后UE编辑器固定512²机位对照，仅将右侧48树权重设1的项目诊断：前视整图RGB8 MAE14.632→14.506，后视16.066→16.141，后一机位退步，故正式场景与生成器保留0.2。树冠轮廓、逆光和雾仍是主要差距；通用静态GI支持不等于UE画质等效。
- i7-11800H/RTX3060 Laptop、DX12/RelWithDebInfo/1280×720、60帧预热后Game/Editor各840统计帧均0帧超33.333ms；Game CPU中位/P95/max22.053/24.884/33.040ms、GPU9.662/10.344/10.971ms，Editor CPU20.219/23.687/29.444ms、GPU11.098/11.355/11.714ms。临时资源保留量旧470,894,592→新493,012,992字节（+22,118,400）；同机历史采样的中位GPU差异不能独立归因于该通道。默认StarbloomSurvivor项目Game从冷缓存完整构建并90帧启动PASS，未测默认Editor。
- C盘空间从406,802,432字节降至不足构建余量后，按`Tools/Agent/Storage.md`核查进程、路径和`.he-keep`，`hectl clean stale`长时间持续扫描而中断；`hectl clean cache`仅删除无人使用的MechanicGirlLab Editor可重建缓存，工具报告逻辑删除7,170,559,225字节，实测C盘空闲增至7,722,975,232字节。之后默认Game冷构建和诊断捕获占用空间，清理与构建不能混为净释放量；此前被审批拒绝的旧`.retired-*`没有绕过工具删除。

## 2026-09-25 树叶透射载荷与UE遮罩连线复核

- 通用 foliage 光照函数的 `strength` 形参看似未使用，但追踪 GBuffer/forward 调用后确认 `TransmissionStrength` 已预乘进 `CharacterTint`，不是失效参数；一度加入的二次乘法已撤回，避免错误衰减。固定UE视口树冠开/关透射诊断在顶部180行没有可见差异，说明该视角的直接透射贡献很小，不能用增强透射解释当前主要树冠差异。
- 只读 UE5.4 材质图审计确认 `M_FloraTS` 的 `MP_OPACITY_MASK` 经 reroute 接 `Diffuse` 纹理，但公开Python属性不允许读取 reroute 内部的通道索引；原图在0.333阈值下红/alpha覆盖率约10.08%/13.71%。在通道未确认前不改正式树材质。撤回试验后正式 Game/Editor 各90帧构建与运行 PASS；UE/Fab源文件和正式项目场景均未修改。

## 2026-09-25 树冠法线与切线重导入核对

- 扩展外部项目 `C:\WorkSpace\HEngineProjects\MechanicGirlLab\Tools\Validate-SickaTreeImport.py`：按位置/UV0匹配已绘制的LOD0索引角点，比较法线、切线及切线手性，规避导入器合并材质primitive后顶点顺序改变造成的误判。四种树模型、69处树实例的几何/UV0/法线/切线均与UE导出的glTF逐值一致，`python Tools/Validate-SickaTreeImport.py` PASS。原Fab/UE资源未修改。
- 因此当前树冠轮廓和受光差距不能归因于LOD0法线/切线在导入时丢失；后续应集中排查树叶材质alpha覆盖、双面受光、UE的后处理和雾。该检查不证明HE实际GPU采样及着色与UE等效。

## 2026-09-25 方向雾光方向与场景隔离验证

- 沿引擎调用链确认 `RenderDirectionalLight.Direction` 表示光线传播方向；原天空方向雾把它当成指向太阳的方向，符号反了，且不透明物体距离雾完全忽略已有 `FogDirectionalColor/Intensity/Exponent`。引擎着色器统一以`-light.Direction`求太阳方向，让第一层解析雾在天空和不透明表面按同一方向加色；默认强度0保持旧场景画面。此为解析方向散射近似，不等于UE遮挡感知的体积雾或局部灯光散射。
- 项目单独生成前后固定机位的四分之一UE方向雾强度诊断场景。方向修正前对比基线几乎无响应（前/后视RGB8均差约0.00018/0.00011），修正后响应约0.226/0.272，证实通路有效；但相对UE画面整体MAE前视14.672→14.698、后视16.025→16.044，预览无足够视觉收益，正式场景不启用。UE/Fab原资产与项目正式场景生成器不变。
- 通用修复后正式Game180帧截图与修复前MAE约0.000081 RGB8、最大差3；Game/Editor各900帧预热60后的840帧CPU最大29.292/28.620ms、GPU最大10.867/11.605ms，均0帧超33.333ms。此前跨供电状态GPU慢区间仍未解释，树冠轮廓、逆光体积光与GI仍未对齐。

## 2026-09-25 RGB 调色通用能力与 SICKA 双机位校准

- 引擎 `PostProcessingComponent` 新增默认中性的 RGB `ColorGain`、`ColorOffset` 及各自覆盖开关；RenderWorld提取/局部体积混合/多视图仿射重基/后处理着色器、Editor面板及场景作者范围校验均已接入。Gain每通道0..2、Offset每通道-0.25..0.25，运算位置为已曝光线性色彩、对比度之后；与UE同名字段不是逐值等效。旧场景无覆盖时保持(1,1,1)/(0,0,0)。UE/Fab源文件不变。
- 外部MechanicGirlLab项目从 UE 报告的 Gain(1.05,1,1)、Offset(0,.002,.005)做前后固定机位 A/B。原强度前视MAE14.717→14.823退步、后视16.190→16.039；半强度前视14.672、后视16.025，双机位小幅改善且预览无明显新色偏，因此项目生成器采用(1.025,1,1)/(0,.001,.0025)，未改引擎默认值。正式场景重建两次SHA同为`c7f57a42bcef52a6acd01135f17d71e529671796ae818acd5e050c8231466c64`，3568实体、21落叶、2112静态光照绑定不变。树冠轮廓、GI、体积雾等主要视觉差距仍在，此步不能称整体UE等效。
- Game/Editor反射生成、构建与实际启动PASS；正式Game180帧截图已目视，Editor mailbox调色编辑、越界拒绝、保存重开PASS。固定回放Game900帧预热60后840帧CPU中位/P95/max21.819/24.162/31.055ms、GPU最大10.737ms；Editor840帧CPU19.819/23.362/26.727ms、GPU最大11.246ms，均0帧超33.333ms。追加1800帧本轮Game1740统计帧CPU最大29.131ms、GPU最大11.077ms，EditorCPU最大30.369ms、GPU最大11.735ms，各0超时；此前其他轮次曾有供电事件邻近的GPU慢区间和独立CPU峰值，跨状态稳定性仍未证明。

## 2026-09-25 后处理字段与落叶色相复核

- 只读 UE 命令行审计 `MAP_Demo` 的 PostProcessVolume：Bloom 1.0、曝光偏置0.5、色彩对比/增益/偏移、色温7300和Vignette 0.6启用；`MI_PP_Wave`、`MI_PP_Sharpen`权重均为0。引擎新增默认关闭、可反射保存和体积混合的通用 Vignette，Editor 后处理面板补齐已有 Bloom/对比度/饱和度/色温以及 Vignette 控件。独立项目诊断场景的前后机位A/B显示直接使用 Vignette 0.6 一处机位退步，因此正式场景保持关闭；Bloom 0.5 在两机位降低参考图MAE，已写入项目生成器并重建正式场景。UE与HE数值不按同名字段直接等价。
- 本轮再次运行 `Tools/Validate-SickaLeafPalette.py`：21/21发射器均匹配邻近 `MI_TreeBranch2` 暖橄榄叶材，绿叶/暖叶效果包分别记录RGB(56,63,43)/(162,156,58)，源纹理、材质因子与效果配置指纹一致；检查近景实际捕获仍为有色叶形、无白片。本地图没有确认的绿色叶树发射器，不能以此声称已在第二树种上完成运行验收。UE/Fab原资源未改。
- 新后处理字段已通过Game/Editor构建和运行、Editor mailbox修改/范围拒绝/保存/重开验收。Bloom正式版固定回放Game 840统计帧CPU中位22.198ms、GPU中位9.569ms，仅帧104 CPU35.513ms超门槛；Editor 840帧CPU最大29.230ms、0帧超门槛。Game严格稳定30FPS与UE全面视觉等效仍未完成；后续继续定位外部供电相关GPU慢区间及独立CPU峰值。

## 2026-09-25 冷平衡雾色后的长时性能门槛

- 正式场景DX12/RelWithDebInfo/1280×720、60帧预热后Game与Editor各测1800帧。Editor1740帧CPU中位/P95/max20.415/24.217/29.873ms、GPU最大11.721ms，0帧超过33.333ms，本次静态Editor视口达30FPS门槛。Game1740帧CPU中位21.834ms、102帧GPU跨度超过33.333ms，GPU最大102.058ms；慢区间387–488帧与Windows Kernel-Power电池有效数量1→0→1事件相邻，帧槽等待峰值80.698ms。该时间相关性不能证明设备根因，也不能把Game结果算PASS。
- 除该GPU慢区间外，Game还有帧577与1492的CPU超时：577帧`windowMs=10.837`，1492帧`updateMs=17.807`及`renderSubmitPresentMs=17.952`（图录制7.42ms）。下一步针对这些独立CPU峰值与外部供电/驱动状态分别取证；无需为102帧GPU骤降盲目降低场景画质。当前Game严格稳定30FPS仍FAIL。

## 2026-09-25 落叶与邻近树叶颜色回归

- 再次核对 UE 原场景树实例、项目叶材与正式场景：21 个落叶发射器全部邻接 `MI_TreeBranch2` 叶材，树叶原图遮罩中位 RGB(101,115,78)，材质基色系数(1,.702,.233)使最终基色偏黄绿/橄榄，而非原图的纯绿。暖落叶包中位 RGB(162,156,58)保持与树材接近的红绿比例；预留绿叶包 RGB(56,63,43)，当前场景没有对应绿叶树发射器，故未混用。项目新增 `Tools/Validate-SickaLeafPalette.py`，检查21处邻树材质、距离、场景脚本与两个效果包的源纹理/材质/配置指纹，防止改树后沿用过期落叶包。
- 重新运行近景第180帧开/关落叶捕获，渲染差异集中叶片；高差值样本实际像素约 RGB(137,134,72)，仍是橄榄色，不是白片。叶片在宽镜头里很小且蓝色雾背景降低辨识度；受光、雾与UE Niagara粒子行为尚未做到视觉等效，不能把资源色相校验当成最终画质验收。原 UE/Fab 资源未修改。
- 已有前后两个固定UE视口与HE诊断场景的次层雾色对照中，`[.26,.27,.40]` 对比原 `[.30,.28,.34]` 的整图RGB8 MAE由前视15.583→14.986、后视16.857→16.597，右树冠前视21.175→19.159、后视25.170→23.890；左树冠略退步。目视两机位后写入项目场景生成器并重建正式场景（3568实体、21个暖落叶绑定不变），Game捕获与Editor启动均PASS。大面积天空、树冠形状与逆光仍与UE有差距，不把这次微调称为整体画质对齐。

## 2026-09-25 UE 场景重导入当前状态复核

- 当前 MechanicGirlLab 外部项目的 `MAP_Demo_HE.gltf` 用新构建的项目 `HAssetImporter` 执行只读 content `plan`：`changed=false`，added/modified/deleted 均0，115个网格资源、3463个导入实体、约850MB资源引用维持稳定；静态源没有主相机的warning符合由项目场景添加可玩相机的流程。`Build-SickaDynastyScene.py` 再生成3568实体正式场景，SHA256重建前后同为 `d9e7c9b8310a4e44b73ef18cb5176ae5c9a2c9c5f3c9f64f6657841de980e637`，2112个静态光照绑定保持。原UE/Fab资产未改。
- 修复通用 `content_sync` 对缺失导入器原先报 `unexpected_failure` 的生产体验：在读取大源、创建staging前检查指定可执行文件，返回明确`importer_argument`和构建提示；18项内容同步单测PASS，真实CLI缺失导入器0秒返回代码2。`Docs/UEFabAssetIntake.md`补充外部项目导入器构建步骤。C盘构建后仅余约0.99GB，按存储工具核查无保护/无进程的闲置Tests RelWithDebInfo缓存，`hectl clean cache`删除621,074,898逻辑字节，实测C盘可用空间+622,878,720字节；其他大缓存含受保护证据或此前审批拒绝的残留，未直接删除。

## 2026-09-25 父节点查找缓存与 30FPS 长测

- 诊断将每帧 2976 个网格实体的提取拆成候选检查与可见对象构造；慢提取帧的中位耗时分别8.847/1.906ms。场景有791个带父节点的网格，深度大多为2；`Level::GetWorldTransform` 每级使用 `TryGetEntityByUUID`，原Game路径每次线性扫约3568个ID。已在通用 `Level` 增加按UUID的惰性查找缓存，命中时核对句柄/ID，销毁、添加ID及直接改ID后可失效或回退扫描；新增重建/改ID测试用例。引擎中其他父子实体调用也复用该修复，不在SICKA脚本写专用捷径。
- 同一i7-11800H/RTX3060 Laptop、DX12/RelWithDebInfo/1280×720、60帧预热、诊断环境下，改前Game1800帧中1740统计帧CPU中位/P95为25.904/28.539ms、12帧>33.333，改后21.927/24.048ms、1帧>33.333；改后网格提取无帧超过11ms日志阈值。编辑器900帧840统计帧CPU中位/P95/max20.290/24.882/31.755ms、0超时，GPU最大11.598。脚本细分计时显示偶发峰值与托管更新或字段快照相关，本轮900帧Game诊断无CPU超时，仍未证明长期稳定。
- 无额外诊断环境的Game3600帧3540统计帧CPU中位/P95/max21.804/26.063/109.742ms，114帧CPU、99帧GPU>33.333；GPU慢帧为603–701连续区间，与Windows Kernel-Power 521电池有效数量1→0→1同时间窗口，仅是相关性。其余15帧CPU独立超时仍待定位。改后第900帧同镜头捕获与暖雾基线RGB8整图MAE约0.01，仅约1276像素差值超过4，差异集中粒子区域，未发现父子物体错位。严格30FPS整体仍FAIL；UE/HE树冠、体积雾和逆光差异仍待完成。

## 2026-09-25 落叶色相复核与渲染提取缓存

- 当前 SICKA 场景 21 个 Niagara 落叶发射器都按最近树木 `MI_TreeBranch2` 绑定暖色效果；右侧 48 棵高覆盖树与左侧 21 棵树的叶材基色系数相同。树冠原贴图不透明像素中位 RGB8(101,115,78)，材质系数(1,.702,.233)后约为(101,97,37)，是黄绿/橄榄色；此前暖色落叶(162,109,58)明显更橙。项目 `SickaLeafAppearance.json` 把额外绿色缩放 .7→1.0，正式 Effekseer 包重新编译，粒子中位色改为(162,156,58)，保持树冠红绿比例。地面枯叶原贴图中位(132,89,28)，项目材质约(120,79,28)，仍是褐色。UE/Fab 原资源未修改；绿叶效果包继续供真正绿叶树使用。
- 通用 `RenderWorldExtractor` 对静态光照图预热路径做跨帧缓存，路径、混合权重、可见性、实体数量或场景代次改变时失效，异常中断后保持脏标记；新增同实体换路径、禁用/恢复光照图的定向断言。Game900/Editor120帧 DX12 构建和运行PASS；Game840统计帧CPU中位/P95/max25.922/28.484/40.104ms，4帧超过33.333ms，尚未达到严格稳定30FPS。专门的 HEngineTests RelWithDebInfo 初次构建因剩余C盘约2.5GB且需编译千余依赖而主动中止，单测尚未运行。抽取峰值报告工具已通过自身 Python 单测。

## 2026-09-25 布料可动约束预筛选与长时帧率复测

- 通用 `ClothSimulation` 每帧依已绘制的运动半径记录可动粒子与非零质量约束边，XPBD迭代仅遍历这些项，保持原边顺序、固定点、碰撞与重置语义。`AnimationPoseContract` 180帧参考解算最大位置差0；配对微基准本轮参考4201.296ms、当前3754.474ms，不能把跨次差值当作场景收益。SICKA Game 两次900帧及Editor一次900帧均构建/运行PASS，但严格30FPS均未通过：Game第一次840统计帧140帧CPU超33.333ms、93帧GPU超时，11:50:40–46慢帧簇与Kernel-Power 521电池计数变化同期；第二次仅1个CPU超时帧，GPU无超时，布料求解中位2.32ms；Editor有1个CPU超时帧。尚须定位跨阶段CPU短峰并排查外部电源状态，不能宣称长期稳定30FPS。
- C盘空间不足时按 `Tools/Agent/Storage.md` 核实并用 `hectl clean cache` 回收默认Tests Debug缓存6,327,054,148字节、默认Editor Debug缓存396,226,349字节；SICKA Tests Debug缓存只删除1,782,090,615/5,393,869,481字节，Effekseer子目录报WinError145并留下`.retired-*`残留。`hectl clean stale`因每个候选重复全盘inventory而长时间未完成，人工中断后仍有约2.51GiB可用。直接递归删残留被自动审批审查拒绝（仅给出`blocked by policy`），未绕过。AsterCourtyard跨项目Game构建因空间持续下降而中断，运行回归未完成；引擎定向单测与SICKA Game/Editor运行结果不替代该项。

## 2026-09-25 UE Niagara 只读检查沉淀

- 新增通用 `Tools/UE/InspectNiagaraDependencies.py`，用 UE Python 命令行对指定 `/Game` 系统、发射器和材质实例读取资产依赖及材质参数，输出到 UE Content 外的 JSON，不保存或修改 Fab 原资产。SICKA 旋涡复核得到系统30项依赖（含 VortexForce、SpawnRate、SpriteRotationRate）、发射器27项依赖，`MI_MagickVortex` 的 Opacity=0.05 及 `T_FireAtlas_Masked` 依赖；现有六处 HE 旋涡本来已接入，本轮未重复制作。该工具不解析 Niagara 曲线，也不证明 HE 效果与 UE 等效。

## 2026-09-25 SICKA 落叶树色复核与 UE 烘焙光照导出

- 正式场景 21 个落叶发射器全部绑定 `MI_TreeBranch2` 树叶材质。核查原图和材质系数后，项目去掉落叶效果多余的 8% 绿色增益，暖色叶片现为黄褐 RGB(73,70,26)，可编辑 Effekseer 源和包已重编译。近景 Game 捕获可见叶形，无白色面片；绿叶效果留给以后真实绿叶树材。配置和编译逻辑只在外部 `MechanicGirlLab` 项目，通用播放继续复用引擎特效系统。
- 新增只读 `HELightmapBridge` UE5.4 命令插件及工具，从原 UE 场景提取 2320 份 LOD 烘焙光照绑定、165 张 HQ 图集及系数，保留原始 UE 资产。165 张图集含完整 mip 已转 BC7 DDS；项目绑定工具校验 2112 个可见静态网格与 HE 场景实体、UV1 和变换一一对应，排除自绘天空球。引擎现有可序列化的每实例图集字段、RenderWorld 快照、DX12 heap-direct 图集管理、UE HQ 双系数解码及延迟光照混合；项目脚本重建后绑定保持一致。UE太阳/天空与40点光均为Stationary，HE 保留其直接光，图集仅以场景权重替代环境漫反射。0.2权重在街道同镜头使近处地面平均亮度更接近 UE，但整图误差几乎不变，树冠/天空/远处街道仍有明显差距；GI画面目标尚未完成。
- 图集首次进入新区块会增加CPU尾帧；渲染器现按场景代际预热去重后的图集（≤256张，超过时明确记录按需加载）。Game/Editor各900帧短测达到0帧>33.333ms，但Game 3600帧中103帧超时，约97帧GPU大峰与Windows Kernel-Power 521电池计数变化同期，另有独立CPU峰。严格长期30FPS仍未通过。

## 2026-09-25 SICKA 帧峰值定位与静态变换缓存

- 新增环境开关 `HE_RUNTIME_SYSTEM_TIMINGS=1` 的运行时系统、特效/渲染提取及网格提取分项计时；`=2` 还按60帧采样。常规运行不启用。两次900帧诊断把早期偶发CPU峰定位到 `RenderSystem3D`，其中特效更新约0.1ms，网格提取常占约10–11ms。正式场景2976个网格实体中2185个无父级，原来每帧重复构造其局部变换矩阵。
- 引擎 `RenderWorldExtractor` 用与 ECS 遍历顺序对应的连续缓存复用未改变的局部矩阵；实体身份或 TRS 变化即重算，父级实体仍按实时层级计算，场景代际或 Reset 清空。哈希表版本反而略慢，已替换。`RenderWorldSubmission` 单测覆盖静止复用、ECS编辑后的当前/前帧矩阵以及父级切换。
- i7-11800H / RTX3060 Laptop / DX12 / RelWithDebInfo / 1280×720，Game相同900帧输入回放两次各840统计帧：CPU wall中位25.919/25.855ms、最大32.695/32.537ms、各0帧超过33.333；旧正式版本中位26.459ms、最大39.002ms、1帧超时。Game 180帧可玩截图已目视检查。Editor同场景900帧两次CPU wall最大37.208/28.330ms、分别1/0帧超时；Editor严格稳定30FPS仍未通过。此数据只覆盖指定回放/空闲Editor视口，不代表全场景、快转镜头或UE画质对齐。

## 2026-09-24 枯叶贴花与树木叶色

- UE 只读导出证实 MAP_Demo 有 298 个 `MI_Decal_DeadLeaves` 贴花，其中 25 个倾斜约 30°；原材质为半透明 Decal，带 2048² RGBA 枯叶底色及法线图。新增引擎通用 `Tools/UE/ExportLevelDecals.py`、`GltfGroundSampler.py` 与 `BuildPlanarDecalGltf.py`，项目把静态、朝地面的贴花烘为 73 个可剔除 glTF 网格组（298处、2682顶点），稳定 ID 导入，并把原法线图及正确切线接入材质。最终版本按导出的静态接收网格采样高度：298/298中心命中，1处靠接缝周边取样；35个无接收面的顶点导致52个小三角形跳过。项目色调与1cm离面距在 `Tools/SickaGroundLeaves.json` / `Build-SickaGroundLeaves.py`；原 UE 素材和导入材质不改。正式可玩场景3495→3568实体，Editor/Game均可加载。它仍是离线网格近似，墙面和复杂台阶投影未等效；UE编辑器射线探测12/12未命中，未用其冒充接收面证据。
- 树叶材质事实复核：`MI_TreeBranch2` 使用偏绿原图，但漫反射系数 (1,0.702458,0.233331) 将其变为暖褐叶色，次表面色另带深红；`MI_TreeBranch` 是较中性的叶色。21 个落叶发射器逐一关联最近树木的主要叶材，结果 21/21 均为 `MI_TreeBranch2`，记录在项目 `Tools/SickaLeafEmitterPalette.generated.json`。因此当前关卡采用暖褐落叶而非随机红绿混合；项目保留另一单节点效果供以后真正的中性/绿叶树使用。近景 1280×720 截图可见褐色叶形，白色面片未再出现。
- 引擎通用布料碰撞预筛选按顶点最大可动半径剔除不可能相交的胶囊。`AnimationPoseContract` 与参考解算最大位置差0，180帧微基准参考4026.308ms、当前3681.971ms；AsterCourtyard Game 90帧跨项目回归PASS。最终含法线图场景 i7-11800H / RTX3060 Laptop / DX12 / RelWithDebInfo / 1280×720，Game固定走跑跳回放900帧（60预热、840统计）CPU wall中位/P95/max26.459/28.772/39.002ms、1帧超33.333，GPU最大10.981ms；Editor同场景900帧19.966/23.960/27.983ms、0超时，实际3D视口约932×329。Editor本次通过，Game严格稳定30FPS仍FAIL；近景落叶镜头也有超时。GI、贴花投射、UE树冠与总画质对齐仍未完成。

## 2026-09-23 当前检查点

- 引擎自定义材质的 UV1 法线已按像素位置/UV1 导数构建切线基底；UV 退化时回退原顶点切线。`HairCards` 还按其颜色纹理的 UV set 构建发丝流向，皮肤/眼/角膜/衣料/ShowcasePBR 共享法线修复。Aster Game 120 帧 `check`、HShaderCompiler 构建/自测及 Selunae 六阶段交互回放均 PASS。当前 Selunae 发片实际材质纹理路径已指向 4K atlas；近景发碎不能简单归因为误用 1K 图，仍须改善发片布局/覆盖和动态画面质量。
- Selunae 本地角色已接入 139 骨、最多 13 个有效权重、发片/皮肤/眼球/角膜/衣料材质、面部形变、44 个发骨和 2662 个裙摆粒子。原始 `mouthPucker` 形变严重损坏，已排除；近景视觉仍有发片颗粒边缘、眼口与服饰质量问题，不能认定 AAA 美术达标。
- 引擎足锁在运动停止且足部动画稳定后会渐进消除残留世界锁点。Selunae 同一回放中，原对话阶段双足锁点误差约 0.148/0.187m，修复后约 0.012/0.011m；初切近景时角色仍受约 0.4m 惯性位移，锁点暂留，需继续处理交互停步体验。`AnimationPoseContract` PASS。
- 自定义材质此前保存纹理 UV0/UV1 选择却总采样 UV0；现在纹理选择进入材质实例打包数据，角色及 ShowcasePBR 着色器按纹理选择 UV，UV1 法线切线一致性也已补齐。`MaterialParameterPacking`、`MaterialSerialization`、编译器自检与 Aster Game 构建均 PASS，实际 Selunae 操作回放 PASS。自定义材质的完整纹理变换/过滤尚未实现，不将 UV 选择修复描述为完整采样系统。
- Selunae 双演员正式 11700 测量帧有 93 帧超过 33.33ms，严格 FAIL；单主角旧版本 11700 帧 0 超预算、最大 32.276ms PASS。足锁与材质更新后的单主角 11700 帧在 `20260923-035351` 有 1 帧 34.4164ms，严格 FAIL（平均65.1503FPS）。Editor 构建 PASS；布料短路优化已写入引擎，数值与新全窗性能复验中。正式 Rain/Carla 与 package6 证据仍仅适用于各自冻结内容。
- 同条件第二次 `20260923-040713` 2 帧超预算，FAIL；增加通用布料有效骨权重槽位缓存后第三次 `20260923-041839` 0 超预算、最大33.3064ms，PASS，但距离门槛只有0.027ms。两次失败不能因一次通过被抹去，正在增加不降低画质的约束求解优化和复验。
- 同条件后续 `042946` 8迭代2帧超、`043748` 隔离4迭代3帧超，均FAIL，4迭代未采用。增加布料求解分项计时后，`044611` 原8迭代0超/max32.8736ms PASS，定位 XPBD 求解均值3.5878ms、蒙皮0.1636ms；保守胶囊AABB剔除的参考轨迹差0，`045520` 原8迭代0超/max25.469ms PASS、求解均值3.4113ms。需同二进制重复全窗确认余量；Editor/默认游戏回归仍待完成。
- 同二进制/场景复验 `050713` 11700帧0超/max33.0347ms PASS，两个最新完整窗口均PASS、595声明输入稳定；第二次仅0.299ms余量，历史失败保留。Aster Editor构建+30帧有界运行、默认Starbloom独立构建+120帧有界运行均PASS。Selunae近景发片边缘和人物美术仍未达到AAA，整体目标未完成。

2026-09-22：保留上一轮未提交工作；默认项目仍为 Project。

当前状态以 [验收记录的当前索引](engine-upgrade-validation.md#当前结论索引) 和 [实施清单](engine-upgrade-plan.md) 为准；下方按时间保留开发过程。特别是早期发片数量、Carla 缺少动画/Smile、旧发布包待更新等描述，不能覆盖后面的修复与最新证据。最终人物美术仍未达到 AAA，整体未完成。

最新检查点（06:57后）：Rain064927/Carla065318完整11700帧均严格PASS，平均81.18848/86.36475FPS，最大24.975/25.8948ms，各0超预算；575/597声明输入无变更，process-only内存监控已实际运行，GPU内存取DXGI。正式头皮canonical修复和06451832帧有限PASS；underlay/DentalV1/V2视觉FAIL未采用，Carla仍原Combined。package6最终Rain已recook且WARP/硬件隔离启动PASS，整体AAA艺术未完成。

当前补充：package6最终exit0，2107文件/572126640字节、hash6a384c8e42cb35b9，WARP与硬件DX12新目录2帧启动PASS；不代表Shipping完整玩法/FPS。Selunae候选仅CPU审计/转换中，整体AAA艺术仍未完成。

## 初期集成（历史记录）

### 07:25后：复杂角色实际接入暴露的通用缺陷

Selunae合法本地候选已下载并经过禁用脚本的CPU审计；原始`mouthPucker`有7067m异常位移，明确排除且源文件未改。V2已有真实分层发片、隔离UV烘焙的虹膜/巩膜和部分服装材质，Importer已运行成功，但尚未通过引擎视觉验收，不能称为AAA角色完成。

实际139个蒙皮骨超出旧SceneRenderer固定100行上传，已改为按实际palette长度追加当前/前帧数据，保持25600矩阵总预算、显式容量失败；同时修复无动画原生网格只使用100个identity的问题，按hskel层级计算并缓存真实绑定姿态。`hectl test RenderWorldSubmission`与`hectl test MeshCpuDecode`PASS；首个误用函数名`RenderWorldSubmissionContract`的调用只构建成功、测试名未知失败，日志保留。Game正在重建并将验收真实139骨角色。此前8E6E性能及package6仍属于明确冻结的上一版本，不覆盖这些新增代码。

角色最多16个原始影响；现4槽导出在重定向采样中误差过大，正在核对8/12/16槽位置误差及完整导入/解码/渲染链，未以静默权重丢弃宣称可玩角色完成。

- 基线 Game 构建与120帧运行通过。
- 三个并行边界：morph资源/CPU形变/历史；人物材质像素着色；Sequence资源/播放器/编辑工具。
- 主任务：布料、接触、美术资源、代表场景与最终集成。
- 已实现并接入：命名 morph 导入/实例形变/前帧顶点历史，XPBD 衣物与身体碰撞、接缝焊接，脚锁/骨盆/双骨 IK，表情曲线与自动眨眼，Sequence 镜头/动画/表情/音频/交互与控制权恢复，编辑器序列面板。
- 新人物使用 Blender Studio Rain（CC BY 4.0），79 骨、7 段重定向动画、6 个面部形变、220 张原创发片与可模拟围巾；正在实机场景集成。美术仍待最终验收，不能声称已达到 AAA。
- 环境增加带碰撞的台阶与坡道、室内外局部漫反射/反射探针、植物与材质细节。
- 验证通过：Game 与 Editor 构建；AnimationPoseContract（含布料接缝/边界）、MeshCpuDecode、SequencePlayback；三次真实导入/重导入 fixture，最新覆盖静态节点变换烘焙。
- 首次升级场景运行发现人物 shader 缺正式编译清单，已修复并重新运行通过。首批截图仍存在旧美术和布料顶点遮罩问题，未计为视觉通过；遮罩已修复，新人物替换中。
- C 盘闲置 scripting-matrix 缓存按 hectl 显式清理，删除 15,013,078,722 字节；因一处目录非空返回部分失败，保留 retired 残余，没有绕过保护强删。当前剩余约 19 GiB。
- 以上为集成初期记录；后续状态见下节，旧“下一步”不代表当前进度。

## 01:45 检查点

- Rain 正向/绑定/材质已经统一，当前默认 UpgradeCourtyard 有199实体、两演员、室内外探针、7点台阶坡道回放和两段表演。
- 最新演出、接触、形变压力、真实Editor六项保存重开及默认Starbloom运行回归均通过；精确命令与目录见 `engine-upgrade-validation.md`。
- 皮肤斑块定位为辐照度积分方向采样噪声，已修复并做同姿态实机对照；发片环形高光能量与双面切线也已修复，单帧证据不当作全面运动视觉验收。
- Shipping正在修复最后已知的Collision资源类型问题。此前跨配置Jolt ABI错误通过源三角形v2容器/发布时目标重烘焙解决，运行时严格ABI校验保留。
- 首轮完整性能平均65.39FPS，但14帧超过33.33ms、最大510.59ms，严格稳定30FPS失败。正对状态日志计时并优化不变morph重复工作及cloth临时缓冲；候选优化未完成单测前不算交付。
- 当前Rain仍为风格化角色，尚未达到用户最终美术目标。Renderpeople写实候选已下载并检查，本机试验与源资产再分发需要区分；候选尚未替换正式样例。
- 当前责任边界：character_deformation优化/测试；performance_tools日志计时/基准；character_shading视觉证据审计/写实候选方案；主线统一构建、发布、文档及后续视觉集成。性能测量期间不并行跑构建或GPU任务。

## 02:14 检查点

- Collision资源类型单测、纯morph缓存/历史/克隆回归、cloth常量预计算与原solver逐帧数值对照、异步日志背压/顺序/排空测试均通过。后续必须用完整新Game重测，当前旧性能FAIL仍未解除。
- `hectl package -Project Examples/AsterCourtyard`第三次完整通过并发布，含WARP隔离Unicode新目录启动2帧、PE依赖闭包、Dev隔离、严格碰撞ABI及目标重烘焙。包早于镜头/衣发资产新修正，最终需再cook。
- 连续帧工具新增 `Tools/Capture-UpgradeMotion.ps1`；捕获orbit/contact/cut窗口成功，首轮误用capture.status失败已修为session.status.capture。
- 连续帧发现真实视觉FAIL：portrait→orbit硬跳和fill延迟骤降（脚本已修，待实机重验）；围巾悬起/穿插、马尾水平僵硬（正在正规资产层修复）。禁用cloth的诊断副本恢复正常颈部围巾，禁用secondary仍有水平马尾，分别指向cloth包络/碰撞配置和发束rest作者形态；这些诊断不是最终关闭功能。
- Carla真实写实肖像已本地导入和上光PBR/Skin对照，纠正初次灯位反向；脸部更写实，但固体低模头发、眼层次和未制作表演仍不足以替换可玩Rain。所有本地受限数据在ignored LocalStudies，未进入包根。
- Editor材质作者通道与灯光验证扩展已实现，正在主线 `upgrade-editor-author-build.log` 构建；需运行新版 `validate_upgrade_editor.py` 证明真实MaterialPanel保存/撤销/缓存/重开，不用改JSON冒充Editor保存。
- 当前任务：deformation负责Rain围巾与ponytail正规准备/重导入/实机检查（独占GPU），主线Editor构建与集成；performance_tools已完成镜头脚本/Editor接口等待验收。下一次性能必须等所有构建/GPU工作停止。

## 02:25 检查点

- 围巾进一步定位：GLB 的 `COLOR_0` 全白，Blender 中作者绘制的 ClothMotion 没进入运行流。修复正规导出为唯一活动色层，并新增导出后流验证；新 GLB 有 801/1360 固定顶点。等待 canonical 重导入及 native/视觉复验，不能沿用旧 fixture 的固定点结论。
- Editor 新材质验收第一次真实失败（`upgrade-editor-30fd6cb7a1804fb78abce8257bb6384f/report.json`）：Hair cutoff .43 被权威顶层 render state 的 .4 覆盖。修复 MaterialPanel 生产 UI 和自动化共享提交函数，将该编辑同步到顶层值，保持 Runtime 契约。
- `hectl editor build -Project Examples/AsterCourtyard` 增量 PASS（5.7s，`upgrade-editor-author-rebuild.log`）；一次重叠会话引起的租约拒绝已通过停止自有会话解决，无终止用户进程。
- GPU 独占权交给 deformation 完成资产与视觉；之后 Editor 复验，再构建最终 Game、重测性能与最终 cook。当前 Game 的旧 SHA 仅用于固定渲染条件的画面比较，不证明最新 Runtime 源码。

### 2026-09-22 02:30 character correction checkpoint

Closed the observed scarf/ponytail visual failures through source preparation and stable reimport, retaining cloth/secondary simulation. Actual native scarf pins changed from0/1360 to801/1360; pony rest/card sections now curve down. Source/export regression validator added. Original pinned-binary48-frame before/after review supports the correction; latest Game build plus full contact and two-delta deformation stress PASS (evidence and SHA in validation record). Earlier numerical PASS was insufficient to establish old visual quality. GPU sessions are stopped and released to Editor validation. No further hair changes pending the independent cap appearance review; eyes are already configured but close-up gaze aesthetic evidence is still required.

### 2026-09-22 Editor persistence completed; benchmark evidence contract

Latest bounded Editor retry PASS: `upgrade-editor-ea4ecacd2e734ea8adb9207e99bee6c2/report.json`. Skin/Hair material parameters (including previously failing cutoff), live cache values/revisions, revert/reopen, directional/point lights, Sequence/Animator/cloth fields and scene override serialization all verified through real authoring paths. Source assets unchanged; owned session stopped and GPU released. This closes the observed cutoff persistence failure; visual quality remains independently evaluated.

Benchmark defaults to RenderStats; disabling it is rejected before launch because wall-only samples cannot verify internal resolution. Stage workload/sequence completion and native1080p checks are now required alongside unchanged full-frame performance criteria. PS syntax/Python compilation/diff checks passed; final latest-script measurement remains for the main integration run. No new runtime build or GPU run was started after Editor release.

02:39 follow-up: closed cyan-teeth regression introduced by cloth-mask export on non-cloth meshes. Corrected source/export isolation, added non-cloth cyan guard, stable reimport and same-pose runtime close-up PASS; scarf801pins and cap assets preserved. See validation evidence. GPU released.

02:52 follow-up: fixed independently confirmed front scarf penetration using source mesh clearance fitting and fitted chest capsule, not simulation disable. Fit is idempotent, native min free-tail front clearance18.55mm,801pins retained. Both Rain instances now share current physical configuration after patches.32front frames +2x16rapid-turn frames reviewed, updated two-delta stress PASS; GPU released. First side-view PASS was insufficient to cover this front-view defect; see validation record for limits and exact evidence.

03:09 checkpoint: final frozen-asset Sequence and 64-frame camera windows PASS (limits in validation). Latest full 12000-frame native1080p benchmark coverage/resolution PASS, strict30FPS FAIL: avg81.0192FPS, P99 15.7596ms, max36.6934ms, 1/11700 measured over budget. Isolated spike is CPU Graph.Execute record, not Present or aligned GPU work; adding scoped CPU graph/pass instrumentation before same-workload rerun. Hair art remains below AAA; final recook pending. All benchmark/validation owned processes stopped.

### 追加关口：暂停生命周期与CPU图诊断

- 只读调用链审查发现：Sequence.Paused会置Animator.bPaused；旧active条件将paused当作停用，清SecondaryMotion/CharacterPose与cloth solver。之前通过的runtime.pause停止整个Level，未覆盖演出独立暂停。现修复此真实差异，新增非rest状态暂停/恢复与单步回归；未验证前不标完成。
- CPU图观测采用独立可选observer和CPU CSV，不能混入延迟GPU passes.csv。必须记录真实frame/world/submission，区分Compile/Realize/Validate/Setup、每pass Barriers/Record、Trailing；正常未启用采集时无额外时钟/分配。当前实现中，待构建、逻辑测试、实际CSV身份完整性和原工作负载实测。
- 新Benchmark工具在有界进程结束后写identity.json（exe/scene SHA、硬件、构建/帧数），此记录不进入测量区间。PS解析PASS，实际输出待后续基准验证。

### 暂停修复验收与长帧继续定位

- AnimationPoseContract、RenderGraph 单测 PASS；`sequence-pause/20260922-032521/result.json` 实机 PASS。独立 Sequence 暂停期间 Level 继续步进，非静止布料位置/速度全状态 hash 与最终骨骼 palette 保持，恢复、暂停 seek、停止恢复输入均已验证。
- CPU RenderGraph 观测已接入真实提交身份；11700 个测量帧的 phase/pass 配对与 submission/world 关联检查 PASS。CPU pass CSV 的 POD 缓冲在退出时写盘，保留完整诊断，不逐帧写这一新增文件。
- `aster-benchmark/20260922-032849` 平均78.0403FPS，69帧超预算，连续GPU慢区间；`033514` 平均79.9078FPS，11帧超预算，最大651.592ms，但该帧GPU graph与已有CPU更新/渲染分段均不足以解释。两次严格30FPS均 FAIL，不以平均帧率替代稳定目标。
- 新增7项互不重叠 App 分段以覆盖统计写盘、GPU timing begin、资源泵、音频输入、capture轮询、提交收集和内存采样。`hectl game build -Project Examples/AsterCourtyard` build-only PASS 34.1s（`upgrade-app-phase-build.log`）；SHA `536EE7B463B5A0401E0D7F361B156578847198D099C6E24F673A4E8C803C2AC7`。实测待下一次独占GPU基准，不提前推断驱动或写盘原因。
- 写实 Carla 只在 ignored LocalStudies 研究；已导入独立脸骨动画，实际前/侧连续帧验收进行中。未替换正式 Rain，未进入 cook 根；功能可行性和最终美术分开判断。

### Carla local locomotion checkpoint

Seven Rain animations are physically retargeted to the 71-bone Carla palette in an independent local study; CPU finite/limb-length/pose reconstruction and bounded idle/walk/run/turn/jump runtime checks passed. Existing 4K materials are referenced. Attack/AttackHeavy and continuous foot contact remain to be verified: the initial action capture occurred during Land and is not accepted as action evidence. Facial/eye refinements must be merged through a single authoring/export pass because the two existing studies have different root bind scale. See the Carla section in engine-upgrade-validation.md; no formal Rain replacement or AAA-quality declaration.

## Current performance acceptance: 041620 v2 PASS

Buffered statistics capture passed the full12000-frame run, preserving exact measured301–12000 after300warmup:79.87493FPS average,P9916.42019ms,max32.4977ms,zero frames over33.333. Reanalysis now strictly checks identity range/order/completeness in addition to original frame-time/workload/resolution and CPU submission checks. Three performances completed without interruption. Report `out/agent/aster-benchmark/20260922-041620/final-performance-report.md/json` contains actual CPU/GPU/memory/buffer results.

Current plan/validation performance rows now point to this complete v2 evidence. Historical prologue/Graph/GPU-wait failures remain; this single configuration/window does not establish all-hardware or long-soak stability. Final content recook and remaining art limits are separate delivery gates.

### Carla Combined local study

One shared bind/export now combines seven real locomotion/actions, reviewed blink/jaw morphs and a finite8mm jacket panel with native fixed points/collision. Final bounded runtime lifecycle and48 consecutive Run/Blink/portrait frames passed the stated checks (`carla-combined/20260922-044545`); formal Rain and its release roots are unchanged. Local benchmark generation preserves the workload and explicitly reports missing Smile; its left-hand button reach also requires actor-specific alignment. No AAA art/performance declaration and no local study benchmark measurement yet.

### Carla final contact/gait follow-up
- Full-cycle numerical replay exposed a real swing-phase foot IK defect; main agent owns the focused runtime fix and regression tests. Original PlantHeight remains unchanged pending root-cause verification.
- Added bounded gait capture/analysis tools and normalized fingertip author data. Local sequence preserves track/event/audio timing while adding two medium-shot keys that can show actual hand/button contact.
- Dev animation status preserves bone-origin diagnostics and adds true local-offset effector world position/distance; source frozen for main-agent build. GPU session stopped; final new-binary gait and contact acceptance pending.

### 2026-09-22 05:45 contact integration checkpoint

- AnimationPoseContract boundary regression PASS (86.7 s): locked MaxCorrection and stateful last-foot pelvis release (`upgrade-contact-boundary-unit.log`).
- 83F6 runtime retained two failures: complete Run right-foot 456.8 mm false reanchor and wrist/hand below button despite accurate fingertip. CPU source animation confirms a real stance phase and reachable old anchor; this was the arbitrary animated-foot-distance release, not a missing swing.
- Runtime now uses physical two-link reach for lock release, continuous horizontal overreach fallback, floor at final target, bounded vertical correction and persistent suppression until a new swing. Added reachable long-displacement, release continuity, suppression and rearming tests.
- Added optional world end orientation (`WorldRotationDegrees`, `RotationWeight`) with actual local surface contact point. Sequence stages original orientation/world targets without repeated-Seek accumulation and restores on Stop. Carla local authoring uses a verified skin support point; actual new visual acceptance pending.
- Unified unit tests + Aster Game/Editor builds are running (`upgrade-oriented-contact-*.log`). No GPU session active. Next: complete gait and visible hand-contact replay, Editor persistence including new fields, affected Rain/default regressions, frozen formal performance, final Release recook. Earlier performance/package results do not cover these latest Runtime changes.

### Final bounded Carla asset freeze
- Canonical local character now has seven retargeted body clips, five independent face targets, a finite jacket panel, visible authored button interaction with real skin effector and explicit hand orientation, and local-only reusable scenes. No unsupported hairline candidate merged.
- Actual shoe-surface diagnostics verified the targeted foot-IK penetration fix; actual CPU lock-state telemetry distinguishes anchors from rendered foot rolling and transition movement. Left-foot strict interior-window coverage remains limited, documented rather than represented as zero slip.
- Button approach/press/withdraw and reduced local button light received final keyframe visual review on Game 8E6E, last owned PID 98000 stopped. Local assets frozen; main agent owns final Editor/performance/package conclusions. No AAA art-quality or continuous-contact guarantee inferred from these bounded checks.

### 2026-09-22 06:25 integration checkpoint
- Current Game 8E6E / Editor 01c2 builds, AnimationPoseContract/SequencePlayback, both character Editor authoring roundtrips, Carla actual canonical reimport and default120-frame regression PASS within documented scope.
- Rain contact061530 and sequence061646 bounded runtime PASS, owned processes stopped. Seven contact waypoints and camera/input restoration verified. Sequential visual review still finds near-button palm overexposure and deficient crown/fringe hair.
- Package5 currently running against pre-light-fix Rain content. Generator/strict asset patch being prepared; after content change recook required. Two12000-frame frozen benchmarks pending; old041620 does not cover current binary.
- Last 05:45 intermediate physical-reach fallback was rejected by runtime (558mm jump). Current solver keeps actual anchor through stance, clamps two-link reach, releases on true swing and preserves animated flat-floor foot orientation. Final targeted gait060349 evidence supersedes that intermediate implementation, without erasing its failure.
- No overall completion declaration: hair/face/garment and final art remain unfinished. These are authoring tasks, not assumed external blockers.

### 2026-09-24 MechanicGirl 本地资源接入

- 独立项目 `C:/WorkSpace/HEngineProjects/MechanicGirlLab` 已实际导入 18 个网格段、115 根骨骼及面部 morph；原始 E 盘资产未修改。Skin/HairCards/Cloth/Eye 材质、可控父实体、全身与近景场景均已运行，Game/Editor 检查通过；细节见项目 `VALIDATION.md`。
- 六个整角 FBX 的真实导入均为 0 动画片段；虽有 AnimationStack/Layer 容器，却无 AnimationCurve/KeyTime。当前移动仍是绑定姿态滑动，衣摆/头发动态和发片近景稳定性也未达到最终验收。
- 通用 `HAssetImporter` 模型模式失败后现回滚本次新建的普通文件；缺贴图真实失败和正向导入均通过验证。

### 2026-09-24 UE Fab MechanicGirl 导入与场景检查点

- 引擎层修复 FBX 默认形变权重（Assimp 单 shape 错写成 1.0，原测试角色 93 个默认目标激活），动画-only FBX 文件名包含源资源名，并在 HairCards 加入独立线性 opacity 贴图。UE 导出的 5 个整角变体、13 个 AnimationSequence 均已进入外部 `C:/WorkSpace/HEngineProjects/MechanicGirlLab`，原始 UE/E 资源未改。
- UE 主角的 9 段材质、115 骨、52 个表情名称与 13 个 89 轨动画可在 HE 读入；已完成根绑定旋转和代码驱动位移的独立重定向副本。`UEFabDemo.he` 使用正式 content sync 发布的 UE 静态舞台、控制角色、走跑控制器和跟随镜头；Game/Editor 均可打开运行。
- 视觉检查仍 FAIL：UE MatID 分区材质未重建，腰带配色不对；glTF 命令行世界导出不含材质烘焙贴图，当前舞台只是 5 个静态结构，环境照明和 UE 演示关卡不一致。衣发二级动态和完整面部表演尚未接入本角色。不得将这个子集的性能 PASS 写成最终 AAA 性能验收。
- 下一步：复现 UE 关卡的角色变体和摆位，补 MatID 服装色层、衣发动态配置、面部曲线与镜头演出；再做 UE/HE 相同视角对照和完整负载基准。项目证据在 `C:/WorkSpace/HEngineProjects/MechanicGirlLab/VALIDATION.md`。
- 已用引擎现有二级骨与布料求解器对 UE 头发 17 根骨链和裤脚 1456 个顶点做项目内可重生成配置，包含头/腿/骨盆碰撞及距离降级。静止120帧运行、有限负载帧时间通过；急转、低帧率、暂停恢复、传送和美术品质尚未逐项验收，不能称为衣发最终完成。
- UE 52 个面部形变中已用笑、眉、下颌、口型和眼球目标制作可编辑 `.hface` 循环近景预览，60/180 帧两相位实际渲染有可见口部/眉眼差异。无对白/音频同步，表演美术未验收。
- `MechanicGirlLab/Assets/Scenes/UEFabShowcase.he` 已按 UE 展台次序将 14 名配角与中央主角横向摆放，舞台随之旋转，30 m 固定镜头能容纳整排。引擎动画系统对动画-only `.hanim` 使用目标网格骨架映射，15 个实例摆脱此前 T-pose；Game capture、300 帧检查及 Editor check 通过。项目脚本可选择固定展台镜头或跟随镜头，横向场景固定输入 W 60 帧使 Player 向前约 3.28 m。横向展台是项目内容，动画映射修复在引擎；细节证据在外部项目 `VALIDATION.md`。
- 测试缓存存在 `CMakeCache.txt` 却缺 `build.ninja` 时，`Tools/Agent/Build.ps1` 现触发重新配置。仓库默认 headless 测试的重新配置仍因缺少 `OpenImageDenoise 2.5.1` 受阻；这不计作 unit-test PASS。最终 AAA 美术、完整场景和低帧率动态质量仍未通过。
- UE 展台 22 个实际角色材质实例现有项目内对应 `.hmat`，按每个 glTF 角色九个槽应用发色、衣服、鞋、眼睛和皮肤变体；新增五套换装近景 `UEFabWardrobe.he`。布料接入揭示通用格式缺陷：Fab `COLOR_0.r` 不是空权重，误覆盖导致整套衣服青绿色偏色。引擎网格解码/校验与布料求解现支持独立 `CLOTH_WEIGHT`，旧资源保留 `COLOR_0.r` 回退；01–04 作者化网格追加该流、源二进制前段不变，近景颜色恢复。14 名配角配置同骨架发束动态，主角和右侧两名示范完整布料，其余降级；错开眨眼后近景 240 帧 CPU p95 21.985 ms/max 26.743 ms、GPU p95 7.530 ms/max 8.505 ms，0 帧 >33.333 ms。UE MatID、脏污层和最终演出质量仍缺，详见外部项目 `VALIDATION.md`。
- 通用角色下蹲现由 `CharacterMovementComponent` 的高度/速度参数和 `CharacterMovementSystem` 的 Jolt 短胶囊体切换实现；起身需要净空，蹲下不能跳跃，缺少 Crouch action 的旧项目不触发。MechanicGirlLab 配 `Left Control` 并以低顶诊断场景实际验证站立阻挡、蹲行穿过、顶下无法起跳、离开恢复跳跃。下蹲专用动画资源不存在，视觉姿态尚未完成，不能把碰撞功能当作全套可玩动作交付。
- 引擎脚本 `Physics.CharacterState` 现返回物理控制器的落地、蹲姿和速度状态，项目动画不再靠按钮时长猜空中阶段。MechanicGirl 的走跑控制器接入 UE 原 Jump/Fall 片段并在 `UEFabDemo.he` 实机固定输入第10/25帧截图验证空中骨骼姿态。仍缺下蹲动画、落地段细调及整套动作循环的视觉验收。
- 中心可控主角原先仍用初始网格材质；现从 UE 展台 skin14 的真实九槽映射绑定到项目七个主角场景，近景 Game capture 已检查。发色/面色可见，但 UE 自定义 MatID、污渍、肤质和灯光没有因此变成等效。
- UE `M_CLOTHES` 原图的自定义节点经只读命令行取证，明确 MatID 八种 RGB 二值颜色与 `Color 01..08` 的映射。引擎通用 Cloth 着色器增设可选线性 MatID 调色板，项目 22 个角色材质改为真正的 `Type=Custom` 并生成颜色参数；之前把 `ShaderPath` 写进 `Type=Default` 资源只走固定 PBR，导致头发/皮肤/衣服专用着色器根本未执行。引擎材质加载器现拒绝 V2 此类矛盾配置。五套换装新截图呈橙、红、米白，不再出现原先明显的绿色腰带；肤质/脏污/鞋 MatID 及 UE 灯光仍需制作。近景 240 帧 CPU p95 24.293ms/max28.100ms，GPU p95 7.782ms/max8.676ms，0 帧 >33.333ms；默认展台 CPU p95 8.431ms/max10.363ms，GPU p95 6.896ms/max7.187ms。硬件/画质/分辨率详见项目 `VALIDATION.md`。
- 衣物/鞋的 MatID 均已从 UE 实例参数生成；引擎 Cloth 着色器新增可选污渍和血渍 RGB 遮罩、各通道颜色/强度，项目脏污款按 UE 静态开关启用，普通款关闭。UE 图中 RGB 通道连接已由只读命令行核实；混合公式是简化近似，尚无同镜头 UE/HE 像素对照。Game 默认横向展台与五套近景换装截图已人工检查，Editor 同场景构建运行 PASS。着色器变更后近景 240 帧 CPU p95/max 23.655/26.488ms、GPU p95/max 7.450/8.400ms，0 帧 >33.333ms；目前画面仍是简易展台，远未达 AAA 场景/人物质量。
- UE `M_HAIR` 图中的 `Wind map` R 通道负责根梢位置，五个发型实例均启用 Albedo，并携带根/中/梢三色。引擎通用 HairCards 着色器现提供可选线性根梢贴图和三色过渡，项目生成器为五个变体填入 UE 参数；近景 `UEHero-HairGradient-preview.jpg` 已视觉检查发梢过渡，默认横向展台再次运行通过。该三段插值近似 UE 材质函数，未完成逐像素对照及发片闪烁/光泽专项验收。最终着色器下近景 240 帧 CPU p95/max 23.864/30.386ms、GPU p95/max 7.764/8.484ms；默认横向展台 CPU p95/max 8.843/11.098ms、GPU p95/max 6.897/7.088ms，两个测试均 0 帧超过 33.333ms。
- UE `M_EYES` 原图证实虹膜/巩膜色由 `T_EYES_DepthMask` R 通道混合，发光贴图乘 `EmissiveTint`；引擎通用 Eye 材质现可选该遮罩、两种颜色、发光和眼部法线，项目两款虹膜实例按 UE 参数生成。通用 Skin 材质新增可选污渍/血渍三通道遮罩和颜色/强度；项目皮肤实例的法线、粗糙度、散射图现从 UE 实例逐项绑定，脏污头/身体使用实际 mask。独立 `Diagnostics/UEDirtyCloseup.he` 近景已看到面部血渍和身体污渍，Game capture 与 Editor check PASS。该图层仍是近似实现，UE 眼部 Specular/Gloss、伤疤图集以及完整皮肤材质图尚未还原。最新五套换装 240 帧 CPU p95/max 23.256/27.096ms、GPU p95/max 7.443/8.501ms，0 帧 >33.333ms；详见项目验收记录。
- UE `M_SKIN` 的伤疤图集 Custom 节点含完整格子、Tile、缩放、偏移和旋转 UV 代码；引擎 Skin 着色器现以可选参数实现同一 UV 变换、按图集 Alpha 混合颜色和法线。项目 Scar 实例从 UE 参数填入 Tile 13、4×4 图集和两张 4K 贴图，`Diagnostics/UEScarCloseup.he` Game capture、Editor check PASS。相同近景关闭图集的诊断对照仅在脸部窄区域出现像素差，证实路径执行，但肉眼效果仍很轻；不能据此声称 UE 伤疤画质等效。最新五套换装 240 帧 CPU p95/max 23.518/25.885ms、GPU p95/max 7.649/13.880ms，0 帧 >33.333ms。完整皮肤材质、眼部 Specular/Gloss 与 UE 同镜头画质仍未完成。
- 项目 `Tools/Author-CrouchPose.py` 从已导入的 UE Idle/Walk 剪辑和骨架制作 `CrouchIdle.hanim`、`CrouchWalk.hanim`，逐帧搜索腿部角度以限制步行足部接触误差（当前最大约 4.42 cm）。控制器新增 `IsCrouched` 参数、两状态与落地/行走转换，脚本从真实物理下蹲状态驱动。Agent 固定输入平地蹲下、蹲行和松开起立的截图已检查，Game check 与 Editor check PASS；动作仍为程序化派生，转身、斜坡、低帧率过渡及美术质量未验收。
- UE World glTF 的 `KHR_lights_punctual` 实含 8 盏点光、3 盏聚光灯和 2 盏方向光；此前横向场景仅有环境光与单盏方向光。项目 `Tools/Import-UnrealStageLights.py` 现将 11 盏局部光的 UE 坐标随展台旋转，复制颜色、坎德拉强度、影响半径和聚光角到 HE 场景；`Configure-UnrealMechanicGirl.py` 重建正式展台与换装场景时会自动应用。Game 默认场景 capture 与 Editor check PASS。点光/聚光 A/B 截图的展台行区域平均 RGB8 绝对差为 2.80/3.25/4.45，舞台可见局部提亮；没有 UE 同镜头基准截图，现有方向光、环境贴图和场景材质仍未还原。新增局部光后 240 帧横向场景 CPU p95/max 17.304/20.576ms、GPU p95/max 16.729/17.064ms；近景换装 CPU p95/max 26.134/29.442ms、GPU p95/max 16.529/17.002ms，均 0 帧 >33.333ms。灯光导致 GPU 成本明显增加，长时稳定性与目标画质仍待验收。
- UE `M_EYES` 的 `MP_SPECULAR` 接 GlossMap；眼球实例均引用 `T_EYES_Gloss`。引擎共享 MaterialSurface 现提供可选 `SpecularLevel`（介电 F0=0.08×强度，默认0.5），Eye 材质采样线性 GlossMap；GBuffer 的模型码小数携带每像素值且保留 RGB 发光，延迟直射/IBL、局部反射替换与前向光照共用该参数。项目两款眼球实例生成贴图和参数，`Diagnostics/UEEyeCloseup.he` 可重复生成。真实材质前后近景面部 ROI 差异很小；将强度暂设为0的诊断 A/B 在眼部 ROI 有 355 像素超过 RGB8 3 级、最大差63，证明链路实际运行，诊断后材质已按原字节恢复。HShaderCompiler 自测、Mechanic Girl Game/Editor、AsterCourtyard Game 回归均 PASS。当前高光仍受角膜层和灯光影响，不能称为 UE 眼球画质等效。11 局部光的换装场景新着色器下 240 帧 CPU p95/max 26.017/29.436ms、GPU p95/max 16.452/17.031ms，0 帧 >33.333ms。
- 通用 Animator 新增可保存的 `SecondaryMotionDistance`；0 继承既有 `DeformationDistance`，正值可只延长轻量发骨骼模拟距离，不连带开启布料/面部顶点变形。`AnimationSystem` 将 pose constraints 与变形的距离门分别处理，并在独立降级时重置各自解算器。项目横向展台右侧 skin03/04 发骨骼设 38 m、布料保持 16 m。固定 120 帧 Agent A/B：邻近未改角色骨骼 palette hash 同为 `595539371662fe78`，右侧两角色打开/关闭远景头发的 hash 不同；两组 clothSolvers 均 0。相同远距离长焦截图差异仅集中在两人头发附近，780 像素至少一通道差 >10，说明发片随骨骼实际移动但可见幅度有限。Game/Editor codegen+build+run、AsterCourtyard Game 回归 PASS。正式展台 240 帧 CPU p95/max 17.242/17.894ms、GPU p95/max 16.796/17.129ms，0 帧 >33.333ms；近景发束美术与长时稳定性仍未验收。
- UE 横向展示扩展到独立动作目录：项目生成器按 UE 清单将 13 段动作放在同一横向场景，每个重定向剪辑绑定正式预览网格；Game capture 画面与 60+60 帧骨骼状态检查 PASS。引擎 Editor 自动化新增 `.hanim` 预览关闭操作以释放面板资源；之前连续预览导致 NVRHI 描述符堆耗尽，修复后同一 Editor 会话 13/13 打开、拖动和关闭 PASS。240 帧测量 CPU p95/max 15.674/19.447ms、GPU p95/max 15.310/15.449ms，0 帧 >33.333ms。详见外部项目 `VALIDATION.md`；该目录是动作诊断内容，默认 15 人横向展台仍是主展示场景，最终 AAA 美术质量未完成。
- UE `M_HAIR` 的 Alpha 图以 MipBias=-1 采样并经 Intensity/Thickness 调整；引擎通用 HairCards 新增这三个可编辑参数，默认 0/1/1 保留旧行为，项目五款发型写入 UE 值。项目独立 A/B 诊断场景仅把偏置设为 0，同镜头 120 帧截图差异集中三名角色发片边缘，背景 ROI 无差异。Game/Editor 和 AsterCourtyard 回归 PASS；近景换装 240 帧 CPU p95/max 26.460/30.955ms、GPU p95/max 16.455/17.969ms，0 帧 >33.333ms。该效果可见但幅度有限，头发运动稳定性和整体 UE 对标仍未验收。
- 通用 Editor `.hmesh` 预览现可枚举、调权、重置并聚焦形变目标；按实际蒙皮段而非 `bAnimated` 判定取景所需 bind palette，`EditorCamera::SetDistance` 同步更新视图。两套 UE 角色网格均有 52 目标，实际 Editor 会话设置 `jawOpen=1` 改变顶点 hash、重置恢复；近景 Editor 截图可见控件和角色。项目中性/张口诊断 Game 截图确认嘴部变形路径运行，口腔内部观感仍粗糙，未与 UE 同镜头达到最终质量。13/13 动画 Editor 预览及 AsterCourtyard Game 回归 PASS；详见外部项目 `VALIDATION.md`。
- UE 只读命令确认 `M_SKIN.MP_SPECULAR` 为 0.5 与缩放的 `T_SKIN_Specular` 按 DetailWeight 遮罩混合。引擎通用 Skin 材质、GBuffer、延迟/反射路径现传递每像素 SpecularLevel；项目头/身实例绑定 UE 原图。张口同镜头 A/B 面部 ROI 均值差 0.090/0.192/0.222 RGB8，背景无差，口部影响极小；因此功能接入 PASS，但口腔内部仍粗糙，未误记为 UE 面部画质等效。换装场景 240 帧 CPU p95/max 26.055/28.792ms、GPU p95/max 16.489/17.238ms，0 帧 >33.333ms，Editor 与 AsterCourtyard 回归 PASS。
- UE `M_SKIN.MP_ROUGHNESS` 采用粗糙度贴图 × lerp(Min/MaxRoughness, 贴图) × Scale，再向视角 Fresnel 粗糙度混合；UE 只读材质标志还确认 TwoSided=True。通用 Skin 新增可选重映射参数，旧材质默认关闭；独立 `SkinTwoSided.hlsl` 共享同一表面求值，项目头/身实例启用 UE 参数和双面剔除。张口 A/B 面部 ROI 均值差 0.389/0.683/0.620 RGB8、1356 像素通道差 >3；单独双面 A/B 最大差 2，故口腔问题非背面剔除主导。换装 240 帧 CPU p95/max 26.073/29.586ms、GPU p95/max 17.208/18.395ms、0 帧 >33.333ms；Game、Editor 和 AsterCourtyard 回归 PASS。UE 同镜头表情和口腔内部仍未对齐。
- UE 关卡只读 actor 清单确认 15 名可见角色沿 Y 轴排列；外部项目默认 `UEFabShowcase.he` 将其按相同顺序和间距映射到 HE X 轴。`python C:\WorkSpace\HEngineProjects\MechanicGirlLab\Tools\Validate-UnrealShowcaseLayout.py` 检查 15/15 横向位置 PASS。项目外置 UE Python 自动化插件已采集原关卡中性面部 1280×720 图，且不保存 UE 资源；现已通过 UE `SetUpdateAnimationInEditor` 正确更新骨骼变换，同相机 1280×720 截图可见张口及单侧眨眼；HE 口腔上部比 UE 明显过亮，具体由自阴影、AO、几何或贴图哪一环造成仍需定位。详见外部项目 `VALIDATION.md`。
- UE 自动化表情截图已用 `SkeletalMeshComponent.set_update_animation_in_editor(True)` 修正；`jawOpen=1` 和 `eyeBlinkLeft=1` 均在真正的 UE 画面中可见。只读检查确认中央角色使用 `AS_Pose_F_01`；外部项目新增可重建的同姿势 HE 张口诊断场景。阴影关闭仅轻微影响口腔，AO 关闭影响较大但强 AO 会错误压暗整张脸，HE 口腔上腔仍明显偏亮，形变内部表面/材质尚待解决。另定位 115 骨资源首次加载时 `Animator` 固定预填 100 矩阵造成渲染世界提取拒绝；引擎改为空初始姿势时使用绑定表，非空截断姿势继续拒绝。`RenderWorldExtractorMeshPath`、`AnimationPoseContract` 单测及 MechanicGirl 同姿势/默认横向 Game 运行 PASS，首帧提取拒绝消失；Editor 回归已构建和运行 PASS。详见外部项目 `VALIDATION.md`。
- 继续检查横向展示场景时发现原 4K PNG 项目副本在 Game/Editor 都发生 `bad allocation` 和纹理回退，旧的退出码 PASS 未覆盖该缺陷。外部项目现用 DirectXTex texconv 将 46 张引用贴图编码为完整 mip 的 BC7 DDS（发片/睫毛保留 alpha coverage），22 个展示材质的 102 个引用全部切换，PNG/UE/E 原件不变；材质生成器重跑自动复用已验证的压缩结果，生成前验证源哈希，避免重导入退回 PNG。压缩后 Game/Editor 横向场景运行日志纹理加载错误为0；同姿势近景相对 PNG 的面部 ROI RGB8 均差约 0.67，画面已检查。i7-11800H/RTX3060 Laptop/DX12/RelWithDebInfo/1280×720 的240测量帧 CPU wall p95/max17.230/19.829ms、GPU span p95/max16.830/17.279ms、0帧>33.333ms。实际内存/显存峰值及长时稳定性未完成，UE 面部口腔画质仍未对齐。

### 2026-09-24 SICKA DYNASTY 关卡与引擎集成检查点

- UE `MAP_Demo` 只读清单 2528 actor；外部 `C:/WorkSpace/HEngineProjects/MechanicGirlLab` 导入 105 种静态网格、47 灯、3463 个关卡实体。`SickaDynasty.he` 加入可控 Mechanic Girl，共 3468 实体；`SickaDynastyOverview.he` 用于 UE/HE 总览对照。UE/E 原件未改。
- 通用引擎修复：90 度附近/镜像 glTF 变换分解；KHR spot light；稳定重导入的 SpotLight 字段；静态网格保守视锥裁剪；可保存的距离/高度雾；角色碰撞后实际位移速度供脚本和动画参数读取。导入流程为只读 UE 导出、稳定 ID glTF、content_sync、BC7/mip 副本、项目场景组合。
- 可玩路径实测 `W` 移动、`W+Space` 跳跃；沿街 `W/Shift` 最初穿入龙雕像，追查为仅有地面/台阶碰撞，已扩展为 1260 个地面/结构碰撞实体。相机现停在雕像外；角色贴墙回到站立姿态。重导入 plan 曾在旧 824 碰撞版本得到 0 差异，新 1260 版本仍需再次 plan。
- 画面已具完整街区构图、贴图、灯笼及雾；UE 的烘焙 GI、298 贴花、27 Niagara、36 反射捕获和 SkyAtmosphere 尚未等效迁入，天空偏白、部分人物近景和口腔仍未达 UE 质量。不能判定整体 AAA 目标完成。
- i7-11800H / RTX 3060 Laptop / DX12 / RelWithDebInfo / 1280x720：最新静态场景 60 预热 + 240 测量帧 CPU wall P95 29.53ms、GPU span P95 10.12ms，1 帧 >33.333ms。100 步沿街跑的时间窗 CPU P95 29.38ms、GPU P95 9.56ms，3/209 帧 >33.333ms；100 步走路时间窗 CPU P95 40.04ms，含一帧 439ms 动画阶段异常，仍需定位卡顿。数据只覆盖这些路径与配置。
- 最终 1260 碰撞版本重新执行 `content_sync.py plan`：`Saved/SickaDynasty-SolidReimportPlan/report.json` 为 `changed=false`、新增/修改/删除 0；Game/Editor 场景运行 PASS。横向展台 `UEFabShowcase.he` 60 帧 Game 回归 PASS。当前仍在做外部 Aster/Clockwork 回归。
- 对照 UE 角色展示关卡清单，`MechanicGirlLab/Tools/Validate-UnrealShowcaseLayout.py` 再次验证 15 名角色的横向 X 坐标与 UE 原次序、间距一致；Game/Editor 默认仍指向 `UEFabShowcase.he`。SICKA 庭院保留 UE 静态 actor 的空间坐标，总览镜头与 UE 对照图同方向。
- SICKA 项目改用可重建的 `SickaDusk.hdr` 暮色天空，并按 UE 材质实例的 diffuse tint 映射树枝/树叶；总览截图已人工检查，天空不再纯白，但 UE 的烘焙照明和体积效果仍未等效。最新带叶色的 `content_sync.py plan` 为 `changed=false`、新增/修改/删除 0；`SceneEditing` 雾参数保存/重开单测 PASS。
- 动画帧卡顿定位：旧 Agent 回放在第一帧模拟前 `cpuAnimationMs=0`，进入模拟时一次性解析控制器/全部片段，记录 `cpuAnimationMs=370.0ms`、CPU wall 443.7ms。通用 `AnimationSystem::OnRuntiemStart` 现于首个游戏帧前加载可用网格的控制器、初始姿势和零步形变拓扑；尚未就绪的网格保留正常延迟加载。相同 100 步 `W` 回放：首帧动画/形变约 8.7/8.0ms，最大 CPU wall 80.5ms（另一次加诊断计时的回放94.8ms），不再出现约370ms动画解析峰值；但仍有23/100帧超过33.333ms，30FPS严格目标未通过。一次诊断日志指向 ScriptSystem 的35.6ms首帧，且CPU PreUpdate/渲染提交仍有峰值，后续须继续定位。临时诊断代码已移除。
- 完整移除诊断计时并重编译后的最终固定 `W` 100步回放：CPU wall 中位/P95/max 28.365/35.525/71.219ms，11帧 >33.333ms；动画最大9.345ms，GPU span最大11.071ms。`AnimationPoseContract` 单测及15人横向展台60帧Game回归PASS。首次动画加载尖峰已修，严格帧时间目标仍未通过。
- 通用脚本运行时在实例创建时预备角色回调和宿主逐帧入口；项目 `MechanicGirlDemo` 的一次性场景查找与镜头绑定移入 `Initialize`，让它们在场景启动阶段完成。固定 `W` 100 步最终回放 CPU wall 中位/P95/max 28.388/33.125/52.714ms，5帧 >33.333ms；动画最大8.457ms，GPU最大10.837ms。`WalkAfterInit.png` 已目视确认角色、龙雕像与跟随镜头；仍未稳定30FPS。
- 通用 `Tools/UE/PrepareLevelForHE.py` 的关卡碰撞和材质选择现由项目 schema 1 profile 提供，不再把 SICKA 名称写死在引擎脚本。项目 `Tools/SickaLevelImportProfile.json` 生成的派生 glTF 与调整前 SHA256 完全相同；`Saved/SickaDynasty-ProfiledReimportPlan/report.json` 为 changed=false、增删改均0。流程见 `Docs/UEFabAssetIntake.md`。
- 最新回放的帧时间分项显示最高 52.714ms 帧含 update 30.499ms、render submit/present 19.274ms，其中形变约7.239ms、图记录9.524ms；另有约49/46ms帧的 `cpuPreUpdateMs` 达22.735/24.796ms，GPU仍低于11ms。需针对 CPU 预更新异常和渲染世界提交继续采样，不能靠降低 GPU 画质宣称已解决卡顿。
- UE `M_FloraTS/M_FloraB` 图与原关卡光照/后处理已用只读命令行采集到外部项目；树冠使用 UE `FuzzyShading`，无法仅靠 glTF 的乘色完整复现。HE 通用 `PostProcessingComponent` 现可保存并混合场景级色温，不再只能改项目全局。项目独立 A/B 场景证明整体降曝光会使地面和建筑过暗；正式 SICKA 改为较暗天空图、补偿 IBL、冷色雾及场景级对比度/饱和度/色温。同机位截图中地面区域平均绝对 RGB8 差由28.9降至24.1，但建筑/树冠依旧显著不一致，烘焙 GI 与叶片材质仍缺。
- 新正式场景 `SickaDynasty.he` Game capture、`RenderWorldSubmission` 单测 PASS；i7-11800H/RTX3060 Laptop/DX12/RelWithDebInfo/1280x720 静止240测量帧 CPU wall 中位/P95/max 26.152/28.985/30.551ms、GPU span最大10.963ms，0帧>33.333ms。固定 `W` 100步的活动帧 CPU 中位/P95/max27.748/34.764/51.096ms，10帧>33.333ms、GPU最大11.307ms，漫游稳定30FPS仍FAIL。
- 横向 UE 人物展台再次检查：`Validate-UnrealShowcaseLayout.py` 确认 15 人横向坐标逐项与 UE 导出一致，Game/Editor 默认场景均为 `UEFabShowcase.he`；最新预览能容纳整排。通用引擎新增 masked foliage 的模型7薄叶片受光，项目 `sicka_foliage_materials.py` 从压缩副本生成可重建的树叶材质并接入69个实体，原导入目录不改。Game 总览截图和着色诊断 PASS，但正式 UE `SubColor` 下同镜头树冠平均 RGB8 差几乎为零，当前光照/色彩仍与 UE 差距明显；该工作不能标作视觉对齐完成。横向展台90帧与 SICKA 240帧 Game 运行通过，移动严格30FPS需继续优化和重测。
- 本轮新增材质后的 `SickaDynasty.he` 静态 60预热+240测量帧（i7-11800H/RTX3060 Laptop/DX12/RelWithDebInfo/1280×720）：CPU wall中位/P95/max27.062/30.592/38.584ms、3帧>33.333ms；GPU span中位/P95/max9.661/10.124/10.705ms。第160帧CPU update/render约23.329/15.067ms，GPU约9.853ms，CPU尖峰仍在；移动模式尚未重测，严格30FPS目标未通过。
- 只读 UE 5.4 函数图检查确认 `M_FloraTS` 的 `FuzzyShading`：BaseColor接 `diffuse color`，CoreDarkness=.2、EdgeBrightness=2、Power未接线（Epic 文档默认6）；引擎通用薄叶片 shader 已增加同式可编辑视角项，项目材质生成器填写该实例值。相同 HE 总览镜头的 Fuzzy开/关树冠 ROI 平均绝对 RGB8 差约(3.44,3.28,0.68)，Game/Editor 运行 PASS，证明材质图差异已实质接入；但未配准 UE/HE 同镜头 JPEG 绝对差没有改善，不能宣称整体画面更接近 UE。新静态240测量帧 CPU wall中位/P95/max26.872/29.946/37.261ms、2帧>33.333ms，GPU最大11.176ms；CPU与 UE 光照/GI 差异均未解决。
- 2026-09-24 横向路线回放：默认 `UEFabShowcase.he` 的 15 人横向排列保持不变。外部项目 `SickaDynasty.he` 的可玩街道出生点改为 X=-20/Z=4、镜头朝 +X，配置由场景组合脚本保存。首次诊断把 `cameraYaw` 误标为 `float`，被引擎严格字段解码拒绝；改为既有 `float64` 后脚本正常加载。正式场景固定 W 120 帧 Player X=-20.09→-13.35，镜头截图已目视确认主角及街道可见。复杂画质与稳定30FPS目标仍未完成。
- 2026-09-24 可玩街道入口纠偏：普通 Game 的启动截图揭示旧 X=-20/Z=4 镜头被栅栏完全遮挡，X=-10/Z=4 启动可见但跑跳最终撞入右侧摊位立柱。项目场景生成器现用 X=-10/Z=0、初始朝 +X、相机 X=-14.5/Z=0。Agent 回放走/跑/跳后世界坐标依次约(-10.00,1.04,0)、(-4.43,1.03,0)、(5.85,1.02,0)、(7.15,2.34,0)，跳跃画面显示人物保持在石板路中央；正式场景重建及 Game 300帧捕获 PASS。此为项目出生点与线路选择，尚无证据表明镜头引擎逻辑需改动。
- 2026-09-24 SSGI A/B：安全入口同场景300帧 Off/ScreenSpaceGI 构建、运行、捕获均 PASS，所选配置为0.5渲染比例、1光线/像素、32步。正常 Game 60预热+240测量帧，Off CPU中位/P95/max26.687/30.096/33.894ms、2/240帧>33.333、GPU中位9.266ms；SSGI On CPU27.113/30.578/40.860ms、3/240帧超时、GPU中位12.136ms。画面平均绝对RGB8差约(3.69,3.34,3.38)，目视只是轻微局部变化，无法替代 UE 烘焙 GI 或证明整体视觉对齐，因此不把此诊断配置接入正式默认画质。CPU超时与视觉差距仍未解决。
- 2026-09-24 UE 树叶透光图复核：`M_FloraTS.MP_SUBSURFACE_COLOR` 的实际连线为 Diffuse×SubColor×SubColor×5。引擎通用 Foliage shader 新增可调透光增益，模型7将独立透光色与表面 BaseColor 分开；项目实例填写5并重建69处树材质引用。Game 总览前后同镜头 RGB8 平均绝对差约(0.004,0.001,0.001)、仅10像素灰度差>12，证明当前日照/阴影视角下该修正几乎不改变画面，不能当作树冠对齐完成。Game capture 与 Editor check 均构建/运行 PASS。
- 2026-09-24 UE 天空参数沉淀：引擎 `Tools/UE/InspectSkySphere.py` 从 UE 命令行只读导出 BP Sky Sphere 运行时向量/标量，本关 Zenith=(.037979,.078994,.196014)、Horizon=(.8872,.724144,1)，与此前人工 HDR 天顶(.4,.5,.72)差异显著。项目生成器可用此报告与 SkyLight 下半球色重建独立 HDR，SHA256重复生成一致；`SickaUESkyOverview.he` Game capture PASS。与 UE 斜俯视未配准截图相比，HE 上部 ROI 平均绝对 RGB8 R/G差约53.33/56.45→50.78/53.73，但整图 R/G/B差51.10/46.95/58.76→52.49/49.26/60.89；环境光随 HDR 改变，整体反而更远，正式场景未切换。云纹理、烘焙 GI 与画面对齐仍待实现。
- 2026-09-24 UE 局部反射导入：新增引擎通用 `ExportProbeFaces.py` 六面采集、`ConvertProbeFaces.py` OpenEXR→HE Radiance HDR 转换与六轴/RGBE单测。快速 commandlet 曾只捕获天空/远处碎片；改用 Epic 文档的完整编辑器 `-ExecutePythonScript` 模式，并修正 Python `Rotator` 位置参数顺序错误后，通用工具成功导出含街道/建筑/地面的六张256²线性 EXR，转成1024×512 HDR。`SickaStreetProbeOverview.he`/`Playable.he` 为独立诊断场景，正式场景未切换。HE Game A/B 60帧同镜头差异集中在探针体积内，初次捕获灰度差>2像素占7.99%、整图RGB8平均绝对差(1.289,1.312,1.409)；画面效果仍偏细微。完整工具重新捕获后的Game 300帧预热后240帧CPU wall中位/P95/max27.175/31.784/42.929ms、7帧>33.333ms，复测26.944/30.399/33.998ms、2帧>33.333ms；GPU span两次最大10.681/11.232ms。7帧超时集中在帧66–71且 update/submit 都变慢，复测未复现同一簇，不能据此断定探针是原因；稳定30FPS仍未通过。
- 六面转换产物已用独立 Radiance 解码器回读并生成经纬图预览：1024×512，解码中位0.031738、最大2.046875，街道左右、顶部天空与底部地面方向正确；测试还覆盖重建场景两次 SHA256 一致。经纬图缩略图可用于发现方向/拼接错误，但其外观不能替代 HE 材质中的反射质量验收。
- 同机位画质对照：引擎通用 `ExportCameraReference.py` 在完整 UE 编辑器模式只读输出512² `SCS_FINAL_COLOR_LDR` PNG，项目 `Build-SickaUECameraCompare.py` 以相同位置/朝向/FOV生成HE诊断场景并去掉主角遮挡。UE天际明亮、地面和建筑很暗，HE原 IBLIntensity1.9 的选定街道路面区域RGB8平均绝对差46.2、全图41.3；仅在诊断场景试0.4时路面18.9/全图31.9，试0.9时路面12.2/全图26.8。基于同机位改善，正式 SICKA 生成脚本将 IBLIntensity改为0.9，原 UE/E 资源不变。地物、树冠、天空和阴影仍有明显差异；SceneCapture曝光与编辑器视口可能不同，不把像素差作为最终美术验收。
- 正式场景新IBL值 Game 300帧捕获/运行PASS，预热后240帧CPU wall中位/P95/max28.267/33.381/35.576ms、13帧>33.333，GPU span最大11.592ms；稳定30FPS FAIL。固定Agent回放走/跑/跳位置 X=-10.00→-4.43→5.85→7.15m，Z约0，跳跃Y=2.338m，session已停止；该回放334记录帧CPU中位/P95/max30.304/41.460/61.356ms、66帧>33.333，GPU span最大11.791ms。Agent逐段step在边界会产生脚本IPC停顿，极端帧不能直接视为连续手动游玩的峰值；走跑各100步的内部帧仍分别有10/26帧超时。角色控制与镜头路线可用，持续帧时间仍须优化。
- 局部阴影缓存通用修复：RenderWorld 提取现用当前骨骼调色板扩张角色包围盒，避免所有局部光都把远处角色视为投影者；局部阴影深度着色器不读取自定义材质时间，因此签名不再因静态 Mask 树枝的 TimeSeconds 每帧失效。SICKA 13 个局部阴影视图从全脏变为 5 脏/8 缓存；同镜头截图逐通道平均差低于 0.001 RGB8。i7-11800H/RTX 3060 Laptop/DX12/RelWithDebInfo/1280x720，预热60+测量240帧 CPU wall 中位/P95/max 26.084/28.743/32.776ms，0帧超过33.333ms；GPU span最大10.489ms。RenderWorldSubmission 单测、Game/Editor 运行及横向展台布局验证 PASS。该静止短跑通过30FPS门槛，移动回放和长时稳定性仍须复测。
- 阴影修复后重新执行Agent固定走/跑/跳：角色X=-10.00→-4.43→5.85→7.15、跳跃Y=2.338m，Agent会话已停止。`SickaTraversal-ShadowCache-1280x720.csv` 共340记录帧，CPU wall中位/P95/max28.399/39.249/60.815ms，52帧>33.333ms；GPU span最大10.960ms。局部阴影缓存中位11/13视图。Agent阶段切换有IPC停顿，此数据不能替代连续手动游玩；移动严格30FPS仍FAIL。
- SICKA 树叶覆盖率复核：UE glTF 的三种树叶遮罩阈值均为0.3333，项目原导入profile曾统一改为0.5。通用 `PrepareLevelForHE.py` 现保留源阈值，仅为缺失值使用profile默认，并支持显式按材质覆盖；新增最小glTF回归测试PASS。content_sync plan预计69处实体/3个材质变更，sync成功后重复plan为0差异。项目 foliage生成器改从压缩manifest和稳定贴图特征查找当前源材质，不再硬编码内容哈希；69处实体重新绑定自定义薄叶材质，重建两次SHA256一致。UE/HE 512平方同镜头整图RGB8均差由(24.728,24.624,31.183)略降到(24.561,24.466,31.027)，约5981像素新旧HE差>2；远未达到UE画质等效。
- 单次连续`W`输入120步的Agent回放（接口上限120步）在预热60步后执行，中途无查询或改键，session已停止。统计文件帧70–189的120个连续渲染帧CPU wall中位/P95/max27.777/31.433/35.865ms，3帧>33.333；GPU最大约10.3ms。超时帧141/154/188的Layer Pre-Update约2.65/2.24/2.33ms，邻帧多为0.1–0.2ms；该阶段尚未定位到具体原因，严格稳定30FPS依旧FAIL。
- 横向展示场景复核：项目 Game/Editor 默认场景均指向 `UEFabShowcase.he`；15名角色的HE X位置与UE展示关卡的横向顺序及坐标一致，现有1280×720截图可见整排。SICKA街道场景是独立的横向路线，不覆盖角色展台。UE树叶材质只读检查进一步确认 `M_FloraTS` 的有效遮罩阈值为0.3333，`MI_TreeBranch2` 没有覆盖该值；因此不应通过改成不透明叶片来掩盖树冠轮廓差异。BC7/PNG A/B仅造成小幅差异，树冠对齐和移动30FPS仍未完成。
- 新增通用 `Tools/UE/ExportViewportReference.py`，使用与SceneCapture相同的相机配置，在完整UE编辑器中输出实际视口512平方PNG；相机画幅/FOV设置后的截图与先前独立视口截图逐通道均差低于0.1 RGB8。UE视口同样显示近景大块树片，排除SceneCapture专有假象。UE隐藏69棵树前后遮挡区约8.27万像素；HE正常遮罩约5.58万，不裁剪诊断约7.39万；前220行轮廓交并比0.66→0.81，但整图RGB差未全面改善，不能直接启用诊断材质。UE树网格Nanite关闭且有4个LOD，LOD0的3个section在HE合并为2个primitive、叶片顶点4695+5109=9804全部保留；UE FBX、glTF、HE导入网格的UV0逐顶点直接差均为0。UE纹理未禁用Alpha、未启用mip覆盖率缩放，材质遮罩使用UV0的Diffuse A通道，覆盖率根因仍在调查。
- 自定义材质纹理的仿射UV设置目前未进入生成的 `g_AlbedoMap_UV`，仅UV set被编码；因此项目只改采样元数据的V翻转诊断图与原图几乎完全相同，已删除无效诊断产物。临时改着色器再恢复的验证命令被自动审批拒绝，未执行；正式材质保持不变。通用UV变换缺口需在材质参数ABI与着色器生成流程中修复并用真实非恒等变换回归验证，但它不是当前SICKA材质（恒等UV）的已证实根因。
- 引擎开发版Game新增 `--input-replay=<绝对JSON>`，`hectl game check/capture -InputReplayPath` 从项目路径解析并运行有界分段按键回放；复用合成键盘，Agent会话与打包游戏不使用。项目 `SickaTraversal.replay.json` 共300帧待机/走/跑/跳，Game无Agent捕获330帧，日志确认5段全部执行；后续测量帧的 `cpuPreUpdateMs` 最大0.044ms，与此前Agent走路帧前2.2–2.7ms尖峰区分开。原版本270测量帧CPU中位/P95/max26.047/29.128/33.916ms、1帧>33.333，GPU最大10.502ms；仍不能声称稳定30FPS。
- 一次无Agent回放在帧229触发表情眨眼，`deformationVertexUpdates` 从10390突增51538，形变耗时约7.5ms。Fab人物9段都保留52个表情名称，但多数段的对应位移/法线/切线增量全为0；引擎 `SubMesh` 现记录每目标是否真正改变几何，仅对有效目标判定缓存变化，人物变形、Mesh加载和预览跳过全零分段，仍保留名称供编辑器显示。MeshCpuDecode单测、Game/Editor构建运行PASS；相同输入最终截图RGB8逐像素相同。眨眼段顶点更新降至18950，形变耗时中位7.424→4.901ms，帧229 CPU wall33.916→26.624ms。首轮优化后270帧仍有1帧>33.333，复测有3帧，最大41.475ms，涉及其他更新/提交波动；严格稳定30FPS尚未通过。
- UE树叶mip策略已进入通用压缩工具：`CompressImportedLevel.py --texture-policy` 允许按源贴图关闭覆盖率保持，SICKA两张树枝图按UE `do_scale_mips_for_alpha_coverage=False`配置。Branch2的64² mip在0.3333阈值下覆盖率由12.79%增至16.58%，接近源图BOX缩小后的17.04%；同镜头HE相对UE视口整图RGB8均差由(29.882,26.766,31.943)降至(28.600,25.792,31.395)，近景大块树冠轮廓和GI仍不等效。源UE和导入PNG不变，项目压缩manifest重跑哈希稳定。走跑跳固定回放运行PASS，但新资源在有UE与HE编辑器同时运行的首次270帧GPU中位48.5ms；旧mip邻近A/B前90帧GPU中位12.3ms，新mip复测中位10.5ms而P95升至135.7ms。差异不稳定，不能将尖峰归因于mip或声称稳定30FPS；须在可控GPU负载下继续定位。
- 保存的 `MAP_Demo` 当前把 `MI_PP_Wave`、`MI_PP_Sharpen` 权重设为0；只读UE检查记录27处Niagara，其中21处落叶、6处魔法旋涡。新 `street-no-post` SceneCapture 可重复取得512²画面；另一异步视口截图任务未产物，不能作新基准。树冠轮廓差异在关掉这两层后仍存在。
- 引擎通用材质顶点偏移钩子现处理主绘制、阴影及上一帧速度；Foliage shader 提供默认关闭的风幅度/角度/频率/波长。项目树材质按0.06m、0.4Hz配置，第90帧开/关风的差异局限树冠，Game capture与Editor指定场景check PASS。Fab落叶贴图只读导出后由项目脚本生成Effekseer效果，21处UE发射器以稳定ID和原坐标进入正式场景；Game近景同帧有/无落叶像素差证实实际绘制，但叶片偏小稀疏，6处魔法旋涡未迁入。
- 当前正式SICKA无Agent走跑跳330帧回放，预热后270帧CPU wall中位28.177ms、P95 30.731ms、最大38.350ms，3帧超过33.333ms；GPU中位9.675ms、最大10.842ms。严格稳定30FPS仍FAIL。AsterCourtyard Game 60帧回归PASS；下一步定位CPU更新/命令录制尖峰、补余下场景VFX和UE大块树冠/间接光差异，再做长时Game/Editor实测。
- 用户截图指出初版落叶是白色方片，实际原因为Effekseer编译时相对贴图文件不在`.efkproj`目录，编译节点颜色纹理索引-1，资源包虽有PNG但运行时绘制无纹理方片。项目 `Build-SickaLeaves.py` 现离线派生UE遮罩cutoff0.3333背光叶色，先把贴图暂存到效果源旁，再编译，纹理索引已变0；同时使用1.80的 `CommonValues/Generation` 真正分帧发射。用户给的UE截图背光叶片偏暗紫褐，项目 `SickaLeafAppearance.json` 可调，最终近景画面叶片中位RGB约[47,41,82]，原白常量方片709像素变为0。UE系统同时使用Niagara（发射与运动）和 `M_Particle_Leaves`（Default Lit、Masked、cutoff0.3333、叶片贴图）；原Fab资源不改。前述初版落叶外观与续播结论已被本次纠正覆盖。
- 最终叶片版Game 180帧1280×720近景捕获、SICKA Editor45帧运行和AsterCourtyard Game60帧回归PASS。正式走跑跳回放270测量帧CPU wall中位26.560ms、P95 28.048ms、最大36.066ms，1帧超过33.333ms；GPU最大10.342ms，稳定30FPS仍FAIL。下一步验证最终版>720帧循环、进一步缩小UE落叶外观/运动差异并定位CPU帧138尖峰。
- 最终落叶改为Effekseer 1.80无限发射、20帧间隔，单发射器理论同时存活不超过13叶；用户截图式的暗紫褐背光色、遮罩叶形与分散下落可在项目JSON编辑并重新生成。800帧Game画面与无落叶同帧有249像素差>20，有限时长版仅121，周期性重启变稀已消除。最终包双次构建哈希一致；Game漫游330帧回放、Editor指定场景45帧PASS。270测量帧CPU wall中位/P95/max26.448/28.311/35.378ms，仍有2帧超33.333，GPU最大10.085ms。严格稳定30FPS与UE完全美术等效未过。
- 落叶最终项目参数定为暗紫褐背光色[58,48,70]、0.48m叶片、20帧间隔无限发射；最终包SHA256 `3c733fae4558cfc59c0bf3fefbcc178ebd2b6565cb8de0c6a8f2502f11fdf13e`。1280×720近景最终图已目视检查，Game/Editor正常加载；900帧长时Game回放预热后840帧CPU wall中位25.401ms、P95 27.759ms、最大35.660ms，仍有2帧超33.333；GPU最大10.369ms。一次270帧短测虽0超时，但不能覆盖更长运行失败。继续定位CPU动画/布料及提交尾部，并测Editor连续FPS。UE截图的粒子密度、旋涡运动和动态光照尚未精确等效。
- Editor 900帧连续检查发现每500ms整场景序列化一次，导致约49ms的周期性Pre-Update尖峰；引擎现对空闲旧写入分帧逐实体检测，已标记编辑及保存仍使用完整快照。动画控制器不再把运行时片段号写进可保存的`currentClipIndex`，修复空闲打开场景后被误标为dirty。相同SICKA Editor场景预热后840帧Pre-Update P95/max由48.742/52.110ms降至0.200/0.347ms，CPU wall P95由84.202降至44.321ms；Editor wall中位仍34.988ms、839/840帧超33.333，需继续优化更新与视口提交。Game固定漫游270测量帧CPU wall中位/P95/max27.052/28.823/32.550ms、0超时，但之前900帧仍有2超时，不能宣称长时稳定30FPS。
- 落叶生成器已把项目树枝叶片原图不透明像素中位RGB(101,115,78)按亮度缩放后与UE截图背光参考色按45%混合，最终叶片基色RGB(53,51,55)，原树叶/落叶资源均不变。两次生成包SHA256一致为`b8c170b28757ff7acf0cf1707895176cbc02a5f884b62965614eb99c61218941`；近景第180帧Game捕获PASS，预览目视为偏中性的深色叶片，无白方片。相同画面相对前版有1257像素RGB8变化，673像素最大通道差>10。树叶与落叶现共享色相参考，但落叶仍是离线色片，尚非UE Default Lit动态光照等效。
- 最终树叶配色包下，SICKA正式关卡同一走跑跳回放连续两次900帧Game检查均PASS，各自60预热后840测量帧0帧超33.333ms；CPU wall中位/P95/max分别25.678/28.023/33.140ms与25.571/27.982/30.671ms，GPU最大10.428/10.340ms。此为指定硬件/分辨率/运行模式的重复观测；此前旧包900帧有2超时，不把结果外推到所有镜头和设备。Editor连续画面仍需优化。
- SICKA Editor旧版900帧CPU wall中位34.988ms、839/840测量帧超33.333ms。通用Embree编辑器拾取从每帧重建场景改为场景首次同步及点击时按需同步；层级面板对展开的子实体列表使用裁剪，仍在过滤或选中项滚动时全遍历。-RenderStats追加cpuImGuiMs和cpuRuntimeRenderMs，旧CSV列保留。两次900帧Editor检查各840测量帧0超时，CPU wall中位/P95/max分别19.656/25.166/30.073ms、19.536/23.357/28.632ms；1280×720实际Editor捕获的层级、视口和保存状态已目视检查。新增有界editor.pick回归：盒体原位射线命中，场景作者事务移走100m后原位未命中且新位置命中同UUID，Editor退出0。AsterCourtyard Editor 90帧跨项目回归PASS。
- Game通用CSV阶段计时接入后同一330帧固定回放两次均构建/运行PASS；第一轮270测量帧第185–186帧CPU wall 41.208/42.916ms（2帧超33.333），GPU约9.4–9.6ms；原样复测0超时、最大31.307ms。该尖峰未能稳定复现或定位，不能把之前两次长测达标推广为全面稳定30FPS。完整UE树冠、GI、贴花、魔法旋涡、面部材质与演出互动仍有未完成项。
- 本轮两帧异常的分阶段CSV显示：帧185/186 updateMs约27.13/27.18，邻近帧约16–18；动画形变约7.33/7.53，邻近约4.2–5.4；RuntimeRender约13.60/15.24，邻近约9.4–14.2；GPU span仅9.43/9.61。原样复测同帧CPU wall约27.57/26.57ms。多个CPU阶段同时变慢，尚无证据证明是固定输入、GPU、资产上传或某个特定系统造成；继续保留混合结果，不以主观推断修改内容。
- 2026-09-24 落叶与树冠再对色：项目独立配置将树叶原图遮罩内中位RGB(101,115,78)作为落叶色相，亮度系数0.55后基色(56,63,43)，修正此前暗紫/黑色叶片；原Fab素材未改。Effekseer包双次生成SHA256一致 e4291a2f2939024e6a5f13f948ece740b6b438873420cf001308c9a57b0962ce，近景1280x720第180帧目视暗绿叶片与树冠更协调且无白面片。正式Game 330帧固定回放与Editor 90帧构建运行PASS；Game预热后270帧有2帧超过33.333ms、最大46.953ms，稳定30FPS仍未通过。
- 2026-09-24 UE旋涡诊断：只读检查 MAP_Demo 的6个 NiagaraSystem_MagickVortex 实例，导出原 FireAtlas 副本并在外部项目生成可编辑 Effekseer 源/包、6个稳定ID场景实体；正式场景重建仅新增这6实体。HE Game 512x512同镜头有/无旋涡180帧捕获均PASS，新增暗红水平环可见，但尚未证明与UE旋涡等效。UE完整编辑器同镜头隐藏Niagara前后仅6像素RGB8差>10，可能是SceneCapture未驱动Niagara；移除器皿静态网格后画面大变，不能据此断定红环来源。UE材质图确认器皿发光图采UV0，先前UV1猜测被排除。六盏器皿SpotLight在UE强度10，glTF缺intensity默认1，但仅把HE强度改10的场景A/B没有显著画面变化（0像素RGB8差>10）；需要继续追踪UE导出颜色/温度及HE光照单位，暂不把诊断参数写入通用导入器。
- `runtime-manual PASS / 性能门槛FAIL（落叶最终配色长时回放）`：`.\Tools\Agent\hectl.ps1 game check -Project C:\WorkSpace\HEngineProjects\MechanicGirlLab -StartupScene Assets/Scenes/SickaDynasty.he -InputReplayPath Tools/SickaTraversal.replay.json -Frames 900 -Resolution 1280x720 -TimeoutSeconds 300 -PerformanceWarmupFrames 60 -RenderStats -RenderStatsPath C:\WorkSpace\HEngineProjects\MechanicGirlLab\Saved\Performance-SickaLeafTreeMatched-900.csv` build/run/replay PASS；i7-11800H/RTX3060 Laptop/DX12/RelWithDebInfo/1280x720，840测量帧CPU wall中位/P95/max27.006/30.002/40.118ms，4帧>33.333ms（帧244、249–251），GPU span最大11.186ms。与330帧短测的2帧超时相符：当前改色/旋涡组合不能声明稳定30FPS。
- 2026-09-24 UE器皿红环归因复核：新增只读UE器皿actor检查，六个SM_LightSet_FirePlaceExt均仅有一个MI_LightSet静态网格组件、无attached child；同机位SceneCapture隐藏六盏SpotLight后仅15像素RGB8差>10，灯光不是该参考捕获中红环的主要直接来源。先前HE发光贴图A/B只改.hmat的Properties而漏改实际Textures绑定，结果无效；修正后纯白发光贴图使整器皿显红（与原画面28797像素RGB8差>10），证明材质覆盖与发光路径可用；原发光贴图纵向翻转只使器皿底部1311像素明显变红，不是UE顶部环，不能将UV翻转作为通用修复。UE SceneCapture中临时改材质参数/材质资源几乎无差，未证明渲染代理完成刷新，此诊断视为无效并撤回工具选项。红环来源及与HE材质/粒子分工仍未确定。
- 2026-09-24 六盏器皿灯修正：UE只读检查六个SpotLight强度10、光色255/159/0并启用2288.57K色温，原UE glTF六项颜色峰值0.002233且无intensity字段。通用 `PrepareLevelForHE.py` 新增按唯一灯名严格校验的profile `lightOverrides`；项目仅将六项导出色相比例归一并设强度10。新旧准备glTF除六盏KHR灯外完全相同；`hectl content plan` 报0增/6改/0删。`content sync -Verify runtime` 先发布后因静态场景无唯一主相机而verification_failed，随后 `-Verify none` 报unchanged/ok；导入场景六项均为10和[1,.112637,0]。重压缩52纹理/18材质后manifest仅sourceSceneSha256变化；重组可玩场景3495实体无增删且仅六个SpotLightComponent变化。Game同镜头导入后画面与独立灯光诊断逐像素相同，器皿邻域相对UE参考RGB8均差由(36.69,22.84,25.51)至(31.86,22.29,25.59)，但红环、GI、树冠仍未对齐。
- 2026-09-24 旋涡放置校准：原六个项目Effekseer环按UE Niagara actor z=200cm直接放置，在HE同机位悬于器皿口沿上方；仅项目 `SickaVortexAppearance.json` 添加emitterHeightOffsetMeters=-0.4，正式3495实体场景与旧版相比只改六个旋涡Transform高度。512²第180帧A/B：参考ROI的UE/HE RGB8均差从(33.20,24.61,28.97)降到(32.76,24.65,28.96)，红环目视更贴近器皿口沿；这是局部小改善，未代表整体UE等效。正式重建效果包双次SHA256相同 d382e0cbdd169976222622e08f5394fe1edd29d50ebfc5a5962aebd4036927bc，同场景诊断图与预先高度A/B仅1像素最大差5。
- 最新旋涡高度正式版Game 900帧固定回放：840测量帧CPU wall中位26.459、P95 29.201、最大45.851ms，1帧超33.333；Editor 900帧同场景840测量帧最大31.157ms、0超时。Game尖峰在帧258的动画/形变/布料CPU阶段，GPU仅9.340ms；30FPS整体严格门槛仍未通过。
- 2026-09-25 树叶与落叶色相核对：UE `T_TreeBranch_BaseColor` 预览几乎全为灰枝条，旧绿色落叶变体因从它取样而呈灰色；真正的绿叶在 `T_TreeBranch2_BaseColor`，遮罩内中位RGB(101,115,78)。外部项目把绿色包取样源改为该贴图，0.55亮度后基色(56,63,43)，并在构建时断言绿色与褐色色相。21个正式发射器仍依最近树的 `MI_TreeBranch2` 选褐色包；绿色包在同机位替换诊断中运行可见，不误称当前关卡存在绿色树。两个运行包/编辑源已重建，UE/E盘源资源未改。
- 通用 `EnvironmentLightComponent.SkyExposureCompensation` 已经由反射序列化、RenderWorld提取和天空shader接通，仅调可见天空、不提高IBL。UE相机512² A/B中+2EV使HE天空由蓝灰接近UE亮天，正式 `SickaDynasty.he` 已保存+2EV；树冠形状/GI仍不等效。Editor `HE_EDITOR_UPDATE_TIMINGS` 和通用应用 `HE_LAYER_UPDATE_TIMINGS` 增加可选CPU尖峰阶段日志。3600帧Editor诊断两轮分别8/3540帧超33.333ms，最大155.066/50.411ms；后者尖峰多CPU阶段同时增时、GPU仍约10ms，根因未定。
- 最新正式场景Game 900帧走跑跳回放：840测量帧CPU wall中位/P95/max26.783/29.402/36.011ms，2帧超33.333，GPU最大10.887ms；Editor 900帧中位/P95/max19.754/24.263/33.742ms，1帧超33.333，GPU最大10.460ms。两者构建运行PASS，严格稳定30FPS仍FAIL。
- 2026-09-25 追加暖叶对色：正式 `MI_TreeBranch2` 叶贴图遮罩中位RGB(101,115,78)，材质 BaseColorFactor=(1,.702458,.233331)；项目生成器按sRGB→线性乘因子→sRGB并做亮度/色相微调，得到暖叶RGB(73,65,26)，替代手填(72,45,25)。新同镜头第180帧 `SickaLeavesCloseWarmTreeMatched.png` 目视暖橄榄色落叶更贴近树冠，绿色包仍为(56,63,43)。此为项目美术参数，通用Effekseer播放无需改动。
- 最终暖叶包重建后，正式场景Game和Editor 900帧各840测量帧均0超时：Game CPU wall中位/P95/max26.814/29.015/33.004ms，Editor19.467/23.066/26.508ms；同机位180帧暖叶截图PASS。历史3600帧Editor和其他900帧尖峰仍未定位，严格长期稳定30FPS及UE树冠/GI等效未完成。
- 2026-09-25 UE树冠根因复核：只读 UE 材质实例报告确认 `MI_TreeBranch2` 的 Diffuse=`T_TreeBranch2_BaseColor`、TextureTiling=1、desaturation=0、`diffuse color`=(1,.702458,.233331)，与HE材质设置一致。源PNG与HE导入PNG逐像素相同；BC7解码后顶级alpha遮罩覆盖率13.6983% vs源13.7060%，平均绝对alpha差0.973/255。项目 `Validate-SickaTreeImport.py` 核对69棵树位置及四种LOD0网格的完整三角形位置+UV multiset均相同，排除漏树、LOD0三角/UV和顶级alpha贴图错误。
- 通用只读 UE 相机采集工具增加 `forceMeshLodPrefixes`+`forceMeshLodIndex`、实际LOD数校验及 `SCS_BASE_COLOR` 输出。SICKA强制LOD3使树变成巨大平面公告板（与LOD0图RGB8均差约26.44/20.54/19.64），而原始与强制LOD0只约0.15/0.13/0.12，故当前参考图不是LOD3公告板。UE Python临时替换叶材质的grid/white/zero A/B在捕获中几乎无差，缺少可见生效证据，不能拿它判断原材质的遮罩路径；近景树冠真实差异仍待定位。
- 原生深度对照定位树冠覆盖缺口：UE `SCS_SCENE_DEPTH` 的有树/无树512平方EXR，与HE现有 `capture.frame source=depth` 的D32有树/无树RAW，同机位上方150行覆盖率分别68.80%/60.89%，上方250行54.52%/47.84%；前250行交并比0.838，UE独有像素7.86%、HE独有1.18%。项目 `Compare-SickaTreeDepth.py` 固化阈值、sidecar校验和双色差分预览，排除单纯曝光/着色导致的假差异。已确认HE树材质CullMode=None、UE材质two_sided=True，不能把缺口简单归咎于单面剔除；需继续核对运行时mip/纹理采样及材质遮罩。未据此更改UE真实0.3333遮罩阈值。
- 独立HE启动会话复核遮罩A/B（热切换未提交的捕获已剔除）：同机位上方150行，源PNG遮罩56.57%、当前DDS 60.89%、不裁剪73.60%；HE阈值0.20为65.11%、0.10为67.32%，UE为68.80%。阈值0.10的前150行轮廓交并比0.912（原0.846），彩色512平方整图UE RGB8均差由(24.33,24.44,31.00)降为(22.32,22.62,29.61)。项目材质生成器因此显式校准 `MI_TreeBranch2` 遮罩为0.10，保留导入源/UE资产的0.3333和通用引擎可编辑阈值；旧`SickaUECameraAlpha033.he`诊断现固定引用0.3333副本以免基线随正式材质漂移。项目材质二次重建SHA稳定。此为项目视觉校准，尚未定位HE与UE纹理采样的根因，GI/天空仍差异明显。
- 正式 `SickaDynasty.he` 保留69棵树材质引用及21个暖落叶发射器。i7-11800H/RTX3060 Laptop、DX12、RelWithDebInfo、1280x720、60帧预热后，Game走跑跳回放与Editor各900帧中840帧测量：Game CPU wall中位/P95/max27.060/29.462/32.820ms，Editor19.802/23.906/30.813ms，均0帧超过33.333ms；GPU最大10.821/10.436ms。此次短测达标，不覆盖此前3600帧Editor尖峰，长期严格稳定30FPS仍待处理。
- 当前树材质0.10的正式Editor 3600帧长测再次FAIL：3540测量帧CPU wall中位/P95/max19.997/25.059/119.106ms，109帧>33.333ms；其中帧842–947附近约105帧连续GPU span从常态约9ms升至50–115ms，GPU pass sum同步上升，CPU frame-slot wait在峰值达62.2ms；可见物452、约216万三角形、阴影绘制399基本不变。UE编辑器同时仍在运行，是否外部GPU竞争或GPU时钟变化尚无同步遥测，不能归因为树材质，也不能宣称长期30FPS通过。需对异常区间采集GPU时钟/温度/其他进程负载再复测。
- UE5只读 `Inspect-SickaFog.py` 找到此前旧属性名漏掉的真实雾设置：FogInscatteringLuminance=(.541667,.954167,1)，DirectionalInscatteringLuminance=(.25,.25,.125)、exponent4、start100m，体积雾开启、albedo=(255,215,168)。HE当前只实现均匀高度雾和地平线混色，没有可编辑的方向性散射/体积雾对应物；天空与远景色差因此还不能靠直接复制UE FogDensity=.05完成。下一步按已测同机位A/B决定通用方向性散射的实现与性能预算。
- 引擎通用环境组件新增可保存的 `FogDirectionalColor/Intensity/Exponent`，场景提取、帧常量和天空shader相连，默认强度0；Editor属性面板可编辑。SICKA隔离场景使用UE实测(.25,.25,.125)/4：朝向太阳的On/Off有35561像素RGB8最大差>5，均差(3.419,2.926,.890)；背向太阳的正式参考机位仅4像素>5，UE RGB均差无改善，因此不将效果打开到正式关卡。旧默认关闭同场景同帧前后像素均差约0.0001、0像素差>5。`RenderWorldSubmission`与`SceneEditing`单测、Game/Editor实际运行均PASS。该实现是天空方向雾，不等效UE体积雾、云与烘焙GI。
- 2026-09-25 落叶与树冠复核：21个正式发射器的最近树均用 `MI_TreeBranch2`，其叶原图遮罩内RGB8中位(101,115,78)，材质因子(1,.702458,.233331)使树冠呈暖橄榄色。旧落叶包另乘0.93绿色压制后RGB(73,65,26)，比树冠显著偏褐；项目 `SickaLeafAppearance.json` 将绿色补偿调到1.08，新包(73,76,26)，近景实际捕获呈橄榄绿且无白片。保留通用绿色包供真正绿色树使用，正式21处仍按树材质选暖橄榄包。此为项目内容校色，不涉及引擎播放功能变更。
- 同步GPU遥测的Editor 3600帧长测仍未过严格30FPS：3540测量帧159帧>33.333ms；帧414–519连续106帧GPU跨度升至约115ms、绘制数稳定，附近 `nvidia-smi` 样本出现210MHz图形时钟、100%利用率、约37W/59°C。关联性支持设备时钟/排队异常，但驱动、电源策略或其他进程何者触发尚无证据。另有帧724 CPU wall144.670ms、GPU9.179ms，属于不同CPU侧尖峰，需单独定位；不可据短测宣布长期30FPS达标。
- 当前0.10树冠遮罩下重新做同机位天空曝光0/1/2/3 EV A/B，避免沿用旧0.3333诊断。UE与HE共同天空18523像素的RGB8均值UE(225.97,222.04,232.52)、HE 0EV(170.13,181.71,208.67)、1EV(213.22,220.10,234.48)、2EV(236.94,240.26,246.69)；天空MAE依次40.10/13.10/18.98，1EV最佳。项目正式场景及可重建生成器改为1EV，保留引擎通用 `SkyExposureCompensation`。固定机位对照仍显示树冠、雾、GI差异，天空曝光不能替代这些功能。
- 引擎通用 `EnvironmentLightComponent.SkyColorTint` 补上天空独立RGB校色：反射保存、RenderWorld提取、FrameConstants、SceneSky、Editor控件与SceneEditingService范围校验已接通；默认(1,1,1)保持旧场景，IBL采样不被染色。SICKA项目在1EV下选(1.30,1.05,0.94)：固定UE机位共同天空18523像素HE均值由(213.22,220.10,234.48)到(224.43,221.99,232.67)，天空MAE13.104→11.589，整图MAE23.369→23.309；默认中性新旧画面仅2像素差>2。正式场景及生成器已保存项目数值。`RenderWorldSubmission`、`SceneEditing`、Game/Editor运行均PASS。整体画面对齐与长期30FPS仍未完成。
- 2026-09-25 纠正UE画面对照基线：`street-final-ldr` 为UE `SceneCapture2D`，而用户要求的是编辑器视口；`street-final-ldr-viewport.png` 与另一次独立的 `street-viewport.png` 均来自只读 `ExportViewportReference.py`，两张视口图RGB8均差仅0.0508。视口里的马车、柱子与HE固定相机的构图更一致，因此前述以SceneCapture得出的1EV最佳及(1.30,1.05,.94)校色结论不适用于正式验收，已明确撤回。用HE深度天空且UE视口明亮像素的交集18717像素复核，HE 1EV/2EV天空误差18.71/11.11；2EV+(1.27,.94,.55)后为7.683，整体像素MAE相对2EV中性25.387→25.160。项目正式场景和生成器改回2EV并采用视口色调，独立 `SickaViewportSkyAligned.preview.jpg` 已目视。树冠逆光、雾与GI仍明显不一致。
- 同机位雾A/B：把UE雾色(.541667,.954167,1)直接放入HE现有距离雾，会把远景染青；整图MAE23.31→24.92，密度翻倍到.024则28.78；较柔蓝色(.35,.48,.62)密度.018为23.86，均不改善旧SceneCapture图且视觉不贴近UE视口。因此不改正式雾，UE体积雾与HE距离雾参数不能直接互拷。`HE_PBR_DEBUG_VIEW=BaseColor` 与UE `SCS_BASE_COLOR` 输出编码/曝光明显不同，不能直接把两张图的RGB差当作材质误差。
- 视口校准后的正式Game走跑跳900帧与Editor900帧均构建运行PASS；i7-11800H/RTX3060 Laptop、DX12、RelWithDebInfo、1280×720、60帧预热后各840测量帧，Game CPU wall中位/P95/max27.922/30.546/40.353ms，4帧>33.333、GPU最大11.211；Editor19.893/23.834/27.079ms，0超时、GPU最大10.469。Game尖峰GPU不高，严格稳定30FPS仍FAIL；此前3600帧Editor低GPU时钟异常仍未解释。
- 2026-09-25 太阳轴向根因：UE只读 Sun Forward=(-.426692,-.356027,-.831372)，glTF换轴后局部`-Z`映射到HE世界(-.426692,-.831372,-.356027)；此前HE固定取节点局部`+Y`，恰好对应UE Actor Up，导致阴影向错误方向投射。引擎定向光新增默认`+Y`的可保存`LocalDirection`，glTF导入器写`-Z`；场景重导仅太阳1处实体变更、0新资源，第二次plan为0差异。固定机位树木开/关：修复前道路区平均RGB8差0.123、无像素最大通道差>32；修复后平均差67.101、80017像素>32，证明树影落入道路。UE视口全图RGB8 MAE仍25.160→25.420，整体对齐未通过；需要继续处理树冠遮挡、GI/雾与色调。
- 修复后正式Game走跑跳与Editor各900帧均构建运行PASS；i7-11800H/RTX3060 Laptop、DX12、RelWithDebInfo、1280×720，CSV已排除60帧预热，各840测量帧Game CPU中位/P95/max26.495/28.975/42.507ms、4帧>33.333、GPU最大11.615；Editor20.113/24.436/32.269ms、0超时、GPU最大11.787。Game及长期Editor严格30FPS仍未证明稳定。引擎`RenderWorldSubmission`单测与glTF定向灯导入专项测试PASS；项目原始UE资源未改。
- 2026-09-25 修正太阳后重测雾：固定UE视口与后退2米的独立视口均以Game第90帧512²对照，项目FogDensity .012→.024使全图RGB8 MAE分别25.420→23.534、30.587→28.245；改善主要在中远景，道路误差基本不变。项目生成器及正式场景保存.024，保留18米起雾；0米起雾的.012/.024测试全图误差28.90/45.89且道路洗白，未采用。UE顶部树冠与HE深度覆盖交集内RGB8均值UE(27,45,66)、HE(31,28,23)，根因指向蓝色环境/体积散射与树冠着色；材质BaseColor为(85,85,33)、HE直接光约(10,6,4)、漫反射IBL约(21,18,6)。未把树原始金色叶材质强行染蓝，仍需真正的环境/大气表现。
- FogDensity .024正式场景Game/Editor各900帧构建运行PASS。840测量帧Editor CPU中位/P95/max19.786/23.761/27.146ms、0超时；Game首次在帧660–753出现94帧连续GPU跨度55–112ms并有98帧CPU>33.333，重复同参数Game时GPU最大11.814ms、CPU中位/P95/max26.586/29.216/50.236ms，仍有7帧>33.333。二次运行GPU遥测样本时钟最低210MHz、运行阶段约2002MHz；首次长GPU异常与过往Editor类似但该轮未同步采样时钟，不能断言具体原因。严格长期30FPS依旧FAIL。
- 2026-09-25 树冠冷蓝问题继续定位：已有UE完整编辑器采集的街道六面探针真实包含树木/建筑；将其通过现有引擎局部反射与漫反射探针放进固定视口隔离场景，Game90帧capture PASS。树冠顶150行改动平均仅0.847 RGB8，全图相对UE视口MAE 23.534→29.342，近路面被明显提亮。因此未写入正式场景；下一步集中检查UE体积雾/天空散射与HE树冠在逆光下的着色，不用改变落叶的树种色谱。当前21个落叶发射器均绑定近邻 `MI_TreeBranch2` 暖橄榄叶材质，绿色树变体仍可用但此关卡没有已确认的绿色叶树。
- 2026-09-25 新增引擎通用第二层解析距离/高度雾：`EnvironmentLightComponent` → `RenderWorld` → frame constants → Deferred/Forward 着色；默认密度0，编辑器和SceneEditing支持修改、保存、重开。UE树冠深度前150行中位12.36米，旧主雾从18米开始，解释了提高主雾密度无效。项目正式场景在既有18米暖远雾上叠加从0米起的低密度冷色层(0.06,0.13,0.34)、密度.006，独立双UE视口全图MAE 23.534→21.196、28.245→25.488；树冠顶150行31.699→25.309、35.720→28.189。旧场景默认关闭回归仅11像素最大RGB8差4。并非遮挡感知体积雾，树冠与UE仍有明显造型/明度差异。
- 第二层雾正式Game/Editor 900帧构建运行PASS，i7-11800H/RTX3060 Laptop/DX12/RelWithDebInfo/1280×720、60预热后840帧：Game CPU中位/P95/max27.338/29.797/43.943ms、5帧>33.333，GPU最大11.906；Editor19.717/24.820/34.474ms、1帧>33.333，GPU最大11.733。严格稳定30FPS依然FAIL，CPU超时主要在update/animation/deformation或图录制短峰，非稳定GPU绘制瓶颈。`RenderWorldSubmission`与`SceneEditing`定向测试PASS。继续定位CPU峰值及真正体积散射，而非宣称画质/性能已过。
- 2026-09-25 渲染提取通用优化：延后视锥外对象构造；仅保留本帧提交对象的上一帧矩阵/骨骼历史，重新入镜时重置运动历史；静态无父节点网格缓存世界包围盒，并以网格指针及变形修订号失效。`RenderWorldSubmission`覆盖移动、换网格、变形、父节点和重新入镜；固定机位新旧图除33个粒子像素外无几何差异。正式Game 3600帧中3540测量帧CPU中位/P95/max25.377/27.065/40.465ms、2帧超33.333，GPU最大11.724；另一900帧Game仍有2帧超时，不能认定长期稳定30FPS达标。尝试按网格去重每帧修订号未测得收益，已撤回。
- 自动化帧数上限日志现在在该帧耗时采样后写出，避免终止日志计入游戏帧。修正后正式Editor 900帧中840测量帧CPU中位/P95/max18.751/23.089/29.681ms、0超时；Game同条件25.462/28.341/40.790ms、3超时。进一步Editor 3600帧3540测量帧19.219/22.812/35.330ms、3超时，GPU最大11.919；这些慢帧为CPU ImGui/渲染命令准备短峰，本次长测未复现旧GPU 50–115ms区间。严格稳定30FPS仍FAIL，后续需处理CPU短峰并复测多轮。
- 2026-09-25 纠正旧SceneCapture校准留下的过亮环境光：项目 `Build-SickaViewportAmbientAB.py` 从双层雾固定镜头生成街口/后退2米的0.45/0.55/0.65/0.9隔离场景，8个Game第90帧512² DX12捕获逐一比较UE编辑器视口。选0.55时街口整图RGB8 MAE21.196→17.622、道路区域16.338→10.472；后退机位整图25.488→23.023、道路区域22.994→17.955。两份并列预览已目视。正式 `SickaDynasty.he` 和生成脚本的IBLIntensity由0.9改0.55，重建仅1个Environment实体该字段变化，其余3567实体不变；Game正式90帧capture、Editor90帧check PASS。树冠巨型轮廓、逆光、GI/体积雾仍与UE明显不同，像素误差改善不是整体画质达标证据。
- 0.55正式场景走跑跳Game900帧和Editor900帧均构建运行PASS；i7-11800H/RTX3060 Laptop、DX12/RelWithDebInfo/1280×720、60帧预热后840测量帧，Game CPU wall中位/P95/max25.218/27.526/39.948ms、1帧>33.333，GPU最大11.600；Editor18.715/22.255/26.610ms、0帧超时，GPU最大11.826。Game严格稳定30FPS仍FAIL，Editor本轮短测通过但此前3600帧仍有CPU短峰。
- 在0.55环境光双层雾结果中，用已有UE/HE原生深度共有树冠遮罩复测前150行49860像素：UE视口RGB8均值(27.27,44.96,66.02)，HE(30.70,39.11,66.46)，但逐像素MAE仍21.344，HE绿色/蓝色标准差33.96/35.96高于UE25.54/24.68。树冠总体均色已接近，剩余差异主要是遮挡轮廓及空间细节/逆光分布；不再仅靠统一染蓝或降低整场环境光修树。
- 2026-09-25 严格长测定位电源状态异常：0.55正式场景Game1800帧启用`HE_RUNTIME_SYSTEM_TIMINGS=2`、`HE_LAYER_UPDATE_TIMINGS=1`，1740测量帧CPU wall中位/P95/max25.198/27.221/30.970ms、0超时，GPU最大11.624；移动上一帧变换查询到视锥剔除后`RenderWorldSubmission` PASS，但另一次同条件1800帧仍1帧35.411ms、无可测稳定收益。3600帧同步`nvidia-smi`复现帧3517–3600共84帧GPU跨度>33.333、最大116.872ms，CPU最大118.898ms；可见三角形恒1065065、可见物恒229。该区间GPU显存时钟7001→810MHz、图形时钟1980→最低210MHz、利用率达100%，Windows Kernel-Power 521于05:43:45/51记录ValidBatteryCount 1→0→1（约6秒）。更早四次相同型态GPU短峰也均与对应的电池计数0事件重合。只能认定时间关联，不能据此确定硬件接口、固件或驱动具体原因；外部状态异常未解决，严格30FPS仍FAIL。CPU帧3138–3139多个系统同步增时，与GPU降频区间分离，仍需独立优化/查证。
- 新增只读`Tools/Agent/Compare-RenderStatsPowerEvents.ps1`，按RenderStats逐帧UTC与Windows Kernel-Power 521事件生成慢GPU区间及邻近电池计数时间线；有异常与无事件两个真实CSV均运行PASS。该工具不修改电源策略，也不会把相关性写成因果结论。
- 2026-09-25 落叶色相复核：UE `T_TreeBranch2_BaseColor` 为绿色叶片贴图，1/4缩图透明遮罩内约61.8%像素为绿叶特征，叶样本中位RGB(100,124,74)；完整不透明区域中位(102,117,79)。正式树材质再乘(1,.702458,.233331)成为暖橄榄色。项目生成包记录暖落叶RGB(73,76,26)、纯绿RGB(56,63,43)；正式`SickaDynasty.he`的21个发射器全引用暖落叶包，纯绿包仅供绿色树变体。已有近景截图目视落叶为暗橄榄色且无白面片；未发现本轮需修改的配色或绑定错误。
- UE静态光照迁移调查：只读`Tools/UE/InspectLightmapAccess.py`已参数化场景、BuiltData资源和样本名。`MAP_Demo_BuiltData`中可枚举495个`LightMapTexture2D`子对象，Python能读取HQ样本的路径与32×32尺寸，但`MapBuildDataRegistry`的网格映射/UV比例偏移未暴露。尝试UE标准`TextureExporterEXR`导出1个样本触发UE 5.4内部`SupportsTexture(Texture)`断言，仅代理启动的`UnrealEditor-Cmd`退出；原UE资产时间戳未变、用户编辑器仍运行。工具已移除不安全导出路径；GI仍未迁入，需专用UE原生导出器成对导出贴图与网格映射，不把现有近似当作GI完成。
- 2026-09-25 树冠UV根因诊断：外部项目新增三套隔离同机位场景，对原始树叶图进行原样BC7、离线垂直翻转BC7、实例仿射UV垂直翻转。原样相对UE视口RGB8整图/顶部200行MAE为22.336/26.126；离线翻转为24.998/25.467，整图退步，因此正式树材质不翻转。诊断同时发现旧`.hmat`以属性名作`StableId`时，通用材质加载器丢弃当前manifest的纹理采样；已仅对这种旧格式迁移，并让标准语义自定义shader按实例`Sampling`使用仿射UV。修复后仿射诊断与离线翻转平均差0.146 RGB8，验证Game真实执行了该路径。最终复核正式21处全部为暖叶效果包，包内当前RGB(73,70,26)，绿色包(56,63,43)留给真实绿树。此修复可复用于其他角色/场景；并未解决UE树冠与HE剩余轮廓和光照差异。
- 2026-09-25 双机位静态光图/太阳直射隔离：项目 `Build-SickaBakedDirectAB.py` 和 `Compare-SickaBakedDirectAB.py` 对2112个光图绑定建立基线(光图0.2/太阳4)、半烘焙半太阳(0.5/2)、全烘焙无太阳(1/0)两机位六场景。后退机位UE视口整图RGB8 MAE依次23.071/22.305/26.311，前进机位17.722/18.794/23.797；半烘焙仅单机位获益，全烘焙两者退步，故不改正式关卡，也不把直射重复叠加作为已证实根因。UE只读报告中Sun和SkyLight为Stationary。道路代表图集的UE原始BGRA8与BC7回读原向平均绝对通道差3.534/255、纵翻82.344/255，排除图集整体上下颠倒；仍需分离UE烘焙间接、屏幕反射、局部光与后处理贡献。
- Shipping项目入口核对：`MechanicGirlLab/Config/Game.json` 原先默认旧展台且Cook.RootFiles未包含正式SICKA关卡，这会使新场景在打包后缺失。现将默认入口设为 `SickaDynasty.he`，把它加入cook根，同时保留`UEFabShowcase.he`与`UEFabDemo.he`。最终独立Release `hectl package -Project C:\WorkSpace\HEngineProjects\MechanicGirlLab` PASS，已发布 `dist/projects/7e44ffe2e91e72bd/Gamee4fb9eeb`；Game.pak内2439文件、1,514,026,534字节，Unicode深路径解包、策略扫描、DX12 WARP两帧启动PASS。
- 打包真实启动暴露两处通用长路径缺陷：导入资源在包内存在但普通Windows文件路径读不到；初次修复后扩展路径与规范化内容根比较不一致。现让包内容根与安全校验后的资源文件读写使用扩展路径，并在内容根反查中统一路径形式；`Test-PackagedGame.ps1`和`Verify-PackagePolicy.ps1`也可完整检查深层哈希文件，不静默跳过。磁盘满时包脚本的证据复制失败不再将已知失败状态误记为unknown。早期失败记录的实际进程退出/失败已核实并经存储工具正式状态接口修复，旧Cook输入已受控回收。此结果证明包内场景启动和短帧运行，不证明最终画面已达AAA或全程稳定30FPS。
- 2026-09-25 树冠/落叶颜色再核验：正式场景21个落叶发射器全部绑定最近树的`MI_TreeBranch2`并使用暖褐包；该树叶原图偏绿，但材质底色因子(1,.702458,.233331)使实际底色转黄褐，暖包基色RGB8(73,70,26)，绿色包只留给绿叶材质。UE编辑器视口的逐Actor隐藏实验确认`SM_Tree_4`贡献主镜头顶部树冠；HE固定镜头树冠遮罩相对UE交并比0.844，未整树漏绘。仅关掉该树透光时重叠树冠区域红通道均值降低1.575/255，不能解释主要色差，正式树材质未改。UE图像、HE图像及可复现实验均保存在外部项目`Saved/Diagnostics`和`Tools/Build-SickaTree*`，原UE/Fab资产未修改。
- 2026-09-25 主树冠遮罩A/B进一步定位：HE引擎`SceneRendererLod.inl`对Mask材质必选LOD0，导入`SM_Tree_4`也没有authored index LOD，排除HE误选低LOD。固定UE视口512²与HE DX12第90帧对照：只将`SM_Tree_4`叶材cutoff从0.1变0，顶部180行Actor差分遮罩IoU由0.8436升至0.8756，整图RGB8 MAE由(20.131,16.975,16.064)降至(17.985,16.897,15.361)；69树全改0时MAE回升至(19.025,17.555,15.305)，左冠过暗。UE临时网格材质替换虽API报告成功，但截图主树区域与基线均差不足1 RGB8，4秒延迟的异步截图未生成PNG，不能用该尝试断定UE实际alpha路径。正式树材质与UE/Fab资产均未改，后续需按树遮罩、光照与视角分离评估。
- 2026-09-25 `SM_Tree_4`跨机位视觉校准进入正式项目：UE街道x=-10m与x=-12m两个512²固定视口中，仅该树cutoff由0.1降至0，HE相对UE整图MAE RGB8分别(20.131,16.975,16.064)→(17.985,16.897,15.361)和(27.448,22.076,19.690)→(24.359,19.349,16.168)；全69树同改反而使左冠过暗。项目`Tools/SickaFoliageCalibration.json`声明单树配置，`sicka_foliage_materials.py`生成独立`MI_TreeBranch2DenseCanopy.hmat`，`Build-SickaDynastyScene.py`按Actor仅覆盖1棵，重建前后正式场景与新材质SHA256不变；语义diff仅`SM_Tree_4`材质引用1处。其余68树保留原叶材、21落叶发射器保留暖包，UE/Fab源资产及通用引擎遮罩逻辑未改。正式漫游同帧对照显示高覆盖使地面阴影明显加密，亮橄榄落叶在变更前后均存在，须另行对色。
- i7-11800H/RTX3060 Laptop，DX12/RelWithDebInfo/1280×720，正式场景900帧回放预热60后840帧：Game高覆盖首测CPU中位/P95/max25.849/28.470/40.102ms、4帧>33.333；重复测中位/P95/max25.888/28.390/35.629ms、2帧>33.333；GPU两轮最大11.283/11.151ms。编辑器900帧预热60后CPU中位/P95/max20.350/23.819/27.508ms、0帧超时，GPU最大11.274ms。Game严格稳定30FPS仍FAIL，不能把短测编辑器PASS扩展为整体达标。对照旧场景同机位Game840帧CPU最大33.137、0超时；可见/阴影绘制计数在慢帧相同，尖峰归因尚未完成。
- 2026-09-25 落叶近景外观与引擎脚本API：正式900帧回放截图中暖落叶约0.48m显得过大且相对树冠偏亮。引擎`VfxPlayback.Play`新增可选RGBA全局颜色，平台VFX服务校验4个有限[0,1]分量、与既有opacity相乘、对不支持全局颜色的效果显式报错并停止新句柄；未传颜色的现有特效保持原行为。项目`SickaLeavesWarm.cs`用可保存的`[ScriptField("tint", Color=true)] Vector4`默认(.55,.52,.75,1)，`SickaLeafAppearance.json`将叶片尺寸从.48m改为.24m，Fab/UE原图与树材质不变。相同第900帧两片叶色由RGB8(67,70,7)/(73,75,8)降为(38,37,5)/(41,41,6)；缩小后近景叶片不再大幅遮挡角色。两效果包二次重建SHA稳定且ZIP CRC通过。该方法是可编辑的未受光粒子调色，并非实现了实时受光/雾。
- 最终颜色/尺寸组合的900帧Game回放后840测量帧CPU中位/P95/max25.952/28.707/31.638ms、GPU最大11.061ms，均0帧超33.333。Editor首轮900帧有94慢GPU帧、GPU最大111.870ms、CPU96帧超时；可见物/三角/阴影绘制不变、CPU frame-slot wait跟随GPU阻塞，Kernel-Power电池数事件0；同场景复测840帧CPU中位/P95/max20.120/23.564/29.233ms、GPU最大11.631ms、0帧超时。不能仅凭复测把前一轮失败剔除或宣称长期稳定30FPS，GPU突发原因仍待定位。
2026-09-25 落叶运行时调色：`VfxPlayback.Update` 现可选传入 RGBA，沿已有 `vfx.ownedUpdate` 服务即时更新 Effekseer 全局色；未传颜色的现有脚本行为不变。MechanicGirlLab 的 `SickaFallingLeavesWarm` 仅在 Inspector 色值改变时发送新颜色，新发射周期也沿用当前色值。Game/Editor 同项目 DX12 各90帧 `check` 构建与运行 PASS。完整脚本持久化单测尚未完成：首次 Debug 测试构建令 C 盘空间不足，已停止并记录。
2026-09-25 清理器长路径修复：空间不足时发现`hectl clean cache`递归枚举了深层`.obj`，但对超过260字符的文件调用普通Windows路径`exists()`误判不存在，导致WinError145非空目录。`Tools/Agent/storage.py`的安全边界仍核实规范化原路径，实际`lstat`/枚举/删除与目录守护句柄改用Windows扩展长度路径；新增长路径缓存契约测试。`py -3 Tools/Agent/Tests/test_storage.py` 25项PASS，后续UNC处理定向2项PASS。旧已退休缓存未强制移除，仍需通过受保护流程回收，C盘空间和脚本字段保存/重开验收仍待解决。
2026-09-25 SICKA 1800帧长采样：Editor静态视角、60帧预热后1740帧CPU中位/P95/max20.422/50.476/113.361ms，116帧超过33.333；GPU中位/P95/max10.871/51.139/109.884ms，连续113帧慢区间在774—886帧。Windows同时间电池数量事件1→0→1，仅时间相关，不能证明驱动/电源根因。Game可重复走跑跳回放1740测量帧CPU中位/P95/max26.088/28.201/45.426ms，3帧超时；GPU最大11.195ms、0超时。Game第288—290帧对象/形变顶点数基本不变，CPU更新与渲染同时变慢，未定位因果。Game/Editor均未达到严格稳定30FPS门槛。
2026-09-25 Editor落叶字段保存/重开验收暴露并修复通用相机作者数据污染：编辑器初次载入场景时把当前视口宽高比序列化进未显式配置`mAspectRatio`的CameraComponent，另一视口布局再打开同场景会将例如1.7778改写为2.8328。`SceneEditingService::Bind/Observe`及Editor逐实体观察现在保留磁盘上“该字段缺席”的状态，仍让运行时SceneCamera按视口更新投影。项目`Tools/Validate-SickaLeafTintRoundtrip.py`在独立临时场景通过Editor mailbox执行落叶vec4修改、保存、重新打开、查询；与未经修改的Editor规范化对照保存相比仅一个ScriptComponent.Fields不同，两份保存均不含视口mAspectRatio，原场景SHA不变。脚本测试PASS，Game/Editor build/run PASS。新增SceneEditing单测断言，但Debug测试目标因C盘空间再度不足主动中止，尚未运行；`hectl clean cache`随后完整回收2,683,105,674逻辑字节测试缓存。
2026-09-25 落叶受光基础：UE只读检查为`M_Particle_Leaves` Masked/Default Lit，HE先前暖落叶是Effekseer未受光节点。引擎现将RenderWorld首个方向光方向/颜色及限幅IBL环境底色经SceneRenderer→VFX RenderGraph→EffekseerRenderer传入SDK；项目外观JSON选择`lightingMode=directional`，生成器给绿/暖节点编译Material=7。双次构建包SHA稳定，ZIP CRC通过，原UE/Fab资产未改。同回放900帧Game正式场景在两片近景叶上的RGB8由(40,39,6)/(37,37,5)变成(11,9,1)/(9,8,1)；仅改Sun为绿色的诊断场景相应为(3,11,0)/(2,10,0)，证实场景光驱动。该SDK材质仍是alpha-blend粒子、无HE阴影接收与场景雾，不能称为UE Masked/Default Lit等效。Game/Editor DX12各900帧build/run通过，但预热后840帧CPU分别5/1帧超过33.333ms，稳定30FPS仍未完成。

- 2026-09-25 修复引擎VFX光照方向约定：场景`DirectionalLight.Direction`为光线传播方向，Effekseer Lit shader与法线点乘要求指向光源；`SceneRenderer`传递取反后的方向。Game同回放第900帧capture PASS；原暖叶近处样本RGB8(11,9,1)/(9,8,1)在方向修复后为(3,3,0)/(2,3,0)，暴露项目叶片过暗及旧脚本蓝色乘色。项目暖叶默认RGBA改为白色，独立资源校准为更小的0.10米发片、每10帧发射、暖树材质推导的棕色底色（生成包基色约(162,109,58)），保留独立绿色变体与Inspector调色。最终900帧图`Saved/Diagnostics/SickaWarmLeavesBrownReplay900.png`目视为小棕叶，未达到UE Masked/Default Lit的阴影、体积雾或二面受光质量。
- 最终Game第900帧capture与Editor120帧check均build/run PASS；暖叶包二次重建SHA256相同。i7-11800H/RTX3060 Laptop、RelWithDebInfo/DX12/1280×720，Game 840测量帧CPU中位/P95/max26.792/54.101/137.438ms、67帧>33.333；GPU最大104.182ms、65帧超时。GPU慢区间帧835–900附近Kernel-Power 521记录有效电池数0，仅能报告时间相关性；其前772测量帧CPU仍3帧超时。稳定30FPS FAIL，仍需解决外部电源/驱动状态异常和独立CPU短峰。

- 2026-09-25 街道树冠双机位校准：沿用项目可编辑的`SickaFoliageCalibration.json`与通用叶材，隔离测试69树全部cutoff=0和世界Z>=0侧48树cutoff=0。UE编辑器两个固定512²视口对照中，只校准右侧让前机位整图RGB8 MAE17.723→16.760、后机位23.072→19.967；后退机位道路18.613→13.006，而全部69树加密使左冠25.639→32.604恶化。正式配置列出48棵树，生成场景3568实体仅新增47处材质引用变化，二次重建SHA一致；导入几何/布局不变。该画面仍非UE完全一致：前机位左树冠误差21.930→23.847，后退机位上部28.302→28.476，逆光/GI/体积雾仍待处理。
- 正式Game走跑跳900帧capture、Editor静态视角900帧check均build/run PASS；i7-11800H+RTX3060 Laptop、RelWithDebInfo、DX12、1280×720、60帧预热后各840测量帧，Game CPU中位/P95/max26.595/29.136/38.980ms、3帧>33.333，GPU最大11.096；Editor20.122/23.727/29.857ms、0超时，GPU最大11.746。两轮0慢GPU帧/0电池数量事件；Game严格稳定30FPS仍FAIL。`Validate-SickaTreeImport.py`69放置/4个LOD0几何PASS，项目Foliage/旧mip/全树诊断生成脚本PASS。
- 左侧树冠中间阈值隔离实验：项目诊断叶材cutoff=.05用于左侧21树、右侧48树保持0；两机位Game第90帧capture PASS。前/后整图MAE相对当前右侧方案16.760→16.730、19.967→19.981，后视左冠25.643→25.780；收益不足且一侧退化，因此未改正式配置。固定机位视觉仍需比简单统一AlphaCutoff更完整的材质/光照方案。

- 2026-09-25 右侧树冠后继续定位左侧颜色：后退机位顶部左区域，把21棵左树全设cutoff=0所改变的14702像素中12373像素相对UE误差变大；UE/HE当前/全覆盖均色分别RGB8(93.2,84.3,92.8)/(66.0,75.9,105.3)/(24.9,35.0,64.9)。根因主要是逆光散射色温/亮度，继续加密不能修复。UE只读`fog-settings.json`证实体积雾开启，暖色散射albedo(255,215,168)、偏蓝emissive及phase参数0.5。项目仅调整已有近气雾层`SecondaryFogColor`从(.06,.13,.34)至(.30,.28,.34)，未宣称实现真正体积雾。前/后双固定UE视口全图RGB8 MAE在右侧树冠正式配置上16.760→15.583、19.967→16.857；后视左冠25.643→19.370、道路13.006→10.340；前视右冠19.700→21.175退化仍在。正式场景3568实体仅Environment一个字段变化，二次重建SHA256一致。
- 暖雾正式Game走跑跳900帧capture与Editor900帧check均build/run PASS；Editor首轮840测量帧95帧GPU>33.333、最大109.560ms，与Windows Kernel-Power 521电池数量1→0→1事件相邻；同内容复测Editor GPU最大11.625ms、CPU中位/P95/max20.282/23.573/30.091ms、0帧超时且0电池事件。Game CPU26.779/29.502/46.750ms、6帧超时，GPU最大11.229ms、无慢GPU/电池事件。只报告相关性，不把电源事件断言为根因；Game严格稳定30FPS仍FAIL。
- 2026-09-26 MechanicGirl 头发优先级提高：引擎 `SecondaryMotion` 针对单帧超过100°的骨架方向跳变清空世界空间发梢历史，避免原地瞬转/演出切镜沿用旧速度；补了同轴心180°旋转回归断言。项目专用 `MechanicGirlHair.py` 将17根短发/两侧/后发链按根到梢分级调节刚度、阻尼、角度和重力；49名正式场景演员使用一致配置。固定机位 walk-in-place 第45帧 A/B 中左右发束分别有12421/14247像素RGB差>10、脸部ROI仅114像素、背景0；过度统一放宽角度造成发束外翻的候选已弃用。正式 SICKA Game300 帧走跑跳回放预热60后CPU最大32.974ms、GPU最大12.486ms、0帧>33.333；近景发片纹理/层次和UE动态完全对齐仍未证明，口腔按功能性验收。引擎单测及Editor本轮验证状态见验收记录。
- 同头发配置Editor正式300帧冷构建/运行PASS、场景`dirty=false`，预热后CPU/GPU最大27.737/12.157ms、0超时；Game正式900帧回放build/run PASS但840测量帧CPU/GPU各15/13帧超33.333，末13帧慢GPU与电池计数0事件同秒，第476/884帧另有CPU单独超时。故短测性能PASS、长期稳定30FPS仍FAIL；不能把GPU事件的时间相关性写成硬件根因。
- 2026-09-26 从当前`SickaDynasty.he`重新生成UE前/后固定比较机位（项目`Build-SickaCurrentStreetCompare.py`，二次生成SHA不变），两机位Game DX12/512²/90帧capture build/run PASS。HE当前树冠已呈独立叶片、旧巨型矩形消失；UE参考仍有块状异常，未追随。道路ROI RGB8 MAE前/后8.67/14.63，中景23.38/20.40；全图20.56/22.00受UE树冠异常主导。两镜头视觉没有新的遮罩或后处理大块错误，路面偏暗蓝与GI/体积雾差异仍在。Back第120帧也捕获PASS，左/右树冠第90→120帧各11.9%/6.5%像素变化>10，但该差分混有树影/粒子/TAA，不是完整风动证明。
- 2026-09-26 当前SICKA正式场景后处理按UE两固定机位校准：项目`Build-SickaCurrentGradeAB.py`对旧/轻/强候选做DX12 512²第90帧A/B，前/后道路ROI RGB8 MAE由8.672/14.628降到强候选7.212/13.254，中景23.382/20.403→22.700/19.704；UE异常树冠导致前视全图误差略升，不作为对齐目标。正式仅改Environment.PostProcessing的Contrast1.25→1.16、Temperature-.35→-.20、蓝ColorOffset.0025→.001，3568实体ID不变且二次重建SHA相同；可操控300帧候选目视无人物/灯光大块回归，正式Game300帧build/run PASS，240帧CPU最大28.365ms、GPU最大12.248ms、0超33.333。Editor本轮状态见验收记录。引擎通用PP无场景硬编码。
- 最终后处理校准后，正式Game/Editor各900帧DX12/1280×720、预热60帧的同配置复测均build/run PASS，840统计帧分别CPU最大28.908/32.014ms、GPU最大11.843/12.104ms，0帧>33.333；项目场景`dirty=false`。不能忽略首轮Game900的29个CPU慢帧（最大48.532ms、GPU正常、0电池事件）和Editor冷构建后300帧的4个CPU超时（最大43.327ms）；同回放帧复测未重现，根因未定。按用户宽松口径记录正常复测可达到30FPS，不宣称所有运行逐帧稳定。

### 2026-09-26 发束碰撞速度稳定性

- 依照角色优先级，继续处理头发而不是扩大口腔美术范围。通用 `SecondaryMotion` 将碰撞体对发梢的位置修正与自由速度分离，碰撞时移除朝向碰撞体的速度，避免接触约束成为下一帧的弹射冲量。项目发型配置和 UE/Fab 原始资源未改。
- `AnimationPoseContract` 新增碰撞体移开后的发梢稳定断言；近景 `HairMotionTuned.he` 以 Game DX12 90 帧 capture 构建、运行并目视通过。当前截图仍有可见发片边缘锯齿和发束质感差异，不能据此宣称 UE 头发自然度已经完全对齐。
- 正式 SICKA 走跑跳 Game300 与 Editor300 均 build/run PASS；各自预热60帧后240帧 CPU 最大29.46/25.25ms、GPU最大11.96/12.15ms、0帧超过33.333ms，Editor场景`dirty=false`。仍保留前述900帧首轮CPU慢段为未解释限制。
- UE 5.4 只读检查两套发型 PhysicsAsset 可取得松发3条、短发2条约束；Python接口不暴露骨体和约束剖面，且仅有资产并不能证明演示 AnimBP 启用它们。因此不把这两个数量直接映射成HE弹簧参数，UE同动作自然度仍待可靠参考。
- 静态沿链复核 `RenderWorldExtractor` 已为蒙皮对象生成本帧/前帧骨骼调色板，`SceneRenderer` 将前帧调色板送入光栅运动信息，`SceneTaa` 按运动、深度、法线和表面ID拒绝失效历史；未发现“发片完全没有骨骼运动矢量”的缺口。发丝细边的动态闪烁仍需逐帧画面验证，不因静态链路审查宣称通过。
- 近景仅把发片覆盖MipBias从UE导入的-1改为0的独立诊断，在第45帧头发ROI RGB8 MAE约(2.76,2.09,1.80)、背景0；目视变化轻微，不能证明动态锯齿改善，因此正式UE匹配材质继续保留-1，诊断脚本可复现。
- 本次头发约束修正后正式Game 900帧同走跑跳回放 build/run PASS；预热60后840帧 CPU中位/P95/max21.91/24.37/29.80ms、GPU10.81/11.47/11.93ms，0帧超33.333。与之前同内容首轮出现CPU慢段的事实并存，连续多次/跨设备稳定性仍未证明。

### 2026-09-26 发片遮罩阈值与通用材质契约

- UE 5.4 只读`M_HAIR.opacity_mask_clip_value`为0.3333，项目原五款发色和模板为0.48。近景同帧A/B在头发ROI RGB8均差(3.31,2.55,2.30)，背景无变化，0.3333发束覆盖较完整且未出现整卡矩形；已统一项目模板/五材质、生成器和BC7/mip记录，UE/E原资源未写。MipBias=-1保持，单帧不能证明抗闪烁已完成。
- V2材质顶层`AlphaCutoff`会覆盖同名typed参数。隔离诊断发现只改typed值画面不变；引擎加载现在对二者不一致直接报错，`MaterialSerialization`单测PASS。正式所有材质字段对齐，Game/Editor均启动通过。
- 材质修正后的SICKA Game900和Editor300均build/run PASS；预热60后Game840帧CPU最大27.29ms、GPU最大12.04ms，Editor240帧CPU最大25.21ms、GPU最大12.06ms，均0帧>33.333；多角色衣装展台90帧截图无整卡异常。既有偶发CPU慢段与UE动态发片精细度仍是限制。
- 进一步只读检查UE `M_HAIR.MP_WORLD_POSITION_OFFSET`无连接，尽管材质实例有`WindIntensity/WindSpeed`值；不能把未使用的参数当作UE发片逐顶点风动证据，暂不在通用HairCards着色器里臆造额外顶点摆动。

### 2026-09-26 落叶前景层次

- 现有21个UE坐标落叶发射器在代表性镜头下主要位于远景，静止截图仅形成深色小点。外部项目新增 `Tools/SickaLeafStaging.json` 的2处可编辑前景发射器，生成器按附近实际HE树叶材质选暖/绿包，保留原UE映射和稳定UUID；原始树叶、UE材质与项目效果包均未修改。
- 同一诊断镜头开关A/B确认前景叶片轮廓可辨，褐色与树冠相符且无白方片/大矩形；正式Game 300帧走跑跳回放和Editor 120帧均build/run PASS。短测预热后Game CPU最大28.672ms、GPU最大12.151ms，0帧超过33.333ms。落叶实时受光/雾及长时稳定性仍未等效UE；发片跨帧稳定性保持优先级，不以单帧截图宣称已通过。
- 新增布置后的900帧Game首次运行第860–900帧出现GPU连续慢段，41帧GPU超过33.333ms，峰值111.342ms；CPU在这些帧主要等待GPU帧槽，可见物/阴影绘制数未增加，同期有电池设备事件但根因未证明。随后Editor900的840测量帧CPU/GPU最大29.500/12.039ms、0超时；相同Game900复测GPU最大12.345ms、0超时，CPU有第110–112帧3次孤立慢帧（最大38.762ms），同帧首轮约24ms。宽松30FPS口径有正常运行证据，严格每次逐帧稳定仍不能宣称通过。
- 前景布置解析对无效名称/坐标报错；绿色诊断改为只要求21个原UE发射器存在，不假设所有新增布置一定为暖叶。场景重建SHA不变，Python静态检查PASS。头发第45/46相邻帧目视无整卡跳变或明显重影，但细边仍有锯齿，未做运动补偿，UE动态对照未完成。

### 2026-09-26 发片TAA历史诊断与有限修正

- 关掉角色动画和发骨力后，静止头发第45/46帧仍有像素变化；8帧静止序列中头顶ROI的平均相邻帧RGB8差为(6.075,4.514,4.074)。原始surface-id RG32_UINT捕获显示92000像素头顶ROI中34771像素相邻帧同对象但图元编号变化，符合多发片交叠/次像素采样导致的历史拒绝，不是单纯骨链摆动。
- 通用 `HairCards` GBuffer在SurfaceId图元编号最高位标记发片。`SceneTaa`历史采样可在同对象、双方均为发片时放宽图元编号；深度/法线检查与当前邻域的精确图元比较保持。无项目路径/角色名分支。候选8帧静止头顶差为(5.788,4.306,3.898)，约4–5%较低；两次Agent会话TAA相位不同，且脸部ROI也有约4%波动，故此数只作有限方向证据，不作确定的整体画质增益。动态第45/46帧差基本不变，未解决细发轮廓锯齿。
- SICKA正式Game300走跑跳预热后240帧CPU/GPU最大27.175/12.004ms、0超33.333；Editor120及五款发色展台90帧build/run PASS且展台目视无整卡。外部AsterCourtyard独立缓存首次构建（550.3s）与90帧Game运行PASS。项目新增可重复的 `Build-HairStaticTemporalDiagnostic.py`，不修改原UE/E素材。

### 2026-09-26 UE发片表面参数同步

- 对照只读UE `M_HAIR` 图和五个展示实例，发现HE展示发片沿用法线强度1.0、粗糙度0.4；UE母材质法线强度默认0.5、`RoughnessMin`输出0.33，03实例单独覆盖法线强度0.2。项目的材质生成脚本现从检查报告及实例继承值读取这些参数；正式五款压缩材质同步更新，46张BC7贴图全部复用。原UE/E资产未修改。引擎`HairCards`已有可调参数，不需要为此加角色专用代码。
- 固定近景第45帧Game捕获PASS，头发区域旧/新RGB8平均绝对差约(3.26,3.26,3.46)，背景为0；目视高光更集中，无新增整卡。正式SICKA Game120预热60后CPU/GPU最大33.616/11.995ms，CPU有1帧超过33.333ms，不能据此宣称逐帧稳定30FPS。Editor展台60帧build/run PASS。发片边缘锯齿、物理自然度与UE动态逐帧对照仍未完成；口腔验收按用户要求保持较宽松，头发优先。
- 通用`SecondaryMotion`原先将所有低于15 FPS的帧截断为1/15秒模拟，造成慢帧后发束反应延迟；现在普通低帧率完整积分到0.25秒，超过0.25秒视为暂停并从当前姿势重置。新增10/120 Hz瞬态对照断言，`AnimationPoseContract`无窗口单测PASS；Game近景第90帧build/run/capture PASS并目视无发束爆开。常规性能目标未因低帧率模拟路径变化而另行推断。
- 引擎修改后的Editor展台60帧build/run PASS。展示材质再次生成、压缩绑定后22份`.hmat`聚合SHA256保持`840f42f4209a8837e8a4dea5d38a45146d9d0fdd45dcfa008fed5ac98eac66d3`，确认重导入步骤不会抹掉发片参数。

### 2026-09-26 佛像交互演出与头发优先级

- 引擎增加通用 C# `SceneSequence.Play/Stop/State`，项目脚本按距离和交互输入触发 `SickaShrineInspection`；可编辑 JSON 定义镜头、表情曲线、音频和事件。正式场景现为3572实体，构建脚本重复输出 SHA256 `4B07860EC921CC581314C99D5948902FE1A47AA0D4180E193B699257A185E395`；UE/E原件未修改。第150帧前脸与佛像同框，第306帧镜头和操控返回游戏，截图已目视。音频/事件时序尚未独立采集，不能仅凭镜头认定同步验收完成。
- 通用渲染器为最近使用的材质贴图保留300帧并设256MiB闲置载荷上限，避免镜头返回时每帧剪掉缓存。正式双次交互回放在i7-11800H/RTX3060 Laptop、DX12/RelWithDebInfo/1280×720下，第二次演出192测量帧CPU中位/P95/max26.71/28.78/30.64ms、0帧超33.333；第一次192帧仍有16个CPU慢帧、最大76.28ms，GPU两次均0慢帧。首次切镜头的同步资源加载仍需解决，不把重复播放通过写成首次也稳定。
- 正式镜头远裁面最终为120m，40m诊断版虽减少可见物却切断远景树和建筑，已撤销。Editor120静态场景预热后CPU/GPU最大25.731/12.041ms、0慢帧；AsterCourtyard Game90与基础 UEFabDemo Game90跨场景运行PASS。头发继续高于口腔：优先急转/低帧率自然度、碰撞与细边跨帧UE/HE对照；口腔只要求曲线可驱动及近景无明显穿帮。现有发片近景仍有细边锯齿，未达到最终AAA人物验收。

### 2026-09-26 发片当前帧TAA邻域

- 8帧静止近景新基线头发ROI相邻帧平均RGB8差(6.225,5.006,4.862)，背景约0；当前帧颜色裁剪邻域原先要求相同图元编号，与已有发片历史匹配规则不一致。通用 `SceneTaa` 现仅对同对象且双方为HairCards时允许不同图元进入当前邻域，仍检查深度/法线；普通物体保持精确图元匹配。
- 独立会话8帧候选头发ROI差(6.057,4.866,4.718)，脸ROI也有小幅会话差异，故不能将约3%变化归因或视为严格性能/画质增益。两图逐张预览没有新增整卡或明显拖影，动态第46帧仍有细边颗粒；UE连续同动作对照仍缺失。
- 改动后正式SICKA Game300预热60的240帧CPU中位/P95/max23.756/25.629/27.113ms、GPU11.161/11.745/12.227ms，0帧超33.333；Editor120运行PASS。头发自然度、边缘锯齿和首次镜头资源卡顿继续保留为未完成。

### 2026-09-26 首次演出镜头的资源卡顿定位

- 双次演出逐帧对照：首次第62帧CPU约45.5ms，其中图录制13.46ms，第二次同相位第272帧图录制4.96ms；首次第75帧CPU76.28ms、帧准备37.13ms，第二次第285帧帧准备2.50ms。GPU两次同相位约10–11ms。首播慢帧不全是同一原因，不能只调整纹理等待。
- 试去掉DDS上传后的逐纹理`waitForIdle()`，首次第75帧反而96.70ms、首段15帧超33.333，未见收益；已撤销。引擎保留默认关闭的`HE_TEXTURE_UPLOAD_TIMINGS=1`分段日志，只在DDS单张总耗时≥2ms时记录读取/创建/提交/等待。诊断运行中第75帧新可见3张约5.6MiB纹理的读取为84/92/70ms，等待各<1ms；大量日志与冷盘竞争使绝对耗时失真，但证实主要卡在读取阶段。无诊断正常路径第75帧帧准备34.32ms、CPU72.57ms，14个首播慢帧。
- `DdsTextureData::Load`把已校验的文件缓冲移入结果，避免加载后再复制完整DDS；公共`Parse(span)`及512MiB上限不变，`DdsTextureData`无窗口单测PASS。改动后无诊断Game110 build/run PASS，但第75帧CPU81.68ms、帧准备42.79ms、16个首播慢帧，样本无可证实帧率增益。真正的后续修复是有界异步预读镜头即将使用的纹理并处理场景切换/取消，不降低画质或缩短120m远裁面。

### 2026-09-26 演出纹理预读

- 引擎通用`AssetReadAhead`提供单后台线程和96MiB场景预算（单文件32MiB），按场景代数清理待办/缓存；`SequenceComponent.PrefetchTextures`通过反射保存，Sequence开始时校验项目内DDS并排队，DDS加载优先消费已读字节，未命中仍走原读取路径。Cooker把该字段登记为硬依赖，确保发布包不会遗漏。关闭时先停止并等待工作线程，避免队列析构竞态。SICKA项目列出首次佛像镜头新见的三张DDS；其他角色/场景可按同一字段复用。
- `DdsTextureData`和`SequencePlayback`无窗口测试、Cooker自测通过；正式场景脚本二次生成SHA256`7A34FB6BE4330F25F116677CF8172D01543385884077388451D1CA04536B4070`。Game110日志`queued=3 hits=3 misses=0`证实实际消费；Game500首次192帧CPU中位/P95/max26.82/37.07/53.26ms、14帧超过33.333，第二次为26.80/28.65/31.41ms、0慢帧。旧基线首次峰值76.28ms、16慢帧，单次A/B不能把全部差异归因；首次稳定30FPS仍未达标，第62帧图录制及其他CPU峰值继续定位。Editor120静态场景运行PASS、`dirty=false`，预热后CPU最大25.443ms、0慢帧。打包验证结果见验收记录。
- 首播第76–91帧超时集中在`cpuRuntimeRenderMs`约24–25ms，对应重播第286–301帧约13–14ms；`cpuGraphRecordMs`与GPU帧时基本相同，差异落在尚未细分的`cpuRenderOtherMs`。第78帧CPU线程忙时46.875ms、全进程93.75ms，重播第288帧分别31.25/31.25ms，提示同期有额外CPU工作；当前不足以断言是PSO编译或异步资源任务。后续应将`RuntimeRenderer`图外阶段细分采样，再决定通用预热边界，避免盲目关同步。

### 2026-09-26 首播图重建原因

- 引擎帧统计新增`cpuGraphBuildMs`与`graphBuildReasonMask`，记录重建耗时和结构性触发源；原因位在`RenderPipeline.h`统一命名，Game/Editor共享。SICKA正式佛像首播第62–91帧的14个慢帧均有图重建，每帧约9.5–10.5ms。原因中6帧为新网格进入缓存（1024）、6帧为上帧首次上传后重建（2049），第75/76帧同时有网格和纹理表变化（3329/3073）。局部阴影、后处理、反射、VFX均未在这些帧提出重建请求。诊断从“图外CPU耗时”收敛到新可见网格生命周期；尚未实现GPU网格预热，首播仍14慢帧。通用解决方向是让将被演出镜头露出的静态资源在预热期进入正常GPU资源生命周期，避免针对单场景画质降级。
- 最终Game110回放build/run PASS，预热后的50帧仍14帧>33.333ms、最大54.70ms；Editor120静态场景build/run PASS、`dirty=false`，预热后CPU最大27.30ms、0慢帧。统计功能本身不解决卡顿，不把归因写成性能改进。

### 2026-09-26 宽松目标的当前验收

- 通用发束求解器在每帧物理子步中插值头骨支点和静止方向，避免低帧率时整段时间只以终点姿势积分；移动发根10/120Hz对照及原有传送、急转、碰撞测试通过。近景第90帧Game capture通过，预览没有整卡或穿脸，但发片细边仍明显粗糙；不宣称UE级头发最终美术完成。
- SICKA当前走跑跳900帧Game回放，预热60后840帧CPU中位/P95/max22.331/25.342/34.363ms，仅1帧超33.333；GPU最大12.38ms。Editor同场景900帧预热后CPU中位/P95/max20.134/23.713/30.248ms，0慢帧，GPU最大12.076ms。以用户放宽后的“预热后基本稳定30FPS”口径，当前漫游及静态Editor帧时可接受；首播佛像镜头14个图重建慢帧仍是已知限制。
- 当前HE/UE街道前后固定机位预览已逐张复核：路面、建筑、树冠层次、雾和暖灯布局基本相似；UE参考上方有异常大块树冠，不作为HE对齐目标。69棵树的放置和四种网格LOD0拓扑/UV/法线/切线校验PASS，落叶21个暖叶发射器调色与Editor保存重开PASS。通用UE工具7项单测通过；正式派生glTF `content plan` 为`changed=false`、新增/修改/删除均0，说明已发布资源可重复导入。此结论是宽松的静态视觉与漫游验收，不等于光照、GI、Niagara逐参数等效。
- 当前正式SICKA第300帧Game走跑跳回放截图已目视：玩家跳跃、路面阴影、灯笼、雾及暖褐落叶正常，无白卡、整块树叶矩形或后处理破图。旧UEFabWardrobe横向五角色第90帧capture及UEFabDemo120帧启动PASS；独立AsterCourtyard与ClockworkGarden各Game90帧build/run PASS。用户允许宽松验收，当前人物与新场景的主流程已有可运行证据；仍把头发细边粗糙、首播演出图重建慢帧、GI/粒子与UE逐参数差异列为限制，不将它们写成已等效。


### 2026-09-28 渲染与前端升级（集成中）

- 已实现相关Smith BRDF统一、解析高度雾、皮肤法线语义修复；RenderingShaderMath单测通过。
- 已实现真正分离皮肤diffuse的屏幕空间扩散（保留高光、雾、对象边界），1080p同镜头OFF/ON构建运行通过；专属两pass合计约0.37ms，画面差异细微，尚非UE等效。
- 新增通用局部曝光与玩家曝光补偿、设置分层持久化。RenderSettings单测发现DefaultEngine缺省漂移后已修复复验PASS；后加的曝光补偿/Forward可用性待最终复验。
- RenderGraph按资源索引访问声明，两项图契约单测PASS。实际运行诊断确认旧ready过早：仍有264张纹理排队，运行中textureCollect单次7–25ms。正在接入通用加载期渲染预热，保持玩法不推进。
- 原创斜切菜单、实际设置与五套换装已可运行，第一次UI截图发现块布局和衣柜镜头遮挡问题，继续修正，不能以静态内容检查当作完成。
- 换装骨架兼容检查已下沉Animation/SkeletonCompatibility.h，拒绝不匹配slot/绑定矩阵和NaN，AnimationPoseContract PASS。
- 所有项目内容仍在C:/WorkSpace/HEngineProjects/MechanicGirlLab；原始UE/Fab只读，未推送代码。

### 用户追加：轻量中文主菜单（2026-09-28）

- 默认入口改 Assets/Scenes/Frontend.he，仅6实体、1个角色；不引用庭院网格/AI/粒子，保留衣柜和人物动画。工具 Tools/Build-FrontendScene.py 从已导入的角色配置重建入口，不修改 UE/Fab 源资产。
- 新 MainMenuMode 只负责人物展示/菜单；游玩请求 SceneFlow.Load(SickaDynasty)，完成引擎既有进度条和真实资源预热后进入玩法；返回标题重新载入 Frontend。
- 默认 zh-CN，设置即时切换简体中文/English并保存。中文使用现有 Noto Sans SC 引擎字体；旧保存文件缺少 Language 时保持中文默认。
- Game 新增量构建 PASS 63.2 秒；新版菜单/语言/加载流程正在运行复验。旧版“庭院背景主菜单”截图不作为最终交付图。
### 2026-09-28 追加：展示台、RmlUi 加载与移动稳定性（复验中）

- 主菜单已改为 11 实体的小型独立展示空间：分层手办台、铜金环和冷暖灯；庭院仍在点击游玩后加载。展示台工具生成 6,224 三角形，未读取原始 UE/Fab 写入；主菜单、衣柜、设置分别使用专属展示动作。换装通过兼容骨架 rebind 保留动画时钟和过渡，AnimationPoseContract 已验证。
- 菜单、HUD 使用 RmlUi；历史加载条确为 ImGui，现将本项目 CopperNoir 加载页迁移到引擎 RmlUi overlay，复用 Noto Sans SC 中文。旧项目 Classic 加载页保留兼容性。新版本尚待实际加载截图复验。
- 用户反馈射击后行走抖动：固定 60Hz 角色控制器原来直接提交阶梯位置。新增引擎 FixedPositionHistory，渲染角色及其子网格、跟随相机目标统一使用 fixed-step alpha，物理/玩法仍使用权威位置。传送/销毁重置，暂停保留插值相位。AnimationPoseContract 的 30/60/75/120/144Hz、传送、暂停测试 PASS；RenderWorldExtractorMeshPath 的父子挂件、运动历史、编辑器坐标及不改物理变换测试 PASS。真实行走/射击画面仍待复验。
- 已排除连续 FOV 变化每帧清空 TAA 的假设：仅重新计收敛帧数，历史仍使用上一帧真实投影；新增连续 FOV 天空重投影回归 PASS。GI/云的保守历史失效未在此轮修改。
- CPU 优化新增共享只读材质快照、每 GPU 材质每帧一次准备、PostTAA 深度 batch 实例化。RenderWorldExtractorMeshPath、HardwareRayTracing 均 PASS；未减少可见对象或关闭材质功能。优化后性能待测，旧 1080p Balanced CPU 中位16.954ms/P95 21.442ms 尚未达到60Hz目标。

### 2026-09-28 射击后抖动根因进一步确认

- 固定120Hz实机回放中，松火后第69→70→71帧真实视线俯仰为 -7.32206°→-4.42049°→-7.39672°，相邻方向跳约2.90°/2.98°；相机位置平滑。无射击对照俯仰保持-8°。这不是合法等价Euler数值跳变，而是重建后方向已改变。
- CameraFollow 的 `quat_cast(inverse(lookAt)) -> glm::eulerAngles -> Transform` 在 yaw≈±90°时发生精度/奇异分解问题。新增通用 `Math::StableEulerAngles`，按GLM quat(vec3)一致的RzRyRx顺序，用double矩阵和atan2提取，极点采用一致零roll；CameraFollow与通用DecomposeTransform共同复用，避免其他相机、挂接变换重复问题。旧Decompose的XYZ替代顺序也不再参与。
- 3,864组方向、roll及±90°邻域旋转往返测试PASS，追加负尺度/非均匀尺度导入变换测试正在复跑；修后shot/control同输入运行正在验收。未用削弱后坐力掩盖抖动。

### 2026-09-28 本轮收尾

- 最终相机同回放验证PASS：修前松火方向突跳2.976°，修后最大相邻0.06494°且符合正常后坐力指数回正；射击/无射击各300帧。旋转转换和固定步插值均在引擎通用层，项目只保留冲刺注视权重平滑。
- Rml加载、中文展示台/菜单、衣柜各动作、设置、保存重载、Play→HUD→返回全流程PASS；原始资源未修改，所有示例内容位于外部MechanicGirlLab。
- 原生1080p均衡档1500帧：排除120预热后98.33%≤16.667ms，CPU中位13.984/P95 15.520ms，GPU最大13.086ms，运行期无待上传纹理。均衡设为默认；High本机未达到60FPS。详细硬件、命令、少量慢帧与短运行限制均已记录。
- 最终Game/Editor构建及实际运行通过；旋转极点/导入负尺度、动画/换装、固定时钟、提取/RT材质契约通过。全部自动会话已STOP，未推送。本轮没有新增Shipping验收，不宣称UE5或AAA最终美术等效。

### 2026-09-29 4K display and render-resolution work (in progress)

- Identified native-4K GPU pressure: owned baseline median GPU 23.739 ms, TAA 5.923 ms; prior Session replay has 1741 render samples and is not substituted for a fixed check run.
- Implemented project-controlled borderless startup before window creation, saved window-mode overrides, and true RmlUi quality/window/internal-resolution dropdowns. Runtime roundtrip already passed on the 3840x2160 desktop; final integrated checks pending.
- Engine RenderHeight cap separates internal 3D extent from native output/UI. TAA/HDR scene use internal pixels; clamped Catmull-Rom upscale is integrated into tone mapping. Editor/offscreen remain native. No FSR/DLSS claim.
- Fixed candidate graph preparation's output extent and deferred TAA history allocation until final scene extent to avoid allocating native-4K history during a capped resize.
- RenderSettings and GameDisplaySettings unit tests PASS. Initial compile failures (missing complete SceneRendererStats include; Windows small macro colliding with test local) corrected. Final Game/Editor runtime, 4K comparative performance and image QA still in progress.

#### Additional first-use stall fix

Matched replay identified frame121/122 graph rebuild reasons MeshAdded/MeshUploaded,
with8ms CPU graph realization at1080internal (68/94ms native4K). The first shot starts
at121 and zero-scale effects were excluded by render extraction. Added generic
RenderSystem3D::PrepareLevelMeshResources, deduplicating current-level resident mesh
references and submitting resource-only snapshots during RuntimeGameLayer loading.
Caller owns warmup requests only until ready/abort/stop; no hidden geometry is drawn
and no other level is loaded. Contracts cover collapsed/offscreen meshes, dedup,
release and restart. Final rebuild and replay after this fix are pending.

### 2026-09-29 final integrated display/performance result

- Default borderless, persisted window mode and Rml quality/resolution dropdowns implemented and runtime-verified. Native4K UI +1080internal is default; legacy projects keep native resolution unless configured.
- Current-level resident mesh warmup includes hidden/offscreen instances and releases requests after loading. Resource contracts passed after fixing the empty-placeholder case. Final replay has no in-game graph rebuild/upload events, including first shot.
- Final same-binary4K Performance native→1080internal GPUmedian23.321→10.977ms; CPU13.085/P9514.517/max17.659ms,99.64% within60FPS budget. Balanced13.197/P9515.068/max22.678ms,98.84%. These25s runs retain all slow frames; High/native4K and long strict60Hz are not certified.
- Game/Editor builds, settings/display/resource tests, native UI/spatial-AA and Game/Editor images passed. Editor deliberately remains native. Native D3D12 debug layer unavailable; NVRHI validation passed.
- Full commands/evidence in engine-upgrade-validation.md, contracts in RenderResolution.md and GameDisplaySettings.md. Latest full-screen main→game→main lifecycle PASS:11→3676→11, residentMeshes4→3103→4, all ready pendingTextures0, overlay pairs3. All owned processes stopped; no source asset changes or Git push.
