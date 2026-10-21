# NVRHI upstream update — 2026-09-29

用户要求先检查 GitHub 新版并尝试，若仍有问题则暂缓 Vulkan，优先保证 DX12。
此次明确授权 ThirdParty 更新；以前等待授权的记录已失效。

## 版本与结论

- 官方仓库：<https://github.com/NVIDIA-RTX/NVRHI>。
- 原版本：`8e8c36e37558acec333204619b95d9d2fcdc4a79`。
- 查询时 main/HEAD：`d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076`，提交日期 2026-09-23。
- 完整应用上游 29 个文件的增量；保留 D3D11 比较采样器以及 Vulkan 未提交命令列表生命周期补丁。
  更新后与上游不同的源文件仍只有 `d3d11-device.cpp`、`vulkan-commandlist.cpp`、`vulkan-queue.cpp`。
  MIT 许可证保留。来源记录见 `Engine/Source/ThirdParty/NVRHI/README.HEngine.md`。
- 新版通过 Game 编译和双后端 1440p/2160p 场景测试，予以保留；DX12 快速角色运动检查通过。
- **新版未修复已知的 Vulkan 纹理分配失败清理问题。** `Device::createTexture` 的函数体、整个
  `vulkan-allocator.cpp` 均与旧版相同。创建 image 后分配内存失败仍可能提前返回，未释放原始 Texture。
  这次正常切换没有触发分配失败，不能声称 OOM 恢复通过。遵照用户要求，暂缓此 Vulkan 缺陷，
  不应用另备的本地清理补丁；DX12 为本阶段优先使用和验收路径。

## 实际验证

以下命令从 `C:/WorkSpace/HEngineDev` 执行。

**static-check PASS**：

```powershell
git ls-remote https://github.com/NVIDIA-RTX/NVRHI.git HEAD refs/heads/main
git -C out/agent/fsr-integration/nvrhi-upstream-review diff --stat 8e8c36e37558acec333204619b95d9d2fcdc4a79 HEAD
git -c core.safecrlf=false diff --check -- Engine/Source/ThirdParty/NVRHI Docs/NvrhiUpstreamValidation.md Docs/FSRStreamingCombatWork.md Docs/RenderResolution.md
```

完整上游增量和源文件哈希审计保存于 `out/agent/fsr-integration/`：
`nvrhi-upstream-8e8c36e-to-d0c8e30.patch`、`nvrhi-upstream-audit-20260929.json`。

**build-only PASS**（172.3 秒）：

```powershell
powershell -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab
```

Game SHA256：`B79B2F53BDB6097AD8F904E424846359A754944050CD7B9D2310593ADB778E8B`。
日志：`out/agent/projects/7e44ffe2e91e72bd/logs/build-game-relwithdebinfo-HGame.log`。

**runtime-manual PASS**，三个独占会话均已停止：

```powershell
$hash=(Get-FileHash out/agent/projects/7e44ffe2e91e72bd/game-relwithdebinfo/bin/Gamee4fb9eeb.exe).Hash
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrHud.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi Vulkan -AllowNative4KRejection -OutputDirectory out/agent/captures/fsr-hud-Vulkan-nvrhi-latest-20260929
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrHud.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi DX12 -OutputDirectory out/agent/captures/fsr-hud-DX12-nvrhi-latest-20260929
powershell -ExecutionPolicy Bypass -File Tools/Agent/Test-FsrMotion.ps1 -Project C:/WorkSpace/HEngineProjects/MechanicGirlLab -ExistingBuildSha256 $hash -GraphicsApi DX12 -OutputDirectory out/agent/captures/fsr-motion-DX12-nvrhi-latest-20260929
```

运行数据分析 **PASS**（不是 headless unit-test）：

```powershell
& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/Agent/analyze_fsr_hud.py out/agent/captures/fsr-hud-DX12-nvrhi-latest-20260929 out/agent/captures/fsr-hud-Vulkan-nvrhi-latest-20260929
& C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe Tools/VFX/analyze_fsr_motion.py out/agent/captures/fsr-motion-DX12-nvrhi-latest-20260929
```

- 两端真实 FSR provider 3.1.4；Performance 内部分辨率 1280×720 → 2560×1440、
  1920×1080 → 3840×2160；独立 TAA 关闭。关闭 FSR 后真正恢复原生 1440p/2160p。
- FSR → HDR 后处理 → 色调映射 → 原生 UI → Present 顺序通过。
  DX12/Vulkan 的 HUD 不透明内部像素分别 20,439 / 20,441 个，差异为零；布局一致。
  背景相关的抗锯齿边缘不包含在完全一致声明中。
- DX12 角色运动：16 帧连续采样，五段回放的骨骼/布料起止哈希匹配。
  身体、头发、布料回投误差 P95 分别 1.153 / 1.339 / 0.967 px，暂停残差最大 0.000407 px。
  与 NativeAA 比较的新露出背景每帧平均 RGB 差异最大约 0.00650。
  保留原检查的遮挡、亚像素对应和尾部误差限制，不能解释为普遍无拖影。
- 人工顺序检查 DX12 1440p/2160p、Vulkan 2160p 缩略图，以及 DX12 第 0/5/10/15 帧
  Quality/NativeAA 对比：场景、材质和 HUD 正常，未见明显大面积拖尾、模型爆炸或缺失。
  缩略图不证明所有原始像素质量或所有战斗镜头表现。

本次没有改动反射声明，无需 codegen 验证。未运行 Editor、Shipping、独立单元测试、
新版长时间流送压力测试或驱动 OOM 测试；此前对应结果及限制保留在
`FSRStreamingCombatWork.md`，不冒充新版的重新测试。未提交或推送 Git。
