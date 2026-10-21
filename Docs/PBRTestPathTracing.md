# PBRTest 原场景路径追踪

2026-09-09/10，Windows DX12。此记录覆盖原始 PBRTest 的支持与成像验证；完整交互验收的未完成项仍见 [总审计](OfflinePathTracingAudit.md)。

## 场景保持不变

`Project/Assets/Scenes/PBRTest.he` 修改前后 SHA-256：

`18b24149055a5497f7d661327241095329424cfeb86f65343d58c4f867134b72`

场景使用 Sponza，25 个 section，其中 22 个 opaque、3 个双面 alpha mask。
旧分类器按第一个不支持条件返回：22 个先遇到平滑法线，另 3 个先遇到贴图。
这不是场景损坏或必须离线烘焙；运行时未接入这些输入导致全部被排除，无法构建可追踪 TLAS。

保留文件中的相机、材质与灯光：相机位置 (0, 2.5, 11)，60° 透视，方向光强度 100000；
点光强度为 0，环境实体在编辑器中隐藏。因此原场景没有环境填充，走廊暗部主要靠间接光。
没有为截图打开隐藏的环境、提高曝光或替换材质。显示采用原来的 EV14 曝光。

无 Editor 的测试宿主会省略 EditorEntityStateComponent，因此测试先在临时 JSON 文档中应用其可见性，再提取世界；
原文件不写回。真实 Editor 验证直接打开原文件，无此测试适配。

## 根因与修改

| 文件 / 函数 | 源码确认的问题 | 实现与验证 |
|---|---|---|
| HardwareRayTracing / ClassifyRayTracingGeometry | 默认平滑、贴图、mask 全部排除 | provider 明确申请 BasePBR 输入；此策略接纳 25/25，原实时 GI 默认分类不变 |
| RayTracingScene / BuildCpuScene、ResolveMaterial | 顶点仅位置/法线，材质仅常量 | 上传 UV、切线、副切线和材质纹理索引；共享 TextureManager 缓存、颜色空间及资源租约 |
| Reference closest/any hit | 缺少纹理与 alpha 语义 | 插值 UV、逆转置平滑法线、TBN 法线贴图；mask 在 any-hit 中 IgnoreHit，阴影也执行裁剪 |
| Reference 光照循环 | 场景方向光未参与路径积分 | 方向/点/聚光直接估计，GGX + diffuse Evaluate/Sample/Pdf；独立遮挡查询跳过 closest-hit |
| SceneRenderer / BuildRayTracingSceneRevisions | 新获准进入 PT 的对象仍可能被旧分类漏出版本检测 | 同一策略用于场景准备与版本；纹理、法线强度、alpha cutoff、spot 参数纳入更新 |
| Reference ray generation | 本机首次与重启的 sample 0 存在差异 | 主射线 `precise` 抑制浮点运算重排；固定 seed 的 1/4/16 spp 和首样本 guides 逐值复现 |

最后一项有本机实测：相机常量逐字节相同、BLAS/TLAS 未重建，只有首次 sample 0 不同，差异按 1/spp 缩小。
主射线 `precise` 修改后 Raw/albedo/normal 一致；快速数学运算是定位结论，未声称抓取了驱动内部指令轨迹。
测试同时验证去噪开关和阶段性修改目标样本不改变 Raw 序列。

## 数据与生命周期

宿主发布 RenderWorld → RayTracingScene 按 revision 准备共享场景表与 BLAS/TLAS → 插件绑定材质纹理和灯光表 →
有界行批次追踪 → GPU 完成后推进计数 → 原始 AOV 快照 → OIDN CPU 工作线程 → 独立 denoised 纹理上传 → 显示转换。

继续使用单 Graphics queue、一批在途工作和 generation 隔离。OIDN 输入为独立线性快照，不回写 radiance accumulation。
材质纹理通过 TextureManager 加载并持有租约，旧 binding set 和场景资源以实际 submission 完成值退休。
SceneRenderer 中 TextureManager 的生命周期覆盖活动和退休的 RayTracingScene。
后台 OIDN 不访问 ECS、ImGui 或可变材质对象。相机不改变 BLAS，材质/灯光修改也不重建 BLAS。

共享顶点 stride 从 32 增至 80 字节，材质记录从 32 增至 80 字节；实时 RT 与 SceneSurfaceData HLSL 同步更新 ABI。
AnalyticLights 是带计数头的 257 × 64 字节缓冲。材质纹理经共享服务缓存，PT descriptor array 最多 512 个纹理。
纹理按场景寿命保留；大量连续替换贴图后的容量/内存降级仍需深化，不宣称无限容量。

Interactive / Reference 复用这些场景表与积分器；Reference 固定 seed、尺寸和反弹数。
已有 v2 设置继续有效，旧 SamplesPerFrame=46 不能绕过预算；没有覆盖用户场景或全部设置。

## 成像证据

`out/agent/captures/pt-pbrtest/` 保存 1/4/16/64/256 spp 的 Raw 与 OIDN PFM、实际 GPU sample-count、albedo/normal、
材质清单、时序 CSV 和 `summary.json`。Raw 是插件 RGBA32 累积导出的线性 float32 RGB，未经过 tone mapping 或去噪。
`pbrtest-stage-preview.jpg` 仅用于浏览，不是数值参考。

测试断言包括：25 included / 0 excluded、在途不超过 1、每像素 GPU count 等于完成 spp、全图有限值、
方向光减半后图像严格减半、首次与重启一致、去噪不污染 Raw、目标样本改变不重排随机序列；
2×2 已知纹理验证 UV/sRGB albedo/emission、切线法线和 alpha 孔洞；金属反射、粗糙度展宽、点光和聚光方向更新；
材质与灯光更新保持 BLAS build count 不变。测试纹理写入 Saved，原有资源不改动。

256 spp 是共享低样本序列的有限参考，不是真值。低样本 OIDN RMSE 更低不意味着高样本去噪也无偏；
此场景 64 spp 去噪与 256 Raw 的 RMSE 可以高于 64 Raw，不隐藏这种情况，也不以色调映射截图证明物理正确。
未做与 Blender 单位、相机、材质严格匹配的独立高样本比较。

本次独占 GPU 的 Editor 1200 帧采集，窗口 1280×720，CSV 跳过前 30 帧：

| 指标（ms） | P50 | P95 | P99 |
|---|---:|---:|---:|
| Editor CPU wall frame | 1.191 | 2.803 | 4.444 |
| GPU frame span（去重延迟样本） | 0.474 | 2.912 | 4.237 |
| frame-slot wait | 0.0005 | 1.637 | 3.140 |
| Present 调用 | 0.110 | 0.215 | 0.430 |

原场景 640×360、8 bounces 的独立 Reference 测试，1→256 spp 含去噪等待、不含导出间隔，共 6.633 秒，
完成吞吐 38.59 spp/s，在途峰值 1。此数不能与 Editor frame time 换算或混为同一指标。
Editor trace pass P95/P99 为 2.340/2.710 ms，中位数为 0（等待已有批次完成的 UI 帧不新增采样）。
PT 图像发布率、相机输入延迟和首次准备耗时尚无完整统计，不能用上述低 frame time 宣称全部交互目标达标。
执行图没有 DirectionalShadow、DeferredLighting、GTAO 或 SSR 叠加到 PT beauty；保留深度、编辑器叠加与显示处理。

## 验证命令

机器：i7-11800H / RTX 3060 Laptop（标称 6 GiB，DXGI 本次报告 5994 MiB）、驱动 591.74、VS2026 v145。
DX12 NVRHI validation 启用；Windows Graphics Tools 缺失，原生 D3D12 debug layer 未启用。
所有运行由 Build.ps1/hectl 管理、限时并回收进程；使用独立 `out/agent` 构建，未更新 VS 当前产物。

```powershell
# test-layer: PASS，原场景 GPU 成像及逐像素断言
$env:HE_PBRTEST_PT_SMOKE='1'
try { powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Run -TimeoutSeconds 600 }
finally { Remove-Item Env:HE_PBRTEST_PT_SMOKE }

# unit-test: PASS
.\hectl.bat test HardwareRayTracing
.\hectl.bat test OfflineReferenceRendererSession

# test-layer: PASS，已有渐进、取消/恢复、代次、空场景安全和 OIDN 回归
$env:HE_OFFLINE_PROGRESSIVE_SMOKE='1'
try { powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Run -TimeoutSeconds 360 }
finally { Remove-Item Env:HE_OFFLINE_PROGRESSIVE_SMOKE }

# test-layer: PASS，共享 RT 场景 ABI、实时 GI 与共享 AOV
$env:HE_SHARED_RT_SCENE_SMOKE='1'
try { powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Run -TimeoutSeconds 360 }
finally { Remove-Item Env:HE_SHARED_RT_SCENE_SMOKE }

# runtime-manual: PASS，直接加载原文件，受控启动/采集/退出
.\hectl.bat editor capture -Frames 240 -Resolution 1280x720 -StartupScene Assets/Scenes/PBRTest.he -StartupViewportMode he.viewport.path-tracing -RenderStats -RenderStatsPath C:\WorkSpace\HEngineDev\out\agent\pt-pbr-editor.csv -CapturePath out/agent/pt-pbr-editor.pfm -TimeoutSeconds 240
.\hectl.bat game check -Frames 120 -StartupScene Assets/Scenes/PBRTest.he -TimeoutSeconds 180

# runtime-manual: PASS，独占采集，作为上表数据来源
.\hectl.bat editor capture -Frames 1200 -Resolution 1280x720 -StartupScene Assets/Scenes/PBRTest.he -StartupViewportMode he.viewport.path-tracing -RenderStats -RenderStatsPath C:\WorkSpace\HEngineDev\out\agent\pt-pbr-editor-exclusive.csv -CapturePath out/agent/pt-pbr-editor-exclusive.pfm -TimeoutSeconds 240

# 数值汇总，标准 Python；编辑器 CSV 必须来自单独的受控运行
python Tools/Rendering/summarize_pbrtest_pt.py out/agent/captures/pt-pbrtest --editor-csv out/agent/pt-pbr-editor-exclusive.csv
# 可选 --preview 生成预览图，要求 Pillow
```

日志前缀 `out/agent/pt-pbr-`，PBR 主测试为 `pt-pbrtest-pbr-smoke.log`。
综合 DX12 smoke 仍记录 FAIL：在到达 RT 段之前，BindlessTextureCount 的旧断言预期 6、实际 4。
没有删除或放宽该断言；专门的共享 RT 路由与旧 Cornell 全帧/九 AOV 契约分开验证。
旧硬件 smoke 的 submitted 即完成断言已改为实际完成后 Collect，再核对 completed spp。
共享路由通过了 64×64 RTGI/SVGF、1080p Forward 与 1440p 默认 Deferred 集成、对象/相机历史、
场景表 AOV 读回。旧 Forward 夹具显式选择 Forward，并接受其现用 RasterSurfaceGuides pass；没有放宽成像断言。
旧综合 Reference 段仍假设固定帧数等于 SPP，专用渐进和 PBR 路由已验证新完成语义；不把旧综合段报告为 PASS。
综合失败的精确命令为同时设置 `HE_HARDWARE_RT_SMOKE=1`、`HE_SURFACE_DATA_AOV_SMOKE=1`，
执行相同 `Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Run -TimeoutSeconds 360`。

## 支持边界

- 已接入：静态 BasePBR opaque/mask、平滑法线、双面、base color、normal（RGB/BC5）、packed MR、
  独立 metallic/roughness 输入、emission、GGX 镜面和漫反射、方向/点/聚光、发光三角形与环境照明。
  其中 PBRTest 本身不含有效点/聚光/发光，灯光与基础纹理语义由独立成像夹具验证；
  BC5 与独立 MR 路径未各自做专门数值夹具，不能把总体 PASS 扩大成每种组合都已验证。
- alpha mask 是裁剪；alpha blend、玻璃透射/IOR、自定义材质、蒙皮、体积和毛发未接入，不把混合透明当折射。
- 纹理使用 mip 0 + 双线性 wrap；没有 ray differential/自适应 mip。AO 贴图不乘入路径积分，避免再次遮蔽真实 GI。
- 环境仍为均匀半球直接采样，缺少亮度分布重要性采样及完整旋转能力；AOV Direct/Indirect 仍是路径深度分组，包含镜面。
- OIDN 2.5.1 CPU 后端实际可用。未实现 DX12/Vulkan GPU OIDN 互操作。
- shader 产物构建包含 DXIL/SPIR-V；Vulkan PT 运行未验证。
- 首次 shader/纹理/AS 准备仍会产生停顿，未完成原任务要求的全程导航输入延迟、120/60 FPS、长期内存与多场景验收。
  原版 PBRTest 无有效 PT 图像，不能给出匹配的前后 SPP/s 提升倍数。

参考：[PBRT 交互与法线](https://www.pbr-book.org/4ed/Geometry_and_Transformations/Interactions)、
[DXR 规范](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)、
[OIDN RT 输入语义](https://www.openimagedenoise.org/documentation.html)。
