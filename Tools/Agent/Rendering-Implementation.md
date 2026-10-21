# 现代渲染实施与验收状态

最新构建、热稳定边界修正、两帧候选性能、精确命令与剩余项以 [Rendering-CurrentValidation.md](Rendering-CurrentValidation.md) 为准；本文保留架构矩阵及历史实施记录。

更新：2026-09-08。工程内已有实际代码、导入资产和运行证据，但整体交付尚未通过验收。RTX 3060 Laptop 6 GiB 上，当前原生 4K 可比配置下，Sponza 热稳定代理通过的持续测量为 Deferred52.955 / Forward55.263 ms/GPU 帧，稳定 60 FPS **FAIL**。Bistro 环境光导入遗漏已修正，旧缺光数据不再作为有效高画质基准。尚未审查的截图不构成画质 PASS。

## 数据与主路径

`RenderWorld` 提供不可变帧快照；现有 GPU Scene 继续管理对象、材质和纹理表。`SceneGeometryArena` 常驻共享几何，dirty range 更新保留稳定索引。没有引入第二套场景或材质资产体系。`SubmittedMotionHistory` 持有实际提交的快照，以对象身份、网格身份和场景世代校验前帧，避免未提交的模拟帧污染速度。中止帧不推进历史，多视图独立持有提交历史。

默认路径是 Deferred；`Rendering.Path` 与 `Rendering.Backend` 独立。Forward 的 `OpaqueForwardHDR` / `SkinnedForwardHDR` 完成不透明物体照明，独立于 Deferred Lighting。两条路径共享材质求值、纹理、蒙皮、裁剪、双面规则和多灯数据。透明前向、2D 和 Effekseer 在不透明 TAA 后合成：保留原有覆盖，但不宣称透明物体具有完整时域抗锯齿或透明运动矢量。

普通光栅表面数据不再从 RayTracingScene 的受限几何集合产生。RT 场景继续作为消费者；现有 RTGI 的次级材质/动画覆盖仍受限制，不宣称完整动态 RT 材质、光追反射或任意自定义变形支持。

### GBuffer ABI

| 存储 | 格式 / 字节每像素 | 语义 |
| --- | --- | --- |
| BaseColorMetalness | RGBA8_UNORM / 4 | 线性基色，金属度 |
| NormalRoughnessAo | RGBA8_UNORM / 4 | 世界空间八面体法线、感知粗糙度、材质 AO |
| EmissiveModel | RGBA16_FLOAT / 8 | 线性 HDR 自发光，Lit=0 / Unlit=1 |
| MotionDepth | RGBA16_FLOAT / 8 | previousUV-currentUV，前帧 device/view depth |
| SurfaceId | RG32_UINT / 8 | 精确实例/图元身份，实例 0xffffffff 表示背景 |

五个 MRT 合计 32 B/px，不含深度和消费者辅助资源。位置从深度重建。运动以归一化 UV 为单位，正方向是到前帧的位置；不以 depth 非零代替有效身份。天空重投影消除相机平移、保留旋转和抖动。新对象、场景/网格替换、镜头跳变、重载及材质发布会拒绝或重置相应历史。启动完成时重置 GI/TAA 采样序列，消除加载期间零模拟步长帧数的不确定性。

Deferred 使用该 GBuffer，并为 GI/TAA 生成所需解码资源。Forward 只在所启用效果需要表面输入时绘制相应引导数据；无相关效果不承担这套绘制。辅助引导仍有可观显存和带宽成本，32 B/px 不是整帧总成本。

## 功能与验证边界

| 能力 | Forward / Deferred 实现 | 当前证据与限制 |
| --- | --- | --- |
| 标准 Lit / Unlit，PBR 贴图，Mask，双面 | 共享材质求值 | 定向场景、两路径运行；全部静态/运动画质审查未完成 |
| 骨骼与物体速度 | 使用实际提交的前帧快照和骨骼调色板 | 提交历史单元测试、骨骼变化诊断；不以变化哈希代替运动画质 |
| 自定义 Surface | 共享 Forward/GBuffer/Depth/Shadow 变体 | 材质热更新与独立视图世代检查；任意新变形需相应前帧定义 |
| 方向/点/聚光灯 | 共用 LightBuffer、BRDF、IBL 和 3D cluster mask | cluster 不依赖不透明深度，避免透明物体漏灯 |
| 局部阴影 | 8 个聚光视图 + 2 个点光源六面，共 20 个 512² 层 | 预算外光源继续照明且不投局部阴影；视图可见列表及脏更新 |
| TAA | 原生分辨率；相机、对象、骨骼及天空引导 | Vulkan 新历史初始布局错误已修复；所有运动边界仍需图像审查 |
| GTAO | 半分辨率积分与边缘保留滤波、上采样 | 不施加到直接光和自发光；可比配置 4 slices、每侧 8 steps |
| SSR / probes | 半分辨率 SSR，局部探针与视差修正，环境回退 | 使用替换组合避免镜面重复叠加；反射回退定向画质待完整审查 |
| SSGI / SVGF | 普通材质与角色光栅引导，半分辨率 | 可比配置 2 rays / 32 steps，4 次 ATrous；当前主要 GPU 瓶颈 |
| Bloom / 调色 / 曝光 | quarter-resolution Bloom，手动/自动曝光 | 可比配置使用手动曝光 0.31；自动曝光不混入可比计时 |
| 透明、玻璃和 Effekseer | 现有前向合成和折射链保留 | 不透明 TAA 后处理，透明时域支持有限 |
| GPU Driven / LOD / 遮挡 | 常驻几何、dirty rows、间接提交、资源 LOD、保守遮挡 | 按独立开关测量；未获整帧收益者不默认启用 |

## 切换与资源峰值

`SceneRendererSwitch.inl` 为全部活动视图准备候选 shader、PSO、图、效果和资源，全部成功才交换。失败保留原配置；提交操作交换已经分配的状态，旧引用按提交 fence 退休。运行脚本覆盖连续切换、SSGI、Resize、镜头跳变、重载、独立视图、材质热更新和真实分配后的故障注入。

Vulkan 原生 4K 实测暴露两个失败方案：独立预分配全部候选帧槽导致 OOM；只预分配一个候选帧槽仍可能在提交后增长失败。因此当前修复恢复全部帧槽预检，通过 `SeedCompletedTransients` 引用已完成旧池中兼容的瞬态分配，候选元数据独立且准备期间不执行候选图。为此切换边界必要时等待 GPU 完成，计入 preparation/transaction 时间；正常逐帧路径不增加等待。持久资源与导入历史不通过该接口共享。旧池与候选池名义字节相加会重复计算共享分配，不能当作 VRAM 峰值。此修复的 Vulkan 完整运行回归与故障注入已通过；DX12 扩展回归及完整画质审查仍待完成。

NVRHI Vulkan 原始纹理分配失败路径存在未释放已创建 VkImage 的问题。`Tools/Patches/NvrhiVulkanTextureFailure.patch` 是已通过应用检查的最小审查补丁；第三方文件尚未修改，等待仓库要求的明确授权。不得把避开 OOM 的运行当作该失败路径已经修复。

## 当前测量

硬件为 MECHREVO X8 Pro Series GM7TG0R、i7-11800H、RTX 3060 Laptop 6 GiB。Windows 电源计划为 Balanced；厂商控制中心模式尚未核实。遥测短运行约 129–130 W、81–84°C，设备级统计可能包含其他进程。所有数据均来自本机 HGame，编辑器检查另行记录。

以下是固定 600 帧相机周期、600 帧预热 / 1200 帧测量的短测。主分辨率 3840×2160，无升采样、插帧、动态分辨率或 VRS；完整几何、显式 2K BC 纹理、LOD off、TAA、半分辨率 GTAO/SSR/SSGI、quarter-resolution Bloom。开启瞬态复用，其他候选优化默认 off。所有超预算和长帧保留。更早二进制之间的结果不是严格同二进制 A/B。

| 证据目录（out/agent/rendering-evidence） | CPU wall 平均 / P95 / P99 ms | GPU graph 平均 ms | 60Hz |
| --- | --- | --- | --- |
| intel-reuse-reference-repeat-1800 | 52.140 / 55.924 / 56.849 | 52.097 | 1200/1200 超预算，FAIL |
| intel-reuse-forward-1800 | 54.61 / 58.215 / 59.509 | 54.57 | 1200/1200 超预算，FAIL |
| bistro-reuse-deferred-1800（缺环境光，画质验收无效） | 51.116 / 54.058 / 59.391 | 51.063 | 1200/1200 超预算，FAIL |
| bistro-reuse-forward-1800（缺环境光，画质验收无效） | 55.18 / 58.542 / 62.521 | 55.14 | 1200/1200 超预算，FAIL |

CPU wall 包含 GPU backpressure，不是 CPU 忙时；CPU/GPU 不能相加。Intel 四次 ATrous 约 15.9 ms、TAA 约 6 ms、SSGI 约 5.8 ms。旧缺环境光 Bistro 的 GBuffer 约6.2ms，仅保留作问题诊断。瞬态复用前后 Intel GPU 约 101→53 ms，阴影约 44→3 ms，与缓解驻留压力相符，但不是热稳定的严格 A/B，不能宣称全部差值为分配器优化收益。

同场景短测：shadow batching 将阴影调用 1604→48（12 multi-draw groups），整帧 GPU 约 52.035 ms；TAA gather 约 51.918 ms；SSR guide cache 约 51.689 ms。收益有限，组合配置仍在回归。SVGF 优化候选出现未解释的长尾，保持关闭。已有遮挡候选只剔除少量对象，没有整帧收益证据；不默认启用。

独立 Render Thread / RHI Thread 尚未实施。当前测量显示 GPU 阶段已超过预算数倍，CPU 大量时间等待帧槽；现有 mutable 提交与资源所有权也不支持直接把函数移到另一个线程。不会把线程名称当作优化成果。WPR CPU+GPU 时间线采集受 OS `0xc5585011` 性能分析策略限制而 FAIL，未修改系统策略或停止其他 WPR 会话。当前只有引擎分阶段 CPU 计时和 GPU 时间戳证据，不能声称完整 CPU/GPU 时间线已经采集。Async Compute、异步上传和 VRS 未获整帧对比收益证据，未设为默认。

## 可重复入口与证据

在工程根目录运行；独立构建目录由 hectl 管理，不复用 VS。精确每次启动命令、二进制/资产/配置 hash、CPU/GPU CSV、设置快照、遥测和相机运行 tick 都保留于对应证据目录。

```powershell
.\hectl.bat game check -Frames 120 -StartupScene Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he -RenderPath Deferred -GraphicsApi Vulkan -Resolution 3840x2160 -RenderValidation -TimeoutSeconds 300
.\hectl.bat editor check -Frames 30 -TimeoutSeconds 300
.\hectl.bat test RenderGraph -TimeoutSeconds 240
.\hectl.bat test RenderGraphResourcePool -TimeoutSeconds 240
.\hectl.bat test RenderWorldSubmission -TimeoutSeconds 240
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-RenderingPaths.ps1 -GraphicsApi DX12 -Resolution 3840x2160 -SwitchCycles 3 -SettleFrames 12 -TransientTextureReuse
```

基准精确命令和持续运行入口见 `Rendering-Benchmark.md`。输入必须显式记录所有开关；不能用新的推荐配置替换已失败的可比配置。资产来源、许可和转换命令见 `Project/SourceAssets/RenderingBenchmarks/IMPLEMENTATION_REPORT.md`、Bistro README 与 `Tools/Assets/RenderingBenchmarks/README.md`。

已有 `unit-test` PASS：RenderWorldSubmission、RenderGraph、RenderGraphResourcePool、DdsTextureData、MeshResourceValidation、RenderWorldExtractorMeshPath、RuntimeSceneRequest、RenderSettings，完整各次命令与日志保留。`runtime-manual`：Vulkan 历史布局修复后 180 帧 native4K 检查 PASS；Editor 30 帧检查 PASS（早于当前资源交接修复）。DX12 SurfaceCoverage 单轮复用回归 22 PASS / 3 CAPTURED / 6 NOT_REVIEWED / 0 FAIL。早期 Vulkan 回归有真实 FAIL；完成资源池交接修复后的独立回归结果见后文，旧失败证据保留。

静态 `reuse-epoch-on.png` 与 `reuse-epoch-off.png` RGB 通道 99.99673675% 完全相同、最大差 1/255；它只支持这组静态复用等价性。动画、遮挡显露、玻璃、反射及复杂场景运动稳定性仍需独立证据。

剩余硬验收：当前资源交接的全后端回归与故障注入、完整静态和运动画质审查、热稳定持续测量、当前源码最终 Editor/受影响目标编译和 codegen 零扰动检查、原生4K性能目标及进一步有依据的优化。未提交、推送或发布仓库。

## 12:35 追加运行证据

资源池共享版本 `DDE26D041D94D4B5988F80CA75F77D7E267C52036913F7E654DB00EA05F5205B` 已完成 Vulkan 原生 4K 两场景三轮回归：53 PASS、6 CAPTURED、18 NOT_REVIEWED、1 NOT_APPLICABLE、0 FAIL。证据目录 `paths-shared-banks-vulkan-native4k` 的 `commands.json` 记录所有精确调用；入口为 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-RenderingPaths.ps1 -GraphicsApi Vulkan -Resolution 3840x2160 -SwitchCycles 3 -SettleFrames 12 -TransientTextureReuse -RenderSettingsPath C:/WorkSpace/HEngineDev/out/agent/rendering-evidence/measured-candidates.json -ExistingBuildSha256 DDE26D041D94D4B5988F80CA75F77D7E267C52036913F7E654DB00EA05F5205B -OutputDirectory C:/WorkSpace/HEngineDev/out/agent/rendering-evidence/paths-shared-banks-vulkan-native4k`，证据类型 runtime-manual。相同内容已保存为 `Project/Assets/RenderingBenchmarks/Configurations/Native4KMeasuredCandidates.json`。

单场景 `-InjectCandidateFailure` 的 `fault-shared-banks-vulkan-native4k` 同样 runtime-manual PASS：真实资源准备后的注入失败保留旧 revision/hash/执行 Pass，旧图继续渲染，原配置请求再次提交成功。未审查的画面仍不计画质 PASS。Vulkan 第三方 OOM 释放缺陷仍未修复。

`intel-sustained-deferred-8400` 已完成 6000 帧预热 / 2400 帧采样，Native4KComparableReuse 配置。固定轨迹、原生分辨率、实际质量参数检查 PASS；热稳定代理 PASS（425 个遥测样本，末两窗口温差 0.089°C、末窗口温度斜率 -0.044°C/min）。CPU wall 平均 53.001 / P95 56.650 / P99 57.730 / max 60.008 ms；GPU graph 平均 52.955 / P95 53.772 / P99 54.067 / max 54.622 ms。2400/2400 帧超 60Hz，strict60FrameBudget FAIL。设备显存采样峰值 5484 MiB；进程 peak working set 1,019,916,288 B、采样 private bytes 峰值 6,063,271,936 B；平均场景表上传 8020 B/frame。

精确命令：`powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Benchmark-Rendering.ps1 -StartupScene Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he -BenchmarkInputPath Project/Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/benchmark_input.json -RenderSettingsPath Project/Assets/RenderingBenchmarks/Configurations/Native4KComparableReuse.json -RenderPath Deferred -GraphicsApi DX12 -Resolution 3840x2160 -WarmupFrames 6000 -MeasureFrames 2400 -TimeoutSeconds 900 -ThermalWindowSeconds 120 -PowerModeNote 'Vendor performance mode unknown; Windows Balanced; AC and hardware telemetry recorded by harness' -OutputDirectory C:/WorkSpace/HEngineDev/out/agent/rendering-evidence/intel-sustained-deferred-8400`。这是 runtime-manual 实测，不能将热稳定 PASS 或输入 PASS 当作帧预算 PASS。Forward 同配置持续测试已完成：CPU55.308/P9558.704/P9959.946ms，GPU55.263/P9556.181/P9956.536ms；热稳定代理 PASS，全部2400帧超60Hz预算。证据 intel-sustained-forward-8400；命令与上一行相同，只替换 RenderPath/输出目录为 Forward 对应值，精确执行参数另存 configuration.json。

## 13:50 显存、资产与候选内核更新

DX12 共享资源池版本两场景三轮扩展回归（含 RTGI、进程/设备内存采样）完成：62 PASS、6 CAPTURED、22 NOT_REVIEWED、1 NOT_APPLICABLE、0 FAIL。精确 commands.json 和内存采样保存在 paths-shared-banks-dx12-native4k。它早于本次两帧配置修改，不能代替后者回归。

Bistro 官方脚本指定的 san_giuseppe_bridge_4k.hdr、强度10已在独立派生场景恢复，原1309实体保持不变；原始与输出HDR的SHA256为5eb82f21bced3f7e901774083cfee1a05fd2bcf22f56ff699f3171c1eec65a50。使用 Trajectories/BistroExterior2048BCGPUMaskFixedEnvironment/Benchmark.he。两路径固定180tick native4K捕获、哈希、实际设置、NVRHI验证通过；旧图93.6248%像素亮度<=8，修正后0.17749%。修正图尚未人工审查；数字亮度恢复不能当作完整画质PASS。

DX12 新增低频进程级 DXGI budget/usage 采样；平台原生调用位于 Runtime/Platform。Vulkan目前明确报告不可用。bistro-environment-budget-deferred-1800 的全程143样本中111超过预算，最大用量5597.906MiB、最小预算4728.238MiB；不能把6GiB物理容量当作进程可用预算，也没有直接页错误时间线证据。分析器按frame过滤预热，区分缺文件/不支持，合成数据契约测试PASS。

IBL烘焙成功后释放不再使用的源纹理托管引用，保留派生环境贴图；原GPU完成等待不变，失败路径按已提交序号退休。hdr-source-release-check.log：HGame构建与DX12修正Bistro120frames NVRHI运行PASS。

Rendering.Presentation.FramesInFlight支持2/3，默认3，重启生效。DX12实际swapchain buffer和frame context按配置，Vulkan frame slot独立于driver所需swapchain image数量；所有资源池消费者沿用设备实际帧数。Native4KTwoFrames.json只改变该参数。codegen run changed=0/deleted=0；RenderSettings单元测试PASS；DX12/Vulkan定向4K120frames运行PASS，Vulkan实际报告2帧。当次二进制A786E21C22713F6F8739866D39FA0DAF929DC00DB166B058BBCE418DDBC3A1E4。完整切换/Resize回归及复杂场景收益测量进行中，默认尚未改变。

SVGF v2缓存仅优化前两次ATrous邻域读取，保留原25tap和采样率。DX12 4K / Vulkan奇数分辨率数值对比最大RGB差1，定向GPU25.521->20.379ms；Sponza短测46.906ms，仍全部超60Hz。具体局限与证据见Rendering-SvgfOptimization.md；默认保持关闭。
