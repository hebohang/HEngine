# SSAO、GTAO 与默认 SSR 调整

2026-09-08 至 2026-09-09。本次未提交、推送；保留已有场景打开修复及用户工作树改动。

## 实现与默认配置

`SceneAmbientOcclusionPass` 复用已有光栅深度/法线、RenderGraph、绑定缓存和资源退休机制。新增 normal-weighted screen-space SSAO：螺旋分布的屏幕空间深度采样、法线加权的局部遮蔽、世界空间半径衰减；它是真实执行的 AO 算法，不是将 GTAO 改名。两种算法都在半宽半高（原生 4K 时 1920×1080）计算，每帧更新，经过半分辨率双边降噪后输出原生 R8 可见度；0 表示遮蔽，1 表示无遮挡。共用 Forward/Deferred 现有间接光合成，不新增 RTScene 依赖。

GTAO 新增四点上采样：保留原来的深度、平面距离和法线拒绝，将原生像素的九点邻域改为四个包围它的半分辨率样本，并用双线性空间权重。保留九点参考路径。算法/开关通过现有设置事务在帧边界应用；参数通过每帧常量更新，128 字节 push constant ABI 不变。未引入时间抽样或依赖 TAA 的动态噪声。

项目 `BaseEngine.json` 的默认配置为 **GTAO、4 slices × 8 steps × 2 sides、FastUpsample=true、SSR=false**；Radius=1、Intensity=1、NormalBias=0.01 保持原值，AA 仍为 FXAA。没有减少 GTAO 采样数、改变主分辨率、关闭 AO 或修改材质。用户持久化覆盖和命令行覆盖仍按原设置优先级工作。引擎参考默认仍保留九点 GTAO，项目默认选择四点。

SSAO 16 samples 作为可选性能方案。在本场景的对照画面中，接触遮蔽明显弱于 GTAO，因此没有将它设为默认。3×6 GTAO 作为额外平衡配置保留，同样没有强制替换 4×8 默认。

| 属性 | 作用 | 范围 |
| --- | --- | --- |
| `Rendering.AO.Technique` | 切换 GTAO / SSAO | `GTAO`, `SSAO` |
| `Rendering.AO.Enabled` | 启用 AO | bool |
| `Rendering.AO.Radius` | 两种算法的世界空间影响半径 | 0.01–10 |
| `Rendering.AO.Intensity` | 可见度的幂次，影响遮蔽强度 | 0–4 |
| `Rendering.AO.NormalBias` | 避免自遮蔽的世界空间偏移 | 0–0.25；运行时还限制为半径的 1/4 |
| `Rendering.AO.SliceCount` | GTAO 剖面方向数 | 2–8 |
| `Rendering.AO.StepsPerSide` | GTAO 每个方向每侧采样数 | 2–16 |
| `Rendering.AO.SampleCount` | SSAO 深度采样数 | 8–64 |
| `Rendering.AO.FastUpsample` | 四点或九点保边缘上采样 | bool |
| `Rendering.Debug.PBR.Mode` | 12 显示屏幕空间 AO；原有 5 继续显示材质 AO | 0–12；诊断设置 |

可在 Rendering / AmbientOcclusion 设置分组中调整；以下平铺 JSON 可直接传给 `-RenderSettingsPath`：

- `Tools/Agent/AmbientOcclusion-GTAO-Reference.json`：4×8、九点滤波、SSR Off。
- `Tools/Agent/AmbientOcclusion-GTAO-Balanced.json`：3×6、四点滤波、SSR Off。
- `Tools/Agent/AmbientOcclusion-SSAO.json`：SSAO 16 samples、四点滤波、SSR Off。

## 画质证据

证据根目录为 `out/agent/ao-comparison`。`Test-AmbientOcclusion.ps1` 启动拥有者标记的暂停 HGame，在步进 90 帧后保持同一场景状态，对比完整画面与独立屏幕 AO 视图；再以 W 步进三个 20 帧区间，每个位置依次切换四种配置。对照过程中不推进模拟；设置切换不持久化，finally 始终停止拥有的会话。

人工检查了 `baseline-preview.jpg` 与 `comparison-preview.jpg` 两张受限预览。后者同时包含完整画面、接触区域 AO 和移动后的场景裁剪。四点 GTAO 没有观察到明显的台座轮廓、角色接触遮蔽变化；SSAO 的接触遮蔽明显减弱。完整原生截图留在 `live-dx12-final`；不能把缩略图检查写成逐像素画质认证。

`image-differences.json` 包含全图和场景 ROI 数据。全图 UI 自身动画会污染差分，因此画质判断采用原生像素矩形 `(1230,750)–(2490,1740)`，并公开范围，不能外推到整张图。四点 GTAO 相对九点参考，显示编码 RGB 0–255 的误差为：

| 位置 | MAE | RMSE | 差异超过 8 的像素比例 |
| --- | --- | --- | --- |
| 静态完整着色 | 0.0183 | 0.1494 | 0.00144% |
| 静态 AO 视图 | 0.0194 | 0.1693 | 0.00152% |
| 运动位置 1 | 0.0180 | 0.1527 | 0.00096% |
| 运动位置 2 | 0.0271 | 0.2424 | 0.00665% |
| 运动位置 3 | 0.0317 | 0.2741 | 0.01138% |

这三个位置是稀疏运动回归，不等价于长时间视频闪烁度量或所有场景画质保证。两种算法都有屏幕外、遮挡层、薄几何信息缺失的限制；SSAO 未与光追 AO ground truth 做参数拟合。

## 首轮性能定位

本机 RTX 3060 Laptop 6 GiB。HGame 原生 3840×2160、MainGame、固定模拟步长 0.0166667、1800 帧（600 预热、1200 记录）。首轮截图发现无输入长时间运行会停在升级选择菜单，遮住大部分场景，因此这组只用于同一菜单状态的 Pass 定位，不作为实际持续游玩性能或画质证据。

| 配置（除第一行外 SSR Off） | AO 主计算 ms | 降噪 ms | 上采样 ms | GPU 整图 ms |
| --- | --- | --- | --- | --- |
| 修改前，SSR On | 2.711 | 0.386 | 1.279 | 13.768 |
| GTAO 4×8 九点 | 2.531 | 0.400 | 1.321 | 11.644 |
| GTAO 4×8 四点 | 2.546 | 0.399 | 0.927 | 11.162 |
| GTAO 3×6 四点 | 1.514 | 0.387 | 0.902 | 9.986 |
| SSAO 16 四点 | 0.994 | 0.386 | 0.897 | 9.442 |

上采样约降低 30%；并非 AO 总耗时降低 30%。SSR Off 在此场景没有活动局部探针时，也省去了反射合成；有探针的场景不保证省去整个合成 Pass。不同轮次存在频率/CPU 波动，不能将整帧差值全部归因于某一个 Pass，也不能直接相加 CPU/GPU 时间。

## 复现与验证

### 菜单前的原生 4K 对照

最终性能命令：`Tools/Agent/Measure-AmbientOcclusion.ps1 -GraphicsApi DX12 -OutputDirectory out/agent/ao-comparison/performance-dx12`，证据类型 `build-only + runtime-manual`，五组均 PASS。具体每次 `hectl game capture` 的完整参数、退出码与日志路径在该目录 `commands.json`。

使用同一构建、MainGame、FXAA、GI Off、原生 3840×2160、2400 帧，其中 1200 预热、1200 测量；固定模拟步长 **0.001 秒**，使总模拟时间只有 2.4 秒，保持菜单前的场景。每帧照常渲染一次，不降主分辨率、不插帧。该设置是渲染诊断轨迹，不能用于正常游戏速度的 CPU 性能推断。画面目标尺寸、参数和可见三角形均已核验，五组平均可见三角形都是 39002.04；`verified-configurations.json` 保存证据。

接电状态沿用本次会话此前核验；Windows 平衡电源方案，厂商性能模式未知。`hardware-sample.csv` 是单点硬件读数，不能作为热稳定证明。GPU 利用率未用作效率结论。

| 配置 | CPU 平均/P95/P99 ms | GPU 整图平均 ms | 超过 16.667 ms 帧 |
| --- | --- | --- | --- |
| 旧设置：GTAO 4×8 九点、SSR On | 11.258 / 12.403 / 13.655 | 11.200 | 5/1200 |
| 参考：GTAO 4×8 九点、SSR Off | 10.794 / 11.883 / 13.275 | 10.739 | 2/1200 |
| **新默认：GTAO 4×8 四点、SSR Off** | **10.051 / 11.093 / 11.610** | **9.995** | **1/1200** |
| SSAO 16 四点、SSR Off | 8.686 / 9.646 / 12.373 | 8.489 | 5/1200 |
| 新默认重复 | 10.057 / 11.134 / 12.331 | 9.994 | 1/1200 |

新默认两次 GPU 整图基本一致，相对旧设置约减少 **10.8%**。参考组的 GTAO 上采样为 1.354 ms，新默认为 0.940 ms，约降低 **30.5%**；采样仍为 4×8×2。AO 主计算约 2.54 ms、半分辨率降噪约 0.39 ms，仍有进一步优化空间。此次仍出现超预算帧，不宣称已经通过稳定 60 FPS 的长期验收。性能数据和 `analyze.py` 保存在磁盘，CPU/GPU 重叠执行，不能相加。

本次保留了可回退的九点上采样。四点滤波不保证逐像素等同，在极细轮廓等未充分覆盖的场景可能产生不同结果；需要时关闭 `FastUpsample`。此前 Sponza/Bistro 全画质持续目标不因这次 MainGame 诊断而改判通过。

所有命令从仓库根目录运行。采集拒绝覆盖已有证据目录，重跑需换目录名。反射声明和资源持久化字段没有变化，没有手工生成代码改动。

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| unit-test | `hectl.bat test RenderSettings` | PASS；新 AO 参数、往返保存、算法切回、非法算法/采样数与现有设置契约，`settings-test-final.log` |
| build-only + runtime-manual | `Tools/Agent/Test-AmbientOcclusion.ps1 -GraphicsApi DX12 -OutputDirectory out/agent/ao-comparison/live-dx12-final` | PASS；26 个采集配置，含 Forward/Deferred 连续切换，`results.json`/`commands.json` 保存每步精确命令与截图 hash |
| build-only + runtime-manual | `Tools/Agent/Test-AmbientOcclusion.ps1 -GraphicsApi Vulkan -OutputDirectory out/agent/ao-comparison/live-vulkan` | PASS；30 个配置，额外包含两算法在 1919×1079 和 3840×2160 的 Resize；PNG 尺寸单独核验，`resize-dimensions.json` |
| build-only | `hectl.bat editor build` | PASS；`editor-build.log` |
| build-only + runtime-manual | `hectl.bat editor check -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Frames 120 -Resolution 3840x2160 -RenderValidation -TimeoutSeconds 240` | PASS；项目默认 GTAO，`editor-default-check.log`；最终源码再验证见 `final-editor-check.log` |
| build-only + runtime-manual | `hectl.bat game check -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Frames 120 -Resolution 3840x2160 -RenderValidation -TimeoutSeconds 240` | PASS；最终源码构建和原生 4K 运行，`final-game-check.log` |
| build-only + runtime-manual | `hectl.bat editor check -StartupScene Assets/Scenes/MainGame.he -GraphicsApi Vulkan -StrictGraphicsApi -Frames 120 -Resolution 3840x2160 -RenderSettingsPath Tools/Agent/AmbientOcclusion-SSAO.json -RenderValidation -TimeoutSeconds 240` | PASS；编辑器 SSAO，`editor-ssao-check.log` |

两后端同姿态场景 ROI 的 RGB MAE 为 0.00105–0.00162/255，见 `backend-differences.json`。该数值只比较所选 ROI，不替代全场景视觉检查。

首轮五组性能的精确入口为：

```powershell
# case 为 baseline / gtao-reference / gtao-fast / gtao-balanced / ssao；各自 JSON、日志和 CSV 已保存。
.\hectl.bat game capture -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Frames 1800 -PerformanceWarmupFrames 600 -Resolution 3840x2160 -RenderStats -RenderSettingsPath out/agent/ao-comparison/<case>.json -RenderStatsPath out/agent/ao-comparison/<case>.csv -CapturePath out/agent/ao-comparison/<case>.png -TimeoutSeconds 240
```

`Measure-AmbientOcclusion.ps1` 用显式 0.001 秒模拟步长保留菜单出现前的镜头，测五组原生 4K 渲染：修改前配置、SSR Off 九点参考、新项目默认、SSAO、重复新默认。它针对渲染性能，不可冒充正常游戏速度的 CPU 性能或热稳定验收。

中间失败保留：首个测试错误地使用函数名 `RenderSettingsContract`，测试运行器报告 UNKNOWN；已改用注册名 `RenderSettings` 并 PASS。第一次会话脚本使用 Windows PowerShell 时因环境模块路径找不到 `Get-FileHash`，改为继承调用脚本的 PowerShell 可执行文件；后一次正确读取诊断字段的独立 `debug` 集合。成功回归在 `live-dx12-final`，失败证据未覆盖；停止操作从未越过 ownerTag 检查。
