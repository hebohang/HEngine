# 屏幕空间皮肤扩散

本模块是通用延迟管线能力，适用于 `Skin.hlsl` / `SkinTwoSided.hlsl` 的所有角色；没有 MechanicGirlLab 材质名或实体名分支。

## 与 UE 参考的关系

2026-09-28 只读检查本机 `C:/WorkSpace/UnrealProjects/SceneTest/Config/DefaultEngine.ini`：DX12/SM6、Lumen GI/反射、虚拟阴影、局部曝光。此前角色材料审计 `MechanicGirlLab/SourceAssets/UEExport/skin-flags-inspection.json` 确认 `M_SKIN` 使用 Subsurface Profile、Opaque、TwoSided。本轮还只读检查本机 UE 5.4 的 `BRDF.ush` 与 `HeightFogCommon.ush`，确认联合 Smith 可见性及解析高度雾积分；HE 的数学实现遵循现有坐标/密度约定，不复制 UE 资产或源文件。

HE 原先的皮肤仅在直接光终止线使用局部 wrapped diffuse，没有邻近像素间的漫反射运输。本实现增加真正的漫反射分离与空间扩散；不是 UE Subsurface Profile 的逐值等效实现，也不包含耳部厚度透射、光线追踪多次散射或离屏扩散。

## 配置与作者化

渲染设置使用已有分层、保存和重新载入流程：

| 设置 ID | 默认 | 范围/作用 |
| --- | --- | --- |
| `Rendering.SkinSubsurface.Enabled` | `false` | 延迟皮肤扩散；关闭时保留单目标照明/局部扩散路径 |
| `Rendering.SkinSubsurface.RadiusMeters` | `0.003` | 0.0001–0.02 米，红通道高斯 sigma；依据当前投影转换为像素 |
| `Rendering.SkinSubsurface.Quality` | `1` | 1：每轴 7 个采样；2：每轴 13 个采样 |

角色材质已有 `skin.diffusion_tint`、`skin.diffusion_strength`、`skin.diffusion_mask` 继续控制逐像素 RGB 扩散权重。波长相对宽度为红/绿/蓝 `1/.48/.25`。这是固定的基础皮肤 profile，尚无独立 profile 资源编辑器。启用限于 Deferred，Forward、离线视口和材质数据调试保留原路径。

## 管线与生命周期

1. `SceneDeferredSkin.hlsl` 在独立 MRT 写入未施加雾的皮肤漫反射，最终 HDR 仍保留直接高光、IBL 高光及雾。启用时替换局部 wrapped diffuse，避免重复散射。
2. `SceneSkinSubsurface.hlsl` 水平/垂直两次扩散，只采样相同对象的皮肤，使用平面距离/法线限制遮挡边界，轮廓外采样替换为中心值以保持常量能量。
3. 合成 `原 HDR + 中心雾透射率 × 权重 × (扩散漫反射 - 原漫反射)`；高光、背景、眼睛、头发与雾的入射颜色不参与扩散。
4. 扩散位于延迟照明之后、天空/后续 GI/局部反射/透明合成之前；后置 GI 漫反射目前不再扩散，避免重复照明。已有 IBL 漫反射参与扩散。
5. 开关改变请求渲染图重建；半径/采样质量是每帧常量。临时纹理由 RenderGraph 管理，绑定缓存进入既有清理和图事务交换，不依赖项目脚本。

开启会增加一个漫反射 MRT、水平中间图和合成输出（RGBA16F，图资源池按寿命复用）。实际 GPU 时间应以 `SkinDiffusionHorizontal` / `SkinDiffusionComposite` 阶段测量；不能从采样数推断达到 60 FPS。

## 同轮基础数学修复

- `Shared/SurfaceLightingMath.hlsli` 统一 Modern 直接光、Modern BRDF LUT 与已有通用 BRDF 的相关 Smith GGX 可见性。
- 高度雾从光线中点密度改为解析积分；积分从 StartDistance 后开始，正确分割穿越 BaseHeight 的线段，并保留 HE 基准高度以下的密度上限。
- Skin 的视角粗糙度使用与最终光照一致的法线解析，包含 UV1 法线图和双面朝向。

## 验证记录

- `unit-test PASS`：`.\hectl.bat test RenderingShaderMath`，2026-09-28，编译实际标量 HLSL 并与 double 精度数值积分/Smith Lambda 独立定义比对；192 组高度雾、125 组可见性与互易性通过。构建 78.5 秒，日志 `out/agent/logs/build-tests-debug-HEngineTests.log`。
- `build-only PASS`：Game 构建编译新的 deferred skin / diffusion 的 DXIL、SPIR-V；GPU 实际运行验证使用 DX12，未把 SPIR-V 编译当作 Vulkan 运行证据。
- `runtime-manual PASS`：SSS OFF/ON 各执行 `.\hectl.bat game capture -Project C:\WorkSpace\HEngineProjects\MechanicGirlLab -StartupScene Assets/Scenes/Diagnostics/UEFaceNeutralStudio.he -Frames 180 -Resolution 1920x1080 -GraphicsApi DX12 -StrictGraphicsApi -FixedDeltaSeconds 0.0166667 -RenderSettingsPath C:\WorkSpace\HEngineDev\out\agent\render-quality\skin-diffusion-{off,on}.json -RenderStats -RenderStatsPath C:\WorkSpace\HEngineDev\out\agent\render-quality\skin-{off,on}.csv -PerformanceWarmupFrames 60 -CapturePath C:\WorkSpace\HEngineDev\out\agent\render-quality\skin-{off,on}.png -TimeoutSeconds 300`（花括号代表分别执行的两个取值）。CLI 的 RenderSettingsPath 是 stable-ID 平铺 patch，不是完整的嵌套配置文件。
- 两图及 `skin-comparison.preview.jpg` 已目视，眼睛/头发/背景未出现扩散污染，鼻部高光形状保留。工作室光照的效果较小，脸部固定 ROI 的 RGB8 平均绝对差为 0.177；此数字只证明画面变化，不是 UE 质量得分。
- 120 个测量帧、RTX 3060 Laptop、DX12/RelWithDebInfo、1920×1080 原生分辨率、3 mm/13 taps：Horizontal/Composite GPU 中位 0.184/0.188 ms，P95 0.185/0.189 ms。总体 GPU span P95 OFF/ON 4.317/4.713 ms。三帧槽的瞬态纹理保留量增加 149,299,200 字节；不是总显存测量。采样期间可能存在其他 C++ 编译负载，因此 CPU 帧时间不能作为 60 FPS 最终证据；整关卡性能由主任务独立测量。
- `runtime-manual PASS`：另将上述命令的场景改为 `Assets/Scenes/Diagnostics/UEFaceNeutralUESunOnly.he`，输出名改为 `skin-sun-{off,on}`，其余参数相同，OFF/ON 两次 build/run/capture 均通过。120 帧 OFF/ON CPU P95 8.293/8.551 ms、GPU span P95 5.109/5.719 ms；ON 两个扩散 pass 中位 0.187/0.196 ms。两图已目视，未发现眼/发/背景污染。此历史场景仍有展台屋顶遮挡，脸部没有预期的强硬阴影分界，故它只证明第二种光照环境复用，不能代替全部明暗边界/运动情况验收。汇总在 `out/agent/render-quality/skin-sun-summary.json`。

## 发片审计边界

同一近景仍可见发片轮廓颗粒。本轮没有降低 alpha cutoff 或放宽 TAA 的跨对象历史检查。只读检查 UE `M_HAIR` 的 opacity 图和项目实际 4096² DDS：独立红通道 coverage、0.3333 cutoff、-1 mip bias 均与现有提取值相符，albedo alpha 接近 1；不是误采样透明通道或低分辨率贴图。

当前 Mask 使用硬裁剪。TAA 已允许同发网格跨 primitive 历史，但轮廓处 hair/背景或 hair/皮肤仍需对象、深度、法线匹配，不能积累完整的亚像素覆盖。后续应设计有覆盖率信息的抗锯齿流程，不能以无条件接受异物体历史换取静态模糊而造成拖影。独立红通道 coverage 的 mip 目前也没有单独保持覆盖率，这是中远景资产处理的剩余项；本轮未声称发片质量等效 UE。
