# 当前渲染实施与验收结果

更新：2026-09-08。**整体任务尚未完成；原生 4K 稳定 60 FPS：FAIL。** 实际实现、官方资产和运行证据已落盘，不能把功能检查、热稳定代理或截图当作完整验收。未提交、推送或发布仓库。

架构、双路径功能矩阵与基础实施说明见 [Rendering-Implementation.md](Rendering-Implementation.md)。本文件中的较新结果优先于历史记录；历史失败文件保留。

2026-09-08 晚间追加：编辑器浏览 `Assets/Scenes` 时的 `Primitive2D` 未声明 GTAO 读取已修复，并修正导入相机转换导致的 Sponza 初始视点偏移。场景访问方式、独立回归命令与日志位置见 [Viewing-RenderingScenes.md](Viewing-RenderingScenes.md)。此次编辑器修复没有重新执行下面的持续 4K 性能基准；不要将新增编辑器截图用作性能证据。

## 本轮代码与资源

- `Rendering.Presentation.FramesInFlight` 支持 2/3，默认 3，重启生效，独立于路径和后端。RuntimeRenderer、RenderDevice、DX12/Vulkan 使用实际帧数；资源池、UI 和 VFX 继续消费设备帧数。运行时拒绝不匹配的帧数事务。DX12 的交换链缓冲数量随配置变化；Vulkan 的帧槽和交换链图像索引保持独立。
- Vulkan 的 Present 等待信号量改为每交换链图像一份，避免以帧槽完成错误推断 Present 已消费信号量；构造失败回收已经创建的资源。平台原生代码仍位于 `Runtime/Platform`。依据 [Khronos semaphore reuse guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)。既有重建/退出 idle 流程保留；没有新增 swapchain-maintenance 呈现完成栅栏机制。
- IBL 烘焙成功后释放 HDR 源纹理的托管引用，保留生成的环境贴图；失败时依据提交序号退休源资源，保留原有 GPU 等待。
- SVGF 的候选缓存保留完整精度、25 taps、迭代次数及工作分辨率。前三次 A-trous 使用缓存；第三次使用按采样间距组织的线程映射。第四次缓存在实测中变慢，已恢复原实现。优化默认关闭，详见 [Rendering-SvgfOptimization.md](Rendering-SvgfOptimization.md)。
- DX12 通过公共 RHI 采样实际进程显存预算，原生 DXGI 调用封装在平台层。Vulkan 明确报告不可用。采样文件区分加载与正式帧，分析器排除预热，保留全程峰值；缺字段不按零使用量解释。
- 基准工具对启动时暂不可读的进程路径重试，校验路径及启动时间，拒绝 PID 复用。热稳定窗口截断于实际帧数结束日志，排除退出阶段；未放宽阈值。
- Bistro 原导入遗漏的官方 `san_giuseppe_bridge_4k.hdr` 已恢复到独立派生场景，强度为官方脚本指定的 10，原 1309 个实体保留。HDR SHA-256：`5eb82f21bced3f7e901774083cfee1a05fd2bcf22f56ff699f3171c1eec65a50`。旧缺光场景的性能不是有效高画质证据。复现入口与许可记录见 [资产工具说明](../Assets/RenderingBenchmarks/README.md)。

## 实测配置与结果

本机 RTX 3060 Laptop 6 GiB、i7-11800H、约 16 GiB RAM；Windows Balanced，厂商性能模式未知。供电与 GPU 功耗、温度、时钟保存在各次 configuration/telemetry 文件中，没有使用桌面显卡数据。

HGame，DX12，原生 3840×2160，VSync/限帧关闭；没有升采样、动态主分辨率、插帧或 VRS。完整导入几何，LOD 关闭，显式 2K BC 纹理，原生 TAA；GTAO/SSR/SSGI 半分辨率，GTAO 4 slices/每侧 8 steps，SSGI 2 rays/32 steps，SVGF 4 次 A-trous，SSR 32 steps/5 次 refinement，Bloom 四分之一分辨率，手动曝光 0.31。没有通过关闭效果或删减场景证明性能。

`Native4KComparableReuse.json` 为相同功能比较配置。`Native4KTwoFramesCandidates.json` 保留相同质量参数，启用两帧、瞬态复用、TAA gather、SSR guide cache、阴影批处理、SVGF 缓存；其他开关由输入 JSON 与实际设置快照逐项核验。它是显式候选配置，没有暗改默认值。

以下均为 2400 个正式 CPU 帧，GPU 使用对应完成样本并去重；CPU wall 包含提交、Present 和 GPU 反压，不能与 GPU 时间相加。所有行均为 **2400/2400 帧超出 16.667 ms，60 Hz FAIL**。

| 场景 / 配置 / 路径 | CPU 平均 / P95 / P99 ms | GPU 平均 / P95 / P99 ms | 预热帧 |
| --- | --- | --- | --- |
| Sponza / ComparableReuse / Deferred | 53.001 / 56.650 / 57.730 | 52.955 / 53.772 / 54.067 | 6000 |
| Sponza / ComparableReuse / Forward | 55.308 / 58.704 / 59.946 | 55.263 / 56.181 / 56.536 | 6000 |
| Sponza / TwoFramesCandidates / Deferred | 46.824 / 50.214 / 51.231 | 46.299 / 46.966 / 47.260 | 6000 |
| Sponza / TwoFramesCandidates / Forward | 49.177 / 52.651 / 53.785 | 48.660 / 49.364 / 49.714 | 12000 |
| 修正环境光 Bistro / TwoFramesCandidates / Deferred | 41.360 / 42.912 / 45.318 | 40.992 / 41.520 / 41.844 | 6000 |
| 修正环境光 Bistro / TwoFramesCandidates / Forward | 43.450 / 45.272 / 47.973 | 43.083 / 43.638 / 43.953 | 6000 |

上述六行在排除结束帧所在整秒及后续退出样本后，热稳定代理均 PASS。该代理要求两个相邻 120 秒窗口温差 ≤2°C、温度斜率绝对值 ≤0.5°C/min、功耗漂移 ≤10%、时钟漂移 ≤5%。它不是全部硬件状态的证明。原 Sponza 候选 Forward 6000 预热运行在严格截断后斜率 0.519，属于 FAIL；使用 12000 预热的新运行替代，原文件未删除。所有长帧仍保留，工作负载稳定代理没有放宽。

候选配置的进程本地显存全程峰值：Sponza Deferred/Forward 3676/3669 MiB，Bistro 4379/4372 MiB；正式采样期均未超 DXGI 预算。进程 peak working set 分别约 0.950/0.943/2.049/2.051 GiB，采样 private bytes 峰值约 4.557/4.555/5.799/5.644 GiB。它们不等于整机 RAM 峰值，整机内存峰值尚无完整采样证据。平均场景表上传 Sponza 8020 B/frame、Bistro 30520 B/frame；draw/batch、可见对象、各 Pass 和峰值列保留在 CSV/result.json。

主要证据目录均位于 `out/agent/rendering-evidence/`：

- `intel-sustained-deferred-8400`、`intel-sustained-forward-8400`。
- `intel-final-candidates-deferred-8400-2`、`intel-final-candidates-forward-14400`。
- `bistro-final-candidates-deferred-8400`、`bistro-final-candidates-forward-8400`。

旧运行的 `thermal-boundary-verification.json` 对退出阶段作了独立复核；最新 14400 帧运行的 result.json 已直接采用结束帧边界。旧无阶段显存 CSV 的 `memory-analysis-phase-correction.json` 优先于原误混加载期的子集统计。

## 精确复现命令

在工程根目录执行。每次使用新的输出目录；完整原始启动命令、资产/轨迹/配置/二进制哈希、实际设置、相机 tick 和供电信息也保存在对应目录的 configuration.json 中。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Benchmark-Rendering.ps1 -StartupScene Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he -BenchmarkInputPath Project/Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/benchmark_input.json -RenderSettingsPath Project/Assets/RenderingBenchmarks/Configurations/Native4KTwoFramesCandidates.json -RenderPath Forward -GraphicsApi DX12 -Resolution 3840x2160 -WarmupFrames 12000 -MeasureFrames 2400 -TimeoutSeconds 1200 -ThermalWindowSeconds 120 -PowerModeNote 'Vendor performance mode unknown; Windows Balanced; AC and hardware telemetry recorded by harness' -OutputDirectory out/agent/rendering-evidence/intel-final-candidates-forward-14400
```

其余候选行使用同一命令：Deferred 主场景预热 6000；Bistro 将两个输入路径改为 `BistroExterior2048BCGPUMaskFixedEnvironment`，预热 6000，路径按表选择；各行原样命令已由工具记录。不要将表中原输出目录覆盖重跑。

| 证据类型 | 精确命令 | 结果与边界 |
| --- | --- | --- |
| build-only + runtime-manual | `hectl.bat game check -GraphicsApi Vulkan -StrictGraphicsApi -RenderValidation -Frames 120 -TimeoutSeconds 180 -Resolution 3840x2160 -StartupScene Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he -RenderSettingsPath Project/Assets/RenderingBenchmarks/Configurations/Native4KTwoFramesCandidates.json -RenderStats -RenderStatsPath out/agent/rendering-evidence/vulkan-image-semaphore-check.csv` | PASS；最新信号量版本，实际两帧 |
| build-only + runtime-manual | `hectl.bat editor check -Frames 30 -TimeoutSeconds 300 -GraphicsApi DX12 -RenderValidation` | PASS；启动/退出 smoke，不是 Editor 性能 |
| unit-test | `hectl.bat test RenderGraphResourcePool` | PASS |
| unit-test | `hectl.bat test RenderSettings` | PASS，含帧数范围/持久化/重启计划 |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-RenderingBenchmarkAnalysis.ps1` | PASS，含阶段/时间边界、缺失数据、P95/P99、样本关联 |
| build-only / codegen | `hectl.bat codegen run -CodeGenHost DevGame -Configuration RelWithDebInfo --json` | PASS；changed=0、deleted=0，237 个输出 hash/mtime/路径零扰动 |
| build-only / codegen | `hectl.bat codegen run -CodeGenHost Editor -Configuration RelWithDebInfo --json` | PASS；同上 |
| runtime-manual / performance | 上述六组 configuration.json 中的精确 command | 输入与分辨率 PASS；热稳定代理 PASS；60/90 Hz 帧预算 FAIL；画质未全验收 |

codegen 的独立前后清单与结果在 `final-codegen-game.*` / `final-codegen-editor.*`；验证入口是 `powershell -NoProfile -File out/agent/rendering-evidence/verify-codegen-noop.ps1 -HostKind DevGame`（Editor 替换 HostKind）。未手改生成代码。

## 切换、故障与画面证据

主二进制 SHA-256：`117C654D324B1DCBBB408EBADB823CECCEF7FCE39CF1B0AFF82B1A9C32621CEA`。以下脚本的 commands.json 保存每次精确事务、步进、Resize、重载和捕获命令。

- `paths-two-frames-cache-dx12-2`：64 PASS、6 CAPTURED、22 NOT_REVIEWED、1 NOT_APPLICABLE、0 FAIL；含两个场景、三轮切换、RTGI、多视图、骨骼、热更新、Resize、重载和内存采样。该较早二进制使用 SVGF v2；后续 step4 的独立数值对比另记。
- `paths-two-frames-step4-vulkan`：80 PASS、9 CAPTURED、26 NOT_REVIEWED、2 NOT_APPLICABLE、3 UNSUPPORTED、0 FAIL；三个场景含 VFXStressTest。UNSUPPORTED 为实际 Vulkan HardwareRTGI 能力不可用。VFX 场景运行不等于全部粒子画质/覆盖已审查。该版本早于 Present 信号量修复。
- 最新 `paths-vulkan-image-semaphore`：27 PASS、3 CAPTURED、8 NOT_REVIEWED、0 FAIL；两轮路径切换、Resize、多视图、重载、热更新。最新 `fault-vulkan-two-frames-final`：3 PASS，包含分配后的回滚与相同请求重试。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-RenderingPaths.ps1 -Scenes Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he -GraphicsApi Vulkan -Resolution 3840x2160 -SwitchCycles 2 -SettleFrames 12 -RenderSettingsPath Project/Assets/RenderingBenchmarks/Configurations/Native4KTwoFramesCandidates.json -ExistingBuildSha256 117C654D324B1DCBBB408EBADB823CECCEF7FCE39CF1B0AFF82B1A9C32621CEA -OutputDirectory out/agent/rendering-evidence/paths-vulkan-image-semaphore
```

故障入口使用同一命令，加入 `-InjectCandidateFailure`、改为 `-SwitchCycles 1` 和新的输出目录。新构建须使用其真实 SHA，不能沿用旧 SHA 冒充验证。

`native-bistro-two-frames-captures` 保存最新两路径原生 PNG、实际设置和 180 tick 轨迹哈希。与较早三帧版本比较：Deferred 最大 RGB 差 1，99.99656% 通道相同；Forward 最大差 5，99.89034% 相同。四个相机序列哈希一致。它是跨版本数值证据，不是严格单变量 A/B 或完整画质 PASS。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Capture-RenderingBenchmark.ps1 -BenchmarkInputPath Project/Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BCGPUMaskFixedEnvironment/benchmark_input.json -RenderSettingsPath Project/Assets/RenderingBenchmarks/Configurations/Native4KTwoFrames.json -GraphicsApi DX12 -Resolution 3840x2160 -Frames 180 -OutputDirectory out/agent/rendering-evidence/native-bistro-two-frames-captures
```

SVGF 原生 4K 与 Vulkan 奇数尺寸对比最大差 1；具体版本、命令和失败候选见优化文档及 `svgf-strided-*` / `svgf-step4-*` 日志。没有再以截图外观代替性能或运动验证。

## 仍未完成与限制

1. 原生 4K 60/90 FPS 均未达到。Sponza 候选 GPU 中 SSGI 约 5.79 ms、TAA 5.33 ms、后两次 A-trous 3.47/4.03 ms、阴影 3.16 ms、GTAO 2.98 ms；表中是整帧时间，不能把这些相加当成新的整帧测量。
2. 完整静态与运动画质审查、复杂场景运行时切换的首次卡顿/稳定成本/显存峰值，仍未覆盖全部验收组合。定向回归不能替代这些项。整机 RAM 峰值、端到端输入延迟也未完整测得。
3. 独立 Render Thread、并行命令录制与异步队列未实施。当前 GPU critical span 约 41–49 ms；即使 CPU 工作完全隐藏，仍不能达到 16.667 ms。CPU 已测有显著帧槽等待。现有瞬态复用依赖单有序命令流，GI/TAA 依赖当帧光照/表面结果；并行队列还需重新证明资源生命周期。没有未经整帧收益验证就默认启用这些机制。WPR 时间线曾受 OS 0xc5585011 策略阻止，现有细分 CPU/GPU 时间戳不等于完整 ETL 时间线。
4. 保留 DX12 现有 RTGI 的受限次级几何/材质覆盖；Vulkan 本次实际 RTGI 不可用。透明与 Effekseer 在不透明 TAA 后合成，完整透明运动矢量/时域质量不宣称支持。完整 RT 材质/动画、光追反射、复杂体积和高级材质模型未作为已实现能力。
5. NVRHI Vulkan OOM 原始纹理释放补丁仍只位于 `Tools/Patches/NvrhiVulkanTextureFailure.patch`，未修改第三方源码；仓库要求的明确授权尚未收到。避免 OOM 的通过运行不代表失败路径已修复。

已尝试并保留失败证据：单资源银行预分配的 Vulkan OOM、第四次 A-trous 缓存变慢、启动配置优先级造成的回归工具失败、进程路径暂不可读、无阶段显存样本误归类，以及含退出阶段的热稳定误判。相应修复和复测均分版本记录。当前成果远超接口骨架，但仍不能宣称整个任务验收完成。
