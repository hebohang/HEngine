# VFX 创作与内容复用实施验收记录

## 任务身份与规则

- 起点：main / cb4181c92f84e52ad44d57a42c31268ad4699c7b。
- 用户原文 SHA256：00dbf798afe61ed46bcda532683c7da00bb5b400d9e974be9db0aa783c5c9130。
- 原始四处 tracked 用户改动与任务 initialChanges 内未跟踪资源全部保留、排除提交；不修改总控记录。
- 本记录按时间保留实施过程；最终交付按用户调整范围完成，结论与限制以 VFXAuthoring-Delivery.md 为准。旧检查点中的进行中/待执行不代表当前状态。
- 引擎构建、GPU 会话串行；使用 hectl owned check/capture 或 finally stop。截图按尺寸先生成预览并逐张核查。
- 2026-09-14 用户追加“朝收尾推进，验收可以放宽”：本次收尾判断见 [VFXAuthoring-Delivery.md](VFXAuthoring-Delivery.md)。进程/OS显存严格零增长降为诊断，原预算与原始FAIL不改；以下历史全矩阵待办不再逐项扩展重跑，只补最终改动影响的必要代表性验证。

## 阶段与状态

| 阶段 | 必需成果 | 当前状态 |
| --- | --- | --- |
| 0 | 源码核查、环境、固定预算、完整矩阵 | 环境/原预算记录完成；用户授权缩减最终矩阵，原严格失败保留 |
| 1 | 生成 stub、覆盖与新鲜度检查、能力契约 | Game18 实际 9 项与 14 模块检查通过；最终共享统计改动未改 Python 绑定，发布符号隔离未复验 |
| 2 | CPU 效果双后端多时刻视觉、性能基线 | 历史 Vulkan 48 项及审图；最终 Game High 双后端各 11 项与 3 图通过，Release 性能未测 |
| 3 | GPU 模拟及提交接通、双后端执行与生命周期证据 | 最终 Game/Editor 双后端代表性执行、计时、收尾和视觉通过；Game23 三项严格内存 FAIL 改作诊断，生产默认仍关闭 |
| 4 | 版本化配方、六类各三变体、两个 GPU 模板 | 24 配方官方编译/回读与 no-op hash/mtime 零扰动通过；最终不重复全模板矩阵 |
| 5 | 参数、预览、重载、事务、表现组合及恢复 | 历史双后端集成各 26 项；角色 DX12 51 断言与实际重载通过，表现组合 13 项通过；具体范围见交付说明 |
| 6 | 子树 Prefab、局部姿态混合/事件、完整常用导入 | 历史 Importer02 13 项及双后端 6 姿态、Editor09 动画双后端通过；角色 large-yaw 视觉 NOT_VERIFIED，任意骨骼挂点不承诺 |
| 7 | he-vfx-authoring 与无历史 Agent 实测 | 项目技能及六类独立创作试运行完成；Low 径向 v3 双后端范围内视觉通过 |
| 8 | 最终全流程、双后端、Editor、发布性能及打包 | 按调整范围完成最终构建、Game/Editor 双后端代表性运行及人工审图；Release/Shipping/原完整压力矩阵未测，不标 PASS |

## 验收矩阵与判定

Game / Editor × DX12 / Vulkan × CPU / GPU 必须独立记录。作者源、运行包、重新导入、发布包分别检查身份；每组涵盖结构、生命周期、多时刻视觉、性能、失败恢复。
构建、启动、生命周期、图像捕获、人工视觉、性能预算分别判定。状态尚未执行一律不得记 PASS；GPU 支持只能在对应后端实际验证后发布为 available。
性能预算已固定在 `Tools/VFX/acceptance-budget.json`；常规与压力规模分别定义，统计预热、多轮 P50/P95，记录 CPU update/draw、GPU VFX/帧、draw/粒子、资源及覆盖代理。下方按时间保留调查与失败记录，当前结论以上表和末尾检查点为准，旧检查点的“待执行”不代表任务重新开始。

## 调查记录

- 2026-09-13：读取 AGENTS.md、he-build-validate、Storage.md 及前两项交接文档；原文哈希匹配。当前 GPU 状态尚未本次验证。
- 文件边界：stub_contract 拥有 Engine/Tools/StubGen 与相关 Tools/Scripting 检查；prefab_animation_audit 只写审计报告；主代理拥有 VFX runtime/interop 与集成记录。

### 第一轮集成检查点

- GPU底层补丁详细根因和版本见 `VFXAuthoring-GPU-Audit.md`。已定位并修正 Vulkan native pass/compute 顺序、零 buffer barrier count、双后端 GPU uniform 内容被后写覆写；旧“GPU draw deferred flush”归因不准确。尚未构建验收，不发布 GPU supported。
- Runtime 开发探测环境变量 `HE_VFX_GPU_PARTICLES=1` 创建真实 SDK system/factory；能力仍为 unverified。新增固定种子播放与受约束参数、Agent `vfx.describe/play/control/stats`，多视图只消费一次 pending compute。失效录制继续 fail-closed，不把重新开启接口当作恢复。
- `Tools/VFX/preview.py` 通过 hectl 受控会话采样并 finally stop，报告独立 visual/performance 状态；固定步进不是发布性能测量。新增中性预览场景 `Assets/Scenes/VFXAuthoringPreview.he`。
- 动画完成首轮局部姿态/连续混合/事件候选及独立数学测试，正式反射已生成；尚未编译/角色运行验证。Prefab方案已完成，尚未实现。
- stub候选及压力入口修复记录见 `VFXAuthoring-Stub-Implementation.md`。真实嵌入Python验收场景已准备，待本轮构建。
- 硬件：Intel UHD 驱动31.0.101.1999；RTX3060 Laptop驱动32.0.15.9174。详细只读结果 `out/agent/vfx-authoring/environment-gpu.json`；实际运行后端仍须查日志，不从设备清单推断选择。
- 工具链实测 VS2026 v145 / MSVC14.51.36231。日常hectl当前仅Debug/RelWithDebInfo；发布性能必须单独记录Release包和开发统计差异。
- 初始 Vulkan 运行期间 Storage mutex busy，登记/回收延后有原始日志；未删除锁或未知历史。Game自身已exit0，不把存储警告抹去。

## 验证账本

| 类型 | 精确命令 | 结果 | 说明 |
| --- | --- | --- | --- |
| 只读环境 | `Get-Location`、`git branch --show-current`、`git rev-parse HEAD` | PASS | cwd、main、起点一致 |
| 原文身份 | `Get-FileHash .git/prompt-scheduler/20260911-rendering-vfx-01/prompt-3-exact.md` | PASS | SHA256 与授权一致 |
| build-only / runtime-manual | `.\hectl.bat game check -Parallel 2 -GraphicsApi DX12 -StrictGraphicsApi -Frames 60 -TimeoutSeconds 180` | PASS / PASS | 初始build110.2s；仅启动运行退出，不是VFX视觉；initial-dx12-check.log |
| runtime-manual | `.\hectl.bat game check -Parallel 2 -GraphicsApi Vulkan -StrictGraphicsApi -Frames 60 -TimeoutSeconds 180 -ExistingBuildSha256 $vfxInitialHash` | PASS | `$vfxInitialHash`完整值存initial-game.sha256；复用初始新构建，仅运行非当前后续源码构建证据；initial-vulkan-check.log |
| codegen | `.\hectl.bat codegen run -CodeGenHost DevGame -Configuration RelWithDebInfo -Json` | PASS | changed=8、deleted=0；不是no-op证据；codegen-devgame-01.log |
| build-only | `.\hectl.bat game build -Parallel 2` | RUNNING | 首轮GPU/动画/API候选；build-game-01.log |

后续实际构建/测试分别使用 build-only、runtime-manual、unit-test 或 test-layer，记录精确命令、配置、产物 hash、证据文件与独立结果。

### 第二轮实际执行检查点（进行中）

- 首轮build-game-01因内部LLGI头泄漏编译失败，已改为公共命令计数接口；build-game-02执行 `.\hectl.bat game build -Parallel 2` build-only PASS（120.9s）。固定候选SHA256为E6015969BD7B30DA82741C912C2C6DDD936856F0DAC4E55B37EF61CB093FD7E4。该候选不包含后续Prefab/导入/绑定修改，所有固定候选运行仅证明该候选运行行为。
- GPU官方压力包 `Assets/Effects/Stress/GpuSparks20k.efkpkg` 在DX12/Vulkan固定步进0/1/30/90/180帧均产生实际GPU绘制，停止后无活动效果。证据在probe-official-gpu-dx12-01和probe-official-gpu-vulkan-01，精确命令见各commands.json；runtime-manual初步视觉通过，不是20k粒子读回、完整生命周期或发布性能验收。GPU模拟采样P95分别约0.031744ms/0.040672ms；暂停步进数据不用于预算通过判断。
- 自制CPU hit首轮视觉FAIL（旧XML颜色被官方编译器忽略）；自制GPU explosion首轮视觉FAIL（IsRendered=false导致无GPU发射器），第二轮仍未通过模板视觉验收（owner和粒子重复缩放）。失败截图保留；新单位owner版本正在第三轮执行。
- 全24配方产物已实现官方工具编译、EDIT逐字段回读和发布事务候选，尚未完成全部双后端视觉与性能验收。固定预算见Tools/VFX/acceptance-budget.json。
- 真实嵌入Python stub运行依次暴露缺少开发stdlib、动态绑定方法漏收、UI常量漏收。stub-runtime-02/03均FAIL，不能以Game exit0冒充生成通过；开发stdlib只用于生成验收，未改变发布Python依赖。
- Prefab子树实现与测试已稳定，主代理注册PrefabDocument.cpp和PrefabSubtree测试。动画候选已编译，测试尚待统一执行。UV1/颜色/采样及离线RT实现仍在子代理静态收尾；实时GI原有常量近似不宣称精确材质采样。
- 表现组合服务、资源和调度/清理单元候选已落盘，真实Game、Editor、发布包验收尚未执行。
- 当前GPU能力仍为unverified；所有阶段未满足最终矩阵前不标记完成，也不提交推送。

更正：stub-runtime-03的UI常量问题属于覆盖检查器漏认ast.Assign，生成器已经输出常量；检查器已修正并保持严格符号覆盖，真实embedded04复跑中。第三轮GPU爆炸commandsPASS，但扩散半径仍待核实，视觉未接受。MaterialSampling测试已注册，等待稳定代码构建。

### 第三轮集成前检查点

- embedded stub-runtime-05：runtime-manual PASS，真实输入查询、场景实体/组件、VFX加载/播放/停止全部true；生成14模块changed=1/deleted=0。未注册C++类型六项仍显式报告，后续绑定修复和最终新鲜度检查未完成。
- `.\hectl.bat codegen run -CodeGenHost DevGame -Configuration RelWithDebInfo -Json`：codegen PASS，changed=18/unchanged=255/deleted=0，12724ms。正式生成PrefabInstanceComponent与TextureSampling等新反射；证据codegen-devgame-02.log。
- PrefabSubtree、AnimationPoseContract、MaterialSampling三项测试已注册；源码稳定等待build03。此前子报告AnimationPose名称以主注册AnimationPoseContract为准。
- preview-gpu-dx12-03原尺寸差分：frame1=26px、frame8=402px、frame20=898px（bbox619,400..660,432），frame45无残留。detail.jpg已检查，有扩散和亮点，但整体视觉质量/颜色/类别区分仍需全模板验收。源码确认GPU速度按frame积分，无60倍单位错误。
- 新增GetEffectStatus读取SDK真实paused/speed；无public per-handle simulationFrame时返回null及unavailable原因。GPU计时仅在实际请求dispatch时计入模拟样本（待build03）。多次Update不Render的SDK delta覆盖风险仍在审计，不宣称完整生命周期可靠。

### 第三轮构建与绑定验收

- `.\hectl.bat game build -Parallel 2`：build-only PASS，305.4s；integration-03 SHA256 BF9EB294FF82A5D486E05AC06159285B536456BC3F30E4E883FD7B5D7C2B71DD，精确输出build-game-03.log。
- stub-runtime-06：runtime-manual PASS，registered_cpp_types/component_type_roundtrip/input_query/scene_entity/scene_component/vfx_load/vfx_play_query/vfx_stop全部true；14模块changed1/deleted0，unregisteredCppTypes=[]。会话正常finally停止。
- `.\hectl.bat test PrefabSubtree -Configuration RelWithDebInfo -Parallel 2`正在构建Tests独立宿主，不与GPU会话并行。
- 主代理重跑 `py -3 Tools/Content/test_sampling_fixture.py` 3 tests PASS；`py -3 Tools/Content/test_sampling_reference.py` 3 tests PASS；`py -3 Tools/VFX/test_presentation.py` 6 tests PASS。源/数值/调度单元测试不替代实际图像验证。
- build03之后新增未编译补丁：CPU UpdateSerial与Python统计标识、Agent seed/handle int32范围拒绝、GPU factory空资源时拒绝发布，保留原缓存事务。GPU模拟/绘制采样还需分别记录submission（组合字段不能给停滞模拟值赋新身份）。
- 新场景Project/Assets/Scenes/PresentationAcceptance.he准备实际组合验证，尚未运行。

### Tests 首轮失败与后续候选（进行中）

- Tests首次配置PASS 383.1s，下载并校验固定OIDN2.5.1依赖；独立宿主重新编译1448项。首轮在OfflineReferenceRenderer HLSL:569失败：屏幕float2 UV误改为float4；不是Prefab测试FAIL，而是测试未能运行。完整日志保留build-tests-01.full.log、test-prefab-subtree-01.log。
- 修复屏幕UV并只读核查其余双UV；补MotionBlend有限极值double插值，Animator.SetFloat拒绝NaN/Inf并保留旧值，新增实质边界测试。第二轮 `.\hectl.bat test PrefabSubtree -Configuration RelWithDebInfo -Parallel 2` 正在增量构建。
- 新候选统计拆分last_gpu_simulation_submission/last_gpu_render_submission和update_serial，防止旧simulation current被新draw submission冒充新样本。
- SDK新增实际allocator失败三类计数与占用快照，Runtime/Python/Agent接出；只称CPU allocator账目，不称GPU live粒子或fence退休内存。生命周期工具要求失败计数不增、30次循环占用回baseline。
- GPU颜色支持按已编译所有GPU节点NotBind_Root确认；6个GPU包已改EDIT ColorInheritType=3并官方编译/回读。仍待实际颜色变化验收，平台gpuValidated仍false。
- Runtime新增SDK真实GetLocation读回；公开Python GetEffectPosition用于角色挂点验证。GetEffectStatus有position/supportsGlobalColor/authoredRadius/intensity。
- 新SetParameter命名入口支持speed/scale、带recipe v1标称radius metadata的radius_m、[0,1] RGBA衰减intensity。现有粒子响应受作者parent binding约束，不宣称重写worldspace历史轨迹。preview新增--color/--intensity并对拒绝明确失败；运行验证待Game04。
- UUID属性返回内部引用问题已确认；仅FollowCamera.TargetUUID按值复制，保持Python值/hash稳定，新烟测待执行。
- 子代理正在制作真实双实例角色/动画事件资源；已有benchmark与五轨表现组合示例，尚未GPU/场景验收。

### 第四轮集成前检查点

- `unit-test` `.\hectl.bat test PrefabSubtree -Configuration RelWithDebInfo -Parallel 2` PASS；第三轮构建87.7s，实际测试日志21:30:37 PASS。测试二进制SHA256 3485BE482B91F86DC7FEA606D0B6E4348ACEA5316FB258288CA2262CA03E9DC0；该产物早于下面的提交链路改动。
- GPU模拟候选改为Layer Update前打开host graphics frame，每次Manager.Update后同步执行独立side-effect VFXSimulation RenderGraph段，同一command list最终提交；视图只Draw。无camera仍Update，多视图不重复Compute。
- 模拟推进后host abort将标记失同步，拒绝继续Update直到重新初始化；禁止活动host frame内销毁GPU配置。尚未编译或实际验证，不作为生命周期PASS。

### 第四轮构建和首个新链路探针

- `codegen` `.\hectl.bat codegen run -CodeGenHost DevGame -Configuration RelWithDebInfo -Json` PASS，changed=0/unchanged=273/deleted=0，12786ms；此记录未独立核对输出mtime，不宣称完整no-op零扰动证据。
- `build-only` `.\hectl.bat game build -Parallel 2` PASS，169.0s。Game04 SHA256 `1DBE8F850C4EBAC30022796C686FEB321ADAF89140CF0A1F02FBB38F93F8E796`。
- `runtime-manual` `py -3 Tools/VFX/preview.py --backend DX12 --effect Assets/Effects/Authoring/explosion-ember.efkpkg --scene Assets/Scenes/VFXAuthoringNeutral.he --frames 0,1,8,20,45 --existing-build-sha256 1DBE8F850C4EBAC30022796C686FEB321ADAF89140CF0A1F02FBB38F93F8E796 --gpu-probe --output out/agent/rendering-evidence/vfx-authoring/probe-gpu-dx12-04` PASS。截图检查发射/扩散/结束，实际累计dispatch在step1/8/20/45为3/10/22/42。近景中性黑背景frame20变化15238px，frame45零残留；像素变化不是overdraw，也未接受全模板美术质量。
- 当前后台Storage collector持续占用登记锁，此探针outer/inner出现registration deferred，记录原始警告。没有删锁或终止collector；证据保留原始路径，运行finally stop正常。
- Editor额外预览只提取视图；全局VFX Update移至活动Level::OnUpdateEditor结束，避免材质/缩略图/资产视图重复推进。

### 参数失败与第五轮候选

- stub-runtime-07 `runtime-manual` PASS：真实嵌入Python9项检查全部true，包括uuid_value_semantics；生成14模块changed2/deleted0，无未注册C++类型。`py -3 Tools/Scripting/CheckPythonStubs.py` PASS；隔离mypy对14模块PASS。
- parameters-dx12-01 `runtime-manual` FAIL：PROVENANCE入口读取未使用包条目小写规范，authoredRadiusMeters=null，radius_m拒绝，reload断言遇None终止。已改NormalizeVirtualPath查找；运行器对缺失数值记录FAIL并继续，不降低断言。
- 同次截图实测CPU颜色/intensity有效；GPU暂停后color/intensity输出逐像素完全不变，visual-review明确FAIL。SDK Instance零delta矩阵早退跳过NotBind_Root颜色传递；最小同步补丁已准备，尚未编译验证。accepted/store读回不作为视觉生效证据。
- `build-only` `.\hectl.bat game build -Parallel 2` PASS49.0s（仅metadata修复），Game05 SHA256 69E8D6973AE7C241D6A02FD47693A179C468F067FD7CD62F5191F70380FE64B5。
- `runtime-manual` `py -3 Tools/VFX/character_acceptance.py --backend DX12 --existing-build-sha256 69E8D6973AE7C241D6A02FD47693A179C468F067FD7CD62F5191F70380FE64B5 --case character --output out/agent/rendering-evidence/vfx-authoring/character-dx12-01` PASS，31必需断言，30固定步骤；Editor事务/视觉审美仍NOT_RUN/NOT_REVIEWED。
- 后续功能Session启用RenderValidation；最初Game04双后端probe没有验证层，不把它们当验证层无错误证据。

### 当前单元验收与恢复候选

- `unit-test` `.\hectl.bat test AnimationPoseContract -Configuration RelWithDebInfo -Parallel 2` PASS（实际运行22:00:28）。
- 相同明确单测命令分别运行 MaterialSampling、MeshCpuDecode、RuntimePrefabResource：PASS。SceneEditing：FAIL，旧单实体Prefab roundtrip多出管理元数据；队列在失败处停止，EffekseerRuntimeLifecycle尚未执行。
- 修复候选仅以非反射legacy来源标志保留旧无管理元数据语义；v2及其嵌套legacy仍完整管理。新增严格旧组件往返/不同UUID与v2嵌套管理断言，原失败断言未修改，待重跑。
- presentation-dx12-01实际PASS，13项状态观察与清理；明确不代表画面审美或音质验收。
- Game06首次启动被旧Game05 cleanup持有lease拦截，未发生编译；build-game-06.log保留。等待旧进程正常结束后，build-game-06-retry正在统一构建SDK零delta、shader、Vulkan abort drain和legacyPrefab补丁。

### Game06 参数与拖尾实际验证（进行中）

- `build-only` `.\hectl.bat game build -Parallel 2` PASS56.8s，build-game-06-retry.log；SHA256 A01F403F553E0676E5474931B87576EE257193D65B770DD65D4F24DA27F5ACB2。包含SDK暂停颜色/零delta拖尾shader、Vulkan abort drain、legacy Prefab修复；此后新增Agent transform尚未编译。
- `runtime-manual` `py -3 Tools/VFX/parameter_acceptance.py --backend DX12 --existing-build-sha256 A01F403F553E0676E5474931B87576EE257193D65B770DD65D4F24DA27F5ACB2 --output out/agent/rendering-evidence/vfx-authoring/parameters-dx12-02` FAIL：37项已记录检查均PASS，但GPU禁用拒绝分支解析混合CP936日志时严格UTF8异常。finally正常，输入/二进制未变。脚本读取增加errors=replace；CP936+invalid_params通过、transport_error失败及session存活检查的小型回归PASS，待整轮重跑。
- 本轮GPU暂停参数截图人工检查：color确实变色、intensity0恢复背景、0.5较暗、1恢复。相对base的>12/255变化像素依次221/284/284/221；crop=[618,397,662,439]，证据enabled/gpu-parameter-crops.jpg。只确认固定小样参数的可见响应，不代表全模板质量/视觉半径通过。
- GPU真正TrailLength16双发射器暂停/速度0测试已开始，结果未定。原失败证据不覆盖；后续证据均使用Storage rendering-evidence/vfx-authoring目录。

### GPU 拖尾双后端实测与 Release 入口

- `runtime-manual` bundled Python `Tools/VFX/trail_pause_acceptance.py --backend DX12 --existing-build-sha256 A01F403F553E0676E5474931B87576EE257193D65B770DD65D4F24DA27F5ACB2 --output out/agent/rendering-evidence/vfx-authoring/trail-pause-dx12-01` PASS25检查；同命令后端Vulkan、输出trail-pause-vulkan-01也PASS25检查。bundled Python完整路径为C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe。
- 两后端人工顺序检查trail-crops.jpg，首零步/其他emitter运行32步保持目标几何，intensity0隐藏/1还原，恢复一步连续；allocator回基线。输入和候选hash未变，finally停止成功。原始报告/PNG保留；不代表粒子state hash、device重建或性能通过。Storage登记busy/deferred警告继续原样记录。
- 根复核Editor驱动：移除195秒后taskkill进程树，避免杀仍在Storage收尾的hectl；超时明确FAIL/unknown并保留PID与恢复材料。Editor自身仍由hectl bounded check负责回收。AST PASS，实际Editor运行未做。
- 公共hectl/Build/Session只扩展Configuration ValidateSet支持Release；原有按配置目录、CMAKE_BUILD_TYPE及PACK_GAME=OFF/HE_ENABLE_GM=ON路径保持。三脚本AST/Release参数检查及hectl help -Configuration Release PASS。实际Release编译/性能尚未执行，不作为发布包证据。

### 参数整轮拒绝契约失败

- parameters-dx12-03最终`runtime-manual` FAIL39检查中1项：GPU禁用时包实际拒绝，但vfx.play抛runtime_error被Agent遮蔽成internal_error。CPU显式fallback播放及其余38项PASS，输入/hash未变、正常收尾。不是放宽脚本的理由。
- 根把已知PlayEffect负句柄拒绝映射为invalid_argument，并公开当前GPU禁用时应选择兼容CPU包；GPU启用时说明格式/资源/预算约束。未知异常仍由框架保持internal_error。该补丁与Agent transform待Game07编译验证。

### Legacy Prefab 回归恢复

- `unit-test` `.\hectl.bat test SceneEditing -Configuration RelWithDebInfo -Parallel 2` PASS；构建HEngineTests PASS74.5s，实际22:36:52原回归断言通过，test-SceneEditing-02.log。修复保留旧单实体Prefab无管理元数据的原行为，不修改原失败测试。
- PrefabSubtree、RuntimePrefabResource、EffekseerRuntimeLifecycle按同一精确hectl test形式串行重跑，尚待本轮最终结果。
- 根重跑benchmark_test.py 7项PASS（后续Storage登记失败不启动子进程回归由子代理扩展至8项，待最终统一检查）。模板矩阵help、相关脚本/SceneSmoke diff --check通过；这些不代替engine/GPU验收。

### 验证层范围与启动前失败

- Game06两个拖尾run的game.stdout/stderr原始日志已复制到各自证据目录。Vulkan原生validation layer enabled，扫描未见VUID/VK_ERROR；DX12明确D3D12 debug layer unavailable(0x887A002D)，仅NVRHI validation enabled。两者stderr为空。不能把DX12本轮写成原生D3D12调试层通过；暂停会话中的RenderPerformance数据不用于性能预算。
- Game07构建节点PASS27.5s，但外层仍在Storage cleanup，hash写出尚未执行。首个parameters-dx12-04启动命令在读取缺失integration-07.sha256时失败，argparse拒绝缺失hash，未启动会话/未产生验收；原始log保留，后续使用新run编号重试，等待完整构建流程退出。
- PrefabSubtree/RuntimePrefabResource/EffekseerRuntimeLifecycle本轮unit-test全部PASS，分别22:37:37/46/55；精确命令均`.\hectl.bat test <Name> -Configuration RelWithDebInfo -Parallel 2`。

### Game07 / Editor01 构建候选

- Game07实际build-only PASS27.5s，根在确认日志PASS后独立读取并固定SHA256 FE154F887B5CE6CF446625C97CFC96BB0231C7AE8C174E49414CC29A8E61BAC7；外层Storage仍可能收尾，不重建或覆盖该候选。
- `codegen` Editor构建节点PASS，changed=26/unchanged=247/deleted=0（config/context变化及新增Prefab反射等）；不是no-op零扰动声明。
- `build-only` `.\hectl.bat editor build -Parallel 2` PASS416.3s，build-editor-01.log；包含EditorSceneMailbox和当前Editor Runtime/Importer改动，实际Editor运行未开始。
- 新准备但尚未engine运行：template_matrix.py、scene_resize_acceptance.py、cpu_path_acceptance.py、sampling_acceptance.py。CPU路径四fixture已官方编译/依赖检查，runtime/visual仍未验；导入driver的stop失败保留场景回归由根运行3项PASS。
- Game07参数DX12(parameters-dx12-05)→Vulkan(parameters-vulkan-01)开始串行运行。parameters-dx12-04只属于缺失启动参数的未运行记录。

### Game07 参数双后端结果与 Vulkan 纹理生命周期缺陷

- `runtime-manual` `py -3 Tools/VFX/parameter_acceptance.py --backend DX12 --existing-build-sha256 FE154F887B5CE6CF446625C97CFC96BB0231C7AE8C174E49414CC29A8E61BAC7 --output out/agent/rendering-evidence/vfx-authoring/parameters-dx12-05` PASS39检查、输入/hash未变。GPU禁用拒绝返回invalid_params和清晰CPU兼容包提示。
- 同命令后端Vulkan/output parameters-vulkan-01：FAIL，已记录38项全部PASS，但disabled会话CPUfallback首步导致VkImage被提前销毁与DeviceRemoved，不能算通过。enabled会话原始日志无validation/device error；disabled stdout/stderr各85条错误（同一批重复输出），最早22:53:49 vkCmdBeginRenderPass引用已销毁image。两会话日志/validation-summary已复制进各证据目录。
- 已定位真实底层遗漏：GraphicsDevice待生成mipmap队列持有被拒GPU包纹理；下一CPU绘制FlushPendingMipMapGenerations录制后clear队列，Vulkan GenerateMipMap没有像CopyTexture和DX12版本一样RegisterReferencedObject(src)，纹理最后引用释放早于command提交。最小第三方修复只在成功记录mip命令后注册源引用，经现有FrameResources持有至安全复用。没有改Runtime预拒绝或放宽验收，详见VFXAuthoring-GPU-Audit.md。待Game08构建/相同失败路径回归。

### Game08 Vulkan 同路径回归通过

- `build-only` `.\hectl.bat game build -Parallel 2` PASS20.2s，Game08 SHA256 CBCB8C1481236B245F04EFDE3489D7DA2D143A4C6B8047D66ADC7813F050AF6C。
- `runtime-manual` `py -3 Tools/VFX/parameter_acceptance.py --backend Vulkan --existing-build-sha256 CBCB8C1481236B245F04EFDE3489D7DA2D143A4C6B8047D66ADC7813F050AF6C --output out/agent/rendering-evidence/vfx-authoring/parameters-vulkan-02` PASS39检查，输入/hash未变。enabled/disabled两会话原始stdout/stderr已复制，validation/device error均0；此前GPU拒绝→CPUfallback的真实崩溃路径恢复。
- Session工具补齐失败检测：已死进程stop仍检查validation日志；正常stop固定native Process.Handle，非0/null退出码失败。公共验证器识别DeviceRemoved/device-lost错误。preview.Session结束时复制原生日志和hash。新Python回归2项PASS（停止失败仍保留日志、拒绝越界日志路径）；PowerShell mock回归待完成。
- 阶段5额外审计确认SDK允许空纹理/模型/材质槽，外层EFKE版本未检查；Runtime发布前资源/版本校验候选正在限定实现。Game08不包含该候选，不据此宣称缺依赖恢复已通过。

### Game09 资源准入候选与验收工具回归

- `unit-test` `powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-VfxSessionStop.ps1` 根运行PASS7项；覆盖已退出/活进程的validation/device-lost错误、0/非0/不可读退出码与重复停止。完全mock，不接触真实会话记录。
- `runtime-manual` `py -3 Tools/VFX/scene_resize_acceptance.py --backend Vulkan --existing-build-sha256 CBCB8C1481236B245F04EFDE3489D7DA2D143A4C6B8047D66ADC7813F050AF6C --output out/agent/rendering-evidence/vfx-authoring/scene-resize-vulkan-01` FAIL：真实client/render已960x540，但check写报告重复status关键字抛异常；finally干净、输入/hash不变。不是GPU崩溃，也不是缩放验收PASS。脚本修复与mock流程回归进行中，原证据保留。
- `build-only` `.\hectl.bat game build -Parallel 2` PASS31.1s，Game09 SHA256 EDEF73E8B38BB7A025E777DAD577686B090A5587FD13CD6213E6526C495B3BA3。新增EFKE/SKFE有界版本检查与SDK声明依赖发布前准入。已扫描语料91个EFKE0与1个raw SKFE1710；非全格式兼容保证。
- `runtime-manual` `py -3 Tools/VFX/resource_failure_acceptance.py --backend Vulkan --existing-build-sha256 EDEF73E8B38BB7A025E777DAD577686B090A5587FD13CD6213E6526C495B3BA3 --output out/agent/rendering-evidence/vfx-authoring/resource-failure-vulkan-01` RUNNING。四种负例验证同键reload拒绝、旧实例与旧缓存可播放；结果未定。
- 后续只读审查发现合法编译-only材质包需要允许源码路径对应的.efkmatd条目；CurveLoader尚未注册。修复候选准备中，Game09不包含这些后续修复，不作为最终兼容性证据。

### Vulkan CPU路径首次实际失败与修复

- Game09 `runtime-manual` resource-failure-vulkan-01 PASS12项，四类损坏同键reload均拒绝且旧实例/旧缓存有效；输入/hash未变，原生日志无validation/device error。仅四项负例，不覆盖compiled-only材质/curve正例。
- `unit-test` `py -3 Tools/VFX/scene_resize_acceptance_test.py` PASS2项（含完整resize/switch双存活策略）；`py -3 Tools/VFX/test_acceptance_contracts.py` PASS6项。
- `runtime-manual` bundled Python `Tools/VFX/cpu_path_acceptance.py --backend Vulkan --existing-build-sha256 EDEF73E8B38BB7A025E777DAD577686B090A5587FD13CD6213E6526C495B3BA3 --output out/agent/rendering-evidence/vfx-authoring/cpu-path-vulkan-01` FAIL。功能断言通过但退出验证器捕获24条原生日志错误（stdout/stderr重复）：D32 SceneDepth被强制R32_FLOAT/color aspect SRV导致VUID image-01762/usage-02277等。会话实际已退出；严格stop成功检出错误，保留unknown证据。
- 根修复EffekseerRenderGraphPass::GetOrCreateDepthBindingSet：移除R32 view override，使用NVRHI默认depth SRV；DX12映射表D32->R32_FLOAT SRV、Vulkan保留D32/depth aspect。没有改第三方或关闭验证，待新候选构建。
- 人工顺序检查path-contact.jpg/temporal-contact.jpg：soft/control差异只在接地26px带[527,403,754,429]，5123像素>12变化；扭曲确实使背景柱边位移；30/90稳定、各停止图与各空图逐像素一致。模型红白纹理可见但bbox[311,0,1039,120]出上边裁切，模型视觉FAIL。模型fixture将按真实bounds调尺寸，其余对照不变；这些观察不能覆盖验证层FAIL。

### 场景切换与窗口缩放首次完整实测通过

- Game09 `runtime-manual` `py -3 Tools/VFX/scene_resize_acceptance.py --backend Vulkan --existing-build-sha256 EDEF73E8B38BB7A025E777DAD577686B090A5587FD13CD6213E6526C495B3BA3 --output out/agent/rendering-evidence/vfx-authoring/scene-resize-vulkan-02` PASS71项，输入/hash未变、stop无错误。覆盖两场景实际generation切换、960x540/1280x720/1024x768真实client/render/PNG尺寸、CPU+GPU共存、新命令和退休基线。
- 根顺序人工看resize-contact.jpg：中性背景粒子保持可见、比例随视口保持合理、最终无残留；地板预览场景上的粒子偏小，仅算切换/缩放运行观察，不代表模板游戏背景质量通过。
- 模板矩阵现有game映射VFXAuthoringPreview.he仅含5实体地板，不能等同实际游戏背景。准备使用MainGame实际地图/lighting的固定背景副本，保留原场景，记录冻结脚本与来源。尚未运行模板矩阵，不覆盖任何旧证据。

### Game10 深度采样与资源重载候选

- `build-only` `.\hectl.bat game build -Parallel 2` PASS23.7s，Game10 SHA256 8E5B9445A30E33AB506C2224358E61F6188BCF009E5DF367B2EE384F2BCC006B。包含NVRHI depth SRV修复、CurveLoader、compiled-only材质准入、关闭SDK仅路径缓存；Runtime整Effect缓存保持，不同Effect资源独立可能提高内存/加载成本，后续实际测量。
- CPU模型修正为scale=.3、offsetY=.65（1.2m局部平面），官方编译/回读PASS；模型包2ec685adf46f2f34260386c62e609e55b8391ab8d7af40d2bbbff85ae6a583df。其他对照源/包hash/mtime未改。generator整数0与0.0引起source hash差异已修复；根独立比较四个源与四个重新编译包全部逐字节一致，临时重建证据he-cpu-fixture-rebuild-check-sm3s5yqg/rebuild-comparison.json。
- `runtime-manual` bundled Python `Tools/VFX/cpu_path_acceptance.py --backend Vulkan --existing-build-sha256 8E5B9445A30E33AB506C2224358E61F6188BCF009E5DF367B2EE384F2BCC006B --output out/agent/rendering-evidence/vfx-authoring/cpu-path-vulkan-02` RUNNING。Storage busy/deferred警告保留，未杀collector或删除锁。

### Vulkan CPU四路径修复复验通过

- Game10 cpu-path-vulkan-02 `runtime-manual` PASS35检查，输入/hash未变、正常stop；原生validation/device error 0。此前D32视图真实错误消失。
- 根顺序检查temporal-contact.jpg与原分辨率model-crops.jpg：完整红箭头/白杆无出画裁切，model bbox[598,209,693,255]；soft/control仍只在接触带5123像素变化，扭曲保持真实背景边缘位移，四效果停止后全部逐像素等于各自空背景。限定四路径人工visual PASS，记录visual-review.json；不是全模板、自然结束或性能验收。
- 同Game10、bundled Python命令改`--backend DX12 --output out/agent/rendering-evidence/vfx-authoring/cpu-path-dx12-01`正在运行；仍独立判断，不借Vulkan结果。

### DX12 CPU四路径与跨后端画面对照

- Game10 cpu-path-dx12-01 `runtime-manual` PASS35项，原样输入/hash未变、正常stop；人工temporal-contact确认soft接地/背景扭曲/完整箭头和停止回空背景。visual-review.json记录限定路径PASS。
- DX12仍明确native debug layer unavailable(0x887A002D)，只有NVRHI validation enabled，日志无对应错误。不能称原生D3D12调试层通过。
- 同固定场景、step30跨后端RGB MAE约0.0081~0.0084/255，>12差异2~3像素；作为对照数据，不要求逐像素跨API一致。四条停止图各自与空图逐像素一致。
- 下一项actual probe通过设置HE_VFX_EARLY_SMOKE=1、HE_VFX_EARLY_SMOKE_BACKEND=Vulkan、HE_VFX_GPU_PARTICLES=1、HE_VFX_DISABLE_STATS=0、HE_DX12_SCENE_SMOKE_DISABLE_VALIDATION=0后运行`powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Parallel 2 -Run -Wait -TimeoutSeconds 180`。finally恢复上述env；build/runtime结果未定，early-vulkan-01.log。

### 同路径纹理重载与曲线首帧断言

- Game10 `runtime-manual` bundled Python `Tools/VFX/resource_reload_pixels.py --backend Vulkan --existing-build-sha256 8E5B9445A30E33AB506C2224358E61F6188BCF009E5DF367B2EE384F2BCC006B --output out/agent/rendering-evidence/vfx-authoring/resource-reload-vulkan-01` PASS6项；DX12同命令换backend/output resource-reload-dx12-01亦PASS6项。两后端根顺序检查reload-contact：旧红实例保持，新同键实例绿，另一包同名资源蓝。旧ROI MAE Vulkan=0、DX12最大0.00000434/255；输入未变，stop/validation无错误（DX12仍NVRHI-only）。各visual-review.json限定PASS。
- 原创MaterialCurveValidation资源由官方CLR材质序列化/官方CLI编译和CurveLoader公开格式产生；源码/依赖/CC0原创声明保留。Game10 `material-curve-vulkan-01` FAIL15项中2项，SDK材质/曲线加载、全部缺依赖拒绝/旧实例保持均PASS，但曲线step1零信号使旧可见性/运动断言失败。核对原作者源FadeIn.Frame=1，首spawn标准sprite alpha=0；custom constant材质未乘顶点alpha所以首frame仍可见。
- 根修driver：保留并明确断言curve第1帧playing且空图，增加第2帧必须>=50信号像素，移动差异从第2到60帧检查，其余30/60和失败恢复不变。并非删除失败样例或放宽阈值；原FAIL证据保留，py_compile PASS，待重跑。原曲线30→60已见28px横向移动，停止两图均逐像素空背景。
- EarlyVulkan01 build-only PASS43.8s，runtime-manual FAIL；probe缺RenderPrimitiveCollector scope已修。异常abort暴露NVRHI open自引用环：未提交list/pool泄漏。追加Vulkan最小补丁在Queue::submit前登记self，未提交析构CAS仅归还本recording的volatile槽；非假提交。earlyprobe加共享单版本volatile buffer两次abort写入回归。NVRHI路径在Git中为大写NVRHI，改动已确认受跟踪。
- EarlyVulkan02同前文env/Build精确命令正在运行；build-only PASS21.1s，实际runtime未定。Game10可执行仍不含NVRHI新补丁，后续需Game11重新链接。

### 质量档矩阵与真实背景首轮

- Game10 `runtime-manual` `py -3 Tools/VFX/quality_acceptance.py --backend Vulkan --existing-build-sha256 8E5B9445A30E33AB506C2224358E61F6188BCF009E5DF367B2EE384F2BCC006B --output out/agent/rendering-evidence/vfx-authoring/quality-vulkan-01` PASS；DX12同命令替换backend/output quality-dx12-01亦PASS。各Low/Medium/High独立会话11检查，共每后端33项；容量0/20224/50176、自然结束和退休均通过，native日志无错误（DX12 NVRHI-only）。frame180 RGB最大0；根顺序看quality-contact，Low显式CPU稀疏火花、Medium/High同GPU样式。完整低档可读性保留到真实背景模板视觉验收，visual-review为PARTIAL而非全模板PASS。
- Game10 `runtime-manual` `py -3 Tools/VFX/template_matrix.py --backend Vulkan --existing-build-sha256 8E5B9445A30E33AB506C2224358E61F6188BCF009E5DF367B2EE384F2BCC006B --recipe explosion-ember --scene game --output out/agent/rendering-evidence/vfx-authoring/matrix-game-vulkan-probe-01` command PASS，visual FAIL/需调整：当前地图原点星形浅色平台遮挡/相交与背景对比不足，不能算合格实际游戏背景模板。根检查game-probe-contact；将依据真实地面几何确定选点，不降低原游戏光照来造通过。
- EarlyVulkan02 build21.1s/runtimeFAIL（collector构造早于BeginFrame），native validation 0但流程未到GPU，因此不算abort通过。根按Level实际顺序修正。
- EarlyVulkan03 build21.3s/runtimeFAIL exit=-1073741819，首无相机compute出现invalid sampler VUID descriptorType-00325；已复制完整原生日志和候选hash0de0b8ea3469e506e53670293189e50d06769f80e602905c08d4ce41c2ec904b。NVRHI自环修复不足以覆盖这一独立问题。
- LLGI sampler全部在Initialize创建、texture reset和descriptor数组容量静态检查未见直接遗漏；已加入默认关闭、仅HE_VFX_EARLY_SMOKE=1最多32列表的创建/首次使用诊断，early输出unitbuf。EarlyVulkan04同env/Build命令运行中。禁止预先Draw绕过noDraw验收。
- 另修真实能力声明：发现File材质的效果保守supportsGlobalColor=false，ActiveEffect持有自身原因不随reload新缓存改变，status给custom_material_color_response_unverified，color/intensity拒绝且状态不变。内置材质继续原GPU继承条件。Runtime源码已冻结，Game10不含该修复，下一Game11需编译；material/curve driver已加对应真实断言。

### 真实地图落点复验与 EarlyVulkan04 定位修正

- Game10 `runtime-manual` `py -3 Tools/VFX/template_matrix.py --backend Vulkan --existing-build-sha256 8E5B9445A30E33AB506C2224358E61F6188BCF009E5DF367B2EE384F2BCC006B --recipe explosion-ember --scene game --output out/agent/rendering-evidence/vfx-authoring/matrix-game-vulkan-probe-02` command PASS，单样例人工 visual PASS。保留 MainGame 原光照，静态地形核对选点(10,.02,10)，空中效果Y=.52；背景scene SHA04ecf2ddfbe92c66229bc542ecfec088cb789b30f90d5bca38f748abae06197a。根检查1200px contact，峰值/衰减清晰完整，>12信号分别8882/19890像素，end/retired与空背景逐像素相等。完整24×背景×API矩阵仍未验收。
- EarlyVulkan04 `build-only` PASS40.3s，`runtime-manual` FAIL exit=-1073741819，候选SHA57dd06f93b74444e7bb7c9dbc9e4afbcc83b114d6effff5f6f4927cf9f23a30e，完整证据early-vulkan-04。unitbuf诊断修正上一条早期定位：4次无相机同host更新均PASS，recordSerial4/nativeDispatch8/draw0；实际崩溃发生随后首次view Draw的vkUpdateDescriptorSets无效sampler 0。列表Initialize/Assign时sampler有效，正在追加Draw写入前诊断；不能把无相机更新通过扩展为完整early/abort通过。

### 实际 Prefab 事务首轮与模板断言补齐

- Game10 `runtime-manual` `py -3 Tools/VFX/prefab_acceptance.py --backend DX12 --existing-build-sha256 8E5B9445A30E33AB506C2224358E61F6188BCF009E5DF367B2EE384F2BCC006B --output out/agent/rendering-evidence/vfx-authoring/prefab-game-dx12-01` FAIL：首scene.plan返回unsafe_lifecycle，SpriteRendererComponent尚无结构编辑adapter，checks=0。原输入/二进制hash不变，正常stop。已交Prefab代理补实际适配和回归，不删组件绕过。
- 模板driver追加neutral color/intensity真实提交与status核对、start/peak/decay存活/end结束、peak drawCalls.current>0；提交计数不冒充实际像素。根独立 `unit-test` `py -3 Tools/VFX/template_matrix_test.py` PASS2tests（24×2 mock与unsupported/零draw负例）；仍需新的真实矩阵执行，旧probe02不包含新增断言。

- Game10 `runtime-manual` `py -3 Tools/VFX/resource_failure_acceptance.py --backend DX12 --existing-build-sha256 8E5B9445A30E33AB506C2224358E61F6188BCF009E5DF367B2EE384F2BCC006B --output out/agent/rendering-evidence/vfx-authoring/resource-failure-dx12-01` PASS12项，输入/二进制hash不变、正常stop。原生日志无VUID/validation/device错误（DX12调试层仍不可用，NVRHI-only）。缺PNG/模型、EFKE/SKFE错误版本拒绝；同键坏文件仍在磁盘时fresh Play使用上版缓存，旧暂停实例强度/半径保持。不是粒子状态hash或视觉验收。
- SpriteRenderer结构适配已冻结：SceneEditingService的支持列表、Defaults和Canonical复用现有反射serializer，无新反射声明。PrefabSubtreeTests新增带Sprite子实体的完整事务链与畸形Color/删除undo回归。待真实unit及新Game重跑；Early05包含此源修改一起构建，但不替代Prefab单测。

- EarlyVulkan05 `build-only` PASS75.8s，候选1f24efc5753f55964013703422d4c753a64da4c70c4be5f5ce41f25aad31021d，`runtime-manual` FAIL invalid sampler，完整证据early-vulkan-05。首Draw两处日志sampler有效，writes11→13、buffers3→5、images8均在容量内；但诊断仅每list首Draw，VUID尚未与精确调用序号/dstSet关联，不能断言该同一次调用的API边界读错。下一诊断需限定多次Draw并记录API返回，排除后续Draw才失败。

- `unit-test` `.\hectl.bat test PrefabSubtree -Configuration RelWithDebInfo -Parallel 2` PASS，HEngineTests build46.5s，日志out/agent/vfx-authoring/test-PrefabSubtree-03.log。新增Sprite结构编辑、反射字段patch/坏向量拒绝、保存重开和删除undo均真实执行通过。此测试无窗口，不能替代Game/Editor事务验收。

- 性能driver预检补齐：benchmark必须预先指定Release候选SHA并在各轮前后/最终验证，要求32帧退休后零实例/allocator恢复及failure counters无变化，所有计时有限且非负。根 `unit-test` `py -3 Tools/VFX/benchmark_test.py` PASS11项。仅数据/契约单测；实际Release性能三轮未运行，预算未变。lifecycle压力审计另外发现缺逐周期live证明、双规模门槛和内存正增长默认容差，正在修正，未将旧PARTIAL报告改为PASS。

- EarlyVulkan06 `build-only` PASS19.3s，候选3c49cddd405fae4c6cdf41321d55f7621fa50b269df9a67151f4c0d8030257a9，`runtime-manual` FAIL，完整证据early-vulkan-06。已证明ordinal1/API_RETURN正常，实际失败ordinal2/dstSetA313AC...2A3；绑定0 wrap=1/filter=331，在after-textures已sampler0，Hpp/native读值一致。排除‘同调用API读错’推断；正在追CPU Renderer SetTextures无界枚举索引的上游值，原stress包保留，官方工具只临时回编译对照。
- 根 `unit-test` `py -3 Tools/VFX/lifecycle_acceptance_test.py` PASS4项；新增20k/50k声明/包参数检查、逐周期live+退休门槛和严格无增长，报告组合保持PARTIAL；真实30周期×两规模×API未运行。

- EarlyVulkan07 `build-only` PASS20.3s，候选617c4cac020ca69ae0597bcb9b7d19557178b69429e4a7f012c87eef7fe2c70a，`runtime-manual` FAIL。Collector入口basic param已filter=-1/wrap=-1；CPU sampler校验明确throw，原数组越界不再执行，但异常收尾暴露map-size0/unmap VUID，仍需修复。完整证据early-vulkan-07。
- 资源代理用官方EDIT回编译及单Filter差分定位旧BIN offset767/new1810 offset810。根独立重读BIN验证：旧官方/派生20k/new官方96字节common完全相同，七组sampler(1,0)，单字段改Nearest只影响新BIN810。证据Docs/VFXGpuStress-Sampler-Audit.md和Temp/he-gpu-common-audit-qgg_uxo4；未改发布包。Early08新增SDK load→BasicParameter copy→node loaded→Collector地址/字段偏移诊断，运行中。

- EarlyVulkan08 `build-only` PASS50.5s，候选c579ece9097724340d6e2986c6283da4c26cf4e67c4cf24ec57fe3f6ad33fc24，`runtime-manual` FAIL。必须看完整原生stderr：SDK加载诊断在开头，外层失败摘要省略了它，不能据摘要断言日志未执行。load(version1800)已读到alpha/filter/wrap=-1，copy/node/Collector的同一basic地址与size200/offset52,84一致。
- 静态与字节证据定位旧格式差量：Depth/Kill正确结束于BIN711，旧包711..726额外16字节碰撞记录，真实common727；当前CollisionsParameter::Load对version1800(<1801)直接返回。因此不是sampler数据本身坏，而是旧样例/当前SDK的格式分支不一致。决定不猜修改所有1800解析：保留旧包拒绝回归，迁移正常stress生成到官方当前1810、EDIT/runtime同源参数。原20k/50k完整保留rendering-evidence/vfx-authoring/legacy-stress-1800并.he-keep。
- Runtime候选准入将拒绝实际builtin CPU节点的非法sampler且保持旧cache；另修异常后重复Upload导致map0，待编译/实际验证。原生shader采样错误不再靠clamp兜底。
- 原创材质离线官方SWIG compiler API实测生成DX12-only .efkmatd（无需GUI/GPU），证据Docs/VFXMaterial-CompiledCache-Audit.md；可供DX12 compiled-only正例/Vulkan wrong-backend负例。VK compiler DLL本地缺失，尚无VK compiled-only正例。driver补齐中。

### Game11 集成候选

- `build-only` `.\hectl.bat game build -Parallel 2` PASS83.4s，SHA256 1E3DD96DC99BF0D3A574A742D030410D1BDFF9A079C9674AC85C0E93F580BD96，integration-11.sha256/build-game-11.log。包含Sprite事务适配、自定义材质颜色能力、NVRHI未提交列表释放、builtin sampler候选验证与异常批次/零上传修复；旧1800压力包预期拒绝，正常stress官方迁移尚在进行。
- 根 `unit-test` `py -3 Tools/VFX/material_curve_acceptance_test.py` PASS3，compiled cache分支实际硬件未运行。Game11 prefab-game-dx12-02实际复验运行中。

- Game11 `runtime-manual` `py -3 Tools/VFX/prefab_acceptance.py --backend DX12 --existing-build-sha256 1E3DD96DC99BF0D3A574A742D030410D1BDFF9A079C9674AC85C0E93F580BD96 --output out/agent/rendering-evidence/vfx-authoring/prefab-game-dx12-02` PASS103项，source/binary hash不变、正常stop、native日志无validation/device错误（DX12 NVRHI-only）。真实Game共享scene author服务覆盖多实例、稳定UUID/parent、局部覆盖、传播、幂等、undo/redo、save/reopen；不冒充Editor UI或角色运行时动画验收。
- Game11 `runtime-manual` `py -3 Tools/VFX/resource_failure_acceptance.py --backend Vulkan --existing-build-sha256 1E3DD96DC99BF0D3A574A742D030410D1BDFF9A079C9674AC85C0E93F580BD96 --legacy-sampler-package out/agent/rendering-evidence/vfx-authoring/legacy-stress-1800/GpuSparks20k.efkpkg --output out/agent/rendering-evidence/vfx-authoring/resource-failure-vulkan-02`运行中。GPU明确enabled避免disabled拒绝伪装sampler准入；原坏包不会被新stress发布覆盖。
- 正常stress迁移保留4个作者gradient色标。官方Gradient.Sample在末端t=1输出透明0，旧手构造末采样[92,18,8,32]，其余31采样差<=1；接受并记录官方导出差异，不改工具/手补BIN/增加色标，实际fade尾部可见性待新图审查。

- Game11 resource-failure-vulkan-02 `runtime-manual` PASS17项，GPU enabled、native原始日志明确旧包filter=-1/wrap=-1拒绝；同键上版cache与旧暂停实例保持，正常stop，inputs/binary不变，VUID/device错误0。此项证明坏包安全拒绝，不替代上传异常注入回归。
- 根独立 `py -3 Tools/VFX/generate_effekseer_stress_assets.py --check` PASS、changed=0，stress-official-check-01.log；新20k SHA8c8f8bd723ff2196226e85f84179d0e0d918c2e9201b32d18217ffd12cec919a、50k SHA6eae4bf6f4219226893aa5ffcd65684f7b6832b5b7f3a409fe114b19dacf7b53。作者源/EDIT重编BIN一致，旧坏包保留，实时GPU验证仍待。
- Game11 material-curve-vulkan-02正在运行，传入官方DX12-only cache固定SHA4a2230b19e1f4bcec192f61613ecf586d2584922c7f12a8838804f1523d4398a；原material/curve正例、custom色控拒绝和VK错后端cache恢复一起检查。

- Game11 `runtime-manual` bundled Python `Tools/VFX/material_curve_acceptance.py --backend Vulkan --existing-build-sha256 1E3DD96DC99BF0D3A574A742D030410D1BDFF9A079C9674AC85C0E93F580BD96 --compiled-cache C:/Users/51906/AppData/Local/Temp/he-material-cache-audit-1abae3f0522d43179f8818f12f3db91d/OriginalCyan.efkmatd --compiled-cache-sha256 4a2230b19e1f4bcec192f61613ecf586d2584922c7f12a8838804f1523d4398a --output out/agent/rendering-evidence/vfx-authoring/material-curve-vulkan-02` PASS29项。DX12同命令替换backend/output material-curve-dx12-01，PASS28项。输入/二进制不变、正常stop、原生日志无验证错误；DX12仅NVRHI validation。
- 两后端材质/曲线人工时序审查限定PASS：材质1/2/30/60稳定青色，曲线第1帧fade空图、第2帧可见，后续记录时刻向左移动。停止相对空图最大RGB差Vulkan两项0，DX12材质1/曲线0。DX12 compiled-only真实绘制与source >16差异像素0、RGB MAE0.000007595486111111111；Vulkan只验证错后端cache拒绝与恢复，未验证VK compiled-only正例。各visual-review.json保留限定范围，完整模板矩阵仍待。
- EarlyVulkan09开始：沿用上文Early精确命令/env，并增加HE_VFX_EARLY_SMOKE_INJECT_UPLOAD_FAILURE=1；当前1810合法压力包用于真实Upload后异常注入、恢复和全部原Early断言。新SDK hook仅开发构建PRIVATE宏，尚未取得实际结果。
- 压力生命周期driver已迁移到完整1810载荷验证，仍交叉检查PROVENANCE的rate/lifetime/emit count/owner lifetime/seed。根独立 `unit-test` `py -3 Tools/VFX/lifecycle_acceptance_test.py` PASS6tests，含20k/50k正例、1800/sampler/rate篡改与保持乘积的伪造元数据；真实30周期尚未执行。
- EarlyVulkan09 `build-only` PASS162.9s，候选2979172C203AF9DB7A44EC0C0C6C72AF0254C6EEC6CE3CF00451FD5BFC0FAD75；`runtime-manual` PASS exit0/49checks。真实非空Upload异常恰一次，异常取消后Unconfigured/Update拒绝、真实Aborted计时、同进程重初始化CPU实际draw恢复，随后完整原Early流程均PASS：四次同host无camera/noDraw更新、两view不重复模拟、正GPU模拟/绘制计时、两次abort共享maxVersions1 volatile槽复用、GPU重初始化和StopAll allocator归零。完整stdout/stderr零VUID/validation/device errors，RTX3060 Vulkan validation enabled。三个“requires zero validation errors”说明性marker不属于错误。证据early-vulkan-09/report.json；不包含像素/resize/scene-switch/device重建验收。DX12同源Early01开始。
- EarlyDX12-01 `build-only` PASS5.4s（同候选hash零变化）；`runtime-manual` PASS exit0/48checks，注入恰一次及完整Early检查通过，真实Aborted三个计时endSubmission=0符合DX12丢弃语义。完整双流无错误；D3D12原生debug layer不可用(0x887A002D)，只验证NVRHI。Storage registration/cleanup busy已保留，不中断collector。Game12重新链接该SDK候选中。
- Editor验收driver补实际双流保存/hash、validation扫描、输入/二进制前后不变、正常stop/exit验证；根 `unit-test` `py -3 -m unittest discover -s Tools/VFX -p test_editor_prefab_evidence.py -v` PASS4项。Mailbox无截图接口，报告明确NOT_CAPTURED，仅作者事务验收。实际Editor仍待新构建。
- Game12 `build-only` `.\hectl.bat game build -Parallel 2` PASS31.6s，SHA1A11A3CFEE92BC2532CDC96682A32A4AEAD22AD180269C361432288DDD4A42D8，integration-12.sha256。包含双后端Early通过的SDK/异常恢复实现；Editor02通过 `.\hectl.bat editor build -Parallel 2` 构建中。压力资源计数审查确认常驻storage/uniform pool不能要求Stop时销毁，需区分槽位退休、池稳定和OS设备用量。
- `build-only` `.\hectl.bat editor build -Parallel 2` PASS108.8s，Editor02 SHA C753B1C865BC198822DDDF1640A883F098A2A6A833E4AE5DE804122BDD948AE9。`powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HAssetImporter -Configuration RelWithDebInfo -Parallel 2` PASS23.5s，Importer01 SHA9A758A1149A4731873CF0E53691ED5C9BE1E27FCFD5241ABB3E1E7377DCC4CB5。各hash/log保留out/agent/vfx-authoring；Editor真实Vulkan Prefab验收开始。Storage忙但构建/进程状态独立判定。
- Editor02 `runtime-manual` `py -3 Tools/VFX/editor_prefab_acceptance.py --backend Vulkan --expected-editor-sha256 C753B1C865BC198822DDDF1640A883F098A2A6A833E4AE5DE804122BDD948AE9 --output out/agent/rendering-evidence/vfx-authoring/editor-prefab-vulkan-01` FAIL1项后请求open失败，实际不能断言文件不存在：旧日志未保存errno。正常stop/exit0、inputs/binary不变、nativeerrors0；邮箱claim/有界IO处理候选待编译验证，不通过重试整个测试抹去失败。
- Game12/Importer01 `py -3 Tools/Content/sampling_acceptance.py --backend Vulkan --importer out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe --importer-sha256 9A758A1149A4731873CF0E53691ED5C9BE1E27FCFD5241ABB3E1E7377DCC4CB5 --existing-build-sha256 1A11A3CFEE92BC2532CDC96682A32A4AEAD22AD180269C361432288DDD4A42D8 --output out/agent/rendering-evidence/vfx-authoring/sampling-vulkan-01` FAIL材质断言：artifacts含保持覆盖的旧材质和mesh默认材质，不能要求其随作者改色。改按state.materials稳定ID核对每个作者材质，所有artifact hash/实际覆盖仍核对；根对原实际数据PASS12作者材质、伪造作者颜色明确reject。
- 同命令output sampling-vulkan-02，`runtime-manual`导入事务PASS七阶段及UV3拒绝恢复；人工visual FAIL：三截图最大RGB1，EV14搭配复制灯光1.25导致曝光不足。下一捕获统一用项目PBRTest相同100000 daylight强度，保持相机/参数/每阶段条件相同；不改已失败图或素材参数来制造视觉通过。GPU资源只读统计patch已应用源码，尚未编译，Game12不含该新增统计。
- sampling-vulkan-03七阶段command PASS，visual FAIL：100000强度导致面板高光裁切/过量光晕，不能评估采样差异。sampling-vulkan-04统一改固定1000检查光照复验中；不修改原失败图。作者参数、shader、相机和每阶段条件不因后端改变。
- 资源池观察源码已应用，GetStats(includeResourcePools=false)默认不遍历；仅显式DevAgent vfx.stats采集实际LLGI buffer身份去重count/GetSize逻辑字节/snapshot数，并附Application既有OS budget。无native分配字节冒充、无反射变化，待Game13/Editor03编译。压力driver预热5cycle后严格池/可用OS趋势检查、私有内存原标准保持；根 `unit-test` `py -3 Tools/VFX/lifecycle_acceptance_test.py` PASS9项。OS不可用记NOT_VERIFIED，完整native/fence/其他资源范围仍限定。
- Editor mailbox根审查撤销代理多余二次rename：现有SaveJsonToFile在JsonWrite.cpp已调用原子发布，不存在已证明的半写响应问题。保留请求claim+下一帧读取和仅IO有界3次重试，错误直接保存rename error_code，异常后errno明确非权威。根专属CPU5tests PASS，不替代C++编译/真实Editor复验。
- Game13构建入口FAIL（未编译）：缓存lease被占用，保留build-game-13.log，无删除锁/终止collector。转Editor03构建FAIL：Interop包含SDK内部GraphicsDevice.h导致LLGI.Buffer.h不在Runtime include范围；根拒绝扩大全Runtime include，改为自包含SDK公开BufferPoolStats.h/free bridge，在SDK.cpp内识别LLGI。候选待重编译。
- sampling-vulkan-04七阶段command PASS，人工visual FAIL：固定1000使11面板可读但normal_uv1黑。静态核实normal PNG/Linear语义/UV1/strength.4/mesh法线切线完整；灯Rotation0实际方向+Y，入射-Y与XY面板掠射，IBL0。之前只归因强度不足不完整。
- Game12 `runtime-manual` `py -3 out/agent/vfx-authoring/probe_sampling_light.py` PASS，sampling-light-vulkan-01。三场景单会话：仅灯X从0到-45、强度保留1000，法线面板立即可见但过曝；后续仅强度4/1.25对照。根顺序看light-crops，选择X=-45/intensity4作为统一采样捕获条件，12面板清晰可见。未改材质/shader来绕过；不是完整增量/另一后端PASS。最终driver已固定该条件，待全流程复验。
- GPU请求策略审查发现两个生产缺口：原初始化仅ENV开启、Low后升档不恢复；shipping也未编译隔离probe。已应用候选requested意图、quiescent质量重建/显式关闭保留、DEV_GAME+GM非PACK probe门槛。capable仍false，未提前发布后端支持；正常默认启用结构依赖未来验收后设置的capable。Low显式enable返回请求接受但actualfalse，vfx.describe.gpu给requested和enableSemantics。Game14正在编译；Early新增同进程质量策略实际断言准备中。
- Game14 `build-only` `.\hectl.bat game build -Parallel 2` PASS124.9s，SHA F5E7A89584E1F2157D5C90EC09D1E0E4899ED5DC2FCB07B06B3D982F38547142，integration-14.sha256。包含公开buffer-pool bridge、显式资源采样、requested质量策略/DEV probe隔离；尚无该候选运行证据。Editor04同源构建中，Early Main已新增真实质量策略用例待独立build/run。

- Editor04 `build-only` `.\hectl.bat editor build -Parallel 2` PASS137.0s，SHA5A75B20408C63DF76E5BB418BAFA995368B16D6766833AB3A61A3F6F47C98201；包含mailbox claim/有界IO与公开pool bridge。实际editor-prefab-vulkan-02运行中。
- EarlyVulkan10：上文同命令/env，build PASS35.7s，SHA96DD6857E36D264C6D393A5514FC6841E87BA270235B5DDE4A8F791ECE0CC2FF。runtime整体FAIL：114状态/实际工作断言均通过、注入恰一次、exit0，但完整native双流发现72条重复报错，唯一VUID-VkWriteDescriptorSet-dstSet-00320。质量切换后SDK descriptor cache复用依赖已销毁layout的sets，不能用程序PASS marker替代验证层。原始证据early-vulkan-10保留，修复中。
- EarlyDX12-02同候选 `build-only` PASS5.9s，`runtime-manual` PASS113检查、一次注入、exit0；完整native双流零validation/device错误，D3D12 debug layer不可用，NVRHI-only。质量请求/实际状态、High-Low-Medium-High恢复、显式关闭保持、忙碌拒绝、恢复后真实GPU计时/dispatch/draw与allocator退休通过。证据early-dx12-02；不替代Vulkan失败或像素验证。
- Storage只读排查：03:04:04完成轮998items/deleted25/failed0/errors0，无挂死证据。collect持全局mutex且每候选重复全量plan导致30秒注册busy；不删除锁/中断collector/解除保护。无需为VFX验收扩大修改storage范围。
- Editor04 Vulkan Prefab复验 `runtime-manual` PASS4组真实Editor主线程事务，实际覆盖两nested实例/override/传播/undo-redo/save-open-save，inputs/binary不变、stopack/exit0、完整native无error。证据editor-prefab-vulkan-02，仍非UI/动画视觉。DX12-01 FAIL0组：hectl check在SDK新改动后重编Editor为24D71A2E51E1FB9BCE878F5715354BCD902169C1705A0F926B142AE0FE596500，driver正确拒绝旧pin，正常退出；未把该次标作功能失败或通过，待冻结新Editor后重跑。
- Vulkan layout缓存根因已修复：DescriptorPoolVulkan cache按offset复用固定同构layout sets，原pipeline可在quality重建时销毁；缓存entry现持有其分配pipeline直到pool先销毁sets后释放owner。graphics/compute分别受既有drawingCount槽位上限约束，当前引用链无环。是有界常驻pipeline/layout持有，不声称quality立即释放所有native对象。
- EarlyVulkan11 `build-only` PASS14.9s，SHA855182C29BDB6EEA06783318FD5F36331D2AEFAB6B97B629E2FB03AB9CE21B94；`runtime-manual` PASS114、一次注入、exit0，完整双流零验证错误，含新quality real GPU路径。early-dx12-03仅入口FAIL cache lease busy，未运行/无新binary；重试须等lease释放，不删除锁。
- benchmark透明覆盖proxy实现与根独立CPU13tests PASS；在三轮600采样完成后另跑empty/active16/empty-repeat三次固定60帧capture，RGB max差>1/4/16覆盖数与边界，空对照漂移检查。明确合成/后处理最终图像变化，不冒充overdraw层数/alpha/性能预算；真实Release执行与帧对齐待验证。
- Importer02 `build-only` `powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HAssetImporter -Configuration RelWithDebInfo -Parallel 2` PASS26.9s，SHA6E21C6BD738E87910676A1FBEDD7A60D69C83B121DAEE91ACD946656CC1A64A0。Game15 `.\hectl.bat game build -Parallel 2` PASS11.4s，SHABE01F5427B5C3BED49890B9632AF911866F6B2779F46F12AECD9A7D963117BFC（含Vulkan descriptor owner修复；不含随后新增vfx.quality端点）。
- Game15 `runtime-manual` `py -3 Tools/VFX/lifecycle_acceptance.py --backend Vulkan --cpu-effect Assets/Effects/Authoring/hit-ember.efkpkg --gpu-effect Assets/Effects/Stress/GpuSparks20k.efkpkg --nominal-particles 20000 --existing-build-sha256 BE01F5427B5C3BED49890B9632AF911866F6B2779F46F12AECD9A7D963117BFC --output out/agent/rendering-evidence/vfx-authoring/lifecycle-vulkan-20k-01` 全30cycles实际完成，严格private-memory FAIL：首末5中位增长626077696 bytes、slope31844153.5015 bytes/cycle。全部30live工作与retired_zero、held buffer pools、allocator归零、cache/effect稳定性PASS；OS local/nonlocal unavailable=NOT_VERIFIED。Game已响应stop退出，outer stop/Storage仍待；不能将尚未写完的RUNNING报告称整体完成。
- 该轮native双流在进程退出后另存native-snapshot.stdout/stderr.log，零VUID/validation/device/error。四对paused/replay图RGB完全一致；根查看lifecycle-preview.jpg限定观察：CPU采样视觉为空，GPU仅小火花，不足完整峰值/尾部视觉或状态读回证明。pixel-observations.json明确限定。
- 内存诊断 `py -3 out/agent/vfx-authoring/probe_empty_memory.py --backend Vulkan --sha BE01F5427B5C3BED49890B9632AF911866F6B2779F46F12AECD9A7D963117BFC --gpu 1 --output out/agent/rendering-evidence/vfx-authoring/empty-memory-vulkan-gpu-on-01` 采集命令PASS11samples：同Preview无active effects，2310steps/recordings private958447616→1055174656，增长96727040。此PASS只指诊断采集，不是内存预算通过。随后gpu0空场对照empty-memory-vulkan-gpu-off-01运行中。压力每cycle虽231模拟步，但因暂停时仍绘制，recordingSerial约+2000，不能把内存增长归一为231 renderframes。
- 新源码待构建：Editor认证mailbox桥复用真实AssetPreviewPanel播放/暂停/scrub和Application presented-backbuffer捕获/完成屏障，固定实际preview帧与speed；根CPU新5+既有5 PASS，尚未实际Editor UI验收。vfx.quality公开query/set复用真实PreUpdate安全边界，matrix按cpu-High12/cpu-Low6/gpu-High6分组并set/query核对，根纯driver5tests PASS，尚未编译。模板审阅助手完整时刻分页/hash/有界JPEG，根CPU2tests PASS，永远NOT_REVIEWED，不自动给视觉PASS。
- Game15 内存对照补全：Vulkan GPUoff同场景2310steps private841904128→939687936（+97783808）；DX12 GPUoff794476544→963682304（+169205760）；均无active effects，采集成功不是内存PASS。Vulkan再仅移除RenderStats、保留validation/GPUoff，830279680→828403712（-1875968）。确认增长与统计路径相关；不能把stats-off作压力预算通过。
- 静态定位Application::Run无界submissionTimings/performanceFrames/memorySamples；Session默认RenderStatsPath空仍累计全历史且不输出CSV。Effekseer/LLGI空场Render提前return、GPUoff不Compute，已审查timing slots/lease cursors/descriptor池/rolling metrics均有界，无证据修改SDK绕过。正改为全量原始数据流式5文件（4CSV+精确wall-ms.bin），运行期只留1pending CPU frame，退出读取单列精确分位数；尚未编译/复验。
- lifecycle-vulkan-20k-01已完成外层stop/报告：commandStatus=FAIL、acceptanceStatus=FAIL、107checks、30cycles，package/budget不变、无cleanupError。完整native日志保存，内存问题是唯一已覆盖断言FAIL；不提前运行更多规模重复同一缺陷。
- Game15/Importer02 `runtime-manual` sampling-vulkan-05 七阶段+UV3拒绝恢复command PASS；根人工raster overview PASS，12面板均可见且无白色裁切，normal panel可读，材质改色、显式UV0材质替换覆盖保留、geometry宽度1→1.4边缘相接符合作者变化。所有3阶段双流native errors0。原PNG未改，sampling-crops.jpg/visual-review.json关联SHA；该overview不替代独立8filter/RT验收。DX12相同pin和灯光条件sampling-dx12-01运行中。
- Game16 `build-only` `.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2` PASS54.3s，SHA974805A94DADCF723D411D74B870EC90276B17BCC07EC00610F6298CD31A2802；包含统计流式落盘和vfx.quality端点。根独立 `unit-test` `py -3 -m unittest discover -s Tools/Agent -p test_render_stats_stream.py` PASS4；`py -3 -m unittest discover -s Tools/VFX -p test_editor_animation_acceptance.py` PASS7。统计开启空场Vulkan回归开始，尚未预算判定。
- Game15/Importer02 sampling-dx12-01七阶段command PASS，根人工raster overview PASS，12面板可见、材质更新与geometry加宽符合预期；3阶段6份native日志零错误。sampling-crops.jpg和visual-review.json记录原PNG/hash；同Vulkan限定仅overview，不替代8filter或RT验收。
- Game16统计开启空场 `runtime-manual` `py -3 out/agent/vfx-authoring/probe_empty_memory.py --backend Vulkan --sha 974805A94DADCF723D411D74B870EC90276B17BCC07EC00610F6298CD31A2802 --gpu 0 --output out/agent/rendering-evidence/vfx-authoring/empty-memory-vulkan-streaming-01` 采集PASS11点。private835350528→840179712，cycle3后稳定约840MB、后半无持续上升；不将单次空场标完整压力预算PASS。完整统计校验 `py -3 Tools/Agent/verify_render_stats_stream.py --frames-csv out/agent/sessions/20260914-034553-894-a50cb2d3806f/Saved/RenderStats/frames-17893287564337468.csv --expected-frames 12571 --require-complete` PASS：12571frames/12607submissions/967967passes/37memory，精确P95=4.82675、P99=8.55676，与native三位小数摘要一致。
- 根CPU回归 `unit-test` bundled Python `-m unittest discover -s Tools/VFX -p '*test*.py'` PASS67；`-m unittest discover -s Tools/Content -p 'test_sampling*.py'` PASS10。最初system py调用因缺PIL产生2import错误，保留python-vfx-16.log；使用已提供bundled依赖后通过，未改测试跳过。
- Game16 `runtime-manual` `py -3 Tools/VFX/lifecycle_acceptance.py --backend Vulkan --cpu-effect Assets/Effects/Authoring/hit-ember.efkpkg --gpu-effect Assets/Effects/Stress/GpuSparks20k.efkpkg --nominal-particles 20000 --existing-build-sha256 974805A94DADCF723D411D74B870EC90276B17BCC07EC00610F6298CD31A2802 --output out/agent/rendering-evidence/vfx-authoring/lifecycle-vulkan-20k-02` 完整30cycles/107checks，唯一已覆盖FAIL仍为严格private proxy：首末5median+4096 bytes，slope=-97.673846 bytes/cycle，fixed5warmup后max曾高73728 bytes。cycle11..21平台1085014016，最终1084944384；与旧版626MB持续增长明显不同，但标准未改且不豁免噪声。全部live GPU/retired zero/held buffer pools/cache回收PASS，native双流零错误，inputs/budget不变，无cleanupError。日志deque容量256与堆高水位是待证实候选，未以此豁免。
- 新压力视觉driver Tools/VFX/stress_visual_acceptance.py覆盖201平台/暂停与恢复/owner1199及1201/1300tail/1401及1431自然退休/重播201后stop；仅CPU4tests根独立PASS，实际取景待运行。新统计校验器流式读取百万pass/memory行，根CPU6tests PASS；full30原始165538帧审计进行中。
- 当前Game codegen `hectl codegen status -CodeGenHost DevGame -Configuration RelWithDebInfo --json` clean/changed0/unchanged273/deleted0。此为只读status，尚不是最终no-op hash/mtime证据；零参数status实际指未配置editor-debug，dirty managedBuildStateMissing，与当前Game无关。
- Editor05 `build-only` `.\hectl.bat editor build -Configuration RelWithDebInfo -Parallel 2` PASS107.2s，SHA6A1C4FA80D7978452151E693082B06E07B4F41F04D662EECC28C4F2D072279F7。包含动画preview/capture认证桥、统计流式与Vulkan layout owner。Storage登记/回收busy有警告，构建本身通过；editor-animation-vulkan-01已启动owned check，实际结果待。
- full30 Game16完整统计离线校验 `py -3 Tools/Agent/verify_render_stats_stream.py --frames-csv out/agent/sessions/20260914-034715-329-dc6be77a1756/Saved/RenderStats/frames-17893288367225038.csv --expected-frames 165538 --require-complete` PASS：165538frames/165574submissions/12865701passes/475memory，P95=3.734215、P99=5.753504，精确spool与CSV一致。结果statistics-verification.json。
- Editor05 `runtime-manual` `py -3 Tools/VFX/editor_animation_acceptance.py --backend Vulkan --expected-editor-sha256 6A1C4FA80D7978452151E693082B06E07B4F41F04D662EECC28C4F2D072279F7 --output out/agent/rendering-evidence/vfx-authoring/editor-animation-vulkan-01` command PASS5groups/7captures：实际scrub .15/.65、12frames×.5/2x然后pause、pause保持、非法路径/过大帧拒绝、controller打开；normalstop/input/exe unchanged/native无错。根图像姿态正确，UI视觉FAIL：1920x1080下默认DPI窗口高度超work area、timeline裁掉，controller初始nodes重叠。原PNG/hash保留visual-review.json，非鼠标可用性/事件运行验收。已加载he-editor-imgui-dpi修复布局，待Editor06重跑；DX12 Editor05同源正在运行。
- Editor05 DX12 animation01 `runtime-manual` command PASS5groups/7captures、normalStop/input/exe不变；根人工6姿态与controller图检查，姿态/暂停正确，窗口越界和controller重叠同Vulkan，visual FAIL保留。4文件UI修复待Editor06：统一work-area像素clamp、仅设计尺寸S一次、dock不干预；timeline置于详情顶部、focusgraph延后一帧。根另只改本任务新增Character.hanimctrl的5个position用于可读排布，非用户initialChanges，程序对比除position外所有JSON相同；不修改通用持久坐标语义。旧两后端证据仍绑定旧资源hash。
- Game16统计开启DX12空场 `runtime-manual` probe_empty_memory --backend DX12 --gpu 0 --sha 974805A94DADCF723D411D74B870EC90276B17BCC07EC00610F6298CD31A2802 --output out/agent/rendering-evidence/vfx-authoring/empty-memory-dx12-streaming-01 采集PASS11点，private770985984→773242880，后段约773MB平台；不是压力预算通过。全部统计 `py -3 Tools/Agent/verify_render_stats_stream.py --frames-csv out/agent/sessions/20260914-040334-669-0f30e5d8232b/Saved/RenderStats/frames-17893298168408950.csv --expected-frames 20791 --require-complete` PASS20791frames/20827submissions/1600907passes/32memory，精确P95=1.91125、P99=2.53539。
- Editor06 `build-only` `.\hectl.bat editor build -Configuration RelWithDebInfo -Parallel 2` PASS72.2s，SHABEDB1C119125494B2DE41D9728F50C867EC496E0D8180637BD002D38EB5FA0D3。editor-animation-vulkan-02及dx12-02实际command PASS5groups/7captures，normalStop/input/binary不变。Vulkan根UI复查window已入屏、节点已无重叠；visual仍FAIL：timeline仍排在长路径metadata后不可首屏见，Attack右侧仍裁切。Editor07最小修复进行：timeline+Time/Duration前置、按node-editor自带example在ed::End后调用NavigateToContent(0)；不改图坐标语义。DX12-02未作完整第二轮视觉审批，不冒充通过。
- Latest EarlyDX12-04 `build-only` `powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Parallel 2 -Run -Wait -TimeoutSeconds 180`（环境同Early协议，DX12/DEV GPU/validation/inject各显式启用）PASS15.8s；`runtime-manual` PASS113checks、一次真实upload故障注入、exit0、native双流零错误。SHA69E047CFDB695CE6E3045B0814C993A800413DDA6B1A5DC96AC14F2A5AC4FEBE；包含最新Application链接与descriptor owner SDK，质量重建/多view/abort恢复通过，不替代压力/像素/device recreation。证据early-dx12-04/report.json。
- Editor07 `build-only` `.\hectl.bat editor build -Configuration RelWithDebInfo -Parallel 2` PASS15.8s，SHAB7627553CA956BC1BDC57B8E9115F4186438D65EA2F3D57F82B6030C5F429FE0。Vulkan animation03 command PASS5/7，根图像timeline首屏可见已修，controller初次fit仍右裁Attack，尚不视觉PASS；正加严格controller state/focus诊断定位，而非继续猜调用位置。未跑此版DX12动画。
- Game16/Importer02 `runtime-manual` bundledPython Tools/Content/sampling_filter_acceptance.py --backend Vulkan|DX12 --importer out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe --importer-sha256 6E21C6BD738E87910676A1FBEDD7A60D69C83B121DAEE91ACD946656CC1A64A0 --existing-build-sha256 974805A94DADCF723D411D74B870EC90276B17BCC07EC00610F6298CD31A2802 --output out/agent/rendering-evidence/vfx-authoring/sampling-filter-<backend>-01 双API command/manual visual PASS，完整native无错。8panel频带与nearest/linear边界可读；crossAPI MAE0.00744575/max56/40pixels>16，仅观察无像素相等阈值。原PNG/hash、visual-review、cross-backend-pixels保留，限raster非RT。
- Editor08 `build-only` `.\hectl.bat editor build -Configuration RelWithDebInfo -Parallel 2` PASS52.5s，SHAE77E08486A5992E836A0457CB0A3DC26340140DB30D6701CCB11F7185BBAA941。Vulkan animation04 command PASS5/8，初次fit独立FAIL：canvas1029x827、bounds1225x537、inverseZoom1、focusExec1、canvas sizeChanges3；显式focus独立PASS：inverseZoom1.30952、visible[-61.25,-272.99]..[1286.25,809.99]含全部nodes。根controller-focused-preview.jpg确认全5节点和连线可读；不将显式成功覆盖初次失败。正修初始布局稳定后自动fit时序。
- stress-visual-vulkan-20k-01 Game16 command PASS自然RETIRED、explicit replay stop归零；根visual FAIL取景：owner XML PVA velocityY0.02/frame，scale.35 positionY.8导致发射点age1199约Y9.193、出视锥，不能用暗尾图证明消散。包未改；driver默认scale.1/positionY-.4覆盖预估Y-.4..2.0、追加age600，明确不是精确包围盒，根CPU5tests PASS，Vulkan20k-02新output正在重跑。
- Editor09 `build-only` `.\hectl.bat editor build -Configuration RelWithDebInfo -Parallel 2` PASS43.9s，SHA26277ABCCA8F509626C46C6122653B9FBC7782F728E4231BC5024593EAA6BDF7。`runtime-manual` editor_animation_acceptance.py --backend Vulkan --expected-editor-sha256 同SHA --output .../editor-animation-vulkan-05 及DX12 .../editor-animation-dx12-03 均command PASS5groups/8captures，initialFit及explicitFocus独立PASS；初始化7实际canvas帧、1attempt、非exhaust，完成后不干预用户resize。根两API人工pose/contact/controller审核PASS：timeline首屏、窗口入屏、全部5节点与连线可读、两scrub与速度姿态正确、pause viewport ROI[35,150,1155,1035] RGBdiff0。完整native零错误，输入/候选不变，normalStop。局限仍非鼠标易用性、Game事件或控制器运行过渡证明。
- 压力视觉02仍FAIL完整取景，所有非空stage RGB>16 bbox触bottom720；发现GPU gravity=(0,-.0006,0)是世界空间、不随runtime scale，life200半隐式下落12.06 world units。root新增独立VFXStressVisual.he camera(0,-5.2,-37)/rotation(pi,0,pi)/FOV60，Neutral与官方包不变；driver scale.35 position(0,-.4,0)，保守包络及相机余量校验，根CPU7tests PASS，Vulkan20k-03正在新相机运行。
- 新固定包络相机下 `runtime-manual` bundledPython Tools/VFX/stress_visual_acceptance.py --backend Vulkan --nominal-particles 20000 --existing-build-sha256 974805A94DADCF723D411D74B870EC90276B17BCC07EC00610F6298CD31A2802 --output .../stress-visual-vulkan-20k-03 --gpu-probe command及根manual visual PASS13stages。DX12同20k .../stress-visual-dx12-20k-01、Vulkan50k .../stress-visual-vulkan-50k-01 同样command/visual PASS。各stage RGB>1/4/16均留边、不再出视锥；pause RGBdiff0、自然retired/explicitstop与empty RGBdiff0，mid-plateau/ownerend/tail可读，50k明显更密。完整native零错误，输入/候选不变；这不是完整30cycle内存或Release性能通过。
- DX12 50k第四组合 `runtime-manual` bundledPython Tools/VFX/stress_visual_acceptance.py --backend DX12 --nominal-particles 50000 --existing-build-sha256 974805A94DADCF723D411D74B870EC90276B17BCC07EC00610F6298CD31A2802 --output out/agent/rendering-evidence/vfx-authoring/stress-visual-dx12-50k-01 --gpu-probe command及根manual visual PASS13stages；检查有界contact sheet、三阈值包络与完整native双流，pause/tail/stop RGBdiff0，原图hash关联visual-review.json。四组合视觉齐全，严格内存与Release仍单独待验收。
- Game16完整24模板×两场景 Vulkan matrix 已启动：`runtime-manual` bundledPython Tools/VFX/template_matrix.py --backend Vulkan --existing-build-sha256 974805A94DADCF723D411D74B870EC90276B17BCC07EC00610F6298CD31A2802 --output out/agent/rendering-evidence/vfx-authoring/template-matrix-vulkan-01。尚未判定；CPU High/Low与GPU High分组真实设置质量。骨骼重导入缺口已授权独立工具实现，复用原生Assimp baked入口与content_sync事务，禁止改原始素材或用手改场景冒充重导。
- 全模板首轮已发现真实时序FAIL：旧CPU CommonValues/GenerationTime字段被1.80.6编辑器忽略，aura-arcane实际默认interval1导致186+2帧便结束，hit-ember默认interval1导致40+23帧尾部超出配方结束。根修正authoring.py为CommonValues/Generation/GenerationTime，显式工具最小interval1e-5；inspection核对MaxGeneration/Life/interval/offset（含已知省略默认）。GPU owner Life由frames+2对齐frames，StopEmit仍保留已发射粒子完成寿命，不缩短GPU粒子life。旧matrix继续保留完整失败基线，新包尚未发布以保持其输入身份。
- `unit-test` `py -3 -m unittest discover -s Tools/VFX -p test_authoring.py -k timing_roundtrip` PASS1：隔离真实工具编译burst/sustained/defaultinterval，旧flat字段被回读检查拒绝；早期命令发现工具将0 clamp到1e-5，已按工具真实下限显式生成，没有放宽检查。`py -3 out/agent/vfx-authoring/verify_all_recipe_timing.py` 隔离真实编译24/24 PASS，report compiled-timing-all-01.json；不是GPU视觉验收。
- 骨骼增量工具4文件已落地，根独立新CPU10、原静态16 PASS。`runtime-manual` `py -3 Tools/Content/baked_model_acceptance.py --project Project --importer C:/WorkSpace/HEngineDev/out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe --importer-sha256 6E21C6BD738E87910676A1FBEDD7A60D69C83B121DAEE91ACD946656CC1A64A0 --report out/agent/content/skeletal-reimport-native-01` 原生12stage report PASS，仍等待outer Storage完成。仅Tools host/CPU，无GPU并行；原始Cactus/Importer身份不变，source time×1.5/2、stable IDs、scripts/materialoverride、引用删除拒绝/unusedclip移除registry、原生坏buffer拒绝、两journal故障恢复。运行画面/Prefab刷新/bind编辑未覆盖，不宣称外部controller引用自动迁移。
- 本轮磁盘曾降至2.13GiB。clean preview确认reclaimableBytes=0，约53.7GB历史受保护；未删除受保护历史。显式 `.\hectl.bat clean cache -CachePath out/agent/tests-relwithdebinfo -WhatIf --json` 可回收6728114057bytes，随后同命令去WhatIf部分回收6031992259bytes，剩余被安全移出缓存命名至tests-relwithdebinfo.retired-15cf6e1703604445acef953dc29c1c06，WinError145目录非空，清理整体记PARTIAL/FAIL。当前Game/Editor/tests-debug未动，可用约7.86GiB。两个JSON及clean-preview-current.log保留精确路径/保护原因。
- 新角色16阶段runtime driver与170/-170任务独立fixture已完成，根CPU5 PASS；实际尚未运行，骨骼socket/动画热重载不在覆盖内。新memory_plateau_probe.py根CPU4 PASS，准备在同paused模拟点持续采ownedPID，无新play周期；仅诊断，不改变严格5warmup/无增长标准。Reference RT采样测试正改为独立engine-owned入口，避免依赖用户PBRTest文件与写死DXIL目录，尚未构建。
- 续接核实：全模板 Vulkan01 完成48effects，command FAIL36（CPU High12/Low6/GPU High6分别按两场景），完整12条native流零错误；native-review.json保留逐项失败。旧输入242文件已封存pre-timing-fix-inputs.zip，未用新包覆盖旧证据身份。
- focused template-timing-vulkan-02 六配方 command PASS、native6流零错误，但根人工environment-arcane page-01视觉FAIL：峰值裁切且frame348为空。进一步定位GPU batch发射应以ceil(count/rate)计算末批，不能套CPU分数interval。当前GPU life=frames-(count-1)//emitPerFrame，environment-arcane由91改187，owner378；新增专用TemplateNeutral/TemplateGame相机生成器，原场景不改。build-library-timing-03.log 24编译PASS；真实timing CPU1 PASS；library-noop-timing-02.json目前记录的是timing03包的24项hash/mtime零扰动（文件名沿用，不能引用作旧包证明）。
- 骨骼native01已外层完成；native02命令同native01但--report out/agent/content/skeletal-reimport-native-02，实际13stage PASS，追加初始/最终精确scene/state snapshots、Mouth bind一致修改+所有clip匹配平移。Walk时长0.541666687→1.083333374，15资源闭包校验PASS；尚非运行视觉或Prefab刷新证明。CPU新11/旧静态16/runtime driver3 PASS。
- memory-plateau-vulkan-01 Game16 diagnostic COMPLETE、acceptance NOT_ASSESSED：一次20k play201/stop30后无新Agent请求，持续同PID300samples/30s，模拟updateSerial不变、presented+15632。private841560064→839512064→839471104，无观察到上涨；完整native双流无错误。此诊断不能覆盖strict30cycle内存FAIL，也未修改原阈值。
- SceneEditing export首次unit FAIL原因是fixture未设置FollowCamera.bHasTarget，inactive引用应被忽略。已只修fixture启用目标并验证结构化拒绝/无文件/author不变。retest02 build-only PASS12.6s，测试最终结果等待Storage收尾；未先写unit PASS。
- Game17 build-only `.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2` PASS44.9s，SHA0B81B6EC8C91E2AEB91389045DCF4981007F260BA1AB2B2E22E504149BB2EDC9，包含scene.export_prefab。Storage busy导致登记/回收deferred单独保留。Editor10构建及template-timing-vulkan-03实际六配方验证已启动。
- 缓存残余目录的后续显式删除被自动批准审查拒绝，原文“blocked by policy”，未提供更具体原因；保留该retired目录，未改用其他路径/方式绕过。
- export02最终 `unit-test` `.\hectl.bat test SceneEditing -Configuration Debug -Parallel 2` PASS，native行05:48:41 HEngine [Test] PASS name=SceneEditing；无窗口测试完成。Editor10 `build-only` `.\hectl.bat editor build -Configuration RelWithDebInfo -Parallel 2` PASS103.6s，SHAFF6CD65359C2697DE11ABABA68BAA88FFFC49FD3C703F59C27C91C83EC0C3DD6，尚未本候选Editor实际验证。
- baked_model_adapter真实引用扫描补正 .hprefab，限制为明确动画/骨架字段；根 `unit-test` `py -3 -m unittest discover -s Tools/Content -p test_baked_model_sync.py` PASS13（11.255s，baked-sync-cpu-03.log）。包括v2 entities/nested overrides、legacy、scene/controller拒绝删除及registry不变、普通字符串不误判；native02资源与证据未动。
- focused03六配方 `runtime-manual` template_matrix.py（Vulkan/Game17、六recipe同02、--scene neutral、output template-timing-vulkan-03）command PASS6，完整native6流零错。根已逐页审阅12 bounded sheets：environment GPU峰值入镜、decay348仍可见、end382无残留确认修复；5项时序/取景visual PASS。trail-arcane visual FAIL：水平运动却仍竖直细线，正在核对实际继承/方向契约；总visual FAIL，不把command PASS当全模板完成。visual-review.json与全部原图hash保留。
- `unit-test` `py -3 -m unittest discover -s Tools/VFX -p test_authoring.py` PASS7（8.611s，authoring-all-tests-timing-03.log），包括当前batch寿命、真实编译/no-op/失败恢复。
- 综合integrated-vulkan-01已到实际Game scene.plan，FAIL read_only_field Camera.mAspectRatio，viewport拥有该字段；正常stop，源/候选身份未变。修验收工具生成事务时不得包含只读字段，保留此失败。骨骼独立runtime首命令报告SHA大写被工具精确比较拒绝、尚未创建output；以同SHA小写启动skeletal-runtime-vulkan-01，尚待实际结果，两个shell日志分别保留。
- trail真实锁定Core/SDK审计确认Always=2会让旧粒子持续读取当前parent矩阵；runtime forward基准local -Z，原trail方向Y导致竖直漂移。仅CPUtrail三bind改WhenCreating=1、三recipe direction=[0,0,-1]，sharedtoon不改；inspection校验bind真实默认2及PVA XYZ真实默认0。agent临时真实编译3正例/Always省略Y负例与安全回归6PASS，根已启动build-library-trail-04与完整CPU复验。focused03旧66files已先封存pre-timing-fix-inputs.zip。
- skeletal-runtime-vulkan-01实际FAIL Missing bounded runtime probe checkpoints，native明确ModuleNotFoundError Scripts/BakedModelRuntimeProbe；正常finally stop，未进行final case。当前PythonRuntime::RegisterModule仅去.py，斜杠资源路径未经模块名规范化，已有dot路径可工作。正最小兼容修复并补embedded PythonRuntimeContract，不能用改fixture为dot掩盖实际资源路径缺陷。
- SceneSmoke新Reference/DepthShadow源码完成，D32实际Depth与DirectionalShadow slice0读回覆盖；首次build-scene-sampling-01在仍运行的Game session缓存lease被拒绝，未发生编译，不删锁/不绕过。session退出后build-scene-sampling-02正常重新启动。
- 当前trail04库 `py -3 Tools/VFX/authoring.py build-library` 全24发布成功；根 `unit-test` `py -3 -m unittest discover -s Tools/VFX -p test_authoring.py` PASS8（14.032s）。`py -3 out/agent/vfx-authoring/verify_library_noop.py out/agent/vfx-authoring/library-noop-trail-04.json` PASS24 changed=0/hash与mtime全部无扰动；helper现要求新输出路径，拒绝覆盖旧报告。
- SceneSmoke02 `build-only` `powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineDX12SceneSmoke -Configuration RelWithDebInfo -Parallel 2` PASS104.0s，SHA4E9C91B0E86FE7E2505C3DD6F7CF4BCC166F1F9321C8DD1407CFAC0E270FF2FB。codegen dirty tracked FrameCaptureSource.h但changed=0/unchanged273/deleted0；尚不是完整hash-mtime no-op审计。两种采样GPU实际尚未运行。
- Python模块路径修复源码冻结（PythonRuntime.cpp及PythonRuntimeTests.cpp），真实嵌入6合法+15非法注册测试待执行；root `hectl test PythonRuntime -Configuration Debug -Parallel 2` build进行中，不能预写PASS。
- `unit-test` `.\hectl.bat test PythonRuntime -Configuration Debug -Parallel 2` PASS（build87.0s，06:05:31原生[Test] PASS），真实嵌入脚本路径合法/非法回归包含在内。Game18 `build-only` `.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2` PASS85.4s，SHADB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE，包含Python路径及DepthShadow capture枚举；Editor11构建中。
- 脚本生命周期复核发现两任务probe写OnDestroy但legacy真实回调为Stop；已改VFXIntegratedAcceptance/VFXCharacterAcceptance为Stop，Start保存UUID避免owner失效阻断，PresentationPlayer.Stop容忍部分Start失败，PresentationDemo原继承正确。根unit-test integrated13/character5 PASS，无C++回调契约变动；实际cleanup仍待当前Game18复验。
- skeletal-runtime-vulkan-02已以native02小写SHA及Game18启动，替代01路径导入失败重跑；当前候选/场景/clip闭包固定。磁盘1.7GiB，tests-debug已完成所需新unit，但cold-cache WhatIf被Storage collector busy拒绝，未进行实际删除，待锁自然释放后重新预览。
- Editor11 `build-only` `.\hectl.bat editor build -Configuration RelWithDebInfo -Parallel 2` PASS183.0s，SHA143FC580AA17D89BE392F875E316A74734F933DF8638B6BC184C3D5FBE989F72，包含脚本路径兼容和只读DirectionalShadow capture，尚未该候选UI实际复验。
- skeletal-runtime-vulkan-02：脚本导入已修复，但probe期望初始0进度实际0.03076923266，FAIL且正常stop。核实Animator.Play只设clip/play状态、不重置mProgress，已在probe添加真实Stop→Play；CPU替身改为Play不重置且初始非零，防止虚假通过。顺带报告SHA比较统一大小写；root unit-test test_baked_model_runtime_acceptance.py PASS3（9.388s）。
- skeletal-runtime-vulkan-03 Game18 初/末各3实际归一化进度checkpoint、完整palette/权重、引用身份全部command PASS、native零错误。但全部6PNG RGBmax=0，根initial0 bounded preview确认全黑，visual-review.json明确FAIL。显示fixture Camera EV14/dir1.25且没有Preview的Environment曝光覆盖；当前仅显示副本camera固定改EV0.3219280948873623，原native场景/资源不改，准备04。综合driver也加入实际EnvironmentLightComponent创建，保留Preview空HDR/IBL0/同曝光并pin原场景。
- 首material-reference-vulkan-01仅外层helper失败：WindowsPowerShell5.1将子进程非致命Storage stderr当NativeCommandError，ErrorAction Stop提前终止；未启动GPU，不记引擎FAIL。helper修正完整保留stderr并按子进程exitCode判断，02已重新启动，待实际marker/native读取。
- Vulkan Reference `run-material-smoke.ps1 -Backend Vulkan -Mode reference -LogPath .../material-reference-vulkan-02.log` actual FAIL capability guard：当前Vulkan Baseline Raster明确AS=false/pipeline=false，NOT_SUPPORTED_BY_CURRENT_BACKEND_PROFILE，0/22；不是新GPU粒子失败，也不扩做VulkanRT启用。完整native已复制hash至material-reference-vulkan-02/report.json。
- DX12 Reference01 base纹理/normal/alpha+前8采样PASS，nearest FAIL .711548。真实PFM统计3591纯绿/448含1/4左纹素/53含1/2/4含3/4；UV1=.5恰在point不连续边界，三角重心舍入跨界。仅Main nearest改.625，linear恢复.5，不改shader/阈值。Reference02新SHA46F76DE141E16B915E096C027D03721C44629D05CD228E2A723C15ECDD33C9F0，nearest通过、linear-sRGB-before-filter FAIL .002197，正在基于实际PFM继续归因，不放宽.002。
- DepthShadow `runtime-manual` `powershell.exe -NoProfile -ExecutionPolicy Bypass -File out/agent/vfx-authoring/run-material-smoke.ps1 -Backend Vulkan|DX12 -Mode depth-shadow -LogPath out/agent/vfx-authoring/material-depth-shadow-<backend>-01.log` 两API实际PASS10/10，完整native零validation/device/error；每API Depth/DirectionalShadow slice0各5比较，14 raw+14 sidecar。Vulkan artifacts.json已hash28files/3676582bytes，非pass名称推断；旧SHA4E9C...与DX12新SHA46F7...按各report区分。仅CPU opaque MASK单cascade，不宣称gpu-driven/skinned/all cascades或RT。
- 冷tests-debug第三次WhatIf已不忙但发现 .he-keep，内容“Active test build cache relocated for native4K task; preserve.”，创建9月12日。保留，不删除标记/缓存。仅尝试当前task game-relwithdebinfo obj/lib/pdb透明压缩，两次都实际0 files processed；1470文件原本均NTFS compressed，约5866456037逻辑bytes，全部hash/size/mtime无变化。报告明确NO_FILES_PROCESSED，不能把同期可用空间变化归因本操作，未删除文件/改exe/source。
- 当前baked-runtime-vulkan-04已用Game18、cameraEV.321928固定显示曝光启动；native02闭包不改，准备复核真实可见姿态。
- 2026-09-14 继续：Reference DX1203根复核实际22/22 PASS，base/final marker齐全、nativeErrors=[]，exe SHA46f76de141e16b915e096c027d03721c44629d05cd228e2a723c15ecdd33c9f0。仅Reference启用HE_TEXTURE_SAMPLING_EXPLICIT_FILTER，Load后解码/插值；本轮shader DXIL/SPIRV构建54.7s PASS。精确runtime-manual命令：powershell.exe -NoProfile -ExecutionPolicy Bypass -File out/agent/vfx-authoring/run-material-smoke.ps1 -Backend DX12 -Mode reference -LogPath out/agent/vfx-authoring/material-reference-dx12-03.log。report/native/hash、26实际采样产物和shader身份均在material-reference-dx12-03下；只覆盖LOD0，不扩大RT mip/实时RT/VulkanRT声明。DepthShadow DX12的28产物hash亦补齐。
- 骨骼Vulkan04 command PASS但6图全黑，未通过视觉；原因是Camera bOverrideExposure=false导致EV值未采用。显示副本设置该标记true，unit-test test_baked_model_runtime_acceptance.py PASS3。Vulkan05启动，initial pose0现可见但光照极暗（RGBmax57、4028非零像素），不能仅据非黑宣布皮肤视觉PASS，继续检查实际方向光与曝光。
- unit-test `py -3 Tools/VFX/test_integrated_acceptance.py` PASS14（0.401s），日志integrated-cpu-14-root-02.log。先前`-m unittest Tools.VFX.test_integrated_acceptance`因模块搜索路径失败，保留日志，不作为测试通过。
- integrated-vulkan-02 实际scene.plan op8 EnvironmentLightComponent返回unsupported_component，FAIL、正常stop，原输入/候选未改。当前driver仅用支持的Camera+Directional创建，Camera覆盖曝光true，unit-test `py -3 Tools/VFX/test_integrated_acceptance.py` PASS14；尚未新实际集成通过。
- `runtime-manual` `py -3 Tools/VFX/template_matrix.py --backend Vulkan --existing-build-sha256 DB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE --recipe trail-ember --recipe trail-arcane --recipe trail-frost --scene neutral --output out/agent/rendering-evidence/vfx-authoring/template-trail-vulkan-04` command PASS3、native2流零错误；root依次审查6页与arcane原比例peak，visual PASS3，均沿X移动展开，decay减弱，end/retired相对empty maxRGBdelta=0。仅CPU High neutral，不替代完整矩阵或转弯检查。首误写trail-toxic在启动前selector拒绝，日志04保留，实际为04b。
- 骨骼05两case各3command PASS，但6图根crop审查因背光过暗visual FAIL。RenderWorldExtractor取local+Y作为光线方向，旧灯使camera -Z正对面NoL=0；保持EV0.321928，仅显示副本模型yaw pi以显面部，key白光4 Rotation[1.919862177,-.436332313,0]，fill白光1 Rotation[1.745329252,.610865238,0]无阴影。源场景/材质/骨骼不改，unit-test `py -3 Tools/Content/test_baked_model_runtime_acceptance.py` PASS3（5.793s）。
- `runtime-manual` `py -3 Tools/Content/baked_model_runtime_acceptance.py --native-report out/agent/content/skeletal-reimport-native-02/report.json --native-report-sha256 <该report SHA256> --backend Vulkan --game-sha256 DB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE --output out/agent/content/skeletal-runtime-vulkan-06` command PASS6，initial/final原生错误各0，原生证据不变。root全帧initial0+全部6实际RGB crop审查visual PASS：面部/四肢/刺完整、无明显拉伸/脱落/裁切，0/.25/.5姿态变化与重导入前后轮廓一致。图/像素/hash/visual-review均归档，仅对应姿态，不宣称全动画或Prefab传播通过。DX12同fixture首轮已启动。
- 内存只读复核未发现测量bug：lifecycle-vulkan-20k-02真实stop30步、排除前5cycle正确；剩余25点max+73728/median+4096严格FAIL保持。被动plateau延后释放不能抵扣实际结果；未增加预热/修改阈值。
- `runtime-manual` 骨骼DX12使用与Vulkan06同一driver/Game18/native02：`py -3 Tools/Content/baked_model_runtime_acceptance.py --native-report out/agent/content/skeletal-reimport-native-02/report.json --native-report-sha256 <report当前SHA256> --backend DX12 --game-sha256 DB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE --output out/agent/content/skeletal-runtime-dx12-01` PASS6，两个case nativeErrors.count均0；根全部6 RGB crop审查visual PASS，边界完整/面部肢体清晰，报告hash和像素记录已写。只声明实际采样姿态。
- integrated显示主光同步为白色4、+Y契约下照向-Zcamera，source actor以真正scene transform事务设置yaw pi；保存后epsilon核对实际float32值，重导入按保存前实际值验证scale/yaw保留。`unit-test` `py -3 Tools/VFX/test_integrated_acceptance.py` PASS15（0.382s）。integrated-vulkan-03当前已启动，尚未通过。
- DX12 trail定向复验 `py -3 Tools/VFX/template_matrix.py --backend DX12 --existing-build-sha256 DB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE --recipe trail-ember --recipe trail-arcane --recipe trail-frost --scene neutral --output out/agent/rendering-evidence/vfx-authoring/template-trail-dx12-01` runtime-manual command PASS3、完整native2流0错误；root依次审查6页visual PASS3，末帧及stop退休图与empty maxRGBdelta均0。两API当前仅3trail CPUHigh neutral通过，完整矩阵仍待执行。
- integrated-vulkan-03已实际export.Actor PASS、两个实例创建与覆盖事务PASS，但author_display_transform_applied以Euler分量比较误判[0,pi,0]与[pi,-8.74e-8,pi]。按当前GLM Euler→quat→矩阵改等价比较，容差1e-6；Scale原容差不变、重导入原始JSON严格比较不变。root unit-test `py -3 Tools/VFX/test_integrated_acceptance.py` PASS16（0.423s），包含真实等价案例与错误朝向负例。
- integrated-vulkan-04前述断言已过，实际probe sequence1两个Animator.ConsumeEvents均空，FAIL；normalStop=true、nativeValidationErrors=[]、binaryUnchanged=true。正在定位真实事件/控制器加载时序，不删除/降低事件断言。
- .agents/skills/he-vfx-authoring/SKILL.md及references/runtime.md新增；root阅读并官方quick_validate PASS（现有Anaconda无关matplotlib .pth启动warning另记）。Skill不声明完整矩阵/严格内存/Release/生产GPU通过。3个无历史Agent已分别真实CPU编译新效果、3变体、unsupported参数失败恢复；全部独立nl-* id未污染canonical24。当前GPU独占委派skill_trial_create实际预览，其余仅CPU，后续6类任务与root复核未完。
- 独立Skill试用nl-low实际暴露fallback lookup仅canonical的问题，high-build.log原FAIL保留。authoring.resolve_fallback现在先canonical、否则已发布作者目录；独立回退必须为指定id的CPU，recipe hash及运行包hash匹配build.json，否则明确拒绝。先build CPU再build GPU，无自动转换/切换；不污染24库。root `unit-test` `py -3 Tools/VFX/test_authoring.py` PASS9（14.042s），含缺失发布、篡改包、错误id/GPU回退拒绝。两trail实际矩阵在工具修改前各已归档36输入文件（历史helper文件名pre-timing-fix-inputs.zip，本次实际是pre-fallback-fix归档）。
- integrated-vulkan-05加载控制器后已越过事件空数组失败，但probe调用GetEffectPosition回传tuple时使用v.x导致FAIL；normalStop=true、nativeAnimationErrors=[]、nativeValidationErrors=[]。已按VFXBinding.hpp真实py::make_tuple契约使xyz同时接受三元tuple和vec3，非三元拒绝。root `unit-test` `py -3 Tools/VFX/test_integrated_acceptance.py` PASS19（.415s）。尚未新的综合实际PASS。
- 新效果Skill首次DX12 command PASS但独立agent visual FAIL，峰值29x29过小；源/01保留，仅独立近景镜头变为[0,1.2,-2]，root现执行preview-dx12-02，不改配方掩盖取景失败。三变体DX12真实command PASS3/cleanup PASS，未审图前不称视觉PASS。外部效果候选真实独立XML→官方1.80.6编译→安全包，现仅staged for owned preview，final import登记待真实图像通过。
- integrated-vulkan-06已通过首轮真实动画事件与表现probe，随后reimport source-owned Transform冲突正确拒绝，normalStop=true。没有修改content_sync所有权规则；当前driver记录v1实际baseline，检查自己写入的display值未被外部编辑，再事务仅恢复Scale/Rotation→save→reimport→显式重新应用display与v2controller→export/sync。脚本/controller/Rigidbody/Capsule和两实例覆盖仍逐项验证。root `unit-test` `py -3 Tools/VFX/test_integrated_acceptance.py` PASS20（.380s），含真实merge_scene未restore拒绝。integrated-vulkan-07现运行中。
- 独立模板fallback工具修改后canonical build-library 24项完成（23compiled、1unchanged；仅此前test已重编explosion）。root `py -3 out/agent/vfx-authoring/verify_library_noop.py out/agent/vfx-authoring/library-noop-fallback-05.json` PASS24 changed0、输出hash/mtime零扰动。三trail EFKE与可编辑XML相对归档仍完全同hash，变化仅包provenance、PROVENANCE.json/build.json及工具源码；新完整矩阵仍需最新包运行。
- Skill create DX12近景02独立审图PASS，root额外查看真实peak/decay并对原PNG验证start/end/retired各maxRGBdelta0、无裁切；root-review.json另存。三变体01 root查看三峰图同圆环轮廓且橙/蓝/紫可辨，独立审图有限PASS但亮起/保持时序需补采样，已准备12阶段dense脚本未运行。low新高配方实际compile PASS9changed，独立fallback hash相符、no-op9输出hash/mtime无扰动。
- Skill recovery实际owned DX12已PASS16checks/cleanup PASS，参数attraction_strength/collision_restitution真实拒绝、非法build拒绝、旧实例和发布输入保留、重新加载与新实例自然终止。根原调用附带--help，但task脚本未定义CLI故实际执行；GPU当时空闲、任务已授权且正常finally stop，明确记录该入口误判，不把它算help检查。独立代理现离线审图/日志，之后仅task脚本增加安全帮助入口。
- Root重新打开官方 https://effekseer.github.io/en/contribute.html 与 https://effekseer.github.io/en/contributes/Pierre02/index.html ：Pierre vol.2位于明确CC0样例段，作者及Fire Ball条目相符；新外部素材仍staged等待owned预览，不提前final_import登记。
- 综合Vulkan07和DX12-01 `runtime-manual` 当前Game18同工具实际command PASS26/normalStop=true；两者nativeAnimationErrors/nativeValidationErrors均空、输入/原源/binary不变。Vulkan07三次stage每实例各真实1条integrated-step（v1 timestamp135.41667，v2 169.27084），normalized进度约.35；最终四轨cleanup全部passed；真实malformed XML编译进程exit0但无输出，按无产物正确判失败并保留旧包。
- 视觉仍FAIL：Vulkan07第一PNG与后四图仅1pixel/maxdelta1，后四PNG完全一致。root看首图三个角色挤在中间，未能确认粒子/recipe-v2颜色半径变化。不是从文件大小推像素一致，实际RGB统计已写visual-review.json。根因只读确认Level顺序Animation→RenderSystem3D→PythonScript；脚本最后才play并写receipt，暂停状态立即capture前无额外SDK update；fresh V2也是play后立即pause。旧driver无vfx.stats调用，不补称draw计数证据。当前编辑专用fixture/驱动以推进真实帧后采集，再复验；绝不把26command PASS当visual PASS。
- 恢复Skill独立QA PASS，root额外看before-invalid/supported-adjusted/fresh-decay并对原PNG验证非法修改前后精确一致、fresh峰与旧峰一致、natural end/stop空场。16checks PASS仅证明不支持参数拒绝保护和恢复，不实现吸引/反弹；task脚本后补真实argparse --help且旧执行副本hash保存。
- 综合Vulkan08 `runtime-manual` `py -3 Tools/VFX/integrated_acceptance.py --backend Vulkan --existing-build-sha256 DB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE --importer C:/WorkSpace/HEngineDev/out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe --importer-sha256 6E21C6BD738E87910676A1FBEDD7A60D69C83B121DAEE91ACD946656CC1A64A0 --output out/agent/vfx-authoring/integrated-vulkan-08` command PASS26、normalStop=true；root五张实际全帧预览逐张视觉PASS，记录于visual-review.json。真实8帧warmup后draw/vertices正值，三模型分开、实例挂点橙色、v2新播与失败缓存恢复青色可见。限制：entity-local socket非骨骼挂点，不能从不同播放scale定量比较半径，音频仅开始/停止回执，非音质或性能证据。DX12-02目前command PASS26/正常stop，视觉待根复核。
- 三变体dense `runtime-manual` `py -3 out/agent/vfx-authoring/skill-trial-variants/preview-dense.py --backend DX12 --output out/agent/rendering-evidence/vfx-authoring/skill-variants-dx12-02-dense` PASS；独立Agent核对36图/身份/native，并有VISUAL-REVIEW.md；root逐张看橙/蓝/紫三张12阶段sequence图，确认亮起、约1秒保持、渐隐、终止空场。仅DX12中性镜头CPU三变体、标称半径，非完整库/游戏背景/性能。
- codegen只读状态 `.\hectl.bat codegen status -CodeGenHost DevGame -Configuration RelWithDebInfo --json` 与Editor同命令均clean、dirtyReasons=[]、changed=0/deleted=0/unchanged=273；codegen-game18-status.log与codegen-editor11-status.log。尚未本候选run前后hash/mtime零扰动证明，不把status等同完整no-op运行。
- 综合DX12-02根依次查看全部5张1280预览，橙色旧实例、青色新版本/失败缓存、第三轮青色实例可辨；command PASS26 + visual PASS（visual-review.json），native双流hash及输入身份由qa/offline-audit.json核对。与Vulkan08均为entity-local socket等已注明范围，不外推骨骼挂点/定量半径/性能。
- 修改Skill `runtime-manual` `py -3 out/agent/vfx-authoring/skill-trial-modify/compare.py --backend DX12 --existing-build-sha256 DB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE --output out/agent/vfx-authoring/skill-trial-modify/compare-dx12-01` PASS2、正常stop、独立与root视觉PASS。橙→蓝白，45帧原已结束而修改版仍可见，65帧均空；同相位像素范围约1.5倍，不称每一相同时刻均1.5倍。原canonical三元数据包变化是父任务fallback重编，不能当本试用原包保护证据；独立两包/场景/候选/图/native身份一致。
- Low Skill `runtime-manual` `py -3 out/agent/vfx-authoring/skill-trial-low/nl-low-owned-compare.py --backend DX12 --existing-build-sha256 DB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE --output out/agent/vfx-authoring/skill-trial-low/compare-dx12-01 --gpu-probe` command PASS两档、自然结束/allocator及active恢复；root顺序查看Low/High五阶段序列，Low衰减很暗，视觉可读性尚未验收，独立QA继续。不把GPU开发probe当生产开放或性能通过。
- C盘空间耗尽导致template-full-vulkan-02实际FAIL（首CPU High组途中vfx-control写盘失败）；normalStop无cleanupError、当前无引擎进程、输入/候选不变，不能作为完整矩阵通过。最终stub驱动写入失败并由Agent移除本次空文件；日志环候选仅patch/bases落盘，notes可能截断。对4个已停止session的23个统计文件尝试NTFS压缩，hash/mtime检查0变化，但仍FreeSpace=0且审计写入失败，不能宣称释放。
- Storage忙进程自然结束后，`.\hectl.bat clean cache -CachePath out/agent/editor-relwithdebinfo -WhatIf --json` 确认reclaimableBytes=8105451737/failed=[]；同命令去WhatIf实际deletedBytes=7364568511（逻辑），因WinError145目录非空部分失败。剩余仅工具返回精确retired路径out/agent/editor-relwithdebinfo.retired-d4824a1dde1f4766864359355e71c52f，未手动续删。Editor11及Importer02构建缓存已失效，后续需重新构建，不再引用其路径为现有产物。Game18仍保留。旧tests retired被自动审批拒绝的删除未重试。
- Game18当前stub实际验证 `runtime-manual` `E:/Python/Python311/python.exe out/agent/vfx-authoring/run_final_stub_acceptance.py --existing-build-sha256 DB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE --backend DX12 --output out/agent/vfx-authoring/stub-final-game18-01` PASS9checks、normalStop=true、ENV恢复、input/candidate不变、nativeErrorLocations=[]；manifest候选SHA匹配，生成14模块changed=0/deleted=0/unregisteredCppTypes=[]。原argv仅原报告留盘，不在安全汇总打印。此为当前Dev接口，不证明Shipping隔离。
- Stub CPU `unit-test` `E:/Python/Python311/python.exe Tools/Scripting/TestPythonStubs.py` PASS5 (.108s)；同解释器 `Tools/Scripting/CheckPythonStubs.py` PASS errors=[]；PYTHONPATH临时指向out/agent/python-typecheck后 `E:/Python/Python311/python.exe -m mypy Engine/Tools/HEngineStub/HEngine --no-incremental --follow-imports=skip` PASS14source，finally恢复ENV。日志stub-final-game18-{tests,check,mypy}.log。
- 视觉矩阵磁盘修复仅工具：Session新增keyword render_stats默认true保留所有原调用行为，template_matrix显式false；VFX GetStats和绘制/生命周期/质量/回收断言仍保留，报告明确continuousCSV和GPUtiming不采集。本矩阵原已排除shipping/performance，性能工具默认不变；未改任何C++或验收阈值。旧preview执行源码从改动逆向重建并与失败02记录SHA b8942f0ad8b07b6bce5815e5eaa28b1bdeaa68df5fb0d68ed5141885a1757e9a完全匹配，存preview-before-visual-no-csv.py。`unit-test` `py -3 Tools/VFX/template_matrix_test.py` PASS6(.130s)，含默认统计/显式省略仍validation/pin。
- 完整当前Vulkan重跑03已启动：`py -3 Tools/VFX/template_matrix.py --backend Vulkan --existing-build-sha256 DB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE --output out/agent/rendering-evidence/vfx-authoring/template-full-vulkan-03`，待48case与全部视觉QA。Low独立QA认定01可读性FAIL；近景两档密采样脚本已准备尚未运行。
- template-full-vulkan-03中性背景已实际PASS24（CPU High12/CPU Low6/GPU High6），现运行game背景。root审CPU High四类三变体共12case，review-partial-01/02的root-review.json保存全部阶段与全RGB结束/停止零残留；Low六case实际是environment/explosion各三色，qa-neutral-low独立hash与root六图顺序复核为PASS_WITH_LIMITATIONS：橙色环境暗小，复杂背景/连续运动未验收，不扩大。
- 统计口径更正：省略Session -RenderStats关闭的是Application连续CSV及一般整帧计时；真实vfx.stats仍含Effekseer内部GPU聚合计时（neutral GPU记录status=1且samples>0）。因此不能称全部GPUtiming未采集；本轮仍不是性能验收，无原始benchmark采样。当前matrix statisticsScope文案过宽，输入冻结至终态，随后修正源码并归档本轮实际源码，原始报告另附口径说明。
- 已结束旧证据110张PNG透明NTFS压缩，completed-evidence-compression-02.json：logical405621480bytes、Failures=[]、hash/mtime零变化。此时观察C盘FreeSpace12724670464，增长远大于本批逻辑文件总量，不能全部归因本压缩操作；可能并发/延迟释放，未自行推断。原始PNG未重新编码/删除。
- neutral-gpu-high六case root逐张审qa-six-stage六图PASS：环境云渐稀疏、爆炸核到扩张壳、三色可辨、结束/停止空场。独立30阶段hash/原生双流hash匹配/nativeErrors0，真实accepted dispatch start→peak增长：environment346/62/178、explosion65/13/35；peak draw=1。DEV gpuValidated=false及非benchmark界限保留，root-review另存。
- game-cpu-high12case root看4模板三变体六阶段拼图及aura全帧、顶沿差异mask；root再对24张end/retired原图算全RGB最大delta=1，y>=32均零差，效果无残留。顶沿微差位于远景树干/建筑，成因未指派，不推断树动画/不宣称全图精确一致。qa-game-cpu-high/root-review.json为PASS_WITH_LIMITATIONS（命中/拖尾细小，未验证连续尾随/循环接缝或通用玩法可读性）；本组native收尾后另补。
- 已应用AgentEventLog固定槽位候选与测试patch（源patch仅保存候选历史）：256槽、1536消息字节预分配，保留cursor/page/reset/truncation/线程锁/不限长metadata；避免deque每条新增节点和消息反复分配。新增AgentEventLogRing公共Read契约单测，CMake仅Tests编入Game日志源；新文件行尾规范化，UTF8单测使用完整codepoint截断边界而非固化非法UTF8。尚未build/unit/runtime，严格PrivateBytes仍FAIL，不声称已修复。当前matrix仍固定Game18且不含此后源码改动，非日志环新构建验收。
- 顺带修正he-single-testlayer-run/SKILL.md一个过时路径game-debug→tests-debug，与现行隔离Tests缓存一致；未改测试调用/放宽验证。公共工具相关`git diff --check`通过；`py -3 Tools/VFX/test_preview_session.py` PASS2(.040s)，默认原有统计选项保留。

- 继续验收：template-full-vulkan-03最终command PASS48、inputsUnchanged/binaryUnchanged true；root `py -3 out/agent/vfx-authoring/review_matrix_native.py out/agent/rendering-evidence/vfx-authoring/template-full-vulkan-03` PASS12streams/nativeErrors0；`preserve_matrix_inputs.py`保存247输入文件。root全部48案例离散阶段审图完成，root-final-review.json=PASS_WITH_READABILITY_LIMITATIONS；草地ember及小型hit/trail可读性限制、顶沿delta1和非benchmark/非连续动画边界保留。原始report未改；statisticsScope原文GPU timing disabled不准确，原输入归档后只修正文案，GPU聚合仍存在。
- external DX12 Game18 preview01因driver记录字段status冲突FAIL，finally cleanup/输入身份PASS；执行源已留存。仅字段名修复后preview02 command PASS/sessionCleanup PASS/ENV restored，native双流校验无错。root看all-stages/160-full：主体青蓝球到烟焰，360/600/stop无残留；独立审阅发现160帧极暗外围触顶，因此未给未裁切PASS，准备独立宽镜头场景复验，不改源包掩盖。
- Low close retry `py -3 out/agent/vfx-authoring/skill-trial-low/nl-low-owned-compare-close.py --backend DX12 --existing-build-sha256 DB7795F59A74A8028A248E7C3498EDDF147445AC0819FA5CC97AA4F79CE950CE --output out/agent/vfx-authoring/skill-trial-low/compare-dx12-02-close --gpu-probe` runtime-manual command PASS两档/allocatorRestored及activeRestored true，Storage收尾、独立QA进行中。原01视觉FAIL保留。
- 日志环新源码开始 unit-test `.\hectl.bat test AgentEventLogRing -Parallel 2`，日志agent-event-log-ring-unit-01.log；未出结果前不宣称通过，严格PrivateBytes仍FAIL。

- unit-test `.\hectl.bat test AgentEventLogRing -Parallel 2` PASS，HEngineTests构建156.4s，进程exit0；日志agent-event-log-ring-unit-01.log。Storage registration/cleanup busy并延迟登记，未终止collector。只读审阅agent-event-log-root-followup-review.md未发现新契约缺陷，但metadata增长、调用者临时串与Read JSON仍分配；单测证明兼容，不宣称零分配/严格内存已通过。
- Low close02 root已顺序看两张完整阶段拼图，记录qa/root-review.json=PARTIAL_ORIGINAL_READABILITY_REQUIREMENT：24粒子轮廓稀疏且晚期暗，原FAIL不改。独立v2 Low64比High256少75%，两版紫色提亮220/135/255/255，名义半径和时序不变；原20文件完整hash闭包保存。v2真实编译/no-op各9输出hash/mtime不变，尚待运行。
- 外部wide独立场景camera沿原注视线距离x2，仅Translation语义改变，SHA4da01a8d0d89023b32eb6189bf999e2acc63e072c088ed807799fd3a2c42e0b4。runtime-manual命令 `py -3 out/agent/vfx-authoring/skill-trial-external/preview_external.py --backend DX12 --existing-build-sha256 db7795f59a74a8028a248e7c3498eddf147445ac0819fa5cc97aa4f79ce950ce --scene Assets/Scenes/nl-external-cyan-fireball-wide.he --output out/agent/vfx-authoring/skill-trial-external/dx12-game18-preview-03-wide` 已启动，终态/审图待定，保持包与共享输入冻结。
- 下一严格memory原预算参数已核对在lifecycle-game19-preflight.md；CPU hit-ember包相对旧02已因模板修正改变，GPU压力包/scene不变，所以新run是当前资源严格回归，不能声称仅logring二进制单变量。5warmup与零增长各阈值均未放宽。

- external wide03最终runtime-manual PASS9采样+cleanup/ENV/inputs/binary，11图和native双流hash匹配/诊断0；root全部阶段拼图及160/220全图通过离散范围审查。原RGB全部非零bounds距边最少29/44px，四边maxRGB0，360/600/stop精确空场。qa/root-review.json=PASS_WITH_SCOPE_LIMITS，18原输入已pre-import-inputs.zip归档。
- 按he-effekseer-asset-intake update_asset_manifest.py先dry-run复核再atomic更新，external asset.json为IMPORTED，final_import_path=Project/Assets/Effects/Authoring/nl-external-cyan-fireball.efkpkg，包SHAaeb40295d1e130b4e4d1a2ec5861b511fd7e6a7b5f0501ed9c338f8075b45e6e不变，仅DX12/CPU/High。两份XML49节点0GPU标签确认；estimated_peak_particles=36875明确是含helper的条件累计birth宽松估计，非可靠同时上界/精确峰值，不作为预算PASS。实测CPUinstances代理与aggregate时序单独保留，profiler_baseline.performance_acceptance=NOT_BENCHMARKED，Release/16并发/3轮性能仍待；final-import-dry-run/result保存命令结果。
- Low v2 `py -3 out/agent/vfx-authoring/skill-trial-low/nl-low-owned-compare-v2.py --backend DX12 --existing-build-sha256 db7795f59a74a8028a248e7c3498eddf147445ac0819fa5cc97aa4f79ce950ce --output out/agent/vfx-authoring/skill-trial-low/compare-dx12-03-v2 --gpu-probe` runtime-manual command PASS两档，20图与两包/native/currentGame18/scene身份PASS，end/retired全RGB空场，allocator/active恢复。root看四张完整阶段图，64粒子明显更密且burst/fade可辨；仍PARTIAL_RADIUS_CONTRACT_PENDING，因为CPU explosion各轴独立速度形成盒状分布，非GPU球壳/1.2m径向约束。独立离线projection审核和官方SDK支持的径向生成候选准备中，不盲加密度掩盖几何差异。
- 生产GPU候选仍未应用。独立审阅发现既有Initialize异常在Initialized置true前漏Shutdown，且Shutdown本身早退漏Manager/GPU等部分资源。仅EffekseerRuntime.cpp加局部rollback guard、统一半状态清理和DEV&&!PACK&&GM显式HE_VFX_VALIDATE_INIT_RECOVERY诊断；2次真实after-manager/after-GPU注入+3次初始化检查，未设变量零注入，失败必须抛错。before/afterhash和精确patch见init-recovery-fix。root已读patch，未改RuntimeRenderer/生产开关/第三方。
- build-only `.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2` 新Game19正在构建，含日志环和初始化rollback；尚无build/runtime结论。root准备run_init_recovery_check.py单backend owned hectl game check封装，安全--help PASS；待新SHA后两backend positive、negative/unset控制及原预算30cycles。

- Game19 build-only `.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2` PASS164.3s，SHA2786CB3427700DA6963D855D078249FD8EE40354C80BD92601CE4121299C732C，含日志环及Initialize rollback。当前启动 `py -3 out/agent/vfx-authoring/run_init_recovery_check.py --backend DX12 --mode positive --existing-build-sha256 2786CB3427700DA6963D855D078249FD8EE40354C80BD92601CE4121299C732C --output out/agent/vfx-authoring/init-recovery-dx12-game19-01`，其owned hectl check只在child env设置故障注入和GPU probe，父ENV不变；待真实运行结论。
- Low v2追加真实camera投影链离线审查：原点处1.2m球投影x448.057..831.943/y193.571..577.754；Low44像素包络467,181,843,559，最大视线切线等效半径1.4666m（>16阈值仍1.4620m），并非恢复粒子3D坐标。root查看radius-Low-44-overlay.jpg可见多点越理论球轮廓；叠图明确ANALYTICAL OVERLAY，不当作运行截图。SDK官方sphere生成径向方案仅准备候选，不改已运行工具/资产；需后续真实编译和同镜头复验，Skill Low继续PARTIAL。

- Game19 DX12 InitRecovery positive runtime-manual PASS：两次真实after-manager/after-GPU异常，postGPU容量50176且enabled=true，各次requireClean+重复Shutdown通过，第三次重初始化成功，owned game check READY/PASS exit0，binary/source不变。root已读全部InitRecovery行；NVRHI validation可用而native D3D12 debug layer缺失，root-native-review.json明确。Vulkan同命令仅backend/output切换到init-recovery-vulkan-game19-01正运行，尚无结论；严格memory仍未重跑。

- Game19 Vulkan InitRecovery positive runtime-manual PASS，root读实际两checkpoint、after-GPU capacity50176和第3次重建PASS，native validation layer enabled/errors0，binary/source不变。negative DX12-01被Storage cache lease占用提前阻止，nativeStreams=false，原FAIL完整保留，不冒充预期异常。等待build19 wrapper/collector真正完成后negative DX12-02通过：底层真实非零、InitRecovery result=FAIL attempts=0、无PASS标记，stderr说明requested renderer初始化失败，属于预期拒绝验证PASS。
- root run_init_recovery_check.py补外层Storage.workflow_scope，child env继承真实HE_STORAGE_PARENT，证据复制/report写盘后才完成外层与auto；四个先前执行目录均留原driver字节+SHA4d65df9de2af1169817bc0f2084dfc872ff21f000b4d9cf07e22f99d318a775d，不重写历史。新wrapper unset Vulkan-01 PASS：无InitRecovery标志、普通CPU启动exit0、两native流/hash/source不变；外层wrapper已终态。
- 当前严格runtime-manual已启动：`E:\Python\Python311\python.exe Tools/VFX/lifecycle_acceptance.py --backend Vulkan --cpu-effect Assets/Effects/Authoring/hit-ember.efkpkg --gpu-effect Assets/Effects/Stress/GpuSparks20k.efkpkg --scene Assets/Scenes/VFXAuthoringPreview.he --seed 1806 --cycles 30 --sample-frames 15 --natural-end-frames 3600 --nominal-particles 20000 --existing-build-sha256 2786CB3427700DA6963D855D078249FD8EE40354C80BD92601CE4121299C732C --output out/agent/rendering-evidence/vfx-authoring/lifecycle-vulkan-20k-03`。5warmup/零增长标准不变，旧02FAIL保留；运行输入冻结直到终态，新结果未出。
- CPU explosion径向候选cpu-explosion-radius-candidate已真实官方三例compile/EDIT/BIN字段匹配和重复hash PASS；新增direction契约仅CPU explosion默认[0,1,0]、其他值明确拒绝，describe说明center radius不等于sprite/fade外缘。独立contract2测试PASS包括错误EffectsRotation实际编译后回读拒绝、非法方向force build前拒绝且发布文件hash/mtime全不变。两候选patch仅out未应用，待当前memory结束再集成/实际运行，不能继承旧Low视觉。

- Game19 lifecycle-vulkan-20k-03完整30cycles/107checks已终态FAIL，唯一FAIL cycles_private_memory_trend：warmup后baseline1091891200、max1091956736（+65536）、medianWindowGrowth=-4096、slope+1159.4830769230769。packageHashesUnchanged/budgetUnchanged true，cleanup/effectCleanup均无错，native双流hash一致/错误0。root-acceptance-review.json保存完整结论，不以末段下降或allocation slots通过抵消严格FAIL。
- 对该Session导出的166513 frame、166549 submission、12941191 pass、477 memory行，正确agent-stop verifier PASS/exactWallSpool true，P953.6888ms/P995.7501ms仅该诊断run观察，非Release预算。首次root误加fixed-frame-only --expect-final-unavailable导致FAIL保留render-stats-verification.json；Application在OnPreUpdate stop前已计算下一CPU间隔，该外部stop不应套ExitAfterFrames尾行规则，源码1629/1694/1933及更正命令详见stats-verification-invocation-note.json。更正仍require-complete/全部row数/双join/全wall数据校验不变。
- 严格终态后已应用CPU radial authoring与test patch：unit-test `py -3 Tools/VFX/test_authoring.py` PASS11(13.558s)真实编译；canonical整库/Low v3真实重拍尚未执行，旧Low报告不改。
- 性能分析器已应用benchmark-gates补丁（此前只读审到缺口）：重复frame/CPU身份/双join明确拒绝，GPU各metric必须完整且不重复覆盖测量frame窗口，全部观测/长值保留诊断；invoke/result双写故障无论report能否持久化均Storage.finish与ENV恢复，不编造成功。unit-test bundled Python `-m unittest discover -s Tools/VFX -p benchmark*_test.py` PASS22(.438s)，含coverage suite，尚无Release实跑。
- DevAgentRuntime去掉response.dump().size重复预检7行，唯一FileMailboxTransport保留同config字节检查/UTF8 replacement/error envelope/原子发布。新增真实FileMailboxAgentTransport unit-test `.\hectl.bat test FileMailboxAgentTransport -Parallel 2` PASS，build113.4s；255/256/257字节、编码、小limit旧error超预算语义、collision不覆盖/不错误归档、成功归档、stopped/非法handle均验证。fixture cleanup显式绝对路径/唯一目录/非symlink校验。候选记录response-serialization-candidate，未把内存增长归因到此。
- Game20 build-only `.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2`已启动，待build/SHA/同3输入原预算严格复测。仅RPC重复序列化删除改变Game逻辑，Game19日志环/初始化rollback保留；Project资产暂不新增v3以减少两次环境差异。已有未发布径向v3草案和bounded serializer后备候选准备中，不自动应用后备。
- Game20 build-only `.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2` PASS149.8s，SHA43AD29790D823214165AA79D986CBAA16FF217EC0484C8A5A874147F265C9398。所有前序build/test/collector wrapper终态后，按Game19相同参数启动lifecycle-vulkan-20k-04；lifecycle-game20-input-comparison.json确认CPU包/GPU压力包/scene三输入hash与03一致，预算不变。19/30循环中间观测尚无超过settled baseline，未作最终PASS。共享C++/工具/资产冻结至终态；v3径向配方和持久serializer仅out候选，后备不自动应用。
- Game20 lifecycle-vulkan-20k-04完整runtime-manual 30cycles/107checks PASS，root独立native双流hash/错误0与原预算确认：baseline=max1099087872，maxGrowth0，medianWindowGrowth=-2154496，slope=-95572.28307692308；三输入和预算未变，effect/session cleanup无错。只证明本候选该负载/后端的既定proxy检查，精确fence-retired GPU bytes仍NOT_VERIFIED，不归因RPC为旧64KiB的已证实根因。wrapper在Storage自动收尾，尚不与下一GPU并发。
- `E:/Python/Python311/python.exe Tools/Agent/verify_render_stats_stream.py --frames-csv C:/WorkSpace/HEngineDev/out/agent/sessions/20260914-093313-614-312cb6bd6eb5/Saved/RenderStats/frames-17893495976312735.csv --expected-frames 166392 --require-complete` PASS：166433 submissions/12931891 passes/480 memory/exactWallSpool true；P953.739345ms/P995.842634ms仅Dev诊断，不是Release性能验收。agent-stop尾行规则未误加fixed-frame-only标志。
- clean status只读盘点已完成，reclaimableBytes0、protectedOverBudget54830831715；inventory scanComplete=false（旧retired缓存路径63处WinError3），不把此结果当完整扫描或删除授权。未发起删除；只准备明确已结束统计文件的NTFS无损压缩，保持hash/mtime并记录实际文件分配字节。
- lifecycle04 wrapper最终exit0后，completed-statistics-compression-02对明确已结束03/04的12统计文件NTFS压缩PASS：hash/mtime/逻辑长度全部不变，GetCompressedFileSizeW测得文件分配减少1001739945 bytes；free space变化另记，未全部归因压缩。01登记busy失败完整保留，未绕过互斥或删除证据。
- v3两配方实际按Low→High编译PASS，verify-noop.py PASS changed0/deleted0/hashmtime零扰动；canonical径向修正后 `py -3 Tools/VFX/authoring.py build-library` 24项PASS，`py -3 out/agent/vfx-authoring/verify_library_noop_radial.py out/agent/vfx-authoring/canonical-radial-noop-01.json` PASS24/changed0/deleted0/hashmtime零扰动。旧v2独立包未覆盖。
- `py -3 out/agent/vfx-authoring/skill-trial-low/v3-draft/preview-v3.py --backend DX12 --existing-build-sha256 43ad29790d823214165aa79d986cbaa16ff217ec0484c8a5a874147f265c9398 --gpu-probe --output out/agent/vfx-authoring/skill-trial-low/compare-dx12-v3-01` runtime-manual command PASS两档，各8采样；28输入/20原图/native四流hash一致/错误0/cleanup及ENV恢复。根看六阶段接触图与Low/High44分析叠图，root_review.py再核20原图和4terminal全RGB零差；root-review.json PASS_WITH_SCOPE_LIMITS，Low64稀疏径向且44已淡、High256更密亮，固定黑底近景无采样裁切/盒形，非游戏背景或连续运动/生产GPU验收。Low34/44二维射线包络.776767/.993290m，High .895651/1.092206m，明确不是3D中心readback。
- 同一Game20/配方Vulkan compare-vulkan-v3-01 command PASS，报告和游戏已终态，独立视觉QA及Storage自动收尾尚在执行。正式剩余3组合外层批次run_remaining_lifecycle_batch.py已准备，逐项原预算/固定输入/native/stats门禁，失败停止；独立审阅后增加登记成功门禁、等待子finally、不确定清理传播unknown，尚未运行。未设置假HE_STORAGE_PARENT或绕过collector。
- Editor动画验收工具两个fit子结果可能FAIL而顶层仍PASS的缺口已修：真实报告/截图保留后任一非PASS即阻断；root `E:/Python/Python311/python.exe Tools/VFX/test_editor_animation_acceptance.py` unit-test PASS9(.018s)。Editor冷缓存待重建/运行；out-only同会话Prefab viewport wrapper已准备未运行，当前只预期默认CPU策略，生产默认切换后需用真实策略重新审定。
- Vulkan v3 root已独立看六接触图与Low/High44分析叠图，独立执行qa/root_review.py复核20原图SHA及四terminal全RGB零差，root-review.json PASS_WITH_SCOPE_LIMITS；两backend都只覆盖固定黑底近景八帧，Low64晚期淡/角分布不均匀、High256更密亮，不扩展为游戏可读性或三维读回证明。Vulkan wrapper已exit0。
- `E:/Python/Python311/python.exe out/agent/vfx-authoring/run_remaining_lifecycle_batch.py --existing-build-sha256 43ad29790d823214165aa79d986cbaa16ff217ec0484c8a5a874147f265c9398 --output out/agent/rendering-evidence/vfx-authoring/lifecycle-game20-remaining-01` 已开始，顺序DX12 20k、DX12 50k、Vulkan50k，每项原30轮/5warmup/零增长预算，native和全stats校验后才进入下一项。正式父Storage run与子继承，不逐case重建/修改输入。
- 根启动前旧输入一致性断言实际FAIL（已在工具日志保留），随后批次已启动并固定当前资源；不是放宽运行门禁，但不能把旧Vulkan04当同包最终证据。canonical-hit-package-transition.json通过旧matrix归档包逐ZIP成员比较证明旧sha30f1e82083e59404f79d9bb38480aafd015691eab6d7a03527c79425aa8f0f99与Vulkan04一致，新sha476b9baa547e6870abdbb4afecda1ab99e402f4f4b859010287955f6575c117f仅PROVENANCE.json变化，效果二进制和纹理一致。预算SHA2004bc5810336a8318b2fcc6784281a8368f407d9f946a7c8348720453b347ce不变；最终需另以新包复验Vulkan20k，不能拼成四组合资源身份完全一致的结论。
- Game20剩余批次首项DX12 20k完整30cycles/107checks FAIL，唯一FAIL private trend：baseline1003376640/max1003417600（+40960）、medianWindowGrowth=-28672、slope+557.6861538461538；effect/session清理无错、native四类扫描0、包/预算/批次inputsUnchanged true。批次正确停止，DX12/Vulkan50k均未启动，Storage状态failed，wrapper最终exit1。末段回落不能抵消FAIL。
- 该失败run全统计 `E:/Python/Python311/python.exe Tools/Agent/verify_render_stats_stream.py --frames-csv C:/WorkSpace/HEngineDev/out/agent/sessions/20260914-100502-980-14898f0d0a9b/Saved/RenderStats/frames-17893515050787925.csv --expected-frames 276724 --require-complete` PASS：276771 submissions/21505092 passes/472memory/exactWallSpool true，P952.2133ms/P993.414031ms仅Dev诊断。
- 已应用bounded-reuse transport候选与真实复用测试；root补Scope/Ref、m命名/include顺序，以及在descriptor发布之前创建local writer、成功后才移交成员。保持256KiB上限/原tiny-error例外/UTF8 replace/原子写入/超限后复用/Stop释放；固定当前nlohmann3.12.0内部serializer API，升级需显式审阅。原候选历史UNAPPLIED说明不重写，后续记录root-applied-review.md。此修正减少明确的临时序列化workspace构造，未宣称40KiB已归因或修好。
- root `.\hectl.bat test FileMailboxAgentTransport -Parallel 2` 正执行，Tests build28.3s PASS，测试结论待；Game新构建尚未启动。新四组合run_full_lifecycle_candidate.py复用已审阅正式父Storage批次，固定同一exe/当前资源，顺序DX1220k/Vulkan20k/DX1250k/Vulkan50k，safe --help PASS，尚未运行。
- unit-test `.\hectl.bat test FileMailboxAgentTransport -Parallel 2` PASS（Tests build28.3s，真实runner PASS/exit0，mailbox-transport-unit-02.log），包含超限后UTF8/双实例/同对象restart和原边界/原子发布契约。Game21 ` .\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2` 正构建，尚未得到新hash，不把测试PASS当内存PASS。
- 为后续冷构建仅创建空 out/agent/editor-relwithdebinfo、out/agent/game-release 并启用目录NTFS Compressed继承，future-cache-compression.json记录；既有源码/资源/缓存文件未改。后续需记录构建环境的此文件系统设置；未执行Release/Editor构建或包装验证。
- Game21 build-only `.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2` PASS45.8s/exit0，SHA04E9CBC9EC504F1F5FE92615686A23500C8786BB580981C45BA4009D976F5DD0，含会话独占有界serializer/输出缓冲。随后启动 `E:/Python/Python311/python.exe out/agent/vfx-authoring/run_full_lifecycle_candidate.py --existing-build-sha256 04e9cbc9ec504f1f5fe92615686a23500c8786bb580981c45ba4009d976f5dd0 --output out/agent/rendering-evidence/vfx-authoring/lifecycle-game21-full-01`。四组合顺序DX1220/Vulkan20/DX1250/Vulkan50，全部保持原30轮/5warmup/零增长；单项失败停止、正常/未知清理区分、native和全统计逐项核对。当前RUNNING，不预写内存PASS。
- Game21 上述批次现已 terminal exit1：首项 DX12 20k 完整30cycles/107checks，唯一 FAIL 为原 private-memory trend，baseline1003847680/max1003921408（+73728），medianWindowGrowth=-28672、slope+1049.206153846154。后续三项未运行；root-acceptance-review.json 核对两个原生日志哈希、错误扫描0、包与预算未变。缓冲复用并未解决严格零增长失败，不继续凭此归因泄漏。
- unit-test（统计文件完整性）`E:/Python/Python311/python.exe Tools/Agent/verify_render_stats_stream.py --frames-csv C:/WorkSpace/HEngineDev/out/agent/sessions/20260914-110657-399-3f0fb24a76f6/Saved/RenderStats/frames-17893552195884851.csv --expected-frames 278497 --require-complete` PASS：278497frames/278542submissions/21643015passes/456memory，exactWallSpool=true。P95 2.0401ms/P99 3.068116ms 仅开发构建诊断，不是 Release 性能验收。下一步按需固定边界进程/默认堆观测，保留原外部 privateBytes 严格门槛；当前尚无堆分配因果证据。
- 已实现 PlatformProcess::ReadCurrentMemory 与开发 Agent runtime.memory 单次下一Tick入口/出口观测，默认不采样；不支持/失败显式availability/error，首次pair=null，pending拒绝覆盖、完成sequence递增。独立子代理只读审阅未发现状态机阻断问题，指出HeapSummary不保证更新last-error，root已补清零与无错误码失败的通用错误。Game22 build PASS53.5s（B01C09A0099E811C6A321A173B88EEE9C7FAC19A2FBE4331557C7BB2D85FD479）；上述错误码修正后Game23 build PASS15.8s（A5D0C0D02AC67D874277386878FB495CF62D9CEF670E45C8951937D6F81F30C8）。两次精确命令均 `.\hectl.bat game build -Configuration RelWithDebInfo -Parallel 2`；Game23自动Storage收尾尚在运行，不并发启动GPU。
- out-only `memory-direct-observation-plan/run_probe.py` 已准备，root补原生日志精确身份/全error扫描、初始化故障环境隔离、效果清理失败Storage unknown。safe --help 与CPU模拟不可用归一化、异常数值拒绝、延迟sequence等待均PASS；实际运行待。首轮计划Game23 DX12六轮，保留201live/30retire和统计记录，外部private采样前后与固定边界pair并列；额外请求有扰动，acceptanceStatus始终NOT_ASSESSED。
- Game23已terminal exit0。首次六轮诊断 probe-dx12-game23-06 在新暂停会话baseline统计status0/samples0时主动FAIL，未运行效果；session正常stop/native0，初始memory sequence0/null契约通过。保留该失败，修复out-only驱动先step一个明确记录的空场帧，使统计真正可用，不把无样本当零。`E:/Python/Python311/python.exe out/agent/vfx-authoring/memory-direct-observation-plan/run_probe.py --backend DX12 --existing-build-sha256 A5D0C0D02AC67D874277386878FB495CF62D9CEF670E45C8951937D6F81F30C8 --output out/agent/vfx-authoring/memory-direct-observation-plan/probe-dx12-game23-06-02 --cycles 6` runtime-manual diagnostic PASS/terminal0：双phase全部available、序号逐次+1、native0、inputs/exe unchanged、cleanup正常；acceptanceStatus=NOT_ASSESSED。默认堆commit六轮188276736恒定，allocated181977846至181924149；第5轮外部前后private相差-102400，非原子观测。没有复现旧严格增长，不宣称解决。
- 同命令输出改 probe-dx12-game23-30-01、cycles30，已启动完整30轮诊断，仍非原严格验收；目前默认堆提交量稳定。此简化工作负载没有原107checks前置阶段，若未复现需对完整前置阶段做并列观测，不据简单场景平台期推断全流程已修复。
- 上述30轮诊断现已完整采完并正常stop：diagnosticStatus=PASS仅表示采集有效，acceptanceStatus=NOT_ASSESSED；最后第27轮确实发生增长，前述“稳定”仅对应当时未完成的早期样本。root-diagnostic-review.json显示samples[5:]固定边界private峰值+45056、defaultHeap allocated峰值+46519、committed+32768、reserved不变；每轮Tick前后各指标差值0。cycle26相25：private+167936/allocated+59772/commit+32768，约发生于baseline观测296.8秒后；只是时间相关线索，不是分配归因。用户反馈已明确更正“未复现”的过早判断。原严格FAIL完全保留。
- root `review_probe.py .../probe-dx12-game23-30-01` PASS核验30轮、序号、双流hash和清理；两个诊断的全统计分别43431/209767frames，`verify_render_stats_stream.py --frames-csv <各自0000-start.stdout.log指向的会话/Saved/RenderStats/frames-*.csv> --expected-frames 43431|209767 --require-complete` 均PASS，精确原始文件身份见各自root-statistics-review.json。正式报告不把这些诊断认作原生命周期验收通过。
- 统一时间原点的只读审阅 timing-correlation.json/md 用原commands started/elapsed与session.status uptime约束起点、完整stats/updateSerial配对（mtime仅交叉代理）：Game21 DX12首次增长采样303.059–304.112s、Game20 DX12 300.688–301.669s、Game23简化诊断301.191–304.050s、Game19 Vulkan301.286–302.237s。Game19另一次增长387.285–388.303s一并保留，未只挑吻合数据。支持约300秒时间相关性，不证明同一分配者或定时器。
- `E:/Python/Python311/python.exe out/agent/vfx-authoring/memory-direct-observation-plan/run_full_probe.py --backend DX12 --existing-build-sha256 A5D0C0D02AC67D874277386878FB495CF62D9CEF670E45C8951937D6F81F30C8 --output out/agent/vfx-authoring/memory-direct-observation-plan/full-dx12-game23-30-01` 完整复用原Acceptance.run前置及30loops，诊断额外请求保持NOT_ASSESSED。107checks仅原private trend FAIL；25个settled固定边界private first1004285952/max1004322816/last1004253184，heap allocated first181504904/max181553814/last181499434，heap committed192241664/reserved209711104恒定。cycle10较9 private+135168/allocated+60156，线程34→36，新增47152/92380，无新增模块；后续线程与内存回落。root另行只读线程查询时两线程已退出（OpenThread error87），不能赋予起始模块/分配归属；原.NET StartAddress返回0也未当作有效地址。额外查询未暂停或写入目标进程。
- full诊断root-diagnostic-review.json核验107checks/30cycles/sequence/原生日志hash/error0/inputs与exe预算不变/normal stop。`E:/Python/Python311/python.exe Tools/Agent/verify_render_stats_stream.py --frames-csv C:/WorkSpace/HEngineDev/out/agent/sessions/20260914-115321-449-c63907fa2e7d/Saved/RenderStats/frames-17893580030565021.csv --expected-frames 322648 --require-complete` PASS（精确命令与结果保存root-statistics-command/review.json）。当前等待Storage终结，随后用同Game23空场330s无VFX播放对照，尚未运行。
- full诊断已terminal0（只代表diagnostic采集完成，原private gate仍FAIL）。`E:/Python/Python311/python.exe out/agent/vfx-authoring/memory-direct-observation-plan/empty_time_probe.py --backend DX12 --existing-build-sha256 A5D0C0D02AC67D874277386878FB495CF62D9CEF670E45C8951937D6F81F30C8 --output out/agent/vfx-authoring/memory-direct-observation-plan/empty-time-dx12-game23-01` 已完成12个真实墙钟样本（0..330s，实际334.484s），正常stop/native0、input/exe unchanged；diagnosticStatus=PASS、acceptanceStatus=NOT_ASSESSED。独立root复核每样本updateSerial=1、VFX draw/dispatch=0、active/cpu=0、实际present推进230056帧，未播放任何特效。
- 空场session uptime308102ms时同样private+135168/heap allocated+60012，heap committed/reserved不变，线程34→36；两个新增线程均实际NtQueryInformationThread成功，起始模块ntdll.dll/offset107232、description为空。process_inventory.py以精确owner/PID/startticks持句柄只读查询，未注入/挂起/写目标；这不是分配调用栈，不能据ntdll入口推断后台工作提交者。root-diagnostic-review.json/source SHA与全raw采样保留。空场不执行特效也复现该现象，原预算和硬门槛仍未改。
- 已向用户提出验收范围选择：保持原process private硬门槛继续定位，或明确允许其降为诊断并保留特效资源硬门槛；问题依据最初“不放宽验收”要求。未收到明确更改前保持原门槛。准备四组合继续采集器：仅已完整且唯一private gate FAIL、native/hash/清理正常时继续其他组合，保留每项FAIL且整批最终FAIL/exit1；其他失败立即停止，不跳过原公式或修改预算。

- Game23四组合原始批次 game23-all-lifecycle-observations-01 在首项DX12 20k完成107checks/30cycles后按fail-closed规则停止：不止private门槛，cycles_os_local_memory_trend与cycles_os_nonlocal_memory_trend也FAIL。local baseline503099392/max504147968、median0/slope9679.163076923078；nonlocal baseline258510848/max259559424、median1048576/slope62914.56；private baseline999100416/max1001308160（+2207744）、median1118208/slope73797.31692307693。native PASS/error0、输入/预算/exe保持不变。后三组合未运行，不扩大允许继续的失败列表、不改原门槛。当前等待Storage自动收尾终态。
- runtime-manual统计证据：`E:/Python/Python311/python.exe Tools/Agent/verify_render_stats_stream.py --frames-csv C:/WorkSpace/HEngineDev/out/agent/sessions/20260914-122435-671-4a6643b0593b/Saved/RenderStats/frames-17893598774126057.csv --expected-frames 280956 --require-complete` PASS；精确命令与完整结果保存首项root-statistics-command.json/root-statistics-review.json。这是数据完整性通过，不能抵消原验收FAIL。

- Game23原始批次收尾terminal exit1。只读逐轮审计 game23-memory-failure-audit/REPORT.json/md：local/private首次同增在cycle6约252s，private另在cycle11约300s+135168、cycle17约357s+1MiB，nonlocal在cycle18约366s+1MiB。三计数并非共同阶跃；公开退休active/cpu/GPUallocator均0、pool恒定不等于native无泄漏。NATIVE-NEXT.md未找到可直接归因本轮1MiB的分配证据。
- idle Game中间文件无损压缩 game23-intermediate-compression-01 PASS，1470个obj/lib/pdb内容/mtime/exe保持不变但已压缩，实际新增回收0。completed-statistics-compression-03对Game20/21/23三份闭合严格DX12统计18files压缩PASS，实际allocation减少2604211204bytes，内容/mtime全不变，不删除失败。
- runtime-manual `E:/Python/Python311/python.exe Tools/VFX/character_acceptance.py --backend DX12 --existing-build-sha256 A5D0C0D02AC67D874277386878FB495CF62D9CEF670E45C8951937D6F81F30C8 --case both --timeout 600 --max-frames 1500 --output out/agent/rendering-evidence/vfx-authoring/character-game23-dx12-01` FAIL/terminal1：character内嵌Python缺uuid导致注册失败/报告超时，presentation子项实际PASS60steps（仍待root native/视觉完整复核）。首次启动设置了三个未使用env名称，不冒充环境隔离；第二次按真实HE_VFX_STRESS_MODE/HE_VFX_VALIDATE_INIT_RECOVERY/HE_VFX_STATS_LOG设置0及HE_VFX_DISABLE_STATS=0/GPU_PARTICLES=0。
- 首次失败脚本原字节保留failed-character-source.py；最小修复VFXCharacterAcceptance.py使用已有os.urandom(16).hex()代替uuid依赖。case character第二次 character-game23-dx12-02已真实完成51断言/16checkpoint，但强制场景重载前live handle检查FAIL（短命效果在fixture推进中已自然结束），原FAIL保留。计划让该专用fixture先真实推进一帧再暂停表现组合，保留live/paused的硬读回后执行重载；不降低重载前必须仍playing门槛。

- 2026-09-14 收到父任务转达用户明确收尾授权：进程private和OSlocal/nonlocal严格零增长降为诊断，保留原FAIL；最终采用代表性双后端验收，不再扩展非阻断排查。Docs/VFXAuthoring-Delivery.md记录新判断边界，原acceptance-budget.json保持不变。
- character-game23-dx12-03 runtime-manual PASS/terminal0：51assertions、16checkpoint、510stepframes，重载fixture先一步再pause，驱动明确验证playing+paused后强制scene generation替换，旧VFX/light回收。完整native双流hash核验/错误0、正常stop。原图联系图和原尺寸crop已逐张查看；large-yaw三组palette哈希完全相同，明确NOT_VERIFIED，不借总体断言PASS宣称大角度插值通过。unit-test `E:/Python/Python311/python.exe Tools/VFX/test_character_acceptance.py` PASS5。第一次03命令误抄SHA，pin门禁拒绝且未启动session；更正为存档Game23 SHA后才执行。
- Editor GPU诊断候选7文件已独立静态复审，最终patch SHA EDB9286FEA40B6A67DBEE44FD19D2FFBE38329041E3FB08F2280F4600590866F；公共DescribeStats两端复用、显式OFF编译probe、单一fixed1/60 step、normalend30退休步+下一Poll零值与emptyRenderWorld检查、实际scene切换保护、禁止end覆盖未完step。候选当前仅out，待cold Editor12完成后集成增量；不会拿当前冷构建当新候选编译证据。
- 已提升真实驱动到Tools/VFX/editor_vfx_acceptance.py及test_editor_vfx_acceptance.py，只调整ROOT为仓库标准位置。unit-test `E:/Python/Python311/python.exe Tools/VFX/test_editor_vfx_acceptance.py` PASS11，safe --help PASS；真实Editor/GPU仍待。代表性每backend单session CPU+GPU多时刻/自然结束/重播stop/退休/清理，原请求与截图上限不变，OS/private不评PASS。
- `hectl editor build -Configuration RelWithDebInfo -Parallel 2` Editor12 cold已configure PASS21.1s，正在编译，仅复用独立compressed cache。最终计划out run_final_representative_checks.py用一个真实外层Storage跑Game/Editor增量与High质量Game双后端、EditorVFX双后端，避免每个内层stop重复回收；尚未运行。
- 提交范围只读审计从task3 initialChanges精确排除32项、2050初始untracked；另768运行副本/UUID导入与Editor scratch不提交、不删除。四处用户tracked当前patch与初始patch一致。git fetch origin main成功，HEAD...origin/main为0/0，尚未stage/commit。两处生成器原样输出使用精确.gitattributes whitespace规则，保留shader disassembly与stub docstring字节；git diff --check当前PASS。


### 2026-09-14 最终收尾验证

最终 Game SHA 27494163d54ee4309d71bdedb94a2831169c289a38393c2dd44df94673cb0e3f、Editor SHA 3cbecc06e1b520d39552ce471f19f4e77da0025259695ca97ed29c20f57abf5a 构建及代表性双后端验证完成。完整精确命令、人工审图、计时和未测范围见 VFXAuthoring-Delivery.md。首次候选缺 RenderMemoryBudget 完整类型头导致编译 FAIL，补公共 RHI RenderDevice.h 后通过；首次 Editor 空 fixture 未设置曝光，虽然命令通过但审图全黑，独立视觉 FAIL 保留。复用既有预览相机/曝光后，最终 Editor 两后端各 14 图、1990 帧、正常退出与 GPU 新鲜计时通过。

最终只做上述受影响路径复验，未扩展性能或严格内存矩阵。760 个预期提交文件按初始 32 项排除及临时资源前缀过滤；用户原 4 文件 SHA 和初始 diff 保持。冷 Editor 构建的 Storage finish 因同一 collector 忙而延期，collector 完成后通过正式 finish 补登记 success；没有删除锁、未知状态或受保护缓存。
