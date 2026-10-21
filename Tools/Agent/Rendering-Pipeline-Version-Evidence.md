> 原始证据目录：`out/agent/pipeline-upgrade/supplement/`；其他E1/R/C目录相对`out/agent/pipeline-upgrade/`。

# 最终版本影响与证据适用范围

本文件是只读差异审计，不是新运行验收。最终提交为 `9f64aa5577e974bdbb735c06c31b27f6992be908`。本审计桥接的是9f64渲染源码；补充提交只增加验证工具与文档。git status列出的4个原用户文件和原未跟踪资源不属于本次版本桥接。没有重新构建或运行GPU。

## 版本身份

| 简称 | 可核实身份 | 证据目录与范围 |
|---|---|---|
| E1 | Game SHA256 `E1EA679FAA6A52F3FBA3797ECF7139CB8D0A863261A8B3A3B03ED94A76A88AB3` | final-evidence-e1ea；路径/功能/云/离线/材质LOD等广覆盖，18次开关性能对照。不是最终二进制。 |
| R | Game SHA256 `EAA5E953C6D984BBD106FA53BF553DC427B43A2E1CAFF847F442C43BC4CC9AC9` | recovery-final/game-sha256.txt；VFX宿主录制隔离及自动配置拒绝恢复修复后的双后端短序列、override-rejection、replacement、构建/单测。 |
| C | Game SHA256 `204B047873104B2617336D64B120344EF6F70014D0D3B467953E88406D410605` | closure-final/game-sha256.txt；Vulkan缓存退役与默认关闭的深度Pass合并实验之后的构建、Graph/Effekseer、codegen no-op、深度正确性/性能；cache-retirement-accepted后续成功重跑同hash。 |
| UI补充 | Editor SHA256 `5516726358E665AEBD4B487D4142FE9A173F762639FCABE4F468C9972FB9B965` | supplement/settings-interactive/editor-before.json及runs.json三次一致。9f64提交后的真实原生交互补验，26图；不能拿Game hash替代此Editor hash。 |
| V：独立起点→最终对照 | baseline Game SHA256 `31FD15BD9B195D2D56FD2EBD53B8DD999C138318B4183BE40DBD6C1CA059CE16`；final Game SHA256 `62D5EE53AED9875C18C0C7C34B58594A323AC4AE6D7D01D34FC27FF7FC008C35` | supplement/performance-versions-v2，baseline为cc7526+初始patch，final为9f64独立构建，两者使用同一个隔离Project；另有temporal-baseline-v5/temporal-final-v2端点对照。此行两个SHA256均为完整32字节摘要（64位十六进制），不是C二进制别名。 |

E1/R/C没有分别形成可直接 `git diff E1..R..C` 的提交：本次提交将演进一起收录。二进制SHA只能证明运行版本身份，不能自行证明完整源树一致。以下采用已保存staging原副本、patch、发布hash和后续重测来桥接；没有完整旧源树证据的部分明确保留限制。

V来源收据在 `supplement/performance-versions-v2/`：`baseline-head.txt`=`cc7526ec156876f37566231405560f29ef70fe61`，`baseline-tracked.patch`保存起点初始修改；`final-head.txt`=`9f64aa5577e974bdbb735c06c31b27f6992be908`，`final-tracked.patch`为空。`baseline-build-result.json`与`final-build-result.json`均state=success、HGame/RelWithDebInfo、Project=`C:/WorkSpace/HEBase-cc7526/Project`。实际产物分别在 `C:/WorkSpace/HEBase-cc7526/out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe` 和 `C:/HEFinal/out/agent/projects/starbloom-survivor-06e59e5c/game-relwithdebinfo/bin/StarbloomSurvivor.exe`；18个运行artifact行与每次前后hash匹配。编译器/CMake收据同目录。这里的62D5…是final可执行文件hash，**不是Project内容hash**；输入一致性由4578项inputs-before/after清单证明。

新增V验收：18次exit0，4578项输入前后稳定，每次1800个连续CPU样本与1796个唯一有效GPU样本。`performance-versions-v2/independent-review.md`与`independent-audit.json`确认Sponza GPU均值10.031719→10.566281ms（+5.33%真实回退），SurfaceCoverage约+1.69%，LocalLights约-0.21%且重复范围重叠。因此起点性能基线**现已建立**，不能继续写“未获得基线”，也不能以E1局部开关收益掩盖版本整体成本。

新增图像对照：`temporal-baseline-v5`与`temporal-final-v2`共24端点、10会话exit0；root在 `image-comparison/visual-review.md`记录顺序检查10张联系表中的42个图像位置及2组放大差异裁剪。12个时间端点配对共同78项effective/5项debug无差异；这批是充分延迟的位姿/SSGI端点，实际renderFrame与历史年龄不同，不是原子连续GPU帧。旧版1ray SSGI已有亮点，8ray在两版均缓解但未完全消除，不能标完整SSGI质量PASS。C/R既有连续帧与缓存矩阵仍引用各自hash，不反标为62D新全集。

## 可核实差异链

| 演进 | 代码边界与本次复核 | 影响与重测 |
|---|---|---|
| E1→R：VFX录制资源 | vfx-retirement-staging/{original,modified}/、original-hashes.json、vfx-retirement.patch、published-hashes.json保留9旧文件+2新helper。DX12/Vulkan各次record独立adapter、常量/描述符和动态VB，按host-slot提交fence复用；RuntimeRenderer三个Begin/Notify/Abort hooks；失败不再Shutdown销毁在途资源。 | E1 Vulkan特效序列曾真实失败，不能沿用为修复后PASS。R temporal-DX12/temporal-Vulkan重新执行，各visual-review.md记录真实连续帧。原图像可保留作定位历史，不能当最终像素基线。 |
| E1→R：自动覆盖拒绝 | override-rejection-staging/changes.patch与publication.json。本次逐文件SHA256核对，5项发布内容均与当前文件完全相等：RuntimeRenderer.cpp、RejectedViewConfigurationInputs.h、Renderer/CMakeLists.txt、RenderSettingsTests.cpp、Test-PipelineOverrideRejection.ps1。 | 专用数据拒绝异常、自动Apply失败输入缓存、保留所有旧视图、显式设置事务仍失败；影响正常与异常渲染入口。R override-rejection-DX12/Vulkan均有4类拒绝/恢复证据和独立审查；只能证明其main+镜像preview模式，不能扩充为独立preview世界输入失败测试。 |
| R→C：Vulkan外部缓存 | Vulkan缓存original-hashes.json的 `78C00C30...BD6F5B5` 与R VFX发布清单同文件hash一致，证明这段patch起点可连接。cache-retirement.patch只改VulkanEffekseerInterop.cpp：旧Framebuffers/ExternalTextures Owner按generation+completed fence回收，环境门控trace。当前文件相对modified副本仅多初始化 `bLogCacheRetirement=false` 一行。 | 影响Vulkan VFX，包括resize/视图重建后的资源寿命；E1/R不能证明该修复。C cache-retirement-accepted用同hash完成24轮主Forward/previewDeferred尺寸切换、背景snapshot、两类Owner退役/plateau、真实取消与16帧恢复。Magic慢速可见夹具+FireBall慢速background探针，不覆盖soft-depth包装器，也不证明VRAM总量或LLGI引用归零。 |
| R→C：深度金字塔合并实验 | 最终源码DepthPyramid相关实现与 `Benchmark-PipelineDepthPyramid.ps1`，checkpoint记录为后续默认0开发开关。未在本审计目录找到与E1完整源树对应的独立深度patch/hash基线，不能宣称完整逐行版本桥接。 | closure-final/depth-correctness 双后端同版开关图像/11→1 pass；depth-performance六次同版对照及independent-review.md。这是C同版A/B，不能替代起点→最终收益；也不把较少timestamp观察成本归为纯GPU算法收益。 |
| R后文档注释 | VFX published-hashes中11项，本次8项与当前原始bytes相同；3项不同分别为RuntimeRenderer（被后续设置发布覆盖，已由设置发布hash桥接）、VulkanInterop（上述缓存patch）、EffekseerRuntime.h（只增加4行host-frame/GPU idle注释，已diff确认）。 | 可以具体解释这份VFX清单的后续差异；不能据此推导整个引擎E1到最终都只有这几处改动。 |
| UI最终补验 | 当前ProjectSettingsPanel在9f64中；补充Driver实际使用Editor551672，启动前hash与三次运行hash一致，期间没有UI/C++改动。本次资产、配置、恢复文件独立核实见 supplement/settings-interactive/independent-evidence-review.md。 | 26原生图由root逐张查看，真实覆盖Profile创建/选择应用/保存复制/重命名alias与草稿取消、Feature同阶段增删排序、Project保存重开、恢复继承、坏参数Apply失败保留/刷新/取消。补齐旧11图仅阴影设置的范围，不能把新26图反标为E1测试。 |

## E1广覆盖如何用于最终报告

| E1既有证据 | 最终适用口径 |
|---|---|
| 18次材质打包/Forward guides开关性能 | **仅E1同版受控结果**。R/C改动涉及录制入口和资源管理，不能把E1 CPU/GPU时间写成C最终数值。新增V起点→最终对照已建立，Sponza约+5.33%回退须与局部机制收益同时报告；不能将E1开关收益冒充跨版本收益。 |
| 路径、材质/LOD、Feature、云广覆盖截图 | 保留为当时实现的行为证据。若宣称最终对应行为，需同时列出后续相关代码边界与针对性重测，不可总括“最终完整双后端回归”。C缓存样例仅主Forward+previewDeferred/FXAA/GI Off，不覆盖所有AA/GI/材质分支。 |
| DX12离线provider完成8spp | 证明E1当时真实provider活跃和收敛；最终没有在本图谱建立同配置离线重跑的完整证据，不写C离线重新验收。Vulkan离线缺能力仍为UNSUPPORTED，不能因最终构建成功改成PASS。 |
| DX12特效短序列 | 后续VFX资源修复改变执行路径，以R双后端短序列重测为主要依据；C再补Vulkan缓存压力。FireBall可见不等于独立折射on/off真值，soft-depth质量不由背景snapshot覆盖。 |
| 老设置面板布局/11张原生阴影图 | 保留旧Editor版本范围；新增26图由明确551672 Editor承担扩展流程。仅设置层概览不能证明World/Camera最后有效图，更不能证明拒绝前后像素完全一致。 |
| SSGI路径与亮斑 | 新V端点对照确认旧版1ray已存在亮点、8ray两版均缓解但未完全消除；足以确认亮点现象早于本轮，但不证明每组改善或完整非回归。R/C运行路径、V端点图像和未解决质量限制分别保留，不升级为质量PASS。 |

## 可追溯限制与交付判据

1. 9f64是最终提交身份；E1/R/C是不同时间的二进制。当前生产文件在提交中不等于已保存一份完整E1源树manifest。本图谱仅对上述保存过原副本/发布hash的文件作精确相等断言，其他模块的“未受影响”属于范围推断，不能取代重跑。
2. closure-final原总控包含一次cache脚本失败，不能将总exit1改写成全绿；缓存最终引用cache-retirement-accepted，前面filename、fixture类名/效龄和PS5聚合失败目录继续保留。PS5聚合正反例见 vulkan-cache-retirement/Test-ArchivedCacheAssertions.ps1及archived-cache-assertions.log。
3. 补充UI证据独立核验：17个Profile manifest hash匹配；A 3→4仅保留builtin.outline.5；A/B/C Settings一致；B alias到C；项目C重开保持；shadow false→字段不存在；拒绝前后Project hash不变；两配置原bytes恢复hash一致。原生图像本审计者未重复查看，引用root已目视结果。
4. 最终报告应分列“E1历史广覆盖”“R针对修复重测”“C新增路径/缓存重测”“UI551672原生交互补验”“V31FD15→62D5EE独立版本性能/端点对照”，并逐条给精确目录与限制。没有证据支持“E1=最终”或“62D最终全部旧矩阵重新执行”。
