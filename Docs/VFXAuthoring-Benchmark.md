# 正常负载基准

入口 `python Tools/VFX/benchmark.py --backend DX12` 默认选择Release，只输出计划、固定预算和资源hash，不启动引擎。显式 `--configuration RelWithDebInfo` 可选择诊断配置。执行必须提供 `--run --existing-build-sha256 <已构建Release候选SHA256>`；Vulkan使用相同命令替换backend。使用带Pillow的bundled Python，以支持性能轮次之后的独立覆盖采集。本文工具验证不是实际Release性能通过证据。

预算读取已冻结的 `Tools/VFX/acceptance-budget.json`：16同时效果、High，每轮600warmup+600sample，共3个独立进程轮次，额外32帧用于GPU查询和效果/分配器退休。场景为 `Assets/Scenes/VFXAuthoringBenchmark.he`，1280×720，种子1806+slot。六模板、三色变体取固定前16项，具体路径和SHA256写入request.json。预加载所有包，再开始效果；one-shot结束立即补播，记录每次Play耗时，全部重播成本保留在采样中。

运行工具复用 `hectl game check -Configuration <Release|RelWithDebInfo> -GraphicsApi <backend> -StrictGraphicsApi -StartupScene Assets/Scenes/VFXAuthoringBenchmark.he -Frames 1232 -FixedDeltaSeconds 0.016666666666666666 -Resolution 1280x720 -TimeoutSeconds 300 -PerformanceWarmupFrames 0 -RenderStats -RenderStatsPath <round>/frames.csv`。这是真实连续运行，非Agent暂停/step计时。固定模拟步长不等于固定墙钟帧时；RenderStats测真实墙钟和完成GPU查询。Python观察与重播耗时不扣除。当前hectl支持Release与RelWithDebInfo，工具原样传递选定配置并校验嵌入式sys.executable位于对应game-release/game-relwithdebinfo目录，CMakeCache的CMAKE_BUILD_TYPE也必须一致，再记录实际exe hash。Release仍是HE_BUILD_DEV_GAME=ON/PACK_GAME=OFF/HE_ENABLE_GM=ON的loose内容开发运行，包含Python观察与统计，不是shipping package性能。入口不支持选定配置时明确失败，不自动降级。

每轮保留原始RenderStats主CSV、submissions/passes/memory旁表、日志、嵌入式逐次VFX快照、预加载和重播成本及实际sys.executable SHA256。主机GPU名称/UUID/驱动尝试通过只读nvidia-smi记录，实际设备选择证据仍以严格backend启动日志为准。输入hash在轮次结束复核，三轮exe hash必须一致。失败与完整证据统一在Storage外层记录，子hectl进程等待/回收和证据分析之后才完成外层记录；Ctrl+C也先等待有界子运行清理。

CPU VFX值按update_serial确认每次真实更新，仅使用该次current，不重复累计滚动p95。当前指标没有world-frame标识，因此明确标记CPU VFX脚本窗口与总CPU窗口的精确frame对应不可用。GPU模拟和渲染分别按last_gpu_simulation_submission/last_gpu_render_submission去重，通过已完成submission记录的graphics submission区间关联原始CPU帧；总GPU按submissionTimingSampleId+cpuTimingFrameId双键关联。任何一个指标缺少600项完整测量均FAIL，不拿0补齐。所有测量长帧保留在原始文件和analysis.json；统计按nearest-rank计算p50/p95。

unit-test：`py -3.11 Tools/VFX/benchmark_test.py` PASS，8项：乱序完成双键join、模拟/渲染独立去重、缺失GPU与长帧保留、锁定16/600/600/3计划、one-shot持续重播及Stop清理、Release默认/配置身份与缓存不匹配拒绝、旧路由不静默降级。测试使用合成计时记录和假HEngine，不是runtime性能证据。

Release配置修复只验证Python工具和当前入口声明，不是Release引擎构建或运行证据；实际构建与测量由主代理执行。新证据目录为Storage白名单下 `out/agent/rendering-evidence/vfx-benchmark/<run>`。

Storage外层注册失败（包括collector lock busy）会在任何引擎进程启动前返回FAIL，保存result.json并标executionStatus=not_executed；新增注入注册失败测试确认Popen未调用。原有owned子进程finally等待保持不变。

## 固定候选与退休检查补充

实际执行现在必须提供预先选定候选：`py -3.11 Tools/VFX/benchmark.py --run --backend DX12 --configuration Release --existing-build-sha256 <Release候选SHA256>`。原有不带hash的运行示例已不适用；默认plan无需候选。验证发生在启动任何hectl之前、每轮前后及最终finally；hectl仍可能执行其构建节点，若它改变候选，即使运行成功也记录FAIL，不能把新二进制冒充指定候选。失败日志、runtime rows与原始CSV均保留。

脚本在首次加载前记录分配器基线。分析要求最后达到测量结束+32退休帧，active_effects与active_cpu_instances均有Exact零读数；gpu_allocated_emitters/gpu_reserved_particles/gpu_reserved_trails恢复干净零基线，三项分配失败计数在全部采样中存在且未增加。缺基线、缺退休行或缺指标均FAIL。这是CPU维护的GPU分配器ownership证据，不宣称真实GPU粒子buffer或fence退休字节读回。

CPU/GPU计时只接受finite且非负数据，VFX数据还要求Exact状态；缺失/非法数据不能凑足600样本。预算未修改，所有长帧保留。unit-test：`py -3.11 Tools/VFX/benchmark_test.py` PASS（11 tests，包括候选变化、负值/无穷计时、缺基线、退休残留、分配失败及正常样本）。未运行引擎或构建。

## 同步流式统计的测量边界

当前 Application 在运行过程中同步写出 RenderStats：前一行主CSV、submission和memory序列化，以及每256次刷新。它们会计入实际 wall、start-to-start、busy 时间，不能从总CPU预算中扣除；update/render/window分段不包含这些写出位置的开销。GPU marker区间没有为文件写出直接扩大，但CPU供给和调度仍可能间接受影响。Python观察、重播和同步流式统计共同属于此Release开发统计构建的测量成本。

沿用原600预热、600采样、3轮及原CPU/GPU预算，不添加“扣除统计开销”的修正值。最终DX12/Vulkan三轮都必须使用包含相同统计实现的新候选重新测量。Game15/Game16或其他统计实现不同的记录不能直接用于把CPU差异归因于渲染改动；保留二进制hash和完整原CSV，分别标明测量实现。此处不把任何尚未执行的新候选写成性能PASS。

## 独立透明覆盖代理与完整原生日志

三轮计时后，另起三个有界 `hectl game capture` 进程：同一场景/分辨率/固定步长/候选的空场、16效果、重复空场，各automation frame 60；不会在600帧性能窗口插入截图readback。按最终RGB最大通道差阈值1/4/16记录变化像素数、比例和bounds。重复空场在阈值16下有变化、或效果图没有变化，均记测量完整性FAIL。代理包括合成与后处理，不是实际透明fragment数量、overdraw层数或填充率成本，也不是覆盖率预算通过。它只是单个固定时刻，不代表整个采样窗口的峰值；人工视觉状态保持NOT_REVIEWED。

性能和覆盖进程均要求唯一 `run HGame: STARTED` 回执，并复制真实stdout/stderr及hash，核对 `run HGame: PASS exit=0`；扫描两流明确的VUID/device lost/NVRHI/fatal错误，完整日志保留，错误详情仅打印/记录有界摘要。性能命令没有启用RenderValidation，不宣称验证层覆盖。覆盖还必须核对原生 `Captured frame 60`、实际输出路径与 `target=presented-backbuffer`。脚本row60另证负载；脚本帧与automation帧精确绑定尚未暴露，二者对齐是启动序列假设，不能冒充同帧测量证明。

正常基准对子进程固定 `HE_VFX_GPU_PARTICLES=1`、`HE_VFX_DISABLE_STATS=0`、`HE_VFX_STATS_LOG=0`、`HE_VFX_STRESS_MODE=0`、空 `HE_VFX_STRESS_EFFECT`、`HE_VFX_VALIDATE_PACKAGES=0`，保存到request.environmentOverrides；父环境不修改。开发GPU probe不等于生产默认启用或已验证能力。输入/候选hash在所有阶段前后复核；原预算SHA256为 `2004bc5810336a8318b2fcc6784281a8368f407d9f946a7c8348720453b347ce`。

CPU验证：bundled Python运行 `-m unittest discover -s Tools/VFX -p 'benchmark*test.py'`，15 tests PASS（包含子环境隔离、覆盖控制漂移、空场生命周期、真实日志链接与capture回执拒绝）；这些模拟测试不是GPU性能/视觉证据。
