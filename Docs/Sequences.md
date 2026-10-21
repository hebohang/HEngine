# 短演出制作与运行

`SequenceComponent` 是随场景反射序列化的通用演出数据；`SequenceSystem` 在动画更新前播放。一个 Level 同时只允许一个演出持有镜头，竞争播放会报告错误。没有该组件的旧场景不改变行为。

## 制作、保存、重开

1. Editor 选中一个实体，在 Animation 分类添加 **Sequence**。
2. 添加轨道，填写 Binding name（例如 `Hero` / `ShotCamera`）和实际实体 UUID。轨道引用绑定名，可将同一份演出导入另一场景，仅重新绑定实体。
3. `camera` 键记录局部位置、欧拉旋转（界面为度、保存为弧度）、垂直 FOV（度）。**Capture bound camera** 从绑定的场景相机采样；拖动键值或添加多个键形成路径。四元数最短路径插值避免 360° 跳转，Smooth 使用 smoothstep。
4. 配置 Duration、Blend in/out。默认入出各 0.25 秒，从原游戏镜头平滑过渡；不同 camera 轨道按首键时间切换。镜头切换、Seek、循环及恢复游戏镜头都显式使时间历史失效。
   可设置 Reference binding 及制作时该实体的位置/旋转/缩放。播放瞬间冻结 `当前实体世界矩阵 × 制作参考矩阵逆`，将 camera、transform 和 lookat 的制作坐标映射到当前角色位置，玩家移动后播放也不会拍摄空镜；不会传送角色。启用参考映射时 camera 键使用制作世界坐标。
5. 保存场景；也可 **Export .hsequence** / **Import .hsequence** 复用。独立文件是制作交换格式，运行时使用组件中的数据，因此打包无需另外加载 `.hsequence`。
6. 进入 Editor 的 Play 模式后，在 Inspector 播放、暂停、拖动 Time 预览；Stop 恢复镜头、跟随状态、角色输入、原动画参数与表情权重。编辑键值应先停止播放。停止 Play 后正常场景保存与重开流程适用。

## 轨道

| Kind | Target | Name | Value |
|---|---|---|---|
| camera | 具有 Camera/Transform 的实体绑定 | 空 | FOV 度 |
| transform | 道具实体绑定 | 空 | 未使用；Position/Rotation 为制作世界姿态 |
| float | Animator 实体绑定 | 已存在的 Float controller 参数 | 连续参数值 |
| int / bool | Animator 实体绑定 | 已存在的 Int / Bool 参数 | 阶梯保持；Bool ≥ 0.5 为真 |
| expression | Animator 实体绑定 | 导入的 morph 名称 | 0–1 权重 |
| lookat | Animator 实体绑定 | 已配置的 LookAt 骨骼名称 | 注视权重，Position 为世界目标 |
| limb | Animator 实体绑定 | 已配置的 LimbTargets.End 骨骼名称 | IK 权重；目标沿用 LimbTargets 的 UUID/偏移 |
| trigger | Animator 实体绑定 | 已存在的 Trigger controller 参数 | 未使用 |
| audio | 可空 | 项目内音频路径（例如 Assets/Audio/line.wav） | 0–1 音量 |
| event | 可空 | 脚本事件名 | 事件数值 |

键必须按 Time 严格递增，所有数值有限且时间处于 Duration 内。无效绑定、未知轨道、参数类型不匹配和缺失音频会停止播放并显示 Error，不静默跳过。动画轨道使用引擎状态机/混合能力；没有独立重写动画播放器。表情名称须匹配导入资产；不存在的形变名不会产生美术效果。

Python：

```python
sequence = entity.GetComponent("Sequence")
sequence.play()
sequence.pause(True)
sequence.seek(1.25)
sequence.pause(False)
for cue in sequence.consume_events():
    print(cue["name"], cue["time"], cue["value"])
sequence.stop()
```

状态可读 `time()`、`is_playing()`、`error()`。事件只在正向跨越键时触发一次；暂停不重复，循环重新触发。Seek 为静音姿态/参数预览，停止本演出音频，不重放历史 trigger/event，也不反向重建动画状态机。需要从头审看演出时 Stop 后 Play。播放结束或中断停止所有本演出的音频句柄，绝不停止其它游戏声音。控制权释放于下一次 Level 更新。

## 验证入口与边界

`limb` 轨道除采样接触权重外，还把播放开始时保存的 `WorldRotationDegrees` 左乘 Reference 舞台的刚体旋转；重复 Seek 始终从保存值计算，不累计旋转。无 `TargetUUID` 的 `WorldTarget` 同样经过完整舞台变换；实体目标由其 transform 轨道负责移动，limb 不再重复变换。Stop 恢复原始末端旋转、世界目标和权重。目标实体局部偏移仍保持局部坐标。

`hectl test SequencePlayback` 覆盖反射往返、相机接管与恢复、参数/表情同步、事件仅一次、暂停、静默 Seek、输入恢复和重复键拒绝。真实相机、音频设备和编辑器可视结果仍需集成场景运行验收；单元测试不证明视觉质量。

`hectl test SceneEditingContainers` 验证反射动态数组、字符串键字典和嵌套演出轨道的编辑及保存重开，同时拒绝未知结构字段、错误元素类型和错误的固定数学向量长度。`SceneEditing` 包含这组检查以及完整场景事务回归，资源测试使用 `Examples/RenderingTests`。

真实 Editor 持久化验收入口为 `python Examples/AsterCourtyard/Tools/validate_upgrade_editor.py --editor <已构建的项目 HEngineEditor.exe>`。脚本以隔离场景副本修改演出关键帧、混入时间、表情权重及布料阻尼，保存后先切换到原副本，再重新打开保存结果并核对二次保存内容。原场景不变；证据位于引擎工作区 `out/agent/rendering-evidence/vfx-authoring`。这证明 Editor 作者数据流程，不代替 UI 操作或视觉验收。

这是中小关卡短演出的最小制作流程。当前不包含非线性剪辑、多层嵌套序列、音频波形界面、自动对白语音识别或任意时间反向求解动作状态。对白可将音频与离线准备的 viseme 表情曲线放在共同时间轴；不能把仅有权重曲线误称自动口型生成。
