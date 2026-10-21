# Starbloom Survivor 四域净化

本文是四域目标、区域遭遇、领域权能、动态普通武器槽和最终 Boss 流程的权威实现说明。数值入口位于 `Project/Scripts/SurvivorRegionObjectives.py`；局外槽位规则位于 `Project/Scripts/SurvivorMetaProgression.py`。运行时代码不得复制这些表。

## RunConfig

每局开始前由 `SurvivorRunConfig.compile_run_config()` 从已校验的 Profile 编译一个深度不可变快照：

- `selectedCharacter`
- `selectedDifficulty`
- 完整、按 Catalog 排序的 `challengeIds`
- 0～15 分钟预编译的 `combinedChallengeTuning`
- `combinedChallengeRewards`
- 本局固定的 `weaponSlotLimit`
- 完整 objective/domain 配置快照
- `runSeed`
- Catalog/Profile/RunConfig schema identity

世界时钟使用 `RunConfig.world_delta()`。区域、中央停留、Boss intro 和 Boss 狂暴使用 raw gameplay delta；Hurry 不缩短区域 60 秒。Pause、升级卡和设置菜单不调用 gameplay update，因此目标时钟不前进。进行中的 RunConfig 不再读取 Profile，局外购买只影响下一局。

## 区域映射

区域判定仅使用 XZ 距离，不依赖碰撞回调，也不比较 Y：

| 区域 | region id | Anchor tag | 半径 | 进入 | 保持 | DomainPower |
| --- | --- | --- | ---: | ---: | ---: | --- |
| 星 | `star` | `EventAnchor_NorthRift` | 9.0m | 8.50m | 8.75m | `domain_star` |
| 雷 | `thunder` | `EventAnchor_WestShrine` | 7.5m | 7.00m | 7.25m | `domain_thunder` |
| 水 | `water` | `EventAnchor_SouthWetland` | 9.0m | 8.50m | 8.75m | `domain_water` |
| 风 | `wind` | `EventAnchor_EastBasin` | 11.0m | 10.50m | 10.75m | `domain_wind` |

四区可按任意顺序完成。区域身份只决定 roster、视觉与权能；本局第几个激活决定怪潮数量、固定精英、HP/伤害倍率、XP/金币预算和宝箱 tier。

`GenerateVerdantRift.py` 为每区生成 10 个 transform-only socket：

- `EventSpawn_Star_00..09`
- `EventSpawn_Thunder_00..09`
- `EventSpawn_Water_00..09`
- `EventSpawn_Wind_00..09`

生成器验证 socket 在区域光圈外、距离玩家起点至少 24m、距离边界至少 3m、0.7m footprint 地面连续、不在水下/悬崖/大物件或碰撞 OBB 内，并保证每区至少两个主要进攻方向。四个 72 段主题 marker 和四个稀疏半透明能量网格穹顶始终 visual-only；穹顶只在 ARMING、ASSAULT 与 OUTSIDE_GRACE 期间由运行时缩放显示，网格结构避免从罩内观察时遮挡战斗画面。

四域使用统一的低多边形童话材质语言，但各自保留远距离可辨认的轮廓：星域为星环、星柱与浮岩，雷域为雷击尖塔，水域为水晶泉与环流，风域为风环与螺旋飘带。主题地标的完整 footprint 必须位于战斗光圈外且不得占用 spawn socket。

## 状态机

区域状态：

```text
DORMANT --圈内连续 1.5s--> ARMING --> ASSAULT --有效 raw 60.0s--> ACTIVATED
   ^                              |          |
   |                              |离圈      |1.25s 内返回
   |                              v          |
   +-- COOLDOWN 12.0s <-- OUTSIDE_GRACE -----+
              超过 1.25s：进度归零、停刷，但不删除现存怪
```

全局状态：

```text
EXPLORATION <-> REGION_ASSAULT
    -> BOSS_READY --中央连续 2s--> BOSS_INTRO --raw 3s--> BOSS_ACTIVE
    -> VICTORY（仅注册过的 objective Star Dragon 死亡）
```

约束：

- 同时只有一个区域可处于 ARMING、ASSAULT、OUTSIDE_GRACE 或 COOLDOWN。
- ARMING 离圈只取消路过触发；ASSAULT 离圈立即暂停进度。
- 宽限内回圈保留进度；超过 1.25 秒严格清零并进入 12 秒 COOLDOWN。
- COOLDOWN 不产生区域怪。原怪保留 native no-distance-retire 标志。
- 冷却结束后，每帧最多回收 5 个约 30m 外或未交战尾怪；近处交战怪转普通游荡并重新启用 48m 世界退休。
- 复活保留 ACTIVATED 区域；当前 ARMING 取消，当前 ASSAULT/OUTSIDE_GRACE 失败并冷却。
- 区域完成事件具有 `completionId`；Domain、overcharge、XP vacuum 和宝箱分别记幂等 receipt。

## 世界威胁与区域遭遇

Adventure 全局威胁 band：

| 已完成区域 | 目标数量 |
| ---: | ---: |
| 0 | 30→45 |
| 1 | 45→60 |
| 2 | 60→80 |
| 3 | 80→105 |
| 4 | 100→125 |

每个新 band 在 45 秒内渐进。区域怪与世界怪分账；所有入口（含召唤）再次检查 `MAX_MONSTERS=320`。第四区完成后普通补怪暂停 9 秒，随后 Boss Ready/战斗阶段按 40→70→100 控制遮挡。

Adventure 区域 tier：

| 激活顺序 | localAlive | 补怪 floor/cap | 固定精英 | HP | 伤害 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 50→85 | 5→8/s | 2 | ×1.00 | ×1.00 |
| 2 | 75→120 | 8→12/s | 3 | ×1.18 | ×1.10 |
| 3 | 105→160 | 11→16/s | 4 | ×1.42 | ×1.22 |
| 4 | 140→210 | 15→22/s | 5 | ×1.72 | ×1.38 |

`localAlive` 只统计圈心 24m 内且 `encounterId` 正确的活怪。Fierce 数量 ×1.15，Nightmare ×1.30，再与完整 challenge tuning 合成并钳制。区域 HP/伤害不叠加世界时间 wave 倍率。

补怪率使用最近 5 秒 EWMA：

```text
spawnRate = clamp(1.15 * recentKillsPerSecond, configuredFloor, configuredCap)
```

低于目标 25% 时，每 5 秒最多一次 tier-bounded burst。burst 与固定精英均进入跨帧 pending 队列，只有实体成功出生才扣减；socket、地面或全局 cap 失败不会静默吞掉压力。约 18/34/50 秒产生固定精英。15/30/45 秒分别消费 `lane_rush`、区域主题 `four_side|four_wall|charge_lanes` 和 `final_peak` socket pattern；这些事件不是未消费的 Catalog 字符串。

每区 roster 始终保留 62% 低血量 fodder；剩余专家按 15 秒阶段逐步开放。

## 防刷预算

区域怪携带独立的 `encounterId`、`zoneId`、`attemptId`。同一区域重试沿用整个 run 的 XP/金币 budget：

- 区域怪基础 XP 为普通对应怪的 50%。
- tier XP budget 为 320/650/950/1300。
- tier 精英金币 budget 为 8/12/16/22。
- `EncounterDirector.begin_attempt()` 只在首次访问区域时建立预算；重试不刷新。
- 固定精英可按新 attempt 重生，但额外金币仍消费共享 budget。
- 转为普通游荡的幸存怪仍保留不可变 reward provenance；它们的 XP、金币和精英任务贡献继续受原区域预算限制。
- 同一区域的近处幸存怪仍使用相同 encounterId，因此重试时计入 localAlive；32m 外历史尾怪在新 attempt 前渐进回收。
- 拾取池满时使用 `source_values` 分项合并；完成 vacuum 只取当前 encounter 的份额，不会漏吸或误吸世界 XP。

## 完成事务

区域完成按以下事件顺序执行：

1. 标记 ACTIVATED。
2. 停止区域刷新。
3. 授予独立 DomainPower，并仅首次写入 `discoveredDomains`。
4. 触发一次 overcharge；普通/精英按配置损失最大生命比例，Boss 不受 overcharge。
5. 自动吸取该 encounter 的活动与待生成 XP。
6. 生成现有 Boss chest，`sourceKind=region_completion`、显式 `zoneId`，tier 为 1/2/2/3。
7. 更新 HUD、任务、遥测与本局状态。
8. 渐进退休超出新世界威胁的远处怪。

默认不回血。

## DomainPower 边界

DomainPower：

- 不在 `WEAPON_ORDER`，不占普通槽，不进升级三选一，不使用武器 1～10 级。
- 不计入普通武器任务，不触发 Trinket/echo/递归 proc，不暴击。
- 使用 `domain_*` 结构化伤害来源与独立 casts/hits/damage/kills/controlTime/visualDegrades。
- 每局重新取得；Profile 只永久记录图鉴发现。
- 以确定性顺序和 1.10 秒全局 VFX spacing 错峰。

初始值：

- 星落：14 秒；0.7 秒预警；AoE 后 3 秒 +8% 易伤。
- 雷霆：11 秒；4～6 链；普通/精英 0.35 秒 interrupt，Boss 只延后攻击。
- 潮汐：15 秒；25% slow 2.5 秒；普通怪轻推，Boss 不位移；water tag 扩区/延长控制。
- 风暴：17 秒；3 秒 pull zone；Boss 只 slow，视觉与实际力方向均为向内吸引。

普通武器版 `Thunder`、`Starfall`、`Tornado` 已退出 Catalog、升级池、商店和武器任务，只保留四域奖励中的独立 `domain_thunder`、`domain_star`、`domain_wind`。既有共享 VFX 继续由 DomainPower 的稳定 RuntimeAssets key 引用，不再以普通武器归因。

平衡目标为单个 12%～15% 以下、四个合计 25%～35%。

## 动态普通武器槽

绝对 UI/系统上限仍为 6。本局上限为已选择的 3～已购上限，并在 RunConfig 中冻结。

| 槽位 | 价格 | 前置 | 低槽结算倍率 |
| ---: | ---: | --- | ---: |
| 3 | 基础 | 无 | ×1.20 |
| 4 | 600 | Adventure 首通 | ×1.10 |
| 5 | 1500 | Fierce 首通 | ×1.05 |
| 6 | 3200 | Nightmare 首通 | ×1.00 |

`meta_weapon_slots` 使用逐级 prerequisite、optimistic expectedLevel、staged mutation、原子存档与精确退款。Hub 对每个商品使用 0.35 秒独立交互防抖，双击及 A→B→A 快速点击不会跨级重复扣款。Hub 可选择 3～已购上限；HUD 的六格分别显示 locked/empty/equipped。低槽 bonus 在 result breakdown 中单列，questGold 不参与倍率；结算事务无条件消费 `begin_run` 的槽位快照，调用者不能用旧参数覆盖。六武器任务在 `meta_weapon_slots:3` 前隐藏且不累计。

## Profile v4 迁移

v3 曾新增：

- `selectedWeaponSlotLimit`，新档为 3。
- `discoveredDomains`，新档为空。
- `lastRun.weaponSlotLimit` 与可扩展 `runConfig/resultSummary`。

v4 移除普通 `thunder`、`starfall`、`tornado` 及 Tornado 进化 `storybooktempest`。迁移会从 owned/eligible/poolDisabled、活动任务进度和图鉴中剔除这些普通武器内容，并按历史商店价格一次性退款（Thunder 0、Tornado 100、Starfall 120）；已完成任务历史保留。v1/v2/v3 迁移仍保留钱包、其他 owned/eligible、任务、训练、mastery、难度/挑战 clear 和历史 lastRun。旧 v2 的历史 run 槽位解释为 6，新的计划槽位为 3；归一化与重复迁移确定且幂等。

## 最终 Boss

`BOSS_EVENTS` 只含无 `time` 字段的 `starbloom_final_objective`。900 秒不产生 Boss，也不胜利。

四区完成后进入 BOSS_READY，中央 4.5m XZ 半径连续停留 2 秒进入 3 秒 intro。运行时显式传入：

- `archetypeId=star_dragon`
- `bossEventId=starbloom_final_objective`
- `objectiveBoss=true`
- `finalBoss=true`
- `no_distance_retire=true`

HP 为 Adventure 50,000、Fierce 75,000、Nightmare 110,000。Native `SurvivorBossBehavior::StarDragon` 实现高血量八向星环、中血量五向扫射吐息、低血量最终冲锋+压区+召唤；120 raw 秒一次性软狂暴。只有已向状态机注册的 entityId+activationId+eventId 目标 Boss 死亡才进入 VICTORY。

Boss 绕过普通 48m 退休。玩家离中央超过 35m 时 Boss 以 4m/s 返回，并以每秒最大生命 0.15% 温和恢复；永不退役或消失。

## Warmup、Cook 与低粒子

- `RegionFoundations`：启动 Adventure 时预热四个 marker mesh；第一批 cast/beacon VFX 由 loading 阶段显式 `VFX.LoadEffect`。
- `RegionRewards`：ASSAULT 开始时排队受支持的 prefab barrier，并显式预载四权能 cast/impact/overcharge。Domain 图鉴的原子保存若暂时失败，会保留完成事件并在后续 gameplay frame 重试，任务记录在持久化成功后才提交。
- `FinalBoss`：第三区完成时排队 Star Dragon prefab，并显式预载中央召唤与 Boss telegraph。
- ToonWater、四域 keys、中央召唤、Star Dragon 与新脚本均在 RuntimeAssets/Cook RootFiles 中。
- gameplay-critical ring、范围与 Boss telegraph 使用 critical VFX 类别；low particle 只缩小装饰 footprint，目标栏、范围与预警不隐藏。
- 项目默认启用 FXAA 处理 3D 场景边缘；RmlUI 颜色与裁剪目标使用 2× MSAA 后解析，单独平滑圆角和边框，避免对 HUD 文本再次做全屏模糊。

## 遥测与诊断

`SurvivorTelemetry` schema v2 记录 RunConfig、seed、difficulty、全部 challenge、slot limit、区域顺序/时间/尝试/离圈、localAlive samples、spawnRate/EWMA/burst/失败原因、共享预算、固定/随机精英、区域失血、武器和 Domain 独立贡献、Boss TTK/阶段/狂暴、p50/p95/p99 和 peak monsters/projectiles/VFX。

Dev Agent/GM 可读取：

```powershell
./hectl.bat agent start -StartupScene Assets/Scenes/MainGame.he
./hectl.bat agent gm-execute -GMCommand survivor.objective.snapshot
```

返回 objective state、active region、progress、completed regions、localAlive/target、spawn rate、grace/cooldown、Domain cooldown、Boss 与性能摘要。

## 确定性场景

设置 `HE_SURVIVOR_CAPTURE_SCENARIO`：

- `region_dormant`
- `region_1_assault_30`
- `region_2_peak`
- `region_3_peak`
- `region_4_final`
- `region_grace`
- `region_cooldown`
- `boss_ready`
- `boss_phase_1|2|3`
- `three_slot_adventure`
- `six_slot_nightmare`
- `low_particle`

另用 `HE_SURVIVOR_ANIMATION_BUDGET=12|16|20` 做 crowd motion profile。Agent 会话必须在 `finally` 中 stop。

区域 assault/grace/cooldown/low-particle fixture 会按该时点 local target 预填遭遇怪，并补入已到期固定精英；因此截图和 Tracy 不再从空场开始，能够覆盖真实高峰压力。

## 性能预算与当前诊断

目标为最低目标硬件 p95 ≤ 16.7ms、p99 ≤ 25ms。Tracy 使用 1280×720、固定 60Hz 模拟输入、第四区 45～60 秒、Adventure/3 槽、16 动画预算、4×120 采样帧；固定 delta 只控制可重复输入，百分位来自真实 `Application::Frame` wall-clock zone。

当前主机高峰样本（203 peak monsters、6 projectiles、7 VFX）为：median 21.590ms、p95 34.961ms、p99 51.825ms，**未达到目标**。分区 p95：Survivor Python 2.64ms、native monster phase 0.16ms、animation 0.38ms、RenderSystem 5.61ms、RmlUi Render 13.35ms。当前首要瓶颈是 RmlUi render/layout 尖峰和 frame 前置等待，不是怪物 native batch 或动画预算；不得通过隐藏降低普通武器槽补偿。Combat/Locomotion controller 参数已分流，该样本 Animator warning=0、stderr=0。权威 trace/metrics 位于 `out/agent/profiles/20260903-182855-162-9d4cf7246568/`。

同一采样的 localAlive p50 为 173、target p50 为 201（86.1%），也低于 90% 目标。已将 burst 改为跨帧可靠补充；后续调参优先在既定 15～22/s 边界内调整 refill/burst 触发与 HUD/RmlUi 更新成本，再做目标硬件复测。Balance harness 的 8 个 seed 仍给出区域等级 9～10 / 17～18 / 25～26 / 30～31、Boss-ready 32～33、Boss TTK 90 秒。

## 验证与调参入口

纯规则和引擎桥合同：

- `SurvivorRegionObjectives`
- `SurvivorWeaponSlots`
- `SurvivorDomainPowers`
- `SurvivorObjectiveBoss`
- `SurvivorRegionSceneResources`
- `SurvivorEnemyBehaviors`
- 既有 Balance/Challenge/Economy/Quests/RunEnd/Wave/Projectile/RuntimeAssets 回归组

后续调参只修改：

- 区域/Domain/Boss：`SurvivorRegionObjectives.py`
- 槽位经济：`SurvivorMetaProgression.py`
- 数据层模拟：`SurvivorBalance.py`

不得用隐式武器槽敌人补偿掩盖 local target、XP budget、固定精英或 Domain 数值问题。
