> 原始证据目录：`out/agent/pipeline-upgrade/supplement/`；其他E1/R/C目录相对`out/agent/pipeline-upgrade/`。

# TAA 后匹配深度的条件与实测成本

证据类型：只读源码审计及既有 runtime-manual CSV 离线复算。本审计没有修改生产代码、构建或运行 GPU。比较提交 `cc7526ec156876f37566231405560f29ef70fe61` 与 `9f64aa5577e974bdbb735c06c31b27f6992be908`。

结论：最终版本并非在所有纯 opaque 场景无条件重放深度。Sponza 实验有真实后合成透明消费者，新增成本不能直接作为“无用 Pass”删除；本轮最终性能确实回退，不能只报告局部 guides 优化。

## 实际生成与使用条件

最终 `Engine/Source/Runtime/Graphics/Renderer/SceneRenderer.cpp:6220` 的 `UpdatePostTaaView` 在以下条件成立时设置 `bPostTaaDepthRequired`：TAA 开启、非 offline，并且透明列表、选中描边列表、Primitive2D 列表、活跃 Effekseer effects、空 Effekseer 验证开关中至少一项非空/开启。条件变化会令图 dirty。没有这些消费者的纯 opaque 场景不会添加匹配深度。

同文件 `:6774` 的条件块在 TAA 和 TemporalComposite 之后发布非 jitter 帧常量，再执行 `AddDepthPrepass(false, true)`；`:7118` 构建 `PostTaaUnjitteredDepth`。`:8184` 从权威 GPU batch 对象行重放，避免复用旧 jitter frustum/occlusion 结果漏遮挡物。它不是仅一个免费的状态切换。随后透明、Primitive2D、SceneOverlay 与 VFX 使用相应投影/深度。

基线同文件 `BuildGraph` 在 `SceneAntiAliasing.AddTaaPasses` 后直接执行 Cloud、Primitive2D、Transparent、Outline；没有 `UpdatePostTaaView`、`PostTaaUnjitteredFrame`、`PostTaaUnjitteredDepth`。因此旧版更低成本不能自行证明旧的投影/深度对齐正确。

## 既有性能证据

见 [18 次版本实验](../../out/agent/pipeline-upgrade/supplement/performance-versions-v2/summary.md)、[独立 CSV 复算](../../out/agent/pipeline-upgrade/supplement/performance-versions-v2/independent-audit.json)。DX12、1920×1080、每场景三对运行；18 次 exitCode=0，输入清单前后 4578 项完全一致。基线二进制为 `31FD15BD9B195D2D56FD2EBD53B8DD999C138318B4183BE40DBD6C1CA059CE16`，最终隔离产物为 `62D5EE53AED9875C18C0C7C34B58594A323AC4AE6D7D01D34FC27FF7FC008C35`，不是旧 C 的 204B hash。

| 场景 | 三对平均 GPU 帧变化 | 新增匹配深度均值 | 解释 |
|---|---:|---:|---|
| Sponza | 10.031719→10.566281 ms，+0.534562 ms / +5.329% | 约 0.68309 ms | 新 depth 约占最终 GPU 帧 6.46%；guide 由约1.17015降到1.07551 ms，无法抵消新增成本。 |
| SurfaceCoverage | +0.038813 ms / 约+1.694% | 约0.02397 ms | 正确性修复成本与其他变化共同作用，不能把帧差全部归于单 Pass。 |
| LocalLights | 三对 -0.001069 / +0.002589 / -0.015052 ms | 没有新 depth | 小幅混合变化，不宣称稳定提速。 |

`performance-versions-v2/Sponza-final-0.csv.passes.csv` 中 `TransparentForwardHDR` 1800 个采样约0.27 ms，`PostTaaUnjitteredDepth` 同样1800项。故 Sponza 不是“无后合成消费者”的纯 opaque 夹具。原始逐 Pass 时间及 GPU 帧跨度不能相加推导独立因果；时钟/温度与稳定性仍以完整实验记录为准。

## 真实的进一步收紧机会（未实施）

- `SceneRenderer.cpp:5894` 只按 `object.bSelected` 填充 `OutlineDrawItems`，条件未直接询问实际启用 Feature 是否调用受控描边。若只有选中对象且消费者全部禁用，可能不必重放；必须由公开实际绘制需求判断，不能硬编码只认识 builtin.outline 而破坏插件。
- `Primitive2DBatches` 包含 `DepthMode`。`:9130` 仅 DepthTest 模式开启深度，其他模式关闭。因此只有 overlay Primitive2D 时可研究分离“非 jitter 投影”与“匹配深度”需求，不能把投影也直接跳过。
- EffekseerRenderGraphPass.cpp:396 即使没有 depth snapshot，也绑定 `DepthRead` 附件；`bRequiresDepthSnapshot=false` 不是没有深度测试的证明。不能据此删掉 VFX 所需匹配深度。

本次保留正确性路径并如实报告回退。优化候选须另行验证遮挡、边界、alpha mask、骨骼和取消/切换安全；没有新的测量，不承诺优化收益。
