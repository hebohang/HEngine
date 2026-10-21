# 插件管理与脚本语言方向

HEngine 的新玩法主力是 C#；Python 保留为编辑器工具、桌面扩展和既有桌面玩法兼容层。渲染、物理和平台宿主仍由 C++ 实现。

当前项目 `Config/Plugins.json` 保持 C#、Python 在 Editor / DevGame / Game / Tests 中开启。原因是默认 `MainMenu.he` 仍绑定 `Scripts.SurvivorGame.py`，正式渲染基准轨迹也依赖 Python。直接关闭会破坏现有内容。新的纯 C# 项目可以关闭 DevGame / Game 的 Python；Editor 可以继续开启两者。`Config/Plugins.Portable.json` 则只启用 C#，Python 在 Web、微信均不支持。

## 面板

编辑器菜单 **插件 → 插件管理**（英文 Plugins → Plugin Manager）。

中文按钮分别是“校验并应用”“放弃改动”“重新读取配置”；下文同时给出对应英文名称。配置列表的“桌面项目”“Web / 微信”分别对应 Desktop project 和 Web / WeChat。

- Desktop project 编辑当前工程 `Config/Plugins.json`；Web / WeChat 编辑 `Config/Plugins.Portable.json`。
- 从引擎 `Source/Plugins/*/Plugin.json` 发现全部插件，因此关闭后仍能在面板看到并重新启用。名称、版本、API、依赖和平台来自这些已有声明。
- Current Editor 显示当前可执行文件中的实际注册/启动状态；下方各宿主复选框是下次构建配置。未勾选不表示当前进程已经卸载插件。
- **Validate and apply** 在后台调用 `Tools/Plugins/ApplyConfig.ps1`，使用正式 CMake 插件解析器检查所有声明、依赖、宿主和平台。桌面还分别校验 Development / Shipping；portable 校验 Web / WeChat。通过后原子替换配置，并在 Saved/PluginSettings 保存原配置与校验记录。
- 读取后配置被其他程序修改时拒绝覆盖。校验失败保留待修改内容。Play 期间不可修改。关闭窗口只隐藏面板；退出编辑器会回收仍在执行的校验进程。
- **Discard changes** 撤销待保存改动，**Reload from disk** 重新读取文件。切换配置前先应用或放弃当前改动。
- 不支持的宿主/平台不能新启用。缺失插件的配置保留并报错；场景数据不会因关闭插件而删除。

保存配置后先保存场景、退出编辑器，再执行需要的目标：

```powershell
.\hectl.bat editor check
.\hectl.bat game check
.\hectl.bat package
.\hectl.bat scripting build-wechat
```

只构建你需要的目标。当前窗口不是动态插件卸载器，不会在运行中的 EXE 上重建/覆盖自身。发布仍由 cooker 检查实际可达场景和脚本依赖；面板校验通过不意味着一个使用 Python 的场景能在关闭 Python 后发布。显式命令行 `-PluginConfig` 使用的覆盖配置不由此面板修改。

## Editor 实际操作验收

`Assets/WeChatSample/ScriptEditorSetup.he` 是保留的操作起点，包含相机和两个 Sprite，故意不包含脚本组件。它不是已经完成的 UI 验收证据。

用户直接启动入口：`.\Win-RunEditor.bat -StartupScene Assets/WeChatSample/ScriptEditorSetup.he`。

1. 打开此场景，用“另存为”保存到 `Assets/WeChatSample/ScriptEditorManual.he`，保留原始操作起点。
2. 选中 `Add CSharp script here`，在属性面板 **Add Component → Script → Script** 添加通用脚本组件。选择 Backend `he.script.csharp`、Script Type `sample.touch`。
3. 编辑 `Speed=5`、`Score=41`、`SaveKey=editor-manual-20260911`；如果重复验收，换一个未使用的 SaveKey，避免已有存档覆盖 Score。
4. 选中 `Add Python script here`，同样添加通用 Script 组件，Backend 选 `he.script.python`，Script Type 选 `sample.python-observer`。编辑 `ObservedScore=17`，两个故障开关保持 false。
5. Ctrl+S，关闭编辑器。重新启动并打开刚保存的场景，确认两个后端、类型及上述字段仍在。
6. 点击 Play，确认 C# 日志包含 `score=41 speed=5`、两种语言均出现 Initialize / Enable。聚焦游戏视口后移动、点击，C# 分数每次加 3，Python 日志出现 observed C# score。
7. 点击 Stop，确认两者 Disable / Destroy，回到编辑态后字段仍是编辑态保存值。再点 Play，确认新实例 Enable count=1；由于第一次 Play 已写存档，C# 恢复保存分数，Python 重新观察该分数。结束后 Stop。
8. 打开插件管理面板，检查当前状态和两个配置；试改一个开关后 Discard，确认磁盘未改变。实际 Apply 后应显示需重建/重启，当前进程状态不能假装即时变化。

记录实际点击、保存前后文件、编辑器日志和观察到的画面。编译通过、启动截图和命令行配置测试都不能替代这些点击步骤。

2026-09-11 首次 Windows 界面工具初始化失败；重启 Codex 后已恢复。实际通过菜单另存了 `ScriptEditorManual.he`，添加两种语言的通用 Script 组件，编辑字段、保存、退出进程并重开，确认类型、后端和字段保留。首轮 Play/Stop 暴露工具栏点击进入玩法输入的问题，修复后已完成最终两轮按钮复验。保留失败尝试，最终证据以 `out/agent/scripting/editor-ui-manual/result.json` 为准。

### Editor 输入约定

公共 `ScriptInputContext` 描述游戏视口的窗口客户区坐标；桌面 Game 默认使用完整窗口。Editor 在编辑态关闭玩法输入，运行态只接受视口内的指针，并将坐标换算到该视口。UI 优先处理事件；从工具栏按下后拖入视口不会触发脚本点击或拖动。脚本指针状态同时接受事件和轮询，保留帧间短点击并避免重复计数。

正交相机运行态保持正常指针，可以直接点击游戏和 Stop；透视相机仍沿用 Alt 临时释放捕获、Escape 停止的约定。捕获时 GLFW 鼠标位置是无界相对值，脚本指针以视口中心为准，不将相对值误判为界外坐标。早期失败记录用 F10 调试面板释放鼠标，最终正交场景不需要此绕行。

## 本轮验证记录

从仓库根目录复现配置测试和全部保留场景检查（先完成一次 `hectl editor check`）：

```powershell
$cmake = (Select-String -LiteralPath out/agent/editor-relwithdebinfo/CMakeCache.txt -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$').Matches[0].Groups[1].Value
& ./Tools/Plugins/TestConfig.ps1 -CMake $cmake
$scenes = @(git ls-files 'Project/Assets/RenderingBenchmarks/*.he' | ForEach-Object { $_.Substring('Project/'.Length) })
& ./Tools/Agent/Test-EditorSceneOpen.ps1 -GraphicsApi DX12 -Scenes $scenes
```

- `build-only` + 有界运行 PASS：`hectl.bat editor check -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30`，首轮 143.9 秒构建，双语言场景启动退出正常。日志 `out/agent/scripting/plugin-panel-editor-check.log`。
- `runtime-manual` 画面核验 PASS：`hectl.bat editor capture -StartupScene Assets/WeChatSample/ScriptCoexist.he -Frames 30 -CapturePath out/agent/scripting/plugin-panel-final.png`。捕获时临时开启项目 DefaultLayout.ShowPlugins，finally 按原始字节恢复配置。真实面板显示两个脚本插件 Started、正确宿主开关和中文控件；`plugin-panel-visual-review.json` 明确不是点击验收。其后只调整了通用提示、布局保存和错误保护，最终源码在后续场景检查中编译通过。
- `unit-test` PASS：`powershell -NoProfile -File Tools/Plugins/TestConfig.ps1 -CMake <本机构建使用的CMake绝对路径>`。7 项真实校验/原子保存用例，证据 `out/agent/plugin-settings-20260911-201324-529/results.json`。实际 CMake 路径记录在 Editor 构建缓存的 `CMAKE_COMMAND`，不是要求用户安装到固定机器路径。
- 清理后的 `runtime-manual` 有界运行 PASS：`Tools/Agent/Test-EditorSceneOpen.ps1 -GraphicsApi DX12 -Scenes <Git中7个RenderingBenchmarks场景> -OutputDirectory out/agent/rendering-evidence/cleanup-scene-validation`。每个场景 120 帧、编辑态 clean、源场景哈希不变；`evidence-index.json` 指向独立保留的运行日志及其 SHA-256。没有用这些启动结果替代 Play/Stop 和 Inspector 点击。
- 操作起点 `build-only` + 有界运行 PASS：`hectl.bat game check -StartupScene Assets/WeChatSample/ScriptEditorSetup.he -Frames 30`，49.3 秒构建、exit=0；日志 `out/agent/scripting/plugin-editor-setup-game-check.log`。这里只验证相机/Sprite/场景结构能加载，没有添加脚本或模拟按钮点击。

## 重启后最终实际界面验收（2026-09-11 21:09）

`runtime-manual` PASS：`.\hectl.bat editor check -StartupScene Assets/WeChatSample/ScriptEditorManual.he -Frames 1000000 -TimeoutSeconds 1800 -Resolution 1920x1080`。33.8 秒构建成功，实际 Windows 窗口由 computer-use 操作，完成后 Alt+F4，exit=0。最终日志为 `out/agent/scripting/editor-ui-final-verified.log`，独立保存的运行日志、截图、场景和真实存档在 `out/agent/scripting/editor-ui-manual/`。

- 从不含脚本组件的 Setup 场景通过菜单添加两种脚本；C# `sample.touch` 的 Speed=5、Score=41，Python `sample.python-observer` 的 ObservedScore=17。原生另存为、Ctrl+S、关闭进程并重开后字段与绑定保持一致。
- 最终 SaveKey 为 `editor-manual-fixed-20260911`。第一轮 Play 初始化 41，点击真实 Sprite 所在视口后变为 44，画面移动并变色，Python 收到 44。直接点击 Stop，仍为 44；两种语言各 Disable/Destroy 一次。
- Stop 后 Inspector 恢复 C# 41/5、Python 17、EnableCount=0；再次 Play 使用新实体句柄，从真实存档恢复 44，两种语言 EnableCount 都从 1 开始。第二次游戏点击变为 47，第二次 Stop 仍为 47，双方均正常回收。
- 插件菜单、桌面/portable 配置、开关与 Discard 实际操作通过；C# Tests 开关先关闭并 Apply，再开启并 Apply，两次都经过正式 CMake 校验和原子保存。`plugin-apply-disabled.png`/`plugin-apply-restored.png`、两份 candidate JSON 和前后配置为证。所有原始插件/宿主值恢复后，清理了保存器产生的纯排版差异，最终项目配置与测试前逐字节一致。没有声称运行中的插件即时卸载。
- `unit-test` PASS：在已配置的隔离 probe 构建中执行 `cmake --build out/agent/scripting/probe-pc-native --target HEngineScriptCoreContracts`，再运行 `out/agent/scripting/probe-pc-native/HEngineScriptCoreContracts.exe`。新增输入边界/拖动起点/短点击/重复计数检查；日志 `editor-input-core-isolated-build.log`、`editor-input-core-isolated-test.log`。完整引擎测试注册名为 `hectl test ScriptCore`；误用函数名 ScriptCoreContract 的首次调用已停止，未计为 PASS。
- 最终 Game `build-only` + 有界运行 PASS：`.\hectl.bat game check -StartupScene Assets/WeChatSample/ScriptEditorManual.he -Frames 30`，27.3 秒构建、exit=0，`editor-input-game-final.log`。
- `runtime-manual-engine-agent` PASS：`& ./Tools/Scripting/TestPcGame.ps1 -EvidenceDirectory out/agent/scripting/editor-input-pc-regression`，10 项真实引擎回归覆盖键盘移动、点击加 3、真实存档、重进、禁用、CoreCLR 异常栈及故障后响应。此处为引擎虚拟输入，不冒充 Editor OS 点击；该次额外 Game 截图未另行人工检查。

有界 check 使用独立 `--saved-root`，真实存档留在该次运行目录；本次另存 `final-save.txt` 的内容是 47。用户正常启动使用自己的 Saved 路径。UI 截图中的 `final-*` 对应最终构建，`reopened-*` 对应保存重开步骤，其余截图和诊断日志是保留的早期尝试。本轮未重新发布 Web/微信或验证 Android/iOS，不改变既有平台证据的范围。
