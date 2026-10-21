# 构建依赖精简实施与验收

范围：Windows x64；默认完整 Game/Editor 功能保持，新增可选构建能力。用户授权按以下顺序实施，不提交或推送 Git。

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 1 | 内置固定 libclang；统一依赖检查、自动安装及环境说明 | PASS（本机验证） |
| 2 | 按宿主隔离插件、工具和编辑器依赖 | PASS |
| 3 | Assimp 格式裁剪、运行时导入隔离、ZIP/zlib 独立归属 | PASS |
| 4 | 完整/DX12/Vulkan 后端配置及对应 Shader 构建 | PASS |
| 5 | 反射增量与依赖跟踪、历史依赖清理 | PASS |
| 6 | 2D/3D 物理、导航、SVG/Lottie 功能裁剪及最终矩阵 | PASS（本机矩阵） |

验证记录只填写实际执行的命令和结果。现有历史日志不计为本次通过。

## 验收要求

- libclang 不借用机器上的 LLVM 安装；版本、C API 头文件、资源头文件一致；外部覆盖显式且验证。
- 依赖脚本支持检查、安装、重复执行及缓存校验；默认项目所需 .NET/Python/Node/npm 纳入检查。
- 默认 Game/Editor 构建及有界运行；独立 Parser 契约；反射无变化时 hash/mtime 不变。
- 精简配置的依赖图中不存在已关闭功能的第三方链接；不支持的资源/组件明确报错。
- 单后端和功能配置通过对应构建、逻辑契约与有界运行。
- 保留完整日志并明确区分 static-check、build-only、unit-test、runtime-manual。

## 阶段 1 实际验收

- `build-only PASS`: `cmake -S . -B out/agent/dependency-validation/parser -G Ninja -DHE_CODEGEN_TOOL_ONLY=ON -DCMAKE_BUILD_TYPE=Release`，随后 `cmake --build out/agent/dependency-validation/parser --parallel 4`。
- `unit-test PASS`: `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Tests/HEngineParser.Contract.ps1 -ParserExecutable out/agent/dependency-validation/parser/bin/Programs/HEngineParser/HEngineParser.exe`。
- `unit-test PASS`: `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Dependencies/Test-Dependencies.ps1`，覆盖路径穿越、损坏哈希、过旧版本拒绝。
- `static-check PASS`: `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 deps install -Profile Editor -Json` 实际下载并安装 Node；再次带 `-Offline` 执行通过。Parser 离线配置检查通过。
- 范围限制：本机已有 VS、Python、.NET；尚未在全新 Windows 虚拟机执行这些安装器，不把已有环境检查计为全新机器安装验收。

## 阶段 2 实际验收

- `build-only PASS`: `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game build -Parallel 6`。
- `runtime-manual PASS`: `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 game check -Frames 30 -TimeoutSeconds 90 -Parallel 4`，READY，exit=0。
- `build-only PASS`: `cmake -S . -B out/agent/dependency-validation/shader -G Ninja -DHE_SHADER_TOOL_ONLY=ON -DCMAKE_BUILD_TYPE=Release`，`cmake --build out/agent/dependency-validation/shader --target HShaderCompiler --parallel 4`。
- `static-check PASS`: `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Dependencies/Test-BuildGraph.ps1 -BuildRoot out/agent/game-relwithdebinfo -AbsentSourcePattern '/(ThirdParty/embree|Plugins/Importers|Plugins/OfflineReferenceRenderer|ThirdParty/imgui-node-editor|ThirdParty/ImGuizmo)/'`，1,333 个编译命令；排除项为 0。
- OIDN 固定 2.5.1 下载/哈希校验及 `deps install -Profile Editor -Offline -Json` 重复安装通过；配置阶段不联网。
- Editor 首轮发现 ImGuizmo 头文件传递依赖，已改为 Editor 显式链接；`powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/hectl.ps1 editor build -Parallel 6` 重新验收 PASS。

## 使用新增入口

- `hectl deps check|install -Profile Editor|DevGame|Tests|Package|Parser`。`-Offline` 只允许使用已缓存且通过校验的归档；`-WhatIf` 检查缺项而不安装。
- `HE_CODEGEN_LIBCLANG_ROOT`（环境变量或 CMake 参数）为空时使用仓库固定 Win64 SDK；非空覆盖必须提供同格式 manifest 和匹配的文件哈希，不再扫描本机 LLVM。
- `HE_CODEGEN_TOOL_ONLY=ON` 独立 Parser；`HE_SHADER_TOOL_ONLY=ON` 独立 ShaderCompiler，均不配置游戏插件、编辑器及运行时。
- `HE_BUILD_ASSET_TOOLS` 默认仅 Editor 开启；`HE_BUILD_RENDER_SMOKES` 默认关闭。hectl 为明确请求的工具/smoke 目标开启对应宿主。
- OIDN 默认位置 `out/agent/dependencies/windows/oidn`，由依赖脚本准备；审计过的替代路径可用 `HE_OIDN_ROOT` 指定。

## 阶段 3 实际验收

- `build-only PASS`: `hectl game build -Parallel 6`、`hectl editor build -Parallel 4`（完整调用前缀同上），记录见 `out/agent/dependency-validation/*-stage3.log`。
- `runtime-manual PASS`: `hectl game check -Frames 30 -TimeoutSeconds 90 -Parallel 4`，READY / exit=0。
- `unit-test PASS`: `cmake --build out/agent/editor-relwithdebinfo --target HEDependencyContracts --parallel 4`；`out/agent/editor-relwithdebinfo/Engine/Source/ThirdParty/HEDependencyContracts.exe out/agent/dependency-validation/import-contract`。ZIP→minizip 包含 NUL 的负载往返、损坏包拒绝、六种扩展名支持、六种移除格式拒绝、实际 OBJ 导入。
- `static-check PASS`: `Test-BuildGraph.ps1 -BuildRoot out/agent/game-relwithdebinfo -AbsentSourcePattern '/ThirdParty/assimp/|/Plugins/Importers/' -PresentSourcePattern '/ThirdParty/Compression/'`；1,105 个编译命令，无 Assimp。
- Game 默认禁用 Runtime 旧式原始模型导入；Editor 默认保留直接模型预览兼容能力，编辑器导入插件仍保留 FBX/GLTF/GLB/OBJ/DAE/BLEND。兼容旧调用时可显式开启 `HE_RUNTIME_ASSET_IMPORT`（Shipping 禁止）；禁用旧式导入时加载原始模型会明确要求先导入为 `.hmesh`。

## 阶段 4 实际验收

- `build-only / runtime-manual PASS`: `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/Agent/Build.ps1 -Target HGame -Configuration RelWithDebInfo -RenderBackends DX12 -Parallel 4 -Run -Wait -ExitAfterFrames 30 -TimeoutSeconds 90 -GraphicsApi DX12 -StrictGraphicsApi -Resolution 1280x720`；同样命令将两处 DX12 换为 Vulkan，通过 READY / exit=0。
- `unit-test PASS`: 双后端独立 ShaderCompiler 与 DX12-only ShaderCompiler 的 `--self-test`；两种单后端 HCooker 的 `--self-test`。Cooker 验证期间用 CMake 临时开启 `HE_BUILD_ASSET_TOOLS=ON`，构建 `HCooker` 后恢复 OFF。
- `static-check PASS`: DX12 编译图 958 条命令，无 SPIRV-Tools/Vulkan；Vulkan 编译图 1,066 条命令，无 D3D12/LLGI DX12/mipmap 编译器。GeneratedContent 中分别没有 SPIRV/DXIL 文件；Vulkan exe 不导入 d3d12.dll。证据在 `out/agent/dependency-validation/*-stage4.*`。
- 修复验收发现：LLGI Windows 无条件 DX12；Effekseer/ImGui 缓存目录硬编码 DXIL；仅 Vulkan 时错误选择 DX12 作为配置回退。
- 磁盘不足曾中断两次构建，不能计为通过。按存储工具完成清理后，对本次使用的 Game/Editor 和两个单后端缓存启用 NTFS 压缩，随后重新构建和运行通过，未删除未知缓存或用户资源。

## 阶段 5 实际验收

- `build-only PASS`: `hectl game build -Parallel 4`，Parser 生成后 Game 消费端编译通过。
- `unit-test PASS`: `HEngineParser.Contract.ps1`、`CodeGenDriver.Smoke.ps1`、`CodeGenEndToEnd.Contract.ps1`；新增真实间接 include 修改检测。
- `unit-test PASS`: `pwsh -NoProfile -File Tools/Dependencies/Test-ReflectionDiscovery.ps1`，普通现有头文件新增反射标记被发现，删除夹具后生成项清理通过。
- `unit-test PASS`: `pwsh -NoProfile -File Tools/Dependencies/Test-CodeGenNoOp.ps1`，273 个生成文件内容哈希与修改时间均不变，changed/deleted=0。
- Driver 追踪 libclang 实际 include 闭包；新增闭包先捕获哈希再复解析，保留并发修改拒绝与事务回滚。CMake 每次只执行一次 Ensure，普通头文件修改可触发输入发现。
- 旧 LLVM bin/lib/include、根目录 Glad/glslang/shaderc、未引用 D3D12/Vulkan 分配器和 HJsonParser.cmake 已移出源码树；可恢复归档位于 `out/agent/dependency-validation/retired-sources`。归档仍占磁盘空间，嵌入 Effekseer 的依赖保留。

## 功能裁剪入口（阶段 6）

五个 CMake 开关默认均为 ON：`HE_ENABLE_PHYSICS_2D`、`HE_ENABLE_PHYSICS_3D`、`HE_ENABLE_NAVIGATION`、`HE_ENABLE_UI_SVG`、`HE_ENABLE_UI_LOTTIE`。关闭后不配置、编译或链接对应第三方库，组件数据结构仍可反射/序列化，但添加或加载不支持的组件会给出开关名称并拒绝操作。导航请求、碰撞网格导入及 `<svg>`/`<lottie>` 元素有明确拒绝路径。Editor 添加组件菜单禁用不可用项。

```powershell
# 默认完整功能
powershell -File Tools/Agent/Build.ps1 -Target HGame -RenderBackends All
# DX12 + 五项全关，独立缓存 game-dx12-minimal-relwithdebinfo
powershell -File Tools/Agent/Build.ps1 -Target HGame -RenderBackends DX12 -RuntimeFeatures Minimal
# 单项或多项关闭（PowerShell 调用）
& ./Tools/Agent/Build.ps1 -Target HGame -DisableFeature Physics3D,Navigation
```

功能选择自动区分构建缓存。切换 CMake 参数时需重新编译；该功能不是运行时卸载模块。完整 Starbloom 游戏可能依赖已关闭功能，精简运行应使用不含这些组件的场景。

- `static-check PASS`: `pwsh -File Tools/Dependencies/Test-FeatureMatrix.ps1`，五个独立关闭配置均移除对应依赖并保留其他四项。
- `static-check PASS`: 精简 DX12 Game 共 693 条编译命令，相比完整 DX12 的 958 条减少 265 条（约 27.7%）；不据此推算实际编译耗时。

- `unit-test PASS`: `pwsh -File Tools/Dependencies/Test-RuntimeFeatures.ps1 -BuildRoot out/agent/game-dx12-minimal-relwithdebinfo -Parallel 2`；使用 HEngineTests 的 BuildFeatures 契约，覆盖禁用组件添加/替换及反序列化拒绝、导航失败与路径清空、空场景行为树启动、SVG/Lottie 元素错误信息。测试临时切换独立验收缓存为 Tests 宿主并在 finally 恢复 DevGame。
- `build-only / runtime-manual PASS`: `pwsh -File Tools/Dependencies/Test-MinimalRun.ps1`；DX12-only + 五功能关闭，临时空场景，READY / 30 帧 / exit=0；夹具运行后移到验收归档。
- `build-only / runtime-manual PASS`: `hectl editor check -Frames 30 -TimeoutSeconds 90 -Parallel 2`，默认完整 Editor exit=0。
- 验收修复了导航关闭时行为树仍无条件预构建 NavMesh，以及多次调用源码收集宏覆盖平台/2D 物理源码的问题；最终独立开关矩阵同时检查公共源码保留。
- PDB 写入曾因磁盘不足失败。通过 `hectl clean cache -CachePath ... -WhatIf -Json` 预览并回收本轮已完成的 `game-dx12-relwithdebinfo`、`game-vulkan-relwithdebinfo` 缓存，外部阶段 4 验收日志保留。清理报告显示已删除大部分文件，但长路径目录有残留，精确 `.retired-*` 路径保存在 `retire-*-result.json` 中，未将部分回收写成全部删除。为满足清理工具的活动进程保护曾停止本轮 Editor 编译，之后已重新构建和运行通过。

- `build-only / runtime-manual PASS`: `hectl game check -Frames 30 -TimeoutSeconds 90 -Parallel 4`，默认完整 Game READY / exit=0。
- `unit-test PASS`: `pwsh -File Tools/Dependencies/Test-CodeGenNoOp.ps1 -BuildRoot out/agent/editor-relwithdebinfo`，以及同命令指向 `out/agent/game-dx12-minimal-relwithdebinfo`；均为 273 个输出 hash/mtime 相同、changed/deleted=0。
- `static-check PASS`: 最终 `hectl deps install -Profile Editor -Offline -Json`；依赖已准备时重复执行通过。新增 PowerShell 脚本语法解析与源文件 diff 空白检查通过。

- `unit-test PASS`: `pwsh -File Tools/Dependencies/Test-RuntimeFeatures.ps1 -BuildRoot out/agent/game-relwithdebinfo -Parallel 4`，完整功能 BuildFeatures 契约通过；随后 `out/agent/game-relwithdebinfo/bin/HEngineTests.exe --run SystemLifecycle` 在自有进程中按 60 秒超时约束运行并通过，覆盖 3D 物理重复启动/停止及脚本生命周期。
- `unit-test PASS`: 恢复 DevGame 上下文并执行 `cmake --build out/agent/game-relwithdebinfo --target HEngineCodeGen --parallel 2` 后，`pwsh -File Tools/Dependencies/Test-CodeGenNoOp.ps1 -BuildRoot out/agent/game-relwithdebinfo` 再次验证 273 个输出 hash/mtime 不变，changed/deleted=0。
- `static-check PASS`: 最终完整 Game 1,105 条编译命令，无 Assimp、Embree、编辑器控件与 Importers/OfflineReferenceRenderer 实现；精简 DX12 Game 693 条，无关闭功能及 Vulkan/SPIRV 实现。

所有阶段现已按顺序完成本机验收。Windows 全新虚拟机安装、其他平台和完整 Shipping 包未执行，不能将本机已有环境的检查或 Cooker 自测视为这些场景的验证。工作区保留变更，未执行 Git 提交/推送。
