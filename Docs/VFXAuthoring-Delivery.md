# 特效工程收尾验收

## 验收边界调整

2026-09-14，用户明确要求“朝收尾推进，验收可以放宽”。据此停止围绕非阻断的进程总量变化持续归因，以最终改动影响的代表性双后端构建、运行、集成与视觉检查作为本次交付依据。

- 原 `Tools/VFX/acceptance-budget.json` 与历次原始报告保持不变。进程 private bytes、OS local/nonlocal memory 的严格零增长、峰值与趋势门槛改作本次交付的诊断项；旧 FAIL 不改写成 PASS。
- 崩溃、原生 GPU 错误、失效引用、实际功能失败、未确认的会话退出仍然阻断相关路径。逻辑资源计数稳定不能证明所有原生分配不存在泄漏。
- 历史完整矩阵仅证明各自记录的二进制和资源；最终版本采用受影响路径的代表性复验。缩减的压力规模、性能采样与未测范围逐项说明，不声称完成原始全矩阵。
- 开发探测开关与经过验证的生产支持是不同状态。当前生产 GPU 默认关闭，Shipping 和 Release 性能不能从开发运行结果推导。

## 原标准结果保留

Game23（`a5d0c0d02ac67d874277386878fb495cf62d9cef670e45c8951937d6f81f30c8`）DX12 20k 完成 30 轮、107 项：101 PASS、3 内存 FAIL、3 NOT_VERIFIED。三项失败分别为 OS local、OS nonlocal 与进程 private trend；完整 280,956 帧统计校验通过，原生日志无错误，资源、预算和构建身份未变。后三个批次组合未运行。

该次 settled 样本 private 峰值增加 2,207,744 字节；local 与 nonlocal 峰值各增加 1 MiB。阶跃并非同时发生，不能统一归因于一个分配者。另一个无特效播放、模拟序号保持为 1 的空场诊断，在约 308 秒同样观测到 private 增加 135,168 字节及两个新线程；这说明进程总量会受非特效执行影响，但不证明所有显存变化均与特效无关。

原始证据位于 `out/agent/rendering-evidence/vfx-authoring/game23-all-lifecycle-observations-01`，逐轮审计及空场证据位置见 `VFXAuthoring-Implementation.md`。证据留在本地，不把大量运行日志、截图和缓存提交为项目源码。

## 最终复验

按上述调整后的交付范围完成代表性复验。最终 Game/Editor 均为 VS2026 v145、RelWithDebInfo、PACK_GAME=OFF；实际设备为 NVIDIA GeForce RTX 3060 Laptop GPU。生产 GPU 默认关闭，Editor 本地候选显式配置 HE_EDITOR_VFX_GPU_PROBE=ON。

- Game SHA256：`27494163d54ee4309d71bdedb94a2831169c289a38393c2dd44df94673cb0e3f`。
- Editor SHA256：`3cbecc06e1b520d39552ce471f19f4e77da0025259695ca97ed29c20f57abf5a`。
- `build-only`：`.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2` PASS（66.3 秒）；`.\hectl.bat editor build -Configuration RelWithDebInfo -Parallel 2` PASS（368.8 秒）。Editor probe 配置成功后执行后者；源码及包身份固定。
- `runtime-manual`：Game High 双后端各 11 项 PASS，CPU/GPU 同场、自然结束及 allocator 回零；各 3 张截图已审，早期亮团展开、结束为空场。两组完整原生双流日志无错误，模拟/绘制提交及正计时可见。这不是完整质量档或性能预算验收。
- `runtime-manual`：Editor 双后端各 1960 个请求步进帧，加正常 end 30 帧，共 1990 帧；各 14 张截图，DX12 138 请求、Vulkan 141 请求，正常退出。固定步进与暂停序号、自然结束、重播、停止、end retirement、所有 allocator 失败计数为零均通过。真实 GPU 模拟/绘制提交号持续前进且完成计时为正；完整原生双流日志无错误。
- 人工视觉：Editor 双后端 28 张截图的联系图与原尺寸局部裁切均已查看，CPU 紫色粒子扩散、GPU 火花位置与范围变化可见，自然/停止/end 时空场无明显残留，范围内 PASS。仅覆盖中性背景、保存的时刻及视口，不推导全连续帧、任意效果或游戏可读性。
- `unit-test`：`E:/Python/Python311/python.exe Tools/VFX/test_editor_vfx_acceptance.py` PASS，11 项；只证明客户端协议逻辑，不替代上述真实运行。

精确运行命令（从仓库根执行）：

```powershell
E:/Python/Python311/python.exe Tools/VFX/quality_acceptance.py --backend DX12 --tier High --existing-build-sha256 27494163d54ee4309d71bdedb94a2831169c289a38393c2dd44df94673cb0e3f --output out/agent/rendering-evidence/vfx-authoring/final-representative-02/game-quality-dx12
E:/Python/Python311/python.exe Tools/VFX/quality_acceptance.py --backend Vulkan --tier High --existing-build-sha256 27494163d54ee4309d71bdedb94a2831169c289a38393c2dd44df94673cb0e3f --output out/agent/rendering-evidence/vfx-authoring/final-representative-02/game-quality-vulkan
E:/Python/Python311/python.exe Tools/VFX/editor_vfx_acceptance.py --backend DX12 --expected-editor-sha256 3cbecc06e1b520d39552ce471f19f4e77da0025259695ca97ed29c20f57abf5a --output out/agent/rendering-evidence/vfx-authoring/final-editor-recheck-01/editor-vfx-dx12
E:/Python/Python311/python.exe Tools/VFX/editor_vfx_acceptance.py --backend Vulkan --expected-editor-sha256 3cbecc06e1b520d39552ce471f19f4e77da0025259695ca97ed29c20f57abf5a --output out/agent/rendering-evidence/vfx-authoring/final-editor-recheck-01/editor-vfx-vulkan
```

输出目录须新建，以上为已执行命令；复现时换新目录。Game 报告位于 `final-representative-02/game-quality-*`；Editor 独立根审在 `final-editor-recheck-01/root-review.json`，原始 `visualStatus=NOT_REVIEWED` 不改写。入口及完成边界见 [Editor 特效诊断](VFXAuthoring-EditorDiagnostic.md)。

修复与失败留存：`final-representative-01` 保留共享统计函数缺完整公共类型头文件的编译 FAIL；补 `RenderDevice.h` 后最终构建通过。`final-representative-02/root-review.json` 保留 Editor 命令 PASS 但全黑视口的视觉 FAIL；测试场景改为复用已有预览相机和曝光，只修正 fixture，未改引擎或特效包；随后上述 Editor 两后端重验通过。旧驱动已与失败图像一同归档。

已完成的角色脚本复验：`E:/Python/Python311/python.exe Tools/VFX/character_acceptance.py --backend DX12 --existing-build-sha256 A5D0C0D02AC67D874277386878FB495CF62D9CEF670E45C8951937D6F81F30C8 --case character --timeout 600 --max-frames 1500 --output out/agent/rendering-evidence/vfx-authoring/character-game23-dx12-03`，`runtime-manual` PASS（51 项断言、16 个采样点、510 个步进帧、正常退出）。根审完整原生日志无错误，检查了全帧联系图和原尺寸角色裁切图；重载前实例仍 playing 且 paused，重载后旧特效与临时灯光消失。

视觉范围限制：三个 `large-yaw` 样本的骨骼 palette hash 相同，因此本次截图不能证明 ±170° 端点及中间混合确实变化；该项明确 NOT_VERIFIED，不把脚本总体 PASS 当作大角度旋转插值的证明。行走、攻击轮廓及挂点特效在本次采样中可见。表现组合独立 DX12 子项完成 13 项逐轨状态检查和正常退出，音频实际听感未测。

## 剩余支持边界

- GPU 路径仍通过显式开发探测开启；生产默认和发布包不据此标记已验证。
- 原生 GPU 物理字节、按 fence 回收的全部分配没有完整观测证明；OS 总量仅诊断，公开池大小是逻辑字节。
- 挂点跟随覆盖实体局部挂点，不等价于任意蒙皮骨骼挂点；导入与动画的具体范围见相应记录。
- 图像审核只覆盖实际保存和查看的采样时刻；不推导未采样的完整连续表现、所有质量档或实际游戏可读性。
- 本轮不重复原始全部压力、模板和性能矩阵；未复验的 Release 性能、Shipping GPU 与包装内容不标记通过。
