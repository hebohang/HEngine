> 原始证据目录：`out/agent/pipeline-upgrade/supplement/`；其他E1/R/C目录相对`out/agent/pipeline-upgrade/`。

# GPU 粒子旧限制独立审计

结论：Effekseer GPU 粒子能力被生产集成明确禁用、启用请求拒绝、含GPU发射器的包被拒绝，这些逻辑在 `cc7526ec` 基线已存在，`9f64aa55` 没有引入或解除这项限制。CPU粒子可见、GPU图形API可用、RTX设备存在，均不构成GPU粒子支持证明。

证据类型：只读源码差异审计，不是GPU运行/单测PASS。未修改生产文件，未构建或启动GPU。

## 基线定义

实际比较 `git show cc7526ec:Engine/Source/Runtime/Graphics/VFX/EffekseerRuntime.cpp` 与 `git show 9f64aa55:Engine/Source/Runtime/Graphics/VFX/EffekseerRuntime.cpp`。

初始用户补丁 `.git/prompt-scheduler/20260911-rendering-vfx-01/initial-tracked.patch` 仅修改 `.github/copilot-instructions.md`、`AGENTS.md`、`CLAUDE.md`、`Project/Assets/Scenes/PBRTest.he`，未改此生产模块。因此对这里的GPU能力/拒绝逻辑，`cc7526 + initial patch` 与 `cc7526` 相同；不将此结论扩大到未跟踪资源或整机环境。

## 生产路径逐点证据

以下行号分别为基线/最终同一文件 `Engine/Source/Runtime/Graphics/VFX/EffekseerRuntime.cpp`。

| 路径 | 基线行 | 最终行 | 行为与比较 |
|---|---:|---:|---|
| GpuParticleRequestDetector | 1067 | 1069 | CreateResource仅增加请求计数并返回nullptr；完整class块文本SHA256相等。它是检测器，不是GPU资源工厂实现。 |
| ConfigureGpuParticles | 1268 | 1270 | 禁用分支解绑system/factory并清状态；真正启用请求记录unsupported external-command-list adapter错误并return false，未创建GPU粒子system。完整函数文本SHA256相等。 |
| 初始化能力 | 1449 | 1440 | 初始化成功后仍显式 `s.bGpuParticlesCapable=false`。源码注释把限制归因于该external command-list集成路径的GPU发射器deferred flush；本审计仅确认此既有实现/注释，不独立证明第三方底层根因。 |
| 安装检测器 | 1480 | 1477 | Manager安装GpuParticleRequestDetector，用来识别包的GPU请求，不启用模拟system。 |
| 包加载检测 | 1806/1823 | 1824/1841 | Effect::Create前后比较检测计数；有GPU请求则记录“Package rejected because it contains GPU particles”并return false，在Effects缓存插入前拒绝。 |
| IsGpuParticleCapable | 2159 | 2177 | 完整函数不变，返回已初始化且capable状态；初始化capable始终false。 |
| SetGpuParticlesEnabled | 2169 | 2187 | 完整函数不变，未初始化返回false，初始化后委托上述拒绝启用函数。 |
| 统计导出 | 对应旧同逻辑 | 2132–2133 | Stats导出capable/enabled实际状态；没有因最终CPU可见修复将它们改为true。 |

状态结构初始capable/enabled=false，Shutdown复位false。当前源码没有将两者设为true的成功启用分支。预算常量50176/50000及字段存在只说明接口/预算模型，不说明GPU粒子已可运行。

## 可复核差异材料

- `gpu-particle-baseline-diff.json`：四个关键class/function完整块的基线/最终SHA256及行号，四项均identical=true；另列初始化、安装检测器、包拒绝定位。
- `gpu-particle-runtime.diff`：实际 `git diff cc7526ec 9f64aa55 -- Engine/Source/Runtime/Graphics/VFX/EffekseerRuntime.cpp` 输出。差异涉及渲染状态失败处理、RH相机/当前投影、宿主帧hook和CPU录制等；没有引入GPU启用成功路径或删除包拒绝。
- 精确只读命令：`git diff cc7526ec 9f64aa55 -- Engine/Source/Runtime/Graphics/VFX/EffekseerRuntime.cpp`；`rg "^diff --git" .git/prompt-scheduler/20260911-rendering-vfx-01/initial-tracked.patch`。

包拒绝日志中仍写“DX12 external-command-list adapter”，但代码不按后端分支跳过检测，capable=false也在共同初始化路径。因此不能仅凭这句日志断言VulkanGPU粒子可用；它是旧日志措辞窄于实际统一拒绝路径。

## 证据边界

本轮实际CPU MagicTornade/FireBall画面与缓存测试可证明CPU效果/快照/Owner寿命，不可提升为20k/50k GPU粒子、GPU计算模拟或GPU粒子吞吐验收。旧限制不是本轮新增退化，但旧限制的存在也不等于原需求中的GPU目标已完成。此审计没有新运行GPU资产拒绝测试，没有借用CPU生命周期单测日志声称完整GPU系统验证。
