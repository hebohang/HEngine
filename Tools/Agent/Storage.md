# hectl 磁盘管理

`hectl` 的普通构建继续复用固定目标、配置、Tracy/portable 和项目缓存；不会在每轮工作后删除 `out`、SDK 或 GeneratedSource。历史回收由 `storage.py` 集中实现，Windows PowerShell 5.1 通过 `Storage.ps1` 调用。使用已有 Python 解释器（优先 `HE_STORAGE_PYTHON`，其次 `py -3`，最后 `python`），不安装第三方 Python 包。

## 命令

```powershell
.\hectl.bat clean status --json
.\hectl.bat clean caches
.\hectl.bat clean caches --json
.\hectl.bat clean status --json -Report out/agent/storage/inventory.json
.\hectl.bat clean preview --json
.\hectl.bat clean stale -WhatIf --json
.\hectl.bat clean stale --json
.\hectl.bat clean retired -WhatIf --json
.\hectl.bat clean retired --json
.\hectl.bat clean maximize -WhatIf
.\hectl.bat clean maximize
.\hectl.bat clean stale -Category evidence -KeepSuccess 5 -FailureDays 7 -BudgetBytes 5368709120 --json
.\hectl.bat clean preview -Project D:\Games\MyGame --json
```

`status` 统计实际存在的 `out/agent`、`out/build`、`out/package`、`out/pkg`、`dist`，以及默认项目资源、源资源、Saved、内容状态和 Engine/Content。项目在 `projects` 内的缓存和证据分开统计。`preview` 只扫描运行登记册中的产物，列出每项路径、状态、原因和预计大小；未知目录在完整 inventory 中只报告。外部项目的持久资源不自动遍历，但其仓库内输出按登记的绝对项目路径过滤。

`caches` 只读列出默认项目和外部项目的 game/editor/tests 构建目录、逻辑大小、最近文件写入时间及 CMake cache 是否存在；不展开海量历史运行记录。大小是候选目录的估算占用，不代表已核实可删除量。删除前对明确路径运行 `clean cache -CachePath <path> -WhatIf`，再决定是否执行清理。缺少 `CMakeCache.txt` 的残留目录会显示为 `no-cmake-cache`，不能由 `clean cache` 当作完整构建缓存处理。

`stale` 执行当前策略；`preview` 和 `stale -WhatIf` 不创建锁文件、报告文件，不改写、改名、截断或删除任何目标。`status -Report` 是显式请求写报告；`-WhatIf` 下即使给出 `-Report` 也不写。CLI 保持 `clean status`、`clean stale` 的用法，但**取消“顶层不在白名单即删除”的旧语义**。

无 `-Category`/`-Project` 筛选的显式 `clean stale` 还会重试 `clean cache` 中断后留下的精确 `*.retired-<32位十六进制>` 缓存目录；`clean retired` 可只处理这些残留，避免在大量历史记录上执行完整 `stale`。`preview`/`-WhatIf` 仅列出候选。只检查受管缓存单元的直属目录，不扫描整棵 `out` 寻找任意同名文件。删除前重新核实路径、目录联接、`.he-keep`、会话/恢复文件、登记证据、缓存 lease 和构建/运行进程；受保护项列入 `skipped`，删除失败列入 `failed`。此项不受 `autoEnabled` 控制的自动回收触发。

`-Category` 可用 `evidence`、`package-intermediate`、`package-staging`、`wechat-backup`、`wechat-staging`。`--json`/`-Json` 输出结构化结果；普通 `stale` 输出候选表及简短汇总。执行后的固定详细报告为 `out/agent/storage/last-clean.json`，不会每轮增加一份报告。

## 保留优先级

默认值集中于 [storage-policy.json](storage-policy.json)：

| 配置 | 默认值 | 行为 |
| --- | --- | --- |
| `successCount` | 5 | 每个项目、稳定工作流名保留最近 5 份成功完整证据 |
| `failureDays` | 7 | 失败证据保留 7 天 |
| `wechatBackups` | 1 | 每个导出目的地保留最近 1 份已确认的备份 |
| `budgetBytes` | 5368709120 | 已结束的历史证据预算，5 GiB |
| `autoIntervalSeconds` | 300 | 普通自动回收最多每 5 分钟一次 |

运行中、未知终态、锁占用、恢复事务、用户固定内容始终保护。记录 PID 和进程创建时间；会话启动后绑定实际引擎进程，防止 PID 重用造成误判。没有终态而 owner 已退出或无法核实的记录显示 unknown，不推断成功。最近成功记录及诊断期限内的失败记录也不能为了预算而删除。超过数量或诊断期限的已确认历史可以回收；容量预算**不会放宽**这些规则。若这些受保护记录自身超过预算，`protectedOverBudgetBytes` 和 `budgetAdvice` 报告超额。未知历史不冒充已结束任务，也不冒充已经满足预算。

目录内放置 `.he-keep`（任何层级均可），或为单文件放置同名的 `.he-keep` 后缀旁文件，可以固定保留。也可在对应运行记录设置 `pinned: true`。固定证据、SDK、当前导出、构建缓存不计入历史预算。不要通过修改记录的状态来“证明”历史成功；异常终止留下 `running`/`unknown` 是有意的保守行为，需要先核实归属、进程及恢复状态。

默认全量回收会删除所有超过保留数量/期限且没有其他保护的登记产物，所以预算不是额外保留历史的理由。每次删除按文件成功的 `unlink` 累加 `deletedBytes`；部分失败会保留运行记录供重试，并在 `failed` 中记录错误，不以预估大小冒充已释放量。所有大小均为**逻辑文件字节**；未测量 NTFS 分配、硬链接共享、压缩、稀疏或延迟删除产生的物理释放量。

## 自动收尾边界

本工作区按用户要求设置 `storage-policy.json` 的 `autoEnabled: false`，关闭普通历史自动回收。
显式 `clean stale` / `clean cache` 仍可使用；成功打包临时输入等工作流自身的收尾不受该开关影响。
本任务后续空间不足时主动清理 `out` 的闲置产物，先核实进程和路径边界，保留源码、运行中内容、
恢复材料和仍需使用的验收证据，不穿过目录联接删除其他磁盘内容。

| 工作流 | 完成与保护范围 |
| --- | --- |
| `Build.ps1` / game、editor build/check/capture | 固定 configure/build 日志仍复用；本轮日志快照和结果另存有限历史。受控运行记录 Saved、stdout/stderr、自动命名截图和二进制 SHA-256。退出、验收及快照结束后完成记录 |
| `Session.ps1` / agent stop | 进程退出、运行校验后完成；保留独立 `owner.json`，不依赖会被下一会话覆盖的 `active.json`。找不到进程且不能确认结果时保持 unknown |
| Tracy / TracyStartup | 外层记录保护内层 stop 后仍要读取的会话；直到工具退出、metadata 和最终摘要读取完毕才完成。无法确认工具收尾时保留 unknown |
| Benchmark-Rendering / Capture-RenderingBenchmark / Profile-RenderingWpr | 包括分析、结果写入和 trace 停止；嵌套输出由父目录记录统一拥有，不重复计量 |
| Capture-PBRShadow | 完整配置恢复和发布后结束外层记录；默认 `Artifacts/PBRShadowCSM` 是固定验收基线，仍永久保留，历史目录只保存引用和 manifest hash |
| Package.ps1 / hectl package | CMake 声明本轮精确 nonce 路径，登记 CookInput 与 staging。构建、发布、验收完成并释放 package 锁后，立即尝试回收成功 CookInput；失败按统一期限保留 |
| 微信 export | 原子发布、恢复 journal 清除后登记原输出备份；释放导出锁后立即将有归属的历史备份限制到配置数量 |
| content sync/plan | 默认新建的导入报告在转换、临时 staging 收尾、验证和 report.json 写完后完成记录；显式 `--report` 输出永久保留，仅用 guard 保护其内层验证 |

普通自动清理只读 `out/agent/storage/runs/*.json` 及其中列出的产物，**不递归扫描构建缓存/整棵 out**。没有在 `agent call`、status、capture 轮询或进程轮询中加入扫描。成功 package 输入和微信备份的限定回收不受普通 5 分钟节流延迟。普通回收异常仅提示报告位置，不覆盖原命令退出码。

独立的运行日志不会被清理器截断或改名。活跃任务不受“已结束历史”预算约束；当前实测会话体积主要来自 PNG，不能据独立 Saved 目录推测重复 Shader 缓存。手动 `open` 的用户进程和自定义导出路径也不会因缺失新记录而被自动删除。

## 扩展工作流

使用稳定的工作流名，产物位于已管理根目录内的 `sessions`、`logs`、`captures`、`profiles`、`benchmarks`、`rendering-evidence`、`content` 子目录。自定义管理根未实现；外层任务使用根外或其他自定义输出时，自动建立只保存引用的 guard 记录来保护其内层任务，原输出不纳入删除。不要注册整个历史桶、当前缓存或任意未知目录。

```powershell
. (Join-Path $PSScriptRoot 'Storage.ps1')
$record = Start-HEStorageRun -Project $projectRoot -Workflow 'my-validation' -Paths @($newEvidenceDirectory) -Locks @($existingWorkflowLock) -Outer
$state = 'failed'
try {
    # 构建/会话、等待退出、复制校验、分析、写摘要。
    $state = 'success'
}
finally {
    # 先关闭/等待所有本轮进程、恢复事务并释放工作流锁。
    # 若无法确认退出或恢复，改用 unknown。
    Complete-HEStorageRun -Run $record -State $state
}
```

`-Outer` 通过继承的 `HE_STORAGE_PARENT` 关联嵌套任务，完成时恢复原环境。内层 stop 不能回收仍由外层使用的证据。父记录保留时，其依赖的子证据也保留；成功后已无用途的 CookInput 不属于结果依赖。父目录已完整包含内层目录时，不重复登记。基线要使用 `.he-keep`，不要给每轮工作流名加时间戳绕过保留数量。

读取已有可回收证据时用 `-Dependencies @($sourcePath)` 声明依赖。Tracy 的 analyze-existing 已登记原 trace；分析运行中或其结果仍受保留策略保护时，原 trace 所属记录一起保护。Python 工作流可使用同一模块的 `Storage.workflow_scope` 管理外层终态和环境恢复。

## 锁、边界与恢复

回收器使用仓库专属 Windows named mutex，登记/终态更新也使用同一 mutex。写入方短暂等待它；两个清理器不会同时删除。删除持有相关 workflow 文件锁，重新读状态和保护规则。目录 guard 拒绝路径穿越及所有 reparse point，并在使用子路径时阻止 Windows 目录重命名替换。扫描失败的整项受保护；所有删除均限定在明确输出根内。

旧目录没有新 metadata 时默认只报告，即使名为 `CookInput-*`、`.previous-*` 或 `sessions`，也不能仅凭时间推断成功。CookInput 回收额外检查项目 marker 和 package 锁；失败 staging 还要求当前包 marker 匹配且没有 `.previous` 恢复槽。发布的 `.previous`、`.deleting-*` 恢复材料继续由原 CMake 发布协议处理，普通回收器不接管它们。绕过 hectl 直接调用底层 CMake 所产生的未登记历史也保持报告状态。

微信 `.he-export.json` 将备份绑定到精确项目和输出路径，`.publish.json` 记录替换事务。发布失败恢复旧输出，已完成回滚的失败 staging 按 7 天策略保留；中断后下一次 export 先恢复。首次发布没有旧输出时，只允许通过 manifest 全文件 hash 校验的 staging 完成恢复。只在当前导出存在且无 journal 时回收有 marker 的历史备份和过期 staging。旧导出无 marker 的备份仍保留，SDK、下载和 Wasm/原生构建缓存不参与普通回收。

内容导入 `content_sync.py` 本身已在 finally 回收本轮 UUID staging。持久 `state.json`、`pending.json`/journal、`Project/Assets/Imported`、SourceAssets、Engine/Content 一律不纳入此回收器；没有实现内容寻址资源 GC。

## 显式冷缓存操作

```powershell
.\hectl.bat clean cache -CachePath out/agent/game-tracy-debug -WhatIf --json
.\hectl.bat clean cache -CachePath out/agent/game-tracy-debug --json
```

仅接受完整的 hectl 隔离缓存单元：顶层或 `projects/<key>` 的 game/editor/tests 缓存、`out/package/build`、`out/pkg/<key>`，且必须有 CMakeCache。VS `out/build`、SDK、微信缓存和未知旧备份只报告。包含会话历史、CookInput、固定标记或未完成 codegen journal 的缓存拒绝删除。

构建、会话启动和 managed codegen 与回收共享缓存目录外的 lease；回收还检查内部锁和当前构建/运行进程。Windows 无法在子文件句柄打开时重命名目录，因此内部锁检查完成后在外部 lease 保护下将整套缓存原子改名，再删除。这样部分失败只留下报告中精确的 `.retired-*` 路径，不留下可被当作有效缓存继续使用的半套状态；原路径可重新 configure。不要与绕开这些锁的手动 CMake/IDE 操作并行运行显式缓存删除。

## 显式最大化清理

用户要求清理 `out`、C 盘空间不足或构建因空间失败时，优先运行 `hectl clean maximize -WhatIf` 检查候选，再运行 `hectl clean maximize`。这两步使用同一套检查；已有清理授权时无需再次确认。不要为这些已支持的范围另写临时递归删除脚本。

`maximize` 的范围固定为当前工作区的 `out`，不接受 `-Project`、`-Category`、`-CachePath` 或保留数量参数，也不处理 `dist`、源码和项目持久资产。它不由普通构建或 `auto()` 触发，不修改 `storage-policy.json`；`autoEnabled: false` 继续有效。

| 范围 | 最大化清理行为 |
| --- | --- |
| 已登记的历史证据 | 本次临时使用成功保留数 0、失败诊断天数 0；只删除已确认终态、没有依赖/父工作流保护的证据。全批次持有仓库 mutex，避免每删除一条就重新扫描全量历史；逐项复核记录、锁、路径和保护标记，保留归属记录供审计 |
| Game/Editor/Tests/项目/package、Android player 构建目录 | 核实 CMake 的源目录是本仓库，在 cache lease 和内部锁保护下，仅清理列明的构建产物；会话目录可以保留在同一个缓存内。先使 CMake 配置失效，外置 `storage/partial-caches` 记录支持删除中断后再次清理 |
| `.retired-<32位十六进制>` | 只处理受管缓存单元的精确同级目录，仍保留历史会话、登记依赖、保护标记和恢复材料 |
| 可重新取得的工具与模板 | 已知 Windows 依赖下载、FSR SDK/压缩包、脚本运行时缓存，以及有 `Template.json` 的标准生成模板；未知 SDK、临时 staging、备份和恢复槽继续保留 |
| 未登记的旧截图 | 仅在 `out/agent/captures` 和 `out/agent/4k60` 直属目录中，接受 `result.json` 的成功终态或已验证停止状态；未知终态不推断成功 |

目录联接、不可核实的运行进程、占用锁、`.he-keep`（包括祖先目录和嵌套标记）、pending/journal 恢复材料、登记依赖和未知产物继续保护。单项保护记录在 `skipped`，实际删除错误记录在 `failed`。不停止用户进程，不把“owner 已退出”改成成功。后续构建会重新配置、编译或下载已删除的缓存。

`-WhatIf` 不创建 lease、部分清理记录或报告，也不改变目录和文件。实际执行报告固定写入 `out/agent/storage/last-maximize-clean.json`，含候选列表、成功删除的逻辑字节、跳过原因、错误、所在卷清理前后的可用空间及差值。`observedLargest` 列出结束时 `out` 的前 30 个大项（预览时是当前占用），`inventoryErrors` 报告未穿过的联接和无法统计的路径。压缩、稀疏文件和并发磁盘活动会使物理空间变化与逻辑删除量不同。完成后检查报告中的剩余大项；未纳入明确范围的产物先核实归属，不因名称像缓存就删除。

`blocked by policy` 是命令执行层的拒绝，不是 `.he-keep` 或 hectl 的保留策略。仓库指令和本工具不能关闭或绕过该限制，也不保证任何命令一定获准执行。固定入口使范围、检查和测试可审查；若仍被拒绝，报告命令和原始原因，不更换解释器、伪造元数据或隐藏操作来绕过审查。

## 验证

```powershell
py -3 Tools/Agent/Tests/test_storage.py
py -3 Tools/WeChat/test_export.py
py -3 Tools/Content/test_content_sync.py
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Tests/Storage.Contract.ps1
```

临时夹具覆盖策略、保护优先级、未知历史、项目隔离、WhatIf、junction、锁与互斥、执行前状态变化、部分删除失败、外层生命周期、整套缓存 lease/重建路径、完整微信导出和回滚，以及真实 CMake staging/publish 和精确路径声明。缓存进程清单在该单元测试中模拟为空，不把用户当前进程混入夹具；实际 Windows 文件锁和 named mutex 会执行。

脚本检查不等于引擎运行验证。本次没有运行完整 HGame/Editor、真实 Tracy/WPR 采集、微信 DevTools 或 shipping 构建；不为工具修改全量重编引擎。真实项目只做只读 inventory 和预览，未删除未知历史或冷缓存。
