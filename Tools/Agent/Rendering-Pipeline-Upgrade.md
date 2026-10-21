# 渲染管线升级实施与验收

任务 `20260911-rendering-vfx-01 / 1`，起点 `cc7526ec156876f37566231405560f29ef70fe61`。
本记录区分实现、运行观察和性能测量；不包含后续任务。

## 实际实现

| 阶段 | 接入的生产行为 |
| --- | --- |
| 管线与 Feature | RuntimeRenderer 经注册工厂持有每视图独立 RenderPipeline；默认实时实现保留 Forward/Deferred，离线适配原 provider/session。描边、HDR 后处理和云走通用 Feature 契约，云独立拥有资源和历史。全部视图先准备和预检，成功后统一发布，失败保留旧输出。 |
| 配置与界面 | 复用 RenderSettingsService 草稿、来源、Requested/Effective 和事务；增加版本化 Profile、迁移、未知依赖保留及 cooker 依赖。项目设置集成 Profile、Feature、视图和作用范围。World/选中 Camera 的持久覆盖进入生产提取链，Volume 连续参数按每视图基准混合。 |
| 材质与时间稳定性 | 符合 ABI 的静态 opaque 自定义材质进入 GPU 提交和 LOD，复杂材质明确回退；参数打包按稳定身份、版本和布局缓存。TAA 后几何用非 jitter 投影和匹配的真实深度；修复 Effekseer 当前相机剔除、颜色加载，并按宿主帧槽及每次录制隔离 descriptor、常量和动态顶点缓冲。 |
| 图与资源 | 修复 Buffer 能力、mip/slice 初始化和访问校验，明确附件 load/store/discard/full overwrite。完整覆盖可裁剪旧内容生产者，排序约束单独保留。资源池只回收已完成缓存，区分活动、缓存、退役和名义字节。Forward 按消费者生成紧凑辅助 MRT，复杂路径保留完整数据。 |

场景/相机参数错误只捕获明确的 invalid_argument 配置拒绝，保留全部旧视图；相同失败输入不逐帧重复准备。
显式设置事务仍返回失败并保留草稿；设备错误、分配失败不被吞掉。提交发出后先登记实际 fence，
即使随后 Present/回调失败也不早释放；取消只放弃未提交录制。

使用与扩展见 [RenderingPipelines.md](../../Docs/RenderingPipelines.md)、[ShaderMaterials.md](../../Docs/ShaderMaterials.md)
和 [RenderGraph 契约](../../Engine/Source/Runtime/Graphics/RenderGraph/Contracts.md)。

## 证据版本

原始证据位于 out/agent/pipeline-upgrade/，不提交生成图像、日志及构建产物。
final-evidence-e1ea/ 保存广覆盖和18次性能运行；Game SHA256：
`E1EA679FAA6A52F3FBA3797ECF7139CB8D0A863261A8B3A3B03ED94A76A88AB3`。
recovery-final/ 保存最后 VFX fence/配置拒绝修复后的编译、单测及双后端回归；Game hash 在 game-sha256.txt。
closure-final/ 保存 Vulkan 缓存回收与 Pass 合并实验之后的编译、Graph/Effekseer单测、三宿主no-op、双后端合并正确性和六次性能对照；最终Game SHA256：
`204B047873104B2617336D64B120344EF6F70014D0D3B467953E88406D410605`。
E1 性能不冒充最终二进制测量。首次交付时初始性能构建中止；随后已恢复原始源码并重新构建，版本对照和原生UI补验见 [补充验收](Rendering-Pipeline-Supplement.md)。
以下 A/B 为同一 E1 版、相同画质的受控开关实验，不是与起点提交比较。

## 构建、生成和测试

| 证据类型 | 精确命令 | 结果 |
| --- | --- | --- |
| build-only | `hectl game build -Parallel 2` | PASS，recovery及最终204B版 |
| build-only | `hectl editor build -Parallel 2` | PASS，recovery及最终闭环 |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Parallel 2` | PASS，宿主 fence 调用已迁移 |
| build-only | `cmake --build out/agent/game-relwithdebinfo --target HEffekseerDX12InteropSmoke --parallel 2` | PASS；经 Toolchain.ps1 导入 v145 且持有 cache lease |
| unit-test | `hectl test EffekseerRuntimeLifecycle -Parallel 2` | PASS，真实剔除、per-record租约/在途拒绝/取消/重复提交 |
| unit-test | `hectl test RenderSettings -Parallel 2` | PASS，Profile/迁移/权限/工厂拒绝/失败输入匹配/事务 |
| unit-test | `hectl test RenderGraph -Parallel 2` | PASS，Buffer/子资源/裁剪/后端深度状态 |
| unit-test | `hectl test RenderGraphResourcePool -Parallel 2` | PASS，预算回收和在途保护 |
| unit-test | `hectl test MaterialParameterPacking -Parallel 2` | PASS，早期同任务验证 |
| unit-test | `hectl test MaterialHotReload -Parallel 2` | PASS，早期同任务验证 |
| unit-test | `hectl test RenderWorldSubmission -Parallel 2` | PASS，输入错误/旧世界保留/恢复 |
| unit-test | `hectl test OfflineReferenceRendererSession -Parallel 2` | PASS |
| unit-test | `out/agent/editor-debug/bin/Programs/HCooker/HCooker.exe --self-test` | PASS，Profile/视图/未知依赖/alias，hcooker-final-self-test.log |

反射首次更新由正式构建节点生成 Camera/World 字段，DevGame/Editor各 changed=11、unchanged=250、deleted=0。
最终 no-op：`hectl codegen run -CodeGenHost DevGame -Configuration RelWithDebInfo --json`，
相应 Editor/RelWithDebInfo、Tests/Debug 三组均 PASS；每组 changed=0、deleted=0，261个生成输出
hash/mtime变更、增加、丢失均为0。证据 recovery-final/codegen-noop/results.json 和 closure-final/codegen-noop/results.json 及前后 manifest 输出快照。
全部新增/修改 PowerShell 脚本语法解析 PASS；git diff --check PASS，换行转换提示不算错误。

## 运行与界面

以下均为 runtime-manual：脚本行为断言加人工图像审查，不是自动图像质量测试。
从仓库根目录调用；$e = out/agent/pipeline-upgrade/final-evidence-e1ea，$h = 上述完整 E1 hash；
各 *.specification.json 保存真实绝对参数。$python 为支持 Pillow 的解释器，本机使用
C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe。

| 精确调用 | 结果和范围 |
| --- | --- |
| `Tools/Agent/Test-RenderingPaths.ps1 -GraphicsApi DX12 -Resolution 1280x720 -SwitchCycles 1 -SettleFrames 30 -IncludeHardwareRTGI -TransientTextureReuse -ExistingBuildSha256 $h -OutputDirectory "$e/paths-DX12"` | PASS，路径/开关/resize/reload/表面；SSGI亮斑单独记录，不等于画质全通过 |
| 同上改 `-GraphicsApi Vulkan -OutputDirectory "$e/paths-Vulkan"` | 脚本PASS，58项：39 PASS、0 FAIL、10 NOT_REVIEWED、6 CAPTURED、2 UNSUPPORTED、1 N/A；40张图另有人工审查 |
| `Tools/Agent/Test-PipelineFeatures.ps1 -GraphicsApi Vulkan -ExistingBuildSha256 $h -OutputDirectory "$e/features-vulkan"` | PASS，主/预览独立、实际World/Camera覆盖、未知Feature和停止验证 |
| `Tools/Agent/Test-CustomMaterialLod.ps1 -GraphicsApi DX12 -ExistingBuildSha256 $h -OutputDirectory "$e/custom-lod" -PythonExecutable $python` | PASS；Forward/Deferred × individual/grouped GPU四组均84602→21150三角形；12图已看。individual GPU不是CPU fallback对照 |
| `Tools/Agent/Validate-Clouds.ps1 -GraphicsApi DX12` | PASS，30图已看；out/agent/rendering-evidence/cloud-flow-20260912-010135-996，AA/多视图/resize/暂停重启 |
| `Tools/Agent/Capture-PipelineOffline.ps1 -GraphicsApi DX12,Vulkan -OutputDirectory "$e/offline"` | DX12真path-tracing provider完成320×180、8 spp；Vulkan UNSUPPORTED，实际后端缺少AS/RayQuery，exit1拒绝把实时回退截图当通过 |
| `Tools/Agent/Capture-PipelineSettings.ps1 -GraphicsApi DX12 -Frames 90 -OutputDirectory "$e/settings-ui"` | PASS，1920×1080@1、1280×720@1、1920×1080@1.5逐张查看，底部操作和滚动可用 |

另执行三次真实界面交互：
`hectl editor check -Configuration RelWithDebInfo -GraphicsApi DX12 -StrictGraphicsApi -StartupScene Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he -Resolution 1600x1000 -Frames 10000000 -TimeoutSeconds 600`。
均用真实控件操作、Alt+F4正常关闭，exit0。关闭阴影草稿→取消恢复；应用会话后场景阴影消失且项目文件不变；
重开恢复Engine来源及阴影；选择Project后保存，再重开为Project来源且阴影关闭。
11张截图、3份日志及配置hash在 settings-interactive/。Editor hash：
`168866E35243429F9AB526C1FC9DA355BF48C19CBDC83140F01B4C723E1658A9`。
finally恢复Project和User原始字节，restoration.json两项hash一致。这覆盖真实控件/事务/画面/保存/重开，不声称点击全部Profile按钮。

recovery-final/results.json 的16项均exit0：构建、四个单测、三宿主no-op、DX12/Vulkan连续帧及覆盖拒绝、管线替换、SSGI六端点、Editor check。双后端各12组/192帧逐张审查，未确认长拖尾；8个Vulkan会话停止成功，旧03047错误为0，仍有Shader-OutputNotConsumed性能警告。四类覆盖拒绝均保留主/镜像预览旧Forward，修正后切Deferred；重复30步未刷屏。该检查不声称独立preview晚期失败或逐帧prepare计数遥测。
E1 DX12连续帧通过；E1 Vulkan出现160条 VUID-vkUpdateDescriptorSets-None-03047，促成per-record资源隔离修复，保留原失败证据。

恢复回归的精确调用如下，均为 runtime-manual PASS。`$r = 'out/agent/pipeline-upgrade/recovery-final'`，
`$rh = 'EAA5E953C6D984BBD106FA53BF553DC427B43A2E1CAFF847F442C43BC4CC9AC9'`，`$api` 分别为 `DX12`、`Vulkan`：

```powershell
Tools/Agent/Capture-PipelineTemporal.ps1 -GraphicsApi $api -OutputDirectory "$r/temporal-$api" -ExistingBuildSha256 $rh -Frames 16 -WarmupFrames 30 -CaseWarmupFrames @{distortion=170} -VfxScenario cpu20k-proxy -ParticleEffectPath Assets/Effects/Survivor/MagicTornade.efkpkg -DistortionEffectPath Assets/Effects/FireBall.efkpkg -PythonExecutable $python
Tools/Agent/Test-PipelineOverrideRejection.ps1 -GraphicsApi $api -OutputDirectory "$r/override-rejection-$api" -ExistingBuildSha256 $rh
Tools/Agent/Test-PipelineReplacement.ps1 -GraphicsApi DX12 -OutputDirectory "$r/replacement-DX12" -ExistingBuildSha256 $rh
hectl editor check -GraphicsApi DX12 -StrictGraphicsApi -Frames 120 -TimeoutSeconds 180
```

最后一项在 recovery 与最终 closure 两次均 PASS。

### Vulkan 缓存退役压力

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-VulkanVfxCacheRetirement.ps1 -RepositoryRoot C:/WorkSpace/HEngineDev -OutputDirectory C:/WorkSpace/HEngineDev/out/agent/pipeline-upgrade/cache-retirement-accepted -ExistingBuildSha256 204B047873104B2617336D64B120344EF6F70014D0D3B467953E88406D410605 -PythonExecutable C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe
```

runtime-manual PASS：最终204B版，Vulkan验证开启，主Forward/预览Deferred，两者FXAA、GI关闭。
24次640×360/1280×720交替resize并销毁重建预览，每次8个真实连续帧；两视图实际包含VFXTransparent及VFXBackgroundCopy且存在绘制和相对无效果基线的图像信号。
实际141条回收记录：native framebuffer与external texture缓存Owner各删除141项；中间8次与最后8次两类remaining最大值均5，未继续增长。
活跃capture取消后16个连续恢复帧完成，两次owned会话停止；日志无VUID、Validation Error、DeviceLost或被检查的存活对象错误。
顺序查看6张联系表、84个图像位置（有端点/序列重复）：全部24轮主/预览端点、基线、首末及取消恢复帧，无观察到的突发缺失或损坏。

临时测试模块从原VFXStressTest复制并归档hash，MagicTornade速度0.1保持可见采样窗口，FireBall速度0.01保持背景快照消费者；原脚本/资产不改动，finally移除本轮精确临时文件。
只证明缓存Owner退役和两尺寸工作集稳定，不证明全部LLGI引用、物理VRAM、正常速度画质、软深度包装器或独立折射精度。
早期运行保留FAIL：预览截图文件名复用、原效果发射空窗、临时类名引用、Windows PowerShell5字典聚合错误分别已定位并修正测试；其中cache-retirement-verified实际完成24轮但汇总异常，不将其原始failed结果改写。
最终脚本对生成模块执行启动前辅助函数检查，Windows PowerShell5统计正例及合成增长负例也已验证。

## 性能：同版受控 A/B

`Tools/Agent/Benchmark-PipelineChanges.ps1 -OutputDirectory out/agent/pipeline-upgrade/final-evidence-e1ea/performance-ab -WarmupFrames 600 -MeasureFrames 1800 -Repeats 3`：PASS。
`python Tools/Agent/Analyze-PipelineChanges.py out/agent/pipeline-upgrade/final-evidence-e1ea/performance-ab`：PASS。

i7-11800H、RTX3060 Laptop 6GiB、驱动591.74、Balanced电源；DX12/1920×1080/TAA和AO开启，
GI/SSR/探针关闭，GPU batch/阴影/纹理复用开启，VSync/RenderValidation关闭，固定步长1/60秒。
每项三次，第二轮反转顺序；18次均600预热、1800稳定CPU帧、1796个跨过GPU预热界限的唯一完成样本。
二进制hash相同、每对场景文件hash相同；场景hash不代表整个资源依赖归档。

| 对照 | 前→后（重复均值） | 结论 |
| --- | --- | --- |
| Sponza完整→紧凑Forward guides | GPU 10.4211→10.2935ms | 约1.22%较低；3个配对均降低但重复范围重叠，仅本机小幅趋势 |
| LocalLights完整→紧凑guides | GPU 2.0596→2.0554ms | 各轮方向不一致，无稳定整帧提速结论 |
| 原始参数打包缓存off→on | Pack约0.0180ms→CSV精度下0；调用3→0、命中0→3 | 减少重复打包；CPU整帧2.3123→2.3167ms、GPU2.2695→2.2766ms，不宣称FPS提升 |
| 紧凑guides名义资源 | 每像素少12B；1080p每帧少23.7305MiB，三帧保留少71.1914MiB | 格式与池计数的名义分配，不是实际VRAM或实测带宽 |

上传未下降：LocalLights60B/帧、Sponza8020B/帧、packing6796B/帧。均值/中位数/p95/p99/max在CSV及summary.json，
保留CPU40.98ms峰值。9对图MAE 0～0.000128/255；通道最大差>4的像素比例最高约8.04e-7，代表图已看一致。
静态图不替代运动质量验证；稳定采样缓存/退役/trim计数0，不据此声称测到预算回收收益。

### 执行取舍

实际1247个DX12 Pass记录均为graphics，逻辑Compute/Copy提示不是异步执行。
DX12后端确实创建DIRECT/COMPUTE/COPY queues交给NVRHI；Vulkan生产路径目前创建单graphics queue。
当前图没有生产跨队列调度，不能声称硬件不支持或已证明异步无收益。本次实际实验选择了图 Pass 合并。
LocalLights TAA约1.0ms；Sponza阴影约2.60ms、opaque1.94ms、TAA1.45ms，TAA后真实匹配深度约0.653ms。
该深度承担后合成遮挡正确性，不能仅为少一个Pass删除。完整覆盖裁剪有实现与单测，但稳定性能图均为live Pass，
没有裁剪开关A/B，不能声称其实测提速。本次默认优化集中在按需纹理、打包复用和安全缓存回收。

#### 深度金字塔合并实验

`Tools/Agent/Benchmark-PipelineDepthPyramid.ps1 -Mode Correctness -OutputDirectory out/agent/pipeline-upgrade/closure-final/depth-correctness -ExistingBuildSha256 204B047873104B2617336D64B120344EF6F70014D0D3B467953E88406D410605 -Python $python`：runtime-manual PASS。
DX12/Vulkan各运行旧11 Pass与新DepthPyramidMerged，显式开启后端与图验证，64帧端点；同后端两图PNG hash完全相同，RGB差为0，4张预览已查看。
这不等同独立验证每个GPU mip，也不把共同存在的SSGI亮斑判为画质通过。

`Tools/Agent/Benchmark-PipelineDepthPyramid.ps1 -Mode Performance -GraphicsApi DX12 -OutputDirectory out/agent/pipeline-upgrade/closure-final/depth-performance -ExistingBuildSha256 204B047873104B2617336D64B120344EF6F70014D0D3B467953E88406D410605 -Python $python`：runtime-manual PASS。
同一最终二进制，LocalLightsOnly、1920×1080、Forward/TAA、半分辨率1 ray SSGI/SVGF/2轮atrous，固定步长；关闭两种验证。
三对AB/BA，600预热+1800稳定帧；每次CPU为601～2400帧，GPU cutoff609之后1796个唯一完成样本，无重复冲突。

| 指标 | 旧11 Pass→合并1 Pass（3次均值） | 解释 |
| --- | --- | --- |
| GPU整帧 | 4.174630→4.135702ms，约低0.93% | 三对分别低0.50/1.12/1.17% |
| 金字塔GPU计时 | 0.129841→0.121040ms | 旧11段之和与新单段相比，含不同instrumentation成本 |
| CPU Graph.Execute | 0.556409→0.544492ms | 平均低2.14%，首对反而高0.16%，不能称每次都改善 |

三对PNG最大通道差均1，差>4的像素为0，六图逐张审查无新增可见差异；共同SSGI噪点仍存在。
实际仍执行11次dispatch及逐mip barrier，只减少图回调和10对计时查询，不是异步或单dispatch算法融合。
因收益尚未剥离计时观察开销、性能仅覆盖一个DX12静态场景，默认保留旧路径；开发开关保留用于复现。
这是已运行的局部收益和保守采用决策，不是声称实验失败或没有收益。

SSGI六端点均exit0且21项实际设置匹配。1 ray到512帧仍有亮斑；8 ray的128/512帧明显缓解但不完全收敛，也不单调改善。固定墙面ROI显示编码亮度std：1 ray为21.00/18.74，8 ray为9.05/9.83；含真实光照梯度，无高采样真值，不能把std全称误差。11级金字塔短运行总均值0.071～0.085ms；这些含冷启动的端点不是稳态重复benchmark。详见recovery-final/ssgi-review.md及offline-metrics.json。

## 兼容与交付边界

- 自定义GPU/LOD优化限定于声明的静态opaque ABI；masked/skinned/transparent保留既有可见回退。
- VFX本轮为CPU效果，GPU粒子不支持；可见性/连续帧不等于作者原始外观或独立折射精度证明。
- Vulkan离线明确不支持；DX12低spp完成不代表最终画质。
- 用户既有指导文件、PBRTest场景及导入/源资产保留工作区；失败采集恢复资产不提交为正式内容。

最终缓存脚本 exit0，两个受控会话均STOPPED；收尾检查无HGame/HEngineEditor进程，也无本轮缓存临时场景或Python模块。
自动Storage回收完成，详情 out/agent/pipeline-upgrade/final-cleanup.json 与 out/agent/storage/last-clean.json；受保护失败/恢复材料不强删，也不将未知历史状态改成成功。
提交范围仅本任务105个源码、脚本和文档文件；原有四处已跟踪改动及初始未跟踪资产、恢复资产、日志和截图均留在工作区，未纳入本提交。
