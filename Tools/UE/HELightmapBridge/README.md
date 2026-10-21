# UE5.4 静态 HQ 光照贴图接入

此插件仅在 UE5.4 命令行读取关卡中的静态光照映射及其源 mip；运行时不依赖 UE。所有导出位置必须在 UE `Content` 目录外，原 `.uasset` 不修改。先确认目标 Fab 资产允许导出及在目标项目中使用。

1. 在 HE 项目 `Saved` 下创建**全新**暂存目录：

   ```powershell
   .\Tools\UE\Prepare-HELightmapBridgeStage.ps1 -SourceProject C:\WorkSpace\UnrealProjects\SICKADYNASTY\SICKADYNASTY.uproject -StageDir C:\WorkSpace\HEngineProjects\MechanicGirlLab\Saved\UELightmapBridge
   ```

   脚本复制 `.uproject`/Config，并以目录联接读取源 `Content` 和插件；它拒绝覆盖已有暂存目录。随后用 UE5.4 的 `Build.bat UnrealEditor Win64 Development -Project=<暂存.uproject> -WaitMutex -NoHotReloadFromIDE` 编译插件。不要在源 UE 工程中安装或编译它。
2. 使用 UE5.4 `UnrealEditor-Cmd.exe <暂存.uproject> -run=HELightmapExport -Level=/Game/SICKA_DYNASTY/Maps/MAP_Demo -Output=<HE项目/SourceAssets/.../lightmap-bindings-hq-allmips.json> -TextureDir=<HE项目/SourceAssets/.../LightmapRaw> -RawBasis=0 -AllMips -AllowCommandletRendering -unattended -nop4 -nosplash`。命令失败或源 mip 数不足时停止；不要以只有元数据的报告冒充可采样资源。
3. 运行 `python Tools/UE/PackHqLightmapDds.py <报告.json> <HE项目> Assets/Lighting/<关卡>/Atlases <texconv.exe>`。该工具要求 HQ `TSF_BGRA8`、完整 mip，并为每张 DDS 记录 SHA256。SICKA 实测为165张图集、约438 MB 压缩文件；具体项目需要按贴图预算检查内存与加载时间。
4. 运行 `python Tools/UE/BindStaticLightmaps.py <HE项目> Assets/Scenes/<场景>.he <UE检查.json> <报告.json> <HE项目/Assets/Lighting/.../HQLightmapTextures.json> Assets/Lighting/<关卡>/StaticLightmapBindings.json --embed-scene --blend 0.2`；仅在确实不属于静态网格导出的 Actor 上使用 `--exclude-actor <名称>`。SICKA 排除天空球。导入资源或重建场景后重复绑定；SICKA 的 `Tools/SickaLightmap.json` 与 `Tools/Build-SickaDynastyScene.py` 已自动执行这一步。

引擎 `MeshComponent` 保存项目相对图集路径、UV1 缩放偏置、两组 HQ 系数和 `StaticLightmapBlend`。渲染提取从整个场景收集唯一图集并在新场景代际预热不超过256张图集；DX12 heap-direct 的 GBuffer 路径按 UE5.4 `LightmapCommon.ush` 解码定向辐照度，将其作为静态网格漫反射的可调组成，同时保留直射光和镜面环境反射。未绑定、不可采样或其他图形后端采用现有环境光路径并给出日志；这些情况不能作为 UE GI 成功迁入的证据。当前没有按平台内存预算流送图集，也没有导入 UE 的阴影遮罩、体积光照贴图或动态图元间接光。

验收需同时检查 `hectl test RenderWorldSubmission`、`hectl test SceneEditingContainers`、Game/Editor 同机位截图、固定输入回放和长时帧时间。SICKA 的0.2混合在街道地面局部比纯图集更接近 UE，但全图与不使用图集基本相当；严格长期30FPS仍未达标，详见 `Docs/engine-upgrade-validation.md`。
