# 原生 4K 阴影、GI 与性能改善

阴影与GI改善、四组正式配置、跨路径、Game恢复及Editor均已完成记录范围内的运行和视觉验收；两套生成代码的新鲜度检查通过。四组性能数据完整保留，正式配置按实测收益选择，验证工作流已完成收尾。

## 保持的条件

完整 Sponza/Bistro，DX12 Deferred，主渲染 3840×2160。Low 沿用原有半尺寸 GI（1920×1080）、常态两射线、两次去噪迭代；没有用更低主分辨率、简化场景或降级 LOD 换取本轮收益。120/90 FPS 只作参考。

测试设备：RTX 3060 Laptop 6 GB（驱动 591.74），i7-11800H，约16 GB内存，Windows 11 build26200。这是本机、这些配置和有限轨迹的结果。

## 画质改善

- 阴影：连续 PCF 默认开启，减少离散阴影取样引起的边缘阶梯和运动跳变。两场景四个位置的新旧正常画面共256张，另有64张诊断图。诊断图变化明确，原灯光下正常合成画面收益较细微。
- GI 历史：对几何连续、但同一三角形仅占少数权重的双线性足迹，保留其完全匹配 SurfaceId 的历史。受控运动样本中的历史下降次数从225降到0；此数字不代表全场景闪烁降低百分比。
- GI 采样：开启环境重要性采样，降低高反差环境下的大范围光斑。常态两射线与响应期八射线都保持均衡的两类采样。两场景p0/p2共64对正常帧完成对照。
- 动态响应：21张线性捕获验证变化后的响应。反射物移开仍会残留数帧旧亮度，受控接收面在请求后16帧的平均残留约为前后变化量的1.07%；光照调暗更快。该受控合成结果不等于纯间接光能量证明。

详细证据：`E:/HEngineEvidence/20260912-native4k/shadow-quality-acceptance-v1/report.json`、`E:/HEngineEvidence/20260912-native4k/gi-quality-acceptance-v1/report.json`。阴影和 GI 已分别完成构建、配置测试及默认值实际运行核验。

## 在已修复画质的版本上测量性能优化

对照双方使用同一程序，只切换 TAA 邻域复用与 GI 不确定度预计算两个选项。后者在 Off 模式下不执行。每个组合按参考/候选/候选/参考顺序测量，每轮1200个实际 CPU/GPU 对应帧，另保留600个尾部帧；没有删除长帧。

| 场景 / GI | 参考完整 GPU 均值 | 候选完整 GPU 均值 | 帧时间减少 | 状态 |
| --- | ---: | ---: | ---: | --- |
| Sponza / Low | 41.714 ms | 40.537 ms | 2.82% | 四轮温度和数据检查通过 |
| Bistro / Low | 36.743 ms | 35.906 ms | 2.28% | 四轮温度和数据检查通过 |
| Sponza / Off | 25.010 ms | 24.418 ms | 2.37% | 四轮温度和数据检查通过 |
| Bistro / Off | 27.647 ms | 27.771 ms | −0.45%（变慢） | 四轮数据检查通过；正式配置保留参考 |

对应的 CPU 帧间隔反映实际帧推进速度，包含等待；它不同于主线程忙碌时间：

| 场景 / GI | 参考 CPU 帧间隔 | 候选 CPU 帧间隔 | 帧间隔减少 |
| --- | ---: | ---: | ---: |
| Sponza / Low | 41.759 ms | 40.583 ms | 2.82% |
| Bistro / Low | 36.792 ms | 35.955 ms | 2.28% |
| Sponza / Off | 25.055 ms | 24.465 ms | 2.36% |
| Bistro / Off | 27.688 ms | 27.813 ms | −0.45%（变慢） |

以上均值为两轮等样本量均值的平均，不是挑选最快一轮。P95保留各轮范围：Sponza/Low参考42.296–42.340 ms、优化41.136–41.243 ms；Bistro/Low参考37.395–37.547 ms、优化36.173–37.025 ms。单个处理阶段节约的时间不能相加后当作完整GPU收益。

Low配置下，两场景内部临时纹理预留统计均为2063.35 MiB，优化前后相同；这不证明新增逻辑资源没有成本，也不等于整卡或独占物理显存。整卡遥测包含桌面和驱动占用。

本轮优先减少GPU重复计算。多线程、并行录制和bindless没有作为强制交付条件，未经净收益验证的架构草案未纳入本次结果。正式配置中，前三组采用候选，Bistro/Off保留参考实现；安装后已在同一份Game程序上重新运行四组配置并完成画面对照。

完整四轮CPU统计显示，Bistro/Off主线程忙碌均值参考27.617 ms、候选27.728 ms，接近其实际帧间隔。该组没有测得整帧净收益；不会将候选TAA阶段的节约写成最终帧率提升。详细CPU区间、忙碌时间、每轮分位数和源文件校验值见`round1-quality/final-cpu-scopes-v1.json`。

CPU 范围核对：已接受的首轮 Off 参考样本中，Sponza 主线程忙碌均值6.745 ms，帧槽等待18.143 ms；Bistro 主线程忙碌27.500 ms，帧槽等待0.001 ms。Bistro 的层更新范围为12.906 ms、渲染提交范围14.460 ms，说明这组存在明显的 CPU 工作负担。这里只分析参考样本，不代表候选收益。完整 GPU 首尾时间戳区间可以包含队列空闲，不能当成 GPU 利用率；最终同时报告 CPU 帧间隔。`cpuRenderOtherMs` 是渲染提交范围减去后端计时之和的残差，不能当成一个独立函数热点，也不能与图录制等范围重复相加。原始记录见 `round1-quality/post-gi-Off-reference-cpu-breakdown-v2.json`。

Off候选画质：两场景各16对连续帧，实际相机/抖动/提交记录配对通过；Bistro逐像素一致，Sponza平均最大通道差0.000204/255、最大8/255。整帧与局部序列人工检查未见明显回退。此结论限于记录的短片段，不是全视点等价证明。详见 post-gi-performance-Off-quality-acceptance-v1.json。

## 复现与核验

| 证据类型 | 精确命令 | 实际结果 |
| --- | --- | --- |
| build-only | `.\hectl.bat game build -Parallel 2` | GI默认版本 PASS，143.8秒 |
| unit-test | `.\hectl.bat test HardwareRayTracing` | PASS |
| unit-test | `.\hectl.bat test RenderSettings -Parallel 2` | 阴影默认阶段 PASS |
| runtime-manual | `& E:/HEngineEvidence/20260912-native4k/round1-quality/run-mis-default-runtime-v1.ps1` | PASS，已记录范围 |
| runtime-manual | `& E:/HEngineEvidence/20260912-native4k/round1-quality/run-post-gi-performance-abba-v1.ps1 -Scene Sponza` | Low四轮数据与温度 PASS |
| runtime-manual | `& E:/HEngineEvidence/20260912-native4k/round1-quality/run-post-gi-performance-abba-v3.ps1 -Scene Bistro -Mode Low -WarmupFrames 12600 -RunVersion v2` | Low四轮数据与温度 PASS，完整工作流 exit0 |
| runtime-manual | `& E:/HEngineEvidence/20260912-native4k/round1-quality/run-Off-performance-pair-v2.ps1` | 两场景Off四轮数据与温度 PASS，完整工作流 exit0；Bistro候选无净收益 |
| runtime-manual | `& E:/HEngineEvidence/20260912-native4k/round1-quality/run-final-integrated-checks-v2.ps1` | 总流程保留FAIL：恢复结果归档格式不匹配；四组配置、跨路径、三项单元测试及Game恢复分项通过 |
| build-only / runtime-manual | `& E:/HEngineEvidence/20260912-native4k/round1-quality/run-final-editor-resume-v3.ps1` | Editor构建PASS，274.9秒；捕获PASS、正常退出，人工画面复核通过 |
| unit-test | `.\hectl.bat test RenderGraph -Configuration Debug -Parallel 2 -TimeoutSeconds 300` | PASS |
| unit-test | `.\hectl.bat test RenderGraphResourcePool -Configuration Debug -Parallel 2 -TimeoutSeconds 300` | PASS |
| unit-test | `.\hectl.bat test RenderWorldSubmission -Configuration Debug -Parallel 2 -TimeoutSeconds 300` | PASS |
| codegen-check | `.\hectl.bat codegen check -CodeGenHost DevGame -Configuration RelWithDebInfo -Json` | PASS，status=clean，无dirty reasons |
| codegen-check | `.\hectl.bat codegen check -CodeGenHost Editor -Configuration RelWithDebInfo -Json` | PASS，status=clean，无dirty reasons |

Editor的实际底层捕获命令如下。它验证4K主窗口和受影响的编辑器集成，不作为Game原生4K性能样本；编辑器场景视口会受停靠面板布局影响。

```powershell
.\hectl.bat editor capture -Configuration RelWithDebInfo -Parallel 2 -Frames 120 -TimeoutSeconds 300 -Resolution 3840x2160 -GraphicsApi DX12 -StrictGraphicsApi -RenderPath Deferred -StartupScene Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he -RenderSettingsPath Project/Config/RenderingProfiles/Native4K/Sponza/Low.json -CapturePath E:/HEngineEvidence/20260912-native4k/round1-quality/final-editor-resume-v3/editor.png
```

上述脚本命令是实际执行记录；已有证据目录禁止覆盖，复跑应使用新的输出目录。常规加载复核可使用正式配置目录README中的`hectl game check`入口。Codegen这里只报告新鲜度检查，不把它写成生成输出hash/mtime零扰动的证明。

当前性能对照程序SHA-256：`a26b5e1be4a5207289d3bbec1281e47faa108d27dbfd87a7bafc5920d4c10271`。详细表为 `post-gi-Sponza-Low-comparison-v1.json` 和 `post-gi-Bistro-Low-comparison-v2.json`，旁边分别有 geometry footprint、资源和遥测核验。

Bistro首次9000帧预热试验的温度窗口均值变化2.43°C，未通过原定2°C阈值，完整保留为失败；重测预先改为12600帧预热，阈值和测量窗口未放宽。旧严格FPS阈值在原始分析里仍显示FAIL，仅作为参考，不能改写成达标。

## 最终验收范围与限制

短运动片段不能证明所有视点、级联交界和遮挡情况无闪烁。显示色阶差异含真实运动、遮挡及TAA，不能转换成纯闪烁百分比。SSGI仍受屏幕可见范围限制，动态间接光不是瞬时更新。阴影阶段已有黑窗细线等局部问题未在本轮宣称解决。

| 项目 | 已核验结果 | 证据与边界 |
| --- | --- | --- |
| Sponza / Off | 采用候选，实测GPU区间减少2.37% | 正式配置运行、16对连续帧及实际CPU/GPU关联通过 |
| Sponza / Low | 采用候选，减少2.82% | 常态两射线、MIS实际执行；正式配置运行与画面对照通过 |
| Bistro / Off | 保留参考实现 | 候选整帧未获净收益；最终16对画面逐像素一致 |
| Bistro / Low | 采用候选，减少2.28% | 正式配置运行、两射线/MIS及画面对照通过 |
| 原生4K与完整场景 | PASS | 实际主场景目标3840×2160，几何记录一致；10,254个冻结输入hash匹配，含6,527个场景资产文件 |
| 阴影与GI画质 | 记录范围内PASS | 正常画面、诊断、短运动序列、历史计数及动态线性捕获；不是全视点无闪烁保证 |
| Vulkan、奇数尺寸、Forward | 记录范围内PASS | 实际后端/路径/目标尺寸与48个完成提交关联；奇数尺寸3841×2161 |
| 生命周期与恢复 | 记录范围内PASS | 三项纯契约测试，以及设置切换、1280→4K Resize、重载、缺失捕获源后的继续运行 |
| Editor与生成代码 | PASS | Editor构建/捕获/退出和界面复核；DevGame、Editor新鲜度检查 |
| 120/90 FPS | 仅参考 | 原始严格预算检查仍为FAIL，未改写为达标 |
| 新增线程、并行录制、bindless | 未采用 | 以实际收益选择实现；本次未引入新的工作线程或多队列生命周期 |

最终四组配置共64对画面、128个双键关联。Off/Sponza最大平均通道差0.000552/255、最大8/255；Off/Bistro全16对一致；Low/Sponza最大平均差0.005983/255、最多5像素超过8级；Low/Bistro最大平均差0.006392/255、最多138像素超过8级。整帧、拱门和路面局部连续图复核未见明显回退；这些显示色阶统计不是线性能量证明。

生命周期覆盖包括逻辑图、帧槽/资源复用和提交状态契约；Game恢复检查注入的是“捕获源不可用”，不是实际GPU提交失败或设备丢失。动态GI仍有数帧收敛；局部点/聚光灯没有纳入最初阴影对照矩阵，不宣称所有灯型和材质都已穷尽验证。

验收文件位于`E:/HEngineEvidence/20260912-native4k/round1-quality/`：`final-profile-runtime-acceptance-v2.json`、`final-crosspath-acceptance-v2.json`、`final-recovery-acceptance-v2.json`、`final-editor-acceptance-v3.json`。输入完整性见`final-frozen-input-audit-v1.json`。Editor程序SHA-256为`71eb8ceaebd9b947e45d4cb079f153005384b65cffcace1c73dd1c5e385355a9`；Game仍为上文同一个a26b版本。

归档失败保持可追溯：`final-integrated-checks-v2`在Game恢复成功后，误调用要求`run-outcome.json`的序列捕获归档脚本，而恢复结果使用`result.json`。该父流程保持FAIL；已通过子项独立保留，只用`final-editor-resume-v3`补跑当时尚未执行的Editor和新鲜度检查。分类记录为`final-integrated-failure-classification-v2.json`，不把此工具格式错误写成渲染失败或抹去原始失败状态。

Sponza/Off首次18000帧预热试验的数据与实际提交关联完整，但末段温度斜率为−1.0769°C/分钟，未通过绝对值0.5°C/分钟阈值。此试验保留失败状态，不用于收益；已预先制定24000帧预热重测，其他阈值与测量窗口保持不变。


原始证据目录：`E:/HEngineEvidence/20260912-native4k/`。各接受记录链接实际数据、配置、日志与校验值；阶段旧报告保留当时的状态和限制，以本文所列最终验收记录为准。

---

## 画质路径

阴影使用连续 PCF 权重和比较采样器，减少相机移动时由离散取样位置带来的边缘跳变。硬阴影的零半径分支继续使用原来的点比较。默认选项已通过原生 4K 正常合成画面、诊断画面及配置验证。相关实现位于 `ShadowContinuousPcf.hlsli`、`RasterSurfaceLighting.hlsli` 和 `SceneLocalShadowSampling.hlsli`。

SSGI 的历史复用继续只读取完全匹配的 `uint2 SurfaceId` 历史。新增几何支持权重用于识别一个双线性足迹整体都满足几何条件、但当前三角形只占少数权重的情形，避免模型内部三角形边界反复丢失历史。轮廓、显露区域和几何不匹配仍使用原来的拒绝条件。该规则不把不同三角形的历史颜色直接混合，也不修改公共 TAA 的 SurfaceId 规则。

环境重要性采样使用已有 HDR 环境的分布提案。偶数射线预算中，一半为余弦半球提案，一半为环境提案，两者使用相同的混合概率密度。常态两射线与变化后的八射线保持一致的策略。奇数预算、缺少提案或其他不支持条件仍走回退路径；默认开启此选项不等于强行开启 GI 本身。

动态物体变化会触发现有的 16 帧响应窗口，提升射线预算并提高新样本的混合比例。它不会瞬间清除所有旧的间接亮度。受控测试中，反射物移开后仍有数帧残留；光照调暗响应更快。这个限制和屏幕空间可见范围限制必须随结果保留。

## 环境采样资源

环境提案在已有 HDR 纹理加载时从同一份已清理浮点像素生成，不在每帧重新解码。是否请求提案被纳入纹理缓存键。CPU 提案与 GPU 的 R32_FLOAT 累积分布纹理由 ManagedTexture 共同持有；图内处理数据另保留只读提案引用，并声明分布纹理的读取依赖。

提案生成或分布纹理创建失败时回退到原采样方式，保留已加载的 HDR 环境，不能把可选优化失败变成黑色环境。资源使用原有加载、提交和退休机制；本改动没有新增可供多线程并发写入的缓存接口。MIS 选项变化被纳入历史状态指纹，避免直接复用另一种估计策略的历史。

## 性能候选的数据与生命周期

TAA 邻域复用由同一工作组协作读取包含边界的 10×10 区域。共享数据只在该工作组内存在；统一同步点位于所有像素提前返回之前，边缘地址仍按尺寸限制。材质、法线、深度、颜色变换、锐化和历史混合规则继续使用原有计算。

GI 不确定度复用将相同的 3×3 预过滤计算提到一次独立处理阶段。输出为当前图内的 `R32_FLOAT` 临时纹理，仅在 SSGI 且去噪迭代数大于一时使用。后续迭代读取该结果；原始方差资源仍供调试和其他使用者读取。纹理不缓存跨帧光照，不引入新的持久历史失效规则，由 RenderGraph 管理读写依赖和资源生命周期。

这些候选没有新增 CPU 工作线程、独立命令列表或跨线程场景对象访问。当前 Sponza / Low 参考测量以 GPU 为主要瓶颈，因此先评估减少着色器重复工作。不得把处理阶段时间减少量直接相加，替代完整 GPU 帧时间的实测收益。

固定的原生4K配置已安装至`Project/Config/RenderingProfiles/Native4K/{Sponza,Bistro}/{Off,Low}.json`，四份文件与被选中的实测输入逐字节一致。共同画质参数保持一致，Low使用已验证的两射线SSGI；Off使用真正关闭路径。不凭4K数据推断所有分辨率、设备和组合都更快。

采用判断与测量验收分开：只有两轮候选的CPU帧间隔和完整图形队列区间均优于两轮参考时，才启用本轮两个实现选项；否则保留参考。这个保守判断是在审阅测量后用于选择配置，不是预先声明的统计显著性检验，也不更改任何原始温度或数据验收门槛。安装凭据`round1-quality/installed-native4k-profiles-v2.json`记录了每份配置的来源、采用理由和校验值；安装本身不证明最终运行通过。

## 证据读取规则

截图以实际记录的相机矩阵、抖动、GI 序列和提交编号为准，不能用截图完成时的相机状态替代。动态请求的延迟零也不保证同一帧截图；已记录的实际偏移必须保留。

性能处理阶段通过 `graphSubmissionId` 对应完整 GPU 提交，再以 `submissionTimingSampleId` 和 `cpuTimingFrameId` 对应 CPU 行。不得通过序号偏移猜测归属。测量窗口、尾部回收帧和原始长帧全部保留。温度检查失败的试验保留失败状态，重新预热必须进入新结果目录。

显示色阶差异不能直接当成线性能量或纯闪烁指标；移动画面差异还包括真实运动、显露和 TAA。NVML 显存数据属于整张显卡，不等同于 Game 独占显存。内部资源预留、逻辑纹理容量和整卡占用应分别报告。

正式配置验证、相关运行检查与准确的复现命令见上面的验收矩阵。原始数据、失败尝试和最终接受记录分别保存，不能将不同实验版本的最好成绩拼接成最终结果。

## 采用与保留的方案

| 方案 | 当前状态与理由 |
| --- | --- |
| 连续PCF、SSGI内部边界历史支持、均衡环境MIS | 已完成相应画质验收并进入当前实现；PCF和MIS默认开启。 |
| TAA邻域复用、GI不确定度预计算 | 正式配置在Sponza/Low、Bistro/Low、Sponza/Off采用，实测整帧收益约2.82%/2.28%/2.37%；Bistro/Off保留参考。通用默认仍关闭，安装后四组运行与画面对照已验收。 |
| 既有SVGF OptimizedKernels、TAA OptimizedHistoryGather及纹理复用 | 原始冻结基准已经启用，当前对照双方一致；不得把这些已有配置的效果再次计入本轮两项开关的收益。 |
| 紧致视锥、派生LOD和局部响应射线预算 | 当前正式对照保持关闭；不以减少几何或未经画质验收的局部预算替代本轮收益。可选实现/外部试验不等于已选用的优化。 |
| DiffuseHitDistanceWeight=0探测 | 保留原值1；该探测不是最终GI光斑修复。 |
| 额外工作线程、并行录制、bindless扩展 | 本次未选入新增实现；当前先减少已有GPU计算中的重复工作，不以技术名称或线程数量作为验收结果。 |

以上状态对应当前冻结版本与已完成阶段。最终采用情况仍须结合Off结果和安装后核验更新。

## 当前冻结版本的代码定位

这些定位已按当前文件内容和哈希核验；运行与画质结论仍以对应验收记录为准。

- [continuous-shadow](C:/WorkSpace/HEngineDev/Engine/Content/Shaders/Modern/ShadowContinuousPcf.hlsli:14)：Continuous comparison-filter sampling implementation; normal and diagnostic acceptance is separate.
- [ssgi-internal-edge-history](C:/WorkSpace/HEngineDev/Engine/Content/Shaders/GlobalIllumination/RtgiSvgf.hlsl:359)：Retains exact-ID history on geometrically supported internal primitive edges; does not admit another primitive color.
- [balanced-environment-mis](C:/WorkSpace/HEngineDev/Engine/Content/Shaders/GlobalIllumination/ScreenSpaceGlobalIllumination.hlsl:811)：Even budgets use equal cosine/environment counts and shared mixturePDF; odd budgets fallback.
- [taa-neighborhood-reuse](C:/WorkSpace/HEngineDev/Engine/Content/Shaders/Modern/SceneTaa.hlsl:380)：Uniform group synchronization after cooperative neighborhood load; visual and performance proofs separate.
- [svgf-current-frame-uncertainty](C:/WorkSpace/HEngineDev/Engine/Source/Runtime/Graphics/GlobalIllumination/SvgfDenoiser.cpp:1188)：SSGI-only,more than one iteration; graph-local R32 output read by atrous filters.
- [environment-proposal-ownership](C:/WorkSpace/HEngineDev/Engine/Source/Runtime/Graphics/RHI/TextureManager.cpp:995)：Proposal built during HDR load, retained with ManagedTexture; optional failure preserves HDR/fallback.
- [mis-history-fingerprint](C:/WorkSpace/HEngineDev/Engine/Source/Runtime/Graphics/GlobalIllumination/ScreenSpaceGlobalIllumination.cpp:430)：Sampling policy participates in history invalidation.

Sponza/Off重测四轮数据与温度检查均通过：参考GPU均值25.0099600104 ms，候选24.4179102408 ms，减少2.3673%。此处按实际四轮记录计算，完整Sponza/Off工作流与补充汇总已完成。

Sponza/Off补充核验：四轮内部临时纹理预留均为1472.73 MiB；实际测量窗口整卡遥测每轮27–29个样本，温度84–86°C。主渲染3840×2160、GI射线预算0、降级LOD为0，115个输入对象与3747018个基础三角形的CPU记录四轮一致。相关四份报告为post-gi-Sponza-Off-{comparison,resources,telemetry,footprint}-v2.json，均通过各自记录检查；这些显存数值不是进程独占物理显存。

Bistro/Off补充核验：四轮内部临时纹理预留同为1472.73 MiB；每轮31–32个整卡遥测样本，温度83–86°C、整卡显存4757–4763 MiB。主渲染3840×2160、GI射线预算0、降级LOD为0，1296个输入对象与2832120个基础三角形四轮一致。参考轮整卡GPU利用率均值约71.5%–71.7%，候选约68.9%–69.2%；该指标不是Game独占利用率，较低的GPU工作量没有转化成这组更快的整帧。相关四份报告为post-gi-Bistro-Off-{comparison,resources,telemetry,footprint}-v2.json。
