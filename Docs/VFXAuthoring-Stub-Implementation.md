# Python 接口与压力入口实施记录

## 范围与事实依据

读取当前 AGENTS.md、PythonScript/Binding、StubGen、实际提交中的 HEngineStub，以及 Storage.ps1/Storage.md。没有修改初始用户改动、绑定 C++、第三方或反射生成目录；引擎构建和 GPU 会话由主代理串行执行。

现有 `Engine/Tools/HEngineStub/HEngine/__init__.pyi:136` 的 `HEngine::SceneCamera` 不能被 Python 解析，且旧目录只包含 HEngine、Input、KeyCode，缺少当前 VFX/UI/Audio/GM 等接口。现有 StubGen 的属性路径未校验类型，生成过程没有来源或覆盖检查。当前命令只支持 Debug/RelWithDebInfo；不能把后者写成 Release。

## 已实现

- StubGen 从实际加载模块反射全部子模块和公开类成员，显式导出子模块。函数和属性的 C++ 命名只有在实际绑定存在时转换为 Python 名称；未绑定类型使用 `typing.Any` 并写入 `unregisteredCppTypes`，不宣称该类型存在或可构造。
- 输出发布前解析 AST 并核对模块/类公开符号。非法签名会失败，不把整条函数悄悄退化成任意参数后报成功。
- `stub-manifest.json` 记录绑定、生成器、反射组件头文件和生成文件 SHA256，记录真实运行解释器和可执行文件身份。未知构建配置、后端明确为 `unknown`；GM 是否可见来自当前模块，开发和发布产物应分别生成。
- 重复生成无变化不改写文件，清理仅限前一份 manifest 声明的旧输出。`CheckPythonStubs.py` 对语法、公开符号、源文件变化和输出篡改进行只读检查。
- `Assets/Scenes/PythonStubGeneration.he` / `Scripts/GeneratePythonStubs.py` 是可启动的实际接口验收入口：生成、输入查询、场景实体/组件读取、CPU 特效加载/播放/状态/停止。结果为 `out/agent/python-stubs/runtime-result.json`，日志带 `PYTHON_STUB_ACCEPTANCE`。它是开发验收资源，不是发布功能。
- `run_effekseer_stress.ps1` 使用 `-StartupScene Assets/Scenes/VFXStressTest.he`，无需修改 Game.json。旧确认开关仅作兼容保留。旧中断 marker 仅在临时配置 SHA256 匹配时恢复；外部修改冲突保留恢复材料。外层 Storage 记录覆盖调用、日志和结束状态，嵌套 hectl 完成之后才结束外层记录。

## 精确验证

以下 Python 路径简称 `$python`：
`C:/Users/51906/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`。

| 类型 | 命令 | 结果 | 证明范围 |
|---|---|---|---|
| unit-test | `& $python Tools/Scripting/TestPythonStubs.py` | PASS，4 项 | C++ 函数/属性类型、实际模拟模块公开符号、重复生成 changed=0/deleted=0 且 hash/mtime 零扰动、源与输出过期检测 |
| unit-test | `& $python Tools/Scripting/TestVfxStressRunner.py` | PASS，4 项 | 真实 PowerShell runner + 假 hectl：启动场景参数、原配置不变、旧 marker 恢复、冲突保护、退出码和 Storage 终态 |
| unit-test | `& $python Tools/Scripting/CheckPythonStubs.py` | FAIL（初始产物，待当前引擎生成） | 检出既有第 136 行语法错误与缺少 manifest，未制造通过 |

首次压力脚本测试发现该子进程中 `Get-FileHash` 不可见，恢复路径改为同等 .NET SHA256 后全部通过。没有模拟 GPU 或把工具契约测试当作引擎视觉验收。

## 待主代理集成运行

运行至少 10 帧：`hectl game check -Configuration RelWithDebInfo -GraphicsApi DX12 -StrictGraphicsApi -StartupScene Assets/Scenes/PythonStubGeneration.he -Frames 10`，读取结果，随后运行 `CheckPythonStubs.py`。不能仅以进程退出码认定脚本验收通过。当前尚未宣称此实际运行通过；新 VFX API 会从当前构建自动覆盖。

后续已由主代理安装 mypy 1.18.2 于 `out/agent/python-typecheck`，验证见下文。发布构建符号隔离仍需当前产物的单独证据。

## 2026-09-13 实际嵌入式回归

主代理固定 integration02 二进制通过 `PythonStubGeneration.he` 实际执行生成器。`out/agent/vfx-authoring/stub-runtime-04/result.json` 证明14模块公开覆盖通过，但组件烟测调用 `bool(ComponentView)` 转发了不支持的 `__len__`，整体FAIL；已改为 `is not None`。此前动态 `py::cpp_function` 的匿名名称和UI普通赋值常量也分别在实际失败报告后修复，没有放宽公开覆盖。

`out/agent/vfx-authoring/stub-runtime-05/result.json` 由主代理实际运行得到PASS：输入、实体、组件以及VFX加载/播放/查询/停止均通过，14模块，changed=1/deleted=0。这个候选版本仍有六个明确列出的未注册C++类型，不能据此宣称所有组件属性均可调用。

unit-test：`$env:PYTHONPATH=(Resolve-Path out/agent/python-typecheck).Path; py -3.11 -m mypy Engine/Tools/HEngineStub/HEngine --no-incremental --follow-imports=skip`，PASS，`Success: no issues found in 14 source files`。初跑发现 `_ComponentView` setter 的 `py::handle` 未注册Python类型；生成器改为其实际通用Python对象契约 `object` 后，由嵌入式05重新生成并通过，未手改产物。

unit-test：`py -3.11 Tools/Scripting/TestPythonStubs.py`，PASS，5项，包含动态方法、UI赋值常量与 `handle` 参数名和类型区分回归。

下一候选构建已新增真实 UUID、SceneCamera、FollowCameraMode、CollisionShape、Rigidbody2DComponent.BodyType、Rigidbody3DComponent.Body3DType 绑定。烟测现在要求 `unregisteredCppTypes` 为空，并实际构造组件、写入并读取六类型属性；当前这部分待主代理构建和实际运行，不沿用05证据。SceneCamera的PerspectiveVerticalFOV使用弧度，所有投影属性setter经原有RecalculateProjection路径。

主代理随后完成build03及embedded06：六类型实际往返PASS，unregisteredCppTypes为空。对06真实产物再跑上述mypy命令发现UUID比较参数过窄与自动hash=None三处错误，未宣称类型检查通过。已将比较接口改为object参数并补值hash；FollowCamera.TargetUUID原def_readwrite getter使用reference_internal，现仅覆盖该属性为UUID值拷贝，避免组件替换后已持有Python UUID/hash改变。新增uuid_value_semantics真实烟测断言，待下一候选构建验证，生成器未添加ignore。
