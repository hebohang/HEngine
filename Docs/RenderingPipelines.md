# 渲染管线与设置

## 使用设置

打开现有「项目设置 → 管线与性能」。先选择默认配置、主视图、编辑器视图或预览视图，再选择保存范围。
「本次会话」用于临时试用；「项目」和「我的设置」通过现有设置服务持久化。草稿通过窗口底部应用，取消不会发布。
每个覆盖字段可以恢复继承。设备、交换链、缓存预算不能由相机覆盖；命令行保留优先级。

场景持久覆盖由唯一的 `RenderSettingsComponent.Overrides` 保存，相机覆盖由实际选中的
`CameraComponent.RenderOverrides` 保存；两者使用现有设置字段 ID 的 JSON 对象。未添加组件或旧相机未保存该字段时，按空覆盖继承。
提取器先读取场景与相机组件，再合并调用方明确提供的对应覆盖；设置服务仍负责字段权限、Requested/Effective 和降级。
编辑器场景相机没有游戏相机实体时不读取游戏相机组件覆盖，仍解析默认配置、编辑器视图预设和场景层；资源预览可通过自己的视图描述提供覆盖。

Profile 是 `Assets/Rendering/PipelineProfiles/*.hepipeline` 的版本化资产。创建/复制、保存、重命名操作写入资产；
选择资产只将其内容放入当前草稿，随后通过同一设置事务应用。配置展开后的值也保存在设置层，因此项目重开不依赖编辑器状态。
Default 保持默认功能，Low Cost 显式关闭 AO/GI/反射并选择 Forward/FXAA，High Quality 是显式选择，不能静默开启。
未知功能和未来版本的参数保留在 Requested，Effective 禁用并显示原因。显式空功能列表表示关闭全部可选 Feature。

## 宿主与实例

`RuntimeRenderer` 拥有设备、交换链、提交与 Present，通过 `RenderPipelineRegistry` 创建每视图的 `RenderPipeline`。
默认 `realtime` 工厂提供现有 `SceneRenderer`，Forward/Deferred 共用兼容的场景、材质、Shader 和资源服务。
现有离线 provider/session 通过同一管线宿主接口适配，未复制路径追踪器。Provider 的离线任务控制继续位于现有 Offline Renderer 面板。

每视图保留独立图、TAA/GI/曝光历史和配置身份。帧开始前只比较配置身份；改变时先准备所有受影响视图，成功后统一提交。
候选失败会 abort，保留上一个有效图；旧图、Feature 和效果资源随实际提交序号退役。
场景/相机自动配置中的参数拒绝保留所有视图的上一配置，并记录具体字段或功能错误；相同失败输入不会每帧重复准备候选。
配置、场景代际、视图生命周期或尺寸变化后重试。显式设置事务始终尝试，并通过原设置服务返回失败与保留草稿。
普通后处理常量修改保留图和历史，结构变化才重建。缓存预算只在已完成资源的回收点生效，不等于显存总上限。

## Feature

注册入口为 `RegisterRenderFeature(descriptor, factory)`；配置为：

```json
{"type":"my.effect","version":1,"instance":"effect-a","enabled":true,"parameters":{}}
```

类型 ID 和配置版本决定工厂兼容性；instance 在一个 Profile 内唯一。同阶段保持配置顺序，跨阶段按依赖阶段排序。
工厂接收 `RenderFeatureCreateContext`（该视图设备、共享 Shader 服务与缓存路径）和参数，在配置准备期间调用，创建失败销毁已创建候选；不要在 `Build` 中同步编译 Shader 或无条件创建持久 GPU 资源。
工厂及配置准备使用 `std::invalid_argument` 报告可恢复的参数拒绝；应先校验 JSON 类型，将参数问题转换为这一明确错误。
设备故障、分配失败等错误仍传播，自动应用不会笼统吞掉任意异常。
`Build` 声明实际 RenderGraph Pass，使用标准图读写与附件契约。`RenderGraphFeatureScope` 将真实 Pass 归属记入诊断。
内建 `builtin.cloud` 独占云的 GPU 资源和历史，通过通用准备、导入、提交、回滚、回收和状态交换 hooks 运行；其质量参数继续来自已有 Clouds 设置。
`builtin.outline` 使用受控的已选择对象绘制能力，`builtin.postprocess` 调用每视图 HDR 后处理服务。
新增/替换这些 Feature 不需要修改 SceneRenderer 编排。

| 阶段 | 输入/颜色 | 坐标与采样 | 输出责任 |
| --- | --- | --- | --- |
| TemporalComposite | 场景合成颜色（TAA 开启时已经重建）；单采样深度与原始光栅深度分别提供 | 云显式支持已 resolve 深度和 MSAA 颜色组合；提供真实样本数 | 云合成后进入透明和描边 |
| SceneOverlay | 场景线性 HDR、当前遮挡深度 | TAA 后几何使用非 jitter 投影及额外真实深度；MSAA 保持匹配样本数 | 描边等保持已有颜色的合成 |
| HdrPostProcess | 已 resolve 的场景线性 HDR，可用法线/运动输入由需求计划产生 | 原生输出尺寸、单样本；时间重建已完成 | HDR 效果；禁止重复最终 Tone Mapping |
| DisplayPostProcess | 已 Tone Mapping/空间 AA 的显示编码颜色目标 | 单样本、无 jitter | 替换颜色须匹配格式/尺寸，由计划复制回宿主输出，随后 UI/截图/Present |

`Depth/Normal/MotionVector/OpaqueColor/HdrColor/DisplayColor/History` 是显式需求位；未提供的阶段输入会拒绝图构建。
`bDepthJittered` 和 `bSurfaceInputsJittered` 分别描述深度与法线/速度的采样坐标；TAA 已重建颜色不表示辅助输入也已去抖动。
需要坐标变换的功能使用帧上下文中的 `RasterCamera`；不能假定同阶段全部纹理共享 jitter。
OpaqueColor 在不透明光照、天空和反射之后、TAA与透明之前按需求生成共享快照；MSAA先resolve。没有消费者时不增加复制。

FSR 3.1 开启时独立 TAA 不执行；透明几何、描边与 VFX 在内部光栅尺寸绘制并保留投影抖动，随后 FSR 重建到输出尺寸，再执行 HDR 后处理、Tone Mapping 和原生尺寸 UI。设置及回退契约见 `RenderResolution.md`。

FSR 的 SceneOverlay 阶段将 `bTransparencyCoverageInAlpha` 设为 true：进入透明绘制前保留 HDR RGB、清零 Alpha，此后的 Alpha 表示透明覆盖率。透明网格及世界精灵按 `srcAlpha + dstAlpha * (1-srcAlpha)` 累积覆盖；当前 Effekseer 普通混合／加法路径使用最大 Alpha。全屏扩展需要保留已有覆盖 Alpha，几何扩展按自身不透明度合成，不能无条件把整屏 Alpha 写为 1。该约定只持续到 FSR 输入准备，HDR 后处理阶段此标志为 false。

Reactive 取颜色差响应与 `0.9 * coverage` 的较大值，Composition 保留完整覆盖率与颜色差响应的较大值。颜色差路径继续处理折射及保留目标 Alpha 的减法／乘法粒子；这类路径没有单独的逐片元覆盖输出。低对比度透明网格、精灵和普通／加法粒子因此不会单靠颜色差漏掉响应。新增的原始输入 AOV 可用于核对覆盖率和颜色差，不能把非零遮罩本身当成拖影质量验收。
内置普通／加法／减法／乘法／扭曲粒子及开发开关下的 GPU 加法粒子已做双后端输入与生命周期验证；测试入口及范围见 [FsrParticleValidation.md](FsrParticleValidation.md)。GPU 探针不改变生产能力状态。

角色横移、转身、头发与布料的可重复序列及逐三角形运动回投检查见 [FsrMotionValidation.md](FsrMotionValidation.md)。Quality 与 NativeAA 的遮挡变化对照单独记录，不作为无误差真值。
通用颜色历史通过 `context.History->ReadColor/StoreColor/IsValid` 声明，在成功提交后才有效；重置和图重建使历史失效；取消不发布未提交历史，并保留此前有效历史。历史由对应每视图效果持有，并随图配置候选和 GPU 完成状态退役；不得跨预览与主视图共享历史。
`context.Color()` 表示当前阶段的可替换目标；声明的颜色访问方式会与 Feature 实际增加的图访问核对，不是仅供显示的标签。
目前通用绘制能力是已分类的选中对象列表；任意场景过滤/材质 Pass 扩展仍必须通过公开能力增加，不能访问 SceneRenderer 私有实现。

场景颜色在后处理中可按自动/手动曝光比例调整，最终 ToneMap 执行一次手动曝光及显示转换。
线性 HDR 截图在后处理前取样，显示截图在输出阶段取样。Effekseer Color 纹理用线性工作流 loader 进行 sRGB 解码，数据纹理保留线性。
Effekseer 的 descriptor、常量内存和动态顶点缓冲按宿主帧槽及该帧内每次录制独立持有；多个视图不会轮用同一份在途内容。
只有真实提交完成后才能复用帧槽。已经发出的提交即使随后 Present/回调失败，也必须先登记退役序号；取消录制不会清掉先前提交的 fence。
Vulkan 桥接的旧 framebuffer/外部纹理缓存还需超过最近使用窗口且提交已完成才回收，避免反复 resize 把旧输出保留到退出。
开发诊断 `HE_VFX_CACHE_RETIREMENT_LOG=1` 记录缓存 Owner 回收数量；它不统计 LLGI 的剩余引用，也不代表实际显存。

## 需求、材质与资源

Forward 在无 GI/反射且材质兼容时只生成 normal/motion/稳定 ID 三个辅助 MRT；Deferred 保持完整表面数据。
复杂自定义表面保留完整数据路径。自定义静态 opaque 的 GPU/LOD 边界见 [ShaderMaterials.md](ShaderMaterials.md)。
Volume 的 Bloom/Contrast/Saturation 按现有优先级和空间权重混合；新增覆盖默认关闭，旧场景兼容。
每字段保留线性混合系数，应用到各视图自己的参数基准，不把离散管线/格式/采样设置当浮点混合。

资源池统计区分活动/保留、空闲缓存、退役和已回收的名义字节。实际系统预算单独显示；Vulkan 未提供预算时显示不可用。
同资源对象复用不称为 heap aliasing；逻辑 Compute 提示不称为异步提交。诊断同时显示实际录制队列。

## 验证

实施清单、精确结果与证据目录见 [升级记录](../Tools/Agent/Rendering-Pipeline-Upgrade.md)。
`Test-PipelineFeatures.ps1` 检查功能开关、未知功能和不同视图实际图；`Capture-PipelineSettings.ps1` 检查三种窗口/DPI。
正式性能运行关闭 RenderValidation，区分预热和完成的 GPU 样本。
Agent A/B 开关 `HE_RENDER_FULL_SURFACE_DATA=1` 强制完整辅助表面，`HE_RENDER_DISABLE_MATERIAL_PACK_CACHE=1` 禁用原始参数缓存。
这两个开关仅读取一次；必须分别启动受控进程，在相同场景和设置下比较，不能把不同画质 Profile 当作等画质提速。
`Benchmark-PipelineDepthPyramid.ps1` 对照开发开关 `HE_RENDER_MERGE_GI_DEPTH_PYRAMID=0/1`：
仅将 GI 的逐 mip 图 Pass 合为一次图回调，保持各级 dispatch 和必要 barrier；默认仍使用原路径，不适用于 Occlusion MAX 金字塔。
`-Mode Correctness` 在 DX12/Vulkan 开启图及后端验证，`-Mode Performance` 单独关闭验证并采样；不能混用两者的计时。
CSV 中 `cpuGraphRecordMs` 只包围 `Graph.Execute`，包括图回调、barrier 和 instrumentation，不包含准备、提交或 Present。
