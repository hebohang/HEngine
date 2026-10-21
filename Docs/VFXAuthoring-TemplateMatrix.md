# 模板截图矩阵

`py -3.11 Tools/VFX/template_matrix.py --backend DX12 --existing-build-sha256 <候选SHA256> --output out/agent/rendering-evidence/vfx-authoring/matrix-dx12-01`

Vulkan替换backend和新output。当前默认24配方×neutral/game两场景，使用6个会话：每场景分别运行CPU High 12、CPU Low 6、GPU High 6；CPU组禁用GPU粒子，GPU组启用。每组实际设置并查询对应quality policy，不能把Low fallback混在High会话中。可用 `--recipe trail-ember --scene neutral` 重跑子集；两参数均可重复。所有会话固定binary hash，不触发构建，整个矩阵有同一Storage外层记录并在finally停止owned session，防止内层回收提前删除证据。

每配方采样start、attack+sustain/2的peak、decay中点、名义结束+4帧，重复时刻合并。播放速度用于帧换算，speed=0拒绝有限结束采样。trail通过公开transform接口沿X轴移动约2倍名义radius，最多约12个运动段加采样边界，不是静止图冒充拖尾。每组先捕空场景，每效果结束再stop并退休32帧，保存截图、完整stats、状态回执、配方/包/场景hash及环境身份。

`commandStatus=PASS`只说明请求/生命周期检查完成；`visualStatus`始终`NOT_REVIEWED`。截图是图像读回，stats含CPU/提交/完成计时证据，但没有实际粒子buffer或fragment overdraw读回，不能据此宣布视觉质量、粒子数或性能通过。失败效果保留证据并继续同组；清理或会话失败停止矩阵。

## 当前实际状态（2026-09-14）

已执行真实GPU矩阵和针对性复验，尚未完成当前trail04资源的两后端完整视觉验收：

- `out/agent/rendering-evidence/vfx-authoring/template-matrix-vulkan-01/report.json` 是实际6会话、48效果组合的历史运行，`commandStatus=FAIL`；保留该失败，不能由后续子集替代整矩阵结果。
- `template-timing-vulkan-03/report.json` 的Vulkan六配方子集command PASS；同目录 `visual-review.json` 总体FAIL，5项时序/取景通过，trail-arcane仍呈竖直细线。该报告明确只覆盖timing/framing，不是完整库或双API验收。
- 后续trail04已修改CPU trail的parent bind与方向，并完成24包编译及 `out/agent/vfx-authoring/library-noop-trail-04.json` 的24项changed=0/hash和mtime不变检查；**尚无使用trail04的新实际矩阵**。这些编译/no-op结果不构成新拖尾视觉PASS。

当前 `template_matrix.py` 的neutral/game分别使用独立 `VFXAuthoringTemplateNeutral.he` / `VFXAuthoringTemplateGame.he`，相机位置分别为(0,6,-10)、(10,6.02,0)，取景比早期近景更远。原Preview和旧GameBackground不是当前矩阵的场景映射。每次报告中的场景/资源hash才标识该次实际输入；下文近景位置与hash均为历史记录，不应套用于当前场景。

## 历史记录说明

以下保留早期静态准备、首轮probe和旧近景契约；其中“未运行”只描述当时阶段。当前运行状态及剩余验收以上节为准。

## 历史：固定真实地图背景（静态准备）

`--scene game` 现映射 `VFXAuthoringGameBackground.he`，从 MainGame.he 完整复制地图/光照；原五实体 VFXAuthoringPreview.he 继续用于 scene/resize，原文件未修改。保留432实体及351个地图网格，源玩家网格移除以免遮挡原点。

来源 MainGame SHA256：`7f31592135ed4fb756b711b1190ff8c969879c0cc1efe9370de79f5f299e26f5`。新场景 SHA256：`b37d18d6b9fcd417c3fa15cbc7115efa98a5ed786be24bc38c56f56a9d70761d`。

移除运行组件（数量）：`{"AnimatorComponent": 1, "AudioListenerComponent": 1, "CapsuleCollider3DComponent": 1, "CharacterMovementComponent": 1, "Player.MeshComponent": 1, "PythonScriptComponent": 1, "FollowCameraComponent": 1, "SpringArmComponent": 1, "Rigidbody3DComponent": 48}`。移除脚本防止刷怪/UI/世界修改，移除动画、角色运动和物理防止位移；移除跟随/弹簧臂防止输入与碰撞改变镜头；音频监听不参与图像验收。保留所有实体UUID、父引用、地图网格、材质、HDR、环境/方向/点光与原曝光；仅将云风速和演化速度设0，冻结云时间噪声。

相机使用已存在 neutral 近景 Transform：位置(0,2.415,-3.2475952)，旋转(2.600846529,0,3.141592741)，60°垂直透视；沿用 MainGame 相机曝光与后处理。光轴约指向(0,0.465,0)，距离约3.79m，焦平面垂直视野约4.37m；1.6m名义直径约占720高截图264px（投影估算，不是粒子边界实测）。原点与trail高度0.5都在近景中心附近；大范围environment可能裁切，不能据此推断完整边界。

静态验证：JSON、唯一主相机、父/文件夹引用及递归Assets路径依赖存在性；145个资源依赖，缺失0。对hmesh/hmat/hanim/hanimctrl JSON递归收集Assets路径，其余二进制只验证存在并hash，未替代引擎加载/Registry/cook验证。依赖路径到SHA256排序紧凑JSON的摘要：`1cabb120851fb2349e3cab1fc4560ebd25a181a632e895a5c14da0a974af9b35`。运行矩阵仍记录所用场景与配方/包hash。

本场景是实际游戏地图的固定背景视觉条件，关闭玩法后的截图不代表完整玩法验收。当前仅静态准备；未运行引擎/build/GPU，visualStatus保持NOT_REVIEWED。

## 历史：真实地图选点修正（首轮 probe 后）

旧原点画面对准 StarPlinth；其本地 bounds Y=[0,.17] 加实体Y=.04，顶面=.21。Player序列化出生点(0,.05,0)不是VFX可直接采用的表面高度，原VFX的Y=0在平台内部。首轮证据保留，command PASS不构成视觉通过。

新anchor=(10,.02,10)，来自真实CentralMeadow静态碰撞地形。hmesh bounds X=[-30,30]、Z=[-28,29]、Y=[-1,.02]；有界解码位置/index accessor并做XZ三角形重心插值，中心以及[6,14]×[6,14]每米采样最高表面均Y=.02。中心没有DirtPaths三角形；周边同一8m方格部分道路高度最高.069，因此不是声称整个方格均无道路。中心8m内无独立装饰网格实体。平面范围及稀疏采样是静态几何证据，不替代实际游戏导航验证。

新相机位置=(10,2.435,6.7524048)，仅按anchor平移，旋转/FOV/所有光照曝光不变。template_matrix按模板设置播放位置：hit/explosion/trail中心Y=.52；telegraph/aura的作者节点本地平移0、固定X旋转90度，因此贴地Y=.035（surface+.015）；environment的作者发射立方体为±radius，中心Y=radius+.035以使初始发射范围在地上（运动后的边界不保证）。trail已有本地高度.5仅加anchor，保持Y=.52，不额外再抬升。报告保存playPosition/trailOffset；seed/时序不变。

新scene SHA256：`04ecf2ddfbe92c66229bc542ecfec088cb789b30f90d5bca38f748abae06197a`。纯函数检查24配方高度与neutral不变PASS；图像观察仅限既有probe contact，未运行新GPU/构建，需重新真实验收。上述新位置取代前段原点相机的位置说明。

## 历史：完整矩阵静态契约复核（旧近景）

每个效果现在在真实播放后读回颜色支持、position和speed；提交中性白色color及配方intensity（默认1也实际调用），核对接受与intensity状态。CPU/GPU模板均需supportsGlobalColor=true，避免默认值从未调用而漏报不支持。每阶段保存handle状态，start/peak/decay要求存活、end要求结束；peak要求实际drawCalls.current>0。该计数只证明提交，不能证明屏幕像素、GPU粒子状态、颜色响应或fragment overdraw。seed仍只保留首次更新前请求，SDK没有种子读回；scale没有真实公开读回，接受请求不等于测量半径。参数边界/变色/intensity0/重载等完整验收由parameter_acceptance补充。

固定近景约4.37m视野，覆盖24当前配方的命名采样阶段；ground模板Y=.035、trailY=.52、体积environment按radius抬高。静态位置合法不保证任何视角下无粒子遮挡或所有动态范围完整入画。start可能因fade-in为空，不能要求每张截图非空。end及retired保存空背景对照，当前driver不自动逐像素比较；逐像素空白与可读性仍需实际图像审查。一个游戏背景样例通过不代表其余47个场景/配方组合通过。完整两后端为96组效果结果，visualStatus保持NOT_REVIEWED。

unit-test：`py -3.11 Tools/VFX/template_matrix_test.py` PASS（2 tests，包含24配方×2场景完整mock流程、unsupported color与零draw负例及finally清理）。mock只验证driver结构/状态断言，不作为引擎/GPU通过证据。
