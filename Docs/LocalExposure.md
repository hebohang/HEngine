# 局部曝光与玩家曝光补偿

通用 HDR 后处理路径 `ScenePostProcessPass` 支持保留颜色比例的局部曝光。
它位于最终 ACES / sRGB 转换之前，Game 和 Editor 使用同一实现。
默认强度为零，旧项目不改变外观；设置走 `RenderSettingsService` 的分层事务、持久化及实时应用。

| 设置 ID | 默认 / 范围 | 含义 |
|---|---|---|
| `Rendering.PostProcess.LocalExposure.ShadowStrength` | 0 / 0–1 | 相对于中灰的暗部压缩强度 |
| `Rendering.PostProcess.LocalExposure.HighlightStrength` | 0 / 0–1 | 相对于中灰的亮部压缩强度 |
| `Rendering.PostProcess.LocalExposure.MaxEV` | 2 / 0–4 | 局部调整的最大档数 |
| `Rendering.PostProcess.ExposureCompensationEV` | 0 / -4–4 | 玩家曝光补偿，正值变亮；叠加在关卡、相机或自动曝光之后 |

四分之一尺寸 HDR 降采样产生 log2 亮度。水平和垂直双边滤波构造基底，
用亮度差抑制跨明暗边缘的混合；全尺寸合成用原始像素亮度引导上采样。
只对基底施加受限曝光修正，像素细节保留，RGB 乘相同的因子以保留色相。
该算法不等效 UE 双边网格实现，没有额外时间历史，因此不会引入自己的历史拖影。

亮度基底复用 bloom 中间图的 alpha，RGB bloom 滤波核不变。
没有 bloom 但启用了局部曝光时仍正常分配并执行亮度路径；关闭两者会释放对应图资源。
参数跨越开/关时重建图，资源退休沿用已有 RenderGraph/提交序号生命周期。
亮度下限、曝光范围和最大 EV 可避免零亮度除法与无限放大；纯黑仍为纯黑。

推荐从暗部 0.25、亮部 0.15、最大 1.25 EV 开始做同镜头对照。
它改善显示范围内的明暗可读性，不替代缺失的 GI、光源、皮肤散射或材质纹理。
尚未实现独立逐体积局部曝光覆盖；当前可以按项目、用户、会话、已有视图设置层保存。

验证记录与完整运行命令见 `engine-upgrade-validation.md`。
