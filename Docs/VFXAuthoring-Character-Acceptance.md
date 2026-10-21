# 角色复用验收资源（2026-09-13）

本批仅新增资源和脚本；没有执行 Game、Editor 或 GPU。不得以资源存在或静态检查代替实际角色验收。

## 真实入口和资源

`Project/Assets/Effects/Authoring/CharacterDemo/CharacterAcceptance.he` 克隆 VFXAuthoringPreview 的地面、灯光、相机，挂载 `Scripts.VFXCharacterAcceptance.py`。脚本通过现有 `HEngine.Instantiate` 建两个 wrapper 实例；Editor 若已建立同名实例，会直接使用现有实例，避免重复创建。ActorA/ActorB 通过 schema2 nested 共享 Actor.hprefab，分别覆盖角色名称及动画速度。它们拥有不同的实例根 UUID，而非一个实例中的两个未隔离分支。

共享角色含模型子节点、Animator、脚本、碰撞组件、feet/hand 挂点与 effect 标记。碰撞采用 kinematic trigger box（同时保存 capsule 作者描述），不宣称已验证阻挡、接触或角色移动解算。脚本复用现有 PresentationService，以动画事件驱动粒子、音频、临时灯。脚步音效使用现有 monster_hit_01.wav 作为验收提示音，不标为正式脚步音色。

模型/纹理/骨骼/二进制全部引用已有 Quaternius BlueDemon；四个 hanim 仅压紧 JSON 并加入事件，保留全部原始 channel/key/node/bone 数据。Idle、Walk、Run、Punch 均为真实源动作。manifest.json 记录源 SHA256 与时长。`build_assets.py` 可确定重建，不改原资产、不复制 mesh/bin。

Locomotion 用 Speed=0/1.5/3 的 1D Idle/Walk/Run 样本；Speed<=.05 独立进入无脚步事件 Idle，运动时 Walk 主 clip 独占事件/时长，Run 仅作同步姿态样本。Attacking 切换真实 Punch clip。脚步位于主 clip 的25%/75%，攻击接触位于35%。这是表现事件，无伤害判定。

## 运行报告

实际脚本检测实例 UUID 隔离、ParentUuid 重映射、从成员查本实例 localId、覆盖、子树脚本已 Start、控制器已加载、参数独立、暂停、单实例跨周期 Step、事件唯一/drain、真实 VFX/音频/灯启动、攻击触发、取消和清理。使用 `HEngine.VFX.GetEffectPosition(handle)` 从实际运行读取粒子位置；API缺失会输出 blocked，不降级为缓存值或模拟 PASS。

挂点是实体局部挂点，脚本改变根平移和旋转，再比较 SDK VFX 位置、挂点世界位置与临时灯世界位置；**不宣称骨骼手部蒙皮跟随**。参数与状态断言也不等于骨骼画面正确，仍需双后端实际画面观察。

报告写入 `HEngine.GetSavedPath("Reports/VFXCharacterAcceptance.json")`，终端标记 `VFX_CHARACTER_ACCEPTANCE`。报告中的 evidence 为 runtime-manual，只有在真实引擎中全部断言通过才写 passed。失败也包含已通过项、事件和位置证据。脚本完成后清理表现轨道，角色回到 Idle 供截图；OnDestroy 清理所有自建实例及表现服务。

## Editor 模板传播与保存重开

`CharacterDemo/EditorTransactions.json` 给出真实 scene 事务 operations 和期待值，主代理在拥有的 Editor 会话执行：

1. 打开克隆场景，scene.plan/apply 建 A/B；用 query 返回的 UUID 解析 localId，不能手写运行 UUID。
2. A 的 hand y 当前=1.2，使用 prefab_override mode=set、pointer=`/TransformComponent/Translation` 记录整个向量的显式覆盖（即使与模板相同也应保留）。当前合并只递归对象，数组按整体覆盖；不支持 `/Translation/1` 逐元素覆盖，本步骤保留全部三个坐标。
3. 保留 Actor.hprefab 原始 bytes；把提供的 Actor.propagation-v2.hprefab 作为模板候选发布到该新测试路径。V2 把 hand y 改1.4并增加 badge。
4. 新建计划同时 prefab_sync 两个独立实例。A y保持1.2，B变1.4；两边新增 badge，名称/速度各自覆盖保留，父引用仍隔离。
5. Undo 应移除两边 badge、恢复B y1.2；redo 应恢复同步结果。scene.save、公开入口重开后重新断言相同值与显式 override 元数据。
6. 最后恢复测试模板原始 bytes；不改原始 BlueDemon 资源。外部模板文件变化不由 scene.undo 暗中撤销。

这些 Editor 步骤目前 NOT_RUN。运行脚本没有伪造 SceneEditingService 的事务或报告模板传播 PASS。

## 本批证据

`py -3.11 Project/Assets/Effects/Authoring/CharacterDemo/build_assets.py` 已生成资源；`validate_assets.py` 检查原始动作数据/哈希、事件时间和 ID、重建 no-op bytes/mtime、真实组合依赖。此检查不导入 HEngine，不是运行验收。`VFXCharacterAcceptance.py` 仅做 Python AST 语法检查；未运行，也没有假 HEngine 测试替代真实入口。

## 固定候选运行器

`Tools/VFX/character_acceptance.py` 复用 preview.Session；必须提供当前候选 SHA256，缺少或不匹配时不启动，也不构建。默认串行运行 character/presentation 两个独立 owned session，Storage 外层覆盖全部报告复制与 finally stop。报告只从匹配 ownerTag/hash 的新 session Saved/Reports 读取，并校验创建时间；原有 Saved 报告不会被接受。runtime.step 显式等待 targetCompletedSteps，失败保存有界日志尾和摘要。

```powershell
py -3.11 Tools/VFX/character_acceptance.py --backend Vulkan --existing-build-sha256 <Game04-SHA256> --output out/agent/rendering-evidence/vfx-authoring/game04-character-vulkan
py -3.11 Tools/VFX/character_acceptance.py --backend DX12 --existing-build-sha256 <Game04-SHA256> --output out/agent/rendering-evidence/vfx-authoring/game04-character-dx12
```

可用 `--case character` 或 `--case presentation` 缩小范围；输出目录必须是新的 Storage 白名单证据子目录。角色报告必须满足全部固定必需 checks、事件/位置证据、两实例唯一 UUID。旧 schema1 PresentationDemo 只有 status/time/error，仅按 timeline completion 记录。当前新增 schema2 报告要求13项真实逐轨观察全部为 true，并包含起止读回；运行器区分两种 evidence scope，不泛化为视觉或听感验收。输出保留 EditorTransactions=NOT_RUN。

运行器自身只执行了 `--help` 和 5 项不完整/失败/非有限报告拒绝检查（unit-test PASS，无会话启动）。上述两条 runtime-manual 命令仍未执行。

已与 stub_contract 代理协调后增强 `PresentationDemo.py`：在真实播放过程中记录 VFX IsEffectPlaying/GetEffectPosition、Audio.IsPlaying、临时灯实体/位置、Sprite 颜色变化和相机偏移；结束后等待两个额外更新边界再读取 VFX/audio stopped、light destroyed、Sprite/camera restored、active tracks empty。schema2 的失败项明确报告，不以创建句柄代替实际播放；没有模拟 SDK 状态。新脚本仅语法检查 PASS，尚未实际运行。音频 IsPlaying 是后端状态观察，不能证明扬声器实际听感。
