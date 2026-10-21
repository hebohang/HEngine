# 场景打开修复与默认场景性能诊断

2026-09-08。此次修复没有改变默认渲染画质，也没有提交或推送。

## 已实施

- 资源浏览器的 `.he` 卡片和文件名支持左键双击。双击、拖入视口及文件对话框统一排队，在下一次 `OnPreUpdate` 替换场景，避免在面板遍历中更换对象。
- 新加载场景以完成反序列化的快照作为干净基线。旧文件补齐反射默认值不会误报修改；原磁盘文档仍独立用于外部写入冲突检查，打开不会自动保存。
- `CameraFollowSystem` 不再在 Edit 的零时间预热中覆盖相机 Transform。跟随、碰撞检测及运行时相机运动继续在 Play 的运行场景执行；编辑视图保留作者保存的位置与旋转。
- 序列化直接读取组件引用，避免复制包含动画片段等运行缓存的组件。场景比较避免复制每个实体的 JSON；脏状态比较只在文档/保存基线改变后重新计算。仍保留每帧观察，未跳过未通知的真实编辑。
- 性能 CSV 增加 `cpuPreUpdateMs`；设置证据区分窗口大小、实际主场景目标大小与 GPU 计时范围。编辑器静止视口会缓存画面，窗口清屏/UI 计时不能当作完整场景 GPU 计时。

## 验证

证据根目录：`out/agent/scene-open-investigation/`。所有运行经拥有进程并负责退出/回收的 `hectl ... check`；无用户进程被停止。没有修改反射声明或持久化字段，未手改生成代码。

| 证据类型 | 精确入口 | 结果 |
| --- | --- | --- |
| unit-test | `hectl.bat test SceneEditing` | PASS；旧格式默认值、Resize、打开不写盘、编辑/撤销/重做、保存及外部文件冲突；`scene-editing-final-test.log` |
| build-only + runtime-manual | `hectl.bat game check -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Frames 120 -Resolution 3840x2160 -RenderValidation -TimeoutSeconds 240` | PASS；最终源码重新编译并完成原生 4K 验证层运行，exit=0；`final-game-validation.log` |
| build-only + runtime-manual | `Tools/Agent/Test-EditorSceneOpen.ps1 -GraphicsApi DX12 -OutputDirectory out/agent/scene-open-investigation/clean-dx12` | PASS；MainGame、SurfaceCoverage、Sponza、Bistro 各 120 帧始终干净、原文件 SHA-256 不变 |
| build-only + runtime-manual | `Tools/Agent/Test-EditorSceneOpen.ps1 -GraphicsApi Vulkan -OutputDirectory out/agent/scene-open-investigation/clean-vulkan` | PASS；同上，具体每场景命令及原始日志见子目录 `results.json` |
| runtime-manual | 下方 UI 命令，加 `sky.click(mouse_button='left', click_count=2)` 点击观测到的 MainGame 文件名 | PASS；实际由 SurfaceCoverage 切换到 MainGame，未出现 dirty=true；`double-click-result.json` |
| build-only + runtime-manual | `hectl.bat editor check -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Frames 900 -PerformanceWarmupFrames 300 -Resolution 3840x2160 -RenderStats -RenderStatsPath out/agent/scene-open-investigation/editor-4k-window-dx12.csv -TimeoutSeconds 240` | PASS；窗口和主离屏场景目标均为 3840×2160；静止缓存视图，整帧平均/P95/P99 6.203/6.885/7.673 ms，不能作为每帧完整 4K 场景刷新速度 |

UI 运行入口，进程最后由 `Alt+F4` 正常退出，拥有者收到 exit=0：

```powershell
$env:HE_EDITOR_CONTENT_DIRECTORY = 'Assets/Scenes'
.\hectl.bat editor check -StartupScene Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he -GraphicsApi DX12 -StrictGraphicsApi -Frames 300000 -Resolution 1280x720 -TimeoutSeconds 180
```

实际执行使用 `try/finally` 恢复环境变量。启动命令日志为 `double-click-ui.log`，双击后截图为 `double-click-main-game.png`。双击完成由运行日志确认；截图作为保存的界面记录，不作为渲染画质或性能验收。

中间失败没有覆盖：`dirty-diagnostics.log` 中的诊断日志宏编译失败、`final-editor-dx12.log` 中的短暂可执行文件链接失败，均已修复或重试并完成后续成功构建；原运行发现的相机位姿漂移保存在 `dirty-diagnostics-fixed.log`。初始批处理输入使用了不适用的嵌套配置格式，启动拒绝，修正为平铺属性后成功运行；见 `game-dx12-batching.log` 和 `game-dx12-batching-retry.log`。

## 默认场景与 1080p 诊断

`Assets/Scenes/MainGame.he` 有 432 个实体、352 个网格组件、61 个不同网格资源。这些资源的 primitive 没有 LOD 层级，且默认 `Rendering.LOD.Enabled=false`，只打开开关不会生成简化网格。

保持默认 DX12/Deferred、FXAA、GI Off、GTAO、SSR、IBL/阴影及后处理。GTAO 为半分辨率，4 slices、每侧 8 steps；SSR 半分辨率、64 steps/5 refinement；Bloom 四分之一分辨率。没有动态分辨率、升采样、插帧或 VRS。

编辑器窗口 1920×1080、静止缓存视口、300 帧预热后 600 帧短测：加计时但尚未优化的整帧平均 7.571 ms，其中预更新 6.445 ms；最终修复后 6.268 ms / 5.126 ms，平均整帧降低约 17%。最终 P95/P99 为 7.518/9.552 ms。此处 GPU 只覆盖呈现图，不能推算完整 3D 场景效率。

HGame 同为 1920×1080，600 帧预热后 600 帧，输入为空、固定步长 0.0166667，结果如下。表中 FPS 仅是平均帧时的倒数，不代表稳定性。

| 配置 | CPU 平均/P95/P99 ms | GPU 平均 ms | 超 16.667 ms 帧 |
| --- | --- | --- | --- |
| DX12 默认 | 11.524 / 17.929 / 47.482 | 3.665 | 36/600 |
| Vulkan 默认 | 9.402 / 13.529 / 26.717 | 3.808 | 22/600 |
| DX12 批处理候选 | 10.460 / 16.072 / 42.762 | 3.533 | 26/600 |

默认 DX12 的 CPU 渲染阶段约 9.47 ms，其中约 9.19 ms 未归入底层提交/Present/帧槽等待，包含上层准备、命令录制和 UI；不能进一步全部归因于某个函数。两 API 可见三角形统计一致，平均约 3.9 万。此分辨率首先是 CPU 问题，不是几何量超过 GPU 能力。

## 原生 4K HGame

RTX 3060 Laptop GPU 6 GiB、i7-11800H；接电，Windows Balanced，厂商性能模式未知。没有建立热稳定证明。以下是定位问题的短测，不替代此前 Sponza/Bistro 全画质持续基准的验收。

场景仍为 MainGame，实际主场景和呈现目标均核验为 **3840×2160**，不是编辑器内的小视口。1200 帧预热，测量帧 1201–2400，1200 个 CPU 样本；GPU 样本去重并排除预热延迟返回的样本。三组可见几何和两后端有效画质设置已对照。默认两 API 只差后端；候选仅开启普通与阴影批处理。

| 配置 | CPU 平均/P95/P99 ms | 平均 FPS | GPU 平均 ms | 超 16.667 ms 帧 |
| --- | --- | --- | --- | --- |
| DX12 默认 | 12.658 / 13.568 / 14.062 | 79.0 | 12.606 | 0/1200 |
| Vulkan 默认 | 13.900 / 14.837 / 15.302 | 71.9 | 13.732 | 2/1200 |
| DX12 批处理候选 | 13.110 / 14.162 / 14.577 | 76.3 | 13.059 | 1/1200 |

这里 DX12 更快，与小窗口下的 CPU 瓶颈结论不同。DX12 的 GPU 主要 Pass：GTAO Horizon 2.598 ms、GTAO Upsample 1.210 ms、Deferred Lighting 1.937 ms、Runtime UI 1.703 ms、GBuffer 1.255 ms。4K 下是像素/全屏效果成本更突出，LOD 不会直接减少这些成本。

批处理候选在 1080p 有收益，但本次 4K 没有整帧收益，因此没有修改全局默认。没有为了提高数字关闭 AO/SSR、减少采样或降低主分辨率。CPU 与 GPU 是重叠执行的，表中的两者不能相加。

精确命令（证据类型 `build-only + runtime-manual`，三条均 PASS 表示成功构建、运行和采集，不表示热稳定目标验收）：

```powershell
.\hectl.bat game check -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Frames 2400 -PerformanceWarmupFrames 1200 -Resolution 3840x2160 -RenderStats -RenderStatsPath out/agent/scene-open-investigation/game-4k-dx12.csv -TimeoutSeconds 300
.\hectl.bat game check -StartupScene Assets/Scenes/MainGame.he -GraphicsApi Vulkan -StrictGraphicsApi -Frames 2400 -PerformanceWarmupFrames 1200 -Resolution 3840x2160 -RenderStats -RenderStatsPath out/agent/scene-open-investigation/game-4k-vulkan.csv -TimeoutSeconds 300
.\hectl.bat game check -StartupScene Assets/Scenes/MainGame.he -GraphicsApi DX12 -StrictGraphicsApi -Frames 2400 -PerformanceWarmupFrames 1200 -Resolution 3840x2160 -RenderStats -RenderSettingsPath out/agent/scene-open-investigation/batching-candidate.json -RenderStatsPath out/agent/scene-open-investigation/game-4k-dx12-batching.csv -TimeoutSeconds 300
```

重跑使用新输出文件名，采集器拒绝覆盖旧 CSV。候选平铺 JSON 内容为 `{"Rendering.GpuBatching.Enabled":true,"Rendering.GpuBatching.ShadowsEnabled":true}`。每个 CSV 旁有 `.passes.csv`、`.memory.csv`、`.settings.json`；`performance-summary.json` 保存帧时分位数、计时范围、实际尺寸和设置，`analyze.py` 可重新生成结果。

原生 4K 的本次 MainGame 默认配置观察到的结果，不意味着此前开启 SSGI/SVGF 的复杂场景已达到稳定 60 FPS。后者仍以 `Rendering-CurrentValidation.md` 的未达标记录为准。
