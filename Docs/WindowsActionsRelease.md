# Windows SDK 的免费 Actions 发布流程

本流程只允许在**公开仓库**使用 GitHub 标准托管 runner。不得通过增加预算、启用大型/GPU runner、自托管云机器或收费缓存来解决构建失败。

实现入口为 `.github/workflows/release_windows.yml`。当前仍在进行 ARM64 集成验证；未通过的架构不能作为已支持的平台发布。

## 执行与费用边界

- 仅 `workflow_dispatch`；不绑定 push、PR、schedule 或 release 事件。
- 每个 job 在分配 runner **之前**检查手动事件、`private == false` 和 `visibility == public`。私有 HEngineDev 中整个流程跳过，不消耗私有仓库构建额度。
- 固定 runner：`ubuntu-24.04`、`windows-2025-vs2026`、`windows-11-vs2026-arm`；输入不能指定任意 runner 标签。
- Python 控制器再次检查仓库身份、runner 类型、OS 和架构；发布前通过 GitHub API 再确认仓库仍为公开。
- 不使用 Actions cache，不调整账户预算，不调用任何计费设置 API，不安装云端机器。Actions 临时产物只在上述公开标准 runner 流程中使用，保留 1 天；最终 ZIP 放在 Release。
- 磁盘不足只调用仓库现有的 `hectl clean maximize -WhatIf` / `hectl clean maximize`。保留待发布产物；仍不足则失败，不切换付费 runner。
- 保持账户已有的 Actions `$0` 超额预算。此 workflow 的源代码保护不能替代账户预算，也不能约束使用者另行修改的 workflow。

政策依据（2026-10-15 核对）：[Actions 计费](https://docs.github.com/en/billing/concepts/product-billing/github-actions)、[标准 runner](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)。公开标准 runner 免费；大型 runner 不享受该免费规则。

## 手动输入

| 输入 | 含义 |
| --- | --- |
| `architecture` | `x64`、`arm64` 或 `both`；默认 `both` |
| `publish` | 默认 false，只产生验收后的 CI 下载产物；true 时所有所选架构通过后才创建/发布 Release |

版本号从同一提交的 `CMakeLists.txt` 获取，不接受任意 shell 字符串。发布源码固定为 `GITHUB_SHA`。正式发布前需要统一更新引擎和模板工程版本。

已发布的 `v1.1.0` 不会被该流程覆盖。后续版本使用新版本号；CI 不移动已有标签，也不覆盖已发布的 Release。断点重试仅允许同一源码创建、内容相同的草稿；冲突附件会报错，不会被悄悄替换。

## 构建与验收

每个 Windows job 复用仓库的 Source dependency、Editor、Shipping/Development template、Android template 和 SDK 组装入口。x64 与 ARM64 使用独立缓存身份，禁止跨架构复用二进制或生成源码。

SDK 在归档前必须通过：

1. 文件布局与原生依赖闭包检查。
2. PE 架构检查，包含 DLL、EXE、Python `.pyd`；只放行真正 AnyCPU 的托管 IL，不能把 x64 托管 EXE 当成 AnyCPU。
3. WARP 下有界 Editor 启动及真实 SDK API 游戏探针。
   FSR 另有独立的 DX12 WARP 执行门禁：640×360 / 800×450，实际 dispatch、像素读回、历史重置与尺寸变化；最多运行 300 秒。只通过 PE 架构检查不能发布。
4. 从 SDK 新建的同一个 SharedArena 工程执行 Windows、Android、Web、微信导出。
5. ZIP CRC、大小与 SHA-256 检查。

发布 job 必须收到全部所选架构的成功回执，核对源码提交、版本、架构、归档哈希及运行能力清单。上传后再次比较 GitHub 返回的大小和 SHA-256，然后发布。发布凭据仅使用当前仓库短期 `GITHUB_TOKEN`；构建 job 只有 `contents: read`。

Release 附件区只上传所选架构的 SDK ZIP；另外两个 Source code 归档由 GitHub 自动提供。`SHA256SUMS.txt`、`artifacts.json`、`RELEASE_PROVENANCE.json` 和 `VALIDATION.md` 不再作为 Release 附件上传。每个 ZIP 的大小、SHA-256、固定源码提交、工作流链接、验证摘要及能力限制写入 Release 说明；完整逐架构回执与执行命令在归档前写入 SDK 的 `Docs/Release/RELEASE_PROVENANCE.json`，发布时必须与 CI 回执一致。归档清单和校验文件仍用于 CI 内部传递和验证。

现有 v1.1.0 的精简仅迁移说明与删除四个辅助附件，原 SDK ZIP 不重打包、不替换；原完整验收记录和构建来源保留在该 Release 说明的折叠详情中。

2026-10-15 精简验证：
- `unit-test`：`py -3.11 Tools/Distribution/Release/test_publish.py -v`，PASS（10 项，包含只发布 ZIP、说明中的大小/哈希/来源/限制、ZIP 字节不变及内外回执一致性）。
- `unit-test`：`py -3.11 Tools/Distribution/Release/test_contract.py -q`，PASS（10 项，免费执行保护保持不变）。
- `static-check`：`out/agent/release-arm64/actionlint/actionlint.exe .github/workflows/release_windows.yml`、`Build-CI.ps1` PowerShell 语法解析、`py -3.11 -m py_compile Tools/Distribution/Release/publish.py Tools/Distribution/Release/test_publish.py`、`git diff --check`，均 PASS。
- 远端核对 PASS：v1.1.0 仅保留上传的 SDK 附件 `642335363`（另有 GitHub 自动源码归档），大小仍为 550164193 bytes，SHA-256 仍为 `3b151610980a35fc4d7fa837fcc21b4eae0b89b99c0f7afa5fb6847c478a7926`；SDK 附件时间戳和版本标签均未改变。本轮未运行 Actions 或重新构建 SDK。

Editor 启动不等于编辑器内 Play/Stop 游戏测试。WARP/云端构建不代替物理显卡、Android 真机或微信设备验证；这些限制会进入发行验证报告。

FSR 验收使用 `warp-test.lock.json` 锁定的 Microsoft.Direct3D.WARP 1.0.21（x64 / ARM64），仅放在验收程序目录。系统自带的旧 WARP 在本机 FSR dispatch 时发生访问异常；固定新版后同一测试通过。该包许可限定开发测试，不进入 SDK 或游戏；发行架构审计明确拒绝任何 `d3d10warp.dll`。此测试依赖免费，无需 GPU runner，也不替换系统驱动。

## Windows ARM64 依赖

`Tools/Dependencies/windows-arm64.lock.json` 锁定以下免费来源，下载后必须核对哈希或完整 Git 提交：

- Microsoft DirectX Agility SDK 1.619.5 的 ARM64 运行库。
- Microsoft DXC 1.8.2505.32 的 ARM64 shader compiler。
- CPython 3.11.9 的官方 ARM64 NuGet 包。
- Embree 4.4.0 固定提交，从源代码构建静态库，使用 INTERNAL 任务调度器。
- Vulkan Loader 1.4.352 固定提交的导出定义，用 ARM64 工具链生成导入库。
- FidelityFX SDK 1.1.4 官方发行 ZIP 的 SHA-256 固定；提取 `sdk/` 和 `ffx-api/`，使用 `DX12_ARM64` / Release 构建 `amd_fidelityfx_dx12.dll`。这是从官方源码构建的库，不冒充 AMD 签名预编译库。
- Node.js 22.14.0 ARM64 官方 ZIP 的 SHA-256 固定；ARM64 SDK 的 esbuild 使用 `@esbuild/win32-arm64`，不会将 x64 npm 原生工具混入 SDK。
- .NET 10.0.10 ARM64 运行时另外由 `runtime.win-arm64.lock.json` 的官方 SHA-512 锁定。

反射解析器作为构建宿主工具，可使用既有的 x64 libclang 在 Windows ARM64 的 x64 仿真环境执行，不进入 ARM64 SDK。引擎进程内加载的运行库必须为 ARM64。

OIDN 按发行要求仅随 x64 离线渲染插件提供；三个 OIDN CPU DLL 位于 `Plugins/OfflineReferenceRenderer`，与 Embree 共用的 oneTBB 保留单份。Windows ARM64 固定关闭 OIDN 后端并跳过 OIDN 依赖下载，保留离线渲染插件的原始输出。Shipping 游戏不携带 OIDN。

FSR ARM64 原生构建已接入 `Prepare-WindowsArm64.ps1`。构建后核对 PE 为 ARM64，并生成 `HEngine.FsrBuild.json`，记录源 ZIP 哈希、运行库哈希和编译器版本；配置和打包均核对该回执。x64 继续使用原先锁定的 AMD 签名库。

`HEngine.Runtime.json` 分别声明 `fsrDx12` / `fsrVulkan`，ARM64 为 true / false。Vulkan 渲染器仍然保留；选择它时通过现有普通渲染回退继续游戏。FSR 的头文件引用、原生调用和 DLL 打包按后端能力分别启用。旧 x64 模板没有这两个字段时保持兼容，ARM64 则必须明确声明，禁止打包时猜测。实际能力和验证限制写入 Release 报告。尚未进行 ARM64 实机游玩验证。

## 本地检查

```powershell
py -3.11 Tools/Distribution/Release/test_contract.py -v
py -3.11 Tools/Distribution/Release/test_publish.py -v
py -3.11 Tools/Distribution/test_distribution.py
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-Toolchain.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Test-Template.ps1
actionlint -shellcheck= .github/workflows/release_windows.yml
```

ARM64 依赖获取检查与原生编译分开：

```powershell
# 只核对下载、锁定哈希和 ARM64 PE，不代表原生编译成功。
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Dependencies/Prepare-WindowsArm64.ps1 -DownloadOnly

# 在具备 ARM64 MSVC 工具链的环境准备原生依赖。
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Dependencies/Prepare-WindowsArm64.ps1
```

本地 `Build-CI.ps1` 不提供跳过免费执行检查的参数；要验证本地构建，直接调用现有 `Build.ps1` / template builder。迁移到公开 HEngine 时同步完整所需源码和锁定依赖入口；不在公开 workflow 中检出私有 HEngineDev。

## 本机阶段验收（2026-10-15）

本轮未推送公开仓库、未触发任何 GitHub Actions、未调整账户预算，也未改动已发布的 1.1.0 附件。下列结果不构成 ARM64 已发布或云端全链路通过的声明。

| 证据类型 | 命令或检查 | 结果 |
| --- | --- | --- |
| static-check | `actionlint -shellcheck= .github/workflows/release_windows.yml`（1.7.12） | PASS |
| unit-test | `py -3.11 Tools/Distribution/Release/test_contract.py -v` | PASS，9 项；拒绝私有仓库、非手动事件、非白名单 runner、被 `OR` 绕过的条件和架构混用 |
| unit-test | `py -3.11 Tools/Distribution/Release/test_publish.py -v` | PASS，6 项；缺少架构、损坏 ZIP、错源码、缺验收、已有正式版本和标签冲突均阻止发布 |
| unit-test | `py -3.11 Tools/Distribution/test_distribution.py`；`py -3.11 Tools/Build/test_preflight.py` | PASS，分别 16 / 15 项 |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Test-Toolchain.ps1`；同方式运行 `Tools/Agent/Test-Architecture.ps1`、`Tools/Distribution/Test-Template.ps1`、`Tools/Dependencies/Test-RuntimeArchitecture.ps1` | PASS；包含严格模式下旧 x64 运行库清单兼容性 |
| build-only | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineEditor -Configuration RelWithDebInfo -Project Engine/Templates/Blank -PortableEditor -PluginConfig Tools/Distribution/Plugins.json -Architecture x64 -Parallel 2` | codegen PASS、build PASS；最后一轮 93.3s |
| unit-test | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Dependencies/Test-CodeGenNoOp.ps1 -BuildRoot out/agent/projects/e1caca9aebe7fcb3/editor-portable-relwithdebinfo` | PASS：321 个生成文件 hash/mtime 相同，changed=0、deleted=0 |
| static-check | `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Dependencies/Prepare-WindowsArm64.ps1 -Root C:/WorkSpace/HEngineDev/out/agent/release-arm64 -DownloadOnly` | PASS：5 项锁定依赖输入、官方 .NET ARM64 SHA-512、实际 ARM64 PE 头；未执行 ARM64 原生编译 |
| static-check | `py -3.11 Tools/Distribution/Release/contract.py audit --root C:/WorkSpace/HEngineSDKLab/free-ci-20261015/HEngine-1.1.0-Windows-x64 --architecture x64` | PASS，615 个 PE 文件 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File C:/WorkSpace/HEngineSDKLab/free-ci-20261015/HEngine-1.1.0-Windows-x64/Tools/Distribution/Test-ScriptSdk.ps1 -Engine C:/WorkSpace/HEngineSDKLab/free-ci-20261015/HEngine-1.1.0-Windows-x64 -Evidence C:/WorkSpace/HEngineDev/out/agent/release-arm64/sdk-gameplay-r2 -UseWarp` | PASS：Development / Shipping 真实脚本、Windows 导出与音频播放状态；静音 PCM 不代表听感验收 |
| test-layer | `powershell -NoProfile -ExecutionPolicy Bypass -File C:/WorkSpace/HEngineSDKLab/free-ci-20261015/HEngine-1.1.0-Windows-x64/Tools/Agent/ProjectLaunch.ps1 -Project C:/WorkSpace/HEngineDev/out/agent/release-arm64/sdk-gameplay-r2/Project -Log C:/WorkSpace/HEngineDev/out/agent/release-arm64/editor-startup-final.log -Check -UseWarp -StartupTimeoutSeconds 300` | PASS：窗口就绪、响应检查、正常关闭并退出 0；未做 Play/Stop |
| static-check | `py -3.11 Tools/Distribution/Archive-Release.py --release C:/WorkSpace/HEngineSDKLab/free-ci-20261015 --engine-only --version 1.1.0 --architecture x64` | PASS：11,811 个文件，550,190,432 bytes，ZIP CRC 检查通过；这是本机验证包，未发布 |

新增 SDK 组装通过 124 项布局检查和 215 个 PE / 401 项导入检查。该本机验证复用此前验收的 `scripting110-r3` Windows、Development 和 Android 原生模板，不能算本轮重新构建了 ARM64 或 Android Player。

过程中修复了 Windows 平台变量被 Web/微信复制循环覆盖，以及旧模板缺少 `architecture` 字段时在严格模式下报错的问题。no-op 检查驱动也改为按 CMake 实际退出码判定，避免 PowerShell 5 把原生 stderr 警告当成失败，警告仍保留在日志中。首次缺失的 OIDN 开发缓存已用锁定依赖恢复；CMake 下载停滞后复用既有 SDK 的同版本工具缓存并通过依赖检查。既有 E 盘构建目录联接产生 storage 登记警告，未绕过或删除联接。

剩余：本机未安装 MSVC ARM64 编译组件；ARM64 原生构建与运行、公开仓库 Actions 全流程均未执行。用户已选择在公开仓库手动运行 Actions；不再补装本机组件，也不在私有仓库运行。OIDN 的 ARM64 排除与 FSR DX12 ARM64 构建/打包链已实现，首次原生构建和运行结果必须由 Actions 门禁确认。

## 首次手动运行

将本次所需源码、工作流和锁定依赖入口同步至公开 HEngine 仓库的默认分支后，进入 **Actions → Release Windows SDK (free runners only) → Run workflow**：

1. 首次选择 `architecture=arm64`、`publish=false`，完成 ARM64 构建、WARP 验证与四平台导出。成功后下载 `sdk-arm64`，内含 `HEngine-1.1.0-Windows-arm64.zip`、校验文件和 `result.json`。临时下载产物只保留 1 天。
2. 检查验证回执，并在 ARM64 设备补充编辑器交互和实际游戏检查；云端 WARP 不代表 Adreno 等物理 GPU 兼容性。
3. 正式发布选择 `both` 和 `publish=true`；所选架构全部通过才发布。若目标公开仓库尚无 `v1.1.0`，可发布 1.1.0；若已有该版本，则该流程拒绝覆盖，需先统一升级源码/模板版本。

任何编译、运行或空间不足错误都会使流程失败；失败日志位于 `failure-arm64`。不会自动切到付费 runner、增加预算或跳过失败的验收。

本次 OIDN 范围调整的 `static-check`：
- `cmake -DHE_WINDOWS_ARCH=arm64 -P out/agent/release-arm64/check-oidn-profile.cmake`：PASS，即使预置 ON 缓存也关闭 OIDN。
- `cmake -DHE_WINDOWS_ARCH=x64 -P out/agent/release-arm64/check-oidn-profile.cmake`：PASS，x64 保留 OIDN。
- `Dependencies.ps1` PowerShell 语法解析、`git diff --check`：PASS。本次未执行 ARM64 编译或设备运行。

## 本轮 FSR ARM64 接入验证（2026-10-15）

- `unit-test` PASS：`py -3.11 Tools/Dependencies/test_fsr_cmake.py -v`，5 项，包含错误架构 DLL、源包哈希、产物篡改与 DX12/Vulkan 能力分离。
- `unit-test` PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Dependencies/Test-FsrRuntime.ps1`，验证 ARM64 FSR 打包契约和 Shipping 排除项；同方式执行 `Test-RuntimeArchitecture.ps1`，验证旧 x64 契约兼容性。
- `unit-test` PASS：`py -3.11 Tools/Distribution/Release/test_contract.py -v`（10 项）、`py -3.11 Tools/Distribution/Release/test_publish.py -v`（8 项）。
- `unit-test` PASS：原有 `py -3.11 Tools/Distribution/test_distribution.py` 的 16 项；新增 `py -3.11 Tools/Distribution/test_distribution.py -k arm64 -v` 的 1 项验证 ARM64 esbuild 布局及缺失拒绝。
- `static-check` PASS：`out/agent/release-arm64/actionlint/actionlint.exe .github/workflows/release_windows.yml`、变更 PowerShell 文件语法解析、Python 编译检查和 `git diff --check`。
- `static-check` PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Dependencies/Prepare-WindowsArm64.ps1 -Root out/agent/release-arm64 -DownloadOnly`，6 项锁定输入，包含 FSR 官方 ZIP；这不是 ARM64 原生编译。
- `build-only` PASS（codegen/build）：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HEngineEditor -Configuration RelWithDebInfo -Project Engine/Templates/Blank -PortableEditor -IncludeRenderSmokes -PluginConfig Tools/Distribution/Plugins.json -Architecture x64 -Parallel 2`，最终 Editor 构建 606.2 秒；日志 `out/agent/release-arm64/x64-fsr-release-final.log`。

首次尝试在 Editor 构建目录编译 FSR 验收程序时，开发 Game 专用的纹理上传故障注入接口不存在；已将该测试源按 `HE_BUILD_DEV_GAME` 隔离，Editor 的 Release 验收仍执行原生 FSR 测试。未修改第三方源码。

- `test-layer` PASS：`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Distribution/Release/Test-FsrRelease.ps1 -BuildRoot out/agent/projects/e1caca9aebe7fcb3/editor-portable-relwithdebinfo -Architecture x64 -Evidence out/agent/release-arm64/fsr-warp-final`。实际加载 FSR 3.1.4，在固定 WARP 1.0.21 下完成 320×180 → 640×360、400×225 → 800×450 的 dispatch、读回、重置、尺寸变化和非法输入拒绝；两组最大读回误差均为 0.00363982。最终日志 `out/agent/release-arm64/x64-fsr-warp-final.log`。
- `static-check` PASS：`CMake/VerifyGamePackage.cmake` 针对独立目录夹具验证旧 x64 契约和只带 DX12 FSR 的 ARM64 清单布局；夹具沿用 x64 二进制，不作为 ARM64 运行证据。

ARM64 原生库、ARM64 SDK ZIP、ARM64 四平台导出和公开 Actions 整体运行均留待用户手动触发验证；本轮没有生成或发布 ARM64 正式包，也没有改动已发布的 v1.1.0 标签与附件。
