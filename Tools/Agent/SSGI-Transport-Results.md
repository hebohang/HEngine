# SSGI 本轮结果与验收边界

本报告对应本地 `f41ea9b0d2bac10b2b1bbaadb7f5daee700e2a9a` 上的未提交修改，证据根目录为 `out/agent/ssgi-audit-20260909`。机制研究、完整实验和版本边界见 [详细记录](SSGI-Transport-Validation.md)。`accepted-*` 表示最终二进制复核组，**不表示所有视觉验收项目已通过**。

## 结论

已修复可以证实的诊断显示、去噪能量损失、运动钳制、历史失效后的采样稳定性和默认法线问题。真实 PBRTest Editor 同机位、太阳 100000 lux、环境关闭、固定 EV14 的最终对照中，源 HDR、深度、法线及反照率与基线逐通道一致；右侧有效表面 GI 均值从 **1.106667 增至 1.162168（+5.015%）**，Temporal→Filtered 的损失从 **7.24% 降至 2.55%**。这里没有改变材质、光强、曝光或 GI 倍率；+5.015% 包含 Low 从 1→2 rays，不能全部归功于方差预滤。保持 1 ray 的单机制预滤对照为 +4.43%。

**正常显示仍有大片很暗的区域。** 本轮没有把它们伪装成完整 GI 已恢复。右侧首射线约 25.39% 命中屏幕表面，只有约 4.62% 命中非零源光；确有射线到达左侧太阳受光表面。另有被单层屏幕深度遮蔽的中间路径，薄遮挡重放已定位两类具体失败：未知后方厚度终止、厚度边缘提前接受弱源。不能靠增加曝光或无限厚度修复。

受控可见反弹场景的最终 Low 无 TAA 单次 Composite 在两个世界 ROI 达到更新面积积分参考的 **97.35% / 93.36%**。该独立数值积分用于检查可见输入的传输，假设相机方向的源辐射各向同性，仍含镜面偏差，不是物理真值。纯 Lambert 解析源另有对照，不能与相机 HDR 参考混称。

## 环境、基线与保护

- Windows x64，RTX 3060 Laptop GPU，驱动 591.74；DX12 为主测后端。VS2026 v145 / MSVC 14.51.36231，独立 Ninja `out/agent`，Game/Editor 为 RelWithDebInfo，PACK_GAME=OFF。
- Editor 窗口/捕获 1920×1080，真实场景画布 **1399×596**，投影纵横比约 2.3473。原始相机 `(0,2.5,11)`，FOV60。正式性能分别直接渲染 1920×1080 和 1399×596，不能混淆窗口大小与场景画布。
- 隐藏 EnvironmentLightComponent 同时取消其 EV12 覆盖，原实际生效值为全局 EV14、曝光乘数 `2^-14`。先按实际值复现，再在**场景副本**添加现有全局 PostProcessingComponent：`bOverrideExposure=true, ExposureEV100=14, ExposureCompensation=0, BlendWeight=1`。没有改变“隐藏光源停止贡献”的语义。
- PBRTest 的一个 point light 强度实际为0；环境为0。将太阳置零的独立 source HDR 捕获逐像素全零，支持把该场景非零源归于太阳照明，但源仍含相机方向镜面。
- 原场景 SHA256：`18b24149055a5497f7d661327241095329424cfeb86f65343d58c4f867134b72`。所有受控场景/材质位于 `Project/Assets/RenderingBenchmarks/Validation/SSGITransport/` 的独立子目录。原场景、材质、相机和用户配置未用于写回；没有提交 Git、修改 ThirdParty 或手改生成输出。
- 最终使用配置恢复了基线 FXAA、AO、Bloom 等非 GI 设置，独立 Editor PNG 保留。TAA 的受控动态/质量测试是另一组，不能称用户原配置为 TAA。

## 代码与缓冲语义

| 环节 | 本轮修改及依据 | 有效证据 |
| --- | --- | --- |
| 调试/显示 | 数据视图按含义映射；Confidence/Coverage/Albedo/Normal/History 等绕过曝光、ACES、调色与 AA。辐射视图仍走明确的场景曝光规则 | Normal 注册 ROI 99.882% 像素平均误差小于8码值；右侧 Confidence 99.915% 为255 |
| 线性捕获 | 加入 PFM/native RAW、不可覆盖的 world sidecar、原始 alpha/双通道 readback、实际 ray/sequence 元数据 | 运行快照、ROI统计、readback unit test；不以 PNG 判断能量 |
| 追踪诊断 | 可关闭的首射线终止分类、背面拒绝次数、距离、HitUV；不把 Coverage/Confidence 当命中率 | PBR 步数64→128预算耗尽3.94%→0.78%，raw均值仅+3.29%，不支持“最大距离10太短”的预判 |
| 空间滤波 | SSGI 独有、几何/材质门控3×3方差均值预滤，仅用于 à-trous 的亮度边缘权重 | 同预算 Temporal不变而空间损失减小；没有修改 radiance/矩、删除黑样本或加邻域亮度补偿 |
| 运动历史 | SSGI 移动时亮度区间从2σ改3σ；RGB统一缩放保颜色 | 可见板相机运动最差 ROI 从参考81.60%升至93.88%；增加到4 rays不能代替这个修复 |
| 次级几何变化 | 对全部可见 opaque/masked 深度写入物体及骨骼的变化建立 fingerprint，排除相机；16提交帧 min blend .25 / history cap4，至少8 rays | 板移出33帧残留约0.52%/1.48%静态；持续运动会持续付出8-ray成本 |
| 启动/灯光重置 | 同一短历史窗口用于实际 reset，减少稀疏重启跌落；成功提交独立扣减，Temporal被裁剪也能退出高预算 | 开灯age3从参考81.82%/68.39%变为98.37%/98.58%；关灯全部七次观测逐像素0；Raw frame239实际回到2 rays |
| 默认法线 | 仅1×1 fallback改为精确半浮点(.5,.5,1,1)，8 bytes；UNORM8的128不能精确表示.5 | Forward暗面错误直接光约63.2→0；DX12/Vulkan两路径复核；实际法线图编码不变 |

一次反弹余弦采样满足 `p(ω)=cosθ/π`，Lambert `f=rho/π`，所以每条射线贡献为 `rho * L_hit`；最后除**全部**射线数。未知/未命中方向采用黑色近似，真实黑命中仍为零样本。没有只除有效命中数、额外1/r²、重复π、环境补光或反馈增益。预设统一一次反弹，无上一帧颜色反馈计光。

| 捕获 | 单位及内容 | 分辨率/解释 |
| --- | --- | --- |
| source | 场景线性、合成 GI 前 opaque HDR；直接漫反射+相机镜面+自发光，有环境时也可能含 IBL | 全分辨率；本轮环境关闭，PBR太阳零对照全零 |
| raw | 一次反弹出射间接辐射，已乘接收材质；alpha为屏幕命中比例 | 预设半分辨率；PFM为了诊断最近邻展开到全分辨率，不能把展开像素当独立样本 |
| demod / temporal / filtered | 去材质后的漫反射光照信号；这三者才能直接做阶段能量比较 | 半分辨率；Temporal矩为RG32，方差R32；这些精度修复在初始HEAD已有 |
| remod | 上采样并重乘接收材质后的 GI | 全分辨率；右侧 `Composite−Off−remod` 平均RGB绝对差1.53e-8 |
| composite | GI合成后、tone map前场景 HDR | 全分辨率；左侧加法存在正常FP16舍入，不能误报漏能量 |
| PNG | 场景曝光、ACES/sRGB及实际显示链 | 仅观感；交付图的EV8 GI行明确标为诊断，不计画质增亮 |

所有这些 HDR 是场景线性值，未存储预曝光历史；显示 EV12/EV14 的 raw 完全一致，50k/100k/200k lux 未饱和 raw 逐像素按0.5/1/2响应。没有因为100000 lux就升级所有GI缓冲。

## 参考与选择

详细的“参考机制—现状—候选—收益—验证”表及公式在[机制记录](SSGI-Transport-Validation.md)。采用 Epic **UE5.6 standalone SSGI**、**UE5.6 Lumen Technical Details**，Unity **HDRP17.0.4**（Graphics commit `275a7f9ad9330e3cf7f3ea57a0b242a551fde291`）和 **SVGF2017 §4.4**。Lumen 的屏幕外数据、HDRP默认sky/probe/APV回退均没有冒充独立 SSGI。HDRP 的预曝光亮度上限不直接移植到本项目场景线性域。STBN只研究，未采用、未声称收益。

项目 HardwareRTGI 当前代码限定1 spp，次级材质使用 `albedo*(1-metallic)` 的纯漫反射路径，和 raster/source 中 F0及相机镜面语义不同。本轮没有将它的图像当作匹配真值，也未扩建离线渲染器；采用矩形板面积积分、遮挡AABB可见性及同随机序列CPU射线重放提供独立依据。这仍留下真正多次反弹/方向性光照参考的边界。

已额外执行 HardwareRTGI+SVGF 共享资源回归：DX12在场景Ready后切换成功、120步后HDR非零且NaN/Inf/负值为0，最大通道18800。不能把它计入纯SSGI改善。直接以HardwareRTGI启动的检查 **FAIL**：初始HEAD已有Game等待首次present才加载scene、RTGI却要求非空RayTracingScene的契约冲突；本轮未改此启动流程。Vulkan运行报告缺少所需RT能力并将GI禁用，明确记为**未覆盖/不可用**，不是HardwareRTGI通过。

## 质量预设与动态

最终三档均 half resolution、一次反弹、距离10、厚度.1、temporalWeight .97；这些距离参数适用于已测世界尺度，不是通用尺度保证。动态/历史重置窗口至少8 rays，静态结束后回配置预算。

| 档位 | 静态 rays / steps / refine / à-trous | 8次TAA观察均值/参考 d1 / d2 | 逐像素标准差/参考 d1 / d2 |
| --- | --- | --- | --- |
| Low | 2 / 64 / 3 / 2 | 97.249% / 93.463% | 1.395% / 3.072% |
| Medium | 2 / 96 / 4 / 3 | 97.300% / 93.965% | 0.969% / 3.142% |
| High | 4 / 128 / 6 / 3 | 97.599% / 94.415% | 0.256% / 1.868% |

High减少静态噪声；Medium远处ROI噪声没有逐项优于Low。8个观察点不足以给长期闪烁频谱。更昂贵的全分辨率High候选在1080p约24.44ms整帧，已拒绝作为默认High。

最终新进程启动收敛对照，使用 **GI sequence index** 7/31/119（其world.frame为9/33/121），三档均在sequence7实际8 rays，sequence31已回2/2/4。低档 d1/d2 为98.87%/97.74%、98.50%/96.99%、97.38%/92.14%；中档为99.07%/98.91%、98.63%/97.64%、97.51%/92.61%；高档为99.09%/98.98%、98.41%/97.95%、97.72%/93.36%。这证明启动早期已有反弹，但**不支持越积累越接近无偏参考**：退出8-ray窗口后远ROI存在约5–6个百分点下降，稳态低预算仍有负偏差。三个检查点不能定位退出瞬间的跳变帧。详见 `analysis-convergence.json`。

运动实验每个相位是完整连续轨迹运行后的独立捕获；四个相位不是四个连续帧。σ3后的相机运动远ROI为参考92.71%–95.15%，物体连续移动的四相位约97.75%–101.04%。开灯早期、关灯、平移、旋转、相机切换、物体移出/恢复有事件快照和实际age，不能用截图命令结束时间代替渲染帧。

不能把σ3叫严格置信区间，亦不能把 `.75^16` 当最终空间滤波/TAA残留上界。高预算状态的触发/退出仅由已捕获帧证明，缺少每个中间帧的观察不进行插值冒充实测。

## 最终 GPU 成本

完整分pass均值/P95、整帧P50/P95/P99、交错Off、动态8-ray、显存及遥测见[性能表](SSGI-Transport-Performance.md)。以下为最终代码、同PBR画面、AA=None、120帧预热后各597个独立GPU样本，单位ms：

| 渲染尺寸 | Low整帧 mean/P95 | Medium | High |
| --- | --- | --- | --- |
| 1399×596 | 2.882 / 3.203 | 3.469 / 3.800 | 4.146 / 4.657 |
| 1920×1080 | 6.040 / 6.477 | 7.577 / 7.975 | 9.087 / 9.617 |

1080p的Trace均值为1.424/1.717/3.431；Temporal为.416/.419/.416；Spatial为1.173/2.413/2.307；Upsample为.602/.599/.575。GI分类timer和整帧增量不同，不能混加。独立预滤enabled/disabled两对A-trous差为 **.26717 / .26390ms**，仅适用于本场景1080p Low2；无新增radiance RT，只将默认1×1法线有效payload从4→8bytes。

持续物体运动fixture+TAA的三档实际最终日志均8 rays，1080p整帧均值4.277/5.275/5.312，对应同fixture Off约1.738。该小场景追踪比PBR便宜，**不能据此预测复杂PBR持续动态的成本**。预热后进程local显存PBR1080p约1577.88/1591.88/1591.94MiB；这是整进程观测，非本轮新增buffer。池预留、Off子集、缺失观测、温度/频率波动均在详细表中单列。

## 验证状态与可复现入口

| 证据类型 | 精确入口 | 状态 |
| --- | --- | --- |
| build-only | `./hectl.bat game build` | PASS，最终HGame 10.0s |
| build-only | `./hectl.bat editor build` | PASS，最终Editor 27.5s |
| runtime-manual | `& out/agent/ssgi-audit-20260909/run-accepted-captures.ps1` | PASS：真实Editor9个线性阶段/指南，DX12/Vulkan×Deferred/Forward共10个fixture捕获 |
| runtime-manual | `& out/agent/ssgi-audit-20260909/run-accepted-quality.ps1` | PASS：实际Editor配置PNG、三档各8个Composite+TAA观察点 |
| runtime-manual | `& out/agent/ssgi-audit-20260909/run-convergence.ps1` | PASS：三档各3个启动检查点；负偏差仍存在 |
| runtime-manual，GPU计时 | `& out/agent/ssgi-audit-20260909/run-accepted-performance.ps1` | PASS：两尺寸静态14组 |
| runtime-manual，GPU计时 | `& out/agent/ssgi-audit-20260909/run-dynamic-performance.ps1` | PASS：两尺寸持续运动10组 |
| runtime-manual，GPU计时 | `& out/agent/ssgi-audit-20260909/run-variance-cost.ps1` | PASS：4组单机制A/B，shader字节/hash恢复 |
| runtime-manual | `& out/agent/ssgi-audit-20260909/run-shared-rtgi-check.ps1` | FAIL：DX12旧有空启动帧问题，未到SVGF |
| runtime-manual | `& out/agent/ssgi-audit-20260909/run-shared-rtgi-live.ps1` | DX12 Ready后切换HardwareRTGI+SVGF捕获PASS；Vulkan有效GI禁用，脚本校验非零退出，未覆盖其硬件RT |
| unit-test | `./hectl.bat test RenderSettings` | PASS；预设/设置合约 |
| unit-test | `./hectl.bat test TextureReadbackPng` | PASS；native/PNG/双通道readback合约 |
| CPU数值检查 | `python Tools/Agent/Analyze-SSGITransport.py --self-test --output out/agent/ssgi-audit-20260909/verified-numeric-selftest.json` | PASS；含无损gzip PFM读取 |
| codegen | 反射声明/字段未改；正常构建节点维护其GeneratedSource | 不需要额外codegen，不以旧生成目录作证据 |

实际 Python 为 `C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`。全部低层 hectl 命令保存在每组 `*-runs.json`，包括二进制/着色器/场景/材质哈希；脚本拒绝覆盖已有证据，因此重跑应修改输出目录和label。

最小新目录复现：

```powershell
$ssgiPython = 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
& $ssgiPython Tools/Agent/Run-SSGITransport.py --output out/agent/ssgi-rerun-editor --host editor --scene pbr --label verify --rays 2 --views source,raw,temporal,filtered,remod,composite
& $ssgiPython Tools/Agent/Run-SSGITransport.py --output out/agent/ssgi-rerun-fixture --host game --scene fixture --label verify --rays 2 --views source,raw,temporal,filtered,remod,composite --match-world out/agent/ssgi-audit-20260909/constant/baseline-editor-fixture-source.pfm.world.json
```

这些命令使用受控副本并有界回收，不会重启或关闭用户现有Editor。正式计时必须单独使用 `--performance`，不与readback、其他渲染进程或重CPU分析并行。

## 交付索引

- `delivery/comparison-preview.jpg`：已检查的同曝光前后图；`pbr-1.png`至`pbr-4.png`为完整尺寸显示。
- `accepted-editor/usage-low-editor-pbr-composite.png`：恢复实际非GI配置的Editor截图。
- `delivery/camera-checkpoints.gif`：明确标记的稀疏相机相位播放，不是连续视频。
- `analysis-accepted.json`、`accepted-backends/analysis-reference-update.json`：最终阶段、四条路径及更新参考。
- `analysis-accepted-quality-summary.json`、`accepted-quality-*/`：最终静态统计及24个HDR。
- `accepted-performance/`、`dynamic-performance/`、`variance-cost/`：最终28组正式计时，独立于所有诊断捕获。
- `accepted-convergence/`、`analysis-convergence.json`：三档启动检查点及实际ray/sequence。
- `analysis-shared-rtgi.json`、`shared-rtgi-ready-DX12/`：额外DX12硬件RT共享SVGF回归；`shared-rtgi/`保留启动失败，`shared-rtgi-ready-Vulkan/`保留能力拒绝。
- `bootstrap-low-taa/`、`motion/`、`occlusion-repeat/`、`response/`：灯光/物体、连续轨迹相位、薄遮挡同序列、光强曝光响应。
- `transport/`、`constant/`：初始HDR基线和受控积分输入。部分已分析序列压缩为 `.pfm.gz`，逐文件解压SHA256校验，manifest记录；分析器支持原逻辑PFM路径，不丢失原始浮点数据。

上述路径均相对证据根目录。完整原始HDR留在磁盘，不放进报告或工具输出。

## 未通过或未覆盖，不能隐藏的限制

1. **完整动态视觉验收未完成**：已有稀疏事件、四相位连续轨迹和噪声统计，但没有逐帧连续视频的拖影长度、闪烁频谱、所有显露边缘的量化验收。不能据此宣布用户所有目标达成。
2. 单层深度薄遮挡测试，16帧raw均值仅为含遮挡参考约61.66%/78.34%。已定位多条3D可通但屏幕表示未知的射线，仍未对全部残差完成方向分类。没有通过穿透所有前景来换亮度。
3. PBR面积积分受几何差分和边界掩码影响显著，未作为物理真值或缺失能量百分比。正常EV14下整体观感仍暗，需屏幕外数据的部分不属于此次纯SSGI修复。
4. Source/Raw视图中reset后若Temporal被裁剪超过16帧，无其他reset地回到Composite时，首次有效历史可能使用静态ray预算；不影响正常Composite连续运行，但诊断切换bootstrap尚有边界。
5. Source缓冲包含相机镜面。下一个最小候选是单独的纯漫反射源输出及严格A/B；不能现在声称其收益。屏幕后方可见性优先评估明确前/后表面区间；屏幕外太阳反弹再独立评估RSM或可见性探针，存储/职责估算见详细记录，尚无实测成本或实现。
6. 原生D3D12 debug layer本机不可用，运行使用已有NVRHI验证；DX12/Vulkan和两条raster路径的成功捕获不能代表所有材料、设备和驱动覆盖。未做shipping/package验证。

因此本轮交付是**已落地且有数值证据的修复与预设，附明确未完成的整体视觉验收项**，不是“SSGI问题全部解决”的宣告。
