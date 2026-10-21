# FSR 快速角色运动验证

`Tools/Agent/Test-FsrMotion.ps1` 使用项目自己的可重复动画场景，分别采集
FSR Quality 显示图、运动矢量、表面编号，以及 NativeAA 显示图和表面编号。
每段先重载场景、步进 120 帧，再在相机切换后仅渲染 8 帧以建立历史，
最后连续步进并采集 16 帧。会话由脚本独占并在 finally 中停止。

当前 MechanicGirlLab 场景由外部项目的 `Tools/Build-FsrMotionDiagnostics.py`
生成，配套脚本为 `Scripts/CSharp/FsrMotionDiagnostic.cs`。它保留实际角色材质、
头发次级骨骼和 8010 粒子的布料模拟，将动画速度设为 2 倍，同时对父实体施加
最大 4.4 m/s 的横移和 4.55 rad/s 的转身。背景是固定明暗条纹。
诊断脚本只挂在该场景上；这是渲染输入验证，不是玩家控制或战斗行为测试。
生成器需要项目原有角色资源和 FsrParticles 背景材质模板。

## 运行

先生成场景并构建包含诊断脚本的项目：

```powershell
py -3 C:/WorkSpace/HEngineProjects/MechanicGirlLab/Tools/Build-FsrMotionDiagnostics.py
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab
```

用实际构建文件的 SHA256 替换下面的值，两个后端分别使用新的输出目录：

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrMotion.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -GraphicsApi DX12 -ExistingBuildSha256 <SHA256> -OutputDirectory out/agent/captures/motion-dx12
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrMotion.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -GraphicsApi Vulkan -ExistingBuildSha256 <SHA256> -OutputDirectory out/agent/captures/motion-vulkan
python Tools/VFX/analyze_fsr_motion.py out/agent/captures/motion-dx12 out/agent/captures/motion-vulkan
```

分析需要 NumPy 和 Pillow。输出 `motion-analysis.json` 和最长边不超过 1280 px
的 `motion-preview.jpg`。报告里的 `visualReview=NOT_REVIEWED` 留给人工检查，
自动脚本不会把生成联系表当作视觉验收。

其他项目可用 `-Fixture` 指定自己的项目相对 manifest。manifest 需提供
`scene`、角色 `actor` UUID、`warmupSteps=120`、`frames=16`、`sections` 和
`motionGroups`。每个 section 包含 `firstPrimitive` / `primitiveCount`，
group 对应 section 数组下标；范围必须与实际提交的 mesh/LOD primitive 编号一致。
场景必须有可重复、可重置的动画和布料，保持固定摄像机，并让各组有充分可见像素。
脚本通过 hectl 解析项目缓存，不包含项目名或缓存路径硬编码。

## 检查含义

- 五段回放的起止完整骨骼 palette 和布料位置哈希必须分别相同。
  同质量档位的 jitter 序列必须一致；提交历史链必须连续，采集时不得再次重置。
- 每个帧的元数据必须显示 FSR 活跃、独立 TAA 关闭、锐化为 0。
  输出为 1280×720；Quality 内部为 853×480，NativeAA 内部为 1280×720。
- 以 object/primitive ID 分离身体、头发和布料。原始矢量为
  `previousUV-currentUV`，含 jitter；回投到上一帧的同一三角形像素集合，
  计算到最近像素中心的距离，并与只有 jitter、没有物体运动的对照比较。
- 当前回归阈值：每组超过 1000 个对应样本，运动中位数超过 2 px，
  回投距离中位数 <0.8 px、P95 <1.5 px，P95 小于零运动对照的 20%。
  暂停并稳定后去除 jitter 的残余运动最大值 <0.01 px。
  阈值用于这类诊断场景，不是任意镜头、分辨率或素材的画质标准。
- 用上一帧角色区域减去当前角色区域及一像素轮廓带，识别新露出的背景。
  记录 Quality 与 NativeAA 的显示 RGB 差异，不给它套用通用“无拖影”阈值。

## 已验证结果与限制

2026-09-29 最终证据位于 `out/agent/captures/fsr-motion-{DX12,Vulkan}-final`。
两端采集和分析 PASS；完整命令与构建哈希见 `FSRStreamingCombatWork.md`。

| 分组 | DX12 回投 P95 / px | Vulkan 回投 P95 / px | 零运动对照 P95 / px |
|---|---:|---:|---:|
| 身体与头部 | 1.153 | 1.153 | 17.58 |
| 头发 | 1.339 | 1.319 | 15.85 |
| 布料 | 0.967 | 0.968 | 16.94 |

最终两端暂停残差最大值均为 0.000185 px。新露背景每帧平均 RGB 绝对差异最大
约 0.00650（显示值范围 0–1），逐帧 P95 最大约 0.05752。人工检查两端
第 0/5/10/15 帧的联系表，差异集中在细发和轮廓，没有观察到明显的大面积拖尾。

这里仅覆盖约 0.267 秒的一段快速运动。NativeAA 本身使用时域重建，且不同档位的
jitter 不同，因此不是数学真值。表面回投比较也只统计前后都有可见像素的三角形：
头发镂空、亚像素三角形和遮挡会减少对应样本。约 0.9%–1.3% 样本距离超过 2 px，
最大值达 11.04 px，完整保留在报告中；没有用深度真值区分这些尾部样本的遮挡变化
与运动误差。不能据此声称每个像素都正确，或所有画质档位、战斗镜头均无拖影。
