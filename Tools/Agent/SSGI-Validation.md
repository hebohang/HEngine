# Deferred SSGI 修复与验证记录

日期：2026-09-09。证据目录：`out/agent/ssgi-deferred-20260909/`。

**追加 PBRTest 验收尚未通过。** 下面 Intel/Bistro 的结果不能作为“PBRTest 关闭环境光后，Low 能明显照亮右侧”的完成证据。PBRTest 的追加复现与限制见文末。

本轮修复屏幕空间追踪漏掉可见反弹光、平面深度阶梯自交，以及最低档仍保留过高滤波开销的问题。结果是可见受光表面附近出现间接光，低档在本机 1080p 的 SSGI 专属 GPU pass 合计约 2.7–3.6 ms。本文记录短时运行和画面观察，不宣称热稳定、普遍硬件性能或完整场景 GI 正确性。

## 改动与质量设置

- 追踪改为沿投影射线遍历实际 Hi-Z 像素单元边界；先裁剪近面和视锥，再做透视除法与世界距离恢复。深度厚度只判定是否命中，不再作为反弹光的额外能量衰减。
- 命中叶节点检查来源表面朝向。离开平面的光线不能因为邻像素的阶梯深度而读取本平面的直射 HDR；拒绝后继续在有限预算内搜索真正朝向射线的表面。
- 单次反弹结束时跳过后续反弹才需要的材质和位置读取；环境强度为零时跳过环境采样。
- 为 SSGI 单独修正时域统计钳制：用邻域亮度方差加上历史均值的不确定性构造亮度区间，再按亮度比例缩放历史 RGB，保留低采样下稀疏但有效的受光面命中及反弹颜色。原先当前邻域的原始极值会不断抹掉这些历史能量。该分支只用于 SSGI，保留 RTGI 原有可见性钳制逻辑。
- 修复 SVGF 上采样中零命中距离排斥有效邻域的问题，并限制无有效权重时跨表面的最近邻回退；合并等价指数权重计算。最低档启用已经存在的优化内核。
- 质量预设完整重置主要追踪参数，避免 Low 继承昂贵的自定义选项；切换优化内核会正确刷新渲染图状态。
- 兼容回归发现 RTGI CPU 校验仍只接受 2–4 次反弹，而 shader 已支持 1 次；将 `RealtimeRayTracedGlobalIllumination.cpp` 的合法范围修正为 1–4，使共享 Low 的单次反弹设置可以切换到 RTGI。重跑结果在下方单独记录。

最终 Low：SSGI 计算宽高均为输出的 0.5 倍、1 条射线、64 次遍历预算、1 次反弹、最大距离 10 个世界单位、2 次 A-trous、优化内核开启。Hi-Z 起始 mip=2、厚度=0.1、边缘淡出=0.05、时域权重=0.97。兼容字段 BinaryRefinementSteps=3；当前解析求交不执行二分细化。

64 次预算有实际画质理由：初始 24 步／5m 配置在 Intel 补充近景明显漏掉反弹；64 步／10m 更接近高采样参考。解析数值 fixture 也表明，跳过平面假命中后到达相邻可见墙面需要超过 24 次遍历。`out/agent/ssgi-deferred-20260909/SSGI-TraceFixtures.json` 保存几何输入及预期；这些是 CPU 数值模型证据，不是 GPU 画面测试。

## 硬件、输入与计时范围

本机为 NVIDIA GeForce RTX 3060 Laptop GPU、Intel Core i7-11800H（8 核／16 线程）、约 15.75 GiB 可见系统内存，Windows x64。主测试使用 DX12、VS2026 v145、RelWithDebInfo HGame 开发构建。未记录可与每帧对应的 GPU 时钟、功耗或稳定温度平台，因此不能把运行间差异全部归因于代码变化。

测试保留 Deferred、TAA、原模型与材质；关掉环境 IBL、AO、SSR、局部反射探针、bloom、自动曝光和 VSync，保留直射光阴影。LOD 关闭。曝光保留为 0.31。每个 Off／On 对只有 GI technique 不同；实际参数、尺寸、后端、场景、可执行文件与选定 shader/renderer 源文件 hash 均保存在 manifest 和 settings sidecar 中。

- Intel 原场景：`IntelSponza2048BCGPULOD/Benchmark.he`，原方向光 intensity=3；只把环境 IBL 设为零。原相机位置 `[-8.80743,1.59221947,-0.85825783]`。
- Bistro 原场景：`BistroExterior2048BCGPUMaskFixedEnvironment/Benchmark.he`，保留原方向光 intensity=150；只把环境 IBL 从 10 改为零。原相机位置 `[24.8228508,3.1605517,-61.6481436]`。
- Intel 补充场景：在同一套模型上，将方向光设为无父节点、rotation=`[pi,0,0]`、intensity=30、color=`[1,0.95,0.85]`；近景相机位置 `[-4,1.6,3]`、rotation=`[-0.25,-1.57,0]`。用于让受光地板与邻接暗面同时可见，**不能当作原场景照明结果**。修改前后的灯光和相机均在 manifest 中记录。
- 静态测试移除轨迹脚本；移动测试保留 600 帧闭环轨迹，固定 `1/60` 秒步长，验证两次运行的实际 pose sequence hash 相同。

静态套件为 120 帧预热＋240 帧测量；1440p 移动套件为 600＋600 帧。CPU CSV 保留全部测量帧；GPU 使用唯一 `gpuSample > gpuWarmupCutoffSample`，分别得到 237／597 个样本，并用同一批 ID 选择 pass 时间。GPU 全帧为 RenderGraph span，不含 Present；CPU wall 包含更新、提交、Present 和可能的 GPU 等待，两者不能相加。

## 当前完整套件的 GPU 成本

单位：ms，均值。SSGI 专属合计为追踪＋全部降噪＋GIComposite；新增 Hi-Z 单列。共享 `RasterSurfaceGuides` 的 On 成本单列，不能全部算成 SSGI 增量，因为 Off 的 TAA 也需要它。

| 场景／输出尺寸 | 追踪 | 降噪 | 合成 | SSGI 专属合计 | 新增 Hi-Z | 共享 guides（On） |
|---|---:|---:|---:|---:|---:|---:|
| Intel 原视角 1920×1080 | 0.69 | 2.03 | 0.05 | 2.78 | 0.15 | 0.22 |
| Bistro 原视角 1920×1080 | 0.90 | 2.60 | 0.07 | 3.57 | 0.14 | 0.20 |
| Intel 补充近景 1920×1080 | 0.63 | 1.99 | 0.06 | 2.68 | 0.16 | 0.19 |
| Intel 移动 2560×1440 | 1.29 | 3.65 | 0.10 | 5.04 | 0.22 | 0.36 |
| Bistro 移动 2560×1440 | 1.11 | 3.41 | 0.12 | 4.64 | 0.17 | 0.33 |
| Intel 原视角 3840×2160 | 2.84 | 8.05 | 0.22 | 11.11 | 0.40 | 0.77 |
| Bistro 原视角 3840×2160 | 2.81 | 8.09 | 0.28 | 11.18 | 0.34 | 0.71 |

对应全帧记录如下；这是各次运行的观察值，特别是 Bistro 1080p／1440p，不能直接把 On−Off 当作 SSGI 本身成本。

| 场景／输出尺寸 | GPU Off 均值 | GPU On 均值 | GPU On P95／P99 | CPU wall On 均值／P95 |
|---|---:|---:|---:|---:|
| Intel 原视角 1080p | 6.34 | 9.41 | 9.80／9.88 | 9.82／10.42 |
| Bistro 原视角 1080p | 9.35 | 9.95 | 10.46／10.57 | 28.93／30.76 |
| Intel 补充近景 1080p | 5.44 | 8.40 | 8.79／8.84 | 8.71／9.14 |
| Intel 移动 1440p | 8.42 | 14.02 | 14.41／14.55 | 14.48／15.11 |
| Bistro 移动 1440p | 9.90 | 11.45 | 11.97／12.09 | 29.48／30.84 |
| Intel 原视角 4K | 14.63 | 26.76 | 27.18／27.46 | 27.14／29.68 |
| Bistro 原视角 4K | 11.83 | 24.42 | 25.26／25.66 | 28.73／29.88 |

Bistro 1080p 的共同 pass 在 On 运行同时变快：TAA 2.44→1.55、方向阴影 2.15→1.43、GBuffer 1.78→1.12、延迟光照 1.20→0.76 ms。1440p 也存在同方向变化。这说明运行条件不匹配；可能涉及 GPU 动态频率或调度，但没有逐帧时钟遥测证明原因。不能宣称“SSGI 只增加 0.60 ms”。如需严格的整帧增量结论，应增加交错 Off／On 重复测量并记录 GPU 时钟／功耗；现有逐 pass 时间可以作为本次短测成本报告。

Bistro 的约 29 ms CPU wall 不是 SSGI 追踪耗时：1080p On 的 update 约 11.98 ms、renderOther 约 16.45 ms，而帧槽等待仅约 0.002 ms。未进一步分解这两个阶段，不归因于某个具体子系统，也不声称整个 Bistro 已达到稳定 60 FPS。4K 的 Low 仍有约 11 ms 专属 GI 成本，超出本机的轻量 60 FPS 预算。On 的 transient pool reservation 从 1080p 约 600 MiB 增至 4K 约 2016 MiB；这是池计数，不是设备总显存使用量。

### 修前／修后的滤波成本观察

对原 CSV 使用相同的唯一 GPU ID／预热排除口径，`before-intel-low` 两轮 A-trous 为 **1.088＋0.861＝1.949 ms**；`final-1080/intel-1-low.input-on` 为 **0.377＋0.412＝0.789 ms**。修前配置为半分辨率、1 ray、24 steps、2 bounces、2 A-trous、优化内核关闭；修后为半分辨率、1 ray、64 steps、1 bounce、2 A-trous、优化内核开启。它们是不同完整配置的短诊断，不能当作严格同采样、单独切换滤波内核的 A/B，也不能把全帧差值全部归因于滤波优化。

原启动默认包含全分辨率、8 rays、4 bounces、4 A-trous，现已改为上述 Low。没有对旧默认完成同条件全程测量，因此不报告旧默认到新默认的加速倍数。

### 追加 1080p 复测

`final-repeat-1080` 使用同样的 shader 与原始直射光场景，240 帧预热＋600 帧测量，得到 597 个合格唯一 GPU 样本。该次复测进一步支持约 2.8／3.6 ms 的 SSGI 专属 pass 成本，仍不构成热稳定测试。

| 场景 | 追踪／降噪／合成 | 专属合计＋新增 Hi-Z | GPU Off→On 均值 | GPU On P95／P99 | CPU wall On 均值／P95 |
|---|---:|---:|---:|---:|---:|
| Intel 1080p 复测 | 0.70／2.05／0.05 | 2.80＋0.15 | 6.518→9.512 | 9.89／9.95 | 9.91／10.50 |
| Bistro 1080p 复测 | 0.91／2.62／0.08 | 3.60＋0.14 | 9.395→10.039 | 10.52／10.75 | 29.71／32.13 |

共享 guides 的 On 均值分别为 0.204／0.198 ms。专属 GI 加新增 Hi-Z 的更精确合计为 Intel **2.9513 ms**、Bistro **3.7422 ms**，尚未把共享 guides 全部计入。Bistro 复测仍出现共同 pass 同时加速：TAA 2.45→1.56、阴影 2.20→1.44、GBuffer 1.77→1.14、延迟光照 1.20→0.77 ms，因此约 0.64 ms 的全帧差仍不能代表 SSGI 专属成本。Bistro CPU wall Off／On 均值为 29.7528／29.7118 ms，本轮没有观察到 GI 开关改善该 CPU 瓶颈；On 的 P99 为 44.64 ms、最大值 58.09 ms，长尾没有被删除或归类为加载。

同一 sRGB 暗区代理下，Intel 复测为 1,879,097 个暗像素中 1,023 个增亮≥3/255（0.0544%），Bistro 为 1,800,947 中 123,854 个（6.877%），与首次静态结论相符；这不替代视频画质检查。

## 画面证据与边界

原始 Intel 视角在关闭 IBL 后仍几乎黑；少量可见受光面不能提供完整走廊照明，屏幕外和遮挡后的几何不会被 SSGI 补全。补充近景中可以看到受光地板对拱门和墙面产生反弹。Bistro 原始直射光下，窗下和街道附近出现可见局部 GI。以上是 `runtime-manual` 观察，不是自动物理正确性测试。

自动图片统计使用 tone-mapped sRGB，暗区阈值为亮度≤8/255，经边缘内缩后统计增亮≥3/255 的像素。此 mask 含黑色天空，既不是几何 mask，也不是严格阴影 mask。

| 1080p 场景 | 暗区像素数 | 增亮≥3/255 的像素 | 占暗区比例 |
|---|---:|---:|---:|
| Intel 原视角 | 1,879,097 | 1,002 | 0.053% |
| Bistro 原视角 | 1,800,947 | 124,778 | 6.93% |
| Intel 补充近景 | 1,448,746 | 162,701 | 11.23% |

720p 补充近景另做以下数值检查，原 PNG 全部保存在证据目录：

- `final-zero-off.png` 与 `final-zero-on.png` 逐像素一致。两图均有相同的 43 个非零像素、最大通道值 130；不能描述为严格全黑。该检查支持“开启 SSGI 没有无中生有增加光照”。
- `final-bounce-low.png` 对 `final-bounce-reference.png` 的亮度 MAE 为 **0.5689/255**。参考配置为半分辨率、8 rays、128 steps、20m、1 bounce、2 A-trous；它是更高采样的 SSGI 参考，不是路径追踪真值。
- 120 帧 `final-bounce-early.png` 对 360 帧 `final-bounce-low.png` 的亮度 MAE 为 **0.6243/255**，亮度绝对差 P95 为 **1.9278/255**。这里亮度为 RGB 按 `0.2126/0.7152/0.0722` 加权，数据见 `final-regression-metrics.json`；不是 RGB MAE。这是两个静态时刻的收敛代理，不是移动视频的鬼影验证。

## 精确复现命令与验证状态

从仓库根目录执行；输出目录必须是新的。以下是本次套件命令，运行实参以及每次生成的场景路径另见各自 `manifest.json` 的 `runs[].arguments`。

```powershell
$ssgiPython = 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$ssgiEvidence = 'out/agent/ssgi-deferred-20260909'
& Tools/Agent/Test-SSGI.ps1 -OutputDirectory "$ssgiEvidence/final-1080" -PythonPath $ssgiPython -WarmupFrames 120 -MeasureFrames 240
& Tools/Agent/Test-SSGI.ps1 -OutputDirectory "$ssgiEvidence/final-intel-bounce" -PythonPath $ssgiPython -WarmupFrames 120 -MeasureFrames 240 -Scenes Intel -LightingCase OverheadDirect -IntelCloseView
& Tools/Agent/Test-SSGI.ps1 -OutputDirectory "$ssgiEvidence/final-motion-1440" -PythonPath $ssgiPython -WarmupFrames 600 -MeasureFrames 600 -Resolution 2560x1440 -MovingCamera
& Tools/Agent/Test-SSGI.ps1 -OutputDirectory "$ssgiEvidence/final-4k" -PythonPath $ssgiPython -WarmupFrames 120 -MeasureFrames 240 -Resolution 3840x2160
& Tools/Agent/Test-SSGI.ps1 -OutputDirectory "$ssgiEvidence/final-repeat-1080" -PythonPath $ssgiPython -WarmupFrames 240 -MeasureFrames 600
& Tools/Agent/Test-RenderingPaths.ps1 -Scenes Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he -GraphicsApi DX12 -Resolution 1280x720 -SwitchCycles 1 -SettleFrames 8 -TimeoutSeconds 180 -IncludeHardwareRTGI -RenderSettingsPath "$ssgiEvidence/off-final.json" -OutputDirectory "$ssgiEvidence/final-live-paths-one-bounce"
```

| 证据类型 | 命令／确切实参记录 | 状态 |
|---|---|---|
| runtime-manual | 上述前四条 `Tools/Agent/Test-SSGI.ps1` 命令 | PASS：运行、实际设置、尺寸、帧数及 hash 检查；四套件 28 份保留 stdout/stderr 未发现 error/critical 或 validation error |
| runtime-manual | 上述 `final-repeat-1080` 的 `Tools/Agent/Test-SSGI.ps1` 命令 | PASS：240＋600 帧复测与实际设置检查；保留 stdout 中未发现 error/critical 或 validation error |
| unit-test | `hectl.bat test RenderSettings` | PASS；`final-settings-test.log` |
| runtime-manual | `hectl.bat editor check -Frames 30 -RenderPath Deferred -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -TimeoutSeconds 300` | PASS；最终 1–4 bounce 修复后的 `final-editor-one-bounce-check.log` 为 build 4.5s、runtime exit=0。此前 `final-editor-retry.log` 也 PASS；更早的 LNK1104 已解除，不把失败尝试算作成功 |
| build-only | `hectl.bat editor build` | PASS：1–4 bounce 兼容修复后编译 42.7s，`final-editor-one-bounce-build.log` |
| runtime-manual | `final-regression.ps1` 中 `ssgi-forward` case：SurfaceCoverage、Forward、DX12、1280×720、180 帧、60 预热、low-64.json | PASS；`final-ssgi-forward.*`，完整 `hectl game capture` 实参由该脚本展开 |
| runtime-manual | `final-regression.ps1` 中 `ssgi-vulkan-odd` case：SurfaceCoverage、Deferred、Vulkan、1281×721、180 帧、60 预热、low-64.json | PASS；`final-ssgi-vulkan-odd.*`。存在材质顶点输出未被片元消费的 Vulkan performance warning，未计为无警告运行 |
| unit-test（CPU 数值模型） | `python out/agent/ssgi-deferred-20260909/trace_numeric_fixtures.py` | PASS；`trace_numeric_fixtures.json`。覆盖奇数 mip、视锥裁剪、透视距离、平面自交与后续有效墙命中，不替代 GPU 测试 |
| runtime-manual | `final-regression.ps1` 中 `rtgi-optimized` case | 历史失败：直接以 RTGI 启动时缺 prepared scene，未在期限内进入 Ready，记录于 `final-rtgi-optimized.log`，不计 PASS。下述单反弹校验修复没有修复这一既存启动限制 |
| runtime-manual | `Tools/Agent/Test-RenderingPaths.ps1`，实参及每次调用见 `final-live-paths/configuration.json` 和 `commands.json` | 首次实时切换测试中 RTGI Forward／Deferred 两项因旧 2–4 bounce guard 拒绝而 FAIL；其他功能断言 PASS。标为 CAPTURED／NOT_REVIEWED 的图像和序列不改记为视觉 PASS |
| runtime-manual | 上述 `Test-RenderingPaths.ps1` 的 `final-live-paths-one-bounce` 重跑 | 功能 PASS：`results.json` 为 27 PASS、0 FAIL、8 NOT_REVIEWED、3 CAPTURED；Forward／Deferred 实际切换到 HardwareRTGI，MaxBounces=1、OptimizedKernels=true。NOT_REVIEWED／CAPTURED 不代表视觉 PASS |

Forward/Vulkan/零光源/参考图的确切 capture 命令模板保存在 `out/agent/ssgi-deferred-20260909/final-regression.ps1`，逐项日志、实际 settings 和 capture 路径与 case 名对应。四套件的 `report.json` 给出完整逐 pass 均值/P95/P99，`*.csv.passes.csv` 保留原始 GPU 查询；`*.csv.settings.json` 为 shutdown 实际设置，`*.camera/camera.runtime.json` 为移动轨迹证据。

本记录尚不覆盖长期热稳定、移动视频鬼影、所有材质/场景、严格匹配频率的 Bistro 整帧增量，以及 RTGI GPU 图像正确性；直接以 RTGI 启动缺 prepared scene 的既存限制仍保留。追加复测与兼容重跑保留独立目录和表格，没有覆盖此前失败或已有结果。

## PBRTest 追加复现：视觉验收 FAIL

证据目录：`out/agent/ssgi-pbrtest-20260909/`。本次使用 `Assets/Scenes/PBRTest.he` 自带的主相机；它加载旧 Sponza 模型，并非 Intel 基准资产。复制场景只关闭 `IBLIntensity` 并移除相机输入脚本，保留原太阳方向、强度 4.99、曝光 0.31、几何和材质。未覆盖原场景或用户 Editor 设置。

确定修复：设备深度递减的射线可能在进入一个像素后穿过真实表面。旧实现只检查像素入口的厚度差，错误拒绝内部有效交点；现在在叶节点求该深度平面的解析交点，仍保留朝向、厚度、距离与视锥检查。数值反例中旧厚度差 0.421369 大于阈值 0.1，修后以 5 步命中 1 个世界单位外的表面，位置误差小于 1e-6。临时诊断输出、无阻挡积分和厚度失败后继续穿行的实验均未保留到最终 shader。

排查观察：SSGI 读取的直射 HDR 与关闭 GI 的画面位置一致；主要可见材质的 AO 为 1，并没有把 GI 乘零；世界法线方向与两侧墙面相符。相机保存视角下仅左上方小区域受光。临时 32×18 可见表面无阻挡积分也很弱，但它是有限采样的诊断估计，**不是严格上界或路径追踪真值**，不足以证明所有黑暗都符合物理。未确认此主相机与用户当时的 Editor 视角一致。

最终 Off/Low/High 结果见 `final-authored/final-preview.jpg`、`pbr-image-summary.json`、`report.json`。720p 右半屏的 RGB 均值（0–255）为 Off=0、Low=0.000645、High=0.000028；Low 仅 6 个像素的最大通道超过 3。人工对照仍几乎全黑，**未达到用户要求的“Low 下明显 GI”**。

最终短测（RTX 3060 Laptop；1280×720；每次 120 帧预热、240 帧测量）：Low 全帧 GPU Off 1.790→On 3.406 ms，增量约 1.616 ms；High Off 1.803→On 31.586 ms，增量约 29.783 ms。High 使用全分辨率、8 rays、4 bounces；这些是不同质量配置的短时全帧对照，不能当作独立 pass 成本或长期稳定性能。前面的诊断运行期间用户 Editor 曾同时运行，不用于此性能结论。

`Test-SSGI.ps1` 新增 `-Scenes PBRTest`，使用同样的实际设置校验、源文件 hash 和成对截图流程；PBRTest 没有标准运动轨迹，明确拒绝搭配 `-MovingCamera`。以下命令已执行：

```powershell
& Tools/Agent/Test-SSGI.ps1 -Scenes PBRTest -OutputDirectory out/agent/ssgi-pbrtest-20260909/final-authored -Resolution 1280x720 -WarmupFrames 120 -MeasureFrames 240 -QualityPaths out/agent/ssgi-pbrtest-20260909/low.json,out/agent/ssgi-pbrtest-20260909/high.json -PythonPath C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe
.\hectl.bat editor check -Frames 30 -StartupScene Assets/Scenes/PBRTest.he -RenderPath Deferred -GraphicsApi DX12 -StrictGraphicsApi -RenderValidation -TimeoutSeconds 300
& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe out/agent/ssgi-deferred-20260909/trace_numeric_fixtures.py
```

- `runtime-manual`：第一条命令的构建、4 次受控运行、配置/hash 校验 PASS；视觉目标 FAIL，不把脚本成功等同于效果达标。
- `runtime-manual`：第二条 Editor 构建与运行 PASS，退出码 0，记录 `final-editor.log`；该启动检查不替代无环境光视觉验收。
- `unit-test`（CPU 数值模型）：第三条 PASS，含递减深度交点反例；不执行 GPU shader。

当前仍需确认用户实际 Editor 相机与照明状态，再决定剩余问题属于追踪/重建错误还是需要屏幕外光照数据。此处不宣告 PBRTest 修复完成。
