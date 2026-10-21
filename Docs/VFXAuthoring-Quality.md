# 初始质量档验收

当前源调用链是 Application::Run 中 OnPreUpdate（1526）→GameLayer::AdvanceStartup→Level::OnRuntimeStart→Python Start，之后才 BeginSimulationFrame（1549）。独立VFXQualityAcceptance初始场景只有一个验收脚本，它只在Start、加载/播放效果前调用一次SetQualityTier。不会在Update、活动host帧或已有实例时切档。该依据针对当前源；执行须固定包含该调用顺序的候选。

`py -3.11 Tools/VFX/quality_acceptance.py --backend DX12 --existing-build-sha256 <hash> --output out/agent/rendering-evidence/vfx-authoring/quality-dx12-01`

默认Low/Medium/High各一个独立owned会话，也可重复指定 `--tier`。GPU请求统一开启，然后初始Start选质量：Low应禁GPU并播放显式explosion-ember-low CPU回退；Medium/High播放CPU hit及GPU explosion。立即核对实际quality_tier，GPU容量是延迟的render采样，必须在8/20帧用Exact状态读回0/20224/50176，同时检查enable、实际句柄及allocator无失败。180帧自然结束、明确stop及32帧退休后验证allocator恢复空值。每档8/20/180帧截图、完整Python和Agent stats、固定exe及输入hash均保留。

工具不构建，Storage外层覆盖三档，finally停止会话并恢复环境。visualStatus为NOT_REVIEWED；容量/句柄证据不是实际粒子内容、GPU fence退休字节或性能证明。本次只完成源码时序核查及语法/纯数据断言，未运行引擎/GPU。
