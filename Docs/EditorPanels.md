# Editor 面板与诊断

## 发布

菜单栏的「发布」与 `Ctrl+P` 打开同一个发布面板。选择平台配置后，可以编辑输出路径、点击「选择目录」使用系统文件夹选择器，或点击「默认」恢复 `Dist/<配置 ID>`。相对路径以当前项目为基准，绝对路径可指向其他磁盘。

「保存配置」保存到当前项目的 `Config/BuildProfiles.json`。「检查环境」「构建」也会先保存配置。场景运行时需先停止 Play。「重新加载」会询问是否放弃未保存的修改。

输出必须是新目录、空目录，或同一项目与配置生成的目录。构建器拒绝覆盖无归属的非空目录、其他配置的产物、项目/引擎根目录及其祖先、链接或联接目录。预览也检查产物归属。跨磁盘发布先复制到目标旁的暂存目录，完成后再替换；上一份输出保留为 `输出目录.previous-<ID>`，位置写入构建日志。复制失败不会替换现有输出。

路径与替换逻辑测试：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Build/Test-OutputDirectory.ps1
```

这项测试不构建 Shipping 游戏，也不证明所有发布后端的产物可交付。

## 统计

「窗口 → 统计」显示主视口最近渲染帧的数据：各通道绘制提交、GPU 间接绘制命令、2D 批次、对象、LOD 后候选三角形、材质程序、纹理绑定、几何缓冲和 CPU/GPU 耗时。GPU 指标来自最近完成的回读；没有采样时显示未采样。各通道数字不能简单相加作为整帧 Draw Call 总数，也不包含 UI/特效。

独立 FPS 叠加层已移除；编辑器 FPS 仍可在底部状态栏和统计面板查看。统计面板可直接打开 RenderGraph 诊断。

## 视口显示

视口「显示」菜单分为视图模式、缓冲区可视化、光照诊断和显示选项。可检查基础色、世界法线、金属度、粗糙度、材质 AO、屏幕空间 AO、对数视深，以及直接光照、环境光、阴影和 HDR 有效性。选择「正常着色」返回合成画面，当前诊断模式显示在视口左上方。

这些选项使用当前会话的渲染设置，不保存到项目；2D 和离线渲染模式不提供这些实时诊断选项。材质通道由共享表面着色路径可视化，不能等同于完整的原始 GBuffer 附件查看器。深度使用视空间 Z 的绝对值，以 `log2(1 + depth) / log2(1001)` 显示，1000 个世界单位及以外为白色。

## 帮助与布局

教程提供最简工作流程，并明确标记内容待定。「关于」介绍作者与共建方式，提供 GitHub 访问和复制地址按钮。材质展示场景与 ImGui 演示归入「示例」分组；运行场景期间禁用材质示例切换。

浮动发布、统计和帮助窗口限制在可用工作区内，并遵循 Editor DPI 缩放。工具栏对旧布局补足实际内容高度；日志搜索框随面板宽度变化。

交互参考：[UE 视口模式](https://dev.epicgames.com/documentation/unreal-engine/viewport-modes-in-unreal-engine)、[Unity 渲染统计](https://docs.unity3d.com/cn/2021.3/Manual/RenderingStatistics.html)、[Godot 项目导出](https://docs.godotengine.org/en/stable/tutorials/export/exporting_projects.html)。
