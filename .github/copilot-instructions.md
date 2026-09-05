# HEngineDev Agent Rules

<!-- Canonical guidance. After editing, run Tools/Agent/Sync-Guidance.ps1. -->

## Baseline

- 默认用中文反馈；命令、日志和符号名保留原文。用户指定其他语言时从其要求。
- 项目使用 C++20、CMake、Windows x64 和 VS2022 v143 工具链。
- 保持最小 diff，不做无关重构、全仓格式化或跨平台扩展。
- 未经明确要求，不修改 `Engine/Source/ThirdParty/`；严禁手动修改 `Engine/Source/_generated/`。

## Architecture and style

- 平台/API 实现与原生类型只能位于 `Engine/Source/Runtime/Platform/`；其他模块经公共抽象调用。处理存量违规时只缩小范围，不顺手做大规模迁移。
- Include 分组顺序：本地、其他引擎、第三方、STL；优先使用 Source 根路径。
- 命名沿用仓库：类型 PascalCase，局部/参数 lowerCamelCase，成员 `m`、静态 `s`、bool `b`。
- 使用 `Scope`/`Ref` 工具、`HE_*` 日志/断言；相对路径用 `std::string`，绝对路径用 `std::filesystem::path`，项目路径经 `ConfigManager` 解析。
- Editor ImGui 尺寸改动必须加载 `he-editor-imgui-dpi`。

## Workflow routing

只构建能覆盖改动的最小目标：

| Scope | Route |
| --- | --- |
| Game、非 Editor Runtime、玩法脚本/资源 | `he-build-validate`；默认 `hectl game check` |
| Editor 或 `WITH_EDITOR` 分支 | 额外执行 `hectl editor check` |
| 单个逻辑/序列化测试 | `he-single-testlayer-run`，使用无窗口 `HEngineTests` |
| Window、Renderer 或游戏 UI 集成行为 | `hectl game check`/`capture`；Editor UI 再加 `hectl editor check`/`capture` |
| 反射声明/字段/类型 | `he-reflection-codegen`，codegen 后再编译受影响目标 |
| 资源反射序列化 | 同时遵循 `he-resource-reflection-serialization`，只执行一次 codegen 链 |
| Shipping/cook/package | `he-pack-game` |
| 复用 VS 当前产物 | 仅用户明确要求时使用 `he-vs-shared-build` |

- 日常零参数入口是 `hectl`：构建 `HGame` 开发版（`HE_BUILD_DEV_GAME=ON`、`PACK_GAME=OFF`）；只有发布前最终验证才执行 `hectl package`。
- Codegen 统一入口是 `hectl codegen status|check|run`；`status` 只读并报告 dirty reasons，`check` 在 dirty 时非零退出，`run` 仅发布内容变化。机器调用加 `--json`，静默成功加 `--quiet`。
- Reflection 输出由 `HEngineCodeGen` 构建节点在各 build/host/config 的 `GeneratedSource` 内维护；manifest 位于 `.he-codegen/manifest.json`，完整日志位于 `out/agent/logs`。`build/GeneratedSource` 只用于兼容的 `Win-CppCodeGen.bat` 独立入口。
- `CPP_CODE_GEN` 仅保留为 cache 兼容项，不再触发 configure-time 生成。不得把 `Engine/Source/_generated` 或无 manifest 的过期 `out/agent/*/GeneratedSource` 当作构建输入、freshness 或验证证据。
- Agent 默认使用独立 `out/agent/<mode>-<config>`；不要复用或等待 VS 的 `out/build/*`，Editor 改动才额外构建 `HEngineEditor`。
- Agent 自动化只能使用会等待并回收进程的 `check`、`capture` 或 `agent start`/`stop`；禁止使用会保留用户进程的 `open`。会话必须在 `finally` 中停止。
- `HE_BUILD_DEV_GAME=ON` 是读取 loose `Project/` 内容的开发 Game，不是打包验证证据；Dev Agent Runtime 不得进入 `PACK_GAME` 构建。
- 任何反射影响改动都必须生成代码并编译；序列化不得在通用路径硬编码字段补丁。

## Validation

- 报告格式：证据类型（`build-only`、`runtime-manual`、`unit-test` 或 `test-layer`）+ 精确命令 + PASS/FAIL。
- 反射变更分别报告 codegen 与 build；运行时观察不能写成自动测试。
- Codegen 验证优先读取 manifest/status；no-op 证据必须同时报告 `changed=0`、`deleted=0` 和生成输出 hash/mtime 零扰动，不递归打印生成树。
- 第三方 `ctest` 不能作为引擎测试覆盖证据。

## Skills

按需加载：`he-build-validate`、`he-vs-shared-build`、`he-pack-game`、`he-single-testlayer-run`、`he-reflection-codegen`、`he-resource-reflection-serialization`、`he-editor-imgui-dpi`。
