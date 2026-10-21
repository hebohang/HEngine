# 独立缩小/放大滤波验收

`Tools/Content/sampling_filter_acceptance.py` 复用原有 fixture 几何生成、
`content_sync.synchronize` 原生导入/验证/事务发布和 sampling_acceptance 的固定
hash owned Session 捕获。它不会改原 fixture，也不涉及 RT。每次使用新 UUID namespace，
发布到独立 `Assets/Imported/<documentId>`；只有 stop 成功才删除运行捕获场景，
失败保留恢复场景和回执并标记 Storage unknown。

原生生成 256×256 灰阶四频段纹理（棋盘周期 1/2/4/8 texels）用于六种 minFilter。
另有同规则的 8×8 纹理用于两种 magFilter。全部 opaque、白色 base factor、相同
roughness、UV0=[0,1]、1×1 米面板，位于同一 z=0 平面。前两排依次是
9728、9729、9984、9985、9986、9987；第三排是 mag 9728、9729。

沿用捕获相机 (2.5,1.9,8)、60° 垂直视角、1280×720：每面板约 77.94 像素宽。
min 足迹为 256/77.94≈3.28 texels/pixel，λ≈1.72，使用 mip 的四模式应落入
1/2 级之间；不使用 mip 的 9728/9729 仍读取第 0 级。mag 为 8/77.94≈0.103，
λ≈−3.28。清单逐面板记录精确 bbox/足迹。这是几何预测，不伪称 shader LOD 读回。

只准备（不需要任何引擎二进制）：

```powershell
py -3.11 Tools/Content/sampling_filter_acceptance.py --prepare-only --output out/agent/rendering-evidence/vfx-authoring/sampling-filter-prepare-02
py -3.11 Tools/Content/test_sampling_filter_fixture.py
```

主代理固定候选后串行执行；运行模式需要 Pillow，启动前检查该依赖：

```powershell
$samplingImporter = (Resolve-Path 'out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe').Path
$samplingImporterHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $samplingImporter).Hash.ToLowerInvariant()
$samplingGameHash = (Get-FileHash -Algorithm SHA256 -LiteralPath 'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe').Hash.ToLowerInvariant()
py -3.11 Tools/Content/sampling_filter_acceptance.py --backend DX12 --importer $samplingImporter --importer-sha256 $samplingImporterHash --existing-build-sha256 $samplingGameHash --output out/agent/rendering-evidence/vfx-authoring/sampling-filter-dx12-01
py -3.11 Tools/Content/sampling_filter_acceptance.py --backend Vulkan --importer $samplingImporter --importer-sha256 $samplingImporterHash --existing-build-sha256 $samplingGameHash --output out/agent/rendering-evidence/vfx-authoring/sampling-filter-vulkan-01
```

输出 `source/manifest.json` 含实际输入 hash/identity/预测 ROI；`import` 保留转换、
验证日志和 state；`gpu` 保留真实捕获场景、截图和命令回执。主报告记录 8 个面板
内部 ROI 的 RGB mean/stddev、水平相邻像素平均绝对差，以及五组滤波模式对比 MAD。
ROI 排除 8 像素边缘，避免几何覆盖边界。不同面板可能有亚像素相位和光照差异，
这些指标不作精确 pixel-equality 或自动优劣判据。

复核要求：首先确认八个面板实际可见且 ROI 对齐；观察 mip 模式在细频段抑制混叠，
保留粗频段；比较 nearest/linear 的边缘平滑，以及 nearest-mip/trilinear 的跨级差异。
mag 两者应表现为清晰 texel 块/插值过渡。全黑、背景相同或全平坦图不能证明采样正确；
部分细频段自然在多个 mip 中均成为灰色，也不能据此认定滤波器失效。
显示像素受 sRGB、曝光、tone mapping、mip 生成影响，尚无与引擎完全匹配的离线金标准。
因此 `status=PASS` 只表示真实导入/回执检查完成，`visualStatus` 始终 NOT_REVIEWED。

本轮 `--prepare-only .../sampling-filter-prepare-01` 已成功；真实导入/GPU NOT_RUN。
