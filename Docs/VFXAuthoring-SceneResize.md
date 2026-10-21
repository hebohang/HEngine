# 场景与窗口生命周期验收

`py -3.11 Tools/VFX/scene_resize_acceptance.py --backend DX12 --existing-build-sha256 <候选hash> --output out/agent/rendering-evidence/vfx-authoring/scene-resize-dx12-01`

Vulkan替换backend及新output。driver复用Session固定hash、Storage外层和finally停止owned进程，不构建。已有公开 `runtime.scene.load` 触发真正运行时场景事务，使用sceneGeneration、scene名称及pending结束确认提交；`window.resize` 触发真实Window.SetClientSize，确认client回执、frame boundary后的render尺寸及PNG IHDR像素尺寸。没有使用scene.open/scene.preview或render.view代替。

CPU environment-ember-low与GPU environment-ember同时播放：960×540缩小、1280×720恢复，切换游戏背景场景，1024×768改宽高比，切回neutral并恢复原尺寸。检查原句柄是否仍存活，但不假设切场景自动stop；记录后明确停止，退休32帧，SDK allocator拥有量恢复初始空值，再在新场景播放新实例。各阶段要求无allocator失败和新的实际native draw/dispatch记录，保存完整stats和截图。两类记录仍不是GPU粒子内容或fence-retired字节读回。

最终visualStatus固定NOT_REVIEWED。没有设备丢失/重建或性能证明。本次只做语法和纯PNG头校验回归，尚未运行引擎/GPU。
