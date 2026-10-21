# 在编辑器中查看本次渲染场景

2026-09-08：`Assets/Scenes` 目录浏览闪退已修复。`Physics2DTest.he` 缩略图的 `Primitive2D` Pass 错误使用了读取 GTAO 的光照帧绑定，违反 RenderGraph 资源声明。现在无光照的 2D Pass 使用普通帧绑定；保留 GTAO、场景内容和缩略图功能。

另修复导入相机初始化：编辑器的无滚转轨道相机现在根据原相机的前向方向求俯仰与偏航，避免直接丢弃等价欧拉角表达中的滚转，使 Sponza 打开后的位置和观察方向保持正确。编辑器仍不保留艺术相机的独立滚转。

## 打开方式

在 Project Manager 中导入 `Examples/RenderingTests/Project.heproject`。编辑模式下可在资源浏览器中**鼠标左键双击 `.he` 场景卡片（含文件名区域）**打开，也保留拖入视口及 **Ctrl+O**（文件 → 打开）。双击、拖放和文件对话框均在下一帧的编辑边界切换场景。

打开后的干净状态以反序列化完成的场景快照为基准；补齐旧文件默认值和视口宽高比变化不会被误报为修改。跟随相机只更新 Play 的运行场景，不再在编辑器首次预热时重写保存的相机位姿。实际编辑、撤销/重做和外部文件冲突检查仍生效，打开不会自动保存场景。

| 用途 | 场景文件（相对于工程根目录） |
| --- | --- |
| 材质、蒙皮、裁剪和透明覆盖 | `Examples/RenderingTests/Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he` |
| 局部灯光 | `Examples/RenderingTests/Assets/RenderingBenchmarks/Validation/LocalLightsOnly.he` |
| 本次 Intel Sponza 基准 | `Examples/RenderingTests/Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he` |
| 本次 Bistro 基准，含恢复的环境照明 | `Examples/RenderingTests/Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BCGPUMaskFixedEnvironment/Benchmark.he` |

路径前缀为 `C:\WorkSpace\HEngineDev\`。`Examples/RenderingTests/Assets/Scenes/Sponza.he` 是原有旧场景，不是这次官方新版基准。

也可以在工程根目录运行以下用户启动入口；它会先增量构建，再保持编辑器打开：

```powershell
.\Win-RunEditor.bat -Project Examples/RenderingTests -StartupScene Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he
```

不需要点运行按钮就能查看编辑模式中的渲染画面。轨迹基准使用固定步长脚本；编辑器中的自由浏览不作为可重复性能轨迹证据。

## 切换 Forward / Deferred

打开 **设置 → 项目设置**，搜索 `Rendering.Path`，选择 `Forward` 或 `Deferred`，点击 **仅在本次会话试用**，检查 Effective 值。默认是 Deferred。图形后端是独立设置。

2026-09-09：当前项目默认 GTAO 4×8、四点上采样，SSR 关闭。搜索 `Rendering.AO.Technique` 可切换 `GTAO` / `SSAO`；`Rendering.AO.FastUpsample` 可恢复九点上采样，`Rendering.Reflections.SSR.Enabled` 可重新开启 SSR。SSAO 接触遮蔽与 GTAO 有明显差异，因此仍以 GTAO 为默认。参数、4K 对照和复现脚本见 [AmbientOcclusion-Validation.md](AmbientOcclusion-Validation.md)。

用于现场切换时，不要传 `-RenderPath` 启动参数：命令行配置优先级更高。下面的自动验证命令固定各路径，只用于重复验证启动结果。

## 复现检查

所有检查使用独立构建、独立 Saved 目录及自动回收的进程，不修改用户场景、布局或个人设置。`HE_EDITOR_CONTENT_DIRECTORY` 仅在有帧数上限时生效，自动显示对应目录，实际运行缩略图解析、加载、渲染和捕获链路。

```powershell
.\Tools\Agent\Test-EditorSceneBrowser.ps1 -GraphicsApi DX12
.\Tools\Agent\Test-EditorSceneBrowser.ps1 -GraphicsApi Vulkan
.\hectl.bat test EditorCameraImportedOrientation
.\hectl.bat test SceneEditing
.\Tools\Agent\Test-EditorSceneOpen.ps1 -GraphicsApi DX12
.\Tools\Agent\Test-EditorSceneOpen.ps1 -GraphicsApi Vulkan
.\hectl.bat game check -Frames 120 -TimeoutSeconds 300 -GraphicsApi DX12 -RenderValidation -StartupScene Assets/Scenes/Physics2DTest.he
.\hectl.bat editor capture -Frames 120 -TimeoutSeconds 300 -GraphicsApi DX12 -RenderValidation -StartupScene Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he -RenderPath Deferred -CapturePath C:/WorkSpace/HEngineDev/out/agent/rendering-evidence/editor-sponza-deferred.png
.\hectl.bat editor capture -Frames 120 -TimeoutSeconds 300 -GraphicsApi DX12 -RenderValidation -StartupScene Assets/RenderingBenchmarks/Trajectories/IntelSponza2048BCGPULOD/Benchmark.he -RenderPath Forward -CapturePath C:/WorkSpace/HEngineDev/out/agent/rendering-evidence/editor-sponza-forward.png
.\hectl.bat editor check -Frames 120 -TimeoutSeconds 300 -GraphicsApi DX12 -RenderValidation -StartupScene Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BCGPUMaskFixedEnvironment/Benchmark.he
```

证据结果见 `out/agent/rendering-evidence/editor-scene-browser-results.json`。这些是 `runtime-manual` 集成检查，不是单元测试，也不是原生 4K 性能验收。此次未修改反射字段；构建节点自动检查生成代码。

`EditorCameraImportedOrientation` 单独属于 `unit-test`：覆盖 Sponza 原始姿态、普通角度、带滚转输入和竖直观察方向，检查前向方向、视点及相机空间中心。

普通编辑器现在保留 `Project/Saved/Logs/Editor.log`；有界检查的日志位于其独立 `out/agent/sessions/<run>/Saved/Logs/Editor.log`。RenderGraph 未声明纹理错误现在包含 Pass 名和纹理索引。

性能和整体功能的未完成项目仍以 [Rendering-CurrentValidation.md](Rendering-CurrentValidation.md) 为准；原生 4K 稳定 60 FPS 仍为 FAIL。
