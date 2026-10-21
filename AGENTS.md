# HEngineDev Agent Rules

仓库指导以本文件和 `.agents/skills/` 为准；直接修改源文件，无需同步副本。

## Conventions

- 默认用中文反馈；命令、日志和符号名保留原文。用户指定其他语言时从其要求。
- 项目使用 C++20、Windows x64；命令行构建优先 VS2026 v145（CMake 4.2+），回退 VS2022 v143，无需启动 VS IDE。
- 保持最小 diff，不做无关重构、全仓格式化或跨平台扩展。
- 未经明确要求，不修改 `Engine/Source/ThirdParty/`；不手改生成文件。
- 平台/API 实现与原生类型只能位于 `Engine/Source/Runtime/Platform/`；其他模块经公共抽象调用。处理存量违规时只缩小范围，不顺手做大规模迁移。
- Include 分组顺序：本地、其他引擎、第三方、STL；优先使用 Source 根路径。
- 命名沿用仓库：类型 PascalCase，局部/参数 lowerCamelCase，成员 `m`、静态 `s`、bool `b`。
- 使用 `Scope`/`Ref` 工具、`HE_*` 日志/断言；相对路径用 `std::string`，绝对路径用 `std::filesystem::path`，项目路径经 `ConfigManager` 解析。

## Development workflow

- 团队工作区由 `HEngine.workspace.json` 指向 `Engine/Templates/Blank`；小树保卫者已迁至独立仓库 `hebohang/StarbloomSurvivor`，本机目录为 `C:/WorkSpace/HEngineProjects/StarbloomSurvivor`。先用 `hectl project current -Json` 查看实际项目和输出路径，无需重复询问。本机/外部项目用 `hectl project use -Project <目录>` 设置忽略提交的默认绑定、`project reset` 恢复团队默认；一次性操作用 `-Project`。项目管理 UI 使用 `Win-ProjectManager.bat` 或 Editor 的“文件 → 项目管理”。
- 开发产物使用 `out/agent`，Game/Editor 默认 `RelWithDebInfo`；零参数 `hectl` 构建当前项目的 loose HGame。外部项目缓存位于 `out/agent/projects/<身份与路径哈希>/`，不得跨项目复用可执行文件、生成源码或 Agent 会话。详见 `Docs/Projects.md`。
- 用户手动看效果使用 `Win-RunEditor.bat` / `Win-RunGame.bat`（先构建再启动），或直接运行已有的 `out/agent/editor-relwithdebinfo/bin/HEngineEditor.exe` / `out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe`（不重新构建）。
- 构建与运行按需加载 `he-build-validate`。Editor 专属改动只验证 Editor；同时影响 Game 路径时再加 Game。按目的选择 `build`、`check` 或 `capture`；后两者已含构建，`capture` 已含运行。
- 只读审查和纯文档/指令修改无需引擎构建，按改动做相关检查。实现改动完成并通过所需验证后结束；没有新改动、失败或未解决疑点时不扩大或重复测试。
- 反射声明、持久化资源字段或生成器改动使用 `he-reflection-codegen`，由同一目标构建完成生成后编译；保持序列化由反射驱动，不在通用路径硬编码字段补丁。
- 单个逻辑/序列化测试使用 `he-single-testlayer-run` 的无窗口 `HEngineTests`；Window、Renderer、游戏 UI 集成改动运行相应目标的 `check`/`capture`。Editor ImGui 尺寸改动加载 `he-editor-imgui-dpi` 并验证相关面板。
- 仅 Shipping/cook/package 请求使用 `he-pack-game`；开发 Game 不能作为打包证据，Dev Agent Runtime 不得进入 `PACK_GAME` 构建。
- Agent 自动验证使用有界 `check`、`capture` 或自有 session，并在 `finally` 中停止会话；不调用保留用户进程的 `open`，不终止用户手动启动的进程。
- 按用户要求关闭普通历史自动回收（`storage-policy.json` 的 `autoEnabled: false`）。用户要求清理 `out`、C 盘空间不足或构建因空间失败时，授权范围内优先执行 `hectl clean maximize -WhatIf`，核对候选后执行 `hectl clean maximize`，无需重复询问。该入口统一处理已结束历史、可重建的缓存部分、`.retired-*`、已知下载缓存和生成模板；保留 `.he-keep`、运行中或终态不明内容、恢复材料、目录联接、未知来源产物和源码。按 `Tools/Agent/Storage.md` 检查报告中的剩余大项，报告逻辑删除量与清理前后磁盘实际可用空间。
- 清理优先使用上述固定入口及其报告，不临时拼接大范围递归删除命令。`blocked by policy` 属于执行层限制，`AGENTS.md` 和 `hectl` 不授予绕过权限；不要关闭安全审查、伪造元数据或换壳隐藏被拒绝的操作。若固定入口仍被拒绝，说明被拒绝的命令和给出的原因；受保护项通过明确的保留原因报告，不将一次局部拒绝当作所有清理都失败。

## Validation

- 对实际执行的验证报告证据类型（`static-check`、`build-only`、`runtime-manual`、`unit-test` 或 `test-layer`）、精确命令与 PASS/FAIL；未执行的明确说明。
- 反射变更分别报告 codegen 与 build 结果，可引用同一命令的两个阶段；运行时观察不能写成自动测试。
- 第三方 `ctest` 不能作为引擎测试覆盖证据。
