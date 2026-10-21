# 渲染管线补充验收

这是 `9f64aa5577e974bdbb735c06c31b27f6992be908` 的补充验收记录；本次不修改渲染算法。原阶段记录见 [Rendering-Pipeline-Upgrade.md](Rendering-Pipeline-Upgrade.md)。下述目录简称相对 `out/agent/pipeline-upgrade/supplement/`，原始失败记录保留。

配套文档：[212条实现与证据映射](Rendering-Pipeline-Requirements.md)、[版本影响与证据适用范围](Rendering-Pipeline-Version-Evidence.md)、[GPU粒子既有限制](Rendering-Pipeline-GpuParticle-Boundary.md)、[匹配深度的实际成本](Rendering-Pipeline-Depth-Cost.md)。映射80项VERIFIED、132项LIMITED；后者明确能力或采样范围，不能读成全部画质/组合通过。

## 版本与可恢复基线

| 用途 | 源码/产物身份 |
| --- | --- |
| 修改前 | `cc7526ec156876f37566231405560f29ef70fe61` 加原 `initial-tracked.patch`，独立目录 `C:/WorkSpace/HEBase-cc7526` |
| 基线 Game | SHA256 `31FD15BD9B195D2D56FD2EBD53B8DD999C138318B4183BE40DBD6C1CA059CE16` |
| 最终版本 | `9f64aa5577e974bdbb735c06c31b27f6992be908`，独立短目录 `C:/HEFinal`，两端共同读取基线目录的 Project |
| 同 Project 最终 Game | SHA256 `62D5EE53AED9875C18C0C7C34B58594A323AC4AE6D7D01D34FC27FF7FC008C35` |
| 首帧切换检查 | 默认 Project 的最终 Game，SHA256 `204B047873104B2617336D64B120344EF6F70014D0D3B467953E88406D410605` |
| 原生设置面板 | Editor SHA256 `5516726358E665AEBD4B487D4142FE9A173F762639FCABE4F468C9972FB9B965`，三次运行一致 |

初始补丁只有四个用户指导/场景文件；没有修改基线引擎实现。`baseline-asset-manifest.json` 对4564个已跟踪资源、脚本和配置逐文件记录SHA256，两端无缺失；22处原始字节差异全部为CRLF/LF，分类见 `asset-difference-classification.json`。未把原工作区未跟踪导入素材作为基线依赖静默补入。性能采集另对实际隔离Project的完整输入树前后建立清单。

build-only：基线命令 `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Parallel 2`，在基线目录执行，PASS；configure 26.4s、build 960.1s。回执为基线目录 `out/agent/logs/build-run-db60642ab1a7436ca18575f3a2cbd933/result.json`。

build-only：最终命令 `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Project C:/WorkSpace/HEBase-cc7526/Project -Parallel 2`，在 `C:/HEFinal` 执行，环境 `BuildInParallel=false`，PASS；成功增量轮耗时326.4s，不把它称为从零完整构建耗时。回执为 `out/agent/projects/starbloom-survivor-06e59e5c/logs/build-run-7da472b933fe4c938b773e93c4a68698/result.json`。两端均VS2026 v145、MSVC14.51.36231、RelWithDebInfo、开发Game。

保留的失败及修正：原长目录触发260字符对象文件路径错误；短目录后，外部GameScripts与最终Runtime对同名C#依赖的并行发布发生CS2012，串行托管构建后通过。`Engine/Managed`、`Project/Scripts/CSharp`、`Tools/Scripting` 两提交间diff为0；没有为比较修改基线源码、禁用插件或调整渲染功能。日志见 `final-isolated-project-build.log`、`final-short-build-managed-failure.log`、`final-short-build-serial-managed.log`。

## 原生设置交互

runtime-manual PASS。三次正式 `hectl editor check -Configuration RelWithDebInfo -GraphicsApi DX12 -StrictGraphicsApi -StartupScene Assets/RenderingBenchmarks/Validation/SurfaceCoverage.he -Resolution 1600x1000 -Frames 10000000 -TimeoutSeconds 1800` 受控运行均exit0，由原生窗口操作完成关闭。`settings-interactive/runs.json`、26张顺序查看的截图和 `independent-evidence-review.md` 相互对应。

| 实际操作 | 文件及运行结果 |
| --- | --- |
| 创建Profile A、选择并会话应用 | 创建独立文件；选择先进入草稿，应用后才改变当前Profile |
| 两次添加Outline、禁用、同阶段前移、移除 | 原文件3项变4项，仅保留启用的 `builtin.outline.5`；非法跨阶段排序禁用 |
| 保存A、复制B | 保存内容含实际Feature变化；复制后的Settings独立文件内容一致 |
| B重命名C、取消待应用草稿 | C保存内容，B为指向C的alias；取消草稿不撤销已完成的资产重命名 |
| 选择C、Project保存、关闭重开 | 当前会话A覆盖暂时高于Project C；新会话正确加载C |
| 阴影设置false、恢复继承 | 先以Project覆盖false，再删除该字段，Effective恢复Engine来源true |
| 坏参数Profile应用失败 | transaction_failed；旧C继续有效，两项草稿保留；刷新仍保留，取消后清除 |
| 再次重开 | C与场景正常；失败事务未写入Project配置 |

17个Profile归档hash一致。拒绝前后项目配置hash相同。测试后原BaseEngine/UserEngine配置逐字节恢复，精确临时Profile文件移除；原用户资产没有纳入清理。`16shadow-project-source` 实际未滚动到字段，不作为阴影来源证明；使用18～20图及配置文件。

## 切换后首帧

runtime-manual PASS（采集协议与限定人工观察）。`camera-cut-final/visual-review.md` 记录实际命令。DX12/Vulkan × Forward/Deferred × GI Off/SSGI，8组各8个真实连续帧。3次跨帧批次被拒绝并重试；接受组第一张严格为cut前 `completedRenderFrames+1`，后续render/simulation计数连续、submission递增。两个会话均STOP。

64帧联系表及首帧原图已查看。GI Off四组未观察到突发缺失或损坏；SSGI四组仍出现亮点，不能标画质PASS。这里在同一相机位置重置历史，未制造大幅镜头跳变，不能推导跳变后无拖影；也不代替已有移动粒子连续帧测试。

## 两版本图像与相机位姿对照

基线 `temporal-baseline-v5` 与最终 `temporal-final-v2` 均五个会话exit0，使用相同隔离Project、场景和共同设置。每版8个相机位姿、4个1/8ray×128/512延迟SSGI端点，共24张PNG。后者命令由 `Test-PipelineVersionImages.ps1` 归档，使用明确产物路径与hash；会话规范停止后保存日志。基线另补归档并核对12张源/副本PNG hash全一致。

旧协议端点不是原子连续帧，128/512是请求延迟，不能称为完全相同的accepted history龄。会话状态、实际render/simulation计数和完整设置均保留。图像差异指标不等同独立渲染真值，也不自动给出画质PASS。

runtime-manual PASS（采集与限定观察），图像结果见 `image-comparison/summary.json`、`visual-review.md`。12对的78项共同effective及5项debug实际差异均0。主代理顺序查看10张联系表42图和两组最大差异附近裁剪：GI关闭的8个位姿未观察到新增对象缺失或损坏；RGB平均绝对差0.142～1.126/255、差>4像素0.762～4.882%，不称完全相等。

SSGI 1ray在原始基线和最终均有明显亮斑，8ray均缓解但未完全消除。固定墙面编码亮度std旧→新：1ray/128为20.7429→19.9663、1ray/512为22.5816→18.5866、8ray/128为10.2563→10.2595、8ray/512为9.4902→10.6066。没有真值、历史年龄不同且包含真实光照梯度，不能把std当误差，也不能宣称每组改善。已证明亮斑现象在修改前存在；本批未见新类型大面积损坏，仍不将SSGI画质标成全面通过。

性能静态图的9对也已查看：LocalLights最大通道差1；Sponza各通道MAE约0.125～0.136/255、差>4通道约0.1538%；Surface约0.0198～0.0248/255、差>4通道0.0528%。最大差异附近的边缘/透明表面裁剪未观察到大块几何丢失。新旧端点、R连续运动与C首帧各自注明版本，互不替代。

## 两版本性能

正式命令入口 `Benchmark-PipelineVersions.ps1`，输入两端实际Build回执、产物和SHA256。三个场景为LocalLightsOnly、IntelSponzaDaylight与SurfaceCoverage；每场景三对AB/BA/AB，DX12、1920×1080、TAA/AO开启、GI/SSR/探针关闭、GPU batch/阴影/纹理复用开启、VSync/图验证关闭、固定步长1/60秒，每次600预热和1800稳定帧。

runtime-manual PASS：`performance-versions-v2` 18次均exit0，实际Build日志artifact行、运行前后产物hash和设置sidecar一致；每次严格1800个CPU帧（601～2400）、跨预热界限的1796个唯一GPU样本，无重复计时冲突。4578项完整输入清单前后相等。结果和独立复算在 `summary.json`、`independent-review.md`、`independent-audit.json`，含mean/median/p95/p99/max。

| 场景 | CPU wall旧→新 ms | GPU跨度旧→新 ms | GPU变化 |
| --- | --- | --- | --- |
| LocalLights | 2.159945→2.151304 | 2.102254→2.097743 | -0.21%，三对方向不一致，基本持平 |
| Sponza | 10.069170→10.607286 | 10.031719→10.566281 | **+5.33%**，三对均变慢 |
| SurfaceCoverage | 2.354478→2.385099 | 2.291733→2.330546 | **+1.69%**，三对均变慢 |

不能宣称本轮升级整体提速。Sponza新增 `PostTaaUnjitteredDepth` 约0.68309ms，guides减少约0.09464ms，未抵消新增成本；Surface新增深度约0.02397ms。源码及实际TransparentForwardHDR证明Sponza有后合成透明消费者，不是纯opaque误生成：匹配深度用于TAA后投影和遮挡对齐，不能直接删除作为无损优化。实现仍有进一步精化空间，正确性目的不等于当前成本最优，见 `post-taa-depth-cost.md`。

资源预留是可观察的局部收益：LocalLights和Sponza各减少74649600B，Surface减少49766400B；不是物理VRAM或带宽。Sponza上传8020B/帧、几何3747018tri不变；Surface上传6776→6796B，GPU对象8→9、batch6→7、几何55464tri不变，不能称上传减少或LOD收益。旧版没有的新列标not_available。

i7-11800H、RTX3060 Laptop、驱动591.74、平衡电源。36个前后快照温度51～73°C，负载后SM时钟1965～2002MHz；端点快照不能证明全程无温度影响。Sponza反序轮中基线更热仍更快，不以温度消除实测回退。正式测量期间没有其他构建或受控GPU测试。

18份控制日志均保留；指向的36份原始运行日志中30份可读并复核，基线首轮三场景的6份日志已被该独立仓库历史保留策略回收，不能声称这6份已查无错误。其CSV、截图、控制日志和退出码仍在。公布脚本已补每次立即归档运行日志，保留此次缺失清单；不通过重写记录抹去限制。

## 限制与版本覆盖

GPU粒子检测、启用拒绝和能力查询的四个完整代码块在cc7526与9f64间hash相同，初始化能力false与包加载拒绝在基线已存在。此为既有集成限制，不能把CPU效果可见、图形API可用或预算字段存在称为GPU粒子支持。独立审计见 `gpu-particle-baseline-limit.md` 与 `gpu-particle-baseline-diff.json`。

E1历史广覆盖、R修复重测、C缓存与深度实验和本次独立Project/Editor测试有不同产物身份。`final-version-impact-map.md` 记录可复核源码hash链及不能完整追溯的部分；不将E1所有截图重新标作最终版全量回归。Vulkan离线hardware RT仍按真实缺少AS/RayQuery报告不支持；DX12低采样完成不代表最终画质。

正式清理删除了旧Editor Debug缓存。Tests Debug冷缓存清理只删除6324784449字节，剩余退役目录遇到WinError145，结果保留FAIL；不强删未知或恢复材料，不把部分回收说成完全成功。清理不涉及用户源码或资产。

收尾 `final-state.json` 确认无遗留Game/Editor进程，原四处tracked patch完全一致，两份用户配置当前hash与测试前相同。补充提交仅含验证脚本、报告和映射；基线/最终隔离目录及原始CSV、截图、失败与恢复证据保留在磁盘。

![失败事务后重开的原生设置面板](../../out/agent/pipeline-upgrade/supplement/settings-interactive/25-reopen-after-rejection.png)

## 精确复现入口

以下为本机实际参数；重跑须选择新的OutputDirectory，脚本拒绝覆盖已有证据。原始基线与最终工作目录须先分别通过上述build-only命令。PowerShell变量只缩短路径，未省略影响渲染的选项。

```powershell
$base = 'C:/WorkSpace/HEBase-cc7526'
$final = 'C:/HEFinal'
$finalCache = "$final/out/agent/projects/starbloom-survivor-06e59e5c"
$baseExe = "$base/out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe"
$finalExe = "$finalCache/game-relwithdebinfo/bin/StarbloomSurvivor.exe"
$baseHash = '31FD15BD9B195D2D56FD2EBD53B8DD999C138318B4183BE40DBD6C1CA059CE16'
$finalHash = '62D5EE53AED9875C18C0C7C34B58594A323AC4AE6D7D01D34FC27FF7FC008C35'
$output = 'C:/WorkSpace/HEngineDev/out/agent/pipeline-upgrade/supplement'
$python = 'C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$env:BuildInParallel = 'false'
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Benchmark-PipelineVersions.ps1 -OutputDirectory "$output/performance-versions-v2" -BaselineRoot $base -FinalRoot $final -BaselineBinary $baseExe -FinalBinary $finalExe -BaselineSha256 $baseHash -FinalSha256 $finalHash -BaselineBuildResult "$base/out/agent/logs/build-run-db60642ab1a7436ca18575f3a2cbd933/result.json" -FinalBuildResult "$finalCache/logs/build-run-7da472b933fe4c938b773e93c4a68698/result.json" -Python $python
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-PipelineVersionImages.ps1 -Side final -FinalRoot $final -ProjectRoot "$base/Project" -OutputDirectory "$output/temporal-final-v2" -FinalArtifact $finalExe -FinalSha256 $finalHash
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-PipelineCameraCutSequence.ps1 -RepositoryRoot C:/WorkSpace/HEngineDev -IncludeSsgi -ExistingBuildSha256 204B047873104B2617336D64B120344EF6F70014D0D3B467953E88406D410605 -OutputDirectory "$output/camera-cut-final"
& $python Tools/Agent/Compare-PipelineVersionImages.py --baseline "$output/temporal-baseline-v5" --final "$output/temporal-final-v2" --output "$output/image-comparison"
```

基线图像用同一图像入口的 `-Side baseline -BaselineRoot $base -BaselineArtifact $baseExe -BaselineSha256 $baseHash`；实际历史成功运行是out目录草稿v5，最终公布脚本保留相同采样逻辑并修正日志归档。首帧历史成功运行也使用out草稿，正式脚本只调整可复用默认路径和必填hash，不将未重跑的新命令说成额外一轮GPU检查。
