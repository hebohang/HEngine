# 脚本系统当前状态

2026-09-21 工作流升级的当前实现、唯一扩展生命周期契约、程序集重载/Play 状态机、模块 v1、字段 schema 2 和本轮验证，见
[C# 工作流升级](ScriptingWorkflowUpgrade.md)。本机实现和验证结果、支持边界及纯净 VM/手机 NOT RUN 项均在该文档中；以下 2026-09-11 记录是原有平台实现的历史证据，不能替代本轮测试。

2026-09-11。整体目标仍未完成；以下只列已有代码及实际验证，不用启动检查替代交互或真机验收。
架构和兼容约定见 [实施文档](ScriptingImplementation.md)，早期审计见 [Scripting.md](Scripting.md)。

## 运行时与边界

| 目标 | C# | Python |
| --- | --- | --- |
| Windows | SDK 10.0.302、CoreCLR 10.0.10、hostfxr；正式包 app-local 运行时 | 保留仓库 CPython 3.11.1；插件私有 pybind/解释器 |
| Web | Mono WebAssembly 10.0.10；独立 Wasm 内存桥接真实 C++/Sokol 引擎 | 不支持，导出拒绝 |
| 微信 | 同一 Mono/玩法源码；WXWebAssembly 宿主、静态注册 | 不支持，导出拒绝 |

Wasm 当前单线程、关闭 SIMD 和三项 Jiterpreter 动态加速；没有将桌面 Native AOT 当成 Mono AOT。
公共 ScriptCore/ScriptComponent/ScriptSystem 不包含语言运行时对象或头文件。字段使用稳定ID、类型化值与实体引用。
生成器负责显式类型/字段访问器、资源依赖和可移植 API 诊断；Web 导出拒绝未审计或不兼容的可达脚本。
PortableApiAudit 是保守支持集合，不是对全部 CLR API 的跨平台保证；新增API仍需要审核、测试和必要的宿主实现。
审计契约现为 v2：覆盖参与实例的 object 虚调用及 BCL 触发的接口回调，外部支持类型校验实际程序集身份。
实际编译夹具证明旧检查遗漏 ToString/Array.Sort 回调中的文件读取；新版同时拒绝不兼容调用、缺失审计和旧版审计产物。

## 可复现入口

从仓库根目录运行，默认配置见 Project/Config/Plugins.json；不要为了矩阵测试修改当前项目配置。

```powershell
.\hectl.bat scripting build-wechat   # 包含 Web 构建和微信导出
.\hectl.bat scripting serve-web      # http://127.0.0.1:8768/
.\hectl.bat package -GameConfig Config/Game.ScriptSample.json
pwsh -NoProfile -File Tools/Scripting/TestShippingPluginMatrix.ps1 -Profile all
pwsh -NoProfile -File Tools/Scripting/TestCookScriptExport.ps1
pwsh -NoProfile -File Tools/Scripting/TestRegistryDependencies.ps1
```

正式发布可传 -PluginConfig 配置文件。发布输出按配置字节身份隔离，并保存构建配置快照；下一次无参数构建明确清空覆盖。
矩阵分别使用 Game.ScriptNative/Python/CSharp/Sample.json 与既有四个样例场景。
新增玩法放在 Project/Scripts/CSharp，使用 ScriptType/ScriptField 的稳定ID；动态选用资源可声明 ScriptDependency。
字段改名使用 FormerIds，不持久化任意托管对象图。程序集由正常构建生成，不能复制旧DLL替代构建。

编辑器选中 C# ScriptComponent 后可使用 **Compile scripts**，支持取消并显示真实编译器诊断。
编译产物暂存到独立目录，当前实例继续使用已加载代码；保存场景并关闭编辑器，经正常构建入口重建后再启动以加载新代码。
命令行同一入口：`powershell -NoProfile -File Tools/Scripting/CompileEditor.ps1 -Project C:/WorkSpace/HEngineDev/Project`。
完整编译日志路径显示在诊断结果中；实际按钮点击验收仍待补齐。

## 已验证

| 范围/证据类型 | 结果与证据目录（均位于 out/agent/scripting） |
| --- | --- |
| PC shipping：均关闭 | PASS，shipping-matrix-20260911-151643-450；2个app-local PE、0个IL程序集，原生玩法启动 |
| PC shipping：仅Python | PASS，shipping-matrix-20260911-152506-410；4个app-local PE、0个IL程序集 |
| PC shipping：仅C# | PASS，shipping-matrix-20260911-153756-814 |
| PC shipping：同时启用 | PASS，shipping-matrix-20260911-154609-051；观察Python读取C#事件，均初始化/销毁 |
| Web headed交互 | 14项PASS，web-evidence-api-v2；clicked.png 已视觉核验，真实纹理画面、输入、计分、保存恢复、GC、异常与场景重进 |
| 微信开发者工具 | 13项PASS，wechat-game-evidence-api-v2；touched.jpg 已视觉核验，刷新和关闭均由维护入口执行 |
| 字段/依赖导出 unit-test | 11项真实cooker检查PASS，cook-script-20260911-151059-852 |
| 可移植API unit-test | web-managed-stage/dependency-contract-20260911-160913-186：间接调用、接口/object/BCL回调、不相关类型隔离及3类实际导出拒绝PASS |
| 编辑器 build-only/有界启动 | editor-selection-preservation-check.log PASS；修复重选当前后端/类型清空字段 |
| 编辑器字段兼容 build-only/有界启动 | editor-field-migration-check.log PASS；枚举类型身份不匹配、改名字段类型变化在 Inspector 保留并提示，需显式 Use Default |
| 编辑器编译链路 unit-test | editor-compiler-contract-20260911-163916-059：关闭构建服务器复用后，真实 PlatformProcess → PowerShell → BuildManaged 成功及隔离语法错误两条路径 PASS |
| 编辑器编译 UI build-only/有界启动 | editor-compiler-complete-check.log PASS；最终面板及进程实现编译、ScriptCoexist 30帧运行 exit=0 |
| 非 Editor PC build-only/有界启动 | compiler-interface-game-check.log PASS；HGame 92.2秒构建，ScriptCoexist 30帧运行 exit=0，Python观察到真实C#事件 |
| 编译进程退出 unit-test / 有界运行 | process-tree-final-tests.log PASS：持续输出、多级子进程及启动前取消；compiler-process-reap-editor-check.log 与 compiler-process-reap-game-check.log 均构建/运行 PASS |
| 原生 Web 基线 build-only / headed交互 | native-web-baseline-20260911-164314-398 构建导出 PASS；native-web-validation-20260911-164505-349 的8项检查和 initial/moved.png 视觉核验 PASS |
| 微信开发者工具性能 runtime-manual | wechat-performance-20260911-165353-294：3阶段各300个真实引擎帧、20次真实GC、运行包身份校验 PASS；非手机性能 |
| PC 脚本性能 runtime-manual | pc-profile-20260911-171423-674：Tracy真实600次C#更新和帧内区段 PASS；普通关闭Tracy构建/运行另见profile-disabled-game-check.log PASS |
| 关闭Tracy的Web/微信 build-only | profile-disabled-portable-build.log PASS；导出buildId及managedIdentity与既有v2交互通过包一致 |

shipping 表中的 PASS 包含构建、cook、发布策略、链接与运行时闭包、Unicode目录真实有界启动。
它不意味着已完成 shipping 键鼠交互或编辑器 Inspector 点击验收。每个 summary.json 包含准确 exe 路径及SHA256。
微信打开 lite 窗口后曾黑屏且 automator 超时；显式 simulator_refresh 后通过。测试入口现请求刷新后再等待真实首帧。
v2 正向构建：`hectl.bat scripting build-wechat` build-only PASS（portable-v2-platform-build.log）。
Web buildId 为 9a26ab4626ed31caa876c423d8d5c25a548704aa8f364b213f5d48764f7c7fd3；微信为 a6d5d5b404295a6e665f6f7b6ab599bb9adc058408cf98c12e135999cdd23278。
两者玩法源身份 36ce8817ea469ab1267d395d4ed300057a9ad7b2824722e5b6d0223309f21e75、managedIdentity 386db5c9777e65ed990ac0a037d2553a7e3a11b131d96a854b5048445ff873c1 相同。
本次交互运行首帧 Web 151.4ms、微信开发者工具1427ms；不是手机或隔离性能基准。下列专项性能数据保留其原始构建口径。

## 当前测量

原始文件字节，不是下载压缩包或NTFS分配。shipping-sizes-all-profiles.json 校验了各自可执行文件和内容身份。

| PC配置 | 发布字节 | 相对原生增量 |
| --- | ---: | ---: |
| 原生 | 44,219,459 | 0 |
| Python | 51,903,096 | 7,683,637 |
| C# | 124,721,008 | 80,501,549 |
| 同时启用 | 132,367,343 | 88,147,884 |

原生/C#场景的 cooked StoredSize 总和差99字节，比较不是完全相同场景文件。C#目录本身80,435,965字节。
PC进程内存约50ms采样（shipping-memory）：PrivateBytes采样最大值原生329,994,240、Python335,495,168、
C#334,938,112、同时启用341,913,600字节；包含DX12 WARP渲染器，不是单独脚本堆或GPU内存。
不同启动缓存状态和采样窗口不适合据此推导稳定内存增量。

PC开发版Tracy：`powershell -NoProfile -File Tools/Scripting/ProfileScripts.ps1`。
600帧的C# Update平均7.859μs/P99 21.701μs；脚本系统含快照平均124.803μs，其中快照平均113.124μs。
4800次托管分派平均3.583μs，1800次引擎API区段平均1.549μs；这些是嵌套墙钟区段，不能相加。
包含Tracy开销及受控D输入，不是shipping键鼠交互或GC事件追踪。原始trace、CSV及身份记录位于上述pc-profile证据目录。
这组数字是逐字段读取的优化前基线；批量快照优化及兼容性回归已完成，见下方后续修订记录。

web-performance-20260911-155350-425 在无编译进程时采集，每阶段600帧：
启用脚本的引擎调用CPU平均1.365ms/P99 2.8ms；桥接1803次，平均14.4μs。
引擎调用包含同步和渲染提交，未单独隔离全部托管Update成本；禁用阶段不是C++玩法基线。
20次强制GC为0.4–1.6ms，随后帧间隔最大16.9ms；引擎和Mono线性内存分别32MiB。
该次首真实帧369.7ms；此前不同运行曾为155.7ms。后台构建时出现过85ms帧调用，预热后采集未复现，根因尚未完全定位。
复测：启动 serve-web 后运行 Tools/Scripting/MeasureWebGame.ps1，结束后关闭HTTP服务。

`hectl.bat wechat export` build-only/原生场景启动/导出 PASS（native-portable-baseline-build.log）。
原生入口显式关闭 HE_BUILD_WEB 与 HE_SCRIPT_CSHARP_WASM，缓存值已核验为 OFF。
`node Tools/Scripting/MeasurePortableSizes.cjs` verified-export-size PASS，portable-sizes.json：
微信原生声明文件1,741,785字节、C# 9,704,059字节，差7,962,274字节；C# runtime目录7,335,961字节。
Web C#声明文件9,575,862字节。每个文件均校验大小和SHA256，原生引擎还匹配最新构建receipt。
口径排除manifest自身、未声明的本地文件及微信配置排除的浏览器诊断文件；不是压缩上传包。
原生TouchSample与C#场景、玩法和宿主有差异，差值包含这些差异。

原生 Web 基线复测：`powershell -NoProfile -File Tools/Scripting/BuildNativeWebBaseline.ps1`，
然后 `powershell -NoProfile -File Tools/Scripting/TestNativeWebBaseline.ps1 -PackageDirectory <输出的web目录>`。
独立HTTP入口：`node Tools/Scripting/serve-web.cjs <输出的web目录>`，访问 `http://127.0.0.1:8768/browser-test.html`。
导出校验实际编译输入及逐文件哈希，移除微信专属启动配置，保留原C++ TouchSample浏览器适配器。
native-web-validation-20260911-164505-349/sizes.json：原生Web 1,744,775字节，C# Web 9,575,862字节，差7,831,087字节。
排除manifest自身，未压缩；这是相对原始样例的整包差值，包含玩法、场景和宿主变化，不是纯CLR增量。
基线 buildId 03d882bc9bbd5927aee5f23041a0bcc7674a6eb014b4b53bbf603c9949f343da。

微信开发者工具专项采集：`powershell -NoProfile -File Tools/Scripting/MeasureWeChatGame.ps1 -Cli <wechatide.cmd路径>`；
`node Tools/Scripting/SummarizeWeChatPerformance.cjs <result.json>` 输出统计。
wechat-performance-20260911-165353-294/summary.json：首个已提交并等待GPU完成的游戏帧1497ms；宿主首批300帧CPU平均1.74ms/P99 4ms/最大10ms。
该CPU数组是宿主预热30帧后的首300个样本，不分别覆盖全部阶段，不含后续帧的GPU完成时间。
启用阶段300引擎帧、900次托管→引擎桥接合计14.4ms，平均16μs；不包含全部原生→托管字段读取和回调成本。
引擎与Mono线性内存各32MiB；不是进程RSS或手机峰值。20次强制GC平均1ms、最大3ms（Date.now毫秒精度）。
GC阶段观察回调间隔最大23ms；19个已记录的GC后回调间隔最大21ms，最后一次GC后无后续样本。
观察RAF并非与引擎帧一一对应，包含观测开销；禁用阶段不是原生玩法基线。
首次传递多行探针失败、第二次错误假定RAF与引擎帧相等的失败记录均保留，最终改为按引擎帧计数划分阶段。

## 最新批量快照回归

C#已使用可选IScriptFieldSnapshot接口一次返回所有声明字段，公共层校验字段集合与类型，Python保持原getter路径。
ScriptCore/CSharpRuntime/PythonRuntime的RelWithDebInfo无窗口测试全部PASS；独立公共层契约构建运行PASS，导入表无Python/.NET。
Web/微信构建PASS，有窗口浏览器14项和微信开发者工具13项交互PASS，代表游戏画面均已查看。
精确命令与失败重试记录见实施文档“批量字段快照”；Web一键回归入口为 `powershell -NoProfile -File Tools/Scripting/TestWebGame.ps1`。
最新Web buildId e4299a5abaa32f33b19f3454ddcd2b72df7bdb380f2b2469d301ae4719eb7d20，微信e02dc59c3fc3d1bee421ca568a6839113c8068e322eb3da06a49256885411f01。
共同托管身份7c833ea9e24f1fa3ffbaf43d195cc2f3b94b100450fef9c99d93efe904a3491d，玩法源码身份未改变。
最新未压缩声明文件：Web9,581,884字节，微信9,710,096字节；旧首帧/内存、shipping矩阵和编辑器构建证据不自动升级到此次产物。
PC每600帧Dispatch由4800降至1200，快照均值113.124μs降至两次测量74.319/69.119μs。
P99基线179.391μs，新版267.638/123.475μs，不能宣称普遍改善尾部延迟；batch-snapshot-comparison.json保留完整比较。

Editor接口审计另发现并修复WITH_EDITOR导致的IScriptBackend虚表错位：编译能力已拆为IScriptCompiler。
混合宏编译契约修复前实际FAIL、修复后PASS，覆盖生命周期及快照接口；该契约不替代UI操作。
双插件正式包最新复验PASS：shipping-matrix-20260911-175501-950，目录dist/projects/264dca7dd50ca1c9/StarbloomSurvivor。
包括cook、依赖闭包、真实有界启动及C#/Python协作；完整交互未验证。
该包132,370,415字节，相对纯C++旧基线增加88,150,956字节（未压缩，包含场景差异）。

Editor最终冷构建580.3秒及增量构建17.0秒均PASS，ScriptCoexist场景各30帧exit0；没有UI点击证据。
Editor专属接口修复后重建Web/微信，产物身份完全未变，本轮14/13项实际交互证据继续有效。

## 性能样本与设备预览（结构化诊断修订前的固定身份）

批量快照修订Web无时间线性能采集PASS：每阶段600帧，启用阶段引擎调用平均1.257ms/P99 2.50ms；20次显式GC最大1.50ms。
独立诊断trace无事件丢失，22次V8顶层GC和20次显式托管GC标记；未发现所采集主线程完整事件超过50ms。
不能用V8事件归因自然Mono GC，历史并行构建期间的85ms记录已从稳定比较排除。
对应微信开发工具性能采集PASS：首次完成帧2856ms，首批300帧CPU平均1.513ms，20次GC最大2ms。
上述均为桌面工具数据；两个Wasm线性内存各32MiB，不是手机RSS。

当前包已实际生成[真机预览二维码](../out/agent/scripting/wechat-preview-20260911-183111-433/preview.png)。
工具报告主包2,298,072字节、runtime分包7,340,057字节，总9,638,129字节；不宣称这是压缩网络传输量。
预览准备PASS，真机运行NOT_TESTED。[设备验收步骤](ScriptingDeviceValidation.md)包含身份、输入/存档及可用真机调试时的生命周期检查。

## 尚未通过的验收

- 公共装载与结构化诊断契约已补齐并通过真实语言回归，详见下节；完整需求终审仍需与未验证的交互、设备项目分开记录。
- 编辑器真实添加/选择/字段编辑、保存重开与Play/Stop/再次Play：computer-use在执行前报kernel assets路径不存在；有界启动不替代它。
- 编辑器 Compile scripts 按钮、取消及诊断区域的实际点击验收：实现和真实编译进程契约已完成，尚未取得按钮操作证据。
- 全部PC shipping交互项目、PC专门的首帧及GC事件归因；Editor实际Play/Stop、编译按钮和保存重开操作（构建和有界启动已复验）。
- 真实设备峰值内存及手机性能；现有Web包体比较并非完全相同场景和宿主的受控比较。已提供包体与工具上传大小，未测量压缩网络流量。
- Android/iOS微信真机：尚无设备操作与运行证据；没有正式发布版本。
- 可移植API契约的进一步边界审查、旧并行构建期间85ms记录的根因及完整需求逐项终审。

依赖版本、更新方式、生命周期、API扩展与已知兼容限制见实施文档；不能把本页的局部PASS解释为全部目标完成。

## 结构化诊断修订验证

公共层新增RuntimeLoading枚举与拥有UTF-8文本的ScriptException，PC C#声明BackendHosted，Web/微信声明HostProvided，Python声明LinkedInterpreter。
C#共用显式JSON错误编码；Python复用pybind11原生真实栈，不引入精简分发中不存在的traceback.py。

| 类型与精确入口 | 结果及证据（out/agent/scripting） |
| --- | --- |
| unit-test：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Run-Test.ps1 -Name ScriptCore -Configuration RelWithDebInfo` | PASS，structured-error-core-final.log |
| unit-test：同上，`-Name CSharpRuntime` | PASS，structured-error-csharp-final.log；真实CoreCLR栈、故障仅报告一次及Python共存 |
| unit-test：同上，`-Name PythonRuntime` | PASS，structured-error-python-final.log；真实文件/行号/Update栈、旧资产和自删除 |
| unit-test：`hectl.bat scripting probe-pc` 及 `out/agent/scripting/probe-pc-native/HEngineEditorScriptCoreContracts.exe` | PASS，structured-error-probe-final.log；公共层导入表仅Windows/VC，structured-error-core-imports.log |
| build-only/有界运行：`hectl.bat game check -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30` | PASS，structured-error-game-final.log；最终Python修复后41.2秒构建、exit0 |
| build-only/有界运行：`hectl.bat editor check -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30` | PASS，structured-error-editor-check.log；123.8秒构建、exit0，位于最后Python格式化修复之前，无UI点击证据 |
| build-only：`hectl.bat scripting build-web` | PASS，structured-error-web-build.log |
| runtime-manual：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestWebGame.ps1 -EvidenceDirectory out/agent/scripting/web-evidence-structured-error-retry` | PASS，15项；visual-review.json记录已复核棋盘画面 |
| build-only：`node Tools/Scripting/export-wechat.cjs` | PASS，structured-error-wechat-export.json；复用已验证Web二进制 |
| runtime-manual：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestWeChatGame.ps1 -Cli 'E:/Program Files (x86)/Tencent/微信web开发者工具/wechatide.cmd' -EvidenceDirectory out/agent/scripting/wechat-game-evidence-structured-error` | PASS，13项；visual-review.json记录青色棋盘与偶数计数38一致，窗口关闭回执success |

首次Web回归因按异常名称统计多行日志而失败，已改为统计诊断记录头并独立检查玩法栈；失败证据web-evidence-structured-error保留。
Python首次及诊断运行失败证明分发中没有traceback模块；最终回归使用真实pybind11栈，未放宽对文件/行号/函数与单次报告的要求。
当前Web buildId=56ca099fa66c718db7eeac4e3807359ec686dff99b96bd0db547ef17badc7472，微信=12048c36e96370fc2547982d06f0d139d898ad8f21a8d33dddcb068a13944537。
玩法身份未变；完整身份与字节数见structured-error-identities.json。此轮并行构建期间的交互时间不作为性能基准。

## 最终 Editor/发布包刷新及 PC 窗口验收

- build-only + 有界运行 PASS：`hectl.bat editor check -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30`；final-refresh-editor-check.log，589.9秒、exit0，已包含最终Python异常适配修复。
- 发布包验证 PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestShippingPluginMatrix.ps1 -Profile both`；shipping-matrix-20260911-193000-894/summary.json。真实双语言初始化/协作、递归依赖闭包及本地发布通过，不含正式版本上传。
- 当前发布可执行文件：`dist/projects/264dca7dd50ca1c9/StarbloomSurvivor/StarbloomSurvivor.exe`，SHA256=8193ab97a0b1b5e1dfa72d9d35b0e562b5865008ac2df821e93add6c80061406。总文件132,377,071字节，C#目录80,437,501字节（final-refresh-package-size.json）。
- runtime-manual PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Scripting/TestPcGame.ps1`；pc-game-evidence-20260911-194650-297/result.json，10项：初始化、键盘移动、点击加3、真实Sprite变色、真实磁盘存档、场景重载恢复、禁用场景不更新、有效CoreCLR栈且只报告一次、销毁、故障后重进。
- 该PC运行固定开发版SHA256=1dc02844f098150b2bf3d72b6eff1605ff85b4ffd2380389b1ba2fa851eba0ac，玩法源码身份仍为36ce8817…；完整值在result.json。开发版和发布版是不同配置产物，不以一个SHA冒充另一个。
- 画面复核 PASS：194154-432的clicked-preview.png已人工检查；最终194650-297截图与它逐像素一致，visual-difference.json和visual-review.json保留证明。可见右侧C#橙色棋盘和左侧Python绿色观察者。

失败尝试保留：首次PC工具回归被原生PowerShell重定向句柄阻塞，自有进程确认存活后显式停止；后续两次断言暴露暂停态未完成字段快照更新，脚本现在等待真实simulation step完成。最终会话已停止，Project/Assets中无遗留AgentScriptValidation目录。
以上不替代同实例Inspector启停或shipping窗口交互。Editor界面工具本轮重新初始化仍在执行前报kernel assets路径不存在；Android/iOS仍无真机证据。

## 2026-09-11 插件管理与历史渲染实验清理

新增菜单“插件 → 插件管理”，实现全部插件发现（包括未编译启用的插件）、当前 Editor 状态、桌面/portable 配置选择、按宿主开关、后台正式 CMake 校验、配置冲突检测与原子保存。静态插件改变的是下一次构建，保存后需要退出、重建并重启；当前进程不会假装即时卸载。详细入口、语言策略和实际点击步骤见 [PluginManagement.md](PluginManagement.md)。

当前桌面项目保留 C# + Python：MainMenu 的 SurvivorGame.py 和基准相机仍依赖 Python。新玩法主打 C#，Python 保留桌面工具/扩展及旧内容兼容；Web/微信仍仅启用 C#。未擅自关闭依赖中的插件或重写旧玩法。

本轮实际证据：

- 清理 PASS：`powershell -NoProfile -File Tools/Agent/Clean-RenderingExperiments.ps1 -Apply -Python E:/Python/Python311/python.exe -OutputDirectory out/agent/rendering-evidence/cleanup-applied`。删除 9,410 个历史文件、3,277,298,773 逻辑字节；清理时 3,515 个保留文件 SHA-256 不变。仅 Assets/RenderingBenchmarks，SourceAssets、许可证、存档和编译缓存未删除。README 随后更新说明。plan.json/result.json 可核验。
- 清理后 Editor 有界运行 `runtime-manual` PASS：7 个 Git 跟踪渲染场景各 120 帧、dirty=false、源场景哈希不变。精确逐场景命令及独立保留的日志见 `out/agent/rendering-evidence/cleanup-scene-validation/evidence-index.json`。
- 面板 `build-only`/有界启动 PASS：`hectl editor check -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30`；真实面板截图核验 PASS（不是点击验收），`out/agent/scripting/plugin-panel-visual-review.json`。最终错误保护与布局代码在后续 7 场景检查中编译通过。
- 配置保存 `unit-test` PASS：`Tools/Plugins/TestConfig.ps1 -CMake <构建缓存 CMAKE_COMMAND>`，7 项校验/保存/拒绝与冲突用例。`out/agent/plugin-settings-20260911-201324-529/results.json`。文档提供自动读取 CMake 路径的精确复现命令。
- 新增 `ScriptEditorSetup.he` 操作起点 `build-only`/有界运行 PASS：`hectl game check -StartupScene Assets/WeChatSample/ScriptEditorSetup.he -Frames 30`，49.3 秒构建、exit=0，`out/agent/scripting/plugin-editor-setup-game-check.log`。起点只有相机/Sprite，未预填脚本来代替添加组件操作。

上述首次尝试的 Editor UI 为 NOT_TESTED；重启 Codex 后已恢复工具并完成下述实际验收。Android/iOS 真机等原有未完成项没有因本轮改动被标为通过。

## 2026-09-11 21:09 Editor 实际点击验收完成

`runtime-manual` PASS：实际 Windows UI 添加 C#/Python 通用脚本组件、选类型、修改字段、原生另存、保存并重启编辑器，绑定和字段均保留。最终完成两轮直接点击 Play/Stop：C# 41→44、Stop 保持44，第二轮恢复44→47、Stop保持47；Python同步观察分数，两个后端每轮 Initialize/Enable/Disable/Destroy，各新实例 EnableCount=1。停止后 Inspector 回到 C# Score41/Speed5、Python ObservedScore17、EnableCount0。

实际验收发现并修复 Editor 工具栏点击进入玩法、捕获鼠标的无界相对坐标被当作绝对指针两个问题。新公共 ScriptInputContext 不引入运行时依赖；PC 统一脚本输入限定视口、保留短点击并拒绝 UI 起点拖动。Editor 正交相机保留正常指针，透视捕获仍保留且脚本瞄准视口中心。最终 Editor build33.8秒/Game build27.3秒及有界运行通过；独立 ScriptCore 契约和 10 项 PC 真引擎回归通过。

插件面板的打开、两个配置、开关/Discard、两次正式 Apply（临时关闭 C# Tests 后恢复）均有实际点击与文件证据；最终项目配置恢复测试前字节。精确命令、截图范围与失败尝试见 [PluginManagement.md](PluginManagement.md)。主证据 `out/agent/scripting/editor-ui-manual/result.json`，最终窗口正常关闭、exit=0。此结果只补齐桌面 Editor 验收，不将 Android/iOS 或本次未重新生成的平台发布包写为 PASS。
