# HEngine 内建材质球展示库

该 5x5 展示矩阵使用 7 个共享 Surface Shader Definition 驱动 25 个材质球实例与 1 个独立地面实例。材质球不与 Shader 一一对应；同类外观通过 `.hmat` 参数复用同一个 Shader，因此修改颜色、金属度、粗糙度或特效强度不会生成新的 Shader/Pipeline。25 个实例也全部共享同一份 `MaterialBall.hmesh/.bin`，只通过 `MeshComponent.MaterialOverrides` 替换 slot 0，避免重复解码和 GPU 几何上传；slot 1 继续使用中性底座材质。编辑器无论通过菜单、文件对话框还是内容浏览器打开该 Engine 场景，都会仅为这个场景持续提交画面，保证时间驱动材质在相机静止时仍持续更新，而不会改变其他场景的全局实时预览设置。

| 编号 | Material Instance | 共享 Shader | 主要展示特性 |
| --- | --- | --- | --- |
| M01 | Ruby Plastic | `ShowcasePBR` | 高光红色塑料、非金属低粗糙度 |
| M02 | Ivory Plastic | `ShowcasePBR` | 象牙色塑料、柔和高光 |
| M03 | Matte Rubber | `ShowcasePBR` | 深色橡胶、高粗糙度 |
| M04 | Rough Ceramic | `ShowcasePBR` | 粗陶、程序化微表面法线 |
| M05 | Glazed Ceramic | `ShowcasePBR` | 釉面陶瓷、低粗糙度 |
| M06 | Brushed Aluminum | `ShowcasePBR` | 拉丝铝、金属度与细节法线 |
| M07 | Polished Silver | `ShowcasePBR` | 抛光银、高金属低粗糙度 |
| M08 | Warm Gold | `ShowcasePBR` | 暖金色导体 |
| M09 | Aged Copper | `ShowcasePBR` | 铜色金属、较粗糙微表面 |
| M10 | Gunmetal | `ShowcasePBR` | 深色枪灰金属 |
| M11 | Jade | `ShowcasePBR` | 玉石色、轻微自发光 |
| M12 | Ruby Gem | `ShowcasePBR` | 红宝石式低粗糙高光 |
| M13 | Sapphire Gem | `ShowcasePBR` | 蓝宝石式低粗糙高光 |
| M14 | Normal Detail | `ShowcasePBR` | 强程序化切线空间法线细节 |
| M15 | Emissive Lava | `ShowcasePBR` | HDR 自发光与表面起伏 |
| M16 | Fresnel Azure | `FresnelRim` | 蓝色 Fresnel 边缘光、慢速呼吸与上行扫描 |
| M17 | Fresnel Magenta | `FresnelRim` | 洋红 Fresnel、反向扫描与错相脉冲 |
| M18 | Iridescent Pearl | `IridescentLaser` | 缓慢漂移的珠光虹彩与柔和光带 |
| M19 | Laser Chrome | `IridescentLaser` | 镭射虹彩、快速动画扫描线、镜面金属 |
| M20 | Procedural Checker | `ProceduralPattern` | 持续漂移的 Unlit 程序棋盘格 |
| M21 | Toon Bands | `ProceduralPattern` | 呼吸变化的 Unlit 分段卡通色带 |
| M22 | Double-Sided Stripes | `ProceduralPattern` | 反向滚动双面条纹与背面色调差异 |
| M23 | Clear Glass | `HologramGlass` | 单层透明玻璃、轻量全息扫描与边缘光 |
| M24 | Hologram Scan | `HologramGlass` | 双层动态扫描线、数字闪烁与高强度轮廓发光 |
| M25 | Masked Dissolve | `MaskedDissolve` | 流动 Mask 溶解、呼吸阈值、发光边缘及一致的 Forward/Depth/Shadow/Outline 裁剪 |

地面使用 Lit 的 `ShowcaseFloor` Shader，呈现类似默认建模地面的中性灰双层网格：细网格随距离先淡出，粗网格延伸得更远，并使用屏幕空间导数降低远处闪烁。地面没有滚动、能量环或自发光，同时保留灯光响应和材质球投影。共享 Shader 位于 `Engine/Content/Shaders/Materials/BuiltInShowcase/`，并由 `HShaderCompiler` 的 runtime shader target 统一编译发布。实例使用 V2 typed `Parameters`/`Textures`，同时保留 `Properties` legacy dual-write 以兼容过渡期读取路径。
