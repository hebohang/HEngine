# SSGI 传输与去噪验证记录

本文保留各实验时点的参考资料、缓冲语义、CPU 几何积分和历史结果。**最终二进制复核及PASS/FAIL以 [本轮结果](SSGI-Transport-Results.md) 为准，最终28组计时见 [性能表](SSGI-Transport-Performance.md)**；下文“待补”描述属于当时阶段，不代表后来未执行。

## 阶段证据与版本边界

本节汇总 `motion`、`bootstrap-low-taa` 与 `backends` 的现有分析，不新增 GPU 运行或构建证据。目录名中的 `final-*` 是当时实验阶段的标签，**不表示其捕获已经覆盖后续所有修改**。以下运动与 bootstrap 数据均早于默认法线中性值调整；当前 accepted 捕获、最终预设及正式动态计时由主线程另行汇总，不能用旧数字自动填充。

### 原始版本与本轮增量

本轮 baseline 是开始任务时的本地 HEAD `f41ea9b0d2bac10b2b1bbaadb7f5daee700e2a9a`，不是从文档描述推测的更早“未修复版本”。开始时已有未跟踪的用户资产，清单见 `out/agent/ssgi-audit-20260909/initial-git-status.txt`；这些资产不属于本轮修改。基线着色器副本为同目录的 `baseline-RtgiSvgf.hlsl` 和 `baseline-SSGI.hlsl`。原始画面保留用户有效曝光复现，受控对照才固定曝光并关闭诊断干扰项；曝光差异不计为 GI 质量收益。

有符号历史深度、`RG32_FLOAT` 亮度矩、去材质/重乘以及稀疏样本统计钳制在该基线已存在，不重复认领为本轮修复。本轮增量应分别归因于调试/捕获可信性、3×3 几何门控方差预滤、反应窗口与预算、运动 clampSigma、以及默认法线源着色修正；不能把诊断修复或新增射线的收益全归给去噪。下文真实 Editor 阶段对照的 +5.01% 包含 ray1→2，而同 ray1 的方差预滤对照为 +4.43%。

### 持续运动状态：sigma3 与采样预算的对照

证据及可重跑入口：`out/agent/ssgi-audit-20260909/motion/analyze_motion.py`、`analysis-motion.json`；候选比较为 `analyze_motion_candidate.py --candidate sigma3|sigma3-noaa|sigma3-high`，输出各自 JSON 及 `analysis-motion-candidates.json`。候选直接复用已收敛的各 phase 面积积分，没有重新用静止板参考代替移动板。

共有 camera/board 各四个固定 tick 状态（240/300/360/420），每个 source/composite 都从独立进程完整运行到该状态；输入固定 `1/60 s`、周期 240 tick。已验证实际相机矩阵、trajectory/script/material hashes、pose sequence hash 和有效设置。相机快照对应 tick−2，误差小于 2.2e−7；移动板的位置另由受光板投影轮廓核验，tick−2 候选 IoU 为 0.999932–1.0，tick−1 为 0.974646–0.998906，没有直接套用相机延迟。四个板中心 z 为 −0.078504、1.497944、0.078504、−1.497944 m。

每个 d1/d2 ROI 各取 240 个已知接收面世界内点，按当时矩阵投影；接收面 source HDR 均为零，因此 Composite−source 没有混入源板的直接光。面积积分使用当时实际移动板及可见 source HDR，64×128→128×256 的最大差异为 0.075%，加权可见源超过 99.9%。它仍是假设相机方向 source 为各向同性的当前输入参考，不是完整物理真值。

下表为相机运动各状态的接收面亮度均值/参考。Low2 是 2 rays、64 steps、半分辨率、2 次 A-trous、refine3；High4 是 4 rays、128 steps、半分辨率、3 次 A-trous、refine6。

| tick | SIGMA2 Low2+TAA，d1/d2 | SIGMA3 Low2+TAA，d1/d2 | SIGMA3 Low2 无 AA，d1/d2 | SIGMA3 High4+TAA，d1/d2 |
| --- | --- | --- | --- | --- |
| 240 | 97.41% / 89.57% | 97.41% / 92.93% | 97.61% / 94.39% | 未捕获 |
| 300 | 97.03% / 92.07% | 97.03% / 92.70% | 97.21% / 93.42% | 未捕获 |
| 360 | 96.19% / 81.60% | 96.17% / 93.88% | 96.30% / 93.35% | 97.88% / 93.77% |
| 420 | 96.30% / 83.89% | 96.41% / 95.15% | 96.81% / 94.63% | 97.47% / 93.49% |

sigma3 候选相对 sigma2 仅修改 SSGI 运动统计钳制的 sigma 2→3，静止仍为 3；配对输入、实际 sample sequence/rays、可执行文件 hash 一致，着色器 hash 差异限定于 `RtgiSvgf.hlsl`。最差两状态 d2 恢复 12.28/11.26 个百分点，说明历史允许区间压低了稀疏真实反弹。这没有解释余下约 5–7% 偏差，也没有证明宽钳制无拖影。

无 AA 对照同时移除了 GI 输入的光栅抖动，不能称“只绕过最后 TAA pass”。相对 sigma3+TAA，d2 变化为 −0.53 至 +1.46 个百分点，远小于旧最差状态的损失；High4 也未进一步恢复 d2，因而不能以增加预算替代该钳制修正。各 RGB 通道比例保存在候选 JSON，旧最差 d2 的 RGB 比例约 `(81.71%,81.50%,81.52%)`，不是只损失某个材质通道。

移动板原组触发实际 8 rays，四状态 d1 为参考的 97.98–101.22%，d2 为 97.75–101.05%。这些状态有各自完整历史，但不是连续帧视频；不能从四个时刻量化逐帧闪烁、轮廓拖尾或显露区域的收敛帧数。ROI 使用未抖动矩阵作投影，TAA 的逐帧抖动会使像素/边缘归属近似；此分析特意选内点，尚不构成边缘漏光验收。所有捕获的非有限、负值和 FP16 最大值计数均为零。

### 重置后 bootstrap：41 个独立事件的实际响应

新增机制是在 SSGI 历史 reset 后同样启动 `ReactiveHistoryFrames=16`，窗口内实际预算至少为 8 rays，历史仍按正常 reset 清空；保留 sigma3。它用于补足重新开始积累时的观测，不是保存关灯前亮度。CPU 重跑命令为：

```powershell
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' Tools/Agent/Analyze-SSGIStepResponse.py --capture-directory out/agent/ssgi-audit-20260909/bootstrap-low-taa --reference-directory out/agent/ssgi-audit-20260909/constant --compare out/agent/ssgi-audit-20260909/sigma3-low-taa/analysis-dynamics.json
```

证据：`bootstrap-low-taa/analysis-dynamics.json`、`analysis-bootstrap-summary.json` 与 `summarize_bootstrap.py`。41 事件均成功读取，有效设置一致，最终 Composite+TAA 已核实。旧 PFM 的无损 gzip 归档由 loader 按逻辑 `.pfm` 路径读取，不能因原路径只剩相邻 `.pfm.gz` 而声称丢失数据；归档 SHA 见证据根 `archive-manifest.json`。

表中 age 为实际 world frame 与 mutationRenderFrame 之差，不是文件名或 settled readback 数。两次 age2 是独立重复开灯，并非一条连续曲线的同一帧。

| 开灯实际 age | 实际 rays | Bootstrap d1/d2 均值/参考 | 旧 sigma3 d1/d2 均值/参考 |
| --- | ---: | --- | --- |
| 2，观测一 | 8 | 98.90% / 94.17% | 90.19% / 109.47% |
| 2，观测二 | 8 | 97.14% / 97.73% | 105.04% / 108.81% |
| 3 | 8 | 98.37% / 98.58% | 81.82% / 68.39% |
| 5 | 8 | 99.68% / 97.06% | 95.27% / 93.69% |
| 9 | 8 | 99.19% / 97.95% | 96.08% / 94.69% |
| 17 | 8 | 98.96% / 97.29% | 98.78% / 98.78% |
| 33 | 2 | 98.19% / 95.52% | 98.54% / 96.90% |

bootstrap 改善了已采到的早期开灯跌落和过冲。age17 仍为 8 rays、age33 已为 2 rays；缺少 age18–32 的观测，不能实测定位确切预算切换帧或峰值成本。

七个关灯观测（age2/2/3/5/9/17/33）的两个接收 ROI 每个像素均严格为零。板移出后 age17 残留为自身静态均值的 1.412%/1.775%，age33 为 0.523%/1.479%；age33 d2 比旧组高约 0.42 个百分点参考值，不能笼统称所有响应均无回退。移开的板仍可能有少量真实贡献，残留不自动全算旧历史。

camera-cut age2 的 d1/d2 为参考的 97.93%/92.94%，camera-restore 为 95.94%/92.44%；两者实际仍为 2 rays，因此这些标签不能证明它们触发了 bootstrap 的明确历史重置。board-restore age33 为 98.18%/97.10%。

静态 8 次观测：新 d1/d2 均值/参考为 97.265%/92.694%，旧为 97.517%/93.329%；逐像素标准差/参考为 1.365%/3.322%，旧为 1.611%/3.857%。ROI 均值的相对标准差为 0.384%/0.836%，旧为 0.715%/0.819%。随机 sequence 不同，这些小变化不能当确定的改善或回退；空间像素也不是独立时间样本。此 41 事件组没有近静态 reactive_noise 子序列或连续视频。全部事件非有限、负值和 FP16 最大值计数均为零。

### Forward 源光照：默认法线偏斜与 GI 归因

证据为 `out/agent/ssgi-audit-20260909/backends/analysis.json`、`analysis-source.json` 及 source/zero/composite 捕获。本次 DX12 Forward、Vulkan Forward/Deferred 运行都确认了实际后端/渲染路径与 SSGI 可用性；但可运行不等于画质等价。此前 Forward Composite 比 Deferred 更亮，分解后发现接收面本身新增了约 63.2 的场景线性灰色直接光。

| 接收 ROI | 旧 Forward source 均值 | Deferred source | Forward Composite | Forward Composite−source | 后者 / Vulkan Deferred Composite |
| --- | ---: | ---: | ---: | ---: | ---: |
| d1 | 63.2525 | 0 | 1091.8073 | 1028.5548 | 100.0408% |
| d2 | 63.2185 | 0 | 505.2311 | 442.0126 | 100.0441% |

代码与数值支持的根因是无贴图材质使用的默认法线 texel `(128,128,255,255)` 为 RGBA8 UNORM；按 `sample*2−1` 解码得到 `(1/255,1/255,1)`，归一化前 XY 不为零，合计偏转约 0.318°。100000 lux 掠射条件把这个小法线误差转成可测直接光。只计 Lambert 的近似为 59.92；代入共享着色公式与两个 bitangent 朝向候选后，预测与 source ROI 平均绝对误差约 0.0076–0.0152。该数值拟合支持默认法线机制，但单独不能确定 mesh TBN 朝向。关太阳后的 Forward source 全图严格为零，排除了此项来自无条件环境常量。

当前代码只将引擎 1×1 默认 normal fallback 改为 RGBA16_FLOAT 的精确 `(0.5,0.5,1,1)`，上传有效 payload 由 4→8 bytes；有内容的法线贴图继续沿用原编码。该改动纠正缺省材质直射源，不是 SSGI 能量增益，也不是将整条 HDR 链改为更昂贵格式。纹理资源实际分配、对齐和绑定成本不能按这 4 bytes payload 差直接声称已测。

Forward 源板 RGB 相对旧 Deferred 的比例约 `(99.997%,99.994%,101.744%)`，源方向性与材料/量化差异仍存在。Composite−source 来自分开运行，含 FP16 舍入与 sequence 差；约 0.04% 的标量接近不证明逐像素 GI 完全相同。**默认法线修改后的源 HDR、接收面零直接光、旧积分参考及各后端对照必须重新核验**，不能把此节旧源捕获作为修后 PASS，也不能继续无条件复用旧 constant/motion 参考。

### 此阶段之后的交付与剩余项

- 最终 accepted source/normal/receiver、真实Editor、三档质量、两尺寸静态/动态GPU/显存及命令已补入 [本轮结果](SSGI-Transport-Results.md)。更新积分参考核验到当前求积精度内一致，不能把旧源捕获单独记作修后PASS。
- 下文旧性能表仍为阶段候选证据，不能代替最终计时。共享SVGF额外DX12 HardwareRTGI Ready后切换通过，启动失败及Vulkan能力不可用单列。
- 连续视频的闪烁/拖尾、窗口退出、显露与薄边 ROI；相机切换是否真正触发 reset。已验证的两类屏幕路径遗漏仍未解释全部遮挡残差，也没有被本轮法线/去噪更改消除。

## 参考版本与适用范围

资料核查日期：2026-09-09。

| 来源 | 固定版本与链接 | 本轮采用范围 |
| --- | --- | --- |
| Epic 独立 Screen Space Global Illumination | [UE 5.6 SSGI 文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/screen-space-global-illumination-in-unreal-engine?application_version=5.6) | 区分独立屏幕空间效果与其他 GI。文档的展示包含烘焙光照；不能将其暗部亮度当作无环境、无烘焙 SSGI 的目标。质量 1–4 的 rays 为 4/8/16/32，steps 为 8/8/8/12，另有半分辨率开关；这些数字不直接映射本项目的 Hi-Z 遍历预算。 |
| Epic Lumen Technical Details | [UE 5.6 Lumen 技术文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine?application_version=5.6) | 仅用于划分职责：屏幕追踪之后还有距离场或硬件追踪，Surface Cache 提供屏幕之外的光照数据。Lumen 的完整效果与成本不是独立 SSGI 的比较基线。 |
| Unity HDRP SSGI 使用文档 | [HDRP 17.0.4 使用文档](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/Override-Screen-Space-GI.html) | 明确区分 Ray Marching、Mixed 与 Ray Tracing。Mixed 包含屏幕外硬件追踪，不能称纯屏幕空间。 |
| Unity HDRP SSGI 属性参考 | [HDRP 17.0.4 属性文档](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.0/manual/reference-screen-space-global-illumination.html) | Ray Miss 默认 Both，包含反射探针和天空；纯 SSGI 比较须使用 Nothing，并核对源码中的 APV 回退、曝光与有效 Volume。不能只因启用 SSGI 后常规 Light Probe 停止贡献，就推断所有回退光照均已关闭。 |
| Unity 公开实现 | `Unity-Technologies/Graphics`，`6000.0/staging`，提交 `275a7f9ad9330e3cf7f3ea57a0b242a551fde291`；[package.json](https://github.com/Unity-Technologies/Graphics/blob/275a7f9ad9330e3cf7f3ea57a0b242a551fde291/Packages/com.unity.render-pipelines.high-definition/package.json) 确认为 `17.0.4` | 以下源码链接均固定到该提交，不使用随时间变化的 master。 |
| SVGF 原论文 | Schied 等，HPG 2017；[作者机构论文页](https://research.nvidia.com/labs/rtr/publication/schied2017spatiotemporal/)、[官方 PDF](https://research.nvidia.com/sites/default/files/pubs/2017-07_Spatiotemporal-Variance-Guided-Filtering%3A/svgf_preprint.pdf) | 去材质、几何重投影、亮度矩和方差引导滤波。重点采用 §4.4/Fig.5 的稀疏样本方差预滤思想。论文场景与本项目不同，不能复制其固定参数或性能。 |
| 时空蓝噪声 | Wolfe 等，EGSR 2022；[Spatiotemporal Blue Noise Masks](https://research.nvidia.com/publication/2022-07_spatiotemporal-blue-noise-masks) | 仅为采样候选。改善误差分布与收敛，不能解决系统性漏命中。尚未采用的候选不得计入本轮收益。 |

Unity 源码入口：

- [ScreenSpaceGlobalIllumination.compute](https://github.com/Unity-Technologies/Graphics/blob/275a7f9ad9330e3cf7f3ea57a0b242a551fde291/Packages/com.unity.render-pipelines.high-definition/Runtime/Lighting/ScreenSpaceLighting/ScreenSpaceGlobalIllumination.compute)：余弦半球采样、命中颜色重投影、曝光换算与回退。
- [RayMarching.hlsl](https://github.com/Unity-Technologies/Graphics/blob/275a7f9ad9330e3cf7f3ea57a0b242a551fde291/Packages/com.unity.render-pipelines.high-definition/Runtime/Lighting/ScreenSpaceLighting/RayMarching.hlsl)：像素边界、深度金字塔、厚度板与叶节点判断。
- [HDRenderPipeline.ScreenSpaceGlobalIllumination.cs](https://github.com/Unity-Technologies/Graphics/blob/275a7f9ad9330e3cf7f3ea57a0b242a551fde291/Packages/com.unity.render-pipelines.high-definition/Runtime/Lighting/ScreenSpaceLighting/HDRenderPipeline.ScreenSpaceGlobalIllumination.cs)：资源时序、历史有效性、时间/空间滤波和上采样。

该版 HDRP 的命中着色读取前帧颜色金字塔 mip 1，先逆换算前帧曝光，再施加当前曝光。`SSGI_CLAMP_VALUE = 7` 限制的是当前预曝光 RGB 转 HSV 后的 Value，**不是场景线性 radiance 的 7**。本项目的场景线性链路不得直接复制该阈值。Epic SSGI 文档列出 `MinimumLuminance`，但没有给出足够编码域信息，亦未据此引入阈值。

## 参考机制与本项目改动的对应关系

| 参考机制 | 本项目已核对现状 | 本轮处理或候选 | 预期收益 | 验证方法 |
| --- | --- | --- | --- | --- |
| Hi-Z 遍历须处理像素边界、近面和厚度；单边深度表示可能在朝相机方向退化 | 已有齐次裁剪、像素边界遍历、叶级朝相机交点处理；法线拒绝后仍消耗预算 | 追踪终止原因、命中类型与预算分布用于诊断；进一步遍历更改须由捕获支持 | 区分不可见路径、假交点和预算不足 | 同一随机序列下比较受光命中、法线拒绝、厚度拒绝、出屏和预算耗尽；同时检查泄漏 |
| 余弦采样应正确抵消 Lambert 项 | `throughput` 已含接收面 diffuse reflectance，结果除总射线数 | 保留一次反弹估计器，禁止按有效命中数补偿 | 保持完整半球积分的归一化 | 常量板面积积分、太阳强度比例、零光源 |
| 源颜色必须明确帧时序与光照内容 | 当前帧预 GI 的 `OpaqueDirectHDR`，包含相机方向的表面着色，不等于纯漫反射源 | 保留当前帧一次反弹基线；漫反射专用源或受几何限制的颜色预滤仅为候选 | 避免重复反馈，隔离镜面方向偏差和源采样噪声 | 比较 GI on/off 的源 HDR；彩色板、光滑/粗糙材质与关灯响应 |
| SVGF 低样本方差可能低估稀有有效反弹 | 原 A-trous 仅用中心方差除有效历史；黑中心可阻断有效邻域 | **已采用：SSGI 专用、几何门控的 3×3 方差预滤**，只影响亮度边缘权重 | 减少零/低方差中心无法接受合理邻域的情况 | 原始/时域/空间/重乘能量分段比较，并同时检查接触边、漏光和动态残留 |
| 去材质和重乘必须匹配，历史限制必须保留稀疏颜色 | 已有去材质/重乘、场景线性亮度矩；二阶矩已用 `RG32_FLOAT`；稀疏样本统计亮度 clamp 已存在 | 保留既有修复，不将它们计为本轮新增；新增限制须逐项证明 | 避免重复修复、失真或错误归因 | 对比实际 Albedo；低反照率、彩色、金属、发光；关灯和曝光对照 |
| STBN 改善时空样本分布 | 当前为 PCG scramble 和连续 R2 序列 | STBN 仅为后续候选 | 降低同预算下的低频噪声和闪烁 | 固定均值容差，比较时域方差、误差频谱、收敛与成本 |
| 其他光照表示补充屏幕缺失 | 本轮环境关闭是必须保留的测试条件 | 探针、RSM 等只可单独评估，不属于纯 SSGI 修复 | 补充可证明缺失的屏幕外路径 | 分开记录方法、资源与成本，不混入本轮 Off/On 对照 |

采用的方差预滤位于 `Engine/Content/Shaders/GlobalIllumination/RtgiSvgf.hlsl`。SSGI 分支对 3×3 邻域进行材质身份一致性检查，并用深度差、法线余弦和 `[1,2,1]` 可分离空间权重归一化每点的 `variance / clamp(historyLength,1,32)`。结果仅用于亮度边缘权重的 sigma；不更改输入 radiance、亮度矩、真实零样本或射线平均分母，不引入环境亮度。RTGI 分支不启用这一 SSGI 调整。

这不是对 SVGF 全算法的复刻。原论文还包含短历史空间方差和各级方差传播等机制；没有实现或验证的部分不能宣称已经采用。

## 缓冲语义与可比较单位

以下语义适用于本轮 RealtimeRaster SSGI 路径。场景线性值遵循引擎的光度标度；不能未经标定将 HDR 数值直接称为 SI 辐射单位。

| 缓冲/证据名称 | 原生分辨率 | RGB 语义 | 可比较方式 |
| --- | --- | --- | --- |
| `source` / `OpaqueDirectHDR` | 全分辨率 | GI 合成前表面着色，含直接光、相机方向镜面及适用的发光；环境启用时也须核对其内容 | 本轮环境为零。它不是只含 diffuse 的入射光，也不能假定任意出射方向都相同 |
| `raw` / 原始间接光 | 追踪分辨率 | 已乘接收面的 diffuse reflectance 的间接出射信号；本轮一次反弹 | 与重乘后的 GI 或匹配源辐射、BRDF 的积分比较 |
| 去材质信号 | 追踪分辨率 | raw 除以实际 diffuse base color；不是把接收面的 Fresnel 能量分配也全部除去 | 必须使用实际 G-buffer Albedo，不能用材质编辑器的名义数值 |
| `temporal` | 追踪分辨率 | 场景线性、去材质后的时域累积 | 与 filtered 直接比较；与 raw 比较前按接收面 Albedo 重乘 |
| `filtered` | 追踪分辨率 | 场景线性、去材质后的空间滤波结果 | 与 temporal 同域；跨分辨率时仍须按表面身份对齐 |
| 上采样结果 | 全分辨率 | 经几何引导重建的去材质信号 | 检查薄表面与轮廓，不能将不同表面的样本混作同一 ROI |
| `remod` | 全分辨率 | 上采样后乘实际 diffuse base color 的 GI | 可与 raw、源匹配积分比较；与 filtered 比较时注意上采样和 Albedo |
| 合成前/后场景 HDR | 全分辨率 | 正常渲染场景线性颜色 | 固定其他 pass，用差值隔离 GI 合成；不要用调试视图替代正常 Composite |
| `albedo` | 全分辨率 | 捕获的线性 RGB 基色；本批 PFM 不携带 metallic/roughness | 常量材质也应验证捕获值；不能凭 RGB 证明 metallic=0 |
| `normal` | 全分辨率 | 有符号世界空间着色法线 | 不是未经法线贴图扰动的几何法线 |
| `depth` | 全分辨率 | 本批调试捕获为正的线性视深度 | 重建时 view-space z 为负；矩阵按 column-major 解释 |
| `hits` / Coverage | 追踪分辨率及其调试重建 | 首次屏幕命中数除总射线数 | 不等于命中受光表面的概率，也不等于 Confidence |
| Confidence / HistoryLength | 相应信号分辨率 | 观测/历史可靠性信息 | 不能用常量置信度解释为屏幕命中率 |
| 最终 PNG | 显示分辨率 | 经过曝光、显示映射等处理 | 用于观察观感，不能替代线性 HDR 能量证据 |

本批 PFM 为 1920×1080 的调试捕获容器。追踪、时域或空间信号即使以此尺寸输出，原生路径仍可能为半分辨率；相邻 2×2 显示像素不能算四个独立蒙特卡洛样本。PFM 也不保留原资源格式/alpha；原始资源精度与统计应结合代码或 raw readback 元数据确认。

## 独立矩形积分：方法、实测基线与限制

受控几何为接收面 `y=0`、源板 `x=0, y∈[0,2], z∈[-2,2]`，相向法线为 `+Y` 与 `+X`。接收点为 `(d,0,z0)`。对已知源 radiance 做面积积分：

\[
L_o(x)=\frac{\rho_r}{\pi}\int_A
L_s(y)\,V(x,y)\,
\frac{\max(n_r\cdot\omega,0)\max(n_s\cdot-\omega,0)}{\|y-x\|^2}\,dA.
\]

这里的距离平方来自面积到立体角的换元，不是给 SSGI 每条 radiance 再乘一次距离衰减。对于常量 Lambertian 源，另外使用矩形的闭式配置因子交叉检查面积积分。对 ROI 的每一个重建世界点分别积分，再求均值，没有只用 ROI 中心代替整块表面。

CPU 证据目录：`out/agent/ssgi-audit-20260909/constant/`。

- `analyze_fixture_constant.py`：自包含重跑入口，导入 `Tools/Agent/Analyze-SSGITransport.py.load_image`。
- `analysis-fixture-constant.json`：矩阵、实际材料、ROI、数值收敛、阶段统计与不确定性。
- 输入为 `baseline-editor-fixture-{source,depth,normal,albedo,raw,temporal,filtered,remod,hits}.pfm` 及 `.world.json`。

这组是既有基线捕获的 CPU 分析，不代表最终修改后的验收。捕获使用 Editor、100000 lux、环境零、8 rays、64 steps、半分辨率、一次反弹、SVGF 2 次、TAA 关闭、240 frames。世界快照的 `frame=30` 是快照 generation，不是 GI 最终 render sequence。

实际接收面 RGB 为常量 `0.49780273` 灰，源板为常量 `(0.599609,0.199951,0.098022)`。此前旧 fixture 的材质仍受纹理影响，其 `temporal×0.5` 与 reflectance `0.48` 假设不成立；不要合并两个 fixture 的数值。

| 常量材质基线 ROI | 独立半分辨率像素 | 源 HDR 积分期望 RGB | Raw / 期望 | Temporal 重乘 / 期望 | Remod / 期望 |
| --- | ---: | --- | --- | --- | --- |
| `x∈[0.9,1.1], z∈[-0.3,0.3]` | 302 | `(2222.49,774.83,405.76)` | 100.10% | 97.62–98.02% | 97.50–97.93% |
| `x∈[1.9,2.1], z∈[-0.3,0.3]` | 383 | `(996.12,347.22,181.79)` | 101.18–101.23% | 97.10–97.21% | 96.94–97.07% |

源板全部投影在屏内，加权可见积分超过 99.95%。64×128 与 128×256 源面积采样结果相差小于 0.044%；闭式几何配置因子与数值积分误差小于 0.0024%。Raw 的 8×8 全分辨率空间分块标准误约为期望的 2.67% / 3.32%；这只是空间采样的不确定性估计，不是来自独立动态序列的置信保证。该数据未显示显著的原始能量不足，不能把另一张 1 spp 截图的波动当系统偏差。

Temporal 重乘后的差异约为 −2% 至 −3%；空间滤波相对 temporal 额外变化约 −0.10% 至 −0.17%。这些静态结果只能限制归因范围，不能证明运动拖影、灯光切换或稀疏更严重区域已通过。

另一个独立比较以纯漫反射源 `L_s = rho_s * 100000 / pi` 计算，不读源 HDR。当前源 HDR 积分相对它高约 RGB `(2.3%,6.9%,14.2%)`，说明实际源着色与理想漫反射存在差异；应核查镜面等成分。读取相机方向 HDR 后假设各向同性的积分，只是与当前 SSGI 输入匹配的参考，不能称完整物理真值。

重跑示例：

```powershell
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' out/agent/ssgi-audit-20260909/constant/analyze_fixture_constant.py
```

## 光照与显示曝光响应：未去噪的受控对照

证据为 `out/agent/ssgi-audit-20260909/response/analysis-response.json`，复跑入口为同目录 `analyze_response.py`。10 个 source/raw 捕获使用 Game 常量 fixture、匹配 Editor 的相机、1920×1080 输出、8 rays / 128 steps / 半分辨率、Denoiser None、AA None；环境、AO、SSR、探针和 Bloom 关闭。world sidecar 证实太阳分别为 50000、100000、200000、100000、0 lux；EV14 曝光乘数为 0.00006103515625，EV12 为其 4 倍。比较的是场景线性 HDR，不把曝光改动算作 GI 改善。

| 运行 | d1 / d2 raw 亮度均值 | 相对 100000 lux 的前景逐像素结果 |
| --- | --- | --- |
| 50000 lux，EV14 | 529.2734 / 245.4299 | source 与 raw 精确为 0.5 倍 |
| 100000 lux，EV14 | 1058.5468 / 490.8597 | 比较基准 |
| 200000 lux，EV14 | 2117.0935 / 981.7194 | source 与 raw 精确为 2 倍 |
| 100000 lux，EV12 | 1058.5468 / 490.8597 | source 与 raw 和 EV14 完全相同 |
| 0 lux，EV14 | 0 / 0 | source 与 raw 连全画面均精确为零 |

上述比例检验配对残差为零。非零光源下 d1/d2 raw 非零率为 100% / 80.575%；全部 10 个捕获的 NaN、Inf、负值及大于等于 65504 的值计数为零。200000 lux 时 source 最大 R=37600、raw 最大 R=11224，未触及 FP16 上限。本组支持估计器的光源线性响应和显示曝光隔离，不代表时域曝光历史或动态关灯已验收。

response 两个材质文件与 constant 当前文件 SHA256 完全一致，并匹配 response 捕获输入清单；均为 SchemaVersion 2、Textures `{}`、metallic 0、roughness 1。相机矩阵最大差小于 0.000001，场景 mesh 绑定及几何一致。response 没有新增 albedo/depth/normal readback：世界 ROI 与实际量化 albedo 沿用 constant 的引导数据，这是文件、绑定和相机证据支持的近似复用，不是新的 albedo 实测。100000 lux raw 相对 constant 的捕获源面积积分为 100.245% / 103.725%；原参考使用 64 steps，本组为 128 steps，且各只有单帧，不据此宣称收敛偏差改善。

## 薄遮挡对照：源方向性与尚未定论的负偏差

证据位于 `out/agent/ssgi-audit-20260909/occlusion/`：`analysis-blackstrip-game-fixture.json`、`analysis-blackabsorber-game-fixture.json` 及可复跑 `analyze_occlusion.py --prefix blackabsorber-game-fixture`。遮挡体 bbox 为 x=[0.74,0.76]、y=[0,1.1]、z=[−0.4,0.4]。两组各捕获 source/raw/remod/depth/normal/albedo/trace，使用 8 rays / 128 steps；raw 为未去噪诊断，remod 为 SVGF 2 次输出，均无 TAA。

`blackstrip` 为黑色 dielectric、metallic 0，`blackabsorber` 仅切换到独立的黑金属材质、metallic 1，保留原材质文件。两组 depth、normal、albedo 全像素精确相同，受光板 source 精确相同，d1/d2 的 trace RGB 精确相同。因此几何与采样路径一致性得到了对应 AOV 支持。

**黑金属仍不是理想零反射吸收体。** `RasterSurfaceLighting.hlsli` 使用 `F = F0 + (1−F0)(1−VoH)^5`，F0=0 仍保留 F90=1 的掠射反射。strip 内部 5743 像素的 source 在两组都非零：dielectric 灰色均值 464.925、范围 371.75–477.5；黑金属灰色均值 1.7044、范围 1.2217–2.2871。两组 strip raw 都为零。残余相机方向镜面源不能解释成漏光，也不能把此材料称为严格黑吸收边界。

| 信号或对照 | d1 | d2 |
| --- | --- | --- |
| receiver source，两组 | 0 | 0 |
| dielectric raw 亮度均值 | 139.7231 | 233.2841 |
| dielectric remod 亮度均值 | 137.8996 | 216.7930 |
| dielectric raw / 无遮挡同像素 raw | 13.20% | 47.53% |
| 黑金属 raw 亮度均值 | 76.0667 | 222.3068 |
| 黑金属 remod 亮度均值 | 74.3397 | 205.5075 |
| 黑金属 raw / 板加 strip 相机源各向同性积分 | 62.92% | 77.32% |

CPU 积分对每个 ROI 世界点积分矩形板，采用 slab AABB 线段求交排除被遮挡路径；另计算板受 strip 投射太阳阴影的理想漫反射模型。实际 source 积分使用本组板 HDR，故包含实际阴影图和相机方向镜面；相机不可见的板样本置零，不补邻域光照。64×128 到 128×256 加密使实际板积分变化约 0.28% / 0.16%；未遮挡路径权重中可取到板 source 的比例为 95.17% / 97.68%。另计 strip 面相机源的各向同性贡献：黑金属仅约 0.2467 / 0.0337 亮度。该参考仍含方向性近似与缺失源面积，不是物理真值或严格上下界。

本次厚度 0.1 对应 trace 原点偏移 0.01 m。把积分起点改为 y=.01、保持 +Y 余弦采样后，板积分由 120.6458 / 287.4773 变为 120.3842 / 287.0187，仅改变 −0.217% / −0.160%，不能解释当前差值。相对偏移后板积分的单帧 raw 差值为 −36.81% / −22.55%；8×8 全分辨率空间簇的启发式 SEM 为参考的 14.91% / 6.03%，16×16 为 23.26% / 7.20%。d1 在较粗簇下的 95% 启发区间跨零。半分辨率复制像素及 R2 序列相关性使这些空间统计不能当作独立时间样本的置信保证；该阶段尚不能判断原始样本是否持续负偏；以下新增 16 帧与同序列/GPU 重放证据已推进此判断。

Trace RGB 仅描述**第一条射线**：终止码、背面拒绝数、到达距离。d1 的 lit/frustum/max-distance/thickness 为 348/712/42/4（1106 像素）；d2 为 201/1143/51/31（1426 像素）。两者背面拒绝与预算耗尽为零，命中黑金属仍被标 lit，因为其 source 大于 0.00001。`frustum` 表示未命中后抵达被裁剪段终点，不能证明该方向原本应命中板；thickness 终止是保守未知域处理，直接改成穿过该区域可能引入漏光。仅凭这张终止分布不足以将负偏差归因为某一机制；下节针对具体首射线补充重放，仍不构成薄遮挡整体质量验收。

### 16 个实际序列的重复观察与同样本解析重放

后续证据为 `occlusion-repeat/analysis-occlusion-repeat.json`、`analysis-analytic-replay.json`，复跑入口为同目录 `analyze_occlusion_repeat.py`、`analyze_analytic_replay.py`，逐射线数组存于 `analytic-replay-rays.npz`。16 个实际 giFrameSequenceIndex 为 326、462、603、745、893、1027、1165、1297、1436、1563、1808、1948、2085、2281、2432、2580；它们是不等距稀疏观察，不是连续视频。所有捕获实际为 8 rays，相机矩阵精确匹配；输入清单的场景和材质 hash 与当前文件匹配。全部画面的 NaN、Inf、负值和 FP16 上限计数为零。

| 时间统计 | d1 | d2 |
| --- | --- | --- |
| 16 次 raw 亮度均值 | 74.5415 | 225.2248 |
| raw / 原面积参考，含 strip 微量源 | 61.66% | 78.34% |
| 每 4 次捕获为一块的时间 SEM / 参考 | 1.58% | 1.85% |
| 对应 Student-t 启发式 95% 均值区间 / 参考 | 56.64%–66.68% | 72.45%–84.22% |
| 所有时间像素估计中的零值比例 | 0 | 30.68% |
| 16 次中永久为零的 ROI 像素数 | 0 | 0 |

时间 SEM 每次只用一个完整 ROI 均值，未将复制的半分辨率像素当独立样本。更粗的每 8 次一块只剩 2 个块，d2 区间跨过参考；R2 序列相关性、非同时 source/guide 与近似参考误差仍未包含在 SEM 中。证据表明旧单帧 62.9% / 77.3% 并非孤立波动，但不能仅据此判定某段源码有错。d2 的零值都保留在均值中；没有每方向身份就不能将其分别计作暗命中或未知 miss。

为了分离采样因素，CPU 进一步按记录的 `sampleIndex=sequence*8+rayIndex` 重放 PCG、R2 和余弦半球。trace half cell 映射到 `fullPixel=2*halfPixel+1`，每个 ROI 仍按完整分辨率覆盖权重统计。实际量化法线归一化后为 `(0,0.99999225,−0.00393866)`，因此使用此法线构造基和偏移 0.01 m，未继续假设理想 +Y。解析射线以板/slab 三维首交点及同一 source HDR 估计，保留视锥、最大距离、source 可见性与 edge fade，不模拟屏幕遍历。

同样本解析均值为 120.6989 / 287.5089；实测 raw 为其 **61.76% / 78.34%**，配对残差按每 4 次观察分块的 SEM 为参考的 1.69% / 0.70%。79.49% / 76.71% 的 cell-frame RGB 在相对 1% 或绝对 0.001 内匹配，明显差异常接近一条受光板射线贡献。CPU float32 三角函数与 GPU/FMA 不保证逐位相同，但这种局部匹配与下一步 GPU 定点重放使“整组随机方向本来缺少板贡献”的解释不再充分。

### GPU 首射线定点重放：两种已观测遗漏

`occlusion/replay326-game-fixture-{raw,trace,hituv}.pfm` 的 sidecar 均确认实际 sequence=326、8 rays、Denoiser None、匹配相机/光照/曝光；raw 与 `occlusion-repeat/static-00.pfm` **全画面逐值完全一致**。code19 输出第一射线终止码/背面拒绝数/到达米数；code20 输出接受 hitUV 和源亮度，未命中 RGB 为零。

| sequence=326，rayIndex=0 | 解析可见板目标 `(x,y,z)` / UV | GPU 结果 |
| --- | --- | --- |
| d1 half(532,345)，primary full(1065,691) | `(0,1.61654,−1.91759)` / `(0.548405,0.371396)` | trace `(6,0,1.16699)`，hitUV=0；raw RGB≈0.23950 灰色 |
| d2 half(588,368)，primary full(1177,737) | `(0,0.648314,−0.907422)` / `(0.524556,0.509903)` | trace `(6,0,1.42188)`，hitUV=0；raw=0 |
| d1 half(533,352)，primary full(1067,705) | `(0,0.846476,−1.43163)` / `(0.536846,0.470284)` | trace `(1,0,0.507324)`；接受 UV `(0.549805,0.594727)`，源亮度 2.18359 |
| d1 half(532,353)，primary full(1065,707) | `(0,0.899022,−1.48086)` / `(0.537995,0.462955)` | trace `(1,0,0.489990)`；接受 UV `(0.549805,0.597656)`，源亮度 2.19727 |

观察到的第一种机制是**三维射线绕过 slab，但中间屏幕路径进入第一层深度后方**。首行射线在 x=.76 处位于 `(.76,.282664,−.568335)`，已超出 slab 的 z≥−.4 边界；随后其投影经过 slab 前景像素，射线点约 `(.480919,.772480,−1.06380)`，对应前景表面约 `(.760438,1.02601,−.379547)`，世界间隙约 .781 m。第二行对应间隙约 .333 m。两者最终板目标仍屏内可见，但中间路径没有第一层深度表示的可验证几何；GPU 确认以 code6 终止。这具体解释了这些路径的保守丢失，不能推广为所有暗区的原因。

第二种机制是**厚度近似提前接受边缘的弱源表面**。第三行接受的 slab 表面约 `(.76076,.28254,−.39539)`，同距离解析射线点约 `(.72963,.23942,−.48238)`，仍在真实 slab 外；第四行也在 slab 的负 z 边界外。厚度 .1 的近似容许此边缘命中，使本来可达的明亮板路径提前终止在黑金属 F90 弱源上。hitUV、距离和旧 guide 为 FP16，坐标为近似；实际接受的低源亮度是 GPU 诊断直接输出。

选取的 8 个 sequence326 缺失实例中 6 个为 code6、2 个为提前接受弱源，样本是为定位而选择的，**不能据其比例分摊整个 ROI 的能量差**。继续穿过所有深度间隙或增大厚度会改变遮挡语义，可能以漏光换亮度；缩小厚度也可能加重未知终止。当前证据用于约束下一步几何/多层表示实验，不把遗漏自动视为都能由单层 SSGI 无偏恢复。

## PBR 可见源积分：原生精度与积分域复核

证据位于 `out/agent/ssgi-audit-20260909/transport/`：旧 `analysis-pbr-visible-integral.json`，以及原生深度复核 `analysis-pbr-visible-integral-native.json`、`analyze_pbr_visible_integral_native.py`。右侧归一化 ROI 为 `(0.6,0.25)–(0.92,0.85)`。CPU 使用源表面局部深度导数面积、双正余弦和 48 个分层接收点，没有做接收点到源面的遮挡测试；未计屏幕外源，输入也不足以证明所有接收面的 metallic/roughness。

原生 RG32 深度与 PFM 平均绝对差约 `0.00133` 世界单位，原生 Normal/Source RGB 与旧 PFM 逐像素相同，所有世界快照一致。使用原生深度后，512/1024 源网格积分亮度为 `1.3816/1.4429`，但导数半径 1/2/4 的敏感性仍约 32.06%。因此，不能把此前的不稳定归因于 FP16 深度精度。

固定三个导数半径的共同源面集合后，导数敏感性降至 **0.084%**；这证明主要变化来自边缘过滤改变了积分域。共同域的 512/1024 网格积分为 `0.9256/0.8782`，差约 5.12%。它保留约 89.17% 亮源像素，却只保留原估计贡献的 66.99%；不能用这个较稳定但更小的源域代替全部可见源面。

**拒绝将这些数值作为右侧合理期望、严格上界或验收依据。** 后续需要可靠处理被删除的边缘和薄几何源面积，并检查遮挡；继续提高深度精度本身不足以解决参考的不确定性。

## 真实 PBRTest Editor：Low2 最终阶段与首命中证据

`out/agent/ssgi-audit-20260909/final-editor/analysis-final-pbr.json` 汇总真实 Editor 捕获，非受控 Game fixture。相机 `(0,2.5,11)`；原始 Editor UI viewport 为 1399×596，诊断捕获的 Renderer offscreen 为 1920×1080，仍保留宽高比约 2.3473 的同一投影。场景为 1 个 Mesh 对象、25 个 GPU section，不能把 world objectCount=1 理解成单个表面。太阳 100000 lux，环境强度 0、路径为空，曝光乘数 0.00006103515625（EV14）。最终捕获的实际 GI sequence=239、2 rays / 64 steps / half / SVGF 2 次；AA None、Bloom 0、自动曝光关闭。正常用户 TAA/后处理配置的最终结果与性能另待补充。

相机、矩阵、视口、曝光、环境、方向光及场景计数与基线逐项匹配；**基线与最终 source HDR 的全部 6220800 个 RGB 分量逐值完全一致**，没有通过增加源亮度解释改善。world.PointLightCount=1 只表示存在点光组件；本地 `Project/Assets/Scenes/PBRTest.he:4` 明确记录其 Intensity=0.0，不能由数量推断实际点光贡献。该点光不作为本轮右侧受光解释。source 的语义仍包含相机方向镜面和材质自发光，因此“正源命中”不能直接等同纯太阳漫反射；需按实际源分量核验。

ROI 经有限正线性深度、四邻域深度变化≤1%、法线邻域点积≥.9、albedo 邻域距离≤.25及一次腐蚀筛选，保留黑材质。右侧有 305735 像素、左侧 164279 像素、接触带 5023 像素。没有 surface/material identity AOV，不能排除相同法线的重叠不同表面；引导缓冲来自基线，未独立 hash 全部最终 mesh/material 内容。

| 右侧阶段 | 基线 1 ray | 仅方差预滤 A/B，1 ray | 最终 Low2，2 rays |
| --- | --- | --- | --- |
| Temporal 去材质亮度均值 | 13.69567 | 13.69567 | 13.65009 |
| Filtered 去材质亮度均值 | 12.70422 | 13.22498 | 13.30218 |
| Spatial 相对 Temporal 的均值损失 | 7.24% | 3.44% | 2.55% |
| Remod 出射 GI 亮度均值 | 1.106667 | 1.155708 | 1.162140 |
| Remod p50 / p95 / p99 | .56879 / 4.39469 / 7.14944 | .59577 / 4.55176 / 7.39314 | .64704 / 4.63069 / 6.86370 |

基线→仅预滤组保持同预算和 Temporal，右侧 Remod 增加 4.43%，支持减少空间阶段能量删除。**基线→最终右侧增加 5.01%，其中同时包含 rayCount 1→2 与响应逻辑，不得全部归因方差预滤。** 最终左侧/接触带 Remod 均值为 .815565 / .828257；阶段数值只能在相同去材质语义下直接比较，单次 2-ray raw 不作收敛或能量守恒真值。

右侧 source 与 GI Off 都精确为零；正常 Composite 与 `Off+Remod` 的 RGB 平均绝对差为 8.04e−8、p99=0，支持该 ROI 合成阶段没有再次删除 GI。左侧有高亮直接光，差值与 FP16 相加量化相符，不能用相同绝对误差阈值比较。全部汇总 AOV 的 NaN、Inf、负值和 FP16 上限计数为零。此证据确认了阶段贡献与合成关系，不代表最终显示观感、连续动态或全部路径恢复已经验收。

首射线 hitUV 在右 ROI 的 77638 个展开像素上记录命中，即 25.39%；其中 14121 个为正源，约占命中的 18.19%、右 ROI 的 4.62%。按 source UV 区域分类：命中左 ROI 的 3763 个中 2146 个正源；命中右 ROI 的 19160 个全部为零源；其余区域 54715 个中 11975 个正源。因此答案是“确有从右侧命中左侧正源的路径，但它只是稀疏贡献的一部分”，并非完全无命中或所有 hit 都受光。

只有 59.72% 的命中 UV 落在源引导的有效内部区域。有效部分的近似接收点→源距离均值 3.30 m、p95=5.96 m、p99=8.47 m，不能仅以最大距离 10 m 推断整个场景超出范围。首射线统计也不能代表全部两条射线的半球概率；half AOV 的 UV 量化会影响边缘世界位置。具体源信号仍需区分太阳漫反射、相机方向镜面与可能的材质自发光；已核实零强度点光不计作贡献。已有 PBR 面积积分的不稳定性也没有因这组命中数据自动消失。

## 稀疏动态序列与最终 TAA 的 CPU 分析

三组目录分别为 `out/agent/ssgi-audit-20260909/dynamics/`、`reactive-dynamics/`、`taa-dynamics/`，各自有 `events.json`、捕获 PFM、`.world.json`、`settings.json` 和 `analysis-dynamics.json`。其中“动态基线”指变换响应修复之前的这组运行，不是本轮所有源码修改之前的原始版本。Reactive 为变换响应调整后，TAA 组为正常 Composite + TAA。均使用同一常量矩形 fixture、1 ray、64 steps、半分辨率、SVGF 2 次；这与前节 8 rays 的收敛参考配置不同。

TAA 组有效设置为 `Rendering.Debug.GI.View=0`、`Rendering.AntiAliasing.Mode=TAA`，自动曝光、Bloom、SSR、反射探针与环境贡献均关闭。源码中 `AddTaaPasses` 位于 `LinearHdrCapture` 之前，所以此次 PFM 包含 GI 合成与 TAA，尚未经过 tone mapping。接收面 ROI 的常量参考 Source HDR 严格为零；太阳沿 −X，而地板法线为 +Y，且世界快照无其他灯光，因此没有把源板的强直接光或全屏直接光均值纳入接收面统计。该确认结合几何、材料与已有 source 捕获，并非额外取得了每个 TAA 事件的 source readback。

下面每个值按 d1/d2 顺序列出。静态均值以独立源 HDR 面积积分为分母；每像素时间标准差也以该积分均值归一化。八个静态样本是间隔观察，不是连续八帧。

| 组别 | 静态均值 / 参考 | ROI 均值相对标准差 | 每像素时间标准差 / 参考 |
| --- | --- | --- | --- |
| 动态基线，无 TAA | 91.26% / 96.54% | 0.487% / 0.425% | 3.85% / 10.49% |
| Reactive，无 TAA | 91.24% / 95.78% | 0.339% / 0.660% | 3.50% / 10.45% |
| Reactive + Composite + TAA | 91.93% / 93.36% | 0.462% / 1.426% | 1.10% / 4.70% |

TAA 降低了这八次观测的每像素变化，但 d2 的 ROI 均值波动上升，且均值仍低于参考约 6.6%；不能将其概括为无偏、所有时间尺度都更稳定。此表同时受低采样、时域滤波、TAA 与取样相位影响，不能单独归因为某一个 pass。

板移出后的数值是“剩余信号 / 本组静态均值”，不直接等同纯拖影。板实际从 `z∈[-2,2]` 移到 `[4,8]`，独立完整几何配置因子仍为原来的约 0.95% / 3.45%；新位置下的可见屏幕贡献和源着色没有单独建立高采样参考。

| 组别 | 移板后实际 2 帧 | 实际 5 帧 | 实际 17 帧 | 实际 33 帧 | 恢复后实际 33 帧 / 原参考 |
| --- | --- | --- | --- | --- | --- |
| 动态基线 | 94.45% / 97.81% | 85.65% / 88.82% | 59.82% / 60.91% | 36.51% / 38.42% | 62.66% / 61.69% |
| Reactive | 74.87% / 75.37% | 31.68% / 33.09% | 1.24% / 2.50% | 0.675% / 1.039% | 96.51% / 98.30% |
| Reactive + TAA | 75.32% / 76.15% | 32.03% / 32.85% | 1.316% / 2.753% | 0.558% / 1.603% | 98.89% / 99.75% |

实际延迟取 `world.frame - mutationRenderFrame`，不能使用 readback 的 `settledRenderFrame`。例如名为 `board-out-16`、`board-out-32` 的事件实际是 17、33 帧。旧动态基线的关灯样本在实际 2–33 帧均为零；旧组平移、旋转和相机切换仅有每次改变后约 2 帧的单次观察。Reactive/TAA 的 13 事件子集没有重跑完整关灯、平移、旋转和切换序列，不可把旧组这些证据自动归到最终配置。

TAA 抖动在 `SceneRenderer` 内应用到 `CurrentRasterCamera`；world sidecar 当前保存的是世界快照的相机矩阵，不含实际 raster jitter。分析复用无 TAA 常量参考的内部地板 ROI，并按未抖动矩阵对齐，因此在此大平面内部是近似的世界位置取样，不是严格逐样本重投影。相机矩阵、像素相位、边缘及实际 TAA 输出位置的差异会影响每像素方差；没有本事件的原生深度/表面身份捕获可验证显露边界。

上述结果没有覆盖 continuous video、拖影长度、完整遮挡显露、连续物体运动、完整最终配置的灯光开关或时间频谱。工具生成的旧模板文字中“No final TAA”不适用于 `taa-dynamics`：本段依据有效设置和 pass 顺序修正其解释，不能将该模板句视为本次捕获事实。

重跑 TAA 分析：

```powershell
& 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' Tools/Agent/Analyze-SSGIStepResponse.py --capture-directory out/agent/ssgi-audit-20260909/taa-dynamics --reference-directory out/agent/ssgi-audit-20260909/constant --compare out/agent/ssgi-audit-20260909/reactive-dynamics/analysis-dynamics.json
```

## 短历史的 8-ray 动态窗与 Low2 候选

补充证据为 `adaptive-low/`、`adaptive-taa/`、`adaptive-low2/` 中各自的 `analysis-dynamics.json` 与 `analysis-adaptive-summary.json`，均在本轮证据根目录下。每组 16 次稀疏捕获：8 次静止、8 次接近静止的变换触发观察。生产端约定每次把源板 z 增加 0.00001，requested delay=12，world 帧差实测为 13；world sidecar 不含逐物体 transform，不能独立核验每次增量。该协议主要隔离缩短历史后的噪声与均值变化，不是连续运动、真正遮挡切换或连续视频。

当前 SSGI 在 GeometryTransformGeneration 改变后开启 16 帧全局响应窗：时域当前帧最小权重 0.25、最大历史长度 4，并将实际射线数提升到至少 8；每帧结束递减计数，持续变换可反复延长窗口。捕获确认 `adaptive-low`/`adaptive-taa` 静止实际为 1 ray、触发后为 8 rays；Low2 静止为 2 rays、触发后为 8 rays。射线预算提升仍按总射线数归一化，不按有效命中数补偿，也不添加环境能量。历史旧信号的简单权重模型 0.75^16 小于 1.1%，只说明该局部混合机制，不等于整个滤波/TAA 链已满足同样残留上限。

以下均值和逐像素时间标准差均除以 constant 独立捕获源积分；每格为 d1 / d2。标准差来自 8 次分离观察，不是连续帧频谱。

| 配置 | 静止均值 / 参考 | 触发后第 13 帧均值 / 参考 | 静止逐像素标准差 / 参考 | 触发后逐像素标准差 / 参考 |
| --- | --- | --- | --- | --- |
| 1 ray→8 rays，Remod，无 TAA | 91.24% / 95.87% | 97.96% / 97.45% | 3.68% / 10.09% | 2.13% / 10.47% |
| 1 ray→8 rays，Composite+TAA | 92.07% / 93.69% | 98.31% / 98.15% | 1.26% / 5.14% | 1.88% / 8.53% |
| Low2 候选：2 rays→8 rays，Remod，无 TAA | 97.23% / 94.03% | 99.03% / 97.29% | 3.95% / 3.71% | 2.25% / 9.76% |

Low2 与 adaptive-low 的有效配置仅 RayCount 1→2 不同，仍为 64 steps、半分辨率、SVGF 2 次。它在 d1 把触发前后均值变化从 +7.36% 缩小到 +1.85%，d2 却由 +1.64% 增至 +3.46%。d2 静止逐像素标准差下降约 63.2%，d1 则增加约 7.5%；不能将 2 rays 描述为所有 ROI 都更平滑或无亮度跳变。Low2 的最终 Composite+TAA 尚未在这批证据中覆盖。

8-ray 动态窗让第 13 帧均值更接近本场景参考，也付出更大追踪预算；TAA 组触发后逐像素噪声仍为自身静止值的约 1.50 / 1.66 倍。Low1、Low2 触发时射线调用预算分别为静止的 8 倍、4 倍，这不是 GPU 毫秒倍数。持续物体运动可使全局高预算持续存在。此处诊断捕获不能计作正式性能测量；缺少窗口内/外及退出瞬间的交错 GPU pass 时间，也缺少 Low2 两分辨率正式成本，不将下一节旧 1-ray Low 或 2-ray/96-step Medium 成本替代它们。

三组太阳均为 100000 lux、环境 0、曝光乘数 0.00006103515625，所有分析样本非有限值计数为零。Composite+TAA 的接收地板直接光为零，因此该内部 ROI 主要比较 GI 加 TAA；仍沿用未抖动参考 ROI，不能扩展至边缘的逐样本正确性。`adaptive-taa` 的有效设置证实最终 TAA 开启；分析 JSON 中沿用的“Final Composite TAA is not established”模板句不适用于该组，应以 pipeline 和 settings 为准。

## 独立 GPU 性能运行与预设候选

数据来自 `out/agent/ssgi-audit-20260909/performance/analysis.json` 及其逐 pass CSV、settings 和 memory sidecar；场景为受控 Game PBR 副本，同一机器和非 GI 设置，覆盖实际画布 1399×596 与 1920×1080。每 run 总计 720 帧，预热 120 帧；解析器按独立正 GPU sample ID 与各行 warmup cutoff 选择 **597 个 GPU 样本**，再按相同 sample ID 连接 pass 时间。实际选中的 CSV frame 为 124–720。计时运行不使用诊断 HDR readback；CSV 中普通 `Capture` 图节点存在不表示执行了诊断捕获。

本次硬件为 NVIDIA GeForce RTX 3060 Laptop GPU，后端 DX12；telemetry 报告驱动 591.74。这里测的是同机成本，不是预设已满足所有画质要求的声明。下表 Low 特指既有静止 1-ray 预算，不能代表新增 Low2 或运动触发的 8-ray 窗口。Off 与各 On run 交错记录，但不把其整帧差当 SSGI 独占成本；追踪和去噪使用对应 pass GPU 时间。CPU 时间不参与这些表格。

| 档位 | Rays / Steps | RenderScale | A-trous | 处理 |
| --- | --- | --- | --- | --- |
| Low | 1 / 64 | 0.5 | 2 | 保留低成本路径；稀疏动态验证采用这一采样预算 |
| Medium | 2 / 96 | 0.5 | 3 | 中档候选，收益需结合对应画质捕获 |
| High，halfhigh | 4 / 128 | 0.5 | 3 | 本轮保留的 High 配置；1080p 整帧均值 9.14 ms |
| 原 full-resolution High 候选 | 4 / 128 | 1.0 | 3 | **拒绝作为最终 High 预设**：1080p 整帧均值 24.44 ms，成本明显高于 halfhigh |

所有预设一次反弹、最大距离 10、厚度 0.1；这些是本场景量测配置，不是通用世界尺度推荐值。

| 分辨率 | 档位 | 整帧均值 / P50 / P95 / P99，ms | 对应 Off 均值，ms | GI 独占 pass 均值合计，ms |
| --- | --- | --- | --- | --- |
| 1399x596 | Low | 2.658 / 2.608 / 2.964 / 3.135 | 1.153 | 1.323 |
| 1399x596 | Medium | 3.497 / 3.415 / 3.824 / 3.931 | 1.252 | 2.116 |
| 1399x596 | High | 4.161 / 4.070 / 4.536 / 4.723 | 1.104 | 2.784 |
| 1399x596 | Full High（拒绝） | 10.183 / 10.125 / 10.772 / 10.916 | 1.079 | 8.766 |
| 1920x1080 | Low | 5.203 / 5.065 / 5.653 / 5.726 | 1.603 | 3.072 |
| 1920x1080 | Medium | 7.317 / 7.128 / 7.739 / 7.799 | 1.594 | 5.189 |
| 1920x1080 | High | 9.141 / 9.149 / 9.694 / 9.825 | 1.586 | 6.991 |
| 1920x1080 | Full High（拒绝） | 24.443 / 24.481 / 24.785 / 24.864 | 1.601 | 22.266 |

GI 独占合计包含 trace、demodulation、temporal、spatial、upsample、remodulation 和 composite；不含共享 depth pyramid / surface guides。Spatial 分类包含 HistoryFix、MomentsAndVariance 及所有 A-trous 迭代。下面为各分类均值，单位 ms；“—”表示全分辨率路径不需要该上采样阶段。没有单独报告出来的源颜色复制成本不能凭整帧差虚构分摊。

| 分辨率 / 档位 | Trace | Temporal | Spatial | Upsample | 去材质 + 重乘 | Composite | 共享 Depth | 共享 Guides |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1399x596 / Low | 0.315 | 0.196 | 0.508 | 0.194 | 0.068 | 0.043 | 0.071 | 0.097 |
| 1399x596 / Medium | 0.632 | 0.205 | 0.968 | 0.201 | 0.067 | 0.043 | 0.095 | 0.102 |
| 1399x596 / High | 1.299 | 0.203 | 0.976 | 0.201 | 0.065 | 0.041 | 0.093 | 0.099 |
| 1399x596 / Full High（拒绝） | 5.019 | 0.383 | 3.230 | — | 0.088 | 0.046 | 0.098 | 0.103 |
| 1920x1080 / Low | 0.715 | 0.417 | 1.126 | 0.570 | 0.144 | 0.101 | 0.145 | 0.232 |
| 1920x1080 / Medium | 1.635 | 0.414 | 2.324 | 0.576 | 0.144 | 0.098 | 0.145 | 0.230 |
| 1920x1080 / High | 3.428 | 0.441 | 2.308 | 0.563 | 0.152 | 0.100 | 0.144 | 0.238 |
| 1920x1080 / Full High（拒绝） | 13.281 | 0.883 | 7.797 | — | 0.201 | 0.104 | 0.158 | 0.236 |

内存取同次运行的后预热观察。Transient 为 renderer transient pool **预留量**，不是当前 live texture 大小；Local Usage 为进程本地 GPU 内存观察值，不是整张显卡已用显存。表中 On−Off 只表示配置切换时的观察差，不能精确归属单个新增缓冲。每 run 的 memory 查询是稀疏观察，不能证明真正峰值；源资源格式与原生分辨率仍应结合资源清单核查。

| 分辨率 / 档位 | Transient On / Off，MiB | 进程 Local On / Off，MiB | 进程 Local 差，MiB |
| --- | --- | --- | --- |
| 1399x596 / Low | 474.31 / 296.96 | 1066.28 / 848.65 | 217.63 |
| 1399x596 / Medium | 479.09 / 296.96 | 1071.66 / 848.65 | 223.01 |
| 1399x596 / High | 479.09 / 296.96 | 1071.66 / 849.71 | 221.95 |
| 1399x596 / Full High（拒绝） | 581.63 / 296.96 | 1248.16 / 849.71 | 398.45 |
| 1920x1080 / Low | 894.02 / 453.04 | 1577.88 / 1030.37 | 547.51 |
| 1920x1080 / Medium | 905.89 / 453.04 | 1591.88 / 1030.37 | 561.51 |
| 1920x1080 / High | 905.89 / 453.04 | 1591.94 / 1030.37 | 561.57 |
| 1920x1080 / Full High（拒绝） | 1160.99 / 453.04 | 2006.88 / 1030.37 | 976.51 |

独立 `*.gpu.csv` 的 1 秒 telemetry 包含启动、加载和退出，不与上表 597 个 GPU sample 一一对齐。以下只报告实际观察范围，不据此宣称固定频率或严格热稳态。

| 分辨率 / 档位 | Telemetry 观察数 | 温度范围，°C | Graphics clock 范围，MHz |
| --- | --- | --- | --- |
| 1399x596 / Low | 33 | 56–64 | 210–2002 |
| 1399x596 / Medium | 27 | 56–65 | 210–1905 |
| 1399x596 / High | 16 | 56–66 | 210–2002 |
| 1399x596 / Full High（拒绝） | 21 | 55–66 | 210–2002 |
| 1920x1080 / Low | 17 | 53–63 | 210–2002 |
| 1920x1080 / Medium | 18 | 54–65 | 210–2002 |
| 1920x1080 / High | 19 | 55–66 | 210–2002 |
| 1920x1080 / Full High（拒绝） | 30 | 54–71 | 210–2002 |

性能表来自专门计时运行；稀疏动态表来自诊断捕获，两者不能混用。以上成本选择、静态噪声及少量阶跃响应不构成最终画质验收。


## 缺失路径的最小补充候选：数据职责与存储估算

以下仅是依据本次路径证据的工程候选，未实现、未测 GPU 时间，也不属于纯单层 SSGI 改善。先区分需要补充的是路径中间的几何/可见性，还是屏幕外的入射光源；新增辐射缓冲不能自动解决未知遮挡。探针可见性的重要性可参考 [DDGI，JCGT 2019](https://jcgt.org/published/0008/02/01/)，光源数据与遮挡数据分离可参考 [Layered Reflective Shadow Maps，2013](https://diglib.eg.org/items/41a14f8e-37f3-4027-9563-dbe61f635a59) 的一手摘要；下列格式和字节预算是本项目候选设计，并非论文或参考引擎的实测配置。

| 候选 | 补充的数据与针对的缺失 | 单缓冲粗略字节公式 | 必須额外验证的代价/限制 |
| --- | --- | --- | --- |
| 第二深度层或明确前后表面区间 | 更接近本次 slab 路径问题：补充第一层后方的几何，必要时附该层法线和预 GI 辐射。仅存“第二个深度”不能证明层间所有空间可穿越，需明确前/背面、实体区间和表面身份 | 仅 R32 depth 为 `4WH`；depth4 + packed normal4 + RGBA16F radiance8 为 `16WH`；若建立完整额外深度 mip 链约再加 `4WH/3` | 额外 raster/depth peeling、双层遍历、绑定和带宽；薄边层顺序、交叠物、离屏与两层之外仍未知。需先用两个已定位射线验证，不可默认两层解决全部路径 |
| 局部辐照度探针及可见性 | 提供相机外/遮挡后方的低频漫反射入射光照；可见性数据防止跨墙插值。必须由实际场景光源更新，环境关闭不等于加入固定环境常量 | 示例 9 系数 RGB FP16 为 `54N`，FP32 为 `108N`；若每探针可见性是 6 面 r×r、两通道 FP16 矩，为 `24Nr²`；位置/索引、atlas 边界、双缓冲另计 | 探针更新的 raster/RT 成本、密度与显存、动态光响应、可见性误差；更新周期和多次反弹语义需单独定义，不能把探针结果记作 SSGI 无偏恢复 |
| 单太阳 RSM | 从光源视角记录位置/法线/反射通量，补充屏幕外但被太阳照亮的源。法线/通量可用纯漫反射语义避免当前相机镜面源偏差 | 用 depth 重建位置时，depth4 + packed normal4 + flux RGBA16F8，约 `16S²`；若复用兼容 shadow depth，可少 `4S²`，其原有成本不能忽略 | 新光源 raster/MRT、采样与重建成本；光源视角缺失、点光/发光未覆盖；接收点到虚拟源的可见性仍需解决，不能忽略遮挡直接增亮 |

仅作存储算术示例：`16WH` 在 1920×1080 为约 31.64 MiB，在 960×540 为约 7.91 MiB；一个 1024²、16 bytes/texel 的 RSM 为 16 MiB。以上不含资源对齐、mip、历史、临时 RT、描述符、分配器或多帧保留，更不是显存总占用或 GPU 毫秒预测。

针对已验证 slab 路径，最小下一步优先评估可证明前后表面区间的受限几何补充；若实际需求是相机移走后仍持续得到太阳反弹，再独立评估 RSM 或可见性探针。所有补充都应有自己的 Off/On、能量与遮挡验收及成本表，不能合并进本轮纯 SSGI 的改善百分比。

## 当前验收缺口

诊断视图切换仍有一个 bootstrap 边界：若 Source/Raw 视图中发生历史失效、Temporal 被裁剪超过 16 个已提交帧，8-ray 窗口会正常结束，但待执行的历史 reset 仍保留；随后仅切回 Composite 且未触发其他 reset 时，首次有效时域历史可能从静态预算开始。这不影响正常 Composite 中的灯光/相机 reset，也不会使诊断视图永久停留在 8 rays。最小后续改进是在重新启用 Temporal 且仍等待 reset 时单次重启窗口，不能恢复为只在 Temporal 执行时倒计时。

完整视觉验收仍未完成。后续最终捕获、构建、计时已经执行，结果汇总在前述独立结果/性能报告；本节只保留尚未覆盖的范围：

- 真实 PBRTest Editor 的受控Low2阶段A/B、实际FXAA/AO/Bloom配置截图已交付；不稳定PBR面积积分仍不能替代真值，源镜面方向性近似仍存在。
- Low2+TAA 的持续运动四状态、sigma3 和 bootstrap41事件并非连续视频。最终source/法线及更新积分已核验一致，三档启动sequence7/31/119和持续动态成本已补；仍缺每个中间帧的窗口退出、拖影与显露边缘完整验收，且远ROI退出高预算后仍有负偏差。
- 薄遮挡 16 帧与首射线重放已定位未知后方终止和厚度边缘提前接受两类路径；仍需覆盖更多方向、薄边/遮挡显露的表面匹配 ROI，定量解释整个残差并验证候选改动的漏光代价。F90 残余和参考的方向性限制仍有效。
- 最终计时覆盖复杂静态PBR和持续运动的小fixture，不是复杂PBR持续动态的最坏成本；进程显存稀疏观察不能证明瞬时峰值或全部逐资源驻留。

已有固定曝光线性响应与零光源结果支持正确性的一部分；它们不覆盖动态历史曝光、正常显示链的全部行为，也不证明右侧所有缺失路径已恢复。
